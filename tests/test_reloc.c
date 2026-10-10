/* A PRX's relocations in the forms that live in a segment rather than in
 * sections: the packed PT_PRXRELOC2 (0x700000A1) and Elf32_Rel entries in a
 * PT_PRXRELOC (0x700000A0). Each module here is two segments moved by
 * 0x00400000, so every patched word says whether its kind was read and
 * applied right. Synthetic: no game data. */
#include "reloc.h"
#include "loader.h"

#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %d: ", __LINE__); \
                           fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static void w16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v) { w16(p, v); w16(p + 2, v >> 16); }
static uint32_t r32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

enum { PH = 52, SEG0 = 0x100, SEG1 = 0x200, REL = 0x300, MOVE = 0x00400000 };

/* Segment 0, linked at 0: a jal, and a lui/addiu pair naming segment 1's
 * 0x10. Segment 1, linked at 0x100: a word naming segment 0's 0x8. */
static void module(uint8_t *d, elf_info *e, uint32_t rel_type, uint32_t rel_size) {
    memset(d, 0, 0x400);
    w32(d + 28, PH); w16(d + 42, 32); w16(d + 44, 3);
    const uint32_t ph[3][3] = { { 1, SEG0, 16 }, { 1, SEG1, 8 }, { rel_type, REL, rel_size } };
    for (int i = 0; i < 3; i++) {
        w32(d + PH + 32 * i, ph[i][0]);
        w32(d + PH + 32 * i + 4, ph[i][1]);
        w32(d + PH + 32 * i + 16, ph[i][2]);
    }
    w32(d + SEG0 + 0, 0x0C000010);   /* jal   0x40 */
    w32(d + SEG0 + 4, 0x3C040000);   /* lui   $a0, 0 */
    w32(d + SEG0 + 8, 0x24840010);   /* addiu $a0, $a0, 0x10 */
    w32(d + SEG1 + 0, 0x00000008);   /* .word 8 */

    memset(e, 0, sizeof *e);
    e->type = ET_PSP_PRX;
    e->nsegments = 2;
    e->seg[0] = (elf_segment){ MOVE + 0x000, SEG0, 16, 16, 5 };
    e->seg[1] = (elf_segment){ MOVE + 0x100, SEG1, 8, 8, 6 };
}

static void moved_right(const uint8_t *d, const char *form) {
    CHECK(r32(d + SEG0 + 0) == 0x0C100010, "%s: jal moves to 0x400040, got %08X", form, r32(d + SEG0));
    CHECK(r32(d + SEG0 + 4) == 0x3C040040, "%s: lui takes 0x400110's upper half, got %08X", form, r32(d + SEG0 + 4));
    CHECK(r32(d + SEG0 + 8) == 0x24840110, "%s: addiu takes its lower half, got %08X", form, r32(d + SEG0 + 8));
    CHECK(r32(d + SEG1 + 0) == 0x00400008, "%s: the word moves with segment 0, got %08X", form, r32(d + SEG1));
}

/* What QueryModuleInfo reports of a module's sizes, as the PSP's loader
 * counts them (elf_exec_sizes; modprobe step 4, fw 6.60). A PRX's by
 * section, exact flags only; a static ELF's by segment. */
static void exec_sizes(void) {
    enum { SH = 0x40, NS = 9 };
    static uint8_t d[SH + 40 * NS];
    memset(d, 0, sizeof d);
    /* type, flags, size */
    const uint32_t sh[NS][3] = {
        { 0, 0, 0 },                     /* the null section */
        { 1, 0x6, 0x100 },               /* .text: AX, text */
        { 1, 0x2, 0x20 },                /* .rodata.sceModuleInfo: A, text */
        { 1, 0x32, 0x10 },               /* .rodata merged strings: AMS, nowhere */
        { 0x7000002A, 0x2, 0x18 },       /* .MIPS.abiflags: not PROGBITS, nowhere */
        { 1, 0x3, 0x30 },                /* .data: WA, data */
        { 1, 0x10000003, 0x8 },          /* .sdata: WA and gp-relative, nowhere */
        { 8, 0x3, 0x40 },                /* .bss: NOBITS WA, bss */
        { 8, 0x10000003, 0x4 },          /* .sbss: nowhere */
    };
    for (int i = 0; i < NS; i++) {
        w32(d + SH + 40 * i + 4, sh[i][0]);
        w32(d + SH + 40 * i + 8, sh[i][1]);
        w32(d + SH + 40 * i + 20, sh[i][2]);
    }
    elf_info e;
    memset(&e, 0, sizeof e);
    e.type = ET_PSP_PRX;
    e.shoff = SH; e.shentsize = 40; e.shnum = NS;
    uint32_t text, data, bss;
    elf_exec_sizes(d, sizeof d, &e, &text, &data, &bss);
    CHECK(text == 0x120 && data == 0x30 && bss == 0x40,
          "a PRX's sizes by section: text %X data %X bss %X, expected 120 30 40", text, data, bss);

    e.type = 2;                          /* ET_EXEC */
    e.nsegments = 2;
    e.seg[0] = (elf_segment){ 0x08804000, 0, 0x100, 0x100, 5 };
    e.seg[1] = (elf_segment){ 0x08804100, 0, 0x20, 0x60, 6 };
    elf_exec_sizes(d, sizeof d, &e, &text, &data, &bss);
    CHECK(text == 0x100 && data == 0x20 && bss == 0x40,
          "a static ELF's by segment: text %X data %X bss %X, expected 100 20 40", text, data, bss);
}

