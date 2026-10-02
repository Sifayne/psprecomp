/* vfpuprobe -- ask a real PSP what its VFPU and FPU compute, bit for bit.
 *
 * Written against PSPSDK (BSD) only. Nothing here needs input: it runs every
 * section, writes vfpuprobe.txt and a few .bin files beside the EBOOT, and
 * returns to the XMB. The same PRX runs under psprecomp
 * (`allegrexrecomp interp vfpuprobe.prx --dispatch`), so the two logs and the
 * two sets of .bin files can be compared directly.
 *
 * What it answers, and where psprecomp currently guesses or leans on
 * pspautotests:
 *
 *   transcendental  vsin, vcos, vnsin, vasin, vexp2, vrexp2, vlog2, vrcp,
 *                   vnrcp, vrsq, vsqrt, vsat0, vsat1 over ~27,000 inputs each.
 *                   src/vfpu.c computes these with the host's libm, so the low
 *                   bits are whatever the host says (vfpu.c:1044).
 *   conversions     every packing/unpacking conversion, with the destination
 *                   pre-filled so lanes written by mistake show (vfpu.c:812),
 *                   vf2h's NaN cases (recomp_rt.h:260), vf2i rounding
 *                   (vfpu.c:771), vi2f scaling.
 *   constants       all 32 vcst entries.
 *   random          vrnds / vrndi / vrndf1 / vrndf2 sequences, and the
 *                   generator's state words.
 *   arithmetic      vadd..vdiv, min/max, dot products and sums on inputs that
 *                   expose rounding, NaN, -0, infinities and denormals;
 *                   vcrsp's infinity rule (vfpu.c:529); square-root edges
 *                   (vfpu.c:1013).
 *   compare         every vcmp condition on every pair of interesting values,
 *                   and which CC bits a narrow compare leaves alone
 *                   (vfpu.c:1492, 1522).
 *   matrix          vmmul orientation (vfpu.c:1340), vtfm, vhtfm, vmscl,
 *                   identity/zero/one fills.
 *   prefixes        swizzle, abs, negate, constants, saturation, write masks,
 *                   and sign handling of -0 and NaN (vfpu.c:152).
 *   fresh thread    what a new thread's FPU, VFPU and general registers hold
 *                   (cpu.h:66-76).
 *   fpu             FCR0..FCR31 read/write masks (cpu.h:86), exception cause
 *                   and flag bits (recomp_rt.h:415), rounding modes and
 *                   flush-to-zero (recomp_rt.h:346).
 *
 * Version 3 adds what version 2's hardware run left open (each new step sits
 * at the end of its section): the generator's rcx words after every draw,
 * from single-bit seeds and single-bit states; vf2h ties; how late an mfvc
 * after vcmp sees the new CC; NaN signs of vdot/vhdp/vdet/vqmul/vcrs/vcrsp;
 * vsrt ties; a fresh thread's fp; more COP1 flag cases; whether an enabled
 * FPU exception traps (last but one, each case skippable); and, last of all,
 * dumps of each transcendental's core over its reduced argument range
 * (section 13, about 91 MB).
 *
 * Version 4 keeps every step of version 3 where it was and appends three to
 * section 13, about 7 MB: every argument of the vsin/vcos and vasin core
 * segments whose fit version 3's dumps left open, and vlog2 for x >= 4, where
 * the result is often a unit below the core's. Section 12's four trap cases,
 * each of which stopped a 6.60 PSP in version 3, are now "not run".
 */
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <pspthreadman.h>

#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "probe.h"

PSP_MODULE_INFO("vfpuprobe", PSP_MODULE_USER, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(4096);

#define PROBE_VERSION 4

typedef unsigned int w32;   /* PSPSDK's u32 is uint32_t, a long here, which %X does not take */

static inline w32 fb(float f) { union { float f; w32 u; } c; c.f = f; return c.u; }

static w32 crc32(const void *p, int n) {
    const unsigned char *b = p;
    w32 c = 0xFFFFFFFFu;
    for (int i = 0; i < n; i++) {
        c ^= b[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

/* ---- operand buffers -----------------------------------------------------
 *
 * Every test loads its operands from S and T, pre-fills its destination from
 * D, runs one instruction, and stores the whole destination quad back to D --
 * so a lane the instruction should not have touched still holds the sentinel. */

#define SENT 0x5A5A5A5Au

static w32 D[4] __attribute__((aligned(16)));
static w32 S[4] __attribute__((aligned(16)));
static w32 T[4] __attribute__((aligned(16)));

static void setq(w32 *q, w32 a, w32 b, w32 c, w32 d) { q[0] = a; q[1] = b; q[2] = c; q[3] = d; }
static void sentinel(void) { setq(D, SENT, SENT, SENT, SENT); }

#define VRUN2(insn) __asm__ volatile(                                         \
    "lv.q C100, 0(%0)\n\t" "lv.q C200, 0(%1)\n\t" insn "\n\t"                 \
    "sv.q C100, 0(%0)\n" :: "r"(D), "r"(S) : "memory")
#define VRUN3(insn) __asm__ volatile(                                         \
    "lv.q C100, 0(%0)\n\t" "lv.q C200, 0(%1)\n\t" "lv.q C300, 0(%2)\n\t"      \
    insn "\n\t" "sv.q C100, 0(%0)\n" :: "r"(D), "r"(S), "r"(T) : "memory")

/* Sixteen nops. An mfvc right after vcmp.q reads the CC from before it
 * (vfpuprobe 1 steps 105-106), so version 3's control-register reads wait
 * this long first; section 6 measures how long is needed. */
#define NOPS16 ".set push\n\t.set noreorder\n\t.rept 16\n\tnop\n\t.endr\n\t.set pop\n\t"

typedef struct { const char *insn; void (*fn)(void); int cat; } vop;

static void logq(const char *label, const w32 *q) {
    out("%s [%08X %08X %08X %08X]", label, q[0], q[1], q[2], q[3]);
}

/* ---- 1. transcendental and other one-operand scalar ops ------------------ */

#define UNARY_LIST(X) \
    X(vsin) X(vcos) X(vnsin) X(vasin) X(vexp2) X(vrexp2) X(vlog2) \
    X(vrcp) X(vnrcp) X(vrsq) X(vsqrt) X(vsat0) X(vsat1)

#define X_DEF(name)                                                            \
    static void u_##name(const w32 *in, w32 *o, int n) {                       \
        for (int i = 0; i < n; i++)                                            \
            __asm__ volatile("lv.s S000, 0(%1)\n\t" #name ".s S001, S000\n\t"  \
                             "sv.s S001, 0(%0)\n"                              \
                             :: "r"(o + i), "r"(in + i) : "memory");           \
    }
UNARY_LIST(X_DEF)
#undef X_DEF

static const struct { const char *name; void (*fn)(const w32 *, w32 *, int); } UNARY[] = {
#define X_TAB(name) { #name, u_##name },
    UNARY_LIST(X_TAB)
#undef X_TAB
};
#define NUNARY ((int)(sizeof UNARY / sizeof UNARY[0]))

/* Inputs worth seeing one by one: zeros, denormals, the integers and halves
 * where trig lands exactly on an axis, one ulp either side of them, and every
 * NaN and infinity. */
static const w32 SPECIALS[] = {
    0x00000000, 0x80000000, 0x00000001, 0x807FFFFF, 0x00800000, 0x80800000,
    0x3F800000, 0xBF800000, 0x40000000, 0xC0000000, 0x40400000, 0x40800000,
    0x3F000000, 0xBF000000, 0x3E800000, 0x40A00000, 0x41000000, 0x41800000,
    0x3F7FFFFF, 0x3F800001, 0x3FFFFFFF, 0x40000001, 0x407FFFFF, 0x40800001,
    0x3F7FFFFE, 0x3F3504F3, 0x3FB504F3, 0x3DCCCCCD, 0x2EDBE6FF, 0x501502F9,
    0x447A0000, 0x47800000, 0x4B800000, 0x4E800000, 0x7F7FFFFF, 0xFF7FFFFF,
    0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00000, 0x7F800001, 0xFF800001,
    0x7FFFFFFF, 0x42C80000, 0xC2C80000, 0x42FE0000, 0x43000000, 0xC3000000,
};
#define NSPECIAL ((int)(sizeof SPECIALS / sizeof SPECIALS[0]))
#define NDENSE   16384      /* i/2048 - 4: [-4, 4) in exact steps */
#define NEXPO_E  81         /* exponents -40..40 */
#define NEXPO_M  128        /* mantissas per exponent */
#define NINPUT   (NSPECIAL + NDENSE + NEXPO_E * NEXPO_M)

static void section_unary(void) {
    section("1. one-operand ops over %d inputs each", NINPUT);
    w32 *in  = memalign(16, NINPUT * 4);
    w32 *res = memalign(16, NINPUT * 4);
    if (!in || !res) { say("out of memory\n"); return; }

    int k = 0;
    for (int i = 0; i < NSPECIAL; i++) in[k++] = SPECIALS[i];
    for (int i = 0; i < NDENSE; i++)   in[k++] = fb((float)i / 2048.0f - 4.0f);
    w32 lcg = 0x2545F491u;
    for (int e = -40; e <= 40; e++)
        for (int m = 0; m < NEXPO_M; m++) {
            lcg = lcg * 1664525u + 1013904223u;
            in[k++] = ((w32)(m & 1) << 31) | ((w32)(127 + e) << 23) | (lcg >> 9);
        }

    out("inputs: %d specials, %d dense (i/2048-4), %d exponent sweep; crc %08X\n",
        NSPECIAL, NDENSE, NEXPO_E * NEXPO_M, crc32(in, NINPUT * 4));
    step("write vfpu_inputs.bin");
    out("  wrote %d bytes\n", probe_write_file("vfpu_inputs.bin", in, NINPUT * 4));

    for (int u = 0; u < NUNARY; u++) {
        step("%s.s over every input", UNARY[u].name);
        UNARY[u].fn(in, res, NINPUT);
        for (int i = 0; i < NSPECIAL; i++)
            out("  %s(%08X) = %08X\n", UNARY[u].name, in[i], res[i]);
        out("  crc dense %08X, exponent sweep %08X\n",
            crc32(res + NSPECIAL, NDENSE * 4),
            crc32(res + NSPECIAL + NDENSE, NEXPO_E * NEXPO_M * 4));
        char name[40];
        snprintf(name, sizeof name, "vfpu_%s.bin", UNARY[u].name);
        out("  %s: %d bytes\n", name, probe_write_file(name, res, NINPUT * 4));
    }
    free(res);
    free(in);
}

/* ---- 2. conversions ------------------------------------------------------- */

enum { C_I2P, C_P2I, C_F2H, C_H2F, C_COL, C_F2I, C_I2F };

#define CONV_LIST(X)                                                 \
    X(cv_vi2uc_q,  "vi2uc.q S100, C200",  C_I2P)                     \
    X(cv_vi2c_q,   "vi2c.q S100, C200",   C_I2P)                     \
    X(cv_vi2us_p,  "vi2us.p S100, C200",  C_I2P)                     \
    X(cv_vi2us_q,  "vi2us.q C100, C200",  C_I2P)                     \
    X(cv_vi2s_p,   "vi2s.p S100, C200",   C_I2P)                     \
    X(cv_vi2s_q,   "vi2s.q C100, C200",   C_I2P)                     \
    X(cv_vus2i_s,  "vus2i.s C100, S200",  C_P2I)                     \
    X(cv_vus2i_p,  "vus2i.p C100, C200",  C_P2I)                     \
    X(cv_vs2i_s,   "vs2i.s C100, S200",   C_P2I)                     \
    X(cv_vs2i_p,   "vs2i.p C100, C200",   C_P2I)                     \
    X(cv_vuc2ifs,  "vuc2ifs.s C100, S200", C_P2I)                    \
    X(cv_vc2i_s,   "vc2i.s C100, S200",   C_P2I)                     \
    X(cv_vf2h_p,   "vf2h.p S100, C200",   C_F2H)                     \
    X(cv_vf2h_q,   "vf2h.q C100, C200",   C_F2H)                     \
    X(cv_vh2f_s,   "vh2f.s C100, S200",   C_H2F)                     \
    X(cv_vh2f_p,   "vh2f.p C100, C200",   C_H2F)                     \
    X(cv_vt4444,   "vt4444.q C100, C200", C_COL)                     \
    X(cv_vt5551,   "vt5551.q C100, C200", C_COL)                     \
    X(cv_vt5650,   "vt5650.q C100, C200", C_COL)                     \
    X(cv_f2in_s0,  "vf2in.s S100, S200, 0",  C_F2I)                  \
    X(cv_f2in_q0,  "vf2in.q C100, C200, 0",  C_F2I)                  \
    X(cv_f2in_q1,  "vf2in.q C100, C200, 1",  C_F2I)                  \
    X(cv_f2in_q16, "vf2in.q C100, C200, 16", C_F2I)                  \
    X(cv_f2in_q31, "vf2in.q C100, C200, 31", C_F2I)                  \
    X(cv_f2iz_q0,  "vf2iz.q C100, C200, 0",  C_F2I)                  \
    X(cv_f2iz_q16, "vf2iz.q C100, C200, 16", C_F2I)                  \
    X(cv_f2iu_q0,  "vf2iu.q C100, C200, 0",  C_F2I)                  \
    X(cv_f2iu_q16, "vf2iu.q C100, C200, 16", C_F2I)                  \
    X(cv_f2id_q0,  "vf2id.q C100, C200, 0",  C_F2I)                  \
    X(cv_f2id_q16, "vf2id.q C100, C200, 16", C_F2I)                  \
    X(cv_i2f_s0,   "vi2f.s S100, S200, 0",   C_I2F)                  \
    X(cv_i2f_q0,   "vi2f.q C100, C200, 0",   C_I2F)                  \
    X(cv_i2f_q1,   "vi2f.q C100, C200, 1",   C_I2F)                  \
    X(cv_i2f_q16,  "vi2f.q C100, C200, 16",  C_I2F)                  \
    X(cv_i2f_q31,  "vi2f.q C100, C200, 31",  C_I2F)

#define X_DEF(id, insn, cat) static void id(void) { VRUN2(insn); }
CONV_LIST(X_DEF)
#undef X_DEF
static const vop CONV[] = {
#define X_TAB(id, insn, cat) { insn, id, cat },
    CONV_LIST(X_TAB)
#undef X_TAB
};

static const w32 SRC_I2P[][4] = {
    { 0x7FFFFFFF, 0x80000000, 0x12345678, 0xFFFFFFFF },
    { 0x00000000, 0x00800000, 0x3F800000, 0x7F000000 },
    { 0x40000000, 0xC0000000, 0x00FFFFFF, 0x7FFF8000 },
    { 0x00010000, 0x0000FFFF, 0x7FFF0000, 0x80010000 },
};
static const w32 SRC_P2I[][4] = {
    { 0x80017FFF, 0x12345678, 0xFFFF0000, 0x00FF7F80 },
    { 0x7F80FF01, 0x0000FFFF, 0x80008000, 0x01020304 },
};
static const w32 SRC_F2H[][4] = {
    { 0x3F800000, 0xC0200000, 0x477FE000, 0x477FF000 },  /* 1, -2.5, 65504, 65520 */
    { 0x322BCC77, 0x7F800000, 0x7FC00000, 0xFFC00000 },  /* 1e-8, inf, +NaN, -NaN */
    { 0x80000000, 0x38800000, 0x33800000, 0x7F800001 },  /* -0, 2^-14, 2^-24, sNaN */
    { 0x477FEF00, 0x501502F9, 0xD01502F9, 0x3EAAAAAB },  /* 65519, 1e10, -1e10, 1/3 */
    { 0x33000000, 0x33C00000, 0x387FC000, 0xFF800001 },  /* 2^-25, 1.5*2^-24, subnormal edge, -sNaN */
};
static const w32 SRC_H2F[][4] = {
    { 0x3C00C000, 0x7C00FC00, 0x7E00FE01, 0x00018001 },
    { 0x7BFF0400, 0x03FF8000, 0x7C01FC01, 0x35553555 },
};
static const w32 SRC_COL[][4] = {
    { 0x12345678, 0xFFFFFFFF, 0x80808080, 0x7F7F7F7F },
    { 0x00000000, 0xF0F0F0F0, 0x0F0F0F0F, 0xFF00FF00 },
};
static const w32 SRC_F2I[][4] = {
    { 0x3F000000, 0x3FC00000, 0x40200000, 0xBF000000 },  /* 0.5, 1.5, 2.5, -0.5 */
    { 0xBFC00000, 0x4F000000, 0xCF32D05E, 0x7FC00000 },  /* -1.5, 2^31, -3e9, NaN */
    { 0x7F800000, 0xFF800000, 0x00000001, 0x3EFFFFFF },  /* inf, -inf, denormal, 0.49999997 */
    { 0x477FFF80, 0xC77FFF80, 0x3F800000, 0xBF800000 },  /* 65535.5, -65535.5, 1, -1 */
    { 0xFFC00000, 0x4EFFFFFF, 0xCF000000, 0x80000000 },  /* -NaN, below 2^31, -2^31, -0 */
};
static const w32 SRC_I2F[][4] = {
    { 0x00000001, 0xFFFFFFFF, 0x7FFFFFFF, 0x80000000 },
    { 0x00000003, 0x01000001, 0x00FFFFFF, 0xFFFFFFFD },
};

static void section_conv(void) {
    section("2. conversions (destination pre-filled with %08X)", SENT);
    for (int c = 0; c < (int)(sizeof CONV / sizeof CONV[0]); c++) {
        const w32 (*src)[4]; int n;
        switch (CONV[c].cat) {
        case C_I2P: src = SRC_I2P; n = 4; break;
        case C_P2I: src = SRC_P2I; n = 2; break;
        case C_F2H: src = SRC_F2H; n = 5; break;
        case C_H2F: src = SRC_H2F; n = 2; break;
        case C_COL: src = SRC_COL; n = 2; break;
        case C_F2I: src = SRC_F2I; n = 5; break;
        default:    src = SRC_I2F; n = 2; break;
        }
        step("%s", CONV[c].insn);
        for (int i = 0; i < n; i++) {
            memcpy(S, src[i], 16);
            sentinel();
            CONV[c].fn();
            logq("  src", S); logq(" -> dst", D); out("\n");
        }
    }

    /* Version 3: ties and near-ties. Version 1's rows (steps 27-30) fit both
     * ties-toward-zero and truncation; x+ below is just above a tie, which
     * the two round differently, and the odd-below ties separate nearest-even
     * from ties-toward-zero. Each row packs into lanes 0 and 1. */
    static const w32 TIES[][4] = {
        { 0x3F801000, 0x3F803000, 0x3F801008, 0x3F801FF8 },  /* 1+2^-11 (3C00|3C01), 1+3*2^-11 (3C01|3C02), x+, just below 3C01 */
        { 0xBF801000, 0xBF803000, 0xBF801008, 0xBF801FF8 },  /* the same, negative */
        { 0x3FFFF000, 0x3FFFF800, 0x40001000, 0x40003000 },  /* 2-2^-11 (3FFF|4000), 2-2^-12, 2+2^-10 (4000|4001), 2+3*2^-10 */
        { 0x477FF001, 0x477FF800, 0x477FFFFF, 0x47800000 },  /* x+ of 65520 (7BFF|7C00), 65528, below 65536, 65536 */
        { 0x387FE000, 0x387FE001, 0x387FF000, 0x387FFFFF },  /* 2^-14(1-2^-11) (03FF|0400), x+, 2^-14(1-2^-12), below 2^-14 */
        { 0x38800000, 0x38801000, 0x38803000, 0x38801001 },  /* 2^-14, 0400|0401, 0401|0402, x+ of the first */
    };
    step("vf2h.q ties and near-ties: nearest-even, ties toward zero or truncation");
    for (int i = 0; i < (int)(sizeof TIES / sizeof TIES[0]); i++) {
        memcpy(S, TIES[i], 16);
        sentinel();
        cv_vf2h_q();
        logq("  src", S); logq(" -> dst", D); out("\n");
    }
}

/* ---- 3. vcst --------------------------------------------------------------
 * Encoded by number rather than name, so index 0 and any unnamed entries are
 * asked for too: vcst.s S100, n = 0xD0600004 | n << 16. */

#define VCST(n) static void vcst_##n(void) {                                   \
        __asm__ volatile("lv.q C100, 0(%0)\n\t"                                \
                         ".word (0xD0600004 | (" #n " << 16))\n\t"             \
                         "sv.q C100, 0(%0)\n" :: "r"(D) : "memory"); }
