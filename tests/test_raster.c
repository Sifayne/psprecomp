/* Rasterizer tests — display list in, pixels out.
 *
 * These use hand-built display lists rather than game data, which is the point:
 * the game cannot yet reach its own draw calls, so waiting for it to render
 * would leave this code completely unmeasured until the last blocker clears.
 * A synthetic list exercises the same path the game will take.
 *
 * The checks are on *where* pixels land, not just how many. A rasterizer that
 * fills the whole screen and one that fills the right rectangle both report a
 * nonzero pixel count, and only one of them is correct.
 */

#include "psprecomp/hle.h"
#include "psprecomp/render.h"
#include "psprecomp/mem.h"
#include "psprecomp/cpu.h"

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

#define FB     0x04000000u          /* eDRAM */
#define LIST   0x08800000u
#define VERTS  0x08810000u

/* VTYPE: 8888 colour, 16-bit position, through mode. */
#define VTYPE_2D  ((7u << 2) | (2u << 7) | (1u << 23))

static uint32_t call(uint32_t nid, uint32_t a0, uint32_t a1, uint32_t a2,
                     uint32_t a3) {
    psp_cpu.r[PSP_REG_A0] = a0;
    psp_cpu.r[PSP_REG_A1] = a1;
    psp_cpu.r[PSP_REG_A2] = a2;
    psp_cpu.r[PSP_REG_A3] = a3;
    psp_hle_call(nid);
    return psp_cpu.r[PSP_REG_V0];
}

static uint32_t *g_list;
static uint32_t g_pc;

static void cmd(uint8_t op, uint32_t arg) {
    psp_write32(LIST + g_pc, ((uint32_t)op << 24) | (arg & 0xFFFFFF));
    g_pc += 4;
}

static void begin_list_vtype(uint32_t vtype) {
    g_pc = 0;
    (void)g_list;
    /* Addresses do not fit in a 24-bit argument. FBP carries the low 24 bits
     * and FBW smuggles bits 24-31 in its own top byte; VADDR is relative to
     * GE_BASE. Getting this wrong yields a plausible-looking address and
     * silently draws nothing. */
    cmd(0x10, (VERTS >> 8) & 0xFF0000);            /* BASE */
    cmd(0x9C, FB & 0xFFFFFF);                      /* FBP */
    cmd(0x9D, ((FB >> 8) & 0xFF0000) | 480);       /* FBW + address high byte */
    cmd(0x12, vtype);                              /* VTYPE */
    cmd(0x01, VERTS & 0xFFFFFF);                   /* VADDR */
}

static void begin_list(void) { begin_list_vtype(VTYPE_2D); }

static void end_list(void) {
    cmd(0x0F, 0);                    /* FINISH */
    cmd(0x0C, 0);                    /* END */
    call(0xAB49E76A, LIST, 0, 0, 0); /* sceGeListEnQueue */
}

/* One 16-bit-position, 8888-colour vertex. Colour precedes position. */
static void vertex(int idx, int x, int y, uint32_t rgba) {
    uint32_t a = VERTS + (uint32_t)idx * 12;   /* 4 colour + 6 pos, padded to 12 */
    psp_write32(a, rgba);
    psp_write16(a + 4, (uint16_t)x);
    psp_write16(a + 6, (uint16_t)y);
    psp_write16(a + 8, 0);
}

static uint32_t pixel(int x, int y) {
    /* Colour only. The framebuffer's alpha byte is the stencil buffer and an
     * ordinary draw does not write it -- gpu/texfunc reads 44ffffff back from
     * a 44444444 fill after every draw -- so what a draw is judged on here is
     * the three channels it does write. */
    return psp_read32(FB + (uint32_t)(y * 480 + x) * 4) & 0x00FFFFFFu;
}

static void clear_fb(void) {
    for (int y = 0; y < 272; y++)
        for (int x = 0; x < 480; x++)
            psp_write32(FB + (uint32_t)(y * 480 + x) * 4, 0);
}

