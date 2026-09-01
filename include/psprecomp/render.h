/* psprecomp � render backend interface. See docs/RENDERER.md.
 *
 * Display-list *interpretation* has one implementation (src/hle/ge.c);
 * *presentation* is pluggable. The split matters because the software
 * rasterizer is the oracle every other backend is checked against, and it has
 * to keep working on a machine with no GPU.
 *
 * Primitives arrive assembled � vertex format decoding happens once, in the
 * interpreter, rather than being duplicated (and mis-duplicated) per backend.
 */
#ifndef PSPRECOMP_RENDER_H
#define PSPRECOMP_RENDER_H

#include <stdint.h>

/* A vertex after format decoding: screen space, colour resolved.
 * Texture coordinates extend this rather than replacing it. */
typedef struct {
    int      x, y;
    /* Window depth, on the PSP's 0..65535 scale. Transformed geometry gets it
     * from the projection and the viewport's z terms; through-mode geometry
     * carries it in the third position component. Without it primitives can
     * only land in submission order, which draws the back of a model over its
     * front. */
    float    z;
    uint32_t rgba;
    /* Texture coordinates in texels, not normalised. Through-mode geometry
     * gives them that way, and transformed geometry is scaled by the texture
     * size on the way in, so both arrive in the same units. */
    float    u, v;
} psp_vertex;

/* GE primitive types, from the PRIM argument's type field. */
enum {
    PSP_PRIM_POINTS = 0,
    PSP_PRIM_LINES,
    PSP_PRIM_LINE_STRIP,
    PSP_PRIM_TRIANGLES,
    PSP_PRIM_TRIANGLE_STRIP,
    PSP_PRIM_TRIANGLE_FAN,
    PSP_PRIM_SPRITES
};

typedef struct {
    const char *name;

    int  (*init)(int width, int height);
    void (*shutdown)(void);

    /* GE_FBP / GE_FBW: the framebuffer being drawn into. */
    void (*set_target)(uint32_t addr, uint32_t stride, int fmt);

    /* The texture to sample, or addr 0 for none. `fmt` is the GE's own
     * TEXFORMAT code and `func` its TEXFUNC; a backend meeting one it does not
     * implement should draw untextured rather than guess. */
    void (*set_texture)(uint32_t addr, uint32_t stride, int w, int h,
                        int fmt, int func, int swizzled);

    /* The palette for the CLUT formats, and how a texel byte indexes it.
     * `shift`, `mask` and `start` come from CLUTFORMAT and are applied as
     * ((texel >> shift) & mask) | start -- a game can page one palette
     * through a larger CLUT with them, so ignoring them samples the wrong
     * colours rather than none. */
    void (*set_clut)(uint32_t addr, int format, int shift, int mask, int start);

    /* Depth test state. `func` is the GE's own comparison code. */
    void (*set_depth)(int test_enable, int func, int write_enable);

    /* One assembled primitive. `count` vertices, already in screen space. */
    void (*draw)(int prim, const psp_vertex *v, int count);

    /* End of a display list � a natural point to flush batched work. */
    void (*finish)(void);

    /* sceDisplaySetFrameBuf � show what has accumulated. */
    void (*present)(void);
} psp_render_backend;

/* Select a backend by name ("software", "null", ...). Returns 0 on success,
 * -1 if the name is unknown, leaving the current backend in place. */
int psp_render_select(const char *name);

/* The active backend. Never NULL � defaults to software. */
const psp_render_backend *psp_render_current(void);

/* Backends provided by the runtime. */
extern const psp_render_backend psp_render_software;
extern const psp_render_backend psp_render_null;

/* Pixels written by the active backend -- the proof of life during bring-up. */
uint64_t psp_render_pixels(void);
uint64_t psp_render_textured_pixels(void);
uint64_t psp_render_flat_pixels(void);
uint64_t psp_render_zfail_pixels(void);
void     psp_render_reset_pixels(void);

#endif