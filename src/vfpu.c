/* psprecomp — the VFPU. See include/psprecomp/vfpu.h. */

#include "psprecomp/vfpu.h"
#include "psprecomp/recomp_rt.h"

#include <math.h>

#include <stdio.h>
#include <string.h>

static uint32_t g_prefix[3];      /* vpfxs, vpfxt, vpfxd */
static int      g_prefix_set[3];
static uint64_t g_traps;

void psp_vfpu_reset(void) {
    memset(g_prefix, 0, sizeof g_prefix);
    memset(g_prefix_set, 0, sizeof g_prefix_set);
    g_traps = 0;
}

void psp_vfpu_set_prefix(int which, uint32_t value) {
    if (which < 0 || which > 2) return;
    g_prefix[which] = value;
    g_prefix_set[which] = 1;
}

int psp_vfpu_prefix_pending(void) {
    return g_prefix_set[0] || g_prefix_set[1] || g_prefix_set[2];
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

/* Consume the pending prefixes. Returns 1 if it is safe to compute, 0 if a
 * prefix was pending -- in which case the caller must trap rather than produce
 * a number that ignores it. */
static int take_prefixes(uint32_t addr, const char *what) {
    if (!psp_vfpu_prefix_pending()) return 1;
    memset(g_prefix_set, 0, sizeof g_prefix_set);
    psp_vfpu_unimplemented(addr, what);
    return 0;
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

/* ---- arithmetic ---------------------------------------------------------- */

#define BINOP(name, expr)                                                    \
    void psp_##name(uint32_t vd, uint32_t vs, uint32_t vt, int size) {       \
        if (!take_prefixes(psp_cpu.pc, #name)) return;                       \
        int d[4], s[4], t[4];                                                \
        int n = psp_vfpu_regs(vd, size, d);                                  \
        psp_vfpu_regs(vs, size, s);                                          \
        psp_vfpu_regs(vt, size, t);                                          \
        /* Read every source before writing any destination: vd may alias vs \
         * or vt, and a lane-by-lane read/write would then feed results back  \
         * into later lanes. */                                              \
        float out[4];                                                        \
        for (int i = 0; i < n; i++) {                                        \
            float a = psp_cpu.v[s[i]], b = psp_cpu.v[t[i]];                  \
            out[i] = (expr);                                                 \
        }                                                                    \
        for (int i = 0; i < n; i++) psp_cpu.v[d[i]] = out[i];                \
    }

BINOP(vadd, a + b)
BINOP(vsub, a - b)
BINOP(vmul, a * b)
BINOP(vdiv, a / b)
BINOP(vmin, a < b ? a : b)
BINOP(vmax, a > b ? a : b)

/* Dot product: sums all lanes into a single destination lane. */
void psp_vdot(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    if (!take_prefixes(psp_cpu.pc, "vdot")) return;
    int d[4], s[4], t[4];
    psp_vfpu_regs(vd, 1, d);
    int n = psp_vfpu_regs(vs, size, s);
    psp_vfpu_regs(vt, size, t);

    float sum = 0.0f;
    for (int i = 0; i < n; i++) sum += psp_cpu.v[s[i]] * psp_cpu.v[t[i]];
    psp_cpu.v[d[0]] = sum;
}

/* Scale: every lane of vs multiplied by the scalar in vt. */
void psp_vscl(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    if (!take_prefixes(psp_cpu.pc, "vscl")) return;
    int d[4], s[4], t[4];
    int n = psp_vfpu_regs(vd, size, d);
    psp_vfpu_regs(vs, size, s);
    psp_vfpu_regs(vt, 1, t);

    const float k = psp_cpu.v[t[0]];
    float out[4];
    for (int i = 0; i < n; i++) out[i] = psp_cpu.v[s[i]] * k;
    for (int i = 0; i < n; i++) psp_cpu.v[d[i]] = out[i];
}

/* ---- unary element-wise ops (VFPU4) -------------------------------------- */

static float sat0(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
static float sat1(float v) { return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v); }

void psp_vunary(int op, uint32_t vd, uint32_t vs, int size) {
    if (!take_prefixes(psp_cpu.pc, "vunary")) return;

    int d[4], s[4];
    int n = psp_vfpu_regs(vd, size, d);
    psp_vfpu_regs(vs, size, s);

    float out[4];
    for (int i = 0; i < n; i++) {
        const float a = psp_cpu.v[s[i]];
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
        /* Conversions move between the float and integer *interpretations* of
         * a vector register; the bits are reinterpreted, not just cast. */
        case PSP_VU_F2IZ: r = psp_bits_to_f32((uint32_t)(int32_t)a); break;
        case PSP_VU_I2F:  r = (float)(int32_t)psp_f32_to_bits(a);    break;
        default:          psp_vfpu_unimplemented(psp_cpu.pc, "vunary"); return;
        }
        out[i] = r;
    }
    for (int i = 0; i < n; i++) psp_cpu.v[d[i]] = out[i];
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
    if (!take_prefixes(psp_cpu.pc, "vmidt")) return;
    int cols[4][4];
    matrix_cols(vd, size, cols);
    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++)
            psp_cpu.v[cols[c][r]] = (c == r) ? 1.0f : 0.0f;
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
    if (!take_prefixes(psp_cpu.pc, "vidt")) return;
    int d[4];
    int n = psp_vfpu_regs(vd, size, d);
    const int one = (int)((vd >> 6) & 3);
    for (int i = 0; i < n; i++) psp_cpu.v[d[i]] = (i == one) ? 1.0f : 0.0f;
}

/* vcst -- load a constant from the VFPU's built-in table.
 *
 * The index is in the vs field. Values follow the hardware table; index 0 is
 * zero and anything past the end reads as zero rather than as garbage. */
void psp_vcst(uint32_t vd, uint32_t which, int size) {
    if (!take_prefixes(psp_cpu.pc, "vcst")) return;

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
    int d[4];
    int n = psp_vfpu_regs(vd, size, d);
    for (int i = 0; i < n; i++) psp_cpu.v[d[i]] = k;
}

void psp_vmzero(uint32_t vd, int size) {
    if (!take_prefixes(psp_cpu.pc, "vmzero")) return;
    int cols[4][4];
    matrix_cols(vd, size, cols);
    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++) psp_cpu.v[cols[c][r]] = 0.0f;
}

