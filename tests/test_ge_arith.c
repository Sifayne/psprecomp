/* The vertex path's short cuts against the exact arithmetic they stand for,
 * bit for bit.
 *
 * uv_axis_texels (ge.c) takes a texture coordinate through TEXSCALE and
 * TEXOFFSET from the float's own bits, with the scale and the offset split
 * once per value; the GE's arithmetic it must reproduce is ge_mul of the
 * coordinate and the scale, ge_mul of the offset and one, and ge_sum of the
 * two, times the texture size -- what geprobe 22 scene 140 was fitted with
 * (test_raster's coordinate cases hold that against the PSP's readings).
 * This compares the two over every 8- and 16-bit coordinate, random floats of
 * every exponent, and zeroes, subnormals, infinities and NaNs, under the
 * probe's scales 1, 2^8 and 2^12, other powers of two and other scales, with
 * and without an offset. shade_lights_of and shade_map_uv are held to the
 * per-vertex form they replaced the same way.
 *
 * ge.c is included whole for its static functions; the library's own copy is
 * then never linked in.
 *
 *   test_ge_arith          the comparisons
 *   test_ge_arith bench    also time uv_axis_texels against the exact sum
 */

#include "../src/hle/ge.c"

#include <time.h>

static int failures;
static uint64_t checks;

#define CHECK(cond, ...)                                       \
    do {                                                       \
        checks++;                                              \
        if (!(cond)) {                                         \
            if (failures < 20) {                               \
                printf("FAIL %s:%d: ", __FILE__, __LINE__);    \
                printf(__VA_ARGS__);                           \
                printf("\n");                                  \
            }                                                  \
            failures++;                                        \
        }                                                      \
    } while (0)

static uint32_t bits_of(float f) { uint32_t b; memcpy(&b, &f, sizeof b); return b; }
static float float_of(uint32_t b) { float f; memcpy(&f, &b, sizeof f); return f; }

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint64_t rng(void) {
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17;
    return rng_state;
}

/* The coordinate in texels as the GE forms it, from ge_mul (ge_split of each
 * operand) rather than ge_mul_sp: an independent route to the same sum. */
static float reference_axis(float u, float scale, float offset, uint32_t size) {
    const ge_term t[2] = { ge_mul(u, scale), ge_mul(offset, 1.0) };
    return (float)(ge_sum(t, 2) * (double)size);
}

/* Two answers the same: the same bits, or both NaN. An input that is not
 * finite makes ge_sum add the plain products in double, and which NaN such a
 * sum keeps -- its sign, its payload -- is the compiler's choice of operand
 * order (x86 keeps the first operand's), which C leaves open and differs
 * between two inlined copies of one function. It is no GE arithmetic. */
static int same(uint32_t a, uint32_t b) {
    return a == b || (isnan(float_of(a)) && isnan(float_of(b)));
}

/* One coordinate both ways, under an axis already made for its constants. */
static void check_one(const uv_axis *a, float u, float scale, float offset, uint32_t size) {
    const uint32_t want = bits_of(reference_axis(u, scale, offset, size));
    const uint32_t fast = bits_of(uv_axis_texels(a, u));
    const uint32_t exact = bits_of(uv_axis_exact(u, scale, offset, size));
    CHECK(same(fast, want) && same(exact, want),
          "u %08X scale %08X offset %08X size %u: fast %08X, exact %08X, reference %08X (path %d)",
          bits_of(u), bits_of(scale), bits_of(offset), size, fast, exact, want, a->path);
}

/* Floats of interest for any operand: zeroes, the extremes of each class,
 * values whose split significand is a power of two with low bits set, and
 * coordinates on either side of a texel. */
static const uint32_t SPECIAL[] = {
    0x00000000u, 0x80000000u,                           /* zeroes */
    0x00000001u, 0x80000001u, 0x007FFFFFu, 0x807FFFFFu, /* subnormals */
    0x00800000u, 0x80800000u, 0x7F7FFFFFu, 0xFF7FFFFFu, /* FLT_MIN, FLT_MAX */
    0x7F800000u, 0xFF800000u, 0x7FC00000u, 0xFFC00000u, 0x7F800001u, /* infinities, NaNs */
    0x3F800000u, 0xBF800000u, 0x3F800001u, 0x3F8000FFu, 0x3F800100u, 0x3FFFFFFFu, 0x3FFFFF00u,
    0x3F000000u, 0x3E800000u, 0x3B800000u, 0x3C000000u, 0x3F7FFFFFu, 0x3F7FFF00u,
    0x43800000u, 0x45800000u, 0x4B800000u, 0x3A800000u, 0x00FFFFFFu, 0x01000000u,
    0x3EAAAAABu, 0x3F2AAAABu, 0x3DCCCCCDu, 0x3F4CCCCDu, 0x40490FDBu, 0xC0490FDBu,
};
enum { NSPECIAL = sizeof SPECIAL / sizeof SPECIAL[0] };

/* A random float of a kind: any bits at all, a texture coordinate's range,
 * or one cut to 16 bits as the GE's float registers are. */
