WT_HOME=/cs/systems/home/fedorova/Work/WiredTiger/wt-dev/build_posix


rm WT_TEST/*
mkdir WT_TEST

${WT_HOME}/bench/wtperf/wtperf -O ${WT_HOME}/../bench/wtperf/runners/evict-btree.wtperf