/* psprecomp — semantic helpers for recompiled code.
 *
 * Most MIPS instructions lower to one line of obvious C, and the emitter
 * writes those inline so the generated file stays readable. The ones that do
 * NOT — trapping arithmetic, division edge cases, the unaligned load/store
 * pairs, the bitfield ops — live here, in exactly one place. If a game
 * diverges from the oracle on one of these, there is one function to fix, not
 * one occurrence per call site.
 *
 * Everything here is a static inline in the header on purpose: the generated
 * C is enormous, and these want to vanish into the surrounding code.
 */
#ifndef PSPRECOMP_RECOMP_RT_H
#define PSPRECOMP_RECOMP_RT_H

#include "cpu.h"
#include "mem.h"

#include <math.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- arithmetic ---------------------------------------------------------- */

/* ADD/SUB/ADDI trap on signed overflow on real hardware. Games essentially
 * never rely on the trap — compilers emit addu/subu for C arithmetic — but the
 * wraparound result must still be exact, so we compute in unsigned. */
static inline uint32_t psp_add(uint32_t a, uint32_t b) { return a + b; }
static inline uint32_t psp_sub(uint32_t a, uint32_t b) { return a - b; }

static inline uint32_t psp_slt (uint32_t a, uint32_t b) { return (int32_t)a < (int32_t)b; }
static inline uint32_t psp_sltu(uint32_t a, uint32_t b) { return a < b; }
static inline uint32_t psp_max (uint32_t a, uint32_t b) { return (int32_t)a > (int32_t)b ? a : b; }
static inline uint32_t psp_min (uint32_t a, uint32_t b) { return (int32_t)a < (int32_t)b ? a : b; }

/* Shifts: C leaves shift-by->=32 undefined, MIPS masks the amount to 5 bits. */
static inline uint32_t psp_sll(uint32_t v, uint32_t s) { return v << (s & 31); }
static inline uint32_t psp_srl(uint32_t v, uint32_t s) { return v >> (s & 31); }
static inline uint32_t psp_sra(uint32_t v, uint32_t s) { return (uint32_t)((int32_t)v >> (s & 31)); }
static inline uint32_t psp_rotr(uint32_t v, uint32_t s) {
    s &= 31;
    return s ? ((v >> s) | (v << (32 - s))) : v;
}

/* ---- multiply / divide --------------------------------------------------- */

static inline void psp_mult(uint32_t a, uint32_t b) {
    int64_t r = (int64_t)(int32_t)a * (int64_t)(int32_t)b;
    psp_cpu.lo = (uint32_t)r;
    psp_cpu.hi = (uint32_t)((uint64_t)r >> 32);
}

static inline void psp_multu(uint32_t a, uint32_t b) {
    uint64_t r = (uint64_t)a * (uint64_t)b;
    psp_cpu.lo = (uint32_t)r;
    psp_cpu.hi = (uint32_t)(r >> 32);
}

/* Division by zero does not trap on MIPS — HI/LO are simply unpredictable.
 * We define them (quotient -1/0, remainder = dividend) so a recompiled game
 * that hits this is deterministic and diffable against the oracle instead of
 * depending on host UB. INT_MIN / -1 also overflows in C, so it is special-cased. */
static inline void psp_div(uint32_t a, uint32_t b) {
    int32_t sa = (int32_t)a, sb = (int32_t)b;
    if (sb == 0) {
        psp_cpu.lo = sa < 0 ? 1u : 0xFFFFFFFFu;
        psp_cpu.hi = a;
    } else if (sa == (int32_t)0x80000000 && sb == -1) {
        psp_cpu.lo = 0x80000000u;
        psp_cpu.hi = 0;
    } else {
        psp_cpu.lo = (uint32_t)(sa / sb);
        psp_cpu.hi = (uint32_t)(sa % sb);
    }
}

static inline void psp_divu(uint32_t a, uint32_t b) {
    if (b == 0) {
        psp_cpu.lo = 0xFFFFFFFFu;
        psp_cpu.hi = a;
    } else {
        psp_cpu.lo = a / b;
        psp_cpu.hi = a % b;
    }
}

