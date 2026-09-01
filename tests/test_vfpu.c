/* VFPU tests — synthetic data only.
 *
 * Two things get pinned here. The first is register addressing, because the
 * layout is what makes a matrix row and column alias correctly and everything
 * else is built on it. The second is the operand prefixes, because nothing in
 * an arithmetic instruction's own encoding says a prefix is rewriting it --
 * a swizzle or a lane negation applied to the wrong operand, or not at all,
 * produces numbers that are wrong and entirely plausible.
 */

#include "psprecomp/vfpu.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

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

/* Relative tolerance, and not a strict inequality.
 *
 * An absolute 1e-5 is far below one ULP once past a few thousand, so
 * `want - 1e-5f` is just `want` and `got > want - 1e-5f` then *rejects*
 * exactly-equal values. Every check here used small numbers, so it went
 * unnoticed until the hardware-pinned vmmul values, which run to five digits,
 * failed while printing "got 1585.000000, want 1585.000000". */
#define CHECK_F(got, want, label)                                            \
    CHECK(fabsf((float)(got) - (float)(want))                                \
              <= 1e-5f * (1.0f + fabsf((float)(want))),                      \
          "%s: got %f, want %f", (label), (double)(got), (double)(want))

static void test_register_addressing(void) {
    int r[4];

    /* A quad in matrix 0, column 0 is four consecutive rows: indices
     * 0, 1, 2, 3 under the mtx*4 + col*32 + row layout. */
    int n = psp_vfpu_regs(0x00, 4, r);
    CHECK(n == 4, "a quad names four registers, got %d", n);
    CHECK(r[0] == 0 && r[1] == 1 && r[2] == 2 && r[3] == 3,
          "quad M000: got %d,%d,%d,%d", r[0], r[1], r[2], r[3]);

    /* A single names exactly one. */
    n = psp_vfpu_regs(0x00, 1, r);
    CHECK(n == 1 && r[0] == 0, "single names one register");

    /* The transpose bit (bit 5) walks the other axis. Column-major access of
     * the same matrix must land on a *different* set of registers -- if it did
     * not, transposing a matrix would be a no-op and every rotation would be
     * wrong. */
    int rowwise[4], colwise[4];
    psp_vfpu_regs(0x00, 4, rowwise);
    psp_vfpu_regs(0x20, 4, colwise);
    CHECK(memcmp(rowwise, colwise, sizeof rowwise) != 0,
          "transposed access reaches different registers");
    CHECK(colwise[0] == 0 && colwise[1] == 32 && colwise[2] == 64 && colwise[3] == 96,
          "transposed quad strides by column: got %d,%d,%d,%d",
          colwise[0], colwise[1], colwise[2], colwise[3]);

    /* Every index must stay inside the 128-register file, for every width and
     * every possible field value. */
    for (uint32_t vreg = 0; vreg < 128; vreg++) {
        for (int size = 1; size <= 4; size++) {
            int idx[4];
            int len = psp_vfpu_regs(vreg, size, idx);
            for (int i = 0; i < len; i++) {
                if (idx[i] < 0 || idx[i] >= 128) {
                    printf("FAIL vreg=0x%02X size=%d lane %d -> %d (out of range)\n",
                           vreg, size, i, idx[i]);
                    failures++;
                }
            }
        }
    }
}

static void test_load_store(void) {
    const uint32_t AT = 0x08860000u;

    /* Quad round-trip through guest memory. */
    float in[4] = { 1.5f, -2.25f, 3.75f, 0.5f };
    for (int i = 0; i < 4; i++) psp_write_f32(AT + (uint32_t)i * 4, in[i]);

    psp_lv_q(0x00, AT);
    int r[4];
    psp_vfpu_regs(0x00, 4, r);
    for (int i = 0; i < 4; i++) CHECK_F(psp_cpu.v[r[i]], in[i], "lv.q lane");

    psp_sv_q(0x00, AT + 64);
    for (int i = 0; i < 4; i++)
        CHECK_F(psp_read_f32(AT + 64 + (uint32_t)i * 4), in[i], "sv.q lane");

    /* Quad addresses are 16-byte aligned; a misaligned address is masked down
     * rather than faulting, which is what the hardware does. */
    psp_lv_q(0x04, AT + 7);
    int r2[4];
    psp_vfpu_regs(0x04, 4, r2);
    CHECK_F(psp_cpu.v[r2[0]], in[0], "misaligned lv.q masks the address");

    psp_lv_s(0x08, AT + 4);
    int r3[4];
    psp_vfpu_regs(0x08, 1, r3);
    CHECK_F(psp_cpu.v[r3[0]], in[1], "lv.s");
}

