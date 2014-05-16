#!/bin/sh

rm perf.data.manicured
cp perf.data.$1 perf.data

./perf-manicured  > $1.out

diff perf.data perf.data.manicured
