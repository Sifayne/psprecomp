/* psprecomp — render backend selection, and the two backends that need no
 * external dependencies. See docs/RENDERER.md.
 *
 * The software backend is not a fallback. It is the reference: every other
 * backend is correct exactly insofar as it agrees with this one on the same
 * display list, and `tests/test_raster.c` pins its behaviour using synthetic
 * lists with no game data and no GPU. A GPU backend that cannot be diffed
 * against something is a backend whose bugs are indistinguishable from the
 * game's.
 */

#include "psprecomp/render.h"
#include "psprecomp/mem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- shared target state ------------------------------------------------- */

static uint32_t g_fb_addr, g_fb_stride;
static uint64_t g_pixels;

/* Textured versus flat, because "the geometry is white" has two causes with
 * nothing in common: a texture that never binds, and vertices that really are
 * white. One counter each is the difference between measuring and guessing. */
static uint64_t g_px_tex, g_px_flat, g_px_zfail, g_px_blend, g_px_atest;
uint64_t psp_render_textured_pixels(void) { return g_px_tex; }
uint64_t psp_render_flat_pixels(void) { return g_px_flat; }
uint64_t psp_render_zfail_pixels(void) { return g_px_zfail; }

uint64_t psp_render_pixels(void) { return g_pixels; }
void     psp_render_reset_pixels(void) {
    g_pixels = g_px_tex = g_px_flat = g_px_zfail = g_px_blend = g_px_atest = 0;
}

/* ---- software backend ---------------------------------------------------- */

static int sw_init(int w, int h) { (void)w; (void)h; return 0; }
static void sw_shutdown(void) { }

static void sw_target(uint32_t addr, uint32_t stride, int fmt) {
    (void)fmt;
    g_fb_addr = addr;
    g_fb_stride = stride;
}

/* ---- texture sampling -----------------------------------------------------
 *
 * One format, 5650, and one function, modulate. That is not a simplification
 * of the hardware but a description of this game: over a full run it sets no
 * other, and the GE state report says so. Anything else draws untextured, so a
 * format arriving that is not handled shows up as flat colour rather than as
 * plausible-looking wrong pixels.
 *
 * Nearest sampling. Bilinear would need the filter state honoured, and a
 * through-mode blit at 1:1 -- which is what a UI layer is -- samples texel
 * centres either way. */
static struct {
    uint32_t addr, stride;
    int      w, h, fmt, func, swizzled;
} g_tex;

/* PSPRECOMP_TEXDUMP=<path> writes each distinct texture the game binds, decoded
 * through this same sampler, as <path>-NN.ppm.
 *
 * "The picture is speckled" has two causes that look identical on screen: the
 * sampler reading the wrong texels, or the texture in memory not being what we
 * think. Decoding it standalone separates them -- if the dump is clean and the
 * frame is not, the fault is downstream of sampling. */
static uint32_t g_dumped[16];
static int      g_dumped_n;

static void dump_texture(void);

static void sw_texture(uint32_t addr, uint32_t stride, int w, int h,
                       int fmt, int func, int swizzled) {
    g_tex.addr = addr; g_tex.stride = stride;
    g_tex.w = w; g_tex.h = h;
    g_tex.fmt = fmt; g_tex.func = func; g_tex.swizzled = swizzled;
    dump_texture();
}

enum {
    GE_TFMT_5650 = 0, GE_TFMT_5551 = 1, GE_TFMT_4444 = 2, GE_TFMT_8888 = 3,
    GE_TFMT_CLUT4 = 4, GE_TFMT_CLUT8 = 5
};

static struct {
    uint32_t addr;
    int      fmt, shift, mask, start;
} g_clut;

static void sw_clut(uint32_t addr, int format, int shift, int mask, int start) {
    g_clut.addr = addr; g_clut.fmt = format;
    g_clut.shift = shift; g_clut.mask = mask; g_clut.start = start;
}

