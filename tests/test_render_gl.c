/* The GL backend against the rules the software path keeps (src/render.c),
 * through real display lists: list in, the target's pixels out. It needs a
 * GL context, which SDL's offscreen driver gives where there is EGL; with no
 * window the test skips (77). Synthetic lists only, no game data. */
#include "psprecomp/hle.h"
#include "psprecomp/render.h"
#include "psprecomp/sched.h"
#include "psprecomp/clock.h"
#include "psprecomp/host/present.h"
#include "psprecomp/host/render_gl.h"
#include "psprecomp/host/settings.h"
#include "psprecomp/host/title.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const psp_settings_schema psp_title_settings = { .title = "GL test", .id = "gl-test" };
const psp_title psp_title_info = { .name = "GL test" };

static int failures;
#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);        \
            printf(__VA_ARGS__);                               \
            printf("\n");                                      \
            failures++;                                        \
        }                                                      \
    } while (0)

#define FB    0x04000000u
#define LIST  0x08800000u
#define VERTS 0x08810000u

#define RED   0x0000FFu
#define GREEN 0x00FF00u
#define BLUE  0xFF0000u
#define WHITE 0xFFFFFFu

/* VTYPE: 8888 colour and 16-bit position, through mode; and 8888 colour with
 * float position, transformed. */
#define VTYPE_2D ((7u << 2) | (2u << 7) | (1u << 23))
#define VTYPE_3D ((7u << 2) | (3u << 7))

enum { PRIM_LINES = 1, PRIM_LINE_STRIP, PRIM_TRIANGLES, PRIM_STRIP, PRIM_FAN, PRIM_SPRITES };

/* The real backend, and the copy the GE draws through: it counts the model
 * path's draws, so a 3D case can tell which path it took. */
static const psp_render_backend *real_gl;
static unsigned model_draws;
static void counted_model(int prim, const psp_model_vertex *v, int count, const psp_xform_state *xs) {
    model_draws++;
    real_gl->draw_model(prim, v, count, xs);
}

static uint32_t call(uint32_t nid, uint32_t a0, uint32_t a1, uint32_t a2) {
    psp_cpu.r[PSP_REG_A0] = a0; psp_cpu.r[PSP_REG_A1] = a1;
    psp_cpu.r[PSP_REG_A2] = a2; psp_cpu.r[PSP_REG_A3] = 0;
    psp_hle_call(nid);
    return psp_cpu.r[PSP_REG_V0];
}

static uint32_t g_pc;
static void cmd(uint8_t op, uint32_t arg) {
    psp_write32(LIST + g_pc, ((uint32_t)op << 24) | (arg & 0xFFFFFF));
    g_pc += 4;
}
static void cmd_float(uint8_t op, float f) {
    uint32_t bits;
    memcpy(&bits, &f, 4);
    cmd(op, bits >> 8);
}

/* A list drawing into the colour buffer at fb, `stride` pixels wide, whose
 * vertices start at VERTS. */
static void begin_list(uint32_t fb, int stride, uint32_t vtype) {
    psp_ge_reset();
    g_pc = 0;
    cmd(0x10, (VERTS >> 8) & 0xFF0000);                 /* BASE */
    cmd(0x9C, fb & 0xFFFFFF);                           /* FBP */
    cmd(0x9D, ((fb >> 8) & 0xFF0000) | (uint32_t)stride); /* FBW + address high byte */
    cmd(0xD2, 3);                                       /* 8888 */
    cmd(0xD4, 0); cmd(0xD5, 479u | 271u << 10);         /* SCISSOR: the screen */
    cmd(0x12, vtype);                                   /* VTYPE */
    cmd(0x01, VERTS & 0xFFFFFF);                        /* VADDR */
}
static void end_list(void) {
    cmd(0x0F, 0); cmd(0x0C, 0);                        /* FINISH, END */
    call(0xAB49E76A, LIST, 0, (uint32_t)-1);            /* sceGeListEnQueue */
    call(0xB287BD61, 0, 0, 0);                          /* sceGeDrawSync(WAIT) */
}

static void vertex_2d(int i, int x, int y, uint32_t rgb) {
    const uint32_t a = VERTS + (uint32_t)i * 12;
    psp_write32(a, 0xFF000000u | rgb);
    psp_write16(a + 4, (uint16_t)x); psp_write16(a + 6, (uint16_t)y); psp_write16(a + 8, 0);
}
static void vertex_3d(int i, float x, float y, float z, uint32_t rgb) {
    const uint32_t a = VERTS + (uint32_t)i * 16;
    const float pos[3] = { x, y, z };
    psp_write32(a, 0xFF000000u | rgb);
    for (int k = 0; k < 3; k++) { uint32_t b; memcpy(&b, &pos[k], 4); psp_write32(a + 4 + 4 * k, b); }
}

