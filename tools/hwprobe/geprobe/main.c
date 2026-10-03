/* geprobe -- reference frames from a real PSP's GE.
 *
 * Written against PSPSDK (BSD) only. Nothing here needs input: it draws a
 * series of small test scenes, saves the framebuffer after each one as a raw
 * file beside the EBOOT, logs a checksum and a few sample pixels, and returns
 * to the XMB. The same PRX runs under psprecomp
 * (`allegrexrecomp interp geprobe.prx --dispatch`), whose software renderer
 * draws into the same guest VRAM, so each scene can be compared pixel for
 * pixel.
 *
 * The scenes aim at what src/hle/ge.c does not implement yet -- dithering,
 * skinning, Bezier and spline patches, bounding-box jumps, GE callbacks -- and
 * at the features it does implement from pspautotests captures (texture
 * functions, filtering, fog, lighting, blending, clipping), so both get a
 * hardware reference from this project's own PSP. Scenes 25 on (version 5)
 * each isolate one rule the earlier scenes left open, scenes 34 on
 * (version 6) what geprobe 5 left open in turn, scenes 43 on (version 7,
 * after the callback steps) what geprobe 6 left open, scenes 50 on
 * (version 8) the two rules geprobe 7 could not settle: the gradient
 * reciprocal and the spline weights, scenes 53 and 54 (version 9) the
 * spline weights again, densely, scenes 55 to 58 (version 10) the
 * spline weights through depth and texture, and the 3D depth anchor, and
 * scenes 59 to 61 (version 11) a vertex's depth, a stage at a time,
 * scenes 62 to 66 (version 12) each of those stages read whole,
 * scenes 67 to 82 (version 13) Bezier and spline patch positions read
 * through depth, replayed from patch13.py's command streams, scenes 83 to
 * 98 (version 14) how colour runs across a triangle, replayed from
 * colour14.py's, scenes 99 to 109 (version 15) how it runs along a line,
 * replayed the same way from lines15.py's, scenes 110 to 114 (version 16)
 * skinned and morphed positions read whole through depth, from skin16.py's,
 * scenes 115 to 119 (version 17) point and spot lights: L.D searched
 * whole through the spot cutoff, and the lighting factors as bytes, from
 * lights17.py's, scene 120 (version 18) the lighting's 1/sqrt at every
 * input the same way, from rsq18.py's, scenes 121 to 124 (version 19)
 * lighting under real world and view matrices, from lights19.py's,
 * scenes 125 to 127 (version 20) where a long edge's pixels fall, from
 * edges20.py's, and scenes 128 to 133 (version 21) tall long edges that
 * lean little or share the height, from edges21.py's, and the order a
 * patch's lines go out in, from plines21.py's.
 *
 * Every raw file is 480 x 272 pixels, rows packed (no stride padding), in the
 * scene's framebuffer format: 4 bytes per pixel for 8888, 2 for the 16-bit
 * formats, little-endian, exactly as the GE wrote VRAM. */
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspgum.h>

#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include "probe.h"

PSP_MODULE_INFO("geprobe", PSP_MODULE_USER, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(8192);

#define PROBE_VERSION 22

typedef unsigned int w32;   /* PSPSDK's u32 is uint32_t, a long here, which %X does not take */

#define SCR_W 480
#define SCR_H 272
#define FB_W  512
#define ZBP   ((void *)0x88000)          /* VRAM offset: after a 512x272x4 colour buffer */
#define VRAM_UNCACHED 0x44000000u

static unsigned int *g_list;             /* from the heap, so it sits in main RAM */
static unsigned char *g_dump;            /* 480x272x4 */
static int g_scene;

static w32 crc32(const void *p, int n) {
    const unsigned char *b = p;
    w32 c = 0xFFFFFFFFu;
    for (int i = 0; i < n; i++) {
        c ^= b[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

/* Copy vertex or index data into the display list, so the GE reads it from
 * where libgu put the list (uncached main RAM) whatever the probe's own
 * sections are linked at. */
static void *gumem(const void *src, int size) {
    void *p = sceGuGetMemory((size + 15) & ~15);
    memcpy(p, src, size);
    return p;
}

/* ---- scene framing ------------------------------------------------------- */

/* Set while a scene is built from several lists: scene_begin then leaves the
 * buffers as the previous list left them. */
static int g_keep;

static void scene_begin(int psm, w32 clear) {
    sceGuStart(GU_DIRECT, g_list);
    sceGuDrawBuffer(psm, (void *)0, FB_W);
    sceGuDepthBuffer(ZBP, FB_W);
    sceGuOffset(2048 - SCR_W / 2, 2048 - SCR_H / 2);
    sceGuViewport(2048, 2048, SCR_W, SCR_H);
    sceGuDepthRange(65535, 0);
    sceGuScissor(0, 0, SCR_W, SCR_H);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDisable(GU_LIGHTING);
    sceGuDisable(GU_FOG);
    sceGuDisable(GU_DITHER);
    sceGuDisable(GU_CULL_FACE);
    sceGuDisable(GU_CLIP_PLANES);
    sceGuDisable(GU_STENCIL_TEST);
    sceGuDisable(GU_COLOR_TEST);
    sceGuDisable(GU_COLOR_LOGIC_OP);
    sceGuDisable(GU_LIGHT0); sceGuDisable(GU_LIGHT1);
    sceGuDisable(GU_LIGHT2); sceGuDisable(GU_LIGHT3);
    sceGuShadeModel(GU_SMOOTH);
    sceGuPixelMask(0);
    sceGuDepthMask(GU_FALSE);
    sceGuFrontFace(GU_CW);
    sceGuClearColor(clear);
    sceGuClearDepth(0);
    sceGuClearStencil(0);
    if (!g_keep)
        sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT | GU_STENCIL_BUFFER_BIT);

    sceGumMatrixMode(GU_PROJECTION);
    sceGumLoadIdentity();
    sceGumPerspective(60.0f, 480.0f / 272.0f, 1.0f, 100.0f);
    sceGumMatrixMode(GU_VIEW);
    sceGumLoadIdentity();
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
    sceGumMatrixMode(GU_TEXTURE);
    sceGumLoadIdentity();
    sceGumMatrixMode(GU_MODEL);
    /* sceGum* only edits libgu's copies; this sends them to the GE. Version 1
     * left it out, so every 3D scene drew nothing on either side. */
    sceGumUpdateMatrix();
}

/* Sample points, the same for every scene: a coarse grid plus a few spots
 * where edges, gradients and patches land. */
static const short SAMPLES[][2] = {
    { 10, 10 }, { 60, 30 }, { 120, 60 }, { 180, 90 }, { 240, 136 }, { 300, 150 },
    { 360, 200 }, { 420, 240 }, { 470, 262 }, { 30, 200 }, { 100, 250 }, { 450, 20 },
    { 64, 64 }, { 65, 64 }, { 64, 65 }, { 200, 100 }, { 201, 101 }, { 280, 40 },
};
#define NSAMPLES ((int)(sizeof SAMPLES / sizeof SAMPLES[0]))

/* Wait for the GE to finish the list, for at most a second. geprobe 2's
 * bounding-box scene never came back on firmware 6.60 (the log stops in it),
 * so a list that is still running after a second is broken off with
 * sceGeBreak(1), which resets the queue, and the probe says so and carries on.
 * Returns what the GE answered: sceGeDrawSync(0) once idle, or the break's
 * result with bit 31 clear and 0x10000000 set when it had to break. */
static int ge_wait(void) {
    for (int ms = 0; ms < 1000; ms++) {
        if (sceGeDrawSync(1) == PSP_GE_LIST_DONE)
            return sceGeDrawSync(0);
        sceKernelDelayThread(1000);
    }
    int peek = sceGeDrawSync(1);
    int b = sceGeBreak(1, NULL);
    out("  GE still busy after 1 s: sceGeDrawSync(peek) = %08X; sceGeBreak(1) %s\n",
        (unsigned)peek, b >= 0 ? "ok" : "error");
    if (b < 0) ret(b);
    return 0x10000000 | (b & 0x0FFFFFFF);
}

static void scene_end(const char *name, int psm, int dump_depth) {
    sceGuFinish();
    int r = ge_wait();
    const int bpp = (psm == GU_PSM_8888) ? 4 : 2;
    const unsigned char *vram = (const unsigned char *)VRAM_UNCACHED;
    for (int y = 0; y < SCR_H; y++)
        memcpy(g_dump + y * SCR_W * bpp, vram + y * FB_W * bpp, SCR_W * bpp);
    const int size = SCR_W * SCR_H * bpp;
    char file[48];
    snprintf(file, sizeof file, "ge_%02d_%s.raw", g_scene, name);
    int wr = probe_write_file(file, g_dump, size);
    out("  sync %08X; %s: %d bytes, psm %d, crc %08X\n", (unsigned)r, file, wr, psm,
        crc32(g_dump, size));
    out("  samples:");
    for (int i = 0; i < NSAMPLES; i++) {
        const unsigned char *p = g_dump + (SAMPLES[i][1] * SCR_W + SAMPLES[i][0]) * bpp;
        if (bpp == 4) out(" %08X", (w32)(p[0] | p[1] << 8 | p[2] << 16 | (w32)p[3] << 24));
        else          out(" %04X", (w32)(p[0] | p[1] << 8));
    }
    out("\n");
    if (dump_depth) {
        /* Read through the plain VRAM address; the hardware's own layout,
         * whatever it is, is the observation. 16 bits per pixel. */
        const unsigned char *z = vram + (w32)ZBP;
        for (int y = 0; y < SCR_H; y++)
            memcpy(g_dump + y * SCR_W * 2, z + y * FB_W * 2, SCR_W * 2);
        snprintf(file, sizeof file, "ge_%02d_%s_depth.raw", g_scene, name);
        wr = probe_write_file(file, g_dump, SCR_W * SCR_H * 2);
        out("  %s: %d bytes, crc %08X\n", file, wr, crc32(g_dump, SCR_W * SCR_H * 2));
    }
}

/* ---- vertex formats ------------------------------------------------------ */

typedef struct { w32 color; float x, y, z; } CV;                       /* colour + position */
typedef struct { float u, v; w32 color; float x, y, z; } TCV;          /* + texture */
typedef struct { w32 color; float nx, ny, nz; float x, y, z; } CNV;    /* + normal */
typedef struct { float w[2]; w32 color; float x, y, z; } W2V;          /* 2 weights */

#define FMT_CV2D  (GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D)
#define FMT_TCV2D (GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D)
#define FMT_CV3D  (GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D)
#define FMT_TCV3D (GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D)
#define FMT_CNV3D (GU_COLOR_8888 | GU_NORMAL_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_3D)

static void rect2d(float x0, float y0, float x1, float y1, w32 c) {
    CV v[2] = { { c, x0, y0, 0 }, { c, x1, y1, 0 } };
    sceGuDrawArray(GU_SPRITES, FMT_CV2D, 2, NULL, gumem(v, sizeof v));
}

static void tri2d(float x0, float y0, w32 c0, float x1, float y1, w32 c1,
                  float x2, float y2, w32 c2) {
    CV v[3] = { { c0, x0, y0, 0 }, { c1, x1, y1, 0 }, { c2, x2, y2, 0 } };
    sceGuDrawArray(GU_TRIANGLES, FMT_CV2D, 3, NULL, gumem(v, sizeof v));
}

/* A horizontal gradient strip made of two triangles, left colour to right. */
static void grad2d(float x0, float y0, float x1, float y1, w32 cl, w32 cr) {
    CV v[6] = { { cl, x0, y0, 0 }, { cr, x1, y0, 0 }, { cl, x0, y1, 0 },
                { cr, x1, y0, 0 }, { cr, x1, y1, 0 }, { cl, x0, y1, 0 } };
    sceGuDrawArray(GU_TRIANGLES, FMT_CV2D, 6, NULL, gumem(v, sizeof v));
}

/* ---- textures ------------------------------------------------------------ */

static w32 *g_tex8888;      /* 16x16 */
static unsigned char *g_texT8;   /* 16x16 indices */
static w32 *g_clut;         /* 256 entries 8888 */
static unsigned short *g_clut16; /* 256 entries 5650 */

static void make_textures(void) {
    g_tex8888 = memalign(16, 16 * 16 * 4);
    g_texT8   = memalign(16, 16 * 16);
    g_clut    = memalign(16, 256 * 4);
    g_clut16  = memalign(16, 256 * 2);
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            const w32 r = x * 17, g = y * 17, b = ((x ^ y) & 1) ? 255 : 0;
            const w32 a = (x + y) * 8;
            g_tex8888[y * 16 + x] = r | g << 8 | b << 16 | a << 24;
            g_texT8[y * 16 + x] = (unsigned char)(y * 16 + x);
        }
    for (int i = 0; i < 256; i++) {
        g_clut[i] = (w32)i | (w32)(255 - i) << 8 | (w32)((i * 7) & 255) << 16 | (w32)(i ^ 0xA5) << 24;
        g_clut16[i] = (unsigned short)(((i >> 3) << 11) | (((255 - i) >> 2) << 5) | ((i * 7 & 255) >> 3));
    }
    sceKernelDcacheWritebackAll();
}

static void tex8888(int filter_min, int filter_mag) {
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexImage(0, 16, 16, 16, g_tex8888);
    sceGuTexFilter(filter_min, filter_mag);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuTexFlush();
}

static void tsprite(float x0, float y0, float x1, float y1, float u0, float v0,
                    float u1, float v1, w32 c) {
    TCV v[2] = { { u0, v0, c, x0, y0, 0 }, { u1, v1, c, x1, y1, 0 } };
    sceGuDrawArray(GU_SPRITES, FMT_TCV2D, 2, NULL, gumem(v, sizeof v));
}

/* ---- scenes -------------------------------------------------------------- */

static void geometry_2d(void) {
    /* Gouraud triangles in both windings, a sliver, sub-pixel ones, and
     * vertices on half-pixel positions: where the edges land. */
    tri2d(20, 20, 0xFF0000FF, 200, 30, 0xFF00FF00, 60, 180, 0xFFFF0000);
    tri2d(220, 20, 0xFF0000FF, 260, 180, 0xFFFF0000, 460, 30, 0xFF00FF00);
    tri2d(20.5f, 200.5f, 0xFFFFFFFF, 200.5f, 201.25f, 0xFFFFFFFF, 20.5f, 202.75f, 0xFFFFFFFF);
    tri2d(300.25f, 200.25f, 0xFF00FFFF, 300.75f, 200.25f, 0xFF00FFFF, 300.25f, 200.75f, 0xFF00FFFF);
    tri2d(310.5f, 200.5f, 0xFF00FFFF, 312.5f, 200.5f, 0xFF00FFFF, 310.5f, 202.5f, 0xFF00FFFF);
    tri2d(320, 200, 0xFFFF00FF, 330.999f, 200, 0xFFFF00FF, 320, 210.999f, 0xFFFF00FF);
    rect2d(340, 200, 350, 210, 0xFF808080);
    rect2d(360.5f, 200.5f, 370.5f, 210.5f, 0xFF40C040);
    rect2d(390, 210, 380, 200, 0xFFC04040);     /* corners given right-to-left */
    /* A flat-shaded triangle: the provoking vertex's colour. */
    sceGuShadeModel(GU_FLAT);
    tri2d(20, 230, 0xFF0000FF, 120, 230, 0xFF00FF00, 70, 265, 0xFFFF0000);
    sceGuShadeModel(GU_SMOOTH);
    /* Lines and points. */
    CV l[6] = { { 0xFFFFFFFF, 140, 230, 0 }, { 0xFF0000FF, 230, 265, 0 },
                { 0xFFFFFFFF, 140, 265, 0 }, { 0xFF00FF00, 230, 231, 0 },
                { 0xFFFFFF00, 240, 230, 0 }, { 0xFFFFFF00, 240.5f, 265.5f, 0 } };
    sceGuDrawArray(GU_LINES, FMT_CV2D, 6, NULL, gumem(l, sizeof l));
    CV p[4] = { { 0xFFFFFFFF, 250, 240, 0 }, { 0xFFFFFFFF, 252.5f, 240.5f, 0 },
                { 0xFFFFFFFF, 254.99f, 240, 0 }, { 0xFFFFFFFF, 256, 240.99f, 0 } };
    sceGuDrawArray(GU_POINTS, FMT_CV2D, 4, NULL, gumem(p, sizeof p));
    /* A long thin gradient, to show colour interpolation precision. */
    grad2d(260, 250, 470, 268, 0xFF000000, 0xFFFFFFFF);
}

static void scene_geometry(int psm, const char *name) {
    if (step("scene %02d: %s", g_scene, name)) return;
    scene_begin(psm, 0xFF201008);
    geometry_2d();
    scene_end(name, psm, 0);
}

/* Dithering. Gradients in every channel, first unlit, then through the
 * dither matrix. Two matrices: an ordinary one, and one of extremes that
 * shows the scale and sign the hardware applies. */
static const ScePspIMatrix4 DITHER_BAYER = {
    { -4, 0, -3, 1 }, { 2, -2, 3, -1 }, { -3, 1, -4, 0 }, { 3, -1, 2, -2 } };
static const ScePspIMatrix4 DITHER_EXTREME = {
    { 7, -8, 7, -8 }, { -8, 7, -8, 7 }, { 0, 1, 2, 3 }, { -1, -2, -3, -4 } };

static void dither_body(void) {
    grad2d(10, 10, 470, 40, 0xFF000000, 0xFF0000FF);      /* red */
    grad2d(10, 50, 470, 80, 0xFF000000, 0xFF00FF00);      /* green */
    grad2d(10, 90, 470, 120, 0xFF000000, 0xFFFF0000);     /* blue */
    grad2d(10, 130, 470, 160, 0xFF000000, 0xFFFFFFFF);    /* grey */
    grad2d(10, 170, 470, 200, 0x00FFFFFF, 0xFFFFFFFF);    /* alpha only */
    rect2d(10, 210, 240, 240, 0xFF7F7F7F);                /* one flat mid value */
    rect2d(240, 210, 470, 240, 0xFF010101);               /* near black */
    rect2d(10, 240, 240, 268, 0xFFFEFEFE);                /* near white */
    rect2d(240, 240, 470, 268, 0x80838383);
}

static void scene_dither(int psm, const char *name, const ScePspIMatrix4 *m, int on) {
    if (step("scene %02d: %s", g_scene, name)) return;
    scene_begin(psm, 0x00000000);
    if (m) sceGuSetDither(m);
    if (on) sceGuEnable(GU_DITHER); else sceGuDisable(GU_DITHER);
    dither_body();
    scene_end(name, psm, 0);
}

/* Blending: bars of a source colour over a background gradient, one bar per
 * equation and factor pair. */
static void scene_blend(void) {
    if (step("scene %02d: blend", g_scene)) return;
    scene_begin(GU_PSM_8888, 0xFF000000);
    grad2d(0, 0, 480, 272, 0x20FF4000, 0xE00040FF);
    sceGuEnable(GU_BLEND);
    static const struct { int op, src, dst; w32 sfix, dfix; } B[] = {
        { GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0 },
        { GU_ADD, GU_FIX, GU_FIX, 0x00808080, 0x00808080 },
        { GU_ADD, GU_FIX, GU_FIX, 0x00FFFFFF, 0x00FFFFFF },
        { GU_ADD, GU_SRC_COLOR, GU_DST_COLOR, 0, 0 },
        { GU_ADD, GU_DST_ALPHA, GU_ONE_MINUS_DST_ALPHA, 0, 0 },
        { GU_ADD, GU_DOUBLE_SRC_ALPHA, GU_ONE_MINUS_DOUBLE_SRC_ALPHA, 0, 0 },
        { GU_ADD, GU_DOUBLE_DST_ALPHA, GU_FIX, 0, 0x00404040 },
        { GU_SUBTRACT, GU_FIX, GU_FIX, 0x00FFFFFF, 0x00808080 },
        { GU_REVERSE_SUBTRACT, GU_SRC_ALPHA, GU_FIX, 0, 0x00FFFFFF },
        { GU_MIN, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0 },
        { GU_MAX, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0 },
        { GU_ABS, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0 },
    };
    const int n = (int)(sizeof B / sizeof B[0]);
    for (int i = 0; i < n; i++) {
        sceGuBlendFunc(B[i].op, B[i].src, B[i].dst, B[i].sfix, B[i].dfix);
        const float y0 = 4 + i * 22, y1 = y0 + 18;
        rect2d(10, y0, 160, y1, 0x80C08040);
        rect2d(160, y0, 310, y1, 0x00FFFFFF);
        grad2d(310, y0, 470, y1, 0x00FF00FF, 0xFF00FF00);
    }
    scene_end("blend", GU_PSM_8888, 0);
}

/* Texture filtering: a 16x16 texture magnified with nearest and linear, at
 * whole and half-texel offsets, and minified. */
static void scene_filter(void) {
    if (step("scene %02d: texture filtering", g_scene)) return;
    scene_begin(GU_PSM_8888, 0xFF000000);
    tex8888(GU_NEAREST, GU_NEAREST);
    tsprite(8, 8, 136, 136, 0, 0, 16, 16, 0xFFFFFFFF);
    tex8888(GU_LINEAR, GU_LINEAR);
    tsprite(144, 8, 272, 136, 0, 0, 16, 16, 0xFFFFFFFF);
    tsprite(280, 8, 408, 136, 0.5f, 0.5f, 16.5f, 16.5f, 0xFFFFFFFF);
    tsprite(8, 144, 136, 272, 3.25f, 3.75f, 7.25f, 7.75f, 0xFFFFFFFF);
    tsprite(144, 144, 152, 152, 0, 0, 16, 16, 0xFFFFFFFF);   /* minified 2:1 */
    tsprite(160, 144, 164, 148, 0, 0, 16, 16, 0xFFFFFFFF);   /* 4:1 */
    sceGuTexWrap(GU_REPEAT, GU_REPEAT);
    tsprite(176, 144, 304, 272, -8, -8, 24, 24, 0xFFFFFFFF); /* repeat */
    tex8888(GU_NEAREST, GU_NEAREST);
    sceGuTexWrap(GU_CLAMP, GU_REPEAT);
    tsprite(312, 144, 440, 272, -8, -8, 24, 24, 0xFFFFFFFF);
    tsprite(416, 8, 472, 64, 15.9f, 0, 16.1f, 16, 0xFFFFFFFF);
    scene_end("filter", GU_PSM_8888, 0);
}

/* Texture functions, with and without texture alpha, a vertex colour, the
 * environment colour, and colour doubling (bit 16 of TFUNC, command 0xC9). */
static void scene_texfunc(void) {
    if (step("scene %02d: texture functions", g_scene)) return;
    scene_begin(GU_PSM_8888, 0xFF303030);
    tex8888(GU_NEAREST, GU_NEAREST);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuTexEnvColor(0x0040C0FF);
    for (int f = 0; f < 5; f++)
        for (int tcc = 0; tcc < 2; tcc++)
            for (int dbl = 0; dbl < 2; dbl++) {
                sceGuSendCommandi(0xC9, f | tcc << 8 | dbl << 16);
                const float x = 8 + (tcc * 2 + dbl) * 116, y = 8 + f * 52;
                tsprite(x, y, x + 48, y + 48, 0, 0, 16, 16, 0xC08040FF);
                tsprite(x + 52, y, x + 100, y + 48, 0, 0, 16, 16, 0x40FFFFFF);
            }
    scene_end("texfunc", GU_PSM_8888, 0);
}

/* CLUT textures: T8 through an 8888 and a 5650 palette, with shift, mask and
 * start offset. */
static void scene_clut(void) {
    if (step("scene %02d: CLUT", g_scene)) return;
    scene_begin(GU_PSM_8888, 0xFF000000);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_T8, 0, 0, 0);
    sceGuTexImage(0, 16, 16, 16, g_texT8);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    static const struct { int psm; unsigned shift, mask, csa; } C[] = {
        { GU_PSM_8888, 0, 0xFF, 0 }, { GU_PSM_8888, 4, 0x0F, 0 }, { GU_PSM_8888, 0, 0x3F, 1 },
        { GU_PSM_8888, 1, 0xFF, 0 }, { GU_PSM_5650, 0, 0xFF, 0 }, { GU_PSM_5650, 2, 0x7F, 2 },
    };
    for (int i = 0; i < 6; i++) {
        sceGuClutMode(C[i].psm, C[i].shift, C[i].mask, C[i].csa);
        sceGuClutLoad(C[i].psm == GU_PSM_8888 ? 32 : 16,
                      C[i].psm == GU_PSM_8888 ? (void *)g_clut : (void *)g_clut16);
        sceGuTexFlush();
        const float x = 8 + (i % 3) * 156, y = 8 + (i / 3) * 132;
        tsprite(x, y, x + 128, y + 128, 0, 0, 16, 16, 0xFFFFFFFF);
    }
    scene_end("clut", GU_PSM_8888, 0);
}

static void quad3d(float x0, float y0, float x1, float y1, float z, w32 c0, w32 c1) {
    CV v[6] = { { c0, x0, y0, z }, { c1, x1, y0, z }, { c0, x0, y1, z },
                { c1, x1, y0, z }, { c1, x1, y1, z }, { c0, x0, y1, z } };
    sceGuDrawArray(GU_TRIANGLES, FMT_CV3D, 6, NULL, gumem(v, sizeof v));
}

/* Fog: quads at stepped depths, one fog range, and a slanted floor. */
static void scene_fog(void) {
    if (step("scene %02d: fog", g_scene)) return;
    scene_begin(GU_PSM_8888, 0xFF000000);
    sceGuEnable(GU_FOG);
    sceGuFog(2.0f, 20.0f, 0x00FF8040);
    for (int i = 0; i < 8; i++) {
        const float z = -1.5f - i * 3.0f, s = -z * 0.08f;
        quad3d(-6 * s + i * 1.5f * s, -2 * s, -5 * s + i * 1.5f * s, 2 * s, z, 0xFFFFFFFF, 0xFF4080C0);
    }
    CV floorv[6] = {
        { 0xFFFFFFFF, -4, -1.5f, -1.5f }, { 0xFFFFFFFF, 4, -1.5f, -1.5f }, { 0xFF00FF00, -4, -1.5f, -40 },
        { 0xFFFFFFFF, 4, -1.5f, -1.5f }, { 0xFF00FF00, 4, -1.5f, -40 }, { 0xFF00FF00, -4, -1.5f, -40 } };
    sceGuDrawArray(GU_TRIANGLES, FMT_CV3D, 6, NULL, gumem(floorv, sizeof floorv));
    scene_end("fog", GU_PSM_8888, 0);
}

/* Lighting: a faceted fan of normals under a directional light, a point light
 * with attenuation and a spot light, with ambient, diffuse and specular. */
static void lit_fan(float cx, float cy, float z) {
    CNV v[3 * 12];
    int k = 0;
    for (int i = 0; i < 12; i++) {
        const float a0 = i * 0.5236f, a1 = (i + 1) * 0.5236f;
        float nx0 = 0, ny0 = 0;
        /* Normals tilt outward with the fan, so each facet catches the light
         * at a different angle. */
        const float c0 = (float)(i - 6) / 6.0f;
        nx0 = c0; ny0 = (float)((i * 5) % 7 - 3) / 3.0f;
        const float nz0 = 1.0f;
        const float x0 = cx + 1.2f * __builtin_cosf(a0), y0 = cy + 1.2f * __builtin_sinf(a0);
        const float x1 = cx + 1.2f * __builtin_cosf(a1), y1 = cy + 1.2f * __builtin_sinf(a1);
        v[k++] = (CNV){ 0xFFFFFFFF, 0, 0, 1, cx, cy, z };
        v[k++] = (CNV){ 0xFFFFFFFF, nx0, ny0, nz0, x0, y0, z };
        v[k++] = (CNV){ 0xFFFFFFFF, nx0, ny0, nz0, x1, y1, z };
    }
    sceGuDrawArray(GU_TRIANGLES, FMT_CNV3D, k, NULL, gumem(v, sizeof v));
}

static void scene_light(void) {
    if (step("scene %02d: lighting", g_scene)) return;
    scene_begin(GU_PSM_8888, 0xFF000000);
    sceGuEnable(GU_LIGHTING);
    sceGuAmbient(0xFF202020);
    sceGuColorMaterial(GU_AMBIENT | GU_DIFFUSE | GU_SPECULAR);
    sceGuSpecular(12.0f);
    sceGuLightMode(GU_SEPARATE_SPECULAR_COLOR);

    ScePspFVector3 dir = { 0.3f, 0.5f, 1.0f };
    sceGuEnable(GU_LIGHT0);
    sceGuLight(0, GU_DIRECTIONAL, GU_DIFFUSE_AND_SPECULAR, &dir);
    sceGuLightColor(0, GU_DIFFUSE, 0xFF80C0FF);
    sceGuLightColor(0, GU_SPECULAR, 0xFFFFFFFF);
    lit_fan(-2.6f, 0.9f, -6.0f);
    sceGuDisable(GU_LIGHT0);

    ScePspFVector3 pos = { 0.0f, 0.9f, -4.5f };
    sceGuEnable(GU_LIGHT1);
    sceGuLight(1, GU_POINTLIGHT, GU_DIFFUSE_AND_SPECULAR, &pos);
    sceGuLightColor(1, GU_DIFFUSE, 0xFF40FF40);
    sceGuLightColor(1, GU_SPECULAR, 0xFFFFFFFF);
    sceGuLightAtt(1, 0.5f, 0.3f, 0.1f);
    lit_fan(0.0f, 0.9f, -6.0f);
    sceGuDisable(GU_LIGHT1);

    ScePspFVector3 spos = { 2.6f, 0.9f, -3.0f }, sdir = { 0.0f, 0.0f, -1.0f };
    sceGuEnable(GU_LIGHT2);
    sceGuLight(2, GU_SPOTLIGHT, GU_DIFFUSE, &spos);
    sceGuLightColor(2, GU_DIFFUSE, 0xFF4040FF);
    sceGuLightAtt(2, 1.0f, 0.0f, 0.0f);
    sceGuLightSpot(2, &sdir, 4.0f, 0.9f);
    lit_fan(2.6f, 0.9f, -6.0f);
    sceGuDisable(GU_LIGHT2);

    sceGuLightMode(GU_SINGLE_COLOR);
    sceGuEnable(GU_LIGHT0);
    sceGuLight(0, GU_DIRECTIONAL, GU_AMBIENT_AND_DIFFUSE, &dir);
    sceGuLightColor(0, GU_AMBIENT, 0xFF101010);
    sceGuLightColor(0, GU_DIFFUSE, 0xFFFFC080);
    sceGuColorMaterial(0);
    sceGuModelColor(0xFF000040, 0xFF404040, 0xFF8080FF, 0xFFFFFFFF);
    lit_fan(-1.3f, -1.4f, -6.0f);
    sceGuColorMaterial(GU_DIFFUSE);
    lit_fan(1.3f, -1.4f, -6.0f);
    scene_end("light", GU_PSM_8888, 0);
}

/* Depth: interpenetrating triangles, each depth function, and the depth
 * buffer itself. */
static void scene_depth(void) {
    if (step("scene %02d: depth", g_scene)) return;
    scene_begin(GU_PSM_8888, 0xFF000000);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuDepthMask(GU_FALSE);                 /* writes on */
    sceGuDepthFunc(GU_GEQUAL);
    CV a[3] = { { 0xFF0000FF, -3, -1.5f, -4 }, { 0xFF0000FF, 1, -1.5f, -8 }, { 0xFF0000FF, -1, 1.5f, -6 } };
    CV b[3] = { { 0xFF00FF00, -3, -1.0f, -8 }, { 0xFF00FF00, 1, -1.0f, -4 }, { 0xFF00FF00, -1, 1.8f, -6 } };
    sceGuDrawArray(GU_TRIANGLES, FMT_CV3D, 3, NULL, gumem(a, sizeof a));
    sceGuDrawArray(GU_TRIANGLES, FMT_CV3D, 3, NULL, gumem(b, sizeof b));
    static const int F[] = { GU_NEVER, GU_ALWAYS, GU_EQUAL, GU_NOTEQUAL, GU_LESS, GU_LEQUAL, GU_GREATER, GU_GEQUAL };
    for (int i = 0; i < 8; i++) {
        sceGuDepthFunc(GU_ALWAYS);
        quad3d(1.2f + (i % 4) * 0.7f, -1.6f + (i / 4) * 0.8f, 1.8f + (i % 4) * 0.7f, -1.0f + (i / 4) * 0.8f,
               -5.0f, 0xFF808080, 0xFF808080);
        sceGuDepthFunc(F[i]);
        quad3d(1.3f + (i % 4) * 0.7f, -1.5f + (i / 4) * 0.8f, 1.9f + (i % 4) * 0.7f, -0.9f + (i / 4) * 0.8f,
               -5.0f, 0xFFFF00FF, 0xFFFFFF00);
    }
    /* Beyond the far plane and in front of the near plane, with depth clamp
     * implied by the range. */
    sceGuDepthFunc(GU_ALWAYS);
    quad3d(-3.5f, 1.2f, -2.5f, 2.0f, -150.0f, 0xFFFFFFFF, 0xFFFFFFFF);
    scene_end("depth", GU_PSM_8888, 1);
}

/* Clipping: triangles crossing the near plane, the screen edges and far past
 * them, with and without clip planes enabled. */
static void scene_clip(void) {
    if (step("scene %02d: clipping", g_scene)) return;
    scene_begin(GU_PSM_8888, 0xFF101010);
    for (int cp = 0; cp < 2; cp++) {
        if (cp) sceGuEnable(GU_CLIP_PLANES); else sceGuDisable(GU_CLIP_PLANES);
        const float ox = cp ? 1.6f : -1.6f;
        CV nearv[3] = { { 0xFF0000FF, ox - 1.0f, -1.0f, -3 }, { 0xFF00FF00, ox + 1.0f, -1.0f, -3 },
                        { 0xFFFF0000, ox, 0.2f, 0.5f } };
        sceGuDrawArray(GU_TRIANGLES, FMT_CV3D, 3, NULL, gumem(nearv, sizeof nearv));
        CV wide[3] = { { 0xFF00FFFF, ox - 40.0f, 0.4f, -5 }, { 0xFFFF00FF, ox + 0.5f, 0.4f, -5 },
                       { 0xFFFFFF00, ox, 1.8f, -5 } };
        sceGuDrawArray(GU_TRIANGLES, FMT_CV3D, 3, NULL, gumem(wide, sizeof wide));
    }
    /* 2D vertices outside the 4096 guard band and the screen. */
    tri2d(-100, 200, 0xFF4040FF, 200, 150, 0xFF40FF40, 100, 600, 0xFFFF4040);
    tri2d(300, 150, 0xFFFFFFFF, 5000, 160, 0xFF000000, 320, 260, 0xFF808080);
    scene_end("clip", GU_PSM_8888, 0);
}

/* Alpha test, colour test, stencil, logic ops and the pixel mask. */
static void scene_tests(void) {
    if (step("scene %02d: alpha/colour/stencil/logic/mask", g_scene)) return;
    scene_begin(GU_PSM_8888, 0x80402010);
    sceGuEnable(GU_ALPHA_TEST);
    sceGuAlphaFunc(GU_GREATER, 0x80, 0xFF);
    grad2d(10, 10, 470, 40, 0x00FFFFFF, 0xFFFFFFFF);
    sceGuAlphaFunc(GU_EQUAL, 0x40, 0xF0);
    grad2d(10, 44, 470, 74, 0x00FFFFFF, 0xFFFFFFFF);
    sceGuDisable(GU_ALPHA_TEST);

    sceGuEnable(GU_COLOR_TEST);
    sceGuColorFunc(GU_NOTEQUAL, 0x00808080, 0x00F0F0F0);
    grad2d(10, 78, 470, 108, 0xFF000000, 0xFFFFFFFF);
    sceGuDisable(GU_COLOR_TEST);

    sceGuEnable(GU_STENCIL_TEST);
    sceGuStencilFunc(GU_ALWAYS, 0x55, 0xFF);
    sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE);
    rect2d(10, 112, 240, 142, 0xFF0000FF);
    sceGuStencilOp(GU_KEEP, GU_KEEP, GU_INCR);
    rect2d(120, 112, 350, 142, 0xFF00FF00);
    sceGuStencilOp(GU_KEEP, GU_KEEP, GU_INVERT);
    rect2d(300, 112, 470, 142, 0xFFFF0000);
    /* Half over the band (stencil 0x55, 0x56, 0xAA, ...) and half below it
     * (stencil 0 from the clear). Version 1 drew it wholly below the band. */
    sceGuStencilFunc(GU_EQUAL, 0x56, 0xFF);
    sceGuStencilOp(GU_KEEP, GU_KEEP, GU_KEEP);
    rect2d(10, 127, 470, 157, 0xFFFFFFFF);
    sceGuDisable(GU_STENCIL_TEST);

    sceGuEnable(GU_COLOR_LOGIC_OP);
    static const int L[] = { GU_CLEAR, GU_AND, GU_XOR, GU_OR, GU_NOR, GU_EQUIV, GU_INVERTED, GU_NAND };
    for (int i = 0; i < 8; i++) {
        sceGuLogicalOp(L[i]);
        rect2d(10 + i * 57, 180, 62 + i * 57, 210, 0xC3A55A3C);
    }
    sceGuDisable(GU_COLOR_LOGIC_OP);

    sceGuPixelMask(0xFF00F0F0);
    rect2d(10, 214, 470, 240, 0x7FFFFFFF);
    sceGuPixelMask(0);
    rect2d(10, 244, 470, 268, 0x00000000);
    scene_end("tests", GU_PSM_8888, 0);
}

