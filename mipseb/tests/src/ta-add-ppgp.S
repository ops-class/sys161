#include "testcommon.h"
#include "basic-exn.h"

   /*
    * signed add: positive + positive -> in-range positive
    */
__start:
   EXNSON
   li t0, 9304
   li t1, 503483
   add t2, t0, t1
   nop
   DUMP(0)
   POWEROFF
