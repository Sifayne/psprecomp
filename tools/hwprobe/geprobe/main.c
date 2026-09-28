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
 * hardware reference from this project's own PSP.
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

#define PROBE_VERSION 2

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

static void scene_end(const char *name, int psm, int dump_depth) {
    sceGuFinish();
    int r = sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
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
    step("scene %02d: %s", g_scene, name);
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
    step("scene %02d: %s", g_scene, name);
    scene_begin(psm, 0x00000000);
    if (m) sceGuSetDither(m);
    if (on) sceGuEnable(GU_DITHER); else sceGuDisable(GU_DITHER);
    dither_body();
    scene_end(name, psm, 0);
}

/* Blending: bars of a source colour over a background gradient, one bar per
 * equation and factor pair. */
static void scene_blend(void) {
    step("scene %02d: blend", g_scene);
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
    step("scene %02d: texture filtering", g_scene);
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
    step("scene %02d: texture functions", g_scene);
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
    step("scene %02d: CLUT", g_scene);
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
    step("scene %02d: fog", g_scene);
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
    step("scene %02d: lighting", g_scene);
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
    step("scene %02d: depth", g_scene);
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
    step("scene %02d: clipping", g_scene);
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
    step("scene %02d: alpha/colour/stencil/logic/mask", g_scene);
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
    step("scene %02d: skinning", g_scene);
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
    step("scene %02d: morph", g_scene);
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
    step("scene %02d: Bezier patches", g_scene);
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
    step("scene %02d: spline patches", g_scene);
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
 * if the GE decides its box is visible. */
static void bbox_object(float cx, float cy, float cz, float h, int slot, w32 color) {
    typedef struct { float x, y, z; } P3;
    P3 box[8];
    for (int i = 0; i < 8; i++)
        box[i] = (P3){ cx + ((i & 1) ? h : -h), cy + ((i & 2) ? h : -h), cz + ((i & 4) ? h : -h) };
    sceGuBeginObject(GU_VERTEX_32BITF | GU_TRANSFORM_3D, 8, NULL, gumem(box, sizeof box));
    rect2d(10 + slot * 58, 220, 60 + slot * 58, 262, color);
    sceGuEndObject();
}

static void scene_bbox(void) {
    step("scene %02d: bounding-box jumps", g_scene);
    scene_begin(GU_PSM_8888, 0xFF000000);
    bbox_object(0.0f, 0.0f, -5.0f, 0.5f, 0, 0xFFFFFFFF);    /* in view */
    bbox_object(20.0f, 0.0f, -5.0f, 0.5f, 1, 0xFF0000FF);   /* far right */
    bbox_object(0.0f, 0.0f, 5.0f, 0.5f, 2, 0xFF00FF00);     /* behind the camera */
    bbox_object(3.2f, 0.0f, -5.0f, 0.6f, 3, 0xFFFF0000);    /* straddles the right edge */
    bbox_object(0.0f, 0.0f, -1.0f, 0.4f, 4, 0xFF00FFFF);    /* straddles the near plane */
    bbox_object(0.0f, 0.0f, -200.0f, 0.5f, 5, 0xFFFF00FF);  /* beyond the far plane */
    bbox_object(0.0f, 30.0f, -5.0f, 0.5f, 6, 0xFFFFFF00);   /* far above */
    bbox_object(0.0f, 2.0f, -5.0f, 0.3f, 7, 0xFF808080);    /* just above the top edge */
    scene_end("bbox", GU_PSM_8888, 0);
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

static void section_callbacks(void) {
    section("GE callbacks");

    step("libgu: signal CONTINUE 0x11, SUSPEND 0x22, then finish id 0x33");
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
    int r = sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    g_phase = 4;
    out("  sceGuSync = %08X\n", (unsigned)r);
    sceKernelDelayThread(20000);
    log_events();
    sceGuSetCallback(GU_CALLBACK_SIGNAL, NULL);
    sceGuSetCallback(GU_CALLBACK_FINISH, NULL);

    step("sceGe: SetCallback, a raw list with SIGNAL and FINISH, EnQueue, ListSync");
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

    section_callbacks();

    probe_screen(1);
    probe_done();
    return 0;
}