/* The target at addr as the GL backend holds it, colour only (an ordinary
 * draw leaves the alpha byte, the stencil, alone). */
static uint32_t *g_shot;
static int g_shot_w, g_shot_h;
static void shoot(uint32_t addr) {
    psp_render_current()->finish();
    free(g_shot);
    g_shot = (uint32_t *)render_gl_capture(addr, &g_shot_w, &g_shot_h);
    CHECK(g_shot != NULL, "no GL target at %08X", addr);
}
static uint32_t shot(int x, int y) {
    if (!g_shot || x < 0 || y < 0 || x >= g_shot_w || y >= g_shot_h) return 0xDEADBEEFu;
    return g_shot[y * g_shot_w + x] & 0x00FFFFFFu;
}
/* Every lit pixel in a box is one colour: answers how many there are, and
 * counts the others in *other. */
static int lit_in(int x0, int y0, int x1, int y1, uint32_t rgb, int *other) {
    int n = 0;
    *other = 0;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            const uint32_t c = shot(x, y);
            if (!c) continue;
            if (c == rgb) n++; else (*other)++;
        }
    return n;
}

/* SHADE clear: a triangle is its last vertex's colour, the third as draw()
 * receives it, and a line its second vertex's (render.c sw_tri and its
 * line walk; geprobe 1, 14 and 15, fw 6.60). A strip's triangle and a
 * fan's take their own last vertex. One 64-pixel cell per case. */
static void test_flat_through(void) {
    /* A list, flat; then the same triangle smooth, which is not one colour. */
    for (int smooth = 0; smooth < 2; smooth++) {
        const int x0 = 8 + 72 * smooth, y0 = 8;
        begin_list(FB, 480, VTYPE_2D);
        cmd(0x50, (uint32_t)smooth);                    /* SHADE */
        vertex_2d(0, x0, y0, RED); vertex_2d(1, x0 + 60, y0, GREEN); vertex_2d(2, x0, y0 + 60, BLUE);
        cmd(0x04, (PRIM_TRIANGLES << 16) | 3);
        end_list();
        shoot(FB);
        const uint32_t c = shot(x0 + 15, y0 + 15);
        if (smooth) CHECK(c != BLUE && c != 0, "smooth triangle reads %06X, a blend", c);
        else        CHECK(c == BLUE, "flat triangle reads %06X, want its last vertex's %06X", c, BLUE);
    }
    {   /* A strip: (0,1,2) is v2's colour, (1,2,3) v3's. */
        const int x0 = 152, y0 = 8;
        begin_list(FB, 480, VTYPE_2D);
        cmd(0x50, 0);
        vertex_2d(0, x0, y0, RED); vertex_2d(1, x0, y0 + 60, GREEN);
        vertex_2d(2, x0 + 60, y0, BLUE); vertex_2d(3, x0 + 60, y0 + 60, WHITE);
        cmd(0x04, (PRIM_STRIP << 16) | 4);
        end_list();
        shoot(FB);
        CHECK(shot(x0 + 10, y0 + 10) == BLUE && shot(x0 + 50, y0 + 50) == WHITE,
              "flat strip reads %06X and %06X, want %06X and %06X",
              shot(x0 + 10, y0 + 10), shot(x0 + 50, y0 + 50), BLUE, WHITE);
    }
    {   /* A fan round its centre: (0,1,2) is v2's colour, (0,2,3) v3's. */
        const int x0 = 224, y0 = 8;
        begin_list(FB, 480, VTYPE_2D);
        cmd(0x50, 0);
        vertex_2d(0, x0 + 30, y0 + 30, RED); vertex_2d(1, x0, y0, GREEN);
        vertex_2d(2, x0 + 60, y0, BLUE); vertex_2d(3, x0 + 60, y0 + 60, WHITE);
        cmd(0x04, (PRIM_FAN << 16) | 4);
        end_list();
        shoot(FB);
        CHECK(shot(x0 + 30, y0 + 8) == BLUE && shot(x0 + 52, y0 + 30) == WHITE,
              "flat fan reads %06X and %06X, want %06X and %06X",
              shot(x0 + 30, y0 + 8), shot(x0 + 52, y0 + 30), BLUE, WHITE);
    }
    {   /* A line, and a strip of two lines. */
        const int x0 = 8, y0 = 88;
        begin_list(FB, 480, VTYPE_2D);
        cmd(0x50, 0);
        vertex_2d(0, x0, y0, RED); vertex_2d(1, x0 + 60, y0, BLUE);
        cmd(0x04, (PRIM_LINES << 16) | 2);
        cmd(0x01, (VERTS + 2 * 12) & 0xFFFFFF);
        vertex_2d(2, x0 + 80, y0, RED); vertex_2d(3, x0 + 140, y0, GREEN); vertex_2d(4, x0 + 140, y0 + 60, BLUE);
        cmd(0x04, (PRIM_LINE_STRIP << 16) | 3);
        end_list();
        shoot(FB);
        int other, n = lit_in(x0, y0 - 2, x0 + 62, y0 + 2, BLUE, &other);
        CHECK(n >= 50 && other == 0, "flat line: %d pixel(s) of its second vertex's %06X, %d other", n, BLUE, other);
        n = lit_in(x0 + 82, y0 - 2, x0 + 136, y0 + 2, GREEN, &other);
        CHECK(n >= 50 && other == 0, "flat line strip, first line: %d pixel(s) of %06X, %d other", n, GREEN, other);
        n = lit_in(x0 + 138, y0 + 4, x0 + 142, y0 + 60, BLUE, &other);
        CHECK(n >= 50 && other == 0, "flat line strip, second line: %d pixel(s) of %06X, %d other", n, BLUE, other);
    }
}

