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
/* Decode one mip level into a caller's RGBA8 buffer, through the same sampler
 * the software path draws with.
 *
 * A GPU backend has to hand GL or Vulkan decoded texels, and the decode is the
 * fiddly part -- five formats, two CLUT widths with their shift/mask/start
 * paging, and a byte-level swizzle. Reimplementing it per backend is exactly
 * what the interface exists to prevent (see docs/RENDERER.md): get it wrong
 * once, centrally, and every backend is wrong the same way, which is at least
 * diagnosable. Wrapping is left to the caller, so the texel grid comes back
 * unwrapped and GL's own wrap modes can do that job.
 *
 * Returns the number of texels written, or 0 if the level is empty or the
 * buffer is too small. g_tex is saved and restored: this can be called between
 * draws without disturbing what the rasterizer is in the middle of. */
size_t psp_render_decode_level(const psp_tex_state *t, int level,
                               const psp_clut_state *clut,
                               uint32_t *out, size_t cap, int *out_w, int *out_h) {
    if (!t || !out || level < 0 || level > 7) return 0;
    const psp_tex_state saved = g_tex;
    /* The palette travels with the call rather than being read from wherever
     * set_clut last left it. Only the *software* backend's set_clut writes
     * that state, so a GPU backend decoding through here would have found a
     * stale palette -- silently, and only for the CLUT formats, which is the
     * kind of wrongness that looks like a sampler bug for a day. */
    const struct { uint32_t addr; int fmt, shift, mask, start; } saved_clut = {
        g_clut.addr, g_clut.fmt, g_clut.shift, g_clut.mask, g_clut.start
    };
    if (clut) {
        g_clut.addr = clut->addr; g_clut.fmt = clut->fmt;
        g_clut.shift = clut->shift; g_clut.mask = clut->mask;
        g_clut.start = clut->start;
    }
    g_tex = *t;
    if (level > 0) {
        g_tex.addr   = t->lv_addr[level];
        g_tex.stride = t->lv_stride[level];
        g_tex.w      = t->lv_w[level];
        g_tex.h      = t->lv_h[level];
    }
    /* Clamp both axes: the sampler wraps, and a decode that wrapped would fold
     * the texture onto itself rather than reporting its own grid. */
    g_tex.wrap_s = g_tex.wrap_t = 1;
    const int w = g_tex.w, h = g_tex.h;
    if (w <= 0 || h <= 0 || (size_t)w * (size_t)h > cap) {
        g_tex = saved;
        g_clut.addr = saved_clut.addr; g_clut.fmt = saved_clut.fmt;
        g_clut.shift = saved_clut.shift; g_clut.mask = saved_clut.mask;
        g_clut.start = saved_clut.start;
        return 0;
    }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            out[(size_t)y * (size_t)w + (size_t)x] = sample_texel(x, y);
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    g_tex = saved;
    g_clut.addr = saved_clut.addr; g_clut.fmt = saved_clut.fmt;
    g_clut.shift = saved_clut.shift; g_clut.mask = saved_clut.mask;
    g_clut.start = saved_clut.start;
    return (size_t)w * (size_t)h;
}

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
int psp_render_lod16(const psp_tex_state *t, float rho) {
    if (!t) return 0;
    int lod;
    switch (t->lod_mode) {
    case 0:  lod = (rho > 0.0f) ? (int)floorf(log2f(rho) * 16.0f) : -4096; break;
    case 2:  lod = (int)floorf(t->lod_slope * 16.0f); break;
    default: lod = 0; break;   /* CONST; and the undefined mode 3 measures the same */
    }
    return lod + t->lod_bias16;
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
 * alpha takes part, and the colour-doubling flag. Codes 5..7 are not
 * defined; they are treated as MODULATE rather than refused, because a
 * refusal draws untextured and that has been the harder thing to notice.
 *
 * The arithmetic is geprobe step 13's (fw 6.60), which it reproduces on all
 * 40 sprites; t is the texel, c the vertex colour, e the TEXENV colour, d the
 * doubling bit:
 *
 *     MODULATE  (t * (c + 1)) >> (8 - d)
 *     DECAL     RGBA: (t * ta + c * (255 - ta) + 255) >> (8 - d);  RGB: t << d
 *     BLEND     (c * (255 - t) + e * t + 255) >> (8 - d)
 *     REPLACE   t << d
 *     ADD       (t + c) << d
 *
 * each clamped to 255. Doubling happens before the final shift, not to the
 * rounded result: MODULATE of t 255 and c 64, doubled, is 129, where the
 * rounded (t * c + 127) / 255 this used, then doubled, gave 128 -- (170,54)
 * reads 000C6DCC on hardware against 000C6CCC. REPLACE and ADD, and which
 * alpha each function keeps, were right already (gpu/texfunc: ADD's "One +
 * Zero" is white, "Half x2 + Half" saturates). The alpha product under RGBA
 * is taken like MODULATE's colour; step 13 cannot tell it from
 * ((2t+1)(2c+1)) >> 10 or a truncated t * c / 255, only from the rounded
 * divide, which it excludes. */
static uint32_t apply_texfunc(uint32_t tex, uint32_t col) {
    const uint32_t ta = chan(tex, 3), ca = chan(col, 3);
    const uint32_t d = g_tex.color_double ? 1u : 0u, sh = 8u - d;
    uint32_t out = 0;
    for (int i = 0; i < 3; i++) {
        const uint32_t t = chan(tex, i), c = chan(col, i), e = chan(g_tex.env, i);
        uint32_t o;
        switch (g_tex.func) {
        case 1:  o = g_tex.tcc_rgba ? (t * ta + c * (255u - ta) + 255u) >> sh : t << d; break;  /* DECAL */
        case 2:  o = (c * (255u - t) + e * t + 255u) >> sh;                            break;  /* BLEND   */
        case 3:  o = t << d;                                                            break;  /* REPLACE */
        case 4:  o = (t + c) << d;                                                      break;  /* ADD     */
        default: o = (t * (c + 1u)) >> sh;                                              break;  /* MODULATE */
        }
        out |= (o > 255u ? 255u : o) << (i * 8);
    }
    uint32_t a;
    switch (g_tex.func) {
    case 1:  a = ca;                                             break;  /* DECAL keeps the vertex alpha */
    case 3:  a = g_tex.tcc_rgba ? ta : ca;                       break;  /* REPLACE */
    default: a = g_tex.tcc_rgba ? (ta * (ca + 1u)) >> 8 : ca;    break;  /* MODULATE, BLEND, ADD */
    }
    return out | (a << 24);
}

/* The framebuffer's alpha byte is the stencil buffer. An ordinary draw leaves
 * it as it was -- gpu/texfunc fills 44444444, draws, and reads 44ffffff back
 * -- and only a clear that asks for the stencil, or a stencil operation (not
 * modelled), writes it.
 *
 * The pixel mask (PMSK1/PMSK2) is applied last, on the packed word: a set bit
 * keeps the framebuffer's bit. geprobe step 19 (fw 6.60) draws 0x7FFFFFFF
 * under mask 0xFF00F0F0 over 0x00402010 and reads 0x00FF2F1F back. A 16-bit
 * target takes the mask through the same packing as the colour, so each field
 * keeps the top bits of its mask byte: geprobe 5 scenes 30-32 (fw 6.60) draw
 * white and black under 0x00F8FCF8, 0x00070307, 0x00808080 and 0xFFFF0000 on
 * 5650, 5551 and 4444, and PMSK2 0xF0, 0x0F and 0x3C over stencil writes. */
static void put_pixel(int x, int y, uint32_t rgba, int stencil) {
    if (!g_fb_addr || !g_fb_stride) return;
    if (x < g_sc_x0 || y < g_sc_y0 || x > g_sc_x1 || y > g_sc_y1) return;
    if (g_fb_fmt != 3) {
        const uint32_t at16 = g_fb_addr + (uint32_t)(y * (int)g_fb_stride + x) * 2;
        if (stencil >= 0) rgba = (rgba & 0x00FFFFFFu) | ((uint32_t)stencil << 24);
        else if (!g_bs.write_alpha && g_fb_fmt != 0)
            rgba = (rgba & 0x00FFFFFFu) | (expand16((uint32_t)psp_read16(at16), g_fb_fmt) & 0xFF000000u);
        uint32_t px = pack16(rgba, g_fb_fmt);
        if (g_bs.pixel_mask) {
            const uint32_t keep = pack16(g_bs.pixel_mask, g_fb_fmt);
            px = ((uint32_t)psp_read16(at16) & keep) | (px & ~keep);
        }
        psp_write16(at16, (uint16_t)px);
        g_pixels++;
        return;
    }
    const uint32_t at = g_fb_addr + (uint32_t)(y * (int)g_fb_stride + x) * 4;
    if (stencil >= 0)           rgba = (rgba & 0x00FFFFFFu) | ((uint32_t)stencil << 24);
    else if (!g_bs.write_alpha) rgba = (rgba & 0x00FFFFFFu) | (psp_read32(at) & 0xFF000000u);
    if (g_bs.pixel_mask) rgba = (psp_read32(at) & g_bs.pixel_mask) | (rgba & ~g_bs.pixel_mask);
    psp_write32(at, rgba);
    g_pixels++;
}

/* The depth buffer: 16 bits a pixel in guest VRAM at ZBP, stride ZBW.
 *
 * It used to live host-side as floats, on the reasoning that nothing reads it
 * back and that 16-bit quantisation makes coplanar surfaces fight. Hardware
 * says otherwise on both counts. geprobe 2 scene 17 (fw 6.60) draws eight
 * pairs of coplanar quads at z = -5 under each depth function: the hardware
 * stores 12577 across every one and EQUAL passes everywhere, while the host
 * floats differed in the last bits between the two quads and EQUAL/NOTEQUAL
 * came out speckled. And the scene's depth dump shows what the CPU reads at
 * ZBP, which is now what it reads here too.
 *
 * The value stored is the interpolated window z floored to an integer:
 * across both of the scene's triangles the hardware's values are exactly a
 * floored linear plane. That plane's own gradient is not quite the exact one
 * (values sit within -4..+3 of it); not modelled yet.
 *
 * The game clears depth itself through the GE, a clear-mode draw with the
 * depth bit set; what is there before its first clear is whatever VRAM held,
 * zero on a fresh start. For a GEQUAL title that rejects nothing, as the old
 * host-side DEPTH_RESET of 0 was chosen to. */
static uint32_t g_zb_addr = PSP_VRAM_BASE, g_zb_stride = 512;
static struct { int test, func, write; } g_zs = { 0, 1 /* always */, 0 };

static void sw_depth(int test_enable, int func, int write_enable) {
    g_zs.test = test_enable; g_zs.func = func; g_zs.write = write_enable;
}

void psp_render_set_depth_buffer(uint32_t addr, uint32_t stride) {
    g_zb_addr = PSP_VRAM_BASE | (addr & 0x001FFFFEu);
    g_zb_stride = stride ? stride : 512;
}

/* Called from psp_ge_reset: the depth-buffer registers go back to their
 * start-of-run values. The buffer's contents are guest VRAM and are reset
 * with it. */
void psp_render_reset_depth(void) {
    g_zb_addr = PSP_VRAM_BASE;
    g_zb_stride = 512;
}

/* Where the GE keeps pixel (x, y)'s depth, as the CPU sees it through the
 * plain VRAM address. Not the linear (y * ZBW + x) * 2: geprobe 2 scene 17
 * (fw 6.60, ZBP 0x88000, ZBW 512) dumps ZBP linearly and the image comes back
 * cut into 16-pixel strips. Matching the strips against the scene's geometry
 * gives one address permutation that puts every one of them back:
 *
 *   - bits 0-4 (a strip of 16 pixels) stay;
 *   - bits 5-9 rotate up by one, bit 9 landing in bit 5, so strips from the
 *     left and right halves of a 512-pixel row alternate;
 *   - bits 6 and 13 are inverted, swapping neighbouring strips and blocks of
 *     8 rows.
 *
 * Those two inversions were constant across everything the scene covers (x
 * 16..463, y 64..223); whether they depend on address bits the scene held
 * fixed (bit 19 is set throughout at that ZBP) is a question for a probe
 * with another ZBP. Only the CPU's view depends on it: the GE reads back
 * through the same mapping it wrote. */
static uint32_t depth_addr(int x, int y) {
    const uint32_t l = (g_zb_addr & 0x001FFFFFu) + ((uint32_t)y * g_zb_stride + (uint32_t)x) * 2u;
    const uint32_t mid = (l >> 5) & 0x1Fu;                       /* bits 5-9 */
    const uint32_t rot = ((mid << 1) | (mid >> 4)) & 0x1Fu;
    const uint32_t p = (l & ~(0x1Fu << 5)) | (rot << 5);
    return PSP_VRAM_BASE | ((p ^ 0x2040u) & 0x001FFFFFu);
}

static int depth_value(float z) {
    if (!(z > 0.0f)) return 0;
    if (z >= 65535.0f) return 65535;
    return (int)z;
}

/* GE comparison codes: 0 never, 1 always, 2 equal, 3 notequal, 4 less,
 * 5 lequal, 6 greater, 7 gequal. */
static int depth_pass(int x, int y, int z) {
    if (!g_zs.test) return 1;
    const int d = psp_read16(depth_addr(x, y));
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

static int      g_fog_enable;
static uint32_t g_fog_colour;
static void sw_fog(int enable, uint32_t colour) { g_fog_enable = enable; g_fog_colour = colour; }

/* Fog: the textured colour toward the fog colour by the vertex coefficient.
 *
 * gpu/commands/fog draws a box under every coefficient 0..255 with the
 * vertex at 0x881100 and the fog at 0xFF33FF and reads the pixel back, 768
 * channel values. Exactly one arithmetic reproduces all of them:
 *
 *     out = (c * f + fog * (255 - f) + 255) >> 8
 *
 * A divide by 255 in any rounding fails -- blue wants 254 at f = 1 where
 * exact is 254.53, green wants 51 at f = 6 where exact is 50.2, and no single
 * rounding of the exact value gives both. Divide by 256 with the +255 bias
 * gives both, and every other row. Alpha is untouched: the readback's stencil
 * byte is the 0x44 the test filled. */
static uint32_t apply_fog(uint32_t col, int f) {
    if (!g_fog_enable || f >= 255) return col;
    if (f < 0) f = 0;
    uint32_t out = col & 0xFF000000u;
    for (int i = 0; i < 3; i++) {
        const uint32_t c = chan(col, i), fc = chan(g_fog_colour, i);
        out |= ((c * (uint32_t)f + fc * (255u - (uint32_t)f) + 255u) >> 8) << (i * 8);
    }
    return out;
}

static uint32_t chan(uint32_t c, int i) { return (c >> (i * 8)) & 0xFFu; }

static uint32_t clamp255(int v) { return v < 0 ? 0u : (v > 255 ? 255u : (uint32_t)v); }

/* One blend term: a channel scaled by its factor.
 *
 * geprobe step 11 (fw 6.60) blends three source colours over a gradient under
 * twelve factor and equation pairs, and one rule reproduces all of its
 * 130560 pixels:
 *
 *     term = ((2c + 1) * (2|f| + 1)) >> 10, negated when f < 0
 *
 * with c and f in 0..255 units. The ((c + 1) * f) >> 8 this used before came
 * from gpu/commands/blend's 192 channel values, which this rule also gives
 * ("Zero + Inverse src alpha" 28 from 64 x 111, "Inverse src alpha + Zero" 55
 * from 128 x 111); on step 11's own inputs it is wrong on up to a third of the
 * samples of a row -- FIX 0x80 over FIX 0x80 with s = 64, d = 5 is 34, not 35.
 *
 * The doubled factors are neither clamped nor floored at zero. Double alpha
 * is 2a, up to 510 (0xFF707070 reads 0xE0, as before), and one minus double
 * alpha is 255 - 2a, signed: step 11's row 5, source 0x80C08040 at alpha
 * 0x80, reads B = 191 over a destination B of 171 and up, which is
 * 192 - T(d, -1), where a factor clamped at zero leaves 192. */
static int blend_term(int c, int f) {
    const int m = ((2 * c + 1) * (2 * (f < 0 ? -f : f) + 1)) >> 10;
    return f < 0 ? -m : m;
}

static int blend_scaled(int code, int i, uint32_t src, uint32_t dst, int is_src) {
    const int c = (int)(is_src ? chan(src, i) : chan(dst, i));
    const int other = (int)(is_src ? chan(dst, i) : chan(src, i));
    const int sa = (int)chan(src, 3), da = (int)chan(dst, 3);
    switch (code) {
    case 0:  return blend_term(c, other);
    case 1:  return blend_term(c, 255 - other);
    case 2:  return blend_term(c, sa);
    case 3:  return blend_term(c, 255 - sa);
    case 4:  return blend_term(c, da);
    case 5:  return blend_term(c, 255 - da);
    case 6:  return blend_term(c, 2 * sa);
    case 7:  return blend_term(c, 255 - 2 * sa);
    case 8:  return blend_term(c, 2 * da);
    case 9:  return blend_term(c, 255 - 2 * da);
    default: return blend_term(c, (int)chan(is_src ? g_bs.fixa : g_bs.fixb, i));
    }
}

static uint32_t blend(uint32_t src, uint32_t dst) {
    uint32_t out = 0;
    for (int i = 0; i < 4; i++) {
        const int s = (int)chan(src, i), d = (int)chan(dst, i);
        const int ss = blend_scaled(g_bs.src, i, src, dst, 1);
        const int dd = blend_scaled(g_bs.dst, i, src, dst, 0);
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

/* The stencil test, against the framebuffer's alpha byte. Same function codes
 * as the depth and alpha tests. */
static int stencil_pass(uint32_t cur) {
    const int a = (int)(cur & (uint32_t)g_bs.stencil_mask);
    const int r = g_bs.stencil_ref & g_bs.stencil_mask;
    switch (g_bs.stencil_func) {
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

/* INCR and DECR count in the target's own stencil width: one step of a 4444
 * target's nibble (0x11 once expanded) and the whole bit of a 5551 target.
 * geprobe 5 scenes 31 and 32 (fw 6.60) INCR a stencil of 0x5A written
 * before: 4444 reads 6 where 0x55 + 1 would pack back to 5, and 5551 reads
 * 1 where 0x00 + 1 would pack back to 0. */
static uint32_t stencil_op(int op, uint32_t cur) {
    const uint32_t step = g_fb_fmt == 2 ? 0x11u : (g_fb_fmt == 1 ? 0xFFu : 1u);
    switch (op) {
    case 1:  return 0u;                                  /* ZERO    */
    case 2:  return (uint32_t)g_bs.stencil_ref & 0xFFu;  /* REPLACE */
    case 3:  return ~cur & 0xFFu;                        /* INVERT  */
    case 4:  return cur <= 255u - step ? cur + step : 255u;   /* INCR */
    case 5:  return cur >= step ? cur - step : 0u;            /* DECR */
    default: return cur;                                 /* KEEP    */
    }
}

/* A stencil operation on a pixel whose colour is not written: only the alpha
 * byte changes. A 5650 target has no stencil and the write is dropped.
 * PMSK2 is applied to it as to any other alpha write, a set bit keeping the
 * old stencil bit: geprobe 5 scenes 29, 31 and 32 (fw 6.60) REPLACE, INCR and
 * INVERT under PMSK2 0xF0, 0x0F and 0x3C on 8888, 5551 and 4444. */
static void write_stencil_only(int x, int y, uint32_t value) {
    if (!g_fb_addr || !g_fb_stride || g_fb_fmt == 0) return;
    const uint32_t keep = 0x00FFFFFFu | g_bs.pixel_mask;
    if (g_fb_fmt != 3) {
        const uint32_t at = g_fb_addr + (uint32_t)(y * (int)g_fb_stride + x) * 2;
        const uint32_t old = (uint32_t)psp_read16(at);
        const uint32_t px = pack16((expand16(old, g_fb_fmt) & 0x00FFFFFFu) | (value << 24), g_fb_fmt);
        const uint32_t k16 = pack16(keep, g_fb_fmt);
        psp_write16(at, (uint16_t)((old & k16) | (px & ~k16)));
        return;
    }
    const uint32_t at = g_fb_addr + (uint32_t)(y * (int)g_fb_stride + x) * 4;
    const uint32_t old = psp_read32(at);
    psp_write32(at, (old & keep) | ((value << 24) & ~keep));
}

/* Colour test (CTE 0x27, CTEST 0xD8, CREF 0xD9, CMSK 0xDA): the masked RGB
 * word against the masked reference, with only four functions. geprobe
 * step 19 (fw 6.60) runs NOTEQUAL 0x808080 / 0xF0F0F0 over a ramp and drops
 * exactly the fragments whose three channels all sit in 0x80..0x8F. */
static int colour_pass(uint32_t rgba) {
    if (!g_bs.colour_test) return 1;
    const uint32_t c = rgba & g_bs.colour_mask & 0x00FFFFFFu;
    const uint32_t r = g_bs.colour_ref & g_bs.colour_mask & 0x00FFFFFFu;
    switch (g_bs.colour_func & 3) {
    case 0:  return 0;
    case 2:  return c == r;
    case 3:  return c != r;
    default: return 1;
    }
}

/* Logic op (LOE 0x28, LOP 0xE6), source s against destination d, in the
 * PSPSDK GU_* order. geprobe step 19 (fw 6.60) combines 0xA55A3C with
 * 0x402010 and reads CLEAR 000000, AND 000010, XOR E57A2C, OR E57A3C,
 * NOR 1A85C3, EQUIV 1A85D3, INVERTED BFDFEF (~d), NAND FFFFEF; the other
 * eight follow the same table and are not measured. */
static uint32_t logic_op(int op, uint32_t s, uint32_t d) {
    switch (op & 15) {
    case 0:  return 0;              /* CLEAR */
    case 1:  return s & d;          /* AND */
    case 2:  return s & ~d;         /* AND_REVERSE */
    case 3:  return s;              /* COPY */
    case 4:  return ~s & d;         /* AND_INVERTED */
    case 5:  return d;              /* NOOP */
    case 6:  return s ^ d;          /* XOR */
    case 7:  return s | d;          /* OR */
    case 8:  return ~(s | d);       /* NOR */
    case 9:  return ~(s ^ d);       /* EQUIV */
    case 10: return ~d;             /* INVERTED */
    case 11: return s | ~d;         /* OR_REVERSE */
    case 12: return ~s;             /* COPY_INVERTED */
    case 13: return ~s | d;         /* OR_INVERTED */
    case 14: return ~(s & d);       /* NAND */
    default: return 0xFFFFFFFFu;    /* SET */
    }
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
    if (g_fb_fmt != 3) {
        const uint32_t px = expand16((uint32_t)psp_read16(g_fb_addr + (uint32_t)(y * (int)g_fb_stride + x) * 2), g_fb_fmt);
        /* A 5650 target has no alpha and no stencil, and the blend reads its
         * destination alpha as zero: gpu/commands/blend565's "Double dest
         * alpha" rows are black and its "Inverse double dest alpha" rows are
         * the source colour whole. The texture sampler's 5650 is opaque, which
         * is why this is not in expand16. */
        return g_fb_fmt == 0 ? (px & 0x00FFFFFFu) : px;
    }
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
    if (!colour_pass(rgba)) {
        if (watched) fprintf(stderr, "pixwatch: (%d,%d) fb %08X prim %d COLOUR-TEST-KILLED %08X  ref %06X mask %06X func %d  pixels so far %llu\n",
                             x, y, g_fb_addr, g_cur_prim, rgba, g_bs.colour_ref, g_bs.colour_mask, g_bs.colour_func, (unsigned long long)g_pixels);
        return;
    }
    /* The stencil test runs before the depth test, and each outcome has its
     * operation: fail, pass-but-depth-fails, pass. Only the last writes colour,
     * all three may write the stencil. gpu/commands/blend runs REPLACE with
     * ref 0xAA on ALWAYS and reads 0xAA in every blended pixel's alpha. */
    int stencil = -1;
    uint32_t cur_stencil = 0;
    if (g_bs.stencil_test) {
        cur_stencil = chan(get_pixel(x, y), 3);
        if (!stencil_pass(cur_stencil)) {
            write_stencil_only(x, y, stencil_op(g_bs.op_sfail, cur_stencil));
            return;
        }
    }
    const int zi = depth_value(z);
    if (!depth_pass(x, y, zi)) {
        g_px_zfail++;
        if (watched) fprintf(stderr, "pixwatch: (%d,%d) fb %08X prim %d DEPTH-FAILED %08X  z %d against %d func %d  tex %08X  pixels so far %llu\n",
                             x, y, g_fb_addr, g_cur_prim, rgba, zi, (int)psp_read16(depth_addr(x, y)), g_zs.func,
                             g_tex.addr, (unsigned long long)g_pixels);
        if (g_bs.stencil_test) write_stencil_only(x, y, stencil_op(g_bs.op_zfail, cur_stencil));
        return;
    }
    /* A disabled depth test writes no depth, as in GL and on the hardware:
     * ZMSK alone does not resurrect the write. Clear mode still writes because
     * the GE layer hands it an enabled test with ALWAYS (src/hle/ge.c). */
    if (g_zs.test && g_zs.write) psp_write16(depth_addr(x, y), (uint16_t)zi);
    if (g_bs.stencil_test) stencil = (int)stencil_op(g_bs.op_zpass, cur_stencil);
    if (!g_bs.write_colour) {
        if (stencil >= 0) write_stencil_only(x, y, (uint32_t)stencil);
        else if (g_bs.write_alpha) write_stencil_only(x, y, chan(rgba, 3));
        return;
    }
    if (g_bs.enable) { rgba = blend(rgba, get_pixel(x, y)); g_px_blend++; }
    /* Dither: an offset per screen position, then the 16-bit formats keep
     * the top bits as always (pack16). geprobe step 10 (fw 6.60) dithers
     * 8888 too: its grey ramp, 7F and 80 at (240,136) and (241,136), reads
     * 86 and 78 there under the probe's +7/-8 matrix row. Placed after the
     * blend; the order against blending is not measured. */
    if (g_bs.dither) {
        const int d = g_bs.dither_m[y & 3][x & 3];
        rgba = (rgba & 0xFF000000u) | clamp255((int)chan(rgba, 0) + d)
             | clamp255((int)chan(rgba, 1) + d) << 8 | clamp255((int)chan(rgba, 2) + d) << 16;
    }
    /* The logic op works on RGB only: step 19's alpha byte (the stencil) is
     * untouched by all eight ops it runs. Its order against the dither is not
     * measured. */
    if (g_bs.logic_enable)
        rgba = (rgba & 0xFF000000u) | (logic_op(g_bs.logic_op, rgba, get_pixel(x, y)) & 0x00FFFFFFu);
    if (watched) {
        g_pw_left--;
        fprintf(stderr, "pixwatch: (%d,%d) fb %08X prim %d arrived %08X wrote %08X  z %.0f  blend %d src %d dst %d eq %d fix %06X/%06X  tex %08X %dx%d fmt %d func %d tcc %d  pixels so far %llu\n",
                x, y, g_fb_addr, g_cur_prim, arrived, rgba, (double)z, g_bs.enable, g_bs.src, g_bs.dst, g_bs.eq, g_bs.fixa, g_bs.fixb,
                g_tex.addr, g_tex.w, g_tex.h, g_tex.fmt, g_tex.func, g_tex.tcc_rgba, (unsigned long long)g_pixels);
    }
    put_pixel(x, y, rgba, stencil);
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

/* v / 2^s, floored (s >= 1). */
static int64_t floor_shr(int64_t v, int s) {
    return v >= 0 ? v >> s : -((-v + ((int64_t)1 << s) - 1) >> s);
}

/* 1/area (area > 0) as the GE's triangle setup has it. Returned as q / 2^sh,
 * q < 2^16 except for a power of two, where it is exact. Colour, fog and
 * depth gradients go through it (sw_tri); a line's take it too, with the
 * major length for the area (psp_render_walk_line).
 *
 * It is a table with a linear step, not a division. The area's significand
 * is cut to 17 bits: its leading nine, h, pick one of 256 entries, which
 * hold 2^27/h and the slope 2^24/h^2, each cut to an integer; the last
 * eight, l, take l times the slope over 32, rounded up, off the first; and
 * the result is cut to 16 bits. geprobe 8 (fw 6.60) scenes 50 and 51 read
 * it at every 10-bit length, as a triangle's area and as a line's length
 * drawn either way, which agree on all 512. Where the length's last bit is
 * set the one-bit step leaves it up to 0.39 of a unit above the exact
 * reciprocal or 1.19 below, so the reciprocal cut to 16 bits that this
 * replaces was a unit out on 50 of them; this matches all 512, and all 44
 * of scene 46's areas of 18 to 20 bits, on depth and every colour channel.
 * Scenes 37, 39, 46, 47, 50 and 51 now match on every pixel. Being a hair
 * small is visible on its own: geprobe step 11 spreads 255 over 480 pixels,
 * exactly 544/1024 a pixel, and the hardware steps 543, while -544/1024
 * stays -544. */
static void area_rcp(int64_t area, int64_t *q, int *sh) {
    int L = 0;
    while (L < 62 && (area >> L) != 0) L++;
    *sh = 16 + L - 1;
    const uint64_t m = L > 17 ? (uint64_t)area >> (L - 17) : (uint64_t)area << (17 - L);
    const uint64_t h = m >> 8, l = m & 0xFFu;
    const uint64_t R = ((uint64_t)1 << 27) / h;
    const uint64_t S = ((uint64_t)1 << 24) / (h * h);
    *q = (int64_t)((R - ((l * S + 31) >> 5)) >> 3);
}

/* Floored, clamped to a channel: the plane's value in 1/16384ths. */
static uint32_t plane_chan(int64_t acc) {
    const int64_t v = acc >> 14;
    return v < 0 ? 0u : (v > 255 ? 255u : (uint32_t)v);
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
    /* Flat shading takes the last vertex as submitted, so before the winding
     * normalisation below can swap it. */
    const uint32_t last_rgba = c->rgba;
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

    /* Colour is a plane per channel, and not the barycentric blend that
     * depth and texture coordinates use below: geprobe (fw 6.60) measured
     *
     *     c(px, py) = cA + gx * (px - xA) + gy * (py - yA)
     *
     * at the pixel centre; gx and gy are the numerators times area_rcp's
     * 1/area, floored to 1/1024 a pixel, as for depth below; the result is
     * floored and clamped. It reproduces every triangle of geprobe 1 steps
     * 1-11, 18 and 19, alpha included, and all 78 of geprobe 6 scene 39's,
     * built to measure the gradient's precision (the exact gradient floored
     * matches 69, a 16-bit reciprocal cut toward zero 75).
     *
     * A, the corner the plane starts from, is the depth plane's: the end of
     * the long edge (top to bottom) on that edge's side -- the rightmost
     * corner when the middle one lies left of the long edge, the leftmost
     * when it lies right, ties to the upper, and of two corners level at
     * the bottom the left one (see zk0 below). All three corners compete,
     * on screen or off and inside the scissor or not. geprobe 14 (fw 6.60)
     * scenes 83-98 settle it: one triangle at 48 orientations, 8 shapes in
     * 6 vertex orders, ties swept through by sixteenths, every sub-pixel
     * offset, corners outside the scissor, gradients from 1/4 pixel wide to
     * anchors 2000 pixels away, strips and fans, alpha, fog and the
     * secondary colour, through mode, 3D and perspective. This matches every
     * plane of them; the leftmost corner, which colour took until then, is
     * 283 to 28,025 pixels off a scene, and so is every other corner or
     * tie rule tried, an anchor among the corners inside the scissor (11,325
     * in scene 87, and its depth twin 97 10,584), and every other gradient
     * arithmetic (truncated, rounded, exact, finer, wrapped, clamped, a start
     * bias of 1/16384 or more). Across geprobe 13's dumps it fixes all 1612
     * colour pixels left in scenes 16, 21, 22, 23 and 26, every one in a
     * triangle whose middle corner lies left of its long edge, where the
     * leftmost corner and this one differ, and changes no other.
     *
     * The fog coefficient is a fifth plane through the same corner, and a
     * lit triangle's secondary colour (psp_vertex.spec) three more, 5 to 7,
     * each floored on its own and added to the colour per pixel, then
     * clamped: of scene 95's 26,998 pixels where that differs from one plane
     * through the summed corners, every one reads the sum.
     *
     * Flat shading (SHADE clear) skips the colour planes: the whole triangle
     * is last_rgba. Measured on a triangle list; a strip's triangle takes its
     * own third vertex by the same rule, which is not measured.
     *
     * Kept in 1/16384ths of a channel (1/1024 of a step times the 1/16 grid)
     * so every pixel is exact integer arithmetic. */
    int64_t col_acc[8] = { 0 }, col_dx[8] = { 0 }, col_dy[8] = { 0 };
    int64_t z_acc = 0, z_dx = 0, z_dy = 0;
    const int flat = g_bs.shade_flat;
    const int sec = !flat && (a->spec_set || b->spec_set || c->spec_set);
    const int nplanes = sec ? 8 : 5;
    {
        const psp_vertex *vs[3] = { a, b, c };
        /* Every plane starts from the end of the long edge (top to bottom)
         * on the long edge's side. With the middle corner left of that edge
         * the long edge is the right side and the planes start from the
         * rightmost corner; with it right, from the leftmost. */
        int z_from_right, zk0 = -1;
        {
            int o[3] = { 0, 1, 2 };                           /* by y, then x */
            for (int i = 0; i < 2; i++)
                for (int j = 0; j < 2 - i; j++) {
                    const psp_vertex *p = vs[o[j]], *q = vs[o[j + 1]];
                    if (p->y > q->y || (p->y == q->y && p->x > q->x)) { const int s = o[j]; o[j] = o[j + 1]; o[j + 1] = s; }
                }
            /* Two corners level at the bottom: the left one is the bottom,
             * as at the top the left one is the top, so a flat-topped or
             * flat-bottomed triangle starts from its leftmost corner
             * (geprobe 5 scene 27's flat-bottomed triangle; scenes 46 and 50's
             * flat-topped ones). */
            if (vs[o[1]]->y == vs[o[2]]->y) { const int s = o[1]; o[1] = o[2]; o[2] = s; }
            const psp_vertex *tv = vs[o[0]], *mv = vs[o[1]], *bv = vs[o[2]];
            z_from_right = (int64_t)(bv->x - tv->x) * (mv->y - tv->y) - (int64_t)(bv->y - tv->y) * (mv->x - tv->x) >= 0;
        }
        for (int k = 0; k < 3; k++)
            if (zk0 < 0 || (z_from_right ? vs[k]->x > vs[zk0]->x : vs[k]->x < vs[zk0]->x) ||
                (vs[k]->x == vs[zk0]->x && vs[k]->y < vs[zk0]->y))
                zk0 = k;
        const int k0 = zk0;
        int64_t rq; int rsh;
        area_rcp(area, &rq, &rsh);
        for (int i = flat ? 4 : 0; i < nplanes; i++) {
            int64_t c0, c1, c2, ck;
            if (i == 4) {
                c0 = a->fog; c1 = b->fog; c2 = c->fog; ck = vs[k0]->fog;
            } else if (i > 4) {
                c0 = a->spec_set ? chan(a->spec, i - 5) : 0;
                c1 = b->spec_set ? chan(b->spec, i - 5) : 0;
                c2 = c->spec_set ? chan(c->spec, i - 5) : 0;
                ck = vs[k0]->spec_set ? chan(vs[k0]->spec, i - 5) : 0;
            } else {
                c0 = chan(a->rgba, i); c1 = chan(b->rgba, i); c2 = chan(c->rgba, i);
                ck = chan(vs[k0]->rgba, i);
            }
            const int64_t nx = (c1 - c0) * (c->y - a->y) - (c2 - c0) * (b->y - a->y);
            const int64_t ny = (c2 - c0) * (b->x - a->x) - (c1 - c0) * (c->x - a->x);
            const int64_t gx = floor_shr(nx * rq, rsh - 14), gy = floor_shr(ny * rq, rsh - 14);
            col_acc[i] = ck * 16384
                       + gx * (px - vs[k0]->x) + gy * (py - vs[k0]->y);
            col_dx[i] = gx * SUBPX;
            col_dy[i] = gy * SUBPX;
        }
        /* Depth is a plane too, in the same 1/16384 units, with the same
         * gradient: the numerator times area_rcp's 1/area, floored to 1/1024
         * a pixel. The vertex depths are integers -- through mode's as
         * given, a transformed vertex's floored from ge_screen_z -- but the
         * plane starts from its own corner, zk0 above: the end of the long
         * edge on that edge's side, the rightmost corner when the middle one
         * lies left of the long edge and the leftmost when it lies right.
         * geprobe 10 (fw 6.60) scene 57 shows it. It draws scene 48's 48 3D
         * triangles again in through mode at psprecomp's corners, with the
         * depth the PSP gives each corner as a point: this rule reproduces
         * every pixel of all 48, where the colour's leftmost corner fits 28
         * and no single corner, nor top, bottom, depth or angle, fits more
         * than 31. With those corner depths scene 48's 3D planes match too,
         * 47 of 48 (one corner a sixteenth off), so the 3D path is the same.
         * The right triangles of scenes 27, 46 and 50, which settled the
         * plane, have their middle corner right of a vertical long edge, so
         * there the rule is the leftmost, as before; scenes 17 and 36 go to
         * no pixel off and scene 27 from 1867 to 1290; with geprobe 12's
         * vertex depths every 3D depth plane matches. Corners outside the
         * scissor compete too: geprobe 14 scene 97 draws scene 87's
         * triangles with corner depths, and an anchor among the corners
         * inside the scissor is 10,584 depth pixels off where this is none. */
        {
            int64_t zv[3];
            for (int k = 0; k < 3; k++) {
                const float z = vs[k]->z;
                zv[k] = !(z > 0.0f) ? 0 : (z >= 65535.0f ? 65535 : (int64_t)z);
            }
            const int64_t nx = (zv[1] - zv[0]) * (c->y - a->y) - (zv[2] - zv[0]) * (b->y - a->y);
            const int64_t ny = (zv[2] - zv[0]) * (b->x - a->x) - (zv[1] - zv[0]) * (c->x - a->x);
            const int64_t gx = floor_shr(nx * rq, rsh - 14), gy = floor_shr(ny * rq, rsh - 14);
            z_acc = zv[zk0] * 16384 + gx * (px - vs[zk0]->x) + gy * (py - vs[zk0]->y);
            z_dx = gx * SUBPX;
            z_dy = gy * SUBPX;
        }
    }

    /* The edge functions are already the barycentric numerators, so texture
     * coordinates come out of the same three values the coverage test
     * computes.
     *
     * Texture coordinates
     * are not: transformed vertices retain reciprocal clip W (and a texture
     * projection Q), so the textured branch below performs the homogeneous
     * divide the PSP uses on oblique geometry. Through-mode vertices carry
     * ones and reduce exactly to the old affine result. */
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
            lod16 = psp_render_lod16(&g_tex, rx > ry ? rx : ry);
        }
    }

    for (int y = miny; y <= maxy; y++) {
        int64_t w0 = row0, w1 = row1, w2 = row2;
        int64_t acc[8];
        for (int i = 0; i < nplanes; i++) acc[i] = col_acc[i];
        int64_t zacc = z_acc;
        for (int x = minx; x <= maxx; x++) {
            if (w0 + bias0 >= 0 && w1 + bias1 >= 0 && w2 + bias2 >= 0) {
                const float l0 = (float)w0 * inv;
                const float l1 = (float)w1 * inv;
                const float l2 = (float)w2 * inv;

                const int64_t zi = zacc >> 14;
                const float z = zi < 0 ? 0.0f : (zi > 65535 ? 65535.0f : (float)zi);

                const int fg = (int)plane_chan(acc[4]);

                uint32_t col = last_rgba;
                if (!flat)
                    col = plane_chan(acc[0]) | plane_chan(acc[1]) << 8 |
                          plane_chan(acc[2]) << 16 | plane_chan(acc[3]) << 24;

                if (textured) {
                    const float den = l0 * a->tex_q * a->inv_w
                                    + l1 * b->tex_q * b->inv_w
                                    + l2 * c->tex_q * c->inv_w;
                    const float rden = den != 0.0f ? 1.0f / den : 0.0f;
                    const float u = (l0 * a->u * a->inv_w
                                   + l1 * b->u * b->inv_w
                                   + l2 * c->u * c->inv_w) * rden;
                    const float v = (l0 * a->v * a->inv_w
                                   + l1 * b->v * b->inv_w
                                   + l2 * c->v * c->inv_w) * rden;
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
                /* After the texture function, which is what the mode is for:
                 * geprobe 14 scene 95's secondary-alone row draws through a
                 * black REPLACE texture and reads the secondary colour. */
                if (sec)
                    for (int k = 0; k < 3; k++) {
                        const uint32_t v = chan(col, k) + plane_chan(acc[5 + k]);
                        col = (col & ~(0xFFu << (8 * k))) | (v > 255 ? 255u : v) << (8 * k);
                    }
                col = apply_fog(col, fg);
                shade_pixel(x, y, z, col);
            }
            w0 -= d0y * SUBPX; w1 -= d1y * SUBPX; w2 -= d2y * SUBPX;
            for (int i = 0; i < nplanes; i++) acc[i] += col_dx[i];
            zacc += z_dx;
        }
        row0 += d0x * SUBPX; row1 += d1x * SUBPX; row2 += d2x * SUBPX;
        for (int i = 0; i < nplanes; i++) col_acc[i] += col_dy[i];
        z_acc += z_dy;
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
    /* du and dv are texels per sixteenth of a pixel, truncated toward zero to
     * 2^-16 of a texel. geprobe step 12 (fw 6.60) maps v 0..16 over 56 rows,
     * and where a pixel centre lands exactly on v = 1.0 -- (440,11) -- the PSP
     * reads row 0, the texel below; the exact quotient reads row 1. Any
     * precision from 2^-8 to 2^-20 per pixel fits that sprite. Truncation
     * leaves power-of-two steps exact, which the 1:1 and the 2:1 and 4:1
     * minified sprites of the same scene need. */
    /* Each ramp starts at the top or left edge, whichever vertex gave it
     * that: geprobe 5 scene 28 (fw 6.60) maps 2 texels onto 7 pixels with
     * the corners given bottom-right first, and where a pixel centre lands
     * on the texel boundary the PSP reads the texel on the far side of it
     * from that vertex's, which a ramp truncated from the top-left gives and
     * one from the first vertex does not (it read the near one; 14 pixels).
     * Given top-left first, as everything before it was, nothing changes.
     * uo and vo are the vertices the u and v ramps start from. */
    const psp_vertex *left = a->x <= b->x ? a : b, *top = a->y <= b->y ? a : b;
    const psp_vertex *uo = transposed ? top : left, *vo = transposed ? left : top;
    const psp_vertex *ue = uo == a ? b : a, *ve = vo == a ? b : a;
    const float du = ldexpf(truncf(ldexpf((ue->u - uo->u) / (float)(transposed ? y1 - y0 : x1 - x0), 16)), -16);
    const float dv = ldexpf(truncf(ldexpf((ve->v - vo->v) / (float)(transposed ? x1 - x0 : y1 - y0), 16)), -16);
    int lod16 = 0;
    if (textured) {
        const float rx = fabsf(du) * 16.0f, ry = fabsf(dv) * 16.0f;
        lod16 = psp_render_lod16(&g_tex, rx > ry ? rx : ry);
    }

    for (int y = py0; y < py1; y++) {
        /* Pixel centres, in 1/16 units, against the exact corner: at 1:1 a
         * corner lands exactly on a texel boundary and the rounding decides
         * which side of it to read. */
        const float ty = (float)(y * SUBPX + SUBPX_HALF - y0);
        const float tv_row = transposed ? 0.0f : vo->v + dv * ty;
        const float tu_row = transposed ? uo->u + du * ty : 0.0f;
        for (int x = px0; x < px1; x++) {
            if (!textured) { g_px_flat++; shade_pixel(x, y, a->z, apply_fog(b->rgba, b->fog)); continue; }
            const float tx = (float)(x * SUBPX + SUBPX_HALF - x0);
            const float tu = transposed ? tu_row : uo->u + du * tx;
            const float tv = transposed ? vo->v + dv * tx : tv_row;
            g_px_tex++;
            shade_pixel(x, y, a->z,
                        apply_fog(apply_texfunc(sample_mip(tu, tv, lod16), b->rgba), b->fog));
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

static int64_t floor_div(int64_t n, int64_t d) {
    return n / d - (n % d < 0);
}

/* Intersect the integer sample interval with lo <= floor((p + d*i)/s)
 * <= hi. Clipping the interval first bounds the work even for very long
 * offscreen lines; it must not restart interpolation at the scissor edge. */
static int line_clip_axis(int64_t p, int64_t d, int64_t s, int lo, int hi,
                          int64_t *first, int64_t *last) {
    const int64_t low = (int64_t)lo * s, high = ((int64_t)hi + 1) * s - 1;
    if (!d) return p >= low && p <= high;
    const int64_t begin = d > 0 ? -floor_div(p - low, d)
                                : -floor_div(high - p, -d);
    const int64_t end = d > 0 ? floor_div(high - p, d)
                              : floor_div(p - low, -d);
    if (*first < begin) *first = begin;
    if (*last > end) *last = end;
    return *first <= *last;
}

void psp_render_walk_line(const psp_vertex *a, const psp_vertex *b,
                          int x0, int y0, int x1, int y1,
                          psp_line_pixel_fn emit, void *opaque) {
    const int64_t dx = (int64_t)b->x - a->x, dy = (int64_t)b->y - a->y;
    const int64_t ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    if ((ax > ay ? ax : ay) < PSP_SUBPX || x0 > x1 || y0 > y1) return;
    /* One pixel per major-axis column (or row) whose centre lies on the
     * segment, start included, end excluded; the minor coordinate, colour
     * and depth are taken where that centre projects onto the line. For
     * whole-pixel endpoints that is step i's centre, i + 1/2: geprobe step 1
     * (fw 6.60) draws (140,230)-(230,265) through (150,234), where i alone
     * gives 233. geprobe 5 scene 27's 3D line, from x 4141/16 to 6101/16,
     * shows the general case: the hardware starts at the first centre past
     * 258.81, pixel 259, and each of its 122 columns sits on the row and at
     * the depth of its centre's projection, where stepping 122 whole steps
     * from 258.81 put six a row off and every depth up to 18 off.
     *
     * The two ends are then adjusted by the pixels' diamonds, below.
     *
     * Major index k = 0..n-1: pixel M0 + sm*k along the major axis, its
     * centre c0 + 16*sm*k; the minor pixel is
     * floor((m_a*|dM| + dm*sm*(centre - M_a)) / (16*|dM|)). Both are
     * floor((p + d*k) / s), so one clip serves either. */
    /* A line as long across as down is y-major: geprobe 15 (fw 6.60) scene
     * 100's 256 lines at exactly 45 degrees, from 64 start offsets in each
     * direction, take their colour from y (2496 pixels a step off as
     * x-major), and so do scene 22's patch diagonals (45 pixels). */
    const int xmajor = ax > ay;
    const int64_t Ma = xmajor ? a->x : a->y, ma = xmajor ? a->y : a->x;
    const int64_t dM = xmajor ? dx : dy, dm = xmajor ? dy : dx, adM = dM < 0 ? -dM : dM;
    const int64_t sm = dM < 0 ? -1 : 1;
    /* First and one-past-last major pixels: centres in [Ma, Mb) going up,
     * (Mb, Ma] going down. */
    const int64_t Mb = Ma + dM;
    const int64_t M0 = sm > 0 ? floor_div(Ma - 8 + 15, 16) : floor_div(Ma - 8, 16);
    const int64_t Mend = sm > 0 ? floor_div(Mb - 8 + 15, 16) : floor_div(Mb - 8, 16);
    const int64_t n = sm > 0 ? Mend - M0 : M0 - Mend;
    const int64_t c0 = 16 * M0 + 8;                         /* centre of k = 0 */
    const int64_t pm = ma * adM + dm * sm * (c0 - Ma), dmk = 16 * dm, smd = 16 * adM;
    /* The ends go by the pixel's diamond, |x - cx| + |y - cy| < 1/2: the
     * last pixel is left out when the end lies in its diamond, and the pixel
     * before the first is drawn when the start lies in its diamond. On the
     * diamond's edge a point on the minor axis's negative side counts as
     * inside, one on its positive side as outside: above the centre for a
     * shallow line, left of it for a steep one. geprobe 15 (fw 6.60) scenes
     * 99-101 put 27 starts and ends of steep and 45-degree lines on diamond
     * edges, and the PSP draws every one this way; taking "above" for
     * steep lines too, as before, was a pixel off at each.
     * geprobe 6 scene 37 (fw 6.60) ends shallow lines on every
     * sixteenth of a row: going right to x + 5/8, the PSP leaves out the
     * last pixel for end rows 2/16 to 13/16 past a whole pixel (its diamond
     * holds the end, the edge included at 2/16 and not at 14/16); going left
     * from x + 7/16 it draws one more pixel at the start for 1/16 to 14/16.
     * Its other lines, whose ends lie on no diamond, match the centre rule
     * above; so does scene 28's 3D line, which ends in its last pixel's
     * diamond. */
    int64_t first = 0, last = n - 1;
    {
        const int64_t mb = ma + dm;
        int64_t k = n - 1;
        for (int end = 0; end < 2; end++, k = -1) {
            const int64_t Mp = M0 + sm * k, mp = floor_div(pm + dmk * k, smd);
            const int64_t pM = end ? Ma : Mb, pmin = end ? ma : mb;
            const int64_t dMaj = pM - (16 * Mp + 8), dMin = pmin - (16 * mp + 8);
            const int64_t sum = (dMaj < 0 ? -dMaj : dMaj) + (dMin < 0 ? -dMin : dMin);
            const int inside = sum < 8 || (sum == 8 && dMin < 0);
            if (inside) { if (end) first = -1; else last = n - 2; }
        }
    }
    if (last < first) return;
    if (!line_clip_axis(xmajor ? M0 : pm, xmajor ? sm : dmk, xmajor ? 1 : smd, x0, x1, &first, &last) ||
        !line_clip_axis(xmajor ? pm : M0, xmajor ? dmk : sm, xmajor ? smd : 1, y0, y1, &first, &last))
        return;
    /* Colour and depth at the projected centre, on the triangle's gradient
     * rule: the difference times area_rcp's reciprocal of the major length,
     * floored to 1/16384 a sixteenth, times the distance from the start,
     * the value floored. Step 1's white-to-blue line reads FDFDFF at its
     * first pixel: 255 - 2902/1024 * 1/2 = 253.6. geprobe 7 (fw 6.60) scene
     * 49's 128 steep red-to-green lines match on all 5862 of their pixels
     * this way; the gradient floored from the exact quotient, as before,
     * left four a step off. geprobe 8 scene 51's 1024 lines, one per 10-bit
     * length each way, and geprobe 6 scene 37's match on every pixel too.
     * Depth goes from the integer vertex depths: scene 27's three
     * through-mode lines match at every step, and its 3D line on every one
     * of its pixels; taken at the step's start, as it was, each read half a
     * step short, 149 or 91 off. The distance is kept in sixteenths
     * (dist16), so the sums are in 1/16384ths. */
    int64_t cg[4], cv[4], lq;
    int lsh;
    area_rcp(adM, &lq, &lsh);
    for (int c = 0; c < 4; c++) {
        cv[c] = chan(a->rgba, c);
        cg[c] = floor_shr(((int64_t)chan(b->rgba, c) - cv[c]) * 16384 * lq, lsh);
    }
    /* Fog and the secondary colour are planes by the same rule: geprobe 15
     * (fw 6.60) scene 104's fogged lines read so on every pixel (a rounded
     * blend of the end values, as before, left 1149 a step off), and scene
     * 105's lit lines read the secondary colour interpolated and added, each
     * channel floored on its own (the first end's secondary throughout left
     * 1136 off; none at all 1138). */
    const int64_t fv = a->fog, fgr = floor_shr(((int64_t)b->fog - a->fog) * 16384 * lq, lsh);
    const int sec = a->spec_set || b->spec_set;
    int64_t sv[3] = { 0, 0, 0 }, sg[3] = { 0, 0, 0 };
    if (sec)
        for (int c = 0; c < 3; c++) {
            sv[c] = a->spec_set ? chan(a->spec, c) : 0;
            const int64_t sb = b->spec_set ? chan(b->spec, c) : 0;
            sg[c] = floor_shr((sb - sv[c]) * 16384 * lq, lsh);
        }
    const int64_t za = !(a->z > 0.0f) ? 0 : (a->z >= 65535.0f ? 65535 : (int64_t)a->z);
    const int64_t zb = !(b->z > 0.0f) ? 0 : (b->z >= 65535.0f ? 65535 : (int64_t)b->z);
    const int64_t zg = floor_shr((zb - za) * 16384 * lq, lsh);
    /* Texture coordinates at the same point. Unprojected (through
     * mode), the texture coordinates go by a fixed step, texels a sixteenth,
     * truncated toward zero to 2^-24 (2^-20 a pixel). geprobe 5 scene 28's
     * three through-mode textured lines (fw 6.60) read the hardware's texel
     * on every pixel this way. Taken at the step's start they read the texel
     * before it wherever a boundary fell inside the step; with an exact step
     * at the centre they read the one after it at the 9 pixels whose centre
     * lands exactly on a boundary; a sprite's coarser 2^-16 step truncates
     * too far and reads the one before (31 pixels). */
    const int affine = a->inv_w == 1.0f && b->inv_w == 1.0f && a->tex_q == 1.0f && b->tex_q == 1.0f;
    const float lu = (float)ldexp(trunc(ldexp((double)(b->u - a->u) / (double)adM, 24)), -24);
    const float lv = (float)ldexp(trunc(ldexp((double)(b->v - a->v) / (double)adM, 24)), -24);
    for (int64_t k = first; k <= last; k++) {
        const int64_t dist16 = sm * (c0 - Ma) + 16 * k;    /* < 0 at k = -1 only */
        const float t = (float)((double)dist16 / (double)adM), s = 1.0f - t;
        const int64_t Mp = M0 + sm * k, mp = floor_div(pm + dmk * k, smd);
        psp_vertex v = *a;
        v.x = (int)(xmajor ? Mp : mp) * PSP_SUBPX + PSP_SUBPX / 2;
        v.y = (int)(xmajor ? mp : Mp) * PSP_SUBPX + PSP_SUBPX / 2;
        v.z = (float)floor_div(za * 16384 + zg * dist16, 16384);
        v.rgba = 0;
        for (int c = 0; c < 4; c++)
            v.rgba |= plane_chan(cv[c] * 16384 + cg[c] * dist16) << (8 * c);
        v.fog = (int)plane_chan(fv * 16384 + fgr * dist16);
        v.spec = 0;
        v.spec_set = sec;
        if (sec)
            for (int c = 0; c < 3; c++) v.spec |= plane_chan(sv[c] * 16384 + sg[c] * dist16) << (8 * c);
        if (affine) {
            v.u = a->u + lu * (float)dist16;
            v.v = a->v + lv * (float)dist16;
        } else {
            const float den = s * a->tex_q * a->inv_w + t * b->tex_q * b->inv_w;
            v.u = den ? (s * a->u * a->inv_w + t * b->u * b->inv_w) / den : 0;
            v.v = den ? (s * a->v * a->inv_w + t * b->v * b->inv_w) / den : 0;
        }
        v.inv_w = v.tex_q = 1.0f;
        emit(&v, opaque);
    }
}

int psp_render_line_lod16(const psp_tex_state *t,
                          const psp_vertex *a, const psp_vertex *b) {
    const float dx = fabsf((float)b->x - (float)a->x) / PSP_SUBPX;
    const float dy = fabsf((float)b->y - (float)a->y) / PSP_SUBPX;
    const float extent = dx > dy ? dx : dy;
    return psp_render_lod16(t, extent ? hypotf(b->u - a->u, b->v - a->v) / extent : 1);
}

static void sw_point_sample(const psp_vertex *v, void *opaque) {
    const int x = (int)floor_div(v->x, PSP_SUBPX), y = (int)floor_div(v->y, PSP_SUBPX);
    if (x < g_sc_x0 || x > g_sc_x1 || y < g_sc_y0 || y > g_sc_y1) return;
    uint32_t col = v->rgba;
    if (texture_usable()) {
        const float q = v->tex_q;
        col = apply_texfunc(sample_mip(q ? v->u / q : 0, q ? v->v / q : 0,
                                       *(const int *)opaque), col);
        g_px_tex++;
    } else g_px_flat++;
    /* A lit point's secondary colour adds after the texture function, as a
     * triangle's does: geprobe 14 (fw 6.60) scene 95's corner points read
     * primary plus secondary, and through a black REPLACE texture the
     * secondary alone. */
    if (v->spec_set)
        for (int k = 0; k < 3; k++) {
            const uint32_t s = chan(col, k) + chan(v->spec, k);
            col = (col & ~(0xFFu << (8 * k))) | (s > 255 ? 255u : s) << (8 * k);
        }
    shade_pixel(x, y, v->z, apply_fog(col, v->fog));
}

static void sw_draw(int prim, const psp_vertex *v, int count) {
    g_cur_prim = prim;
    const uint64_t t0 = now_ns();
    switch (prim) {
    case PSP_PRIM_POINTS: {
        int lod16 = psp_render_lod16(&g_tex, 1.0f);
        for (int i = 0; i < count; i++) sw_point_sample(&v[i], &lod16);
        break;
    }
    case PSP_PRIM_LINES:
    case PSP_PRIM_LINE_STRIP:
        for (int i = 0; i + 1 < count; i += prim == PSP_PRIM_LINES ? 2 : 1) {
            int lod16 = psp_render_line_lod16(&g_tex, &v[i], &v[i + 1]);
            /* A flat-shaded line is its second vertex's colour throughout,
             * as a flat triangle is its last's: geprobe 15 (fw 6.60) scene
             * 102's lines and strips, in through mode and 3D, and scene 75's
             * flat patch lines (1494 pixels, half of them off when the
             * shading was ignored). Fog and the secondary colour are not
             * measured flat and keep their planes. */
            psp_vertex fa = v[i];
            if (g_bs.shade_flat) fa.rgba = v[i + 1].rgba;
            psp_render_walk_line(&fa, &v[i + 1], g_sc_x0, g_sc_y0, g_sc_x1, g_sc_y1,
                                  sw_point_sample, &lod16);
        }
        break;
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
        break;                       /* fans are assembled by the GE */
    }
    g_raster_ns += now_ns() - t0;
}

static void sw_noop(void) { }

/* init() is called by the host once the backend is chosen (boot.c, from
 * PSPRECOMP_RENDER); shutdown() and present() are still unwired -- nothing in
 * the runtime or the host calls either. They stay because a windowed backend
 * will need them, but check that before hanging behaviour off one. Note that
 * boot.c's teardown is skipped whenever a guest thread is still live, which is
 * the common case, so shutdown() would not run reliably even if it were wired.
 * This slot used to hold
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
    .set_fog     = sw_fog,
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
static void null_fog(int enable, uint32_t colour) { (void)enable; (void)colour; }
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
    .set_fog     = null_fog,
    .draw        = null_draw,
    .finish      = null_noop,
    .present     = null_noop,
};

/* ---- selection ----------------------------------------------------------- */

/* The table is seeded with the backends the runtime carries and grows by
 * registration, because anything needing a window or a GL context cannot live
 * here: the core has no external dependencies on purpose and SDL2 is the
 * host's. So the host implements such a backend and hands it over, and every
 * caller keeps selecting by name without knowing which side it came from. */
enum { PSP_RENDER_MAX = 8 };
static const psp_render_backend *g_all[PSP_RENDER_MAX] = {
    &psp_render_software, &psp_render_null
};
static size_t g_n_all = 2;

static const psp_render_backend *g_backend = &psp_render_software;

const psp_render_backend *psp_render_current(void) { return g_backend; }

int psp_render_register(const psp_render_backend *b) {
    if (!b || !b->name || !*b->name) return -1;
    if (g_n_all >= PSP_RENDER_MAX) return -1;
    /* Every entry point must be present. A half-filled backend would pass
     * registration and crash at whichever call it forgot, arbitrarily far
     * from here -- and the interface is twelve calls precisely so that a
     * backend cannot quietly not implement one of them. */
    if (!b->init || !b->shutdown || !b->set_target || !b->set_scissor ||
        !b->set_texture || !b->set_clut || !b->set_depth || !b->set_blend ||
        !b->set_fog || !b->draw || !b->finish || !b->present) return -1;
    for (size_t i = 0; i < g_n_all; i++)
        if (strcmp(g_all[i]->name, b->name) == 0) return -1;   /* name taken */
    g_all[g_n_all++] = b;
    return 0;
}

int psp_render_select(const char *name) {
    if (!name) return -1;
    for (size_t i = 0; i < g_n_all; i++) {
        if (strcmp(g_all[i]->name, name) == 0) { g_backend = g_all[i]; return 0; }
    }
    return -1;                       /* unknown: keep the current backend */
}

const char *psp_render_backend_name(size_t i) {
    return i < g_n_all ? g_all[i]->name : NULL;
}
