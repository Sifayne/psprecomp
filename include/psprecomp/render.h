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
#include <stddef.h>          /* size_t, for the backend enumeration */

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
    /* Homogeneous terms for texture interpolation.  Ordinary transformed
     * geometry carries reciprocal clip-space W in inv_w and tex_q == 1, so a
     * rasterizer reconstructs
     *
     *     uv = sum(bary * uv * inv_w) / sum(bary * inv_w)
     *
     * instead of stretching an affine map across oblique world geometry.
     * Texture-matrix projection replaces the denominator with
     * sum(bary * tex_q * inv_w).  Through-mode geometry and sprites use 1 for
     * both and therefore retain their screen-space affine mapping. */
    float    inv_w, tex_q;
    /* Fog coefficient, 0..255: 255 is unfogged and 0 the fog colour, the
     * hardware's own byte. Transformed geometry gets it from its eye-space
     * depth against FOG1/FOG2; through-mode and clear-mode geometry are never
     * fogged and carry 255. Interpolated like a colour channel, applied after
     * the texture function and before blending. */
    int      fog;
    /* True for presentation-space geometry: either the GE's THROUGH bit
     * supplied an already projected vertex, or an affine/orthographic matrix
     * transformed it. Most backends do not need to care because the
     * coordinates above are screen space either way. An aspect-aware backend
     * does: the perspective camera may already be corrected while HUD and 2D
     * geometry still need a separate safe-area transform. */
    int      screen_space;
    /* Optional pre-quantization screen position for enhanced GPU rendering.
     * The legacy x/y fields remain the software and 1x coverage contract.
     * Zero-initialized and through-mode vertices use x/y unless precise is set. */
    float    precise_x, precise_y;
    int      precise;
    /* The secondary colour, 0x00BBGGRR, valid when spec_set: in lighting's
     * separate-specular mode (LIGHTMODE 1) the specular terms land here and
     * the rest in rgba, each clamped to 255 per vertex. The rasterizer
     * interpolates it as three more planes and adds it to the pixel's colour,
     * clamping the sum: geprobe 7 (fw 6.60) scene 16's two specular fans,
     * whose centre vertex sums past 255, match on every pixel in 46 of their
     * 72 triangle channels that way and are a few pixels off in the rest
     * (slips the single-colour fans below them show too). As one plane through
     * the unclamped sum, which is what this replaced, 9 fit whatever the
     * corners (each searched within 4), and those 9 are 255 throughout. In
     * single-colour mode the specular joins rgba before the clamp and
     * spec_set is clear. */
    uint32_t spec;
    int      spec_set;
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
    /* SHADE (0x50) clear: a triangle is one colour, its last vertex's, the
     * third as draw() receives it (geprobe step 1, fw 6.60: the flat red
     * triangle reads 00FF0000 throughout). Pushed with the rest of the draw
     * state because the GE reads it per primitive, as it does these. */
    int      shade_flat;
    /* Dither, DTE (0x20) and DITH1..4 (0xE2..0xE5): with it on, each of R,
     * G and B becomes clamp(c + m[y & 3][x & 3]) before the write, in every
     * framebuffer format, 8888 included, and alpha is not dithered (geprobe
     * steps 5-10, fw 6.60). Row n is DITH(n+1), and element j is the signed
     * nibble at bits 4j..4j+3 of it. */
    int      dither;
    int8_t   dither_m[4][4];
    /* The colour test, CTE (0x27) with CTEST/CREF/CMSK (0xD8-0xDA): a
     * fragment passes when (rgb & mask) OP (ref & mask), OP 0 never, 1
     * always, 2 equal, 3 not equal, compared as one 24-bit word. */
    int      colour_test, colour_func;
    uint32_t colour_ref, colour_mask;
    /* The logic op, LOE (0x28) and LOP (0xE6): the sixteen PSPSDK GU_CLEAR
     * .. GU_SET codes, applied to RGB against the framebuffer after blending;
     * the alpha/stencil byte is left alone. */
    int      logic_enable, logic_op;
    /* PMSK1 | PMSK2 << 24 (0xE8, 0xE9), in 0xAABBGGRR: a set bit keeps the
     * framebuffer's bit (geprobe step 19, fw 6.60: 0xFF00F0F0 with 0x7FFFFFFF
     * over 0x00402010 reads 0x00FF2F1F). */
    uint32_t pixel_mask;
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