VCST(0)  VCST(1)  VCST(2)  VCST(3)  VCST(4)  VCST(5)  VCST(6)  VCST(7)
VCST(8)  VCST(9)  VCST(10) VCST(11) VCST(12) VCST(13) VCST(14) VCST(15)
VCST(16) VCST(17) VCST(18) VCST(19) VCST(20) VCST(21) VCST(22) VCST(23)
VCST(24) VCST(25) VCST(26) VCST(27) VCST(28) VCST(29) VCST(30) VCST(31)
static void (*const VCSTS[32])(void) = {
    vcst_0,  vcst_1,  vcst_2,  vcst_3,  vcst_4,  vcst_5,  vcst_6,  vcst_7,
    vcst_8,  vcst_9,  vcst_10, vcst_11, vcst_12, vcst_13, vcst_14, vcst_15,
    vcst_16, vcst_17, vcst_18, vcst_19, vcst_20, vcst_21, vcst_22, vcst_23,
    vcst_24, vcst_25, vcst_26, vcst_27, vcst_28, vcst_29, vcst_30, vcst_31,
};

static void section_vcst(void) {
    section("3. vcst.s, all 32 indices");
    step("vcst.s S100, 0..31");
    for (int i = 0; i < 32; i++) {
        sentinel();
        VCSTS[i]();
        out("  vcst %2d = %08X  (lane 1 %08X)\n", i, D[0], D[1]);
    }
}

/* ---- 4. the random generator ---------------------------------------------- */

static void mfvc_all(w32 v[16]) {
    __asm__ volatile(
        "mfvc %0, $128\n\t" "mfvc %1, $129\n\t" "mfvc %2, $130\n\t" "mfvc %3, $131\n\t"
        "mfvc %4, $132\n\t" "mfvc %5, $133\n\t" "mfvc %6, $134\n\t" "mfvc %7, $135\n"
        : "=r"(v[0]), "=r"(v[1]), "=r"(v[2]), "=r"(v[3]),
          "=r"(v[4]), "=r"(v[5]), "=r"(v[6]), "=r"(v[7]));
    __asm__ volatile(
        "mfvc %0, $136\n\t" "mfvc %1, $137\n\t" "mfvc %2, $138\n\t" "mfvc %3, $139\n\t"
        "mfvc %4, $140\n\t" "mfvc %5, $141\n\t" "mfvc %6, $142\n\t" "mfvc %7, $143\n"
        : "=r"(v[8]), "=r"(v[9]), "=r"(v[10]), "=r"(v[11]),
          "=r"(v[12]), "=r"(v[13]), "=r"(v[14]), "=r"(v[15]));
}

static void log_vfpu_ctrl(const char *label) {
    w32 v[16];
    mfvc_all(v);
    out("  %s: pfxs %08X pfxt %08X pfxd %08X cc %08X inf4 %08X rsv5 %08X rsv6 %08X rev %08X\n",
        label, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
    out("  %s: rcx0-7 %08X %08X %08X %08X %08X %08X %08X %08X\n",
        label, v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15]);
}

static w32 rnd_i(void)  { __asm__ volatile("vrndi.s S100\n\tsv.s S100, 0(%0)\n" :: "r"(D) : "memory"); return D[0]; }
static w32 rnd_f1(void) { __asm__ volatile("vrndf1.s S100\n\tsv.s S100, 0(%0)\n" :: "r"(D) : "memory"); return D[0]; }
static w32 rnd_f2(void) { __asm__ volatile("vrndf2.s S100\n\tsv.s S100, 0(%0)\n" :: "r"(D) : "memory"); return D[0]; }
static void rnd_seed(w32 seed) {
    S[0] = seed;
    __asm__ volatile("lv.s S200, 0(%0)\n\tvrnds.s S200\n" :: "r"(S) : "memory");
}

/* ---- 4, version 3: the generator's state after every draw ------------------
 *
 * Version 1 saw only outputs, which left the generator fitted rather than
 * known: four of six streams fit x' = 69069x+1, a 13/17/5 xorshift and a lag-2
 * sum, and what the carry nibbles become after a draw does not. These steps
 * read rcx0-7 after every draw, from the failing seeds, from every single-bit
 * seed and from every single-bit state written with mtvc. */

/* rcx0-7, sixteen instructions after whatever came before. */
static void rcx_read(w32 v[8]) {
    __asm__ volatile(
        NOPS16
        "mfvc %0, $136\n\t" "mfvc %1, $137\n\t" "mfvc %2, $138\n\t" "mfvc %3, $139\n\t"
        "mfvc %4, $140\n\t" "mfvc %5, $141\n\t" "mfvc %6, $142\n\t" "mfvc %7, $143\n"
        : "=r"(v[0]), "=r"(v[1]), "=r"(v[2]), "=r"(v[3]),
          "=r"(v[4]), "=r"(v[5]), "=r"(v[6]), "=r"(v[7]));
}

