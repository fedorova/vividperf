#!/bin/sh

for var in "$@"
do
    echo "$var" >> log.txt
done

#echo $0 $1 $2 $3 $4 $5 $6 >> log.txt
