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

#include <string.h>

/* ---- shared target state ------------------------------------------------- */

static uint32_t g_fb_addr, g_fb_stride;
static uint64_t g_pixels;

uint64_t psp_render_pixels(void) { return g_pixels; }
void     psp_render_reset_pixels(void) { g_pixels = 0; }

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

static void sw_texture(uint32_t addr, uint32_t stride, int w, int h,
                       int fmt, int func, int swizzled) {
    g_tex.addr = addr; g_tex.stride = stride;
    g_tex.w = w; g_tex.h = h;
    g_tex.fmt = fmt; g_tex.func = func; g_tex.swizzled = swizzled;
}

#define GE_TFMT_5650 0

static int texture_usable(void) {
    return g_tex.addr && g_tex.w > 0 && g_tex.h > 0 &&
           g_tex.fmt == GE_TFMT_5650 && !g_tex.swizzled;
}

/* One texel, expanded to eight bits a channel. 5650 packs red in the low bits;
 * green is six wide, which is why it shifts by two where the others shift by
 * three. Replicating the high bits down keeps white at 0xFF, not 0xF8. */
static uint32_t sample_5650(int u, int v) {
    if (u < 0) u = 0; else if (u >= g_tex.w) u = g_tex.w - 1;
    if (v < 0) v = 0; else if (v >= g_tex.h) v = g_tex.h - 1;

    const uint32_t at = g_tex.addr + (uint32_t)(v * (int)g_tex.stride + u) * 2u;
    const uint16_t p  = (uint16_t)psp_read16(at);

    uint32_t r = (uint32_t)( p        & 0x1F); r = (r << 3) | (r >> 2);
    uint32_t g = (uint32_t)((p >>  5) & 0x3F); g = (g << 2) | (g >> 4);
    uint32_t b = (uint32_t)((p >> 11) & 0x1F); b = (b << 3) | (b >> 2);
    return 0xFF000000u | (b << 16) | (g << 8) | r;
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

    for (int y = miny; y <= maxy; y++) {
        for (int x = minx; x <= maxx; x++) {
            int w0 = (b->x - a->x) * (y - a->y) - (b->y - a->y) * (x - a->x);
            int w1 = (c->x - b->x) * (y - b->y) - (c->y - b->y) * (x - b->x);
            int w2 = (a->x - c->x) * (y - c->y) - (a->y - c->y) * (x - c->x);
            if ((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0))
                put_pixel(x, y, a->rgba);
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
            if (!textured) { put_pixel(x, y, b->rgba); continue; }
            const float tu = a->u + du * (float)(x - x0);
            put_pixel(x, y, modulate(sample_5650((int)tu, (int)tv), b->rgba));
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

const psp_render_backend psp_render_software = {
    "software", sw_init, sw_shutdown, sw_target, sw_texture, sw_draw, sw_noop, sw_noop
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
static void null_draw(int p, const psp_vertex *v, int n) { (void)p; (void)v; (void)n; }
static void null_noop(void) { }

const psp_render_backend psp_render_null = {
    "null", null_init, null_noop, null_target, null_texture, null_draw, null_noop, null_noop
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
