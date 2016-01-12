/*
 * Opcodes in the main opcode field
 */

#define OPM_SPECIAL     0
#define OPM_BCOND       1
#define OPM_J           2
#define OPM_JAL         3
#define OPM_BEQ         4
#define OPM_BNE         5
#define OPM_BLEZ        6
#define OPM_BGTZ        7
#define OPM_ADDI        8
#define OPM_ADDIU       9
#define OPM_SLTI        10
#define OPM_SLTIU       11
#define OPM_ANDI        12
#define OPM_ORI         13
#define OPM_XORI        14
#define OPM_LUI         15
#define OPM_COP0        16
#define OPM_COP1        17
#define OPM_COP2        18
#define OPM_COP3        19
#define OPM_LB          32
#define OPM_LH          33
#define OPM_LWL         34
#define OPM_LW          35
#define OPM_LBU         36
#define OPM_LHU         37
#define OPM_LWR         38
#define OPM_SB          40
#define OPM_SH          41
#define OPM_SWL         42
#define OPM_SW          43
#define OPM_SWR         46
#define OPM_CACHE	47
#define OPM_LWC0        48
#define OPM_LWC1        49
#define OPM_LWC2        50
#define OPM_LWC3        51
#define OPM_SWC0        56
#define OPM_SWC1        57
#define OPM_SWC2        58
#define OPM_SWC3        59

/*
 * Opcodes in alternate field when the main opcode field is OPM_SPECIAL
 */
#define OPS_SLL         0
#define OPS_SRL         2
#define OPS_SRA         3
#define OPS_SLLV        4
#define OPS_SRLV        6
#define OPS_SRAV        7
#define OPS_JR          8
#define OPS_JALR        9
#define OPS_SYSCALL     12
#define OPS_BREAK       13
#define OPS_SYNC	15
#define OPS_MFHI        16
#define OPS_MTHI        17
#define OPS_MFLO        18
#define OPS_MTLO        19
#define OPS_MULT        24
#define OPS_MULTU       25
#define OPS_DIV         26
#define OPS_DIVU        27
#define OPS_ADD         32
#define OPS_ADDU        33
#define OPS_SUB         34
#define OPS_SUBU        35
#define OPS_AND         36
#define OPS_OR          37
#define OPS_XOR         38
#define OPS_NOR         39
#define OPS_SLT         42
#define OPS_SLTU        43

/*
 * Complete opcode of RFE, which has some special-case crap.
 */
#define FULLOP_RFE      0x42000010