/* Bytes per texel, doubled, so the 4-bit format can be expressed as 1. */
static int tex_halfbytes(int fmt) {
    switch (fmt) {
    case GE_TFMT_CLUT4:                     return 1;
    case GE_TFMT_CLUT8:                     return 2;
    case GE_TFMT_5650: case GE_TFMT_5551:
    case GE_TFMT_4444:                      return 4;
    case GE_TFMT_8888:                      return 8;
    default:                                return 0;
    }
}

static int texture_usable(void) {
    if (!g_tex.addr || g_tex.w <= 0 || g_tex.h <= 0) return 0;
    if (!tex_halfbytes(g_tex.fmt)) return 0;
    if ((g_tex.fmt == GE_TFMT_CLUT4 || g_tex.fmt == GE_TFMT_CLUT8) && !g_clut.addr)
        return 0;
    return 1;
}

/* Swizzled textures are stored as blocks 16 bytes wide and 8 rows tall. The
 * swizzle is on *bytes*, not texels, so one mapping serves every format --
 * which is why this takes a byte offset rather than a texel coordinate. */
static uint32_t swizzled_byte(uint32_t byte_x, uint32_t y, uint32_t row_bytes) {
    if (!g_tex.swizzled || row_bytes < 16) return y * row_bytes + byte_x;
    const uint32_t rowblocks = row_bytes / 16;
    const uint32_t block = ((y / 8) * rowblocks + (byte_x / 16)) * (16 * 8);
    return block + (y % 8) * 16 + (byte_x % 16);
}

/* 16-bit palette and texel formats, expanded to eight bits a channel with the
 * high bits replicated down so full-scale stays 0xFF rather than 0xF8. */
static uint32_t expand16(uint32_t p, int fmt) {
    uint32_t r, g, b, a;
    switch (fmt) {
    case GE_TFMT_5650:
        r = p & 0x1F;         r = (r << 3) | (r >> 2);
        g = (p >> 5)  & 0x3F; g = (g << 2) | (g >> 4);
        b = (p >> 11) & 0x1F; b = (b << 3) | (b >> 2);
        a = 0xFF; break;
    case GE_TFMT_5551:
        r = p & 0x1F;         r = (r << 3) | (r >> 2);
        g = (p >> 5)  & 0x1F; g = (g << 3) | (g >> 2);
        b = (p >> 10) & 0x1F; b = (b << 3) | (b >> 2);
        a = (p >> 15) ? 0xFF : 0x00; break;
    default: /* 4444 */
        r = p & 0xF;          r = (r << 4) | r;
        g = (p >> 4)  & 0xF;  g = (g << 4) | g;
        b = (p >> 8)  & 0xF;  b = (b << 4) | b;
        a = (p >> 12) & 0xF;  a = (a << 4) | a; break;
    }
    return (a << 24) | (b << 16) | (g << 8) | r;
}

static uint32_t clut_entry(uint32_t raw) {
    const uint32_t idx =
        (uint32_t)((((int)raw >> g_clut.shift) & g_clut.mask) | g_clut.start);
    if (g_clut.fmt == 3) return psp_read32(g_clut.addr + idx * 4u);
    return expand16((uint32_t)psp_read16(g_clut.addr + idx * 2u),
                    g_clut.fmt == 0 ? GE_TFMT_5650 :
                    g_clut.fmt == 1 ? GE_TFMT_5551 : GE_TFMT_4444);
}

/* One texel, clamped. Stride is in texels, as the GE reports it, so the byte
 * pitch a swizzle block is measured against has to be derived per format. */