/* Skinning: two bone matrices, vertices weighted between them. */
static void scene_skin(void) {
    if (step("scene %02d: skinning", g_scene)) return;
    scene_begin(GU_PSM_8888, 0xFF000000);
    ScePspFMatrix4 b0, b1, b2;
    memset(&b0, 0, sizeof b0); memset(&b1, 0, sizeof b1); memset(&b2, 0, sizeof b2);
    b0.x.x = b0.y.y = b0.z.z = b0.w.w = 1.0f;
    b1 = b0; b1.w.x = 1.5f; b1.w.y = 0.8f;                       /* translate */
    b2 = b0; b2.x.x = 0.0f; b2.x.y = 1.0f; b2.y.x = -1.0f; b2.y.y = 0.0f;   /* rotate 90 */
    sceGuBoneMatrix(0, &b0);
    sceGuBoneMatrix(1, &b1);
    sceGuBoneMatrix(2, &b2);
    static const float W[][2] = { { 1, 0 }, { 0.75f, 0.25f }, { 0.5f, 0.5f }, { 0, 1 }, { 1.5f, -0.5f } };
    for (int i = 0; i < 5; i++) {
        const float x = -2.6f + i * 1.0f;
        W2V v[3] = { { { W[i][0], W[i][1] }, 0xFF0000FF, x, -1.2f, -5 },
                     { { W[i][0], W[i][1] }, 0xFF00FF00, x + 0.8f, -1.2f, -5 },
                     { { W[i][0], W[i][1] }, 0xFFFF0000, x + 0.4f, -0.4f, -5 } };
        sceGuDrawArray(GU_TRIANGLES, GU_WEIGHTS(2) | GU_WEIGHT_32BITF | FMT_CV3D, 3, NULL, gumem(v, sizeof v));
    }
    /* 8-bit weights: 0x80 is the unit weight on this format. */
    typedef struct { unsigned char w[2]; unsigned char pad[2]; w32 color; float x, y, z; } W2B;
    static const unsigned char WB[][2] = { { 0x80, 0 }, { 0x40, 0x40 }, { 0, 0x80 }, { 0xFF, 0 } };
    for (int i = 0; i < 4; i++) {
        const float x = -2.6f + i * 1.2f;
        W2B v[3] = { { { WB[i][0], WB[i][1] }, { 0, 0 }, 0xFFFFFF00, x, 0.2f, -5 },
                     { { WB[i][0], WB[i][1] }, { 0, 0 }, 0xFF00FFFF, x + 0.8f, 0.2f, -5 },
                     { { WB[i][0], WB[i][1] }, { 0, 0 }, 0xFFFF00FF, x + 0.4f, 1.0f, -5 } };
        sceGuDrawArray(GU_TRIANGLES, GU_WEIGHTS(2) | GU_WEIGHT_8BIT | FMT_CV3D, 3, NULL, gumem(v, sizeof v));
    }
    /* Three weights, rotation bone included. */
    typedef struct { float w[3]; w32 color; float x, y, z; } W3V;
    W3V v3[3] = { { { 0.2f, 0.3f, 0.5f }, 0xFFFFFFFF, 1.5f, 1.2f, -5 },
                  { { 0.2f, 0.3f, 0.5f }, 0xFF808080, 2.3f, 1.2f, -5 },
                  { { 0.2f, 0.3f, 0.5f }, 0xFF404040, 1.9f, 1.9f, -5 } };
    sceGuDrawArray(GU_TRIANGLES, GU_WEIGHTS(3) | GU_WEIGHT_32BITF | FMT_CV3D, 3, NULL, gumem(v3, sizeof v3));
    scene_end("skin", GU_PSM_8888, 0);
}

/* Morphing: two vertex sets blended by the morph weights. */
static void scene_morph(void) {
    if (step("scene %02d: morph", g_scene)) return;
    scene_begin(GU_PSM_8888, 0xFF000000);
    static const float MW[][2] = { { 1, 0 }, { 0.5f, 0.5f }, { 0, 1 }, { 0.25f, 1.0f } };
    for (int i = 0; i < 4; i++) {
        sceGuMorphWeight(0, MW[i][0]);
        sceGuMorphWeight(1, MW[i][1]);
        const float x = -2.6f + i * 1.4f;
        CV v[6] = {
            { 0xFF0000FF, x, -1, -5 },        { 0xFF00FF00, x + 0.6f, 0.5f, -5 },
            { 0xFF00FF00, x + 1, -1, -5 },    { 0xFFFF0000, x + 1.2f, -0.5f, -5 },
            { 0xFFFF0000, x + 0.5f, 1, -5 },  { 0xFF0000FF, x - 0.2f, 1.6f, -5 } };
        sceGuDrawArray(GU_TRIANGLES, GU_VERTICES(2) | FMT_CV3D, 3, NULL, gumem(v, sizeof v));
    }
    scene_end("morph", GU_PSM_8888, 0);
}

/* Bezier and spline patches over one 4x4 (and one 5x4) control grid. */
static void patch_grid(CV *v, int nu, int nv, float cx, float cy, float s) {
    for (int j = 0; j < nv; j++)
        for (int i = 0; i < nu; i++) {
            const float x = cx + (i - (nu - 1) / 2.0f) * s, y = cy + (j - (nv - 1) / 2.0f) * s;
            const float z = -5.0f + (((i + j) & 1) ? 0.6f : -0.6f) + ((i == 1 || i == 2) && (j == 1 || j == 2) ? 1.2f : 0.0f);
            const w32 c = 0xFF000000u | (w32)(i * 255 / (nu - 1)) | (w32)(j * 255 / (nv - 1)) << 8 |
                          (w32)(((i + j) & 1) ? 255 : 64) << 16;
            v[j * nu + i] = (CV){ c, x, y, z };
        }
}

static void scene_bezier(void) {
    if (step("scene %02d: Bezier patches", g_scene)) return;
    scene_begin(GU_PSM_8888, 0xFF000000);
    CV g[16];
    static const struct { int du, dv, prim; float cx, cy; } P[] = {
        { 4, 4, GU_TRIANGLE_STRIP, -2.4f, 1.0f }, { 16, 16, GU_TRIANGLE_STRIP, 0.0f, 1.0f },
        { 3, 7, GU_TRIANGLE_STRIP, 2.4f, 1.0f },  { 8, 8, GU_LINE_STRIP, -2.4f, -1.2f },
        { 8, 8, GU_POINTS, 0.0f, -1.2f },
    };
    for (int i = 0; i < 5; i++) {
        patch_grid(g, 4, 4, P[i].cx, P[i].cy, 0.55f);
        sceGuPatchDivide(P[i].du, P[i].dv);
        sceGuPatchPrim(P[i].prim);
        sceGuDrawBezier(FMT_CV3D, 4, 4, NULL, gumem(g, sizeof g));
    }
    /* Textured with no UVs in the vertex: the GE generates them. */
    tex8888(GU_NEAREST, GU_NEAREST);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGB);
    sceGuPatchPrim(GU_TRIANGLE_STRIP);
    sceGuPatchDivide(8, 8);
    patch_grid(g, 4, 4, 2.4f, -1.2f, 0.55f);
    sceGuDrawBezier(FMT_CV3D, 4, 4, NULL, gumem(g, sizeof g));
    sceGuDisable(GU_TEXTURE_2D);
    scene_end("bezier", GU_PSM_8888, 0);
}

static void scene_spline(void) {
    if (step("scene %02d: spline patches", g_scene)) return;
    scene_begin(GU_PSM_8888, 0xFF000000);
    CV g[20];
    static const struct { int ue, ve; float cx, cy; } E[] = {
        { GU_FILL_FILL, GU_FILL_FILL, -2.4f, 1.0f }, { GU_OPEN_FILL, GU_FILL_FILL, 0.0f, 1.0f },
        { GU_FILL_OPEN, GU_FILL_FILL, 2.4f, 1.0f },  { GU_OPEN_OPEN, GU_OPEN_OPEN, -2.4f, -1.2f },
    };
    sceGuPatchPrim(GU_TRIANGLE_STRIP);
    sceGuPatchDivide(8, 8);
    for (int i = 0; i < 4; i++) {
        patch_grid(g, 4, 4, E[i].cx, E[i].cy, 0.55f);
        sceGuDrawSpline(FMT_CV3D, 4, 4, E[i].ue, E[i].ve, NULL, gumem(g, 16 * sizeof(CV)));
    }
    patch_grid(g, 5, 4, 0.0f, -1.2f, 0.5f);
    sceGuDrawSpline(FMT_CV3D, 5, 4, GU_FILL_FILL, GU_FILL_FILL, NULL, gumem(g, 20 * sizeof(CV)));
    sceGuPatchPrim(GU_LINE_STRIP);
    patch_grid(g, 5, 4, 2.4f, -1.2f, 0.5f);
    sceGuDrawSpline(FMT_CV3D, 5, 4, GU_OPEN_OPEN, GU_FILL_FILL, NULL, gumem(g, 20 * sizeof(CV)));
    scene_end("spline", GU_PSM_8888, 0);
}

/* Bounding-box jumps: each object draws a marker sprite at a fixed screen spot
 * if the GE decides its box is visible.
 *
 * Both vertex blocks are put in the list before sceGuBeginObject. That call
 * writes BASE 0 and BJUMP 0 as a placeholder that sceGuEndObject patches with
 * the real target, and libgu's sceGuDrawArray leaves the stall address alone
 * inside an object, but sceGuGetMemory does not: called in between, it lets
 * the GE past the placeholder, and a box the GE finds invisible then jumps to
 * address 0. geprobe 2 did that (rect2d inside the object) and firmware 6.60
 * switched itself off; version 1 got away with it because, without matrices,
 * every box was visible. */
static void bbox_object(float cx, float cy, float cz, float h, int slot, w32 color) {
    typedef struct { float x, y, z; } P3;
    P3 box[8];
    for (int i = 0; i < 8; i++)
        box[i] = (P3){ cx + ((i & 1) ? h : -h), cy + ((i & 2) ? h : -h), cz + ((i & 4) ? h : -h) };
    CV mark[2] = { { color, 10 + slot * 58, 220, 0 }, { color, 60 + slot * 58, 262, 0 } };
    void *bv = gumem(box, sizeof box);
    void *mv = gumem(mark, sizeof mark);
    sceGuBeginObject(GU_VERTEX_32BITF | GU_TRANSFORM_3D, 8, NULL, bv);
    sceGuDrawArray(GU_SPRITES, FMT_CV2D, 2, NULL, mv);
    sceGuEndObject();
}

/* One list per object, each its own step, so a box that goes wrong is named
 * and a restart skips only that box. The
 * marker's centre pixel says whether the GE drew it (the box was visible). */
static void scene_bbox(void) {
    static const struct { float cx, cy, cz, h; w32 color; const char *what; } B[8] = {
        { 0.0f,  0.0f,   -5.0f, 0.5f, 0xFFFFFFFF, "in view" },
        { 20.0f, 0.0f,   -5.0f, 0.5f, 0xFF0000FF, "far right" },
        { 0.0f,  0.0f,    5.0f, 0.5f, 0xFF00FF00, "behind the camera" },
        { 3.2f,  0.0f,   -5.0f, 0.6f, 0xFFFF0000, "straddling the right edge" },
        { 0.0f,  0.0f,   -1.0f, 0.4f, 0xFF00FFFF, "straddling the near plane" },
        { 0.0f,  0.0f, -200.0f, 0.5f, 0xFFFF00FF, "beyond the far plane" },
        { 0.0f,  30.0f,  -5.0f, 0.5f, 0xFFFFFF00, "far above" },
        { 0.0f,  2.0f,   -5.0f, 0.3f, 0xFF808080, "just above the top edge" },
    };
    if (step("scene %02d: bounding-box jumps: clear", g_scene)) return;
    scene_begin(GU_PSM_8888, 0xFF000000);
    sceGuFinish();
    out("  wait %08X\n", (unsigned)ge_wait());
    g_keep = 1;
    for (int i = 0; i < 8; i++) {
        if (step("scene %02d: bounding box %d, %s", g_scene, i, B[i].what)) continue;
        scene_begin(GU_PSM_8888, 0xFF000000);
        bbox_object(B[i].cx, B[i].cy, B[i].cz, B[i].h, i, B[i].color);
        sceGuFinish();
        int r = ge_wait();
        const w32 *px = (const w32 *)VRAM_UNCACHED + 241 * FB_W + 35 + i * 58;
        out("  wait %08X; marker %s (pixel %08X)\n", (unsigned)r,
            (*px & 0xFFFFFF) == (B[i].color & 0xFFFFFF) ? "drawn" : "not drawn", *px);
    }
    if (!step("scene %02d: bounding-box jumps: dump", g_scene)) {
        scene_begin(GU_PSM_8888, 0xFF000000);
        scene_end("bbox", GU_PSM_8888, 0);
    }
    g_keep = 0;
}

/* ---- geprobe 5: what scenes 15-24 left open ------------------------------
 *
 * Each scene below isolates one rule that the 3D scenes show only mixed with
 * others (fw660-run4/findings/geprobe.md): the specular and powered-diffuse
 * curves, how precisely patches, morphing and skinning place vertices and
 * colours, depth interpolation, far-off and 16-bit through-mode positions,
 * sprite texel mapping, the logic ops scene 19 left out, the pixel mask on
 * stencil writes and on 16-bit targets, and what a clear does with the mask,
 * the logic op, dither and the tests. */

typedef struct { float nx, ny, nz; float x, y, z; } NV;               /* normal + position */
typedef struct { float x, y, z; } BP3;                                 /* bounding-box corner */
typedef struct { w32 color; short x, y, z, pad; } CV16;                /* colour + 16-bit position */
#define FMT_NV3D   (GU_NORMAL_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_3D)
#define FMT_CV16_2D (GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D)
/* sceGuLight's `components` 8 is light kind 2, powered diffuse: PSPSDK's
 * libgu maps 6 (diffuse and specular) to kind 1, 8 to kind 2, the rest to 0. */
#define LIGHT_POWERED_DIFFUSE 8

/* Unit normals (sin, 0, cos): with the light and the eye both on +Z, N.L and
 * N.H are 1, 0.995, 0.99, 0.98 ... 0.45. The last two are the 0.9 normal
 * scaled by 2 and by 0.5, to see whether the GE normalises. */
static const float SPEC_N[18][3] = {
    { 0.0f, 0.0f, 1.0f },                { 0.0998749211f, 0.0f, 0.995000005f },
    { 0.141067356f, 0.0f, 0.99000001f }, { 0.198997483f, 0.0f, 0.980000019f },
    { 0.24310492f, 0.0f, 0.970000029f }, { 0.280000001f, 0.0f, 0.959999979f },
    { 0.312249899f, 0.0f, 0.949999988f }, { 0.341174453f, 0.0f, 0.939999998f },
    { 0.391918361f, 0.0f, 0.920000017f }, { 0.4358899f, 0.0f, 0.899999976f },
    { 0.493051708f, 0.0f, 0.870000005f }, { 0.557763398f, 0.0f, 0.829999983f },
    { 0.62577951f, 0.0f, 0.779999971f },  { 0.714142859f, 0.0f, 0.699999988f },
    { 0.800000012f, 0.0f, 0.600000024f }, { 0.893028557f, 0.0f, 0.449999988f },
    { 0.87177980f, 0.0f, 1.8f },          { 0.21794495f, 0.0f, 0.45f },
};

static NV g_nv[6 * 18];

static void nquad(NV *v, float x0, float y0, float x1, float y1, float z, const float n[3]) {
    const NV a = { n[0], n[1], n[2], x0, y0, z }, b = { n[0], n[1], n[2], x1, y0, z };
    const NV c = { n[0], n[1], n[2], x0, y1, z }, d = { n[0], n[1], n[2], x1, y1, z };
    v[0] = a; v[1] = b; v[2] = c; v[3] = b; v[4] = d; v[5] = c;
}

/* One row of 18 quads at z = -6, one per normal, top edge at y0. */
static void spec_row(float y0) {
    for (int i = 0; i < 18; i++) {
        const float x0 = -5.9f + i * 0.66f;
        nquad(g_nv + 6 * i, x0, y0, x0 + 0.58f, y0 - 0.38f, -6.0f, SPEC_N[i]);
    }
    sceGuDrawArray(GU_TRIANGLES, FMT_NV3D, 6 * 18, NULL, gumem(g_nv, sizeof g_nv));
}

/* Scene 16's specular reads as if the exponent were about 1.28 times the
 * coefficient, and its diffuse-only fans are one step off in places. Here
 * every quad has one normal, a white light and a white material, so each
 * quad is one flat value: 255 * f(c) for the column's c. Rows 0-7 specular
 * alone with coefficients 1 to 32, rows 8-9 powered diffuse with 4 and 12,
 * row 10 plain diffuse; then two wide quads of one normal each, which a fixed
 * eye direction lights evenly and an eye at the origin does not. */
static void scene_specular(void) {
    if (step("scene %02d: specular and diffuse curves", g_scene)) return;
    scene_begin(GU_PSM_8888, 0xFF000000);
    sceGuEnable(GU_LIGHTING);
    sceGuLightMode(GU_SINGLE_COLOR);
    sceGuAmbient(0xFF000000);
    sceGuColorMaterial(0);
    sceGuModelColor(0x000000, 0x000000, 0xFFFFFF, 0xFFFFFF);
    sceGuAmbientColor(0xFF000000);            /* material ambient black, alpha 0xFF */
    ScePspFVector3 dir = { 0.0f, 0.0f, 1.0f };
    sceGuEnable(GU_LIGHT0);
    sceGuLightColor(0, GU_AMBIENT, 0x000000);

    static const float K[8] = { 1.0f, 2.0f, 4.0f, 8.0f, 12.0f, 16.0f, 24.0f, 32.0f };
    sceGuLight(0, GU_DIRECTIONAL, GU_DIFFUSE_AND_SPECULAR, &dir);
    sceGuLightColor(0, GU_DIFFUSE, 0x000000);
    sceGuLightColor(0, GU_SPECULAR, 0xFFFFFF);
    for (int r = 0; r < 8; r++) {
        sceGuSpecular(K[r]);
        spec_row(3.3f - r * 0.45f);
    }
    sceGuLight(0, GU_DIRECTIONAL, LIGHT_POWERED_DIFFUSE, &dir);
    sceGuLightColor(0, GU_DIFFUSE, 0xFFFFFF);
    sceGuLightColor(0, GU_SPECULAR, 0x000000);
    sceGuSpecular(4.0f);
    spec_row(3.3f - 8 * 0.45f);
    sceGuSpecular(12.0f);
    spec_row(3.3f - 9 * 0.45f);
    sceGuLight(0, GU_DIRECTIONAL, GU_DIFFUSE, &dir);
    spec_row(3.3f - 10 * 0.45f);

    sceGuLight(0, GU_DIRECTIONAL, GU_DIFFUSE_AND_SPECULAR, &dir);
    sceGuLightColor(0, GU_DIFFUSE, 0x000000);
    sceGuLightColor(0, GU_SPECULAR, 0xFFFFFF);
    sceGuSpecular(12.0f);
    nquad(g_nv, -5.9f, -1.9f, 5.9f, -2.5f, -6.0f, SPEC_N[6]);
    nquad(g_nv + 6, -5.9f, -2.65f, 5.9f, -3.25f, -6.0f, SPEC_N[2]);
    sceGuDrawArray(GU_TRIANGLES, FMT_NV3D, 12, NULL, gumem(g_nv, 12 * sizeof(NV)));
    scene_end("specular", GU_PSM_8888, 0);
}

/* Evenly spaced control points on the plane z = -6. Plain (mode 0), their
 * colours are linear in u and v, so the exact surface and its colours are
 * bilinear and every vertex a patch generates has an exact position and
 * colour. Mode 1 makes the four inner control colours white and the rest
 * black instead; mode 2 (geprobe 6) gives them patch_grid's colours, blue
 * alternating between 64 and 255. */
static void flat_grid(CV *v, float cx, float cy, float s, int mode) {
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++) {
            const int inner = (i == 1 || i == 2) && (j == 1 || j == 2);
            const w32 c = mode == 1 ? (inner ? 0xFFFFFFFFu : 0xFF000000u)
                        : mode == 2 ? 0xFF000000u | (w32)(i * 85) | (w32)(j * 85) << 8 |
                                      (w32)(((i + j) & 1) ? 255 : 64) << 16
                                    : 0xFF400000u | (w32)(i * 85) | (w32)(j * 85) << 8;
            v[j * 4 + i] = (CV){ c, cx + (i - 1.5f) * s, cy + (j - 1.5f) * s, -6.0f };
        }
}

/* One triangle at awkward positions and depths, Gouraud red/green/blue. */
static void awkward_tri(CV *v, float x) {
    v[0] = (CV){ 0xFF0000FF, x + 0.123f, -1.537f, -5.9f };
    v[1] = (CV){ 0xFF00FF00, x + 1.271f, -1.611f, -6.3f };
    v[2] = (CV){ 0xFFFF0000, x + 0.577f, -2.29f,  -5.7f };
}

/* Patches (scenes 22-23), morphing (21) and skinning (20) each came out a
 * step of colour or a sixteenth of position off in places. Row 1: Bezier
 * patches over a flat_grid, divisions 1 to 4, and the same grid as an
 * open/open spline, division 4. Row 2: scene 22's curved grid at divisions 1
 * and 2, the white-bump colours at division 4, the curved grid as an
 * open/open spline at division 2. Row 3: one quad per pair of morph weights,
 * both vertex sets in the same place, so each quad is one flat colour, the
 * blend of 0xFF03FE81 and 0xFF7C0100. Row 4: one triangle unskinned, then the
 * same shape skinned through identity bones with one float weight of 1, two
 * of 0.5, and one 8-bit weight of 0x80; then two morphed in position. */
