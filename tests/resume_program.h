/* The resume test's program, shared by its generator (gen_resume.c, which
 * emits it with --resume) and the test (test_resume.c, which also loads it
 * into guest memory for the interpreter).
 *
 * main -> A -> B -> C -> a firmware call. Each level keeps its argument in
 * its own stack frame across its call and folds it into $v0 afterwards, so
 * the result says which frames ran their tails and in what order: resuming
 * at the wrong site, or climbing the chain wrongly, changes it. */
#ifndef PSPRECOMP_TEST_RESUME_PROGRAM_H
#define PSPRECOMP_TEST_RESUME_PROGRAM_H

#include <stdint.h>

#define RP_BASE   0x08804000u
#define RP_MAIN   (RP_BASE + 0 * 4)
#define RP_A      (RP_BASE + 10 * 4)
#define RP_B      (RP_BASE + 21 * 4)
#define RP_C      (RP_BASE + 33 * 4)
#define RP_STUB   (RP_BASE + 43 * 4)
#define RP_WORDS  45
#define RP_NID    0x5E5E0001u        /* the firmware call the test registers */
#define RP_DATA   0x08A00000u        /* main stores its result here */

#define RP_JAL(t)              (0x0C000000u | (((t) >> 2) & 0x03FFFFFFu))
#define RP_ADDIU(rt, rs, imm)  (0x24000000u | (rs) << 21 | (rt) << 16 | ((imm) & 0xFFFFu))
#define RP_SW(rt, off, base)   (0xAC000000u | (base) << 21 | (rt) << 16 | ((off) & 0xFFFFu))
#define RP_LW(rt, off, base)   (0x8C000000u | (base) << 21 | (rt) << 16 | ((off) & 0xFFFFu))
#define RP_ADDU(rd, rs, rt)    (0x00000021u | (rs) << 21 | (rt) << 16 | (rd) << 11)
#define RP_SLL(rd, rt, sa)     ((rt) << 16 | (rd) << 11 | (sa) << 6)
#define RP_XORI(rt, rs, imm)   (0x38000000u | (rs) << 21 | (rt) << 16 | ((imm) & 0xFFFFu))
#define RP_LUI(rt, imm)        (0x3C000000u | (rt) << 16 | ((imm) & 0xFFFFu))
#define RP_JR_RA               0x03E00008u
enum { RP_ZERO = 0, RP_V0 = 2, RP_A0 = 4, RP_T0 = 8, RP_T1 = 9, RP_SP = 29, RP_RA = 31 };

static const uint32_t RP_CODE[RP_WORDS] = {
    /* main: a0 = 3; v0 = A(3) + 100; store it */
    RP_ADDIU(RP_SP, RP_SP, -16), RP_SW(RP_RA, 12, RP_SP),
    RP_JAL(RP_A), RP_ADDIU(RP_A0, RP_ZERO, 3),
    RP_ADDIU(RP_V0, RP_V0, 100),                       /* main's return site */
    RP_LUI(RP_T0, RP_DATA >> 16), RP_SW(RP_V0, 0, RP_T0),
    RP_LW(RP_RA, 12, RP_SP), RP_JR_RA, RP_ADDIU(RP_SP, RP_SP, 16),
    /* A(a): v0 = B(a + 1) * 2 + a */
    RP_ADDIU(RP_SP, RP_SP, -16), RP_SW(RP_RA, 12, RP_SP), RP_SW(RP_A0, 8, RP_SP),
    RP_JAL(RP_B), RP_ADDIU(RP_A0, RP_A0, 1),
    RP_LW(RP_T0, 8, RP_SP),                            /* A's return site */
    RP_SLL(RP_V0, RP_V0, 1), RP_ADDU(RP_V0, RP_V0, RP_T0),
    RP_LW(RP_RA, 12, RP_SP), RP_JR_RA, RP_ADDIU(RP_SP, RP_SP, 16),
    /* B(a): v0 = C(a + 1) * 3 + a */
    RP_ADDIU(RP_SP, RP_SP, -16), RP_SW(RP_RA, 12, RP_SP), RP_SW(RP_A0, 8, RP_SP),
    RP_JAL(RP_C), RP_ADDIU(RP_A0, RP_A0, 1),
    RP_LW(RP_T0, 8, RP_SP),                            /* B's return site */
    RP_SLL(RP_T1, RP_V0, 1), RP_ADDU(RP_V0, RP_V0, RP_T1), RP_ADDU(RP_V0, RP_V0, RP_T0),
    RP_LW(RP_RA, 12, RP_SP), RP_JR_RA, RP_ADDIU(RP_SP, RP_SP, 16),
    /* C(a): v0 = (firmware(a) + a) ^ 0x55 */
    RP_ADDIU(RP_SP, RP_SP, -16), RP_SW(RP_RA, 12, RP_SP),
    RP_JAL(RP_STUB), RP_SW(RP_A0, 8, RP_SP),
    RP_LW(RP_T0, 8, RP_SP),                            /* C's return site */
    RP_ADDU(RP_V0, RP_V0, RP_T0), RP_XORI(RP_V0, RP_V0, 0x55),
    RP_LW(RP_RA, 12, RP_SP), RP_JR_RA, RP_ADDIU(RP_SP, RP_SP, 16),
    /* the import stub, as the linker leaves it */
    RP_JR_RA, 0,
};

#endif
