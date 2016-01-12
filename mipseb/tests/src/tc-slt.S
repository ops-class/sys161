#include "testcommon.h"

   /*
    * slt
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
   li t0, 503655
   li t1, 707
   li t2, -533
   li t3, -625943
   li t4, 503655
   li t5, -625943
   slt s0, t1, t0
   slt s1, t0, t1
   slt s2, t0, t2
   slt s3, t2, t0
   slt s4, t3, t2
   slt s5, t2, t3
   slt s6, t0, t4
   slt s7, t3, t5
   nop
   DUMP(0)
   POWEROFF
   
