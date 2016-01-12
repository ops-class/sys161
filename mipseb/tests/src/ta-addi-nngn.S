#include "testcommon.h"
#include "basic-exn.h"

   /*
    * signed addi: negative + negative -> in-range negative
    */
__start:
   EXNSON
   li t0, -9304
   addi t2, t0, -11057
   nop
   DUMP(0)
   POWEROFF
