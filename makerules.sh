#!/bin/sh
#
# makerules - write makefile rules for System/161 build
#
# arguments: none
# stdin:     list of directories (no suffix) and sources in those 
#            directories (.c, .cc, .C, .s, .S suffixes recognized)
#


awk '{ for (i=1; i<=NF; i++) print $i; }' |\
  awk '
    BEGIN {
	curdir = "NOGOOD";
	printf "# Automatically generated file - do not edit\n\n";
    }

    function rule(file, base) {
	printf "%s.o: %s/%s\n", base, curdir, file;
	printf "\t$(CC) $(CFLAGS) -I%s -c %s/%s\n", curdir, curdir, file;
	printf "SRCS+=%s/%s\n", curdir, file;
	printf "OBJS+=%s.o\n", base;
	printf "\n";
    }

    /\.[cCsS]$/ {
	file = $1;
	base = $1;
	sub("\\.[cCsS]$", "", base);
	rule(file, base);
	next;
    }

    /\.cc$/ {
	file = $1;
	base = $1;
	sub("\\.cc$", "", base);
	rule(file, base);
	next;
    }

    {
	curdir = "$S/" $1;
	printf "DEPINCLUDES+=-I%s\n\n", curdir;
    }
'