static uint32_t sample_texel(int u, int v) {
    if (u < 0) u = 0; else if (u >= g_tex.w) u = g_tex.w - 1;
    if (v < 0) v = 0; else if (v >= g_tex.h) v = g_tex.h - 1;

    const int hb = tex_halfbytes(g_tex.fmt);
    const uint32_t row_bytes = ((uint32_t)g_tex.stride * (uint32_t)hb) / 2u;

    if (g_tex.fmt == GE_TFMT_CLUT4) {
        const uint32_t bx = (uint32_t)u / 2u;
        const uint32_t off = swizzled_byte(bx, (uint32_t)v, row_bytes);
        const uint32_t byte = psp_read8(g_tex.addr + off);
        return clut_entry((u & 1) ? (byte >> 4) : (byte & 0xF));
    }
    if (g_tex.fmt == GE_TFMT_CLUT8) {
        const uint32_t off = swizzled_byte((uint32_t)u, (uint32_t)v, row_bytes);
        return clut_entry(psp_read8(g_tex.addr + off));
    }
    if (g_tex.fmt == GE_TFMT_8888) {
        const uint32_t off = swizzled_byte((uint32_t)u * 4u, (uint32_t)v, row_bytes);
        return psp_read32(g_tex.addr + off);
    }
    const uint32_t off = swizzled_byte((uint32_t)u * 2u, (uint32_t)v, row_bytes);
    return expand16((uint32_t)psp_read16(g_tex.addr + off), g_tex.fmt);
}

