/* Original synthetic host-upload bounds tests. No console image or emulator
 * source: known bytes at memory-region ends and exact decoded RGBA values. */
#include "psprecomp/render.h"
#include "psprecomp/mem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures;
#define CHECK(c) do { if (!(c)) { failures++; fprintf(stderr, "line %d: %s\n", __LINE__, #c); } } while (0)
static unsigned accesses;
static void observed(uint32_t a, uint32_t n) { (void)a; (void)n; accesses++; }

static void span(void) {
    const uint32_t end = PSP_VRAM_BASE + PSP_VRAM_SIZE;
    psp_mem_set_vram_access_observer(observed, PSP_VRAM_BASE, end);
    CHECK(psp_mem_mapped_span(end - 7) == 7);
    CHECK(psp_mem_mapped_span((end - 7) | 0x40000000u) == 7);
    CHECK(psp_mem_mapped_span(end) == 0);
    CHECK(psp_mem_mapped_span(PSP_RAM_BASE + PSP_RAM_SIZE - 1) == 1);
    CHECK(psp_mem_mapped_span(PSP_SCRATCH_BASE + PSP_SCRATCH_SIZE - 3) == 3);
    CHECK(psp_mem_map_module(0, 4096) == 0);
    CHECK(psp_mem_mapped_span(4095) == 1 && psp_mem_mapped_span(4096) == 0);
    CHECK(accesses == 0);                    /* generation metadata must not read back an RT */
    psp_mem_set_vram_access_observer(NULL, 0, 0);
}

static void direct_and_mip(void) {
    const uint32_t address = PSP_VRAM_BASE + PSP_VRAM_SIZE - 512 * 272 * 4;
    for (unsigned y = 0; y < 272; y++)
        for (unsigned x = 0; x < 512; x++)
            psp_write32(address + (y * 512 + x) * 4, 0xff000000u | (y << 12) | x);
    uint32_t *pixels = malloc(512 * 512 * sizeof *pixels);
    CHECK(pixels != NULL); if (!pixels) return;
    psp_tex_state t = {.addr = address, .stride = 512, .w = 512, .h = 512, .fmt = 3};
    int w = 0, h = 0; size_t padded = 0;
    const uint64_t bad = psp_mem_bad_access;
    CHECK(psp_render_decode_level_padded(&t, 0, NULL, pixels, 512 * 512, &w, &h, &padded) == 512 * 512);
    CHECK(w == 512 && h == 512 && padded == 512 * 240);
    for (unsigned y = 0; y < 512; y++)
        for (unsigned x = 0; x < 512; x++)
            CHECK(pixels[y * 512 + x] == (y < 272 ? 0xff000000u | (y << 12) | x : 0));
    t.addr |= 0x40000000u;
    psp_write32(address + (271 * 512 + 511) * 4, 0xff123456);
    CHECK(psp_render_decode_level_padded(&t, 0, NULL, pixels, 512 * 512, NULL, NULL, &padded) == 512 * 512);
    CHECK(pixels[271 * 512 + 511] == 0xff123456);
    t.lv_addr[1] = PSP_VRAM_BASE + PSP_VRAM_SIZE - 6;
    t.lv_stride[1] = 2; t.lv_w[1] = 2; t.lv_h[1] = 2;
    psp_write32(t.lv_addr[1], 0xffaabbcc);
    CHECK(psp_render_decode_level_padded(&t, 1, NULL, pixels, 4, &w, &h, &padded) == 4);
    CHECK(w == 2 && h == 2 && padded == 3 && pixels[0] == 0xffaabbcc);
    CHECK(pixels[1] == 0 && pixels[2] == 0 && pixels[3] == 0); /* straddling texel */
    CHECK(psp_mem_bad_access == bad);
    /* The strict decoder must still report a genuine out-of-bounds sample. */
    t.addr = PSP_VRAM_BASE + PSP_VRAM_SIZE; t.w = t.h = t.stride = 1;
    CHECK(psp_render_decode_level(&t, 0, NULL, pixels, 1, NULL, NULL) == 1);
    CHECK(psp_mem_bad_access == bad + 1);
    free(pixels);
}

static void packed_and_palette(void) {
    const uint32_t end = PSP_VRAM_BASE + PSP_VRAM_SIZE;
    uint32_t pixels[512]; size_t padded = 0;
    const uint64_t bad = psp_mem_bad_access;
    /* A partial swizzle block contains rows interleaved by byte blocks.
     * Populate the first 16x8-byte tile: rows 0..7, columns 0..3 of RGBA8. */
    psp_tex_state t = {.addr = end - 128, .stride = 8, .w = 8, .h = 8, .fmt = 3, .swizzled = 1};
    for (unsigned i = 0; i < 32; i++) psp_write32(t.addr + 4 * i, 0xffabcdef);
    CHECK(psp_render_decode_level_padded(&t, 0, NULL, pixels, 512, NULL, NULL, &padded) == 64);
    CHECK(padded == 32);
    for (unsigned y = 0; y < 8; y++)
        for (unsigned x = 0; x < 8; x++) CHECK(pixels[y * 8 + x] == (x < 4 ? 0xffabcdef : 0));
    /* CLUT4 nibbles and a partially backed palette are checked independently. */
    psp_clut_state clut = {.addr = PSP_RAM_BASE, .fmt = 3, .mask = 15};
    psp_write32(clut.addr + 4, 0xff102030); psp_write32(clut.addr + 8, 0xff405060);
    t = (psp_tex_state){.addr = end - 1, .stride = 4, .w = 4, .h = 1, .fmt = 4};
    psp_write8(t.addr, 0x21);
    CHECK(psp_render_decode_level_padded(&t, 0, &clut, pixels, 512, NULL, NULL, &padded) == 4);
    CHECK(padded == 2 && pixels[0] == 0xff102030 && pixels[1] == 0xff405060 && !pixels[2] && !pixels[3]);
    clut.addr = end - 8;
    psp_write32(clut.addr, 0xff112233); psp_write32(clut.addr + 4, 0xff445566);
    t = (psp_tex_state){.addr = PSP_RAM_BASE + 64, .stride = 4, .w = 4, .h = 1, .fmt = 5};
    psp_write32(t.addr, 0x03020100);
    CHECK(psp_render_decode_level_padded(&t, 0, &clut, pixels, 512, NULL, NULL, &padded) == 4);
    CHECK(padded == 2 && pixels[0] == 0xff112233 && pixels[1] == 0xff445566 && !pixels[2] && !pixels[3]);
    /* Direct 16-bit formats retain their expansion in the last complete texel. */
    for (int fmt = 0; fmt < 3; fmt++) {
        t = (psp_tex_state){.addr = end - 2, .stride = 2, .w = 2, .h = 1, .fmt = fmt};
        psp_write16(t.addr, 0xffff);
        CHECK(psp_render_decode_level_padded(&t, 0, NULL, pixels, 512, NULL, NULL, &padded) == 2);
        CHECK(pixels[0] == 0xffffffff && pixels[1] == 0 && padded == 1);
    }
    CHECK(psp_mem_bad_access == bad);
}

int main(void) {
    if (psp_mem_init()) return 2;
    span(); direct_and_mip(); packed_and_palette();
    psp_mem_free();
    CHECK(psp_mem_mapped_span(PSP_VRAM_BASE) == 0);
    printf("Texture upload bounds: %u failures\n", failures);
    return failures ? 1 : 0;
}