/* A sprite is the PSP's 2D quad: two vertices, opposite corners. */
static void test_sprite(void) {
    psp_ge_reset();
    clear_fb();
    begin_list();
    vertex(0, 100, 50, 0xFF0000FFu);
    vertex(1, 200, 150, 0xFF0000FFu);
    cmd(0x04, (6u << 16) | 2);       /* PRIM sprites, 2 vertices */
    end_list();

    CHECK(psp_ge_pixels() == 100 * 100, "sprite pixel count: %llu (want 10000)",
          (unsigned long long)psp_ge_pixels());
    CHECK(pixel(150, 100) == 0x000000FFu, "sprite interior: 0x%08X", pixel(150, 100));
    CHECK(pixel(99, 100) == 0, "left of sprite should be untouched: 0x%08X", pixel(99, 100));
    CHECK(pixel(150, 49) == 0, "above sprite should be untouched: 0x%08X", pixel(150, 49));
    /* Half-open on the far edge, so adjacent sprites tile without overlapping. */
    CHECK(pixel(200, 100) == 0, "far edge is exclusive: 0x%08X", pixel(200, 100));
}

static void test_triangle(void) {
    psp_ge_reset();
    clear_fb();
    begin_list();
    vertex(0, 10, 10, 0xFF00FF00u);
    vertex(1, 110, 10, 0xFF00FF00u);
    vertex(2, 10, 110, 0xFF00FF00u);
    cmd(0x04, (3u << 16) | 3);       /* PRIM triangles */
    end_list();

    CHECK(psp_ge_pixels() > 4000, "triangle should cover ~5000 px, got %llu",
          (unsigned long long)psp_ge_pixels());
    CHECK(pixel(20, 20) == 0x0000FF00u, "inside triangle: 0x%08X", pixel(20, 20));
    /* The hypotenuse runs from (110,10) to (10,110); (100,100) is well past it. */
    CHECK(pixel(100, 100) == 0, "outside hypotenuse: 0x%08X", pixel(100, 100));
}

/* Winding must not matter while back-face culling is unimplemented, or half of
 * any real model silently disappears. */
static void test_winding(void) {
    psp_ge_reset();
    clear_fb();
    begin_list();
    vertex(0, 10, 10, 0xFFFFFFFFu);
    vertex(1, 10, 110, 0xFFFFFFFFu);
    vertex(2, 110, 10, 0xFFFFFFFFu);
    cmd(0x04, (3u << 16) | 3);
    end_list();
    CHECK(pixel(20, 20) == 0x00FFFFFFu, "reversed winding still fills: 0x%08X",
          pixel(20, 20));
}

/* Off-screen geometry must clip, not scribble outside the framebuffer or wrap
 * to the opposite edge. */
static void test_clipping(void) {
    psp_ge_reset();
    clear_fb();
    begin_list();
    vertex(0, -50, -50, 0xFFFF0000u);
    vertex(1, 50, 50, 0xFFFF0000u);
    cmd(0x04, (6u << 16) | 2);
    end_list();

    CHECK(psp_ge_pixels() == 50 * 50, "clipped sprite: %llu (want 2500)",
          (unsigned long long)psp_ge_pixels());
    CHECK(pixel(0, 0) == 0x00FF0000u, "clipped sprite covers origin");
    CHECK(pixel(479, 271) == 0, "far corner untouched: 0x%08X", pixel(479, 271));
}

/* Transformed geometry is not implemented. It must be *counted*, not drawn at
 * the wrong place — wrong pixels are harder to diagnose than no pixels. */
static void test_transformed_is_skipped(void) {
    psp_ge_reset();
    clear_fb();
    g_pc = 0;
    cmd(0x10, (VERTS >> 8) & 0xFF0000);
    cmd(0x9C, FB & 0xFFFFFF);
    cmd(0x9D, ((FB >> 8) & 0xFF0000) | 480);
    cmd(0x12, VTYPE_2D & ~(1u << 23));   /* through bit cleared */
    cmd(0x01, VERTS & 0xFFFFFF);
    vertex(0, 10, 10, 0xFFFFFFFFu);
    vertex(1, 110, 110, 0xFFFFFFFFu);
    cmd(0x04, (6u << 16) | 2);
    end_list();

    CHECK(psp_ge_pixels() == 0, "transformed geometry must not be drawn, got %llu px",
          (unsigned long long)psp_ge_pixels());
}

static void test_triangle_strip(void) {
    psp_ge_reset();
    clear_fb();
    begin_list();
    vertex(0, 10, 10, 0xFFFFFFFFu);
    vertex(1, 110, 10, 0xFFFFFFFFu);
    vertex(2, 10, 110, 0xFFFFFFFFu);
    vertex(3, 110, 110, 0xFFFFFFFFu);
    cmd(0x04, (4u << 16) | 4);       /* strip: 4 vertices -> 2 triangles */
    end_list();

    /* Two triangles sharing an edge tile the square without a seam. */
    CHECK(pixel(20, 20) == 0x00FFFFFFu, "strip tri 0: 0x%08X", pixel(20, 20));
    CHECK(pixel(100, 100) == 0x00FFFFFFu, "strip tri 1: 0x%08X", pixel(100, 100));
}

