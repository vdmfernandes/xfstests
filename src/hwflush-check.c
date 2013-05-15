/*
 * Copyright (c) 2011-2013 Parallels Inc.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

Compile:
	gcc  hwflush-check.c -o hwflush-check -lpthread
Usage example:
-------
  1. On a server with the hostname test_server, run: hwflush-check -l
  2. On a client, run: hwflush-check -s test_server -d /mnt/test -t 100
  3. Turn off the client, and then turn it on again.
  4. Restart the client: hwflush-check -s test_server -d /mnt/test -t 100
  5. Check the server output for lines containing the message "cache error detected!"

*/
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdint.h>
#include <signal.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <unistd.h>

enum prealloc_type {
	PA_NONE = 0,
	PA_POSIX_FALLOC = 1,
	PA_WRITE = 2,
	PA_LAST = PA_WRITE,
};

static int alloc_type = PA_POSIX_FALLOC;
static int is_server = 0;
static int is_check_stage = 0;
static int is_prepare = 0;
static int use_fdatasync = 0;
static char *host = NULL;
static char *port = "32000";
static char *dir = NULL;
/* block size should be a multiply of 8 */
static off_t blocksize = 16 * 1024 - 104;
static off_t blocksmax = 1024 + 1;
static unsigned int threads = 32;
#define THREADS_MAX	1024

static int exit_flag = 0;

/* returns 0 if ok or -errno if error */
int swrite(int fd, void *buf, int sz)
{
	int w = sz;

	while (w) {
		int n = write(fd, buf, w);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (n == 0)
			return -EIO;
		buf += n;
		w -= n;
	}
	return sz;
}

/* returns number of bytes read */
int sread(int fd, void *buf, int sz)
{
	int r = 0;
	while (sz) {
		int n = read(fd, buf, sz);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (n == 0)
			break;
		buf += n;
		r += n;
		sz -= n;
	}
	return r;
}

static int connect_to_server(void)
{
	struct addrinfo *result, *rp, hints;
	int sock = -1;
	int ret;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_V4MAPPED | AI_ADDRCONFIG;

	/* resolve address */
	ret = getaddrinfo(host, port, &hints, &result);
	if (ret != 0) {
		fprintf(stderr, "getaddrinfo() failed: %s\n", gai_strerror(ret));
		return -1;
	}

	/* getaddrinfo() returns a list of address structures.
	   Try each address until we successfully connect(2).
	   If socket(2) (or connect(2)) fails, we (close the socket
	   and) try the next address. */
	for (rp = result; rp != NULL; rp = rp->ai_next) {
		sock = socket(rp->ai_family, rp->ai_socktype,
				rp->ai_protocol);
		if (sock < 0) {
			fprintf(stderr, "Could not create socket: %s\n", strerror(errno));
			continue;
		}

		if (connect(sock, rp->ai_addr, rp->ai_addrlen) < 0) {
			fprintf(stderr, "connect() failed: %s\n", strerror(errno));
			close(sock);
			sock = -1;
			continue;
		}

		fprintf(stderr, "Connected to %s:%s\n", host, port);
		break;	/* Success */
	}

	if (rp == NULL) /* No address succeeded */
		fprintf(stderr, "Could not connect to server\n");

	/* addrinfo is not needed any longer, free it */
	freeaddrinfo(result);

	return sock;
}

static uint64_t find_last_counter(int fd, char *buf, off_t *offset)
{
	uint64_t cnt = 0;
	off_t i, len;

	for (i = 0; i < blocksmax; i++) {
		uint64_t t;
		unsigned int c, j;

		len = sread(fd, buf, blocksize);
		if (len < 0) {
			fprintf(stderr, "read() failed: %s\n", strerror(-len));
			break;
		}
		if (len != blocksize) {
			fprintf(stderr, "Failed to read block %llu\n",
					(unsigned long long)i);
			break;
		}

		t = *(uint64_t*)buf;
		if (cnt >= t)
			break;

		/* validate block */
		memset(&c, t & 0xff, sizeof(c));
		for (j = sizeof(t); j < blocksize; j += sizeof(c))
			if (c != *(unsigned int*)(buf + j))
				break;
		if (j < blocksize) {
			fprintf(stderr, "Block %llu with number %llu is invalid "
				"at %d, blocksize %llu \n", (unsigned long long)i,
				(unsigned long long)t, j,
				(unsigned long long)blocksize);
			break;
		}

		/* ok, block is good, store counter */
		cnt = t;
	}

	*offset = blocksize * i;

	return cnt;
}

