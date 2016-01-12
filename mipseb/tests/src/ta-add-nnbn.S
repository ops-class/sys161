#include "testcommon.h"
#include "basic-exn.h"

   /*
    * signed add: negative + negative -> out-of-range negative
    */
__start:
   EXNSON
   li t0, 0x88888888
   li t1, 0x88888888
   add t2, t0, t1
   nop
   DUMP(0)
   POWEROFF
