#include "testcommon.h"
#include "basic-exn.h"

   /*
    * unsigned sub: positive - positive -> negative
    */
__start:
   EXNSON
   li t0, 0x90000000
   li t1, 0xa0000000
   sub t2, t0, t1
   nop
   DUMP(0)
   POWEROFF
