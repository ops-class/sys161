#!/bin/sh
#
# makerules.sh - generate make rules for mips tests
# usage: ls t*.S | ./makerules.sh
#

awk '{ for (i=1;i<=NF;i++) print $i; }' |\
  awk '
  {
    src=$1;
    testname=src;
    sub("\\.S$", "", testname);

    image = testname ".img";
    logg = testname ".log";
    diff = testname ".diff";
    good = "$T/good/" testname ".good";

    printf "################################\n";
    printf "# %s\n", testname;
    printf "\n";

    printf "images: %s\n", image;
    printf "%s: $T/src/%s\n", image, src;
    printf "\t$(TARGETCC) $(TARGETFLAGS) $T/src/%s -o %s\n", src, image;
    printf "\t$T/checkbin.sh %s\n", image;
    printf "\n";

    printf "run-tests: %s\n", diff;
    printf "%s: %s %s\n", diff, logg, good;
    printf "\tdiff -u %s %s > %s || true\n", good, logg, diff;
    printf "%s: $(SYS161) %s\n", logg, image;
    printf "\t$(SYS161) $(SYS161FLAGS) %s 2>&1 | $T/cleanlog.sh > %s\n", \
	    image, logg;
    printf "\n";

    printf "good: %s.good\n", testname;
    printf "%s.good:\n", testname;
    printf "\tcp %s %s\n", logg, good;
    printf "\n";

    printf ".PHONY: %s.good\n", testname;
    printf "\n";
}'