/* Load a quad register from an array, for the arithmetic tests. */
static void set_quad(uint32_t vreg, const float f[4]) {
    int r[4];
    psp_vfpu_regs(vreg, 4, r);
    for (int i = 0; i < 4; i++) psp_cpu.v[r[i]] = f[i];
}
static void get_quad(uint32_t vreg, float f[4]) {
    int r[4];
    psp_vfpu_regs(vreg, 4, r);
    for (int i = 0; i < 4; i++) f[i] = psp_cpu.v[r[i]];
}

static void test_arithmetic(void) {
    psp_vfpu_reset();

    const float a[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
    const float b[4] = { 5.0f, 6.0f, 7.0f, 8.0f };
    float out[4];

    /* Three different matrices so vd, vs and vt do not alias. */
    set_quad(0x00, a);
    set_quad(0x04, b);

    psp_vadd(0x08, 0x00, 0x04, 4);
    get_quad(0x08, out);
    for (int i = 0; i < 4; i++) CHECK_F(out[i], a[i] + b[i], "vadd lane");

    psp_vmul(0x08, 0x00, 0x04, 4);
    get_quad(0x08, out);
    for (int i = 0; i < 4; i++) CHECK_F(out[i], a[i] * b[i], "vmul lane");

    psp_vsub(0x08, 0x04, 0x00, 4);
    get_quad(0x08, out);
    for (int i = 0; i < 4; i++) CHECK_F(out[i], b[i] - a[i], "vsub lane");

    /* vdot collapses to one lane: 1*5 + 2*6 + 3*7 + 4*8 = 70. */
    psp_vdot(0x08, 0x00, 0x04, 4);
    int d[4];
    psp_vfpu_regs(0x08, 1, d);
    CHECK_F(psp_cpu.v[d[0]], 70.0f, "vdot");

    /* vscl multiplies every lane by a scalar. */
    int s[4];
    psp_vfpu_regs(0x0C, 1, s);
    psp_cpu.v[s[0]] = 3.0f;
    psp_vscl(0x10, 0x00, 0x0C, 4);
    get_quad(0x10, out);
    for (int i = 0; i < 4; i++) CHECK_F(out[i], a[i] * 3.0f, "vscl lane");

    /* Destination aliasing a source must still be correct: every source lane
     * has to be read before any destination lane is written. */
    set_quad(0x00, a);
    set_quad(0x04, b);
    psp_vadd(0x00, 0x00, 0x04, 4);
    get_quad(0x00, out);
    for (int i = 0; i < 4; i++) CHECK_F(out[i], a[i] + b[i], "vadd into its own source");
}

static void test_prefixes(void) {
    psp_vfpu_reset();

    const float a[4] = { 1.0f, -2.0f, 3.0f, -4.0f };
    const float ones[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    float out[4];

    /* The identity prefix is the state after a reset, and after every op. An
     * implementation that only ever saw 0xE4 would pass everything below this
     * point, so it is checked first and separately. */
    CHECK(!psp_vfpu_prefix_pending(), "reset leaves the identity prefix");

    set_quad(0x00, a);
    set_quad(0x04, ones);
    psp_vadd(0x08, 0x00, 0x04, 4);
    get_quad(0x08, out);
    for (int i = 0; i < 4; i++) CHECK_F(out[i], a[i], "no prefix, no rewrite");

    /* Swizzle. 0x1B is w,z,y,x -- selectors 3,2,1,0 packed two bits per lane,
     * lane 0 in the low bits -- so this reverses the operand. It also has to
     * *exchange* rather than duplicate, which is why the source is copied
     * before any lane is rewritten. */
    set_quad(0x04, ones);
    psp_vfpu_set_prefix(0, 0x1B);
    psp_vadd(0x08, 0x00, 0x04, 4);
    get_quad(0x08, out);
    for (int i = 0; i < 4; i++) CHECK_F(out[i], a[3 - i], "source swizzle reverses");
    CHECK(!psp_vfpu_prefix_pending(), "the op consumed the prefix");

    /* And it is one-shot: the same op again must see no prefix. */
    psp_vadd(0x08, 0x00, 0x04, 4);
    get_quad(0x08, out);
    for (int i = 0; i < 4; i++) CHECK_F(out[i], a[i], "a prefix lasts one instruction");

    /* Negate lane 1 only -- bits 16..19, one per lane. This is the whole of
     * what pspgl's glRotatef needs, and ignoring it silently produced an
     * identity matrix where a rotation belonged. */
    psp_vfpu_set_prefix(0, 0xE4 | (1u << 17));
    psp_vadd(0x08, 0x00, 0x04, 4);
    get_quad(0x08, out);
    for (int i = 0; i < 4; i++)
        CHECK_F(out[i], i == 1 ? -a[i] : a[i], "negate applies per lane");

    /* Absolute value, bits 8..11. */
    psp_vfpu_set_prefix(0, 0xE4 | 0xF00u);
    psp_vadd(0x08, 0x00, 0x04, 4);
    get_quad(0x08, out);
    for (int i = 0; i < 4; i++)
        CHECK_F(out[i], a[i] < 0 ? -a[i] : a[i], "abs applies per lane");

    /* Constants, bits 12..15: the lane's swizzle selector indexes a table
     * rather than the operand, and the abs bit picks the upper half of it.
     * Selector 1 with abs clear is 1.0; selector 1 with abs set is 1/3. */
    psp_vfpu_set_prefix(0, 0x1000u | 0x1u);          /* lane 0: const, sel 1 */
    psp_vadd(0x08, 0x00, 0x04, 4);
    get_quad(0x08, out);
    CHECK_F(out[0], 1.0f, "constant 1.0 substitutes for the operand");

    psp_vfpu_set_prefix(0, 0x1000u | 0x100u | 0x1u); /* + abs -> upper half */
    psp_vadd(0x08, 0x00, 0x04, 4);
    get_quad(0x08, out);
    CHECK_F(out[0], 1.0f / 3.0f, "the abs bit selects the second constant bank");

    /* The T prefix rewrites the second operand, independently of the first. */
    set_quad(0x04, a);
    psp_vfpu_set_prefix(1, 0x1B);
    psp_vsub(0x08, 0x00, 0x04, 4);
    get_quad(0x08, out);
    for (int i = 0; i < 4; i++)
        CHECK_F(out[i], a[i] - a[3 - i], "the T prefix rewrites vt alone");

    /* Destination saturation, two bits per lane: 1 clamps to [0,1], 3 to
     * [-1,1], 0 and 2 leave the value alone. */
    set_quad(0x00, a);
    set_quad(0x04, ones);
    psp_vfpu_set_prefix(2, 1u | (3u << 2));
    psp_vadd(0x08, 0x00, 0x04, 4);
    get_quad(0x08, out);
    CHECK_F(out[0],  1.0f, "lane 0 clamped to [0,1]");
    CHECK_F(out[1], -1.0f, "lane 1 clamped to [-1,1]");
    CHECK_F(out[2],  3.0f, "lane 2 unsaturated");

    /* The write mask, bits 8..11: a masked lane keeps what it held. */
    const float seed[4] = { 100.0f, 200.0f, 300.0f, 400.0f };
    set_quad(0x08, seed);
    psp_vfpu_set_prefix(2, 1u << 9);                 /* mask lane 1 */
    psp_vadd(0x08, 0x00, 0x04, 4);
    get_quad(0x08, out);
    CHECK_F(out[0], a[0],   "unmasked lane is written");
    CHECK_F(out[1], 200.0f, "masked lane keeps its old value");
    CHECK_F(out[2], a[2],   "unmasked lane is written");

    /* Every VFPU op consumes the prefixes, including ones that ignore them.
     * A matrix op that left a swizzle set would apply it to whatever came
     * next, which is a bug that only shows up two instructions later. */
    psp_vfpu_set_prefix(0, 0x1B);
    psp_vmidt(0x00, 4);
    CHECK(!psp_vfpu_prefix_pending(), "a matrix op consumes prefixes it ignores");
}

static void test_compare(void) {
    psp_vfpu_reset();

    const float a[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
    const float b[4] = { 1.0f, 9.0f, 0.0f, 4.0f };
    set_quad(0x00, a);
    set_quad(0x04, b);

    psp_vcmp(1 /* EQ */, 0x00, 0x04, 4);
    /* Lanes 0 and 3 are equal, so bits 0 and 3, plus "any" but not "all". */
    CHECK((psp_cpu.vfpu_cc & 0xF) == 0x9,
          "vcmp EQ per-lane bits: got 0x%X", psp_cpu.vfpu_cc & 0xF);
    CHECK(psp_cpu.vfpu_cc & (1u << 4), "the any-lane bit is set");
    CHECK(!(psp_cpu.vfpu_cc & (1u << 5)), "the all-lanes bit is not");

    psp_vcmp(1, 0x00, 0x00, 4);
    CHECK(psp_cpu.vfpu_cc & (1u << 5), "comparing a vector to itself sets all-lanes");
}

/* Read/write a whole matrix through the same addressing the ops use. */
static void set_matrix(uint32_t v, int n, const float m[4][4]) {
    const uint32_t mtx = (v >> 2) & 7;
    for (int c = 0; c < n; c++) {
        int r[4];
        psp_vfpu_regs((mtx << 2) | (uint32_t)c, n, r);
        for (int i = 0; i < n; i++) psp_cpu.v[r[i]] = m[c][i];
    }
}
static void get_matrix(uint32_t v, int n, float m[4][4]) {
    const uint32_t mtx = (v >> 2) & 7;
    for (int c = 0; c < n; c++) {
        int r[4];
        psp_vfpu_regs((mtx << 2) | (uint32_t)c, n, r);
        for (int i = 0; i < n; i++) m[c][i] = psp_cpu.v[r[i]];
    }
}

static void test_matrix_ops(void) {
    psp_vfpu_reset();
    float m[4][4], out[4][4];

    /* vmidt must produce a real identity: ones on the diagonal, zeros
     * elsewhere. This is what catches the column-addressing bug -- a helper
     * that walks rows while claiming to walk columns still writes four ones,
     * just in the wrong places, so only checking the off-diagonal zeros
     * distinguishes them. */
    psp_vmidt(0x00, 4);
    get_matrix(0x00, 4, out);
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            CHECK_F(out[c][r], (c == r) ? 1.0f : 0.0f, "vmidt element");

    psp_vmzero(0x04, 4);
    get_matrix(0x04, 4, out);
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) CHECK_F(out[c][r], 0.0f, "vmzero element");

    /* vmmov must copy every element, including off-diagonal ones -- a
     * diagonal-only copy would pass an identity round-trip. */
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) m[c][r] = (float)(c * 4 + r + 1);
    set_matrix(0x08, 4, m);
    psp_vmmov(0x0C, 0x08, 4);
    get_matrix(0x0C, 4, out);
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) CHECK_F(out[c][r], m[c][r], "vmmov element");

    /* Scaling is orientation-independent, so it can be checked outright. */
    int k[4];
    psp_vfpu_regs(0x40, 1, k);
    psp_cpu.v[k[0]] = 2.5f;
    psp_vmscl(0x10, 0x08, 0x40, 4);
    get_matrix(0x10, 4, out);
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) CHECK_F(out[c][r], m[c][r] * 2.5f, "vmscl element");
}