static void scene_precision(void) {
    if (step("scene %02d: patch, morph and skin precision", g_scene)) return;
    scene_begin(GU_PSM_8888, 0xFF000000);
    CV g[16];
    sceGuPatchPrim(GU_TRIANGLE_STRIP);
    for (int i = 0; i < 4; i++) {
        flat_grid(g, -4.8f + i * 2.4f, 2.5f, 0.4f, 0);
        sceGuPatchDivide(i + 1, i + 1);
        sceGuDrawBezier(FMT_CV3D, 4, 4, NULL, gumem(g, sizeof g));
    }
    flat_grid(g, 4.8f, 2.5f, 0.4f, 0);
    sceGuPatchDivide(4, 4);
    sceGuDrawSpline(FMT_CV3D, 4, 4, GU_OPEN_OPEN, GU_OPEN_OPEN, NULL, gumem(g, sizeof g));

    patch_grid(g, 4, 4, -3.6f, 0.5f, 0.4f);
    sceGuPatchDivide(1, 1);
    sceGuDrawBezier(FMT_CV3D, 4, 4, NULL, gumem(g, sizeof g));
    patch_grid(g, 4, 4, -1.2f, 0.5f, 0.4f);
    sceGuPatchDivide(2, 2);
    sceGuDrawBezier(FMT_CV3D, 4, 4, NULL, gumem(g, sizeof g));
    flat_grid(g, 1.2f, 0.5f, 0.4f, 1);
    sceGuPatchDivide(4, 4);
    sceGuDrawBezier(FMT_CV3D, 4, 4, NULL, gumem(g, sizeof g));
    patch_grid(g, 4, 4, 3.6f, 0.5f, 0.4f);
    sceGuPatchDivide(2, 2);
    sceGuDrawSpline(FMT_CV3D, 4, 4, GU_OPEN_OPEN, GU_OPEN_OPEN, NULL, gumem(g, sizeof g));

    static const float MW[10][2] = {
        { 1.0f, 0.0f }, { 0.875f, 0.125f }, { 0.75f, 0.25f }, { 0.625f, 0.375f }, { 0.5f, 0.5f },
        { 0.375f, 0.625f }, { 0.25f, 0.75f }, { 0.125f, 0.875f }, { 0.3f, 0.3f }, { 0.9f, 0.9f } };
    for (int i = 0; i < 10; i++) {
        const float x0 = -5.8f + i * 1.15f, x1 = x0 + 0.95f, y0 = -0.6f, y1 = -1.2f;
        const float P[6][2] = { { x0, y0 }, { x1, y0 }, { x0, y1 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
        CV mv[12];
        for (int k = 0; k < 6; k++) {
            mv[2 * k]     = (CV){ 0xFF03FE81, P[k][0], P[k][1], -6.0f };
            mv[2 * k + 1] = (CV){ 0xFF7C0100, P[k][0], P[k][1], -6.0f };
        }
        sceGuMorphWeight(0, MW[i][0]);
        sceGuMorphWeight(1, MW[i][1]);
        sceGuDrawArray(GU_TRIANGLES, GU_VERTICES(2) | FMT_CV3D, 6, NULL, gumem(mv, sizeof mv));
    }

    ScePspFMatrix4 id;
    memset(&id, 0, sizeof id);
    id.x.x = id.y.y = id.z.z = id.w.w = 1.0f;
    sceGuBoneMatrix(0, &id);
    sceGuBoneMatrix(1, &id);
    CV t[3];
    awkward_tri(t, -5.6f);
    sceGuDrawArray(GU_TRIANGLES, FMT_CV3D, 3, NULL, gumem(t, sizeof t));
    typedef struct { float w; w32 color; float x, y, z; } W1V;
    awkward_tri(t, -3.7f);
    W1V w1[3];
    for (int k = 0; k < 3; k++) w1[k] = (W1V){ 1.0f, t[k].color, t[k].x, t[k].y, t[k].z };
    sceGuDrawArray(GU_TRIANGLES, GU_WEIGHTS(1) | GU_WEIGHT_32BITF | FMT_CV3D, 3, NULL, gumem(w1, sizeof w1));
    awkward_tri(t, -1.8f);
    W2V w2[3];
    for (int k = 0; k < 3; k++) w2[k] = (W2V){ { 0.5f, 0.5f }, t[k].color, t[k].x, t[k].y, t[k].z };
    sceGuDrawArray(GU_TRIANGLES, GU_WEIGHTS(2) | GU_WEIGHT_32BITF | FMT_CV3D, 3, NULL, gumem(w2, sizeof w2));
    typedef struct { unsigned char w, pad[3]; w32 color; float x, y, z; } W1B;
    awkward_tri(t, 0.1f);
    W1B wb[3];
    for (int k = 0; k < 3; k++) wb[k] = (W1B){ 0x80, { 0, 0, 0 }, t[k].color, t[k].x, t[k].y, t[k].z };
    sceGuDrawArray(GU_TRIANGLES, GU_WEIGHTS(1) | GU_WEIGHT_8BIT | FMT_CV3D, 3, NULL, gumem(wb, sizeof wb));
    static const float PW[2][2] = { { 0.5f, 0.5f }, { 0.3f, 0.7f } };
    for (int i = 0; i < 2; i++) {
        awkward_tri(t, 2.0f + i * 1.9f);
        CV mv[6];
        for (int k = 0; k < 3; k++) {
            mv[2 * k] = t[k];
            mv[2 * k + 1] = (CV){ t[k].color, t[k].x - 0.37f, t[k].y + 0.21f, t[k].z };
        }
        sceGuMorphWeight(0, PW[i][0]);
        sceGuMorphWeight(1, PW[i][1]);
        sceGuDrawArray(GU_TRIANGLES, GU_VERTICES(2) | FMT_CV3D, 3, NULL, gumem(mv, sizeof mv));
    }
    scene_end("precision", GU_PSM_8888, 0);
}

/* Scene 17's depth buffer is a step off along some rows. Through mode puts
 * z in the buffer as given, so its triangles show the interpolation alone:
 * a full-range one, a nearly flat one, a constant one and one steep in y;
 * then lines with z. On the right, 3D quads at one depth, sloping in y and
 * sloping in x, and a 3D line. Depth test ALWAYS, writes on; the depth
 * buffer is saved too. */
static void scene_depthplanes(void) {
    if (step("scene %02d: depth interpolation", g_scene)) return;
    scene_begin(GU_PSM_8888, 0xFF000000);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuDepthFunc(GU_ALWAYS);
    sceGuDepthMask(GU_FALSE);                 /* writes on */
    CV t[12] = {
        { 0xFF0000FF, 10, 10, 0 },      { 0xFF0000FF, 230, 10, 65535 },  { 0xFF0000FF, 10, 90, 30000 },
        { 0xFF00FF00, 230, 20, 1000 },  { 0xFF00FF00, 230, 100, 1003 },  { 0xFF00FF00, 20, 100, 1010 },
        { 0xFFFF0000, 10, 110, 12345 }, { 0xFFFF0000, 230, 110, 12345 }, { 0xFFFF0000, 120, 170, 12345 },
        { 0xFFFFFF00, 10, 175, 0 },     { 0xFFFFFF00, 230, 175, 0 },     { 0xFFFFFF00, 120, 200, 65535 } };
    sceGuDrawArray(GU_TRIANGLES, FMT_CV2D, 12, NULL, gumem(t, sizeof t));
    CV l[6] = { { 0xFFFFFFFF, 10, 210, 0 },     { 0xFFFFFFFF, 230, 215, 65535 },
                { 0xFFFFFFFF, 10, 225, 65535 }, { 0xFFFFFFFF, 230, 225, 0 },
                { 0xFFFFFFFF, 230, 235, 0 },    { 0xFFFFFFFF, 10, 262, 40000 } };
    sceGuDrawArray(GU_LINES, FMT_CV2D, 6, NULL, gumem(l, sizeof l));

    quad3d(0.3f, -2.0f, 0.9f, 2.0f, -5.0f, 0xFF0000FF, 0xFF0000FF);
    CV q[12] = {
        { 0xFF00FF00, 1.2f, -1.8f, -4.5f }, { 0xFF00FF00, 1.7f, -1.8f, -4.5f }, { 0xFF00FF00, 1.2f, 1.8f, -5.5f },
        { 0xFF00FF00, 1.7f, -1.8f, -4.5f }, { 0xFF00FF00, 1.7f, 1.8f, -5.5f },  { 0xFF00FF00, 1.2f, 1.8f, -5.5f },
        { 0xFFFF0000, 2.2f, -1.8f, -5.5f }, { 0xFFFF0000, 3.8f, -1.8f, -4.5f }, { 0xFFFF0000, 2.2f, 1.8f, -5.5f },
        { 0xFFFF0000, 3.8f, -1.8f, -4.5f }, { 0xFFFF0000, 3.8f, 1.8f, -4.5f },  { 0xFFFF0000, 2.2f, 1.8f, -5.5f } };
    sceGuDrawArray(GU_TRIANGLES, FMT_CV3D, 12, NULL, gumem(q, sizeof q));
    CV l3[2] = { { 0xFFFFFFFF, 0.4f, -2.35f, -5.0f }, { 0xFFFFFFFF, 3.6f, -2.45f, -6.0f } };
    sceGuDrawArray(GU_LINES, FMT_CV3D, 2, NULL, gumem(l3, sizeof l3));
    scene_end("depthplanes", GU_PSM_8888, 1);
}

/* Positions far off screen and 16-bit through-mode ones, sprites mapping 2
 * texels onto 7 and 70 pixels (flipped too), and texture coordinates along
 * lines. Top band: a through-mode triangle with a vertex at x = -5000 (float)
 * and one at x = 5000; second band: the same two with 16-bit positions.
 * Then 16-bit sprites at x -10, 4100 and -4086 and float ones at 4100 and
 * -10, one row each at the left edge (whether 16-bit positions are signed,
 * and whether either wraps at 4096); a 3D textured line to their right. */
static void scene_positions(void) {
    if (step("scene %02d: far-off and 16-bit positions, 2-texel sprites, line uv", g_scene)) return;
    scene_begin(GU_PSM_8888, 0xFF000000);
    tri2d(-5000, 5, 0xFF0000FF, 230, 60, 0xFF00FF00, 230, 5, 0xFFFF0000);
    tri2d(250, 5, 0xFFFF0000, 250, 60, 0xFF00FF00, 5000, 5, 0xFF0000FF);
    CV16 s[6] = { { 0xFF0000FF, -5000, 65, 0, 0 }, { 0xFF00FF00, 230, 120, 0, 0 }, { 0xFFFF0000, 230, 65, 0, 0 },
                  { 0xFFFF0000, 250, 65, 0, 0 },   { 0xFF00FF00, 250, 120, 0, 0 }, { 0xFF0000FF, 5000, 65, 0, 0 } };
    sceGuDrawArray(GU_TRIANGLES, FMT_CV16_2D, 6, NULL, gumem(s, sizeof s));
    CV16 sp[6] = { { 0xFF0000FF, -10, 125, 0, 0 },   { 0xFF0000FF, 30, 133, 0, 0 },
                   { 0xFF00FF00, 4100, 135, 0, 0 },  { 0xFF00FF00, 4130, 143, 0, 0 },
                   { 0xFFFF0000, -4086, 145, 0, 0 }, { 0xFFFF0000, -4056, 153, 0, 0 } };
    sceGuDrawArray(GU_SPRITES, FMT_CV16_2D, 6, NULL, gumem(sp, sizeof sp));
    rect2d(4100, 155, 4130, 163, 0xFF00FFFF);
    rect2d(-10, 165, 30, 173, 0xFFFF00FF);

    tex8888(GU_NEAREST, GU_NEAREST);
    TCV l3[2] = { { 0, 8, 0xFFFFFFFF, 0.4f, 0.35f, -4.0f }, { 16, 8, 0xFFFFFFFF, 4.2f, -0.9f, -8.0f } };
    sceGuDrawArray(GU_LINES, FMT_TCV3D, 2, NULL, gumem(l3, sizeof l3));
    /* 2 texels (4..6) onto 7 pixels: plain, u flipped, v flipped, both, and
     * the corners given bottom-right first; nearest, then linear. */
    for (int f = 0; f < 2; f++) {
        if (f) tex8888(GU_LINEAR, GU_LINEAR);
        const float y = 180 + f * 10;
        tsprite(10, y, 17, y + 7, 4, 4, 6, 6, 0xFFFFFFFF);
        tsprite(22, y, 29, y + 7, 6, 4, 4, 6, 0xFFFFFFFF);
        tsprite(34, y, 41, y + 7, 4, 6, 6, 4, 0xFFFFFFFF);
        tsprite(46, y, 53, y + 7, 6, 6, 4, 4, 0xFFFFFFFF);
        tsprite(65, y + 7, 58, y, 4, 4, 6, 6, 0xFFFFFFFF);
    }
    /* 2 texels onto 70 pixels: linear, linear flipped, nearest, nearest flipped. */
    tsprite(10, 205, 80, 219, 4, 4, 6, 6, 0xFFFFFFFF);
    tsprite(90, 205, 160, 219, 6, 6, 4, 4, 0xFFFFFFFF);
    tex8888(GU_NEAREST, GU_NEAREST);
    tsprite(170, 205, 240, 219, 4, 4, 6, 6, 0xFFFFFFFF);
    tsprite(250, 205, 320, 219, 6, 6, 4, 4, 0xFFFFFFFF);
    /* Lines with texture coordinates: across, diagonal, right to left. */
    TCV tl[6] = { { 0, 8, 0xFFFFFFFF, 10, 226, 0 },  { 16, 8, 0xFFFFFFFF, 470, 226, 0 },
                  { 0, 0, 0xFFFFFFFF, 10, 232, 0 },  { 16, 16, 0xFFFFFFFF, 470, 268, 0 },
                  { 0, 4, 0xFFFFFFFF, 470, 240, 0 }, { 16, 4, 0xFFFFFFFF, 10, 250, 0 } };
    sceGuDrawArray(GU_LINES, FMT_TCV2D, 6, NULL, gumem(tl, sizeof tl));
    sceGuDisable(GU_TEXTURE_2D);
    scene_end("positions", GU_PSM_8888, 0);
}

/* A clear confined by the scissor to one band, with one state switched on.
 * Returns with the state off again and the scissor full. */
static void clear_band(int y0, int y1, int what, int flags) {
    sceGuScissor(10, y0, 470, y1);
    switch (what) {
    case 0: sceGuPixelMask(0x00FF00FF); break;
    case 1: sceGuEnable(GU_COLOR_LOGIC_OP); sceGuLogicalOp(GU_XOR); break;
    case 2: sceGuSetDither(&DITHER_EXTREME); sceGuEnable(GU_DITHER); break;
    case 3: sceGuEnable(GU_COLOR_TEST); sceGuColorFunc(GU_NOTEQUAL, 0x808080, 0xFFFFFF);
            sceGuEnable(GU_ALPHA_TEST); sceGuAlphaFunc(GU_GREATER, 0x80, 0xFF); break;
    case 4: sceGuPixelMask(0xF0000000); break;
    case 5: sceGuEnable(GU_BLEND); sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0x808080, 0x808080); break;
    }
    sceGuClearColor(0x80808080);
    sceGuClearStencil(0xAB);
    sceGuClear(flags);
    sceGuPixelMask(0);
    sceGuDisable(GU_COLOR_LOGIC_OP);
    sceGuDisable(GU_DITHER);
    sceGuDisable(GU_COLOR_TEST);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_BLEND);
    sceGuScissor(0, 0, SCR_W, SCR_H);
}

/* Six clears over what is there, one band each: pixel mask 0x00FF00FF, logic
 * op XOR, dither (the extreme matrix), colour and alpha tests that the clear
 * colour 0x80808080 fails, pixel mask 0xF0000000 on a stencil clear to 0xAB,
 * and blending. */
static void clear_bands(int y0) {
    for (int i = 0; i < 6; i++)
        clear_band(y0 + i * 13, y0 + i * 13 + 13, i,
                   i == 4 ? GU_STENCIL_BUFFER_BIT : GU_COLOR_BUFFER_BIT);
}

/* Stencil writes under PMSK2: stencil 0x5A everywhere first, then REPLACE
 * 0xAB with the alpha mask 0xF0, INCR with 0x0F and INVERT with 0x3C. */
static void pmsk2_stencil(int y0, int y1) {
    sceGuEnable(GU_STENCIL_TEST);
    sceGuStencilFunc(GU_ALWAYS, 0x5A, 0xFF);
    sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE);
    rect2d(10, y0, 470, y1, 0x00FFFFFF);
    sceGuStencilFunc(GU_ALWAYS, 0xAB, 0xFF);
    sceGuPixelMask(0xF0000000);
    rect2d(10, y0, 160, y1, 0x00FFFFFF);
    sceGuStencilOp(GU_KEEP, GU_KEEP, GU_INCR);
    sceGuPixelMask(0x0F000000);
    rect2d(160, y0, 310, y1, 0x00FFFFFF);
    sceGuStencilOp(GU_KEEP, GU_KEEP, GU_INVERT);
    sceGuPixelMask(0x3C000000);
    rect2d(310, y0, 470, y1, 0x00FFFFFF);
    sceGuPixelMask(0);
    sceGuDisable(GU_STENCIL_TEST);
}

/* Scene 19's follow-ups on 8888: the eight logic ops it left out, over two
 * gradients; its stencil band (y 112-142) again with EQUAL 0x56 and EQUAL
 * 0x55 rectangles wholly inside it (version 2 put the one rectangle half
 * below it); stencil writes under PMSK2; and clears with the mask, logic
 * op, dither, tests and blending on. */
static void scene_tests2(void) {
    if (step("scene %02d: other logic ops, stencil band, PMSK2 with stencil ops, clears", g_scene)) return;
    scene_begin(GU_PSM_8888, 0x80402010);
    grad2d(10, 10, 470, 50, 0xFF000000, 0xFFFFFFFF);
    grad2d(10, 50, 470, 90, 0x00FF8040, 0xFF4080FF);
    sceGuEnable(GU_COLOR_LOGIC_OP);
    static const int L2[] = { GU_AND_REVERSE, GU_COPY, GU_AND_INVERTED, GU_NOOP,
                              GU_OR_REVERSE, GU_COPY_INVERTED, GU_OR_INVERTED, GU_SET };
    for (int i = 0; i < 8; i++) {
        sceGuLogicalOp(L2[i]);
        rect2d(10 + i * 57, 14, 62 + i * 57, 86, 0xC3A55A3C);
    }
    sceGuDisable(GU_COLOR_LOGIC_OP);

    sceGuEnable(GU_STENCIL_TEST);
    sceGuStencilFunc(GU_ALWAYS, 0x55, 0xFF);
    sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE);
    rect2d(10, 112, 240, 142, 0xFF0000FF);
    sceGuStencilOp(GU_KEEP, GU_KEEP, GU_INCR);
    rect2d(120, 112, 350, 142, 0xFF00FF00);
    sceGuStencilOp(GU_KEEP, GU_KEEP, GU_INVERT);
    rect2d(300, 112, 470, 142, 0xFFFF0000);
    sceGuStencilOp(GU_KEEP, GU_KEEP, GU_KEEP);
    sceGuStencilFunc(GU_EQUAL, 0x56, 0xFF);
    rect2d(10, 116, 470, 126, 0xFFFFFFFF);
    sceGuStencilFunc(GU_EQUAL, 0x55, 0xFF);
    rect2d(10, 128, 470, 138, 0xFF00FFFF);
    sceGuDisable(GU_STENCIL_TEST);

    pmsk2_stencil(150, 175);
    grad2d(10, 185, 470, 263, 0xFF000000, 0xFFFFFFFF);
    clear_bands(185);
    scene_end("tests2", GU_PSM_8888, 0);
}

/* The pixel mask, colour test, logic ops, clears and PMSK2 on a 16-bit
 * target, over a grey gradient. Rows: four pixel masks (0x00F8FCF8, the
 * bits 5650 keeps; 0x00070307, the bits it drops; 0x00808080; 0xFFFF0000)
 * writing white, then black; colour test EQUAL 0x808080 under masks
 * 0xF0F0F0 and 0xFFFFFF and NOTEQUAL under 0xF0F0F0 on a black-to-white
 * gradient over red; logic ops XOR, AND, OR, INVERTED; the six clears of
 * scene 29; stencil writes under PMSK2. Dither is off except in its clear. */
static void scene_masks16(int psm, const char *name) {
    if (step("scene %02d: %s", g_scene, name)) return;
    scene_begin(psm, 0x00000000);
    grad2d(0, 0, 480, 272, 0xFF000000, 0xFFFFFFFF);
    static const w32 M[4] = { 0x00F8FCF8, 0x00070307, 0x00808080, 0xFFFF0000 };
    for (int i = 0; i < 4; i++) {
        sceGuPixelMask(M[i]);
        rect2d(8 + i * 116, 8, 120 + i * 116, 30, 0xFFFFFFFF);
        rect2d(8 + i * 116, 32, 120 + i * 116, 54, 0x00000000);
    }
    sceGuPixelMask(0);
    rect2d(8, 60, 472, 90, 0xFF0000FF);
    sceGuEnable(GU_COLOR_TEST);
    sceGuColorFunc(GU_EQUAL, 0x808080, 0xF0F0F0);
    grad2d(8, 60, 472, 70, 0xFF000000, 0xFFFFFFFF);
    sceGuColorFunc(GU_EQUAL, 0x808080, 0xFFFFFF);
    grad2d(8, 70, 472, 80, 0xFF000000, 0xFFFFFFFF);
    sceGuColorFunc(GU_NOTEQUAL, 0x808080, 0xF0F0F0);
    grad2d(8, 80, 472, 90, 0xFF000000, 0xFFFFFFFF);
    sceGuDisable(GU_COLOR_TEST);
    sceGuEnable(GU_COLOR_LOGIC_OP);
    static const int L4[4] = { GU_XOR, GU_AND, GU_OR, GU_INVERTED };
    for (int i = 0; i < 4; i++) {
        sceGuLogicalOp(L4[i]);
        rect2d(8 + i * 116, 96, 120 + i * 116, 120, 0xC3A55A3C);
    }
    sceGuDisable(GU_COLOR_LOGIC_OP);
    clear_bands(126);
    pmsk2_stencil(210, 262);
    scene_end(name, psm, 0);
}

/* ---- more bounding boxes ------------------------------------------------- */

static void box_corners(BP3 *b, float cx, float cy, float cz, float h) {
    for (int i = 0; i < 8; i++)
        b[i] = (BP3){ cx + ((i & 1) ? h : -h), cy + ((i & 2) ? h : -h), cz + ((i & 4) ? h : -h) };
}

/* As bbox_object, with the marker anywhere. */
static void bbox_object_at(float cx, float cy, float cz, float h,
                           float mx0, float my0, float mx1, float my1, w32 color) {
    BP3 box[8];
    box_corners(box, cx, cy, cz, h);
    CV mark[2] = { { color, mx0, my0, 0 }, { color, mx1, my1, 0 } };
    void *bv = gumem(box, sizeof box);
    void *mv = gumem(mark, sizeof mark);
    sceGuBeginObject(GU_VERTEX_32BITF | GU_TRANSFORM_3D, 8, NULL, bv);
    sceGuDrawArray(GU_SPRITES, FMT_CV2D, 2, NULL, mv);
    sceGuEndObject();
}

/* BBOX by hand: vertex type, BASE, VADDR, then BBOX with the count. */
static void bbox_raw(const void *box, int count) {
    const w32 a = (w32)box;
    sceGuSendCommandi(18, GU_VERTEX_32BITF | GU_TRANSFORM_3D);   /* VTYPE */
    sceGuSendCommandi(16, (a >> 8) & 0x0F0000);                    /* BASE: address bits 24-27 */
    sceGuSendCommandi(1, a & 0xFFFFFF);                             /* VADDR */
    sceGuSendCommandi(7, count);                                    /* BBOX */
}

/* Run 4 answered for eight boxes; these ask what those left open. Boxes 0-2
 * are wholly or partly behind the camera and far to one side: does the test
 * divide by a negative w (and mirror them), or treat them otherwise? 3 and 4
 * sit in the middle of the screen with the scissor cut to the top-left
 * corner (away from the box, then over part of it): is it the scissor that
 * the corners are tested against? 5 is on screen but, with the viewport cut
 * to half size, outside the clip volume (|x| > w): the clip volume or the
 * screen? 6 draws its marker with a PRIM straight after BBOX and no VADDR:
 * the marker's vertices follow the box's, so it is drawn only if BBOX moved
 * VADDR past them. 7 is a hidden box whose marker is drawn with no BJUMP:
 * BBOX by itself should not stop a draw. */
static void scene_bbox2(void) {
    static const struct { float cx, cy, cz, h; int kind; w32 color; const char *what; } B[8] = {
        { 20.0f, 0.0f,   5.0f, 0.5f, 0, 0xFFFFFFFF, "behind the camera, far right" },
        { 0.0f,  30.0f,  5.0f, 0.5f, 0, 0xFF0000FF, "behind the camera, far above" },
        { 20.0f, 0.0f,   0.0f, 0.5f, 0, 0xFF00FF00, "far right, across the camera plane" },
        { 0.0f,  0.0f,  -5.0f, 0.5f, 1, 0xFFFF0000, "in view, scissor 0-100 away from it" },
        { 0.0f,  0.0f,  -5.0f, 0.5f, 2, 0xFF00FFFF, "in view, scissor 0-245 over part of it" },
        { 7.0f,  0.0f,  -5.0f, 0.3f, 3, 0xFFFF00FF, "on screen, outside a half-size viewport's clip volume" },
        { 0.0f,  0.0f,  -5.0f, 0.5f, 4, 0xFFFFFF00, "PRIM right after BBOX, no VADDR" },
        { 20.0f, 0.0f,  -5.0f, 0.5f, 5, 0xFF808080, "hidden, marker drawn without BJUMP" },
    };
    for (int i = 0; i < 8; i++) {
        if (step("scene %02d: bounding box %d, %s", g_scene, i, B[i].what)) continue;
        scene_begin(GU_PSM_8888, 0xFF000000);
        float mx0 = 10 + i * 58, my0 = 220, mx1 = 60 + i * 58, my1 = 262;
        switch (B[i].kind) {
        case 1:
        case 2:
            mx0 = 20; my0 = 20; mx1 = 80; my1 = 80;
            if (B[i].kind == 1) sceGuScissor(0, 0, 100, 100); else sceGuScissor(0, 0, 245, 140);
            bbox_object_at(B[i].cx, B[i].cy, B[i].cz, B[i].h, mx0, my0, mx1, my1, B[i].color);
            break;
        case 3:
            sceGuViewport(2048, 2048, SCR_W / 2, SCR_H / 2);
            bbox_object_at(B[i].cx, B[i].cy, B[i].cz, B[i].h, mx0, my0, mx1, my1, B[i].color);
            break;
        case 4: {
            struct { BP3 box[8]; CV mark[2]; } d;
            box_corners(d.box, B[i].cx, B[i].cy, B[i].cz, B[i].h);
            d.mark[0] = (CV){ B[i].color, mx0, my0, 0 };
            d.mark[1] = (CV){ B[i].color, mx1, my1, 0 };
            bbox_raw(gumem(&d, sizeof d), 8);
            sceGuSendCommandi(18, FMT_CV2D);                         /* VTYPE */
            sceGuSendCommandi(4, (GU_SPRITES << 16) | 2);            /* PRIM, VADDR untouched */
            break;
        }
        case 5: {
            BP3 box[8];
            box_corners(box, B[i].cx, B[i].cy, B[i].cz, B[i].h);
            bbox_raw(gumem(box, sizeof box), 8);
            rect2d(mx0, my0, mx1, my1, B[i].color);
            break;
        }
        default:
            bbox_object_at(B[i].cx, B[i].cy, B[i].cz, B[i].h, mx0, my0, mx1, my1, B[i].color);
            break;
        }
        sceGuFinish();
        int r = ge_wait();
        const w32 *px = (const w32 *)VRAM_UNCACHED + (int)((my0 + my1) / 2) * FB_W + (int)((mx0 + mx1) / 2);
        out("  wait %08X; marker %s (pixel %08X)\n", (unsigned)r,
            (*px & 0xFFFFFF) == (B[i].color & 0xFFFFFF) ? "drawn" : "not drawn", *px);
    }
}

/* ---- geprobe 6: what geprobe 5 left open ---------------------------------
 *
 * fw660-run5/findings/geprobe.md lists what the version 5 scenes left
 * unsettled: how precisely the GE lights a vertex whose normal or light is
 * off the axes (scene 16's fans come out 1 or 2 below psprecomp's), point and
 * spot lights, which vertex a 3D triangle's depth plane starts from, where a
 * line with fractional ends stops, the colours a patch generates at
 * divisions that are not powers of 2, how precise a colour gradient is,
 * whether a morph blend keeps a colour's fraction, bounding boxes across the
 * camera plane, and whether indexed draws move IADDR. */

/* A marker's centre pixel: drawn in its colour or not. */
static void report_marker(const char *what, float x0, float y0, float x1, float y1, w32 color) {
    const w32 *px = (const w32 *)VRAM_UNCACHED + (int)((y0 + y1) / 2) * FB_W + (int)((x0 + x1) / 2);
    out("  %s: %s (pixel %08X)\n", what, (*px & 0xFFFFFF) == (color & 0xFFFFFF) ? "drawn" : "not drawn", *px);
}

/* Scene 16's fan normals ((i - 6)/6, ((5i mod 7) - 3)/3, 1), unnormalised,
 * computed as lit_fan computes them. */
static void fan_normal(int i, float n[3]) {
    n[0] = (float)(i - 6) / 6.0f;
    n[1] = (float)((i * 5) % 7 - 3) / 3.0f;
    n[2] = 1.0f;
}

/* The same normalised, and (0, sin, cos) of i * 7 degrees. */
static const float FAN_NN[12][3] = {
    { -0.577350269f, -0.577350269f, 0.577350269f }, { -0.569802882f, 0.455842306f, 0.683763459f },
    { -0.554700196f, 0.0f, 0.832050294f },          { -0.384110640f, -0.512147520f, 0.768221280f },
    { -0.229415734f, 0.688247202f, 0.688247202f },  { -0.156173762f, 0.312347524f, 0.937042571f },
    { 0.0f, -0.316227766f, 0.948683298f },          { 0.117041147f, -0.702246883f, 0.702246883f },
    { 0.267261242f, 0.534522484f, 0.801783726f },   { 0.447213595f, 0.0f, 0.894427191f },
    { 0.485071250f, -0.485071250f, 0.727606875f },  { 0.507673083f, 0.609207699f, 0.609207699f },
};
static const float YZ_N[12][3] = {
    { 0.0f, 0.0f, 1.0f },                 { 0.0f, 0.121869343f, 0.992546152f },
    { 0.0f, 0.241921896f, 0.970295726f }, { 0.0f, 0.358367950f, 0.933580426f },
    { 0.0f, 0.469471563f, 0.882947593f }, { 0.0f, 0.573576436f, 0.819152044f },
    { 0.0f, 0.669130606f, 0.743144825f }, { 0.0f, 0.754709580f, 0.656059029f },
    { 0.0f, 0.829037573f, 0.559192903f }, { 0.0f, 0.891006524f, 0.453990500f },
    { 0.0f, 0.939692621f, 0.342020143f }, { 0.0f, 0.974370065f, 0.224951054f },
};

/* Row r of 12 flat quads at z = -6, one normal each: 0 scene 16's fan
 * normals, 1 the same normalised, 2 YZ_N. */
static void lit_row(int r, int normals) {
    for (int i = 0; i < 12; i++) {
        float n[3];
        if (normals == 1)      { n[0] = FAN_NN[i][0]; n[1] = FAN_NN[i][1]; n[2] = FAN_NN[i][2]; }
        else if (normals == 2) { n[0] = YZ_N[i][0];   n[1] = YZ_N[i][1];   n[2] = YZ_N[i][2]; }
        else fan_normal(i, n);
        const float x0 = -5.7f + i * 0.95f, y0 = 3.1f - r * 0.72f;
        nquad(g_nv + 6 * i, x0, y0, x0 + 0.8f, y0 - 0.55f, -6.0f, n);
    }
    sceGuDrawArray(GU_TRIANGLES, FMT_NV3D, 72, NULL, gumem(g_nv, 72 * sizeof(NV)));
}

/* Light 0 white diffuse only, a white material, no ambient anywhere: a quad
 * lit by it is 255 times the light's factor (scene 25's set-up). */
static void lit_white(void) {
    sceGuAmbient(0xFF000000);
    sceGuColorMaterial(0);
    sceGuModelColor(0x000000, 0x000000, 0xFFFFFF, 0xFFFFFF);
    sceGuAmbientColor(0xFF000000);            /* material ambient black, alpha 0xFF */
    sceGuLightColor(0, GU_AMBIENT, 0x000000);
    sceGuLightColor(0, GU_DIFFUSE, 0xFFFFFF);
    sceGuLightColor(0, GU_SPECULAR, 0x000000);
}

/* Scene 16's fans are 1 or 2 below psprecomp where scene 25, with the light
 * and the normals in the x-z plane, is exact. One flat quad per normal, each
 * row one change from row 0: row 0 scene 16's light (0.3, 0.5, 1) and fan
 * normals, white; 1 the normals normalised; 2 the light normalised; 3 the
 * light on +z and normals in the y-z plane; 4 the light's diffuse 0x80C0FF;
 * 5 the material's diffuse 0x80C0FF; 6 all of scene 16's lower-left fan
 * (ambient, emissive, light ambient, colours); 7 and 8 specular alone,
 * coefficients 12 and 1. */
static void scene_lightmath(void) {
    if (step("scene %02d: lighting arithmetic, one flat quad per normal", g_scene)) return;
    scene_begin(GU_PSM_8888, 0xFF000000);
    sceGuEnable(GU_LIGHTING);
    sceGuLightMode(GU_SINGLE_COLOR);
    sceGuEnable(GU_LIGHT0);
    ScePspFVector3 dir = { 0.3f, 0.5f, 1.0f }, zdir = { 0.0f, 0.0f, 1.0f };
    ScePspFVector3 ndir = { 0.259160528f, 0.431934213f, 0.863868426f };
    lit_white();
    sceGuLight(0, GU_DIRECTIONAL, GU_DIFFUSE, &dir);
    lit_row(0, 0);
    lit_row(1, 1);
    sceGuLight(0, GU_DIRECTIONAL, GU_DIFFUSE, &ndir);
    lit_row(2, 0);
    sceGuLight(0, GU_DIRECTIONAL, GU_DIFFUSE, &zdir);
    lit_row(3, 2);
    sceGuLight(0, GU_DIRECTIONAL, GU_DIFFUSE, &dir);
    sceGuLightColor(0, GU_DIFFUSE, 0x80C0FF);
    lit_row(4, 0);
    sceGuLightColor(0, GU_DIFFUSE, 0xFFFFFF);
    sceGuModelColor(0x000000, 0x000000, 0x80C0FF, 0xFFFFFF);
    sceGuAmbientColor(0xFF000000);
    lit_row(5, 0);
    sceGuAmbient(0xFF202020);
    sceGuLight(0, GU_DIRECTIONAL, GU_AMBIENT_AND_DIFFUSE, &dir);
    sceGuLightColor(0, GU_AMBIENT, 0xFF101010);
    sceGuLightColor(0, GU_DIFFUSE, 0xFFFFC080);
    sceGuModelColor(0xFF000040, 0xFF404040, 0xFF8080FF, 0xFFFFFFFF);
    lit_row(6, 0);
    lit_white();
    sceGuLight(0, GU_DIRECTIONAL, GU_DIFFUSE_AND_SPECULAR, &dir);
    sceGuLightColor(0, GU_DIFFUSE, 0x000000);
    sceGuLightColor(0, GU_SPECULAR, 0xFFFFFF);
    sceGuSpecular(12.0f);
    lit_row(7, 0);
    sceGuSpecular(1.0f);
    lit_row(8, 0);
    scene_end("lightmath", GU_PSM_8888, 0);
}

static NV g_pts[200];

/* 20 x 10 points 0.25 apart around (cx, cy) on z = -6, facing +z: one pixel
 * per vertex, so each shows one vertex's lit colour. */
static void lit_points(float cx, float cy) {
    for (int r = 0; r < 10; r++)
        for (int c = 0; c < 20; c++)
            g_pts[r * 20 + c] = (NV){ 0.0f, 0.0f, 1.0f, cx + (c - 9.5f) * 0.25f, cy + (r - 4.5f) * 0.25f, -6.0f };
    sceGuDrawArray(GU_POINTS, FMT_NV3D, 200, NULL, gumem(g_pts, sizeof g_pts));
}

/* Scene 16's point and spot lights, white, on a grid of points each (the
 * clear is pure red, which no point lit white can be): top left a point
 * light 1.5 in front of the plane with attenuation (0.5, 0.3, 0.1); top
 * right a spot 3 in front, exponent 4, cutoff 0.9; bottom left the point
 * light's specular alone, coefficient 8, no attenuation; bottom right a spot
 * with exponent 1.5, cutoff 0.5 and scene 16's attenuation. Each light sits
 * 0.45 right of and 0.35 below its grid's centre. The spots' direction is
 * +z, towards the camera: scene 16's spot, pointed at its fan (-z), lit
 * nothing on fw 6.60, so the GE compares the direction with the one from the
 * vertex to the light. */
static void scene_locallights(void) {
    if (step("scene %02d: point and spot lights, one point per vertex", g_scene)) return;
    scene_begin(GU_PSM_8888, 0xFF0000FF);
    sceGuEnable(GU_LIGHTING);
    sceGuLightMode(GU_SINGLE_COLOR);
    sceGuEnable(GU_LIGHT0);
    lit_white();
    ScePspFVector3 p0 = { -2.55f, 1.35f, -4.5f }, p1 = { 3.45f, 1.35f, -3.0f };
    ScePspFVector3 p2 = { -2.55f, -2.05f, -4.5f }, p3 = { 3.45f, -2.05f, -3.0f };
    ScePspFVector3 up = { 0.0f, 0.0f, 1.0f };
    sceGuLight(0, GU_POINTLIGHT, GU_DIFFUSE, &p0);
    sceGuLightAtt(0, 0.5f, 0.3f, 0.1f);
    lit_points(-3.0f, 1.7f);
    sceGuLight(0, GU_SPOTLIGHT, GU_DIFFUSE, &p1);
    sceGuLightAtt(0, 1.0f, 0.0f, 0.0f);
    sceGuLightSpot(0, &up, 4.0f, 0.9f);
    lit_points(3.0f, 1.7f);
    sceGuLight(0, GU_POINTLIGHT, GU_DIFFUSE_AND_SPECULAR, &p2);
    sceGuLightColor(0, GU_DIFFUSE, 0x000000);
    sceGuLightColor(0, GU_SPECULAR, 0xFFFFFF);
    sceGuSpecular(8.0f);
    lit_points(-3.0f, -1.7f);
    sceGuLight(0, GU_SPOTLIGHT, GU_DIFFUSE, &p3);
    sceGuLightColor(0, GU_DIFFUSE, 0xFFFFFF);
    sceGuLightColor(0, GU_SPECULAR, 0x000000);
    sceGuLightAtt(0, 0.5f, 0.3f, 0.1f);
    sceGuLightSpot(0, &up, 1.5f, 0.5f);
    lit_points(3.0f, -1.7f);
    scene_end("locallights", GU_PSM_8888, 0);
}

/* Four triangles in the unit square, (u, v, z) per corner: z sloping in x,
 * in y, in both, and a skewed one sloping in both. */
static const float DEPTH_SHAPES[4][3][3] = {
    { { 0.0f, 0.0f, -5.0f }, { 1.0f, 0.0f, -5.6f }, { 0.0f, 1.0f, -5.0f } },
    { { 0.0f, 0.0f, -5.0f }, { 1.0f, 0.0f, -5.0f }, { 0.0f, 1.0f, -5.7f } },
    { { 0.0f, 0.0f, -5.0f }, { 1.0f, 0.0f, -5.4f }, { 0.0f, 1.0f, -5.5f } },
    { { 0.0f, 0.0f, -5.2f }, { 1.0f, 0.3f, -5.5f }, { 0.4f, 1.0f, -4.9f } },
};

/* Scenes 17 and 27's 3D depth is a step off in places whichever way
 * psprecomp anchors the plane. One row per shape (0.9 across), six copies
 * each: the corners given starting from each of the three, then the same in
 * the other winding. Depth test ALWAYS, writes on; the depth buffer is saved
 * too. */
