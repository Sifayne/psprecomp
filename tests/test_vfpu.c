/* VFPU tests — synthetic data only.
 *
 * Two things get pinned here. The first is register addressing, because the
 * layout is what makes a matrix row and column alias correctly and everything
 * else is built on it. The second is the operand prefixes, because nothing in
 * an arithmetic instruction's own encoding says a prefix is rewriting it --
 * a swizzle or a lane negation applied to the wrong operand, or not at all,
 * produces numbers that are wrong and entirely plausible.
 */

#include "psprecomp/recomp_rt.h"
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

static void test_large_vector_normalize(void) {
    psp_vfpu_reset();

    /* This is the same three-lane vdot -> vrsq -> vscl sequence and magnitude
     * used by the game's aim-basis normalizer. It used to yield a vector only
     * 0.000125 long, folding an upward weapon direction below the horizon. */
    const float in[4] = {
        0.0f,
        psp_bits_to_f32(0x51F9A354u), /* 1.34023381e11 */
        psp_bits_to_f32(0x4F948EB7u), /* 4.98476186e9  */
        0.0f,
    };
    float out[4];
    int scalar[4];

    set_quad(0x00, in);
    psp_vdot(0x04, 0x00, 0x00, 3);
    psp_vfpu_regs(0x04, 1, scalar);
    CHECK(psp_f32_to_bits(psp_cpu.v[scalar[0]]) == 0x6473C556u,
          "large-vector vdot: got 0x%08X, want 0x6473C556",
          psp_f32_to_bits(psp_cpu.v[scalar[0]]));

    psp_vunary(PSP_VU_RSQ, 0x04, 0x04, 1);
    psp_vscl(0x08, 0x00, 0x04, 3);
    get_quad(0x08, out);

    CHECK_F(out[0], 0.0f, "large-vector normalized x");
    CHECK_F(out[1], 0.99930906f, "large-vector normalized y");
    CHECK_F(out[2], 0.037167527f, "large-vector normalized z");
    CHECK_F(out[0] * out[0] + out[1] * out[1] + out[2] * out[2],
            1.0f, "large-vector normalized length");
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

/* ---- special values -------------------------------------------------------
 *
 * Rows of vfpuprobe section 5 as a PSP on firmware 6.60 printed them: operand
 * quads s and t in, the destination quad out, bit for bit. */
static void set_bits(uint32_t vreg, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    const uint32_t w[4] = { a, b, c, d };
    int r[4];
    psp_vfpu_regs(vreg, 4, r);
    for (int i = 0; i < 4; i++) psp_cpu.v[r[i]] = psp_bits_to_f32(w[i]);
}
static int quad_is(uint32_t vreg, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    const uint32_t w[4] = { a, b, c, d };
    int r[4], ok = 1;
    psp_vfpu_regs(vreg, 4, r);
    for (int i = 0; i < 4; i++) {
        const uint32_t got = psp_f32_to_bits(psp_cpu.v[r[i]]);
        if (got != w[i]) {
            printf("    lane %d: got %08X, want %08X\n", i, got, w[i]);
            ok = 0;
        }
    }
    return ok;
}

typedef void (*vbinop)(uint32_t, uint32_t, uint32_t, int);
static int binop_is(vbinop op, const uint32_t s[4], const uint32_t t[4],
                    uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    set_bits(0x00, s[0], s[1], s[2], s[3]);
    set_bits(0x04, t[0], t[1], t[2], t[3]);
    op(0x08, 0x00, 0x04, 4);
    return quad_is(0x08, a, b, c, d);
}

static void test_special_values(void) {
    psp_vfpu_reset();
    static const uint32_t NANINF_S[4] = { 0x7FC00000, 0x80000000, 0x7F800000, 0xFF800000 };
    static const uint32_t NANINF_T[4] = { 0x3F800000, 0x00000000, 0xFF800000, 0xFF800000 };
    static const uint32_t DEN_S[4]    = { 0x000116C2, 0x800116C2, 0x006CE3EE, 0x40400000 };
    static const uint32_t DEN_T[4]    = { 0x000116C2, 0x3F800000, 0x006CE3EE, 0x3EAAAAAB };
    static const uint32_t SNAN_S[4]   = { 0x7F800001, 0xFFC00000, 0x3F800000, 0x40000000 };
    static const uint32_t SNAN_T[4]   = { 0x40000000, 0x3F800000, 0x7FC00001, 0x80000000 };
    static const uint32_t ZERO_S[4]   = { 0x00000000, 0x80000000, 0x00000000, 0x80000000 };
    static const uint32_t ZERO_T[4]   = { 0x00000000, 0x00000000, 0x80000000, 0x80000000 };
    static const uint32_t EDGE_S[4]   = { 0x007FFFFF, 0x80000001, 0xBF800000, 0xFF800000 };
    static const uint32_t EDGE_T[4]   = { 0x00000000, 0x3F800000, 0x7FC00000, 0x7F800000 };

    /* One NaN, 7F800001, positive from vadd and signed by the operands in
     * vmul/vdiv; denormal operands are zeros and tiny results flush. */
    CHECK(binop_is(psp_vadd, NANINF_S, NANINF_T, 0x7F800001, 0x00000000, 0x7F800001, 0xFF800000),
          "vadd nan-inf (step 51)");
    CHECK(binop_is(psp_vadd, DEN_S, DEN_T, 0x00000000, 0x3F800000, 0x00000000, 0x40555555),
          "vadd denormal (step 51)");
    CHECK(binop_is(psp_vadd, SNAN_S, SNAN_T, 0x7F800001, 0x7F800001, 0x7F800001, 0x40000000),
          "vadd snan (step 51)");
    CHECK(binop_is(psp_vmul, SNAN_S, SNAN_T, 0x7F800001, 0xFF800001, 0x7F800001, 0x80000000),
          "vmul snan (step 53)");
    CHECK(binop_is(psp_vmul, EDGE_S, EDGE_T, 0x00000000, 0x80000000, 0xFF800001, 0xFF800000),
          "vmul sqrt-edge (step 53)");
    CHECK(binop_is(psp_vdiv, ZERO_S, ZERO_T, 0x7F800001, 0xFF800001, 0xFF800001, 0x7F800001),
          "vdiv zeros (step 54)");
    CHECK(binop_is(psp_vdiv, DEN_S, DEN_T, 0x7F800001, 0x80000000, 0x7F800001, 0x41100000),
          "vdiv denormal (step 54)");

    /* The sign-magnitude order, bits kept, ties to t. */
    CHECK(binop_is(psp_vmin, SNAN_S, SNAN_T, 0x40000000, 0xFFC00000, 0x3F800000, 0x80000000),
          "vmin snan (step 55)");
    CHECK(binop_is(psp_vmin, EDGE_S, EDGE_T, 0x00000000, 0x80000001, 0xBF800000, 0xFF800000),
          "vmin sqrt-edge (step 55)");
    CHECK(binop_is(psp_vmax, NANINF_S, NANINF_T, 0x7FC00000, 0x00000000, 0x7F800000, 0xFF800000),
          "vmax nan-inf (step 56)");
    set_bits(0x00, EDGE_S[0], EDGE_S[1], EDGE_S[2], EDGE_S[3]);
    set_bits(0x04, EDGE_T[0], EDGE_T[1], EDGE_T[2], EDGE_T[3]);
    psp_vcmp_val(0x08, 0x00, 0x04, 0, 4);
    CHECK(quad_is(0x08, 0x00000000, 0xBF800000, 0xBF800000, 0xBF800000),
          "vscmp sqrt-edge (step 57)");

    /* vabs and vsgn on the bits. */
    set_bits(0x00, 0x00000000, 0x80000000, 0xFFC00000, 0x7F800001);
    psp_vunary(PSP_VU_ABS, 0x08, 0x00, 4);
    CHECK(quad_is(0x08, 0x00000000, 0x00000000, 0x7FC00000, 0x7F800001), "vabs (step 69)");
    set_bits(0x00, DEN_S[0], DEN_S[1], DEN_S[2], DEN_S[3]);
    psp_vfpu9(0x08, 0x00, 10, 4);
    CHECK(quad_is(0x08, 0x00000000, 0x00000000, 0x00000000, 0x3F800000), "vsgn denormal (step 71)");

    /* The sorts: ties give both lanes one value. */
    set_bits(0x00, ZERO_S[0], ZERO_S[1], ZERO_S[2], ZERO_S[3]);
    psp_vfpu9(0x08, 0x00, 0, 4);
    CHECK(quad_is(0x08, 0x00000000, 0x00000000, 0x00000000, 0x00000000), "vsrt1 zeros (step 77)");
    psp_vfpu9(0x08, 0x00, 1, 4);
    CHECK(quad_is(0x08, 0x00000000, 0x80000000, 0x80000000, 0x00000000), "vsrt2 zeros (step 78)");
    psp_vfpu9(0x08, 0x00, 9, 4);
    CHECK(quad_is(0x08, 0x80000000, 0x00000000, 0x00000000, 0x80000000), "vsrt4 zeros (step 80)");
    set_bits(0x00, NANINF_S[0], NANINF_S[1], NANINF_S[2], NANINF_S[3]);
    psp_vfpu9(0x08, 0x00, 9, 4);
    CHECK(quad_is(0x08, 0x7FC00000, 0x7F800000, 0x80000000, 0xFF800000), "vsrt4 nan-inf (step 80)");
    set_bits(0x00, DEN_S[0], DEN_S[1], DEN_S[2], DEN_S[3]);
    psp_vfpu9(0x08, 0x00, 3, 4);
    CHECK(quad_is(0x08, 0x00000000, 0x40400000, 0x00000000, 0xC0400000), "vbfy2 denormal (step 82)");

    /* vcmp: a denormal is zero. */
    int s[4], t[4];
    psp_vfpu_regs(0x00, 1, s);
    psp_vfpu_regs(0x04, 1, t);
    psp_cpu.v[s[0]] = psp_bits_to_f32(0x00000001u);
    psp_cpu.v[t[0]] = psp_bits_to_f32(0x80000000u);
    psp_vcmp(1 /* EQ */, 0x00, 0x04, 1);
    CHECK(psp_cpu.vfpu_cc & 1u, "vcmp EQ: 00000001 equals -0 (step 90)");
    psp_vcmp(8 /* EZ */, 0x00, 0x04, 1);
    CHECK(psp_cpu.vfpu_cc & 1u, "vcmp EZ: 00000001 is zero (step 97)");
    psp_vfpu_reset();
}

/* ---- cross products, matrix rounding, vcst, half floats -------------------
 *
 * vfpuprobe steps 27-30, 50, 65 and 107 (fw 6.60). */
static void test_units_and_conversions(void) {
    psp_vfpu_reset();

    /* vcrsp.t: each lane a two-term dot, so an infinity meets no padding
     * zero (step 65). */
    set_bits(0x00, 0x7F800000, 0x3F800000, 0x40000000, 0x00000000);
    set_bits(0x04, 0x3F800000, 0x40000000, 0x40400000, 0x00000000);
    set_bits(0x08, 0x5A5A5A5A, 0x5A5A5A5A, 0x5A5A5A5A, 0x5A5A5A5A);
    psp_vcrsp(0x08, 0x00, 0x04, 3);
    CHECK(quad_is(0x08, 0xBF800000, 0xFF800000, 0x7F800000, 0x5A5A5A5A), "vcrsp.t cross-inf");
    set_bits(0x00, 0x007FFFFF, 0x80000001, 0xBF800000, 0xFF800000);
    set_bits(0x04, 0x00000000, 0x3F800000, 0x7FC00000, 0x7F800000);
    psp_vcrsp(0x08, 0x00, 0x04, 3);
    CHECK(quad_is(0x08, 0x7F800001, 0x7F800001, 0x00000000, 0x5A5A5A5A), "vcrsp.t sqrt-edge");

    /* vmmul.q M300, M100, M200 over the probe's A and B, one rounding per
     * element: 43055555, not the running sum's 43055556 (step 107). */
    static const uint32_t A[16] = {
        0x3F800000, 0x40000000, 0x40400000, 0x40800000, 0x40A00000, 0x40C00000, 0x40E00000, 0x41000000,
        0x41100000, 0x41200000, 0x41300000, 0x41400000, 0x41500000, 0x41600000, 0x41700000, 0x41800000,
    };
    static const uint32_t B[16] = {
        0xC0400000, 0x40800000, 0x00000000, 0x3F000000, 0x40400000, 0xBF800000, 0x40C00000, 0x40000000,
        0xC0000000, 0x40A00000, 0x3F800000, 0xC0400000, 0x3EAAAAAB, 0x00000000, 0x40E00000, 0x40400000,
    };
    static const uint32_t M3[16] = {
        0x41BC0000, 0x41C80000, 0x41D40000, 0x41E00000, 0x429C0000, 0x42B00000, 0x42C40000, 0x42D80000,
        0xC0E00000, 0xC0C00000, 0xC0A00000, 0xC0800000, 0x42CCAAAA, 0x42E15555, 0x42F60000, 0x43055555,
    };
    for (int c = 0; c < 4; c++) {         /* memory is column-major */
        set_bits(0x04 | (uint32_t)c, A[4 * c], A[4 * c + 1], A[4 * c + 2], A[4 * c + 3]);
        set_bits(0x08 | (uint32_t)c, B[4 * c], B[4 * c + 1], B[4 * c + 2], B[4 * c + 3]);
    }
    psp_vmmul(0x0C, 0x24, 0x08, 4);       /* vs carries the transpose bit */
    for (int c = 0; c < 4; c++)
        CHECK(quad_is(0x0C | (uint32_t)c, M3[4 * c], M3[4 * c + 1], M3[4 * c + 2], M3[4 * c + 3]),
              "vmmul.q column %d (step 107)", c);

    /* vcst 6 is 1/pi correctly rounded (step 50). */
    psp_vcst(0x00, 6, 1);
    int r0[4];
    psp_vfpu_regs(0x00, 1, r0);
    CHECK(psp_f32_to_bits(psp_cpu.v[r0[0]]) == 0x3EA2F983u, "vcst 6: %08X",
          psp_f32_to_bits(psp_cpu.v[r0[0]]));

    /* Half floats (steps 27-30): no subnormals either way, NaN and inf keep
     * the low mantissa bits, and 65520 stays finite. */
    static const uint32_t f2h[][2] = {
        { 0x3F800000, 0x3C00 }, { 0xC0200000, 0xC100 }, { 0x477FE000, 0x7BFF },
        { 0x477FF000, 0x7BFF }, { 0x322BCC77, 0x0000 }, { 0x7F800000, 0x7C00 },
        { 0x7FC00000, 0x7C00 }, { 0xFFC00000, 0xFC00 }, { 0x80000000, 0x8000 },
        { 0x38800000, 0x0400 }, { 0x33800000, 0x0000 }, { 0x7F800001, 0x7C01 },
        { 0x477FEF00, 0x7BFF }, { 0x501502F9, 0x7C00 }, { 0xD01502F9, 0xFC00 },
        { 0x3EAAAAAB, 0x3555 }, { 0x33000000, 0x0000 }, { 0x33C00000, 0x0000 },
        { 0x387FC000, 0x0000 }, { 0xFF800001, 0xFC01 },
    };
    for (size_t i = 0; i < sizeof f2h / sizeof f2h[0]; i++) {
        const uint16_t got = psp_f32_to_half(psp_bits_to_f32(f2h[i][0]));
        CHECK(got == f2h[i][1], "vf2h %08X: %04X, want %04X", f2h[i][0], got, f2h[i][1]);
    }
    static const uint32_t h2f[][2] = {
        { 0x3C00, 0x3F800000 }, { 0xC000, 0xC0000000 }, { 0x7C00, 0x7F800000 },
        { 0xFC00, 0xFF800000 }, { 0x0400, 0x38800000 }, { 0x7BFF, 0x477FE000 },
        { 0x8000, 0x80000000 }, { 0x03FF, 0x00000000 },
    };
    for (size_t i = 0; i < sizeof h2f / sizeof h2f[0]; i++) {
        const uint32_t got = psp_f32_to_bits(psp_half_to_f32((uint16_t)h2f[i][0]));
        CHECK(got == h2f[i][1], "vh2f %04X: %08X, want %08X", h2f[i][0], got, h2f[i][1]);
    }
    psp_vfpu_reset();
}

/* ---- the transcendental unit -----------------------------------------------
 *
 * Words from vfpuprobe's dumps (steps 2-12 and 120-127, fw 6.60). The exact
 * rows are ones the model reproduces; the last two are where its cores are
 * one unit in the 22nd bit off the hardware's, and are held to that. */
static void test_transcendentals(void) {
    psp_vfpu_reset();
    static const struct { int op; uint32_t in, out; } exact[] = {
        { PSP_VU_SIN,  0x40000000, 0x80000000 },   /* vsin(2) is -0         */
        { PSP_VU_SIN,  0xC0000000, 0x00000000 },
        { PSP_VU_COS,  0x3F800000, 0x80000000 },   /* vcos(1) is -0         */
        { PSP_VU_COS,  0x40400000, 0x00000000 },
        { PSP_VU_COS,  0x3F7FFFFF, 0x34480000 },   /* the 2^-24 bit is lost */
        { PSP_VU_SIN,  0x3F000000, 0x3F3504F0 },   /* truncated, not 3F3504F3 */
        { PSP_VU_SIN,  0x2EDBE6FF, 0x00000000 },   /* vsin(1e-10)           */
        { PSP_VU_NSIN, 0x3F000000, 0xBF3504F0 },
        { PSP_VU_ASIN, 0x3F7F8000, 0x3F7AE7A4 },   /* not asin's 0.96020    */
        { PSP_VU_ASIN, 0x3F000000, 0x3EAAAAA8 },
        { PSP_VU_ASIN, 0x3F800001, 0x7F800001 },
        { PSP_VU_EXP2, 0xBF800000, 0x3EFFFFFC },   /* vexp2(-1)             */
        { PSP_VU_EXP2, 0xC2C80000, 0x0D7FFFFC },   /* vexp2(-100)           */
        { PSP_VU_EXP2, 0x42FE0000, 0x7F000000 },   /* vexp2(127)            */
        { PSP_VU_EXP2, 0xC3000000, 0x00000000 },   /* vexp2(-128)           */
        { PSP_VU_EXP2, 0x80800000, 0x3F7FFFFC },   /* vexp2(-2^-126)        */
        { PSP_VU_REXP2,0x2EDBE6FF, 0x3F7FFFFC },
        { PSP_VU_LOG2, 0x407FFFFF, 0x3FFFFFFE },
        { PSP_VU_LOG2, 0x7F7FFFFF, 0x42FFFFFE },
        { PSP_VU_LOG2, 0x3F800001, 0x00000000 },
        { PSP_VU_LOG2, 0x3DCCCCCD, 0xC0549A80 },   /* 15 fraction bits below 1 */
        { PSP_VU_LOG2, 0x3F7FFFFF, 0x80000000 },
        { PSP_VU_LOG2, 0xBF800000, 0x7F800001 },
        { PSP_VU_RCP,  0x807FFFFF, 0xFF800000 },
        { PSP_VU_RCP,  0x7F7FFFFF, 0x00000000 },
        { PSP_VU_RCP,  0x40400000, 0x3EAAAAA8 },
        { PSP_VU_NRCP, 0x40400000, 0xBEAAAAA8 },
        { PSP_VU_RSQ,  0xBF800000, 0xFF800001 },
        { PSP_VU_RSQ,  0x40000000, 0x3F3504F0 },
        { PSP_VU_SQRT, 0x40000000, 0x3FB504F0 },
        { PSP_VU_SQRT, 0x807FFFFF, 0x00000000 },
    };
    int r[4];
    psp_vfpu_regs(0x00, 1, r);
    for (size_t i = 0; i < sizeof exact / sizeof exact[0]; i++) {
        psp_cpu.v[r[0]] = psp_bits_to_f32(exact[i].in);
        psp_vunary(exact[i].op, 0x00, 0x00, 1);
        const uint32_t got = psp_f32_to_bits(psp_cpu.v[r[0]]);
        CHECK(got == exact[i].out, "op %d of %08X: %08X, want %08X",
              exact[i].op, exact[i].in, got, exact[i].out);
    }
    static const struct { int op; uint32_t in, out; } near[] = {
        { PSP_VU_SIN,  0x501502F9, 0xBEFC7DA0 },   /* 1e10 reduces to a non-zero angle */
        { PSP_VU_ASIN, 0x3F7FFFFF, 0x3F7FFFE8 },
    };
    for (size_t i = 0; i < sizeof near / sizeof near[0]; i++) {
        psp_cpu.v[r[0]] = psp_bits_to_f32(near[i].in);
        psp_vunary(near[i].op, 0x00, 0x00, 1);
        const uint32_t got = psp_f32_to_bits(psp_cpu.v[r[0]]);
        const uint32_t d = got > near[i].out ? got - near[i].out : near[i].out - got;
        CHECK(d <= 4, "op %d of %08X: %08X, want %08X within 4", near[i].op, near[i].in,
              got, near[i].out);
    }

    /* vrot is the same sine and cosine (steps 120 and 122). */
    set_bits(0x00, 0x3E800000, 0, 0, 0);
    psp_vrot(0x08, 0x00, 0x04, 2);                         /* [c, s] */
    int d[4];
    psp_vfpu_regs(0x08, 2, d);
    CHECK(psp_f32_to_bits(psp_cpu.v[d[0]]) == 0x3F6C835Cu &&
          psp_f32_to_bits(psp_cpu.v[d[1]]) == 0x3EC3EF14u, "vrot.p [c,s] of 1/4: %08X %08X",
          psp_f32_to_bits(psp_cpu.v[d[0]]), psp_f32_to_bits(psp_cpu.v[d[1]]));
    set_bits(0x00, 0x00000000, 0, 0, 0);
    psp_vrot(0x08, 0x00, 0x14, 2);                         /* [c, -s] */
    CHECK(psp_f32_to_bits(psp_cpu.v[d[0]]) == 0x3F800000u &&
          psp_f32_to_bits(psp_cpu.v[d[1]]) == 0x80000000u, "vrot.p [c,-s] of 0: %08X %08X",
          psp_f32_to_bits(psp_cpu.v[d[0]]), psp_f32_to_bits(psp_cpu.v[d[1]]));
    psp_vfpu_reset();
}

/* ---- the random generator --------------------------------------------------
 *
 * What vfpuprobe measured (steps 154-159, fw 6.60): the seeding, the output
 * forms, and the streams the model reproduces exactly -- from the reset state
 * and from seeds 0, 1 and 12345678. The streams from FFFFFFFF and 3F800000
 * depend on a carry rule nobody has identified and are deliberately not
 * asserted. */
static void draw_quad(int kind, uint32_t out[4]) {
    int q[4];
    psp_vfpu_regs(0x00, 4, q);
    psp_vrnd(0x00, kind, 4);
    for (int i = 0; i < 4; i++) out[i] = psp_f32_to_bits(psp_cpu.v[q[i]]);
}

static void test_random(void) {
    psp_vfpu_reset();
    int r[4];
    psp_vfpu_regs(0x08, 1, r);

    psp_cpu.v[r[0]] = psp_bits_to_f32(0x12345678u);
    psp_vrnds(0x08, 1);
    static const uint32_t seeded[8] = {
        0x3F885678, 0x3F875678, 0x3F865678, 0x3F855678,
        0x3F841234, 0x3F831234, 0x3F821234, 0x3F811234,
    };
    for (int i = 0; i < 8; i++)
        CHECK(psp_mfvc(PSP_VFPU_RCX0 + i) == seeded[i], "vrnds 12345678: rcx%d %08X, want %08X",
              i, psp_mfvc(PSP_VFPU_RCX0 + i), seeded[i]);
    psp_cpu.v[r[0]] = psp_bits_to_f32(0x3F800000u);
    psp_vrnds(0x08, 1);
    CHECK(psp_mfvc(PSP_VFPU_RCX0 + 0) == 0x3F800000u && psp_mfvc(PSP_VFPU_RCX0 + 5) == 0x3F883F80u,
          "vrnds 3F800000: rcx0 %08X rcx5 %08X", psp_mfvc(PSP_VFPU_RCX0), psp_mfvc(PSP_VFPU_RCX0 + 5));

    /* From the reset state, before anything seeds it (step 154). */
    uint32_t got[4];
    psp_vfpu_reset();
    draw_quad(0, got);
    CHECK(got[0] == 0x00094E24u && got[1] == 0x245A1029u && got[2] == 0xECF210C2u &&
          got[3] == 0x91ABC47Bu, "vrndi from reset: %08X %08X %08X %08X",
          got[0], got[1], got[2], got[3]);

    /* Seeds 0, 1 and 12345678, eight draws as two quads, in all three forms:
     * the float forms take the same stream's low 23 bits. */
    static const struct { uint32_t seed, raw[8]; } streams[] = {
        { 0x00000000, { 0x00000001, 0x00010DCE, 0x1C5983F7, 0xC35937CC,
                        0x2E130A5D, 0xE723057A, 0xE3CC94B3, 0x63132A58 } },
        { 0x00000001, { 0x00052DF3, 0x20618A01, 0x6125E0A7, 0x4068A3E1,
                        0x761C1DCB, 0x103BF1B8, 0x88C5605C, 0x93D08434 } },
        { 0x12345678, { 0x632F0A9E, 0x9CB065E1, 0xA483C0E3, 0x752E3534,
                        0xE235C17D, 0x89248F57, 0xD8FA70D8, 0x213C6031 } },
    };
    for (unsigned sidx = 0; sidx < sizeof streams / sizeof streams[0]; sidx++) {
        for (int kind = 0; kind < 3; kind++) {
            psp_cpu.v[r[0]] = psp_bits_to_f32(streams[sidx].seed);
            psp_vrnds(0x08, 1);
            for (int half = 0; half < 2; half++) {
                draw_quad(kind, got);
                for (int i = 0; i < 4; i++) {
                    const uint32_t raw = streams[sidx].raw[4 * half + i];
                    const uint32_t want = kind == 0 ? raw
                        : (kind == 1 ? 0x3F800000u : 0x40000000u) | (raw & 0x7FFFFFu);
                    CHECK(got[i] == want, "seed %08X kind %d draw %d: %08X, want %08X",
                          streams[sidx].seed, kind, 4 * half + i, got[i], want);
                }
            }
        }
    }
    psp_vfpu_reset();
}

/* ---- control state --------------------------------------------------------
 *
 * The reset values and the per-thread ownership, as a PSP on firmware 6.60
 * reports them (vfpuprobe, the lines before step 1 and step 147). */
static void test_control_state(void) {
    static const uint32_t want[16] = {
        0x000000E4, 0x000000E4, 0x00000000, 0x0000003F,
        0x00000000, 0x00000000, 0x00000000, 0x7772CEAB,
        0x3F800001, 0x3F800002, 0x3F800004, 0x3F800008,
        0x3F800000, 0x3F800000, 0x3F800000, 0x3F800000,
    };

    /* A fresh thread: the whole register file, then the control words. */
    psp_cpu_reset_thread();
    for (int i = 0; i < 16; i++)
        CHECK(psp_mfvc(i) == want[i], "fresh thread: control %d is %08X, want %08X",
              i, psp_mfvc(i), want[i]);
    CHECK(psp_cpu.r[PSP_REG_AT] == 0xDEADBEEFu && psp_cpu.r[PSP_REG_S7] == 0xDEADBEEFu &&
          psp_cpu.r[PSP_REG_T9] == 0xDEADBEEFu,
          "fresh thread: unset general registers hold DEADBEEF");
    CHECK(psp_cpu.r[PSP_REG_ZERO] == 0 && psp_cpu.r[PSP_REG_K1] == 0,
          "fresh thread: $zero and $k1 are zero");
    CHECK(psp_cpu.fcr31 == 0x00000E00u, "fresh thread: FCR31 %08X", psp_cpu.fcr31);
    CHECK(psp_f32_to_bits(psp_cpu.f[7]) == 0x7F800001u &&
          psp_f32_to_bits(psp_cpu.v[77]) == 0x7F800001u,
          "fresh thread: float and vector registers hold 7F800001");

    /* The first VFPU op after a reset sees the identity prefix, not zero --
     * zero is the swizzle x,x,x,x and used to broadcast lane 0. */
    const float a[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
    float out[4];
    set_quad(0x00, a);
    psp_vunary(PSP_VU_MOV, 0x04, 0x00, 4);
    get_quad(0x04, out);
    for (int i = 0; i < 4; i++) CHECK_F(out[i], a[i], "first op after reset: no swizzle");

    /* Per thread: the state travels with psp_cpu, which is what a thread
     * switch saves and restores. A prefix pending in one thread and the
     * generator state it seeded are invisible to another. */
    psp_mtvc(PSP_VFPU_RCX0, 0x3F812345u);
    psp_vfpu_set_prefix(0, 0x1B);
    psp_cpu.vfpu_cc = 0x15u;
    const psp_cpu_state main_ctx = psp_cpu;
    psp_cpu_reset_thread();
    CHECK(!psp_vfpu_prefix_pending(), "another thread sees no pending prefix");
    CHECK(psp_mfvc(PSP_VFPU_CC) == 0x3Fu && psp_mfvc(PSP_VFPU_RCX0) == 0x3F800001u,
          "another thread sees its own CC and rcx");
    psp_cpu = main_ctx;
    CHECK(psp_vfpu_prefix_pending() && psp_mfvc(PSP_VFPU_PFXS) == 0x1Bu,
          "switching back restores the pending prefix");
    CHECK(psp_mfvc(PSP_VFPU_CC) == 0x15u && psp_mfvc(PSP_VFPU_RCX0) == 0x3F812345u,
          "switching back restores CC and rcx");
    psp_vfpu_reset();
}

int main(void) {
    if (psp_mem_init() != 0) { printf("memory init failed\n"); return 1; }
    test_control_state();
    psp_cpu_reset();
    psp_vfpu_reset();

    test_register_addressing();
    test_load_store();
    test_arithmetic();
    test_large_vector_normalize();
    test_prefixes();
    test_compare();
    test_matrix_ops();
    test_matrix_transform();
    test_vrot();
    test_special_values();
    test_random();
    test_units_and_conversions();
    test_transcendentals();

    psp_mem_free();

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("all VFPU checks passed\n");
    return 0;
}
