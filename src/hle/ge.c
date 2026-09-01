/* psprecomp — sceGe_user.
 *
 * The GE is the PSP's GPU. It is not driven by function calls: user code builds
 * a **display list** — an array of 32-bit words, each an 8-bit command and 24
 * bits of argument — and hands the GE a pointer plus a *stall address*. The GE
 * consumes commands up to the stall, and the CPU moves the stall forward as it
 * writes more. That producer/consumer arrangement is the whole API.
 *
 * So `sceGu*` (the list-building library) is ordinary user code and gets
 * recompiled like anything else. Only list *execution* is emulated, and that is
 * this file.
 *
 * ## What this does and does not do
 *
 * It walks the list, follows control flow (JUMP/CALL/RET/END/FINISH), and
 * tracks the state commands that matter — framebuffer, vertex format,
 * primitive counts. It does **not rasterize**. No triangles are drawn.
 *
 * That is deliberately the useful half to build first. During bring-up the
 * question is not "does it look right" but "is the game drawing anything at
 * all, and what?" — and a command-stream summary answers that, while a
 * half-working rasterizer answers it misleadingly. psp_ge_dump_stats() reports
 * what the game asked for; making those triangles appear is a separate phase
 * with its own correctness problem.
 */

#include "psprecomp/hle.h"
#include "psprecomp/render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Display-list opcodes. Only the ones the walk needs to be correct about are
 * named; everything else is counted rather than guessed at, because a
 * misidentified state command silently changes rendering. */
#define GE_NOP          0x00
#define GE_VADDR        0x01
#define GE_IADDR        0x02
#define GE_PRIM         0x04
#define GE_BEZIER       0x05
#define GE_SPLINE       0x06
#define GE_JUMP         0x08
#define GE_BJUMP        0x09
#define GE_CALL         0x0A
#define GE_RET          0x0B
#define GE_END          0x0C
#define GE_SIGNAL       0x0E
#define GE_FINISH       0x0F
#define GE_BASE         0x10
#define GE_VTYPE        0x12
#define GE_OFFSET_ADDR  0x13
#define GE_ORIGIN_ADDR  0x14
/* Texture state. Numbers from PPSSPP's GPU/ge_constants.h. */
#define GE_TEXADDR0     0xA0
#define GE_TEXBUFWIDTH0 0xA8
#define GE_CLUTADDR     0xB0
#define GE_CLUTADDRUPPER 0xB1
#define GE_TEXSIZE0     0xB8
#define GE_TEXMODE      0xC2
#define GE_TEXFORMAT    0xC3
#define GE_LOADCLUT     0xC4
#define GE_CLUTFORMAT   0xC5
#define GE_TEXFILTER    0xC6
#define GE_TEXWRAP      0xC7
#define GE_TEXFUNC      0xC9

/* Block transfer -- how a game gets image data into VRAM. */
/* Whether texturing applies at all. Distinct from whether a texture is bound:
 * the GE keeps its texture state across draws, so geometry drawn with
 * texturing off must not be painted with whatever was last set up. */
#define GE_TEXTUREMAPENABLE 0x1E

#define GE_TRANSFERSRC     0xB2
#define GE_TRANSFERSRCW    0xB3
#define GE_TRANSFERDST     0xB4
#define GE_TRANSFERDSTW    0xB5
#define GE_TRANSFERSTART   0xEA
#define GE_TRANSFERSRCPOS  0xEB
#define GE_TRANSFERDSTPOS  0xEC
#define GE_TRANSFERSIZE    0xEE

#define GE_FBP          0x9C
#define GE_FBW          0x9D

/* Transform and lighting state.
 *
 * Matrix uploads are two commands: a NUMBER that sets the write index, then a
 * run of DATA words each carrying one element. World, view and texgen are 4
 * columns of 3 rows -- the bottom row is implied (0,0,0,1) and never sent --
 * so twelve elements; projection is a full 4x4, so sixteen. That asymmetry is
 * the SDK's, not a guess: sceGuSetMatrix sends 12 for GU_MODEL/GU_VIEW and 16
 * for GU_PROJECTION.
 *
 * DATA carries 24 bits where a float needs 32. The hardware takes the low
 * eight off the mantissa, so the word reassembles as arg << 8. */
#define GE_WORLDMATRIXNUMBER 0x3A
#define GE_WORLDMATRIXDATA   0x3B
#define GE_VIEWMATRIXNUMBER  0x3C
#define GE_VIEWMATRIXDATA    0x3D
#define GE_PROJMATRIXNUMBER  0x3E
#define GE_PROJMATRIXDATA    0x3F
#define GE_VIEWPORTXSCALE    0x42
#define GE_VIEWPORTYSCALE    0x43
#define GE_VIEWPORTZSCALE    0x44
#define GE_VIEWPORTXCENTER   0x45
#define GE_VIEWPORTYCENTER   0x46
#define GE_VIEWPORTZCENTER   0x47
#define GE_OFFSETX           0x4C
#define GE_OFFSETY           0x4D
#define GE_CULLFACEENABLE    0x1D
#define GE_CULL              0x9B
#define GE_MASKRGB           0xD8
#define GE_MASKALPHA         0xD9
#define GE_ZTESTENABLE       0x23
#define GE_ZTEST             0xDE
#define GE_ZWRITEDISABLE     0xE7
#define GE_CLEARMODE         0xD3
#define GE_ALPHABLENDENABLE  0x21
#define GE_ALPHATESTENABLE   0x22
#define GE_BLENDMODE         0xDF
#define GE_BLENDFIXEDA       0xE0
#define GE_BLENDFIXEDB       0xE1
#define GE_ALPHATEST         0xDB

/* VTYPE field extraction. */
#define VT_TEX(v)     ((v) & 3)
#define VT_COLOR(v)   (((v) >> 2) & 7)
#define VT_NORMAL(v)  (((v) >> 5) & 3)
#define VT_POS(v)     (((v) >> 7) & 3)
#define VT_WEIGHT(v)  (((v) >> 9) & 3)
#define VT_INDEX(v)   (((v) >> 11) & 3)
#define VT_THROUGH(v) (((v) >> 23) & 1)

#define MAX_QUEUES 8
#define GE_STACK   8

/* Primitive types, from the PRIM argument's type field. */
static const char *const PRIM_NAME[8] = {
    "points", "lines", "line-strip", "triangles",
    "triangle-strip", "triangle-fan", "sprites", "?"
};

typedef struct {
    uint32_t id;
    uint32_t list;      /* current read pointer */
    uint32_t stall;     /* stop before this address; 0 means "no stall" */
    uint32_t base;      /* GE_BASE: high bits for addresses */
    uint32_t origin;
    int      used;
    int      done;
} ge_queue;

static ge_queue g_queue[MAX_QUEUES];
static uint32_t g_next_id;

/* Vertices the draw path declined, split by why.
 *
 * These were one counter, reported as "transformed, or no position". Three
 * causes with three unrelated fixes -- a missing transform pipeline, a vertex
 * pointer the stream never set, and a layout this does not decode -- summed
 * into a number that could not tell you which you were looking at. The first
 * needs T&L, the second is a state-tracking bug, the third is a decoder gap.
 * Guessing between them is the same mistake as reading one stop reason for
 * another, so they are counted apart. */
static uint64_t g_skip_noaddr;     /* no vertex address in the stream */
static uint64_t g_skip_layout;     /* weighted, or no position -- vertex_layout declined */
static uint64_t g_skip_nearplane;  /* transformed behind the eye; no clipper yet */
static uint64_t g_culled;          /* backfacing, by the game's own winding rule */
static uint64_t g_xformed;         /* vertices that went through the pipeline */
/* Draws by path and by whether a texture was bound. "Most pixels are flat" has
 * two very different readings depending on which path they came from. */
static uint64_t g_draw_2d_tex, g_draw_2d_flat, g_draw_3d_tex, g_draw_3d_flat;
/* The distinct vertex colours the transform path reads. "Everything is white"
 * needs to distinguish a white model from a colour that is not being read. */
static uint32_t g_col_seen[8]; static int g_col_n;
static uint64_t g_clear_draws, g_clear_z_draws;
static void note_colour(uint32_t c) {
    for (int i = 0; i < g_col_n; i++) if (g_col_seen[i] == c) return;
    if (g_col_n < 8) g_col_seen[g_col_n++] = c;
}