static inline void psp_madd(uint32_t a, uint32_t b) {
    int64_t acc = (int64_t)(((uint64_t)psp_cpu.hi << 32) | psp_cpu.lo);
    acc += (int64_t)(int32_t)a * (int64_t)(int32_t)b;
    psp_cpu.lo = (uint32_t)acc;
    psp_cpu.hi = (uint32_t)((uint64_t)acc >> 32);
}

static inline void psp_maddu(uint32_t a, uint32_t b) {
    uint64_t acc = ((uint64_t)psp_cpu.hi << 32) | psp_cpu.lo;
    acc += (uint64_t)a * (uint64_t)b;
    psp_cpu.lo = (uint32_t)acc;
    psp_cpu.hi = (uint32_t)(acc >> 32);
}

static inline void psp_msub(uint32_t a, uint32_t b) {
    int64_t acc = (int64_t)(((uint64_t)psp_cpu.hi << 32) | psp_cpu.lo);
    acc -= (int64_t)(int32_t)a * (int64_t)(int32_t)b;
    psp_cpu.lo = (uint32_t)acc;
    psp_cpu.hi = (uint32_t)((uint64_t)acc >> 32);
}

static inline void psp_msubu(uint32_t a, uint32_t b) {
    uint64_t acc = ((uint64_t)psp_cpu.hi << 32) | psp_cpu.lo;
    acc -= (uint64_t)a * (uint64_t)b;
    psp_cpu.lo = (uint32_t)acc;
    psp_cpu.hi = (uint32_t)(acc >> 32);
}

/* ---- bit manipulation (MIPS32r2 + Allegrex) ------------------------------ */

static inline uint32_t psp_clz(uint32_t v) {
    if (v == 0) return 32;
    uint32_t n = 0;
    while (!(v & 0x80000000u)) { v <<= 1; n++; }
    return n;
}

static inline uint32_t psp_clo(uint32_t v) { return psp_clz(~v); }

/* ext rt, rs, pos, size — extract `size` bits starting at `pos`. */
static inline uint32_t psp_ext(uint32_t v, uint32_t pos, uint32_t size) {
    if (size == 0 || size > 32) return 0;
    if (size == 32) return v >> pos;
    return (v >> pos) & ((1u << size) - 1);
}

/* ins rt, rs, pos, size — splice `size` bits of `src` into `dst` at `pos`. */
static inline uint32_t psp_ins(uint32_t dst, uint32_t src, uint32_t pos, uint32_t size) {
    if (size == 0 || size > 32) return dst;
    uint32_t mask = (size == 32) ? 0xFFFFFFFFu : (((1u << size) - 1) << pos);
    return (dst & ~mask) | ((src << pos) & mask);
}

static inline uint32_t psp_seb(uint32_t v) { return (uint32_t)(int32_t)(int8_t)(v & 0xFF); }
static inline uint32_t psp_seh(uint32_t v) { return (uint32_t)(int32_t)(int16_t)(v & 0xFFFF); }

/* wsbh: swap bytes within each halfword. wsbw: reverse all four bytes. */
static inline uint32_t psp_wsbh(uint32_t v) {
    return ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
}
static inline uint32_t psp_wsbw(uint32_t v) {
    return (v << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8) | (v >> 24);
}

/* bitrev: reverse the bit order of the whole word (Allegrex extension). */
static inline uint32_t psp_bitrev(uint32_t v) {
    v = ((v & 0x55555555u) << 1)  | ((v >> 1)  & 0x55555555u);
    v = ((v & 0x33333333u) << 2)  | ((v >> 2)  & 0x33333333u);
    v = ((v & 0x0F0F0F0Fu) << 4)  | ((v >> 4)  & 0x0F0F0F0Fu);
    v = ((v & 0x00FF00FFu) << 8)  | ((v >> 8)  & 0x00FF00FFu);
    return (v << 16) | (v >> 16);
}

/* ---- unaligned load / store ---------------------------------------------- */
/* lwl/lwr and swl/swr are how MIPS compilers implement unaligned 32-bit
 * access: a pair of instructions that each move the part of the word that
 * lies in one aligned container. The PSP is little-endian, which is the case
 * people get wrong — these follow the LE definitions. */

