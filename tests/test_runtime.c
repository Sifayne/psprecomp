/* Runtime tests — the semantic helpers that recompiled code lowers to.
 *
 * These are the instructions whose C translation is *not* obvious, which is
 * exactly the set where a wrong answer hides for months: division edge cases,
 * the unaligned load/store pairs, and the bitfield ops. All inputs are
 * synthetic; nothing here touches a game.
 */

#include "psprecomp/recomp_rt.h"
#include "psprecomp/clock.h"

#include <stdio.h>

static int failures;

#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);        \
            printf(__VA_ARGS__);                               \
            printf("\n");                                      \
            failures++;                                        \
        }                                                      \
    } while (0)

#define CHECK_EQ(got, want, label)                             \
    CHECK((got) == (want), "%s: got 0x%08X, want 0x%08X",       \
          (label), (unsigned)(got), (unsigned)(want))

static void test_zero_register(void) {
    /* $zero is hardwired. A recompiled store to it must be a no-op, not a
     * write — otherwise one bad decode corrupts every later read of $zero. */
    psp_cpu_reset();
    psp_set_reg(PSP_REG_ZERO, 0xDEADBEEF);
    CHECK_EQ(psp_cpu.r[PSP_REG_ZERO], 0u, "$zero stays zero");

    psp_set_reg(PSP_REG_A0, 0xDEADBEEF);
    CHECK_EQ(psp_cpu.r[PSP_REG_A0], 0xDEADBEEFu, "$a0 is writable");
}

static void test_clock_mode_report(void) {
    uint64_t guest = UINT64_MAX, wall = UINT64_MAX;
    psp_clock_realtime(0);
    psp_clock_reset();
    CHECK(psp_clock_realtime_stats(&guest, &wall) == 0,
          "virtual clock does not report a wall-time mapping");

    psp_clock_realtime(1);
    psp_clock_reset();
    CHECK(psp_clock_realtime_stats(&guest, &wall) == 1,
          "real-time clock reports its wall-time mapping");
    CHECK(guest == 0, "fresh real-time guest clock starts at zero");
    CHECK(wall < 1000000u, "fresh real-time wall clock has a sane origin");
    psp_clock_realtime(0);
}

static void test_division(void) {
    /* MIPS does not trap on divide-by-zero; HI/LO are architecturally
     * unpredictable. We define them so a recompiled game is deterministic and
     * can be diffed against the oracle rather than depending on host UB. */
    psp_div(100, 7);
    CHECK_EQ(psp_cpu.lo, 14u, "div quotient");
    CHECK_EQ(psp_cpu.hi, 2u,  "div remainder");

    psp_div(0xFFFFFF9Cu /* -100 */, 7);
    CHECK_EQ(psp_cpu.lo, (uint32_t)-14, "signed div quotient");
    CHECK_EQ(psp_cpu.hi, (uint32_t)-2,  "signed div remainder");

    psp_div(5, 0);
    CHECK_EQ(psp_cpu.hi, 5u, "div by zero leaves the dividend in hi");

    /* INT_MIN / -1 overflows in C — this must not trap or crash. */
    psp_div(0x80000000u, 0xFFFFFFFFu);
    CHECK_EQ(psp_cpu.lo, 0x80000000u, "INT_MIN / -1 quotient");
    CHECK_EQ(psp_cpu.hi, 0u,           "INT_MIN / -1 remainder");

    psp_divu(0xFFFFFFFFu, 2);
    CHECK_EQ(psp_cpu.lo, 0x7FFFFFFFu, "unsigned div treats the operand as unsigned");

    psp_divu(5, 0);
    CHECK_EQ(psp_cpu.lo, 0xFFFFFFFFu, "unsigned div by zero");
}

static void test_multiply(void) {
    psp_mult(0xFFFFFFFFu, 0xFFFFFFFFu);   /* -1 * -1 == 1 */
    CHECK_EQ(psp_cpu.lo, 1u, "signed mult lo");
    CHECK_EQ(psp_cpu.hi, 0u, "signed mult hi");

    psp_multu(0xFFFFFFFFu, 0xFFFFFFFFu);  /* 0xFFFFFFFE00000001 */
    CHECK_EQ(psp_cpu.lo, 0x00000001u, "unsigned mult lo");
    CHECK_EQ(psp_cpu.hi, 0xFFFFFFFEu, "unsigned mult hi");

    /* madd accumulates into the existing HI/LO pair. */
    psp_cpu.hi = 0; psp_cpu.lo = 10;
    psp_madd(3, 4);
    CHECK_EQ(psp_cpu.lo, 22u, "madd accumulates");

    psp_cpu.hi = 0; psp_cpu.lo = 100;
    psp_msub(3, 4);
    CHECK_EQ(psp_cpu.lo, 88u, "msub subtracts");
}

static void test_shifts(void) {
    /* C leaves shift-by->=32 undefined; MIPS masks the amount to 5 bits. */
    CHECK_EQ(psp_sll(1, 32), 1u,  "sll by 32 masks to 0");
    CHECK_EQ(psp_srl(2, 33), 1u,  "srl by 33 masks to 1");
    CHECK_EQ(psp_sra(0x80000000u, 31), 0xFFFFFFFFu, "sra sign-extends");
    CHECK_EQ(psp_srl(0x80000000u, 31), 0x00000001u, "srl does not sign-extend");

    CHECK_EQ(psp_rotr(0x12345678u, 8),  0x78123456u, "rotr by 8");
    CHECK_EQ(psp_rotr(0x12345678u, 0),  0x12345678u, "rotr by 0 is identity");
    CHECK_EQ(psp_rotr(0x12345678u, 32), 0x12345678u, "rotr by 32 is identity");
}