/* The transform pipeline's state.
 *
 * Kept apart from g_ge because it resets differently: matrices persist across
 * lists, and a NUMBER command sets a cursor that the following DATA words walk
 * forward. Out-of-range writes are dropped rather than wrapped -- a list still
 * being built by the CPU can be read mid-write, and wrapping would corrupt the
 * matrix rather than skip a word of it. */
static struct {
    float world[12], view[12], proj[16];
    int   world_n, view_n, proj_n;
    float vp_xs, vp_ys, vp_zs, vp_xc, vp_yc, vp_zc;
    float off_x, off_y;
    int   vp_set;
    int   cull_enable, cull_ccw;
    int   ztest_enable, ztest_func, zwrite_off, clear_mode, clear_colour, clear_z;
    psp_blend_state blend;
    /* Which matrices the stream actually uploaded, and where the result lands.
     * "Geometry is being transformed" and "transformed by the matrices the game
     * meant" are different claims, and a screen-space bounding box separates
     * them: a plausible scene sits inside the frame, an identity-by-omission
     * pipeline piles everything at one point, and a wrong matrix throws it to
     * coordinates with no relation to a 480x272 screen. */
    uint32_t world_words, view_words, proj_words;
    float bb_x0, bb_y0, bb_x1, bb_y1;
    int   bb_seen;
} g_tl;

/* A GE float argument: 24 bits of mantissa-truncated float, in the low bits. */
static float ge_float(uint32_t arg) {
    union { uint32_t u; float f; } c;
    c.u = arg << 8;
    return c.f;
}

/* Tracked state, and the counters that make the report worth reading. */
static struct {
    uint32_t fbp, fbw, vtype, vaddr;
    /* Texture state, recorded so the sampler can be built against what this
     * game uses rather than against the whole hardware surface. */
    uint32_t tex_addr, tex_stride, tex_w, tex_h, tex_enable;
    uint32_t tex_format, tex_func, tex_filter, tex_wrap, tex_swizzled;
    uint32_t clut_addr, clut_format, clut_raw;
    uint32_t tex_formats_seen, tex_funcs_seen;
    uint32_t xfer_src, xfer_srcw, xfer_dst, xfer_dstw;
    uint32_t xfer_srcpos, xfer_dstpos, xfer_size, xfer_start;
    uint64_t xfers, xfer_bytes, xfer_starts, xfer_rejected;
    /* Measured over geometry that actually draws, as opposed to the state
     * last *set* -- which is a different thing, and confusing the two has
     * already sent this investigation down one blind alley. */
    uint32_t drawn_vtype;
    uint64_t drawn_prims;
    float    u_lo, u_hi, v_lo, v_hi;
    int      uv_seen;
    uint64_t clut_loads;
    uint64_t commands;
    uint64_t prims[8];
    uint64_t vertices;
    uint64_t unknown;
    uint64_t lists;
    uint64_t finishes;
} g_ge;

/* The texel range the draws actually sample.
 *
 * u_lo/u_hi were declared and printed from the first version of this file and
 * never once assigned, so the summary reported "u 0.0..0.0" for every run ever
 * made -- a reading that looks like a measurement and is a fixed constant. It
 * is what tells "the texture is sampled at one corner" apart from "the texture
 * is sampled across its whole surface", which is the question that comes up
 * every time the frame is one flat colour. */
static void note_uv(float u, float v) {
    if (!g_ge.uv_seen) {
        g_ge.u_lo = g_ge.u_hi = u;
        g_ge.v_lo = g_ge.v_hi = v;
        g_ge.uv_seen = 1;
        return;
    }
    if (u < g_ge.u_lo) g_ge.u_lo = u;
    if (u > g_ge.u_hi) g_ge.u_hi = u;
    if (v < g_ge.v_lo) g_ge.v_lo = v;
    if (v > g_ge.v_hi) g_ge.v_hi = v;
}


void psp_ge_reset(void) {
    memset(g_queue, 0, sizeof g_queue);
    memset(&g_ge, 0, sizeof g_ge);
    psp_render_reset_pixels();
    psp_render_reset_depth();
    g_skip_noaddr = g_skip_layout = g_skip_nearplane = 0;
    g_culled = g_xformed = 0;
    g_draw_2d_tex = g_draw_2d_flat = g_draw_3d_tex = g_draw_3d_flat = 0;
    g_col_n = 0;
    g_clear_draws = g_clear_z_draws = 0;
    memset(&g_tl, 0, sizeof g_tl);
    g_next_id = 0x00080000u;
}

void psp_ge_init(void) { psp_ge_reset(); }

void psp_ge_dump_stats(FILE *out) {
    fprintf(out, "GE: %llu lists, %llu commands, %llu finishes\n",
            (unsigned long long)g_ge.lists,
            (unsigned long long)g_ge.commands,
            (unsigned long long)g_ge.finishes);
    if (g_ge.tex_addr || g_ge.tex_formats_seen) {
        static const char *const TF[16] = {
            "5650","5551","4444","8888","clut4","clut8","clut16","clut32",
            "dxt1","dxt3","dxt5","?","?","?","?","?" };
        static const char *const FN[8] = {
            "modulate","decal","blend","replace","add","?","?","?" };
        fprintf(out, "    texture    0x%08X %ux%u stride %u, %s, %s%s\n",
                g_ge.tex_addr, g_ge.tex_w, g_ge.tex_h, g_ge.tex_stride,
                TF[g_ge.tex_format & 15], FN[g_ge.tex_func & 7],
                g_ge.tex_swizzled ? ", swizzled" : "");
        fprintf(out, "    formats used:");
        for (int i = 0; i < 16; i++)
            if (g_ge.tex_formats_seen & (1u << i)) fprintf(out, " %s", TF[i]);
        for (int i = 0; i < 8; i++)
            if (g_ge.tex_funcs_seen & (1u << i)) fprintf(out, " %s", FN[i]);
        if (g_ge.clut_loads)
            fprintf(out, "  (%llu clut loads, fmt %u)",
                    (unsigned long long)g_ge.clut_loads, g_ge.clut_format);
        fprintf(out, "\n");
        /* Sampling state, which the report has never carried. It decides
         * whether a glyph edge is a hard texel boundary or a ramp, so "the
         * letters are ragged" cannot be reasoned about without it. Both are
         * tracked and neither reaches the backend yet; printing them says which
         * of the two is worth wiring up for this game rather than for the
         * hardware in general. */
        {
            static const char *const FI[8] = {
                "nearest","linear","?","?",
                "near/mip-near","lin/mip-near","near/mip-lin","lin/mip-lin" };
            fprintf(out, "    sampling   filter min %s mag %s, wrap s %s t %s\n",
                    FI[g_ge.tex_filter & 7], FI[(g_ge.tex_filter >> 8) & 7],
                    (g_ge.tex_wrap & 1) ? "clamp" : "repeat",
                    ((g_ge.tex_wrap >> 8) & 1) ? "clamp" : "repeat");
        }
    }
    if (g_ge.drawn_prims)
        fprintf(out, "    drawn      %llu prims, vertex type 0x%06X"
                     " (tex %u colour %u pos %u through %u), u %.1f..%.1f v %.1f..%.1f, texturing %s\n",
                (unsigned long long)g_ge.drawn_prims, g_ge.drawn_vtype,
                VT_TEX(g_ge.drawn_vtype), VT_COLOR(g_ge.drawn_vtype),
                VT_POS(g_ge.drawn_vtype), VT_THROUGH(g_ge.drawn_vtype),
                (double)g_ge.u_lo, (double)g_ge.u_hi,
                (double)g_ge.v_lo, (double)g_ge.v_hi,
                g_ge.tex_enable ? "on" : "off");
    if (g_ge.xfer_starts)
        fprintf(out, "    transfers  %llu asked, %llu done (%llu bytes), %llu rejected\n",
                (unsigned long long)g_ge.xfer_starts, (unsigned long long)g_ge.xfers,
                (unsigned long long)g_ge.xfer_bytes,
                (unsigned long long)g_ge.xfer_rejected);
    fprintf(out, "    framebuffer 0x%08X stride %u, vertex type 0x%06X\n",
            g_ge.fbp, g_ge.fbw, g_ge.vtype);
    fprintf(out, "    vertices submitted: %llu\n", (unsigned long long)g_ge.vertices);
    for (int i = 0; i < 8; i++)
        if (g_ge.prims[i])
            fprintf(out, "    %-15s %llu\n", PRIM_NAME[i], (unsigned long long)g_ge.prims[i]);
    if (g_ge.unknown)
        fprintf(out, "    %llu commands not individually decoded\n",
                (unsigned long long)g_ge.unknown);
    fprintf(out, "    pixels written: %llu (%llu textured, %llu flat)\n",
            (unsigned long long)psp_render_pixels(),
            (unsigned long long)psp_render_textured_pixels(),
            (unsigned long long)psp_render_flat_pixels());
    fprintf(out, "    depth: test %s func %d, write %s, %llu pixels rejected\n",
            g_tl.ztest_enable ? "on" : "off", g_tl.ztest_func,
            g_tl.zwrite_off ? "off" : "on",
            (unsigned long long)psp_render_zfail_pixels());
    fprintf(out, "    blend %s (src %d dst %d eq %d), alpha test %s func %d ref %d: "
                 "%llu blended, %llu alpha-killed\n",
            g_tl.blend.enable ? "on" : "off", g_tl.blend.src, g_tl.blend.dst,
            g_tl.blend.eq, g_tl.blend.alpha_test ? "on" : "off",
            g_tl.blend.alpha_func, g_tl.blend.alpha_ref,
            (unsigned long long)psp_render_blended_pixels(),
            (unsigned long long)psp_render_alphakill_pixels());
    fprintf(out, "    clear-mode draws: %llu (%llu clearing depth)\n",
            (unsigned long long)g_clear_draws, (unsigned long long)g_clear_z_draws);
    if (g_xformed) {
        fprintf(out, "    transformed %llu vertices; viewport %s",
                (unsigned long long)g_xformed,
                g_tl.vp_set ? "set" : "defaulted");
        if (g_tl.vp_set)
            fprintf(out, " (scale %.1f,%.1f centre %.1f,%.1f offset %.1f,%.1f)",
                    g_tl.vp_xs, g_tl.vp_ys, g_tl.vp_xc, g_tl.vp_yc,
                    g_tl.off_x, g_tl.off_y);
        fprintf(out, ", cull %s\n", g_tl.cull_enable ? (g_tl.cull_ccw ? "ccw" : "cw") : "off");
        fprintf(out, "    draws: 2d %llu textured / %llu flat, 3d %llu textured / %llu flat\n",
                (unsigned long long)g_draw_2d_tex, (unsigned long long)g_draw_2d_flat,
                (unsigned long long)g_draw_3d_tex, (unsigned long long)g_draw_3d_flat);
        fprintf(out, "    vertex colours seen (first %d):", g_col_n);
        for (int i = 0; i < g_col_n; i++) fprintf(out, " %08X", g_col_seen[i]);
        fprintf(out, "\n");
        fprintf(out, "    matrix words: world %u, view %u, proj %u\n",
                g_tl.world_words, g_tl.view_words, g_tl.proj_words);
        if (g_tl.bb_seen)
            fprintf(out, "    screen bounds: x %.1f..%.1f  y %.1f..%.1f\n",
                    g_tl.bb_x0, g_tl.bb_x1, g_tl.bb_y0, g_tl.bb_y1);
        if (g_skip_nearplane)
            fprintf(out, "    %llu vertices dropped at the near plane (no clipper)\n",
                    (unsigned long long)g_skip_nearplane);
        if (g_culled)
            fprintf(out, "    %llu vertices culled as backfacing\n",
                    (unsigned long long)g_culled);
    }
    if (g_skip_noaddr)
        fprintf(out, "    %llu vertices dropped: no vertex address set\n",
                (unsigned long long)g_skip_noaddr);
    if (g_skip_layout)
        fprintf(out, "    %llu vertices dropped: layout not decoded (weighted, or no position)\n",
                (unsigned long long)g_skip_layout);
}