static void test_matrix_transform(void) {
    psp_vfpu_reset();

    /* Pinned to real hardware: pspautotests cpu/vfpu/matrix, `vmmul.q 1`.
     *
     * The inputs are that test's m1 and m2 as they sit in memory, and the
     * result is what a PSP prints. This is the check the old geometric
     * properties could not be: an identity test, and even a "vmmul and vtfm
     * agree" test, passes just as happily with both operands transposed. Those
     * held here for a long time while the orientation was wrong.
     *
     * vs carries the transpose bit because the assembler puts it there --
     * `vmmul.q M200, M000, M100` encodes vs as 0x20, not 0x00 -- and vmmul
     * then indexes that operand transposed again. Passing 0x08 here instead of
     * 0x28 would test something the hardware never executes. */
    static const float m1[4][4] = {   /* [row][col], as declared in memory */
        {  2,  3,  5,  7 }, { 11, 13, 17, 19 },
        { 23, 29, 31, 37 }, { 41, 43, 47, 53 },
    };
    static const float m2[4][4] = {
        {  59,  61,  67,  71 }, {  73,  79,  83,  89 },
        {  97, 101, 103, 107 }, { 109, 113, 127, 131 },
    };
    static const float want[4][4] = {
        {  1585,  1655,  1787,  1861 }, {  5318,  5562,  5980,  6246 },
        { 10514, 11006, 11840, 12378 }, { 15894, 16634, 17888, 18710 },
    };

    float a[4][4], b[4][4], out[4][4];
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) { a[c][r] = m1[r][c]; b[c][r] = m2[r][c]; }
    set_matrix(0x08, 4, a);               /* matrix 2 */
    set_matrix(0x10, 4, b);               /* matrix 4 */

    psp_vmmul(0x14, 0x28, 0x10, 4);       /* matrix 5 = 2 * 4, vs transposed */
    get_matrix(0x14, 4, out);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            CHECK_F(out[c][r], want[r][c], "vmmul matches hardware");

    /* vtfm of a basis vector is the matrix ROW, not the column: output lane i
     * reads m[i][k], so e_b selects element (col i, row b) across i. The
     * previous convention here claimed the column, which is its transpose. */
    for (int basis = 0; basis < 4; basis++) {
        int t[4], d[4];
        psp_vfpu_regs(0x40, 4, t);
        for (int i = 0; i < 4; i++) psp_cpu.v[t[i]] = (i == basis) ? 1.0f : 0.0f;

        psp_vtfm(0x44, 0x08, 0x40, 4, 0);
        psp_vfpu_regs(0x44, 4, d);
        for (int i = 0; i < 4; i++)
            CHECK_F(psp_cpu.v[d[i]], a[i][basis], "vtfm of a basis vector is that row");
    }

    /* Multiplying by the identity is a no-op in both positions. Weak on its
     * own -- kept because it still catches a mis-addressed operand. */
    psp_vmidt(0x00, 4);                   /* matrix 0 = I */
    psp_vmmul(0x0C, 0x28, 0x00, 4);       /* matrix 3 = A * I, vs transposed */
    get_matrix(0x0C, 4, out);
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) CHECK_F(out[c][r], a[c][r], "M * I == M");

    /* vmmul and vtfm agree. vtfm applies the transpose of its matrix, so a
     * product must be undone in the opposite order: (AB)^T = B^T A^T, hence
     * A first and then B. Getting this order backwards is exactly the bug the
     * hardware comparison above found, so the order is the assertion. */
    int t[4], d[4];
    psp_vfpu_regs(0x18, 4, t);            /* matrix 6 holds x */
    const float in[4] = { 1.0f, -2.0f, 0.5f, 3.0f };
    for (int i = 0; i < 4; i++) psp_cpu.v[t[i]] = in[i];

    psp_vmmul(0x14, 0x28, 0x10, 4);       /* matrix 5 = A * B */
    psp_vtfm(0x1C, 0x14, 0x18, 4, 0);     /* matrix 7 = (AB) x */
    float combined[4];
    psp_vfpu_regs(0x1C, 4, d);
    for (int i = 0; i < 4; i++) combined[i] = psp_cpu.v[d[i]];

    psp_vtfm(0x00, 0x08, 0x18, 4, 0);     /* matrix 0 = A x */
    psp_vtfm(0x04, 0x10, 0x00, 4, 0);     /* matrix 1 = B (A x) */
    psp_vfpu_regs(0x04, 4, d);
    for (int i = 0; i < 4; i++)
        CHECK_F(psp_cpu.v[d[i]], combined[i],
                "vmmul and vtfm agree: (AB)x == B(Ax)");
}


