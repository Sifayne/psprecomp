/* psprecomp — the VFPU. See include/psprecomp/vfpu.h. */

#include "psprecomp/vfpu.h"
#include "psprecomp/recomp_rt.h"

#include <math.h>

#include <stdio.h>
#include <string.h>

/* vpfxs, vpfxt, vpfxd: control registers 0..2, which are thread context and so
 * live in psp_cpu (see cpu.h). */
#define PFXS (psp_cpu.vfpu_ctrl[PSP_VFPU_PFXS])
#define PFXT (psp_cpu.vfpu_ctrl[PSP_VFPU_PFXT])
#define PFXD (psp_cpu.vfpu_ctrl[PSP_VFPU_PFXD])
static uint64_t g_traps;

/* The identity prefixes.
 *
 * 0xE4 is the swizzle x,y,z,w -- lane i takes source lane i -- with no
 * absolute value, constant or negation. A zero destination prefix saturates
 * nothing and masks nothing.
 *
 * These are the values the hardware restores after *every* VFPU instruction,
 * so "no prefix set" and "the identity prefix" are the same state and nothing
 * has to track which it is. That is why there is no longer a `set` flag. */
#define PFX_ST_NONE 0x0000E4u
#define PFX_D_NONE  0x000000u

/* `<=`, not `<`, so that -0.0 comes out as +0.0.
 *
 * The clamp substitutes the bound rather than passing the value through, and
 * that is observable: pspautotests saturates {-nan, -inf, -0.0, 3.0} to [0,1]
 * and hardware prints 0.000000 for the third lane where a strict comparison
 * leaves -0.000000. The [-1,1] clamp on the next line of the same test keeps
 * -0.0, which is the check that this is about the bound and not about zero. */
static float sat0(float v) { return v <= 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
static float sat1(float v) { return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v); }

void psp_vfpu_reset(void) {
    psp_cpu_reset_vfpu_ctrl();    /* prefixes, CC, rev and rcx; see cpu.c */
    g_traps = 0;
}

void psp_vfpu_set_prefix(int which, uint32_t value) {
    if (which < 0 || which > 2) return;
    psp_cpu.vfpu_ctrl[PSP_VFPU_PFXS + which] = value;
}

int psp_vfpu_prefix_pending(void) {
    return PFXS != PFX_ST_NONE || PFXT != PFX_ST_NONE
        || PFXD != PFX_D_NONE;
}

/* A prefix lasts exactly one instruction.
 *
 * Every VFPU op consumes all three, whether or not it uses them: a matrix op
 * ignores a pending swizzle but must still clear it, or it would be applied to
 * whatever came next instead. */
static void eat_prefixes(void) {
    PFXS = PFXT = PFX_ST_NONE;
    PFXD = PFX_D_NONE;
}

uint64_t psp_vfpu_trap_count(void) { return g_traps; }

void psp_vfpu_unimplemented(uint32_t addr, const char *what) {
    /* First few only: a VFPU-heavy inner loop would otherwise produce
     * megabytes of identical lines and hide everything else. */
    if (g_traps < 16)
        fprintf(stderr, "psprecomp: VFPU %s at 0x%08X not implemented\n", what, addr);
    else if (g_traps == 16)
        fprintf(stderr, "psprecomp: (further VFPU traps suppressed)\n");
    g_traps++;
}

/* ---- register addressing ------------------------------------------------- */

int psp_vfpu_regs(uint32_t vreg, int size, int out[4]) {
    const int mtx       = (vreg >> 2) & 7;
    const int col       = vreg & 3;
    int transpose       = (vreg >> 5) & 1;
    int row = 0, len = 1;

    switch (size) {
    case 1: row = (vreg >> 5) & 3; transpose = 0; len = 1; break;
    case 2: row = (vreg >> 5) & 2;                len = 2; break;
    case 3: row = (vreg >> 6) & 1;                len = 3; break;
    default:row = (vreg >> 5) & 2;                len = 4; break;
    }

    for (int i = 0; i < len; i++) {
        /* Transposed access walks columns instead of rows -- the same storage
         * seen the other way round, which is what makes a matrix transpose
         * free on this hardware. */
        const int step = (row + i) & 3;
        out[i] = transpose ? mtx * 4 + step * 32 + col
                           : mtx * 4 + col  * 32 + step;
    }
    return len;
}

/* ---- operand prefixes ----------------------------------------------------
 *
 * A prefix instruction rewrites the operands of the *next* VFPU instruction:
 * it swizzles lanes, takes absolute values, substitutes constants, negates,
 * saturates the result and masks lanes out of the write. Nothing in the
 * arithmetic instruction says any of this is happening.
 *
 * This used to be deliberately unimplemented -- an op with a prefix pending
 * reported and skipped rather than computing something that ignored it, on the
 * grounds that a loud gap beats plausible wrong numbers. That was the right
 * call while it lasted: pspgl's glRotatef builds (cos, sin) and (-sin, cos) as
 * two vmov.p under a vpfxs whose only content is a lane negation, and ignoring
 * the prefix silently produces the identity.
 *
 * Values checked against a hardware-validated implementation rather than
 * inferred: swizzle at bits 0..7 (two per lane), abs at 8..11, constant at
 * 12..15, negate at 16..19; the constant table {0, 1, 2, 0.5, 3, 1/3, 1/4,
 * 1/6} indexed by the swizzle bits with abs selecting the upper half;
 * saturation at bits 0..7 of the D prefix (1 -> [0,1], 3 -> [-1,1]) and the
 * write mask at 8..11. */

