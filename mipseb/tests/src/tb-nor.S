#include "testcommon.h"

   /*
    * bitwise nor
    */
__start:
   EXNSON
   li t0, 0x12481248
   li t1, 0x00001111
   li t2, 0x22224444
   li t3, 0x8888ffff
   nor s1, t0, t1
   nor s2, t0, t2
   nor s3, t0, t3
   nop
   DUMP(0)
   POWEROFF
   
