#include "testcommon.h"

   /*
    * bitwise andi
    */
__start:
   EXNSON
   li t0, 0x12481248
   andi t1, t0, 0x0000
   andi t2, t0, 0x1111
   andi t3, t0, 0x2222
   andi t4, t0, 0x4444
   andi t5, t0, 0x8888
   andi t6, t0, 0xffff
   nop
   DUMP(0)
   POWEROFF
   
