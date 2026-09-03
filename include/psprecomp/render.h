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

/* Screen positions carry four fractional bits: PSP_SUBPX units per pixel, the
 * precision the hardware rasterizes at. Truncating to whole pixels moved every
 * edge and every texel boundary by up to a pixel -- gpu/filtering's
 * precision tests place a two-pixel sprite at x = -i/16 and hardware covers
 * pixels 0 and 1 with texels 0 and 1; whole pixels covered one with the wrong
 * texel. A pixel's centre is at 16*x + 8 in these units. */
#define PSP_SUBPX 16

/* A vertex after format decoding: screen space, colour resolved.
 * Texture coordinates extend this rather than replacing it. */
typedef struct {
    int      x, y;             /* 12.4 fixed point: PSP_SUBPX units per pixel */
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
    /* Fog coefficient, 0..255: 255 is unfogged and 0 the fog colour, the
     * hardware's own byte. Transformed geometry gets it from its eye-space
     * depth against FOG1/FOG2; through-mode and clear-mode geometry are never
     * fogged and carry 255. Interpolated like a colour channel, applied after
     * the texture function and before blending. */
    int      fog;
} psp_vertex;

/* The bound texture, as the GE describes it.
 *
 * A struct rather than more arguments: set_texture already took seven, and the
 * sampling state the GE tracks -- filter and wrap, at least -- would have made
 * eleven. set_blend already takes its state this way, and everything still owed
 * here (texenv colour, mip levels, the texture matrix) then costs a field
 * instead of a signature change across every backend. */
typedef struct {
    uint32_t addr, stride;
    int      w, h;
    /* The GE's own TEXFORMAT and TEXFUNC codes: a backend meeting one it does
     * not implement should draw untextured rather than guess. */
    int      fmt, func, swizzled;
    /* GE_TEXFILTER's two three-bit fields, raw. Bit 0 selects linear within the
     * level; 4..7 are the mipmap variants, and 2 and 3 are not legal values.
     * Which of the two applies needs a pixel-to-texel scale, so the choice is
     * the backend's to make and not the interpreter's to guess. */
    int      min_filter, mag_filter;
    /* GE_TEXWRAP, per axis: 0 repeat, 1 clamp. Sampling outside [0,size) is
     * ordinary -- a scrolling background does it every frame -- and clamping
     * where the game asked to repeat smears the edge texel across whatever
     * should have wrapped. */
    int      wrap_s, wrap_t;
    /* GE_TEXFUNC's other two fields: bit 8 says the texture's alpha takes part
     * (RGBA) rather than only its colour (RGB), bit 16 doubles the result's
     * colour. GE_TEXENVCOLOR is the constant the BLEND function mixes toward.
     * Measured in gpu/texfunc: "One + Zero" under ADD is white, "Half x2 +
     * Half" saturates, and every line keeps the vertex alpha under RGB. */
    int      tcc_rgba, color_double;
    uint32_t env;

    /* The mip chain. Level 0 repeats addr/stride/w/h; levels 1..7 come from
     * TEX_ADDR/BUF_WIDTH/SIZE 1..7 and are only meaningful up to max_level,
     * TEX_MODE's bits 16..18. lod_mode and lod_bias16 are TEX_LEVEL (0xC8):
     * mode 0 computes the level from the texel-per-pixel ratio, 1 takes the
     * bias alone, 2 the slope register (0xD0) plus the bias; the bias is a
     * signed count of sixteenths. Rules and numbers: gpu/textures/mipmap. */
    uint32_t lv_addr[8], lv_stride[8];
    int      lv_w[8], lv_h[8];
    int      max_level;
    int      lod_mode;
    int      lod_bias16;
    float    lod_slope;
} psp_tex_state;

/* Blend and alpha-test state, as the GE encodes it. Factors and the equation
 * are GEBlendSrcFactor / GEBlendDstFactor / GEBlendMode codes; the alpha-test
 * function shares the GE's comparison codes with the depth test. */