/* press Ctrl-C twice on freeze */
static void sighandler(int sig)
{
	if (exit_flag) {
		signal(sig, SIG_DFL);
		raise(sig);
	}
	exit_flag = 1;
}

struct client {
	int sock;
	pthread_mutex_t mutex;
};

struct worker {
	pthread_t thr;
	uint32_t id;
	struct client *cl;
};

enum {
	REP_FL_UPDATE = 1,
};

struct report {
	uint32_t id;
	uint32_t flags;
	uint64_t cnt;
} __attribute__((aligned(8)));

static void *run_client_thread(void *arg)
{
	struct worker *w = arg;
	int ret;
	int fd;
	off_t offset = 0;
	char *buf;
	char file[strlen(dir) + 6];
	struct report rp = {
		.id = w->id,
		.flags = 0,
		.cnt = 0
	};

	buf = malloc(blocksize);
	if (!buf) {
		fprintf(stderr, "malloc() failed\n");
		return NULL;
	}

	snprintf(file, sizeof(file), "%s/%04u", dir, w->id);
	/* first try to find last used counter */
	fd = open(file, O_RDWR, 0666);
	if (fd < 0) {
		if (is_check_stage) {
			fprintf(stderr, "Failed to open file '%s': %s\n", file, strerror(errno));
			goto out_free;
		}
		if ((errno != ENOENT) || ((fd = creat(file, 0666)) < 0)) {
			fprintf(stderr, "Failed to open file '%s': %s\n", file, strerror(errno));
			goto out_free;
		}
		switch (alloc_type) {
		case PA_NONE:
			break;
		case PA_POSIX_FALLOC:
			if (posix_fallocate(fd, 0, blocksize * blocksmax) < 0) {
				fprintf(stderr, "fallocate() failed: %s\n",
					strerror(errno));
				goto out_close_fd;
			}
			break;
		case PA_WRITE: {
			off_t num, count = blocksize * blocksmax;
			int ret;
			memset(buf, 0, blocksize);
			while (count) {
				num = blocksize < count ? blocksize : count;
				ret = write(fd, buf, num);
				if (ret < 0) {
					fprintf(stderr, "write() failed: %s\n",
						strerror(errno));
					goto out_close_fd;
				}
				count -= ret;
			}
			lseek(fd, 0, SEEK_SET);
			break;
		}
		default:
			fprintf(stderr, "Incorrect prealloc type ");
			goto out_close_fd;
			break;
		}

	} else {
		int r;
		rp.cnt = find_last_counter(fd, buf, &offset);
		if (lseek(fd, offset, SEEK_SET) < 0) {
			fprintf(stderr, "lseek() failed: %s\n", strerror(errno));
			goto out_close_fd;
		}
		fprintf(stderr, "id %u: latest valid id %llu\n", w->id, (unsigned long long)rp.cnt);
		rp.id = w->id;
		pthread_mutex_lock(&w->cl->mutex);
		r = swrite(w->cl->sock, &rp, sizeof(rp));
		pthread_mutex_unlock(&w->cl->mutex);
		if (r < 0) {
			fprintf(stderr, "Failed to write to socket: %s\n", strerror(-r));
			goto out_close_fd;
		}
		if (is_check_stage)
			goto out_close_fd;
	}
	if (fsync(fd)) {
		fprintf(stderr, "fsync(2) failed: %s\n", strerror(errno));
		goto out_close_fd;
	}
	if (is_prepare)
		goto out_close_fd;

	rp.flags = REP_FL_UPDATE;
	while (!exit_flag) {
		int r;

		if (offset >= blocksize * blocksmax) {
			offset = 0;
			lseek(fd, 0, SEEK_SET);
		}

		rp.cnt++;
		*(uint64_t*)buf = rp.cnt;
		memset(buf + sizeof(rp.cnt), rp.cnt & 0xff, blocksize - sizeof(rp.cnt));
		r = swrite(fd, buf, blocksize);
		if (r != blocksize) {
			fprintf(stderr, "Failed to write to file '%s': %s\n", file, strerror(-r));
			break;
		}
		if (use_fdatasync)
			ret = fdatasync(fd);
		else
			ret = fsync(fd);

		if (ret < 0) {
			fprintf(stderr, "%s failed: %s\n", use_fdatasync ?
				"fdatasync()" : "fsync()", strerror(errno));
			break;
		}

		pthread_mutex_lock(&w->cl->mutex);
		r = swrite(w->cl->sock, &rp, sizeof(rp));
		pthread_mutex_unlock(&w->cl->mutex);
		if (r < 0) {
			fprintf(stderr, "Failed to write to socket: %s\n", strerror(-r));
			break;
		}

		offset += blocksize;
	}

out_close_fd:
	close(fd);
out_free:
	free(buf);

	return NULL;
}

