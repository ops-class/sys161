#include "testcommon.h"
#include "basic-exn.h"

   /*
    * signed addi: negative + positive -> in-range negative
    */
__start:
   EXNSON
   li t0, -9304
   addi t2, t0, 503
   nop
   DUMP(0)
   POWEROFF
