/* The GL backend's texture cache against guest memory written again: a
 * rewrite of the same bytes keeps the texels already uploaded, and any byte
 * that did change -- a texel, a palette entry, one row -- is decoded again.
 * Through real display lists, in a GL context from SDL's offscreen driver;
 * with no window the test skips (77). Synthetic data only. */
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

const psp_settings_schema psp_title_settings = { .title = "GL texture cache test", .id = "gl-texcache-test" };
const psp_title psp_title_info = { .name = "GL texture cache test" };

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

enum { PRIM_SPRITES = 6 };

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

/* A textured sprite, 1:1, through mode: u16 uv, colour, s16 position. */
#define VTYPE_2D_TEX (2u | (7u << 2) | (2u << 7) | (1u << 23))
static void vertex_uv(int i, int x, int y, int u, int v) {
    const uint32_t a = VERTS + (uint32_t)i * 16;
    psp_write16(a, (uint16_t)u); psp_write16(a + 2, (uint16_t)v);
    psp_write32(a + 4, 0xFFFFFFFFu);
    psp_write16(a + 8, (uint16_t)x); psp_write16(a + 10, (uint16_t)y); psp_write16(a + 12, 0);
}

/* One list: a texture drawn 1:1 at (x, y) into the 480-wide 8888 buffer at
 * FB, through the palette at clut (0: none). */
static void draw(uint32_t tex, int log2, int fmt, int swizzled, uint32_t clut, int x, int y) {
    psp_ge_reset();
    g_pc = 0;
    cmd(0x10, (VERTS >> 8) & 0xFF0000);                 /* BASE */
    cmd(0x9C, FB & 0xFFFFFF);                           /* FBP */
    cmd(0x9D, ((FB >> 8) & 0xFF0000) | 480u);           /* FBW */
    cmd(0xD2, 3);                                       /* 8888 */
    cmd(0xD4, 0); cmd(0xD5, 479u | 271u << 10);         /* SCISSOR: the screen */
    cmd(0x12, VTYPE_2D_TEX);
    cmd(0x01, VERTS & 0xFFFFFF);                        /* VADDR */
    if (clut) {
        cmd(0xB0, clut & 0xFFFFFF); cmd(0xB1, (clut >> 8) & 0x0F0000);
        cmd(0xC5, 3u | 0xFFu << 8);                     /* CLUTFORMAT: 8888, mask FF */
        cmd(0xC4, 32);                                  /* LOADCLUT: 256 entries */
    }
    cmd(0x1E, 1);                                       /* TEXTUREMAPENABLE */
    cmd(0xA0, tex & 0x00FFFFF0u);                       /* TEXADDR0 */
    cmd(0xA8, ((tex >> 8) & 0x000F0000u) | (1u << log2));
    cmd(0xB8, (uint32_t)log2 | ((uint32_t)log2 << 8));
    cmd(0xC3, (uint32_t)fmt);
    cmd(0xC2, (uint32_t)swizzled);
    cmd(0xC6, 0);                                       /* nearest */
    cmd(0xC9, 3u | 1u << 8);                            /* REPLACE, RGBA */
    const int size = 1 << log2;
    vertex_uv(0, x, y, 0, 0); vertex_uv(1, x + size, y + size, size, size);
    cmd(0x04, (PRIM_SPRITES << 16) | 2);
    cmd(0x0F, 0); cmd(0x0C, 0);                         /* FINISH, END */
    call(0xAB49E76A, LIST, 0, (uint32_t)-1);            /* sceGeListEnQueue */
    call(0xB287BD61, 0, 0, 0);                          /* sceGeDrawSync(WAIT) */
}

/* The colour at (x, y) of what the backend holds for FB. */
static uint32_t pixel(int x, int y) {
    psp_render_current()->finish();
    int w = 0, h = 0;
    uint32_t *shot = (uint32_t *)render_gl_capture(FB, &w, &h);
    const uint32_t c = shot && x < w && y < h ? shot[y * w + x] & 0x00FFFFFFu : 0xDEADBEEFu;
    free(shot);
    return c;
}

/* The number just before `label` in the GL report's texture line. */
static unsigned long long reported(const char *label) {
    static char text[1 << 16];
    FILE *f = tmpfile();
    if (!f) return ~0ull;
    render_gl_report(f);
    rewind(f);
    const size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = 0;
    const char *at = strstr(text, label);
    if (!at) return ~0ull;
    while (at > text && at[-1] >= '0' && at[-1] <= '9') at--;
    return strtoull(at, NULL, 10);
}

/* A CLUT8 texture, swizzled, its palette in VRAM next to it the way WipEout
 * keeps its pool. Rewriting both with the same bytes moves their write
 * generation; the first time that costs an upload (and the entry its copy),
 * after that nothing. A changed texel, then a changed palette entry, does. */