/* ---- texturing -----------------------------------------------------------
 *
 * None of the tests above binds a texture, so until these the whole sampling
 * path -- texel addressing, u/v interpolation, the CLUT, the swizzle -- was
 * unmeasured, and the bug these were written for lived in it for the life of
 * the file.
 *
 * Exact assertions are possible because two things are identities: modulate
 * with a vertex colour of 0xFFFFFFFF returns the texel unchanged, and the 8888
 * sampler returns the stored word verbatim. So a texel written here arrives at
 * pixel() bit for bit, and a test can name the texel it expects rather than
 * settling for "something was drawn". */

#define TEX 0x08820000u

/* 16-bit texcoords, 8888 colour, 16-bit position, through mode. The GE's field
 * order is fixed -- texcoord, colour, normal, position -- and each field is
 * aligned to its own size, which puts texcoords at 0, colour at 4, position at
 * 8, and rounds the stride up to 16. */
#define VTYPE_2D_TEX (2u | (7u << 2) | (2u << 7) | (1u << 23))

static void vertex_uv(int idx, int x, int y, int u, int v, uint32_t rgba) {
    uint32_t a = VERTS + (uint32_t)idx * 16;
    psp_write16(a,      (uint16_t)u);
    psp_write16(a + 2,  (uint16_t)v);
    psp_write32(a + 4,  rgba);
    psp_write16(a + 8,  (uint16_t)x);
    psp_write16(a + 10, (uint16_t)y);
    psp_write16(a + 12, 0);
}

/* Texel (u,v) carries its own coordinates: u in the red channel, v in green.
 * Any sampling error is then legible as the offset it is, rather than as a
 * colour that happens to be wrong. */
static void upload_ramp_texture(int w, int h) {
    for (int v = 0; v < h; v++)
        for (int u = 0; u < w; u++)
            psp_write32(TEX + (uint32_t)(v * w + u) * 4,
                        0xFF000000u | ((uint32_t)v << 8) | (uint32_t)u);
}

static uint32_t ramp_texel(int u, int v) {
    return 0x00000000u | ((uint32_t)v << 8) | (uint32_t)u;
}

/* TEXSIZE takes log2 of each dimension. The texture address arrives split
 * across two registers, and its high nibble rides in bits 16-19 of TEXBUFWIDTH
 * rather than in that register's low byte -- the trap that put the palette
 * inside the loaded module and speckled the logo once already. */
static void texture_state(uint32_t addr, int stride, int log2w, int log2h,
                          int fmt, int swizzled, int filter) {
    cmd(0x1E, 1);                                              /* TEXTUREMAPENABLE */
    cmd(0xA0, addr & 0x00FFFFF0u);                             /* TEXADDR0 */
    cmd(0xA8, ((addr >> 8) & 0x000F0000u) | (uint32_t)stride); /* TEXBUFWIDTH0 */
    cmd(0xB8, (uint32_t)log2w | ((uint32_t)log2h << 8));       /* TEXSIZE0 */
    cmd(0xC3, (uint32_t)fmt);                                  /* TEXFORMAT */
    cmd(0xC2, (uint32_t)swizzled);                             /* TEXMODE */
    cmd(0xC9, 0);                                              /* TEXFUNC: modulate */
    cmd(0xC6, (uint32_t)filter | ((uint32_t)filter << 8));     /* TEXFILTER */
}

/* A 1:1 blit must map texel k to pixel k, and must write each pixel once.
 *
 * 13x11 rather than a power of two on purpose: the reciprocal of the doubled
 * area is then inexact, which is the condition the old corner-sampling code
 * needed to go wrong. It put the sample point exactly on a texel boundary, so a
 * few ULP of error in the barycentric reconstruction chose texel k or k-1 at
 * random, per pixel.
 *
 * The pixel count is the other half. Two triangles of a strip share a diagonal,
 * and without a fill rule a pixel lying exactly on it satisfies both -- drawn
 * twice, which is invisible on opaque geometry and double-composites the moment
 * anything blends. */