/* A vertex as the display list stores it, decoded but not yet transformed:
 * what draw_model receives. The colour is the vertex's own or, when the
 * format carries none, the material colour the CPU path would have used; u and
 * v are already scaled and offset into texels, as psp_vertex carries them. The
 * normal is only meaningful when the state lights. */
typedef struct {
    float    pos[3];
    float    nrm[3];
    uint32_t rgba;
    float    u, v;
} psp_model_vertex;

/* Everything ge.c's transform pipeline applies between a psp_model_vertex and
 * the psp_vertex it hands to draw(), captured at the draw. Matrices are the
 * GE's 4x3 (three columns of three, then the translation) and 4x4; lights are
 * already in eye space, as light_vertex sees them. A backend implementing
 * draw_model reproduces draw_prim_transformed and emit_tri from this alone. */
typedef struct {
    float world[12], view[12], proj[16], tgen[12];
    int   lighting, mat_update, mat_alpha;
    struct {
        int   enable, type, kind;
        float pos[3], dir[3];           /* eye space */
        float atten[3], exponent, cutoff;
        float amb[3], dif[3], spec[3];
    } light[4];
    float mat_emissive[3], mat_ambient[3], mat_diffuse[3], mat_specular[3];
    float mat_spec_coef, global_amb[3];
    int   fog_enable;
    float fog_end, fog_range;
    int   vp_set;
    float vp_xs, vp_ys, vp_zs, vp_xc, vp_yc, vp_zc, off_x, off_y;
    int   depth_clamp, cull_enable, cull_ccw;
    int   tex_map_mode, tex_proj_mode;
    int   tex_w, tex_h;
} psp_xform_state;

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

    /* Optional: the transform on the backend. When both are set and model_ok()
     * answers 1, ge.c hands transformed-3D triangle draws over as model-space
     * vertices plus the transform-and-lighting state instead of running its
     * own pipeline; the backend then owes the result the CPU path would have
     * produced (see psp_xform_state). Either NULL, or model_ok() == 0, keeps
     * the CPU path. */
    int  (*model_ok)(void);
    void (*draw_model)(int prim, const psp_model_vertex *v, int count,
                       const psp_xform_state *xs);

    /* Optional placement metadata, supplied before every draw (including
     * through/clear and immediate draws). The projected viewport rectangle
     * is in guest pixels after OFFSET_X/Y, with absolute width/height. It
     * does not transform vertices again. Hosts can use it to distinguish
     * inset 3D views from the main scene on both CPU and GPU transform paths. */
    void (*set_viewport)(float x, float y, float width, float height);

    /* Optional: sceDisplaySetFrameBuf's buffer, its stride in pixels and its
     * pixel format, given just before present(). A backend that composes its
     * own targets shows this buffer -- not the last one the GE drew into --
     * so a buffer the CPU or a DMA filled, such as a decoded movie picture,
     * is what the window shows. */
    void (*set_display)(uint32_t addr, uint32_t stride, int fmt);

    /* Optional, NULL for a backend that draws in guest memory: every target
     * it holds written back there, synchronously, before a save state
     * copies VRAM. On the GE's thread, at the safe point. */
    void (*to_memory)(void);
} psp_render_backend;

/* Select a backend by name ("software", "null", ...). Returns 0 on success,
 * -1 if the name is unknown, leaving the current backend in place. A caller
 * that ignores the -1 gets the software backend while believing it asked for
 * something else, so check it. */
int psp_render_select(const char *name);

/* Decode one mip level of a texture into `out` as RGBA8, through the same
 * sampler the software backend draws with -- five formats, both CLUT widths
 * with their shift/mask/start paging, and the byte swizzle. A GPU backend must
 * hand its API decoded texels, and doing that decode per backend is what this
 * interface exists to prevent. Wrapping is the caller's: the grid comes back
 * unwrapped so GL's or Vulkan's own wrap modes can apply it.
 *
 * Returns texels written (w*h), or 0 if the level is empty or `cap` is too
 * small. Safe to call between draws; the sampler's state is restored. */
