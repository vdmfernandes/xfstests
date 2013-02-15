#! /bin/sh

function do_fail()
{
    echo "$*" | tee -a ploop.full
    echo "(see ploop.full for details)"
    status=1
    exit 1
}

function run_check()
{
	echo "# $@" >> ploop.full 2>&1
	"$@" >> ploop.full 2>&1 || do_fail "failed: '$@'"
}

function ploop_create_start()
{
    if [ $# -ne 4 ]
    then
        echo " ploop_create_start $# Usage: image_dir dev_id size mkfs_opt" 1>&2
	exit 1
    fi
    img=$1
    dev=$2
    size=$3
    mkfs_opt=$4

    ploop umount -d /dev/ploop$dev >> ploop.full 2>&1
    rm -rf $img > /dev/null
    unlink  /dev/ploop${dev}

    run_check mkdir $img
    run_check mknod /dev/ploop${dev} b 182 $((dev*16))
    run_check ploop init -s${size}  -f ploop1 $img/ploop.img
    run_check ploop mount -f ploop1 -d /dev/ploop${dev} $img/ploop.img
    run_check mkfs.ext4 $mkfs_opt /dev/ploop${dev} 
}

function ploop_stop_check()
{
    if [ $# -ne 2 ]
    then
        echo "ploop_stop_check Usage: image_dir dev_id <check_opt>" 1>&2
	exit 1
    fi
    img=$1
    dev=$2
    check_opt=""
    if [ $# -eq 3 ]
    then
	check_opt=$3
    fi

    run_check ploop umount -d /dev/ploop$dev
    run_check ploop fsck -fc $check_opt $img/ploop.img
}

function snap_merge_loop()
{
    if [ $# -ne 4 ]
    then
        echo "Usage: DiskDescriptor.xml dev_id max_delta_depth run_file" 1>&2
	exit 1
    fi

    desc=$1
    dev=$2
    depth=$3
    run_file=$4
    while [ -f $run_file ]
    do
	for ((i=0; i < $depth; i++))
	do
	    run_check ploop snapshot -d /dev/ploop$dev $desc
	    sleep 1
	done
	for ((i=0; i < $depth; i++))
	do
	    run_check ploop merge -d /dev/ploop$dev $desc
	    sleep 1
	done

	for ((i=0; i < $depth; i++))
	do
	    run_check ploop snapshot -d /dev/ploop$dev $desc
	    sleep 1
	done
	run_check ploop merge -A -d /dev/ploop$dev $desc
    done
}

function do_run_tests()
{
    args=$@

    test_dev=$START_DEV_ID
    scratch_dev=$(($START_DEV_ID + 1))
    size="10g"
    mkfs_opt="$MKFS_OPTIONS"
    mnt_opt="$MOUNT_OPTIONS"
    depth=$DEPTH
    run_file=`mktemp`
    SPACE=$PLOOP_SPACE

    echo "XXX-START $0 $*" | tee -a ploop.full > /dev/kmsg
    ploop_create_start $SPACE/test_dev $test_dev $size "$mkfs_opt"
    ploop_create_start $SPACE/scratch_dev $scratch_dev $size "$mkfs_opt"

    touch $run_file
    run_check snap_merge_loop $SPACE/test_dev/DiskDescriptor.xml $test_dev $depth $run_file &
    pid1=$!
    run_check snap_merge_loop $SPACE/scratch_dev/DiskDescriptor.xml $scratch_dev $depth $run_file &
    pid2=$!

    export TEST_DIR=/mnt_test-$test_dev
    export SCRATCH_MNT=/mnt_scratch-$test_dev
    mkdir -p $TEST_DIR
    mkdir -p $SCRATCH_MNT

    export TEST_DEV=/dev/ploop$test_dev
    export SCRATCH_DEV=/dev/ploop$scratch_dev
    echo "export TEST_DEV=/dev/ploop$test_dev"
    echo "export SCRATCH_DEV=/dev/ploop$scratch_dev"
    
    ./check $args

    unlink $run_file
    wait $pid1
    wait $pid2

    ploop_stop_check $SPACE/test_dev $test_dev
    ploop_stop_check $SPACE/scratch_dev $scratch_dev
}

export START_DEV_ID=100
export PLOOP_SPACE=${PLOOP_SPACE:="/vz/test"}
export DEPTH=${DEPTH:="5"}

do_run_tests $@