uint64_t psp_ge_command_count(void) { return g_ge.commands; }
uint64_t psp_ge_vertex_count(void)  { return g_ge.vertices; }

/* ---- rasterizer ----------------------------------------------------------
 *
 * Enough of the GE to put pixels in the framebuffer. Deliberately narrow:
 *
 *  - "through" mode only (VTYPE bit 23), where vertex coordinates are already
 *    in screen space. That is what 2D games and every UI layer use. Transformed
 *    geometry needs the matrix pipeline and is not attempted here -- drawing it
 *    with the wrong transform would look like a rendering bug rather than a
 *    missing feature.
 *  - Position as 16-bit or float; colour as 8888 or none.
 *  - Flat/interpolated colour, no texturing, no depth, no blending.
 *
 * The point is to close the loop from display list to visible pixels so the
 * rest can be measured against something. Everything omitted is omitted
 * loudly: unsupported vertex formats are counted, not guessed at.
 */



/* Where the GE actually writes.
 *
 * FBP is an offset into VRAM, not an address: the base is implicit in the
 * hardware and never appears in the display list. This game sets 0x00088000
 * and expects 0x04088000 -- which it confirms itself, by flushing the cache
 * over exactly that address before it presents.
 *
 * Taking the offset literally does not merely draw in the wrong place. The
 * module image is mapped at zero, so 0x00088000 lands inside the game's own
 * code, and every rasterized pixel overwrites an instruction. */
/* ---- block transfer --------------------------------------------------------
 *
 * The GE's 2D copy, and the only way a texture reaches VRAM: a game decodes or
 * loads an image into main memory and blits it across. Without this the
 * texture sampler reads whatever VRAM happened to contain, which is zeros --
 * a correctly sampled empty texture.
 *
 * Field extraction follows PPSSPP's GPUState.h getters exactly. The parts
 * worth naming, because each is a way to be quietly wrong:
 *   - address low bits come from SRC/DST masked to 0xFFFFF0, and bits 24-31
 *     from the *stride* register's high byte -- the same split as FBP/FBW.
 *   - width and height are stored as n-1.
 *   - the stride field is 0x7F8 wide, and anything above 0x400 means zero.
 *   - TRANSFERSTART bit 0 selects 32-bit pixels; everything else is 16-bit.
 */
static uint32_t xfer_addr(uint32_t base, uint32_t widthreg) {
    return (base & 0xFFFFF0u) | ((widthreg & 0xFF0000u) << 8);
}

static uint32_t xfer_stride(uint32_t widthreg) {
    const uint32_t stride = widthreg & 0x7F8u;
    return stride > 0x400u ? 0u : stride;
}

static void do_block_transfer(void) {
    const uint32_t src  = xfer_addr(g_ge.xfer_src, g_ge.xfer_srcw);
    const uint32_t dst  = xfer_addr(g_ge.xfer_dst, g_ge.xfer_dstw);
    const uint32_t ss   = xfer_stride(g_ge.xfer_srcw);
    const uint32_t ds   = xfer_stride(g_ge.xfer_dstw);
    const uint32_t sx   = g_ge.xfer_srcpos & 0x3FFu;
    const uint32_t sy   = (g_ge.xfer_srcpos >> 10) & 0x3FFu;
    const uint32_t dx   = g_ge.xfer_dstpos & 0x3FFu;
    const uint32_t dy   = (g_ge.xfer_dstpos >> 10) & 0x3FFu;
    const uint32_t w    = (g_ge.xfer_size & 0x3FFu) + 1u;
    const uint32_t h    = ((g_ge.xfer_size >> 10) & 0x3FFu) + 1u;
    const uint32_t bpp  = (g_ge.xfer_start & 1u) ? 4u : 2u;

    g_ge.xfer_starts++;
    if (!src || !dst || !ss || !ds) {
        /* Counted separately: a transfer the game asked for but that names no
         * usable source, destination or stride is a different problem from one
         * that never arrived. */
        g_ge.xfer_rejected++;
        return;
    }

    /* Row at a time, because source and destination strides differ in general
     * and a single memcpy would only be right when they happen to match. */
    for (uint32_t row = 0; row < h; row++) {
        const uint32_t s = src + ((sy + row) * ss + sx) * bpp;
        const uint32_t d = dst + ((dy + row) * ds + dx) * bpp;
        for (uint32_t i = 0; i < w * bpp; i += 4)
            psp_write32(d + i, psp_read32(s + i));
    }

    g_ge.xfers++;
    g_ge.xfer_bytes += (uint64_t)w * h * bpp;
}

static uint32_t ge_fb_address(uint32_t fbp) {
    return PSP_VRAM_BASE | (fbp & 0x001FFFF0u);
}

uint64_t psp_ge_pixels(void) { return psp_render_pixels(); }

/* Where the GE last drew, as a real address. */
uint32_t psp_ge_target(void) { return g_ge.fbp ? ge_fb_address(g_ge.fbp) : 0; }