/* Read `size` lanes of `vreg` through a source prefix. */
static int read_src(uint32_t vreg, int size, uint32_t pfx, float out[4]) {
    int r[4];
    const int n = psp_vfpu_regs(vreg, size, r);

    /* The whole operand is read before any lane is rewritten: a swizzle names
     * source lanes, not results, so `y,x` must exchange rather than duplicate.
     * Lanes the operand does not have read as zero -- a `z` swizzle on a pair
     * is legal encoding and the hardware does not fault. */
    float in[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    for (int i = 0; i < n; i++) in[i] = psp_cpu.v[r[i]];

    if (pfx == PFX_ST_NONE) {                  /* the overwhelmingly common case */
        for (int i = 0; i < n; i++) out[i] = in[i];
        return n;
    }

    static const float K[8] = {
        0.0f, 1.0f, 2.0f, 0.5f, 3.0f, 1.0f / 3.0f, 0.25f, 1.0f / 6.0f
    };

    for (int i = 0; i < n; i++) {
        const unsigned sel = (pfx >>  (i * 2)) & 3;
        const unsigned abs = (pfx >> ( 8 + i)) & 1;
        const unsigned con = (pfx >> (12 + i)) & 1;
        const unsigned neg = (pfx >> (16 + i)) & 1;

        /* Bit operations, not fabsf and unary minus.
         *
         * The hardware clears or flips the sign bit, and that is observable:
         * negating a zero must give -0.0, and negating a NaN must flip its
         * sign rather than leaving it to the compiler. pspautotests prints
         * both -- `nan (-)` against `nan (+)`, and `-0.000000` against
         * `0.000000` -- and they were the last two lines of cpu/vfpu/prefixes
         * that did not match. */
        float v;
        if (con) {
            v = K[sel + (abs << 2)];
        } else {
            uint32_t bits = psp_f32_to_bits(in[sel]);
            if (abs) bits &= 0x7FFFFFFFu;
            v = psp_bits_to_f32(bits);
        }
        if (neg) v = psp_bits_to_f32(psp_f32_to_bits(v) ^ 0x80000000u);
        out[i] = v;
    }
    return n;
}

/* Write `size` lanes to `vreg` through the destination prefix. A masked lane
 * keeps whatever it held, which is how one component of a vector is written
 * without a read-modify-write. */
static void write_dst(uint32_t vreg, int size, const float in[4]) {
    int r[4];
    const int n = psp_vfpu_regs(vreg, size, r);
    const uint32_t pfx = PFXD;

    if (pfx == PFX_D_NONE) {
        for (int i = 0; i < n; i++) psp_cpu.v[r[i]] = in[i];
        return;
    }
    for (int i = 0; i < n; i++) {
        if ((pfx >> (8 + i)) & 1) continue;         /* lane masked out */
        float v = in[i];
        switch ((pfx >> (i * 2)) & 3) {
        case 1: v = sat0(v); break;                 /* clamp to [0, 1]  */
        case 3: v = sat1(v); break;                 /* clamp to [-1, 1] */
        default: break;
        }
        psp_cpu.v[r[i]] = v;
    }
}

/* ---- special values -------------------------------------------------------
 *
 * The VFPU's arithmetic is not the host's IEEE arithmetic at the edges, and
 * vfpuprobe on firmware 6.60 pins down how (steps 41-42, 51-84, 89-104):
 *
 *   - an operand whose exponent field is 0 -- a zero or a denormal -- is a
 *     signed zero: 1e-40/1e-40 is 7F800001, 3 * 1e-40 is 0, vcmp EZ says a
 *     denormal is zero, vf2iu turns 00000001 into 0;
 *   - a result below 2^-126 is flushed to a signed zero: 1e-38 + 1e-38 is 0,
 *     not the normal 00D9C7DC;
 *   - every NaN result is the one pattern 7F800001. Its sign is fixed per
 *     operation, not propagated: vadd/vsub/vbfy/vocp give +, vmul/vdiv/vscl/
 *     vcrs give sign(a) ^ sign(b) -- vmul of FFC00000 and 1.0 is FF800001,
 *     vadd of the same is 7F800001, vdiv of -0 by +0 is FF800001.
 *
 * vin/vout wrap each lane of those operations. vmov, vneg, vabs, vmin/vmax and
 * vsat move bits and keep denormals and NaN payloads as they are, as the
 * hardware does. The dot-product unit below applies the same rules itself. */
#define VNAN_BITS 0x7F800001u

static inline float vin(float f) {
    const uint32_t b = psp_f32_to_bits(f);
    return (b & 0x7F800000u) ? f : psp_bits_to_f32(b & 0x80000000u);
}

/* `nan_sign` is 0 or 0x80000000: the sign the operation gives a NaN. */
static inline float vout(float r, uint32_t nan_sign) {
    const uint32_t b = psp_f32_to_bits(r);
    const uint32_t e = b & 0x7F800000u;
    if (e == 0x7F800000u)
        return (b & 0x007FFFFFu) ? psp_bits_to_f32(VNAN_BITS | nan_sign) : r;
    return e ? r : psp_bits_to_f32(b & 0x80000000u);
}

static inline uint32_t sign_of(float f) { return psp_f32_to_bits(f) & 0x80000000u; }

/* The order vmin, vmax, vscmp and the sorts use: the bits read as a
 * sign-magnitude integer, with an exponent of 0 as zero. A NaN therefore
 * orders by its bits -- +NaN above +inf, -NaN below -inf -- and -0 equals +0
 * (steps 55-57, 69, 71, 77-80). Host `<` said false for every NaN, so vmin of
 * 2 and 7F800001 depended on the operand order. */
static inline int32_t vkey(float f) {
    const uint32_t b = psp_f32_to_bits(f);
    if (!(b & 0x7F800000u)) return 0;
    const int32_t m = (int32_t)(b & 0x7FFFFFFFu);
    return (b >> 31) ? -m : m;
}

/* ---- the dot-product unit -------------------------------------------------
 *
 * Reproduced rather than approximated, because every reduction in the VFPU is
 * this one circuit and a sum of products is not it. The shape:
 *
 *   - each product is computed to 24+2 bits with round-to-odd, so that the
 *     later truncation cannot round twice in the same direction;
 *   - all four are aligned to the largest exponent by *truncation*, and the
 *     sum of the aligned integers is exact;
 *   - the single rounding happens at the end, to nearest, ties to even;
 *   - infinities are resolved before any of that. inf * 0 and inf - inf are
 *     NaN; anything else with an infinity in it is that infinity, regardless
 *     of what the finite terms would have contributed.
 *
 * The constants are the hardware's: two extra bits, and an alignment shift
 * clamped at 28 because past that the term cannot reach the result anyway. */
float psp_vfpu_dot(const float a[4], const float b[4]) {
    enum { EXTRA_BITS = 2 };
    const uint32_t I = 1u << 23, J = 1u << (23 - EXTRA_BITS);

    int32_t  s[4], e[4], ehi = -2 * 127;
    uint32_t p[4];
    int      has_inf = 0;

    for (int i = 0; i < 4; i++) {
        const uint32_t x = psp_f32_to_bits(a[i]), y = psp_f32_to_bits(b[i]);
        const int32_t  ex = (int32_t)((x >> 23) & 255), ey = (int32_t)((y >> 23) & 255);
        const uint32_t mx = x & (I - 1), my = y & (I - 1);

        if (ex == 255 || ey == 255) {
            const int sgn = ((x ^ y) >> 31) ? -1 : +1;
            /* A quiet NaN with the low bit set -- the pattern the VFPU
             * produces, not the host's. */
            if ((ex == 255 && mx != 0) ||          /* x is NaN            */
                (ey == 255 && my != 0) ||          /* y is NaN            */
                (ex == 255 && ey == 0) ||          /* inf * 0             */
                (ey == 255 && ex == 0) ||          /* 0 * inf             */
                (has_inf && has_inf != sgn))       /* inf - inf           */
                return psp_bits_to_f32(0x7F800001u);
            has_inf = sgn;
        }

        s[i] = (int32_t)((x ^ y) >> 31);
        e[i] = ex + ey - 2 * 127;
        /* The implicit ones are put back before multiplying: this is the full
         * 24x24 product, kept to 26 bits with the discarded tail folded into
         * the low bit (round-to-odd) so the alignment below cannot lose it. */
        const uint64_t v = (uint64_t)(I + mx) * (uint64_t)(I + my);
        p[i] = (uint32_t)(v >> (23 - EXTRA_BITS));
        if (v & (J - 1)) p[i] |= 1;
        if (!(ex && ey)) { e[i] = -2 * 127; p[i] = 0; }   /* subnormals are zero */
        if (e[i] > ehi) ehi = e[i];
    }

    if (has_inf) return psp_bits_to_f32(has_inf < 0 ? 0xFF800000u : 0x7F800000u);

    int32_t val = 0;
    for (int i = 0; i < 4; i++) {
        int32_t d = ehi - e[i];
        if (d > 28) d = 28;
        val += (s[i] ? -1 : +1) * (int32_t)(p[i] >> d);
    }

    uint32_t m = (uint32_t)(val < 0 ? -val : val);
    m >>= EXTRA_BITS;

    if (m != 0) {
        /* Normalise to 2^23 <= m < 2^24, rounding to nearest with ties to
         * even -- the only rounding in the whole operation. */
        const int shift = 8 - (int)psp_clz(m);
        ehi += shift;
        if (shift > 0) {
            const uint32_t r = 1u << (shift - 1);
            m = (m >> shift) + ((m & (2 * r - 1)) + ((m >> shift) & 1) > r);
            if (m >= 2 * I) { m >>= 1; ehi += 1; }
        } else if (shift < 0) {
            m <<= -shift;
        }
    } else {
        ehi = -128;
    }

    if (ehi <= -127) { ehi = -127; m = 0; }    /* underflow flushes to zero */
    if (ehi >= +128) { ehi = +128; m = 0; }    /* overflow is an infinity   */

    return psp_bits_to_f32(((uint32_t)(val < 0) << 31) |
                           ((uint32_t)(ehi + 127) << 23) |
                           (m & 0x007FFFFFu));
}

/* ---- VFPU control registers ----------------------------------------------- */

/* All sixteen are per-thread state in psp_cpu.vfpu_ctrl (the condition codes in
 * psp_cpu.vfpu_cc), reset by psp_cpu_reset_vfpu_ctrl. They used to be file
 * statics here and were never initialised outside the unit tests: every thread
 * shared one set, and a program read rev and rcx as zero. */
uint32_t psp_mfvc(int index) {
    if (index == PSP_VFPU_CC) return psp_cpu.vfpu_cc;
    return (index >= 0 && index < 16) ? psp_cpu.vfpu_ctrl[index] : 0;
}

void psp_mtvc(int index, uint32_t value) {
    switch (index) {
    /* Writing a prefix here is the same as executing vpfxs/vpfxt/vpfxd: the
     * next VFPU op consumes it and clears it. Storing it anywhere else would
     * make the two views of the same register disagree. */
    case 0: case 1: case 2: psp_cpu.vfpu_ctrl[index] = value & 0xFFFFFFu; break;
    case 3:                 psp_cpu.vfpu_cc = value;            break;
    default:
        if (index >= 0 && index < 16) psp_cpu.vfpu_ctrl[index] = value;
        break;
    }
}

/* ---- the random generator: vrnds, vrndi, vrndf1, vrndf2 ------------------
 *
 * The state is the eight rcx control registers, each a 1.0f-shaped word whose
 * low 20 bits carry state (bits 16..19 and 0..15). Measured on firmware 6.60
 * (vfpuprobe steps 147 and 154-159) and reproduced exactly:
 *
 *   - vrnds S writes rcx_i = 0x3F800000 | ((S >> 4i) & 0xF) << 16
 *                           | (i < 4 ? S & 0xFFFF : S >> 16),
 *     for all five seeds probed (0, 1, 12345678, FFFFFFFF, 3F800000);
 *   - vrndf1 is 0x3F800000 | (r & 0x7FFFFF) and vrndf2 0x40000000 |
 *     (r & 0x7FFFFF), where r is the value vrndi would have returned: all
 *     three advance one stream, one step per lane;
 *   - the state is per thread and reset with the other control registers.
 *
 * Read that way the 160 state bits are five 32-bit words, and vrnds sets all
 * five to S: x = rcx0|rcx4 (low halves), y = rcx1|rcx5, z = rcx2|rcx6,
 * w = rcx3|rcx7, and c = the eight nibbles, rcx_i's at bits 4i. The reset
 * state is x=1 y=2 z=4 w=8 c=0. Each draw is then
 *
 *     x = 69069x + 1;  y = xorshift(y; <<13, >>17, <<5);
 *     t = z + 2w + c;  z = w;  w = t;          result x + y + w
 *
 * which is fitted, not read from a document, and reproduces every value the
 * probe logged from the reset state (00094E24 245A1029 ECF210C2 91ABC47B, in
 * the main thread and in a fresh one) and from seeds 0, 1 and 12345678 --
 * 28 of 28 -- in all three output forms.
 *
 * What is not known is the carry: what c becomes after a draw. Taking it as
 * zero is what the four sequences above require; the textbook multiply-with-
 * carry rule (c = t >> 32) breaks seed 12345678 from its fourth draw. But from
 * seed FFFFFFFF the hardware adds 2 and then 1 more to t at every draw after
 * the first, and from 3F800000 1 at the fourth and eighth, and no rule over
 * this state found so far produces both. So from those two seeds this model
 * matches the first draw and the first three respectively, and after that
 * differs in the low bits (by 2, 5, 13 ... 457 for FFFFFFFF). Reading rcx
 * after each draw would settle it. */
static uint32_t rcx_word(int lo) {
    return (psp_cpu.vfpu_ctrl[PSP_VFPU_RCX0 + lo] & 0xFFFFu) |
           (psp_cpu.vfpu_ctrl[PSP_VFPU_RCX0 + lo + 4] & 0xFFFFu) << 16;
}
static void rcx_set_word(int lo, uint32_t v) {
    uint32_t *r = &psp_cpu.vfpu_ctrl[PSP_VFPU_RCX0 + lo];
    r[0] = (r[0] & ~0xFFFFu) | (v & 0xFFFFu);
    r[4] = (r[4] & ~0xFFFFu) | (v >> 16);
}
static uint32_t rcx_nibbles(void) {
    uint32_t c = 0;
    for (int i = 0; i < 8; i++)
        c |= ((psp_cpu.vfpu_ctrl[PSP_VFPU_RCX0 + i] >> 16) & 0xFu) << (4 * i);
    return c;
}
static void rcx_set_nibbles(uint32_t c) {
    for (int i = 0; i < 8; i++) {
        uint32_t *r = &psp_cpu.vfpu_ctrl[PSP_VFPU_RCX0 + i];
        *r = (*r & ~0xF0000u) | ((c >> (4 * i)) & 0xFu) << 16;
    }
}

static uint32_t vrnd_next(void) {
    uint32_t x = rcx_word(0), y = rcx_word(1), z = rcx_word(2), w = rcx_word(3);
    const uint32_t c = rcx_nibbles();
    x = 69069u * x + 1u;
    y ^= y << 13; y ^= y >> 17; y ^= y << 5;
    const uint32_t t = z + 2u * w + c;
    z = w;
    w = t;
    rcx_set_word(0, x); rcx_set_word(1, y); rcx_set_word(2, z); rcx_set_word(3, w);
    rcx_set_nibbles(0);
    return x + y + w;
}

void psp_vrnds(uint32_t vs, int size) {
    float sv[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    read_src(vs, size, PFXS, sv);
    const uint32_t s = psp_f32_to_bits(sv[0]);
    for (int i = 0; i < 8; i++)
        psp_cpu.vfpu_ctrl[PSP_VFPU_RCX0 + i] = 0x3F800000u | ((s >> (4 * i)) & 0xFu) << 16
                                             | (i < 4 ? s & 0xFFFFu : s >> 16);
    eat_prefixes();
}

void psp_vrnd(uint32_t vd, int kind, int size) {
    int r[4];
    const int n = psp_vfpu_regs(vd, size, r);
    float out[4];
    for (int i = 0; i < n; i++) {
        const uint32_t v = vrnd_next();
        out[i] = psp_bits_to_f32(kind == 0 ? v
                                 : (kind == 1 ? 0x3F800000u : 0x40000000u) | (v & 0x7FFFFFu));
    }
    if (kind == 0) {
        /* An integer: the destination prefix masks lanes but does not
         * saturate, as for vf2i. */
        for (int i = 0; i < n; i++)
            if (!((PFXD >> (8 + i)) & 1)) psp_cpu.v[r[i]] = out[i];
    } else {
        write_dst(vd, size, out);
    }
    eat_prefixes();
}

/* ---- integer/vector moves ------------------------------------------------ */

uint32_t psp_mfv(uint32_t vd) {
    int r[4];
    psp_vfpu_regs(vd, 1, r);
    return psp_f32_to_bits(psp_cpu.v[r[0]]);
}

void psp_mtv(uint32_t vd, uint32_t bits) {
    int r[4];
    psp_vfpu_regs(vd, 1, r);
    psp_cpu.v[r[0]] = psp_bits_to_f32(bits);
}

/* ---- load / store -------------------------------------------------------- */

void psp_lv_s(uint32_t vt, uint32_t addr) {
    int r[4];
    psp_vfpu_regs(vt, 1, r);
    psp_cpu.v[r[0]] = psp_read_f32(addr & ~3u);
}

void psp_sv_s(uint32_t vt, uint32_t addr) {
    int r[4];
    psp_vfpu_regs(vt, 1, r);
    psp_write_f32(addr & ~3u, psp_cpu.v[r[0]]);
}

void psp_lv_q(uint32_t vt, uint32_t addr) {
    int r[4];
    psp_vfpu_regs(vt, 4, r);
    addr &= ~15u;                       /* quad access is 16-byte aligned */
    for (int i = 0; i < 4; i++)
        psp_cpu.v[r[i]] = psp_read_f32(addr + (uint32_t)i * 4);
}

void psp_sv_q(uint32_t vt, uint32_t addr) {
    int r[4];
    psp_vfpu_regs(vt, 4, r);
    addr &= ~15u;
    for (int i = 0; i < 4; i++)
        psp_write_f32(addr + (uint32_t)i * 4, psp_cpu.v[r[i]]);
}

/* ---- unaligned quad load/store -------------------------------------------
 *
 * `offset` is which lane the address lands on, from bits 3:2. lvl fills lanes
 * 3 down to 3-offset from descending addresses; lvr fills lanes 0 up to
 * 3-offset from ascending ones. Together they move a quad from any 4-byte
 * boundary, and each leaves the lanes it does not reach alone -- so both start
 * by reading the register. */
void psp_lvl_q(uint32_t vt, uint32_t addr) {
    int r[4];
    psp_vfpu_regs(vt, 4, r);
    const int offset = (int)((addr >> 2) & 3);
    for (int i = 0; i <= offset; i++)
        psp_cpu.v[r[3 - i]] = psp_read_f32((addr - (uint32_t)i * 4) & ~3u);
}

void psp_lvr_q(uint32_t vt, uint32_t addr) {
    int r[4];
    psp_vfpu_regs(vt, 4, r);
    const int offset = (int)((addr >> 2) & 3);
    for (int i = 0; i <= 3 - offset; i++)
        psp_cpu.v[r[i]] = psp_read_f32((addr + (uint32_t)i * 4) & ~3u);
}

void psp_svl_q(uint32_t vt, uint32_t addr) {
    int r[4];
    psp_vfpu_regs(vt, 4, r);
    const int offset = (int)((addr >> 2) & 3);
    for (int i = 0; i <= offset; i++)
        psp_write_f32((addr - (uint32_t)i * 4) & ~3u, psp_cpu.v[r[3 - i]]);
}

void psp_svr_q(uint32_t vt, uint32_t addr) {
    int r[4];
    psp_vfpu_regs(vt, 4, r);
    const int offset = (int)((addr >> 2) & 3);
    for (int i = 0; i <= 3 - offset; i++)
        psp_write_f32((addr + (uint32_t)i * 4) & ~3u, psp_cpu.v[r[i]]);
}

/* ---- horizontal reductions ------------------------------------------------
 *
 * On hardware both are a dot product against a constant vector, taken over all
 * four lanes with the unused ones reading zero. Writing it that way rather
 * than as a loop over `size` keeps one property that a loop loses: vavg of a
 * single lane is *zero*, because the constant for size 1 is 0 and not 1. */
static void reduce(uint32_t vd, uint32_t vs, int size, float k) {
    float sv[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    read_src(vs, size, PFXS, sv);

    /* A dot against a constant vector, which is literally what the hardware
     * does -- and the reason a single-lane vavg is zero, since the constant
     * for size 1 is 0 rather than 1. */
    const float kv[4] = { k, k, k, k };
    float out[4] = { psp_vfpu_dot(sv, kv), 0.0f, 0.0f, 0.0f };
    write_dst(vd, 1, out);
    eat_prefixes();
}

void psp_vfad(uint32_t vd, uint32_t vs, int size) {
    reduce(vd, vs, size, 1.0f);
}

void psp_vavg(uint32_t vd, uint32_t vs, int size) {
    static const float K[5] = { 0.0f, 0.0f, 0.5f, 1.0f / 3.0f, 0.25f };
    reduce(vd, vs, size, K[size >= 1 && size <= 4 ? size : 0]);
}

/* ---- colour packing -------------------------------------------------------
 *
 * Four 8888 pixels in, four 16-bit ones out, packed two per destination lane.
 * The lanes are read as *integers*: these are the only VFPU ops that treat the
 * register file as pixels rather than as floats, which is why the source goes
 * through the bit view and not through read_src's float path. */
void psp_vcolor(uint32_t vd, uint32_t vs, int fmt, int size) {
    int r[4];
    psp_vfpu_regs(vs, 4, r);

    uint16_t col[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < 4; i++) {
        const uint32_t in = psp_f32_to_bits(psp_cpu.v[r[i]]);
        const uint32_t a = (in >> 24) & 0xFF, b = (in >> 16) & 0xFF;
        const uint32_t g = (in >>  8) & 0xFF, rr = in & 0xFF;
        switch (fmt) {
        case 1: col[i] = (uint16_t)(((a >> 4) << 12) | ((b >> 4) << 8) |
                                    ((g >> 4) <<  4) |  (rr >> 4)); break;
        case 2: col[i] = (uint16_t)(((a >> 7) << 15) | ((b >> 3) << 10) |
                                    ((g >> 3) <<  5) |  (rr >> 3)); break;
        case 3: col[i] = (uint16_t)(((b >> 3) << 11) | ((g >> 2) <<  5) |
                                     (rr >> 3));                    break;
        default: break;
        }
    }

    float out[4];
    out[0] = psp_bits_to_f32((uint32_t)col[0] | ((uint32_t)col[1] << 16));
    out[1] = psp_bits_to_f32((uint32_t)col[2] | ((uint32_t)col[3] << 16));
    /* Half as many lanes come out as went in: a quad of pixels is a pair of
     * words. A single-lane source still writes one. */
    write_dst(vd, size == 1 ? 1 : 2, out);
    eat_prefixes();
}

void psp_vcmov(uint32_t vd, uint32_t vs, int cc_sel, int want, int size) {
    float s[4] = { 0.0f, 0.0f, 0.0f, 0.0f }, d[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    const int n = read_src(vs, size, PFXS, s);
    /* The destination is read as the second operand, T prefix and all: a lane
     * that is not moved keeps its old value, so this is a read-modify-write. */
    read_src(vd, size, PFXT, d);

    const uint32_t cc = psp_cpu.vfpu_cc;
    if (cc_sel < 6) {
        if ((int)((cc >> cc_sel) & 1) == want)
            for (int i = 0; i < n; i++) d[i] = s[i];
    } else if (cc_sel == 6) {
        /* Selector 6: every lane consults its own condition bit. */
        for (int i = 0; i < n; i++)
            if ((int)((cc >> i) & 1) == want) d[i] = s[i];
    }

    write_dst(vd, size, d);
    eat_prefixes();
}

/* One encoding, two operations, told apart by the operand width: a triple is
 * the cross product and a quad is the quaternion product. Both go through the
 * dot-product unit. The cross product's lanes are two-term dots with no padding
 * terms: vfpuprobe step 65 (fw 6.60) gives (inf,1,2) x (1,2,3) as
 * [BF800000 FF800000 7F800000], the plain cross product, where padding with a
 * zero lane made an inf * 0 NaN of lane 0; and the sqrt-edge row's lane 2 is 0,
 * not a NaN from s[3] * t[2]. */
void psp_vcrsp(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    float s[4] = { 0.0f, 0.0f, 0.0f, 0.0f }, t[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    read_src(vs, size, PFXS, s);
    read_src(vt, size, PFXT, t);

    float d[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    if (size == 4) {                                     /* vqmul.q */
        const float t0[4] = {  t[3], t[2], -t[1], t[0] };
        const float t1[4] = { -t[2], t[3],  t[0], t[1] };
        const float t2[4] = {  t[1], -t[0], t[3], t[2] };
        const float t3[4] = { -t[0], -t[1], -t[2], t[3] };
        d[0] = psp_vfpu_dot(s, t0);
        d[1] = psp_vfpu_dot(s, t1);
        d[2] = psp_vfpu_dot(s, t2);
        d[3] = psp_vfpu_dot(s, t3);
    } else {                                             /* vcrsp.t */
        const float a0[4] = { s[1], s[2], 0.0f, 0.0f }, b0[4] = { t[2], -t[1], 0.0f, 0.0f };
        const float a1[4] = { s[2], s[0], 0.0f, 0.0f }, b1[4] = { t[0], -t[2], 0.0f, 0.0f };
        const float a2[4] = { s[0], s[1], 0.0f, 0.0f }, b2[4] = { t[1], -t[0], 0.0f, 0.0f };
        d[0] = psp_vfpu_dot(a0, b0);
        d[1] = psp_vfpu_dot(a1, b1);
        d[2] = psp_vfpu_dot(a2, b2);
    }

    write_dst(vd, size, d);
    eat_prefixes();
}

void psp_vhdp(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    float sv[4] = { 0.0f, 0.0f, 0.0f, 0.0f }, tv[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    const int n = read_src(vs, size, PFXS, sv);
    read_src(vt, size, PFXT, tv);

    /* The last lane of the source is a forced 1: that is the whole difference
     * from vdot, and it is what makes this the homogeneous form. */
    sv[n - 1] = 1.0f;
    float out[4] = { psp_vfpu_dot(sv, tv), 0.0f, 0.0f, 0.0f };
    write_dst(vd, 1, out);
    eat_prefixes();
}

void psp_vcrs(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    float sv[4] = { 0.0f, 0.0f, 0.0f, 0.0f }, tv[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    read_src(vs, size, PFXS, sv);
    read_src(vt, size, PFXT, tv);

    /* s is forced to yzx and t to zxy, then multiplied lane by lane. There is
     * no subtraction: this is half a cross product, and a full one is two of
     * these with a vsub between. Each lane is a vmul, special values and all:
     * -0 * -inf is 7F800001 and FFC00000 * 7FC00001 is FF800001 (step 64). */
    static const int SI[4] = { 1, 2, 0, 3 }, TI[4] = { 2, 0, 1, 3 };
    float out[4];
    for (int i = 0; i < 4; i++) {
        const float a = sv[SI[i]], b = tv[TI[i]];
        out[i] = vout(vin(a) * vin(b), sign_of(a) ^ sign_of(b));
    }

    write_dst(vd, size, out);
    eat_prefixes();
}

void psp_vdet(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    float sv[4] = { 0.0f, 0.0f, 0.0f, 0.0f }, tv[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    read_src(vs, size, PFXS, sv);
    read_src(vt, size, PFXT, tv);

    /* t's first two lanes are forced to yx and s's second is negated, so the
     * dot comes out as s0*t1 - s1*t0. Lanes beyond the pair contribute as they
     * are, which is why they are read at all -- normally they are zero. */
    const float sn[4] = { sv[0], -sv[1], sv[2], sv[3] };
    const float ts[4] = { tv[1],  tv[0], tv[2], tv[3] };
    float out[4] = { psp_vfpu_dot(sn, ts), 0.0f, 0.0f, 0.0f };
    write_dst(vd, 1, out);
    eat_prefixes();
}

/* ---- comparisons that produce values ------------------------------------- */

void psp_vcmp_val(uint32_t vd, uint32_t vs, uint32_t vt, int kind, int size) {
    float sv[4] = { 0.0f, 0.0f, 0.0f, 0.0f }, tv[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    const int n = read_src(vs, size, PFXS, sv);
    read_src(vt, size, PFXT, tv);

    float d[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    for (int i = 0; i < n; i++) {
        if (kind == 0) {                                  /* vscmp: -1, 0, 1 */
            /* The sign-magnitude order of vmin/vmax, NaNs and denormals
             * included: vscmp of 007FFFFF and 0 is 0, of -1 and 7FC00000 is
             * -1 (step 57). Subtracting first, as this used to, compared the
             * denormal as a number and fell back to an int32 difference that
             * could overflow. */
            const int32_t ks = vkey(sv[i]), kt = vkey(tv[i]);
            d[i] = (float)((ks > kt) - (ks < kt));
        } else {
            /* A NaN on either side is false, not "unordered": both of these
             * answer 0.0 rather than propagating it -- unlike vscmp, which
             * orders NaNs (vfpuprobe steps 58-59 agree). A denormal is a zero,
             * as it is to vcmp. */
            const float a = vin(sv[i]), b = vin(tv[i]);
            const int nan = (a != a) || (b != b);
            if (nan)              d[i] = 0.0f;
            else if (kind == 1)   d[i] = (a >= b) ? 1.0f : 0.0f;
            else                  d[i] = (a <  b) ? 1.0f : 0.0f;
        }
    }
    write_dst(vd, size, d);
    eat_prefixes();
}

/* vwbn rebases lane 0 onto a given exponent, shifting the mantissa the other
 * way so the value is as close as it can be. A zero, an infinity or a NaN has
 * no mantissa to shift, so the exponent is simply ORed in. Lanes above 0 pass
 * through untouched. */
void psp_vwbn(uint32_t vd, uint32_t vs, int exp, int size) {
    float sv[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    read_src(vs, size, PFXS, sv);

    const uint32_t e = (uint32_t)(exp & 0xFF);
    const uint32_t b = psp_f32_to_bits(sv[0]);
    const uint32_t sign = b & 0x80000000u;
    const uint32_t prev = (b & 0x7F800000u) >> 23;
    uint32_t man = (b & 0x007FFFFFu) | 0x00800000u;

    float out[4];
    if (prev != 0xFF && prev != 0) {
        /* The shift wraps at 16: the hardware masks it to four bits, so a very
         * large exponent change rotates rather than flushing to zero. */
        if (e > prev) man >>= ((e - prev) & 0xF);
        else          man <<= ((prev - e) & 0xF);
        out[0] = psp_bits_to_f32(sign | (man & 0x007FFFFFu) | (e << 23));
    } else {
        out[0] = psp_bits_to_f32(b | (e << 23));
    }
    for (int i = 1; i < 4; i++) out[i] = sv[i];

    write_dst(vd, size, out);
    eat_prefixes();
}

void psp_vsbn(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    float sv[4] = { 0.0f, 0.0f, 0.0f, 0.0f }, tv[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    read_src(vs, size, PFXS, sv);
    read_src(vt, size, PFXT, tv);

    /* vt's first lane is read as an *integer* exponent, biased on the way in. */
    const uint32_t exp = (uint32_t)(uint8_t)(127 + (int32_t)psp_f32_to_bits(tv[0]));

    float out[4];
    const uint32_t bits = psp_f32_to_bits(sv[0]);
    const uint32_t prev = bits & 0x7F800000u;
    /* Only a normal number gets its exponent replaced: a zero, an infinity or
     * a NaN is already saying something the exponent field cannot carry. */
    out[0] = (prev != 0 && prev != 0x7F800000u)
           ? psp_bits_to_f32((bits & ~0x7F800000u) | (exp << 23))
           : sv[0];
    for (int i = 1; i < 4; i++) out[i] = sv[i];

    write_dst(vd, size, out);
    eat_prefixes();
}

/* ---- VFPU9: a vector against a swizzled copy of itself --------------------
 *
 * The sorts, the butterflies, the one's complement and the sign. Hardware
 * builds the second operand by forcing a swizzle or a constant into the T
 * prefix and then running an ordinary min/max/add, which is why none of these
 * names a second register. Written out directly here: synthesising a prefix in
 * order to consume it would be a faithful description of the hardware and a
 * worse description of the arithmetic. */
/* One compare-exchange of the sorts, in the vmin/vmax order (vkey): lanes i < j
 * receive the smaller and the larger value, or the other way round.
 *
 * A tie -- -0 against +0, a denormal against zero -- does not keep both values:
 * both lanes receive the same one, the lower lane's in vsrt1/vsrt2 and the
 * higher lane's in vsrt3/vsrt4. vsrt1 of (0, -0, 0, -0) is all 00000000 and
 * vsrt3 of it all 80000000 (steps 77-80; only the zeros and denormal rows
 * exercise ties). */
static void sort_pair(const float s[4], float d[4], int i, int j, int ascending) {
    const int32_t ki = vkey(s[i]), kj = vkey(s[j]);
    if (ki == kj) { d[i] = d[j] = ascending ? s[i] : s[j]; return; }
    const float lo = ki < kj ? s[i] : s[j], hi = ki < kj ? s[j] : s[i];
    d[i] = ascending ? lo : hi;
    d[j] = ascending ? hi : lo;
}

void psp_vfpu9(uint32_t vd, uint32_t vs, int kind, int size) {
    float s[4] = { 0.0f, 0.0f, 0.0f, 0.0f }, d[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    const int n = read_src(vs, size, PFXS, s);

    /* The butterflies and vocp are vadd/vsub lanes, special values and all:
     * NaN results are +7F800001 whatever the operands' signs (steps 72, 81,
     * 82). */
    #define ADD(a, b) vout(vin(a) + vin(b), 0)
    #define SUB(a, b) vout(vin(a) - vin(b), 0)
    switch (kind) {
    case 0:  sort_pair(s, d, 0, 1, 1); sort_pair(s, d, 2, 3, 1); break;  /* vsrt1 */
    case 1:  sort_pair(s, d, 0, 3, 1); sort_pair(s, d, 1, 2, 1); break;  /* vsrt2 */
    case 8:  sort_pair(s, d, 0, 1, 0); sort_pair(s, d, 2, 3, 0); break;  /* vsrt3 */
    case 9:  sort_pair(s, d, 0, 3, 0); sort_pair(s, d, 1, 2, 0); break;  /* vsrt4 */
    case 2:                                              /* vbfy1 */
        d[0] = ADD(s[0], s[1]); d[1] = SUB(s[0], s[1]);
        d[2] = ADD(s[2], s[3]); d[3] = SUB(s[2], s[3]);
        break;
    case 3:                                              /* vbfy2 */
        d[0] = ADD(s[0], s[2]); d[1] = ADD(s[1], s[3]);
        d[2] = SUB(s[0], s[2]); d[3] = SUB(s[1], s[3]);
        break;
    case 4:                                              /* vocp: 1 - s */
        for (int i = 0; i < 4; i++) d[i] = SUB(1.0f, s[i]);
        break;
    case 10:                                             /* vsgn */
        /* On the bits: an exponent of 0 gives +0 (vsgn of -1e-40 is 0, not
         * -1), anything else its sign, NaNs and infinities included (step
         * 71). */
        for (int i = 0; i < n; i++) {
            const uint32_t b = psp_f32_to_bits(s[i]);
            d[i] = !(b & 0x7F800000u) ? 0.0f : (b >> 31) ? -1.0f : 1.0f;
        }
        break;
    default:
        psp_vfpu_unimplemented(psp_cpu.pc, "vfpu9");
        eat_prefixes();
        return;
    }
    #undef ADD
    #undef SUB

    write_dst(vd, size, d);
    eat_prefixes();
}

static void read_bits(uint32_t vs, int size, uint32_t out[4]);

/* ---- float/integer conversion with a scale -------------------------------
 *
 * The scale is a 5-bit exponent in the instruction: vf2i multiplies by 2^n
 * before rounding and vi2f divides by it after, which is how the VFPU does
 * fixed point. psp_vunary carried "f2iz" and "i2f" entries that ignored it
 * entirely -- correct for n = 0, which is what a compiler emits for a plain
 * cast, and silently wrong for every other n. Those entries are gone. */
void psp_vf2i(uint32_t vd, uint32_t vs, int mode, int scale, int size) {
    float sv[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    const int n = read_src(vs, size, PFXS, sv);
    const double mult = (double)(1u << (scale & 0x1F));

    uint32_t d[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < n; i++) {
        /* A denormal is zero here too: vf2iu of 00000001 is 0, not 1 (step
         * 41). */
        const float f = vin(sv[i]);
        if (f != f) { d[i] = 0x7FFFFFFFu; continue; }      /* NaN -> INT_MAX */
        const double v = (double)f * mult;
        /* Compared in double: (float)0x7FFFFFFF rounds up to 0x80000000, so a
         * float comparison would saturate one value early. */
        if (v >  2147483647.0)      d[i] = 0x7FFFFFFFu;
        else if (v <= -2147483648.0) d[i] = 0x80000000u;
        else {
            double r;
            switch (mode) {
            /* nearbyint, not round: the tie goes to even, not away from
             * zero. Hardware turns 0.5 into 0 and 2.5 into 2, and `round`
             * gives 1 and 3 -- four lines of cpu/vfpu/convert. */
            case 0:  r = nearbyint(v);                        break;  /* nearest */
            case 1:  r = (f >= 0.0f) ? floor(v) : ceil(v);    break;  /* to zero */
            case 2:  r = ceil(v);                             break;  /* up */
            default: r = floor(v);                            break;  /* down */
            }
            d[i] = (uint32_t)(int32_t)r;
        }
    }
    /* The destination prefix masks lanes here but does not saturate: the value
     * is already an integer and clamping it to [0,1] would be nonsense. */
    int r[4];
    const int dn = psp_vfpu_regs(vd, size, r);
    for (int i = 0; i < dn; i++)
        if (!((PFXD >> (8 + i)) & 1))
            psp_cpu.v[r[i]] = psp_bits_to_f32(d[i]);
    eat_prefixes();
}

/* Defined with the packed-integer conversions below, which is where it belongs;
 * declared here because vi2f is the one caller that precedes them. */
static void read_bits(uint32_t vs, int size, uint32_t out[4]);

void psp_vi2f(uint32_t vd, uint32_t vs, int scale, int size) {
    uint32_t s[4];
    read_bits(vs, size, s);
    const float mult = 1.0f / (float)(1u << (scale & 0x1F));
    float out[4];
    for (int i = 0; i < 4; i++) out[i] = (float)(int32_t)s[i] * mult;
    write_dst(vd, size, out);
    eat_prefixes();
}

/* ---- packed-integer conversions ------------------------------------------
 *
 * The register file is read as integers here, so these go through the bit view
 * rather than read_src's float path. Each variant decides its own output width
 * from the input width -- unpacking widens, packing narrows -- and that is the
 * part worth checking against hardware rather than assuming, because a wrong
 * width writes lanes nobody asked for. */

static void read_bits(uint32_t vs, int size, uint32_t out[4]) {
    int r[4];
    const int n = psp_vfpu_regs(vs, size, r);
    for (int i = 0; i < 4; i++) out[i] = 0;
    for (int i = 0; i < n; i++) out[i] = psp_f32_to_bits(psp_cpu.v[r[i]]);
}

static void write_bits(uint32_t vd, int size, const uint32_t in[4]) {
    float f[4];
    for (int i = 0; i < 4; i++) f[i] = psp_bits_to_f32(in[i]);
    write_dst(vd, size, f);
    eat_prefixes();
}

void psp_vx2i(uint32_t vd, uint32_t vs, int kind, int size) {
    uint32_t s[4], d[4] = { 0, 0, 0, 0 };
    read_bits(vs, size, s);
    int oz = 2;

    switch (kind) {
    case 0: {                                   /* vuc2i */
        /* 8-bit unsigned to 31-bit signed: the byte is smeared across all four
         * bytes so that 0xFF maps to just under INT_MAX rather than to a value
         * with a hole in it, then shifted down one to leave the sign clear. */
        uint32_t v = s[0];
        for (int i = 0; i < 4; i++) { d[i] = (v & 0xFF) * 0x01010101u >> 1; v >>= 8; }
        oz = 4;
        break;
    }
    case 1:                                     /* vc2i -- signed, so no smear */
        d[0] = (s[0] & 0x000000FFu) << 24;
        d[1] = (s[0] & 0x0000FF00u) << 16;
        d[2] = (s[0] & 0x00FF0000u) <<  8;
        d[3] = (s[0] & 0xFF000000u);
        oz = 4;
        break;
    case 2:                                     /* vus2i */
    case 3: {                                   /* vs2i */
        /* One source lane becomes two, so a pair is the widest input that
         * fits; triples and quads are treated as pairs. */
        const int n = size >= 2 ? 2 : 1;
        oz = n * 2;
        for (int i = 0; i < n; i++) {
            const uint32_t v = s[i];
            if (kind == 2) {                    /* unsigned: shift to 31 bits */
                d[i * 2]     = (v & 0x0000FFFFu) << 15;
                d[i * 2 + 1] = (v & 0xFFFF0000u) >> 1;
            } else {                            /* signed: straight into the top */
                d[i * 2]     = (v & 0x0000FFFFu) << 16;
                d[i * 2 + 1] =  v & 0xFFFF0000u;
            }
        }
        break;
    }
    default: break;
    }
    write_bits(vd, oz, d);
}

void psp_vi2x(uint32_t vd, uint32_t vs, int kind, int size) {
    uint32_t s[4], d[4] = { 0, 0, 0, 0 };
    read_bits(vs, size, s);
    int oz;

    switch (kind) {
    case 0:                                     /* vi2uc -- negatives clamp to 0 */
        for (int i = 0; i < 4; i++) {
            int32_t v = (int32_t)s[i];
            if (v < 0) v = 0;
            d[0] |= ((uint32_t)(v >> 23) & 0xFF) << (i * 8);
        }
        oz = 1;
        break;
    case 1:                                     /* vi2c -- signed, top byte */
        for (int i = 0; i < 4; i++) d[0] |= (s[i] >> 24) << (i * 8);
        oz = 1;
        break;
    case 2:                                     /* vi2us */
    case 3: {                                   /* vi2s */
        const int elems = (size + 1) / 2;
        for (int i = 0; i < elems; i++) {
            if (kind == 2) {
                int32_t lo = (int32_t)s[i * 2], hi = (int32_t)s[i * 2 + 1];
                if (lo < 0) lo = 0;
                if (hi < 0) hi = 0;
                d[i] = ((uint32_t)(lo >> 15) & 0xFFFFu) | ((uint32_t)(hi >> 15) << 16);
            } else {
                d[i] = (s[i * 2] >> 16) | ((s[i * 2 + 1] >> 16) << 16);
            }
        }
        oz = size >= 3 ? 2 : 1;
        break;
    }
    default: oz = 1; break;
    }
    write_bits(vd, oz, d);
}

void psp_vh2f(uint32_t vd, uint32_t vs, int size) {
    uint32_t s[4];
    read_bits(vs, size, s);
    float out[4];
    /* Every size but single is treated as a pair: two packed halves per lane,
     * so one lane in gives two out and two give four. */
    const int oz = (size == 1) ? 2 : 4;
    for (int i = 0; i < oz / 2; i++) {
        out[i * 2]     = psp_half_to_f32((uint16_t)(s[i] & 0xFFFF));
        out[i * 2 + 1] = psp_half_to_f32((uint16_t)(s[i] >> 16));
    }
    write_dst(vd, oz, out);
    eat_prefixes();
}

void psp_vf2h(uint32_t vd, uint32_t vs, int size) {
    float sv[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    read_src(vs, size, PFXS, sv);
    const int oz = (size <= 2) ? 1 : 2;
    uint32_t d[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < oz; i++)
        d[i] = (uint32_t)psp_f32_to_half(sv[i * 2]) |
               ((uint32_t)psp_f32_to_half(sv[i * 2 + 1]) << 16);
    write_bits(vd, oz, d);
}

/* ---- arithmetic ---------------------------------------------------------- */

/* `a` and `b` are the lane's operands after the prefixes, `expr` the result;
 * each op wraps its own special-value rules around the arithmetic (see vin and
 * vout above). */
#define BINOP(name, expr)                                                    \
    void psp_##name(uint32_t vd, uint32_t vs, uint32_t vt, int size) {       \
        /* Read every source before writing any destination: vd may alias vs \
         * or vt, and a lane-by-lane read/write would then feed results back  \
         * into later lanes. read_src copies, so this holds for free. */     \
        float sv[4], tv[4], out[4];                                          \
        const int n = read_src(vs, size, PFXS, sv);                          \
        read_src(vt, size, PFXT, tv);                                        \
        for (int i = 0; i < n; i++) {                                        \
            float a = sv[i], b = tv[i];                                      \
            out[i] = (expr);                                                 \
        }                                                                    \
        write_dst(vd, size, out);                                            \
        eat_prefixes();                                                      \
    }

BINOP(vadd, vout(vin(a) + vin(b), 0))
BINOP(vsub, vout(vin(a) - vin(b), 0))
BINOP(vmul, vout(vin(a) * vin(b), sign_of(a) ^ sign_of(b)))
BINOP(vdiv, vout(vin(a) / vin(b), sign_of(a) ^ sign_of(b)))
/* Ties return t, and the bits come through untouched, denormals and NaN
 * payloads included (vmax of -0 and +0 is +0, of +0 and -0 is -0). */
BINOP(vmin, vkey(a) < vkey(b) ? a : b)
BINOP(vmax, vkey(a) > vkey(b) ? a : b)

/* Dot product: sums all lanes into a single destination lane. */
void psp_vdot(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    float sv[4] = { 0.0f, 0.0f, 0.0f, 0.0f }, tv[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    read_src(vs, size, PFXS, sv);
    read_src(vt, size, PFXT, tv);

    /* Four lanes always: the unused ones are zero and contribute nothing, and
     * going through the one unit is what makes the rounding match. */
    float out[4] = { psp_vfpu_dot(sv, tv), 0.0f, 0.0f, 0.0f };
    write_dst(vd, 1, out);
    /* Recorded beside the comparisons: a clip test reading NaN is only half a
     * finding, because the dot product that fed it is where the NaN either
     * came from or was passed along, and telling those apart needs its inputs.
     * Recorded after write_dst, so a destination that never gets written shows
     * as a result the file does not contain. */
    psp_vfpu_note_dot(vd, vs, vt, sv, tv, out[0]);
    eat_prefixes();
}

/* Scale: every lane of vs multiplied by the scalar in vt. */
void psp_vscl(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    float sv[4], tv[4], out[4];
    const int n = read_src(vs, size, PFXS, sv);
    read_src(vt, 1, PFXT, tv);

    const float k = tv[0];
    for (int i = 0; i < n; i++)
        out[i] = vout(vin(sv[i]) * vin(k), sign_of(sv[i]) ^ sign_of(k));
    write_dst(vd, size, out);
    eat_prefixes();
}

/* ---- unary element-wise ops (VFPU4) -------------------------------------- */

void psp_vunary(int op, uint32_t vd, uint32_t vs, int size) {
    float sv[4], out[4];
    const int n = read_src(vs, size, PFXS, sv);

    for (int i = 0; i < n; i++) {
        const float a = sv[i];
        float r;
        switch (op) {
        case PSP_VU_MOV:  r = a;            break;
        /* On the bits: vabs of -0 is +0 and of FFC00000 is 7FC00000, where
         * `a < 0 ? -a : a` kept both (step 69). vneg likewise flips only the
         * sign bit, NaN payloads and denormals included. */
        case PSP_VU_ABS:  r = psp_bits_to_f32(psp_f32_to_bits(a) & 0x7FFFFFFFu); break;
        case PSP_VU_NEG:  r = psp_bits_to_f32(psp_f32_to_bits(a) ^ 0x80000000u); break;
        case PSP_VU_ZERO: r = 0.0f;         break;
        case PSP_VU_ONE:  r = 1.0f;         break;
        case PSP_VU_RCP:  r = 1.0f / a;     break;
        case PSP_VU_NRCP: r = -1.0f / a;    break;

        /* The square roots classify their argument before computing anything,
         * and the classes are not what the C library would do. psp_fsqrt is a
         * general geometry helper, so the instruction's zero, denormal,
         * negative, infinity and NaN rules live here rather than in it.
         *
         * Ordinary values already agree to the last bit; only the edges did
         * not, and they were 28 lines of cpu/vfpu/vector. */
        case PSP_VU_SQRT: {
            const uint32_t b = psp_f32_to_bits(a);
            if ((b & 0x7FFFFFFFu) <= 0x007FFFFFu)
                r = 0.0f;                                   /* zero, denormal, either sign */
            else if (b >> 31)
                r = psp_bits_to_f32(0x7F800001u);           /* negative -> NaN */
            else if ((b >> 23) == 255u)
                r = psp_bits_to_f32(0x7F800000u + ((b & 0x007FFFFFu) != 0u));
            else
                r = psp_fsqrt(a);
            break;
        }
        case PSP_VU_RSQ: {
            const uint32_t b = psp_f32_to_bits(a);
            if ((b & 0x7FFFFFFFu) <= 0x007FFFFFu)
                r = psp_bits_to_f32(0x7F800000u | (b & 0x80000000u)); /* +-0 -> +-inf */
            else if (b >> 31)
                r = psp_bits_to_f32(0xFF800001u);           /* negative -> negative NaN */
            else if ((b >> 23) == 255u)
                r = psp_bits_to_f32((b & 0x007FFFFFu) ? 0x7F800001u : 0u); /* inf -> 0 */
            else
                r = 1.0f / psp_fsqrt(a);
            break;
        }
        /* The PSP's trig takes its argument in *quarter turns*: vsin(x) is
         * sin(x * pi/2), not sin(x). Treating it as radians gives a result
         * that is smooth, plausible, and wrong -- rotations end up at the
         * wrong angle rather than visibly broken. */
        case PSP_VU_SIN:  r = sinf(a * 1.5707963267948966f);  break;
        case PSP_VU_COS:  r = cosf(a * 1.5707963267948966f);  break;
        case PSP_VU_NSIN: r = -sinf(a * 1.5707963267948966f); break;
        case PSP_VU_ASIN: r = asinf(a) * 0.6366197723675814f; break;  /* 2/pi */
        case PSP_VU_EXP2: r = powf(2.0f, a);   break;
        case PSP_VU_REXP2:r = 1.0f / powf(2.0f, a); break;
        case PSP_VU_LOG2: r = logf(a) * 1.4426950408889634f; break;   /* 1/ln2 */
        case PSP_VU_SAT0: r = sat0(a);      break;
        case PSP_VU_SAT1: r = sat1(a);      break;
        default:
            psp_vfpu_unimplemented(psp_cpu.pc, "vunary");
            eat_prefixes();
            return;
        }
        out[i] = r;
    }
    write_dst(vd, size, out);
    eat_prefixes();
}

/* ---- matrix ops without a multiply --------------------------------------- */

/* A matrix register names `size` consecutive columns, each a vector of `size`
 * lanes. Writing one column at a time through the same addressing the vector
 * ops use keeps the two consistent -- which matters because a game builds a
 * matrix with these and then transforms with vtfm. */
static void matrix_cols(uint32_t vd, int size, int cols[4][4]) {
    /* A matrix register names a *sub-matrix*, not just a matrix.
     *
     * `M022` is the 2x2 at column 2, row 2 -- the bottom-right quarter -- and
     * `vmidt.p M022` writes an identity there while leaving the rest alone.
     * The base column is bits 1:0 and the base row is bit 6, exactly as for a
     * vector register, and both indices wrap within the 4x4.
     *
     * An earlier version built a fresh register per column from the matrix and
     * transpose bits only, which forced both offsets to zero: every op wrote
     * the top-left corner whatever register it was given. Before that, one
     * modified bits 6:5 of the incoming register, which varies the *row* field
     * and so walked rows while claiming to walk columns. Third time: derive
     * the indices directly, so there is no synthetic register to get wrong.
     *
     * `cols[c][r]` is the element at column c, row r of the operand as the
     * instruction sees it -- so the transpose bit swaps which storage axis
     * each index lands on, and callers never have to know. */
    const int mtx       = (int)((vd >> 2) & 7);
    const int col       = (int)(vd & 3);
    const int transpose = (int)((vd >> 5) & 1);
    const int row       = (size == 3) ? (int)((vd >> 6) & 1)
                                      : (int)((vd >> 5) & 2);

    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++) {
            const int cc = (col + c) & 3;
            const int rr = (row + r) & 3;
            /* Storage is v[matrix*4 + column*32 + row]; see vfpu.h. */
            cols[c][r] = transpose ? mtx * 4 + rr * 32 + cc
                                   : mtx * 4 + cc * 32 + rr;
        }
}

/* Read a whole matrix out into [col][row] order. */
static void matrix_read(uint32_t v, int size, float m[4][4]) {
    int cols[4][4];
    matrix_cols(v, size, cols);
    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++) m[c][r] = psp_cpu.v[cols[c][r]];
}

static void matrix_write(uint32_t v, int size, const float m[4][4]) {
    int cols[4][4];
    matrix_cols(v, size, cols);
    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++) psp_cpu.v[cols[c][r]] = m[c][r];
}

void psp_vmidt(uint32_t vd, int size) {
    int cols[4][4];
    matrix_cols(vd, size, cols);
    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++)
            psp_cpu.v[cols[c][r]] = (c == r) ? 1.0f : 0.0f;
    eat_prefixes();
}

/* vidt -- an identity *vector*: all zeroes but for a single 1.0.
 *
 * Which element gets the 1 is the destination's own lane -- vd & 3, the same
 * element field every vector op in this file addresses by -- rather than
 * given as an operand, which is how one instruction builds any basis vector.
 *
 * This used to take bits 6-7, which names row 0 for every quad lane register
 * the rotation builders use (v0..v3 all read lane 0), so each of the three
 * builders wrote (1,0,0,0) for two of its four rows. Their outputs feed
 * vmmul directly, and a rotation with two identical rows collapses the
 * geometry it transforms: every per-object world matrix in the hangar came
 * out rank-1 ([X,X,X,T]), the room folded onto a line, and the scene rendered
 * nearly black. The builders are the oracle here -- with lane = vd & 3 they
 * produce orthonormal X/Y/Z rotations, with anything else they cannot -- and
 * no pspautotests suite covers vidt, so only the game could say it.
 *
 * This is matrix-setup code. Trapping it to a no-op leaves whatever was in the
 * register, so downstream geometry is built on a basis that is not a basis. */
void psp_vidt(uint32_t vd, int size) {
    int r[4];
    const int n = psp_vfpu_regs(vd, size, r);
    const int one = (int)(vd & 3);
    float out[4];
    for (int i = 0; i < n; i++) out[i] = (i == one) ? 1.0f : 0.0f;
    write_dst(vd, size, out);
    eat_prefixes();
}

/* vcst -- load a constant from the VFPU's built-in table.
 *
 * The index is in the vs field. Values follow the hardware table; index 0 is
 * zero and anything past the end reads as zero rather than as garbage. */
void psp_vcst(uint32_t vd, uint32_t which, int size) {
    static const float K[20] = {
        0.0f,
        3.4028235e38f,          /* max float          */
        1.41421356f,            /* sqrt(2)            */
        0.70710678f,            /* sqrt(1/2)          */
        1.12837917f,            /* 2/sqrt(pi)         */
        0.63661977f,            /* 2/pi               */
        0.318309886f,           /* 1/pi, 3EA2F983: the old literal 0.31830989f
                                 * rounded to 3EA2F984 (vfpuprobe step 50) */
        0.78539816f,            /* pi/4               */
        1.57079633f,            /* pi/2               */
        3.14159265f,            /* pi                 */
        2.71828183f,            /* e                  */
        1.44269504f,            /* log2(e)            */
        0.43429448f,            /* log10(e)           */
        0.69314718f,            /* ln(2)              */
        2.30258509f,            /* ln(10)             */
        6.28318531f,            /* 2*pi               */
        0.52359878f,            /* pi/6               */
        0.30103000f,            /* log10(2)           */
        3.32192809f,            /* log2(10)           */
        0.86602540f,            /* sqrt(3)/2          */
    };

    const float k = which < 20 ? K[which] : 0.0f;
    int r[4];
    const int n = psp_vfpu_regs(vd, size, r);
    float out[4];
    for (int i = 0; i < n; i++) out[i] = k;
    write_dst(vd, size, out);
    eat_prefixes();
}

void psp_vmzero(uint32_t vd, int size) {
    int cols[4][4];
    matrix_cols(vd, size, cols);
    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++) psp_cpu.v[cols[c][r]] = 0.0f;
    eat_prefixes();
}

void psp_vmone(uint32_t vd, int size) {
    int cols[4][4];
    matrix_cols(vd, size, cols);
    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++) psp_cpu.v[cols[c][r]] = 1.0f;
    eat_prefixes();
}

void psp_vmmov(uint32_t vd, uint32_t vs, int size) {
    int dc[4][4], sc[4][4];
    matrix_cols(vd, size, dc);
    matrix_cols(vs, size, sc);
    float tmp[4][4];
    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++) tmp[c][r] = psp_cpu.v[sc[c][r]];
    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++) psp_cpu.v[dc[c][r]] = tmp[c][r];
    eat_prefixes();
}

/* ---- matrix multiply and transform --------------------------------------- */

/* Scale every element of a matrix by a scalar. Orientation-independent, so
 * this one is unambiguous. */
/* vrot -- a row of a rotation matrix from one angle.
 *
 * Transcribed from the reference description in pspdev/vfpu-docs
 * (inst-vfpu-desc.yaml, auxiliary function `ivrot`), not from memory: the
 * lane rules are easy to state wrongly and the equal-selector case is a
 * genuine special case rather than a degenerate one.
 *
 *   cl = imm & 3          the lane that receives cos
 *   sl = (imm >> 2) & 3   the lane that receives sin
 *   imm & 0x10            negate the sine
 *
 * When cl != sl, every other lane is zero. When cl == sl, the named lane gets
 * the cosine and *all* the others get the sine -- not zero, which is the
 * mistake this comment exists to prevent.
 *
 * The angle is in quarter-turns, matching psp_vunary's sin/cos above: a value
 * of 1.0 is a right angle.
 *
 * The hardware requires vd and vs not to overlap (the docs mark this
 * `no-overlap`). Reading the angle once, before writing anything, means an
 * overlapping pair still produces a defined result here rather than depending
 * on lane order. */
void psp_vrot(uint32_t vd, uint32_t vs, uint32_t imm, int size) {
    /* Through psp_vfpu_regs, like every other op in this file.
     *
     * This used to index psp_cpu.v[] directly -- `v[vs & 127]` for the angle
     * and `v[(vd + i) & 127]` for each lane -- which is wrong twice. Lanes of a
     * vector are 32 apart in the register file, not adjacent, so stepping by
     * one walks across four different matrices; and the row offset in bit 6 is
     * never applied, so a register naming row 2 writes row 0.
     *
     * pspgl's glRotatef is one vrot, and it showed both at once: the rotation
     * landed in registers nobody read, leaving the identity behind, and what
     * did get read came out two rows over. */
    int s_reg[4], d[4];
    psp_vfpu_regs(vs, 1, s_reg);
    const float arg = psp_cpu.v[s_reg[0]];

    const unsigned cl = imm & 3;
    const unsigned sl = (imm >> 2) & 3;

    float s = sinf(arg * 1.5707963267948966f);
    const float c = cosf(arg * 1.5707963267948966f);
    if (imm & 0x10) s = -s;

    const int n = psp_vfpu_regs(vd, size, d);
    float out[4];
    for (int i = 0; i < n; i++) {
        float r;
        if (cl == sl) r = ((unsigned)i == cl) ? c : s;
        else          r = ((unsigned)i == cl) ? c
                        : ((unsigned)i == sl) ? s : 0.0f;
        out[i] = r;
    }
    write_dst(vd, size, out);
    eat_prefixes();
}

void psp_vmscl(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    float m[4][4];
    int t[4];
    matrix_read(vs, size, m);
    psp_vfpu_regs(vt, 1, t);
    const float k = psp_cpu.v[t[0]];
    /* vscl on every column, so vscl's special values. Only ordinary values
     * were probed here (step 115). */
    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++)
            m[c][r] = vout(vin(m[c][r]) * vin(k), sign_of(m[c][r]) ^ sign_of(k));
    matrix_write(vd, size, m);
    eat_prefixes();
}

/* Transform a vector by a matrix: vd[i] = sum over k of M[i][k] * v[k].
 *
 * Output lane i selects the matrix *column* i, so the result is the transpose
 * of the column-major product this used to compute. Unlike vmmul the encoding
 * does not set the transpose bit here -- vtfm's vs comes through as 0x1C, not
 * 0x3C -- so there is nothing to cancel it, and the two ops end up indexing
 * their matrix operand the same way for different reasons.
 *
 * `homogeneous` is the vhtfm form: the vector supplies one element fewer than
 * the matrix order and an implicit 1 fills the last, which is how a 4x4 with
 * translation applies to a 3-vector. There is no separate opcode for it -- it
 * is vtfm with the vector one size smaller than the instruction's own, so the
 * caller has to work it out from both and cannot recover it from `size`. */
void psp_vtfm(uint32_t vd, uint32_t vs, uint32_t vt, int size, int homogeneous) {
    float m[4][4];
    int d[4], t[4];
    matrix_read(vs, size, m);
    psp_vfpu_regs(vt, size, t);
    psp_vfpu_regs(vd, size, d);

    float in[4], out[4];
    for (int i = 0; i < size; i++) in[i] = psp_cpu.v[t[i]];

    /* Each lane is one pass through the dot-product unit, the implicit 1 of
     * the homogeneous form included, as vhdp does it. */
    const int n = homogeneous ? size - 1 : size;
    for (int r = 0; r < size; r++) {
        float a[4] = { 0.0f, 0.0f, 0.0f, 0.0f }, b[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        for (int c = 0; c < n; c++) { a[c] = m[r][c]; b[c] = in[c]; }
        if (homogeneous) { a[n] = m[r][size - 1]; b[n] = 1.0f; }
        out[r] = psp_vfpu_dot(a, b);
    }
    /* vd may be one of the sources, so write only after the whole result is
     * computed. */
    for (int r = 0; r < size; r++) psp_cpu.v[d[r]] = out[r];
    eat_prefixes();
}

/* Matrix product. `out[c][r] = sum over k of vs[r][k] * vt[c][k]`.
 *
 * The vs index looks transposed and is. vmmul reads its first operand *twice*
 * transposed and the two cancel: the assembler sets the transpose bit in the
 * encoding -- `vmmul.q M200, M000, M100` encodes vs as 0x20, not 0x00 -- and
 * the hardware then indexes that operand by [output row][summation] rather
 * than [summation][output row]. Honour only the encoded bit, as this did, and
 * the result is the product of the *transpose* of vs: for the pspautotests
 * matrix case, 7621 where hardware gives 1585.
 *
 * Verified against cpu/vfpu/matrix.expected, which is real-hardware output.
 * It replaces a note here saying the orientation was unverified because the
 * identity and composition tests hold for the transposed convention too. They
 * do; that is exactly why it needed a test that does not. */
void psp_vmmul(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    /* Zeroed because `size` is not provably <= 4 to the compiler, and a
     * maybe-uninitialised warning on every build hides the ones that matter. */
    float a[4][4] = {{0}}, b[4][4] = {{0}}, out[4][4] = {{0}};
    matrix_read(vs, size, a);
    matrix_read(vt, size, b);

    /* Every element is a dot product, rounded once: vfpuprobe steps 107 and
     * 110 (fw 6.60) give 43055555 where a running float sum gave 43055556. */
    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++) {
            float x[4] = { 0.0f, 0.0f, 0.0f, 0.0f }, y[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            for (int k = 0; k < size; k++) { x[k] = a[r][k]; y[k] = b[c][k]; }
            out[c][r] = psp_vfpu_dot(x, y);
        }
    matrix_write(vd, size, out);
    eat_prefixes();
}

/* Compare, writing one condition bit per lane plus the any/all summary bits
 * that vcmov and the bvt/bvf branches read. */
/* ---- the comparison ring ------------------------------------------------- */

#define VCMP_RING 24

typedef struct {
    int      is_dot;                 /* a vdot record rather than a vcmp     */
    uint32_t cond, vs, vt, vd, cc_before, cc_after;
    int      size, lanes;
    float    s[4], t[4], result;
    uint64_t seq;
} vcmp_rec;

static vcmp_rec g_vcmp[VCMP_RING];
static uint64_t g_vcmp_n;
static int      g_vcmp_on;

void psp_vfpu_cmp_record(int enable) { g_vcmp_on = enable; }

void psp_vfpu_note_dot(uint32_t vd, uint32_t vs, uint32_t vt,
                       const float *sv, const float *tv, float result) {
    if (!g_vcmp_on) return;
    vcmp_rec *r = &g_vcmp[g_vcmp_n % VCMP_RING];
    memset(r, 0, sizeof *r);
    r->seq = g_vcmp_n++;
    r->is_dot = 1;
    r->vd = vd; r->vs = vs; r->vt = vt; r->lanes = 4;
    r->result = result;
    for (int i = 0; i < 4; i++) { r->s[i] = sv[i]; r->t[i] = tv[i]; }
}

static const char *VCMP_NAME[16] = {
    "FL","EQ","LT","LE","TR","NE","GE","GT",
    "EZ","EN","EI","ES","NZ","NN","NI","NS"
};

void psp_vfpu_dump_cmps(FILE *out) {
    if (!g_vcmp_on) return;

    /* How much of the register file nothing has written.
     *
     * psp_cpu_reset_fp fills the VFPU with 0x7F800001, which is what the
     * hardware powers on holding, and arithmetic quiets it to 0x7FC00001. A
     * register still carrying either has never been written this run -- so a
     * NaN reaching a comparison is not necessarily a NaN the game computed,
     * and the difference decides whether to look at the arithmetic or at
     * whatever was supposed to load the register. */
    int untouched = 0;
    for (int i = 0; i < 128; i++) {
        const uint32_t b = psp_f32_to_bits(psp_cpu.v[i]);
        if (b == 0x7F800001u || b == 0x7FC00001u) untouched++;
    }
    fprintf(out, "  vfpu file: %d of 128 registers still hold the power-on "
                 "NaN (0x7F800001, quieted 0x7FC00001)\n", untouched);

    /* The whole file, in the layout vfpu.h describes -- eight 4x4 matrices,
     * element (m, row, col) at v[m*4 + col*32 + row].
     *
     * This is what PSPRECOMP_VDUMP gives, except that VDUMP hangs off the
     * interpreter's step loop and so cannot see a recompiled run -- and a bug
     * that only appears several minutes into real gameplay is not reachable by
     * the interpreter. Printed once, at the fault: the next instruction
     * destroys it.
     *
     * Printed as matrices rather than as a flat array on purpose. A flat dump
     * indexed 0..127 invites reading slot 107 as "register v107", and the two
     * are not the same thing -- that mistake turns a register nothing wrote
     * into a wrong-destination bug that was never there. */
    for (int m = 0; m < 8; m++) {
        fprintf(out, "    M%d ", m);
        for (int row = 0; row < 4; row++) {
            fprintf(out, "[");
            for (int col = 0; col < 4; col++)
                fprintf(out, "%s%08X", col ? " " : "",
                        psp_f32_to_bits(psp_cpu.v[m * 4 + col * 32 + row]));
            fprintf(out, "]");
        }
        fprintf(out, "\n");
    }

    if (!g_vcmp_n) { fprintf(out, "  (no VFPU comparison recorded)\n"); return; }

    const uint64_t n = g_vcmp_n < VCMP_RING ? g_vcmp_n : VCMP_RING;
    fprintf(out, "  last %llu VFPU comparison(s), newest first "
                 "(cc bits: 0-3 per lane, 4 any, 5 all):\n",
            (unsigned long long)n);
    for (uint64_t i = 0; i < n; i++) {
        const vcmp_rec *r = &g_vcmp[(g_vcmp_n - 1 - i) % VCMP_RING];
        if (r->is_dot) {
            fprintf(out, "    #%llu vdot v%u <- v%u,v%u  = %.7g [%08X]\n",
                    (unsigned long long)r->seq, r->vd, r->vs, r->vt,
                    (double)r->result, psp_f32_to_bits(r->result));
            for (int k = 0; k < 4; k++)
                fprintf(out, "        lane %d:  %-14.7g [%08X] . %-14.7g [%08X]\n",
                        k, (double)r->s[k], psp_f32_to_bits(r->s[k]),
                        (double)r->t[k], psp_f32_to_bits(r->t[k]));
            continue;
        }
        fprintf(out, "    #%llu vcmp.%s %-2s v%u,v%u  cc %02X -> %02X\n",
                (unsigned long long)r->seq,
                r->size == 1 ? "s" : r->size == 2 ? "p" : r->size == 3 ? "t" : "q",
                VCMP_NAME[r->cond & 0xF], r->vs, r->vt,
                r->cc_before, r->cc_after);
        for (int k = 0; k < r->lanes; k++) {
            /* Bits as well as the value. "nan" is not one answer: the VFPU
             * powers on filled with 0x7F800001, and arithmetic quiets that to
             * 0x7FC00001 -- so those two patterns mean "a register nothing
             * ever wrote", while 0x7FC00000 means a real 0/0 or inf-inf. The
             * decimal rendering collapses all three into the same word. */
            fprintf(out, "        lane %d:  %-14.7g [%08X] vs %-14.7g [%08X]  -> %d\n",
                    k, (double)r->s[k], psp_f32_to_bits(r->s[k]),
                    (double)r->t[k], psp_f32_to_bits(r->t[k]),
                    (r->cc_after >> k) & 1);
        }
    }
}

void psp_vcmp(uint32_t cond, uint32_t vs, uint32_t vt, int size) {
    float sv[4], tv[4];
    const int n = read_src(vs, size, PFXS, sv);
    read_src(vt, size, PFXT, tv);

    uint32_t cc = 0;
    int all = 1, any = 0;
    for (int i = 0; i < n; i++) {
        /* A denormal operand is a zero: 00000001 is EQ to 0 and to -0, EZ,
         * not NZ, and neither LT nor GT zero (steps 90-97 and 101). */
        const float a = vin(sv[i]), b = vin(tv[i]);
        int r;
        /* The upper eight conditions test the *first* operand's class rather
         * than comparing the two, and they had all been falling into the
         * default and answering 0. That is 576 lines of cpu/vfpu/vector, which
         * exercises every code against every interesting pair. */
        const int a_nan = (a != a);
        const int a_inf = !a_nan && (a > 3.4028235e38f || a < -3.4028235e38f);
        switch (cond & 0xF) {
        case 0:  r = 0;              break;   /* FL  */
        case 1:  r = (a == b);       break;   /* EQ  */
        case 2:  r = (a <  b);       break;   /* LT  */
        case 3:  r = (a <= b);       break;   /* LE  */
        case 4:  r = 1;              break;   /* TR  */
        case 5:  r = (a != b);       break;   /* NE  */
        case 6:  r = (a >= b);       break;   /* GE  */
        case 7:  r = (a >  b);       break;   /* GT  */
        case 8:  r = (a == 0.0f);    break;   /* EZ -- both zeroes count */
        case 9:  r = a_nan;          break;   /* EN  */
        case 10: r = a_inf;          break;   /* EI  */
        case 11: r = (a_nan || a_inf); break; /* ES  */
        case 12: r = (a != 0.0f);    break;   /* NZ  */
        case 13: r = !a_nan;         break;   /* NN  */
        case 14: r = !a_inf;         break;   /* NI  */
        default: r = !(a_nan || a_inf); break;/* NS  */
        }
        if (r) { cc |= 1u << i; any = 1; } else { all = 0; }
    }
    if (any) cc |= 1u << 4;
    if (all) cc |= 1u << 5;

    /* Only the bits this comparison is about are written: one per lane it
     * actually compared, plus the any/all pair. A `vcmp.t` leaves bit 3 alone,
     * and a program can rely on that -- cpu/vfpu/vector sets it with a quad
     * compare and then reads it back after a triple one. Overwriting the whole
     * register cleared it, which cost 25 lines and looked like a vcmov bug.
     *
     * Not yet confirmed on hardware: vfpuprobe step 105 read CC with an mfvc
     * straight after each vcmp.q/.t, and on firmware 6.60 that read returns
     * the *previous* CC, so the step measured the pipeline, not this rule. */
    const uint32_t affected = (1u << 4) | (1u << 5) | ((1u << n) - 1u);
    const uint32_t before = psp_cpu.vfpu_cc;
    psp_cpu.vfpu_cc = (psp_cpu.vfpu_cc & ~affected) | (cc & affected);

    /* Recorded after the write, so the operands and the codes they produced
     * are one record rather than two things to line up by hand. The prefixes
     * are already applied to sv/tv here, which is the point: what the compare
     * saw, not what the instruction named. */
    if (g_vcmp_on) {
        vcmp_rec *r = &g_vcmp[g_vcmp_n % VCMP_RING];
        /* Cleared, not just assigned: the ring is shared with vdot, and a slot
         * that still said is_dot printed this comparison's operands beside the
         * previous dot product's result. That read as a dot of two zero
         * vectors returning -1.997, which is impossible and cost a detour. */
        memset(r, 0, sizeof *r);
        r->seq = g_vcmp_n++;
        r->cond = cond; r->vs = vs; r->vt = vt; r->size = size; r->lanes = n;
        r->cc_before = before; r->cc_after = psp_cpu.vfpu_cc;
        for (int i = 0; i < 4; i++) { r->s[i] = i < n ? sv[i] : 0.0f;
                                      r->t[i] = i < n ? tv[i] : 0.0f; }
    }
    eat_prefixes();
}

/* viim / vfim -- write a single lane from an immediate encoded in the
 * instruction. No prefix rewrites it: there is no source operand, and the
 * destination prefix does not apply either. It still consumes them, because
 * every VFPU instruction does. */
void psp_vimm(uint32_t vd, float value) {
    int d[4];
    psp_vfpu_regs(vd, 1, d);
    psp_cpu.v[d[0]] = value;
    eat_prefixes();
}
