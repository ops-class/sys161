#include "testcommon.h"
#include "basic-exn.h"

   /*
    * signed addu: negative + positive -> in-range negative
    */
__start:
   EXNSON
   li t0, -9304
   li t1, 503
   addu t2, t0, t1
   nop
   DUMP(0)
   POWEROFF