static int run_client(void)
{
	struct stat st;
	int ret = 0;
	int flag = 1;
	int i;
	struct client clnt;
	struct worker *thrs;

	if (stat(dir, &st) < 0) {
		if (errno != ENOENT) {
			fprintf(stderr, "stat() for '%s' failed: %s\n", dir, strerror(errno));
			return -1;
		}
		if (mkdir(dir, 0777) < 0) {
			fprintf(stderr, "Failed to create directory '%s': %s\n", dir, strerror(errno));
			return -1;
		}
	} else if (!S_ISDIR(st.st_mode)) {
		fprintf(stderr, "'%s' is not a directory\n", dir);
		return -1;
	}

	clnt.sock = connect_to_server();
	if (clnt.sock < 0)
		return -1;

	if (setsockopt(clnt.sock, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(int)) < 0) {
		fprintf(stderr, "setsockopt(TCP_NODELAY) failed: %s\n", strerror(errno));
		ret = -1;
		goto out_close_sock;
	}

	/* make things fancier for the server */
	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);

	thrs = malloc(threads * sizeof(struct worker));
	if (!thrs) {
		fprintf(stderr, "malloc() failed\n");
		ret = -1;
		goto out_close_sock;
	}

	pthread_mutex_init(&clnt.mutex, NULL);

	for (i = 0; i < threads; i++) {
		thrs[i].id = i;
		thrs[i].cl = &clnt;
		if (pthread_create(&thrs[i].thr, NULL, run_client_thread, (void*)&thrs[i])) {
			fprintf(stderr, "Failed to start thread %u\n", i);
			ret = -1;
			break;
		}
	}

	for (i--; i >= 0; i--)
		pthread_join(thrs[i].thr, NULL);

	free(thrs);
out_close_sock:
	close(clnt.sock);
	
	return ret;
}

static int prepare_for_listening(void)
{
	struct addrinfo *result, *rp, hints;
	int sock = -1;
	int ret;

	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE; /* For wildcard IP address */

	ret = getaddrinfo(NULL, port, &hints, &result);
	if (ret != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(ret));
		return -1;
	}

	/* getaddrinfo() returns a list of address structures.
	   Try each address until we successfully bind(2).
	   If socket(2) (or bind(2)) fails, we (close the socket
	   and) try the next address. */

	for (rp = result; rp != NULL; rp = rp->ai_next) {
		int flag = 1;

		sock = socket(rp->ai_family, rp->ai_socktype,
				rp->ai_protocol);
		if (sock < 0) {
			fprintf(stderr, "Could not create socket: %s\n", strerror(errno));
			continue;
		}

		if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char*)&flag, sizeof(int)) < 0) {
			fprintf(stderr, "setsockopt(SO_REUSEADDR) failed: %s\n", strerror(errno));
			close(sock);
			sock = -1;
			continue;
		}

		if (bind(sock, rp->ai_addr, rp->ai_addrlen) < 0) {
			fprintf(stderr, "bind() failed: %s\n", strerror(errno));
			close(sock);
			sock = -1;
			continue;
		}

		fprintf(stderr, "Listening on port %s\n", port);
		break; /* Success */
	}

	if (rp == NULL) /* No address succeeded */
		fprintf(stderr, "Could not bind\n");

	freeaddrinfo(result); /* No longer needed */

	return sock;
}

