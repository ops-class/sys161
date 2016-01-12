#include "testcommon.h"

   /*
    * divu: "neg" / "neg" -> pos
    */
__start:
   EXNSON
   li t0, -78
   li t1, -9304
   divu t0, t1
   nop
   nop
   nop
   DUMP(0)
   POWEROFF
