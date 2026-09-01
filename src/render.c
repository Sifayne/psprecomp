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
#include "psprecomp/os.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
static uint64_t g_filter_split;
static uint64_t g_raster_ns;         /* see sw_draw */
void     psp_render_reset_pixels(void) {
    g_pixels = g_px_tex = g_px_flat = g_px_zfail = g_px_blend = g_px_atest = 0;
    g_filter_split = 0;
    g_raster_ns = 0;
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
 * Sampled at pixel centres, nearest or bilinear as GE_TEXFILTER asks. The claim
 * that used to stand here -- that a 1:1 blit "samples texel centres either
 * way", so the filter state did not matter -- was exactly backwards: at 1:1 a
 * corner sample lands on the texel *boundary*, which is the one place a few ULP
 * of interpolation error changes the answer. See sw_tri. */
static psp_tex_state g_tex;

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

static void sw_texture(const psp_tex_state *t) {
    g_tex = *t;
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

/* Floor, not truncate.
 *
 * (int) rounds toward zero, so a u in (-1, 0) lands on texel 0 rather than -1:
 * invisible while the sampler clamps, half a texture out the moment one
 * repeats, and an inverted fractional part for anything that wants the weights.
 * floorf would do it, but this file has no <math.h> and on the SSE2 baseline
 * that is a libm call in a per-pixel loop. */
static int ifloor(float f) { const int i = (int)f; return i - (f < (float)i); }

/* One axis of a texel coordinate, wrapped the way the game asked.
 *
 * Every PSP texture dimension is a power of two -- TEXSIZE gives log2 of each,
 * so it cannot express anything else -- which makes repeat a mask, and the mask
 * is already right for negative coordinates: -1 & 511 is 511, which is what
 * flooring and then wrapping means. A size that is somehow not a power of two
 * degrades to clamping rather than to garbage.
 *
 * This clamped unconditionally, which is not a neutral default. It is the
 * opposite of what this game sets on both axes, and it turns geometry that
 * should have tiled into one smeared edge texel. */
static int wrap_axis(int t, int size, int clamp) {
    if (!clamp && size > 0 && (size & (size - 1)) == 0) return t & (size - 1);
    return t < 0 ? 0 : (t >= size ? size - 1 : t);
}

/* One texel. Stride is in texels, as the GE reports it, so the byte pitch a
 * swizzle block is measured against has to be derived per format. */
static uint32_t sample_texel(int u, int v) {
    u = wrap_axis(u, g_tex.w, g_tex.wrap_s);
    v = wrap_axis(v, g_tex.h, g_tex.wrap_t);

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
    /* And the alpha channel, as greyscale. The colour dump of an alpha-mask
     * texture is uniformly white and says nothing; the shape is entirely in
     * alpha, and so is any question about how it composites. */
    snprintf(path, sizeof path, "%s-%02d-alpha.ppm", base, g_dumped_n);
    f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", g_tex.w, g_tex.h);
        for (int v = 0; v < g_tex.h; v++)
            for (int u = 0; u < g_tex.w; u++) {
                const uint8_t a = (uint8_t)((sample_texel(u, v) >> 24) & 0xFF);
                const uint8_t rgb[3] = { a, a, a };
                fwrite(rgb, 1, 3, f);
            }
        fclose(f);
    }
    fprintf(stderr, "tex: %s  0x%08X %dx%d stride %u fmt %d%s clut 0x%08X fmt %d "
                    "shift %d mask %02X start %d\n",
            path, g_tex.addr, g_tex.w, g_tex.h, g_tex.stride, g_tex.fmt,
            g_tex.swizzled ? " swizzled" : "", g_clut.addr, g_clut.fmt,
            g_clut.shift, g_clut.mask, g_clut.start);
    g_dumped_n++;
}

/* ---- filtering -----------------------------------------------------------
 *
 * GE_TEXFILTER was parsed into the GE's state from the beginning and read by
 * nothing: set_texture had no filter parameter, so the state could not reach a
 * backend even in principle. This game asks for linear on both fields.
 *
 * Values are GU_NEAREST 0 and GU_LINEAR 1, then 4..7 for the mipmap variants
 * (2 and 3 are not legal). Bit 0 selects linear *within* the level across all
 * six, so the mip chain -- which nothing here builds -- does not have to exist
 * for the in-level choice to be right. */

uint64_t psp_render_filter_split(void) { return g_filter_split; }

static int filter_is_linear(void) {
    /* Magnification, unconditionally. Choosing properly needs the
     * pixel-to-texel scale, which needs derivatives this rasterizer does not
     * compute, and there would be no mip chain to select from if it did. Mag is
     * the honest default -- it is the one that applies at the scale a UI layer
     * draws at, and it is what a scale factor of 1 selects anyway.
     *
     * The counter asks whether that choice ever *matters*, which is not the
     * same as whether the two fields differ. This game sets min 5 and mag 1 --
     * different values that both mean linear within the level -- so comparing
     * them raw would fire on every primitive and measure nothing. Comparing the
     * bit that selects the filter is the question actually being asked, and a
     * non-zero count is the evidence that a per-primitive scale factor is worth
     * the division it would cost. */
    if ((g_tex.min_filter & 1) != (g_tex.mag_filter & 1)) g_filter_split++;
    return g_tex.mag_filter & 1;
}

/* Bilinear, with the half-texel that makes it agree with nearest at 1:1.
 *
 * Texel k covers [k, k+1), so its centre is at k + 0.5 and the four taps belong
 * around u - 0.5. Dropping that offset is the usual way to get this wrong: it
 * averages every texel with its neighbour during a 1:1 blit and softens a UI
 * layer that the hardware leaves sharp. With it, a 1:1 blit lands on frac 0,
 * puts all the weight on one tap, and comes out bit-identical to nearest.
 *
 * All four channels, alpha included. This game's logo is an alpha mask -- white
 * throughout, the letterforms entirely in the alpha channel -- so filtering
 * only RGB and taking alpha from a single tap would leave the edges exactly as
 * hard as nearest and look like the filter had never been implemented. */
static uint32_t sample_bilinear(float u, float v) {
    const float fu = u - 0.5f, fv = v - 0.5f;
    const int   u0 = ifloor(fu), v0 = ifloor(fv);
    const float au = fu - (float)u0, av = fv - (float)v0;

    const uint32_t t00 = sample_texel(u0,     v0);
    const uint32_t t10 = sample_texel(u0 + 1, v0);
    const uint32_t t01 = sample_texel(u0,     v0 + 1);
    const uint32_t t11 = sample_texel(u0 + 1, v0 + 1);

    const float w00 = (1.0f - au) * (1.0f - av), w10 = au * (1.0f - av);
    const float w01 = (1.0f - au) * av,          w11 = au * av;

    uint32_t out = 0;
    for (int i = 0; i < 4; i++) {
        const float s = w00 * (float)((t00 >> (i * 8)) & 0xFF)
                      + w10 * (float)((t10 >> (i * 8)) & 0xFF)
                      + w01 * (float)((t01 >> (i * 8)) & 0xFF)
                      + w11 * (float)((t11 >> (i * 8)) & 0xFF);
        const int q = (int)(s + 0.5f);
        out |= (uint32_t)(q < 0 ? 0 : (q > 255 ? 255 : q)) << (i * 8);
    }
    return out;
}

/* The sampler the rasterizer calls: one texel, filtered as the game asked. */
static uint32_t sample_filtered(float u, float v, int linear) {
    return linear ? sample_bilinear(u, v) : sample_texel(ifloor(u), ifloor(v));
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
 * coplanar surfaces fight.
 *
 * The *game* clears it, not us. A clear-mode draw with the depth bit set writes
 * its own z across the rectangle it covers, which is what the hardware does and
 * what ge.c now asks for. There is deliberately no host-side "clear to far":
 * "far" is whichever end of the 0..65535 window the game's comparison treats as
 * farthest, and only the game knows which. This one runs GEQUAL, where farthest
 * is 0 -- a buffer cleared to 65535 would fail every one of those tests and draw
 * nothing at all. That is the trap an earlier `#define DEPTH_FAR 1.0e30f` fell
 * into, and lowering it to 65535 did not climb out: for GEQUAL both values
 * reject everything.
 *
 * The reset value below is therefore the one that rejects nothing under the
 * comparison this game uses, so the frames before its first clear draw rather
 * than vanish. It is a placeholder for a real per-title depth convention, not a
 * claim about hardware; a LEQUAL title needs the other end and will need this
 * revisited. */
#define DEPTH_RESET 0.0f
static float g_depth[480 * 272];
static struct { int test, func, write; } g_zs = { 0, 1 /* always */, 0 };

static void sw_depth(int test_enable, int func, int write_enable) {
    g_zs.test = test_enable; g_zs.func = func; g_zs.write = write_enable;
}

/* Called from psp_ge_reset, so a second run in the same process -- the test
 * suite does exactly that -- does not inherit the previous run's depth.
 *
 * Deliberately *not* hung off the backend's init() hook, which looks like the
 * natural home and is never called; neither is shutdown() or present(). Putting
 * the reset there would reproduce the bug this replaces, where the only call to
 * the depth clear sat in an unwired vtable slot. */
void psp_render_reset_depth(void) {
    for (int i = 0; i < 480 * 272; i++) g_depth[i] = DEPTH_RESET;
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

static psp_blend_state g_bs = { .write_colour = 1 };

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
    if (!g_bs.write_colour) return;
    if (g_bs.enable) { rgba = blend(rgba, get_pixel(x, y)); g_px_blend++; }
    put_pixel(x, y, rgba);
}

/* Sample positions per pixel edge. Two is enough to express the pixel centre
 * as an integer -- 2*(x + 0.5) is 2x + 1 -- which is all this needs today.
 *
 * Named rather than written as literal 2s because sub-pixel vertex precision
 * is the same substitution: the hardware rasterizes at sixteenths, so SUBPX 16
 * with positions snapped to 28.4 turns this function into that one without
 * touching its structure. */
#define SUBPX      2
#define SUBPX_HALF 1

/* A directed edge owns the pixels lying exactly on it if it is a top or a left
 * edge of the triangle. Screen y grows downward, and the caller has normalised
 * the winding so the interior is where every edge function is non-negative;
 * under that convention a top edge runs left-to-right and a left edge runs
 * upward. Two triangles sharing an edge traverse it in opposite directions, so
 * exactly one of them satisfies this and the shared pixels are drawn once. */
static int edge_is_top_left(int64_t dx, int64_t dy) {
    return (dy == 0 && dx > 0) || dy < 0;
}

/* Barycentric fill with integer edge functions, evaluated at pixel centres.
 *
 * Sampling at the pixel *corner* -- which is what this did -- puts the sample
 * point exactly on a texel boundary whenever the blit is 1:1, which is what a
 * UI layer is. The barycentric reconstruction carries a few ULP of error, so
 * (int)u came back as N or N-1 pseudo-randomly, per pixel. Inside a glyph both
 * texels are the same and nothing shows; on its outline the neighbour is
 * background, so the letters came out with texel-sized holes punched along
 * every edge. Half a pixel across is half a texel from that discontinuity, and
 * the error stops deciding anything.
 *
 * Coverage and attributes move together. Testing coverage at the corner and
 * interpolating at the centre would let the weights go slightly negative on a
 * silhouette pixel, running u and v up to half a texel past the geometry. */
static void sw_tri(const psp_vertex *a, const psp_vertex *b, const psp_vertex *c) {
    int minx = a->x < b->x ? (a->x < c->x ? a->x : c->x) : (b->x < c->x ? b->x : c->x);
    int maxx = a->x > b->x ? (a->x > c->x ? a->x : c->x) : (b->x > c->x ? b->x : c->x);
    int miny = a->y < b->y ? (a->y < c->y ? a->y : c->y) : (b->y < c->y ? b->y : c->y);
    int maxy = a->y > b->y ? (a->y > c->y ? a->y : c->y) : (b->y > c->y ? b->y : c->y);

    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx > 479) maxx = 479;
    if (maxy > 271) maxy = 271;

    /* 64-bit because through-mode positions are s16: a coordinate difference
     * reaches 65535 and the product 2.2e9, which overflowed the int this used
     * and inverted coverage for the whole triangle. */
    int64_t area = (int64_t)(b->x - a->x) * (c->y - a->y)
                 - (int64_t)(b->y - a->y) * (c->x - a->x);
    if (area == 0) return;

    /* Normalise the winding rather than accepting both. The fill rule below is
     * a tie-break between two triangles that disagree about a pixel, and it can
     * only be one if both are asked the same question -- applied to a mixed
     * pair it drops the shared edge instead of assigning it. Swapping two
     * vertices flips the sign and reverses every edge; the weights follow,
     * because the pointers moved with them. */
    if (area < 0) { const psp_vertex *t = b; b = c; c = t; area = -area; }

    const int64_t d0x = c->x - b->x, d0y = c->y - b->y;
    const int64_t d1x = a->x - c->x, d1y = a->y - c->y;
    const int64_t d2x = b->x - a->x, d2y = b->y - a->y;

    /* Accept w > 0 always, w == 0 only on a top-left edge. As a bias that is
     * -1 for the edges that do not own their boundary, which is exact against
     * these values because the only tie is exact zero. */
    const int64_t bias0 = edge_is_top_left(d0x, d0y) ? 0 : -1;
    const int64_t bias1 = edge_is_top_left(d1x, d1y) ? 0 : -1;
    const int64_t bias2 = edge_is_top_left(d2x, d2y) ? 0 : -1;

    /* The three edge functions at the centre of the first pixel, scaled by
     * SUBPX. They still sum to SUBPX * area, so they are still the barycentric
     * numerators -- only the denominator changes. Stepping them by their own
     * derivatives keeps every value exact and takes the six multiplies out of
     * the inner loop. */
    const int64_t px = (int64_t)SUBPX * minx + SUBPX_HALF;
    const int64_t py = (int64_t)SUBPX * miny + SUBPX_HALF;
    int64_t row0 = d0x * (py - (int64_t)SUBPX * c->y) - d0y * (px - (int64_t)SUBPX * c->x);
    int64_t row1 = d1x * (py - (int64_t)SUBPX * a->y) - d1y * (px - (int64_t)SUBPX * a->x);
    int64_t row2 = d2x * (py - (int64_t)SUBPX * b->y) - d2y * (px - (int64_t)SUBPX * b->x);

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
    const float inv = 1.0f / (float)((int64_t)SUBPX * area);
    const int textured = texture_usable();
    const int linear = textured && filter_is_linear();

    for (int y = miny; y <= maxy; y++) {
        int64_t w0 = row0, w1 = row1, w2 = row2;
        for (int x = minx; x <= maxx; x++) {
            if (w0 + bias0 >= 0 && w1 + bias1 >= 0 && w2 + bias2 >= 0) {
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
                    col = modulate(sample_filtered(u, v, linear), col);
                    g_px_tex++;
                } else g_px_flat++;
                shade_pixel(x, y, z, col);
            }
            w0 -= d0y * SUBPX; w1 -= d1y * SUBPX; w2 -= d2y * SUBPX;
        }
        row0 += d0x * SUBPX; row1 += d1x * SUBPX; row2 += d2x * SUBPX;
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
    const int linear = textured && filter_is_linear();
    /* Against the vertices as submitted, not against the sorted corners. The
     * ramp used to be built from x1 - x0, which is positive by construction, so
     * a sprite whose second corner is left of or above its first mapped its
     * texture backwards. The guard above is what makes these divisions safe:
     * the corners can only coincide if the extents are empty. */
    const float du = (b->u - a->u) / (float)(b->x - a->x);
    const float dv = (b->v - a->v) / (float)(b->y - a->y);

    for (int y = y0; y < y1; y++) {
        /* Pixel centres, for the same reason sw_tri uses them: at 1:1 a corner
         * lands exactly on a texel boundary and the rounding decides which side
         * of it to read. */
        const float tv = a->v + dv * ((float)y + 0.5f - (float)a->y);
        for (int x = x0; x < x1; x++) {
            if (!textured) { g_px_flat++; shade_pixel(x, y, a->z, b->rgba); continue; }
            const float tu = a->u + du * ((float)x + 0.5f - (float)a->x);
            g_px_tex++;
            shade_pixel(x, y, a->z,
                        modulate(sample_filtered(tu, tv, linear), b->rgba));
        }
    }
}

/* Wall-clock nanoseconds spent inside the rasterizer.
 *
 * The question this exists to answer is whether a software rasterizer is the
 * architecture or a placeholder: everything since the transform pipeline is
 * built on it, and if a real frame costs tens of milliseconds then the GE
 * wants GPU-backed display-list translation instead. That is a decision, and
 * it should be made against a number rather than an impression.
 *
 * Timed around sw_draw rather than around the pixel loop: what matters is the
 * cost of a primitive as the GE hands it over, setup and clipping included. A
 * per-pixel timer would also cost more than the work it measures.
 *
 * CLOCK_MONOTONIC, not CLOCK_PROCESS_CPUTIME_ID -- the guest is paced in real
 * time and the interesting quantity is how much of a frame's 16.7ms budget
 * this consumes, not how many cycles it retires. */
static uint64_t now_ns(void) { return psp_os_mono_ns(); }

uint64_t psp_render_raster_ns(void) { return g_raster_ns; }

static void sw_draw(int prim, const psp_vertex *v, int count) {
    const uint64_t t0 = now_ns();
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
    g_raster_ns += now_ns() - t0;
}

static void sw_noop(void) { }

/* init(), shutdown() and present() are all unwired: nothing in the runtime or
 * the host calls any of them. They stay because a windowed backend will need
 * them, but check that before hanging behaviour off one. This slot used to hold
 * the depth clear, and a clear that is never called is a buffer that is never
 * cleared -- the depth buffer spent the whole run at its initial contents and
 * the comment above it described a per-frame clear that did not happen. Depth is
 * now cleared where the game asks for it, in ge.c's clear-mode path; per-frame
 * work lives in hle_SetFrameBuf until something actually drives present(). */
static void sw_present(void) { }

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
static void null_texture(const psp_tex_state *t) { (void)t; }
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
