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
#define GE_TEXSIZE0     0xB8
#define GE_TEXMODE      0xC2
#define GE_TEXFORMAT    0xC3
#define GE_LOADCLUT     0xC4
#define GE_CLUTFORMAT   0xC5
#define GE_TEXFILTER    0xC6
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

static uint64_t g_unsupported;     /* vertices in a format we do not read */

/* Tracked state, and the counters that make the report worth reading. */
static struct {
    uint32_t fbp, fbw, vtype, vaddr;
    /* Texture state, recorded so the sampler can be built against what this
     * game uses rather than against the whole hardware surface. */
    uint32_t tex_addr, tex_stride, tex_w, tex_h, tex_enable;
    uint32_t tex_format, tex_func, tex_filter, tex_swizzled;
    uint32_t clut_addr, clut_format;
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

void psp_ge_reset(void) {
    memset(g_queue, 0, sizeof g_queue);
    memset(&g_ge, 0, sizeof g_ge);
    psp_render_reset_pixels();
    g_unsupported = 0;
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
    fprintf(out, "    pixels written: %llu\n", (unsigned long long)psp_render_pixels());
    if (g_unsupported)
        fprintf(out, "    %llu vertices in an unsupported format (transformed, or no position)\n",
                (unsigned long long)g_unsupported);
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
    if (col_off >= 0 && VT_COLOR(vtype) == 7)
        out->rgba = psp_read32(addr + (uint32_t)col_off);

    switch (VT_POS(vtype)) {
    case 2:   /* 16-bit */
        out->x = (int16_t)psp_read16(addr + (uint32_t)pos_off);
        out->y = (int16_t)psp_read16(addr + (uint32_t)pos_off + 2);
        return 1;
    case 3: { /* float */
        out->x = (int)psp_read_f32(addr + (uint32_t)pos_off);
        out->y = (int)psp_read_f32(addr + (uint32_t)pos_off + 4);
        return 1;
    }
    default:
        return 0;
    }
}


static void draw_prim(uint32_t type, uint32_t count) {
    /* The sampler is told the current texture at draw time rather than on every
     * state command: the GE sets these fields in any order, and only their
     * value at the draw matters. */

    if (!VT_THROUGH(g_ge.vtype)) { g_unsupported += count; return; }
    if (!g_ge.vaddr) { g_unsupported += count; return; }

    int col_off = -1, pos_off = 0, tex_off = -1;
    int stride = vertex_layout(g_ge.vtype, &col_off, &pos_off, &tex_off);
    if (!stride) { g_unsupported += count; return; }

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
    psp_render_current()->set_texture(
        has_uv ? g_ge.tex_addr : 0,
        g_ge.tex_stride, (int)g_ge.tex_w, (int)g_ge.tex_h,
        (int)g_ge.tex_format, (int)g_ge.tex_func, (int)g_ge.tex_swizzled);

    /* Decode the whole batch, then hand it to the backend in one call.
     *
     * Format decoding stays here rather than in each backend: the stride
     * arithmetic and component alignment are fiddly, and duplicating them per
     * backend means every backend is wrong in its own way. Wrong once,
     * centrally, is at least diagnosable. */
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
        /* ---- texture state -------------------------------------------------
         *
         * Recorded, not yet sampled. What the sampler has to support is a
         * question about this game rather than about the hardware, and the
         * cheapest way to answer it is to watch which formats and sizes it
         * actually sets -- the PSP offers eleven pixel formats, four palette
         * formats and a swizzle, and building all of that before knowing which
         * are used is how a rasterizer ends up mostly untested code. */
        case GE_TEXADDR0:
            g_ge.tex_addr = (g_ge.tex_addr & 0xFF000000u) | (arg & 0xFFFFFFu);
            break;
        case GE_TEXBUFWIDTH0:
            g_ge.tex_stride = arg & 0xFFFF;
            g_ge.tex_addr   = (g_ge.tex_addr & 0x00FFFFFFu) | ((arg & 0xFF0000u) << 8);
            break;
        case GE_TEXSIZE0:
            /* log2 of each dimension, width in the low byte. */
            g_ge.tex_w = 1u << (arg & 0xFF);
            g_ge.tex_h = 1u << ((arg >> 8) & 0xFF);
            break;
        case GE_TEXFORMAT:
            g_ge.tex_format = arg & 0xF;
            g_ge.tex_formats_seen |= 1u << (arg & 0xF);
            break;
        case GE_TEXMODE:
            g_ge.tex_swizzled = arg & 1;        /* bit 0 selects swizzled */
            break;
        case GE_CLUTFORMAT:
            g_ge.clut_format = arg & 3;
            break;
        case GE_CLUTADDR:
            g_ge.clut_addr = (g_ge.clut_addr & 0xFF000000u) | (arg & 0xFFFFFFu);
            break;
        case GE_TEXFUNC:
            g_ge.tex_func = arg & 7;
            g_ge.tex_funcs_seen |= 1u << (arg & 7);
            break;
        case GE_TEXFILTER:
            g_ge.tex_filter = arg & 0xFFFF;
            break;
        case GE_LOADCLUT:
            g_ge.clut_loads++;
            break;

        case GE_TEXTUREMAPENABLE: g_ge.tex_enable = arg & 1; break;

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