static void test_same_bytes(void) {
    const uint32_t TEX = 0x04100000u, CLUT = 0x04101000u;
    uint8_t texels[16 * 16];
    uint32_t palette[256] = {0};
    memset(texels, 1, sizeof texels);
    texels[5 * 16 + 5] = 2;              /* width 16: the swizzled layout is linear */
    palette[1] = 0xFF000000u | RED;
    palette[2] = 0xFF000000u | GREEN;
    psp_mem_write_block(TEX, texels, sizeof texels);
    psp_mem_write_block(CLUT, palette, sizeof palette);

    draw(TEX, 4, 5, 1, CLUT, 100, 100);
    const unsigned long long uploads = reported(" upload(s)");
    psp_mem_write_block(TEX, texels, sizeof texels);
    draw(TEX, 4, 5, 1, CLUT, 100, 100);
    CHECK(reported(" upload(s)") == uploads + 1, "the first rewrite of the same bytes did not upload again");

    const unsigned long long same = reported(" rewrite(s) of the same bytes");
    psp_mem_write_block(TEX, texels, sizeof texels);
    draw(TEX, 4, 5, 1, CLUT, 100, 100);
    psp_mem_write_block(CLUT, palette, sizeof palette);
    draw(TEX, 4, 5, 1, CLUT, 100, 100);
    CHECK(reported(" upload(s)") == uploads + 1, "a rewrite of the same texels or palette uploaded (%llu, want %llu)",
          reported(" upload(s)"), uploads + 1);
    CHECK(reported(" rewrite(s) of the same bytes") == same + 2, "same-byte rewrites counted %llu, want %llu",
          reported(" rewrite(s) of the same bytes"), same + 2);
    CHECK(pixel(102, 102) == RED && pixel(105, 105) == GREEN, "after same-byte rewrites the texture reads %06X/%06X",
          pixel(102, 102), pixel(105, 105));

    texels[6 * 16 + 6] = 2;
    psp_mem_write_block(TEX, texels, sizeof texels);
    draw(TEX, 4, 5, 1, CLUT, 100, 100);
    CHECK(pixel(106, 106) == GREEN, "a changed texel reads %06X, want %06X", pixel(106, 106), GREEN);

    palette[1] = 0xFF000000u | BLUE;
    psp_mem_write_block(CLUT, palette, sizeof palette);
    draw(TEX, 4, 5, 1, CLUT, 100, 100);
    CHECK(pixel(102, 102) == BLUE, "a changed palette entry reads %06X, want %06X", pixel(102, 102), BLUE);
    CHECK(reported(" upload(s)") == uploads + 3, "uploads %llu, want %llu after two real changes",
          reported(" upload(s)"), uploads + 3);
}

/* An 8888 texture, refreshed by rows: once the entry holds a copy, a rewrite
 * of every row with one of them changed decodes that row alone. */
static void test_rows(void) {
    const uint32_t TEX = 0x04120000u;
    static uint32_t texels[64 * 64];
    for (int i = 0; i < 64 * 64; i++) texels[i] = 0xFF000000u | RED;
    psp_mem_write_block(TEX, texels, sizeof texels);
    draw(TEX, 6, 3, 0, 0, 200, 100);
    psp_mem_write_block(TEX, texels, sizeof texels);
    draw(TEX, 6, 3, 0, 0, 200, 100);                    /* the copy is taken here */

    const unsigned long long kept = reported(" unchanged row(s) kept");
    for (int x = 0; x < 64; x++) texels[10 * 64 + x] = 0xFF000000u | GREEN;
    psp_mem_write_block(TEX, texels, sizeof texels);
    draw(TEX, 6, 3, 0, 0, 200, 100);
    CHECK(reported(" unchanged row(s) kept") == kept + 63, "rows kept %llu, want %llu",
          reported(" unchanged row(s) kept"), kept + 63);
    CHECK(pixel(205, 110) == GREEN && pixel(205, 109) == RED && pixel(205, 111) == RED,
          "rows 9-11 read %06X %06X %06X, want red, green, red", pixel(205, 109), pixel(205, 110), pixel(205, 111));
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
    const psp_render_backend *gl = render_gl_backend();
    if (!gl || psp_render_register(gl) || psp_render_select(gl->name)) return 2;
    present_want_gl();
    if (present_start() != 0) {
        printf("GL texture cache: no window, skipped\n");
        return 77;
    }
    if (gl->init(480, 272) != 0) return 2;

    test_same_bytes();
    test_rows();

    gl->shutdown();
    psp_mem_free();
    printf("GL texture cache: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