/* The four byte-offset cases are a lookup, not arithmetic — this is the form
 * the MIPS manual defines them in, and it avoids the shift-by-32 UB that the
 * arithmetic form walks into at offset 0. */
static const uint32_t PSP_LWL_MASK [4] = { 0x00FFFFFFu, 0x0000FFFFu, 0x000000FFu, 0x00000000u };
static const uint32_t PSP_LWL_SHIFT[4] = { 24, 16, 8, 0 };
static const uint32_t PSP_LWR_MASK [4] = { 0x00000000u, 0xFF000000u, 0xFFFF0000u, 0xFFFFFF00u };
static const uint32_t PSP_LWR_SHIFT[4] = { 0, 8, 16, 24 };
static const uint32_t PSP_SWL_MASK [4] = { 0xFFFFFF00u, 0xFFFF0000u, 0xFF000000u, 0x00000000u };
static const uint32_t PSP_SWL_SHIFT[4] = { 24, 16, 8, 0 };
static const uint32_t PSP_SWR_MASK [4] = { 0x00000000u, 0x000000FFu, 0x0000FFFFu, 0x00FFFFFFu };
static const uint32_t PSP_SWR_SHIFT[4] = { 0, 8, 16, 24 };

static inline uint32_t psp_lwl(uint32_t old, uint32_t addr) {
    uint32_t b = addr & 3;
    uint32_t w = psp_read32(addr & ~3u);
    return (old & PSP_LWL_MASK[b]) | (w << PSP_LWL_SHIFT[b]);
}

static inline uint32_t psp_lwr(uint32_t old, uint32_t addr) {
    uint32_t b = addr & 3;
    uint32_t w = psp_read32(addr & ~3u);
    return (old & PSP_LWR_MASK[b]) | (w >> PSP_LWR_SHIFT[b]);
}

static inline void psp_swl(uint32_t val, uint32_t addr) {
    uint32_t b = addr & 3, base = addr & ~3u;
    psp_write32(base, (psp_read32(base) & PSP_SWL_MASK[b]) | (val >> PSP_SWL_SHIFT[b]));
}

static inline void psp_swr(uint32_t val, uint32_t addr) {
    uint32_t b = addr & 3, base = addr & ~3u;
    psp_write32(base, (psp_read32(base) & PSP_SWR_MASK[b]) | (val << PSP_SWR_SHIFT[b]));
}

/* ---- COP1, single precision --------------------------------------------- */

/* `mtc1`/`mfc1` move raw bits between the integer and FP register files — they
 * are not conversions. Going through a union keeps that explicit and avoids
 * the strict-aliasing violation a pointer cast would introduce. */
static inline float psp_bits_to_f32(uint32_t b) {
    union { uint32_t u; float f; } c;
    c.u = b;
    return c.f;
}

static inline uint32_t psp_f32_to_bits(float v) {
    union { uint32_t u; float f; } c;
    c.f = v;
    return c.u;
}

/* Half precision, both directions.
 *
 * Three things need this and they must agree: the decoder, expanding vfim's
 * immediate at translation time; and vh2f/vf2h, converting at run time. Two
 * copies of a float conversion is exactly the drift the differential oracle
 * cannot see -- it runs the same helper on both sides -- so there is one.
 *
 * What the hardware does (vfpuprobe steps 27-30, fw 6.60):
 *
 *   - half subnormals do not exist either way. vh2f of 03FF is 00000000, and
 *     vf2h of anything that lands below 2^-14 is a signed zero: 2^-24, 2^-25,
 *     1.5 * 2^-24 and 387FC000 (the largest half subnormal) all give 0000;
 *   - a NaN or infinity becomes sign | 7C00 | the float's *low* ten mantissa
 *     bits: 7FC00000 -> 7C00 (a quiet NaN turns into +inf), 7F800001 -> 7C01,
 *     FF800001 -> FC01. It used to give 7FFF;
 *   - vh2f of an all-ones exponent ORs the half's mantissa in at the bottom
 *     rather than shifting it up (7C01 -> 7F800001);
 *   - 65520, the exact midpoint between 65504 and 65536, gives 7BFF, not
 *     infinity, and 1e10 gives 7C00.
 *
 * The mantissa is truncated, toward zero: 3F801000 and 3F803000 (ties) give
 * 3C00 and 3C01, 3F801FF8 (just below a tie) 3C00, 3FFFF000 3FFF, 477FF001
 * to 477FFFFF 7BFF and only 2^16 itself 7C00; everything below 2^-14 is
 * a zero, 387FFFFF included, and 38801000 is 0400 (vfpuprobe v3 step 50,
 * fw 6.60; the same with the sign set). */