static void test_bitops(void) {
    CHECK_EQ(psp_clz(0x00000000u), 32u, "clz of zero");
    CHECK_EQ(psp_clz(0x80000000u), 0u,  "clz of the top bit");
    CHECK_EQ(psp_clz(0x00000001u), 31u, "clz of the bottom bit");
    CHECK_EQ(psp_clo(0xFFFFFFFFu), 32u, "clo of all-ones");
    CHECK_EQ(psp_clo(0xF0000000u), 4u,  "clo of a nibble");

    CHECK_EQ(psp_ext(0x12345678u, 4, 8),  0x67u,       "ext middle bits");
    CHECK_EQ(psp_ext(0x12345678u, 0, 32), 0x12345678u, "ext full word");
    CHECK_EQ(psp_ins(0xFFFFFFFFu, 0x0u, 8, 8), 0xFFFF00FFu, "ins clears a byte");
    CHECK_EQ(psp_ins(0x00000000u, 0xABu, 8, 8), 0x0000AB00u, "ins sets a byte");

    CHECK_EQ(psp_seb(0x000000FFu), 0xFFFFFFFFu, "seb sign-extends 0xFF");
    CHECK_EQ(psp_seb(0x0000007Fu), 0x0000007Fu, "seb leaves 0x7F positive");
    CHECK_EQ(psp_seh(0x0000FFFFu), 0xFFFFFFFFu, "seh sign-extends 0xFFFF");

    CHECK_EQ(psp_wsbh(0x12345678u), 0x34127856u, "wsbh swaps within halfwords");
    CHECK_EQ(psp_wsbw(0x12345678u), 0x78563412u, "wsbw reverses the word");

    CHECK_EQ(psp_bitrev(0x00000001u), 0x80000000u, "bitrev of bit 0");
    CHECK_EQ(psp_bitrev(0x80000000u), 0x00000001u, "bitrev of bit 31");
    CHECK_EQ(psp_bitrev(psp_bitrev(0x12345678u)), 0x12345678u, "bitrev is its own inverse");

    CHECK_EQ(psp_max(5, (uint32_t)-3), 5u,           "max is signed");
    CHECK_EQ(psp_min(5, (uint32_t)-3), (uint32_t)-3, "min is signed");
    CHECK_EQ(psp_slt((uint32_t)-1, 1), 1u, "slt is signed");
    CHECK_EQ(psp_sltu((uint32_t)-1, 1), 0u, "sltu is unsigned");
}

static void test_square_root(void) {
    /* Cover the exponent range, not just convenient values near one. The old
     * fixed-iteration Newton helper passed 1 and 4 but had not converged after
     * 24 steps for the large finite value below. That value is the squared
     * length which exposed the broken aim basis in Last Raven. */
    static const struct {
        uint32_t input;
        uint32_t output;
        const char *label;
    } cases[] = {
        { 0x00000001u, 0x1A3504F3u, "smallest subnormal" },
        { 0x00800000u, 0x20000000u, "smallest normal" },
        { 0x3F800000u, 0x3F800000u, "one" },
        { 0x40800000u, 0x40000000u, "four" },
        { 0x6473C556u, 0x51F9CF83u, "large aim-vector length" },
        { 0x7F7FFFFFu, 0x5F7FFFFFu, "largest finite" },
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const uint32_t got = psp_f32_to_bits(
            psp_fsqrt(psp_bits_to_f32(cases[i].input)));
        CHECK_EQ(got, cases[i].output, cases[i].label);
    }

    CHECK_EQ(psp_f32_to_bits(psp_fsqrt(0.0f)), 0u, "sqrt zero");
    CHECK_EQ(psp_f32_to_bits(psp_fsqrt(-4.0f)), 0u,
             "general sqrt keeps its negative-input rule");
}

/* ---- the COP1 FPU ----------------------------------------------------------
 *
 * Every row below is a line of vfpuprobe steps 150-153 as a PSP on firmware
 * 6.60 printed it: the operands, FCR31 before, and the result and FCR31
 * after. The ops are named by one letter: + - * / arithmetic, q sqrt.s,
 * n neg.s, a abs.s, m mov.s, w cvt.w.s (FCR31's mode), r t c f round/trunc/
 * ceil/floor.w.s, s cvt.s.w. */
static uint32_t fpu_apply(char op, uint32_t a, uint32_t b) {
    const float fa = psp_bits_to_f32(a), fb = psp_bits_to_f32(b);
    switch (op) {
    case '+': return psp_f32_to_bits(psp_fadd(fa, fb));
    case '-': return psp_f32_to_bits(psp_fsub(fa, fb));
    case '*': return psp_f32_to_bits(psp_fmul(fa, fb));
    case '/': return psp_f32_to_bits(psp_fdiv(fa, fb));
    case 'q': return psp_f32_to_bits(psp_fsqrt_cop1(fa));
    case 'n': return psp_f32_to_bits(psp_fneg_cop1(fa));
    case 'a': return psp_f32_to_bits(psp_fabs_cop1(fa));
    case 'm': return a;
    case 'w': return psp_f32_to_i32(fa, (int)(psp_cpu.fcr31 & 3u));
    case 'r': return psp_f32_to_i32(fa, PSP_RM_RN);
    case 't': return psp_f32_to_i32(fa, PSP_RM_RZ);
    case 'c': return psp_f32_to_i32(fa, PSP_RM_RP);
    case 'f': return psp_f32_to_i32(fa, PSP_RM_RM);
    case 's': return psp_f32_to_bits(psp_cvt_s_w(a));
    default:  return 0xDEADDEADu;
    }
}

