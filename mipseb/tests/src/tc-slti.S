#include "testcommon.h"

   /*
    * slti
    *       pos < pos -> true
    *       pos < pos -> false
    *       pos < neg -> false
    *       neg < pos -> true
    *       neg < neg -> true
    *       neg < neg -> false
    *       pos < self -> false
    *       neg < self -> false
    */
__start:
   EXNSON
   li t0, 7677
   li t1, -5844
   slti s0, t0, 9000
   slti s1, t0, 300
   slti s2, t0, -500
   slti s3, t1, 355
   slti s4, t1, -8
   slti s5, t1, -9553
   slti s6, t0, 7677
   slti s7, t1, -5844
   nop
   DUMP(0)
   POWEROFF
   