static inline float psp_half_to_f32(uint16_t h) {
    const uint32_t sign = (uint32_t)(h >> 15) << 31;
    const uint32_t exp  = (h >> 10) & 0x1F;
    const uint32_t man  = h & 0x3FF;
    uint32_t bits;
    if (exp == 0)       bits = sign;
    else if (exp == 31) bits = sign | 0x7F800000u | man;
    else                bits = sign | ((exp + 127 - 15) << 23) | (man << 13);
    union { uint32_t u; float f; } c;
    c.u = bits;
    return c.f;
}

static inline uint16_t psp_f32_to_half(float v) {
    union { uint32_t u; float f; } c;
    c.f = v;
    const uint32_t sign = (c.u >> 16) & 0x8000u;
    const uint32_t a    = c.u & 0x7FFFFFFFu;
    if (a >= 0x7F800000u) return (uint16_t)(sign | 0x7C00u | (a & 0x3FFu));

    const int32_t e = (int32_t)(a >> 23) - 127 + 15;
    if (e >= 31) return (uint16_t)(sign | 0x7C00u);            /* 2^16 and up */
    if (e <= 0) return (uint16_t)sign;                         /* below 2^-14 */
    return (uint16_t)(sign | ((uint32_t)e << 10) | ((a >> 13) & 0x3FFu));
}

static inline float psp_fabs(float v)  { return v < 0.0f ? -v : v; }

/* floor/ceil without libm: generated C is compiled and linked on its own and
 * this header is the whole of its runtime. Values at or beyond 2^23 have no
 * fractional part in binary32, so the cast round-trip is exact below that and
 * unnecessary above it. */
static inline float psp_floorf(float v) {
    if (!(psp_fabs(v) < 8388608.0f)) return v;         /* >= 2^23, or NaN */
    const float t = (float)(int32_t)v;                 /* truncates toward 0 */
    return (t > v) ? t - 1.0f : t;
}
static inline float psp_ceilf(float v) {
    if (!(psp_fabs(v) < 8388608.0f)) return v;
    const float t = (float)(int32_t)v;
    return (t < v) ? t + 1.0f : t;
}
/* Is this integral value odd? Used only for round-half-to-even. */
static inline float psp_fmodf2(float v) {
    if (!(psp_fabs(v) < 8388608.0f)) return 0.0f;      /* all even up there */
    return (float)(((int32_t)v) & 1);
}

/* Use the host's binary32 square root rather than an unscaled Newton iteration.
 * Starting Newton at v and stopping after a fixed number of steps does not
 * converge across binary32's exponent range: for example, the square of a
 * 1.34e11-length vector was still nearly 8000 times too large after 24 steps.
 * The instruction-specific wrappers below classify their own edge cases. */
static inline float psp_fsqrt(float v) {
    return v <= 0.0f ? 0.0f : sqrtf(v);
}