static void scene_depthanchor(void) {
    if (step("scene %02d: 3D depth planes, each shape from each corner and in both windings", g_scene)) return;
    scene_begin(GU_PSM_8888, 0xFF000000);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuDepthFunc(GU_ALWAYS);
    sceGuDepthMask(GU_FALSE);                 /* writes on */
    static CV t[4 * 6 * 3];
    int n = 0;
    for (int s = 0; s < 4; s++)
        for (int k = 0; k < 6; k++) {
            const float X = -3.75f + k * 1.5f, Y = 2.2f - s * 1.25f;
            const w32 c = 0xFF800000u | (w32)(0x40 + s * 0x30) | (w32)(0x40 + k * 0x20) << 8;
            for (int j = 0; j < 3; j++) {
                const int v = k < 3 ? (k + j) % 3 : (k + 3 - j) % 3;
                t[n++] = (CV){ c, X + DEPTH_SHAPES[s][v][0] * 0.9f, Y - DEPTH_SHAPES[s][v][1] * 0.9f,
                               DEPTH_SHAPES[s][v][2] };
            }
        }
    sceGuDrawArray(GU_TRIANGLES, FMT_CV3D, n, NULL, gumem(t, n * sizeof(CV)));
    scene_end("depthanchor", GU_PSM_8888, 1);
}

static CV g_lines[2 * 192];
static int g_nl;

/* A through-mode line, red at its start and green at its end. */
static void line2d(float x0, float y0, float x1, float y1) {
    g_lines[g_nl++] = (CV){ 0xFF0000FF, x0, y0, 0 };
    g_lines[g_nl++] = (CV){ 0xFF00FF00, x1, y1, 0 };
}

/* Scene 28's 3D line stops a pixel from where psprecomp stops it, at an end
 * of 5818/16. Through-mode lines whose ends fall on every sixteenth, one
 * per row k (fraction k/16), y = 6 + 8k: going right with the end's
 * fraction, then the start's; going left likewise; going right and slightly
 * down to x fractions 0.625 and 0.4375 with the end's y fraction k/16, and
 * the same two reversed. Then steep lines, one per column k: going down with
 * the end's fraction, going up with it, going down and up with the start's. */
static void scene_lineends(void) {
    if (step("scene %02d: line ends on every sixteenth", g_scene)) return;
    scene_begin(GU_PSM_8888, 0xFF000000);
    g_nl = 0;
    for (int k = 0; k < 16; k++) {
        const float f = k / 16.0f, y = 6 + 8 * k;
        line2d(10, y, 60 + f, y);
        line2d(70 + f, y, 120, y);
        line2d(180, y, 130 + f, y);
        line2d(240 + f, y, 190, y);
        line2d(250, y, 290.625f, y + 3 + f);
        line2d(300, y, 340.4375f, y + 3 + f);
        line2d(390.625f, y + 3 + f, 350, y);
        line2d(440.4375f, y + 3 + f, 400, y);
    }
    for (int k = 0; k < 16; k++) {
        const float f = k / 16.0f, xl = 10 + 14 * k, xr = 250 + 14 * k;
        line2d(xl, 135, xl + 2, 180 + f);
        line2d(xr, 180, xr + 2, 135 + f);
        line2d(xl, 190 + f, xl + 3, 235);
        line2d(xr, 240 + f, xr + 3, 195);
    }
    sceGuDrawArray(GU_LINES, FMT_CV2D, g_nl, NULL, gumem(g_lines, g_nl * sizeof(CV)));
    scene_end("lineends", GU_PSM_8888, 0);
}

/* Patches drawn as points, so each generated vertex is one pixel in its own
 * colour, unblended with its neighbours: Bezier at divisions 3, 5, 6, 7 and
 * 12, and splines (fill/fill at 3 and 5, open/open at 3). Row 1 over a
 * flat_grid (bilinear colours), row 2 the same grid with blue alternating,
 * row 3 scene 22's curved grid, smaller. */
static void scene_patchpoints(void) {
    if (step("scene %02d: patch vertices as points, divisions 3 to 12", g_scene)) return;
    scene_begin(GU_PSM_8888, 0xFF000000);
    static const struct { int div, spline, edge; } C[8] = {
        { 3, 0, 0 }, { 5, 0, 0 }, { 6, 0, 0 }, { 7, 0, 0 }, { 12, 0, 0 },
        { 3, 1, GU_FILL_FILL }, { 5, 1, GU_FILL_FILL }, { 3, 1, GU_OPEN_OPEN } };
    CV g[16];
    sceGuPatchPrim(GU_POINTS);
    for (int row = 0; row < 3; row++)
        for (int i = 0; i < 8; i++) {
            const float cx = -5.075f + i * 1.45f;
            if (row < 2) flat_grid(g, cx, 2.3f - row * 2.1f, 0.4f, row ? 2 : 0);
            else patch_grid(g, 4, 4, cx * 0.7f, -1.9f, 0.3f);
            sceGuPatchDivide(C[i].div, C[i].div);
            if (C[i].spline) sceGuDrawSpline(FMT_CV3D, 4, 4, C[i].edge, C[i].edge, NULL, gumem(g, sizeof g));
            else sceGuDrawBezier(FMT_CV3D, 4, 4, NULL, gumem(g, sizeof g));
        }
    sceGuPatchPrim(GU_TRIANGLE_STRIP);
    scene_end("patchpoints", GU_PSM_8888, 0);
}

/* How precisely a colour gradient steps. Scene 17's quads are a step off
 * where a finer gradient would fix them, scene 20's triangles a step off
 * where a coarser one would. Through mode, one row per width 5 to 211:
 * a triangle whose colour changes in x only (red 255 to 0, green 0 to 255,
 * blue 0x40 to 0xC0), at whole-pixel corners on the left and at fractional
 * ones on the right. Below, in 3D at z = -5: scene 17's magenta-to-yellow
 * quads, ten 0.6 wide, then ten 0.3 to 1.2 wide, then scene 20's
 * red-green-blue triangle ten times along a row. */
static void scene_gradients(void) {
    if (step("scene %02d: colour gradient precision", g_scene)) return;
    scene_begin(GU_PSM_8888, 0xFF000000);
    static const short W[14] = { 5, 7, 11, 17, 23, 31, 47, 63, 85, 113, 150, 180, 199, 211 };
    const w32 c0 = 0xFF4000FF, c1 = 0xFFC0FF00;
    for (int i = 0; i < 14; i++) {
        const float y = 4 + 12 * i;
        tri2d(10, y, c0, 10 + W[i], y, c1, 10, y + 10, c0);
        tri2d(250.3125f, y + 0.5625f, c0, 250.75f + W[i], y + 0.5625f, c1, 250.3125f, y + 10.5625f, c0);
    }
    for (int i = 0; i < 10; i++)
        quad3d(-4.6f + i * 0.9f, -1.35f, -4.0f + i * 0.9f, -0.9f, -5.0f, 0xFFFF00FF, 0xFFFFFF00);
    float x = -4.6f;
    for (int i = 0; i < 10; i++) {
        const float w = 0.3f + 0.1f * i;
        quad3d(x, -1.95f, x + w, -1.5f, -5.0f, 0xFFFF00FF, 0xFFFFFF00);
        x += w + 0.15f;
    }
    CV t[30];
    for (int i = 0; i < 10; i++) {
        const float tx = -4.6f + i * 0.95f;
        t[3 * i]     = (CV){ 0xFF0000FF, tx, -2.75f, -5.0f };
        t[3 * i + 1] = (CV){ 0xFF00FF00, tx + 0.8f, -2.75f, -5.0f };
        t[3 * i + 2] = (CV){ 0xFFFF0000, tx + 0.4f, -2.05f, -5.0f };
    }
    sceGuDrawArray(GU_TRIANGLES, FMT_CV3D, 30, NULL, gumem(t, sizeof t));
    scene_end("gradients", GU_PSM_8888, 0);
}

/* Whether a morph blend keeps a colour's fraction. One wide quad per row at
 * z = -6, both vertex sets in the same place, left colour 0xFF40FF00 and
 * 0xFF41FE01, right 0xFFC000FF and 0xFFBF01FE: weights 1/0, 0/1, 0.5/0.5,
 * 0.25/0.75, 0.75/0.25, 0.3/0.7. Then, unmorphed, the first set's gradient
 * and the gradient that 0.5/0.5 truncated at the corners gives (0xFF40FE00
 * to 0xFFBF00FE). */
static void scene_morphgrad(void) {
    if (step("scene %02d: morph blends of colour gradients", g_scene)) return;
    scene_begin(GU_PSM_8888, 0xFF000000);
    static const float MW[6][2] = {
        { 1.0f, 0.0f }, { 0.0f, 1.0f }, { 0.5f, 0.5f }, { 0.25f, 0.75f }, { 0.75f, 0.25f }, { 0.3f, 0.7f } };
    const w32 L0 = 0xFF40FF00, R0 = 0xFFC000FF, L1 = 0xFF41FE01, R1 = 0xFFBF01FE;
    for (int r = 0; r < 8; r++) {
        const float y0 = 2.9f - r * 0.75f, y1 = y0 - 0.6f;
        if (r == 6) { quad3d(-5.0f, y1, 5.0f, y0, -6.0f, L0, R0); continue; }
        if (r == 7) { quad3d(-5.0f, y1, 5.0f, y0, -6.0f, 0xFF40FE00, 0xFFBF00FE); continue; }
        const float P[6][2] = { { -5, y0 }, { 5, y0 }, { -5, y1 }, { 5, y0 }, { 5, y1 }, { -5, y1 } };
        CV mv[12];
        for (int k = 0; k < 6; k++) {
            const int right = P[k][0] > 0;
            mv[2 * k]     = (CV){ right ? R0 : L0, P[k][0], P[k][1], -6.0f };
            mv[2 * k + 1] = (CV){ right ? R1 : L1, P[k][0], P[k][1], -6.0f };
        }
        sceGuMorphWeight(0, MW[r][0]);
        sceGuMorphWeight(1, MW[r][1]);
        sceGuDrawArray(GU_TRIANGLES, GU_VERTICES(2) | FMT_CV3D, 6, NULL, gumem(mv, sizeof mv));
    }
    scene_end("morphgrad", GU_PSM_8888, 0);
}

/* Bounding boxes at the camera, one list and step each, no dump: across the
 * camera plane (z -0.5 to 0.5) at x 0.5, 2, 5 and 10; behind the camera just
 * off the axis; between the camera and the near plane; across the near
 * plane; behind the camera and wide. */
static void scene_bbox3(void) {
    static const struct { float cx, cy, cz, h; w32 color; const char *what; } B[8] = {
        { 0.5f,  0.0f,  0.0f, 0.5f, 0xFFFFFFFF, "across the camera plane, x 0.5" },
        { 2.0f,  0.0f,  0.0f, 0.5f, 0xFF0000FF, "across the camera plane, x 2" },
        { 5.0f,  0.0f,  0.0f, 0.5f, 0xFF00FF00, "across the camera plane, x 5" },
        { 10.0f, 0.0f,  0.0f, 0.5f, 0xFFFF0000, "across the camera plane, x 10" },
        { 0.3f,  0.2f,  3.0f, 0.5f, 0xFF00FFFF, "behind the camera, just off the axis" },
        { 0.0f,  0.0f, -0.5f, 0.3f, 0xFFFF00FF, "between the camera and the near plane" },
        { 0.0f,  0.0f, -1.1f, 0.3f, 0xFFFFFF00, "across the near plane" },
        { 0.0f,  0.0f,  2.0f, 1.5f, 0xFF808080, "behind the camera, wide" },
    };
    for (int i = 0; i < 8; i++) {
        if (step("scene %02d: bounding box %d, %s", g_scene, i, B[i].what)) continue;
        scene_begin(GU_PSM_8888, 0xFF000000);
        const float mx0 = 10 + i * 58, my0 = 220, mx1 = 60 + i * 58, my1 = 262;
        bbox_object_at(B[i].cx, B[i].cy, B[i].cz, B[i].h, mx0, my0, mx1, my1, B[i].color);
        sceGuFinish();
        out("  wait %08X\n", (w32)ge_wait());
        report_marker("marker", mx0, my0, mx1, my1, B[i].color);
    }
}

/* A sprite marker in slot 0-7 of a row starting at y. */
static void mark(CV *v, int slot, float y, w32 color) {
    v[0] = (CV){ color, 10 + slot * 58, y, 0 };
    v[1] = (CV){ color, 60 + slot * 58, y + 40, 0 };
}

static void send_addr(int cmd, w32 a) {
    sceGuSendCommandi(16, (a >> 8) & 0x0F0000);                    /* BASE: address bits 24-27 */
    sceGuSendCommandi(cmd, a & 0xFFFFFF);                            /* VADDR (1) or IADDR (2) */
}

/* Scene 33 showed that BBOX and PRIM move VADDR past the vertices they read.
 * Indexed draws, by hand: rows 1 and 2 (16- and 8-bit indices 0, 1, 4, 5 over
 * four sprites A-D in slots 0-3) draw PRIM 2 then PRIM 2 with no address in
 * between. The second draws A again if neither address moved, B if only
 * VADDR moved (by two vertices), C if only IADDR moved, D if both did. Row 3
 * is the same unindexed (A then B). Row 4: an indexed BBOX (indices 0-9 over
 * 8 corners, then sprites) followed by an indexed PRIM 2: the sprite in
 * slot 0 if IADDR moved past the box's 8 indices and VADDR stayed, slot 1
 * if only VADDR moved past the corners, slot 2 if both moved. */
static void scene_indices(void) {
    if (step("scene %02d: indexed draws: do PRIM and BBOX move IADDR", g_scene)) return;
    scene_begin(GU_PSM_8888, 0xFF000000);
    static const w32 COL[4] = { 0xFFFFFFFF, 0xFF0000FF, 0xFF00FF00, 0xFFFF0000 };
    static const unsigned short I16[4] = { 0, 1, 4, 5 };
    static const unsigned char I8[4] = { 0, 1, 4, 5 };
    for (int row = 0; row < 2; row++) {
        CV v[8];
        for (int m = 0; m < 4; m++) mark(v + 2 * m, m, 10 + row * 50, COL[m]);
        const w32 va = (w32)gumem(v, sizeof v);
        const w32 ia = row ? (w32)gumem(I8, sizeof I8) : (w32)gumem(I16, sizeof I16);
        sceGuSendCommandi(18, FMT_CV2D | (row ? GU_INDEX_8BIT : GU_INDEX_16BIT));   /* VTYPE */
        send_addr(1, va);
        send_addr(2, ia);
        sceGuSendCommandi(4, (GU_SPRITES << 16) | 2);                                 /* PRIM */
        sceGuSendCommandi(4, (GU_SPRITES << 16) | 2);                                 /* PRIM again */
    }
    CV u[4];
    mark(u, 0, 110, COL[0]);
    mark(u + 2, 1, 110, COL[1]);
    sceGuSendCommandi(18, FMT_CV2D);
    send_addr(1, (w32)gumem(u, sizeof u));
    sceGuSendCommandi(4, (GU_SPRITES << 16) | 2);
    sceGuSendCommandi(4, (GU_SPRITES << 16) | 2);

    /* Corners at bytes 0-95; the sprite for "VADDR moved" at 96 (index 0 past
     * the corners), for "IADDR moved" at 128 (index 8 as a 16-byte vertex),
     * for both at 224 (index 8 past the corners). */
    struct { BP3 box[8]; CV m1[2]; CV m2[2]; CV pad[4]; CV m3[2]; } d;
    memset(&d, 0, sizeof d);
    box_corners(d.box, 0.0f, 0.0f, -5.0f, 0.5f);
    mark(d.m2, 0, 160, COL[0]);
    mark(d.m1, 1, 160, COL[1]);
    mark(d.m3, 2, 160, COL[2]);
    static const unsigned short IB[10] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    const w32 da = (w32)gumem(&d, sizeof d), ib = (w32)gumem(IB, sizeof IB);
    sceGuSendCommandi(18, GU_VERTEX_32BITF | GU_TRANSFORM_3D | GU_INDEX_16BIT);
    send_addr(1, da);
    send_addr(2, ib);
    sceGuSendCommandi(7, 8);                                                          /* BBOX */
    sceGuSendCommandi(18, FMT_CV2D | GU_INDEX_16BIT);
    sceGuSendCommandi(4, (GU_SPRITES << 16) | 2);
    scene_end("indices", GU_PSM_8888, 0);

    static const char *const WHAT[4] = { "A (first PRIM)", "B (VADDR moved)", "C (IADDR moved)", "D (both moved)" };
    char s[64];
    for (int row = 0; row < 2; row++)
        for (int m = 0; m < 4; m++) {
            snprintf(s, sizeof s, "%s indices, %s", row ? "8-bit" : "16-bit", WHAT[m]);
            report_marker(s, 10 + m * 58, 10 + row * 50, 60 + m * 58, 50 + row * 50, COL[m]);
        }
    report_marker("unindexed, A", 10, 110, 60, 150, COL[0]);
    report_marker("unindexed, B", 68, 110, 118, 150, COL[1]);
    report_marker("after indexed BBOX, IADDR moved", 10, 160, 60, 200, COL[0]);
    report_marker("after indexed BBOX, VADDR moved", 68, 160, 118, 200, COL[1]);
    report_marker("after indexed BBOX, both moved", 126, 160, 176, 200, COL[2]);
}

/* ---- geprobe 7: what geprobe 6 left open ---------------------------------
 *
 * fw660-run6/findings/geprobe.md lists what the version 6 scenes left
 * unsettled: what a coloured light or material does to a lit channel (the
 * white light is exact), which weights a patch puts on each control point
 * (linear control colours come out up to 1.5 high past the middle), the
 * depth the GE gives a transformed vertex (eleven values pin a model, not a
 * mechanism), how precise 1/area is in a gradient (scene 39 prefers a short
 * reciprocal, scene 20 an exact one), which corner a 3D depth plane starts
 * from, and the colour of steep lines. These scenes run after the callback
 * steps, so every step before them keeps its version 6 number. */

/* Eye-space x and y that the probes' perspective (60 degrees, 480/272)
 * puts at screen position (sx, sy), in pixels, at depth z (< 0). */
static void eye_xy(float sx, float sy, float z, float *x, float *y) {
    const float t = 0.577350269f * -z;                  /* tan 30 * |z| */
    *x = (sx - 240.0f) / 240.0f * (480.0f / 272.0f) * t;
    *y = (136.0f - sy) / 136.0f * t;
}

/* The centre of pixel (px, py). */
static void eye_at(int px, int py, float z, float *x, float *y) {
    eye_xy(px + 0.5f, py + 0.5f, z, x, y);
}

/* The pixel of point i of a lit row: 64 to a line, 7 pixels apart, four
 * lines 8 apart from y0. */
static void lit_px(int i, int y0, int *px, int *py) {
    *px = 12 + 7 * (i % 64);
    *py = y0 + 8 * (i / 64);
}

/* What the step stands for in each channel of a stepped colour: R i, G
 * 255 - i, B 37 i mod 256, so each channel takes every value once. */
static w32 step_colour(int i) {
    return (w32)i | (w32)(255 - i) << 8 | (w32)((i * 37) & 255) << 16;
}

static CNV g_lp[256];

/* Scene 34's white light is exact, floor(256 N.L) on every normal; a
 * coloured light or material (its rows 4-6) is off by one in a pattern no
 * gain tried reproduces. One pixel per lit vertex, normal on the light
 * (N.L = 1) unless said, no ambient, z = -6, five rows of 256 points
 * (lit_px), 0 to 255 in every channel (step_colour): row A the light's
 * diffuse colour stepped, material white, one point per draw; B the
 * material's diffuse colour stepped through the vertex colour, light white;
 * C the same under the light 0x80C0FF; D the light stepped, material
 * 0x80C0FF; E N.L stepped from 1 to 0 (normal (0, 2t, 1 - t^2) / (1 + t^2),
 * t = i/255), light 0x80C0FF, material white. */
static void scene_lightgain(void) {
    if (step("scene %02d: light and material colour gains, one point per value", g_scene)) return;
    scene_begin(GU_PSM_8888, 0xFF000000);
    sceGuEnable(GU_LIGHTING);
    sceGuLightMode(GU_SINGLE_COLOR);
    sceGuEnable(GU_LIGHT0);
    ScePspFVector3 zdir = { 0.0f, 0.0f, 1.0f };
    lit_white();
    sceGuLight(0, GU_DIRECTIONAL, GU_DIFFUSE, &zdir);
    for (int i = 0; i < 256; i++) {                                     /* A */
        int px, py; float x, y;
        lit_px(i, 12, &px, &py);
        eye_at(px, py, -6.0f, &x, &y);
        NV p = { 0.0f, 0.0f, 1.0f, x, y, -6.0f };
        sceGuLightColor(0, GU_DIFFUSE, step_colour(i));
        sceGuDrawArray(GU_POINTS, FMT_NV3D, 1, NULL, gumem(&p, sizeof p));
    }
    sceGuLightColor(0, GU_DIFFUSE, 0xFFFFFF);
    sceGuColorMaterial(GU_DIFFUSE);
    for (int row = 0; row < 2; row++) {                                 /* B, C */
        if (row) sceGuLightColor(0, GU_DIFFUSE, 0x80C0FF);
        for (int i = 0; i < 256; i++) {
            int px, py; float x, y;
            lit_px(i, 60 + 48 * row, &px, &py);
            eye_at(px, py, -6.0f, &x, &y);
            g_lp[i] = (CNV){ 0xFF000000u | step_colour(i), 0.0f, 0.0f, 1.0f, x, y, -6.0f };
        }
        sceGuDrawArray(GU_POINTS, FMT_CNV3D, 256, NULL, gumem(g_lp, sizeof g_lp));
    }
    sceGuColorMaterial(0);
    sceGuModelColor(0x000000, 0x000000, 0x80C0FF, 0x000000);
    sceGuAmbientColor(0xFF000000);
    for (int i = 0; i < 256; i++) {                                     /* D */
        int px, py; float x, y;
        lit_px(i, 156, &px, &py);
        eye_at(px, py, -6.0f, &x, &y);
        NV p = { 0.0f, 0.0f, 1.0f, x, y, -6.0f };
        sceGuLightColor(0, GU_DIFFUSE, step_colour(i));
        sceGuDrawArray(GU_POINTS, FMT_NV3D, 1, NULL, gumem(&p, sizeof p));
    }
    lit_white();
    sceGuLight(0, GU_DIRECTIONAL, GU_DIFFUSE, &zdir);
    sceGuLightColor(0, GU_DIFFUSE, 0x80C0FF);
    for (int i = 0; i < 256; i++) {                                     /* E */
        int px, py; float x, y;
        lit_px(i, 204, &px, &py);
        eye_at(px, py, -6.0f, &x, &y);
        const float t = i / 255.0f, d = 1.0f + t * t;
        g_nv[0] = (NV){ 0.0f, 2.0f * t / d, (1.0f - t * t) / d, x, y, -6.0f };
        sceGuDrawArray(GU_POINTS, FMT_NV3D, 1, NULL, gumem(g_nv, sizeof(NV)));
    }
    scene_end("lightgain", GU_PSM_8888, 0);
}

/* Control points as flat_grid places them, black but for column k, red,
 * and row k, green: the colour along u is 255 times control column k's
 * weight, along v row k's. */
static void onehot_grid(CV *v, float cx, float cy, float s, int k) {
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++)
            v[j * 4 + i] = (CV){ 0xFF000000u | (i == k ? 0xFFu : 0u) | (j == k ? 0xFF00u : 0u),
                                 cx + (i - 1.5f) * s, cy + (j - 1.5f) * s, -6.0f };
}

/* Scene 38's linear control colours (0, 85, 170, 255) come out up to 1.5
 * high past a patch's middle, which no cut of the weights or forward
 * differencing reproduces. The weights alone: patches drawn as points
 * (scene 38's eight kinds, one column each: Bezier at divisions 3, 5, 6, 7
 * and 12, splines fill/fill at 3 and 5, open/open at 3), one row per k =
 * 0-3, control column k red and row k green (onehot_grid). */
static void scene_patchweights(void) {
    if (step("scene %02d: patch basis weights, one control column and row lit", g_scene)) return;
    scene_begin(GU_PSM_8888, 0xFF000000);
    static const struct { int div, spline, edge; } C[8] = {
        { 3, 0, 0 }, { 5, 0, 0 }, { 6, 0, 0 }, { 7, 0, 0 }, { 12, 0, 0 },
        { 3, 1, GU_FILL_FILL }, { 5, 1, GU_FILL_FILL }, { 3, 1, GU_OPEN_OPEN } };
    CV g[16];
    sceGuPatchPrim(GU_POINTS);
    for (int k = 0; k < 4; k++)
        for (int i = 0; i < 8; i++) {
            onehot_grid(g, -5.075f + i * 1.45f, 2.3f - k * 1.5f, 0.4f, k);
            sceGuPatchDivide(C[i].div, C[i].div);
            if (C[i].spline) sceGuDrawSpline(FMT_CV3D, 4, 4, C[i].edge, C[i].edge, NULL, gumem(g, sizeof g));
            else sceGuDrawBezier(FMT_CV3D, 4, 4, NULL, gumem(g, sizeof g));
        }
    sceGuPatchPrim(GU_TRIANGLE_STRIP);
    scene_end("patchweights", GU_PSM_8888, 0);
}

#define NZP 3840
static CV g_zp[NZP];

/* The eye depth of point k of scene_vertexdepth: k < 1920 even steps from
 * -1.05 to -12, then even steps in 1/z from -12 to -99. */
static float zp_depth(int k) {
    if (k < 1920) return -(1.05f + k * (10.95f / 1919.0f));
    const float r0 = 1.0f / 12.0f, r1 = 1.0f / 99.0f;
    return -1.0f / (r0 - (k - 1920) * ((r0 - r1) / 1919.0f));
}

/* Eleven vertex depths pin psprecomp's depth arithmetic as a fit, not a
 * mechanism. A point writes its vertex's depth as it is, so: 3840 points,
 * each at its own eye depth (zp_depth), 96 to a row 5 pixels apart, 40 rows
 * 6 apart, each coloured by its index (red the low byte, green the high,
 * blue 0x80). Depth test ALWAYS, writes on; the depth buffer is saved. */
static void scene_vertexdepth(void) {
    if (step("scene %02d: one point per eye depth, -1.05 to -99", g_scene)) return;
    scene_begin(GU_PSM_8888, 0xFF000000);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuDepthFunc(GU_ALWAYS);
    sceGuDepthMask(GU_FALSE);
    for (int k = 0; k < NZP; k++) {
        const float z = zp_depth(k);
        float x, y;
        eye_at(2 + 5 * (k % 96), 8 + 6 * (k / 96), z, &x, &y);
        g_zp[k] = (CV){ 0xFF800000u | (w32)(k & 0xFF) | (w32)(k >> 8) << 8, x, y, z };
    }
    sceGuDrawArray(GU_POINTS, FMT_CV3D, NZP, NULL, gumem(g_zp, sizeof g_zp));
    scene_end("vertexdepth", GU_PSM_8888, 1);
}

/* Row r of the gradient sweeps is 3 + 7r/16 pixels high. */
#define GS_ROWS 22
static float gs_height(int r) { return 3.0f + 0.4375f * r; }

/* Scene 39 prefers a short 1/area for colour gradients, scene 20's skinned
 * triangles an exact one. One gradient over many areas: through mode, a
 * column of right triangles 200 pixels wide (whole corners) and one 200.4375
 * wide (corners on sixteenths), each row 7/16 of a pixel taller than the one
 * above (3 to 12.2), red 0 to 250, green 0 to 255, blue 5 to 206, depth 1000
 * to 51000. Depth test ALWAYS, writes on; the depth buffer is saved. */
static void scene_gradsweep(void) {
    if (step("scene %02d: one gradient over 44 areas, through mode", g_scene)) return;
    scene_begin(GU_PSM_8888, 0xFF000000);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuDepthFunc(GU_ALWAYS);
    sceGuDepthMask(GU_FALSE);
    const w32 c0 = 0xFF050000u, c1 = 0xFFCEFFFAu;
    for (int col = 0; col < 2; col++) {
        const float x0 = col ? 250.3125f : 10.0f, w = col ? 200.4375f : 200.0f;
        float y = 4.0f;
        for (int r = 0; r < GS_ROWS; r++) {
            const float h = gs_height(r);
            CV v[3] = { { c0, x0, y, 1000.0f }, { c1, x0 + w, y, 51000.0f }, { c0, x0, y + h, 1000.0f } };
            sceGuDrawArray(GU_TRIANGLES, FMT_CV2D, 3, NULL, gumem(v, sizeof v));
            y += h + 1.0f;
        }
    }
    scene_end("gradsweep", GU_PSM_8888, 1);
}

/* The same triangles in 3D at z = -5, placed where the perspective puts
 * them on the same screen positions (eye_xy), constant depth. */
static void scene_gradsweep3d(void) {
    if (step("scene %02d: the same gradients in 3D", g_scene)) return;
    scene_begin(GU_PSM_8888, 0xFF000000);
    const w32 c0 = 0xFF050000u, c1 = 0xFFCEFFFAu;
    for (int col = 0; col < 2; col++) {
        const float x0 = col ? 250.3125f : 10.0f, w = col ? 200.4375f : 200.0f;
        float y = 4.0f;
        for (int r = 0; r < GS_ROWS; r++) {
            const float h = gs_height(r);
            CV v[3];
            float x, yy;
            eye_xy(x0, y, -5.0f, &x, &yy);         v[0] = (CV){ c0, x, yy, -5.0f };
            eye_xy(x0 + w, y, -5.0f, &x, &yy);     v[1] = (CV){ c1, x, yy, -5.0f };
            eye_xy(x0, y + h, -5.0f, &x, &yy);     v[2] = (CV){ c0, x, yy, -5.0f };
            sceGuDrawArray(GU_TRIANGLES, FMT_CV3D, 3, NULL, gumem(v, sizeof v));
            y += h + 1.0f;
        }
    }
    scene_end("gradsweep3d", GU_PSM_8888, 0);
}

/* Eight triangles in the unit square, (u, v down, z) per corner, chosen so
 * the top, leftmost, rightmost and bottom corners fall on different
 * vertices in different shapes, with depths far enough apart that a plane
 * started from another corner writes other values. */
static const float ANCHOR_SHAPES[8][3][3] = {
    { { 0.0f, 0.0f, -5.0f }, { 1.0f, 0.2f, -7.4f }, { 0.3f, 1.0f, -5.9f } },
    { { 0.0f, 0.5f, -5.3f }, { 0.6f, 0.0f, -7.6f }, { 1.0f, 1.0f, -6.1f } },
    { { 0.0f, 1.0f, -6.8f }, { 0.4f, 0.0f, -5.1f }, { 1.0f, 0.6f, -7.9f } },
    { { 0.0f, 0.3f, -7.7f }, { 1.0f, 0.0f, -5.2f }, { 0.5f, 1.0f, -6.0f } },
    { { 0.0f, 0.0f, -6.2f }, { 1.0f, 0.6f, -7.8f }, { 0.2f, 1.0f, -5.0f } },
    { { 0.3f, 0.0f, -5.1f }, { 0.0f, 0.8f, -7.2f }, { 1.0f, 1.0f, -6.3f } },
    { { 0.0f, 0.45f, -5.0f }, { 1.0f, 0.0f, -7.9f }, { 0.9f, 1.0f, -5.8f } },
    { { 0.5f, 0.0f, -6.9f }, { 0.0f, 1.0f, -5.0f }, { 1.0f, 0.7f, -7.6f } },
};

/* Scene 36's one shape whose top corner is not its leftmost matches when its
 * depth plane starts from the top, but scene 17 fits worse that way. One
 * row per ANCHOR_SHAPES shape (0.55 across), six copies each: the corners
 * given from each of the three, then the same in the other winding, as in
 * scene 36. Flat colour per triangle; depth test ALWAYS, writes on; the
 * depth buffer is saved too. */
static void scene_depthanchor2(void) {
    if (step("scene %02d: 3D depth planes, eight more shapes from each corner", g_scene)) return;
    scene_begin(GU_PSM_8888, 0xFF000000);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuDepthFunc(GU_ALWAYS);
    sceGuDepthMask(GU_FALSE);
    static CV t[8 * 6 * 3];
    int n = 0;
    for (int s = 0; s < 8; s++)
        for (int k = 0; k < 6; k++) {
            const float X = -4.2f + k * 1.5f, Y = 2.45f - s * 0.68f;
            const w32 c = 0xFF800000u | (w32)(0x40 + s * 0x18) | (w32)(0x40 + k * 0x20) << 8;
            for (int j = 0; j < 3; j++) {
                const int v = k < 3 ? (k + j) % 3 : (k + 3 - j) % 3;
                t[n++] = (CV){ c, X + ANCHOR_SHAPES[s][v][0] * 0.55f, Y - ANCHOR_SHAPES[s][v][1] * 0.55f,
                               ANCHOR_SHAPES[s][v][2] };
            }
        }
    sceGuDrawArray(GU_TRIANGLES, FMT_CV3D, n, NULL, gumem(t, n * sizeof(CV)));
    scene_end("depthanchor2", GU_PSM_8888, 1);
}

/* Scene 37's steep lines are a step of colour off on 7 pixels. Through-mode
 * lines, red to green, one per column k (fraction k/16 of a pixel across):
 * down 46.3 pixels, up the same, down with the start a fraction lower, up
 * likewise. */