static void dump_texture(void) {
    const char *base = getenv("PSPRECOMP_TEXDUMP");
    if (!base || !*base || g_dumped_n >= 16) return;
    if (!texture_usable()) return;
    for (int i = 0; i < g_dumped_n; i++) if (g_dumped[i] == g_tex.addr) return;
    g_dumped[g_dumped_n] = g_tex.addr;

    char path[1024];
    snprintf(path, sizeof path, "%s-%02d.ppm", base, g_dumped_n);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", g_tex.w, g_tex.h);
    for (int v = 0; v < g_tex.h; v++)
        for (int u = 0; u < g_tex.w; u++) {
            const uint32_t c = sample_texel(u, v);
            const uint8_t rgb[3] = { (uint8_t)(c & 0xFF), (uint8_t)((c >> 8) & 0xFF),
                                     (uint8_t)((c >> 16) & 0xFF) };
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
    fprintf(stderr, "tex: %s  0x%08X %dx%d stride %u fmt %d%s clut 0x%08X fmt %d "
                    "shift %d mask %02X start %d\n",
            path, g_tex.addr, g_tex.w, g_tex.h, g_tex.stride, g_tex.fmt,
            g_tex.swizzled ? " swizzled" : "", g_clut.addr, g_clut.fmt,
            g_clut.shift, g_clut.mask, g_clut.start);
    g_dumped_n++;
}

/* Modulate: texel times vertex colour, per channel. */
static uint32_t modulate(uint32_t tex, uint32_t col) {
    uint32_t out = 0;
    for (int i = 0; i < 4; i++) {
        const uint32_t t = (tex >> (i * 8)) & 0xFF;
        const uint32_t c = (col >> (i * 8)) & 0xFF;
        out |= ((t * c + 127u) / 255u) << (i * 8);
    }
    return out;
}

static void put_pixel(int x, int y, uint32_t rgba) {
    if (!g_fb_addr || !g_fb_stride) return;
    if (x < 0 || y < 0 || x >= 480 || y >= 272) return;
    psp_write32(g_fb_addr + (uint32_t)(y * (int)g_fb_stride + x) * 4, rgba);
    g_pixels++;
}

/* The depth buffer.
 *
 * Kept host-side rather than in guest VRAM at ZBP. The game only ever writes
 * it through the GE, so nothing reads back a value we did not put there, and
 * an array of floats avoids the 16-bit quantisation that would otherwise make
 * coplanar surfaces fight. Cleared once a frame at present(): full clear-mode
 * emulation is a separate piece of work, and a game that does not clear every
 * frame would accumulate depth until nothing drew at all -- which fails in a
 * way that looks like a broken test rather than a missing clear. */
#define DEPTH_FAR 1.0e30f
static float g_depth[480 * 272];
static struct { int test, func, write; } g_zs = { 0, 1 /* always */, 0 };

static void sw_depth(int test_enable, int func, int write_enable) {
    g_zs.test = test_enable; g_zs.func = func; g_zs.write = write_enable;
}

static void depth_clear(void) {
    for (int i = 0; i < 480 * 272; i++) g_depth[i] = DEPTH_FAR;
}

/* GE comparison codes: 0 never, 1 always, 2 equal, 3 notequal, 4 less,
 * 5 lequal, 6 greater, 7 gequal. */
static int depth_pass(int x, int y, float z) {
    if (!g_zs.test) return 1;
    const float d = g_depth[y * 480 + x];
    switch (g_zs.func) {
    case 0: return 0;
    case 2: return z == d;
    case 3: return z != d;
    case 4: return z <  d;
    case 5: return z <= d;
    case 6: return z >  d;
    case 7: return z >= d;
    default: return 1;
    }
}

/* ---- blending ------------------------------------------------------------ */

static psp_blend_state g_bs;

uint64_t psp_render_blended_pixels(void) { return g_px_blend; }
uint64_t psp_render_alphakill_pixels(void) { return g_px_atest; }

static void sw_blend(const psp_blend_state *b) { g_bs = *b; }

static uint32_t chan(uint32_t c, int i) { return (c >> (i * 8)) & 0xFFu; }

static uint32_t clamp255(int v) { return v < 0 ? 0u : (v > 255 ? 255u : (uint32_t)v); }

/* A blend factor, per channel, on the 0..255 scale the channels use. The
 * doubling variants are the PSP's way of reaching 2x without a separate
 * equation, and they saturate rather than wrap. */
static uint32_t blend_factor(int code, int i, uint32_t src, uint32_t dst, int is_src) {
    const uint32_t sa = chan(src, 3), da = chan(dst, 3);
    switch (code) {
    case 0:  return is_src ? chan(dst, i) : chan(src, i);
    case 1:  return 255u - (is_src ? chan(dst, i) : chan(src, i));
    case 2:  return sa;
    case 3:  return 255u - sa;
    case 4:  return da;
    case 5:  return 255u - da;
    case 6:  return clamp255((int)sa * 2);
    case 7:  return clamp255(510 - (int)sa * 2);
    case 8:  return clamp255((int)da * 2);
    case 9:  return clamp255(510 - (int)da * 2);
    default: return chan(is_src ? g_bs.fixa : g_bs.fixb, i);
    }
}

static uint32_t blend(uint32_t src, uint32_t dst) {
    uint32_t out = 0;
    for (int i = 0; i < 4; i++) {
        const int s = (int)chan(src, i), d = (int)chan(dst, i);
        const int fs = (int)blend_factor(g_bs.src, i, src, dst, 1);
        const int fd = (int)blend_factor(g_bs.dst, i, src, dst, 0);
        const int ss = (s * fs + 127) / 255, dd = (d * fd + 127) / 255;
        int v;
        switch (g_bs.eq) {
        case 1:  v = ss - dd; break;
        case 2:  v = dd - ss; break;
        case 3:  v = s < d ? s : d; break;
        case 4:  v = s > d ? s : d; break;
        case 5:  v = s > d ? s - d : d - s; break;
        default: v = ss + dd; break;
        }
        out |= clamp255(v) << (i * 8);
    }
    return out;
}

static int alpha_pass(uint32_t rgba) {
    if (!g_bs.alpha_test) return 1;
    const int a = (int)(chan(rgba, 3) & (uint32_t)g_bs.alpha_mask);
    const int r = g_bs.alpha_ref & g_bs.alpha_mask;
    switch (g_bs.alpha_func) {
    case 0: return 0;
    case 2: return a == r;
    case 3: return a != r;
    case 4: return a <  r;
    case 5: return a <= r;
    case 6: return a >  r;
    case 7: return a >= r;
    default: return 1;
    }
}

static uint32_t get_pixel(int x, int y) {
    if (!g_fb_addr || !g_fb_stride) return 0;
    return psp_read32(g_fb_addr + (uint32_t)(y * (int)g_fb_stride + x) * 4);
}

static void shade_pixel(int x, int y, float z, uint32_t rgba) {
    if (x < 0 || y < 0 || x >= 480 || y >= 272) return;
    if (!alpha_pass(rgba)) { g_px_atest++; return; }
    if (!depth_pass(x, y, z)) { g_px_zfail++; return; }
    if (g_zs.write) g_depth[y * 480 + x] = z;
    if (g_bs.enable) { rgba = blend(rgba, get_pixel(x, y)); g_px_blend++; }
    put_pixel(x, y, rgba);
}

/* Barycentric fill with integer edge functions, so a shared edge belongs to
 * exactly one triangle: adjacent geometry neither double-draws nor leaves
 * seams. Both windings are accepted — back-face culling is not implemented, and
 * rejecting one winding would silently drop half of any real model. */
static void sw_tri(const psp_vertex *a, const psp_vertex *b, const psp_vertex *c) {
    int minx = a->x < b->x ? (a->x < c->x ? a->x : c->x) : (b->x < c->x ? b->x : c->x);
    int maxx = a->x > b->x ? (a->x > c->x ? a->x : c->x) : (b->x > c->x ? b->x : c->x);
    int miny = a->y < b->y ? (a->y < c->y ? a->y : c->y) : (b->y < c->y ? b->y : c->y);
    int maxy = a->y > b->y ? (a->y > c->y ? a->y : c->y) : (b->y > c->y ? b->y : c->y);

    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx > 479) maxx = 479;
    if (maxy > 271) maxy = 271;

    const int area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
    if (area == 0) return;

    /* The edge functions are already the barycentric numerators, so colour,
     * depth and texture coordinates come out of the same three values the
     * coverage test computes. Filling with a->rgba instead -- which is what
     * this did -- paints every triangle one flat colour and ignores the
     * texture entirely, which reads as "the geometry is not arriving" when the
     * geometry is arriving and being shaded wrong.
     *
     * Interpolation is affine, not perspective-correct: there is no w here to
     * divide by. On a fullscreen quad that is exact, and on a steeply oblique
     * one it skews the texture. */
    const float inv = 1.0f / (float)area;
    const int textured = texture_usable();

    for (int y = miny; y <= maxy; y++) {
        for (int x = minx; x <= maxx; x++) {
            const int w0 = (c->x - b->x) * (y - b->y) - (c->y - b->y) * (x - b->x);
            const int w1 = (a->x - c->x) * (y - c->y) - (a->y - c->y) * (x - c->x);
            const int w2 = (b->x - a->x) * (y - a->y) - (b->y - a->y) * (x - a->x);
            if (!((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0)))
                continue;

            const float l0 = (float)w0 * inv;
            const float l1 = (float)w1 * inv;
            const float l2 = (float)w2 * inv;

            const float z = l0 * a->z + l1 * b->z + l2 * c->z;

            uint32_t col = 0;
            for (int i = 0; i < 4; i++) {
                float ch = l0 * (float)((a->rgba >> (i * 8)) & 0xFF)
                         + l1 * (float)((b->rgba >> (i * 8)) & 0xFF)
                         + l2 * (float)((c->rgba >> (i * 8)) & 0xFF);
                if (ch < 0.0f) ch = 0.0f; else if (ch > 255.0f) ch = 255.0f;
                col |= (uint32_t)(ch + 0.5f) << (i * 8);
            }

            if (textured) {
                const float u = l0 * a->u + l1 * b->u + l2 * c->u;
                const float v = l0 * a->v + l1 * b->v + l2 * c->v;
                col = modulate(sample_texel((int)u, (int)v), col);
                g_px_tex++;
            } else g_px_flat++;
            shade_pixel(x, y, z, col);
        }
    }
}

