#!/bin/sh

# Before you run this, you need to do the following:
#
# git bisect start
# git bisect bad <commit>
# git bisect good <commit>
#
# Modify track-regression.sh and run-test.sh as needed.
# CD into the directory where the run-test.sh resides
#


command="grep \"is the first bad commit\" track.out"
done=1

./track-regression.sh > track.out


# Grep returns 0 if the lines were found
while [ $done != 0 ]; 
do
    ./track-regression.sh >> track.out
    eval $command
    done=$?
done