static void scene_steeplines(void) {
    if (step("scene %02d: steep line colours", g_scene)) return;
    scene_begin(GU_PSM_8888, 0xFF000000);
    g_nl = 0;
    for (int k = 0; k < 32; k++) {
        const float f = k / 16.0f, x = 10 + 14 * k;
        line2d(x, 20, x + f, 66.3125f);
        line2d(x + f, 130.3125f, x, 84);
        line2d(x, 150 + f / 2, x + 3 + f, 196);
        line2d(x + 3 + f, 250, x, 204.5f + f / 2);
    }
    sceGuDrawArray(GU_LINES, FMT_CV2D, g_nl, NULL, gumem(g_lines, g_nl * sizeof(CV)));
    scene_end("steeplines", GU_PSM_8888, 0);
}

/* ---- version 8 ------------------------------------------------------------
 *
 * Scene 46's depth gradients want the triangle setup's 1/area cut to 16 bits
 * on 35 of 44 areas and another value on the rest: seven a unit or two
 * above it (five of those at or above the exact value), two below it, with
 * no order in which areas do which. Scene 37's line of length 797/16 wants
 * one above it too. Scenes 50 and 51 read that reciprocal once
 * per 10-bit length: the numerator is a power of two, so the depth gradient
 * is the reciprocal itself, and a depth range of 32768 over 32 to 64 pixels
 * pins it to a fraction of its last bit. */

#define RS_SLOTS 630                      /* 7 columns x 90 rows */
static CV g_rs[RS_SLOTS * 3];

/* Scene 50: through-mode right triangles 2 pixels high (32 sixteenths, a
 * power of two), so the area's significant bits are the width's. Slot i
 * (7 to a row, 68 pixels apart; rows 3 pixels apart) has its right angle at
 * a whole pixel. Slots 0-511 are 512 + i sixteenths wide, depth 16384 to
 * 49152 (a difference of 2^15) left to right; slots 512-629 are 514 + 4 (i -
 * 512) wide, depth 0 to 65535, so the numerator has 16 significant bits.
 * Colour black to 0x2080FF (red 255, green 128, blue 32). Depth test ALWAYS,
 * writes on; the depth buffer is saved. */
static void scene_rcpsweep(void) {
    if (step("scene %02d: one triangle reciprocal per 10-bit width, through mode", g_scene)) return;
    scene_begin(GU_PSM_8888, 0xFF000000);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuDepthFunc(GU_ALWAYS);
    sceGuDepthMask(GU_FALSE);
    const w32 c0 = 0xFF000000u, c1 = 0xFF2080FFu;
    int n = 0;
    for (int i = 0; i < RS_SLOTS; i++) {
        const float x0 = (float)(2 + 68 * (i % 7)), y0 = (float)(1 + 3 * (i / 7));
        const int w16 = i < 512 ? 512 + i : 514 + 4 * (i - 512);
        const float za = i < 512 ? 16384.0f : 0.0f, zb = i < 512 ? 49152.0f : 65535.0f;
        g_rs[n++] = (CV){ c0, x0, y0, za };
        g_rs[n++] = (CV){ c1, x0 + w16 / 16.0f, y0, zb };
        g_rs[n++] = (CV){ c0, x0, y0 + 2.0f, za };
    }
    sceGuDrawArray(GU_TRIANGLES, FMT_CV2D, n, NULL, gumem(g_rs, n * sizeof(CV)));
    scene_end("rcpsweep", GU_PSM_8888, 1);
}

static CV g_ls[512 * 4];

/* Scene 51: the same for lines. Horizontal through-mode lines 512 + i
 * sixteenths long, i = 0-511, 7 to a row (68 pixels apart, rows 3 apart):
 * on the row's first line left to right from a whole pixel, depth 16384 to
 * 49152, black to 0x2080FF; on its second the same line right to left, so
 * the differences are negative. Depth test ALWAYS, writes on; the depth
 * buffer is saved. */
static void scene_linesweep(void) {
    if (step("scene %02d: one line reciprocal per 10-bit length, through mode", g_scene)) return;
    scene_begin(GU_PSM_8888, 0xFF000000);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuDepthFunc(GU_ALWAYS);
    sceGuDepthMask(GU_FALSE);
    const w32 c0 = 0xFF000000u, c1 = 0xFF2080FFu;
    int n = 0;
    for (int i = 0; i < 512; i++) {
        const float x0 = (float)(2 + 68 * (i % 7)), y = 1.5f + 3 * (i / 7), x1 = x0 + (512 + i) / 16.0f;
        g_ls[n++] = (CV){ c0, x0, y, 16384.0f };
        g_ls[n++] = (CV){ c1, x1, y, 49152.0f };
        g_ls[n++] = (CV){ c1, x1, y + 1.0f, 49152.0f };
        g_ls[n++] = (CV){ c0, x0, y + 1.0f, 16384.0f };
    }
    sceGuDrawArray(GU_LINES, FMT_CV2D, n, NULL, gumem(g_ls, n * sizeof(CV)));
    scene_end("linesweep", GU_PSM_8888, 1);
}

/* Scene 44's fill/fill splines read their weights a step off the uniform
 * B-spline's at most samples, and 10 samples do not settle how; scene 23's
 * splines mix open and filled ends. Scene 52: splines drawn as points, u the
 * kind under test and v open/open over 4 rows at one division (v = 0 and 1,
 * weights exactly 1 and 0), so each point's colour is three u weights: control
 * column k red, k + 1 green, k + 2 blue, k = 0 and 3. Every u edge mode, 4,
 * 5 and 6 control columns, divisions 2, 3, 4, 6 and 8: 120 patches, 4 to a
 * row 119 pixels apart, rows 6 apart, control columns 2.5 x division pixels
 * apart so the points fall at least 2.5 pixels apart, at z = -6. The clear
 * is red 255, green 1, blue 255, which no point can be (its red and blue are
 * two weights of one sample), so the black points show too. */
static void scene_splineweights(void) {
    if (step("scene %02d: spline weights along u, three control columns lit", g_scene)) return;
    scene_begin(GU_PSM_8888, 0xFFFF01FF);
    static const int EDGE[4] = { GU_FILL_FILL, GU_OPEN_FILL, GU_FILL_OPEN, GU_OPEN_OPEN };
    static const int DIV[5] = { 2, 3, 4, 6, 8 };
    CV g[6 * 4];
    int cell = 0;
    sceGuPatchPrim(GU_POINTS);
    for (int d = 0; d < 5; d++)
        for (int e = 0; e < 4; e++)
            for (int cols = 4; cols <= 6; cols++)
                for (int k = 0; k < cols; k += 3) {
                    const float X = 4.5f + 119 * (cell % 4), Y = 2.5f + 6 * (cell / 4), s = 2.5f * DIV[d];
                    for (int j = 0; j < 4; j++)
                        for (int i = 0; i < cols; i++) {
                            w32 c = 0xFF000000u;
                            if (i == k) c |= 0xFFu;
                            if (i == k + 1) c |= 0xFF00u;
                            if (i == k + 2) c |= 0xFF0000u;
                            float x, y;
                            eye_xy(X + i * s, Y + j, -6.0f, &x, &y);
                            g[j * cols + i] = (CV){ c, x, y, -6.0f };
                        }
                    sceGuPatchDivide(DIV[d], 1);
                    sceGuDrawSpline(FMT_CV3D, cols, 4, EDGE[e], GU_OPEN_OPEN, NULL,
                                    gumem(g, cols * 4 * (int)sizeof(CV)));
                    cell++;
                }
    sceGuPatchPrim(GU_TRIANGLE_STRIP);
    scene_end("splineweights", GU_PSM_8888, 0);
}

/* ---- version 9 ------------------------------------------------------------
 *
 * Scene 52's weights fit exact Cox-de Boor values everywhere on a Bezier
 * piece, but the two inner weights of a B-spline span read a step off at
 * some samples: both on a uniform span, the second after an open start and
 * the third before an open end, each pair off in opposite directions. Its
 * one control colour (255) and 2 to 8 steps cannot tell a weight that is
 * off from colour arithmetic that is. Scenes 53 and 54 read the same
 * weights at 40 and 48 steps a span, with control colours 255, 254 and
 * 129, and with two neighbouring columns lit in one channel, whose sum
 * shows whether the pair's errors cancel. */

typedef struct { int edge, cols; float s, first; } SW2;

/* The splines under test: a uniform span, two uniform spans, and two spans
 * with each kind of open end. s is the control columns' spacing in pixels,
 * first the column the first sample sits on, so every row starts at x = 20
 * and the samples fall at least 2.5 pixels apart. */
static const SW2 SW2_SPLINES[5] = {
    { GU_FILL_FILL, 4, 300.0f, 1.0f }, { GU_FILL_FILL, 5, 200.0f, 1.0f },
    { GU_OPEN_OPEN, 5, 110.0f, 0.0f }, { GU_OPEN_FILL, 5, 140.0f, 0.0f },
    { GU_FILL_OPEN, 5, 140.0f, 1.0f } };

/* One patch, a row 6 pixels high at y: v open/open over 4 rows at one
 * division, as in scene 52, so each point's colour is the u weights alone.
 * Single (pair = 0): control column k red, k + 1 green, k + 2 blue, each
 * channel `level`. Pair: columns k and k + 1 red, k + 1 and k + 2 green,
 * k + 2 and k + 3 blue, at 255. */
static void sw2_patch(const SW2 *sp, int div, int k, int pair, int level, float y) {
    CV g[6 * 4];
    const float x0 = 20.0f - sp->first * sp->s;
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < sp->cols; i++) {
            w32 c = 0xFF000000u;
            if (pair) {
                if (i == k || i == k + 1) c |= 0xFFu;
                if (i == k + 1 || i == k + 2) c |= 0xFF00u;
                if (i == k + 2 || i == k + 3) c |= 0xFF0000u;
            } else {
                if (i == k) c |= (w32)level;
                if (i == k + 1) c |= (w32)level << 8;
                if (i == k + 2) c |= (w32)level << 16;
            }
            float x, yy;
            eye_xy(x0 + i * sp->s, y + j, -6.0f, &x, &yy);
            g[j * sp->cols + i] = (CV){ c, x, yy, -6.0f };
        }
    sceGuPatchDivide(div, 1);
    sceGuDrawSpline(FMT_CV3D, sp->cols, 4, sp->edge, GU_OPEN_OPEN, NULL,
                    gumem(g, sp->cols * 4 * (int)sizeof(CV)));
}

/* Scenes 53 and 54: for each spline, levels 255, 254 and 129 at k = 0 and
 * 3, then the pairs from k = 0 (and k = 1 with 5 columns), one patch to a
 * row, 39 rows from y = 2.5. Clear red 255, green 1, blue 255 as in scene
 * 52: no single point reaches 255, and no pair point lights both red and
 * blue fully. */
static void scene_splinedense(int div) {
    if (step("scene %02d: spline weights at %d steps a span, three levels and pairs", g_scene, div)) return;
    scene_begin(GU_PSM_8888, 0xFFFF01FF);
    static const int LEVEL[3] = { 255, 254, 129 };
    sceGuPatchPrim(GU_POINTS);
    int row = 0;
    for (int n = 0; n < 5; n++) {
        const SW2 *sp = &SW2_SPLINES[n];
        for (int l = 0; l < 3; l++)
            for (int k = 0; k < sp->cols; k += 3)
                sw2_patch(sp, div, k, 0, LEVEL[l], 2.5f + 6 * row++);
        for (int k = 0; k + 3 < sp->cols; k++)
            sw2_patch(sp, div, k, 1, 255, 2.5f + 6 * row++);
    }
    sceGuPatchPrim(GU_TRIANGLE_STRIP);
    char name[24];
    snprintf(name, sizeof name, "splinedense%d", div);
    scene_end(name, GU_PSM_8888, 0);
}

/* ---- version 10 -----------------------------------------------------------
 *
 * Scenes 53 and 54 found every spline weight one value at all three levels,
 * a function of the span and t alone (908 read at two divisions agree), but
 * a colour reads it only to 1/255. Scene 55 reads the same weights through
 * depth, about 128 times finer, and scene 56 reads the parameter t the GE
 * itself uses, through a texture. Scenes 48's 3D depth planes are a step off
 * in one direction only; scene 57 redraws them in through mode at
 * psprecomp's corners with the depths the PSP gives each corner as a point,
 * and scene 58 moves one shape by every sixteenth of a pixel. */

/* Through-mode-like placement for 3D draws: x and y in pixels, eye z from 0
 * (depth 65535) to -1 (depth 0), no perspective. */
static void ortho_screen(void) {
    sceGumMatrixMode(GU_PROJECTION);
    sceGumLoadIdentity();
    sceGumOrtho(0.0f, 480.0f, 272.0f, 0.0f, 0.0f, 1.0f);
    sceGumMatrixMode(GU_MODEL);
    sceGumUpdateMatrix();
}

/* The depth buffer whole, 512 pixels a row through the plain VRAM address,
 * as `<name>_depthfull.bin`: the 480-wide dump cannot read the pixels whose
 * addresses fall in columns 480-511 (render.c depth_addr). */
static void dump_depth_full(const char *name) {
    const unsigned char *z = (const unsigned char *)VRAM_UNCACHED + (w32)ZBP;
    const int size = FB_W * SCR_H * 2;
    memcpy(g_dump, z, size);
    char file[48];
    snprintf(file, sizeof file, "ge_%02d_%s_depthfull.bin", g_scene, name);
    const int wr = probe_write_file(file, g_dump, size);
    out("  %s: %d bytes, crc %08X\n", file, wr, crc32(g_dump, size));
}

/* Where the GE keeps pixel (x, y)'s depth, through the plain VRAM address:
 * the permutation geprobe 2 scene 17 measured (render.c depth_addr). */
static unsigned depth_at(int x, int y) {
    const w32 l = (w32)ZBP + (w32)(y * FB_W + x) * 2;
    const w32 mid = (l >> 5) & 0x1F, rot = ((mid << 1) | (mid >> 4)) & 0x1F;
    const w32 p = ((l & ~(0x1Fu << 5)) | (rot << 5)) ^ 0x2040u;
    const unsigned char *b = (const unsigned char *)VRAM_UNCACHED + p;
    return (unsigned)b[0] | (unsigned)b[1] << 8;
}

typedef struct { int edge, cols; float s, first; } SD10;
static const SD10 SD10_SPLINES[6] = {
    { GU_FILL_FILL, 4, 300.0f, 1.0f }, { GU_OPEN_OPEN, 4, 140.0f, 0.0f },
    { GU_FILL_FILL, 5, 200.0f, 1.0f }, { GU_OPEN_OPEN, 5, 110.0f, 0.0f },
    { GU_OPEN_FILL, 5, 140.0f, 0.0f }, { GU_FILL_OPEN, 5, 140.0f, 1.0f } };

/* Scene 55: one patch per spline and control column k, 48 steps a span, as
 * points in a row 6 pixels high, orthographic. Every control point sits at
 * eye z -0.25 except column k's, at -0.75, so a point's depth is
 * 32767.5 (1.5 - w) for column k's weight w; column k is also red 255, so
 * the colour reads the same weight as scenes 53 and 54 do. The spline
 * order is a uniform span, a Bezier piece (open/open, 4 columns), two
 * uniform spans, then 5 columns open/open, open/fill and fill/open: 28 rows
 * from y = 2.5. Depth test ALWAYS, writes on; both depth dumps. */
static void scene_splinedepth(void) {
    if (step("scene %02d: spline weights read through depth, 48 steps a span, one column at a time", g_scene)) return;
    scene_begin(GU_PSM_8888, 0xFFFF01FF);
    ortho_screen();
    sceGuEnable(GU_DEPTH_TEST);
    sceGuDepthFunc(GU_ALWAYS);
    sceGuDepthMask(GU_FALSE);
    sceGuPatchPrim(GU_POINTS);
    int row = 0;
    CV g[6 * 4];
    for (int n = 0; n < 6; n++) {
        const SD10 *sp = &SD10_SPLINES[n];
        for (int k = 0; k < sp->cols; k++, row++) {
            const float x0 = 20.0f - sp->first * sp->s, y = 2.5f + 6 * row;
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < sp->cols; i++)
                    g[j * sp->cols + i] = (CV){ i == k ? 0xFF0000FFu : 0xFF000000u,
                                                x0 + i * sp->s, y + j, i == k ? -0.75f : -0.25f };
            sceGuPatchDivide(48, 1);
            sceGuDrawSpline(FMT_CV3D, sp->cols, 4, sp->edge, GU_OPEN_OPEN, NULL,
                            gumem(g, sp->cols * 4 * (int)sizeof(CV)));
        }
    }
    sceGuPatchPrim(GU_TRIANGLE_STRIP);
    scene_end("splinedepth", GU_PSM_8888, 1);
    dump_depth_full("splinedepth");
}

static w32 *g_texramp;           /* 512 x 8, texel x = x in red (low) and green (high) */

/* Scene 56: the same splines with no texture coordinates, so the GE gives
 * each point its surface parameter as (u, v); a 512-texel ramp, nearest,
 * repeated, replaces the colour with the texel, red + 256 green = floor of
 * 512 times u's fraction. Each spline at texture scale 1, 4 and 16, so the
 * texel pins u to 1/512, 1/2048 and 1/8192 of a turn: 18 rows. */
static void scene_splineparam(void) {
    if (step("scene %02d: the spline parameter the GE uses, through a 512-texel ramp", g_scene)) return;
    if (!g_texramp) {
        g_texramp = memalign(16, 512 * 8 * 4);
        if (!g_texramp) { out("  out of memory\n"); return; }
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 512; x++) g_texramp[y * 512 + x] = 0xFF000000u | (w32)(x & 0xFF) | (w32)(x >> 8) << 8;
        sceKernelDcacheWritebackAll();
    }
    scene_begin(GU_PSM_8888, 0xFFFF01FF);
    ortho_screen();
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexImage(0, 512, 8, 512, g_texramp);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuTexWrap(GU_REPEAT, GU_REPEAT);
    sceGuTexOffset(0.0f, 0.0f);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuPatchPrim(GU_POINTS);
    static const float SCALE[3] = { 1.0f, 4.0f, 16.0f };
    int row = 0;
    CV g[6 * 4];
    for (int n = 0; n < 6; n++) {
        const SD10 *sp = &SD10_SPLINES[n];
        for (int l = 0; l < 3; l++, row++) {
            const float x0 = 20.0f - sp->first * sp->s, y = 2.5f + 6 * row;
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < sp->cols; i++)
                    g[j * sp->cols + i] = (CV){ 0xFF000000u, x0 + i * sp->s, y + j, -0.5f };
            sceGuTexScale(SCALE[l], 1.0f);
            sceGuPatchDivide(48, 1);
            sceGuDrawSpline(FMT_CV3D, sp->cols, 4, sp->edge, GU_OPEN_OPEN, NULL,
                            gumem(g, sp->cols * 4 * (int)sizeof(CV)));
        }
    }
    sceGuPatchPrim(GU_TRIANGLE_STRIP);
    sceGuTexScale(1.0f, 1.0f);
    sceGuDisable(GU_TEXTURE_2D);
    scene_end("splineparam", GU_PSM_8888, 0);
}

/* Scene 48's 48 triangles as psprecomp projects them (1/16 pixel, corners
 * in the order drawn), which the earlier runs confirmed through their depth
 * planes. */
static const short TWIN_XY[48][6] = {
#include "twin_table.inc"
};

/* Scene 48's corner (s, k, j): the shape, its copy and the corner's place in
 * the copy's order, as scene_depthanchor2 draws them. */
static CV anchor_corner(int s, int k, int j, w32 c) {
    const float X = -4.2f + k * 1.5f, Y = 2.45f - s * 0.68f;
    const int v = k < 3 ? (k + j) % 3 : (k + 3 - j) % 3;
    return (CV){ c, X + ANCHOR_SHAPES[s][v][0] * 0.55f, Y - ANCHOR_SHAPES[s][v][1] * 0.55f, ANCHOR_SHAPES[s][v][2] };
}

/* Scene 57: first each of scene 48's 144 corners as a point (its index in
 * red and green), read back: the pixel its colour landed on near
 * psprecomp's corner, and that pixel's depth, logged 8 to a line as
 * "x,y=depth". Then the 48 triangles again in through mode, at psprecomp's
 * corners with those depths (psprecomp's floored depth where a point is not
 * found, marked '?'), colour as scene 48. If these match scene 48 the 3D
 * planes are the through-mode rule on the corners the GE has; if not, the
 * 3D path differs. Depth test ALWAYS, writes on; both depth dumps. */
static void scene_anchortwin(void) {
    if (step("scene %02d: scene 48's corners as points, then its triangles in through mode at those depths", g_scene)) return;
    static CV pts[144], tri[144];
    scene_begin(GU_PSM_8888, 0xFF000000);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuDepthFunc(GU_ALWAYS);
    sceGuDepthMask(GU_FALSE);
    for (int t = 0; t < 48; t++)
        for (int j = 0; j < 3; j++) {
            const int i = t * 3 + j;
            pts[i] = anchor_corner(t / 6, t % 6, j, 0xFF550000u | (w32)(i & 0xFF) | (w32)(i >> 8) << 8);
        }
    sceGuDrawArray(GU_POINTS, FMT_CV3D, 144, NULL, gumem(pts, sizeof pts));
    sceGuFinish();
    ge_wait();
    const w32 *fb = (const w32 *)VRAM_UNCACHED;
    int found = 0;
    for (int i = 0; i < 144; i++) {
        const int t = i / 3, j = i % 3;
        const int cx = TWIN_XY[t][2 * j] >> 4, cy = TWIN_XY[t][2 * j + 1] >> 4;
        const w32 want = 0x550000u | (w32)(i & 0xFF) | (w32)(i >> 8) << 8;
        int hx = -1, hy = -1;
        for (int dy = -1; dy <= 1 && hx < 0; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                const int x = cx + dx, y = cy + dy;
                if (x < 0 || y < 0 || x >= SCR_W || y >= SCR_H) continue;
                if ((fb[y * FB_W + x] & 0xFFFFFF) == want) { hx = x; hy = y; break; }
            }
        const w32 c = 0xFF800000u | (w32)(0x40 + (t / 6) * 0x18) | (w32)(0x40 + (t % 6) * 0x20) << 8;
        float z = -1.0f;
        if (hx >= 0) { z = (float)depth_at(hx, hy); found++; }
        tri[i] = (CV){ c, TWIN_XY[t][2 * j] / 16.0f, TWIN_XY[t][2 * j + 1] / 16.0f, z };
        if (i % 8 == 0) out("  ");
        if (hx >= 0) out(" %d,%d=%u", hx, hy, (unsigned)z);
        else out(" ?");
        if (i % 8 == 7) out("\n");
    }
    out("  %d of 144 corners found\n", found);
    for (int i = 0; i < 144; i++)
        if (tri[i].z < 0.0f) tri[i].z = 0.0f;        /* not found: depth 0, logged as '?' */
    scene_begin(GU_PSM_8888, 0xFF000000);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuDepthFunc(GU_ALWAYS);
    sceGuDepthMask(GU_FALSE);
    sceGuDrawArray(GU_TRIANGLES, FMT_CV2D, 144, NULL, gumem(tri, sizeof tri));
    scene_end("anchortwin", GU_PSM_8888, 1);
    dump_depth_full("anchortwin");
}

/* Scene 58: scene 48's shape 1 (the one most off), 40 pixels across in its
 * first order, at screen positions moved by i/16 of a pixel across (top two
 * rows of 8) and down (bottom two rows), i = 0..15, each copy its own flat
 * colour, at the shape's eye depths. Depth test ALWAYS, writes on; both
 * depth dumps. */
static void scene_anchorsweep(void) {
    if (step("scene %02d: one 3D shape at every sixteenth of a pixel across and down", g_scene)) return;
    scene_begin(GU_PSM_8888, 0xFF000000);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuDepthFunc(GU_ALWAYS);
    sceGuDepthMask(GU_FALSE);
    static CV t[32 * 3];
    int n = 0;
    for (int c = 0; c < 32; c++) {
        const float bx = 20.0f + 55.0f * (c % 8), by = 12.0f + 62.0f * (c / 8);
        const float dx = c < 16 ? (c % 16) / 16.0f : 0.0f, dy = c < 16 ? 0.0f : (c % 16) / 16.0f;
        const w32 col = 0xFF000000u | (w32)(0x40 + 4 * c) | (w32)(0xC0 - 4 * c) << 8 | 0x600000u;
        for (int j = 0; j < 3; j++) {
            float x, y;
            const float *v = ANCHOR_SHAPES[1][j];
            eye_xy(bx + v[0] * 40.0f + dx, by + v[1] * 40.0f + dy, v[2], &x, &y);
            t[n++] = (CV){ col, x, y, v[2] };
        }
    }
    sceGuDrawArray(GU_TRIANGLES, FMT_CV3D, n, NULL, gumem(t, n * sizeof(CV)));
    scene_end("anchorsweep", GU_PSM_8888, 1);
    dump_depth_full("anchorsweep");
}

/* ---- version 11 -----------------------------------------------------------
 *
 * Every 3D depth plane still off is a corner depth a step off (scene 57: 6
 * of scene 48's 144 corners), and scene 45's 3840 points fit psprecomp's
 * depth arithmetic on 2883. These take the arithmetic apart. Scene 59 has
 * no divide (w = 1), so depth is the viewport's scale and centre on the
 * eye z alone. Scene 60 makes clip z / clip w the same at every eye depth
 * from -1 to -100, so every point's depth would be one value in exact
 * arithmetic and what varies is the reciprocal and the product. Scene 61
 * repeats scene 60's first batch with the eye depth made by a model matrix
 * translation, which shows whether the world and view transforms keep
 * fewer bits. Points are 96 to a row, 5 pixels apart, rows 6 apart, each
 * coloured by its index (red the low byte, green the high, blue 0x40 + the
 * batch); depth test ALWAYS, writes on; both depth dumps. */

#define VD_N 1920                         /* points a batch of scene 59 */
static CV g_vd[3840];

static void vd_load(const ScePspFMatrix4 *m) {
    sceGumMatrixMode(GU_PROJECTION);
    sceGumLoadMatrix(m);
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
    sceGumUpdateMatrix();
}

static w32 vd_colour(int k, int batch) {
    return 0xFF000000u | (w32)(k & 0xFF) | (w32)(k >> 8) << 8 | (w32)(0x40 + batch) << 16;
}

/* Scene 59: identity projection (clip = eye, w = 1). Batch 0: viewport z
 * scale and centre 32768, so depth = 32768 (1 + z); batch 1: the scale and
 * centre sceGuDepthRange(65535, 0) sets, -32768 and 32767 (it halves
 * 65535 as an integer). 1920 eye depths each from -0.99 to 0.99 in even
 * float steps. */
static void scene_depthaffine(void) {
    if (step("scene %02d: point depths with no divide, two viewport scales", g_scene)) return;
    scene_begin(GU_PSM_8888, 0xFF000000);
    static const ScePspFMatrix4 id = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };
    vd_load(&id);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuDepthFunc(GU_ALWAYS);
    sceGuDepthMask(GU_FALSE);
    for (int b = 0; b < 2; b++) {
        if (b == 0) { sceGuSendCommandf(0x44, 32768.0f); sceGuSendCommandf(0x47, 32768.0f); }
        else sceGuDepthRange(65535, 0);
        for (int k = 0; k < VD_N; k++) {
            const int i = b * VD_N + k, px = 2 + 5 * (i % 96), py = 8 + 6 * (i / 96);
            const float z = -0.99f + (float)k * (1.98f / (VD_N - 1));
            g_vd[i] = (CV){ vd_colour(k, b), (px + 0.5f - 240.0f) / 240.0f, (136.0f - (py + 0.5f)) / 136.0f, z };
        }
        sceGuDrawArray(GU_POINTS, FMT_CV3D, VD_N, NULL, gumem(&g_vd[b * VD_N], VD_N * (int)sizeof(CV)));
    }
    sceGuDepthRange(65535, 0);
    scene_end("depthaffine", GU_PSM_8888, 1);
    dump_depth_full("depthaffine");
}

/* Scenes 60 and 61's projection: clip x and y the eye's, clip z = a z,
 * clip w = -z, so clip z / clip w = -a at every depth. */
static void vd_ratio_matrix(ScePspFMatrix4 *m, float a) {
    *m = (ScePspFMatrix4){ { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, a, -1 }, { 0, 0, 0, 0 } };
}

/* 1280 eye depths from -1 to -100, even steps in 1/z, as scene 45 spaces
 * its far points. */
static float vd_depth(int k) {
    const float r0 = 1.0f, r1 = 1.0f / 100.0f;
    return -1.0f / (r0 - (float)k * ((r0 - r1) / 1279.0f));
}

/* Scene 60: the ratio projection with a = -0.3, 0.45 and -0.82 (one batch
 * each), the standard depth range, so every point of a batch would read
 * 32767 + 32768 a' with a' the GE's 16-bit a: 22936.75, 47512.5, 5897.5. */
static void scene_depthratio(void) {
    if (step("scene %02d: point depths with clip z / w fixed, eye z -1 to -100", g_scene)) return;
    scene_begin(GU_PSM_8888, 0xFF000000);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuDepthFunc(GU_ALWAYS);
    sceGuDepthMask(GU_FALSE);
    static const float A[3] = { -0.3f, 0.45f, -0.82f };
    ScePspFMatrix4 m;
    for (int b = 0; b < 3; b++) {
        vd_ratio_matrix(&m, A[b]);
        vd_load(&m);
        for (int k = 0; k < 1280; k++) {
            const int i = b * 1280 + k, px = 2 + 5 * (i % 96), py = 8 + 6 * (i / 96);
            const float z = vd_depth(k);
            g_vd[i] = (CV){ vd_colour(k, b), (px + 0.5f - 240.0f) / 240.0f * -z, (136.0f - (py + 0.5f)) / 136.0f * -z, z };
        }
        sceGuDrawArray(GU_POINTS, FMT_CV3D, 1280, NULL, gumem(&g_vd[b * 1280], 1280 * (int)sizeof(CV)));
    }
    scene_end("depthratio", GU_PSM_8888, 1);
    dump_depth_full("depthratio");
}

/* Scene 61: scene 60's first batch (a = -0.3) with each point's model z
 * its eye z plus 37.125 and the model matrix translating z by -37.125, and
 * again by 0.4375 and -0.4375: the eye depth is then the GE's own sum. */
static void scene_depthmodel(void) {
    if (step("scene %02d: scene 60's first batch with eye z from a model translation", g_scene)) return;
    scene_begin(GU_PSM_8888, 0xFF000000);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuDepthFunc(GU_ALWAYS);
    sceGuDepthMask(GU_FALSE);
    ScePspFMatrix4 m;
    vd_ratio_matrix(&m, -0.3f);
    vd_load(&m);
    static const float T[3] = { 37.125f, 0.4375f, -0.4375f };
    for (int b = 0; b < 3; b++) {
        ScePspFVector3 tr = { 0.0f, 0.0f, -T[b] };
        sceGumMatrixMode(GU_MODEL);
        sceGumLoadIdentity();
        sceGumTranslate(&tr);
        sceGumUpdateMatrix();
        for (int k = 0; k < 1280; k++) {
            const int i = b * 1280 + k, px = 2 + 5 * (i % 96), py = 8 + 6 * (i / 96);
            const float z = vd_depth(k);
            g_vd[i] = (CV){ vd_colour(k, b), (px + 0.5f - 240.0f) / 240.0f * -z, (136.0f - (py + 0.5f)) / 136.0f * -z, z + T[b] };
        }
        sceGuDrawArray(GU_POINTS, FMT_CV3D, 1280, NULL, gumem(&g_vd[b * 1280], 1280 * (int)sizeof(CV)));
    }
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
    sceGumUpdateMatrix();
    scene_end("depthmodel", GU_PSM_8888, 1);
    dump_depth_full("depthmodel");
}

/* ---- Scenes 62 to 66 (version 12): each stage read whole ------------------
 *
 * A point's depth is floor(zc + zs * clip z / clip w), the product cut to
 * the larger term's 16 significant bits first (scene 59). With zc 0 and zs a
 * power of two that puts the product between 32768 and 65536, the depth is
 * clip z / clip w's top 16 bits as the GE has them: where scenes 59 to 61
 * saw a step either side of a rounding, these read a stage's result whole.
 * Scene 62 reads 1/w, 63 a projection row's sum with w 1, 64 clip z times
 * 1/w, 65 how the world, view and projection matrices combine, and 66 the
 * probes' own perspective end to end.
 *
 * Every input is built from integer bit patterns (xorshift32 from a fixed
 * seed per scene), each matrix entry with 16 significant bits so the GE's
 * 24-bit words hold it exactly, and each scene logs a CRC of its points' eye
 * z so the analysis can check it made the same ones. Points sit two pixels
 * apart, 240 to a row from row 10; the viewport scale is 256 across and -128
 * down so a slot's ndc x and y are exact. Each is coloured by its slot (red
 * the low byte, green the high) and its batch (blue, from 0x40). */

#define RD_N 30480                          /* 240 slots a row, 127 rows */
static CV  g_rd[RD_N];
static w32 g_rdz[RD_N];
static int g_rdn;
static w32 g_rng;