/* Size of one vertex in bytes, and the offsets within it. Components appear in
 * a fixed order (weights, texture, colour, normal, position) and each is
 * aligned to its own size, which is what makes the stride awkward enough to be
 * worth computing rather than assuming. */
static int vertex_layout(uint32_t vtype, int *col_off, int *pos_off, int *tex_off) {
    static const int tex_sz[4]   = { 0, 1, 2, 4 };
    static const int col_sz[8]   = { 0, 0, 0, 0, 2, 2, 2, 4 };
    static const int norm_sz[4]  = { 0, 1, 2, 4 };
    static const int pos_sz[4]   = { 0, 1, 2, 4 };

    int off = 0, align = 1;
    int t = tex_sz[VT_TEX(vtype)] * 2;
    int c = col_sz[VT_COLOR(vtype)];
    int n = norm_sz[VT_NORMAL(vtype)] * 3;
    int p = pos_sz[VT_POS(vtype)] * 3;

    if (VT_WEIGHT(vtype)) return 0;          /* skinning: not handled */

    int ts = tex_sz[VT_TEX(vtype)];
    if (ts) { off = (off + ts - 1) & ~(ts - 1); *tex_off = off; off += t;
              if (ts > align) align = ts; }
    else *tex_off = -1;
    int cs = col_sz[VT_COLOR(vtype)];
    if (cs) { off = (off + cs - 1) & ~(cs - 1); *col_off = off; off += c; if (cs > align) align = cs; }
    else *col_off = -1;
    int ns = norm_sz[VT_NORMAL(vtype)];
    if (ns) { off = (off + ns - 1) & ~(ns - 1); off += n; if (ns > align) align = ns; }
    int ps = pos_sz[VT_POS(vtype)];
    if (!ps) return 0;                        /* no position: nothing to draw */
    off = (off + ps - 1) & ~(ps - 1); *pos_off = off; off += p;
    if (ps > align) align = ps;

    return (off + align - 1) & ~(align - 1);  /* stride */
}

static int read_vertex(uint32_t addr, uint32_t vtype, int col_off, int pos_off,
                       int tex_off, psp_vertex *out) {
    out->rgba = 0xFFFFFFFFu;
    out->u = out->v = 0.0f;

    /* Through-mode texture coordinates are in texels, whatever their width, so
     * every form is taken as it comes. Leaving the narrow ones at zero -- which
     * is what this did at first -- makes every sprite sample texel (0,0) and
     * paints the screen in whatever colour happens to be in that corner. */
    switch (tex_off >= 0 ? VT_TEX(vtype) : 0) {
    case 1:   /* 8-bit */
        out->u = (float)psp_read8(addr + (uint32_t)tex_off);
        out->v = (float)psp_read8(addr + (uint32_t)tex_off + 1);
        break;
    case 2:   /* 16-bit */
        out->u = (float)psp_read16(addr + (uint32_t)tex_off);
        out->v = (float)psp_read16(addr + (uint32_t)tex_off + 2);
        break;
    case 3:   /* float */
        out->u = psp_read_f32(addr + (uint32_t)tex_off);
        out->v = psp_read_f32(addr + (uint32_t)tex_off + 4);
        break;
    default:
        break;
    }
    if (tex_off >= 0) note_uv(out->u, out->v);
    if (col_off >= 0 && VT_COLOR(vtype) == 7)
        out->rgba = psp_read32(addr + (uint32_t)col_off);

    switch (VT_POS(vtype)) {
    case 2:   /* 16-bit */
        out->x = (int16_t)psp_read16(addr + (uint32_t)pos_off);
        out->y = (int16_t)psp_read16(addr + (uint32_t)pos_off + 2);
        /* Through-mode depth is already a window value, and unsigned: the
         * screen z range is 0..65535, not -32768..32767. */
        out->z = (float)(uint16_t)psp_read16(addr + (uint32_t)pos_off + 4);
        return 1;
    case 3: { /* float */
        out->x = (int)psp_read_f32(addr + (uint32_t)pos_off);
        out->y = (int)psp_read_f32(addr + (uint32_t)pos_off + 4);
        out->z = psp_read_f32(addr + (uint32_t)pos_off + 8);
        return 1;
    }
    default:
        return 0;
    }
}


/* Model-space position, for transformed geometry.
 *
 * Narrow components are normalised here and raw in through-mode: an s16 is a
 * signed fraction of 32768, an s8 of 128. Reading them raw instead puts a unit
 * cube 32768 units across, which projects to nothing recognisable and looks
 * like a broken matrix rather than a scaling mistake. */
static int read_pos_model(uint32_t addr, uint32_t vtype, int pos_off, float p[3]) {
    const uint32_t a = addr + (uint32_t)pos_off;
    switch (VT_POS(vtype)) {
    case 1:
        p[0] = (float)(int8_t)psp_read8(a)       / 128.0f;
        p[1] = (float)(int8_t)psp_read8(a + 1)   / 128.0f;
        p[2] = (float)(int8_t)psp_read8(a + 2)   / 128.0f;
        return 1;
    case 2:
        p[0] = (float)(int16_t)psp_read16(a)     / 32768.0f;
        p[1] = (float)(int16_t)psp_read16(a + 2) / 32768.0f;
        p[2] = (float)(int16_t)psp_read16(a + 4) / 32768.0f;
        return 1;
    case 3:
        p[0] = psp_read_f32(a);
        p[1] = psp_read_f32(a + 4);
        p[2] = psp_read_f32(a + 8);
        return 1;
    default:
        return 0;
    }
}

/* Texture coordinates for transformed geometry are normalised, not texels, so
 * they scale by the texture size. Through-mode gives texels directly, which is
 * why the two paths read them differently. */
static void read_uv_model(uint32_t addr, uint32_t vtype, int tex_off, psp_vertex *out) {
    out->u = out->v = 0.0f;
    if (tex_off < 0) return;
    const uint32_t a = addr + (uint32_t)tex_off;
    float u = 0.0f, v = 0.0f;
    switch (VT_TEX(vtype)) {
    case 1: u = (float)(int8_t)psp_read8(a)       / 128.0f;
            v = (float)(int8_t)psp_read8(a + 1)   / 128.0f;   break;
    case 2: u = (float)(int16_t)psp_read16(a)     / 32768.0f;
            v = (float)(int16_t)psp_read16(a + 2) / 32768.0f; break;
    case 3: u = psp_read_f32(a); v = psp_read_f32(a + 4);     break;
    default: return;
    }
    out->u = u * (float)g_ge.tex_w;
    out->v = v * (float)g_ge.tex_h;
    note_uv(out->u, out->v);
}

/* World and view are 4 columns of 3 rows; the implied bottom row makes the
 * fourth column a translation. */
static void mul_4x3(const float m[12], const float in[3], float out[3]) {
    out[0] = m[0]*in[0] + m[3]*in[1] + m[6]*in[2] + m[9];
    out[1] = m[1]*in[0] + m[4]*in[1] + m[7]*in[2] + m[10];
    out[2] = m[2]*in[0] + m[5]*in[1] + m[8]*in[2] + m[11];
}

/* Projection is a full 4x4, column-major, and produces the w that the divide
 * needs -- which is the whole reason it is not folded into the 4x3 above. */
static void mul_4x4(const float m[16], const float in[3], float out[4]) {
    out[0] = m[0]*in[0] + m[4]*in[1] + m[8] *in[2] + m[12];
    out[1] = m[1]*in[0] + m[5]*in[1] + m[9] *in[2] + m[13];
    out[2] = m[2]*in[0] + m[6]*in[1] + m[10]*in[2] + m[14];
    out[3] = m[3]*in[0] + m[7]*in[1] + m[11]*in[2] + m[15];
}

/* Clip space to screen. The viewport is the game's if it set one; the fallback
 * is the standard 480x272 arrangement, with y scaled negative because screen y
 * grows downward and clip y grows up. */
static void to_screen(const float clip[4], float *sx, float *sy, float *sz) {
    const float inv = 1.0f / clip[3];
    const float nx = clip[0] * inv, ny = clip[1] * inv, nz = clip[2] * inv;
    if (g_tl.vp_set) {
        *sx = nx * g_tl.vp_xs + g_tl.vp_xc - g_tl.off_x;
        *sy = ny * g_tl.vp_ys + g_tl.vp_yc - g_tl.off_y;
    } else {
        *sx = nx * 240.0f + 240.0f;
        *sy = ny * -136.0f + 136.0f;
    }
    /* The z terms are set independently of the x/y ones, so a game can leave
     * them at zero; that would collapse every depth to one value and make the
     * test meaningless, so fall back to the full 0..65535 window range. */
    *sz = (g_tl.vp_zs != 0.0f) ? nz * g_tl.vp_zs + g_tl.vp_zc
                               : (nz * 0.5f + 0.5f) * 65535.0f;
}