static void log_rcx(const char *lead, const w32 v[8]) {
    out("%s rcx %08X %08X %08X %08X %08X %08X %08X %08X\n",
        lead, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
}

#define MTVC_RCX(n) static void mtvc_##n(w32 v) {                               \
        __asm__ volatile("mtvc %0, $" #n "\n" :: "r"(v)); }
MTVC_RCX(136) MTVC_RCX(137) MTVC_RCX(138) MTVC_RCX(139)
MTVC_RCX(140) MTVC_RCX(141) MTVC_RCX(142) MTVC_RCX(143)
static void (*const MTVC_RCX[8])(w32) = {
    mtvc_136, mtvc_137, mtvc_138, mtvc_139, mtvc_140, mtvc_141, mtvc_142, mtvc_143,
};

/* vrndi.s, then rcx0 read n instructions later. */
#define RNDLAT(n) static w32 rndlat_##n(void) { w32 v;                          \
        __asm__ volatile(".set push\n\t.set noreorder\n\t"                      \
                         "vrndi.s S100\n\t.rept " #n "\n\tnop\n\t.endr\n\t"     \
                         "mfvc %0, $136\n\t.set pop\n" : "=r"(v)); return v; }
RNDLAT(0) RNDLAT(1) RNDLAT(2) RNDLAT(3) RNDLAT(4) RNDLAT(5) RNDLAT(6) RNDLAT(8) RNDLAT(16)
static const struct { int n; w32 (*fn)(void); } RNDLATS[] = {
    { 0, rndlat_0 }, { 1, rndlat_1 }, { 2, rndlat_2 }, { 3, rndlat_3 }, { 4, rndlat_4 },
    { 5, rndlat_5 }, { 6, rndlat_6 }, { 8, rndlat_8 }, { 16, rndlat_16 },
};

static void section_random_v3(void) {
    static const w32 seeds[] = { 0x00000000, 0x00000001, 0x12345678, 0xFFFFFFFF, 0x3F800000 };
    w32 v[8];
    for (int s = 0; s < (int)(sizeof seeds / sizeof seeds[0]); s++) {
        step("rcx0-7 after vrnds %08X and after each of 16 vrndi", seeds[s]);
        rnd_seed(seeds[s]);
        rcx_read(v);
        log_rcx("  seed       ", v);
        for (int d = 1; d <= 16; d++) {
            char lead[24];
            const w32 r = rnd_i();
            rcx_read(v);
            snprintf(lead, sizeof lead, "  %2d %08X", d, r);
            log_rcx(lead, v);
        }
    }

    step("mfvc of rcx0 0..16 instructions after vrndi.s, from seed 0");
    for (int i = 0; i < (int)(sizeof RNDLATS / sizeof RNDLATS[0]); i++) {
        rnd_seed(0);
        rcx_read(v);
        const w32 early = RNDLATS[i].fn();
        rcx_read(v);
        out("  %2d instructions: rcx0 %08X, 16 later %08X\n", RNDLATS[i].n, early, v[0]);
    }

    if (!step("mtvc rcx0-7 from seed 0: all eight read back after each write")) {
        static const w32 vals[] = { 0xFFFFFFFF, 0x00000000, 0x12345678, 0x3F800000 };
        for (int i = 0; i < 8; i++)
            for (int k = 0; k < (int)(sizeof vals / sizeof vals[0]); k++) {
                char lead[32];
                rnd_seed(0);
                rcx_read(v);
                MTVC_RCX[i](vals[k]);
                rcx_read(v);
                snprintf(lead, sizeof lead, "  rcx%d <- %08X:", i, vals[k]);
                log_rcx(lead, v);
            }
        rnd_seed(0);
    }

    /* Per seed: rcx0-7 after vrnds, then 16 x (the vrndi value, rcx0-7). */
    step("seeds 1<<0..1<<31: rcx after vrnds and after each of 16 vrndi -> vfpu_rng_seeds.bin");
    {
        enum { PER = 8 + 16 * 9 };
        w32 *buf = memalign(16, 32 * PER * 4);
        if (!buf) { out("  out of memory\n"); return; }
        for (int k = 0; k < 32; k++) {
            w32 *p = buf + k * PER;
            rnd_seed(1u << k);
            rcx_read(p);
            p += 8;
            for (int d = 0; d < 16; d++) {
                *p++ = rnd_i();
                rcx_read(p);
                p += 8;
            }
            const w32 *q = buf + k * PER + 8;
            out("  seed %08X: vrndi %08X %08X %08X %08X\n", 1u << k, q[0], q[9], q[18], q[27]);
        }
        out("  vfpu_rng_seeds.bin: %d bytes, crc %08X\n",
            probe_write_file("vfpu_rng_seeds.bin", buf, 32 * PER * 4), crc32(buf, 32 * PER * 4));
        free(buf);
    }

    /* Per state: rcx0-7 after the write, then 8 x (the vrndi value, rcx0-7).
     * State (i, b) is seed 0's (every rcx 3F800000) with bit b of rcx i
     * flipped by mtvc; 256 states, rcx0 bit 0 first. */
    if (!step("seed 0 with one rcx bit flipped by mtvc: rcx and 8 vrndi each -> vfpu_rng_states.bin")) {
        enum { PER = 8 + 8 * 9 };
        w32 *buf = memalign(16, 256 * PER * 4);
        if (!buf) { out("  out of memory\n"); return; }
        for (int i = 0; i < 8; i++)
            for (int b = 0; b < 32; b++) {
                w32 *p = buf + (i * 32 + b) * PER;
                rnd_seed(0);
                rcx_read(p);
                MTVC_RCX[i](0x3F800000u ^ (1u << b));
                rcx_read(p);
                p += 8;
                for (int d = 0; d < 8; d++) {
                    *p++ = rnd_i();
                    rcx_read(p);
                    p += 8;
                }
            }
        for (int i = 0; i < 8; i++)
            for (int b0 = 0; b0 < 32; b0 += 8) {
                out("  rcx%d bits %2d-%2d, first vrndi:", i, b0, b0 + 7);
                for (int b = b0; b < b0 + 8; b++) out(" %08X", buf[(i * 32 + b) * PER + 8]);
                out("\n");
            }
        out("  vfpu_rng_states.bin: %d bytes, crc %08X\n",
            probe_write_file("vfpu_rng_states.bin", buf, 256 * PER * 4), crc32(buf, 256 * PER * 4));
        free(buf);
        rnd_seed(0);
    }
}

static void section_random(void) {
    section("4. vrnds / vrndi / vrndf1 / vrndf2 (runs after 11)");
    step("generator state before this probe seeds it");
    log_vfpu_ctrl("state");
    out("  vrndi x4:");
    for (int i = 0; i < 4; i++) out(" %08X", rnd_i());
    out("\n");

    static const w32 seeds[] = { 0x00000000, 0x00000001, 0x12345678, 0xFFFFFFFF, 0x3F800000 };
    for (int s = 0; s < (int)(sizeof seeds / sizeof seeds[0]); s++) {
        step("vrnds.s seed %08X", seeds[s]);
        rnd_seed(seeds[s]);
        log_vfpu_ctrl("after seed");
        out("  vrndi :"); for (int i = 0; i < 8; i++) out(" %08X", rnd_i());  out("\n");
        rnd_seed(seeds[s]);
        out("  vrndf1:"); for (int i = 0; i < 8; i++) out(" %08X", rnd_f1()); out("\n");
        rnd_seed(seeds[s]);
        out("  vrndf2:"); for (int i = 0; i < 8; i++) out(" %08X", rnd_f2()); out("\n");
    }
    section_random_v3();
}

/* ---- 5. arithmetic -------------------------------------------------------- */

#define ARITH_LIST(X)                                        \
    X(ar_vadd,   "vadd.q C100, C200, C300")                  \
    X(ar_vsub,   "vsub.q C100, C200, C300")                  \
    X(ar_vmul,   "vmul.q C100, C200, C300")                  \
    X(ar_vdiv,   "vdiv.q C100, C200, C300")                  \
    X(ar_vmin,   "vmin.q C100, C200, C300")                  \
    X(ar_vmax,   "vmax.q C100, C200, C300")                  \
    X(ar_vscmp,  "vscmp.q C100, C200, C300")                 \
    X(ar_vsge,   "vsge.q C100, C200, C300")                  \
    X(ar_vslt,   "vslt.q C100, C200, C300")                  \
    X(ar_vdot,   "vdot.q S100, C200, C300")                  \
    X(ar_vdot_t, "vdot.t S100, C200, C300")                  \
    X(ar_vhdp,   "vhdp.q S100, C200, C300")                  \
    X(ar_vscl,   "vscl.q C100, C200, S300")                  \
    X(ar_vcrs,   "vcrs.t C100, C200, C300")                  \
    X(ar_vcrsp,  "vcrsp.t C100, C200, C300")                 \
    X(ar_vdet,   "vdet.p S100, C200, C300")                  \
    X(ar_vqmul,  "vqmul.q C100, C200, C300")

#define UNARYQ_LIST(X)                                       \
    X(aq_vmov,   "vmov.q C100, C200")                        \
    X(aq_vabs,   "vabs.q C100, C200")                        \
    X(aq_vneg,   "vneg.q C100, C200")                        \
    X(aq_vsgn,   "vsgn.q C100, C200")                        \
    X(aq_vocp,   "vocp.q C100, C200")                        \
    X(aq_vfad,   "vfad.q S100, C200")                        \
    X(aq_vavg,   "vavg.q S100, C200")                        \
    X(aq_vfad_t, "vfad.t S100, C200")                        \
    X(aq_vavg_t, "vavg.t S100, C200")                        \
    X(aq_vsrt1,  "vsrt1.q C100, C200")                       \
    X(aq_vsrt2,  "vsrt2.q C100, C200")                       \
    X(aq_vsrt3,  "vsrt3.q C100, C200")                       \
    X(aq_vsrt4,  "vsrt4.q C100, C200")                       \
    X(aq_vbfy1,  "vbfy1.q C100, C200")                       \
    X(aq_vbfy2,  "vbfy2.q C100, C200")                       \
    X(aq_vsqrtq, "vsqrt.q C100, C200")                       \
    X(aq_vrsqq,  "vrsq.q C100, C200")                        \
    X(aq_vzero,  "vzero.p C100")                             \
    X(aq_vone,   "vone.t C100")                              \
    X(aq_vidt_p, "vidt.p C100")                              \
    X(aq_vidt_q, "vidt.q C100")

#define X_DEF3(id, insn) static void id(void) { VRUN3(insn); }
#define X_DEF2(id, insn) static void id(void) { VRUN2(insn); }
ARITH_LIST(X_DEF3)
UNARYQ_LIST(X_DEF2)
#undef X_DEF3
#undef X_DEF2
static const vop ARITH[] = {
#define X_TAB(id, insn) { insn, id, 0 },
    ARITH_LIST(X_TAB)
#undef X_TAB
};
static const vop UNARYQ[] = {
#define X_TAB(id, insn) { insn, id, 0 },
    UNARYQ_LIST(X_TAB)
#undef X_TAB
};

/* Operand pairs, each built to expose one thing: cancellation that a fused or
 * wider accumulator would survive, NaN and infinity propagation, signed
 * zeros, denormals (flushed or kept), overflow, and ordinary inexact values. */
static const struct { const char *name; w32 s[4]; w32 t[4]; } PAIRS[] = {
    { "cancel",   { 0x4CBEBC20, 0x3F800000, 0xCCBEBC20, 0x3F800000 },   /* 1e8, 1, -1e8, 1 */
                  { 0x3F800000, 0x3F800000, 0x3F800000, 0x3F800000 } },
    { "nan-inf",  { 0x7FC00000, 0x80000000, 0x7F800000, 0xFF800000 },
                  { 0x3F800000, 0x00000000, 0xFF800000, 0xFF800000 } },
    { "denormal", { 0x000116C2, 0x800116C2, 0x006CE3EE, 0x40400000 },   /* 1e-40, -1e-40, 1e-38, 3 */
                  { 0x000116C2, 0x3F800000, 0x006CE3EE, 0x3EAAAAAB } },
    { "inexact",  { 0x3F8CCCCD, 0x400CCCCD, 0x40533333, 0x408CCCCD },   /* 1.1 2.2 3.3 4.4 */
                  { 0x3F666666, 0xBDCCCCCD, 0x40F66666, 0x3A83126F } }, /* .9 -.1 7.7 1e-3 */
    { "snan",     { 0x7F800001, 0xFFC00000, 0x3F800000, 0x40000000 },
                  { 0x40000000, 0x3F800000, 0x7FC00001, 0x80000000 } },
    { "overflow", { 0x7F7FFFFF, 0x7F7FFFFF, 0xFF7FFFFF, 0x3F800000 },
                  { 0x40000000, 0x7F7FFFFF, 0x7F7FFFFF, 0x00800000 } },
    { "cross-inf",{ 0x7F800000, 0x3F800000, 0x40000000, 0x00000000 },   /* vfpu.c:529 */
                  { 0x3F800000, 0x40000000, 0x40400000, 0x00000000 } },
    { "zeros",    { 0x00000000, 0x80000000, 0x00000000, 0x80000000 },
                  { 0x00000000, 0x00000000, 0x80000000, 0x80000000 } },
    { "sqrt-edge",{ 0x007FFFFF, 0x80000001, 0xBF800000, 0xFF800000 },   /* vfpu.c:1013 */
                  { 0x00000000, 0x3F800000, 0x7FC00000, 0x7F800000 } },
};
#define NPAIRS ((int)(sizeof PAIRS / sizeof PAIRS[0]))

/* ---- 5, version 3: NaN signs and sort ties ---------------------------------
 *
 * Which sign a NaN result takes, one NaN or one invalid product or sum at a
 * time; version 1 settled it for vadd..vdiv but not for the dot-unit ops
 * (vqmul, vhdp, vdet). 1 is 3F800000, -1 BF800000. */
#define P1 0x3F800000
#define M1 0xBF800000
static const struct { const char *name; w32 s[4]; w32 t[4]; } NANROWS[] = {
    { "s0 +sNaN",     { 0x7F800001, P1, P1, P1 }, { P1, P1, P1, P1 } },
    { "s0 -sNaN",     { 0xFF800001, P1, P1, P1 }, { P1, P1, P1, P1 } },
    { "s0 +qNaN",     { 0x7FC00000, P1, P1, P1 }, { P1, P1, P1, P1 } },
    { "s0 -qNaN",     { 0xFFC00000, P1, P1, P1 }, { P1, P1, P1, P1 } },
    { "s0 +sNaN t-1", { 0x7F800001, P1, P1, P1 }, { M1, M1, M1, M1 } },
    { "s0 -sNaN t-1", { 0xFF800001, P1, P1, P1 }, { M1, M1, M1, M1 } },
    { "s0 +qNaN t-1", { 0x7FC00000, P1, P1, P1 }, { M1, M1, M1, M1 } },
    { "s0 -qNaN t-1", { 0xFFC00000, P1, P1, P1 }, { M1, M1, M1, M1 } },
    { "t1 +sNaN s-1", { M1, M1, M1, M1 }, { P1, 0x7F800001, P1, P1 } },
    { "t1 -sNaN s-1", { M1, M1, M1, M1 }, { P1, 0xFF800001, P1, P1 } },
    { "t1 +qNaN s-1", { M1, M1, M1, M1 }, { P1, 0x7FC00000, P1, P1 } },
    { "t1 -qNaN s-1", { M1, M1, M1, M1 }, { P1, 0xFFC00000, P1, P1 } },
    { "s3 -qNaN",     { P1, P1, P1, 0xFFC00000 }, { P1, P1, P1, P1 } },
    { "t3 -qNaN",     { P1, P1, P1, P1 }, { P1, P1, P1, 0xFFC00000 } },
    { "s0 +, s1 -",   { 0x7F800001, 0xFF800001, P1, P1 }, { P1, P1, P1, P1 } },
    { "s0 -, s1 +",   { 0xFF800001, 0x7F800001, P1, P1 }, { P1, P1, P1, P1 } },
    { "+inf*+0",      { 0x7F800000, 0, 0, 0 }, { 0, 0, 0, 0 } },
    { "-inf*+0",      { 0xFF800000, 0, 0, 0 }, { 0, 0, 0, 0 } },
    { "+inf*-0",      { 0x7F800000, 0, 0, 0 }, { 0x80000000, 0, 0, 0 } },
    { "lane1 inf*-0", { 0, 0x7F800000, 0, 0 }, { 0, 0x80000000, 0, 0 } },
    { "+inf-inf",     { 0x7F800000, 0x7F800000, 0, 0 }, { P1, M1, 0, 0 } },
    { "-inf+inf",     { 0xFF800000, 0x7F800000, 0, 0 }, { P1, P1, 0, 0 } },
};
#undef P1
#undef M1
#define NNANROWS ((int)(sizeof NANROWS / sizeof NANROWS[0]))

/* Rows whose lanes tie under the sign-magnitude order (every exponent-0 value
 * is 0) but differ in their bits, so where each lane lands shows. Version 1's
 * vsrt tie direction rests on two rows. */
static const w32 TIEROWS[][4] = {
    { 0x00000000, 0x80000000, 0x00000001, 0x80000001 },
    { 0x80000000, 0x00000000, 0x80000001, 0x00000001 },
    { 0x00000001, 0x80000000, 0x00000000, 0x807FFFFF },
    { 0x007FFFFF, 0x00000000, 0x80000000, 0x80000001 },
    { 0x80000001, 0x00000001, 0x80000000, 0x00000000 },
    { 0x00000000, 0x3F800000, 0x80000000, 0x3F800000 },
};
#define NTIEROWS ((int)(sizeof TIEROWS / sizeof TIEROWS[0]))

static void section_arith_v3(void) {
    static const vop NANOPS[] = {
        { "vdot.q S100, C200, C300", ar_vdot, 0 },  { "vhdp.q S100, C200, C300", ar_vhdp, 0 },
        { "vdet.p S100, C200, C300", ar_vdet, 0 },  { "vqmul.q C100, C200, C300", ar_vqmul, 0 },
        { "vcrs.t C100, C200, C300", ar_vcrs, 0 },  { "vcrsp.t C100, C200, C300", ar_vcrsp, 0 },
    };
    step("NaN-sign operand rows");
    for (int r = 0; r < NNANROWS; r++) {
        out("  %-12s s", NANROWS[r].name); logq("", NANROWS[r].s);
        out(" t"); logq("", NANROWS[r].t); out("\n");
    }
    for (int a = 0; a < (int)(sizeof NANOPS / sizeof NANOPS[0]); a++) {
        step("NaN signs: %s", NANOPS[a].insn);
        for (int r = 0; r < NNANROWS; r++) {
            memcpy(S, NANROWS[r].s, 16); memcpy(T, NANROWS[r].t, 16);
            sentinel();
            NANOPS[a].fn();
            out("  %-12s", NANROWS[r].name); logq("", D); out("\n");
        }
    }

    static const vop SRTS[] = {
        { "vsrt1", aq_vsrt1, 0 }, { "vsrt2", aq_vsrt2, 0 }, { "vsrt3", aq_vsrt3, 0 }, { "vsrt4", aq_vsrt4, 0 },
    };
    step("vsrt1-4.q on ties that differ in their bits (+-0, +-denormals)");
    for (int a = 0; a < 4; a++)
        for (int r = 0; r < NTIEROWS; r++) {
            memcpy(S, TIEROWS[r], 16);
            sentinel();
            SRTS[a].fn();
            out("  %s", SRTS[a].insn); logq(" s", S); logq(" ->", D); out("\n");
        }

    static const vop MINMAX[] = {
        { "vmin ", ar_vmin, 0 }, { "vmax ", ar_vmax, 0 }, { "vscmp", ar_vscmp, 0 },
    };
    step("vmin, vmax, vscmp.q on ties that differ in their bits, t the next row");
    for (int a = 0; a < 3; a++)
        for (int r = 0; r < NTIEROWS; r++) {
            memcpy(S, TIEROWS[r], 16); memcpy(T, TIEROWS[(r + 1) % NTIEROWS], 16);
            sentinel();
            MINMAX[a].fn();
            out("  %s", MINMAX[a].insn); logq(" s", S); logq(" t", T); logq(" ->", D); out("\n");
        }
}

static void section_arith(void) {
    section("5. arithmetic on %d operand pairs", NPAIRS);
    for (int p = 0; p < NPAIRS; p++) {
        out("pair %-9s s", PAIRS[p].name); logq("", PAIRS[p].s);
        out(" t"); logq("", PAIRS[p].t); out("\n");
    }
    for (int a = 0; a < (int)(sizeof ARITH / sizeof ARITH[0]); a++) {
        step("%s", ARITH[a].insn);
        for (int p = 0; p < NPAIRS; p++) {
            memcpy(S, PAIRS[p].s, 16); memcpy(T, PAIRS[p].t, 16);
            sentinel();
            ARITH[a].fn();
            out("  %-9s", PAIRS[p].name); logq("", D); out("\n");
        }
    }
    for (int a = 0; a < (int)(sizeof UNARYQ / sizeof UNARYQ[0]); a++) {
        step("%s", UNARYQ[a].insn);
        for (int p = 0; p < NPAIRS; p++) {
            memcpy(S, PAIRS[p].s, 16);
            sentinel();
            UNARYQ[a].fn();
            out("  %-9s", PAIRS[p].name); logq("", D); out("\n");
        }
    }
    section_arith_v3();
}

/* ---- 6. vcmp -------------------------------------------------------------- */

static w32 read_cc(void) { w32 v; __asm__ volatile("mfvc %0, $131\n" : "=r"(v)); return v; }

#define VCMP_LIST(X) X(FL) X(EQ) X(LT) X(LE) X(TR) X(NE) X(GE) X(GT) \
                     X(EZ) X(EN) X(EI) X(ES) X(NZ) X(NN) X(NI) X(NS)
#define X_DEF(c) static void vc_##c(void) {                                    \
        __asm__ volatile("lv.s S200, 0(%0)\n\tlv.s S300, 0(%1)\n\t"            \
                         "vcmp.s " #c ", S200, S300\n" :: "r"(S), "r"(T) : "memory"); }
VCMP_LIST(X_DEF)
#undef X_DEF
static const struct { const char *name; void (*fn)(void); } VCMPS[16] = {
#define X_TAB(c) { #c, vc_##c },
    VCMP_LIST(X_TAB)
#undef X_TAB
};

static const w32 CMPVALS[] = {
    0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x7F800000, 0xFF800000,
    0x7FC00000, 0xFFC00000, 0x7F800001, 0x00000001,
};
#define NCMPVALS ((int)(sizeof CMPVALS / sizeof CMPVALS[0]))

/* ---- 6, version 3: when mfvc sees a new CC --------------------------------
 *
 * Version 1 read the CC with an mfvc straight after each vcmp; after vcmp.q
 * and vcmp.t that read returned the CC from before the compare (steps
 * 105-106), while vcmov right after saw the new one. Each function below sets
 * the CC with a vcmp.q, waits 16 nops, runs the second compare, waits n nops
 * and reads. */
#define CCLAT(id, sz, r2, r3, first, second, n)                                 \
    static w32 id(void) {                                                       \
        w32 v;                                                                  \
        __asm__ volatile(".set push\n\t.set noreorder\n\t"                      \
                         "vcmp.q " first ", C200, C300\n\t"                     \
                         ".rept 16\n\tnop\n\t.endr\n\t"                         \
                         "vcmp." sz " " second ", " r2 ", " r3 "\n\t"           \
                         ".rept " #n "\n\tnop\n\t.endr\n\t"                     \
                         "mfvc %0, $131\n\t.set pop\n" : "=r"(v));              \
        return v; }
#define CCLAT_ALL(tag, sz, r2, r3, first, second)                               \
    CCLAT(tag##_0, sz, r2, r3, first, second, 0)                                \
    CCLAT(tag##_1, sz, r2, r3, first, second, 1)                                \
    CCLAT(tag##_2, sz, r2, r3, first, second, 2)                                \
    CCLAT(tag##_3, sz, r2, r3, first, second, 3)                                \
    CCLAT(tag##_4, sz, r2, r3, first, second, 4)                                \
    CCLAT(tag##_5, sz, r2, r3, first, second, 5)                                \
    CCLAT(tag##_6, sz, r2, r3, first, second, 6)                                \
    CCLAT(tag##_8, sz, r2, r3, first, second, 8)                                \
    CCLAT(tag##_16, sz, r2, r3, first, second, 16)
#define CCLAT_ROW(tag) { tag##_0, tag##_1, tag##_2, tag##_3, tag##_4, tag##_5, \
                         tag##_6, tag##_8, tag##_16 }
CCLAT_ALL(lat_q_fl, "q", "C200", "C300", "TR", "FL")
CCLAT_ALL(lat_q_tr, "q", "C200", "C300", "FL", "TR")
CCLAT_ALL(lat_t_fl, "t", "C200", "C300", "TR", "FL")
CCLAT_ALL(lat_t_tr, "t", "C200", "C300", "FL", "TR")
CCLAT_ALL(lat_p_fl, "p", "C200", "C300", "TR", "FL")
CCLAT_ALL(lat_p_tr, "p", "C200", "C300", "FL", "TR")
CCLAT_ALL(lat_s_fl, "s", "S200", "S300", "TR", "FL")
CCLAT_ALL(lat_s_tr, "s", "S200", "S300", "FL", "TR")
static const int CCLAT_N[9] = { 0, 1, 2, 3, 4, 5, 6, 8, 16 };
static const struct { const char *name; w32 (*fn[9])(void); } CCLATS[] = {
    { "TR.q, then FL.q", CCLAT_ROW(lat_q_fl) }, { "FL.q, then TR.q", CCLAT_ROW(lat_q_tr) },
    { "TR.q, then FL.t", CCLAT_ROW(lat_t_fl) }, { "FL.q, then TR.t", CCLAT_ROW(lat_t_tr) },
    { "TR.q, then FL.p", CCLAT_ROW(lat_p_fl) }, { "FL.q, then TR.p", CCLAT_ROW(lat_p_tr) },
    { "TR.q, then FL.s", CCLAT_ROW(lat_s_fl) }, { "FL.q, then TR.s", CCLAT_ROW(lat_s_tr) },
};

static w32 read_cc_late(void) {
    w32 v;
    __asm__ volatile(NOPS16 "mfvc %0, $131\n" : "=r"(v));
    return v;
}

static void section_vcmp_v3(void) {
    step("mfvc $131 0..16 instructions after vcmp, which size, which direction");
    setq(S, 0x3F800000, 0x3F800000, 0x3F800000, 0x3F800000);
    setq(T, 0x3F800000, 0x3F800000, 0x3F800000, 0x3F800000);
    __asm__ volatile("lv.q C200, 0(%0)\n\tlv.q C300, 0(%1)\n" :: "r"(S), "r"(T) : "memory");
    out("  instructions between:     ");
    for (int n = 0; n < 9; n++) out(" %3d", CCLAT_N[n]);
    out("\n");
    for (int r = 0; r < (int)(sizeof CCLATS / sizeof CCLATS[0]); r++) {
        out("  %-18s cc read:", CCLATS[r].name);
        for (int n = 0; n < 9; n++) out("  %02X", CCLATS[r].fn[n]());
        out("\n");
    }

    /* Steps 105 and 106 again, each read 16 instructions after its vcmp. */
    step("CC bits a narrow compare leaves alone, read 16 instructions later");
    setq(S, 0x3F800000, 0x3F800000, 0x3F800000, 0x3F800000);
    setq(T, 0x3F800000, 0x3F800000, 0x3F800000, 0x3F800000);
    __asm__ volatile("lv.q C200, 0(%0)\n\tlv.q C300, 0(%1)\n\tvcmp.q TR, C200, C300\n"
                     :: "r"(S), "r"(T) : "memory");
    out("  after vcmp.q TR:      cc %08X\n", read_cc_late());
    __asm__ volatile("vcmp.t FL, C200, C300\n");
    out("  then vcmp.t FL:       cc %08X\n", read_cc_late());
    __asm__ volatile("vcmp.q TR, C200, C300\n\tvcmp.p EQ, C200, C300\n");
    out("  vcmp.q TR, vcmp.p EQ: cc %08X\n", read_cc_late());
    __asm__ volatile("vcmp.q TR, C200, C300\n\tvcmp.s NE, S200, S300\n");
    out("  vcmp.q TR, vcmp.s NE: cc %08X\n", read_cc_late());
    __asm__ volatile("vcmp.q TR, C200, C300\n\tvcmp.s FL, S200, S300\n");
    out("  vcmp.q TR, vcmp.s FL: cc %08X\n", read_cc_late());
    __asm__ volatile("vcmp.q FL, C200, C300\n\tvcmp.p TR, C200, C300\n");
    out("  vcmp.q FL, vcmp.p TR: cc %08X\n", read_cc_late());
    setq(S, 0x3F800000, 0x00000000, 0x3F800000, 0x00000000);
    __asm__ volatile("lv.q C200, 0(%0)\n\tvcmp.q EZ, C200, C200\n" :: "r"(S) : "memory");
    out("  vcmp.q EZ on 1,0,1,0: cc %08X\n", read_cc_late());

    step("vcmovt / vcmovf on a known CC, read 16 instructions later");
    setq(S, 0x3F800000, 0x40000000, 0x40400000, 0x40800000);
    setq(T, 0x3F800000, 0x00000000, 0x40400000, 0x00000000);
    __asm__ volatile("lv.q C200, 0(%0)\n\tlv.q C300, 0(%1)\n\tvcmp.q EQ, C200, C300\n"
                     :: "r"(S), "r"(T) : "memory");
    out("  cc %08X\n", read_cc_late());
    sentinel();
    __asm__ volatile("lv.q C100, 0(%0)\n\tvcmovt.q C100, C200, 6\n\tsv.q C100, 0(%0)\n" :: "r"(D) : "memory");
    logq("  vcmovt.q imm 6 (per-lane)", D); out("\n");
    sentinel();
    __asm__ volatile("lv.q C100, 0(%0)\n\tvcmovf.q C100, C200, 6\n\tsv.q C100, 0(%0)\n" :: "r"(D) : "memory");
    logq("  vcmovf.q imm 6 (per-lane)", D); out("\n");
    sentinel();
    __asm__ volatile("lv.q C100, 0(%0)\n\tvcmovt.q C100, C200, 0\n\tsv.q C100, 0(%0)\n" :: "r"(D) : "memory");
    logq("  vcmovt.q imm 0 (CC bit 0)", D); out("\n");
    sentinel();
    __asm__ volatile("lv.q C100, 0(%0)\n\tvcmovt.q C100, C200, 5\n\tsv.q C100, 0(%0)\n" :: "r"(D) : "memory");
    logq("  vcmovt.q imm 5 (all bit)", D); out("\n");
    out("  cc after the vcmovs %08X\n", read_cc_late());
}

static void section_vcmp(void) {
    section("6. vcmp.s: bit 0 of CC for every condition and pair");
    out("values:"); for (int j = 0; j < NCMPVALS; j++) out(" %08X", CMPVALS[j]); out("\n");
    out("each row: s fixed, t across the values above; digit = CC bit 0 (lane result)\n");
    for (int c = 0; c < 16; c++) {
        step("vcmp.s %s", VCMPS[c].name);
        for (int i = 0; i < NCMPVALS; i++) {
            out("  %s s=%08X ", VCMPS[c].name, CMPVALS[i]);
            for (int j = 0; j < NCMPVALS; j++) {
                S[0] = CMPVALS[i]; T[0] = CMPVALS[j];
                VCMPS[c].fn();
                out("%u", (unsigned)(read_cc() & 1u));
            }
            out("\n");
        }
    }

    step("CC bits a narrow compare leaves alone (vfpu.c:1522)");
    setq(S, 0x3F800000, 0x3F800000, 0x3F800000, 0x3F800000);
    setq(T, 0x3F800000, 0x3F800000, 0x3F800000, 0x3F800000);
    __asm__ volatile("lv.q C200, 0(%0)\n\tlv.q C300, 0(%1)\n\tvcmp.q TR, C200, C300\n"
                     :: "r"(S), "r"(T) : "memory");
    out("  after vcmp.q TR:      cc %08X\n", read_cc());
    __asm__ volatile("vcmp.t FL, C200, C300\n");
    out("  then vcmp.t FL:       cc %08X\n", read_cc());
    __asm__ volatile("vcmp.q TR, C200, C300\n\tvcmp.p EQ, C200, C300\n");
    out("  vcmp.q TR, vcmp.p EQ: cc %08X\n", read_cc());
    __asm__ volatile("vcmp.q TR, C200, C300\n\tvcmp.s NE, S200, S300\n");
    out("  vcmp.q TR, vcmp.s NE: cc %08X\n", read_cc());
    setq(S, 0x3F800000, 0x00000000, 0x3F800000, 0x00000000);
    __asm__ volatile("lv.q C200, 0(%0)\n\tvcmp.q EZ, C200, C200\n" :: "r"(S) : "memory");
    out("  vcmp.q EZ on 1,0,1,0: cc %08X\n", read_cc());

    step("vcmovt / vcmovf on a known CC");
    setq(S, 0x3F800000, 0x40000000, 0x40400000, 0x40800000);
    setq(T, 0x3F800000, 0x00000000, 0x40400000, 0x00000000);
    __asm__ volatile("lv.q C200, 0(%0)\n\tlv.q C300, 0(%1)\n\tvcmp.q EQ, C200, C300\n"
                     :: "r"(S), "r"(T) : "memory");
    out("  cc %08X\n", read_cc());
    sentinel();
    __asm__ volatile("lv.q C100, 0(%0)\n\tvcmovt.q C100, C200, 6\n\tsv.q C100, 0(%0)\n" :: "r"(D) : "memory");
    logq("  vcmovt.q imm 6 (per-lane)", D); out("\n");
    sentinel();
    __asm__ volatile("lv.q C100, 0(%0)\n\tvcmovf.q C100, C200, 6\n\tsv.q C100, 0(%0)\n" :: "r"(D) : "memory");
    logq("  vcmovf.q imm 6 (per-lane)", D); out("\n");
    sentinel();
    __asm__ volatile("lv.q C100, 0(%0)\n\tvcmovt.q C100, C200, 0\n\tsv.q C100, 0(%0)\n" :: "r"(D) : "memory");
    logq("  vcmovt.q imm 0 (CC bit 0)", D); out("\n");
    sentinel();
    __asm__ volatile("lv.q C100, 0(%0)\n\tvcmovt.q C100, C200, 5\n\tsv.q C100, 0(%0)\n" :: "r"(D) : "memory");
    logq("  vcmovt.q imm 5 (all bit)", D); out("\n");

    section_vcmp_v3();
}

/* ---- 7. matrices ---------------------------------------------------------- */

static float MA[16] __attribute__((aligned(16)));
static float MB[16] __attribute__((aligned(16)));
static w32   MD[16] __attribute__((aligned(16)));

#define LOADM(m, p) "lv.q C" m "00, 0(" p ")\n\tlv.q C" m "10, 16(" p ")\n\t" \
                    "lv.q C" m "20, 32(" p ")\n\tlv.q C" m "30, 48(" p ")\n\t"
#define STOREM(m, p) "sv.q C" m "00, 0(" p ")\n\tsv.q C" m "10, 16(" p ")\n\t" \
                     "sv.q C" m "20, 32(" p ")\n\tsv.q C" m "30, 48(" p ")\n"

#define MAT_LIST(X)                                  \
    X(mx_vmmul_q,  "vmmul.q M300, M100, M200")       \
    X(mx_vmmul_t,  "vmmul.t M300, M100, M200")       \
    X(mx_vmmul_p,  "vmmul.p M300, M100, M200")       \
    X(mx_vmmul_eq, "vmmul.q M300, E100, M200")       \
    X(mx_vtfm4,    "vtfm4.q C300, M100, C200")       \
    X(mx_vtfm3,    "vtfm3.t C300, M100, C200")       \
    X(mx_vhtfm4,   "vhtfm4.q C300, M100, C200")      \
    X(mx_vhtfm3,   "vhtfm3.t C300, M100, C200")      \
    X(mx_vmscl,    "vmscl.q M300, M100, S200")       \
    X(mx_vmidt_t,  "vmidt.t M300")                   \
    X(mx_vmzero_p, "vmzero.p M300")                  \
    X(mx_vmone_t,  "vmone.t M300")                   \
    X(mx_vmmov_t,  "vmmov.t M300, E100")

#define X_DEF(id, insn) static void id(void) {                                 \
        __asm__ volatile(LOADM("1", "%0") LOADM("2", "%1") LOADM("3", "%2")     \
                         insn "\n\t" STOREM("3", "%2")                          \
                         :: "r"(MA), "r"(MB), "r"(MD) : "memory"); }
MAT_LIST(X_DEF)
#undef X_DEF
static const vop MATS[] = {
#define X_TAB(id, insn) { insn, id, 0 },
    MAT_LIST(X_TAB)
#undef X_TAB
};

static void section_matrix(void) {
    section("7. matrices (memory is column-major: C<m>c0 = words 4c..4c+3)");
    for (int i = 0; i < 16; i++) { MA[i] = (float)(i + 1); MB[i] = (float)((i * 7) % 11) - 3.0f; }
    MB[3] = 0.5f; MB[12] = 1.0f / 3.0f;
    out("A:"); for (int i = 0; i < 16; i++) out(" %08X", fb(MA[i])); out("\n");
    out("B:"); for (int i = 0; i < 16; i++) out(" %08X", fb(MB[i])); out("\n");
    for (int m = 0; m < (int)(sizeof MATS / sizeof MATS[0]); m++) {
        step("%s", MATS[m].insn);
        for (int i = 0; i < 16; i++) MD[i] = SENT;
        MATS[m].fn();
        out("  "); for (int i = 0; i < 16; i++) out("%08X%s", MD[i], (i & 3) == 3 ? "  " : " ");
        out("\n");
    }
}

/* ---- 8. vrot -------------------------------------------------------------- */

#define ROT_LIST(X)                                  \
    X(rt_cs,    "vrot.p C100, S200, [c,s]")          \
    X(rt_sc,    "vrot.p C100, S200, [s,c]")          \
    X(rt_cns,   "vrot.p C100, S200, [c,-s]")         \
    X(rt_nsc,   "vrot.p C100, S200, [-s,c]")         \
    X(rt_csss,  "vrot.q C100, S200, [c,s,s,s]")      \
    X(rt_c0s0,  "vrot.q C100, S200, [c,0,s,0]")      \
    X(rt_s00c,  "vrot.q C100, S200, [s,0,0,c]")      \
    X(rt_tc0s,  "vrot.t C100, S200, [c,0,-s]")

#define X_DEF(id, insn) static void id(void) { VRUN2(insn); }
ROT_LIST(X_DEF)
#undef X_DEF
static const vop ROTS[] = {
#define X_TAB(id, insn) { insn, id, 0 },
    ROT_LIST(X_TAB)
#undef X_TAB
};

static void section_vrot(void) {
    section("8. vrot (angle in quarter turns)");
    static const w32 ang[] = { 0x00000000, 0x3E800000, 0x3F000000, 0x3F800000,
                               0x3FC00000, 0x40000000, 0x40400000, 0xBF800000,
                               0x3DCCCCCD, 0x7FC00000 };
    for (int r = 0; r < (int)(sizeof ROTS / sizeof ROTS[0]); r++) {
        step("%s", ROTS[r].insn);
        for (int a = 0; a < (int)(sizeof ang / sizeof ang[0]); a++) {
            setq(S, ang[a], 0, 0, 0);
            sentinel();
            ROTS[r].fn();
            out("  %08X", ang[a]); logq(" ->", D); out("\n");
        }
    }
}

/* ---- 9. prefixes ----------------------------------------------------------
 * Raw encodings: vpfxs = 0xDC000000 | imm, vpfxt = 0xDD000000 | imm,
 * vpfxd = 0xDE000000 | imm. Source/target imm: swizzle 2 bits per lane at
 * 0..7, abs at 8..11, constant at 12..15, negate at 16..19. Destination imm:
 * saturation 2 bits per lane at 0..7, write mask at 8..11. */

#define PFX(id, pre, insn, label) static void id(void) {                              \
        __asm__ volatile("lv.q C100, 0(%0)\n\t" "lv.q C200, 0(%1)\n\t"          \
                         "lv.q C300, 0(%2)\n\t" pre "\n\t" insn "\n\t"          \
                         "sv.q C100, 0(%0)\n" :: "r"(D), "r"(S), "r"(T) : "memory"); }

#define PFX_LIST(X)                                                                              \
    X(pf_swz_wzyx,  ".word 0xDC00001B",               "vmov.q C100, C200", "S swizzle w,z,y,x")  \
    X(pf_swz_xxxx,  ".word 0xDC000000",               "vmov.q C100, C200", "S swizzle x,x,x,x")  \
    X(pf_abs,       ".word 0xDC000FE4",               "vmov.q C100, C200", "S abs all")          \
    X(pf_neg,       ".word 0xDC0F00E4",               "vmov.q C100, C200", "S negate all")       \
    X(pf_negabs,    ".word 0xDC0F0FE4",               "vmov.q C100, C200", "S -|x| all")         \
    X(pf_k0123,     ".word 0xDC00F0E4",               "vmov.q C100, C200", "S constants 0,1,2,1/2") \
    X(pf_k4567,     ".word 0xDC00FFE4",               "vmov.q C100, C200", "S constants 3,1/3,1/4,1/6") \
    X(pf_kneg,      ".word 0xDC0FF0E4",               "vmov.q C100, C200", "S negated constants 0,1,2,1/2") \
    X(pf_zpair,     ".word 0xDC0000EE",               "vmov.p C100, C200", "S swizzle z,w on a pair") \
    X(pf_t_swz,     ".word 0xDD00001B",               "vadd.q C100, C200, C300", "T swizzle w,z,y,x") \
    X(pf_t_k,       ".word 0xDD00F055",               "vadd.q C100, C200, C300", "T constants") \
    X(pf_st,        ".word 0xDC0A00E4\n\t.word 0xDD050000", "vmul.q C100, C200, C300", "S negate y,w; T x,x,x,x negate x,z") \
    X(pf_sat01,     ".word 0xDE000055",               "vmov.q C100, C200", "D saturate [0,1] all") \
    X(pf_sat11,     ".word 0xDE0000FF",               "vmov.q C100, C200", "D saturate [-1,1] all") \
    X(pf_mask,      ".word 0xDE000A00",               "vmov.q C100, C200", "D mask y,w") \
    X(pf_mask_sat,  ".word 0xDE000511",               "vmov.q C100, C200", "D mask x,z, saturate x,z [0,1]") \
    X(pf_dot_sat,   ".word 0xDE000001",               "vdot.q S100, C200, C300", "D saturate [0,1] on vdot") \
    X(pf_nonvfpu,   ".word 0xDC00001B\n\taddiu $zero, $zero, 1", "vmov.q C100, C200", "S swizzle, then a non-VFPU instruction") \
    X(pf_lvq,       ".word 0xDC00001B\n\tlv.q C200, 0(%1)", "vmov.q C100, C200", "S swizzle, then lv.q, then vmov")

PFX_LIST(PFX)
#undef PFX
static const struct { const char *label; void (*fn)(void); } PFXS[] = {
#define X_TAB(id, pre, insn, label) { label ": " insn, id },
    PFX_LIST(X_TAB)
#undef X_TAB
};

static void section_prefix(void) {
    section("9. prefixes");
    static const w32 srcs[][4] = {
        { 0x3F800000, 0xC0000000, 0x40400000, 0xC0800000 },   /* 1, -2, 3, -4 */
        { 0x80000000, 0x7FC00000, 0xFFC00000, 0x00000000 },   /* -0, +NaN, -NaN, 0 */
        { 0xBF000000, 0x3F400000, 0x7F800000, 0xFF800000 },   /* -0.5, 0.75, inf, -inf */
    };
    static const w32 tq[4] = { 0x3F800000, 0x40000000, 0x40400000, 0x40800000 };
    for (int p = 0; p < (int)(sizeof PFXS / sizeof PFXS[0]); p++) {
        step("%s", PFXS[p].label);
        for (int i = 0; i < 3; i++) {
            memcpy(S, srcs[i], 16); memcpy(T, tq, 16);
            sentinel();
            PFXS[p].fn();
            logq("  s", S); logq(" ->", D); out("\n");
        }
    }
}

/* ---- 10. what a fresh thread's registers hold -----------------------------
 *
 * The entry point is assembly so nothing runs before the general registers
 * are saved; the C body then stores the FPU and VFPU files before it computes
 * anything with them. */

static w32 g_gpr[32];
static w32 g_fpr[32];
static w32 g_vpr[128] __attribute__((aligned(16)));
static w32 g_fcr31_fresh;
static w32 g_ctrl_fresh[16];
static w32 g_rnd_fresh[2];

int fresh_body(w32 *saved);

__asm__(
    ".text\n"
    ".set push\n.set noreorder\n.set noat\n"
    ".global fresh_entry\n"
    ".ent fresh_entry\n"
    "fresh_entry:\n"
    "  addiu $sp, $sp, -144\n"
    "  sw $1, 4($sp)\n  sw $2, 8($sp)\n  sw $3, 12($sp)\n  sw $4, 16($sp)\n"
    "  sw $5, 20($sp)\n  sw $6, 24($sp)\n  sw $7, 28($sp)\n  sw $8, 32($sp)\n"
    "  sw $9, 36($sp)\n  sw $10, 40($sp)\n  sw $11, 44($sp)\n  sw $12, 48($sp)\n"
    "  sw $13, 52($sp)\n  sw $14, 56($sp)\n  sw $15, 60($sp)\n  sw $16, 64($sp)\n"
    "  sw $17, 68($sp)\n  sw $18, 72($sp)\n  sw $19, 76($sp)\n  sw $20, 80($sp)\n"
    "  sw $21, 84($sp)\n  sw $22, 88($sp)\n  sw $23, 92($sp)\n  sw $24, 96($sp)\n"
    "  sw $25, 100($sp)\n  sw $26, 104($sp)\n  sw $27, 108($sp)\n  sw $28, 112($sp)\n"
    "  sw $30, 120($sp)\n  sw $31, 124($sp)\n"
    "  addiu $1, $sp, 144\n  sw $1, 116($sp)\n"
    "  sw $31, 128($sp)\n"
    "  jal fresh_body\n"
    "  move $4, $sp\n"
    "  lw $31, 128($sp)\n"
    "  jr $31\n"
    "  addiu $sp, $sp, 144\n"
    ".end fresh_entry\n"
    ".set pop\n");
int fresh_entry(SceSize args, void *argp);

int fresh_body(w32 *saved) {
    __asm__ volatile(
        "swc1 $f0, 0(%0)\n\t"   "swc1 $f1, 4(%0)\n\t"   "swc1 $f2, 8(%0)\n\t"   "swc1 $f3, 12(%0)\n\t"
        "swc1 $f4, 16(%0)\n\t"  "swc1 $f5, 20(%0)\n\t"  "swc1 $f6, 24(%0)\n\t"  "swc1 $f7, 28(%0)\n\t"
        "swc1 $f8, 32(%0)\n\t"  "swc1 $f9, 36(%0)\n\t"  "swc1 $f10, 40(%0)\n\t" "swc1 $f11, 44(%0)\n\t"
        "swc1 $f12, 48(%0)\n\t" "swc1 $f13, 52(%0)\n\t" "swc1 $f14, 56(%0)\n\t" "swc1 $f15, 60(%0)\n\t"
        "swc1 $f16, 64(%0)\n\t" "swc1 $f17, 68(%0)\n\t" "swc1 $f18, 72(%0)\n\t" "swc1 $f19, 76(%0)\n\t"
        "swc1 $f20, 80(%0)\n\t" "swc1 $f21, 84(%0)\n\t" "swc1 $f22, 88(%0)\n\t" "swc1 $f23, 92(%0)\n\t"
        "swc1 $f24, 96(%0)\n\t" "swc1 $f25, 100(%0)\n\t" "swc1 $f26, 104(%0)\n\t" "swc1 $f27, 108(%0)\n\t"
        "swc1 $f28, 112(%0)\n\t" "swc1 $f29, 116(%0)\n\t" "swc1 $f30, 120(%0)\n\t" "swc1 $f31, 124(%0)\n"
        :: "r"(g_fpr) : "memory");
    __asm__ volatile(
        "sv.q C000, 0(%0)\n\tsv.q C010, 16(%0)\n\tsv.q C020, 32(%0)\n\tsv.q C030, 48(%0)\n\t"
        "sv.q C100, 64(%0)\n\tsv.q C110, 80(%0)\n\tsv.q C120, 96(%0)\n\tsv.q C130, 112(%0)\n"
        :: "r"(g_vpr) : "memory");
    __asm__ volatile(
        "sv.q C200, 128(%0)\n\tsv.q C210, 144(%0)\n\tsv.q C220, 160(%0)\n\tsv.q C230, 176(%0)\n\t"
        "sv.q C300, 192(%0)\n\tsv.q C310, 208(%0)\n\tsv.q C320, 224(%0)\n\tsv.q C330, 240(%0)\n\t"
        "sv.q C400, 256(%0)\n\tsv.q C410, 272(%0)\n\tsv.q C420, 288(%0)\n\tsv.q C430, 304(%0)\n\t"
        "sv.q C500, 320(%0)\n\tsv.q C510, 336(%0)\n\tsv.q C520, 352(%0)\n\tsv.q C530, 368(%0)\n\t"
        "sv.q C600, 384(%0)\n\tsv.q C610, 400(%0)\n\tsv.q C620, 416(%0)\n\tsv.q C630, 432(%0)\n\t"
        "sv.q C700, 448(%0)\n\tsv.q C710, 464(%0)\n\tsv.q C720, 480(%0)\n\tsv.q C730, 496(%0)\n"
        :: "r"(g_vpr) : "memory");
    __asm__ volatile("cfc1 %0, $31\n" : "=r"(g_fcr31_fresh));
    mfvc_all(g_ctrl_fresh);
    for (int i = 0; i < 32; i++) g_gpr[i] = saved[i];
    /* Last, so a runtime without vrndi still reports everything above. */
    g_rnd_fresh[0] = rnd_i();
    g_rnd_fresh[1] = rnd_i();
    return 0;
}

/* ---- 10, version 3: where fp and k0 point ----------------------------------
 *
 * Version 1 logged a fresh thread's fp and k0 as "other". Here they are given
 * against the thread's own $sp at entry and against the top of its stack
 * (ReferThreadStatus's stack + stackSize), which say where they point without
 * naming an address. */

static w32 g_fcr31_main;   /* main()'s first instruction reads it */

static int gp_now(void) { int v; __asm__ volatile("move %0, $gp\n" : "=r"(v)); return v; }

/* A register as a fill value, or as an offset from sp or from the stack top
 * when it lies within 64 KB of them, else "elsewhere". */
static void rel(char *buf, int cap, w32 v, w32 sp, w32 top) {
    if (v == 0 || v == 0xDEADBEEFu || v == 0xFFFFFFFFu) snprintf(buf, cap, "%08X", v);
    else if (v - sp + 0x10000u < 0x20000u) snprintf(buf, cap, "sp%+d (top%+d)", (int)(v - sp), (int)(v - top));
    else if (v - top + 0x10000u < 0x20000u) snprintf(buf, cap, "top%+d", (int)(v - top));
    else snprintf(buf, cap, "elsewhere");
}

static void section_fresh_v3(void) {
    step("the main thread's fcr31 when the probe started");
    out("  fcr31 %08X\n", g_fcr31_main);

    step("fresh threads' fp, k0 and a1 against their sp and stack top, 0 and 16 bytes of arguments");
    static char argbuf[16] = "vfpuprobe args";
    w32 ra[2] = { 0, 0 };
    for (int a = 0; a < 2; a++) {
        const int len = a ? 16 : 0;
        memset(g_gpr, 0x11, sizeof g_gpr);
        SceUID th = sceKernelCreateThread("fresh", fresh_entry, 0x10, 0x4000,
                                          PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU, NULL);
        if (th < 0) { out("  args %2d: create", len); ret(th); continue; }
        int r = sceKernelStartThread(th, len, a ? argbuf : NULL);
        SceUInt timeout = 1000000;
        int w = sceKernelWaitThreadEnd(th, &timeout);
        SceKernelThreadInfo info;
        memset(&info, 0, sizeof info);
        info.size = sizeof info;
        int rr = sceKernelReferThreadStatus(th, &info);
        sceKernelDeleteThread(th);
        out("  args %2d: start %08X, wait end %08X, refer %08X\n", len, (unsigned)r, (unsigned)w, (unsigned)rr);
        /* Without the stack top, offsets from it read as offsets from sp. */
        const w32 sp = g_gpr[29], top = rr >= 0 ? (w32)info.stack + (w32)info.stackSize : sp;
        char fp[40], k0[40], a1[40];
        rel(fp, sizeof fp, g_gpr[30], sp, top);
        rel(k0, sizeof k0, g_gpr[26], sp, top);
        rel(a1, sizeof a1, g_gpr[5], sp, top);
        out("  args %2d: sp = top%+d (sp&15 %u); fp %s; k0 %s; a1 %s; a0 %u; gp %s main's\n", len,
            (int)(sp - top), (unsigned)(sp & 15u), fp, k0, a1, g_gpr[4],
            g_gpr[28] == (w32)gp_now() ? "same as" : "not");
        ra[a] = g_gpr[31];
    }
    out("  ra the same for both threads: %s\n", ra[0] == ra[1] ? "yes" : "no");
}

static void log_gpr(int i, const char *name) {
    const w32 v = g_gpr[i];
    if (v == 0xDEADBEEFu || v == 0 || v == 0xFFFFFFFFu) out("  %-4s %08X\n", name, v);
    else out("  %-4s other\n", name);
}

static void section_fresh(void) {
    section("10. a fresh thread's registers");
    step("create and start a VFPU thread at priority 0x10 that saves its registers");
    memset(g_gpr, 0x11, sizeof g_gpr);
    SceUID th = sceKernelCreateThread("fresh", fresh_entry, 0x10, 0x4000,
                                      PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU, NULL);
    out("  create: %s\n", th > 0 ? "uid" : "error");
    if (th < 0) { ret(th); return; }
    int r = sceKernelStartThread(th, 0, NULL);
    out("  start = %08X\n", (unsigned)r);
    SceUInt timeout = 1000000;
    r = sceKernelWaitThreadEnd(th, &timeout);
    out("  wait end = %08X\n", (unsigned)r);
    sceKernelDeleteThread(th);

    static const char *const NAMES[32] = {
        "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
        "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
        "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
        "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra" };
    out("general registers at entry (DEADBEEF, 0 and FFFFFFFF shown, anything else 'other'):\n");
    out("  a0   %08X (argument size)\n", g_gpr[4]);
    for (int i = 1; i < 32; i++) {
        if (i == 4 || i == 5 || i == 28 || i == 29 || i == 31) continue;   /* args, gp, sp, ra */
        log_gpr(i, NAMES[i]);
    }
    out("fpu registers:\n  ");
    for (int i = 0; i < 32; i++) out("%08X%s", g_fpr[i], (i & 7) == 7 ? "\n  " : " ");
    out("\nfcr31 %08X\n", g_fcr31_fresh);
    out("vfpu registers (column order, C<m><c>0):\n");
    for (int q = 0; q < 32; q++) {
        if ((q & 3) == 0) out("  M%d:", q >> 2);
        out(" [%08X %08X %08X %08X]", g_vpr[q * 4], g_vpr[q * 4 + 1], g_vpr[q * 4 + 2], g_vpr[q * 4 + 3]);
        if ((q & 3) == 3) out("\n");
    }
    out("vfpu control: pfxs %08X pfxt %08X pfxd %08X cc %08X rev %08X\n",
        g_ctrl_fresh[0], g_ctrl_fresh[1], g_ctrl_fresh[2], g_ctrl_fresh[3], g_ctrl_fresh[7]);
    out("rcx0-7:"); for (int i = 8; i < 16; i++) out(" %08X", g_ctrl_fresh[i]); out("\n");
    out("first two vrndi: %08X %08X\n", g_rnd_fresh[0], g_rnd_fresh[1]);

    section_fresh_v3();
}

/* ---- 11. the FPU ----------------------------------------------------------- */

#define FCR_RW(n) static w32 fcr_rw_##n(w32 v) {                               \
        w32 r;                                                                 \
        __asm__ volatile("ctc1 %1, $" #n "\n\tnop\n\tcfc1 %0, $" #n "\n\t"     \
                         "ctc1 $zero, $" #n "\n" : "=r"(r) : "r"(v));          \
        return r; }
FCR_RW(1) FCR_RW(2) FCR_RW(25) FCR_RW(26) FCR_RW(28) FCR_RW(30)

static w32 fcr31_rw(w32 v) {
    w32 r;
    __asm__ volatile("ctc1 %1, $31\n\tnop\n\tcfc1 %0, $31\n\tctc1 $zero, $31\n"
                     : "=r"(r) : "r"(v));
    return r;
}

/* One FPU operation with FCR31 set to `fcr` first; returns the result and the
 * FCR31 it left. */
#define FOP2(id, insn) static w32 id(w32 a, w32 b, w32 fcr, w32 *after) {      \
        w32 r, f;                                                              \
        __asm__ volatile("mtc1 %2, $f12\n\tmtc1 %3, $f14\n\tctc1 %4, $31\n\t"  \
                         "nop\n\t" insn " $f0, $f12, $f14\n\tnop\n\t"          \
                         "cfc1 %1, $31\n\tmfc1 %0, $f0\n\tctc1 $zero, $31\n"   \
                         : "=&r"(r), "=&r"(f) : "r"(a), "r"(b), "r"(fcr)       \
                         : "$f0", "$f12", "$f14");                             \
        *after = f; return r; }
#define FOP1(id, insn) static w32 id(w32 a, w32 b, w32 fcr, w32 *after) {      \
        w32 r, f; (void)b;                                                     \
        __asm__ volatile("mtc1 %2, $f12\n\tctc1 %3, $31\n\tnop\n\t"            \
                         insn " $f0, $f12\n\tnop\n\t"                          \
                         "cfc1 %1, $31\n\tmfc1 %0, $f0\n\tctc1 $zero, $31\n"   \
                         : "=&r"(r), "=&r"(f) : "r"(a), "r"(fcr)               \
                         : "$f0", "$f12");                                     \
        *after = f; return r; }
FOP2(f_add, "add.s") FOP2(f_sub, "sub.s") FOP2(f_mul, "mul.s") FOP2(f_div, "div.s")
FOP1(f_sqrt, "sqrt.s") FOP1(f_cvtws, "cvt.w.s") FOP1(f_roundw, "round.w.s")
FOP1(f_truncw, "trunc.w.s") FOP1(f_ceilw, "ceil.w.s") FOP1(f_floorw, "floor.w.s")
FOP1(f_cvtsw, "cvt.s.w") FOP1(f_neg, "neg.s") FOP1(f_abs, "abs.s") FOP1(f_mov, "mov.s")

typedef w32 (*fop_fn)(w32, w32, w32, w32 *);

static void fop(const char *name, fop_fn fn, w32 a, w32 b, w32 fcr) {
    w32 after;
    w32 r = fn(a, b, fcr, &after);
    out("  %-10s %08X %08X fcr %08X -> %08X fcr %08X\n", name, a, b, fcr, r, after);
}

/* c.cond.s for all 16 conditions, reading FCC (bit 23) back. */
#define FCMP(c) static w32 fc_##c(w32 a, w32 b, w32 *after) {                  \
        w32 f;                                                                 \
        __asm__ volatile("mtc1 %1, $f12\n\tmtc1 %2, $f14\n\tctc1 $zero, $31\n\t" \
                         "nop\n\tc." #c ".s $f12, $f14\n\tnop\n\t"             \
                         "cfc1 %0, $31\n\tctc1 $zero, $31\n"                   \
                         : "=&r"(f) : "r"(a), "r"(b) : "$f12", "$f14");         \
        *after = f; return (f >> 23) & 1; }
#define FCMP_LIST(X) X(f) X(un) X(eq) X(ueq) X(olt) X(ult) X(ole) X(ule) \
                     X(sf) X(ngle) X(seq) X(ngl) X(lt) X(nge) X(le) X(ngt)
FCMP_LIST(FCMP)
static const struct { const char *name; w32 (*fn)(w32, w32, w32 *); } FCMPS[16] = {
#define X_TAB(c) { "c." #c ".s", fc_##c },
    FCMP_LIST(X_TAB)
#undef X_TAB
};

/* ---- 11, version 3: the COP1 cases version 1 generalised over -------------- */

static void section_fpu_v3(void) {
    const w32 fs = 0x01000000, one = 0x3F800000;

    step("div.s overflow and underflow flags, every rounding mode");
    static const struct { const char *name; w32 a, b; } DIV[] = {
        { "max/0.5",   0x7F7FFFFF, 0x3F000000 }, { "-max/0.5", 0xFF7FFFFF, 0x3F000000 },
        { "max/den",   0x7F7FFFFF, 0x00000001 }, { "1/2^-128", 0x3F800000, 0x00200000 },
        { "1/2^-127",  0x3F800000, 0x00400000 }, { "min/3",    0x00800000, 0x40400000 },
        { "-min/3",    0x80800000, 0x40400000 }, { "min/2",    0x00800000, 0x40000000 },
        { "den/1",     0x00400000, 0x3F800000 }, { "den/den",  0x00000001, 0x00000001 },
        { "0/den",     0x00000000, 0x00000001 }, { "min/2^24", 0x00800000, 0x4B800000 },
    };
    for (int rm = 0; rm < 4; rm++)
        for (int i = 0; i < (int)(sizeof DIV / sizeof DIV[0]); i++)
            fop(DIV[i].name, f_div, DIV[i].a, DIV[i].b, (w32)rm);
    for (int i = 0; i < (int)(sizeof DIV / sizeof DIV[0]); i++)
        fop(DIV[i].name, f_div, DIV[i].a, DIV[i].b, fs);

    step("neg.s, abs.s and mov.s of -sNaN, -qNaN, +sNaN and other signed specials");
    static const w32 SGN[] = { 0xFF800001, 0xFFC00000, 0x7F800001, 0x7FC00001, 0xFFFFFFFF,
                               0x7FBFFFFF, 0xFFBFFFFF, 0xFF800000, 0x80000000, 0x80000001 };
    for (int i = 0; i < (int)(sizeof SGN / sizeof SGN[0]); i++) {
        fop("neg", f_neg, SGN[i], 0, 0);
        fop("abs", f_abs, SGN[i], 0, 0);
        fop("mov", f_mov, SGN[i], 0, 0);
    }

    step("sqrt.s of -0, denormals and the smallest normals: FS 0 and 1, every rounding mode");
    static const w32 SQ[] = { 0x80000000, 0x00000000, 0x00000001, 0x00000002, 0x00400000,
                              0x007FFFFF, 0x80000001, 0x807FFFFF, 0x00800000, 0x00800001,
                              0xFF800000, 0x7F800000 };
    for (int i = 0; i < (int)(sizeof SQ / sizeof SQ[0]); i++) {
        fop("sqrt", f_sqrt, SQ[i], 0, 0);
        fop("sqrt", f_sqrt, SQ[i], 0, fs);
    }
    for (int rm = 1; rm < 4; rm++) {
        fop("sqrt", f_sqrt, 0x00000001, 0, (w32)rm);
        fop("sqrt", f_sqrt, 0x007FFFFF, 0, (w32)rm);
        fop("sqrt", f_sqrt, 0x00000002, 0, (w32)rm);
    }

    /* Version 1 ORed the flags over a row that held both NaN kinds. */
    step("c.cond.s: FCC and flags for each pair, fcr31 cleared before each");
    static const struct { w32 a, b; } CP[] = {
        { one, 0x7FC00000 }, { 0x7FC00000, one }, { one, 0x7F800001 }, { 0x7F800001, one },
        { 0x7FC00000, 0x7FC00000 }, { 0x7F800001, 0x7F800001 }, { 0xFFC00000, one }, { one, one },
    };
    const int ncp = (int)(sizeof CP / sizeof CP[0]);
    out("  pairs:");
    for (int j = 0; j < ncp; j++) out(" %08X/%08X", CP[j].a, CP[j].b);
    out("\n  each: FCC:flags and cause bits (fcr31 & 0003F07C)\n");
    for (int c = 0; c < 16; c++) {
        out("  %-8s", FCMPS[c].name);
        for (int j = 0; j < ncp; j++) {
            w32 after;
            const w32 cc = FCMPS[c].fn(CP[j].a, CP[j].b, &after);
            out(" %u:%05X", (unsigned)cc, after & 0x0003F07Cu);
        }
        out("\n");
    }
}

static void section_fpu(void) {
    section("11. the FPU");

    step("fcr0 and the unimplemented control registers (cpu.h:80)");
    w32 fcr0; __asm__ volatile("cfc1 %0, $0\n" : "=r"(fcr0));
    out("  fcr0 %08X\n", fcr0);
    out("  fcr1  write FFFFFFFF -> %08X\n", fcr_rw_1(0xFFFFFFFFu));
    out("  fcr2  write FFFFFFFF -> %08X\n", fcr_rw_2(0xFFFFFFFFu));
    out("  fcr25 write FFFFFFFF -> %08X\n", fcr_rw_25(0xFFFFFFFFu));
    out("  fcr26 write FFFFFFFF -> %08X\n", fcr_rw_26(0xFFFFFFFFu));
    out("  fcr28 write FFFFFFFF -> %08X\n", fcr_rw_28(0xFFFFFFFFu));
    out("  fcr30 write FFFFFFFF -> %08X\n", fcr_rw_30(0xFFFFFFFFu));

    step("fcr31 write/read by field (cpu.h:86); enables and causes never together");
    static const w32 fields[] = { 0x00000003, 0x0000007C, 0x00000F80, 0x0001F000,
                                  0x00800000, 0x01000000, 0x00400000, 0x00200000,
                                  0xFE000000, 0x00040000, 0x00080000, 0x00100000 };
    for (int i = 0; i < (int)(sizeof fields / sizeof fields[0]); i++)
        out("  write %08X -> %08X\n", fields[i], fcr31_rw(fields[i]));

    step("exception cause and flag bits (recomp_rt.h:415)");
    const w32 one = 0x3F800000, three = 0x40400000, zero = 0, max = 0x7F7FFFFF, nan = 0x7FC00000;
    fop("sqrt(-1)", f_sqrt, 0xBF800000, 0, 0);
    fop("0/0",      f_div,  zero, zero, 0);
    fop("nan*nan",  f_mul,  nan, nan, 0);
    fop("max*max",  f_mul,  max, max, 0);
    fop("1/max",    f_div,  one, max, 0);
    fop("1/3",      f_div,  one, three, 0);
    fop("1/0",      f_div,  one, zero, 0);
    fop("-1/0",     f_div,  0xBF800000, zero, 0);
    fop("inf-inf",  f_sub,  0x7F800000, 0x7F800000, 0);
    fop("snan+1",   f_add,  0x7F800001, one, 0);
    fop("1+1",      f_add,  one, one, 0);
    fop("1+1 flags", f_add, one, one, 0x0000007C);
    fop("den+0",    f_add,  0x00000001, zero, 0);
    fop("den*1",    f_mul,  0x00400000, one, 0);
    fop("tiny*tiny", f_mul, 0x1E3CE508, 0x1E3CE508, 0);   /* 1e-20 squared */
    fop("cvt.w 3e9", f_cvtws, 0x4F32D05E, 0, 0);
    fop("cvt.w nan", f_cvtws, nan, 0, 0);
    fop("cvt.w inf", f_cvtws, 0x7F800000, 0, 0);
    fop("cvt.w -inf", f_cvtws, 0xFF800000, 0, 0);
    fop("cvt.w 2^31", f_cvtws, 0x4F000000, 0, 0);
    fop("cvt.s.w big", f_cvtsw, 0x7FFFFFFF, 0, 0);
    fop("neg nan",  f_neg,  nan, 0, 0);
    fop("abs -nan", f_abs,  0xFFC00000, 0, 0);
    fop("mov snan", f_mov,  0x7F800001, 0, 0);
    fop("neg snan", f_neg,  0x7F800001, 0, 0);

    step("rounding modes 0..3 (recomp_rt.h:346)");
    static const struct { const char *name; fop_fn fn; w32 a, b; } R[] = {
        { "1/3",       f_div,    0x3F800000, 0x40400000 },
        { "-1/3",      f_div,    0xBF800000, 0x40400000 },
        { "1.1*1.1",   f_mul,    0x3F8CCCCD, 0x3F8CCCCD },
        { "1+2^-24*3", f_add,    0x3F800000, 0x34400000 },
        { "1-2^-25",   f_sub,    0x3F800000, 0x33000000 },
        { "sqrt 2",    f_sqrt,   0x40000000, 0 },
        { "cvt.w 2.5", f_cvtws,  0x40200000, 0 },
        { "cvt.w -2.5", f_cvtws, 0xC0200000, 0 },
        { "cvt.w 1.5", f_cvtws,  0x3FC00000, 0 },
        { "cvt.w -0.5", f_cvtws, 0xBF000000, 0 },
        { "round.w 2.5", f_roundw, 0x40200000, 0 },
        { "trunc.w -2.5", f_truncw, 0xC0200000, 0 },
        { "ceil.w 2.1", f_ceilw, 0x40066666, 0 },
        { "floor.w -2.1", f_floorw, 0xC0066666, 0 },
        { "cvt.s.w 2^24+1", f_cvtsw, 0x01000001, 0 },
        { "cvt.s.w -(2^24+1)", f_cvtsw, 0xFEFFFFFF, 0 },
        { "max+max",   f_add,    0x7F7FFFFF, 0x7F7FFFFF },
        { "-max-max",  f_sub,    0xFF7FFFFF, 0x7F7FFFFF },
        { "tiny/2",    f_div,    0x00800001, 0x40000000 },
    };
    for (int rm = 0; rm < 4; rm++)
        for (int i = 0; i < (int)(sizeof R / sizeof R[0]); i++)
            fop(R[i].name, R[i].fn, R[i].a, R[i].b, (w32)rm);

    step("flush-to-zero, FS = 0x01000000 (recomp_rt.h:346)");
    const w32 fs = 0x01000000;
    fop("tiny*tiny", f_mul, 0x1E3CE508, 0x1E3CE508, fs);
    fop("den+0",     f_add, 0x00000001, zero, fs);
    fop("den*1",     f_mul, 0x00400000, one, fs);
    fop("min/2",     f_div, 0x00800000, 0x40000000, fs);
    fop("-min/2",    f_div, 0x80800000, 0x40000000, fs);
    fop("den+min",   f_add, 0x00400000, 0x00800000, fs);
    fop("cvt.w den", f_cvtws, 0x00400000, 0, fs);
    fop("mov den",   f_mov, 0x00400000, 0, fs);
    fop("neg den",   f_neg, 0x00400000, 0, fs);

    step("c.cond.s: FCC (bit 23) for 16 conditions over value pairs");
    static const w32 cv[] = { 0x00000000, 0x80000000, 0x3F800000, 0xBF800000,
                              0x7F800000, 0x7FC00000, 0x7F800001 };
    const int ncv = (int)(sizeof cv / sizeof cv[0]);
    out("values:"); for (int j = 0; j < ncv; j++) out(" %08X", cv[j]); out("\n");
    for (int c = 0; c < 16; c++) {
        for (int i = 0; i < ncv; i++) {
            out("  %-8s a=%08X ", FCMPS[c].name, cv[i]);
            w32 flagsor = 0;
            for (int j = 0; j < ncv; j++) {
                w32 after;
                out("%u", (unsigned)FCMPS[c].fn(cv[i], cv[j], &after));
                flagsor |= after & 0x0007F07Cu;
            }
            out("  (flags/cause seen %08X)\n", flagsor);
        }
    }

    section_fpu_v3();
}

/* ---- 12, version 3: does an enabled FPU exception trap? ---------------------
 *
 * A fresh thread's fcr31 is 00000E00 (vfpuprobe 1 step 147): the overflow,
 * divide-by-zero and invalid enables are set. Each case runs in its own
 * thread, which does nothing else, and reports how far it got; the main thread
 * waits for it with a timeout. A case that takes the PSP down is skipped when
 * the probe is started again. On firmware 6.60 all four did (vfpuprobe 3,
 * four runs that each ended in the next case), so from version 4 they are
 * known crashes: "not run" unless built with -DRUN_KNOWN_CRASHES. */

/* Stage 1: entered and read fcr31; 2: about to run the operation; 3: done. */
static volatile w32 g_trap_stage, g_trap_entry, g_trap_after, g_trap_res;
static w32 g_trap_a, g_trap_b, g_trap_fcr;
static int g_trap_op;   /* 0 div, 1 mul, 3 only the ctc1 */

static int trap_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    w32 f, r = 0;
    __asm__ volatile("cfc1 %0, $31\n" : "=r"(f));
    g_trap_entry = f;
    g_trap_stage = 1;
    if (g_trap_op == 3) {
        __asm__ volatile("ctc1 %1, $31\n\tnop\n\tcfc1 %0, $31\n" : "=r"(f) : "r"(g_trap_fcr));
    } else {
        if (g_trap_fcr != 0xFFFFFFFFu) __asm__ volatile("ctc1 %0, $31\n\tnop\n" :: "r"(g_trap_fcr));
        g_trap_stage = 2;
        if (g_trap_op == 0)
            __asm__ volatile("mtc1 %2, $f12\n\tmtc1 %3, $f14\n\tnop\n\tdiv.s $f0, $f12, $f14\n\tnop\n\t"
                             "cfc1 %1, $31\n\tmfc1 %0, $f0\n"
                             : "=&r"(r), "=&r"(f) : "r"(g_trap_a), "r"(g_trap_b) : "$f0", "$f12", "$f14");
        else
            __asm__ volatile("mtc1 %2, $f12\n\tmtc1 %3, $f14\n\tnop\n\tmul.s $f0, $f12, $f14\n\tnop\n\t"
                             "cfc1 %1, $31\n\tmfc1 %0, $f0\n"
                             : "=&r"(r), "=&r"(f) : "r"(g_trap_a), "r"(g_trap_b) : "$f0", "$f12", "$f14");
    }
    g_trap_res = r;
    g_trap_after = f;
    g_trap_stage = 3;
    __asm__ volatile("ctc1 $zero, $31\n");
    return 0;
}

/* fcr 0xFFFFFFFF leaves the thread's own fcr31 alone. */
static void trap_case(const char *title, int op, w32 a, w32 b, w32 fcr) {
    if (step("%s", title) || KNOWN_CRASH("stopped the PSP on firmware 6.60 (vfpuprobe 3)")) return;
    g_trap_op = op; g_trap_a = a; g_trap_b = b; g_trap_fcr = fcr;
    g_trap_stage = 0; g_trap_entry = g_trap_after = g_trap_res = 0x5A5A5A5Au;
    SceUID th = sceKernelCreateThread("trap", trap_thread, 0x10, 0x4000, PSP_THREAD_ATTR_USER, NULL);
    if (th < 0) { out("  create"); ret(th); return; }
    int r = sceKernelStartThread(th, 0, NULL);
    SceUInt timeout = 2000000;
    int w = sceKernelWaitThreadEnd(th, &timeout);
    out("  start %08X, wait end %08X, the thread got to stage %u of 3\n",
        (unsigned)r, (unsigned)w, (unsigned)g_trap_stage);
    out("  fcr31 at entry %08X; result %08X, fcr31 after %08X\n",
        g_trap_entry, g_trap_res, g_trap_after);
    if (g_trap_stage == 3) sceKernelDeleteThread(th);
    else out("  terminate and delete %08X\n", (unsigned)sceKernelTerminateDeleteThread(th));
}

static void section_fpu_trap(void) {
    section("12. FPU traps: the E cause bit, then enabled exceptions");
    /* On firmware 6.60 this write switched the PSP off (vfpuprobe 1,
     * 2026-09-28). Version 2 called it "... so it runs last"; the cases
     * below and section 13 now come after it. */
    if (!step("fcr31 write 00020000 (the E cause bit) -- may trap") &&
        !KNOWN_CRASH("switched the PSP off on firmware 6.60 (vfpuprobe 1)"))
        out("  write 00020000 -> %08X\n", fcr31_rw(0x00020000u));

    trap_case("1/0 in a fresh thread with its own fcr31 (Z enabled) -- may trap",
              0, 0x3F800000, 0x00000000, 0xFFFFFFFFu);
    trap_case("0/0 in a fresh thread with its own fcr31 (V enabled) -- may trap",
              0, 0x00000000, 0x00000000, 0xFFFFFFFFu);
    trap_case("max*max in a fresh thread with its own fcr31 (O enabled) -- may trap",
              1, 0x7F7FFFFF, 0x7F7FFFFF, 0xFFFFFFFFu);
    trap_case("ctc1 fcr31 00008400 (Z cause with Z enabled) -- may trap",
              3, 0, 0, 0x00008400u);
}

/* ---- 13. transcendental cores ----------------------------------------------
 *
 * Each op over its reduced argument range, one result word per input, so the
 * hardware's own approximations can be fitted bit for bit. Every one of them
 * works on a reduced argument of about 2^23 values (2^24 for vsqrt and vrsq,
 * which see the exponent's parity): the quarter-turn fraction for vsin and
 * vcos, x in 23-bit fixed point for vasin, the fraction for vexp2, the
 * mantissa for vlog2, vrcp, vsqrt and vrsq. All of it would be 352 MB, so
 * the main segment of each takes every 3rd input (every 5th for vsqrt and
 * vrsq), an odd stride that still meets every residue of the low bits, and
 * smaller segments check the reductions themselves. A segment is
 *
 *   'k': x = (start + stride*i) * 2^-23   (exact: the multiple is below 2^24)
 *   'b': x has the bits start + stride*i
 *
 * for i = 0 .. count-1, and a file is its segments in order. Written last:
 * about 91 MB, and a memory stick that fills up loses only these. */

typedef struct { char kind; w32 start, stride, count; const char *what; } coreseg;
#define CORE_CHUNK 65536
#define CORE_SEGS  16

typedef struct { const char *op; void (*fn)(const w32 *, w32 *, int); coreseg seg[CORE_SEGS]; } coredump;

static const coredump CORES[] = {
    { "vsin", u_vsin, {
        { 'k', 0, 3, 2796203, "x = k*2^-23 in [0,1): the core, one quadrant" },
        { 'b', 0x3F800000, 31, 270601, "x in [1,2): the reflected quadrant" },
        { 'b', 0x3F000000, 1, 65536, "x from 0.5 at 2^-24 steps: finer than the 2^-23 grid" } } },
    { "vcos", u_vcos, {
        { 'k', 0, 31, 270601, "x = k*2^-23 in [0,1): the same core, if it is one" },
        { 'b', 0x3F000000, 1, 65536, "x from 0.5 at 2^-24 steps" } } },
    { "vasin", u_vasin, {
        { 'k', 0, 3, 2796203, "x = k*2^-23 in [0,1): the core" },
        { 'b', 0x3F000000, 1, 65536, "x from 0.5 at 2^-24 steps" } } },
    { "vexp2", u_vexp2, {
        { 'b', 0x3F800000, 3, 2796203, "x in [1,2): the fraction on the 2^-23 grid" },
        { 'b', 0x3F000000, 1, 65536, "x from 0.5 at 2^-24 steps" } } },
    { "vlog2", u_vlog2, {
        { 'b', 0x3F800000, 3, 2796203, "x in [1,2): the mantissa, 22 fraction bits out" },
        { 'b', 0x3F000000, 7, 1198373, "x in [0.5,1): the path with 15 fraction bits" } } },
    { "vrcp", u_vrcp, {
        { 'b', 0x3F800000, 3, 2796203, "x in [1,2): the mantissa" } } },
    { "vsqrt", u_vsqrt, {
        { 'b', 0x3F800000, 5, 3355444, "x in [1,4): the mantissa and the exponent's parity" } } },
    { "vrsq", u_vrsq, {
        { 'b', 0x3F800000, 5, 3355444, "x in [1,4): the mantissa and the exponent's parity" } } },
};

/* Version 4: where version 3's dumps did not settle the fit (fw660-v3.txt,
 * steps 193-197), into vfpu_core4_<op>.bin, about 7 MB.
 *
 * vsin, vcos and vasin turned out to be the shared core with an exponent per
 * segment (src/vfpu.c), but every 3rd argument leaves some of V's steps open
 * where the stride aliases with the slope: in 11 of the cosine core's 128
 * segments and 13 of vasin's. Here those segments are dumped whole. vcos of
 * x = X * 2^-23 is the cosine core at X itself (one quarter turn on, then
 * reflected), so segment s is X = s*2^16 .. s*2^16 + 65535.
 *
 * vlog2 of x >= 4 is ex + log2(m) truncated to 23 bits in about half of the
 * 2,510 swept inputs and one unit below that in the rest, by a rule the
 * sweep cannot pin. The input is (ex, m): every 255th mantissa for one
 * exponent of each bit length 2..7 (ex = 2, 4, 8, 16, 32, 64), every 2040th,
 * a subset of those, at the top of each length (3, 7, 15, 31, 63, 127), to
 * tell whether ex matters beyond its length, and 2048 consecutive mantissas
 * from the start and the middle of a segment for ex = 2 and 32. */
static const coredump CORES4[] = {
    { "vcos", u_vcos, {
        { 'k', 0x000000, 1, 65536, "cosine core segment 0" },
        { 'k', 0x0B0000, 1, 65536, "cosine core segment 11" },
        { 'k', 0x110000, 1, 65536, "cosine core segment 17" },
        { 'k', 0x220000, 1, 65536, "cosine core segment 34" },
        { 'k', 0x230000, 1, 65536, "cosine core segment 35" },
        { 'k', 0x240000, 1, 65536, "cosine core segment 36" },
        { 'k', 0x380000, 1, 65536, "cosine core segment 56" },
        { 'k', 0x510000, 1, 65536, "cosine core segment 81" },
        { 'k', 0x520000, 1, 65536, "cosine core segment 82" },
        { 'k', 0x530000, 1, 65536, "cosine core segment 83" },
        { 'k', 0x540000, 1, 65536, "cosine core segment 84" } } },
    { "vasin", u_vasin, {
        { 'k', 0x0A0000, 1, 65536, "segment 10" },
        { 'k', 0x220000, 1, 65536, "segment 34" },
        { 'k', 0x230000, 1, 65536, "segment 35" },
        { 'k', 0x240000, 1, 65536, "segment 36" },
        { 'k', 0x250000, 1, 65536, "segment 37" },
        { 'k', 0x260000, 1, 65536, "segment 38" },
        { 'k', 0x270000, 1, 65536, "segment 39" },
        { 'k', 0x280000, 1, 65536, "segment 40" },
        { 'k', 0x490000, 1, 65536, "segment 73" },
        { 'k', 0x520000, 1, 65536, "segment 82" },
        { 'k', 0x620000, 1, 65536, "segment 98" },
        { 'k', 0x700000, 1, 65536, "segment 112" },
        { 'k', 0x770000, 1, 65536, "segment 119" } } },
    { "vlog2", u_vlog2, {
        { 'b', 0x40800000, 255, 32897, "x in [4,8): every 255th mantissa" },
        { 'b', 0x41800000, 255, 32897, "x in [2^4,2^5)" },
        { 'b', 0x43800000, 255, 32897, "x in [2^8,2^9)" },
        { 'b', 0x47800000, 255, 32897, "x in [2^16,2^17)" },
        { 'b', 0x4F800000, 255, 32897, "x in [2^32,2^33)" },
        { 'b', 0x5F800000, 255, 32897, "x in [2^64,2^65)" },
        { 'b', 0x41000000, 2040, 4113, "x in [2^3,2^4): every 2040th mantissa" },
        { 'b', 0x43000000, 2040, 4113, "x in [2^7,2^8)" },
        { 'b', 0x47000000, 2040, 4113, "x in [2^15,2^16)" },
        { 'b', 0x4F000000, 2040, 4113, "x in [2^31,2^32)" },
        { 'b', 0x5F000000, 2040, 4113, "x in [2^63,2^64)" },
        { 'b', 0x7F000000, 2040, 4113, "x in [2^127,2^128)" },
        { 'b', 0x40800000, 1, 2048, "x from 4, consecutive mantissas" },
        { 'b', 0x40C07C00, 1, 2048, "x from 4 * (1.5 + 0x7C00*2^-23), segment 64's middle" },
        { 'b', 0x4F800000, 1, 2048, "x from 2^32, consecutive mantissas" },
        { 'b', 0x4FC07C00, 1, 2048, "x from 2^32 * (1.5 + 0x7C00*2^-23)" } } },
};

/* One step: the op over each segment, into probe_dir()/file. */
static void dump_core(const coredump *c, const char *file, const char *title, w32 *in, w32 *res) {
    const float grid = 1.0f / 8388608.0f;
    char path[300];
    if (step("%s.s %s -> %s", c->op, title, file)) return;
    snprintf(path, sizeof path, "%s%s", probe_dir(), file);
    SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) { out("  open"); ret(fd); return; }
    int total = 0, failed = 0;
    for (int s = 0; s < CORE_SEGS && c->seg[s].kind && !failed; s++) {
        const coreseg *g = &c->seg[s];
        w32 sum = 0, rx = 0;
        for (w32 i0 = 0; i0 < g->count && !failed; i0 += CORE_CHUNK) {
            const int n = g->count - i0 < CORE_CHUNK ? (int)(g->count - i0) : CORE_CHUNK;
            for (int j = 0; j < n; j++) {
                const w32 v = g->start + g->stride * (i0 + (w32)j);
                in[j] = g->kind == 'k' ? fb((float)(int)v * grid) : v;
            }
            c->fn(in, res, n);
            for (int j = 0; j < n; j++) { sum += res[j]; rx = ((rx << 1) | (rx >> 31)) ^ res[j]; }
            const int wr = sceIoWrite(fd, res, n * 4);
            if (wr != n * 4) { out("  write at word %d", total / 4); ret(wr); failed = 1; }
            else total += wr;
        }
        out("  %c start %08X stride %u count %u, %s: sum %08X rotxor %08X\n",
            g->kind, g->start, g->stride, g->count, g->what, sum, rx);
    }
    sceIoClose(fd);
    out("  %s: %d bytes\n", file, total);
}

static void section_cores(void) {
    section("13. transcendental cores over their reduced ranges (runs last)");
    w32 *in  = memalign(64, CORE_CHUNK * 4);
    w32 *res = memalign(64, CORE_CHUNK * 4);
    if (!in || !res) { say("out of memory\n"); return; }
    char file[40];
    for (int c = 0; c < (int)(sizeof CORES / sizeof CORES[0]); c++) {
        snprintf(file, sizeof file, "vfpu_core_%s.bin", CORES[c].op);
        dump_core(&CORES[c], file, "core", in, res);
    }
    for (int c = 0; c < (int)(sizeof CORES4 / sizeof CORES4[0]); c++) {
        snprintf(file, sizeof file, "vfpu_core4_%s.bin", CORES4[c].op);
        dump_core(&CORES4[c], file, "core where v3's dump left the fit open", in, res);
    }
    free(res);
    free(in);
}

/* ---- main ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    __asm__ volatile("cfc1 %0, $31\n" : "=r"(g_fcr31_main));
    probe_init("vfpuprobe", PROBE_VERSION, argc, argv);

    /* Before anything else touches the VFPU: the main thread's generator. */
    section("0. main thread before any VFPU use");
    log_vfpu_ctrl("main");

    section_unary();
    section_conv();
    section_vcst();
    section_arith();
    section_vcmp();
    section_matrix();
    section_vrot();
    section_prefix();
    section_fresh();
    section_fpu();
    /* The generator after everything else: a runtime that lacks it stops
     * here rather than losing the sections above. */
    section_random();
    section_fpu_trap();
    section_cores();
    probe_done();
    return 0;
}
