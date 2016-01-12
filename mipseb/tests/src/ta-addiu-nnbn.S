#include "testcommon.h"
#include "basic-exn.h"

   /*
    * signed addiu: negative + negative -> out-of-range negative
    */
__start:
   EXNSON
   li t0, 0x80000000
   addiu t2, t0, 0xf000
   nop
   DUMP(0)
   POWEROFF
