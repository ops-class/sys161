#include "testcommon.h"
#include "basic-exn.h"

   /*
    * signed addi: positive + negative -> in-range negative
    */
__start:
   EXNSON
   li t0, 9304
   addi t2, t0, -11712
   nop
   DUMP(0)
   POWEROFF