/* A sprite is the PSP's 2D primitive: two vertices giving opposite corners of
 * an axis-aligned rectangle. The far edge is exclusive so adjacent sprites tile
 * without overlapping. */
/* A sprite is an axis-aligned quad given by two corners, so its texture map is
 * a straight linear ramp in each axis -- no barycentric weights, no
 * perspective. The colour comes from the second vertex, which is where the GE
 * takes it from. */
static void sw_sprite(const psp_vertex *a, const psp_vertex *b) {
    const int x0 = a->x < b->x ? a->x : b->x, x1 = a->x > b->x ? a->x : b->x;
    const int y0 = a->y < b->y ? a->y : b->y, y1 = a->y > b->y ? a->y : b->y;
    if (x1 <= x0 || y1 <= y0) return;

    const int textured = texture_usable();
    const float du = (b->u - a->u) / (float)(x1 - x0);
    const float dv = (b->v - a->v) / (float)(y1 - y0);

    for (int y = y0; y < y1; y++) {
        const float tv = a->v + dv * (float)(y - y0);
        for (int x = x0; x < x1; x++) {
            if (!textured) { g_px_flat++; shade_pixel(x, y, a->z, b->rgba); continue; }
            const float tu = a->u + du * (float)(x - x0);
            g_px_tex++;
            shade_pixel(x, y, a->z,
                        modulate(sample_texel((int)tu, (int)tv), b->rgba));
        }
    }
}

