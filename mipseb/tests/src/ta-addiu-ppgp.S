#include "testcommon.h"
#include "basic-exn.h"

   /*
    * signed addiu: positive + positive -> in-range positive
    */
__start:
   EXNSON
   li t0, 9304
   addiu t2, t0, 11763
   nop
   DUMP(0)
   POWEROFF
