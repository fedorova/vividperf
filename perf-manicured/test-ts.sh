#!/bin/sh

rm perf.data.manicured
cp perf.data.$1 perf.data

./perf-manicured -s 2689217978660222 -b 2689257902051738  -e 2689257995783687   > $1.out

diff perf.data perf.data.manicured
