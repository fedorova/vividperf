#!/bin/sh

# Execute these commands prior to running the script:
#
# % git clone git@github.com:wiredtiger/wiredtiger.git -b develop wt-dev-bisect
#
# % git log --pretty=oneline
#
# Find the commit hashes for the "good" and "bad" revisions, for instance:
#
# 09873817509789d23c6ea8df55cfdd38b3713bc9 : bad (c-u-o-2 merge)
#
# 75e68dfa5ed883827996104c2a66f85b399c394e : good -- 1.4.2
#
# % git bisect start
# % git bisect bad 09873817509789d23c6ea8df55cfdd38b3713bc9
# % git bisect good  75e68dfa5ed883827996104c2a66f85b399c394e
#
# Then edit the run-test.sh file to include the test you need.
# Edit the environmental variables below to point to the right directories.
# Edit the LABEL_PER_ARGS below to specify the performance threshold and units. 
# Run label-perf.py without arguments for help on defining that variable, which
# will contain arguments to label-perf.py
#
# Keep executing this script until git bisect tells you its done. 
#
# Then run git bisect log from WT_DIR. The last two lines will show the
# closest bad and good revisions found. The diff between these two revisions
# will contain the bug.
#

WT_DIR="/cs/systems/home/fedorova/Work/WiredTiger/wt-dev-bisect"
LDB_DIR="/cs/systems/home/fedorova/Work/WiredTiger/leveldb-dev-branch"
LABEL_PERF_ARGS="0.200 micros/op; greater perf.txt"


# Rebuild the library and the benchmark
# build-with-bisect.sh is just the build.sh that points to the
# WT directory that we use for bisection
#
cd ${WT_DIR}
./build_posix/reconf 
cd ${WT_DIR}/build_posix
../configure --enable-snappy --enable-debug
make
cd ${LDB_DIR}; ./build-with-bisect.sh

# Run the test, label it's performance as 'good' or 'bad'
# based on the configuration string in the LABEL_PERF_ARGS
#
./run-test.sh
echo #### PERFORMANCE TEST OUTPUT BEGIN #########
cat perf.txt
echo #### PERFORMANCE TEST OUTPUT END #########
./label-perf.py $LABEL_PERF_ARGS > label


# Tell git whether this revision is good or bad
#
LABEL=$(cat label)
cd ${WT_DIR}
echo This was a $LABEL revision
echo git bisect $LABEL | sh