/* Transformed geometry, one primitive at a time.
 *
 * Two things are missing and both are stated rather than hidden. There is no
 * clipper: a primitive with any vertex at or behind the eye is dropped whole,
 * because the perspective divide is meaningless there and the alternative --
 * dividing anyway -- projects the vertex to the wrong side of the screen and
 * draws a triangle across the whole frame. And there is no depth buffer, so
 * primitives land in submission order. Backface culling is honoured, which
 * removes the half of a closed mesh that would otherwise paint over the half
 * in front of it, but it is not a substitute for a depth test. */
/* PSPRECOMP_GE_DRAWLOG=<n> narrates the first n primitives: where they landed,
 * what colour, and whether a texture was bound.
 *
 * The summary reports aggregates -- a bounding box over every transformed
 * vertex, one vertex type, one texture. When one element on screen looks wrong
 * and the rest looks right, aggregates cannot say which draw is the bad one.
 * This can. */
static int drawlog_left(void) {
    static int n = -1;
    if (n < 0) { const char *v = getenv("PSPRECOMP_GE_DRAWLOG"); n = (v && *v) ? atoi(v) : 0; }
    return n > 0 ? n-- : 0;
}

static void draw_prim_transformed(uint32_t type, uint32_t count,
                                  int col_off, int pos_off, int tex_off, int stride) {
    enum { BATCH = 256 };
    psp_vertex v[BATCH];
    float      w[BATCH];
    const psp_render_backend *be = psp_render_current();

    uint32_t done = 0;
    while (done < count) {
        uint32_t n = count - done;
        if (n > BATCH) n = BATCH;

        uint32_t decoded = 0;
        for (; decoded < n; decoded++) {
            const uint32_t a = g_ge.vaddr + (done + decoded) * (uint32_t)stride;
            float model[3], world[3], eye[3], clip[4];
            if (!read_pos_model(a, g_ge.vtype, pos_off, model)) break;

            mul_4x3(g_tl.world, model, world);
            mul_4x3(g_tl.view,  world, eye);
            mul_4x4(g_tl.proj,  eye,   clip);

            psp_vertex *o = &v[decoded];
            o->rgba = 0xFFFFFFFFu;
            if (col_off >= 0 && VT_COLOR(g_ge.vtype) == 7)
                o->rgba = psp_read32(a + (uint32_t)col_off);
            note_colour(o->rgba);
            read_uv_model(a, g_ge.vtype, tex_off, o);

            w[decoded] = clip[3];
            float sx, sy, sz;
            if (clip[3] > 1e-6f) to_screen(clip, &sx, &sy, &sz);
            else                 sx = sy = sz = 0.0f;
            o->x = (int)sx;
            o->y = (int)sy;
            o->z = sz;
            if (clip[3] > 1e-6f) {
                if (!g_tl.bb_seen) { g_tl.bb_x0 = g_tl.bb_x1 = sx;
                                     g_tl.bb_y0 = g_tl.bb_y1 = sy; g_tl.bb_seen = 1; }
                if (sx < g_tl.bb_x0) g_tl.bb_x0 = sx;
                if (sx > g_tl.bb_x1) g_tl.bb_x1 = sx;
                if (sy < g_tl.bb_y0) g_tl.bb_y0 = sy;
                if (sy > g_tl.bb_y1) g_tl.bb_y1 = sy;
            }
        }
        if (!decoded) break;
        g_xformed += decoded;

        if (drawlog_left()) {
            int x0 = v[0].x, x1 = v[0].x, y0 = v[0].y, y1 = v[0].y;
            for (uint32_t i = 1; i < decoded; i++) {
                if (v[i].x < x0) x0 = v[i].x;   if (v[i].x > x1) x1 = v[i].x;
                if (v[i].y < y0) y0 = v[i].y;   if (v[i].y > y1) y1 = v[i].y;
            }
            fprintf(stderr, "draw: %-14s %2u verts  x %4d..%-4d y %4d..%-4d  "
                            "fbp %08X  rgba %08X  vtype %06X  tex %s\n",
                    PRIM_NAME[type & 7], decoded, x0, x1, y0, y1,
                    ge_fb_address(g_ge.fbp), v[0].rgba,
                    g_ge.vtype, (g_ge.tex_enable && tex_off >= 0 && g_ge.tex_addr)
                                ? "yes" : "no");
            fprintf(stderr, "      cols");
            for (uint32_t i = 0; i < decoded && i < 4; i++)
                fprintf(stderr, " %08X", v[i].rgba);
            fprintf(stderr, "   blend %s src %d dst %d eq %d  atest %s func %d ref %d"
                            "  clear %s\n",
                    g_tl.blend.enable ? "on" : "off", g_tl.blend.src, g_tl.blend.dst,
                    g_tl.blend.eq, g_tl.blend.alpha_test ? "on" : "off",
                    g_tl.blend.alpha_func, g_tl.blend.alpha_ref,
                    g_tl.clear_mode ? "ON" : "off");
            fprintf(stderr, "      fbp %08X fbw %u  vp scale %.1f,%.1f centre %.1f,%.1f"
                            "  offset %.1f,%.1f%s\n      world",
                    ge_fb_address(g_ge.fbp), g_ge.fbw,
                    g_tl.vp_xs, g_tl.vp_ys, g_tl.vp_xc, g_tl.vp_yc,
                    g_tl.off_x, g_tl.off_y, g_tl.vp_set ? "" : " (defaulted)");
            for (int i = 0; i < 12; i++) fprintf(stderr, " %.2f", g_tl.world[i]);
            fprintf(stderr, "\n      view ");
            for (int i = 0; i < 12; i++) fprintf(stderr, " %.2f", g_tl.view[i]);
            fprintf(stderr, "\n      proj ");
            for (int i = 0; i < 16; i++) fprintf(stderr, " %.3f", g_tl.proj[i]);
            fprintf(stderr, "\n      raw v0@%08X:", g_ge.vaddr + done * (uint32_t)stride);
            for (int b = 0; b < stride && b < 32; b++)
                fprintf(stderr, "%02X", psp_read8(g_ge.vaddr + done * (uint32_t)stride + (uint32_t)b));
            fprintf(stderr, "  stride %d tex_off %d col_off %d pos_off %d",
                    stride, tex_off, col_off, pos_off);
            fprintf(stderr, "\n      model v0");
            {
                float m[3];
                for (uint32_t i = 0; i < decoded && i < 4; i++) {
                    read_pos_model(g_ge.vaddr + (done + i) * (uint32_t)stride,
                                   g_ge.vtype, pos_off, m);
                    fprintf(stderr, "  (%.1f,%.1f,%.1f)", m[0], m[1], m[2]);
                }
            }
            fprintf(stderr, "\n");
        }

        /* Emit primitive by primitive rather than handing the backend the
         * batch: near-plane rejection and culling are per-primitive decisions,
         * and a batch cannot express "all but this one". */
        const int step = (type == PSP_PRIM_TRIANGLE_STRIP ||
                          type == PSP_PRIM_TRIANGLE_FAN) ? 1 : 3;
        if (type == PSP_PRIM_TRIANGLES || type == PSP_PRIM_TRIANGLE_STRIP ||
            type == PSP_PRIM_TRIANGLE_FAN) {
            for (uint32_t i = 2; i < decoded; i += (uint32_t)step) {
                uint32_t i0 = (type == PSP_PRIM_TRIANGLE_FAN) ? 0 : i - 2;
                uint32_t i1 = i - 1, i2 = i;
                if (type == PSP_PRIM_TRIANGLES) { i0 = i - 2; i1 = i - 1; i2 = i; }

                if (w[i0] <= 1e-6f || w[i1] <= 1e-6f || w[i2] <= 1e-6f) {
                    g_skip_nearplane += 3;
                    continue;
                }
                /* Signed area in screen space. A strip alternates winding, so
                 * every second triangle flips -- ignoring that culls exactly
                 * half of every strip and leaves a mesh full of holes. */
                const long ax = v[i1].x - v[i0].x, ay = v[i1].y - v[i0].y;
                const long bx = v[i2].x - v[i0].x, by = v[i2].y - v[i0].y;
                long area = ax * by - ay * bx;
                if (type == PSP_PRIM_TRIANGLE_STRIP && ((i - 2) & 1)) area = -area;
                if (g_tl.cull_enable && area != 0 &&
                    ((area < 0) == (g_tl.cull_ccw != 0))) { g_culled += 3; continue; }

                const psp_vertex tri[3] = { v[i0], v[i1], v[i2] };
                be->draw(PSP_PRIM_TRIANGLES, tri, 3);
            }
        } else {
            be->draw((int)type, v, (int)decoded);
        }

        if ((type == PSP_PRIM_TRIANGLE_STRIP || type == PSP_PRIM_TRIANGLE_FAN) &&
            decoded == BATCH && done + decoded < count)
            done += decoded - 2;
        else
            done += decoded;
    }
}