static void test_texture_1to1(void) {
    psp_ge_reset();
    clear_fb();
    upload_ramp_texture(16, 16);

    begin_list_vtype(VTYPE_2D_TEX);
    texture_state(TEX, 16, 4, 4, 3 /* 8888 */, 0, 0 /* nearest */);
    vertex_uv(0, 40, 30,  0,  0, 0xFFFFFFFFu);
    vertex_uv(1, 53, 30, 13,  0, 0xFFFFFFFFu);
    vertex_uv(2, 40, 41,  0, 11, 0xFFFFFFFFu);
    vertex_uv(3, 53, 41, 13, 11, 0xFFFFFFFFu);
    cmd(0x04, (4u << 16) | 4);       /* PRIM triangle strip */
    end_list();

    CHECK(psp_ge_pixels() == 13 * 11,
          "1:1 quad must write each pixel once: %llu (want 143)",
          (unsigned long long)psp_ge_pixels());

    int bad = 0;
    for (int j = 0; j < 11 && bad < 4; j++)
        for (int k = 0; k < 13 && bad < 4; k++) {
            const uint32_t got = pixel(40 + k, 30 + j);
            if (got != ramp_texel(k, j)) {
                bad++;
                CHECK(0, "1:1 texel at (%d,%d): got 0x%08X want 0x%08X",
                      k, j, got, ramp_texel(k, j));
            }
        }
}

/* Minified 3:1, which separates corner sampling from centre sampling
 * deterministically rather than by luck.
 *
 * The scale has to be odd. At 2:1 the pixel centre lands exactly on a texel
 * boundary -- the same knife edge, one texel over -- and at 1:1 corner and
 * centre differ only by accumulated float error. At 3:1 the centre lands at
 * 3k + 1.5, a texel and a half in, while the corner lands at 3k. The arithmetic
 * is exact in both cases: the doubled area is 512, a power of two.
 */
static void test_texture_minified_samples_centre(void) {
    psp_ge_reset();
    clear_fb();
    upload_ramp_texture(64, 64);

    begin_list_vtype(VTYPE_2D_TEX);
    texture_state(TEX, 64, 6, 6, 3, 0, 0);
    vertex_uv(0, 40, 30,  0,  0, 0xFFFFFFFFu);
    vertex_uv(1, 56, 30, 48,  0, 0xFFFFFFFFu);
    vertex_uv(2, 40, 46,  0, 48, 0xFFFFFFFFu);
    vertex_uv(3, 56, 46, 48, 48, 0xFFFFFFFFu);
    cmd(0x04, (4u << 16) | 4);
    end_list();

    int bad = 0;
    for (int j = 0; j < 16 && bad < 4; j++)
        for (int k = 0; k < 16 && bad < 4; k++) {
            const uint32_t got  = pixel(40 + k, 30 + j);
            const uint32_t want = ramp_texel(3 * k + 1, 3 * j + 1);
            if (got != want) {
                bad++;
                CHECK(0, "minified texel at (%d,%d): got 0x%08X want 0x%08X"
                         " (corner sampling would give 0x%08X)",
                      k, j, got, want, ramp_texel(3 * k, 3 * j));
            }
        }
}

/* The sprite path has the same defect and shares none of the code, so it needs
 * its own check: two corners, a linear ramp, no barycentric weights. */
static void test_sprite_texture_samples_centre(void) {
    psp_ge_reset();
    clear_fb();
    upload_ramp_texture(64, 64);

    begin_list_vtype(VTYPE_2D_TEX);
    texture_state(TEX, 64, 6, 6, 3, 0, 0);
    vertex_uv(0, 40, 30,  0,  0, 0xFFFFFFFFu);
    vertex_uv(1, 56, 46, 48, 48, 0xFFFFFFFFu);
    cmd(0x04, (6u << 16) | 2);       /* PRIM sprites */
    end_list();

    int bad = 0;
    for (int j = 0; j < 16 && bad < 4; j++)
        for (int k = 0; k < 16 && bad < 4; k++) {
            const uint32_t got  = pixel(40 + k, 30 + j);
            const uint32_t want = ramp_texel(3 * k + 1, 3 * j + 1);
            if (got != want) {
                bad++;
                CHECK(0, "sprite texel at (%d,%d): got 0x%08X want 0x%08X",
                      k, j, got, want);
            }
        }
}

/* Float texcoords, 8888 colour, 16-bit position, through mode: tex at 0 (8
 * bytes), colour at 8, position at 12, stride 20. The only way to express a
 * negative u -- 16-bit through-mode texcoords are read unsigned, so -16 in that
 * form arrives as 65520. */
#define VTYPE_2D_TEXF (3u | (7u << 2) | (2u << 7) | (1u << 23))

