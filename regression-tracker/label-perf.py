#!/usr/bin/python -tt

help = """Take the file containing the program output and compare the performance
value contained in it to the user-supplied performance. We expect that the performance
number will appear in the format '<number> <units>'. The number and the units
should be separated by a space. For instance, the file could contain a line with 
the following words:

       239 micros/op

and the user could supply the '200 micros/op greater perf.txt' as the argument.

Then the program will find the matching performance string in the file perf.txt,
compare the performance to the user-supplied value of 200 micros/op, and will
output 'bad' if the contained performance value is greater than the user-supplied one,
and 'good' if it is less or equal.

Alternatively, we could give an argument that is something like: '700 MB/S less
perf.txt'. In that case, the program will output 'bad' if the performance value
contained in the file is smaller than 700 MB/S.

If there are multiple lines containing performance values in the file, the program
will average them before comparing to the user supplied threshold. 


"""


from os import system
import os.path

import re
import sys



###
def parse_and_print_performance(fname, string, moreOrLess, pNumber):

    totalValues = 0;
    cumValues = 0.0;
    file = open(fname, 'r');

    for line in file:
        if string in line:
            words = line.split(" ");
            for i in range(0, len(words)):
                if words[i] == string and i > 0:
                    totalValues += 1;
                    cumValues += float(words[i-1])
 
    if totalValues > 0:
        perf= cumValues / totalValues

        if(moreOrLess == "greater"):
            if perf > pNumber:
                print "bad"
            else:
                print "good"
        elif (moreOrLess == "less"):
            if perf < pNumber:
                print "bad"
            else:
                print "good"
    else: # Something went wrong, so we will tell git to skip
        print "skip"

###
def main():

    numargs = len(sys.argv)
    if numargs < 4:
        print '\nusage: ./' + sys.argv[0] + '<perf value> <perf unit> <greater or less> <filename>\n\n'
        print help
        sys.exit(1)



    fname = sys.argv[numargs-1]
    pNumber = float(sys.argv[1])
    moreOrLess = sys.argv[numargs-2]
    pUnit = sys.argv[2];
    for i in range(3, numargs-2):
        pUnit = pUnit + " " + sys.argv[i]

  #  print "File: " + fname + ", perf: " + str(pNumber) + ", unit: " + pUnit + ", " + moreOrLess

    if not os.path.exists(fname):
        print '\nusage: ./' + sys.argv[0] + '<perf value> <perf unit> <filename>\n\n'
        print help
        sys.exit(1)

    parse_and_print_performance(fname, pUnit, moreOrLess, pNumber);


if __name__ == '__main__':
    main()