/* ---- the COP1 FPU -----------------------------------------------------------
 *
 * IEEE 754 single precision, with the PSP's departures. Each of these is what
 * a PSP on firmware 6.60 did in vfpuprobe steps 150-153:
 *
 *   - an invalid operation -- sqrt(-1), 0/0, inf-inf -- gives 7FC00000, where
 *     the host gives FFC00000 and the old sqrt gave 7FBFFFFF;
 *   - a NaN operand passes through quieted (7F800001 + 1 -> 7FC00001) and
 *     raises V only if it was signalling: NaN * NaN of two quiet NaNs raises
 *     nothing (fcr 00000000), which a comment here used to deny;
 *   - add and sub overflow raises O without I (max+max -> 7F800000, fcr
 *     00004010; 7F7FFFFF and 00004011 under RZ); mul raises O|I (00005014);
 *   - mul and div raise U for every tiny result, exact or not: den * 1 ->
 *     00400000 with 00002008, tiny * tiny -> 000116C2 with 0000300C. add and
 *     sub never do -- a tiny sum is always exact -- and den + 0 raises nothing;
 *   - FS flushes exactly those underflowing mul/div results, to a signed zero,
 *     raising U and no I (tiny * tiny -> 0, fcr 01002008). It flushes no
 *     operand and no sum: den + 0 is 00000001 and den + min 00C00000;
 *   - sqrt honours RM and raises nothing but V (sqrt 2 is 3FB504F3, and
 *     3FB504F4 under RP, with no I);
 *   - cvt.s.w honours RM and raises I when inexact (7FFFFFFF -> 4F000000, fcr
 *     00001004); the float-to-integer conversions raise I when inexact and
 *     give 7FFFFFFF or 80000000 with V for NaN, infinities and out of range;
 *   - neg.s and abs.s quiet a NaN, keep its sign and raise V for a signalling
 *     one (neg 7F800001 -> 7FC00001, neg 7FC00000 -> 7FC00000, abs FFC00000 ->
 *     FFC00000); mov.s copies bits and raises nothing;
 *   - of the sixteen compare predicates only olt, ole, lt and le raise V, and
 *     they do for any NaN; the rest raise nothing even for a signalling NaN.
 *
 * Generalised rather than measured: div overflow raises O|I like mul, inf/0
 * raises nothing, a qNaN meeting an sNaN returns the first operand quieted,
 * and neg/abs/compare rewrite the Cause field like the arithmetic ops.
 *
 * Directed rounding is emulated rather than delegated to fesetround(), which
 * needs `#pragma STDC FENV_ACCESS ON` -- GCC does not implement it, so -O2 is
 * free to move arithmetic across the mode change. Instead: compute in double,
 * which is exact for float add, sub and mul, then round once to float in the
 * requested direction. For division the double quotient is not exact, but
 * binary64 carries 53 bits against the 2p+2 = 50 needed to decide a binary32
 * quotient, so rounding it to float still lands where a correctly-rounded
 * float division would. */
enum { PSP_RM_RN = 0, PSP_RM_RZ = 1, PSP_RM_RP = 2, PSP_RM_RM = 3 };
#define PSP_FCR31_FS      (1u << 24)
#define PSP_FPU_DEFAULT(f) (((f) & (PSP_FCR31_FS | 3u)) == 0u)

/* The five IEEE exceptions, in the order FCR31 packs them: Cause at 12..16,
 * rewritten by every operation, and Flags at 2..6, sticky until software
 * clears them (1+1 with flags 7C set leaves 0000007C). */
#define PSP_FE_I  1u
#define PSP_FE_U  2u
#define PSP_FE_O  4u
#define PSP_FE_Z  8u
#define PSP_FE_V  16u
#define PSP_FCR31_CAUSE 0x0001F000u

static inline void psp_fpu_raise(uint32_t c) {
    psp_cpu.fcr31 = (psp_cpu.fcr31 & ~PSP_FCR31_CAUSE) | (c << 12) | (c << 2);
}

/* The NaN an invalid operation makes, and the two kinds of NaN operand: bit
 * 22 set is quiet, clear is signalling (7F800001 is quieted to 7FC00001). */
#define PSP_FPU_QNAN 0x7FC00000u
static inline int psp_fnan_bits(uint32_t b)  { return (b & 0x7FFFFFFFu) > 0x7F800000u; }
static inline int psp_fsnan_bits(uint32_t b) { return psp_fnan_bits(b) && !(b & 0x00400000u); }

/* One ULP along the real line. ±0 steps to the smallest denormal of the
 * target sign rather than across it, which is why zero is special-cased. */