/* The same rule for transformed geometry: a perspective triangle, which the
 * GPU model path draws, and a line, which the CPU path does. */
static void test_flat_3d(void) {
    /* sceGumPerspective(60, 480/272, 1, 100), column-major. */
    static const float PROJ[16] = { 0.981491089f, 0, 0, 0, 0, 1.73202515f, 0, 0,
                                    0, 0, -1.02017212f, -1, 0, 0, -2.0201416f, 0 };
    begin_list(FB, 480, VTYPE_3D);
    for (int m = 0; m < 2; m++) {                       /* world, view: identity */
        cmd((uint8_t)(0x3A + 2 * m), 0);
        for (int i = 0; i < 12; i++) cmd((uint8_t)(0x3B + 2 * m), i % 4 == 0 ? 0x3F8000 : 0);
    }
    cmd(0x3E, 0);
    for (int i = 0; i < 16; i++) cmd_float(0x3F, PROJ[i]);
    cmd_float(0x42, 240.0f); cmd_float(0x43, -136.0f); cmd_float(0x44, -32768.0f);
    cmd_float(0x45, 2048.0f); cmd_float(0x46, 2048.0f); cmd_float(0x47, 32767.0f);
    cmd(0x4C, 1808u << 4); cmd(0x4D, 1912u << 4);
    cmd(0x50, 0);                                       /* SHADE: flat */
    /* x -1..1 and y -1..1 at z -4.5 land 188..292 and 84..188. */
    vertex_3d(0, -1, -1, -4.5f, RED); vertex_3d(1, 1, -1, -4.5f, GREEN); vertex_3d(2, 0, 1, -4.5f, BLUE);
    cmd(0x04, (PRIM_TRIANGLES << 16) | 3);
    cmd(0x01, (VERTS + 3 * 16) & 0xFFFFFF);
    vertex_3d(3, -1, -1.3f, -4.5f, RED); vertex_3d(4, 1, -1.3f, -4.5f, GREEN);
    cmd(0x04, (PRIM_LINES << 16) | 2);
    const unsigned before = model_draws;
    end_list();
    shoot(FB);
    CHECK(model_draws > before, "the 3D triangle did not take the model path");
    CHECK(shot(240, 140) == BLUE, "flat 3D triangle reads %06X, want its last vertex's %06X",
          shot(240, 140), BLUE);
    int other, n = lit_in(180, 190, 300, 210, GREEN, &other);
    CHECK(n >= 50 && other == 0, "flat 3D line: %d pixel(s) of its second vertex's %06X, %d other", n, GREEN, other);
}

