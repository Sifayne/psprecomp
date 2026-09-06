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
 * The narrowing is the round-to-nearest-even algorithm the PSP's own toolchain
 * uses (float_to_half_fast3), including its clamp of anything too large to
 * infinity rather than to the largest finite half. */
static inline float psp_half_to_f32(uint16_t h) {
    const uint32_t sign = (uint32_t)(h >> 15) << 31;
    const uint32_t exp  = (h >> 10) & 0x1F;
    const uint32_t man  = h & 0x3FF;
    uint32_t bits;
    if (exp == 0)       bits = sign | (man ? ((127 - 15 + 1) << 23) | (man << 13) : 0);
    /* An exponent of all ones does *not* shift the mantissa up. Hardware ORs
     * the half's mantissa in at the bottom: vh2f of the half 0x7F80 gives
     * 0x7F800380, not the 0x7FF00000 a shift would produce. Pinned by
     * pspautotests cpu/vfpu/convert; the normal and subnormal cases below do
     * shift, as usual. */
    else if (exp == 31) bits = sign | 0x7F800000u | man;
    else                bits = sign | ((exp + 127 - 15) << 23) | (man << 13);
    union { uint32_t u; float f; } c;
    c.u = bits;
    return c.f;
}

static inline uint16_t psp_f32_to_half(float v) {
    union { uint32_t u; float f; } c;
    c.f = v;
    const uint32_t sign = c.u & 0x80000000u;
    c.u ^= sign;

    uint32_t out;
    if (c.u >= 0x7F800000u) {
        /* NaN saturates the mantissa rather than becoming a quiet NaN: the
         * hardware answer is 0x7FFF, where the software algorithm the PSP
         * toolchain uses gives 0x7E00. Only the positive case is pinned by
         * cpu/vfpu/convert; the sign is carried through on the assumption it
         * behaves like every other path here. */
        out = (c.u > 0x7F800000u) ? 0x7FFFu : 0x7C00u;
    } else {
        union { uint32_t u; float f; } magic;
        magic.u = 15u << 23;                  /* 2^-112 */
        c.u &= ~0xFFFu;
        c.f *= magic.f;
        c.u -= ~0xFFFu;
        if (c.u > (31u << 23)) c.u = 31u << 23;   /* clamp to infinity */
        out = c.u >> 13;
    }
    return (uint16_t)(out | (sign >> 16));
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
 * Three of the five were decoded and implemented nowhere, and `cvt.w.s` was
 * aliased to truncation -- which is right only when the rounding mode happens
 * to be RZ, and the PSP comes up in RN.
 *
 * The saturation is not incidental. A C cast of an out-of-range float to int
 * is undefined behaviour, and on x86 it yields 0x80000000 for *everything*
 * out of range including large positives, where MIPS answers 0x7FFFFFFF. The
 * range test is against 2^31 exactly, done in float, so it does not depend on
 * the conversion it is guarding. */
enum { PSP_RM_RN = 0, PSP_RM_RZ = 1, PSP_RM_RP = 2, PSP_RM_RM = 3 };

static inline uint32_t psp_f32_to_i32(float v, int rm) {
    if (v != v) return 0x7FFFFFFFu;                    /* NaN */
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
    if (r >=  2147483648.0f) return 0x7FFFFFFFu;
    if (r <  -2147483648.0f) return 0x80000000u;
    return (uint32_t)(int32_t)r;
}

/* ---- FCR31's effect on arithmetic ---------------------------------------
 *
 * Two fields of the control/status register change what add/sub/mul/div
 * *produce*, not merely what they record: the rounding mode (RM, bits 0..1)
 * and flush-to-zero (FS, bit 24). cpu/fpu/fpu measures both -- one multiply
 * gives four different answers under the four rounding modes, and a denormal
 * result becomes zero when FS is set.
 *
 * The host FPU always rounds to nearest-even, so a directed mode is emulated
 * rather than delegated. Not with fesetround(): honouring it requires
 * `#pragma STDC FENV_ACCESS ON`, which GCC does not actually implement, so an
 * optimiser is free to move arithmetic across the mode change -- and the
 * generated C is compiled at -O2. Getting a wrong answer from a compiler
 * reordering is worse than the arithmetic being slightly slower.
 *
 * Instead: compute in double, which is wide enough to be *exact* for float
 * add, sub and mul, then round once to float in the requested direction. For
 * division the double quotient is not exact, but binary64 carries 53 bits
 * against the 2p+2 = 50 needed to decide a binary32 quotient, so rounding it
 * to float still lands on the same value a correctly-rounded float division
 * would -- there is no double-rounding error for any of the four.
 *
 * The default state is RN with FS clear, which is what the PSP boots into and
 * what every game stays in, so that path stays a plain float operation and
 * pays nothing. */
#define PSP_FCR31_FS      (1u << 24)
#define PSP_FPU_DEFAULT(f) (((f) & (PSP_FCR31_FS | 3u)) == 0u)

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
 * FLT_MAX -- which is exactly what RZ and RM are supposed to give. NaN
 * survives because every comparison against it is false. */
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

static inline float psp_fpu_arith(double exact, uint32_t fcr31) {
    float r = psp_round_mode(exact, (int)(fcr31 & 3u));
    if (fcr31 & PSP_FCR31_FS) {                    /* denormal -> zero, sign kept */
        const uint32_t b = psp_f32_to_bits(r);
        if ((b & 0x7F800000u) == 0u) r = psp_bits_to_f32(b & 0x80000000u);
    }
    return r;
}

/* The five IEEE exceptions, in the order FCR31 packs them. The same five bits
 * appear twice: Cause at 12..16, rewritten by every operation, and Flags at
 * 2..6, sticky until software clears them. That the two fields share an order
 * is what makes the update one shift each.
 *
 * The encoding was not assumed -- it is what cpu/fpu/fcr measures. Each of its
 * four situations pins it exactly:
 *
 *   sqrt(-1), 0/0, NaN*NaN  -> 0x00010040 = V   at cause 16, flag 6
 *   FLT_MAX * FLT_MAX       -> 0x00005014 = O|I at cause 14,12 flag 4,2
 *   1.0 / FLT_MAX           -> 0x0000300C = U|I
 *   1.0 / 3.0               -> 0x00001004 = I
 */
#define PSP_FE_I  1u
#define PSP_FE_U  2u
#define PSP_FE_O  4u
#define PSP_FE_Z  8u
#define PSP_FE_V  16u
#define PSP_FCR31_CAUSE 0x0001F000u

/* Which exceptions this result raised. `exact` is the infinitely-precise
 * answer as a double, which for all four operations is either exact or close
 * enough to decide every one of these. */
static inline uint32_t psp_fpu_except(double exact, float r, int div_by_zero) {
    if (r != r) return PSP_FE_V;                    /* any NaN result is invalid */
    const uint32_t rb = psp_f32_to_bits(r) & 0x7F800000u;
    if (rb == 0x7F800000u) {                        /* infinite result */
        if (div_by_zero) return PSP_FE_Z;
        /* Infinite because the operands were, or because we overflowed? Only
         * the second is an exception, and `exact` distinguishes them: a double
         * holds FLT_MAX*FLT_MAX finitely. */
        return (exact == exact && exact - exact != exact - exact)
             ? 0u : (PSP_FE_O | PSP_FE_I);
    }
    uint32_t c = ((double)r != exact) ? PSP_FE_I : 0u;
    /* Tiny *and* inexact is underflow. A denormal that is exactly
     * representable has lost nothing and raises neither. */
    if (rb == 0u && (c & PSP_FE_I) && exact != 0.0) c |= PSP_FE_U;
    return c;
}

static inline void psp_fpu_raise(uint32_t c) {
    psp_cpu.fcr31 = (psp_cpu.fcr31 & ~PSP_FCR31_CAUSE) | (c << 12) | (c << 2);
}

/* One operation: round it under the current mode, flush it if FS says so, and
 * record what it raised. Kept in one place because every caller needs all
 * three and doing two of them is a subtly wrong FPU. */
static inline float psp_fpu_op(double exact, int div_by_zero) {
    const float r = psp_fpu_arith(exact, psp_cpu.fcr31);
    psp_fpu_raise(psp_fpu_except(exact, r, div_by_zero));
    return r;
}

/* sqrt.s. Separate from psp_fsqrt, whose non-positive-input rule is useful to
 * general geometry code but does not implement the instruction's edge cases.
 *
 * Exactness is decidable without an exact square root: r is the correctly
 * rounded result, so r*r is a 24x24-bit product and therefore exact in a
 * double. If it reproduces the operand, nothing was lost.
 *
 * Not modelled: the rounding mode does not reach psp_fsqrt's iteration. No
 * test covers it and inventing a directed square root to go untested is worse
 * than the gap. */
static inline float psp_fsqrt_cop1(float v) {
    const uint32_t b = psp_f32_to_bits(v);
    if (v != v)             { psp_fpu_raise(PSP_FE_V); return v; }
    if (b == 0x7F800000u)   { psp_fpu_raise(0u);       return v; }   /* +inf */
    if (b & 0x80000000u) {                                           /* any negative, -0 aside */
        if ((b & 0x7FFFFFFFu) == 0u) { psp_fpu_raise(0u); return v; }
        psp_fpu_raise(PSP_FE_V);
        return psp_bits_to_f32(0x7FBFFFFFu);
    }
    const float r = psp_fsqrt(v);
    psp_fpu_raise(((double)r * (double)r != (double)v) ? PSP_FE_I : 0u);
    return r;
}

static inline float psp_fadd(float a, float b) { return psp_fpu_op((double)a + (double)b, 0); }
static inline float psp_fsub(float a, float b) { return psp_fpu_op((double)a - (double)b, 0); }
static inline float psp_fmul(float a, float b) { return psp_fpu_op((double)a * (double)b, 0); }
static inline float psp_fdiv(float a, float b) {
    return psp_fpu_op((double)a / (double)b, b == 0.0f && a == a && a != 0.0f);
}

/* `c.<cond>.s` condition codes. The distinction that matters is *ordered* vs
 * *unordered*: with a NaN operand the ordered forms are false and the
 * unordered forms are true. Comparisons involving NaN are false in C, so the
 * NaN case is tested explicitly rather than assumed. */
static inline int psp_fcmp(unsigned cond, float a, float b) {
    const int unordered = (a != a) || (b != b);   /* either is NaN */
    switch (cond & 0xF) {
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