static inline float psp_nextup(float v) {
    const uint32_t b = psp_f32_to_bits(v);
    if ((b & 0x7FFFFFFFu) == 0u) return psp_bits_to_f32(0x00000001u);
    return psp_bits_to_f32((b & 0x80000000u) ? b - 1u : b + 1u);
}
static inline float psp_nextdown(float v) {
    const uint32_t b = psp_f32_to_bits(v);
    if ((b & 0x7FFFFFFFu) == 0u) return psp_bits_to_f32(0x80000001u);
    return psp_bits_to_f32((b & 0x80000000u) ? b + 1u : b - 1u);
}

/* Round an exact double to float under an explicit mode.
 *
 * Overflow falls out of this rather than needing a case: an exact value past
 * FLT_MAX rounds to +inf under RN, and stepping one ULP down from +inf is
 * FLT_MAX -- which is exactly what RZ and RM are supposed to give. */
static inline float psp_round_mode(double exact, int rm) {
    const float n = (float)exact;                  /* nearest, ties to even */
    if ((rm & 3) == PSP_RM_RN) return n;
    const double dn = (double)n;
    if (dn == exact) return n;                     /* representable; no direction to pick */
    switch (rm & 3) {
    case PSP_RM_RP: return (dn < exact) ? psp_nextup(n)   : n;
    case PSP_RM_RM: return (dn > exact) ? psp_nextdown(n) : n;
    default:                                       /* RZ */
        if (exact > 0.0 && dn > exact) return psp_nextdown(n);
        if (exact < 0.0 && dn < exact) return psp_nextup(n);
        return n;
    }
}

/* COP1 float-to-integer, with the rounding mode named explicitly.
 *
 * MIPS has five of these and they differ only in how they round:
 *
 *   round.w.s  RN  to nearest, ties to even
 *   trunc.w.s  RZ  toward zero
 *   ceil.w.s   RP  toward +inf
 *   floor.w.s  RM  toward -inf
 *   cvt.w.s        whatever FCR31's RM field currently says
 *
 * and the four named ones ignore FCR31 (step 151: round.w 2.5 is 2 under
 * every mode). NaN, +inf and anything at or past 2^31 give 7FFFFFFF with V;
 * -inf gives 80000000 with V, and so, by the same rule, does anything below
 * -2^31. Otherwise I says whether the value had a fraction (a denormal is 0
 * with I). The range test is in float against 2^31 exactly, so it does not
 * depend on the conversion it is guarding -- a C cast of an out-of-range
 * float is undefined, and on x86 yields 80000000 for large positives too. */
static inline uint32_t psp_f32_to_i32(float v, int rm) {
    if (v != v) { psp_fpu_raise(PSP_FE_V); return 0x7FFFFFFFu; }
    float r;
    switch (rm & 3) {
    case PSP_RM_RZ: r = (v < 0.0f) ? -psp_floorf(-v) : psp_floorf(v); break;
    case PSP_RM_RP: r = psp_ceilf(v);                                 break;
    case PSP_RM_RM: r = psp_floorf(v);                                break;
    default: {                                         /* RN, ties to even */
        const float f = psp_floorf(v), d = v - f;
        if (d > 0.5f)                          r = f + 1.0f;
        else if (d < 0.5f)                     r = f;
        else                                   r = (psp_fmodf2(f) != 0.0f) ? f + 1.0f : f;
        break; }
    }
    if (r >=  2147483648.0f) { psp_fpu_raise(PSP_FE_V); return 0x7FFFFFFFu; }
    if (r <  -2147483648.0f) { psp_fpu_raise(PSP_FE_V); return 0x80000000u; }
    psp_fpu_raise(r != v ? PSP_FE_I : 0u);
    return (uint32_t)(int32_t)r;
}

/* cvt.s.w: every int32 is exact in a double, so this is one directed
 * rounding (2^24+1 is 4B800001 under RP, CB800001 negated under RM). */
static inline float psp_cvt_s_w(uint32_t w) {
    const double exact = (double)(int32_t)w;
    const float r = psp_round_mode(exact, (int)(psp_cpu.fcr31 & 3u));
    psp_fpu_raise((double)r != exact ? PSP_FE_I : 0u);
    return r;
}

