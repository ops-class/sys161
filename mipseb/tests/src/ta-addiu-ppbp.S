#include "testcommon.h"
#include "basic-exn.h"

   /*
    * signed addiu: positive + positive -> out-of-range positive
    */
__start:
   EXNSON
   li t0, 0x7ffff000
   addiu t2, t0, 0x7fff
   nop
   DUMP(0)
   POWEROFF