static void vertex_uvf(int idx, int x, int y, float u, float v, uint32_t rgba) {
    uint32_t a = VERTS + (uint32_t)idx * 20;
    psp_write_f32(a,      u);
    psp_write_f32(a + 4,  v);
    psp_write32(a + 8,    rgba);
    psp_write16(a + 12,   (uint16_t)x);
    psp_write16(a + 14,   (uint16_t)y);
    psp_write16(a + 16,   0);
}

/* Sampling outside [0,size) is ordinary, and what happens there is the game's
 * choice, not the sampler's. The two modes are checked against each other over
 * the same geometry: under repeat a u range one whole texture to the left must
 * render exactly as the unshifted one does, and under clamp it must collapse to
 * the edge texel.
 *
 * Negative coordinates are the half that a mask gets right and a `%` gets
 * wrong: -16 % 16 is 0 in C, but -16 wrapped is texel 0 only by luck, and -1
 * must land on 15 rather than on -1 % 16 == -1.
 */
static void test_texture_wrap(void) {
    psp_ge_reset();
    clear_fb();
    upload_ramp_texture(16, 16);

    /* Repeat, sampling a full texture width to the left of the origin. */
    begin_list_vtype(VTYPE_2D_TEXF);
    texture_state(TEX, 16, 4, 4, 3, 0, 0);
    cmd(0xC7, 0);                                  /* TEXWRAP: repeat, repeat */
    vertex_uvf(0, 40, 30, -16.0f,  0.0f, 0xFFFFFFFFu);
    vertex_uvf(1, 53, 30,  -3.0f,  0.0f, 0xFFFFFFFFu);
    vertex_uvf(2, 40, 41, -16.0f, 11.0f, 0xFFFFFFFFu);
    vertex_uvf(3, 53, 41,  -3.0f, 11.0f, 0xFFFFFFFFu);
    cmd(0x04, (4u << 16) | 4);
    end_list();

    int bad = 0;
    for (int j = 0; j < 11 && bad < 4; j++)
        for (int k = 0; k < 13 && bad < 4; k++) {
            const uint32_t got = pixel(40 + k, 30 + j);
            if (got != ramp_texel(k, j)) {
                bad++;
                CHECK(0, "repeat at (%d,%d): got 0x%08X want 0x%08X",
                      k, j, got, ramp_texel(k, j));
            }
        }

    /* The same list under clamp must instead read the left edge texel
     * everywhere, which is what the sampler used to do unconditionally. */
    psp_ge_reset();
    clear_fb();
    begin_list_vtype(VTYPE_2D_TEXF);
    texture_state(TEX, 16, 4, 4, 3, 0, 0);
    cmd(0xC7, 1u | (1u << 8));                     /* TEXWRAP: clamp, clamp */
    vertex_uvf(0, 40, 30, -16.0f,  0.0f, 0xFFFFFFFFu);
    vertex_uvf(1, 53, 30,  -3.0f,  0.0f, 0xFFFFFFFFu);
    vertex_uvf(2, 40, 41, -16.0f, 11.0f, 0xFFFFFFFFu);
    vertex_uvf(3, 53, 41,  -3.0f, 11.0f, 0xFFFFFFFFu);
    cmd(0x04, (4u << 16) | 4);
    end_list();

    CHECK(pixel(40, 30) == ramp_texel(0, 0), "clamp at (0,0): 0x%08X", pixel(40, 30));
    CHECK(pixel(52, 30) == ramp_texel(0, 0), "clamp at (12,0): 0x%08X", pixel(52, 30));
    CHECK(pixel(52, 40) == ramp_texel(0, 10), "clamp at (12,10): 0x%08X", pixel(52, 40));
}

/* A 2x1 texture magnified 2x across a 4-pixel span, which puts the four pixel
 * centres at u = 0.25, 0.75, 1.25, 1.75. Taps are taken around u - 0.5, so the
 * weights run 0.75/0.25 clamped at the left, then 0.25/0.75, then 0.75/0.25,
 * then clamped at the right -- four distinct values that pin the tap offset,
 * the weights and the edge behaviour in one row.
 *
 * `lo` and `hi` are the two texels; the caller says which channel to read back,
 * because the case that matters for this game is alpha rather than colour. */