/* ---- vrot -----------------------------------------------------------------
 *
 * Transcribed from pspdev/vfpu-docs (auxiliary function `ivrot`) rather than
 * from an emulator's behaviour, so this checks the specification and not
 * another implementation's reading of it.
 *
 * The lane rules are the whole content of the instruction, and the
 * equal-selector case is the one that reads like a degenerate case and is not:
 * every lane *except* the named one receives the sine. */
static void test_vrot(void) {
    psp_vfpu_reset();

    const float quarter = 1.0f;          /* angles are in quarter-turns */
    const float s90 = 1.0f, c90 = 0.0f;  /* sin(pi/2), cos(pi/2) */

    /* cos in lane 0, sin in lane 1, the rest zero. */
    psp_cpu.v[0] = quarter;
    psp_vrot(8, 0, /* sl=1, cl=0 */ (1u << 2) | 0u, 4);
    CHECK_F(psp_cpu.v[8],  c90, "vrot: cosine lane");
    CHECK_F(psp_cpu.v[9],  s90, "vrot: sine lane");
    CHECK_F(psp_cpu.v[10], 0.0f, "vrot: unnamed lane is zero");
    CHECK_F(psp_cpu.v[11], 0.0f, "vrot: unnamed lane is zero");

    /* Bit 4 negates the sine and nothing else. */
    psp_vrot(8, 0, 0x10u | (1u << 2) | 0u, 4);
    CHECK_F(psp_cpu.v[8],  c90,  "vrot: negation leaves cosine alone");
    CHECK_F(psp_cpu.v[9], -s90,  "vrot: bit 4 negates the sine");

    /* Equal selectors: the named lane takes the cosine and every *other* lane
     * takes the sine -- not zero. Getting this wrong yields a rotation row
     * that is right in two lanes and silently wrong in the rest. */
    psp_vrot(8, 0, /* sl == cl == 2 */ (2u << 2) | 2u, 4);
    CHECK_F(psp_cpu.v[8],  s90, "vrot: equal selectors, lane 0 is sine");
    CHECK_F(psp_cpu.v[9],  s90, "vrot: equal selectors, lane 1 is sine");
    CHECK_F(psp_cpu.v[10], c90, "vrot: equal selectors, named lane is cosine");
    CHECK_F(psp_cpu.v[11], s90, "vrot: equal selectors, lane 3 is sine");

    /* A zero angle gives cos = 1, sin = 0 -- the identity row, and a check that
     * the quarter-turn scaling is applied rather than radians. */
    psp_cpu.v[0] = 0.0f;
    psp_vrot(8, 0, (1u << 2) | 0u, 4);
    CHECK_F(psp_cpu.v[8], 1.0f, "vrot: angle 0 gives cos = 1");
    CHECK_F(psp_cpu.v[9], 0.0f, "vrot: angle 0 gives sin = 0");

    /* Half a quarter-turn is 45 degrees; both halves equal sqrt(2)/2. If the
     * scaling were radians this would be sin(0.5) = 0.479. */
    psp_cpu.v[0] = 0.5f;
    psp_vrot(8, 0, (1u << 2) | 0u, 4);
    CHECK_F(psp_cpu.v[8], 0.70710678f, "vrot: angle is in quarter-turns");
    CHECK_F(psp_cpu.v[9], 0.70710678f, "vrot: angle is in quarter-turns");

    /* Pairs write two lanes and leave the rest untouched. */
    psp_cpu.v[12] = psp_cpu.v[13] = psp_cpu.v[14] = -1.0f;
    psp_cpu.v[0] = 0.0f;
    psp_vrot(12, 0, (1u << 2) | 0u, 2);
    CHECK_F(psp_cpu.v[12],  1.0f, "vrot.p: lane 0");
    CHECK_F(psp_cpu.v[13],  0.0f, "vrot.p: lane 1");
    CHECK_F(psp_cpu.v[14], -1.0f, "vrot.p: writes only two lanes");
}

int main(void) {
    if (psp_mem_init() != 0) { printf("memory init failed\n"); return 1; }
    psp_cpu_reset();
    psp_vfpu_reset();

    test_register_addressing();
    test_load_store();
    test_arithmetic();
    test_prefixes();
    test_compare();
    test_matrix_ops();
    test_matrix_transform();
    test_vrot();

    psp_mem_free();

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("all VFPU checks passed\n");
    return 0;
}
