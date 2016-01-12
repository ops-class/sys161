#include "testcommon.h"

   /*
    * div: neg / pos -> neg
    */
__start:
   EXNSON
   li t0, -9304
   li t1, 78
   div t0, t1
   nop
   nop
   nop
   DUMP(0)
   POWEROFF
