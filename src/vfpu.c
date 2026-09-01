/* psprecomp — the VFPU. See include/psprecomp/vfpu.h. */

#include "psprecomp/vfpu.h"
#include "psprecomp/recomp_rt.h"

#include <math.h>

#include <stdio.h>
#include <string.h>

static uint32_t g_prefix[3];      /* vpfxs, vpfxt, vpfxd */
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
    g_prefix[0] = g_prefix[1] = PFX_ST_NONE;
    g_prefix[2] = PFX_D_NONE;
    g_traps = 0;
}

void psp_vfpu_set_prefix(int which, uint32_t value) {
    if (which < 0 || which > 2) return;
    g_prefix[which] = value;
}

int psp_vfpu_prefix_pending(void) {
    return g_prefix[0] != PFX_ST_NONE || g_prefix[1] != PFX_ST_NONE
        || g_prefix[2] != PFX_D_NONE;
}

/* A prefix lasts exactly one instruction.
 *
 * Every VFPU op consumes all three, whether or not it uses them: a matrix op
 * ignores a pending swizzle but must still clear it, or it would be applied to
 * whatever came next instead. */
static void eat_prefixes(void) {
    g_prefix[0] = g_prefix[1] = PFX_ST_NONE;
    g_prefix[2] = PFX_D_NONE;
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
    const uint32_t pfx = g_prefix[2];

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

/* ---- VFPU control registers ----------------------------------------------- */

/* Everything past the prefixes and the condition codes: the revision word and
 * the random-number state. Kept together here because nothing reads them yet
 * and giving each a home of its own would be inventing structure. */
static uint32_t g_vfpu_ctrl_rest[16];

uint32_t psp_mfvc(int index) {
    switch (index) {
    case 0: case 1: case 2: return g_prefix[index];
    case 3:                 return psp_cpu.vfpu_cc;
    default:
        return (index >= 0 && index < 16) ? g_vfpu_ctrl_rest[index] : 0;
    }
}

void psp_mtvc(int index, uint32_t value) {
    switch (index) {
    /* Writing a prefix here is the same as executing vpfxs/vpfxt/vpfxd: the
     * next VFPU op consumes it and clears it. Storing it anywhere else would
     * make the two views of the same register disagree. */
    case 0: case 1: case 2: g_prefix[index] = value & 0xFFFFFFu; break;
    case 3:                 psp_cpu.vfpu_cc = value;            break;
    default:
        if (index >= 0 && index < 16) g_vfpu_ctrl_rest[index] = value;
        break;
    }
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
    float sv[4] = { 0.0f, 0.0f, 0.0f, 0.0f }, out[4];
    read_src(vs, size, g_prefix[0], sv);

    float sum = 0.0f;
    for (int i = 0; i < 4; i++) sum += sv[i] * k;
    out[0] = sum;
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
    const int n = read_src(vs, size, g_prefix[0], s);
    /* The destination is read as the second operand, T prefix and all: a lane
     * that is not moved keeps its old value, so this is a read-modify-write. */
    read_src(vd, size, g_prefix[1], d);

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
 * the cross product and a quad is the quaternion product. Hardware expresses
 * both as dot products against a forced swizzle-and-negate of t; written out
 * here as the products themselves, which is what they are. */
void psp_vcrsp(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    float s[4] = { 0.0f, 0.0f, 0.0f, 0.0f }, t[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    read_src(vs, size, g_prefix[0], s);
    read_src(vt, size, g_prefix[1], t);

    float d[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    if (size == 4) {                                     /* vqmul.q */
        d[0] =  s[0]*t[3] + s[1]*t[2] - s[2]*t[1] + s[3]*t[0];
        d[1] = -s[0]*t[2] + s[1]*t[3] + s[2]*t[0] + s[3]*t[1];
        d[2] =  s[0]*t[1] - s[1]*t[0] + s[2]*t[3] + s[3]*t[2];
        d[3] = -s[0]*t[0] - s[1]*t[1] - s[2]*t[2] + s[3]*t[3];
    } else {                                             /* vcrsp.t */
        d[0] = s[1]*t[2] - s[2]*t[1];
        d[1] = s[2]*t[0] - s[0]*t[2];
        /* The third lane comes out of the same forced-swizzle dot as the other
         * two, which for a triple (t[3] and s[3] zero) is the cross term.
         *
         * Infinities are flushed to zero first, and only for this lane. That
         * looks arbitrary and is what the hardware does: inf * 0 in the dot
         * would be a NaN, and the PSP answers with the finite part instead.
         * Nine lines of cpu/vfpu/vector turn on it. */
        float fs[4], ft[4];
        for (int i = 0; i < 4; i++) {
            fs[i] = (s[i] >  3.4028235e38f || s[i] < -3.4028235e38f) ? 0.0f : s[i];
            ft[i] = (t[i] >  3.4028235e38f || t[i] < -3.4028235e38f) ? 0.0f : t[i];
        }
        d[2] = fs[0]*ft[1] - fs[1]*ft[0] + fs[2]*ft[3] + fs[3]*ft[2];
    }

    write_dst(vd, size, d);
    eat_prefixes();
}

void psp_vhdp(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    float sv[4] = { 0.0f, 0.0f, 0.0f, 0.0f }, tv[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    const int n = read_src(vs, size, g_prefix[0], sv);
    read_src(vt, size, g_prefix[1], tv);

    /* The last lane of the source is a forced 1: that is the whole difference
     * from vdot, and it is what makes this the homogeneous form. */
    sv[n - 1] = 1.0f;
    float sum = 0.0f;
    for (int i = 0; i < n; i++) sum += sv[i] * tv[i];

    float out[4] = { sum, 0.0f, 0.0f, 0.0f };
    write_dst(vd, 1, out);
    eat_prefixes();
}

void psp_vcrs(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    float sv[4] = { 0.0f, 0.0f, 0.0f, 0.0f }, tv[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    read_src(vs, size, g_prefix[0], sv);
    read_src(vt, size, g_prefix[1], tv);

    /* s is forced to yzx and t to zxy, then multiplied lane by lane. There is
     * no subtraction: this is half a cross product, and a full one is two of
     * these with a vsub between. */
    float out[4];
    out[0] = sv[1] * tv[2];
    out[1] = sv[2] * tv[0];
    out[2] = sv[0] * tv[1];
    out[3] = sv[3] * tv[3];

    write_dst(vd, size, out);
    eat_prefixes();
}

void psp_vdet(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    float sv[4] = { 0.0f, 0.0f, 0.0f, 0.0f }, tv[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    read_src(vs, size, g_prefix[0], sv);
    read_src(vt, size, g_prefix[1], tv);

    /* t's first two lanes are forced to yx, so the dot with s negated on lane
     * 1 is s0*t1 - s1*t0. Lanes beyond the pair contribute as they are, which
     * is why they are read at all -- normally they are zero. */
    const float d0 = sv[0] * tv[1] - sv[1] * tv[0] + sv[2] * tv[2] + sv[3] * tv[3];
    float out[4] = { d0, 0.0f, 0.0f, 0.0f };
    write_dst(vd, 1, out);
    eat_prefixes();
}

/* ---- comparisons that produce values ------------------------------------- */

void psp_vcmp_val(uint32_t vd, uint32_t vs, uint32_t vt, int kind, int size) {
    float sv[4] = { 0.0f, 0.0f, 0.0f, 0.0f }, tv[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    const int n = read_src(vs, size, g_prefix[0], sv);
    read_src(vt, size, g_prefix[1], tv);

    float d[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    for (int i = 0; i < n; i++) {
        if (kind == 0) {                                  /* vscmp: -1, 0, 1 */
            const float a = sv[i] - tv[i];
            if (a != a) {
                /* A NaN difference means at least one side is NaN or the two
                 * are opposite infinities. The hardware still orders them, by
                 * signed magnitude -- the same treatment vmin/vmax give. */
                const int32_t si = (int32_t)psp_f32_to_bits(sv[i]);
                const int32_t ti = (int32_t)psp_f32_to_bits(tv[i]);
                const int32_t sm = si & 0x7FFFFFFF, tm = ti & 0x7FFFFFFF;
                const int32_t b = (si < 0 ? -sm : sm) - (ti < 0 ? -tm : tm);
                d[i] = (float)((0 < b) - (b < 0));
            } else {
                d[i] = (float)((0.0f < a) - (a < 0.0f));
            }
        } else {
            /* A NaN on either side is false, not "unordered": both of these
             * answer 0.0 rather than propagating it. */
            const int nan = (sv[i] != sv[i]) || (tv[i] != tv[i]);
            if (nan)              d[i] = 0.0f;
            else if (kind == 1)   d[i] = (sv[i] >= tv[i]) ? 1.0f : 0.0f;
            else                  d[i] = (sv[i] <  tv[i]) ? 1.0f : 0.0f;
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
    read_src(vs, size, g_prefix[0], sv);

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
    read_src(vs, size, g_prefix[0], sv);
    read_src(vt, size, g_prefix[1], tv);

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
void psp_vfpu9(uint32_t vd, uint32_t vs, int kind, int size) {
    float s[4] = { 0.0f, 0.0f, 0.0f, 0.0f }, d[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    const int n = read_src(vs, size, g_prefix[0], s);

    /* The two swizzles the sorts and butterflies use. */
    const float yxwz[4] = { s[1], s[0], s[3], s[2] };
    const float wzyx[4] = { s[3], s[2], s[1], s[0] };
    const float zwxy[4] = { s[2], s[3], s[0], s[1] };

    #define MIN(a,b) ((a) < (b) ? (a) : (b))
    #define MAX(a,b) ((a) > (b) ? (a) : (b))
    switch (kind) {
    case 0:                                              /* vsrt1 */
        d[0] = MIN(s[0], yxwz[0]); d[1] = MAX(s[1], yxwz[1]);
        d[2] = MIN(s[2], yxwz[2]); d[3] = MAX(s[3], yxwz[3]);
        break;
    case 1:                                              /* vsrt2 */
        d[0] = MIN(s[0], wzyx[0]); d[1] = MIN(s[1], wzyx[1]);
        d[2] = MAX(s[2], wzyx[2]); d[3] = MAX(s[3], wzyx[3]);
        break;
    case 8:                                              /* vsrt3 */
        d[0] = MAX(s[0], yxwz[0]); d[1] = MIN(s[1], yxwz[1]);
        d[2] = MAX(s[2], yxwz[2]); d[3] = MIN(s[3], yxwz[3]);
        break;
    case 9:                                              /* vsrt4 */
        d[0] = MAX(s[0], wzyx[0]); d[1] = MAX(s[1], wzyx[1]);
        d[2] = MIN(s[2], wzyx[2]); d[3] = MIN(s[3], wzyx[3]);
        break;
    case 2:                                              /* vbfy1 */
        d[0] = s[0] + yxwz[0]; d[1] = -s[1] + yxwz[1];
        d[2] = s[2] + yxwz[2]; d[3] = -s[3] + yxwz[3];
        break;
    case 3:                                              /* vbfy2 */
        d[0] = s[0] + zwxy[0]; d[1] =  s[1] + zwxy[1];
        d[2] = -s[2] + zwxy[2]; d[3] = -s[3] + zwxy[3];
        break;
    case 4:                                              /* vocp: 1 - s */
        for (int i = 0; i < 4; i++) d[i] = 1.0f - s[i];
        break;
    case 10:                                             /* vsgn */
        for (int i = 0; i < n; i++) {
            /* Through the bits, so that a NaN difference does not compare
             * equal to zero and both zeroes give exactly +0. */
            const uint32_t b = psp_f32_to_bits(s[i] - 0.0f);
            d[i] = (b == 0 || b == 0x80000000u) ? 0.0f
                 : (b >> 31) == 0               ? 1.0f : -1.0f;
        }
        break;
    default:
        psp_vfpu_unimplemented(psp_cpu.pc, "vfpu9");
        eat_prefixes();
        return;
    }
    #undef MIN
    #undef MAX

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
    const int n = read_src(vs, size, g_prefix[0], sv);
    const double mult = (double)(1u << (scale & 0x1F));

    uint32_t d[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < n; i++) {
        const float f = sv[i];
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
        if (!((g_prefix[2] >> (8 + i)) & 1))
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
    read_src(vs, size, g_prefix[0], sv);
    const int oz = (size <= 2) ? 1 : 2;
    uint32_t d[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < oz; i++)
        d[i] = (uint32_t)psp_f32_to_half(sv[i * 2]) |
               ((uint32_t)psp_f32_to_half(sv[i * 2 + 1]) << 16);
    write_bits(vd, oz, d);
}

/* ---- arithmetic ---------------------------------------------------------- */

#define BINOP(name, expr)                                                    \
    void psp_##name(uint32_t vd, uint32_t vs, uint32_t vt, int size) {       \
        /* Read every source before writing any destination: vd may alias vs \
         * or vt, and a lane-by-lane read/write would then feed results back  \
         * into later lanes. read_src copies, so this holds for free. */     \
        float sv[4], tv[4], out[4];                                          \
        const int n = read_src(vs, size, g_prefix[0], sv);                   \
        read_src(vt, size, g_prefix[1], tv);                                 \
        for (int i = 0; i < n; i++) {                                        \
            float a = sv[i], b = tv[i];                                      \
            out[i] = (expr);                                                 \
        }                                                                    \
        write_dst(vd, size, out);                                            \
        eat_prefixes();                                                      \
    }

BINOP(vadd, a + b)
BINOP(vsub, a - b)
BINOP(vmul, a * b)
BINOP(vdiv, a / b)
BINOP(vmin, a < b ? a : b)
BINOP(vmax, a > b ? a : b)

/* Dot product: sums all lanes into a single destination lane. */
void psp_vdot(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    float sv[4], tv[4], out[4];
    const int n = read_src(vs, size, g_prefix[0], sv);
    read_src(vt, size, g_prefix[1], tv);

    float sum = 0.0f;
    for (int i = 0; i < n; i++) sum += sv[i] * tv[i];
    out[0] = sum;
    write_dst(vd, 1, out);
    eat_prefixes();
}

/* Scale: every lane of vs multiplied by the scalar in vt. */
void psp_vscl(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    float sv[4], tv[4], out[4];
    const int n = read_src(vs, size, g_prefix[0], sv);
    read_src(vt, 1, g_prefix[1], tv);

    const float k = tv[0];
    for (int i = 0; i < n; i++) out[i] = sv[i] * k;
    write_dst(vd, size, out);
    eat_prefixes();
}

/* ---- unary element-wise ops (VFPU4) -------------------------------------- */

void psp_vunary(int op, uint32_t vd, uint32_t vs, int size) {
    float sv[4], out[4];
    const int n = read_src(vs, size, g_prefix[0], sv);

    for (int i = 0; i < n; i++) {
        const float a = sv[i];
        float r;
        switch (op) {
        case PSP_VU_MOV:  r = a;            break;
        case PSP_VU_ABS:  r = a < 0 ? -a : a; break;
        case PSP_VU_NEG:  r = -a;           break;
        case PSP_VU_ZERO: r = 0.0f;         break;
        case PSP_VU_ONE:  r = 1.0f;         break;
        case PSP_VU_RCP:  r = 1.0f / a;     break;
        case PSP_VU_NRCP: r = -1.0f / a;    break;
        case PSP_VU_RSQ:  r = 1.0f / psp_fsqrt(a); break;
        case PSP_VU_SQRT: r = psp_fsqrt(a); break;
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
 * Which element gets the 1 is encoded in the destination register number
 * rather than given as an operand, which is how one instruction builds any
 * basis vector. Bits 6-7 of vd select the position.
 *
 * This is matrix-setup code. Trapping it to a no-op leaves whatever was in the
 * register, so downstream geometry is built on a basis that is not a basis. */
void psp_vidt(uint32_t vd, int size) {
    int r[4];
    const int n = psp_vfpu_regs(vd, size, r);
    const int one = (int)((vd >> 6) & 3);
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
        0.31830989f,            /* 1/pi               */
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
    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++) m[c][r] *= k;
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

    const int n = homogeneous ? size - 1 : size;
    for (int r = 0; r < size; r++) {
        float sum = 0.0f;
        for (int c = 0; c < n; c++) sum += m[r][c] * in[c];
        if (homogeneous) sum += m[r][size - 1];
        out[r] = sum;
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

    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++) {
            float sum = 0.0f;
            for (int k = 0; k < size; k++) sum += a[r][k] * b[c][k];
            out[c][r] = sum;
        }
    matrix_write(vd, size, out);
    eat_prefixes();
}

/* Compare, writing one condition bit per lane plus the any/all summary bits
 * that vcmov and the bvt/bvf branches read. */
void psp_vcmp(uint32_t cond, uint32_t vs, uint32_t vt, int size) {
    float sv[4], tv[4];
    const int n = read_src(vs, size, g_prefix[0], sv);
    read_src(vt, size, g_prefix[1], tv);

    uint32_t cc = 0;
    int all = 1, any = 0;
    for (int i = 0; i < n; i++) {
        float a = sv[i], b = tv[i];
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
    psp_cpu.vfpu_cc = cc;
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
