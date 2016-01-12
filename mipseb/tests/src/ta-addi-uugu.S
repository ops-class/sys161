#include "testcommon.h"
#include "basic-exn.h"

   /*
    * unsigned addi: positive + positive -> in-range positive
    */
__start:
   EXNSON
   li t0, 0x90000000
   addi t2, t0, 0x3000
   nop
   DUMP(0)
   POWEROFF