static w32 rd_rand(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static float rd_f(w32 bits) { float f; memcpy(&f, &bits, 4); return f; }

/* (-1)^s 2^e (1 + m / 2^23). */
static float rd_mk(int s, int e, w32 m) {
    return rd_f((w32)(s & 1) << 31 | (w32)(e + 127) << 23 | (m & 0x7FFFFFu));
}

/* The same with 16 significant bits: m15 the 15 below the leading one. */
static float rd_mk16(int s, int e, w32 m15) { return rd_mk(s, e, (m15 & 0x7FFFu) << 8); }

static float rd_ndcx(int i) { return ((float)(2 * (i % 240)) + 0.5f - 240.0f) / 256.0f; }
static float rd_ndcy(int i) { return (136.0f - (float)(10 + 2 * (i / 240)) - 0.5f) / 128.0f; }

static void rd_put(int batch, float x, float y, float z) {
    const int i = g_rdn++;
    g_rd[i] = (CV){ 0xFF000000u | (w32)(i & 0xFF) | (w32)((i >> 8) & 0xFF) << 8 | (w32)(0x40 + batch) << 16, x, y, z };
    memcpy(&g_rdz[i], &z, 4);
}

/* The next slot's point at eye z z, placed for clip w w by a projection
 * whose x and y rows are the eye's x and y. */
static void rd_at(int batch, float w, float z) {
    const int i = g_rdn;
    rd_put(batch, rd_ndcx(i) * w, rd_ndcy(i) * w, z);
}

/* World, view and projection, each the identity but for the z row:
 * world z = rx x + s z + t, eye z = v (world z) + u, clip z = a (eye z) + b,
 * clip w = wz (eye z) + ww. */
static void rd_load(float a, float b, float wz, float ww, float v, float u, float rx, float s, float t) {
    const ScePspFMatrix4 P = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, a, wz }, { 0, 0, b, ww } };
    const ScePspFMatrix4 V = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, v, 0 }, { 0, 0, u, 1 } };
    const ScePspFMatrix4 W = { { 1, 0, rx, 0 }, { 0, 1, 0, 0 }, { 0, 0, s, 0 }, { 0, 0, t, 1 } };
    sceGumMatrixMode(GU_PROJECTION);
    sceGumLoadMatrix(&P);
    sceGumMatrixMode(GU_VIEW);
    sceGumLoadMatrix(&V);
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadMatrix(&W);
    sceGumUpdateMatrix();
}

/* A projection row alone: clip z = cx x + cy y + a z + b, clip w = wz z + ww. */
static void rd_proj(float cx, float cy, float a, float b, float wz, float ww) {
    const ScePspFMatrix4 P = { { 1, 0, cx, 0 }, { 0, 1, cy, 0 }, { 0, 0, a, wz }, { 0, 0, b, ww } };
    sceGumMatrixMode(GU_PROJECTION);
    sceGumLoadMatrix(&P);
    sceGumMatrixMode(GU_VIEW);
    sceGumLoadIdentity();
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
    sceGumUpdateMatrix();
}

/* Viewport z: scale zs, centre 0. */
static void rd_zview(float zs) {
    sceGuSendCommandf(0x44, zs);
    sceGuSendCommandf(0x47, 0.0f);
}

static void rd_draw(int from) {
    const int n = g_rdn - from;
    if (n > 0) sceGuDrawArray(GU_POINTS, FMT_CV3D, n, NULL, gumem(&g_rd[from], n * (int)sizeof(CV)));
}

static void rd_begin(void) {
    scene_begin(GU_PSM_8888, 0xFF000000);
    sceGuViewport(2048, 2048, 512, 256);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuDepthFunc(GU_ALWAYS);
    sceGuDepthMask(GU_FALSE);
    g_rdn = 0;
}

static void rd_end(const char *name) {
    out("  %d points, eye z crc %08X\n", g_rdn, crc32(g_rdz, g_rdn * 4));
    scene_end(name, GU_PSM_8888, 0);
    dump_depth_full(name);
}

/* A 24-bit significand, never 0. */
static w32 rd_m24(void) {
    w32 m;
    do m = rd_rand() & 0x7FFFFFu; while (!m);
    return m;
}

/* 1024 clip w in (2, 4), which scene 62 reads 1/w of and scene 64 multiplies. */
static float g_w64[1024];
static void rd_make_w64(void) {
    g_rng = 0x64000001u;
    for (int j = 0; j < 1024; j++) g_w64[j] = rd_mk(0, 1, rd_m24());
}

/* Scene 62: clip z c = 2^e, clip w = -z in [2^e, 2^(e+1)), so the depth is
 * 65536 c / w: 1/w's top 16 bits. Batch 0 is w in (1, 2): the 127 w with
 * 8-bit significands, 4096 with random 16-bit ones, 2048 with random 24-bit
 * ones, and two runs of 256 consecutive 24-bit ones. Batches 1 to 8 are the
 * binades from 2^-3 to 2^13, 192 random w each, but 2^1's, scene 64's 1024. */
static void scene_rcpread(void) {
    if (step("scene %02d: 1/w read whole: clip z a power of two, depth 1/w's top 16 bits", g_scene)) return;
    rd_make_w64();
    rd_begin();
    rd_zview(65536.0f);
    g_rng = 0x62000001u;
    rd_proj(0, 0, 0, 1.0f, -1.0f, 0);
    int from = g_rdn;
    for (int j = 1; j < 128; j++) { const float w = rd_mk(0, 0, (w32)j << 16); rd_at(0, w, -w); }
    for (int j = 0; j < 4096; j++) {
        w32 m;
        do m = (rd_rand() & 0x7FFFu) << 8; while (!m);
        const float w = rd_mk(0, 0, m);
        rd_at(0, w, -w);
    }
    for (int j = 0; j < 2048; j++) { const float w = rd_mk(0, 0, rd_m24()); rd_at(0, w, -w); }
    for (int j = 0; j < 256; j++) { const float w = rd_mk(0, 0, 0x400000u + (w32)j); rd_at(0, w, -w); }
    for (int j = 0; j < 256; j++) { const float w = rd_mk(0, 0, 0x155500u + (w32)j); rd_at(0, w, -w); }
    rd_draw(from);
    static const int E[8] = { -3, -1, 1, 2, 4, 7, 10, 13 };
    for (int b = 0; b < 8; b++) {
        rd_proj(0, 0, 0, rd_mk(0, E[b], 0), -1.0f, 0);
        from = g_rdn;
        if (E[b] == 1)
            for (int j = 0; j < 1024; j++) rd_at(1 + b, g_w64[j], -g_w64[j]);
        else
            for (int j = 0; j < 192; j++) { const float w = rd_mk(0, E[b], rd_m24()); rd_at(1 + b, w, -w); }
        rd_draw(from);
    }
    rd_end("rcpread");
}

/* Scene 63: clip w 1, so the depth is zs times clip z, read whole. Batch 0:
 * clip z = z, z in [0.5, 1), as the GE takes z in. 1-4: z - 1 for z = 1 + d,
 * d in [2^e, 2^(e+1)), e -2, -9, -16 and -21: how much of z reaches the
 * sum. 5-12: a z, a in [1, 1.25), z in [0.5, 0.75), half of them with 16
 * significant bits. 13-24: z + b, b in [5/8, 3/4), z of either sign in
 * [2^-g, 2^(1-g)) for g 4, 7, 11, 15, 17 and 19: the smaller term's
 * alignment. 25: a carry, b in [7/16, 1/2), z in [1/16, 1/8). 26: z = -(b -
 * k 2^-23), b in [1.5, 2), k in [2^18, 2^19): a cancellation. 27-30: four
 * terms, cx x + cy y + a z + b, each a different size: the order they are
 * summed in. Clip z stays below 1, inside the clip volume. */
static void scene_dotread(void) {
    if (step("scene %02d: a projection row read whole: clip w 1, depth clip z's top 16 bits", g_scene)) return;
    rd_begin();
    g_rng = 0x63000001u;
    int from, b = 0;
    w32 r;
    rd_proj(0, 0, 1.0f, 0, 0, 1.0f);
    rd_zview(65536.0f);
    from = g_rdn;
    for (int j = 0; j < 1024; j++) rd_at(b, 1.0f, rd_mk(0, -1, rd_rand()));
    rd_draw(from);
    b++;
    static const int DE[4] = { -2, -9, -16, -21 };
    rd_proj(0, 0, 1.0f, -1.0f, 0, 1.0f);
    for (int q = 0; q < 4; q++, b++) {
        rd_zview(rd_mk(0, 15 - DE[q], 0));
        const w32 lo = 1u << (23 + DE[q]);
        from = g_rdn;
        for (int j = 0; j < 256; j++) rd_at(b, 1.0f, rd_mk(0, 0, lo | (rd_rand() & (lo - 1))));
        rd_draw(from);
    }
    rd_zview(65536.0f);
    for (int q = 0; q < 8; q++, b++) {
        r = rd_rand();
        rd_proj(0, 0, rd_mk16(0, 0, r & 0x1FFFu), 0, 0, 1.0f);
        from = g_rdn;
        for (int j = 0; j < 256; j++) {
            r = rd_rand();
            rd_at(b, 1.0f, rd_mk(0, -1, j < 128 ? (r & 0x3FFFu) << 8 : r & 0x3FFFFFu));
        }
        rd_draw(from);
    }
    static const int G[6] = { 4, 7, 11, 15, 17, 19 };
    for (int q = 0; q < 12; q++, b++) {
        r = rd_rand();
        rd_proj(0, 0, 1.0f, rd_mk16(0, -1, 0x2000u | (r & 0x1FFFu)), 0, 1.0f);
        from = g_rdn;
        for (int j = 0; j < 128; j++) rd_at(b, 1.0f, rd_mk(q & 1, -G[q >> 1], rd_rand()));
        rd_draw(from);
    }
    r = rd_rand();
    rd_proj(0, 0, 1.0f, rd_mk16(0, -2, 0x6000u | (r & 0x1FFFu)), 0, 1.0f);
    from = g_rdn;
    for (int j = 0; j < 128; j++) rd_at(b, 1.0f, rd_mk(0, -4, rd_rand()));
    rd_draw(from);
    b++;
    rd_zview(1048576.0f);
    r = rd_rand();
    const w32 mb = (0x4000u | (r & 0x3FFFu)) << 8;
    rd_proj(0, 0, 1.0f, rd_mk(0, 0, mb), 0, 1.0f);
    from = g_rdn;
    for (int j = 0; j < 128; j++) rd_at(b, 1.0f, rd_mk(1, 0, mb - ((1u << 18) | (rd_rand() & 0x3FFFFu))));
    rd_draw(from);
    b++;
    rd_zview(65536.0f);
    static const signed char T4[4][4] = { { -6, -8, 0, -5 }, { -4, -10, -2, -4 }, { -13, -5, 0, -6 }, { -7, -7, 1, -7 } };
    for (int q = 0; q < 4; q++, b++) {
        r = rd_rand(); const float cx = rd_mk16((int)(r & 1), T4[q][0], r >> 1);
        r = rd_rand(); const float cy = rd_mk16((int)(r & 1), T4[q][1], r >> 1);
        r = rd_rand(); const float a = rd_mk16(0, T4[q][2], r);
        r = rd_rand(); const float c = rd_mk16(0, -1, 0x1000u + (r & 0x1FFFu));
        rd_proj(cx, cy, a, c, 0, 1.0f);
        from = g_rdn;
        for (int j = 0; j < 512; j++) rd_at(b, 1.0f, rd_mk(0, T4[q][3], rd_rand()));
        rd_draw(from);
    }
    rd_end("dotread");
}

/* Scene 64: clip z b in [1.5, 2), clip w = -z scene 62's 1024 w in (2, 4):
 * the depth is 65536 b / w (most of them; the rest, below 1/2, 32768ths),
 * the product of b and 1/w as the GE forms it. Four b. */
static void scene_mulread(void) {
    if (step("scene %02d: clip z times 1/w read whole, for scene 62's w in (2, 4)", g_scene)) return;
    rd_make_w64();
    rd_begin();
    rd_zview(65536.0f);
    g_rng = 0x64000002u;
    for (int q = 0; q < 4; q++) {
        const w32 r = rd_rand();
        rd_proj(0, 0, 0, rd_mk16(0, 0, 0x4000u | (r & 0x3FFFu)), -1.0f, 0);
        const int from = g_rdn;
        for (int j = 0; j < 1024; j++) rd_at(q, g_w64[j], -g_w64[j]);
        rd_draw(from);
    }
    rd_end("mulread");
}

/* Scene 65: clip w 1, clip z = a (eye z) + b with world and view z rows of
 * their own, so the depth is clip z read whole however the GE combines
 * them. 0: a world translation T in [40, 48) and z = -(T - d), d in
 * [1/2, 3/4): one eye z the size of d, from two terms 60 times larger
 * (scene 61's batch 0, read whole). 1: a view translation U in -[16, 24) as
 * well. 2: three z scales, a, view and world. 3: world z = rx x + z. 4:
 * clip z = a (z + T) - T. 5: batch 0 with T in [1/4, 1/2). Clip z stays
 * in [1/2, 1) but in batch 3. */
static void scene_matread(void) {
    if (step("scene %02d: world, view and projection combined: clip w 1, depth clip z's top 16 bits", g_scene)) return;
    rd_begin();
    rd_zview(65536.0f);
    g_rng = 0x65000001u;
    int from;
    w32 r;
    float a, T, U, v, s, rx;
    r = rd_rand(); a = rd_mk16(0, 0, r & 0x1FFFu);
    r = rd_rand(); T = rd_mk16(0, 5, 0x2000u | (r & 0x1FFFu));
    rd_load(a, 0, 0, 1.0f, 1.0f, 0, 0, 1.0f, T);
    from = g_rdn;
    for (int j = 0; j < 512; j++) { const float d = rd_mk(0, -1, rd_rand() & 0x3FFFC0u); rd_at(0, 1.0f, -(T - d)); }
    rd_draw(from);
    r = rd_rand(); a = rd_mk16(0, 0, r & 0x1FFFu);
    r = rd_rand(); T = rd_mk16(0, 5, 0x2000u | (r & 0x1FFFu));
    r = rd_rand(); U = rd_mk16(1, 4, r & 0x3FFFu);
    rd_load(a, 0, 0, 1.0f, 1.0f, U, 0, 1.0f, T);
    from = g_rdn;
    for (int j = 0; j < 512; j++) { const float d = rd_mk(0, -1, rd_rand() & 0x3FFFE0u); rd_at(1, 1.0f, -((T + U) - d)); }
    rd_draw(from);
    r = rd_rand(); a = rd_mk16(0, 0, r & 0xFFFu);
    r = rd_rand(); v = rd_mk16(0, -1, r & 0xFFFu);
    r = rd_rand(); s = rd_mk16(0, 0, r & 0xFFFu);
    rd_load(a, 0, 0, 1.0f, v, 0, 0, s, 0);
    from = g_rdn;
    for (int j = 0; j < 512; j++) rd_at(2, 1.0f, rd_mk(0, 0, rd_rand() & 0x1FFFFFu));
    rd_draw(from);
    r = rd_rand(); a = rd_mk16(0, 0, r & 0xFFFu);
    r = rd_rand(); rx = rd_mk16((int)(r & 1), -5, r >> 1);
    rd_load(a, 0, 0, 1.0f, 1.0f, 0, rx, 1.0f, 0);
    from = g_rdn;
    for (int j = 0; j < 512; j++) rd_at(3, 1.0f, rd_mk(0, -1, rd_rand() & 0x1FFFFFu));
    rd_draw(from);
    r = rd_rand(); a = rd_mk16(0, 0, r & 0x3FFu);
    r = rd_rand(); T = rd_mk16(0, 3, r & 0xFFFu);
    rd_load(a, -T, 0, 1.0f, 1.0f, 0, 0, 1.0f, T);
    from = g_rdn;
    for (int j = 0; j < 512; j++) rd_at(4, 1.0f, rd_mk(0, -1, rd_rand() & 0x1FFFFFu));
    rd_draw(from);
    r = rd_rand(); a = rd_mk16(0, 2, r & 0x1FFFu);
    r = rd_rand(); T = rd_mk16(0, -2, r);
    rd_load(a, 0, 0, 1.0f, 1.0f, 0, 0, 1.0f, T);
    from = g_rdn;
    for (int j = 0; j < 512; j++) { const float d = rd_mk16(0, -3, rd_rand() & 0x3FFFu); rd_at(5, 1.0f, -(T - d)); }
    rd_draw(from);
    rd_end("matread");
}

/* Scene 66: the probes' perspective (60 degrees, 1 to 100), read whole.
 * Batch 0: eye z from -4 to -96, ndc z in [1/2, 1); batch 1: eye z from
 * -1.016 to -1.266, ndc z in -[1/2, 1), zs -65536. */
static void scene_perspread(void) {
    if (step("scene %02d: the probes' perspective read whole: ndc z's top 16 bits", g_scene)) return;
    rd_begin();
    sceGumMatrixMode(GU_PROJECTION);
    sceGumLoadIdentity();
    sceGumPerspective(60.0f, 480.0f / 272.0f, 1.0f, 100.0f);
    sceGumMatrixMode(GU_VIEW);
    sceGumLoadIdentity();
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
    sceGumUpdateMatrix();
    g_rng = 0x66000001u;
    const float kx = 0.981491089f, ky = 1.73205078f;    /* the projection's x and y scales */
    rd_zview(65536.0f);
    int from = g_rdn;
    for (int e = 2; e <= 6; e++)
        for (int j = 0; j < (e < 6 ? 192 : 256); j++) {
            const float w = rd_mk(0, e, e < 6 ? rd_rand() : rd_rand() & 0x3FFFFFu);
            const int i = g_rdn;
            rd_put(0, rd_ndcx(i) * w / kx, rd_ndcy(i) * w / ky, -w);
        }
    rd_draw(from);
    rd_zview(-65536.0f);
    from = g_rdn;
    for (int j = 0; j < 1024; j++) {
        const float w = rd_mk(0, 0, 0x20000u + (rd_rand() & 0x1FFFFFu));
        const int i = g_rdn;
        rd_put(1, rd_ndcx(i) * w / kx, rd_ndcy(i) * w / ky, -w);
    }
    rd_draw(from);
    rd_end("perspread");
}

/* ---- geprobe 13: patch positions read through depth (scenes 67-82) --------
 *
 * Every scene step is a command stream that patch13.py writes into
 * patch13_data.inc: the matrices, viewport z, patch division and primitive,
 * a record per item, every control vertex blob exactly as the GE is to read
 * it (float, s16, s8, indexed, morphed, skinned), the cal band's and the
 * anchors' plain points, and scene 75's colour read-back. p13_run() replays
 * one; it computes nothing of its own but the read-back, so the streams are
 * the inputs, and each step logs their counts and CRCs for patch13.py to
 * check its copy against. Every blob is copied into the display list
 * (gumem); a list that passes 448 KB is run and a new one started, which
 * the GE's state outlives. In scenes 76-82 each batch is its own list and
 * logs its GE time and the list's headroom before it; in 67-75 the step
 * logs one GE time. */
#include "patch13_data.inc"

#define P13_LIST_BYTES (512 * 1024)
#define P13_FLUSH_AT   (448 * 1024)