/* A textured sprite, 1:1, through mode: u16 uv, colour, s16 position. */
#define VTYPE_2D_TEX (2u | (7u << 2) | (2u << 7) | (1u << 23))
static void vertex_uv(int i, int x, int y, int u, int v) {
    const uint32_t a = VERTS + (uint32_t)i * 16;
    psp_write16(a, (uint16_t)u); psp_write16(a + 2, (uint16_t)v);
    psp_write32(a + 4, 0xFFFFFFFFu);
    psp_write16(a + 8, (uint16_t)x); psp_write16(a + 10, (uint16_t)y); psp_write16(a + 12, 0);
}
static void texture(uint32_t addr, int stride, int log2w, int log2h, int fmt) {
    cmd(0x1E, 1);                                       /* TEXTUREMAPENABLE */
    cmd(0xA0, addr & 0x00FFFFF0u);                      /* TEXADDR0 */
    cmd(0xA8, ((addr >> 8) & 0x000F0000u) | (uint32_t)stride);
    cmd(0xB8, (uint32_t)log2w | ((uint32_t)log2h << 8));
    cmd(0xC3, (uint32_t)fmt);
    cmd(0xC2, 0);                                       /* not swizzled */
    cmd(0xC6, 0);                                       /* nearest */
    cmd(0xC9, 3u | 1u << 8);                            /* REPLACE, RGBA */
}
static void sprite_tex(int x, int y, int w, int h) {
    vertex_uv(0, x, y, 0, 0); vertex_uv(1, x + w, y + h, w, h);
    cmd(0x04, (PRIM_SPRITES << 16) | 2);
}

/* A texture's bytes, for finding the render targets under it: its texels
 * at their own size. A CLUT32 texture taken at two bytes a texel ended
 * halfway, so a target drawn into its second half was neither read back nor
 * seen, and the decoder read what guest memory held before the draw. The
 * target here holds palette index 5 (green) from the GPU; guest memory still
 * reads index 0 (red). */
static void test_texture_span(void) {
    const uint32_t TEX = 0x04100000u, CLUT = 0x08900000u;
    const uint32_t TARGET = TEX + 48 * 64 * 4;          /* texture row 48 of 64 */
    psp_write32(CLUT, 0xFF000000u | RED);
    psp_write32(CLUT + 5 * 4, 0xFF000000u | GREEN);
    begin_list(TARGET, 64, VTYPE_2D);
    cmd(0xD4, 0); cmd(0xD5, 63u | 15u << 10);           /* SCISSOR: 64x16 */
    vertex_2d(0, 0, 0, 0x000005u); vertex_2d(1, 64, 16, 0x000005u);
    cmd(0x04, (PRIM_SPRITES << 16) | 2);
    end_list();

    begin_list(FB, 480, VTYPE_2D_TEX);
    cmd(0xB0, CLUT & 0xFFFFFF); cmd(0xB1, (CLUT >> 8) & 0x0F0000);
    cmd(0xC5, 3u | 0xFFu << 8);                         /* CLUTFORMAT: 8888, mask FF */
    cmd(0xC4, 2);                                       /* LOADCLUT: 16 entries */
    texture(TEX, 64, 6, 6, 7);                          /* CLUT32 */
    sprite_tex(8, 160, 64, 64);
    end_list();
    shoot(FB);
    CHECK(shot(18, 170) == RED, "CLUT32 texel above the target reads %06X, want %06X", shot(18, 170), RED);
    CHECK(shot(18, 210) == GREEN, "CLUT32 texel in the target reads %06X, want the GPU's %06X",
          shot(18, 210), GREEN);
}

int main(void) {
    if (psp_mem_init()) return 2;
    psp_hle_init(); psp_cpu_reset(); psp_sched_set_threading(0); psp_clock_reset();
    psp_display_reset(); psp_ge_reset();
    psp_settings settings;
    char error[PSP_SETTINGS_ERROR];
    psp_settings_defaults(&settings);
    if (psp_settings_set(&settings, PSP_OPT_RENDER, "gl", PSP_SOURCE_COMMAND_LINE, error) ||
        psp_settings_resolve(&settings, error)) {
        fprintf(stderr, "settings: %s\n", error);
        return 2;
    }
    psp_settings_use(&settings);
    real_gl = render_gl_backend();
    if (!real_gl) return 2;
    psp_render_backend counted = *real_gl;
    counted.name = "counted-gl";
    counted.draw_model = counted_model;
    if (psp_render_register(&counted) || psp_render_select("counted-gl")) return 2;
    present_want_gl();
    if (present_start() != 0) {
        printf("GL backend: no window, skipped\n");
        return 77;
    }
    if (counted.init(480, 272) != 0) return 2;

    test_flat_through();
    test_flat_3d();
    test_texture_span();

    free(g_shot);
    counted.shutdown();
    psp_mem_free();
    printf("GL backend: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
