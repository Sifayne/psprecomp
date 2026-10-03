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
#include <math.h>
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
#define ZB     0x04088000u          /* depth, past a 512x272x4 colour buffer */
#define LIST   0x08800000u
#define VERTS  0x08810000u
#define INDICES 0x08830000u

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
    cmd(0x9E, ZB & 0xFFFFFF);                      /* ZBP */
    cmd(0x9F, ((ZB >> 8) & 0xFF0000) | 512);       /* ZBW */
    cmd(0x12, vtype);                              /* VTYPE */
    cmd(0x01, VERTS & 0xFFFFFF);                   /* VADDR */
}

static void begin_list(void) { begin_list_vtype(VTYPE_2D); }

static void end_list(void) {
    cmd(0x0F, 0);                    /* FINISH */
    cmd(0x0C, 0);                    /* END */
    call(0xAB49E76A, LIST, 0, 0, 0); /* sceGeListEnQueue */
    call(0xB287BD61, 0, 0, 0, 0);    /* sceGeDrawSync(WAIT): deferred GE drains here */
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

/* The colour buffer, and the depth buffer as a fresh start leaves it: the
 * depth lives in VRAM, so a test that wants none left over from the last one
 * zeroes it the way a new run's memory would be. */
static void clear_fb(void) {
    for (int y = 0; y < 272; y++)
        for (int x = 0; x < 480; x++)
            psp_write32(FB + (uint32_t)(y * 480 + x) * 4, 0);
    for (uint32_t a = 0; a < 512u * 272u * 2u; a += 4)
        psp_write32(ZB + a, 0);
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

/* PRIM and BBOX leave VADDR past the vertices they read. geprobe 5 scene 33
 * (fw 6.60) draws a marker with a PRIM straight after BBOX and no VADDR, and
 * the marker is the vertices after the box; libgu's sceGuDrawArrayN sends
 * one VADDR for several PRIMs. */
static void test_vertex_pointer_advances(void) {
    psp_ge_reset();
    clear_fb();
    begin_list();
    vertex(0, 0, 0, 0xFFFFFFFFu);            /* the "box": BBOX reads these two */
    vertex(1, 479, 271, 0xFFFFFFFFu);
    vertex(2, 10, 10, 0xFF0000FFu);          /* first PRIM */
    vertex(3, 20, 20, 0xFF0000FFu);
    vertex(4, 30, 10, 0xFF00FF00u);          /* second PRIM, no VADDR between */
    vertex(5, 40, 20, 0xFF00FF00u);
    cmd(0x07, 2);                    /* BBOX, 2 vertices */
    cmd(0x04, (6u << 16) | 2);       /* PRIM sprites */
    cmd(0x04, (6u << 16) | 2);       /* PRIM sprites */
    end_list();
    CHECK(pixel(15, 15) == 0x000000FFu, "PRIM after BBOX reads past the box: 0x%08X", pixel(15, 15));
    CHECK(pixel(35, 15) == 0x0000FF00u, "second PRIM reads past the first: 0x%08X", pixel(35, 15));
    CHECK(pixel(100, 100) == 0, "the box itself is not drawn: 0x%08X", pixel(100, 100));
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

/* A lit vertex's secondary (specular) colour is interpolated on its own and
 * added to the pixel, the sum clamped there and not at the vertex (geprobe 7
 * scene 16, fw 6.60). Red 255 in both colours at the left corner and 0 at the
 * others is 255 over the left half of the base and about 160 a third of the
 * way in; the two summed and clamped at the vertex would give 255 and 80. */
static void test_secondary_colour(void) {
    psp_ge_reset();
    clear_fb();
    const psp_render_backend *be = psp_render_current();
    psp_blend_state blend = { .write_colour = 1 };
    be->set_target(FB, 480, 3);
    be->set_scissor(0, 0, 479, 271);
    be->set_texture(&(psp_tex_state){ 0 });
    be->set_depth(0, 1, 0);
    be->set_blend(&blend);
    be->set_fog(0, 0);
    psp_vertex tri[3] = {
        { .x =  40 * PSP_SUBPX, .y = 100 * PSP_SUBPX, .rgba = 0xFF0000FFu, .inv_w = 1, .tex_q = 1,
          .fog = 255, .spec = 0x0000FFu, .spec_set = 1 },
        { .x = 340 * PSP_SUBPX, .y = 100 * PSP_SUBPX, .rgba = 0xFF000000u, .inv_w = 1, .tex_q = 1, .fog = 255 },
        { .x =  40 * PSP_SUBPX, .y = 130 * PSP_SUBPX, .rgba = 0xFF000000u, .inv_w = 1, .tex_q = 1, .fog = 255 },
    };
    be->draw(PSP_PRIM_TRIANGLES, tri, 3);
    CHECK((pixel(100, 100) & 0xFF) == 255, "the summed red saturates near the corner: %06X", pixel(100, 100));
    const uint32_t r = pixel(240, 100) & 0xFF;
    CHECK(r >= 158 && r <= 163, "two planes from 255 summed, not one from 255, at (240,100): %u", r);
}

/* World geometry must not use the through-mode affine UV rule.  These three
 * triangles have identical screen-space coordinates and texcoords.  The first
 * has its far edge at four times the clip-space W, so the sample a quarter of
 * the way across lands near texel 1 rather than the affine texel 4.  The
 * second pins the control (all homogeneous terms one), and the third checks
 * texture-matrix projection's separate Q denominator.
 *
 * This drives the backend contract directly.  Matrix decoding and clipping
 * have their own tests; the failure this guards is losing W at the seam
 * between the GE and a rasterizer, which made floors and roads visibly pull
 * toward the camera. */
static void test_texture_perspective_interpolation(void) {
    psp_ge_reset();
    clear_fb();
    upload_ramp_texture(16, 16);

    const psp_render_backend *be = psp_render_current();
    psp_tex_state tex = {
        .addr = TEX, .stride = 16, .w = 16, .h = 16,
        .fmt = 3, .func = 0, .min_filter = 0, .mag_filter = 0,
    };
    psp_blend_state blend = { .write_colour = 1 };
    be->set_target(FB, 480, 3);
    be->set_scissor(0, 0, 479, 271);
    be->set_texture(&tex);
    be->set_depth(0, 1, 0);
    be->set_blend(&blend);
    be->set_fog(0, 0);

    psp_vertex tri[3] = {
        { .x =  40 * PSP_SUBPX, .y =  30 * PSP_SUBPX, .rgba = 0xFFFFFFFFu,
          .u =  0.0f, .v =  0.0f, .inv_w = 1.00f, .tex_q = 1.0f, .fog = 255 },
        { .x = 140 * PSP_SUBPX, .y =  30 * PSP_SUBPX, .rgba = 0xFFFFFFFFu,
          .u = 16.0f, .v =  0.0f, .inv_w = 0.25f, .tex_q = 1.0f, .fog = 255 },
        { .x =  40 * PSP_SUBPX, .y = 130 * PSP_SUBPX, .rgba = 0xFFFFFFFFu,
          .u =  0.0f, .v = 16.0f, .inv_w = 0.25f, .tex_q = 1.0f, .fog = 255 },
    };
    be->draw(PSP_PRIM_TRIANGLES, tri, 3);
    CHECK(pixel(65, 55) == ramp_texel(1, 1),
          "perspective UV at (65,55): got 0x%08X want texel (1,1)",
          pixel(65, 55));

    for (int i = 0; i < 3; i++) {
        tri[i].x += 140 * PSP_SUBPX;
        tri[i].inv_w = 1.0f;
    }
    be->draw(PSP_PRIM_TRIANGLES, tri, 3);
    CHECK(pixel(205, 55) == ramp_texel(4, 4),
          "affine control at (205,55): got 0x%08X want texel (4,4)",
          pixel(205, 55));

    for (int i = 0; i < 3; i++) tri[i].x += 140 * PSP_SUBPX;
    tri[1].tex_q = tri[2].tex_q = 4.0f;
    be->draw(PSP_PRIM_TRIANGLES, tri, 3);
    CHECK(pixel(345, 55) == ramp_texel(1, 1),
          "projective Q at (345,55): got 0x%08X want texel (1,1)",
          pixel(345, 55));
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

/* LOD is backend-independent GE arithmetic. A GPU backend must not let its
 * driver recompute this from derivatives: CONST and SLOPE do not use them,
 * AUTO is floored to a sixteenth before the signed bias is added, and mode 3
 * was measured to behave like CONST. */
static void test_texture_lod_rules(void) {
    psp_tex_state t = { 0 };

    t.lod_mode = 0;                              /* AUTO */
    CHECK(psp_render_lod16(&t, 4.0f) == 32,
          "auto LOD at 4 texels/pixel: %d", psp_render_lod16(&t, 4.0f));
    CHECK(psp_render_lod16(&t, 0.5f) == -16,
          "auto LOD at half a texel/pixel: %d", psp_render_lod16(&t, 0.5f));
    t.lod_bias16 = -7;
    CHECK(psp_render_lod16(&t, 4.0f) == 25,
          "auto LOD applies signed bias after quantising: %d",
          psp_render_lod16(&t, 4.0f));

    t.lod_mode = 1;                              /* CONST */
    CHECK(psp_render_lod16(&t, 123.0f) == -7,
          "constant LOD ignores the gradient: %d", psp_render_lod16(&t, 123.0f));
    t.lod_mode = 3;                              /* undefined, measured as CONST */
    CHECK(psp_render_lod16(&t, 123.0f) == -7,
          "mode 3 follows constant LOD: %d", psp_render_lod16(&t, 123.0f));

    t.lod_mode = 2;                              /* SLOPE */
    t.lod_bias16 = -3;
    t.lod_slope = 2.25f;
    CHECK(psp_render_lod16(&t, 123.0f) == 33,
          "slope LOD plus bias: %d", psp_render_lod16(&t, 123.0f));
    t.lod_bias16 = 0;
    t.lod_slope = -0.1f;
    CHECK(psp_render_lod16(&t, 1.0f) == -2,
          "negative slope LOD floors rather than truncates: %d",
          psp_render_lod16(&t, 1.0f));
}

/* A real two-level chain, not just the LOD arithmetic. Level zero is black,
 * level one red=240, and mip-linear at +8/16 must land exactly halfway. This
 * pins the independent addresses/sizes and the fractional level blend that
 * the GL cache and shader now consume through the same backend contract. */
static void test_texture_mip_chain(void) {
    enum { MIP1 = TEX + 0x1000 };
    psp_ge_reset();
    clear_fb();
    for (int i = 0; i < 16; i++) psp_write32(TEX + (uint32_t)i * 4, 0xFF000000u);
    for (int i = 0; i < 4; i++) psp_write32(MIP1 + (uint32_t)i * 4, 0xFF0000F0u);

    const psp_render_backend *be = psp_render_current();
    psp_tex_state tex = {
        .addr = TEX, .stride = 4, .w = 4, .h = 4,
        .fmt = 3, .func = 0, .min_filter = 6, .mag_filter = 0,
        .wrap_s = 1, .wrap_t = 1, .max_level = 1,
        .lod_mode = 1, .lod_bias16 = 8,
    };
    tex.lv_addr[0] = TEX;  tex.lv_stride[0] = 4;
    tex.lv_w[0] = 4;       tex.lv_h[0] = 4;
    tex.lv_addr[1] = MIP1; tex.lv_stride[1] = 2;
    tex.lv_w[1] = 2;       tex.lv_h[1] = 2;
    psp_blend_state blend = { .write_colour = 1 };
    psp_vertex sprite[2] = {
        { .x = 40 * PSP_SUBPX, .y = 30 * PSP_SUBPX, .rgba = 0xFFFFFFFFu,
          .u = 0.0f, .v = 0.0f, .inv_w = 1.0f, .tex_q = 1.0f, .fog = 255 },
        { .x = 44 * PSP_SUBPX, .y = 34 * PSP_SUBPX, .rgba = 0xFFFFFFFFu,
          .u = 4.0f, .v = 4.0f, .inv_w = 1.0f, .tex_q = 1.0f, .fog = 255 },
    };
    be->set_target(FB, 480, 3);
    be->set_scissor(0, 0, 479, 271);
    be->set_texture(&tex);
    be->set_depth(0, 1, 0);
    be->set_blend(&blend);
    be->set_fog(0, 0);
    be->draw(PSP_PRIM_SPRITES, sprite, 2);

    CHECK((pixel(41, 31) & 0xFFu) == 120,
          "halfway between mip levels: red=%u want 120", pixel(41, 31) & 0xFFu);
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

/* Depth is 16 bits a pixel in VRAM at ZBP, where the CPU can read it, in the
 * layout geprobe 2 scene 17 (fw 6.60) dumped for ZBP 0x88000, ZBW 512: the
 * value for pixel (x, y) sits at the linear offset with address bits 5-9
 * rotated up by one and bits 6 and 13 inverted. (150, 100): linear offset
 * 0x88000 + (100 * 512 + 150) * 2 = 0x9912C; bits 5-9 are 0b01001, rotated
 * 0b10010, so 0x9924C; inverted, 0x9B20C. */
static void test_depth_in_vram(void) {
    psp_ge_reset();
    clear_fb();
    begin_list();
    depth_state(GEQUAL);
    vertex_z(0, 100, 50, 1000, 0xFF0000FFu);
    vertex_z(1, 200, 150, 1000, 0xFF0000FFu);
    cmd(0x04, (6u << 16) | 2);
    end_list();
    CHECK(psp_read16(0x0409B20Cu) == 1000, "depth of (150,100) at VRAM 0x9B20C: %u",
          psp_read16(0x0409B20Cu));
    CHECK(psp_read16(0x0409912Cu) == 0, "and not at its linear address: %u",
          psp_read16(0x0409912Cu));

    /* A fresh start's zeroed VRAM is a fresh depth buffer. */
    psp_ge_reset();
    clear_fb();
    begin_list();
    depth_state(GEQUAL);
    vertex_z(0, 100, 50, 500, 0xFF00FF00u);
    vertex_z(1, 200, 150, 500, 0xFF00FF00u);
    cmd(0x04, (6u << 16) | 2);
    end_list();
    CHECK(pixel(150, 100) == 0x0000FF00u,
          "zeroed VRAM rejects nothing under GEQUAL: 0x%08X", pixel(150, 100));
}

/* The depth at (x, y), through the VRAM layout test_depth_in_vram describes. */
static uint16_t depth_at(int x, int y) {
    const uint32_t l = 0x88000u + (uint32_t)(y * 512 + x) * 2;
    const uint32_t mid = (l >> 5) & 0x1F, rot = ((mid << 1) | (mid >> 4)) & 0x1F;
    return psp_read16(0x04000000u + (((l & ~(0x1Fu << 5)) | (rot << 5)) ^ 0x2040u));
}

/* Depth across a triangle is a plane with the GE's 1/area (render.c
 * area_rcp). geprobe 5 (fw 6.60) scene 27's four through-mode triangles,
 * drawn the same way, and pixels of each that the float blend this replaced
 * had one off, with what the hardware wrote there. */
static void test_depth_plane(void) {
    static const int T[12][3] = {
        { 10, 10, 0 },      { 230, 10, 65535 },  { 10, 90, 30000 },
        { 230, 20, 1000 },  { 230, 100, 1003 },  { 20, 100, 1010 },
        { 10, 110, 12345 }, { 230, 110, 12345 }, { 120, 170, 12345 },
        { 10, 175, 0 },     { 230, 175, 0 },     { 120, 200, 65535 } };
    static const int P[][3] = {
        { 49, 11, 12328 },  { 40, 26, 15272 },   { 128, 42, 47486 },  { 31, 80, 32841 },
        { 208, 29, 1000 },  { 215, 63, 1001 },   { 208, 83, 1002 },   { 223, 98, 1002 },
        { 16, 111, 12345 }, { 212, 113, 12345 }, { 209, 119, 12345 }, { 159, 141, 12345 },
        { 43, 181, 17038 }, { 74, 186, 30145 },  { 140, 189, 38009 }, { 126, 197, 58980 } };
    psp_ge_reset();
    clear_fb();
    begin_list();
    depth_state(1);                  /* ALWAYS, writes on */
    for (int i = 0; i < 12; i++) vertex_z(i, T[i][0], T[i][1], T[i][2], 0xFFFFFFFFu);
    cmd(0x04, (3u << 16) | 12);      /* PRIM: triangles */
    end_list();
    for (unsigned i = 0; i < sizeof P / sizeof P[0]; i++)
        CHECK(depth_at(P[i][0], P[i][1]) == P[i][2], "depth at (%d,%d): %u, hardware %d",
              P[i][0], P[i][1], depth_at(P[i][0], P[i][1]), P[i][2]);
}

/* A 16-bit through-mode vertex far off screen saturates to the 12.4 range
 * and anchors the colour plane: geprobe 5 scene 28 (fw 6.60), its second
 * band, with the pixels the PSP wrote. */
static void test_far_vertex_16bit(void) {
    psp_ge_reset(); clear_fb(); begin_list();
    vertex(0, -5000, 65, 0xFF0000FFu); vertex(1, 230, 120, 0xFF00FF00u); vertex(2, 230, 65, 0xFFFF0000u);
    cmd(0x04, (3u << 16) | 3);
    end_list();
    CHECK(pixel(0, 112) == 0x07DC18 && pixel(100, 112) == 0x12DC0D && pixel(200, 112) == 0x1EDC02 &&
          pixel(0, 114) == 0,
          "far 16-bit vertex: %06X %06X %06X %06X, hardware 07DC18 12DC0D 1EDC02 000000",
          pixel(0, 112), pixel(100, 112), pixel(200, 112), pixel(0, 114));
}

static void float_vertex(int i, float x, float y, float z);

static void cmd_float(uint8_t op, float f) {
    uint32_t bits;
    memcpy(&bits, &f, 4);
    cmd(op, bits >> 8);
}

/* A transformed vertex's depth is computed in the GE's 16-bit-significand
 * float (ge.c ge_screen_z). geprobe 5 (fw 6.60) scenes 17 and 27 under
 * sceGumPerspective(60, 480/272, 1, 100) and sceGuDepthRange(65535, 0): eye
 * z -4.5, -5.5 and -8 give 14049, 11374 and 7612, where a float computation
 * gives 14048.22, 11373.64 and 7612.5. */
static void test_transformed_depth(void) {
    static const float PROJ[16] = { 0.981491089f, 0, 0, 0, 0, 1.73202515f, 0, 0,
                                    0, 0, -1.02017212f, -1, 0, 0, -2.0201416f, 0 };
    static const struct { float z; int depth; } Z[] = { { -4.5f, 14049 }, { -5.5f, 11374 }, { -8.0f, 7612 } };
    for (unsigned k = 0; k < sizeof Z / sizeof Z[0]; k++) {
        psp_ge_reset(); clear_fb();
        begin_list_vtype((7u << 2) | (3u << 7));
        for (int m = 0; m < 2; m++) {                          /* world, view: identity */
            cmd((uint8_t)(0x3A + 2 * m), 0);
            for (int i = 0; i < 12; i++) cmd((uint8_t)(0x3B + 2 * m), i % 4 == 0 ? 0x3F8000 : 0);
        }
        cmd(0x3E, 0);
        for (int i = 0; i < 16; i++) cmd_float(0x3F, PROJ[i]);
        cmd_float(0x42, 240.0f); cmd_float(0x43, -136.0f); cmd_float(0x44, -32768.0f);
        cmd_float(0x45, 2048.0f); cmd_float(0x46, 2048.0f); cmd_float(0x47, 32767.0f);
        cmd(0x4C, 1808u << 4); cmd(0x4D, 1912u << 4);
        depth_state(1);                                        /* ALWAYS, writes on */
        float_vertex(0, -1, -1, Z[k].z); float_vertex(1, 1, -1, Z[k].z); float_vertex(2, 0, 1, Z[k].z);
        cmd(0x04, (3u << 16) | 3);
        end_list();
        CHECK(depth_at(240, 140) == Z[k].depth, "eye z %.1f: depth %u, hardware %d",
              (double)Z[k].z, depth_at(240, 140), Z[k].depth);
    }
}

/* One point at eye (0, 0, z) through world W, identity view and
 * projection P (column-major, as the GE takes them), viewport z scale zs
 * and centre zc: the depth it writes at the screen centre. */
static unsigned point_depth(const float W[12], const float P[16], float z, float zs, float zc) {
    psp_ge_reset(); clear_fb();
    begin_list_vtype((7u << 2) | (3u << 7));
    cmd(0x3A, 0);
    for (int i = 0; i < 12; i++) cmd_float(0x3B, W[i]);
    cmd(0x3C, 0);
    for (int i = 0; i < 12; i++) cmd(0x3D, i % 4 == 0 ? 0x3F8000 : 0);
    cmd(0x3E, 0);
    for (int i = 0; i < 16; i++) cmd_float(0x3F, P[i]);
    cmd_float(0x42, 240.0f); cmd_float(0x43, -136.0f); cmd_float(0x44, zs);
    cmd_float(0x45, 2048.0f); cmd_float(0x46, 2048.0f); cmd_float(0x47, zc);
    cmd(0x4C, 1808u << 4); cmd(0x4D, 1912u << 4);
    depth_state(1);                                        /* ALWAYS, writes on */
    float_vertex(0, 0, 0, z);
    cmd(0x04, (0u << 16) | 1);                             /* one point */
    end_list();
    return depth_at(240, 136);
}

static float bits_float(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }

/* A vertex's depth in the GE's own arithmetic (ge.c ge_sum, ge_rcp16,
 * ge_wvp): points the PSP drew in geprobe 11 and 12 (fw 6.60). Viewport z
 * centre 0 and scale 65536 make the depth clip z / w's top 16 bits.
 *  - Scene 66, the probes' perspective: two eye z where m10 z carries into
 *    the next exponent, so the sum lines it up by its operands' exponents.
 *  - Scene 62, clip z a power of two over w = -z: the 1/w table, a step
 *    above 65536 / w at 1.0859375 (60349.7) and in the binade 2^7.
 *  - Scene 61, a = -0.3 and a world translation of -37.125: the GE folds
 *    the translation into the projection's row, and these read 3 above and
 *    4 below the 22937 an eye z formed first gives.
 *  - Scene 65 batch 0: world translation 41.32 with w = 1. */
static void test_vertex_depth_ge(void) {
    static const float ID[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 };
    static const float PERSP[16] = { 0.981491089f, 0, 0, 0, 0, 1.73202515f, 0, 0,
                                     0, 0, -1.02017212f, -1, 0, 0, -2.0201416f, 0 };
    static const float RCP1[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, -1, 0, 0, 1, 0 };
    static const float RCP128[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, -1, 0, 0, 128, 0 };
    static const float RATIO[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -0.3f, -1, 0, 0, 0, 0 };
    static const float W61[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, -37.125f };
    static const float P65[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1.020538330078125f, 0, 0, 0, 0, 1 };
    static const float W65[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 41.322265625f };
    static const struct { const float *w, *p; uint32_t z; float zs, zc; unsigned hw; } C[] = {
        { ID,  PERSP,  0xC0FB4351u, 65536.0f, 0.0f,      49996 },
        { ID,  PERSP,  0xC17F3FCBu, 65536.0f, 0.0f,      58559 },
        { ID,  RCP1,   0xBF8B0000u, 65536.0f, 0.0f,      60350 },
        { ID,  RCP1,   0xBF955505u, 65536.0f, 0.0f,      56175 },
        { ID,  RCP128, 0xC319901Cu, 65536.0f, 0.0f,      54627 },
        { W61, RATIO,  0x42107D9Eu, -32768.0f, 32767.0f, 22940 },
        { W61, RATIO,  0x42107CD2u, -32768.0f, 32767.0f, 22933 },
        { W65, P65,    0xC22348E8u, 65536.0f, 0.0f,      33600 },
    };
    for (unsigned k = 0; k < sizeof C / sizeof C[0]; k++) {
        const unsigned d = point_depth(C[k].w, C[k].p, bits_float(C[k].z), C[k].zs, C[k].zc);
        CHECK(d == C[k].hw, "case %u (z %08X): depth %u, hardware %u", k, C[k].z, d, C[k].hw);
    }
}

/* Skinning and morphing in the GE's own arithmetic (ge.c ge_acc,
 * ge_skin_axis): points the PSP drew (or did not) in geprobe 16 (fw 6.60),
 * scenes 110-114. The projection's z row is 2^k times one model axis, w is
 * 1 and the viewport z scale +-65536, so a point's depth is that skinned
 * coordinate's top 16 bits; the screen offset puts the point at about
 * (240, 136). Vertex words, bone and morph command words exactly as the
 * probe sent them. The last case's skinned y is 2.1 w: the PSP drops a
 * point outside the clip volume's x and y planes. */
static void test_skin_ge(void) {
    static const struct {
        uint32_t vtype; int nv; uint32_t v[16];
        int nb; uint32_t bone[3][12];
        int nm; uint32_t mw[4];
        int axis, k; float zs; int ox, oy, hw;
    } C[] = {
        /* 110 b1: two bones, float weights (point 160); float skinning gave 41861 */
        { 0x00479Cu, 6,
              { 0x3ED65300u, 0x3F14D600u, 0xFF0000FFu, 0xBEF16644u, 0x3EC09DF0u, 0x3F25523Cu },
          2, { { 0x3F8000, 0x000000, 0x3CD7C3, 0x000000, 0x3F8000, 0x3C81A8, 0x000000, 0x000000, 0x3F18CF, 0x000000, 0x000000, 0x3E80CF },
                { 0x3F8000, 0x000000, 0xBC8C7B, 0x000000, 0x3F8000, 0x3C9F8F, 0x000000, 0x000000, 0x3F0A63, 0x000000, 0x000000, 0x3E8F6D } },
          0, { 0 }, 2, 0, 65536.0f, 1694, 1860, 41859 },
        /* 111 b0: 8-bit weights 0xFF, 0 (point 0); float skinning gave 38950 */
        { 0x00439Cu, 5,
              { 0x000000FFu, 0xFF0000FFu, 0xBE86AFA3u, 0xBE822BCDu, 0x3F0695CCu },
          2, { { 0x3F8000, 0x000000, 0xBCA499, 0x000000, 0x3F8000, 0xBCE16D, 0x000000, 0x000000, 0x3F1654, 0x000000, 0x000000, 0x3E8D24 },
                { 0x3F8000, 0x000000, 0xBCF8B0, 0x000000, 0x3F8000, 0x3CD005, 0x000000, 0x000000, 0x3F0D4E, 0x000000, 0x000000, 0x3E8778 } },
          0, { 0 }, 2, -1, 65536.0f, 1682, 1980, 38948 },
        /* 114 b2: four morph sets (point 321); float skinning gave 49650 */
        { 0x0C019Cu, 16,
              { 0xFF0000FFu, 0xBE959D92u, 0xBEDC2439u, 0x3F2DA034u, 0xFF0000FFu, 0x3EACBAABu,
                0xBEDE3A7Au, 0x3F30E6B0u, 0xFF0000FFu, 0x3EDED44Cu, 0x3EC73CF4u, 0x3F58AE76u,
                0xFF0000FFu, 0x3E9A9237u, 0x3EC0B409u, 0x3F5A6C27u },
          0, { { 0 } },
          4, { 0x3DCDF5, 0x3EF049, 0x3E8DFC, 0x3E1C79 }, 2, 0, 65536.0f, 1878, 1923, 49649 },
        /* 114 b3: morph sets with two bones each (point 480); float skinning gave 46938 */
        { 0x04479Cu, 12,
              { 0x3F2425A2u, 0x3EB7B4BCu, 0xFF0000FFu, 0x3EF975A4u, 0xBEF4A3B2u, 0x3F49BEFBu,
                0x3EE3C29Cu, 0x3F0E1EB2u, 0xFF0000FFu, 0x3E97B35Fu, 0x3E92B16Cu, 0x3F4826EDu },
          2, { { 0x3F8000, 0x000000, 0xBCE898, 0x000000, 0x3F8000, 0xBC88BD, 0x000000, 0x000000, 0x3F06A0, 0x000000, 0x000000, 0x3E9322 },
                { 0x3F8000, 0x000000, 0x3CE3E3, 0x000000, 0x3F8000, 0xBCD4E5, 0x000000, 0x000000, 0x3F0E7C, 0x000000, 0x000000, 0x3E9589 } },
          2, { 0x3F53C0, 0x3E30FF }, 2, 0, 65536.0f, 1917, 1959, 46967 },
        /* 110 b8: clip y beyond w, not drawn (point 1269); float skinning gave 40960 */
        { 0x00879Cu, 7,
              { 0x3E4CCCCDu, 0x3E99999Au, 0x3F000000u, 0xFF0000FFu, 0x3FB80000u, 0x3F99999Au,
                0xC0A00000u },
          3, { { 0x3F8000, 0x000000, 0x000000, 0x000000, 0x3F8000, 0x000000, 0x000000, 0x000000, 0x3F8000, 0x000000, 0x000000, 0x000000 },
                { 0x3F8000, 0x000000, 0x000000, 0x000000, 0x3F8000, 0x000000, 0x000000, 0x000000, 0x3F8000, 0x3FC000, 0x3F4CCC, 0x000000 },
                { 0x000000, 0x3F8000, 0x000000, 0xBF8000, 0x000000, 0x000000, 0x000000, 0x000000, 0x3F8000, 0x000000, 0x000000, 0x000000 } },
          0, { 0 }, 2, -3, -65536.0f, 1944, 1700, -1 },
    };
    for (unsigned c = 0; c < sizeof C / sizeof C[0]; c++) {
        psp_ge_reset(); clear_fb();
        begin_list_vtype(C[c].vtype);
        cmd(0x3A, 0);
        for (int i = 0; i < 12; i++) cmd(0x3B, i % 4 == 0 ? 0x3F8000 : 0);
        cmd(0x3C, 0);
        for (int i = 0; i < 12; i++) cmd(0x3D, i % 4 == 0 ? 0x3F8000 : 0);
        float P[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
        P[C[c].axis * 4 + 2] = ldexpf(1.0f, C[c].k);
        cmd(0x3E, 0);
        for (int i = 0; i < 16; i++) cmd_float(0x3F, P[i]);
        cmd_float(0x42, 240.0f); cmd_float(0x43, -136.0f); cmd_float(0x44, C[c].zs);
        cmd_float(0x45, 2048.0f); cmd_float(0x46, 2048.0f); cmd_float(0x47, 0.0f);
        cmd(0x4C, (uint32_t)C[c].ox << 4); cmd(0x4D, (uint32_t)C[c].oy << 4);
        for (int b = 0; b < C[c].nb; b++) {
            cmd(0x2A, (uint32_t)(12 * b));
            for (int i = 0; i < 12; i++) cmd(0x2B, C[c].bone[b][i]);
        }
        for (int m = 0; m < C[c].nm; m++) cmd((uint8_t)(0x2C + m), C[c].mw[m]);
        depth_state(1);                                    /* ALWAYS, writes on */
        for (int i = 0; i < C[c].nv; i++) psp_write32(VERTS + 4u * (uint32_t)i, C[c].v[i]);
        cmd(0x04, (0u << 16) | 1);                         /* one point */
        end_list();
        int fx = -1, fy = -1;
        for (int y = 133; y <= 139; y++)
            for (int x = 237; x <= 243; x++)
                if (pixel(x, y)) { fx = x; fy = y; }
        if (C[c].hw < 0)
            CHECK(fx < 0, "case %u: drawn at (%d,%d), the PSP drew nothing", c, fx, fy);
        else
            CHECK(fx >= 0 && depth_at(fx, fy) == C[c].hw, "case %u: depth %d, hardware %d", c,
                  fx >= 0 ? depth_at(fx, fy) : -1, C[c].hw);
    }
}

/* A spline point's colour is de Boor's algorithm on the control colours
 * with 8-bit parameters and 1/128 cuts (ge.c deboor_fix), not a blend by
 * exact weights. geprobe 9 (fw 6.60) scene 54: a uniform span (fill/fill,
 * 4 columns) at 48 steps, column 1 red 255, v open/open over 4 rows at one
 * division; the PSP's red at each of the 49 points, 17 of which the exact
 * weights put a step low. Identity matrices, so control x -0.8 .. 0.4 puts
 * the points between pixels 144 and 240 of the row at y 0.4963 (pixel 68). */
static void test_spline_colour(void) {
    static const int HW[49] = { 171, 171, 170, 170, 169, 168, 167, 166, 165, 163, 161, 159, 157, 155, 152, 150, 148,
                                145, 142, 139, 136, 133, 130, 126, 123, 119, 116, 112, 108, 105, 102, 98, 94, 91, 87,
                                84, 80, 76, 74, 70, 67, 63, 61, 57, 54, 51, 48, 46, 43 };
    psp_ge_reset(); clear_fb();
    begin_list_vtype((7u << 2) | (3u << 7));
    for (int m = 0; m < 3; m++) {                          /* world, view, projection: identity */
        cmd((uint8_t)(0x3A + 2 * m), 0);
        const int n = m < 2 ? 12 : 16, rowlen = m < 2 ? 3 : 4;
        for (int i = 0; i < n; i++) cmd((uint8_t)(0x3B + 2 * m), i % (rowlen + 1) == 0 ? 0x3F8000 : 0);
    }
    cmd_float(0x42, 240.0f); cmd_float(0x43, -136.0f); cmd_float(0x44, -32768.0f);
    cmd_float(0x45, 2048.0f); cmd_float(0x46, 2048.0f); cmd_float(0x47, 32767.0f);
    cmd(0x4C, 1808u << 4); cmd(0x4D, 1912u << 4);
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++) {
            const int v = j * 4 + i;
            float_vertex(v, -0.8f + 0.4f * (float)i, 0.4963f - 0.03f * (float)j, 0.5f);
            psp_write32(VERTS + (uint32_t)v * 16, i == 1 ? 0xFF0000FFu : 0xFF000000u);
        }
    cmd(0x36, 48u | (1u << 8));                            /* PATCHDIVISION */
    cmd(0x37, 2);                                          /* PATCHPRIMITIVE: points */
    cmd(0x06, 4u | (4u << 8) | (0u << 16) | (3u << 18));   /* SPLINE 4x4, u fill/fill, v open/open */
    end_list();
    int n = 0, bad = 0, first_bad = -1;
    for (int x = 100; x < 300 && n < 49; x++) {
        const uint32_t p = pixel(x, 68);
        if (!p) continue;
        if ((int)(p & 0xFF) != HW[n] && first_bad < 0) first_bad = n;
        bad += (int)(p & 0xFF) != HW[n];
        n++;
    }
    CHECK(n == 49 && bad == 0, "spline points: %d found, %d reds off the PSP's (first at %d)", n, bad, first_bad);
}

/* A Bezier patch's points in the GE's own arithmetic (ge.c draw_patch,
 * la_lerp, patch_param): geprobe 13 (fw 6.60) scene 68 item 60, a 4x4
 * strip at division 21 (1 down) as points, identity matrices, viewport z
 * scale 65536 and centre 0 so each depth is the point's z's top 16 bits.
 * The PSP's 44 points, pixel and depth. Division 21 is the smallest where
 * the GE's step (256/21 in 8.6, rounded up) leaves floor(256 i/21): t 61,
 * 122, 134 and 195, not 60, 121, 135 and 196. */
static void test_patch_points_ge(void) {
    static const uint32_t C[16][3] = {
        { 0x3F1A8000u, 0xBDE80000u, 0x3F02BB00u }, { 0x3F2A8000u, 0xBDE80000u, 0x3F0CB800u }, { 0x3F3A8000u, 0xBDE80000u, 0x3F1AA400u }, { 0x3F4A8000u, 0xBDE80000u, 0x3F1D2000u },
        { 0x3F1A8000u, 0xBDF80000u, 0x3F02BB00u }, { 0x3F2A8000u, 0xBDF80000u, 0x3F0CB800u }, { 0x3F3A8000u, 0xBDF80000u, 0x3F1AA400u }, { 0x3F4A8000u, 0xBDF80000u, 0x3F1D2000u },
        { 0x3F1A8000u, 0xBE040000u, 0x3F0C5000u }, { 0x3F2A8000u, 0xBE040000u, 0x3F120200u }, { 0x3F3A8000u, 0xBE040000u, 0x3F4E2900u }, { 0x3F4A8000u, 0xBE040000u, 0x3F7DA100u },
        { 0x3F1A8000u, 0xBE0C0000u, 0x3F0C5000u }, { 0x3F2A8000u, 0xBE0C0000u, 0x3F120200u }, { 0x3F3A8000u, 0xBE0C0000u, 0x3F4E2900u }, { 0x3F4A8000u, 0xBE0C0000u, 0x3F7DA100u } };
    static const struct { int x, y; unsigned z; } HW[44] = {
        { 394, 150, 33467 }, { 396, 150, 33831 }, { 399, 150, 34208 }, { 401, 150, 34593 },
        { 403, 150, 34984 }, { 405, 150, 35412 }, { 408, 150, 35807 }, { 410, 150, 36202 },
        { 412, 150, 36591 }, { 414, 150, 36976 }, { 417, 150, 37382 }, { 419, 150, 37744 },
        { 422, 150, 38122 }, { 424, 150, 38453 }, { 426, 150, 38765 }, { 428, 150, 39055 },
        { 431, 150, 39322 }, { 433, 150, 39581 }, { 435, 150, 39791 }, { 438, 150, 39970 },
        { 440, 150, 40114 }, { 442, 150, 40224 }, { 394, 153, 35920 }, { 396, 153, 36213 },
        { 399, 153, 36682 }, { 401, 153, 37313 }, { 403, 153, 38096 }, { 405, 153, 39102 },
        { 408, 153, 40168 }, { 410, 153, 41352 }, { 412, 153, 42645 }, { 414, 153, 44036 },
        { 417, 153, 45641 }, { 419, 153, 47202 }, { 422, 153, 48966 }, { 424, 153, 50651 },
        { 426, 153, 52379 }, { 428, 153, 54138 }, { 431, 153, 55920 }, { 433, 153, 57863 },
        { 435, 153, 59656 }, { 438, 153, 61438 }, { 440, 153, 63199 }, { 442, 153, 64929 } };
    psp_ge_reset(); clear_fb();
    begin_list_vtype((7u << 2) | (3u << 7));
    for (int m = 0; m < 3; m++) {
        cmd((uint8_t)(0x3A + 2 * m), 0);
        const int n = m < 2 ? 12 : 16, rowlen = m < 2 ? 3 : 4;
        for (int i = 0; i < n; i++) cmd((uint8_t)(0x3B + 2 * m), i % (rowlen + 1) == 0 ? 0x3F8000 : 0);
    }
    cmd_float(0x42, 256.0f); cmd_float(0x43, -128.0f); cmd_float(0x44, 65536.0f);
    cmd_float(0x45, 2048.0f); cmd_float(0x46, 2048.0f); cmd_float(0x47, 0.0f);
    cmd(0x4C, 1808u << 4); cmd(0x4D, 1912u << 4);
    depth_state(1);                                        /* ALWAYS, writes on */
    for (int k = 0; k < 16; k++) {
        psp_write32(VERTS + (uint32_t)k * 16, 0xFF41003Du);
        for (int c = 0; c < 3; c++) psp_write32(VERTS + (uint32_t)k * 16 + 4 + (uint32_t)c * 4, C[k][c]);
    }
    cmd(0x36, 21u | (1u << 8));                            /* PATCHDIVISION */
    cmd(0x37, 2);                                          /* PATCHPRIMITIVE: points */
    cmd(0x05, 4u | (4u << 8));                             /* BEZIER 4x4 */
    end_list();
    int bad = 0, first = -1;
    for (int k = 0; k < 44; k++)
        if (depth_at(HW[k].x, HW[k].y) != HW[k].z) { bad++; if (first < 0) first = k; }
    CHECK(bad == 0, "patch points: %d of 44 depths off the PSP's (first: point %d, %u against %u)", bad, first,
          first < 0 ? 0 : depth_at(HW[first].x, HW[first].y), first < 0 ? 0 : HW[first].z);
}

/* A depth plane starts from the end of the long edge on that edge's side
 * (render.c sw_tri, zk0), not the colour's leftmost corner. geprobe 10
 * (fw 6.60) scene 57's first triangle in through mode, corners and depths
 * as the PSP drew it: its middle corner lies left of the long edge, so the
 * plane starts from the rightmost corner, and these pixels read a step
 * higher than from the leftmost. */
static void test_depth_plane_side(void) {
    static const struct { int x, y, z; } P[] = {
        { 47, 23, 12297 }, { 72, 37, 10977 }, { 78, 45, 10641 }, { 100, 55, 9489 }, { 103, 59, 9321 } };
    psp_ge_reset(); clear_fb();
    begin_list_vtype((7u << 2) | (3u << 7) | (1u << 23));
    depth_state(1);                                            /* ALWAYS, writes on */
    float_vertex(0, 675 / 16.0f, 330 / 16.0f, 12577);
    float_vertex(1, 1982 / 16.0f, 985 / 16.0f, 8284);
    float_vertex(2, 1263 / 16.0f, 963 / 16.0f, 10558);
    cmd(0x04, (3u << 16) | 3);
    end_list();
    for (unsigned i = 0; i < sizeof P / sizeof P[0]; i++)
        CHECK(depth_at(P[i].x, P[i].y) == (unsigned)P[i].z, "depth at (%d,%d): %u, hardware %d",
              P[i].x, P[i].y, depth_at(P[i].x, P[i].y), P[i].z);
}

/* A colour plane starts from the depth plane's corner (render.c sw_tri):
 * the end of the long edge on that edge's side. geprobe 14 (fw 6.60) scene
 * 83's triangle 33, drawn in through mode, has its middle corner left of
 * its long edge, so its planes start from the rightmost corner, and these
 * ten pixels read a step higher in one channel than from the leftmost. */
static void test_colour_plane_anchor(void) {
    static const struct { int x, y; uint32_t c; } P[] = {
        { 86, 190, 0x883347 }, { 92, 190, 0x8F3D38 }, { 90, 191, 0x8F3E43 }, { 93, 191, 0x93433B },
        { 86, 192, 0x8D3C53 }, { 88, 192, 0x8F3F4E }, { 91, 192, 0x934446 }, { 86, 193, 0x8F4159 },
        { 89, 193, 0x934651 }, { 84, 194, 0x8F4264 } };
    psp_ge_reset(); clear_fb();
    begin_list_vtype((7u << 2) | (3u << 7) | (1u << 23));
    float_vertex(0, 1687 / 16.0f, 3468 / 16.0f, 0);
    float_vertex(1, 1453 / 16.0f, 2968 / 16.0f, 0);
    float_vertex(2, 1127 / 16.0f, 3308 / 16.0f, 0);
    psp_write32(VERTS + 0 * 16, 0xFFE2CDABu);
    psp_write32(VERTS + 1 * 16, 0xFF812320u);
    psp_write32(VERTS + 2 * 16, 0xFF9C64CFu);
    cmd(0x04, (3u << 16) | 3);
    end_list();
    int bad = 0;
    for (unsigned i = 0; i < sizeof P / sizeof P[0]; i++) {
        const uint32_t got = pixel(P[i].x, P[i].y) & 0xFFFFFFu;
        if (got != P[i].c) {
            bad++;
            CHECK(0, "colour at (%d,%d): %06X, hardware %06X", P[i].x, P[i].y, got, P[i].c);
        }
    }
    CHECK(bad == 0, "%d of 10 colour-plane pixels off the PSP's", bad);
}

/* Lines as the PSP draws them (render.c psp_render_walk_line, sw_draw),
 * from geprobe 15 (fw 6.60). Scene 100's line 41 runs at exactly 45
 * degrees, so it is y-major, and starts 5/16 left of and 3/16 below a
 * pixel centre: on that pixel's diamond, on the minor axis's negative side,
 * so inside. The PSP draws that pixel, (137,17), and stops before (147,27),
 * with these colours; taken x-major, or "inside" meaning above the centre,
 * the line shifts a pixel and its colours a step. Scene 102's first line,
 * flat-shaded, is its second vertex's colour throughout. */
static void test_line_rules(void) {
    static const struct { int x, y; uint32_t c; } P[] = {
        { 137, 17, 0x5FCA4C }, { 138, 18, 0x6EBC58 }, { 139, 19, 0x7EAE64 }, { 140, 20, 0x8DA170 },
        { 141, 21, 0x9D937C }, { 142, 22, 0xAC8588 }, { 143, 23, 0xBC7794 }, { 144, 24, 0xCB69A0 },
        { 145, 25, 0xDB5CAC }, { 146, 26, 0xEA4EB8 } };
    psp_ge_reset(); clear_fb();
    begin_list_vtype((7u << 2) | (3u << 7) | (1u << 23));
    float_vertex(0, 2195 / 16.0f, 283 / 16.0f, 0);
    float_vertex(1, 2355 / 16.0f, 443 / 16.0f, 0);
    psp_write32(VERTS + 0 * 16, 0xFF62C84Fu);
    psp_write32(VERTS + 1 * 16, 0xFFFD3EC7u);
    cmd(0x04, (1u << 16) | 2);                             /* one line */
    end_list();
    int bad = 0;
    for (unsigned i = 0; i < sizeof P / sizeof P[0]; i++)
        bad += (pixel(P[i].x, P[i].y) & 0xFFFFFFu) != P[i].c;
    CHECK(bad == 0, "45-degree line: %d of 10 pixels off the PSP's (first (137,17) reads %06X)",
          bad, pixel(137, 17) & 0xFFFFFFu);
    CHECK((pixel(147, 27) & 0xFFFFFFu) == 0, "45-degree line drew (147,27), which the PSP leaves out");

    psp_ge_reset(); clear_fb();
    begin_list_vtype((7u << 2) | (3u << 7) | (1u << 23));
    cmd(0x50, 0);                                          /* SHADE: flat */
    float_vertex(0, 141 / 16.0f, 276 / 16.0f, 0);
    float_vertex(1, 525 / 16.0f, 284 / 16.0f, 0);
    psp_write32(VERTS + 0 * 16, 0xFF91FF6Au);
    psp_write32(VERTS + 1 * 16, 0xFF4494D0u);
    cmd(0x04, (1u << 16) | 2);
    end_list();
    int lit = 0, off = 0;
    for (int y = 16; y <= 19; y++)
        for (int x = 8; x <= 34; x++) {
            const uint32_t c = pixel(x, y) & 0xFFFFFFu;
            if (c) { lit++; off += c != 0x4494D0u; }
        }
    CHECK(lit >= 20 && off == 0, "flat line: %d pixels, %d not its second vertex's colour", lit, off);
}

/* Gradients take 1/area from the GE's reciprocal table (render.c area_rcp),
 * which is a unit off the reciprocal cut to 16 bits where its linear step
 * misses. geprobe 8 (fw 6.60) scene 50's triangles 517/16 and 885/16 pixels
 * wide and 2 high, depth 16384 to 49152, with pixels the PSP wrote where the
 * cut reciprocal reads a step high and a step low. */
static void test_gradient_reciprocal(void) {
    static const struct { float x0, y0; int w16, px[2], z[2]; } T[] = {
        { 342, 1, 517, { 353, 365 }, { 28045, 40214 } },
        { 138, 160, 885, { 147, 164 }, { 22012, 32083 } } };
    psp_ge_reset(); clear_fb();
    begin_list_vtype((7u << 2) | (3u << 7) | (1u << 23));
    depth_state(1);                                            /* ALWAYS, writes on */
    for (int i = 0; i < 2; i++) {
        float_vertex(3 * i, T[i].x0, T[i].y0, 16384);
        float_vertex(3 * i + 1, T[i].x0 + (float)T[i].w16 / 16.0f, T[i].y0, 49152);
        float_vertex(3 * i + 2, T[i].x0, T[i].y0 + 2, 16384);
    }
    cmd(0x04, (3u << 16) | 6);
    end_list();
    for (int i = 0; i < 2; i++)
        for (int k = 0; k < 2; k++) {
            const int x = T[i].px[k], y = (int)T[i].y0;
            CHECK(depth_at(x, y) == T[i].z[k], "width %d/16, depth at (%d,%d): %u, hardware %d",
                  T[i].w16, x, y, depth_at(x, y), T[i].z[k]);
        }
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

/* Registration, which is how a backend that cannot live in the runtime gets
 * in. A GL backend needs a window and a GL context, and the core has no
 * external dependencies on purpose, so the host builds one and hands it over.
 * Nothing here needs a GPU: what is being tested is the seam. */
static int probe_init(int w, int h) { (void)w; (void)h; return 0; }
static void probe_target(uint32_t a, uint32_t s, int f) { (void)a; (void)s; (void)f; }
static void probe_scissor(int a, int b, int c, int d) { (void)a;(void)b;(void)c;(void)d; }
static void probe_texture(const psp_tex_state *t) { (void)t; }
static void probe_clut(uint32_t a, int f, int sh, int m, int st) {
    (void)a; (void)f; (void)sh; (void)m; (void)st;
}
static void probe_depth(int t, int f, int w) { (void)t; (void)f; (void)w; }
static void probe_blend(const psp_blend_state *b) { (void)b; }
static void probe_fog(int e, uint32_t c) { (void)e; (void)c; }
static unsigned g_probe_draws;
static unsigned g_probe_vertices;
static unsigned g_probe_sequence_errors;
static int g_probe_check_sequence;
static psp_vertex g_probe_first;

static unsigned quad_index(unsigned position) {
    static const unsigned corners[6] = { 0, 1, 2, 0, 2, 3 };
    return position / 6 * 4 + corners[position % 6];
}

static void probe_draw(int p, const psp_vertex *v, int n) {
    g_probe_draws++;
    if (n) g_probe_first=v[0];
    if (!g_probe_check_sequence) return;
    if (p != PSP_PRIM_TRIANGLES || n <= 0 || n % 3 != 0)
        g_probe_sequence_errors++;
    for (int i = 0; i < n; i++) {
        const unsigned want = quad_index(g_probe_vertices);
        if ((v[i].rgba & 0xFFu) != want) g_probe_sequence_errors++;
        g_probe_vertices++;
    }
}
static void probe_noop(void) { }

static const psp_render_backend probe_backend = {
    .name = "probe", .init = probe_init, .shutdown = probe_noop,
    .set_target = probe_target, .set_scissor = probe_scissor,
    .set_texture = probe_texture, .set_clut = probe_clut,
    .set_depth = probe_depth, .set_blend = probe_blend, .set_fog = probe_fog,
    .draw = probe_draw, .finish = probe_noop, .present = probe_noop,
};

static void test_backend_registration(void) {
    CHECK(psp_render_register(&probe_backend) == 0, "a backend can be registered");
    CHECK(psp_render_register(&probe_backend) != 0, "the same name twice is refused");
    CHECK(psp_render_register(NULL) != 0, "NULL backend refused");

    /* A backend missing an entry point would pass registration and then crash
     * at whichever call it forgot, arbitrarily far from the mistake. */
    psp_render_backend gap = probe_backend;
    gap.name = "gap"; gap.set_fog = NULL;
    CHECK(psp_render_register(&gap) != 0, "a backend missing an entry point is refused");

    /* It shows up in the enumeration, which is what the host prints when it
     * refuses an unknown name -- so the list cannot drift from the table. */
    int listed = 0;
    for (size_t i = 0; psp_render_backend_name(i); i++)
        if (strcmp(psp_render_backend_name(i), "probe") == 0) listed = 1;
    CHECK(listed, "a registered backend is enumerated");

    /* And it is selectable and actually driven by the GE, which is the whole
     * point: the interpreter must not care which side a backend came from. */
    CHECK(psp_render_select("probe") == 0, "registered backend selectable");
    psp_ge_reset();
    clear_fb();
    g_probe_draws = 0;
    begin_list();
    vertex(0, 10, 10, 0xFFFFFFFFu);
    vertex(1, 60, 60, 0xFFFFFFFFu);
    cmd(0x04, (6u << 16) | 2);
    end_list();
    CHECK(g_probe_draws == 1, "the GE drove the registered backend, %u draw(s)",
          g_probe_draws);
    CHECK(psp_ge_pixels() == 0, "a backend that draws nothing writes no pixels");

    CHECK(psp_render_select("software") == 0, "software reselectable after");
}

static void test_points_and_lines(void) {
    psp_ge_reset(); clear_fb(); begin_list();
    vertex(0, 10, 10, 0xFF00FF00); vertex(1, 12, 10, 0xFF00FF00);
    vertex(2, 24, 10, 0xFFFF00FF); vertex(3, 22, 10, 0xFFFF00FF);
    vertex(4, 26, 10, 0xFFFFFFFF);  /* incomplete pair must draw nothing */
    cmd(0x04, (PSP_PRIM_LINES << 16) | 5); end_list();
    for (int x = 0; x < 30; x++) {
        const uint32_t want = x == 10 || x == 11 ? 0x00FF00 :
                               x == 22 || x == 23 ? 0xFF00FF : 0;
        CHECK(pixel(x, 10) == want, "line endpoint x=%d: %06X want %06X", x, pixel(x, 10), want);
    }
    psp_ge_reset(); clear_fb(); begin_list();
    vertex(0, 10, 10, 0xFF00FF00); vertex(1, 12, 10, 0xFF00FF00);
    cmd(0x04, (PSP_PRIM_POINTS << 16) | 2); end_list();
    CHECK(pixel(10, 10) == 0x00FF00 && pixel(12, 10) == 0x00FF00 && !pixel(11, 10),
          "points include each submitted endpoint, not the interval");

    psp_ge_reset(); clear_fb(); begin_list();
    for (int i = 0; i < 260; i++) vertex(i, 10 + i, 20, 0xFFFFFFFF);
    cmd(0x04, (PSP_PRIM_LINE_STRIP << 16) | 260); end_list();
    for (int x = 0; x < 280; x++)
        CHECK(pixel(x, 20) == (x >= 10 && x < 269 ? 0xFFFFFFu : 0),
              "line strip preserves batch boundary, x=%d", x);
}

static void test_alpha_only_clear(void) {
    psp_ge_reset(); clear_fb();
    psp_write32(FB + (10*480+10)*4, 0x44332211);
    begin_list();
    vertex(0, 10, 10, 0x99FFFFFF); vertex(1, 11, 11, 0x99FFFFFF);
    cmd(0xD3, 1 | (2 << 8)); /* clear mode, stencil only */
    cmd(0x04, (PSP_PRIM_SPRITES << 16) | 2);
    cmd(0xD3, 0); end_list();
    CHECK(psp_read32(FB + (10*480+10)*4) == 0x99332211,
          "stencil-only clear writes alpha and preserves RGB");
}

static void float_vertex(int i, float x, float y, float z) {
    const float pos[] = {x, y, z};
    psp_write32(VERTS + (uint32_t)i*16, 0xFFFFFFFF);
    for (int k = 0; k < 3; k++) {
        uint32_t bits; memcpy(&bits, &pos[k], 4);
        psp_write32(VERTS + (uint32_t)i*16 + 4 + (uint32_t)k*4, bits);
    }
}

/* The mission text is 64 indexed glyph quads in one triangle draw: 384
 * indices over 256 vertices.  The GE used to split that at 256 rather than a
 * multiple of three, lose one corner, and regroup every remaining triangle.
 * Check both paths because transformed UI text and through-mode geometry have
 * separate batching loops. */
static void write_indexed_quads(int transformed) {
    static const int x[4] = { -1, -1, 1, 1 };
    static const int y[4] = { -1,  1, 1, -1 };
    for (int i = 0; i < 256; i++) {
        const uint32_t colour = 0xFF000000u | (uint32_t)i;
        if (transformed) {
            float_vertex(i, (float)x[i & 3] * 0.25f,
                         (float)y[i & 3] * 0.25f, 0.0f);
            psp_write32(VERTS + (uint32_t)i * 16, colour);
        } else {
            vertex(i, 200 + x[i & 3], 100 + y[i & 3], colour);
        }
    }
    for (unsigned i = 0; i < 384; i++)
        psp_write8(INDICES + i, (uint8_t)quad_index(i));
}

static void identity_matrices(void) {
    for (int m = 0; m < 3; m++) {
        cmd((uint8_t)(0x3A + 2*m), 0);
        const int words = m == 2 ? 16 : 12;
        for (int i = 0; i < words; i++)
            cmd((uint8_t)(0x3B + 2*m), i % (m == 2 ? 5 : 4) == 0 ? 0x3F8000 : 0);
    }
}

static void put_f32(uint32_t a, float f) { uint32_t b; memcpy(&b, &f, 4); psp_write32(a, b); }

/* Skinned, morphed and patch vertices reach the rasteriser where geprobe 2
 * scenes 20-22 (fw 6.60) put them. Identity matrices and no viewport, so a
 * model x of -0.5 lands at screen x 120 and +0.5 at 360. */
static void test_skin_morph_patch(void) {
    CHECK(psp_render_select("probe") == 0, "probe selectable for skin/morph test");

    /* Morph: two sets per record, colour then position, 16 bytes each;
     * weights 0.5 and 0.5 put the point half-way, colour included, and the
     * colour's 127.5 truncates to 127 (geprobe 2 scene 21, fw 6.60). */
    psp_ge_reset();
    begin_list_vtype((7u << 2) | (3u << 7) | (1u << 18));
    identity_matrices();
    cmd(0x2C, 0x3F0000); cmd(0x2D, 0x3F0000);        /* MORPHWEIGHT 0.5, 0.5 */
    psp_write32(VERTS, 0xFF0000FFu);
    put_f32(VERTS + 4, -0.5f); put_f32(VERTS + 8, 0.0f); put_f32(VERTS + 12, 0.0f);
    psp_write32(VERTS + 16, 0xFF00FF00u);
    put_f32(VERTS + 20, 0.5f); put_f32(VERTS + 24, 0.0f); put_f32(VERTS + 28, 0.0f);
    g_probe_draws = 0;
    cmd(0x04, (PSP_PRIM_POINTS << 16) | 1);
    end_list();
    CHECK(g_probe_draws == 1 && fabsf(g_probe_first.precise_x - 240.0f) < 0.01f,
          "morphed point at x 240: %u draws, %.3f", g_probe_draws, g_probe_first.precise_x);
    CHECK(g_probe_first.rgba == 0xFF007F7Fu, "morphed colour 0xFF007F7F: %08X", g_probe_first.rgba);

    /* Skinning: one float weight of 1.0 on bone 0, which moves x by 0.5. */
    psp_ge_reset();
    begin_list_vtype((3u << 9) | (7u << 2) | (3u << 7));
    identity_matrices();
    cmd(0x2A, 0);
    for (int i = 0; i < 12; i++)
        cmd(0x2B, i % 4 == 0 ? 0x3F8000 : i == 9 ? 0x3F0000 : 0);
    put_f32(VERTS, 1.0f);
    psp_write32(VERTS + 4, 0xFFFFFFFFu);
    put_f32(VERTS + 8, 0.0f); put_f32(VERTS + 12, 0.0f); put_f32(VERTS + 16, 0.0f);
    g_probe_draws = 0;
    cmd(0x04, (PSP_PRIM_POINTS << 16) | 1);
    end_list();
    CHECK(g_probe_draws == 1 && fabsf(g_probe_first.precise_x - 360.0f) < 0.01f,
          "skinned point at x 360: %u draws, %.3f", g_probe_draws, g_probe_first.precise_x);

    /* A Bezier patch drawn as points with a 1x1 division is its four
     * corner control points, the far one last. */
    psp_ge_reset();
    begin_list_vtype((7u << 2) | (3u << 7));
    identity_matrices();
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++)
            float_vertex(j * 4 + i, -0.5f + (float)i / 3.0f, -0.5f + (float)j / 3.0f, 0.0f);
    cmd(0x36, 1 | (1 << 8));                         /* PATCHDIVISION 1x1 */
    cmd(0x37, 2);                                    /* PATCHPRIMITIVE points */
    g_probe_draws = 0;
    cmd(0x05, 4 | (4 << 8));                         /* BEZIER 4x4 */
    end_list();
    CHECK(g_probe_draws == 4 && fabsf(g_probe_first.precise_x - 360.0f) < 0.01f,
          "Bezier corners: %u draws, last at x %.3f", g_probe_draws, g_probe_first.precise_x);

    CHECK(psp_render_select("software") == 0, "software reselected after skin/morph test");
}

static void test_indexed_triangle_batch_boundary(void) {
    CHECK(psp_render_select("probe") == 0, "probe selectable for batch test");

    psp_ge_reset();
    begin_list_vtype((7u << 2) | (3u << 7) | (1u << 11));
    cmd(0x02, INDICES & 0xFFFFFF);                 /* IADDR, 8-bit indices */
    identity_matrices();
    write_indexed_quads(1);
    g_probe_draws = g_probe_vertices = g_probe_sequence_errors = 0;
    g_probe_check_sequence = 1;
    cmd(0x04, (PSP_PRIM_TRIANGLES << 16) | 384);
    end_list();
    g_probe_check_sequence = 0;
    CHECK(g_probe_draws == 128 && g_probe_vertices == 384 && !g_probe_sequence_errors,
          "transformed indexed triangles cross batch intact: %u draws, %u vertices, %u errors",
          g_probe_draws, g_probe_vertices, g_probe_sequence_errors);

    psp_ge_reset();
    begin_list_vtype(VTYPE_2D | (1u << 11));
    cmd(0x02, INDICES & 0xFFFFFF);                 /* IADDR, 8-bit indices */
    write_indexed_quads(0);
    g_probe_draws = g_probe_vertices = g_probe_sequence_errors = 0;
    g_probe_check_sequence = 1;
    cmd(0x04, (PSP_PRIM_TRIANGLES << 16) | 384);
    end_list();
    g_probe_check_sequence = 0;
    CHECK(g_probe_draws == 2 && g_probe_vertices == 384 && !g_probe_sequence_errors,
          "through indexed triangles cross batch intact: %u draws, %u vertices, %u errors",
          g_probe_draws, g_probe_vertices, g_probe_sequence_errors);

    CHECK(psp_render_select("software") == 0, "software reselectable after batch test");
}

static void test_precise_vertex_payload(void) {
    CHECK(psp_render_select("probe")==0,"probe selectable for float vertex test");
    psp_ge_reset();
    begin_list_vtype((7u<<2)|(3u<<7));
    identity_matrices();
    float_vertex(0,(40.24f-240.0f)/240.0f,0,0);
    cmd(0x04,(PSP_PRIM_POINTS<<16)|1); end_list();
    /* Before the 1/16 grid, in the GE's arithmetic: the eye x cut to 16
     * significant bits (ge_clip in ge.c) puts 40.24 at 40.2429. */
    CHECK(g_probe_first.precise && fabsf(g_probe_first.precise_x-40.2429f)<0.0001f,
          "GE retains pre-quantization projection %.8f",g_probe_first.precise_x);
    /* 40.25: left of the viewport centre a position goes to the sixteenth
     * nearer the centre (geprobe 2, fw 6.60; see screen_axis_fx16 in ge.c). */
    CHECK(g_probe_first.x==644,"transformed geometry goes to 40.25 pixels: %d",g_probe_first.x);
    /* 40.21 also goes up, where rounding would give 40.1875; right of the
     * centre, 300.05 goes down to 300.0 where rounding would give 300.0625. */
    psp_ge_reset();
    begin_list_vtype((7u<<2)|(3u<<7));
    identity_matrices();
    float_vertex(0,(40.21f-240.0f)/240.0f,0,0);
    cmd(0x04,(PSP_PRIM_POINTS<<16)|1); end_list();
    CHECK(g_probe_first.x==644,"40.21 goes toward the centre, to 40.25: %d",g_probe_first.x);
    psp_ge_reset();
    begin_list_vtype((7u<<2)|(3u<<7));
    identity_matrices();
    float_vertex(0,(300.05f-240.0f)/240.0f,0,0);
    cmd(0x04,(PSP_PRIM_POINTS<<16)|1); end_list();
    CHECK(g_probe_first.x==4800,"300.05 goes toward the centre, to 300.0: %d",g_probe_first.x);
    psp_ge_reset(); begin_list(); vertex(0,10,20,0xFFFFFFFF);
    cmd(0x04,(PSP_PRIM_POINTS<<16)|1); end_list();
    CHECK(!g_probe_first.precise,"through-mode vertices keep the PSP coordinate contract");
    CHECK(psp_render_select("software")==0,"software reselected after float payload test");
}

static void test_transformed_lines(void) {
    psp_ge_reset(); clear_fb();
    begin_list_vtype((7u<<2) | (3u<<7));
    for (int m = 0; m < 3; m++) {
        cmd((uint8_t)(0x3A + 2*m), 0);
        for (int i = 0; i < (m == 2 ? 16 : 12); i++)
            cmd((uint8_t)(0x3B + 2*m), i % (m == 2 ? 5 : 4) == 0 ? 0x3F8000 : 0);
    }
    /* Identity matrices/default viewport. Clip at z=-1 halfway along this
     * segment, so only its right half (x=240..359) is visible. */
    float_vertex(0,-0.5f,0,-2); float_vertex(1,0.5f,0,0);
    cmd(0x1C, 1); cmd(0x04, (PSP_PRIM_LINES<<16) | 2); end_list();
    CHECK(!pixel(239,136) && pixel(240,136) == 0xFFFFFF &&
          pixel(359,136) == 0xFFFFFF && !pixel(360,136), "transformed line clips near plane");

    psp_ge_reset(); clear_fb(); begin_list_vtype((7u<<2) | (3u<<7));
    for (int m = 0; m < 2; m++) {
        cmd((uint8_t)(0x3A + 2*m), 0);
        for (int i = 0; i < 12; i++) cmd((uint8_t)(0x3B + 2*m), i%4 == 0 ? 0x3F8000 : 0);
    }
    float_vertex(0,0,0,0); float_vertex(1,0.5f,0,0);
    cmd(0x3E, 0); /* A projection with W=-1: do not draw a point at (0,0). */
    for (int i = 0; i < 16; i++) {
        const float f = i == 15 ? -1.0f : i%5 == 0 ? 1.0f : 0.0f;
        uint32_t bits; memcpy(&bits, &f, 4); cmd(0x3F, bits>>8);
    }
    cmd(0x04, (PSP_PRIM_POINTS<<16) | 2); end_list();
    CHECK(psp_ge_pixels() == 0, "behind-eye points are rejected");
}

typedef struct { int count; psp_vertex first, last; } line_samples;
static void collect_line(const psp_vertex *v, void *data) {
    line_samples *s = data;
    if (!s->count++) s->first = *v;
    s->last = *v;
}

static void test_line_interpolation_and_clipping(void) {
    psp_vertex a = { .x = -16 * 1000000, .y = 16 * 5, .rgba = 0xFF000000,
                     .inv_w = 1, .tex_q = 1, .fog = 255 };
    psp_vertex b = a; b.x = 16 * 1000000; b.rgba = 0xFFFFFFFF;
    line_samples s = {0};
    psp_render_walk_line(&a, &b, 10, 0, 19, 9, collect_line, &s);
    CHECK(s.count == 10 && s.first.x == 168 && s.last.x == 312,
          "huge offscreen line clips to ten samples, got %d", s.count);
    /* The colour gradient is floored to 1/1024 a step (geprobe step 1, fw
     * 6.60), so over two million steps it is zero; a 4000-step line, the
     * longest the 12.4 grid holds, keeps 65/1024 and reads 127 mid-way. */
    CHECK((s.first.rgba & 255) == 0, "a sub-1/1024 gradient does not move the colour");
    a.x = -16 * 2000; b.x = 16 * 2000;
    s = (line_samples){0};
    psp_render_walk_line(&a, &b, 10, 0, 19, 9, collect_line, &s);
    CHECK(s.count == 10 && s.first.x == 168 && (s.first.rgba & 255) == 127,
          "scissor must not reset colour interpolation: %d samples, %08X",
          s.count, s.first.rgba);
    a.x = a.y = 0; b.x = b.y = 64; b.inv_w = 0.5f; b.u = 8; b.z = 100;
    s = (line_samples){0};
    psp_render_walk_line(&a, &b, 2, 2, 2, 2, collect_line, &s);
    CHECK(s.count == 1 && s.first.x == 40 && s.first.y == 40, "diagonal scissor sample");
    /* Depth and the perspective-divided u at the step's centre, 2.5 of 4
     * steps, like colour: geprobe 5 scenes 27 and 28 (fw 6.60). u is
     * (2.5/4 * 8 * 0.5) / (1.5/4 + 2.5/4 * 0.5) = 3.636. */
    CHECK(s.first.z == 62 && s.first.u > 3.636f && s.first.u < 3.637f &&
          s.first.inv_w == 1 && s.first.tex_q == 1, "line perspective and depth interpolation");
    s = (line_samples){0};
    psp_render_walk_line(&a, &a, 0, 0, 10, 10, collect_line, &s);
    CHECK(!s.count, "zero length line is not a point");
    b.x = -64; b.y = -64;
    psp_render_walk_line(&a, &b, 0, 0, 10, 10, collect_line, &s);
    CHECK(!s.count, "negative coordinates floor and descending boundary excludes origin");
}

/* A line with endpoints between pixel centres: geprobe 5 scene 27's 3D line
 * (fw 6.60), 258.81 to 381.31 across, its depths 12577 to 10371. The
 * hardware starts at pixel 259, puts each column on the row of its centre's
 * projection and writes the depth there. */
typedef struct { int n, first; int y[4]; float z[4]; } line_picks;
static void pick_line(const psp_vertex *v, void *data) {
    line_picks *p = data;
    static const int want[4] = { 259, 264, 272, 380 };
    const int x = v->x >> 4;
    if (!p->n++) p->first = x;
    for (int i = 0; i < 4; i++)
        if (x == want[i]) { p->y[i] = v->y >> 4; p->z[i] = v->z; }
}

static void test_line_between_centres(void) {
    psp_vertex a = { .x = 4141, .y = 3947, .z = 12577.5f, .rgba = 0xFFFFFFFF, .inv_w = 1, .tex_q = 1 };
    psp_vertex b = a; b.x = 6101; b.y = 3714; b.z = 10371.0f;
    line_picks p = {0};
    psp_render_walk_line(&a, &b, 0, 0, 479, 271, pick_line, &p);
    CHECK(p.n == 122 && p.first == 259, "pixels %d from %d, hardware 122 from 259", p.n, p.first);
    CHECK(p.y[1] == 246 && p.y[2] == 245 && p.y[3] == 232,
          "rows at 264, 272, 380: %d %d %d, hardware 246 245 232", p.y[1], p.y[2], p.y[3]);
    CHECK(p.z[0] == 12564 && p.z[1] == 12474 && p.z[2] == 12330 && p.z[3] == 10385,
          "depths %.0f %.0f %.0f %.0f, hardware 12564 12474 12330 10385",
          (double)p.z[0], (double)p.z[1], (double)p.z[2], (double)p.z[3]);
}

/* Colour interpolation against geprobe's hardware frames (step 1, fw 6.60):
 * the Gouraud triangle, the flat one and a line of the geometry scene, with
 * the pixels the PSP wrote. */
static void test_hardware_shading(void) {
    psp_ge_reset(); clear_fb(); begin_list();
    vertex(0, 20, 20, 0xFF0000FF); vertex(1, 200, 30, 0xFF00FF00); vertex(2, 60, 180, 0xFFFF0000);
    cmd(0x04, (PSP_PRIM_TRIANGLES << 16) | 3); end_list();
    CHECK(pixel(120, 60) == 0x388144 && pixel(64, 64) == 0x432F8B && pixel(50, 100) == 0x7F0E70,
          "Gouraud plane: %06X %06X %06X, hardware 388144 432F8B 7F0E70",
          pixel(120, 60), pixel(64, 64), pixel(50, 100));

    psp_ge_reset(); clear_fb(); begin_list();
    vertex(0, 20, 230, 0xFF0000FF); vertex(1, 120, 230, 0xFF00FF00); vertex(2, 70, 265, 0xFFFF0000);
    cmd(0x50, 0);                                  /* SHADE: flat */
    cmd(0x04, (PSP_PRIM_TRIANGLES << 16) | 3); end_list();
    CHECK(pixel(70, 250) == 0xFF0000 && pixel(40, 240) == 0xFF0000,
          "flat triangle takes its last vertex: %06X %06X", pixel(70, 250), pixel(40, 240));

    psp_ge_reset(); clear_fb(); begin_list();
    vertex(0, 140, 230, 0xFFFFFFFF); vertex(1, 230, 265, 0xFF0000FF);
    cmd(0x04, (PSP_PRIM_LINES << 16) | 2); end_list();
    CHECK(pixel(140, 230) == 0xFDFDFF && pixel(150, 234) == 0xE1E1FF &&
          pixel(180, 245) == 0x8C8CFF && pixel(229, 264) == 0x0101FF && !pixel(150, 233),
          "line minor axis and colour at i + 1/2: %06X %06X %06X %06X",
          pixel(140, 230), pixel(150, 234), pixel(180, 245), pixel(229, 264));
}

/* Dither on an 8888 target, geprobe step 10 (fw 6.60): a flat 7F sprite
 * under the probe's extreme matrix, rows {7,-8,7,-8} {-8,7,-8,7} {0,1,2,3}. */
/* geprobe 2 scene 15 (fw 6.60): the fogged floor, one transformed triangle
 * with two corners off screen, white to green and fog 255 to 0. Its colour
 * and fog planes are anchored at the leftmost corner, (-388.125, 371.5),
 * not at the one on screen; these pixels are the hardware's and each is one
 * step off in some channel with the on-screen anchor. */
static void test_hardware_transformed_anchor_fog(void) {
    psp_ge_reset();
    clear_fb();
    const psp_render_backend *be = psp_render_current();
    psp_blend_state blend = { .write_colour = 1 };
    be->set_target(FB, 480, 3);
    be->set_scissor(0, 0, 479, 271);
    be->set_texture(&(psp_tex_state){ 0 });
    be->set_depth(0, 1, 0);
    be->set_blend(&blend);
    be->set_fog(1, 0xFF8040u);
    psp_vertex tri[3] = {
        { .x = -6210, .y = 5944, .rgba = 0xFFFFFFFFu, .inv_w = 1, .tex_q = 1, .fog = 255, .precise = 1 },
        { .x = 13890, .y = 5944, .rgba = 0xFFFFFFFFu, .inv_w = 1, .tex_q = 1, .fog = 255, .precise = 1 },
        { .x =  3464, .y = 2317, .rgba = 0xFF00FF00u, .inv_w = 1, .tex_q = 1, .fog = 0,   .precise = 1 },
    };
    be->draw(PSP_PRIM_TRIANGLES, tri, 3);
    be->set_fog(0, 0);
    CHECK(pixel(127, 178) == 0xDE933C && pixel(252, 203) == 0xCEA141 && pixel(93, 259) == 0xBFC061,
          "fogged floor: %06X %06X %06X, hardware DE933C CEA141 BFC061",
          pixel(127, 178), pixel(252, 203), pixel(93, 259));
}

/* BBOX and BJUMP as geprobe 4 scene 24 (fw 6.60) measured them: a box whose
 * corners all project beyond one edge of the screen is skipped; one behind
 * the camera (negative w) projects mirrored onto the screen and is not. */
static int bbox_marker_drawn(float bx, float bz, float w) {
    psp_ge_reset();
    clear_fb();
    begin_list_vtype((7u << 2) | (3u << 7));
    identity_matrices();
    /* The projection's w row: w = -z * w_per_z + w (identity keeps w = 1). */
    for (int i = 0; i < 8; i++)
        float_vertex(i, bx + ((i & 1) ? 0.1f : -0.1f), (i & 2) ? 0.1f : -0.1f, bz + ((i & 4) ? 0.1f : -0.1f));
    if (w != 1.0f) {
        /* Replace the projection with one whose w is the constant `w`. */
        cmd(0x3E, 0);
        for (int i = 0; i < 16; i++) {
            float f = (i % 5 == 0 && i != 15) ? 1.0f : (i == 15 ? w : 0.0f);
            uint32_t bits; memcpy(&bits, &f, 4);
            cmd(0x3F, bits >> 8);
        }
    }
    cmd(0x07, 8);                                    /* BBOX: 8 corners */
    const uint32_t bj = g_pc;
    cmd(0x10, (LIST >> 8) & 0xFF0000);               /* BASE */
    cmd(0x09, 0);                                    /* BJUMP, patched below */
    cmd(0x12, VTYPE_2D);
    cmd(0x01, (VERTS + 20 * 12) & 0xFFFFFF);
    vertex(20, 10, 10, 0xFF00FF00u);
    vertex(21, 20, 20, 0xFF00FF00u);
    cmd(0x04, (6u << 16) | 2);
    psp_write32(LIST + bj + 4, (0x09u << 24) | ((LIST + g_pc) & 0xFFFFFF));
    end_list();
    return pixel(15, 15) == 0x00FF00u;
}

static void test_bbox_jump(void) {
    CHECK(bbox_marker_drawn(0.0f, 0.0f, 1.0f), "a box in view is drawn");
    CHECK(!bbox_marker_drawn(3.0f, 0.0f, 1.0f), "a box right of the screen is skipped");
    CHECK(!bbox_marker_drawn(-3.0f, 0.0f, 1.0f), "a box left of the screen is skipped");
    CHECK(bbox_marker_drawn(0.0f, 0.0f, -1.0f), "a box behind the camera (w < 0) is drawn");
    CHECK(bbox_marker_drawn(0.0f, 50.0f, 1.0f), "depth does not count: a box far out in z is drawn");
}

static void test_hardware_dither(void) {
    psp_ge_reset(); clear_fb(); begin_list();
    vertex(0, 10, 210, 0xFF7F7F7F); vertex(1, 240, 240, 0xFF7F7F7F);
    cmd(0xE2, 0x8787); cmd(0xE3, 0x7878); cmd(0xE4, 0x3210); cmd(0xE5, 0xCDEF);
    cmd(0x20, 1);                                  /* dither on */
    cmd(0x04, (PSP_PRIM_SPRITES << 16) | 2); end_list();
    CHECK(pixel(12, 212) == 0x868686 && pixel(13, 212) == 0x777777 &&
          pixel(12, 213) == 0x777777 && pixel(13, 214) == 0x808080,
          "dither offsets: %06X %06X %06X %06X, hardware 868686 777777 777777 808080",
          pixel(12, 212), pixel(13, 212), pixel(12, 213), pixel(13, 214));
}

/* Blend arithmetic on geprobe step 11's inputs (fw 6.60): a one-pixel sprite
 * over a destination written straight into the framebuffer. */
static uint32_t blend_one(uint32_t dst, uint32_t src, uint32_t mode, uint32_t fixa, uint32_t fixb) {
    psp_ge_reset(); clear_fb();
    psp_write32(FB + (uint32_t)(20 * 480 + 20) * 4, dst);
    begin_list();
    vertex(0, 20, 20, src); vertex(1, 21, 21, src);
    cmd(0x21, 1); cmd(0xDF, mode); cmd(0xE0, fixa); cmd(0xE1, fixb);
    cmd(0x04, (PSP_PRIM_SPRITES << 16) | 2); end_list();
    return pixel(20, 20);
}

static void test_hardware_blend(void) {
    /* FIX 0x80 + FIX 0x80: 64 over 5 is 34 (((c + 1) * f) >> 8 said 35). */
    uint32_t p = blend_one(0x05, 0xFF000040, 10 | 10 << 4, 0x808080, 0x808080);
    CHECK((p & 0xFF) == 34, "FIX/FIX 64 over 5: %u, hardware 34", p & 0xFF);
    /* DOUBLE_SRC_ALPHA / ONE_MINUS_DOUBLE_SRC_ALPHA at alpha 0x80: the
     * destination factor is -1, so B 0xC0 over B 171 reads 191, over 170 192. */
    p = blend_one(0x00AB0000, 0x80C08040, 6 | 7 << 4, 0, 0);
    CHECK((p >> 16) == 191, "negative doubled factor subtracts: B %u, hardware 191", p >> 16);
    p = blend_one(0x00AA0000, 0x80C08040, 6 | 7 << 4, 0, 0);
    CHECK((p >> 16) == 192, "negative doubled factor on 170: B %u, hardware 192", p >> 16);
}

/* MODULATE with colour doubling, geprobe step 13 (fw 6.60): doubling acts
 * before the final shift, so texel 255 by vertex 64 is (255 * 65) >> 7 = 129,
 * not twice the rounded 64. */
static void test_hardware_texfunc(void) {
    psp_ge_reset(); clear_fb();
    psp_write32(TEX, 0xFFFFFFFFu);
    begin_list_vtype(VTYPE_2D_TEX);
    texture_state(TEX, 1, 0, 0, 3, 0, 0);
    cmd(0xC9, 0 | 1u << 16);                       /* MODULATE, RGB, doubled */
    vertex_uv(0, 30, 30, 0, 0, 0xFF404040u);
    vertex_uv(1, 31, 31, 1, 1, 0xFF404040u);
    cmd(0x04, (PSP_PRIM_SPRITES << 16) | 2); end_list();
    CHECK(pixel(30, 30) == 0x818181, "doubled MODULATE 255 x 64: %06X, hardware 818181",
          pixel(30, 30));
}

/* Colour test, logic op and pixel mask, geprobe step 19 (fw 6.60): a
 * one-pixel sprite over a destination written straight into the framebuffer,
 * with the state commands passed in; returns the whole word, stencil byte
 * included. */
static uint32_t draw_one_with(uint32_t dst, uint32_t src, const uint32_t *state, int n) {
    psp_ge_reset(); clear_fb();
    psp_write32(FB + (uint32_t)(20 * 480 + 20) * 4, dst);
    begin_list();
    vertex(0, 20, 20, src); vertex(1, 21, 21, src);
    for (int i = 0; i < n; i++) cmd(state[i] >> 24, state[i] & 0xFFFFFFu);
    cmd(0x04, (PSP_PRIM_SPRITES << 16) | 2); end_list();
    return psp_read32(FB + (uint32_t)(20 * 480 + 20) * 4);
}

static void test_hardware_colour_logic_mask(void) {
    /* LOE on, LOP n: source 0xA55A3C over 0x402010; the stencil byte stays. */
    static const struct { int op; uint32_t want; const char *name; } L[] = {
        { 0, 0x000000, "CLEAR" }, { 1, 0x000010, "AND" },   { 6, 0xE57A2C, "XOR" },
        { 7, 0xE57A3C, "OR" },    { 8, 0x1A85C3, "NOR" },   { 9, 0x1A85D3, "EQUIV" },
        { 10, 0xBFDFEF, "INVERTED" }, { 14, 0xFFFFEF, "NAND" },
    };
    for (unsigned i = 0; i < sizeof L / sizeof L[0]; i++) {
        const uint32_t st[] = { 0x28u << 24 | 1, 0xE6u << 24 | (uint32_t)L[i].op };
        const uint32_t p = draw_one_with(0x33402010, 0xC3A55A3C, st, 2);
        CHECK(p == (0x33000000u | L[i].want), "logic op %s: %08X, hardware %08X",
              L[i].name, p, 0x33000000u | L[i].want);
    }
    /* PMSK1 0x00F0F0, PMSK2 0xFF: a set bit keeps the framebuffer's bit. */
    const uint32_t pm[] = { 0xE8u << 24 | 0x00F0F0, 0xE9u << 24 | 0xFF };
    uint32_t p = draw_one_with(0x00402010, 0x7FFFFFFF, pm, 2);
    CHECK(p == 0x00FF2F1F, "pixel mask 0xFF00F0F0: %08X, hardware 00FF2F1F", p);
    /* CTE on, NOTEQUAL 0x808080 / 0xF0F0F0: 0x8A8F80 is dropped, 0x908080 is
     * drawn. */
    const uint32_t ct[] = { 0x27u << 24 | 1, 0xD8u << 24 | 3, 0xD9u << 24 | 0x808080, 0xDAu << 24 | 0xF0F0F0 };
    p = draw_one_with(0x00402010, 0xFF808F8A, ct, 4);
    CHECK(p == 0x00402010, "colour test drops 0x8A8F80: %08X", p);
    p = draw_one_with(0x00402010, 0xFF808090, ct, 4);
    CHECK((p & 0xFFFFFF) == 0x808090, "colour test passes 0x908080: %08X", p);
}

/* Float through-mode coordinates saturate to 12.4, geprobe step 18 (fw 6.60):
 * the triangle with a vertex at x = 5000 reads E0E0E0 at (470,155) and draws
 * exactly as with that vertex at 2048. */
static uint32_t g_sat_frame[3][2];
static void draw_far_triangle(float x1, int k) {
    psp_ge_reset(); clear_fb();
    begin_list_vtype((7u << 2) | (3u << 7) | (1u << 23));
    float_vertex(0, 300, 150, 0); psp_write32(VERTS + 0 * 16, 0xFFFFFFFF);
    float_vertex(1, x1, 160, 0);  psp_write32(VERTS + 1 * 16, 0xFF000000);
    float_vertex(2, 320, 260, 0); psp_write32(VERTS + 2 * 16, 0xFF808080);
    cmd(0x04, (PSP_PRIM_TRIANGLES << 16) | 3); end_list();
    g_sat_frame[k][0] = pixel(470, 155);
    g_sat_frame[k][1] = pixel(400, 200);
}

static void test_hardware_through_saturation(void) {
    draw_far_triangle(5000.0f, 0);
    draw_far_triangle(2048.0f, 1);
    CHECK(g_sat_frame[0][0] == 0xE0E0E0, "x = 5000 saturates: %06X at (470,155), hardware E0E0E0",
          g_sat_frame[0][0]);
    CHECK(g_sat_frame[0][0] == g_sat_frame[1][0] && g_sat_frame[0][1] == g_sat_frame[1][1],
          "x = 5000 draws as x = 2048: %06X %06X against %06X %06X",
          g_sat_frame[0][0], g_sat_frame[0][1], g_sat_frame[1][0], g_sat_frame[1][1]);
    /* Far enough out that an unclamped float would not fit an int. */
    draw_far_triangle(1.0e12f, 2);
    CHECK(g_sat_frame[2][0] == g_sat_frame[0][0], "x = 1e12 saturates too: %06X", g_sat_frame[2][0]);
}

/* The sprite texel step, geprobe step 12 (fw 6.60): v 0..16 over the 56 rows
 * of (416,8)-(472,64). Row 11's centre lands exactly on v = 1.0 and the PSP
 * reads the texel below it, row 0. */
static void test_hardware_sprite_step(void) {
    psp_ge_reset(); clear_fb();
    upload_ramp_texture(16, 16);
    begin_list_vtype(VTYPE_2D_TEXF);
    texture_state(TEX, 16, 4, 4, 3, 0, 0);
    cmd(0xC7, 1u);                                 /* TEXWRAP: clamp u, repeat v */
    vertex_uvf(0, 416, 8, 15.9f, 0.0f, 0xFFFFFFFFu);
    vertex_uvf(1, 472, 64, 16.1f, 16.0f, 0xFFFFFFFFu);
    cmd(0x04, (PSP_PRIM_SPRITES << 16) | 2); end_list();
    CHECK(pixel(440, 10) == ramp_texel(15, 0) && pixel(440, 11) == ramp_texel(15, 0) &&
          pixel(440, 12) == ramp_texel(15, 1),
          "v = 1.0 exactly takes row 0: %06X %06X %06X", pixel(440, 10), pixel(440, 11), pixel(440, 12));
}

/* Texel boundaries that land exactly on a pixel centre, geprobe 5 scene 28
 * (fw 6.60). A sprite mapping u and v 4..6 onto 7 pixels with its corners
 * given bottom-right first reads texel 5 at the centre where u = 5 exactly,
 * its ramp starting from the top-left corner (from the first vertex it read
 * texel 4). A through-mode line from x 10 to 470 with u 0..16 reads texel 1
 * at x 67, where u = 2 exactly: its step, truncated, falls just short. At
 * x 96 the centre's u is 3.009 and the PSP reads texel 3; the left edge's,
 * where psprecomp used to take it, is 2.991. */
static void test_hardware_texel_boundaries(void) {
    psp_ge_reset(); clear_fb();
    upload_ramp_texture(16, 16);
    begin_list_vtype(VTYPE_2D_TEXF);
    texture_state(TEX, 16, 4, 4, 3, 0, 0);
    vertex_uvf(0, 65, 187, 4.0f, 4.0f, 0xFFFFFFFFu);
    vertex_uvf(1, 58, 180, 6.0f, 6.0f, 0xFFFFFFFFu);
    cmd(0x04, (PSP_PRIM_SPRITES << 16) | 2); end_list();
    CHECK(pixel(60, 183) == ramp_texel(5, 5) && pixel(61, 183) == ramp_texel(5, 5) &&
          pixel(62, 183) == ramp_texel(4, 5) && pixel(61, 184) == ramp_texel(5, 4),
          "reversed sprite at u = 5: %06X %06X %06X %06X", pixel(60, 183), pixel(61, 183),
          pixel(62, 183), pixel(61, 184));
    psp_ge_reset(); clear_fb();
    begin_list_vtype(VTYPE_2D_TEXF);
    texture_state(TEX, 16, 4, 4, 3, 0, 0);
    vertex_uvf(0, 10, 226, 0.0f, 8.0f, 0xFFFFFFFFu);
    vertex_uvf(1, 470, 226, 16.0f, 8.0f, 0xFFFFFFFFu);
    cmd(0x04, (PSP_PRIM_LINES << 16) | 2); end_list();
    CHECK(pixel(67, 226) == ramp_texel(1, 8) && pixel(68, 226) == ramp_texel(2, 8) &&
          pixel(96, 226) == ramp_texel(3, 8),
          "line at u = 2 and 3.009: %06X %06X %06X", pixel(67, 226), pixel(68, 226), pixel(96, 226));
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
    test_vertex_pointer_advances();
    test_texture_1to1();
    test_texture_minified_samples_centre();
    test_texture_perspective_interpolation();
    test_secondary_colour();
    test_sprite_texture_samples_centre();
    test_texture_wrap();
    test_texture_bilinear_midpoint();
    test_texture_filter_is_honoured();
    test_texture_lod_rules();
    test_texture_mip_chain();
    test_bilinear_equals_nearest_at_1to1();
    test_clear_mode_clears_depth();
    test_depth_test_still_rejects();
    test_depth_in_vram();
    test_depth_plane();
    test_transformed_depth();
    test_vertex_depth_ge();
    test_skin_ge();
    test_patch_points_ge();
    test_colour_plane_anchor();
    test_line_rules();
    test_gradient_reciprocal();
    test_depth_plane_side();
    test_spline_colour();
    test_line_between_centres();
    test_far_vertex_16bit();
    test_backend_selection();
    test_backend_registration();
    test_indexed_triangle_batch_boundary();
    test_precise_vertex_payload();
    test_skin_morph_patch();
    test_points_and_lines();
    test_alpha_only_clear();
    test_transformed_lines();
    test_line_interpolation_and_clipping();
    test_hardware_shading();
    test_hardware_transformed_anchor_fog();
    test_bbox_jump();
    test_hardware_dither();
    test_hardware_blend();
    test_hardware_texfunc();
    test_hardware_colour_logic_mask();
    test_hardware_through_saturation();
    test_hardware_sprite_step();
    test_hardware_texel_boundaries();

    psp_mem_free();
    printf(failures ? "raster: %d failure(s)\n" : "raster: all tests passed\n", failures);
    return failures ? 1 : 0;
}