static void sw_draw(int prim, const psp_vertex *v, int count) {
    switch (prim) {
    case PSP_PRIM_SPRITES:
        for (int i = 0; i + 1 < count; i += 2) sw_sprite(&v[i], &v[i + 1]);
        break;
    case PSP_PRIM_TRIANGLES:
        for (int i = 0; i + 2 < count; i += 3) sw_tri(&v[i], &v[i + 1], &v[i + 2]);
        break;
    case PSP_PRIM_TRIANGLE_STRIP:
        for (int i = 0; i + 2 < count; i++) sw_tri(&v[i], &v[i + 1], &v[i + 2]);
        break;
    default:
        break;                       /* points, lines, fans: not yet */
    }
}

static void sw_noop(void) { }

/* Depth is cleared here rather than on a clear-mode draw: one clear a frame is
 * what a game does anyway, and it fails safe. Accumulating depth across frames
 * would progressively reject everything, which looks like a broken test. */
static void sw_present(void) { depth_clear(); }

const psp_render_backend psp_render_software = {
    .name        = "software",
    .init        = sw_init,
    .shutdown    = sw_shutdown,
    .set_target  = sw_target,
    .set_texture = sw_texture,
    .set_clut    = sw_clut,
    .set_depth   = sw_depth,
    .set_blend   = sw_blend,
    .draw        = sw_draw,
    .finish      = sw_noop,
    .present     = sw_present,
};

/* ---- null backend -------------------------------------------------------- */

/* Draws nothing, and that is the point. During bring-up the question is
 * usually "did the game ask to draw" rather than "does it look right", and the
 * answer is clearer without pixels in the way. */

static int null_init(int w, int h) { (void)w; (void)h; return 0; }
static void null_target(uint32_t a, uint32_t s, int f) { (void)a; (void)s; (void)f; }
static void null_texture(uint32_t a, uint32_t s, int w, int h, int f, int fn, int z) {
    (void)a; (void)s; (void)w; (void)h; (void)f; (void)fn; (void)z;
}
static void null_clut(uint32_t a, int f, int s, int m, int st) {
    (void)a; (void)f; (void)s; (void)m; (void)st;
}
static void null_depth(int t, int f, int w) { (void)t; (void)f; (void)w; }
static void null_blend(const psp_blend_state *b) { (void)b; }
static void null_draw(int p, const psp_vertex *v, int n) { (void)p; (void)v; (void)n; }
static void null_noop(void) { }

const psp_render_backend psp_render_null = {
    .name        = "null",
    .init        = null_init,
    .shutdown    = null_noop,
    .set_target  = null_target,
    .set_texture = null_texture,
    .set_clut    = null_clut,
    .set_depth   = null_depth,
    .set_blend   = null_blend,
    .draw        = null_draw,
    .finish      = null_noop,
    .present     = null_noop,
};

/* ---- selection ----------------------------------------------------------- */

static const psp_render_backend *g_backend = &psp_render_software;

const psp_render_backend *psp_render_current(void) { return g_backend; }

int psp_render_select(const char *name) {
    static const psp_render_backend *const all[] = {
        &psp_render_software, &psp_render_null
    };
    if (!name) return -1;
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
        if (strcmp(all[i]->name, name) == 0) { g_backend = all[i]; return 0; }
    }
    return -1;                       /* unknown: keep the current backend */
}