static void test_fpu(void) {
    static const struct {
        const char *label;
        char op;
        uint32_t a, b, fcr_in, want, fcr_out;
    } rows[] = {
        { "sqrt(-1)", 'q', 0xBF800000, 0x00000000, 0x00000000, 0x7FC00000, 0x00010040 },
        { "0/0", '/', 0x00000000, 0x00000000, 0x00000000, 0x7FC00000, 0x00010040 },
        { "nan*nan", '*', 0x7FC00000, 0x7FC00000, 0x00000000, 0x7FC00000, 0x00000000 },
        { "max*max", '*', 0x7F7FFFFF, 0x7F7FFFFF, 0x00000000, 0x7F800000, 0x00005014 },
        { "1/max", '/', 0x3F800000, 0x7F7FFFFF, 0x00000000, 0x00200000, 0x0000300C },
        { "1/3", '/', 0x3F800000, 0x40400000, 0x00000000, 0x3EAAAAAB, 0x00001004 },
        { "1/0", '/', 0x3F800000, 0x00000000, 0x00000000, 0x7F800000, 0x00008020 },
        { "-1/0", '/', 0xBF800000, 0x00000000, 0x00000000, 0xFF800000, 0x00008020 },
        { "inf-inf", '-', 0x7F800000, 0x7F800000, 0x00000000, 0x7FC00000, 0x00010040 },
        { "snan+1", '+', 0x7F800001, 0x3F800000, 0x00000000, 0x7FC00001, 0x00010040 },
        { "1+1", '+', 0x3F800000, 0x3F800000, 0x00000000, 0x40000000, 0x00000000 },
        { "1+1 flags", '+', 0x3F800000, 0x3F800000, 0x0000007C, 0x40000000, 0x0000007C },
        { "den+0", '+', 0x00000001, 0x00000000, 0x00000000, 0x00000001, 0x00000000 },
        { "den*1", '*', 0x00400000, 0x3F800000, 0x00000000, 0x00400000, 0x00002008 },
        { "tiny*tiny", '*', 0x1E3CE508, 0x1E3CE508, 0x00000000, 0x000116C2, 0x0000300C },
        { "cvt.w 3e9", 'w', 0x4F32D05E, 0x00000000, 0x00000000, 0x7FFFFFFF, 0x00010040 },
        { "cvt.w nan", 'w', 0x7FC00000, 0x00000000, 0x00000000, 0x7FFFFFFF, 0x00010040 },
        { "cvt.w inf", 'w', 0x7F800000, 0x00000000, 0x00000000, 0x7FFFFFFF, 0x00010040 },
        { "cvt.w -inf", 'w', 0xFF800000, 0x00000000, 0x00000000, 0x80000000, 0x00010040 },
        { "cvt.w 2^31", 'w', 0x4F000000, 0x00000000, 0x00000000, 0x7FFFFFFF, 0x00010040 },
        { "cvt.s.w big", 's', 0x7FFFFFFF, 0x00000000, 0x00000000, 0x4F000000, 0x00001004 },
        { "neg nan", 'n', 0x7FC00000, 0x00000000, 0x00000000, 0x7FC00000, 0x00000000 },
        { "abs -nan", 'a', 0xFFC00000, 0x00000000, 0x00000000, 0xFFC00000, 0x00000000 },
        { "mov snan", 'm', 0x7F800001, 0x00000000, 0x00000000, 0x7F800001, 0x00000000 },
        { "neg snan", 'n', 0x7F800001, 0x00000000, 0x00000000, 0x7FC00001, 0x00010040 },
        { "1/3", '/', 0x3F800000, 0x40400000, 0x00000000, 0x3EAAAAAB, 0x00001004 },
        { "-1/3", '/', 0xBF800000, 0x40400000, 0x00000000, 0xBEAAAAAB, 0x00001004 },
        { "1.1*1.1", '*', 0x3F8CCCCD, 0x3F8CCCCD, 0x00000000, 0x3F9AE148, 0x00001004 },
        { "1+2^-24*3", '+', 0x3F800000, 0x34400000, 0x00000000, 0x3F800002, 0x00001004 },
        { "1-2^-25", '-', 0x3F800000, 0x33000000, 0x00000000, 0x3F800000, 0x00001004 },
        { "sqrt 2", 'q', 0x40000000, 0x00000000, 0x00000000, 0x3FB504F3, 0x00000000 },
        { "cvt.w 2.5", 'w', 0x40200000, 0x00000000, 0x00000000, 0x00000002, 0x00001004 },
        { "cvt.w -2.5", 'w', 0xC0200000, 0x00000000, 0x00000000, 0xFFFFFFFE, 0x00001004 },
        { "cvt.w 1.5", 'w', 0x3FC00000, 0x00000000, 0x00000000, 0x00000002, 0x00001004 },
        { "cvt.w -0.5", 'w', 0xBF000000, 0x00000000, 0x00000000, 0x00000000, 0x00001004 },
        { "round.w 2.5", 'r', 0x40200000, 0x00000000, 0x00000000, 0x00000002, 0x00001004 },
        { "trunc.w -2.5", 't', 0xC0200000, 0x00000000, 0x00000000, 0xFFFFFFFE, 0x00001004 },
        { "ceil.w 2.1", 'c', 0x40066666, 0x00000000, 0x00000000, 0x00000003, 0x00001004 },
        { "floor.w -2.1", 'f', 0xC0066666, 0x00000000, 0x00000000, 0xFFFFFFFD, 0x00001004 },
        { "cvt.s.w 2^24+1", 's', 0x01000001, 0x00000000, 0x00000000, 0x4B800000, 0x00001004 },
        { "cvt.s.w -(2^24+1)", 's', 0xFEFFFFFF, 0x00000000, 0x00000000, 0xCB800000, 0x00001004 },
        { "max+max", '+', 0x7F7FFFFF, 0x7F7FFFFF, 0x00000000, 0x7F800000, 0x00004010 },
        /* div.s overflow is O without I (v3 step 169), unlike mul.s. */
        { "max/0.5", '/', 0x7F7FFFFF, 0x3F000000, 0x00000000, 0x7F800000, 0x00004010 },
        { "-max/0.5 RP", '/', 0xFF7FFFFF, 0x3F000000, 0x00000002, 0xFF7FFFFF, 0x00004012 },
        { "max/den RZ", '/', 0x7F7FFFFF, 0x00000001, 0x00000001, 0x7F7FFFFF, 0x00004011 },
        { "1/2^-128 FS", '/', 0x3F800000, 0x00200000, 0x01000000, 0x7F800000, 0x01004010 },
        { "-max-max", '-', 0xFF7FFFFF, 0x7F7FFFFF, 0x00000000, 0xFF800000, 0x00004010 },
        { "tiny/2", '/', 0x00800001, 0x40000000, 0x00000000, 0x00400000, 0x0000300C },
        { "1/3", '/', 0x3F800000, 0x40400000, 0x00000001, 0x3EAAAAAA, 0x00001005 },
        { "-1/3", '/', 0xBF800000, 0x40400000, 0x00000001, 0xBEAAAAAA, 0x00001005 },
        { "1.1*1.1", '*', 0x3F8CCCCD, 0x3F8CCCCD, 0x00000001, 0x3F9AE148, 0x00001005 },
        { "1+2^-24*3", '+', 0x3F800000, 0x34400000, 0x00000001, 0x3F800001, 0x00001005 },
        { "1-2^-25", '-', 0x3F800000, 0x33000000, 0x00000001, 0x3F7FFFFF, 0x00001005 },
        { "sqrt 2", 'q', 0x40000000, 0x00000000, 0x00000001, 0x3FB504F3, 0x00000001 },
        { "cvt.w 2.5", 'w', 0x40200000, 0x00000000, 0x00000001, 0x00000002, 0x00001005 },
        { "cvt.w -2.5", 'w', 0xC0200000, 0x00000000, 0x00000001, 0xFFFFFFFE, 0x00001005 },
        { "cvt.w 1.5", 'w', 0x3FC00000, 0x00000000, 0x00000001, 0x00000001, 0x00001005 },
        { "cvt.w -0.5", 'w', 0xBF000000, 0x00000000, 0x00000001, 0x00000000, 0x00001005 },
        { "round.w 2.5", 'r', 0x40200000, 0x00000000, 0x00000001, 0x00000002, 0x00001005 },
        { "trunc.w -2.5", 't', 0xC0200000, 0x00000000, 0x00000001, 0xFFFFFFFE, 0x00001005 },
        { "ceil.w 2.1", 'c', 0x40066666, 0x00000000, 0x00000001, 0x00000003, 0x00001005 },
        { "floor.w -2.1", 'f', 0xC0066666, 0x00000000, 0x00000001, 0xFFFFFFFD, 0x00001005 },
        { "cvt.s.w 2^24+1", 's', 0x01000001, 0x00000000, 0x00000001, 0x4B800000, 0x00001005 },
        { "cvt.s.w -(2^24+1)", 's', 0xFEFFFFFF, 0x00000000, 0x00000001, 0xCB800000, 0x00001005 },
        { "max+max", '+', 0x7F7FFFFF, 0x7F7FFFFF, 0x00000001, 0x7F7FFFFF, 0x00004011 },
        { "-max-max", '-', 0xFF7FFFFF, 0x7F7FFFFF, 0x00000001, 0xFF7FFFFF, 0x00004011 },
        { "tiny/2", '/', 0x00800001, 0x40000000, 0x00000001, 0x00400000, 0x0000300D },
        { "1/3", '/', 0x3F800000, 0x40400000, 0x00000002, 0x3EAAAAAB, 0x00001006 },
        { "-1/3", '/', 0xBF800000, 0x40400000, 0x00000002, 0xBEAAAAAA, 0x00001006 },
        { "1.1*1.1", '*', 0x3F8CCCCD, 0x3F8CCCCD, 0x00000002, 0x3F9AE149, 0x00001006 },
        { "1+2^-24*3", '+', 0x3F800000, 0x34400000, 0x00000002, 0x3F800002, 0x00001006 },
        { "1-2^-25", '-', 0x3F800000, 0x33000000, 0x00000002, 0x3F800000, 0x00001006 },
        { "sqrt 2", 'q', 0x40000000, 0x00000000, 0x00000002, 0x3FB504F4, 0x00000002 },
        { "cvt.w 2.5", 'w', 0x40200000, 0x00000000, 0x00000002, 0x00000003, 0x00001006 },
        { "cvt.w -2.5", 'w', 0xC0200000, 0x00000000, 0x00000002, 0xFFFFFFFE, 0x00001006 },
        { "cvt.w 1.5", 'w', 0x3FC00000, 0x00000000, 0x00000002, 0x00000002, 0x00001006 },
        { "cvt.w -0.5", 'w', 0xBF000000, 0x00000000, 0x00000002, 0x00000000, 0x00001006 },
        { "round.w 2.5", 'r', 0x40200000, 0x00000000, 0x00000002, 0x00000002, 0x00001006 },
        { "trunc.w -2.5", 't', 0xC0200000, 0x00000000, 0x00000002, 0xFFFFFFFE, 0x00001006 },
        { "ceil.w 2.1", 'c', 0x40066666, 0x00000000, 0x00000002, 0x00000003, 0x00001006 },
        { "floor.w -2.1", 'f', 0xC0066666, 0x00000000, 0x00000002, 0xFFFFFFFD, 0x00001006 },
        { "cvt.s.w 2^24+1", 's', 0x01000001, 0x00000000, 0x00000002, 0x4B800001, 0x00001006 },
        { "cvt.s.w -(2^24+1)", 's', 0xFEFFFFFF, 0x00000000, 0x00000002, 0xCB800000, 0x00001006 },
        { "max+max", '+', 0x7F7FFFFF, 0x7F7FFFFF, 0x00000002, 0x7F800000, 0x00004012 },
        { "-max-max", '-', 0xFF7FFFFF, 0x7F7FFFFF, 0x00000002, 0xFF7FFFFF, 0x00004012 },
        { "tiny/2", '/', 0x00800001, 0x40000000, 0x00000002, 0x00400001, 0x0000300E },
        { "1/3", '/', 0x3F800000, 0x40400000, 0x00000003, 0x3EAAAAAA, 0x00001007 },
        { "-1/3", '/', 0xBF800000, 0x40400000, 0x00000003, 0xBEAAAAAB, 0x00001007 },
        { "1.1*1.1", '*', 0x3F8CCCCD, 0x3F8CCCCD, 0x00000003, 0x3F9AE148, 0x00001007 },
        { "1+2^-24*3", '+', 0x3F800000, 0x34400000, 0x00000003, 0x3F800001, 0x00001007 },
        { "1-2^-25", '-', 0x3F800000, 0x33000000, 0x00000003, 0x3F7FFFFF, 0x00001007 },
        { "sqrt 2", 'q', 0x40000000, 0x00000000, 0x00000003, 0x3FB504F3, 0x00000003 },
        { "cvt.w 2.5", 'w', 0x40200000, 0x00000000, 0x00000003, 0x00000002, 0x00001007 },
        { "cvt.w -2.5", 'w', 0xC0200000, 0x00000000, 0x00000003, 0xFFFFFFFD, 0x00001007 },
        { "cvt.w 1.5", 'w', 0x3FC00000, 0x00000000, 0x00000003, 0x00000001, 0x00001007 },
        { "cvt.w -0.5", 'w', 0xBF000000, 0x00000000, 0x00000003, 0xFFFFFFFF, 0x00001007 },
        { "round.w 2.5", 'r', 0x40200000, 0x00000000, 0x00000003, 0x00000002, 0x00001007 },
        { "trunc.w -2.5", 't', 0xC0200000, 0x00000000, 0x00000003, 0xFFFFFFFE, 0x00001007 },
        { "ceil.w 2.1", 'c', 0x40066666, 0x00000000, 0x00000003, 0x00000003, 0x00001007 },
        { "floor.w -2.1", 'f', 0xC0066666, 0x00000000, 0x00000003, 0xFFFFFFFD, 0x00001007 },
        { "cvt.s.w 2^24+1", 's', 0x01000001, 0x00000000, 0x00000003, 0x4B800000, 0x00001007 },
        { "cvt.s.w -(2^24+1)", 's', 0xFEFFFFFF, 0x00000000, 0x00000003, 0xCB800001, 0x00001007 },
        { "max+max", '+', 0x7F7FFFFF, 0x7F7FFFFF, 0x00000003, 0x7F7FFFFF, 0x00004013 },
        { "-max-max", '-', 0xFF7FFFFF, 0x7F7FFFFF, 0x00000003, 0xFF800000, 0x00004013 },
        { "tiny/2", '/', 0x00800001, 0x40000000, 0x00000003, 0x00400000, 0x0000300F },
        { "tiny*tiny", '*', 0x1E3CE508, 0x1E3CE508, 0x01000000, 0x00000000, 0x01002008 },
        { "den+0", '+', 0x00000001, 0x00000000, 0x01000000, 0x00000001, 0x01000000 },
        { "den*1", '*', 0x00400000, 0x3F800000, 0x01000000, 0x00000000, 0x01002008 },
        { "min/2", '/', 0x00800000, 0x40000000, 0x01000000, 0x00000000, 0x01002008 },
        { "-min/2", '/', 0x80800000, 0x40000000, 0x01000000, 0x80000000, 0x01002008 },
        { "den+min", '+', 0x00400000, 0x00800000, 0x01000000, 0x00C00000, 0x01000000 },
        { "cvt.w den", 'w', 0x00400000, 0x00000000, 0x01000000, 0x00000000, 0x01001004 },
        { "mov den", 'm', 0x00400000, 0x00000000, 0x01000000, 0x00400000, 0x01000000 },
        { "neg den", 'n', 0x00400000, 0x00000000, 0x01000000, 0x80400000, 0x01000000 },
    };
    psp_cpu_reset();
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        psp_cpu.fcr31 = rows[i].fcr_in;
        const uint32_t got = fpu_apply(rows[i].op, rows[i].a, rows[i].b);
        CHECK(got == rows[i].want && psp_cpu.fcr31 == rows[i].fcr_out,
              "%s (fcr %08X): got %08X fcr %08X, want %08X fcr %08X", rows[i].label,
              rows[i].fcr_in, got, psp_cpu.fcr31, rows[i].want, rows[i].fcr_out);
    }

    /* c.cond.s, step 153: condition, a, then the FCC for each b in `vals`
     * (bit i for vals[i]) and the V the row raised. Only olt, ole, lt and le
     * raise it, for any NaN. */
    static const uint32_t vals[7] = {
        0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x7F800000, 0x7FC00000, 0x7F800001,
    };
    static const struct { unsigned cond; uint32_t a, fcc, seen; } cmps[] = {
        {  0, 0x00000000, 0x00, 0x00000000 },
        {  0, 0x80000000, 0x00, 0x00000000 },
        {  0, 0x3F800000, 0x00, 0x00000000 },
        {  0, 0xBF800000, 0x00, 0x00000000 },
        {  0, 0x7F800000, 0x00, 0x00000000 },
        {  0, 0x7FC00000, 0x00, 0x00000000 },
        {  0, 0x7F800001, 0x00, 0x00000000 },
        {  1, 0x00000000, 0x60, 0x00000000 },
        {  1, 0x80000000, 0x60, 0x00000000 },
        {  1, 0x3F800000, 0x60, 0x00000000 },
        {  1, 0xBF800000, 0x60, 0x00000000 },
        {  1, 0x7F800000, 0x60, 0x00000000 },
        {  1, 0x7FC00000, 0x7F, 0x00000000 },
        {  1, 0x7F800001, 0x7F, 0x00000000 },
        {  2, 0x00000000, 0x03, 0x00000000 },
        {  2, 0x80000000, 0x03, 0x00000000 },
        {  2, 0x3F800000, 0x04, 0x00000000 },
        {  2, 0xBF800000, 0x08, 0x00000000 },
        {  2, 0x7F800000, 0x10, 0x00000000 },
        {  2, 0x7FC00000, 0x00, 0x00000000 },
        {  2, 0x7F800001, 0x00, 0x00000000 },
        {  3, 0x00000000, 0x63, 0x00000000 },
        {  3, 0x80000000, 0x63, 0x00000000 },
        {  3, 0x3F800000, 0x64, 0x00000000 },
        {  3, 0xBF800000, 0x68, 0x00000000 },
        {  3, 0x7F800000, 0x70, 0x00000000 },
        {  3, 0x7FC00000, 0x7F, 0x00000000 },
        {  3, 0x7F800001, 0x7F, 0x00000000 },
        {  4, 0x00000000, 0x14, 0x00010040 },
        {  4, 0x80000000, 0x14, 0x00010040 },
        {  4, 0x3F800000, 0x10, 0x00010040 },
        {  4, 0xBF800000, 0x17, 0x00010040 },
        {  4, 0x7F800000, 0x00, 0x00010040 },
        {  4, 0x7FC00000, 0x00, 0x00010040 },
        {  4, 0x7F800001, 0x00, 0x00010040 },
        {  5, 0x00000000, 0x74, 0x00000000 },
        {  5, 0x80000000, 0x74, 0x00000000 },
        {  5, 0x3F800000, 0x70, 0x00000000 },
        {  5, 0xBF800000, 0x77, 0x00000000 },
        {  5, 0x7F800000, 0x60, 0x00000000 },
        {  5, 0x7FC00000, 0x7F, 0x00000000 },
        {  5, 0x7F800001, 0x7F, 0x00000000 },
        {  6, 0x00000000, 0x17, 0x00010040 },
        {  6, 0x80000000, 0x17, 0x00010040 },
        {  6, 0x3F800000, 0x14, 0x00010040 },
        {  6, 0xBF800000, 0x1F, 0x00010040 },
        {  6, 0x7F800000, 0x10, 0x00010040 },
        {  6, 0x7FC00000, 0x00, 0x00010040 },
        {  6, 0x7F800001, 0x00, 0x00010040 },
        {  7, 0x00000000, 0x77, 0x00000000 },
        {  7, 0x80000000, 0x77, 0x00000000 },
        {  7, 0x3F800000, 0x74, 0x00000000 },
        {  7, 0xBF800000, 0x7F, 0x00000000 },
        {  7, 0x7F800000, 0x70, 0x00000000 },
        {  7, 0x7FC00000, 0x7F, 0x00000000 },
        {  7, 0x7F800001, 0x7F, 0x00000000 },
        {  8, 0x00000000, 0x00, 0x00000000 },
        {  8, 0x80000000, 0x00, 0x00000000 },
        {  8, 0x3F800000, 0x00, 0x00000000 },
        {  8, 0xBF800000, 0x00, 0x00000000 },
        {  8, 0x7F800000, 0x00, 0x00000000 },
        {  8, 0x7FC00000, 0x00, 0x00000000 },
        {  8, 0x7F800001, 0x00, 0x00000000 },
        {  9, 0x00000000, 0x60, 0x00000000 },
        {  9, 0x80000000, 0x60, 0x00000000 },
        {  9, 0x3F800000, 0x60, 0x00000000 },
        {  9, 0xBF800000, 0x60, 0x00000000 },
        {  9, 0x7F800000, 0x60, 0x00000000 },
        {  9, 0x7FC00000, 0x7F, 0x00000000 },
        {  9, 0x7F800001, 0x7F, 0x00000000 },
        { 10, 0x00000000, 0x03, 0x00000000 },
        { 10, 0x80000000, 0x03, 0x00000000 },
        { 10, 0x3F800000, 0x04, 0x00000000 },
        { 10, 0xBF800000, 0x08, 0x00000000 },
        { 10, 0x7F800000, 0x10, 0x00000000 },
        { 10, 0x7FC00000, 0x00, 0x00000000 },
        { 10, 0x7F800001, 0x00, 0x00000000 },
        { 11, 0x00000000, 0x63, 0x00000000 },
        { 11, 0x80000000, 0x63, 0x00000000 },
        { 11, 0x3F800000, 0x64, 0x00000000 },
        { 11, 0xBF800000, 0x68, 0x00000000 },
        { 11, 0x7F800000, 0x70, 0x00000000 },
        { 11, 0x7FC00000, 0x7F, 0x00000000 },
        { 11, 0x7F800001, 0x7F, 0x00000000 },
        { 12, 0x00000000, 0x14, 0x00010040 },
        { 12, 0x80000000, 0x14, 0x00010040 },
        { 12, 0x3F800000, 0x10, 0x00010040 },
        { 12, 0xBF800000, 0x17, 0x00010040 },
        { 12, 0x7F800000, 0x00, 0x00010040 },
        { 12, 0x7FC00000, 0x00, 0x00010040 },
        { 12, 0x7F800001, 0x00, 0x00010040 },
        { 13, 0x00000000, 0x74, 0x00000000 },
        { 13, 0x80000000, 0x74, 0x00000000 },
        { 13, 0x3F800000, 0x70, 0x00000000 },
        { 13, 0xBF800000, 0x77, 0x00000000 },
        { 13, 0x7F800000, 0x60, 0x00000000 },
        { 13, 0x7FC00000, 0x7F, 0x00000000 },
        { 13, 0x7F800001, 0x7F, 0x00000000 },
        { 14, 0x00000000, 0x17, 0x00010040 },
        { 14, 0x80000000, 0x17, 0x00010040 },
        { 14, 0x3F800000, 0x14, 0x00010040 },
        { 14, 0xBF800000, 0x1F, 0x00010040 },
        { 14, 0x7F800000, 0x10, 0x00010040 },
        { 14, 0x7FC00000, 0x00, 0x00010040 },
        { 14, 0x7F800001, 0x00, 0x00010040 },
        { 15, 0x00000000, 0x77, 0x00000000 },
        { 15, 0x80000000, 0x77, 0x00000000 },
        { 15, 0x3F800000, 0x74, 0x00000000 },
        { 15, 0xBF800000, 0x7F, 0x00000000 },
        { 15, 0x7F800000, 0x70, 0x00000000 },
        { 15, 0x7FC00000, 0x7F, 0x00000000 },
        { 15, 0x7F800001, 0x7F, 0x00000000 },
    };
    for (size_t i = 0; i < sizeof cmps / sizeof cmps[0]; i++) {
        uint32_t fcc = 0, seen = 0;
        for (int j = 0; j < 7; j++) {
            psp_cpu.fcr31 = 0;
            fcc |= (uint32_t)psp_fcmp(cmps[i].cond, psp_bits_to_f32(cmps[i].a),
                                      psp_bits_to_f32(vals[j])) << j;
            seen |= psp_cpu.fcr31;
        }
        CHECK(fcc == cmps[i].fcc && seen == cmps[i].seen,
              "c.cond %u a=%08X: fcc %02X seen %08X, want %02X %08X", cmps[i].cond,
              cmps[i].a, fcc, seen, cmps[i].fcc, cmps[i].seen);
    }

    /* ctc1 of the E cause bit is an FPU exception (step 160 powered the PSP
     * off): the write is refused and reported. Cause bits alone are stored,
     * as step 149 read them back. */
    psp_cpu.fcr31 = 0x00000E00u;
    CHECK(psp_fcr_write(31, 0x00020000u) != 0 && psp_cpu.fcr31 == 0x00000E00u,
          "ctc1 of the E bit is refused, fcr31 %08X", psp_cpu.fcr31);
    CHECK(psp_fcr_write(31, 0x0001F000u) == 0 && psp_cpu.fcr31 == 0x0001F000u,
          "ctc1 of the other cause bits stores them, fcr31 %08X", psp_cpu.fcr31);
    CHECK(psp_fcr_write(1, 0x00020000u) == 0, "only fcr31 has an E bit");

    /* A cause bit written together with its enable is one too (v3 step 192,
     * 00008400), and so is an operation that raises an enabled exception
     * (steps 189-191: 1/0, 0/0 and max*max under the power-on 00000E00). */
    psp_cpu.fcr31 = 0x00000E00u;
    CHECK(psp_fcr_write(31, 0x00008400u) != 0 && psp_cpu.fcr31 == 0x00000E00u,
          "ctc1 of the Z cause with the Z enable is refused, fcr31 %08X", psp_cpu.fcr31);
    CHECK(psp_fcr_write(31, 0x00004400u) == 0 && !psp_fpu_trap_pending(),
          "ctc1 of the O cause with only the Z enable stores it, fcr31 %08X", psp_cpu.fcr31);
    static const struct { char op; uint32_t a, b; int traps; } enabled[] = {
        { '/', 0x3F800000, 0x00000000, 1 }, { '/', 0x00000000, 0x00000000, 1 },
        { '*', 0x7F7FFFFF, 0x7F7FFFFF, 1 }, { '/', 0x3F800000, 0x40400000, 0 },
        { '*', 0x1E3CE508, 0x1E3CE508, 0 }, { 'q', 0xBF800000, 0x00000000, 1 },
    };
    for (size_t i = 0; i < sizeof enabled / sizeof enabled[0]; i++) {
        psp_cpu.fcr31 = 0x00000E00u;
        (void)fpu_apply(enabled[i].op, enabled[i].a, enabled[i].b);
        CHECK(psp_fpu_trap_pending() == enabled[i].traps,
              "%c %08X %08X under 00000E00: trap %d, want %d", enabled[i].op, enabled[i].a,
              enabled[i].b, psp_fpu_trap_pending(), enabled[i].traps);
    }
    psp_cpu.fcr31 = 0;
}