static void bilinear_row(uint32_t lo, uint32_t hi, int filter, int wrap_clamp,
                         int via_blend, uint32_t out[4]) {
    psp_ge_reset();
    clear_fb();
    psp_write32(TEX,     lo);
    psp_write32(TEX + 4, hi);

    begin_list_vtype(VTYPE_2D_TEX);
    texture_state(TEX, 2, 1 /* w=2 */, 0 /* h=1 */, 3, 0, filter);
    cmd(0xC7, wrap_clamp ? (1u | (1u << 8)) : 0u);
    vertex_uv(0, 40, 30, 0, 0, 0xFFFFFFFFu);
    vertex_uv(1, 44, 30, 2, 0, 0xFFFFFFFFu);
    vertex_uv(2, 40, 34, 0, 1, 0xFFFFFFFFu);
    vertex_uv(3, 44, 34, 2, 1, 0xFFFFFFFFu);
    /* The framebuffer's alpha byte is the stencil and an ordinary draw leaves
     * it alone, so a sampled alpha cannot be read back directly. Blending the
     * fragment over black by its own alpha puts that alpha in the colour:
     * red = 255 * a / 255. Source SRC_ALPHA (2), destination
     * ONE_MINUS_SRC_ALPHA (3), equation ADD (0), as the game itself blends. */
    /* TEXFUNC modulate with the RGBA flag: the texture's alpha takes part. At
     * the reset default (RGB) the fragment alpha is the vertex's, which is
     * what hardware does -- gpu/texfunc's blended "(RGB)" lines -- and what
     * a game undoes with sceGuTexFunc(..., GU_TCC_RGBA) before drawing an
     * alpha-masked texture. */
    cmd(0xC9, 1u << 8);
    if (via_blend) { cmd(0x21, 1); cmd(0xDF, 2u | (3u << 4)); }
    cmd(0x04, (4u << 16) | 4);
    end_list();

    for (int k = 0; k < 4; k++) out[k] = pixel(40 + k, 30);
}

static void test_texture_bilinear_midpoint(void) {
    uint32_t row[4];

    /* Colour first: black to red. Clamped at both ends, ramping between. */
    bilinear_row(0xFF000000u, 0xFF0000FFu, 1, 1, 0, row);
    /* Truncated, as hardware does: a quarter of the way between 00 and ff is
     * 63.75, and gpu/filtering/precisionlinear2d shows hardware answering the
     * lower value at every step. */
    static const uint32_t want_r[4] = { 0, 63, 191, 255 };
    for (int k = 0; k < 4; k++)
        CHECK((row[k] & 0xFF) == want_r[k],
              "bilinear red at %d: got %u want %u", k, row[k] & 0xFF, want_r[k]);

    /* Then the shape this game's letterforms actually have: white throughout,
     * the mask entirely in alpha. Filtering RGB and taking alpha from one tap
     * passes the check above and fails this one, while leaving every glyph edge
     * exactly as hard as nearest. */
    bilinear_row(0x00FFFFFFu, 0xFFFFFFFFu, 1, 1, 1, row);
    for (int k = 0; k < 4; k++)
        CHECK((row[k] & 0xFF) == want_r[k],
              "bilinear alpha at %d (seen through the blend): got %u want %u",
              k, row[k] & 0xFF, want_r[k]);
}

/* The filter state must be obeyed in both directions. Bilinear applied
 * unconditionally would pass the test above and blur every UI layer the game
 * asked to be sharp. */
static void test_texture_filter_is_honoured(void) {
    uint32_t row[4];
    bilinear_row(0xFF000000u, 0xFF0000FFu, 0 /* nearest */, 1, 0, row);
    static const uint32_t want_r[4] = { 0, 0, 255, 255 };
    for (int k = 0; k < 4; k++)
        CHECK((row[k] & 0xFF) == want_r[k],
              "nearest red at %d: got %u want %u", k, row[k] & 0xFF, want_r[k]);
}

/* At exactly 1:1 the fractional part is zero, all the weight lands on one tap,
 * and linear must come out bit-identical to nearest. This is the property that
 * keeps a UI layer sharp, and it is exact rather than approximate. */
static void test_bilinear_equals_nearest_at_1to1(void) {
    psp_ge_reset();
    clear_fb();
    upload_ramp_texture(16, 16);

    begin_list_vtype(VTYPE_2D_TEX);
    texture_state(TEX, 16, 4, 4, 3, 0, 1 /* linear */);
    vertex_uv(0, 40, 30,  0,  0, 0xFFFFFFFFu);
    vertex_uv(1, 53, 30, 13,  0, 0xFFFFFFFFu);
    vertex_uv(2, 40, 41,  0, 11, 0xFFFFFFFFu);
    vertex_uv(3, 53, 41, 13, 11, 0xFFFFFFFFu);
    cmd(0x04, (4u << 16) | 4);
    end_list();

    int bad = 0;
    for (int j = 0; j < 11 && bad < 4; j++)
        for (int k = 0; k < 13 && bad < 4; k++) {
            const uint32_t got = pixel(40 + k, 30 + j);
            if (got != ramp_texel(k, j)) {
                bad++;
                CHECK(0, "linear at 1:1 must equal nearest at (%d,%d):"
                         " got 0x%08X want 0x%08X", k, j, got, ramp_texel(k, j));
            }
        }
}