/* A NaN operand to a two-operand op: the result is that NaN quieted, the
 * first operand's if both are NaN, and V only if one was signalling. Returns
 * 0, touching nothing, when neither operand is a NaN. */
static inline int psp_fpu_nan_operand(float a, float b, float *out) {
    const uint32_t ab = psp_f32_to_bits(a), bb = psp_f32_to_bits(b);
    if (!psp_fnan_bits(ab) && !psp_fnan_bits(bb)) return 0;
    psp_fpu_raise((psp_fsnan_bits(ab) || psp_fsnan_bits(bb)) ? PSP_FE_V : 0u);
    *out = psp_bits_to_f32((psp_fnan_bits(ab) ? ab : bb) | 0x00400000u);
    return 1;
}

/* Round an operation's exact answer under RM and record what it raised.
 * `muldiv` selects the mul/div rules (1 mul.s, 2 div.s): a tiny result
 * raises U even when exact, and FS flushes it. An overflow raises O, and
 * mul.s adds I to it where add.s, sub.s and div.s do not: max*max gives
 * cause 14 but max+max, max/0.5, max/denormal and 1/2^-128 cause 10, in
 * every rounding mode and under FS (vfpuprobe v3 steps 165, 166 and 169,
 * fw 6.60). */
static inline float psp_fpu_result(double exact, int muldiv) {
    const uint32_t fcr = psp_cpu.fcr31;
    if (exact != exact) {                                   /* inf-inf, 0*inf, 0/0 */
        psp_fpu_raise(PSP_FE_V);
        return psp_bits_to_f32(PSP_FPU_QNAN);
    }
    const float r = psp_round_mode(exact, (int)(fcr & 3u));
    if (exact - exact != 0.0) { psp_fpu_raise(0u); return r; }   /* an infinite operand */

    const uint32_t rb  = psp_f32_to_bits(r);
    const double   mag = exact < 0.0 ? -exact : exact;
    if ((rb & 0x7F800000u) == 0x7F800000u || mag >= 0x1p128) {  /* overflow, any mode */
        psp_fpu_raise(muldiv == 1 ? (PSP_FE_O | PSP_FE_I) : PSP_FE_O);
        return r;
    }
    const uint32_t inexact = (double)r != exact ? PSP_FE_I : 0u;
    if (muldiv && (rb & 0x7F800000u) == 0u && exact != 0.0) {   /* tiny */
        if (fcr & PSP_FCR31_FS) {
            psp_fpu_raise(PSP_FE_U);
            return psp_bits_to_f32(rb & 0x80000000u);
        }
        psp_fpu_raise(PSP_FE_U | inexact);
        return r;
    }
    psp_fpu_raise(inexact);
    return r;
}

static inline float psp_fadd(float a, float b) {
    float n;
    if (psp_fpu_nan_operand(a, b, &n)) return n;
    return psp_fpu_result((double)a + (double)b, 0);
}
static inline float psp_fsub(float a, float b) {
    float n;
    if (psp_fpu_nan_operand(a, b, &n)) return n;
    return psp_fpu_result((double)a - (double)b, 0);
}
static inline float psp_fmul(float a, float b) {
    float n;
    if (psp_fpu_nan_operand(a, b, &n)) return n;
    return psp_fpu_result((double)a * (double)b, 1);
}
static inline float psp_fdiv(float a, float b) {
    float n;
    if (psp_fpu_nan_operand(a, b, &n)) return n;
    const uint32_t ab = psp_f32_to_bits(a), bb = psp_f32_to_bits(b);
    if ((bb & 0x7FFFFFFFu) == 0u && (ab & 0x7FFFFFFFu) != 0u &&
        (ab & 0x7F800000u) != 0x7F800000u) {                /* finite / 0: Z */
        psp_fpu_raise(PSP_FE_Z);
        return psp_bits_to_f32(((ab ^ bb) & 0x80000000u) | 0x7F800000u);
    }
    return psp_fpu_result((double)a / (double)b, 2);
}