static void draw_prim(uint32_t type, uint32_t count) {
    /* The sampler is told the current texture at draw time rather than on every
     * state command: the GE sets these fields in any order, and only their
     * value at the draw matters. */

    if (!g_ge.vaddr) { g_skip_noaddr += count; return; }

    int col_off = -1, pos_off = 0, tex_off = -1;
    int stride = vertex_layout(g_ge.vtype, &col_off, &pos_off, &tex_off);
    if (!stride) { g_skip_layout += count; return; }

    /* Bound only when *these* vertices carry coordinates to sample with. The
     * texture state is global and outlives the draw that set it, so geometry
     * with no texcoords would otherwise be painted with whatever texture was
     * last bound, sampled at texel zero -- a whole screen of one colour, which
     * looks like a working renderer having a bad day rather than like
     * untextured geometry. */
    /* The texture address is complete as decoded -- unlike FBP, which is a
     * VRAM offset with the base implied. A texture may legitimately live in
     * main RAM, and forcing it into the VRAM window would send those reads
     * somewhere unrelated. */
    g_ge.drawn_vtype = g_ge.vtype;
    g_ge.drawn_prims++;

    const int has_uv = g_ge.tex_enable && tex_off >= 0 && g_ge.tex_addr;
    psp_render_current()->set_clut(g_ge.clut_addr, (int)(g_ge.clut_raw & 3),
                                   (int)((g_ge.clut_raw >> 2) & 0x1F),
                                   (int)((g_ge.clut_raw >> 8) & 0xFF),
                                   (int)(((g_ge.clut_raw >> 16) & 0x1F) << 4));
    {
        /* The two filter fields are handed over raw. Which one applies depends
         * on the pixel-to-texel scale, which only the rasterizer can work out,
         * so picking one here would be the interpreter guessing at a decision
         * that is not its to make. */
        const psp_tex_state t = {
            .addr       = has_uv ? g_ge.tex_addr : 0,
            .stride     = g_ge.tex_stride,
            .w          = (int)g_ge.tex_w,
            .h          = (int)g_ge.tex_h,
            .fmt        = (int)g_ge.tex_format,
            .func       = (int)g_ge.tex_func,
            .swizzled   = (int)g_ge.tex_swizzled,
            .min_filter = (int)(g_ge.tex_filter & 7),
            .mag_filter = (int)((g_ge.tex_filter >> 8) & 7),
            .wrap_s     = (int)(g_ge.tex_wrap & 1),
            .wrap_t     = (int)((g_ge.tex_wrap >> 8) & 1),
        };
        psp_render_current()->set_texture(&t);
    }
    if (g_tl.clear_mode) { g_clear_draws++; if (g_tl.clear_z) g_clear_z_draws++; }
    if (VT_THROUGH(g_ge.vtype)) { if (has_uv) g_draw_2d_tex++; else g_draw_2d_flat++; }
    else                        { if (has_uv) g_draw_3d_tex++; else g_draw_3d_flat++; }
    {
        /* Clear mode writes the clear values straight through: no blend, no
         * alpha test, or the clear would be filtered by the state it is
         * supposed to be resetting. */
        psp_blend_state b = g_tl.blend;
        b.write_colour = 1;
        if (g_tl.clear_mode) {
            b.enable = 0; b.alpha_test = 0;
            b.write_colour = g_tl.clear_colour;
        }
        psp_render_current()->set_blend(&b);
    }
    /* Clear mode bypasses the depth test as well as texturing, blending and the
     * alpha test. It is a blit of the clear values, so the comparison is forced
     * to ALWAYS and depth write comes from the clear-mode depth bit rather than
     * ZMSK. PPSSPP's software rasterizer does exactly this -- FuncId.cpp sets
     * `depthTestFunc = GE_COMP_ALWAYS` and `depthWrite = isClearModeDepthMask()`
     * under clearMode.
     *
     * An earlier revision ran the game's own test here instead, on the reasoning
     * that a clear should not overwrite geometry that rejected it. That gets the
     * dependency backwards: a clear is what *establishes* the value everything
     * else is tested against, so testing it against the values it is replacing
     * makes it a no-op exactly when it matters. This game runs GEQUAL and clears
     * to the near end, so every clear failed its own test and the depth buffer
     * was never cleared at all. */
    psp_render_current()->set_depth(
        g_tl.clear_mode ? 0 : g_tl.ztest_enable,
        g_tl.clear_mode ? 1 : g_tl.ztest_func,
        g_tl.clear_mode ? g_tl.clear_z : !g_tl.zwrite_off);

    /* Decode the whole batch, then hand it to the backend in one call.
     *
     * Format decoding stays here rather than in each backend: the stride
     * arithmetic and component alignment are fiddly, and duplicating them per
     * backend means every backend is wrong in its own way. Wrong once,
     * centrally, is at least diagnosable. */
    if (!VT_THROUGH(g_ge.vtype)) {
        draw_prim_transformed(type, count, col_off, pos_off, tex_off, stride);
        return;
    }

    enum { BATCH = 256 };
    psp_vertex v[BATCH];
    const psp_render_backend *be = psp_render_current();

    uint32_t done = 0;
    while (done < count) {
        uint32_t n = count - done;
        if (n > BATCH) n = BATCH;

        /* Strips are order-dependent, so a batch boundary must overlap by two
         * vertices or the triangle spanning it is lost. */
        uint32_t decoded = 0;
        for (; decoded < n; decoded++) {
            if (!read_vertex(g_ge.vaddr + (done + decoded) * (uint32_t)stride,
                             g_ge.vtype, col_off, pos_off, tex_off, &v[decoded]))
                break;
        }
        if (!decoded) break;

        if (drawlog_left()) {
            int x0=v[0].x,x1=v[0].x,y0=v[0].y,y1=v[0].y;
            for (uint32_t i=1;i<decoded;i++){
                if(v[i].x<x0)x0=v[i].x; if(v[i].x>x1)x1=v[i].x;
                if(v[i].y<y0)y0=v[i].y; if(v[i].y>y1)y1=v[i].y;
            }
            fprintf(stderr, "2d:   %-14s %2u verts  x %4d..%-4d y %4d..%-4d  "
                            "fbp %08X  rgba %08X %08X  vtype %06X  tex %s\n",
                    PRIM_NAME[type & 7], decoded, x0,x1,y0,y1,
                    ge_fb_address(g_ge.fbp),
                    v[0].rgba, v[decoded>1?1:0].rgba, g_ge.vtype,
                    has_uv ? "yes" : "no");
            fprintf(stderr, "      raw");
            for (uint32_t i = 0; i < decoded && i < 2; i++) {
                const uint32_t a = g_ge.vaddr + (done + i) * (uint32_t)stride;
                fprintf(stderr, "  v%u@%08X:", i, a);
                for (int b = 0; b < stride && b < 16; b++)
                    fprintf(stderr, "%02X", psp_read8(a + (uint32_t)b));
            }
            fprintf(stderr, "  stride %d col_off %d pos_off %d\n", stride, col_off, pos_off);
            fprintf(stderr, "      clear %s (z %d)  blend %s src %d dst %d  "
                            "atest %s func %d ref %d\n",
                    g_tl.clear_mode ? "ON" : "off", g_tl.clear_z,
                    g_tl.blend.enable ? "on" : "off", g_tl.blend.src, g_tl.blend.dst,
                    g_tl.blend.alpha_test ? "on" : "off",
                    g_tl.blend.alpha_func, g_tl.blend.alpha_ref);
        }
        be->draw((int)type, v, (int)decoded);

        if (type == 4 && decoded == BATCH && done + decoded < count)
            done += decoded - 2;     /* strip overlap */
        else
            done += decoded;
    }
}