static w32 crc32_more(w32 crc, const void *p, int n) {
    const unsigned char *b = p;
    w32 c = ~crc;
    for (int i = 0; i < n; i++) {
        c ^= b[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static float p13_f(w32 w) { float f; memcpy(&f, &w, 4); return f; }

static void p13_load(int mode, const w32 *w) {
    ScePspFMatrix4 m;
    memcpy(&m, w, sizeof m);
    sceGumMatrixMode(mode);
    sceGumLoadMatrix(&m);
}

static w32 g_p13_us;       /* GE time since the last report */
static int g_p13_flushes;

/* Run the list built so far and start the next; the GE's state carries over. */
static void p13_flush(void) {
    const w32 t0 = sceKernelGetSystemTimeLow();
    sceGuFinish();
    ge_wait();
    g_p13_us += sceKernelGetSystemTimeLow() - t0;
    sceGuStart(GU_DIRECT, g_list);
}

static void p13_room(void) {
    if (sceGuCheckList() > P13_FLUSH_AT) { p13_flush(); g_p13_flushes++; }
}

/* Counts and CRCs of a whole stream, before it runs. */
static void p13_sums(const w32 *s, int n, int *items, int *samples, w32 *cc, w32 *ic) {
    *items = *samples = 0;
    *cc = *ic = 0;
    for (int i = 0; i < n; i += 1 + (int)(s[i] & 0xFFFFFF)) {
        const w32 *a = s + i + 1;
        switch (s[i] >> 24) {
        case P13_ITEM:   *ic = crc32_more(*ic, a, 16); (*items)++; *samples += (int)a[4]; break;
        case P13_BEZIER: *cc = crc32_more(*cc, a + 5, (int)(a[3] + a[4])); break;
        case P13_SPLINE: *cc = crc32_more(*cc, a + 7, (int)(a[5] + a[6])); break;
        case P13_DRAW:   if (a[4]) *samples += (int)a[2]; break;
        }
    }
}

/* Scene 75 b9: run the list, read each A sample's colour from the 3 x 3
 * pixels around its exact position, then draw the C cells as plain triangle
 * strips with those colours. */
static void p13_b9(const w32 *a) {
    static CV v[2 * 8];
    static w32 col[8][64];
    const w32 t0 = sceKernelGetSystemTimeLow();
    sceGuFinish();
    ge_wait();
    g_p13_us += sceKernelGetSystemTimeLow() - t0;
    const volatile w32 *fb = (const volatile w32 *)VRAM_UNCACHED;
    const int nshape = (int)a[0];
    const w32 *p = a + 1;
    for (int sh = 0; sh < nshape && sh < 8; sh++) {
        const int nu = (int)p[0], nv = (int)p[1];
        const w32 *smp = p + 2;
        for (int k = 0; k < nu * nv; k++) {
            const int px = (int)(smp[4 * k] & 0xFFFF), py = (int)(smp[4 * k] >> 16);
            w32 c = 0;
            int found = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    const int x = px + dx, y = py + dy;
                    if (x < 0 || x >= SCR_W || y < 0 || y >= SCR_H) continue;
                    const w32 q = fb[y * FB_W + x];   /* alpha is the stencil: compare RGB */
                    if (q & 0xFFFFFFu) { if (!found) c = q; found++; }
                }
            if (!found) out("  b9 miss %d %d %d\n", sh, k % nu, k / nu);
            else if (found > 1) out("  b9 %d pixels lit around %d %d %d\n", found, sh, k % nu, k / nu);
            col[sh][k] = c;
        }
        out("  b9 shape %d (%d x %d) colours:", sh, nu, nv);
        for (int k = 0; k < nu * nv; k++) out("%s %08X", k && k % 12 == 0 ? "\n   " : "", col[sh][k]);
        out("\n");
        p += 2 + 4 * nu * nv;
    }
    sceKernelDcacheWritebackAll();
    sceGuStart(GU_DIRECT, g_list);
    p = a + 1;
    for (int sh = 0; sh < nshape && sh < 8; sh++) {
        const int nu = (int)p[0], nv = (int)p[1];
        const w32 *smp = p + 2;
        for (int j = 0; j + 1 < nv; j++) {
            for (int i = 0; i < nu; i++)
                for (int h = 0; h < 2; h++) {
                    const w32 *q = smp + 4 * ((j + h) * nu + i);
                    const w32 c = col[sh][(j + h) * nu + i];
                    v[2 * i + h] = (CV){ c ? (c | 0xFF000000u) : 0, p13_f(q[1]), p13_f(q[2]), p13_f(q[3]) };
                }
            p13_room();
            sceGuDrawArray(GU_TRIANGLE_STRIP, FMT_CV3D, 2 * nu, NULL, gumem(v, 2 * nu * (int)sizeof(CV)));
        }
        p += 2 + 4 * nu * nv;
    }
}

static void p13_run(const struct p13_step *st) {
    g_scene = st->scene;
    if (step("scene %02d: %s", st->scene, st->title)) return;
    const w32 *s = st->s;
    int items, samples, timing = 0, batch = -1;
    w32 cc, ic;
    p13_sums(s, st->n, &items, &samples, &cc, &ic);
    const w32 sc = crc32(s, st->n * 4);
    g_p13_us = 0;
    g_p13_flushes = 0;
    for (int i = 0; i < st->n; i += 1 + (int)(s[i] & 0xFFFFFF)) {
        const w32 *a = s + i + 1;
        switch (s[i] >> 24) {
        case P13_BEGIN:
            scene_begin(GU_PSM_8888, 0xFF000000);
            sceGuViewport(2048, 2048, (int)a[0], (int)a[1]);
            sceGuEnable(GU_DEPTH_TEST);
            sceGuDepthFunc(GU_ALWAYS);
            sceGuDepthMask(GU_FALSE);
            sceGuPatchPrim(GU_POINTS);
            sceGuMorphWeight(0, 1.0f);
            for (int k = 1; k < 8; k++) sceGuMorphWeight(k, 0.0f);
            timing = (int)a[2];
            break;
        case P13_MATS:
            if (a[0] & 1) p13_load(GU_PROJECTION, a + 1);
            if (a[0] & 2) p13_load(GU_VIEW, a + 1 + 16 * !!(a[0] & 1));
            if (a[0] & 4) p13_load(GU_MODEL, a + 1 + 16 * (!!(a[0] & 1) + !!(a[0] & 2)));
            sceGumMatrixMode(GU_MODEL);
            sceGumUpdateMatrix();
            break;
        case P13_ZVIEW:
            sceGuSendCommandf(0x44, p13_f(a[0]));
            sceGuSendCommandf(0x47, p13_f(a[1]));
            break;
        case P13_DIVIDE:    sceGuPatchDivide(a[0], a[1]); break;
        case P13_PATCHPRIM: sceGuPatchPrim((int)a[0]); break;
        case P13_SHADE:     sceGuShadeModel((int)a[0]); break;
        case P13_BLEND:
            if (a[0]) {
                sceGuEnable(GU_BLEND);
                sceGuBlendFunc((int)a[1], (int)a[2], (int)a[3], a[4], a[5]);
            } else {
                sceGuDisable(GU_BLEND);
            }
            break;
        case P13_MORPH:
            for (int k = 0; k < 8; k++) sceGuMorphWeight(k, p13_f(a[k]));
            break;
        case P13_BONE: {
            ScePspFMatrix4 m;
            memcpy(&m, a + 1, sizeof m);
            sceGuBoneMatrix(a[0], &m);
            break;
        }
        case P13_ITEM: break;
        case P13_BEZIER: {
            p13_room();
            unsigned char *v = gumem(a + 5, (int)(a[3] + a[4]));
            sceGuDrawBezier((int)a[0], (int)a[1], (int)a[2], a[4] ? v + a[3] : NULL, v);
            break;
        }
        case P13_SPLINE: {
            p13_room();
            unsigned char *v = gumem(a + 7, (int)(a[5] + a[6]));
            sceGuDrawSpline((int)a[0], (int)a[1], (int)a[2], (int)a[3], (int)a[4], a[6] ? v + a[5] : NULL, v);
            break;
        }
        case P13_DRAW:
            p13_room();
            sceGuDrawArray((int)a[0], (int)a[1], (int)a[2], NULL, gumem(a + 5, (int)a[3]));
            break;
        case P13_BATCH:
            if (timing) {
                if (batch >= 0) {
                    p13_flush();
                    out("  b%d GE %u us\n", batch, (unsigned)g_p13_us);
                    g_p13_us = 0;
                }
                out("  b%d list headroom %d bytes\n", (int)a[0], P13_LIST_BYTES - sceGuCheckList());
            }
            batch = (int)a[0];
            break;
        case P13_CALDONE:
            out("  %d items, %d samples, control crc %08X, item crc %08X, stream crc %08X\n",
                items, samples, cc, ic, sc);
            break;
        case P13_B9: p13_b9(a); break;
        case P13_END:
            sceGuPatchPrim(GU_TRIANGLE_STRIP);
            sceGuPatchDivide(8, 8);
            sceGuDisable(GU_BLEND);
            sceGuMorphWeight(0, 1.0f);
            for (int k = 1; k < 8; k++) sceGuMorphWeight(k, 0.0f);
            {
                ScePspFMatrix4 id;
                memset(&id, 0, sizeof id);
                id.x.x = id.y.y = id.z.z = id.w.w = 1.0f;
                sceGuBoneMatrix(0, &id);
            }
            sceGumMatrixMode(GU_VIEW);
            sceGumLoadIdentity();
            sceGumMatrixMode(GU_MODEL);
            sceGumLoadIdentity();
            sceGumUpdateMatrix();
            sceGuShadeModel(GU_SMOOTH);
            p13_flush();
            if (timing) out("  b%d GE %u us\n", batch, (unsigned)g_p13_us);
            else out("  GE %u us\n", (unsigned)g_p13_us);
            if (g_p13_flushes) out("  %d list flush(es) at 448 KB\n", g_p13_flushes);
            scene_end(st->name, GU_PSM_8888, 0);
            dump_depth_full(st->name);
            break;
        default:
            out("  bad stream word %d: %08X\n", i, s[i]);
            return;
        }
    }
}

/* ---- geprobe 14: colour planes, scenes 83-98 ----------------------------
 *
 * Every scene is a u32 command stream from colour14.py (c14_data.inc), in
 * the pattern of patch13.py: the C side draws nothing of its own. Each word
 * is (op << 24) | nargs, then the args. The second command of every stream
 * is SUMS, which logs the draw count, vertex count, the CRC of all vertex
 * bytes and the stream CRC, so readout can confirm the tables before
 * reading a pixel. Place after p13_run (it uses p13_room/p13_flush, gumem,
 * scene_begin/scene_end, dump_depth_full, crc32). */
#include "c14_data.inc"
#include "c15_data.inc"      /* geprobe 15: line colours, scenes 99-109, from lines15.py */
#include "c16_data.inc"      /* geprobe 16: skinning and morphing, scenes 110-114, from skin16.py */
#include "c20_data.inc"      /* geprobe 20: long edges, scenes 125-127, from edges20.py */
#include "c21_data.inc"      /* geprobe 21: tall long edges, scenes 128-129, from edges21.py */
#include "c21p_data.inc"     /* geprobe 21: patch line order, scenes 130-133, from plines21.py */
#include "c22e_data.inc"     /* geprobe 22: long edges' order and extent, scene 134, from edges22.py */
#include "c22t_data.inc"     /* geprobe 22: patch triangle order, scenes 135-137, from ptris22.py */
#include "c22n_data.inc"     /* geprobe 22: skinned and morphed normals, scene 138, from norms22.py */
#include "c22m_data.inc"     /* geprobe 22: morphed colours and coordinates, scenes 139-140, from morph22.py */
#include "c22c_data.inc"     /* geprobe 22: past the clip volume, scene 141, from clip22.py */

static w32 g_c14_tex[16 * 16] __attribute__((aligned(16)));   /* all 0xFF000000 */
static w32 g_c14_vb[8192] __attribute__((aligned(16)));        /* LADDER's vertices */

static float c14_f(w32 w) { float f; memcpy(&f, &w, 4); return f; }

/* Scene 95's lighting. mode 1: light 0 directional along +Z, white diffuse
 * and specular, no ambient anywhere, vertex colour as diffuse and specular
 * material, separate specular. mode 2: the same with specular light 0
 * (primary alone). mode 0: lighting off. */
static void c14_light(int mode, float coef) {
    if (!mode) {
        sceGuDisable(GU_LIGHT0);
        sceGuDisable(GU_LIGHTING);
        sceGuColorMaterial(0);
        sceGuLightMode(GU_SINGLE_COLOR);
        return;
    }
    ScePspFVector3 dir = { 0.0f, 0.0f, 1.0f };
    sceGuEnable(GU_LIGHTING);
    sceGuLightMode(GU_SEPARATE_SPECULAR_COLOR);
    sceGuAmbient(0xFF000000);
    sceGuModelColor(0x000000, 0x000000, 0xFFFFFF, 0xFFFFFF);
    sceGuAmbientColor(0xFF000000);
    sceGuColorMaterial(GU_DIFFUSE | GU_SPECULAR);
    sceGuSpecular(coef);
    sceGuEnable(GU_LIGHT0);
    sceGuLight(0, GU_DIRECTIONAL, GU_DIFFUSE_AND_SPECULAR, &dir);
    sceGuLightColor(0, GU_AMBIENT, 0x000000);
    sceGuLightColor(0, GU_DIFFUSE, 0xFFFFFF);
    sceGuLightColor(0, GU_SPECULAR, mode == 1 ? 0xFFFFFF : 0x000000);
}

/* geprobe 17's lighting: light 0 only, white diffuse, no ambient anywhere,
 * white diffuse and specular material, one colour; attenuation off and a
 * specular coefficient of 1 until LGT sets them. */
static void l17_lights_on(void) {
    sceGuEnable(GU_LIGHTING);
    sceGuLightMode(GU_SINGLE_COLOR);
    sceGuAmbient(0xFF000000);
    sceGuColorMaterial(0);
    sceGuModelColor(0x000000, 0x000000, 0xFFFFFF, 0xFFFFFF);
    sceGuAmbientColor(0xFF000000);
    sceGuEnable(GU_LIGHT0);
    sceGuLightColor(0, GU_AMBIENT, 0x000000);
    sceGuLightColor(0, GU_DIFFUSE, 0xFFFFFF);
    sceGuLightColor(0, GU_SPECULAR, 0x000000);
    sceGuLightAtt(0, 1.0f, 0.0f, 0.0f);
    sceGuSpecular(1.0f);
}

static void c14_tex(int on) {
    if (!on) { sceGuDisable(GU_TEXTURE_2D); return; }
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexImage(0, 16, 16, 16, g_c14_tex);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
    sceGuTexFlush();
}

/* geprobe 22's coordinate texture: 256 x 256, texel (s, t) red s, green t, blue
 * 0x80, so a point's colour names the texel its coordinates fell in. TEXC turns it
 * on with a scale and offset (repeating, nearest, replacing), or off. */
static w32 g_c22_tex[256 * 256] __attribute__((aligned(16)));
static void c22_texc(const w32 *a) {
    if (!a[0]) {
        c14_tex(0);
        sceGuTexScale(1.0f, 1.0f);
        sceGuTexOffset(0.0f, 0.0f);
        return;
    }
    if (!g_c22_tex[0]) {
        for (int t = 0; t < 256; t++)
            for (int s = 0; s < 256; s++) g_c22_tex[t * 256 + s] = 0xFF800000u | (w32)t << 8 | (w32)s;
        sceKernelDcacheWritebackAll();
    }
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexImage(0, 256, 256, 256, g_c22_tex);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuTexWrap(GU_REPEAT, GU_REPEAT);
    sceGuTexMapMode(GU_TEXTURE_COORDS, 0, 0);
    sceGuTexScale(c14_f(a[1]), c14_f(a[2]));
    sceGuTexOffset(c14_f(a[3]), c14_f(a[4]));
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
    sceGuTexFlush();
}

static void c14_run(const struct c14_step *st) {
    g_scene = st->scene;
    if (step("scene %02d: %s", st->scene, st->title)) return;
    const w32 *s = st->s;
    const w32 sc = crc32(s, st->n * 4);
    int psm = GU_PSM_8888;
    g_p13_us = 0;
    g_p13_flushes = 0;
    for (int i = 0; i < 256; i++) g_c14_tex[i] = 0xFF000000u;
    sceKernelDcacheWritebackAll();
    for (int i = 0; i < st->n; i += 1 + (int)(s[i] & 0xFFFFFF)) {
        const w32 *a = s + i + 1;
        switch (s[i] >> 24) {
        case C14_NOP: break;
        case C14_BEGIN:
            psm = (int)a[0];
            scene_begin(psm, a[1]);
            break;
        case C14_SUMS: {
            /* the CRC of every DRAW/LADDER's vertex bytes, in stream order */
            w32 vc = 0;
            for (int j = 0; j < st->n; j += 1 + (int)(s[j] & 0xFFFFFF)) {
                const w32 op = s[j] >> 24;
                if (op == C14_DRAW || op == C14_LADDER) vc = crc32_more(vc, s + j + 1 + (op == C14_DRAW ? 5 : 6), (int)s[j + 4]);
                if (op == C14_PATCH) vc = crc32_more(vc, s + j + 11, (int)s[j + 10]);   /* geprobe 21 */
            }
            out("  %u draws, %u vertices, vertex crc %08X, stream crc %08X\n",
                (unsigned)a[0], (unsigned)a[1], vc, sc);
            if (vc != a[2]) out("  vertex crc differs from the table's %08X\n", a[2]);
            break;
        }
        case C14_SCISSOR:
            sceGuScissor((int)a[0], (int)a[1], (int)(a[2] - a[0] + 1), (int)(a[3] - a[1] + 1));
            break;
        case C14_SHADE: sceGuShadeModel((int)a[0]); break;
        case C14_DRAW: {
            p13_room();
            const int vb = (int)a[3], ib = (int)a[4];
            unsigned char *v = gumem(a + 5, vb + ib);
            sceGuDrawArray((int)a[0], (int)a[1], (int)a[2], ib ? v + vb : NULL, v);
            break;
        }
        case C14_MATS:
            p13_load(GU_PROJECTION, a + 1);
            p13_load(GU_VIEW, a + 17);
            p13_load(GU_MODEL, a + 33);
            sceGumMatrixMode(GU_MODEL);
            sceGumUpdateMatrix();
            break;
        case C14_VIEWPORT: sceGuViewport((int)a[0], (int)a[1], (int)a[2], (int)a[3]); break;
        case C14_OFFSET:   sceGuOffset(a[0], a[1]); break;
        case C14_ALPHA:
            if (a[0]) { sceGuEnable(GU_ALPHA_TEST); sceGuAlphaFunc((int)a[1], (int)a[2], (int)a[3]); }
            else sceGuDisable(GU_ALPHA_TEST);
            break;
        case C14_STENCIL:
            if (a[0]) {
                sceGuEnable(GU_STENCIL_TEST);
                sceGuStencilFunc(GU_ALWAYS, 0, 0xFF);
                sceGuStencilOp(GU_KEEP, GU_KEEP, GU_INCR);
            } else sceGuDisable(GU_STENCIL_TEST);
            break;
        case C14_FOG:
            if (a[0]) { sceGuEnable(GU_FOG); sceGuFog(c14_f(a[1]), c14_f(a[2]), a[3]); }
            else sceGuDisable(GU_FOG);
            break;
        case C14_LADDER: {
            /* Draw once with the alpha and stencil tests off, then pass r =
             * first..last with alpha test GEQUAL r and stencil INCR on pass:
             * the alpha byte (the stencil) counts the passes, i.e. it is the
             * alpha plane's value. The vertices live outside the list,
             * because p13_room() may restart it. */
            const int prim = (int)a[0], vt = (int)a[1], cnt = (int)a[2], vb = (int)a[3];
            if (vb > (int)sizeof g_c14_vb) { out("  ladder vertices too big: %d\n", vb); break; }
            memcpy(g_c14_vb, a + 6, vb);
            sceKernelDcacheWritebackAll();
            sceGuDisable(GU_ALPHA_TEST);
            sceGuDisable(GU_STENCIL_TEST);
            sceGuDrawArray(prim, vt, cnt, NULL, g_c14_vb);
            sceGuEnable(GU_STENCIL_TEST);
            sceGuStencilFunc(GU_ALWAYS, 0, 0xFF);
            sceGuStencilOp(GU_KEEP, GU_KEEP, GU_INCR);
            sceGuEnable(GU_ALPHA_TEST);
            for (int r = (int)a[4]; r <= (int)a[5]; r++) {
                p13_room();
                sceGuAlphaFunc(GU_GEQUAL, r, 0xFF);
                sceGuDrawArray(prim, vt, cnt, NULL, g_c14_vb);
            }
            sceGuDisable(GU_ALPHA_TEST);
            sceGuDisable(GU_STENCIL_TEST);
            break;
        }
        case C14_LIGHT: c14_light((int)a[0], c14_f(a[1])); break;
        case C14_TEX:   c14_tex((int)a[0]); break;
        case C14_DEPTH:
            /* Scene 97: depth test with writes on (GU_FALSE is "writes on"),
             * so the depth planes land in the buffer END dumps. */
            if (a[0]) {
                sceGuEnable(GU_DEPTH_TEST);
                sceGuDepthFunc((int)a[1]);
                sceGuDepthMask(GU_FALSE);
            } else sceGuDisable(GU_DEPTH_TEST);
            break;
        case C14_BONE: {                                  /* geprobe 16: bone a[0], 16 words */
            ScePspFMatrix4 m;
            memcpy(&m, a + 1, sizeof m);
            sceGuBoneMatrix(a[0], &m);
            break;
        }
        case C14_MORPH:                                   /* eight morph weights */
            for (int k = 0; k < 8; k++) sceGuMorphWeight(k, c14_f(a[k]));
            break;
        case C14_ZVIEW:                                   /* viewport z scale and centre, raw */
            sceGuSendCommandf(0x44, c14_f(a[0]));
            sceGuSendCommandf(0x47, c14_f(a[1]));
            break;
        case C14_LGT: {                                   /* geprobe 17: light 0, whole */
            const ScePspFVector3 p = { c14_f(a[2]), c14_f(a[3]), c14_f(a[4]) };
            const ScePspFVector3 d = { c14_f(a[5]), c14_f(a[6]), c14_f(a[7]) };
            sceGuLight(0, (int)a[0], (int)a[1], &p);
            sceGuLightSpot(0, &d, c14_f(a[8]), c14_f(a[9]));
            sceGuLightAtt(0, c14_f(a[10]), c14_f(a[11]), c14_f(a[12]));
            sceGuSpecular(c14_f(a[13]));
            sceGuLightColor(0, GU_DIFFUSE, a[14]);
            sceGuLightColor(0, GU_SPECULAR, a[15]);
            break;
        }
        case C14_LMODE:                                   /* white lighting on, or off */
            if (a[0]) l17_lights_on(); else c14_light(0, 1.0f);
            break;
        case C14_PATCH: {                                 /* geprobe 21: a Bezier or spline patch */
            /* kind, vtype, ucount, vcount, uedge, vedge, primitive, udiv, vdiv,
             * bytes, then the control points */
            p13_room();
            const w32 *v = (const w32 *)gumem(a + 10, (int)a[9]);
            sceGuPatchDivide(a[7], a[8]);
            sceGuPatchPrim((int)a[6]);
            if (a[0]) sceGuDrawSpline((int)a[1], (int)a[2], (int)a[3], (int)a[4], (int)a[5], NULL, v);
            else      sceGuDrawBezier((int)a[1], (int)a[2], (int)a[3], NULL, v);
            break;
        }
        case C14_TEXC: c22_texc(a); break;               /* geprobe 22: on, su, sv, ou, ov */
        case C14_CLAMP:                                   /* geprobe 22: depth clamping on or off */
            if (a[0]) sceGuEnable(GU_CLIP_PLANES); else sceGuDisable(GU_CLIP_PLANES);
            break;
        case C14_BLEND:                                   /* on, op, src, dst, fixed a, fixed b */
            if (a[0]) { sceGuEnable(GU_BLEND); sceGuBlendFunc((int)a[1], (int)a[2], (int)a[3], a[4], a[5]); }
            else sceGuDisable(GU_BLEND);
            break;
        case C14_END:
            sceGuScissor(0, 0, SCR_W, SCR_H);
            sceGuMorphWeight(0, 1.0f);
            for (int k = 1; k < 8; k++) sceGuMorphWeight(k, 0.0f);
            sceGuShadeModel(GU_SMOOTH);
            sceGuDisable(GU_ALPHA_TEST);
            sceGuDisable(GU_STENCIL_TEST);
            sceGuDisable(GU_FOG);
            sceGuDisable(GU_DEPTH_TEST);
            c14_light(0, 1.0f);
            c14_tex(0);
            sceGuOffset(2048 - SCR_W / 2, 2048 - SCR_H / 2);
            sceGuViewport(2048, 2048, SCR_W, SCR_H);
            sceGumMatrixMode(GU_VIEW);
            sceGumLoadIdentity();
            sceGumMatrixMode(GU_MODEL);
            sceGumLoadIdentity();
            sceGumUpdateMatrix();
            p13_flush();
            out("  GE %u us\n", (unsigned)g_p13_us);
            if (g_p13_flushes) out("  %d list flush(es) at 448 KB\n", g_p13_flushes);
            /* END 1: also the whole depth buffer, as _depthfull.bin (the
             * 480-wide _depth.raw cannot read every pixel's depth). */
            scene_end(st->name, psm, 0);
            if (a[0]) dump_depth_full(st->name);
            break;
        default:
            out("  bad stream word %d: %08X\n", i, s[i]);
            return;
        }
    }
}

/* ---- version 17: point and spot lights -----------------------------------
 *
 * Scene 35's last ten wrong points are each a step low where a power is
 * taken (a spot of exponent 4, a specular of coefficient 8), next to a byte
 * boundary: the GE's power, or what goes into it, comes out a hair higher.
 * Scene 115 reads what goes into the spot's power whole. The spot cutoff is
 * a threshold on L.D, so for each geometry (a vertex and its normal, a spot
 * light's position and direction) the probe searches the cutoff's 24-bit
 * code, every geometry at once, a pass at a time: draw each point with its
 * own cutoff, wait for the GE, read back which came out lit, halve each
 * bracket. What is left is the largest code that still lights the point.
 * Scenes 116-119 then draw the spot, diffuse, specular and attenuation
 * factors as bytes from lights17.py's streams (c14_run's LGT and LMODE).
 *
 * Every point goes to clip (0, 0, 0, 1) through a zero projection, so the
 * screen offset alone puts it on its pixel; lighting still sees the vertex
 * as given. The clear is red, which a white light cannot give: red is a
 * point not drawn, black one the spot leaves unlit. */
#include "l17_data.inc"      /* geprobe 17: scene 115's geometries and 116-119's streams, from lights17.py */

static void l17_slot(int i, int *x, int *y) { *x = 1 + 2 * (i % 239); *y = 1 + 2 * (i / 239); }

static void l17_mats(void) {
    static const ScePspFMatrix4 P = { { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 1 } };
    sceGumMatrixMode(GU_PROJECTION);
    sceGumLoadMatrix(&P);
    sceGumMatrixMode(GU_VIEW);
    sceGumLoadIdentity();
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
    sceGumUpdateMatrix();
}

static w32 g_l17_lo[L17_NGEO], g_l17_hi[L17_NGEO], g_l17_res[2 * L17_NGEO];
static signed char g_l17_dx[L17_NGEO], g_l17_dy[L17_NGEO];

/* One pass: every geometry still open drawn once with its cutoff code (pass
 * 0 the bracket's bottom, pass 1 its top, then the middle); returns when
 * the GE has finished. */
static void l17_pass(int pass) {
    scene_begin(GU_PSM_8888, 0xFF0000FF);
    l17_mats();
    l17_lights_on();
    for (int i = 0; i < L17_NGEO; i++) {
        const w32 lo = g_l17_lo[i], hi = g_l17_hi[i];
        if (pass >= 2 && hi - lo <= 1) continue;
        const w32 c = pass == 0 ? lo : pass == 1 ? hi : lo + (hi - lo) / 2;
        const w32 *g = L17_GEO[i];
        int x, y;
        l17_slot(i, &x, &y);
        p13_room();
        sceGuOffset(2048 - x, 2048 - y);
        const ScePspFVector3 p = { c14_f(g[6]), c14_f(g[7]), c14_f(g[8]) };
        const ScePspFVector3 d = { c14_f(g[9]), c14_f(g[10]), c14_f(g[11]) };
        sceGuLight(0, GU_SPOTLIGHT, GU_DIFFUSE, &p);
        sceGuLightSpot(0, &d, 1.0f, c14_f(c << 8));
        const float v[6] = { c14_f(g[3]), c14_f(g[4]), c14_f(g[5]), c14_f(g[0]), c14_f(g[1]), c14_f(g[2]) };
        sceGuDrawArray(GU_POINTS, GU_NORMAL_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_3D, 1, NULL,
                       gumem(v, sizeof v));
    }
    sceGuFinish();
    ge_wait();
}

/* Point i's pixel: -1 not drawn (the red clear), 0 unlit, else its grey. */
static int l17_read(int x, int y) {
    const w32 c = *(volatile w32 *)(VRAM_UNCACHED + (w32)(y * FB_W + x) * 4) & 0xFFFFFFu;
    return c == 0x0000FFu ? -1 : (int)((c >> 8) & 0xFF);
}

static void l17_search(void) {
    g_scene = 115;
    if (step("scene %02d: spot cutoff search, L.D read whole", g_scene)) return;
    out("  %d geometries, input crc %08X\n", L17_NGEO, crc32(L17_GEO, sizeof L17_GEO));
    for (int i = 0; i < L17_NGEO; i++) {
        g_l17_lo[i] = L17_CUT_LO;
        g_l17_hi[i] = L17_CUT_HI;
        g_l17_res[2 * i] = g_l17_res[2 * i + 1] = 0;
    }
    /* Flags (result word's top byte): 1 not drawn at the bracket's bottom,
     * 2 unlit there, 4 lit at its top, 8 not drawn in a later pass. */
    int passes = 0, open = L17_NGEO, absent = 0, unlit = 0, toplit = 0, lost = 0;
    const w32 t0 = sceKernelGetSystemTimeLow();
    for (int pass = 0; pass < 40 && (pass < 2 || open); pass++) {
        l17_pass(pass);
        passes++;
        open = 0;
        for (int i = 0; i < L17_NGEO; i++) {
            w32 *r = &g_l17_res[2 * i];
            if (pass >= 2 && g_l17_hi[i] - g_l17_lo[i] <= 1) continue;
            int x, y, v = -1;
            l17_slot(i, &x, &y);
            if (pass == 0) {
                /* find the pixel once: the slot or a neighbour */
                for (int k = 0; k < 9 && v < 0; k++) {
                    const int dx = k % 3 == 0 ? 0 : k % 3 == 1 ? -1 : 1, dy = k / 3 == 0 ? 0 : k / 3 == 1 ? -1 : 1;
                    v = l17_read(x + dx, y + dy);
                    if (v >= 0) { g_l17_dx[i] = (signed char)dx; g_l17_dy[i] = (signed char)dy; }
                }
                if (v < 0) { *r |= 1u << 24; absent++; g_l17_hi[i] = g_l17_lo[i]; continue; }
                if (v == 0) { *r |= 2u << 24; unlit++; g_l17_hi[i] = g_l17_lo[i]; continue; }
                r[1] = (w32)v;
            } else {
                v = l17_read(x + g_l17_dx[i], y + g_l17_dy[i]);
                if (v < 0) { *r |= 8u << 24; lost++; g_l17_hi[i] = g_l17_lo[i]; continue; }
                const w32 c = pass == 1 ? g_l17_hi[i] : g_l17_lo[i] + (g_l17_hi[i] - g_l17_lo[i]) / 2;
                if (pass == 1) {
                    if (v > 0) { *r |= 4u << 24; toplit++; g_l17_lo[i] = g_l17_hi[i]; }
                } else if (v > 0) { g_l17_lo[i] = c; r[1] = (w32)v; }
                else g_l17_hi[i] = c;
            }
            if (g_l17_hi[i] - g_l17_lo[i] > 1) open++;
        }
    }
    for (int i = 0; i < L17_NGEO; i++) g_l17_res[2 * i] |= g_l17_lo[i] & 0xFFFFFFu;
    out("  %d passes, %u us; flagged: %d not drawn, %d unlit at 1/32, %d lit at 16, %d lost\n", passes,
        (unsigned)(sceKernelGetSystemTimeLow() - t0), absent, unlit, toplit, lost);
    const int wr = probe_write_file("ge_115_spotcut.bin", g_l17_res, sizeof g_l17_res);
    out("  ge_115_spotcut.bin: %d bytes, crc %08X\n", wr, crc32(g_l17_res, sizeof g_l17_res));
    out("  first codes:");
    for (int i = 0; i < 8; i++) out(" %06X", (unsigned)(g_l17_res[2 * i] & 0xFFFFFF));
    out("\n");
}

/* ---- version 18: the lighting's 1/sqrt, every input -----------------------
 *
 * Scene 120 reads 1/sqrt at all 65536 16-bit inputs in [1, 4), the table's
 * whole range: for each, a light at (px, py, 1) over a vertex at the origin
 * with D = +z, px and py chosen (rsq18.py) so that the GE's L.L is that
 * input exactly, so the spot's L.D is 1/sqrt(L.L) itself. The cutoff is
 * searched as in scene 115, but in three chunks of slots, and from a bracket
 * of L18_BRACKET codes either side of the current table's value: two checks
 * and six halvings a chunk. Results go to ge_120_rsqfull.bin, two words an
 * input as in scene 115's file. */
#include "l18_data.inc"      /* geprobe 18: scene 120's inputs, from rsq18.py */

#define L18_PER ((L18_N + L18_CHUNKS - 1) / L18_CHUNKS)
static w32 g_l18_lo[L18_PER], g_l18_hi[L18_PER], g_l18_res[2 * L18_N];
static signed char g_l18_dx[L18_PER], g_l18_dy[L18_PER];

static void l18_pass(int base, int n, int pass) {
    static const ScePspFVector3 up = { 0.0f, 0.0f, 1.0f };
    scene_begin(GU_PSM_8888, 0xFF0000FF);
    l17_mats();
    l17_lights_on();
    sceGuLightSpot(0, &up, 1.0f, 1.0f / 32);
    for (int j = 0; j < n; j++) {
        const w32 lo = g_l18_lo[j], hi = g_l18_hi[j];
        if (pass >= 2 && hi - lo <= 1) continue;
        const w32 c = pass == 0 ? lo : pass == 1 ? hi : lo + (hi - lo) / 2;
        const w32 *g = L18_GEO[base + j];
        int x, y;
        l17_slot(j, &x, &y);
        p13_room();
        sceGuOffset(2048 - x, 2048 - y);
        const ScePspFVector3 p = { c14_f(g[0]), c14_f(g[1]), 1.0f };
        sceGuLight(0, GU_SPOTLIGHT, GU_DIFFUSE, &p);
        sceGuSendCommandi(0x8B, (int)c);                     /* light 0's cutoff, the code itself */
        const float v[6] = { p.x, p.y, p.z, 0.0f, 0.0f, 0.0f };
        sceGuDrawArray(GU_POINTS, GU_NORMAL_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_3D, 1, NULL,
                       gumem(v, sizeof v));
    }
    sceGuFinish();
    ge_wait();
}

static void l18_search(void) {
    g_scene = 120;
    if (step("scene %02d: the lighting's 1/sqrt at every input, through the spot cutoff", g_scene)) return;
    out("  %d inputs, input crc %08X\n", L18_N, crc32(L18_GEO, sizeof L18_GEO));
    const w32 t0 = sceKernelGetSystemTimeLow();
    int passes = 0, absent = 0, unlit = 0, toplit = 0, lost = 0;
    for (int base = 0; base < L18_N; base += L18_PER) {
        const int n = L18_N - base < L18_PER ? L18_N - base : L18_PER;
        for (int j = 0; j < n; j++) {
            g_l18_lo[j] = L18_GEO[base + j][2] - L18_BRACKET;
            g_l18_hi[j] = L18_GEO[base + j][2] + L18_BRACKET;
            g_l18_res[2 * (base + j)] = g_l18_res[2 * (base + j) + 1] = 0;
        }
        int open = n;
        for (int pass = 0; pass < 16 && (pass < 2 || open); pass++) {
            l18_pass(base, n, pass);
            passes++;
            open = 0;
            for (int j = 0; j < n; j++) {
                w32 *r = &g_l18_res[2 * (base + j)];
                if (pass >= 2 && g_l18_hi[j] - g_l18_lo[j] <= 1) continue;
                int x, y, v = -1;
                l17_slot(j, &x, &y);
                if (pass == 0) {
                    for (int k = 0; k < 9 && v < 0; k++) {
                        const int dx = k % 3 == 0 ? 0 : k % 3 == 1 ? -1 : 1, dy = k / 3 == 0 ? 0 : k / 3 == 1 ? -1 : 1;
                        v = l17_read(x + dx, y + dy);
                        if (v >= 0) { g_l18_dx[j] = (signed char)dx; g_l18_dy[j] = (signed char)dy; }
                    }
                    if (v < 0) { *r |= 1u << 24; absent++; g_l18_hi[j] = g_l18_lo[j]; continue; }
                    if (v == 0) { *r |= 2u << 24; unlit++; g_l18_hi[j] = g_l18_lo[j]; continue; }
                    r[1] = (w32)v;
                } else {
                    v = l17_read(x + g_l18_dx[j], y + g_l18_dy[j]);
                    if (v < 0) { *r |= 8u << 24; lost++; g_l18_hi[j] = g_l18_lo[j]; continue; }
                    const w32 c = pass == 1 ? g_l18_hi[j] : g_l18_lo[j] + (g_l18_hi[j] - g_l18_lo[j]) / 2;
                    if (pass == 1) {
                        if (v > 0) { *r |= 4u << 24; toplit++; g_l18_lo[j] = g_l18_hi[j]; }
                    } else if (v > 0) { g_l18_lo[j] = c; r[1] = (w32)v; }
                    else g_l18_hi[j] = c;
                }
                if (g_l18_hi[j] - g_l18_lo[j] > 1) open++;
            }
        }
        for (int j = 0; j < n; j++) g_l18_res[2 * (base + j)] |= g_l18_lo[j] & 0xFFFFFFu;
    }
    out("  %d passes, %u us; flagged: %d not drawn, %d unlit at the bracket's bottom, %d lit at its top, %d lost\n",
        passes, (unsigned)(sceKernelGetSystemTimeLow() - t0), absent, unlit, toplit, lost);
    const int wr = probe_write_file("ge_120_rsqfull.bin", g_l18_res, sizeof g_l18_res);
    out("  ge_120_rsqfull.bin: %d bytes, crc %08X\n", wr, crc32(g_l18_res, sizeof g_l18_res));
    int same = 0;
    for (int i = 0; i < L18_N; i++) same += (g_l18_res[2 * i] & 0xFFFFFFu) == L18_GEO[i][2] && !(g_l18_res[2 * i] >> 24);
    out("  %d of %d as the table predicts\n", same, L18_N);
}

/* ---- version 19: lighting under real world and view matrices -------------
 *
 * Scenes 115-120 settled lighting's arithmetic with world and view identity.
 * Scene 121 runs scene 115's cutoff search with W and V loaded per group of
 * points (lights19.py): large translations put the vertex and the light far
 * from the eye but near each other, and world matrices whose 3x3 cancels,
 * so how the GE forms the eye-space vertex, normal, light and spot direction
 * shows in L.D by many codes. Scenes 122-124 (lights19.py's streams, c14_run)
 * read diffuse, specular and attenuation bytes the same way. */
#include "l19_data.inc"      /* geprobe 19: scene 121's table and 122-124's streams, from lights19.py */

static w32 g_l19_lo[L19_NGEO], g_l19_hi[L19_NGEO], g_l19_res[2 * L19_NGEO];
static signed char g_l19_dx[L19_NGEO], g_l19_dy[L19_NGEO];

static void l19_pass(int pass) {
    scene_begin(GU_PSM_8888, 0xFF0000FF);
    l17_mats();
    l17_lights_on();
    int last = -1;
    for (int i = 0; i < L19_NGEO; i++) {
        const w32 lo = g_l19_lo[i], hi = g_l19_hi[i];
        if (pass >= 2 && hi - lo <= 1) continue;
        const w32 c = pass == 0 ? lo : pass == 1 ? hi : lo + (hi - lo) / 2;
        const w32 *g = L19_GEO[i];
        p13_room();
        if ((int)g[0] != last) {
            ScePspFMatrix4 m;
            memcpy(&m, L19_MAT[g[0]], sizeof m);
            sceGuSetMatrix(GU_MODEL, &m);
            memcpy(&m, L19_MAT[g[0]] + 16, sizeof m);
            sceGuSetMatrix(GU_VIEW, &m);
            last = (int)g[0];
        }
        int x, y;
        l17_slot(i, &x, &y);
        sceGuOffset(2048 - x, 2048 - y);
        const ScePspFVector3 p = { c14_f(g[7]), c14_f(g[8]), c14_f(g[9]) };
        const ScePspFVector3 d = { c14_f(g[10]), c14_f(g[11]), c14_f(g[12]) };
        sceGuLight(0, GU_SPOTLIGHT, GU_DIFFUSE, &p);
        sceGuLightSpot(0, &d, 1.0f, c14_f(c << 8));
        const float v[6] = { c14_f(g[4]), c14_f(g[5]), c14_f(g[6]), c14_f(g[1]), c14_f(g[2]), c14_f(g[3]) };
        sceGuDrawArray(GU_POINTS, GU_NORMAL_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_3D, 1, NULL,
                       gumem(v, sizeof v));
    }
    sceGuFinish();
    ge_wait();
}

static void l19_search(void) {
    g_scene = 121;
    if (step("scene %02d: spot cutoff search under real world and view matrices", g_scene)) return;
    out("  %d geometries, %d matrix pairs, input crc %08X\n", L19_NGEO, L19_NMAT,
        crc32_more(crc32(L19_MAT, sizeof L19_MAT), L19_GEO, sizeof L19_GEO));
    for (int i = 0; i < L19_NGEO; i++) {
        g_l19_lo[i] = L17_CUT_LO;
        g_l19_hi[i] = L17_CUT_HI;
        g_l19_res[2 * i] = g_l19_res[2 * i + 1] = 0;
    }
    int passes = 0, open = L19_NGEO, absent = 0, unlit = 0, toplit = 0, lost = 0;
    const w32 t0 = sceKernelGetSystemTimeLow();
    for (int pass = 0; pass < 40 && (pass < 2 || open); pass++) {
        l19_pass(pass);
        passes++;
        open = 0;
        for (int i = 0; i < L19_NGEO; i++) {
            w32 *r = &g_l19_res[2 * i];
            if (pass >= 2 && g_l19_hi[i] - g_l19_lo[i] <= 1) continue;
            int x, y, v = -1;
            l17_slot(i, &x, &y);
            if (pass == 0) {
                for (int k = 0; k < 9 && v < 0; k++) {
                    const int dx = k % 3 == 0 ? 0 : k % 3 == 1 ? -1 : 1, dy = k / 3 == 0 ? 0 : k / 3 == 1 ? -1 : 1;
                    v = l17_read(x + dx, y + dy);
                    if (v >= 0) { g_l19_dx[i] = (signed char)dx; g_l19_dy[i] = (signed char)dy; }
                }
                if (v < 0) { *r |= 1u << 24; absent++; g_l19_hi[i] = g_l19_lo[i]; continue; }
                if (v == 0) { *r |= 2u << 24; unlit++; g_l19_hi[i] = g_l19_lo[i]; continue; }
                r[1] = (w32)v;
            } else {
                v = l17_read(x + g_l19_dx[i], y + g_l19_dy[i]);
                if (v < 0) { *r |= 8u << 24; lost++; g_l19_hi[i] = g_l19_lo[i]; continue; }
                const w32 c = pass == 1 ? g_l19_hi[i] : g_l19_lo[i] + (g_l19_hi[i] - g_l19_lo[i]) / 2;
                if (pass == 1) {
                    if (v > 0) { *r |= 4u << 24; toplit++; g_l19_lo[i] = g_l19_hi[i]; }
                } else if (v > 0) { g_l19_lo[i] = c; r[1] = (w32)v; }
                else g_l19_hi[i] = c;
            }
            if (g_l19_hi[i] - g_l19_lo[i] > 1) open++;
        }
    }
    for (int i = 0; i < L19_NGEO; i++) g_l19_res[2 * i] |= g_l19_lo[i] & 0xFFFFFFu;
    out("  %d passes, %u us; flagged: %d not drawn, %d unlit at 1/32, %d lit at 16, %d lost\n", passes,
        (unsigned)(sceKernelGetSystemTimeLow() - t0), absent, unlit, toplit, lost);
    const int wr = probe_write_file("ge_121_eyesearch.bin", g_l19_res, sizeof g_l19_res);
    out("  ge_121_eyesearch.bin: %d bytes, crc %08X\n", wr, crc32(g_l19_res, sizeof g_l19_res));
}

/* ---- GE callbacks --------------------------------------------------------
 *
 * Handlers only record; they run in interrupt context. `g_phase` says where
 * the main thread was when each one ran. */

#define MAXEV 64
static volatile int g_nev;
static volatile int g_ev[MAXEV][3];
static volatile int g_phase;

static void note(int kind, int a) {
    const int i = g_nev;
    if (i < MAXEV) { g_ev[i][0] = kind; g_ev[i][1] = a; g_ev[i][2] = g_phase; g_nev = i + 1; }
}
static void gu_signal_cb(int id) { note(1, id); }
static void gu_finish_cb(int id) { note(2, id); }
static void ge_signal_cb(int id, void *arg) { note(3, id | ((int)arg & 0xFF) << 16); }
static void ge_finish_cb(int id, void *arg) { note(4, id | ((int)arg & 0xFF) << 16); }

static void log_events(void) {
    static const char *const K[] = { "?", "gu signal", "gu finish", "ge signal", "ge finish" };
    out("  %d callback(s)\n", g_nev);
    for (int i = 0; i < g_nev; i++)
        out("    %s arg %08X phase %d\n", K[g_ev[i][0]], (w32)g_ev[i][1], g_ev[i][2]);
}

/* Raw lists for the steps split out of step 35 and after it: a static
 * buffer on its own cache lines, written through the cache and then written
 * back and invalidated, so memory holds the list and no cache line holds an
 * older copy of it. The callback data is static too. */
#define UNCACHED(p) ((void *)((w32)(p) | 0x40000000u))
static unsigned int g_raw[512] __attribute__((aligned(64)));
static CV g_rawv[2] __attribute__((aligned(64)));
static PspGeCallbackData g_cb;
static int g_cbid = -1, g_lid = -1, g_raw_ready;

static void raw_build_signals(void) {
    static const unsigned int L[7] = {
        0x0E010044, 0x0C000000,     /* SIGNAL, behaviour 1 (suspend), id 0x0044; END */
        0x0E020055, 0x0C000000,     /* SIGNAL, behaviour 2 (continue), id 0x0055; END */
        0x0F000066, 0x0C000000,     /* FINISH 0x66; END */
        0 };
    memset(g_raw, 0, sizeof g_raw);
    for (int k = 0; k < 7; k++) g_raw[k] = L[k];
    sceKernelDcacheWritebackInvalidateRange(g_raw, sizeof g_raw);
    g_raw_ready = 1;
}

static int words_intact(const volatile unsigned int *p, const unsigned int *w, int n) {
    for (int i = 0; i < n; i++)
        if (p[i] != w[i]) return 0;
    return 1;
}

/* geprobe 6's handlers: one that reads the system clock (timing), one that
 * calls sceGeContinue from the PAUSE signal 0x7B. */
static volatile w32 g_t[8];
static volatile int g_nt;
static PspGeCallbackData g_cbt;
static void ge_time_cb(int id, void *arg) {
    (void)id; (void)arg;
    const int i = g_nt;
    if (i < 8) { g_t[i] = sceKernelGetSystemTimeLow(); g_nt = i + 1; }
}
static volatile int g_cont;
static void ge_signal_continue_cb(int id, void *arg) {
    note(3, id | ((int)arg & 0xFF) << 16);
    if (id == 0x7B) g_cont = sceGeContinue();
}

/* SIGNAL continue 0x21, FINISH 0x22, queued with the stall `at` words in;
 * peeks, 10 ms, then the stall moved to the end. */
static void stall_step(int at) {
    g_nev = 0; g_phase = 0;
    const int cbid = sceGeSetCallback(&g_cb);
    if (cbid < 0) {
        out("  sceGeSetCallback error\n");
        ret(cbid);
        return;
    }
    int k = 0;
    g_raw[k++] = 0x0E020021; g_raw[k++] = 0x0C000000;   /* SIGNAL continue 0x21, END */
    g_raw[k++] = 0x0F000022; g_raw[k++] = 0x0C000000;   /* FINISH 0x22, END */
    g_raw[k++] = 0;
    sceKernelDcacheWritebackInvalidateRange(g_raw, sizeof g_raw);
    g_phase = 1;
    const int lid = sceGeListEnQueue(UNCACHED(g_raw), UNCACHED(g_raw + at), cbid, NULL);
    g_phase = 2;
    out("  sceGeListEnQueue %s; %d callback(s) before it returned\n", lid >= 0 ? "ok (id >= 0)" : "error", g_nev);
    if (lid < 0) {
        ret(lid);
    } else {
        out("  sceGeListSync(peek) = %08X, sceGeDrawSync(peek) = %08X\n",
            (w32)sceGeListSync(lid, 1), (w32)sceGeDrawSync(1));
        sceKernelDelayThread(10000);
        out("  after 10 ms: %d callback(s); sceGeListSync(peek) = %08X, sceGeDrawSync(peek) = %08X\n",
            g_nev, (w32)sceGeListSync(lid, 1), (w32)sceGeDrawSync(1));
        g_phase = 3;
        const int u = sceGeListUpdateStallAddr(lid, UNCACHED(g_raw + 4));
        g_phase = 4;
        out("  sceGeListUpdateStallAddr = %08X; %d callback(s) by its return; sceGeListSync(peek) = %08X\n",
            (w32)u, g_nev, (w32)sceGeListSync(lid, 1));
        out("  wait %08X\n", (w32)ge_wait());
        log_events();
    }
    out("  sceGeUnsetCallback = %08X\n", (w32)sceGeUnsetCallback(cbid));
}

static void section_callbacks(void) {
    section("GE callbacks");

    int r;
    if (!step("libgu: signal CONTINUE 0x11, SUSPEND 0x22, then finish id 0x33")) {
        g_nev = 0; g_phase = 0;
        sceGuSetCallback(GU_CALLBACK_SIGNAL, gu_signal_cb);
        sceGuSetCallback(GU_CALLBACK_FINISH, gu_finish_cb);
        g_phase = 1;
        sceGuStart(GU_DIRECT, g_list);
        sceGuClearColor(0xFF000000);
        sceGuClear(GU_COLOR_BUFFER_BIT);
        sceGuSignal(GU_BEHAVIOR_CONTINUE, 0x11);
        rect2d(0, 0, 32, 32, 0xFFFFFFFF);
        sceGuSignal(GU_BEHAVIOR_SUSPEND, 0x22);
        rect2d(32, 0, 64, 32, 0xFFFFFFFF);
        g_phase = 2;
        sceGuFinishId(0x33);
        g_phase = 3;
        r = sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
        g_phase = 4;
        out("  sceGuSync = %08X\n", (unsigned)r);
        sceKernelDelayThread(20000);
        log_events();
        sceGuSetCallback(GU_CALLBACK_SIGNAL, NULL);
        sceGuSetCallback(GU_CALLBACK_FINISH, NULL);
    }

    if (!step("sceGe: SetCallback, a raw list with SIGNAL and FINISH, EnQueue, ListSync") &&
        !KNOWN_CRASH("switched fw 6.60 off in geprobe 4; split into the steps below")) {
        g_nev = 0; g_phase = 0;
        PspGeCallbackData cb;
        memset(&cb, 0, sizeof cb);
        cb.signal_func = ge_signal_cb;
        cb.signal_arg  = (void *)0x5A;
        cb.finish_func = ge_finish_cb;
        cb.finish_arg  = (void *)0xA5;
        int cbid = sceGeSetCallback(&cb);
        out("  sceGeSetCallback %s\n", cbid >= 0 ? "ok (id >= 0)" : "error");
        if (cbid < 0) ret(cbid);
        unsigned int *raw = (unsigned int *)((unsigned int)memalign(16, 64) | 0x40000000u);
        int k = 0;
        raw[k++] = 0x0E010044;     /* SIGNAL, behaviour 1 (suspend), id 0x0044 */
        raw[k++] = 0x0C000000;     /* END */
        raw[k++] = 0x0E020055;     /* SIGNAL, behaviour 2 (continue), id 0x0055 */
        raw[k++] = 0x0C000000;     /* END */
        raw[k++] = 0x0F000066;     /* FINISH 0x66 */
        raw[k++] = 0x0C000000;     /* END */
        raw[k++] = 0;
        g_phase = 1;
        int lid = sceGeListEnQueue(raw, NULL, cbid, NULL);
        g_phase = 2;
        out("  sceGeListEnQueue %s\n", lid >= 0 ? "ok (id >= 0)" : "error");
        if (lid < 0) ret(lid);
        int s = sceGeListSync(lid, 0);
        g_phase = 3;
        out("  sceGeListSync(wait) = %08X\n", (unsigned)s);
        s = sceGeDrawSync(0);
        out("  sceGeDrawSync(wait) = %08X\n", (unsigned)s);
        sceKernelDelayThread(20000);
        log_events();
        s = sceGeListSync(lid, 1);
        out("  sceGeListSync(peek) after = %08X\n", (unsigned)s);
        if (cbid >= 0) { r = sceGeUnsetCallback(cbid); out("  sceGeUnsetCallback = %08X\n", (unsigned)r); }
    }

    /* geprobe 5: step 35 split up. In geprobe 4 fw 6.60 switched itself off
     * in it (version 1 had run it) and took the step's log lines with it:
     * they reach the memory stick only at the next step. One thing in it
     * depends on what ran before: its list sits in a 64-byte block from
     * memalign, 16 bytes into a 64-byte cache line whose first half holds the
     * block's malloc header (psprecomp puts it there with the same heap
     * calls). The header is written through the cache and the list through the
     * uncached alias, so the cache keeps a dirty line with the old contents of
     * the list's memory, and if it is written back before the GE fetches the
     * list (by eviction, or by the driver writing back the cache), the GE
     * runs whatever was there. Here the list sits in a static 64-byte-aligned
     * buffer written back from the cache, the callback data outlives the
     * call, and each call is its own step; the last steps repeat version 4's
     * code exactly with the log flushed after every call, and check whether
     * such a block's words survive a cache write-back. */
    if (!step("sceGe raw list, split: sceGeSetCallback")) {
        memset(&g_cb, 0, sizeof g_cb);
        g_cb.signal_func = ge_signal_cb;
        g_cb.signal_arg  = (void *)0x5A;
        g_cb.finish_func = ge_finish_cb;
        g_cb.finish_arg  = (void *)0xA5;
        g_cbid = sceGeSetCallback(&g_cb);
        out("  sceGeSetCallback %s\n", g_cbid >= 0 ? "ok (id >= 0)" : "error");
        if (g_cbid < 0) ret(g_cbid);
    }
    if (!step("sceGe raw list, split: DrawSync(wait), the list in a static buffer written back from the cache")) {
        out("  sceGeDrawSync(wait) = %08X\n", (w32)sceGeDrawSync(0));
        raw_build_signals();
        out("  list ready\n");
    }
    if (!step("sceGe raw list, split: EnQueue (SIGNAL suspend 0x44, SIGNAL continue 0x55, FINISH 0x66)")) {
        if (g_cbid < 0 || !g_raw_ready) {
            out("  not run: no callback or no list (a step above did not run)\n");
        } else {
            g_nev = 0; g_phase = 1;
            g_lid = sceGeListEnQueue(UNCACHED(g_raw), NULL, g_cbid, NULL);
            g_phase = 2;
            out("  sceGeListEnQueue %s\n", g_lid >= 0 ? "ok (id >= 0)" : "error");
            if (g_lid < 0) ret(g_lid);
            out("  %d callback(s) before it returned\n", g_nev);
        }
    }
    if (!step("sceGe raw list, split: ListSync(wait), ListSync(peek), DrawSync(wait), ListSync(peek)")) {
        if (g_lid < 0) {
            out("  not run: no list\n");
        } else {
            int s = sceGeListSync(g_lid, 0);
            g_phase = 3;
            out("  sceGeListSync(wait) = %08X\n", (w32)s);
            s = sceGeListSync(g_lid, 1);
            out("  sceGeListSync(peek) = %08X\n", (w32)s);
            s = sceGeDrawSync(0);
            g_phase = 4;
            out("  sceGeDrawSync(wait) = %08X\n", (w32)s);
            s = sceGeListSync(g_lid, 1);
            out("  sceGeListSync(peek) = %08X\n", (w32)s);
        }
    }
    if (!step("sceGe raw list, split: the callbacks, then sceGeUnsetCallback")) {
        sceKernelDelayThread(20000);
        log_events();
        if (g_cbid >= 0) {
            r = sceGeUnsetCallback(g_cbid);
            out("  sceGeUnsetCallback = %08X\n", (w32)r);
            g_cbid = -1;
        }
    }

    /* PAUSE (SIGNAL behaviour 3), as sceGuSignal(GU_SIGNAL_PAUSE) writes it:
     * SIGNAL, END, FINISH, END. Which handler runs, whether the list stops
     * there until sceGeContinue, and what it reports meanwhile. Phase 1 is
     * inside EnQueue, 2 before sceGeContinue, 4 after it. */
    if (!step("sceGe PAUSE: SIGNAL pause 0x77 and FINISH 0x78, SIGNAL continue 0x79, FINISH 0x7A; sceGeContinue")) {
        g_nev = 0; g_phase = 0;
        const int cbid = sceGeSetCallback(&g_cb);
        if (cbid < 0) {
            out("  sceGeSetCallback error\n");
            ret(cbid);
        } else {
            int k = 0;
            g_raw[k++] = 0x0E030077; g_raw[k++] = 0x0C000000;   /* SIGNAL pause 0x77, END */
            g_raw[k++] = 0x0F000078; g_raw[k++] = 0x0C000000;   /* FINISH 0x78, END */
            g_raw[k++] = 0x0E020079; g_raw[k++] = 0x0C000000;   /* SIGNAL continue 0x79, END */
            g_raw[k++] = 0x0F00007A; g_raw[k++] = 0x0C000000;   /* FINISH 0x7A, END */
            g_raw[k++] = 0;
            sceKernelDcacheWritebackInvalidateRange(g_raw, sizeof g_raw);
            g_phase = 1;
            const int lid = sceGeListEnQueue(UNCACHED(g_raw), NULL, cbid, NULL);
            g_phase = 2;
            out("  sceGeListEnQueue %s\n", lid >= 0 ? "ok (id >= 0)" : "error");
            if (lid < 0) {
                ret(lid);
            } else {
                sceKernelDelayThread(20000);
                const int ls = sceGeListSync(lid, 1), ds = sceGeDrawSync(1);
                out("  after 20 ms: %d callback(s); sceGeListSync(peek) = %08X, sceGeDrawSync(peek) = %08X\n",
                    g_nev, (w32)ls, (w32)ds);
                g_phase = 3;
                r = sceGeContinue();
                g_phase = 4;
                out("  sceGeContinue = %08X\n", (w32)r);
                sceKernelDelayThread(20000);
                out("  after 20 ms more: sceGeListSync(peek) = %08X\n", (w32)sceGeListSync(lid, 1));
                out("  wait %08X\n", (w32)ge_wait());
                log_events();
            }
            r = sceGeUnsetCallback(cbid);
            out("  sceGeUnsetCallback = %08X\n", (w32)r);
        }
    }

    /* A list with no stall that keeps the GE busy for a while: 200 full-screen
     * sprites, a SIGNAL (continue) before the first, another before the
     * 101st, FINISH after the last. Run 1's short list ran entirely inside
     * EnQueue (phase 1); does this one, or does EnQueue return first? */
    if (!step("sceGe long list without a stall: SIGNAL 0x01, 100 sprites, SIGNAL 0x02, 100 sprites, FINISH 0x03")) {
        g_nev = 0; g_phase = 0;
        const int cbid = sceGeSetCallback(&g_cb);
        if (cbid < 0) {
            out("  sceGeSetCallback error\n");
            ret(cbid);
        } else {
            g_rawv[0] = (CV){ 0xFF203040, 0, 0, 0 };
            g_rawv[1] = (CV){ 0xFF203040, SCR_W, SCR_H, 0 };
            const w32 va = (w32)g_rawv;
            int k = 0;
            g_raw[k++] = 0x12000000 | FMT_CV2D;                 /* VTYPE */
            g_raw[k++] = 0x10000000 | ((va >> 8) & 0x0F0000);   /* BASE */
            g_raw[k++] = 0x0E020001; g_raw[k++] = 0x0C000000;   /* SIGNAL continue 0x01, END */
            for (int i = 0; i < 200; i++) {
                if (i == 100) { g_raw[k++] = 0x0E020002; g_raw[k++] = 0x0C000000; }
                g_raw[k++] = 0x01000000 | (va & 0xFFFFFF);                  /* VADDR */
                g_raw[k++] = 0x04000000 | (GU_SPRITES << 16) | 2;           /* PRIM */
            }
            g_raw[k++] = 0x0F000003; g_raw[k++] = 0x0C000000;   /* FINISH 0x03, END */
            g_raw[k++] = 0;
            sceKernelDcacheWritebackInvalidateRange(g_rawv, sizeof g_rawv);
            sceKernelDcacheWritebackInvalidateRange(g_raw, sizeof g_raw);
            g_phase = 1;
            const int lid = sceGeListEnQueue(UNCACHED(g_raw), NULL, cbid, NULL);
            g_phase = 2;
            const int peek = lid >= 0 ? sceGeListSync(lid, 1) : 0;
            g_phase = 3;
            out("  sceGeListEnQueue %s; sceGeListSync(peek) as it returns = %08X\n",
                lid >= 0 ? "ok (id >= 0)" : "error", (w32)peek);
            if (lid < 0) ret(lid);
            out("  wait %08X\n", (w32)ge_wait());
            log_events();
            r = sceGeUnsetCallback(cbid);
            out("  sceGeUnsetCallback = %08X\n", (w32)r);
        }
    }

    /* How many callback slots a program gets (libgu holds one) and the error
     * past the last. */
    if (!step("sceGeSetCallback until it fails, then sceGeUnsetCallback on each")) {
        int ids[40], n = 0, e = 0;
        while (n < 40) {
            const int id = sceGeSetCallback(&g_cb);
            if (id < 0) { e = id; break; }
            ids[n++] = id;
        }
        out("  %d accepted", n);
        if (e) out(", then %08X", (w32)e);
        out("\n");
        int bad = 0;
        for (int i = 0; i < n; i++)
            if (sceGeUnsetCallback(ids[i]) != 0) bad++;
        out("  sceGeUnsetCallback: %d error(s)\n", bad);
    }

    /* Version 4's step 35 again, call for call, with the log flushed after
     * each one so it names where the PSP stops if it does; then whether the
     * list's words still read as written once the cache is written back.
     * geprobe 5 on fw 6.60 logged "sceGeListEnQueue ok" and stopped there,
     * and the next step found all 16 words of such a block replaced by a
     * cache write-back. */
    if (!step("sceGe raw list as geprobe 4 built it (memalign, uncached writes), log flushed after each call") &&
        !KNOWN_CRASH("switched fw 6.60 off after EnQueue in geprobe 5: the list shares a dirty cache line "
                     "with its malloc header, whose write-back replaces it (next step)")) {
        g_nev = 0; g_phase = 0;
        PspGeCallbackData cb;
        memset(&cb, 0, sizeof cb);
        cb.signal_func = ge_signal_cb;
        cb.signal_arg  = (void *)0x5A;
        cb.finish_func = ge_finish_cb;
        cb.finish_arg  = (void *)0xA5;
        int cbid = sceGeSetCallback(&cb);
        out("  sceGeSetCallback %s\n", cbid >= 0 ? "ok (id >= 0)" : "error");
        if (cbid < 0) ret(cbid);
        probe_flush();
        unsigned int *raw = (unsigned int *)((unsigned int)memalign(16, 64) | 0x40000000u);
        static const unsigned int WORDS[7] = {
            0x0E010044, 0x0C000000, 0x0E020055, 0x0C000000, 0x0F000066, 0x0C000000, 0 };
        for (int k = 0; k < 7; k++) raw[k] = WORDS[k];
        out("  list written\n");
        probe_flush();
        g_phase = 1;
        int lid = sceGeListEnQueue(raw, NULL, cbid, NULL);
        g_phase = 2;
        out("  sceGeListEnQueue %s\n", lid >= 0 ? "ok (id >= 0)" : "error");
        if (lid < 0) ret(lid);
        probe_flush();
        int s = sceGeListSync(lid, 0);
        g_phase = 3;
        out("  sceGeListSync(wait) = %08X\n", (w32)s);
        probe_flush();
        s = sceGeDrawSync(0);
        out("  sceGeDrawSync(wait) = %08X\n", (w32)s);
        probe_flush();
        sceKernelDelayThread(20000);
        log_events();
        s = sceGeListSync(lid, 1);
        out("  sceGeListSync(peek) after = %08X\n", (w32)s);
        if (cbid >= 0) { r = sceGeUnsetCallback(cbid); out("  sceGeUnsetCallback = %08X\n", (w32)r); }
        probe_flush();
        out("  list words %s\n", words_intact(raw, WORDS, 7) ? "as written" : "changed");
        sceKernelDcacheWritebackAll();
        out("  after sceKernelDcacheWritebackAll: list words %s\n",
            words_intact(raw, WORDS, 7) ? "as written" : "changed");
    }

    /* The same without the GE: a fresh memalign(16, 64) block written through
     * the uncached alias, then the cache written back. Words that change came
     * from a dirty cache line over the block. */
    if (!step("memalign(16, 64) written through the uncached alias, then sceKernelDcacheWritebackAll")) {
        unsigned int *blk = (unsigned int *)((unsigned int)memalign(16, 64) | 0x40000000u);
        static const unsigned int PAT[16] = {
            0x11111111, 0x22222222, 0x33333333, 0x44444444, 0x55555555, 0x66666666, 0x77777777, 0x88888888,
            0x99999999, 0xAAAAAAAA, 0xBBBBBBBB, 0xCCCCCCCC, 0xDDDDDDDD, 0xEEEEEEEE, 0x12345678, 0x9ABCDEF0 };
        for (int k = 0; k < 16; k++) blk[k] = PAT[k];
        const int before = words_intact(blk, PAT, 16);
        sceKernelDcacheWritebackAll();
        int changed = 0;
        for (int k = 0; k < 16; k++) changed += blk[k] != PAT[k];
        out("  before the write-back %s; after it %d of 16 words changed\n",
            before ? "as written" : "already changed", changed);
    }

    /* geprobe 6. What sceGeContinue answers when no list is paused. */
    if (!step("sceGeContinue with nothing paused")) {
        out("  sceGeDrawSync(peek) = %08X\n", (w32)sceGeDrawSync(1));
        r = sceGeContinue();
        out("  sceGeContinue = %08X; sceGeDrawSync(peek) after = %08X\n", (w32)r, (w32)sceGeDrawSync(1));
    }

    /* A list queued with a stall address: at its start, then just after its
     * SIGNAL. What the peeks report while it waits, whether the part before
     * the stall runs (and its handler) before EnQueue returns, and when the
     * rest runs once sceGeListUpdateStallAddr moves the stall to the end. */
    if (!step("sceGe stall at the list's start (SIGNAL 0x21, FINISH 0x22), then sceGeListUpdateStallAddr to its end"))
        stall_step(0);
    if (!step("sceGe stall just after the SIGNAL 0x21, then sceGeListUpdateStallAddr to the end"))
        stall_step(2);

    /* How long lists take: 100 sprites between SIGNAL 0x01 and SIGNAL 0x02,
     * then FINISH 0x03, at three sizes; the handlers read the system clock.
     * psprecomp's run gives its own timing model's answer. */
    if (!step("sceGe timing: 100 sprites between SIGNALs, 480x272, 64x64 and 16x16; microseconds to each callback")) {
        memset(&g_cbt, 0, sizeof g_cbt);
        g_cbt.signal_func = ge_time_cb;
        g_cbt.finish_func = ge_time_cb;
        const int cbid = sceGeSetCallback(&g_cbt);
        if (cbid < 0) {
            out("  sceGeSetCallback error\n");
            ret(cbid);
        } else {
            static const short SZ[3][2] = { { SCR_W, SCR_H }, { 64, 64 }, { 16, 16 } };
            for (int s = 0; s < 3; s++) {
                g_rawv[0] = (CV){ 0xFF302010, 0, 0, 0 };
                g_rawv[1] = (CV){ 0xFF302010, SZ[s][0], SZ[s][1], 0 };
                const w32 va = (w32)g_rawv;
                int k = 0;
                g_raw[k++] = 0x12000000 | FMT_CV2D;                 /* VTYPE */
                g_raw[k++] = 0x10000000 | ((va >> 8) & 0x0F0000);   /* BASE */
                g_raw[k++] = 0x0E020001; g_raw[k++] = 0x0C000000;   /* SIGNAL continue 0x01, END */
                for (int i = 0; i < 100; i++) {
                    g_raw[k++] = 0x01000000 | (va & 0xFFFFFF);                  /* VADDR */
                    g_raw[k++] = 0x04000000 | (GU_SPRITES << 16) | 2;           /* PRIM */
                }
                g_raw[k++] = 0x0E020002; g_raw[k++] = 0x0C000000;   /* SIGNAL continue 0x02, END */
                g_raw[k++] = 0x0F000003; g_raw[k++] = 0x0C000000;   /* FINISH 0x03, END */
                g_raw[k++] = 0;
                sceKernelDcacheWritebackInvalidateRange(g_rawv, sizeof g_rawv);
                sceKernelDcacheWritebackInvalidateRange(g_raw, sizeof g_raw);
                g_nt = 0;
                const w32 t0 = sceKernelGetSystemTimeLow();
                const int lid = sceGeListEnQueue(UNCACHED(g_raw), NULL, cbid, NULL);
                const w32 t1 = sceKernelGetSystemTimeLow();
                const int nt1 = g_nt;
                const int ds = sceGeDrawSync(0);
                const w32 t2 = sceKernelGetSystemTimeLow();
                out("  %dx%d: EnQueue %s after %u us (%d callback(s) by then), DrawSync = %08X after %u us;"
                    " callbacks at", SZ[s][0], SZ[s][1], lid >= 0 ? "returned" : "failed", (unsigned)(t1 - t0),
                    nt1, (w32)ds, (unsigned)(t2 - t0));
                for (int i = 0; i < g_nt; i++) out(" %u", (unsigned)(g_t[i] - t0));
                out(" us\n");
                if (lid < 0) { ret(lid); break; }
            }
            r = sceGeUnsetCallback(cbid);
            out("  sceGeUnsetCallback = %08X\n", (w32)r);
        }
    }

    /* Last, as it may not come back: PAUSE whose signal handler calls
     * sceGeContinue itself (in geprobe 5 the handler ran inside EnQueue and
     * the list waited for sceGeContinue from the thread). If the list is
     * still paused 20 ms on, the thread continues it. */
    if (!step("sceGe PAUSE 0x7B with sceGeContinue in its signal handler; FINISH 0x7C, SIGNAL 0x7D, FINISH 0x7E")) {
        static PspGeCallbackData cbc;
        memset(&cbc, 0, sizeof cbc);
        cbc.signal_func = ge_signal_continue_cb;
        cbc.signal_arg  = (void *)0x5A;
        cbc.finish_func = ge_finish_cb;
        cbc.finish_arg  = (void *)0xA5;
        g_nev = 0; g_phase = 0; g_cont = 0x7FFFFFFF;
        const int cbid = sceGeSetCallback(&cbc);
        if (cbid < 0) {
            out("  sceGeSetCallback error\n");
            ret(cbid);
        } else {
            int k = 0;
            g_raw[k++] = 0x0E03007B; g_raw[k++] = 0x0C000000;   /* SIGNAL pause 0x7B, END */
            g_raw[k++] = 0x0F00007C; g_raw[k++] = 0x0C000000;   /* FINISH 0x7C, END */
            g_raw[k++] = 0x0E02007D; g_raw[k++] = 0x0C000000;   /* SIGNAL continue 0x7D, END */
            g_raw[k++] = 0x0F00007E; g_raw[k++] = 0x0C000000;   /* FINISH 0x7E, END */
            g_raw[k++] = 0;
            sceKernelDcacheWritebackInvalidateRange(g_raw, sizeof g_raw);
            g_phase = 1;
            const int lid = sceGeListEnQueue(UNCACHED(g_raw), NULL, cbid, NULL);
            g_phase = 2;
            out("  sceGeListEnQueue %s; %d callback(s) before it returned\n",
                lid >= 0 ? "ok (id >= 0)" : "error", g_nev);
            if (lid < 0) {
                ret(lid);
            } else {
                sceKernelDelayThread(20000);
                const int ls = sceGeListSync(lid, 1);
                out("  after 20 ms: %d callback(s); the handler's sceGeContinue = %08X;"
                    " sceGeListSync(peek) = %08X, sceGeDrawSync(peek) = %08X\n",
                    g_nev, (w32)g_cont, (w32)ls, (w32)sceGeDrawSync(1));
                if (ls == 4) {
                    g_phase = 3;
                    r = sceGeContinue();
                    g_phase = 4;
                    out("  still paused: sceGeContinue = %08X\n", (w32)r);
                }
                out("  wait %08X\n", (w32)ge_wait());
                log_events();
            }
            r = sceGeUnsetCallback(cbid);
            out("  sceGeUnsetCallback = %08X\n", (w32)r);
        }
    }
}

/* ---- main ---------------------------------------------------------------- */

int main(int argc, char **argv) {
    probe_init("geprobe", PROBE_VERSION, argc, argv);

    g_list = memalign(64, 512 * 1024);
    g_dump = memalign(64, SCR_W * SCR_H * 4);
    if (!g_list || !g_dump) { say("out of memory\n"); probe_done(); }
    make_textures();

    sceGuInit();
    sceGuStart(GU_DIRECT, g_list);
    sceGuDrawBuffer(GU_PSM_8888, (void *)0, FB_W);
    sceGuDispBuffer(SCR_W, SCR_H, (void *)0, FB_W);
    sceGuDepthBuffer(ZBP, FB_W);
    sceGuFinish();
    sceGuSync(0, 0);
    sceGuDisplay(GU_TRUE);

    section("scenes (the screen shows each one as it is drawn)");
    probe_screen(0);

    g_scene = 1;  scene_geometry(GU_PSM_8888, "geom_8888");
    g_scene = 2;  scene_geometry(GU_PSM_5650, "geom_5650");
    g_scene = 3;  scene_geometry(GU_PSM_5551, "geom_5551");
    g_scene = 4;  scene_geometry(GU_PSM_4444, "geom_4444");
    g_scene = 5;  scene_dither(GU_PSM_5650, "dither_off_5650", &DITHER_BAYER, 0);
    g_scene = 6;  scene_dither(GU_PSM_5650, "dither_bayer_5650", &DITHER_BAYER, 1);
    g_scene = 7;  scene_dither(GU_PSM_5650, "dither_extreme_5650", &DITHER_EXTREME, 1);
    g_scene = 8;  scene_dither(GU_PSM_5551, "dither_bayer_5551", &DITHER_BAYER, 1);
    g_scene = 9;  scene_dither(GU_PSM_4444, "dither_bayer_4444", &DITHER_BAYER, 1);
    g_scene = 10; scene_dither(GU_PSM_8888, "dither_extreme_8888", &DITHER_EXTREME, 1);
    g_scene = 11; scene_blend();
    g_scene = 12; scene_filter();
    g_scene = 13; scene_texfunc();
    g_scene = 14; scene_clut();
    g_scene = 15; scene_fog();
    g_scene = 16; scene_light();
    g_scene = 17; scene_depth();
    g_scene = 18; scene_clip();
    g_scene = 19; scene_tests();
    g_scene = 20; scene_skin();
    g_scene = 21; scene_morph();
    g_scene = 22; scene_bezier();
    g_scene = 23; scene_spline();
    g_scene = 24; scene_bbox();
    g_scene = 25; scene_specular();
    g_scene = 26; scene_precision();
    g_scene = 27; scene_depthplanes();
    g_scene = 28; scene_positions();
    g_scene = 29; scene_tests2();
    g_scene = 30; scene_masks16(GU_PSM_5650, "masks_5650");
    g_scene = 31; scene_masks16(GU_PSM_5551, "masks_5551");
    g_scene = 32; scene_masks16(GU_PSM_4444, "masks_4444");
    g_scene = 33; scene_bbox2();
    g_scene = 34; scene_lightmath();
    g_scene = 35; scene_locallights();
    g_scene = 36; scene_depthanchor();
    g_scene = 37; scene_lineends();
    g_scene = 38; scene_patchpoints();
    g_scene = 39; scene_gradients();
    g_scene = 40; scene_morphgrad();
    g_scene = 41; scene_bbox3();
    g_scene = 42; scene_indices();

    section_callbacks();

    section("scenes, version 7");
    g_scene = 43; scene_lightgain();
    g_scene = 44; scene_patchweights();
    g_scene = 45; scene_vertexdepth();
    g_scene = 46; scene_gradsweep();
    g_scene = 47; scene_gradsweep3d();
    g_scene = 48; scene_depthanchor2();
    g_scene = 49; scene_steeplines();

    section("scenes, version 8");
    g_scene = 50; scene_rcpsweep();
    g_scene = 51; scene_linesweep();
    g_scene = 52; scene_splineweights();

    section("scenes, version 9");
    g_scene = 53; scene_splinedense(40);
    g_scene = 54; scene_splinedense(48);

    section("scenes, version 10");
    g_scene = 55; scene_splinedepth();
    g_scene = 56; scene_splineparam();
    g_scene = 57; scene_anchortwin();
    g_scene = 58; scene_anchorsweep();

    section("scenes, version 11");
    g_scene = 59; scene_depthaffine();
    g_scene = 60; scene_depthratio();
    g_scene = 61; scene_depthmodel();

    section("scenes, version 12");
    g_scene = 62; scene_rcpread();
    g_scene = 63; scene_dotread();
    g_scene = 64; scene_mulread();
    g_scene = 65; scene_matread();
    g_scene = 66; scene_perspread();

    /* Version 13: scenes 67-82, each step a stream from patch13.py, the
     * risky ones (76-82) last and in the order of what is least known. */
    section("scenes, version 13");
    for (int k = 0; k < P13_NSTEPS; k++) p13_run(&P13_STEPS[k]);

    section("scenes, version 14");
    for (int k = 0; k < C14_NSTEPS; k++) c14_run(&C14_STEPS[k]);

    section("scenes, version 15");
    for (int k = 0; k < C15_NSTEPS; k++) c14_run(&C15_STEPS[k]);

    section("scenes, version 16");
    for (int k = 0; k < C16_NSTEPS; k++) c14_run(&C16_STEPS[k]);

    section("scenes, version 17");
    l17_search();
    for (int k = 0; k < C17_NSTEPS; k++) c14_run(&C17_STEPS[k]);

    section("scenes, version 18");
    l18_search();

    section("scenes, version 19");
    l19_search();
    for (int k = 0; k < C19_NSTEPS; k++) c14_run(&C19_STEPS[k]);

    section("scenes, version 20");
    for (int k = 0; k < C20_NSTEPS; k++) c14_run(&C20_STEPS[k]);

    section("scenes, version 21");
    for (int k = 0; k < C21_NSTEPS; k++) c14_run(&C21_STEPS[k]);
    for (int k = 0; k < C21P_NSTEPS; k++) c14_run(&C21P_STEPS[k]);

    section("scenes, version 22");
    for (int k = 0; k < C22E_NSTEPS; k++) c14_run(&C22E_STEPS[k]);
    for (int k = 0; k < C22T_NSTEPS; k++) c14_run(&C22T_STEPS[k]);
    for (int k = 0; k < C22N_NSTEPS; k++) c14_run(&C22N_STEPS[k]);
    for (int k = 0; k < C22M_NSTEPS; k++) c14_run(&C22M_STEPS[k]);
    for (int k = 0; k < C22C_NSTEPS; k++) c14_run(&C22C_STEPS[k]);

    probe_screen(1);
    probe_done();
    return 0;
}
