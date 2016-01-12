#!/bin/sh
#
# cleanlog.sh - strip stuff that varies on each run from an output log
#
# usage: sys161 [stuff] | cleanlog.sh
#

sed '
    /^sys161: clock: [0-9][0-9]*\.[0-9][0-9]* secs/d
    /^sys161: Elapsed real time: /d
    /^sys161: System\/161.*version.*compiled/d
'