static int set_sock_keepalive(int sock)
{
	int val = 1;

	/* enable TCP keepalives on socket */
	if (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &val,
				sizeof(val)) < 0) {
		fprintf(stderr, "setsockopt() failed: %s\n", strerror(errno));
		return -1;
	}
	/* set idle timeout to 1 second */
	if (setsockopt(sock, SOL_TCP, TCP_KEEPIDLE, &val,
				sizeof(val)) < 0) {
		fprintf(stderr, "setsockopt() failed: %s\n", strerror(errno));
		return -1;
	}
	/* set consecutive interval to 1 second */
	if (setsockopt(sock, SOL_TCP, TCP_KEEPINTVL, &val,
				sizeof(val)) < 0) {
		fprintf(stderr, "setsockopt() failed: %s\n", strerror(errno));
		return -1;
	}
	/* set number of keepalives before dropping to 3 */
	val = 3;
	if (setsockopt(sock, SOL_TCP, TCP_KEEPCNT, &val,
				sizeof(val)) < 0) {
		fprintf(stderr, "setsockopt() failed: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

static int run_server(void)
{
	int sock;
	struct sockaddr_storage peer_addr;
	socklen_t peer_addr_len;
	char boundaddr[NI_MAXHOST] = "";
	ssize_t nread;
	uint64_t *rcv;
	struct report rp;
	int ret = 0;

	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);

	sock = prepare_for_listening();
	if (sock < 0)
		return -1;

	if (listen(sock, 5) < 0) {
		fprintf(stderr, "listen() failed: %s\n", strerror(errno));
		ret = -1;
		goto out_close_sock;
	}

	rcv = calloc(THREADS_MAX, sizeof(uint64_t));
	if (!rcv) {
		fprintf(stderr, "calloc() failed\n");
		ret = -1;
		goto out_close_sock;
	}

	while (!exit_flag) {
		char claddr[NI_MAXHOST];
		int conn;

		peer_addr_len = sizeof(struct sockaddr_storage);
		conn = accept(sock, (struct sockaddr *) &peer_addr, &peer_addr_len);
		if (conn < 0) {
			fprintf(stderr, "accept() failed: %s\n", strerror(errno));
			ret = -1;
			break;
		}

		ret = set_sock_keepalive(conn);
		if (ret < 0) {
			close(conn);
			break;
		}

		ret = getnameinfo((struct sockaddr *) &peer_addr,
				peer_addr_len, claddr, NI_MAXHOST,
				NULL, 0, NI_NUMERICHOST);
		if (ret < 0) {
			fprintf(stderr, "getnameinfo() failed: %s\n", gai_strerror(ret));
			close(conn);
			break;
		}

		if (boundaddr[0] == 0) {
			strncpy(boundaddr, claddr, NI_MAXHOST-1);
			fprintf(stderr, "Accepting messages from %s\n", boundaddr);
		} else {
			if (strncmp(boundaddr, claddr, NI_MAXHOST) != 0) {
				fprintf(stderr, "Skip connection from invalid address %s\n", claddr);
				close(conn);
				continue;
			}
			fprintf(stderr, "Restarted connection from %s\n", boundaddr);
		}

		while (!ret) {
			uint32_t expected_id;

			nread = sread(conn, &rp, sizeof(rp));
			if (nread < 0) {
				fprintf(stderr, "read() failed: %s\n", strerror(-nread));
				break;
			}
			if (nread == 0)
				break;
			if (nread != sizeof(rp)) {
				fprintf(stderr, "Failed to read counter\n");
				break;
			}

			if ((rp.id < 0) || (rp.id >= THREADS_MAX)) {
				fprintf(stderr, "Bad id received: %u\n", rp.id);
				break;
			}
			if (rp.flags & REP_FL_UPDATE)
				expected_id = rcv[rp.id] + 1;
			else /* simple check */
				expected_id = rcv[rp.id];

			if (rp.cnt < expected_id) {
				printf("id %u: %llu %s %llu, cache error detected!\n",
				       rp.id, (unsigned long long)rcv[rp.id],
				       rp.flags & REP_FL_UPDATE ? "->" : "!=",
				       (unsigned long long)rp.cnt);
				ret = 1;
			} else if (rp.cnt > expected_id)
				fprintf(stderr, "id %u: %llu -> %llu, probably missed some packets\n",
						rp.id, (unsigned long long)rcv[rp.id],
						(unsigned long long)rp.cnt);
			if (rp.flags & REP_FL_UPDATE)
				rcv[rp.id] = rp.cnt;
		}
		close(conn);
		fprintf(stderr, "Connection closed\n");
		if (ret)
			exit_flag = 1;
	}
	free(rcv);
out_close_sock:
	close(sock);
	return ret;
}

static const char *progname(const char *prog)
{
	char *s = strrchr(prog, '/');
	return s ? s+1 : prog;
}

static void usage(const char *prog)
{
	fprintf(stderr, "Flush test tool.\n");
	fprintf(stderr, "Usage: %s [options...]\n", progname(prog));
	fprintf(stderr, "Options:\n"
			"  -l, --listen          Run as a server.\n"
			"  -c, --check           Check data\n"
			"  -P, --prepare         Perform only preparation stage\n"
			"  -s, --server=IP       Set server host name or IP address\n"
			"  -p, --port=PORT       Set server port\n"
			"  -d, --dir=DIR         Set test directory\n"
			"  -f, --fdatasync={0,1} Use fdatasync(2) instead of fsync(2)\n"
			"  -b, --blocksize=SIZE  Set block size\n"
			"  -n, --blocksmax=NUM   Set maximum number of blocks\n"
			"  -t, --threads=NUM     Set number of client threads to use\n"
			"  -a, --alloc_type=NUM  Set prealloc type 0:NONE, 1:posix_falloc, 2:write\n"
			"  -h, --help            Show usage information\n"
	       );

	exit(-1);
}

static const struct option long_opts[] = {
	{"listen",	0, 0, 'l'},
	{"check",	0, 0, 'c'},
	{"prepare",	0, 0, 'P'},
	{"server",	1, 0, 's'},
	{"port",	1, 0, 'p'},
	{"dir",		1, 0, 'd'},
	{"blocksize",	1, 0, 'b'},
	{"fdatasync",	1, 0, 'f'},
	{"blocksmax",	1, 0, 'n'},
	{"threads",	1, 0, 't'},
	{"alloc_type",	1, 0, 'a'},
	{"help",	0, 0, 'h'},
	{0, 0, 0, 0}
};

int main(int argc, char *argv[])
{
	int ch;

	/* process options, stop at first nonoption */
	while ((ch = getopt_long(argc, argv, "Pcls:p:d:a:b:f:n:t:h", long_opts, NULL)) != -1) {
		switch (ch) {
		case 'l':
			is_server = 1;
			break;
		case 'c':
			is_check_stage = 1;
			break;
		case 'P':
			is_prepare = 1;
			break;
		case 's':
			host = optarg;
			break;
		case 'p':
			port = optarg;
			break;
		case 'd':
			dir = optarg;
			break;
		case 'a':
			alloc_type = atoi(optarg);
			if (alloc_type > PA_LAST) {
				fprintf(stderr, "Invalid prealloc type\n");
				usage(argv[0]);
			}
			break;
		case 'f':
			use_fdatasync = atoi(optarg);
			break;
		case 'b': {
			char *p;
			blocksize = strtoull(optarg, &p, 10);
			if (p[0] != '\0') {
				fprintf(stderr, "Invalid block size\n");
				usage(argv[0]);
			}
			blocksize &= ~7LL;
			break;
		}
		case 'n': {
			char *p;
			blocksmax = strtoull(optarg, &p, 10);
			if (p[0] != '\0') {
				fprintf(stderr, "Invalid maximum number of blocks\n");
				usage(argv[0]);
			}
			break;
		}
		case 't': {
			char *p;
			threads = strtoul(optarg, &p, 10);
			if (p[0] != '\0') {
				fprintf(stderr, "Invalid number of threads\n");
				usage(argv[0]);
			}
			if (threads > THREADS_MAX) {
				fprintf(stderr, "Number of threads is too big\n");
				usage(argv[0]);
			}
			break;
		}
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (!is_server) {
		if (host == NULL) {
			fprintf(stderr, "Please specify server address\n");
			usage(argv[0]);
		}
		if (dir == NULL) {
			fprintf(stderr, "Please specify test directory\n");
			usage(argv[0]);
		}
		return run_client();
	} else
		return run_server();
}