static unsigned observed_writes;
static uint32_t observed_addr, observed_size, observed_value;
static unsigned accesses; static uint32_t access_addr, access_size; static uint8_t access_seen_value;
static const uint8_t *access_raw;   /* host byte of the watched address, taken before arming */
static void observe_access(uint32_t addr, uint32_t size) {
    accesses++; access_addr = addr; access_size = size;
    /* Not through psp_read8: the observer is re-entered by its own pointer
     * lookups, and a backend retires its state before it writes. The raw
     * byte is what the guest's store has not yet changed. */
    access_seen_value = *access_raw;
}
static void observe_write(uint32_t addr, uint32_t size) {
    observed_writes++; observed_addr=addr; observed_size=size;
    observed_value=psp_read8(addr);
}

static void test_memory(void) {
    CHECK(psp_mem_init() == 0, "memory init");

    const uint32_t tracked = PSP_VRAM_BASE + 0x1000u;
    const uint32_t nearby  = tracked + 0x80u;
    const uint32_t remote  = tracked + 0x1000u;
    const uint64_t initial_serial = psp_mem_write_serial();
    CHECK(initial_serial != 0, "memory reset has a cache-visible serial");
    CHECK(psp_mem_range_generation(tracked, 4) == 0,
          "fresh range has generation zero");
    psp_mem_set_write_observer(observe_write);
    psp_write8(0x44001000u, 0x5A);
    CHECK(observed_writes==1 && observed_addr==tracked && observed_size==1 && observed_value==0x5A,
          "write observer sees normalized address and bytes after the store");
    psp_write8(tracked, 0x5A);
    CHECK(observed_writes==2,"same-value stores still notify GPU ownership");
    (void)psp_read8(tracked);
    CHECK(observed_writes==2,"reads do not notify write observer");
    {
        const uint32_t far = tracked + 0x8000u;   /* touched by nothing below */
        psp_mem_set_write_observer_range(tracked, tracked + 0x40u);
        psp_write8(far, 0x5A);
        CHECK(observed_writes==2,"a write outside the observer's range is not reported");
        psp_write8(tracked + 0x3Fu, 0x5A);
        CHECK(observed_writes==3,"a write inside the observer's range is reported");
        psp_mem_set_write_observer(observe_write);
        psp_write8(far, 0x5B);
        CHECK(observed_writes==4,"setting an observer resets its range to everything");
        observed_writes = 2;   /* the checks below were written against this count */
    }
    psp_mem_set_write_observer(NULL);

    /* The VRAM access observer runs before the pointer is handed out, for a
     * load or a store, only inside its range, and only while armed. */
    accesses = 0; access_addr = access_size = 0; access_seen_value = 0;
    psp_write8(nearby, 0x11);
    access_raw = (const uint8_t *)psp_mem_ptr(nearby, 1);
    psp_mem_set_vram_access_observer(observe_access, tracked, tracked + 0x100u);
    CHECK(psp_read8(nearby) == 0x11 && accesses == 1 && access_addr == nearby && access_size == 1,
          "access observer sees a load inside the range, before it");
    psp_write8(nearby, 0x22);
    CHECK(accesses == 2 && access_seen_value == 0x11,
          "access observer runs before a store lands");
    (void)psp_read32(remote);
    (void)psp_read32(PSP_RAM_BASE + 0x1000u);
    CHECK(accesses == 2, "accesses outside the range do not reach the observer");
    (void)psp_read32(tracked + 0xFEu);
    CHECK(accesses == 3, "an access straddling the range's end is inside it");
    psp_mem_set_vram_access_observer(NULL, 0, 0);
    (void)psp_read8(nearby);
    CHECK(accesses == 3, "a removed access observer is not called");

    psp_write32(tracked, 0xAABBCCDDu);
    const uint64_t first_generation = psp_mem_range_generation(tracked, 4);
    CHECK(first_generation > initial_serial, "scalar store marks its range");
    CHECK(psp_mem_range_generation(nearby, 4) == first_generation,
          "nearby bytes conservatively share a dirty granule");
    CHECK(psp_mem_range_generation(remote, 4) == 0,
          "a remote range stays clean");
    CHECK(psp_mem_range_generation(0x44001000u, 4) == first_generation,
          "cache mirror shares the write generation");

    const uint8_t block[4] = { 1, 2, 3, 4 };
    CHECK(psp_mem_write_block(remote, block, sizeof block) == 0,
          "block write succeeds");
    CHECK(psp_mem_range_generation(remote, sizeof block) > first_generation,
          "block write marks its destination");

    const uint32_t edge = tracked + 0xFFu;
    psp_write16(edge, 0x7788u);
    const uint64_t edge_generation = psp_mem_range_generation(edge, 2);
    CHECK(edge_generation == psp_mem_range_generation(edge, 1) &&
          edge_generation == psp_mem_range_generation(edge + 1, 1),
          "a write crossing a granule marks both sides");

    const uint64_t before_raw = psp_mem_write_serial();
    void *raw = psp_mem_ptr(remote + 0x1000u, 16);
    CHECK(raw != NULL, "raw write destination maps");
    if (raw) {
        memset(raw, 0x5A, 16);
        CHECK(psp_mem_write_serial() == before_raw,
              "raw pointer write is invisible until explicitly marked");
        psp_mem_mark_write(remote + 0x1000u, 16);
        CHECK(psp_mem_write_serial() > before_raw,
              "explicit mark advances the write serial");
    }

    const uint64_t before_read = psp_mem_write_serial();
    (void)psp_read32(tracked);
    CHECK(psp_mem_write_serial() == before_read,
          "reads do not change the write serial");
    CHECK(psp_mem_range_generation(0x01000000u, 4) == 0,
          "unmapped ranges have no generation");

    CHECK(psp_mem_map_module(0x00100000u, 0x1000u) == 0,
          "module region maps for generation test");
    psp_write8(0x00100100u, 0x42u);
    CHECK(psp_mem_range_generation(0x00100100u, 1) != 0,
          "module image writes carry generations too");

    psp_write32(0x08800000u, 0x12345678u);
    CHECK_EQ(psp_read32(0x08800000u), 0x12345678u, "round-trip a word");
    CHECK_EQ(psp_read8 (0x08800000u), 0x78u, "little-endian byte 0");
    CHECK_EQ(psp_read8 (0x08800003u), 0x12u, "little-endian byte 3");
    CHECK_EQ(psp_read16(0x08800000u), 0x5678u, "little-endian halfword");

    /* The three cache-behaviour mirrors must alias the same storage. */
    CHECK_EQ(psp_read32(0x48800000u), 0x12345678u, "uncached mirror aliases RAM");
    CHECK_EQ(psp_read32(0x88800000u), 0x12345678u, "kernel mirror aliases RAM");

    /* Unmapped access is counted, not fatal — the counter is how a recompiled
     * game tells you the analysis missed something. */
    uint64_t before = psp_mem_bad_access;
    CHECK_EQ(psp_read32(0x00000000u), 0u, "unmapped read returns zero");
    CHECK(psp_mem_bad_access == before + 1, "unmapped read is counted");

    /* An access straddling the end of a region must be rejected outright
     * rather than reading past the allocation. */
    before = psp_mem_bad_access;
    (void)psp_read32(PSP_VRAM_BASE + PSP_VRAM_SIZE - 2);
    CHECK(psp_mem_bad_access == before + 1, "straddling read is rejected");

    const uint64_t before_reset = psp_mem_write_serial();
    psp_mem_set_write_observer(observe_write);
    psp_mem_free();
    CHECK(psp_mem_init() == 0, "memory reinitialises");
    CHECK(psp_mem_write_serial() > before_reset,
          "memory reset invalidates surviving external caches");
    CHECK(psp_mem_range_generation(tracked, 4) == 0,
          "memory reset restores pristine range generations");
    psp_write8(tracked, 0xCC);
    CHECK(observed_writes==2,"memory reset removes the old GPU observer");
    psp_mem_set_vram_access_observer(observe_access, tracked, tracked + 0x100u);
    psp_mem_free();
    CHECK(psp_mem_init() == 0, "memory reinitialises again");
    accesses = 0;
    (void)psp_read8(tracked);
    CHECK(accesses == 0, "memory reset removes the old access observer");
    psp_mem_free();
}

