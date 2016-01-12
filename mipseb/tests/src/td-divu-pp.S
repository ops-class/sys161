#include "testcommon.h"

   /*
    * divu: pos / pos -> pos
    */
__start:
   EXNSON
   li t0, 9304
   li t1, 78
   divu t0, t1
   nop
   nop
   nop
   DUMP(0)
   POWEROFF