void psp_vmone(uint32_t vd, int size) {
    if (!take_prefixes(psp_cpu.pc, "vmone")) return;
    int cols[4][4];
    matrix_cols(vd, size, cols);
    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++) psp_cpu.v[cols[c][r]] = 1.0f;
}

void psp_vmmov(uint32_t vd, uint32_t vs, int size) {
    if (!take_prefixes(psp_cpu.pc, "vmmov")) return;
    int dc[4][4], sc[4][4];
    matrix_cols(vd, size, dc);
    matrix_cols(vs, size, sc);
    float tmp[4][4];
    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++) tmp[c][r] = psp_cpu.v[sc[c][r]];
    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++) psp_cpu.v[dc[c][r]] = tmp[c][r];
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
    if (!take_prefixes(psp_cpu.pc, "vrot")) return;

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
    for (int i = 0; i < n; i++) {
        float r;
        if (cl == sl) r = ((unsigned)i == cl) ? c : s;
        else          r = ((unsigned)i == cl) ? c
                        : ((unsigned)i == sl) ? s : 0.0f;
        psp_cpu.v[d[i]] = r;
    }
}

void psp_vmscl(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    if (!take_prefixes(psp_cpu.pc, "vmscl")) return;
    float m[4][4];
    int t[4];
    matrix_read(vs, size, m);
    psp_vfpu_regs(vt, 1, t);
    const float k = psp_cpu.v[t[0]];
    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++) m[c][r] *= k;
    matrix_write(vd, size, m);
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
    if (!take_prefixes(psp_cpu.pc, "vtfm")) return;
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
    if (!take_prefixes(psp_cpu.pc, "vmmul")) return;
    float a[4][4], b[4][4], out[4][4];
    matrix_read(vs, size, a);
    matrix_read(vt, size, b);

    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++) {
            float sum = 0.0f;
            for (int k = 0; k < size; k++) sum += a[r][k] * b[c][k];
            out[c][r] = sum;
        }
    matrix_write(vd, size, out);
}

/* Compare, writing one condition bit per lane plus the any/all summary bits
 * that vcmov and the bvt/bvf branches read. */
void psp_vcmp(uint32_t cond, uint32_t vs, uint32_t vt, int size) {
    if (!take_prefixes(psp_cpu.pc, "vcmp")) return;
    int s[4], t[4];
    int n = psp_vfpu_regs(vs, size, s);
    psp_vfpu_regs(vt, size, t);

    uint32_t cc = 0;
    int all = 1, any = 0;
    for (int i = 0; i < n; i++) {
        float a = psp_cpu.v[s[i]], b = psp_cpu.v[t[i]];
        int r;
        switch (cond & 0xF) {
        case 0:  r = 0;              break;   /* FL  */
        case 1:  r = (a == b);       break;   /* EQ  */
        case 2:  r = (a <  b);       break;   /* LT  */
        case 3:  r = (a <= b);       break;   /* LE  */
        case 4:  r = 1;              break;   /* TR  */
        case 5:  r = (a != b);       break;   /* NE  */
        case 6:  r = (a >= b);       break;   /* GE  */
        case 7:  r = (a >  b);       break;   /* GT  */
        default: r = 0;              break;
        }
        if (r) { cc |= 1u << i; any = 1; } else { all = 0; }
    }
    if (any) cc |= 1u << 4;
    if (all) cc |= 1u << 5;
    psp_cpu.vfpu_cc = cc;
}

/* viim / vfim -- write a single lane from an immediate encoded in the
 * instruction. No prefixes apply: there is no source operand to rewrite. */
void psp_vimm(uint32_t vd, float value) {
    int d[4];
    psp_vfpu_regs(vd, 1, d);
    psp_cpu.v[d[0]] = value;
}