/* sqrt.s. Separate from psp_fsqrt, whose non-positive-input rule is useful to
 * general geometry code but does not implement the instruction's edge cases.
 *
 * The host's sqrtf is correctly rounded to nearest; the directed modes step
 * from it. r*r is a 24x24-bit product, exact in a double, so comparing it with
 * the operand says which side of the true root r lies on. -0 is -0, +inf is
 * +inf, and any other negative is 7FC00000 with V. */
static inline float psp_fsqrt_cop1(float v) {
    const uint32_t b = psp_f32_to_bits(v);
    if (psp_fnan_bits(b)) {
        psp_fpu_raise(psp_fsnan_bits(b) ? PSP_FE_V : 0u);
        return psp_bits_to_f32(b | 0x00400000u);
    }
    if ((b & 0x7FFFFFFFu) == 0u || b == 0x7F800000u) { psp_fpu_raise(0u); return v; }
    if (b & 0x80000000u) { psp_fpu_raise(PSP_FE_V); return psp_bits_to_f32(PSP_FPU_QNAN); }
    float r = sqrtf(v);
    const double sq = (double)r * (double)r, x = (double)v;
    switch (psp_cpu.fcr31 & 3u) {
    case PSP_RM_RP:              if (sq < x) r = psp_nextup(r);   break;
    case PSP_RM_RZ: case PSP_RM_RM: if (sq > x) r = psp_nextdown(r); break;
    default: break;
    }
    psp_fpu_raise(0u);
    return r;
}

/* neg.s and abs.s: sign-bit operations on numbers, arithmetic on NaNs. */
static inline float psp_fneg_cop1(float v) {
    const uint32_t b = psp_f32_to_bits(v);
    if (psp_fnan_bits(b)) {
        psp_fpu_raise(psp_fsnan_bits(b) ? PSP_FE_V : 0u);
        return psp_bits_to_f32(b | 0x00400000u);
    }
    psp_fpu_raise(0u);
    return psp_bits_to_f32(b ^ 0x80000000u);
}
static inline float psp_fabs_cop1(float v) {
    const uint32_t b = psp_f32_to_bits(v);
    if (psp_fnan_bits(b)) {
        psp_fpu_raise(psp_fsnan_bits(b) ? PSP_FE_V : 0u);
        return psp_bits_to_f32(b | 0x00400000u);
    }
    psp_fpu_raise(0u);
    return psp_bits_to_f32(b & 0x7FFFFFFFu);
}

/* `c.<cond>.s` condition codes. The distinction that matters is *ordered* vs
 * *unordered*: with a NaN operand the ordered forms are false and the
 * unordered forms are true. Comparisons involving NaN are false in C, so the
 * NaN case is tested explicitly rather than assumed. Every FCC result was
 * confirmed on hardware (step 153); only the V rule above is PSP-specific --
 * MIPS would have codes 8..15 signal and 0..7 signal only for an sNaN. */
static inline int psp_fcmp(unsigned cond, float a, float b) {
    const int unordered = (a != a) || (b != b);   /* either is NaN */
    const unsigned c = cond & 0xF;
    psp_fpu_raise(unordered && (c == 0x4 || c == 0x6 || c == 0xC || c == 0xE) ? PSP_FE_V : 0u);
    switch (c) {
    case 0x0: case 0x8: return 0;                              /* F, SF */
    case 0x1: case 0x9: return unordered;                      /* UN, NGLE */
    case 0x2: case 0xA: return !unordered && a == b;           /* EQ, SEQ */
    case 0x3: case 0xB: return unordered || a == b;            /* UEQ, NGL */
    case 0x4: case 0xC: return !unordered && a <  b;           /* OLT, LT */
    case 0x5: case 0xD: return unordered || a <  b;            /* ULT, NGE */
    case 0x6: case 0xE: return !unordered && a <= b;           /* OLE, LE */
    default:            return unordered || a <= b;            /* ULE, NGT */
    }
}

#ifdef __cplusplus
}
#endif

#endif /* PSPRECOMP_RECOMP_RT_H */
