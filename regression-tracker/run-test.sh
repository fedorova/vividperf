
# Create the DB
env LD_LIBRARY_PATH=../wt-dev-bisect/build_posix/.libs:../wt-dev-bisect/build_posix/ext/compressors/snappy/.libs/ ./db_bench_wiredtiger --cache_size=534217728 --threads=1 --use_lsm=0 --db=/tmpfs/leveldb --benchmarks=fillseq


# Run the measuring test
echo 'MEASUREMENT'

env LD_LIBRARY_PATH=../wt-dev-bisect/build_posix/.libs:../wt-dev-bisect/build_posix/ext/compressors/snappy/.libs/ ./db_bench_wiredtiger --cache_size=534217728  --use_existing_db=1 --threads=1 --use_lsm=0 --db=/tmpfs/leveldb --reads=50000000 --benchmarks=readseq > perf.txt

env LD_LIBRARY_PATH=../wt-dev-bisect/build_posix/.libs:../wt-dev-bisect/build_posix/ext/compressors/snappy/.libs/ ./db_bench_wiredtiger --cache_size=534217728  --use_existing_db=1 --threads=1 --use_lsm=0 --db=/tmpfs/leveldb --reads=50000000 --benchmarks=readseq >> perf.txt

env LD_LIBRARY_PATH=../wt-dev-bisect/build_posix/.libs:../wt-dev-bisect/build_posix/ext/compressors/snappy/.libs/ ./db_bench_wiredtiger --cache_size=534217728  --use_existing_db=1 --threads=1 --use_lsm=0 --db=/tmpfs/leveldb --reads=50000000 --benchmarks=readseq >> perf.txt



