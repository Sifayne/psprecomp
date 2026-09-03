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

#include <math.h>
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

/* The scissor, inclusive corners; the raster bound everywhere below. */
static int g_sc_x0 = 0, g_sc_y0 = 0, g_sc_x1 = 479, g_sc_y1 = 271;
enum { DEPTH_STRIDE = 512, DEPTH_ROWS = 272 };

static void sw_scissor(int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > DEPTH_STRIDE - 1) x1 = DEPTH_STRIDE - 1;
    if (y1 > DEPTH_ROWS - 1)   y1 = DEPTH_ROWS - 1;
    g_sc_x0 = x0; g_sc_y0 = y0; g_sc_x1 = x1; g_sc_y1 = y1;
}

/* FRAMEBUF_PIX_FORMAT (0xD2): 0 5650, 1 5551, 2 4444, 3 8888. Until this
 * was honoured every target was written as 8888 -- four bytes a pixel into a
 * buffer laid out for two -- so a scene rendered into a 16-bit target and
 * read back as a texture came back as noise. */
static int g_fb_fmt = 3;

static void sw_target(uint32_t addr, uint32_t stride, int fmt) {
    g_fb_addr = addr;
    g_fb_stride = stride;
    g_fb_fmt = fmt & 3;
}

static uint32_t pack16(uint32_t rgba, int fmt) {
    const uint32_t r = rgba & 0xFF, g = (rgba >> 8) & 0xFF, b = (rgba >> 16) & 0xFF, a = rgba >> 24;
    switch (fmt) {
    case 0:  return (r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11);
    case 1:  return (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | ((a >> 7) << 15);
    default: return (r >> 4) | ((g >> 4) << 4) | ((b >> 4) << 8) | ((a >> 4) << 12);
    }
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
/* Sixteen was not enough to reach past the first screen: the title alone binds
 * that many, so a dump taken to look at a later menu's glyphs contained the
 * title's textures and nothing else -- an instrument that answers, plausibly,
 * about the wrong thing. */
#define TEXDUMP_MAX 256
static uint32_t g_dumped[TEXDUMP_MAX];
static int      g_dumped_n;

static void dump_texture(void);

static void sw_texture(const psp_tex_state *t) {
    g_tex = *t;
    dump_texture();
}

enum {
    GE_TFMT_5650 = 0, GE_TFMT_5551 = 1, GE_TFMT_4444 = 2, GE_TFMT_8888 = 3,
    GE_TFMT_CLUT4 = 4, GE_TFMT_CLUT8 = 5, GE_TFMT_CLUT16 = 6, GE_TFMT_CLUT32 = 7
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
    case GE_TFMT_4444: case GE_TFMT_CLUT16: return 4;
    case GE_TFMT_8888: case GE_TFMT_CLUT32: return 8;
    default:                                return 0;
    }
}

static int texture_usable(void) {
    if (!g_tex.addr || g_tex.w <= 0 || g_tex.h <= 0) return 0;
    if (!tex_halfbytes(g_tex.fmt)) return 0;
    if (g_tex.fmt >= GE_TFMT_CLUT4 && !g_clut.addr) return 0;
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
    /* Wide indices: the whole 16- or 32-bit texel is the raw index, and the
     * palette mode's shift and mask pick the bits that count. gpu/clut/shifts
     * and masks are written against these -- a texel of 12345678 shifted 4 and
     * masked ff indexes entry 67 -- and they drew flat white while the formats
     * were refused. This game's census has never named either; the tests are
     * the reason they exist. */
    if (g_tex.fmt == GE_TFMT_CLUT16) {
        const uint32_t off = swizzled_byte((uint32_t)u * 2u, (uint32_t)v, row_bytes);
        return clut_entry(psp_read16(g_tex.addr + off));
    }
    if (g_tex.fmt == GE_TFMT_CLUT32) {
        const uint32_t off = swizzled_byte((uint32_t)u * 4u, (uint32_t)v, row_bytes);
        return clut_entry(psp_read32(g_tex.addr + off));
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
    if (!base || !*base || g_dumped_n >= TEXDUMP_MAX) return;
    if (!texture_usable()) return;
    for (int i = 0; i < g_dumped_n; i++) if (g_dumped[i] == g_tex.addr) return;
    g_dumped[g_dumped_n] = g_tex.addr;

    /* The filename carries the texture's identity, not just its ordinal: with
     * a hundred of them the question is always "which of these is the one the
     * GE summary named", and an index alone cannot answer it. */
    static const char *FMT[8] = { "5650","5551","4444","8888","clut4","clut8","clut16","clut32" };
    char path[1024];
    snprintf(path, sizeof path, "%s-%03d-%08X-%dx%d-%s.ppm", base, g_dumped_n,
             g_tex.addr, g_tex.w, g_tex.h, FMT[g_tex.fmt & 7]);
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
    snprintf(path, sizeof path, "%s-%03d-%08X-%dx%d-%s-alpha.ppm", base, g_dumped_n,
             g_tex.addr, g_tex.w, g_tex.h, FMT[g_tex.fmt & 7]);
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
/* Bilinear, with the weights hardware uses: the fraction of a texel is kept to
 * four bits, floored. gpu/filtering/precisionlinear2d stretches two texels over
 * 256 pixels, so a continuous filter would start changing the colour at pixel
 * 64, by one step; hardware first changes it at pixel 72 -- one sixteenth of a
 * texel in -- and by 0x10 (ff -> ef), and on the reversed sprite starts at 64,
 * where the fraction is already fifteen sixteenths. Sixteen levels, floored. */
static uint32_t sample_bilinear(float u, float v) {
    /* The coordinate is quantised to sixteenths, floored, and split into the
     * texel and the weight. Floored, not rounded: gpu/filtering's linear tests
     * stretch two texels over 256 pixels and hardware holds the first colour
     * until a full sixteenth of a texel has accumulated -- rounding starts the
     * ramp a pixel early, everywhere.
     *
     * The epsilon is for our own arithmetic, not hardware's. The interpolated
     * coordinate carries a few ULP of error, and at a 1:1 blit that puts it
     * just below a texel boundary: the weight becomes fifteen sixteenths on
     * the texel below, and truncation then drops a whole texel, so linear
     * stopped agreeing with nearest at the one scale where they must agree.
     * A thousandth of a texel is four orders of magnitude below the sixteenth
     * being measured and cannot move a weight hardware would place elsewhere. */
    const int fu = ifloor((u - 0.5f) * 16.0f + 1.0e-3f);
    const int fv = ifloor((v - 0.5f) * 16.0f + 1.0e-3f);
    const int   u0 = fu >> 4, v0 = fv >> 4;
    const float au = (float)(fu & 15) / 16.0f;
    const float av = (float)(fv & 15) / 16.0f;

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
        /* Truncated, not rounded. gpu/filtering/precisionlinear2d blends 00
         * and ff one sixteenth of the way across and hardware answers 0f, not
         * the 10 that rounding gives; at the halfway point it answers 7f, not
         * 80. Every one of this test's 35 remaining value differences was this
         * half-step. */
        const int q = (int)s;
        out |= (uint32_t)(q < 0 ? 0 : (q > 255 ? 255 : q)) << (i * 8);
    }
    return out;
}

/* The sampler the rasterizer calls: one texel, filtered as the game asked. */
static uint32_t sample_filtered(float u, float v, int linear) {
    return linear ? sample_bilinear(u, v) : sample_texel(ifloor(u), ifloor(v));
}

/* Mipmapping, to gpu/textures/mipmap's numbers.
 *
 * The level of detail is a count of sixteenths. In AUTO mode it is log2 of
 * the texel-per-pixel ratio -- the larger of the two axes: the test's
 * "Minify 4x W", which shrinks only the width, lands on level 2 -- plus the
 * bias; CONST is the bias alone; SLOPE is the slope register plus the bias
 * (the test sets a slope of 2.0 and reads level 2 with no bias, which this
 * fits and log2 would not); the undefined mode 3 measures exactly like
 * CONST. The level is the floor, capped at TEX_MODE's top
 * level, and below zero is zero. With a mip-linear minification filter the
 * next level is blended in by the fraction, exact to the sixteenth: bias
 * +07 at 1:1 reads 07 between levels coloured 00 and 10, and +87 (-7 9/16)
 * at 256x reads 07 again. A filter without mipmapping stays on level 0
 * whatever the ratio. Mip-nearest's rounding is not measured by the test as
 * built -- it was compiled with the linear variant -- so it rounds half up.
 *
 * The level is chosen once per primitive, from the primitive's own texture
 * gradient, not per pixel. Within a level the filter is the min filter when
 * minifying and the mag filter when magnifying. */
static int lod_sixteenths(float rho) {
    int lod;
    switch (g_tex.lod_mode) {
    case 0:  lod = (rho > 0.0f) ? (int)floorf(log2f(rho) * 16.0f) : -4096; break;
    case 2:  lod = (int)floorf(g_tex.lod_slope * 16.0f); break;
    default: lod = 0; break;   /* CONST; and the undefined mode 3 measures the same */
    }
    return lod + g_tex.lod_bias16;
}

static uint32_t sample_level(float u, float v, int L, int linear) {
    const uint32_t a = g_tex.addr, s = g_tex.stride; const int w = g_tex.w, h = g_tex.h;
    g_tex.addr = g_tex.lv_addr[L]; g_tex.stride = g_tex.lv_stride[L];
    g_tex.w = g_tex.lv_w[L] > 0 ? g_tex.lv_w[L] : 1;
    g_tex.h = g_tex.lv_h[L] > 0 ? g_tex.lv_h[L] : 1;
    const float su = (float)g_tex.w / (float)(w > 0 ? w : 1);
    const float sv = (float)g_tex.h / (float)(h > 0 ? h : 1);
    const uint32_t c = sample_filtered(u * su, v * sv, linear);
    g_tex.addr = a; g_tex.stride = s; g_tex.w = w; g_tex.h = h;
    return c;
}

static uint32_t sample_mip(float u, float v, int lod16) {
    const int minifying = lod16 > 0;
    const int linear = (minifying ? g_tex.min_filter : g_tex.mag_filter) & 1;
    if (g_tex.min_filter < 4 || g_tex.max_level <= 0) return sample_filtered(u, v, linear);

    const int top = g_tex.max_level > 7 ? 7 : g_tex.max_level;
    if (lod16 < 0) lod16 = 0;
    if (lod16 > top * 16) lod16 = top * 16;
    if (!(g_tex.min_filter & 2)) return sample_level(u, v, (lod16 + 8) >> 4 > top ? top : (lod16 + 8) >> 4, linear);

    const int L = lod16 >> 4, f = lod16 & 15;
    const uint32_t c0 = sample_level(u, v, L, linear);
    if (f == 0 || L >= top) return c0;
    const uint32_t c1 = sample_level(u, v, L + 1, linear);
    uint32_t out = 0;
    for (int i = 0; i < 4; i++) {
        const int a = (int)((c0 >> (i * 8)) & 0xFF), b = (int)((c1 >> (i * 8)) & 0xFF);
        out |= (uint32_t)(a + ((b - a) * f) / 16) << (i * 8);
    }
    return out;
}

static uint32_t chan(uint32_t c, int i);
static psp_blend_state g_bs;

/* The texture function: how a texel and the vertex colour become the fragment.
 *
 * The five GE_TEXFUNC codes, the RGB/RGBA flag that says whether the texel's
 * alpha takes part, and the colour-doubling flag. Measured in gpu/texfunc,
 * one test per function: under ADD "One + Zero" is white and "Half + Half"
 * is 0xFE, "Half x2 + Half" saturates to white, and under RGB the fragment
 * alpha is the vertex's. Only MODULATE was implemented before, hardcoded at
 * both call sites, so the other four drew as MODULATE. Codes 5..7 are not
 * defined; they are treated as MODULATE rather than refused, because a
 * refusal draws untextured and that has been the harder thing to notice. */
static uint32_t apply_texfunc(uint32_t tex, uint32_t col) {
    const uint32_t ta = chan(tex, 3), ca = chan(col, 3);
    uint32_t out = 0;
    for (int i = 0; i < 3; i++) {
        const uint32_t t = chan(tex, i), c = chan(col, i), e = chan(g_tex.env, i);
        uint32_t o;
        switch (g_tex.func) {
        case 1:  o = g_tex.tcc_rgba ? (t * ta + c * (255u - ta) + 127u) / 255u : t; break;  /* DECAL   */
        case 2:  o = (c * (255u - t) + e * t + 127u) / 255u;                          break;  /* BLEND   */
        case 3:  o = t;                                                                break;  /* REPLACE */
        case 4:  o = t + c; if (o > 255u) o = 255u;                                    break;  /* ADD     */
        default: o = (t * c + 127u) / 255u;                                            break;  /* MODULATE */
        }
        if (g_tex.color_double) { o *= 2u; if (o > 255u) o = 255u; }
        out |= o << (i * 8);
    }
    uint32_t a;
    switch (g_tex.func) {
    case 1:  a = ca;                                                  break;  /* DECAL keeps the vertex alpha */
    case 3:  a = g_tex.tcc_rgba ? ta : ca;                            break;  /* REPLACE */
    default: a = g_tex.tcc_rgba ? (ta * ca + 127u) / 255u : ca;       break;  /* MODULATE, BLEND, ADD */
    }
    return out | (a << 24);
}

/* The framebuffer's alpha byte is the stencil buffer. An ordinary draw leaves
 * it as it was -- gpu/texfunc fills 44444444, draws, and reads 44ffffff back
 * -- and only a clear that asks for the stencil, or a stencil operation (not
 * modelled), writes it. */
static void put_pixel(int x, int y, uint32_t rgba) {
    if (!g_fb_addr || !g_fb_stride) return;
    if (x < g_sc_x0 || y < g_sc_y0 || x > g_sc_x1 || y > g_sc_y1) return;
    if (g_fb_fmt != 3) {
        psp_write16(g_fb_addr + (uint32_t)(y * (int)g_fb_stride + x) * 2, (uint16_t)pack16(rgba, g_fb_fmt));
        g_pixels++;
        return;
    }
    const uint32_t at = g_fb_addr + (uint32_t)(y * (int)g_fb_stride + x) * 4;
    if (!g_bs.write_alpha) rgba = (rgba & 0x00FFFFFFu) | (psp_read32(at) & 0xFF000000u);
    psp_write32(at, rgba);
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
static float g_depth[DEPTH_STRIDE * DEPTH_ROWS];
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
    for (int i = 0; i < DEPTH_STRIDE * DEPTH_ROWS; i++) g_depth[i] = DEPTH_RESET;
}

/* GE comparison codes: 0 never, 1 always, 2 equal, 3 notequal, 4 less,
 * 5 lequal, 6 greater, 7 gequal. */
static int depth_pass(int x, int y, float z) {
    if (!g_zs.test) return 1;
    const float d = g_depth[y * DEPTH_STRIDE + x];
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
    if (g_fb_fmt != 3)
        return expand16((uint32_t)psp_read16(g_fb_addr + (uint32_t)(y * (int)g_fb_stride + x) * 2), g_fb_fmt);
    return psp_read32(g_fb_addr + (uint32_t)(y * (int)g_fb_stride + x) * 4);
}

/* PSPRECOMP_PIXWATCH=x,y logs every write to one pixel: the colour that
 * arrived, what it became, and the texture bound at the time. "Which draw
 * painted this?" is the question a wrong pixel raises, and the draw log
 * answers it only by elimination. */
static int g_pw_x = -1, g_pw_y = -1, g_pw_left = 64, g_pw_skip = 0;
static int g_cur_prim = -1;   /* what sw_draw is drawing, for the watch */
static void pixwatch_init(void) {
    static int done = 0;
    if (done) return;
    done = 1;
    const char *e = getenv("PSPRECOMP_PIXWATCH");
    if (e && *e && sscanf(e, "%d,%d", &g_pw_x, &g_pw_y) != 2) g_pw_x = g_pw_y = -1;
    /* PSPRECOMP_PIXWATCH_SKIP=<n> skips the first n writes, so the watch can
     * be aimed at the end of a run like the draw log. */
    const char *k = getenv("PSPRECOMP_PIXWATCH_SKIP");
    g_pw_skip = (k && *k) ? atoi(k) : 0;
    const char *m = getenv("PSPRECOMP_PIXWATCH_MAX");
    if (m && *m) g_pw_left = atoi(m);
}

static void shade_pixel(int x, int y, float z, uint32_t rgba) {
    if (x < g_sc_x0 || y < g_sc_y0 || x > g_sc_x1 || y > g_sc_y1) return;
    pixwatch_init();
    int watched = (x == g_pw_x && y == g_pw_y && g_pw_left > 0);
    if (watched && g_pw_skip > 0) { g_pw_skip--; watched = 0; }
    const uint32_t arrived = rgba;
    /* A rejection is as much a write as a write is, for the question "why is
     * this pixel not what it should be": the object that should be here may
     * have arrived and been turned away. */
    if (!alpha_pass(rgba)) {
        g_px_atest++;
        if (watched) fprintf(stderr, "pixwatch: (%d,%d) fb %08X prim %d ALPHA-KILLED %08X  tex %08X %dx%d fmt %d  pixels so far %llu\n",
                             x, y, g_fb_addr, g_cur_prim, rgba, g_tex.addr, g_tex.w, g_tex.h, g_tex.fmt, (unsigned long long)g_pixels);
        return;
    }
    if (!depth_pass(x, y, z)) {
        g_px_zfail++;
        if (watched) fprintf(stderr, "pixwatch: (%d,%d) fb %08X prim %d DEPTH-FAILED %08X  z %.0f against %.0f func %d  tex %08X  pixels so far %llu\n",
                             x, y, g_fb_addr, g_cur_prim, rgba, (double)z, (double)g_depth[y * DEPTH_STRIDE + x], g_zs.func,
                             g_tex.addr, (unsigned long long)g_pixels);
        return;
    }
    if (g_zs.write) g_depth[y * DEPTH_STRIDE + x] = z;
    if (!g_bs.write_colour) return;
    if (g_bs.enable) { rgba = blend(rgba, get_pixel(x, y)); g_px_blend++; }
    if (watched) {
        g_pw_left--;
        fprintf(stderr, "pixwatch: (%d,%d) fb %08X prim %d arrived %08X wrote %08X  z %.0f  blend %d src %d dst %d eq %d fix %06X/%06X  tex %08X %dx%d fmt %d func %d tcc %d  pixels so far %llu\n",
                x, y, g_fb_addr, g_cur_prim, arrived, rgba, (double)z, g_bs.enable, g_bs.src, g_bs.dst, g_bs.eq, g_bs.fixa, g_bs.fixb,
                g_tex.addr, g_tex.w, g_tex.h, g_tex.fmt, g_tex.func, g_tex.tcc_rgba, (unsigned long long)g_pixels);
    }
    put_pixel(x, y, rgba);
}

/* Sample positions per pixel edge. Two is enough to express the pixel centre
 * as an integer -- 2*(x + 0.5) is 2x + 1 -- which is all this needs today.
 *
 * Named rather than written as literal 2s because sub-pixel vertex precision
 * is the same substitution: the hardware rasterizes at sixteenths, so SUBPX 16
 * with positions snapped to 28.4 turns this function into that one without
 * touching its structure. */
#define SUBPX      PSP_SUBPX
#define SUBPX_HALF (PSP_SUBPX / 2)

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
    /* Positions arrive in 1/16 pixel; the pixel box that can contain a centre
     * inside them is floor(min/16) .. floor((max + 15)/16), and the shifts
     * floor for negatives where a division would not. */
    int minx = a->x < b->x ? (a->x < c->x ? a->x : c->x) : (b->x < c->x ? b->x : c->x);
    int maxx = a->x > b->x ? (a->x > c->x ? a->x : c->x) : (b->x > c->x ? b->x : c->x);
    int miny = a->y < b->y ? (a->y < c->y ? a->y : c->y) : (b->y < c->y ? b->y : c->y);
    int maxy = a->y > b->y ? (a->y > c->y ? a->y : c->y) : (b->y > c->y ? b->y : c->y);
    minx >>= 4; miny >>= 4;
    maxx = (maxx + 15) >> 4; maxy = (maxy + 15) >> 4;

    if (minx < g_sc_x0) minx = g_sc_x0;
    if (miny < g_sc_y0) miny = g_sc_y0;
    if (maxx > g_sc_x1) maxx = g_sc_x1;
    if (maxy > g_sc_y1) maxy = g_sc_y1;

    /* 64-bit because through-mode positions are s16 in 1/16 units: a
     * coordinate difference reaches a million and the product 1e12, which
     * overflowed the int this used and inverted coverage for the whole
     * triangle. */
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

    /* The three edge functions at the centre of the first pixel, in the same
     * 1/16 units the positions came in. They sum to area, so they are the
     * barycentric numerators. Stepping them by their own derivatives keeps
     * every value exact and takes the six multiplies out of the inner loop. */
    const int64_t px = (int64_t)SUBPX * minx + SUBPX_HALF;
    const int64_t py = (int64_t)SUBPX * miny + SUBPX_HALF;
    int64_t row0 = d0x * (py - c->y) - d0y * (px - c->x);
    int64_t row1 = d1x * (py - a->y) - d1y * (px - a->x);
    int64_t row2 = d2x * (py - b->y) - d2y * (px - b->x);

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
    int lod16 = 0;
    if (textured) {
        /* The texture gradient across the triangle: solve the affine map from
         * pixels to texels on the two edges from a. */
        const float e1x = (float)(b->x - a->x) / 16.0f, e1y = (float)(b->y - a->y) / 16.0f;
        const float e2x = (float)(c->x - a->x) / 16.0f, e2y = (float)(c->y - a->y) / 16.0f;
        const float det = e1x * e2y - e1y * e2x;
        if (det != 0.0f) {
            const float du1 = b->u - a->u, du2 = c->u - a->u, dv1 = b->v - a->v, dv2 = c->v - a->v;
            const float dudx = (du1 * e2y - du2 * e1y) / det, dudy = (du2 * e1x - du1 * e2x) / det;
            const float dvdx = (dv1 * e2y - dv2 * e1y) / det, dvdy = (dv2 * e1x - dv1 * e2x) / det;
            const float rx = sqrtf(dudx * dudx + dvdx * dvdx), ry = sqrtf(dudy * dudy + dvdy * dvdy);
            lod16 = lod_sixteenths(rx > ry ? rx : ry);
        }
    }

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
                    const uint32_t texel = sample_mip(u, v, lod16);
                    /* The watched pixel's two inputs, separately: which of the
                     * texel and the shaded vertex colour is the dark one is
                     * not a question the final colour can answer. */
                    if (x == g_pw_x && y == g_pw_y)
                        fprintf(stderr, "pixsrc: (%d,%d) vcol %08X texel %08X uv %.1f,%.1f "
                                        "func %d tcc %d tex %08X %dx%d fmt %d lod %d\n",
                                x, y, col, texel, (double)u, (double)v, g_tex.func,
                                g_tex.tcc_rgba, g_tex.addr, g_tex.w, g_tex.h, g_tex.fmt, lod16);
                    col = apply_texfunc(texel, col);
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
    /* Corners in 1/16 pixel. A pixel is covered when its centre, 16*i + 8,
     * lies in [x0, x1): the first such pixel is ceil((x0 - 8) / 16) and the
     * one past the last is ceil((x1 - 8) / 16), both written as (v + 7) >> 4
     * so that negative corners floor rather than round toward zero. This is
     * what puts gpu/filtering's sprite at x = -1/16 onto pixels 0 and 1. */
    const int x0 = a->x < b->x ? a->x : b->x, x1 = a->x > b->x ? a->x : b->x;
    const int y0 = a->y < b->y ? a->y : b->y, y1 = a->y > b->y ? a->y : b->y;
    if (x1 <= x0 || y1 <= y0) return;
    int px0 = (x0 + 7) >> 4, px1 = (x1 + 7) >> 4;
    int py0 = (y0 + 7) >> 4, py1 = (y1 + 7) >> 4;
    if (px0 < g_sc_x0) px0 = g_sc_x0;
    if (py0 < g_sc_y0) py0 = g_sc_y0;
    if (px1 > g_sc_x1 + 1) px1 = g_sc_x1 + 1;
    if (py1 > g_sc_y1 + 1) py1 = g_sc_y1 + 1;

    const int textured = texture_usable();
    /* Against the vertices as submitted, not against the sorted corners. The
     * ramp used to be built from x1 - x0, which is positive by construction, so
     * a sprite whose second corner is left of or above its first mapped its
     * texture backwards. The guard above is what makes these divisions safe:
     * the corners can only coincide if the extents are empty. */
    /* Which screen axis each texture axis runs along. With both corners in
     * order, or both flipped, u follows x and v follows y. With exactly one
     * axis flipped hardware runs u along y and v along x: the two corners the
     * GE generates take u from the vertex that gave them their y and v from the
     * one that gave them their x. Measured in gpu/filtering/precisionnearest2d
     * on a 2x2 texture: TR->BL reads texel (0,1) at the top-left pixel, (0,0)
     * to its right and (1,1) below it; BL->TR reads (1,0), (1,1) and (0,0). The
     * standard mapping answers those with (1,0) and (0,1) at the top-left and
     * was one line wrong on every orientation with one flip. */
    const int transposed = (b->x < a->x) != (b->y < a->y);
    const float du = (b->u - a->u) / (float)(transposed ? (b->y - a->y) : (b->x - a->x));
    const float dv = (b->v - a->v) / (float)(transposed ? (b->x - a->x) : (b->y - a->y));
    /* du and dv are texels per sixteenth of a pixel. */
    int lod16 = 0;
    if (textured) {
        const float rx = fabsf(du) * 16.0f, ry = fabsf(dv) * 16.0f;
        lod16 = lod_sixteenths(rx > ry ? rx : ry);
    }

    for (int y = py0; y < py1; y++) {
        /* Pixel centres, in 1/16 units, against the exact corner: at 1:1 a
         * corner lands exactly on a texel boundary and the rounding decides
         * which side of it to read. */
        const float ty = (float)(y * SUBPX + SUBPX_HALF - a->y);
        const float tv_row = transposed ? 0.0f : a->v + dv * ty;
        const float tu_row = transposed ? a->u + du * ty : 0.0f;
        for (int x = px0; x < px1; x++) {
            if (!textured) { g_px_flat++; shade_pixel(x, y, a->z, b->rgba); continue; }
            const float tx = (float)(x * SUBPX + SUBPX_HALF - a->x);
            const float tu = transposed ? tu_row : a->u + du * tx;
            const float tv = transposed ? a->v + dv * tx : tv_row;
            g_px_tex++;
            shade_pixel(x, y, a->z,
                        apply_texfunc(sample_mip(tu, tv, lod16), b->rgba));
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
    g_cur_prim = prim;
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
    .set_scissor = sw_scissor,
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
static void null_scissor(int a, int b, int c, int d) { (void)a; (void)b; (void)c; (void)d; }
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
    .set_scissor = null_scissor,
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