static float random_float(int kind) {
    const uint32_t r = (uint32_t)rng();
    switch (kind) {
    case 0: return float_of(r);
    case 1: return ((float)(int32_t)r / 2147483648.0f) * 8.0f;
    case 2: return float_of(r & 0xFFFFFF00u);
    default: {
        /* a normal float of any exponent and sign */
        const uint32_t ex = 1u + (uint32_t)(rng() % 254u);
        return float_of((r & 0x807FFFFFu) | (ex << 23));
    }
    }
}

static void check_axis(float scale, float offset, uint32_t size, int full) {
    uv_axis a = { 0 };
    uv_axis_of(&a, scale, offset, size);
    for (unsigned i = 0; i < NSPECIAL; i++) check_one(&a, float_of(SPECIAL[i]), scale, offset, size);
    for (int i = 0; i < 256; i++) check_one(&a, (float)i / 128.0f, scale, offset, size);     /* 8-bit */
    if (full)
        for (int i = 0; i < 65536; i++) check_one(&a, (float)i / 32768.0f, scale, offset, size);   /* 16-bit */
    for (int i = 0; i < (full ? 40000 : 400); i++) check_one(&a, random_float(i & 3), scale, offset, size);
    /* u whose product with the scale carries into a 17th bit, and does not */
    for (int i = 0; i < (full ? 4000 : 100); i++) {
        const uint32_t frac = (uint32_t)rng() & 0x7FFFFFu;
        check_one(&a, float_of(0x3F800000u | frac), scale, offset, size);
        check_one(&a, float_of(0xBF800000u | frac), scale, offset, size);
    }
}

/* The axis cached under one set of constants and asked under another: the
 * key must notice each operand. */
static void test_axis_rekeys(void) {
    uv_axis a = { 0 };
    const float s[3] = { 1.0f, 256.0f, 1.0f }, o[3] = { 0.0f, 0.0f, 0.5f };
    const uint32_t z[3] = { 256, 256, 64 };
    for (int round = 0; round < 3; round++)
        for (int k = 0; k < 3; k++) {
            uv_axis_of(&a, s[k], o[k], z[k]);
            check_one(&a, 0.3f, s[k], o[k], z[k]);
            check_one(&a, 0.0f, s[k], o[k], z[k]);
        }
    uv_axis_of(&a, 1.0f, 0.0f, 128);
    uv_axis_of(&a, 1.0f, -0.0f, 128);                   /* a zero's sign is a new key */
    check_one(&a, -0.0f, 1.0f, -0.0f, 128);
}

/* geprobe 22's three scales, the offsets its scene 140 used, and the sizes a
 * texture can have, over every narrow coordinate. */
static void test_probe_scales(void) {
    const float scales[] = { 1.0f, 256.0f, 4096.0f };
    const float offsets[] = { 0.0f, -0.0f, ge_float(0xC08000), ge_float(0xBF9A00), ge_float(0xBF7000),
                              ge_float(0xC30800), ge_float(0xC46F00), ge_float(0xC544E0), ge_float(0xC555B0) };
    const uint32_t sizes[] = { 1, 2, 4, 8, 16, 32, 64, 128, 256, 512 };
    for (unsigned s = 0; s < sizeof scales / sizeof scales[0]; s++)
        for (unsigned o = 0; o < sizeof offsets / sizeof offsets[0]; o++)
            for (unsigned z = 0; z < sizeof sizes / sizeof sizes[0]; z++)
                check_axis(scales[s], offsets[o], sizes[z], z == 8 || z == 0);
}

/* Every special scale and offset against each other, and random ones. */
static void test_other_scales(void) {
    const uint32_t sizes[] = { 0, 1, 3, 256, 480, 512, 1024, 0x80000000u, 0xFFFFFFFFu };
    for (unsigned s = 0; s < NSPECIAL; s++)
        for (unsigned o = 0; o < NSPECIAL; o += 3)
            for (unsigned z = 0; z < sizeof sizes / sizeof sizes[0]; z += 2)
                check_axis(float_of(SPECIAL[s]), float_of(SPECIAL[o]), sizes[z], 0);
    for (int i = 0; i < 20000; i++) {
        const float scale = random_float(i & 3), offset = (i & 4) ? 0.0f : random_float((i >> 3) & 3);
        const uint32_t size = (i & 8) ? 1u << (rng() % 10) : (uint32_t)rng();
        uv_axis a = { 0 };
        uv_axis_of(&a, scale, offset, size);
        for (int k = 0; k < 64; k++) check_one(&a, random_float(k & 3), scale, offset, size);
    }
}

/* uv_to_texels through the registers, as the draws use it. */
static void test_registers(void) {
    psp_ge_reset();
    g_ge.tex_scale_u = 256.0f; g_ge.tex_scale_v = 1.0f;
    g_ge.tex_offset_u = 0.0f;  g_ge.tex_offset_v = -0.25f;
    g_ge.tex_w = 128; g_ge.tex_h = 64;
    for (int i = 0; i < 1000; i++) {
        const float u = random_float(1), v = random_float(3);
        psp_vertex o;
        uv_to_texels(u, v, &o);
        CHECK(same(bits_of(o.u), bits_of(reference_axis(u, 256.0f, 0.0f, 128))) &&
              same(bits_of(o.v), bits_of(reference_axis(v, 1.0f, -0.25f, 64))),
              "registers: u %08X v %08X", bits_of(u), bits_of(v));
    }
}