typedef struct {
    int      enable, src, dst, eq;
    uint32_t fixa, fixb;
    int      alpha_test, alpha_func, alpha_ref, alpha_mask;
    /* Clear mode names which buffers it clears. A depth-only clear must not
     * touch colour -- otherwise it paints the clear colour over the frame,
     * which looks like a wrong background rather than like a missing mask. */
    int      write_colour;
    /* The framebuffer's alpha byte is the stencil buffer, and an ordinary draw
     * does not write it -- gpu/texfunc reads 44ffffff back from a 44444444
     * fill after every draw. A clear-mode draw writes it when its stencil bit
     * is set. */
    int      write_alpha;
    /* The stencil test and its operations, GE_STENCILTEST and GE_STENCILOP:
     * the comparison shares the depth test's function codes, the operations
     * are KEEP, ZERO, REPLACE, INVERT, INCR, DECR. The stencil value is the
     * framebuffer's alpha byte, and a passing pixel's zpass operation is what
     * writes it. With the test off the byte is left alone. */
    int      stencil_test, stencil_func, stencil_ref, stencil_mask;
    int      op_sfail, op_zfail, op_zpass;
} psp_blend_state;

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

    /* SCISSOR1 / SCISSOR2: the pixel rectangle draws are confined to, both
     * corners inclusive. On the PSP the scissor is always in force -- the GU
     * library's "disable" sets it to the whole target -- so the rasterizer's
     * bounds are this rectangle and nothing else. Before it was decoded the
     * bounds were a hardcoded 480x272, and gpu/clipping/homogeneous, which
     * draws into a 512-wide target with a 512-wide scissor, lost its last 32
     * columns. */
    void (*set_scissor)(int x0, int y0, int x1, int y1);

    /* The texture to sample, or addr 0 for none. */
    void (*set_texture)(const psp_tex_state *t);

    /* The palette for the CLUT formats, and how a texel byte indexes it.
     * `shift`, `mask` and `start` come from CLUTFORMAT and are applied as
     * ((texel >> shift) & mask) | start -- a game can page one palette
     * through a larger CLUT with them, so ignoring them samples the wrong
     * colours rather than none. */
    void (*set_clut)(uint32_t addr, int format, int shift, int mask, int start);

    /* Depth test state. `func` is the GE's own comparison code. */
    void (*set_depth)(int test_enable, int func, int write_enable);

    /* Alpha blending and the alpha test. Without these a fade overlay -- a
     * quad whose vertices carry a near-zero alpha -- is drawn fully opaque and
     * covers whatever it was meant to be fading. */
    void (*set_blend)(const psp_blend_state *b);
    /* GE_FOGENABLE and GE_FOGCOLOR, raw 0xBBGGRR. The coefficient itself
     * travels in the vertex; this is the colour it blends toward, and whether
     * to. */
    void (*set_fog)(int enable, uint32_t colour);

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
uint64_t psp_render_blended_pixels(void);
uint64_t psp_render_alphakill_pixels(void);
/* Primitives whose minification and magnification filters select *differently*
 * -- not merely differ, since the mipmap variants encode the same in-level
 * choice. The backend uses the magnification one, having no scale factor to
 * choose with; a non-zero count is the evidence that computing one is worth it. */
uint64_t psp_render_filter_split(void);
/* Wall-clock nanoseconds spent inside the rasterizer, cumulative. Answers
 * whether the software rasterizer can carry a real scene or is a placeholder
 * for GPU-backed display-list translation -- see the note at sw_draw. */
uint64_t psp_render_raster_ns(void);
void     psp_render_reset_pixels(void);

/* Return the depth buffer to its start-of-run contents. The game clears depth
 * itself through the GE -- a clear-mode draw with the depth bit set -- so this
 * is only the value in place before its first such draw, not a per-frame clear.
 * psp_ge_reset calls it. */
void     psp_render_reset_depth(void);

#endif