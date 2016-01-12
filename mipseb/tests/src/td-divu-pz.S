#include "testcommon.h"
#include "basic-exn.h"

   /*
    * divu: pos / zero -> exception
    */
__start:
   EXNSON
   li t0, 9304
   li t1, 0
   divu t0, t1
   nop
   nop
   nop
   DUMP(0)
   POWEROFF