int main(void) {
    static uint8_t d[0x400];
    elf_info e;
    exec_sizes();

    /* Packed: part1s 2, part2s 3, one segment bit. First table: entry 0 is
     * the table's own size, 4, which reads as "segment, 32-bit offset
     * follows"; 1 a relocation stepping by the high bits; 2 the same with a
     * 16-bit addend after; 3 "segment, offset in the high bits". Second
     * table: entry k is kind k. */
    static const uint8_t stream[] = {
        0, 0, 2, 3,
        4, 0x01, 0x11, 0x00,                 /* first table */
        8, 1, 2, 3, 4, 5, 6, 7,              /* second table */
        0x00, 0x00, 0, 0, 0, 0,              /* segment 0, offset 0 */
        0x39, 0x00,                          /* +0: kind 7, jal, value in segment 0 */
        0x26, 0x01, 0x10, 0x00,              /* +4: kind 4, upper half, segment 1, addend 0x10 */
        0x2D, 0x01,                          /* +4: kind 5, lower half, segment 1 */
        0x07, 0x00,                          /* segment 1, offset 0 */
        0x11, 0x00,                          /* +0: kind 2, word, value in segment 0 */
    };
    module(d, &e, 0x700000A1, sizeof stream);
    memcpy(d + REL, stream, sizeof stream);
    psp_reloc r[8];
    CHECK(psp_relocs_read(d, sizeof d, &e, 0, r, 8) == 4, "packed: four relocations");
    CHECK(r[0].kind == PSP_RELOC_26 && r[1].kind == PSP_RELOC_HI16_ADDEND && r[1].addend == 0x10 &&
          r[2].kind == PSP_RELOC_LO16 && r[3].kind == PSP_RELOC_32 && r[3].ofs_seg == 1,
          "packed: kinds, addend and segments as encoded");
    psp_load_info li;
    CHECK(psp_relocate_image(d, sizeof d, &e, &li) == 0 && li.nrelocs == 4 && !li.nreloc_skipped,
          "packed: all four applied");
    moved_right(d, "packed");

    /* Truncated after the second table: nothing to read, not malformed. Cut
     * inside a 32-bit offset: malformed. */
    module(d, &e, 0x700000A1, 16);
    memcpy(d + REL, stream, 16);
    CHECK(psp_relocs_read(d, sizeof d, &e, 0, r, 8) == 0, "packed: tables alone hold no relocations");
    module(d, &e, 0x700000A1, 20);
    memcpy(d + REL, stream, 20);
    CHECK(psp_relocs_read(d, sizeof d, &e, 0, r, 8) == -1, "packed: a cut offset is malformed");

    /* Seeds from packed upper halves, in the relocated image. The first names
     * segment 1's 0x8, data; its stored low half alone would name 0x400008,
     * inside the code. The second names segment 0's 0x4, which is code. */
    static const uint8_t halves[] = {
        0, 0, 2, 3,
        4, 0x01, 0x11, 0x00,
        8, 1, 2, 3, 4, 5, 6, 7,
        0x00, 0x00, 0, 0, 0, 0,              /* segment 0, offset 0 */
        0x26, 0x01, 0x08, 0x00,              /* +4: kind 4, segment 1, addend 8 */
        0x22, 0x02, 0x04, 0x00,              /* +12: kind 4, segment 0, addend 4 */
    };
    module(d, &e, 0x700000A1, sizeof halves);
    memcpy(d + REL, halves, sizeof halves);
    e.text_addr = MOVE; e.text_size = 16;
    CHECK(psp_relocate_image(d, sizeof d, &e, &li) == 0 && li.nrelocs == 2, "halves: both applied");
    uint32_t seeds[4];
    const int nseeds = psp_collect_pointer_seeds(d, sizeof d, &e, 0, seeds, 4);
    CHECK(nseeds == 1 && seeds[0] == MOVE + 4,
          "halves: only the code address seeds, got %d (%08X)", nseeds, nseeds ? seeds[0] : 0);

    /* Elf32_Rel in a segment: r_info is the type, then the patched segment,
     * then the value's. */
    static const uint32_t rel[][2] = {
        { 0, 0x00000004 },                   /* R_MIPS_26, segment 0, value in 0 */
        { 4, 0x00010005 },                   /* R_MIPS_HI16, value in segment 1 */
        { 8, 0x00010006 },                   /* R_MIPS_LO16 */
        { 0, 0x00000102 },                   /* R_MIPS_32 in segment 1, value in 0 */
    };
    module(d, &e, 0x700000A0, sizeof rel);
    for (unsigned i = 0; i < 4; i++) { w32(d + REL + 8 * i, rel[i][0]); w32(d + REL + 8 * i + 4, rel[i][1]); }
    /* The classic pair carries no addend: the LO16 word holds it. */
    CHECK(psp_relocate_image(d, sizeof d, &e, &li) == 0 && li.nrelocs == 4 && !li.nreloc_skipped,
          "segment Elf32_Rel: all four applied");
    moved_right(d, "segment Elf32_Rel");

    printf("relocations: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