/* Depth commands, and a vertex carrying a z. The 16-bit position slot has one
 * and the plain `vertex` helper leaves it at zero, which is invisible until a
 * test actually turns the depth test on. */
#define ZTESTENABLE   0x23
#define ZTEST         0xDE
#define ZWRITEDISABLE 0xE7
#define CLEARMODE     0xD3
#define GEQUAL        7

static void vertex_z(int idx, int x, int y, int z, uint32_t rgba) {
    uint32_t a = VERTS + (uint32_t)idx * 12;
    psp_write32(a, rgba);
    psp_write16(a + 4, (uint16_t)x);
    psp_write16(a + 6, (uint16_t)y);
    psp_write16(a + 8, (uint16_t)z);
}

/* VADDR does not advance across PRIM: each one reads from the address last set,
 * so a second prim in the same list needs the pointer moved by hand. */
static void vaddr_at(int idx) {
    cmd(0x01, (VERTS + (uint32_t)idx * 12) & 0xFFFFFF);
}

static void depth_state(int func) {
    cmd(ZTESTENABLE, 1);
    cmd(ZTEST, (uint32_t)func);
    cmd(ZWRITEDISABLE, 0);           /* write enabled */
}

/* A clear-mode draw with the depth bit set must clear depth, and must do it
 * without consulting the depth test -- the clear establishes the values that
 * everything else is tested against, so testing it against the values it is
 * replacing makes it a no-op exactly when it matters.
 *
 * These lists are the shape the game uses: GEQUAL, where the larger z wins and
 * the clear goes to the near end. Draw far, clear to 0, draw nearer. With the
 * clear consulting the depth test the clear fails its own GEQUAL (0 >= 1000 is
 * false), depth stays at 1000, and the third draw is rejected (500 >= 1000 is
 * false) -- the screen keeps the first sprite and the clear silently did
 * nothing.
 *
 * Split across three lists because a list only runs at end_list(), so a check
 * between two draws has to be a check between two lists. GE state and the depth
 * buffer both persist across them; only psp_ge_reset clears either. */
static void test_clear_mode_clears_depth(void) {
    psp_ge_reset();
    clear_fb();

    /* Far geometry first, at z = 1000, into a depth buffer that starts at 0. */
    begin_list();
    depth_state(GEQUAL);
    vertex_z(0, 100, 50, 1000, 0xFF0000FFu);
    vertex_z(1, 200, 150, 1000, 0xFF0000FFu);
    cmd(0x04, (6u << 16) | 2);
    end_list();
    CHECK(pixel(150, 100) == 0x000000FFu, "z=1000 sprite should draw: 0x%08X",
          pixel(150, 100));

    /* A full-screen clear-mode sprite: colour and depth, z = 0. */
    begin_list();
    depth_state(GEQUAL);
    cmd(CLEARMODE, 1u | (1u << 8) | (1u << 10));   /* on, colour, depth */
    vertex_z(2, 0, 0, 0, 0xFF000000u);
    vertex_z(3, 480, 272, 0, 0xFF000000u);
    vaddr_at(2);
    cmd(0x04, (6u << 16) | 2);
    cmd(CLEARMODE, 0);
    end_list();
    CHECK(pixel(150, 100) == 0x00000000u,
          "the clear should repaint over the sprite: 0x%08X", pixel(150, 100));

    /* Nearer than the buffer's cleared value, farther than what it held before
     * the clear. It draws only if the clear actually landed. */
    begin_list();
    depth_state(GEQUAL);
    vertex_z(4, 120, 60, 500, 0xFF00FF00u);
    vertex_z(5, 180, 140, 500, 0xFF00FF00u);
    vaddr_at(4);
    cmd(0x04, (6u << 16) | 2);
    end_list();
    CHECK(pixel(150, 100) == 0x0000FF00u,
          "z=500 after a depth clear should draw: 0x%08X", pixel(150, 100));
}

/* The other half: outside clear mode the depth test is still obeyed, so the
 * bypass above is scoped to the clear rather than having disabled depth. Same
 * two draws, no clear between them. */