/* Walk a list until END/FINISH, the stall address, or a step budget.
 *
 * The budget is not paranoia: a list whose JUMP forms a cycle is a normal
 * intermediate state while the CPU is still writing, and without a bound a
 * malformed or partially-written list hangs the host with no diagnostic. */
static void run_list(ge_queue *q) {
    uint32_t stack[GE_STACK];
    int sp = 0;
    uint64_t budget = 1u << 22;

    g_ge.lists++;

    while (budget--) {
        if (q->stall && q->list == q->stall) break;   /* caught up to the CPU */

        uint32_t word = psp_read32(q->list);
        uint32_t cmd  = word >> 24;
        uint32_t arg  = word & 0x00FFFFFF;
        q->list += 4;
        g_ge.commands++;

        switch (cmd) {
        case GE_NOP:
            break;

        case GE_PRIM: {
            uint32_t type  = (arg >> 16) & 7;
            uint32_t count = arg & 0xFFFF;
            g_ge.prims[type]++;
            g_ge.vertices += count;
            draw_prim(type, count);
            break;
        }
        case GE_BEZIER:
        case GE_SPLINE:
            /* Patches expand to triangles on hardware; counted as their own
             * thing rather than folded into the triangle count. */
            g_ge.prims[3]++;
            break;

        case GE_JUMP:
            q->list = (q->base | (arg & 0xFFFFFC));
            break;
        case GE_CALL:
            if (sp < GE_STACK) stack[sp++] = q->list;
            q->list = (q->base | (arg & 0xFFFFFC));
            break;
        case GE_RET:
            if (sp > 0) q->list = stack[--sp];
            break;
        case GE_BJUMP:
            /* Conditional on the bounding-box test, which needs geometry we do
             * not process. Not taking it means we walk the enclosed commands
             * rather than skipping them -- the conservative direction, since
             * skipping would under-report what the game drew. */
            break;

        case GE_END:
        case GE_FINISH:
            if (cmd == GE_FINISH) g_ge.finishes++;
            q->done = 1;
            return;

        case GE_SIGNAL:
            /* Raises a callback on hardware. Callbacks are not delivered yet
             * (no scheduler), so this is recorded and ignored. */
            break;

        case GE_BASE:        q->base = (arg & 0xFF0000) << 8; break;
        case GE_ORIGIN_ADDR: q->origin = q->list - 4; break;
        case GE_OFFSET_ADDR: q->base = arg << 8; break;

        case GE_VTYPE: g_ge.vtype = arg; break;

        /* ---- transform state ----------------------------------------------
         *
         * NUMBER sets the write cursor, DATA advances it. Writes past the end
         * are dropped: a list can be read while the CPU is still writing it,
         * and wrapping the cursor would scribble over elements already set. */
        case GE_WORLDMATRIXNUMBER:
            if (drawlog_left()) fprintf(stderr, "mtx: WORLD NUMBER arg=%06X -> %d\n",
                                        arg, (int)(arg & 0xF));
            g_tl.world_n = (int)(arg & 0xF); break;
        case GE_VIEWMATRIXNUMBER:  g_tl.view_n  = (int)(arg & 0xF); break;
        case GE_PROJMATRIXNUMBER:  g_tl.proj_n  = (int)(arg & 0x1F); break;
        case GE_WORLDMATRIXDATA:
            if (drawlog_left()) fprintf(stderr, "mtx: WORLD DATA  arg=%06X -> [%d] = %.2f\n",
                                        arg, g_tl.world_n, ge_float(arg));
            if (g_tl.world_n < 12) g_tl.world[g_tl.world_n++] = ge_float(arg);
            g_tl.world_words++;
            break;
        case GE_VIEWMATRIXDATA:
            if (g_tl.view_n < 12) g_tl.view[g_tl.view_n++] = ge_float(arg);
            g_tl.view_words++;
            break;
        case GE_PROJMATRIXDATA:
            if (g_tl.proj_n < 16) g_tl.proj[g_tl.proj_n++] = ge_float(arg);
            g_tl.proj_words++;
            break;

        case GE_VIEWPORTXSCALE:  g_tl.vp_xs = ge_float(arg); g_tl.vp_set = 1; break;
        case GE_VIEWPORTYSCALE:  g_tl.vp_ys = ge_float(arg); g_tl.vp_set = 1; break;
        case GE_VIEWPORTZSCALE:  g_tl.vp_zs = ge_float(arg); break;
        case GE_VIEWPORTXCENTER: g_tl.vp_xc = ge_float(arg); break;
        case GE_VIEWPORTYCENTER: g_tl.vp_yc = ge_float(arg); break;
        case GE_VIEWPORTZCENTER: g_tl.vp_zc = ge_float(arg); break;

        /* Offsets are in sixteenths of a pixel: sceGuOffset sends x << 4. */
        case GE_OFFSETX: g_tl.off_x = (float)(arg & 0xFFFFFu) / 16.0f; break;
        case GE_OFFSETY: g_tl.off_y = (float)(arg & 0xFFFFFu) / 16.0f; break;

        case GE_MASKRGB:
        case GE_MASKALPHA:
            if (drawlog_left())
                fprintf(stderr, "msk: %s arg=%06X\n",
                        cmd == GE_MASKRGB ? "MASKRGB  " : "MASKALPHA", arg);
            break;
        case GE_CULLFACEENABLE: g_tl.cull_enable = (int)(arg & 1); break;
        case GE_CULL:           g_tl.cull_ccw    = (int)(arg & 1); break;
        /* ---- texture state -------------------------------------------------
         *
         * Recorded, not yet sampled. What the sampler has to support is a
         * question about this game rather than about the hardware, and the
         * cheapest way to answer it is to watch which formats and sizes it
         * actually sets -- the PSP offers eleven pixel formats, four palette
         * formats and a swizzle, and building all of that before knowing which
         * are used is how a rasterizer ends up mostly untested code. */
        /* Texture and palette addresses arrive in two registers, and the
         * second one carries the high *nibble* -- bits 24..27 -- in its own
         * bits 16..19, not in its low byte. Both addresses are 16-byte
         * aligned, so the low four bits of the base are not part of it
         * either. Taking the low byte instead reads the palette from a
         * completely different place, which leaves texel *indices* right and
         * every colour wrong: legible shapes, speckled everywhere. */
        case GE_TEXADDR0:
            g_ge.tex_addr = (g_ge.tex_addr & 0x0F000000u) | (arg & 0x00FFFFF0u);
            break;
        case GE_TEXBUFWIDTH0:
            g_ge.tex_stride = arg & 0x7FF;
            g_ge.tex_addr   = (g_ge.tex_addr & 0x00FFFFF0u) | ((arg << 8) & 0x0F000000u);
            break;
        case GE_TEXSIZE0:
            /* log2 of each dimension, four bits each. Masking a whole byte
             * lets a stray high bit ask for a 1 << 200 texture. */
            g_ge.tex_w = 1u << (arg & 0xF);
            g_ge.tex_h = 1u << ((arg >> 8) & 0xF);
            break;
        case GE_TEXFORMAT:
            g_ge.tex_format = arg & 0xF;
            g_ge.tex_formats_seen |= 1u << (arg & 0xF);
            break;
        case GE_TEXMODE:
            g_ge.tex_swizzled = arg & 1;        /* bit 0 selects swizzled */
            break;
        case GE_CLUTFORMAT:
            /* The low two bits are the palette format; the rest is how a texel
             * indexes it -- shift, mask and a start offset. Keeping only the
             * format, which is what this did, samples entry (texel & 0xFF) of
             * whichever palette page happens to be first. */
            g_ge.clut_raw    = arg;
            g_ge.clut_format = arg & 3;
            break;
        case GE_CLUTADDR:
            g_ge.clut_addr = (g_ge.clut_addr & 0x0F000000u) | (arg & 0x00FFFFF0u);
            break;
        case GE_CLUTADDRUPPER:
            g_ge.clut_addr = (g_ge.clut_addr & 0x00FFFFF0u) | ((arg << 8) & 0x0F000000u);
            break;
        case GE_TEXFUNC:
            g_ge.tex_func = arg & 7;
            g_ge.tex_funcs_seen |= 1u << (arg & 7);
            break;
        case GE_TEXFILTER:
            g_ge.tex_filter = arg & 0xFFFF;
            break;
        case GE_TEXWRAP:
            g_ge.tex_wrap = arg & 0xFFFF;
            break;
        case GE_LOADCLUT:
            g_ge.clut_loads++;
            break;

        case GE_TEXTUREMAPENABLE: g_ge.tex_enable = arg & 1; break;

        case GE_ALPHABLENDENABLE: g_tl.blend.enable     = (int)(arg & 1); break;
        case GE_ALPHATESTENABLE:  g_tl.blend.alpha_test = (int)(arg & 1); break;
        case GE_BLENDMODE:
            g_tl.blend.src = (int)(arg & 0xF);
            g_tl.blend.dst = (int)((arg >> 4) & 0xF);
            g_tl.blend.eq  = (int)((arg >> 8) & 7);
            break;
        case GE_BLENDFIXEDA: g_tl.blend.fixa = arg & 0xFFFFFFu; break;
        case GE_BLENDFIXEDB: g_tl.blend.fixb = arg & 0xFFFFFFu; break;
        case GE_ALPHATEST:
            g_tl.blend.alpha_func = (int)(arg & 7);
            g_tl.blend.alpha_ref  = (int)((arg >> 8) & 0xFF);
            g_tl.blend.alpha_mask = (int)((arg >> 16) & 0xFF);
            break;
        case GE_ZTESTENABLE:   g_tl.ztest_enable = (int)(arg & 1); break;
        case GE_ZTEST:         g_tl.ztest_func   = (int)(arg & 7); break;
        case GE_ZWRITEDISABLE: g_tl.zwrite_off   = (int)(arg & 1); break;
        /* Clear mode turns the draw into a blit of the clear values: the depth
         * test is bypassed and depth is written only when the Z bit is set. */
        case GE_CLEARMODE:
            if (drawlog_left())
                fprintf(stderr, "clr: CLEARMODE arg=%06X  enable %d  colour %d "
                                "alpha %d  depth %d\n",
                        arg, (int)(arg & 1), (int)((arg >> 8) & 1),
                        (int)((arg >> 9) & 1), (int)((arg >> 10) & 1));
            g_tl.clear_mode   = (int)(arg & 1);
            g_tl.clear_colour = (int)((arg >> 8) & 1);
            g_tl.clear_z      = (int)((arg >> 10) & 1);
            break;

        case GE_TRANSFERSRC:    g_ge.xfer_src    = arg; break;
        case GE_TRANSFERSRCW:   g_ge.xfer_srcw   = arg; break;
        case GE_TRANSFERDST:    g_ge.xfer_dst    = arg; break;
        case GE_TRANSFERDSTW:   g_ge.xfer_dstw   = arg; break;
        case GE_TRANSFERSRCPOS: g_ge.xfer_srcpos = arg; break;
        case GE_TRANSFERDSTPOS: g_ge.xfer_dstpos = arg; break;
        case GE_TRANSFERSIZE:   g_ge.xfer_size   = arg; break;
        case GE_TRANSFERSTART:
            g_ge.xfer_start = arg;
            do_block_transfer();
            break;

        case GE_FBP:
            g_ge.fbp = (g_ge.fbp & 0xFF000000u) | arg;
            psp_render_current()->set_target(ge_fb_address(g_ge.fbp), g_ge.fbw, 0);
            break;
        case GE_FBW:
            g_ge.fbw = arg & 0xFFFF;
            g_ge.fbp = (g_ge.fbp & 0x00FFFFFFu) | ((arg & 0xFF0000) << 8);
            psp_render_current()->set_target(ge_fb_address(g_ge.fbp), g_ge.fbw, 0);
            break;

        case GE_VADDR: g_ge.vaddr = (q->base | (arg & 0xFFFFFF)); break;
        case GE_IADDR: break;

        default:
            /* A real state command we do not decode individually. Counted --
             * and, for the first few, named. "240 commands not individually
             * decoded" hides whether the game is configuring a draw or just
             * poking state, which is the difference between a rendering bug
             * and a game that has not asked to render yet. */
            if (g_ge.unknown < 64)
                fprintf(stderr, "  GE cmd 0x%02X arg 0x%06X\n", cmd, arg);
            g_ge.unknown++;
            break;
        }
    }
}