/* Shade mapping as it was, the light's length taken for every vertex. */
static float old_shade_axis(int light, const float n[3]) {
    const float *l = g_tl.light[light & 3].pos;
    const float len = sqrtf(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
    return len > 0.0f ? (l[0] * n[0] + l[1] * n[1] + l[2] * n[2]) / len : n[2];
}
static void old_shade_map_uv(const float model_nrm[3], float *u, float *v) {
    float n[3];
    mul_3x3(g_tl.world, model_nrm, n);
    const float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (len > 0.0f) { n[0] /= len; n[1] /= len; n[2] /= len; }
    else { n[0] = n[1] = 0.0f; n[2] = 1.0f; }
    *u = (1.0f + old_shade_axis(g_tl.tex_shade_u, n)) * 0.5f * g_ge.tex_scale_u * (float)g_ge.tex_w;
    *v = (1.0f + old_shade_axis(g_tl.tex_shade_v, n)) * 0.5f * g_ge.tex_scale_v * (float)g_ge.tex_h;
}
static void test_shade_map(void) {
    psp_ge_reset();
    for (int round = 0; round < 2000; round++) {
        for (int i = 0; i < 12; i++) g_tl.world[i] = random_float(1);
        for (int l = 0; l < 4; l++)
            for (int k = 0; k < 3; k++) g_tl.light[l].pos[k] = (round % 7 == l) ? 0.0f : random_float(1);
        g_tl.tex_shade_u = (int)(rng() & 7); g_tl.tex_shade_v = (int)(rng() & 7);
        g_ge.tex_scale_u = random_float(2); g_ge.tex_scale_v = random_float(2);
        g_ge.tex_w = 1u << (rng() % 10); g_ge.tex_h = 1u << (rng() % 10);
        shade_lights sl;
        shade_lights_of(&sl);
        for (int k = 0; k < 16; k++) {
            const float n[3] = { random_float(1), random_float(1), k == 0 ? 0.0f : random_float(1) };
            const float zero[3] = { 0.0f, 0.0f, 0.0f };
            const float *nm = k == 1 ? zero : n;
            float u0, v0, u1, v1;
            old_shade_map_uv(nm, &u0, &v0);
            shade_map_uv(&sl, nm, &u1, &v1);
            CHECK(bits_of(u0) == bits_of(u1) && bits_of(v0) == bits_of(v1),
                  "shade map: %08X %08X, was %08X %08X", bits_of(u1), bits_of(v1), bits_of(u0), bits_of(v0));
        }
    }
}

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/* Nanoseconds a coordinate, fast and exact, over 8-bit and float inputs. */
static void bench(void) {
    enum { N = 1 << 20, ROUNDS = 64 };
    static float in[N];
    const struct { const char *name; float scale, offset; int narrow; } C[] = {
        { "scale 1, no offset, 8-bit", 1.0f, 0.0f, 1 },
        { "scale 1, no offset, float", 1.0f, 0.0f, 0 },
        { "scale 1, offset, float", 1.0f, 0.5f, 0 },
        { "scale 0.75, no offset, float", 0.75f, 0.0f, 0 },
    };
    for (unsigned c = 0; c < sizeof C / sizeof C[0]; c++) {
        for (int i = 0; i < N; i++) in[i] = C[c].narrow ? (float)(rng() & 0xFF) / 128.0f : random_float(1) * 0.25f;
        uv_axis a = { 0 };
        uv_axis_of(&a, C[c].scale, C[c].offset, 256);
        volatile float sink = 0.0f;
        double t0 = now_s();
        for (int r = 0; r < ROUNDS; r++) { float s = 0.0f; for (int i = 0; i < N; i++) s += uv_axis_texels(&a, in[i]); sink += s; }
        double t1 = now_s();
        for (int r = 0; r < ROUNDS; r++) { float s = 0.0f; for (int i = 0; i < N; i++) s += reference_axis(in[i], C[c].scale, C[c].offset, 256); sink += s; }
        double t2 = now_s();
        (void)sink;
        printf("  %-30s fast %5.2f ns, exact %5.2f ns a coordinate\n", C[c].name,
               (t1 - t0) * 1e9 / ((double)N * ROUNDS), (t2 - t1) * 1e9 / ((double)N * ROUNDS));
    }
}

int main(int argc, char **argv) {
    if (psp_mem_init() != 0) { printf("memory init failed\n"); return 1; }
    test_axis_rekeys();
    test_probe_scales();
    test_other_scales();
    test_registers();
    test_shade_map();
    if (argc > 1 && !strcmp(argv[1], "bench")) bench();
    psp_mem_free();
    if (failures) printf("ge_arith: %d failure(s) of %llu checks\n", failures, (unsigned long long)checks);
    else printf("ge_arith: all %llu checks passed\n", (unsigned long long)checks);
    return failures ? 1 : 0;
}
