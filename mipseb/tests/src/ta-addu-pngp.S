#include "testcommon.h"
#include "basic-exn.h"

   /*
    * signed addu: positive + negative -> in-range positive
    */
__start:
   EXNSON
   li t0, 9304
   li t1, -577
   addu t2, t0, t1
   nop
   DUMP(0)
   POWEROFF
