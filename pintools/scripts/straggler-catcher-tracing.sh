WT_HOME=/cs/systems/home/fedorova/Work/WiredTiger/wt-dev/build_posix


rm WT_TEST/*
mkdir WT_TEST

pin.sh -t $CUSTOM_PINTOOLS_HOME/obj-intel64/straggler-catcher.so -s ./my_script.sh -trace 1  -- ${WT_HOME}/bench/wtperf/wtperf -O ${WT_HOME}/../bench/wtperf/runners/evict-btree.wtperf