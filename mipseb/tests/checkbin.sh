#!/bin/sh
#
# checkbin.sh - check a mips binary for having exception handlers in the
#               right place.
#
# Usage: checkbin.sh files
#
# Assumes the exception handlers are called "utlbexn" and "genexn".
# This will only fail if these symbols are present but in the wrong place.
#

mips-harvard-os161-nm -o "$@" | egrep ' utlbexn$| genexn$' |\
  sed '
	/^[^:]*:80000000 [Tt] utlbexn$/d
	/^[^:]*:80000080 [Tt] genexn$/d
	/^[^:]*:ffffffff80000000 [Tt] utlbexn$/d
	/^[^:]*:ffffffff80000080 [Tt] genexn$/d
  ' |\
  sed 's/:.*//' | awk '
	{ f[++n] = $1 }
	END {
	    if (n>0) {
		printf "Failing files:\n" >"/dev/stderr";
		for (i=1;i<=n;i++) {
		    print f[i];
		}
		exit(1);
	    }
	}
' || exit 1
