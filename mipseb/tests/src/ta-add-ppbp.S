#include "testcommon.h"
#include "basic-exn.h"

   /*
    * signed add: positive + positive -> out-of-range positive
    */
__start:
   EXNSON
   li t0, 0x12345678
   li t1, 0x7fff8000
   add t2, t0, t1
   nop
   DUMP(0)
   POWEROFF