/* ---- the calls ----------------------------------------------------------- */

static ge_queue *find_queue(uint32_t id) {
    for (int i = 0; i < MAX_QUEUES; i++)
        if (g_queue[i].used && g_queue[i].id == id) return &g_queue[i];
    return NULL;
}

static void enqueue(int head) {
    /* (list, stall, cbid, arg) */
    ge_queue *q = NULL;
    for (int i = 0; i < MAX_QUEUES; i++) if (!g_queue[i].used) { q = &g_queue[i]; break; }

    /* A slot is only worth keeping while its list can still be referred to. A
     * finished list is kept so that a late sceGeListUpdateStallAddr can still
     * resolve its id, but it is holding a slot it no longer needs -- so when
     * the pool is full, the oldest finished list is what to give up. Ids come
     * from a monotonic counter and are never reused, so a stale id resolves to
     * "unknown uid" rather than to somebody else's list.
     *
     * Without this nothing ever clears `used` and the pool is consumed once,
     * for the life of the process. The game gets MAX_QUEUES display lists in
     * total and every enqueue after that is refused with NO_MEMORY: measured
     * here at 8 accepted and 204 refused during the intro. That is invisible
     * from the outside -- the GE summary counts lists that *ran*, the call
     * histogram is a top-N, and a non-zero error escapes the zero-return ring
     * -- so it reads as "the game submits no geometry" when the truth is that
     * the geometry was submitted and turned away. */
    if (!q)
        for (int i = 0; i < MAX_QUEUES; i++)
            if (g_queue[i].done && (!q || g_queue[i].id < q->id)) q = &g_queue[i];

    if (!q) { psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }

    memset(q, 0, sizeof *q);
    q->id    = g_next_id++;
    q->list  = psp_arg(0) & ~3u;
    q->stall = psp_arg(1) & ~3u;
    q->used  = 1;
    (void)head;

    /* Hardware runs the list asynchronously. We run it here and finish before
     * returning, which is indistinguishable from the game's point of view
     * because every way it can observe progress -- ListSync, DrawSync -- then
     * reports completion. */
    run_list(q);
    psp_ret(q->id);
}

static void hle_ListEnQueue(void)     { enqueue(0); }
static void hle_ListEnQueueHead(void) { enqueue(1); }

static void hle_ListUpdateStallAddr(void) {
    ge_queue *q = find_queue(psp_arg(0));
    if (!q) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_UID); return; }
    q->stall = psp_arg(1) & ~3u;
    if (!q->done) run_list(q);          /* the new stall released more commands */
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* Lists are complete by the time they are enqueued, so every sync succeeds
 * immediately. */
static void hle_ListSync(void) { psp_ret(SCE_KERNEL_ERROR_OK); }
static void hle_DrawSync(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

static void hle_Break(void)    { psp_ret(SCE_KERNEL_ERROR_OK); }
static void hle_Continue(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

static void hle_SetCallback(void)   { psp_ret(0); }
static void hle_UnsetCallback(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

/* eDRAM is the GPU-visible VRAM window: 2 MB at 0x04000000. */
static void hle_EdramGetAddr(void) { psp_ret(PSP_VRAM_BASE); }
static void hle_EdramGetSize(void) { psp_ret(PSP_VRAM_SIZE); }

void psp_ge_register(void) {
    psp_hle_register(0xAB49E76A, "sceGe_user", "sceGeListEnQueue",         hle_ListEnQueue);
    psp_hle_register(0x1C0D95A6, "sceGe_user", "sceGeListEnQueueHead",     hle_ListEnQueueHead);
    psp_hle_register(0xE0D68148, "sceGe_user", "sceGeListUpdateStallAddr", hle_ListUpdateStallAddr);
    psp_hle_register(0x03444EB4, "sceGe_user", "sceGeListSync",            hle_ListSync);
    psp_hle_register(0xB287BD61, "sceGe_user", "sceGeDrawSync",            hle_DrawSync);
    psp_hle_register(0xB448EC0D, "sceGe_user", "sceGeBreak",               hle_Break);
    psp_hle_register(0x4C06E472, "sceGe_user", "sceGeContinue",            hle_Continue);
    psp_hle_register(0xA4FC06A4, "sceGe_user", "sceGeSetCallback",         hle_SetCallback);
    psp_hle_register(0x05DB22CE, "sceGe_user", "sceGeUnsetCallback",       hle_UnsetCallback);
    psp_hle_register(0xE47E40E4, "sceGe_user", "sceGeEdramGetAddr",        hle_EdramGetAddr);
    psp_hle_register(0x1F6752AD, "sceGe_user", "sceGeEdramGetSize",        hle_EdramGetSize);
}