static void test_unaligned(void) {
    CHECK(psp_mem_init() == 0, "memory init");

    /* Lay down a known byte pattern: 00 11 22 33 44 55 66 77. */
    for (uint32_t i = 0; i < 8; i++)
        psp_write8(0x08800000u + i, (uint8_t)(i * 0x11));

    /* The LE compiler idiom for an unaligned 32-bit load at address A is
     *     lwr rt, A       lwl rt, A+3
     * Bytes 1..4 are 11 22 33 44, so the loaded word must be 0x44332211. */
    uint32_t rt = 0xCCCCCCCCu;
    rt = psp_lwr(rt, 0x08800001u);
    rt = psp_lwl(rt, 0x08800004u);
    CHECK_EQ(rt, 0x44332211u, "unaligned load via lwr+lwl");

    /* The store idiom, mirrored. Writing 0xAABBCCDD at address 1 must leave
     * bytes 1..4 as DD CC BB AA and must not disturb bytes 0, 5, 6, 7. */
    psp_swr(0xAABBCCDDu, 0x08800001u);
    psp_swl(0xAABBCCDDu, 0x08800004u);
    CHECK_EQ(psp_read8(0x08800000u), 0x00u, "swl/swr left byte 0 alone");
    CHECK_EQ(psp_read8(0x08800001u), 0xDDu, "unaligned store byte 1");
    CHECK_EQ(psp_read8(0x08800002u), 0xCCu, "unaligned store byte 2");
    CHECK_EQ(psp_read8(0x08800003u), 0xBBu, "unaligned store byte 3");
    CHECK_EQ(psp_read8(0x08800004u), 0xAAu, "unaligned store byte 4");
    CHECK_EQ(psp_read8(0x08800005u), 0x55u, "swl/swr left byte 5 alone");

    /* Round-trip: read back what we just stored. */
    rt = 0;
    rt = psp_lwr(rt, 0x08800001u);
    rt = psp_lwl(rt, 0x08800004u);
    CHECK_EQ(rt, 0xAABBCCDDu, "unaligned store then load round-trips");

    /* An aligned address must behave exactly like a plain lw. */
    psp_write32(0x08800010u, 0xFEEDFACEu);
    rt = 0;
    rt = psp_lwr(rt, 0x08800010u);
    rt = psp_lwl(rt, 0x08800013u);
    CHECK_EQ(rt, 0xFEEDFACEu, "the unaligned pair degrades to lw when aligned");

    psp_mem_free();
}

int main(void) {
    test_zero_register();
    test_clock_mode_report();
    test_division();
    test_multiply();
    test_shifts();
    test_bitops();
    test_square_root();
    test_fpu();
    test_memory();
    test_unaligned();

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("all runtime checks passed\n");
    return 0;
}