static void test_depth_test_still_rejects(void) {
    psp_ge_reset();
    clear_fb();

    begin_list();
    depth_state(GEQUAL);
    vertex_z(0, 100, 50, 1000, 0xFF0000FFu);
    vertex_z(1, 200, 150, 1000, 0xFF0000FFu);
    cmd(0x04, (6u << 16) | 2);
    end_list();

    begin_list();
    depth_state(GEQUAL);
    vertex_z(2, 120, 60, 500, 0xFF00FF00u);
    vertex_z(3, 180, 140, 500, 0xFF00FF00u);
    vaddr_at(2);
    cmd(0x04, (6u << 16) | 2);
    end_list();

    CHECK(pixel(150, 100) == 0x000000FFu,
          "z=500 behind z=1000 must be rejected under GEQUAL: 0x%08X",
          pixel(150, 100));
}

/* psp_ge_reset returns depth to its start-of-run contents. Without that, a
 * process that runs the GE twice -- this test binary, the oracle -- carries the
 * first run's depth into the second, and the second silently draws less. */
static void test_ge_reset_clears_depth(void) {
    psp_ge_reset();
    clear_fb();
    begin_list();
    depth_state(GEQUAL);
    vertex_z(0, 100, 50, 1000, 0xFF0000FFu);
    vertex_z(1, 200, 150, 1000, 0xFF0000FFu);
    cmd(0x04, (6u << 16) | 2);
    end_list();

    /* Fresh run: the z=1000 left behind above must not reject this. */
    psp_ge_reset();
    clear_fb();
    begin_list();
    depth_state(GEQUAL);
    vertex_z(0, 100, 50, 500, 0xFF00FF00u);
    vertex_z(1, 200, 150, 500, 0xFF00FF00u);
    cmd(0x04, (6u << 16) | 2);
    end_list();

    CHECK(pixel(150, 100) == 0x0000FF00u,
          "depth must not survive psp_ge_reset: 0x%08X", pixel(150, 100));
}

/* The backend interface itself. The software path is the reference every other
 * backend is diffed against, so selection has to be predictable: an unknown
 * name must not silently leave you rendering into nothing. */
static void test_backend_selection(void) {
    CHECK(psp_render_current() != NULL, "there is always a backend");
    CHECK(strcmp(psp_render_current()->name, "software") == 0,
          "software is the default, got %s", psp_render_current()->name);

    CHECK(psp_render_select("null") == 0, "null backend selectable");
    CHECK(strcmp(psp_render_current()->name, "null") == 0, "null is active");

    /* An unknown name keeps the current backend rather than falling back to
     * something arbitrary -- a typo should not silently change what renders. */
    CHECK(psp_render_select("vulkan-that-does-not-exist") != 0,
          "unknown backend rejected");
    CHECK(strcmp(psp_render_current()->name, "null") == 0,
          "rejected selection leaves the backend alone");

    CHECK(psp_render_select(NULL) != 0, "NULL name rejected");

    /* The null backend must draw nothing: it is what bring-up uses to ask
     * "did the game request a draw" without pixels confusing the answer. */
    psp_ge_reset();
    clear_fb();
    begin_list();
    vertex(0, 10, 10, 0xFFFFFFFFu);
    vertex(1, 60, 60, 0xFFFFFFFFu);
    cmd(0x04, (6u << 16) | 2);
    end_list();
    CHECK(psp_ge_pixels() == 0, "null backend wrote %llu pixels",
          (unsigned long long)psp_ge_pixels());

    CHECK(psp_render_select("software") == 0, "software reselectable");
}

int main(void) {
    if (psp_mem_init() != 0) { printf("memory init failed\n"); return 1; }
    psp_cpu_reset();
    psp_hle_init();
    psp_cpu.r[PSP_REG_SP] = 0x08F00000u;

    test_sprite();
    test_triangle();
    test_winding();
    test_clipping();
    test_transformed_is_skipped();
    test_triangle_strip();
    test_texture_1to1();
    test_texture_minified_samples_centre();
    test_sprite_texture_samples_centre();
    test_texture_wrap();
    test_texture_bilinear_midpoint();
    test_texture_filter_is_honoured();
    test_bilinear_equals_nearest_at_1to1();
    test_clear_mode_clears_depth();
    test_depth_test_still_rejects();
    test_ge_reset_clears_depth();
    test_backend_selection();

    psp_mem_free();
    printf(failures ? "raster: %d failure(s)\n" : "raster: all tests passed\n", failures);
    return failures ? 1 : 0;
}
