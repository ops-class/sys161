#include "testcommon.h"
#include "basic-exn.h"

   /*
    * signed addi: positive + negative -> in-range positive
    */
__start:
   EXNSON
   li t0, 9304
   addi t2, t0, -577
   nop
   DUMP(0)
   POWEROFF