/* The palette a CLUT-format texture decodes through. Passed explicitly rather
 * than read from wherever set_clut last left it: only the software backend's
 * set_clut writes that, so any other backend would decode through a stale
 * palette -- silently, and only for the CLUT formats. */
typedef struct {
    uint32_t addr;
    int      fmt, shift, mask, start;
} psp_clut_state;

size_t psp_render_decode_level(const psp_tex_state *t, int level,
                               const psp_clut_state *clut,
                               uint32_t *out, size_t cap,
                               int *out_w, int *out_h);
/* Host upload variant: preserve the declared dimensions, filling texels whose
 * source or palette entry lies outside its backing region with transparent
 * padding. Reports those texels separately; does not pretend they were guest
 * reads or emulate hardware mirroring. The strict decoder and software
 * sampler continue to report invalid sampled memory normally. */
size_t psp_render_decode_level_padded(const psp_tex_state *t, int level,
                                      const psp_clut_state *clut,
                                      uint32_t *out, size_t cap,
                                      int *out_w, int *out_h, size_t *padded);

/* Resolve the PSP's per-primitive level of detail to a signed count of
 * sixteenths. `rho` is the greatest texture-coordinate gradient in texels per
 * screen pixel; `w` is the primitive's clip-space W, the mean of
 * psp_render_vertex_w over its vertices, which SLOPE mode scales by. Keeping
 * this rule in the runtime lets software and GPU backends make the same
 * AUTO / CONST / SLOPE decision before their samplers choose and blend mip
 * levels. */
int psp_render_lod16(const psp_tex_state *t, float rho, float w);
/* A vertex's clip-space W: 1/inv_w, and 1 for through-mode geometry. */
float psp_render_vertex_w(const psp_vertex *v);

/* One-pixel primitives use explicit coverage, not the host API's line rules.
 * Walk a half-open segment, clipped to inclusive pixel bounds. Each callback
 * receives a pixel-centred vertex with already interpolated colour, depth,
 * fog and divided texture coordinates (inv_w = tex_q = 1). Position, colour
 * and depth follow the hardware (geprobe step 1 and geprobe 5 scene 27, fw
 * 6.60): one pixel per major-axis column whose centre lies on the segment,
 * the minor coordinate, colour and depth taken where that centre projects
 * onto it (for whole-pixel endpoints, step i's centre, i + 1/2), colour and
 * depth on gradients floored to 1/1024 a pixel. */
typedef void (*psp_line_pixel_fn)(const psp_vertex *sample, void *opaque);
void psp_render_walk_line(const psp_vertex *a, const psp_vertex *b,
                          int x0, int y0, int x1, int y1,
                          psp_line_pixel_fn emit, void *opaque);
int psp_render_line_lod16(const psp_tex_state *t,
                          const psp_vertex *a, const psp_vertex *b);

/* Add a backend the runtime does not carry. Anything needing a window or a GL
 * context lives in the host -- the core has no external dependencies and SDL2
 * is the host's -- so the host builds one and registers it here, after which
 * it is selectable by name like any other. The pointer is kept, not copied, so
 * it must outlive the run. Returns 0, or -1 if the name is empty, already
 * taken, the table is full, or any of the twelve entry points is missing.
 * Registration is the only place that missing one can be caught cheaply. */
int psp_render_register(const psp_render_backend *b);

/* The i'th backend's name, or NULL once past the end -- so a caller can say
 * which names it would have accepted without keeping its own copy of the list
 * for that list to drift from. */
const char *psp_render_backend_name(size_t i);

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

/* ZBP / ZBW: where the depth buffer lives in VRAM and its row stride in
 * pixels. The software backend keeps depth there, 16 bits a pixel. */
void     psp_render_set_depth_buffer(uint32_t addr, uint32_t stride);

/* Return the depth-buffer registers to their start-of-run values. The depth
 * itself is guest VRAM; the game clears it through the GE -- a clear-mode
 * draw with the depth bit set. psp_ge_reset calls it. */
void     psp_render_reset_depth(void);

#endif
