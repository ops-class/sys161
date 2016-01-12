#include "testcommon.h"
#include "basic-exn.h"

   /*
    * unsigned addu: positive + positive -> in-range positive
    */
__start:
   EXNSON
   li t0, 0x90000000
   li t1, 0x30000000
   addu t2, t0, t1
   nop
   DUMP(0)
   POWEROFF
