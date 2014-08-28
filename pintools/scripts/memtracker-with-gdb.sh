#!/bin/bash

#
# IMPORTANT!!! In order for this script to work, execute the following as root before running it:
#
# $ echo 0 > /proc/sys/kernel/yama/ptrace_scope
# 
# This ensures that the OS allows Pin to attach to the process.
# For more information, regarding child injection, see Injection section in the Pin User Manual.
#

# Check that the ptrace_scope file has the correct value
value=$(</proc/sys/kernel/yama/ptrace_scope)
echo 

if [ "$value" -ne 0 ]; then
    echo "In order to run this script execute the following as root: "
    echo "     $ echo 0 > /proc/sys/kernel/yama/ptrace_scope "
    echo "This ensures that the OS allows Pin to attach to the process"
    exit 1
fi

GDB_CMD=gdb-commands.txt
GDB_OUT=gdb-out.txt
MEMTRACKER_HOME=/cs/systems/home/fedorova/Work/VIVIDPERF/vividperf/pintools/scripts


# Create the DB
env LD_LIBRARY_PATH=../wt-dev/build_posix/.libs:../wt-dev/build_posix/ext/compressors/snappy/.libs/ ./db_bench_wiredtiger --cache_size=534217728 --threads=1 --use_lsm=0 --db=/tmpfs/leveldb --benchmarks=fillseq --value_size=62 

# Start the debugging session for the target application. 
# We will later ask the debugger about data types allocated 
# by the application. GDB will read commands from commands.txt, 
# so create this file ahead of time
rm ${GDB_CMD}
echo '' > ${GDB_CMD}
echo '' > ${GDB_OUT}
tail -f ${GDB_CMD} | env LD_LIBRARY_PATH=../wt-dev/build_posix/.libs:../wt-dev/build_posix/ext/compressors/snappy/.libs/ gdb ./db_bench_wiredtiger &> ${GDB_OUT} &

# Start the benchmark reading from DB via Pin, pipe the output 
# to the python script that will drive the gdb session. 

env LD_LIBRARY_PATH=../wt-dev/build_posix/.libs:../wt-dev/build_posix/ext/compressors/snappy/.libs/  pin.sh  -appdebug -t $CUSTOM_PINTOOLS_HOME/obj-intel64/memtracker.so -g 1 --  ./db_bench_wiredtiger --cache_size=534217728  --use_existing_db=1 --threads=1 --use_lsm=0 --db=/tmpfs/leveldb --reads=10000 --benchmarks=readrandom --value_size=62 | ${MEMTRACKER_HOME}/memtracker-gdb-driver.py -c ${GDB_CMD} &> python-out.txt

