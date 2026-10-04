/* allegrexrecomp — module loading. See loader.h. */

#include "loader.h"

#include "psprecomp/mem.h"

#include <stdlib.h>
#include <string.h>

/* PRX keeps its relocations in a section of its own type rather than SHT_REL,
 * which is why `readelf -r` reports a module like this as having none. */
#define SHT_PRXRELOC 0x700000A0u

#define R_MIPS_NONE  0
#define R_MIPS_16    1
#define R_MIPS_32    2
#define R_MIPS_REL32 3
#define R_MIPS_26    4
#define R_MIPS_HI16  5
#define R_MIPS_LO16  6
#define R_MIPS_GPREL16 7

/* r_info packs the MIPS type with two program-header selectors:
 *
 *   OFS_BASE   which segment r_offset is measured from — i.e. where to patch
 *   ADDR_BASE  which segment's base to add — i.e. what the stored value is
 *              relative to
 *
 * Both are indices into the PT_LOAD list. */
#define R_TYPE(i)      ((i) & 0xFFu)
#define R_OFS_BASE(i)  (((i) >> 8) & 0xFFu)
#define R_ADDR_BASE(i) (((i) >> 16) & 0xFFu)

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* Relocations are applied to the *file image*, not to guest memory.
 *
 * That matters more than it looks. Patching memory after loading fixes the
 * interpreter, which fetches instructions from memory -- and leaves the
 * recompiled C untouched, because its address literals were baked in when the
 * emitter read the file. The two then compute different addresses from the same
 * instruction, which is a divergence created entirely by the loader.
 *
 * Relocating the image up front means the emitter and the interpreter consume
 * identical bytes, so both are relocated and both agree. */
typedef struct {
    uint8_t       *data;
    size_t         len;
    const elf_segment *seg;
    int            nseg;
} image;

/* Guest address -> file offset, via the segment that contains it. */
static uint8_t *at(const image *im, uint32_t seg_idx, uint32_t off) {
    if ((int)seg_idx >= im->nseg) return NULL;
    const elf_segment *s = &im->seg[seg_idx];
    if (off + 4 > s->filesz) return NULL;         /* .bss has no file bytes */
    const size_t o = (size_t)s->offset + off;
    if (o + 4 > im->len) return NULL;
    return im->data + o;
}

/* A HI16 carries only the top half of an address, and the bottom half — with
 * its sign — lives in a LO16 that may be several instructions away. Neither can
 * be patched alone, so HI16s are held until the LO16 that completes them
 * arrives. More than one HI16 may share a single LO16, which is why this is a
 * list rather than one pending entry. */
#define MAX_PENDING_HI 64

typedef struct {
    uint8_t *p[MAX_PENDING_HI];
    int      n;
    int      overflow;
} hi_queue;

static void apply_lo16(hi_queue *q, uint8_t *lo_p, uint32_t delta) {
    const uint32_t lo_op = rd32(lo_p);
    const int32_t  lo_s  = (int16_t)(lo_op & 0xFFFF);

    /* Each pending HI16 combines with *this* LO16 to name a full address. */
    for (int i = 0; i < q->n; i++) {
        uint8_t *hi_p = q->p[i];
        const uint32_t hi_op = rd32(hi_p);
        const uint32_t full  = (uint32_t)(((hi_op & 0xFFFF) << 16) + lo_s + delta);
        /* Split back so that (hi << 16) + (int16_t)lo reproduces `full`; the
         * subtraction is what carries the sign of the low half into the high. */
        const uint32_t new_lo = full & 0xFFFF;
        const uint32_t new_hi = (uint32_t)((full - (uint32_t)(int32_t)(int16_t)new_lo) >> 16);
        wr32(hi_p, (hi_op & 0xFFFF0000u) | (new_hi & 0xFFFF));
    }

    /* The LO16 itself takes the low half of the same address. When no HI16 is
     * pending the stored value is still segment-relative and still needs the
     * base added. */
    const uint32_t full = (uint32_t)(lo_s + (int32_t)delta);
    wr32(lo_p, (lo_op & 0xFFFF0000u) | (full & 0xFFFF));

    q->n = 0;
}

static int apply_section(const image *im,
                         uint32_t sh_off, uint32_t sh_size,
                         const uint32_t *segbase, int nseg,
                         psp_load_info *out) {
    if ((size_t)sh_off + sh_size > im->len) return -1;

    hi_queue q = { {0}, 0, 0 };

    for (uint32_t o = 0; o + 8 <= sh_size; o += 8) {
        const uint32_t r_offset = rd32(im->data + sh_off + o);
        const uint32_t r_info   = rd32(im->data + sh_off + o + 4);

        const uint32_t type = R_TYPE(r_info);
        const uint32_t ofsb = R_OFS_BASE(r_info);
        const uint32_t addb = R_ADDR_BASE(r_info);

        if (ofsb >= (uint32_t)nseg || addb >= (uint32_t)nseg) {
            out->nreloc_skipped++;
            continue;
        }

        uint8_t *p = at(im, ofsb, r_offset);
        if (!p) { out->nreloc_skipped++; continue; }

        const uint32_t delta = segbase[addb];

        switch (type) {
        case R_MIPS_NONE:
            break;

        case R_MIPS_32:
            wr32(p, rd32(p) + delta);
            out->nrelocs++;
            break;

        case R_MIPS_26: {
            /* The 26-bit field is a word address within the same 256 MB region,
             * so it shifts down by two. */
            const uint32_t op  = rd32(p);
            const uint32_t tgt = ((op & 0x03FFFFFFu) << 2) + delta;
            wr32(p, (op & 0xFC000000u) | ((tgt >> 2) & 0x03FFFFFFu));
            out->nrelocs++;
            break;
        }

        case R_MIPS_HI16:
            if (q.n < MAX_PENDING_HI) q.p[q.n++] = p;
            else                      q.overflow = 1;
            out->nrelocs++;
            break;

        case R_MIPS_LO16:
            apply_lo16(&q, p, delta);
            out->nrelocs++;
            break;

        case R_MIPS_GPREL16:
            /* Nothing to patch. The field holds `target - gp`, and a module is
             * relocated as a unit, so both move by the same delta and the
             * difference is already right. What it does need is for $gp to
             * actually be loaded -- see module_gp() below.
             *
             * Named rather than left to the default arm: counting these as
             * "skipped" says the loader mis-loaded something when it did the
             * correct thing, and that reading cost real time. */
            out->nrelocs++;
            break;

        default:
            /* R_MIPS_16 and REL32 do not appear in PSP modules in practice.
             * Counted rather than ignored: a module that needs one would
             * otherwise be quietly mis-loaded. */
            out->nreloc_skipped++;
            break;
        }
    }

    if (q.overflow) return -2;
    return 0;
}

int psp_relocate_image(uint8_t *data, size_t len, const elf_info *e,
                       psp_load_info *out) {
    memset(out, 0, sizeof *out);
    if (e->nsegments <= 0) return -1;

    uint32_t lo = UINT32_MAX, hi = 0;
    uint32_t segbase[8];
    for (int i = 0; i < e->nsegments && i < 8; i++) {
        /* Each segment sits where it was linked, so a relocation against
         * segment 0 adds zero and every code address stays put. Only pointers
         * into the data segment move, which is the whole point. */
        segbase[i] = e->seg[i].addr;
        if (e->seg[i].addr < lo) lo = e->seg[i].addr;
        if (e->seg[i].addr + e->seg[i].memsz > hi) hi = e->seg[i].addr + e->seg[i].memsz;
    }
    if (lo > hi) return -1;

    out->lo = lo;
    out->hi = hi;
    out->nsegments = e->nsegments;

    if (!e->shoff || !e->shnum) return 0;

    const image im = { data, len, e->seg, e->nsegments };
    for (uint32_t i = 0; i < e->shnum; i++) {
        const size_t so = (size_t)e->shoff + (size_t)i * e->shentsize;
        if (so + 24 > len) break;
        if (rd32(data + so + 4) != SHT_PRXRELOC) continue;
        const uint32_t sh_off  = rd32(data + so + 16);
        const uint32_t sh_size = rd32(data + so + 20);
        if (apply_section(&im, sh_off, sh_size, segbase, e->nsegments, out) != 0)
            return -2;
    }
    return 0;
}

uint32_t psp_rebase_image(elf_info *e, uint32_t base, int *err) {
    *err = 0;
    if (e->type != ET_PSP_PRX || e->nsegments <= 0) { *err = -1; return 0; }
    if (base & 0xFFu) { *err = -2; return 0; }
    uint32_t lo = UINT32_MAX;
    for (int i = 0; i < e->nsegments && i < 8; i++)
        if (e->seg[i].addr < lo) lo = e->seg[i].addr;
    /* Wraps when base < lo, which shifts down; the sums wrap back. */
    const uint32_t shift = base - lo;
    for (int i = 0; i < e->nsegments && i < 8; i++) e->seg[i].addr += shift;
    e->entry += shift;
    e->text_addr += shift;
    if (e->stub_addr) e->stub_addr += shift;
    if (e->modinfo_addr) e->modinfo_addr += shift;
    return shift;
}

int psp_find_section(const psp_blob *b, const elf_info *e, const char *name,
                     psp_section *out) {
    if (!e->shoff || !e->shnum || b->size < 52) return -1;

    /* e_shstrndx lives at offset 50 of the ELF header and is not carried in
     * elf_info; reading it here beats widening that struct for one caller. */
    const uint32_t shstrndx = (uint32_t)b->data[50] | ((uint32_t)b->data[51] << 8);
    if (shstrndx >= e->shnum) return -1;

    const size_t strtab_hdr = (size_t)e->shoff + (size_t)shstrndx * e->shentsize;
    if (strtab_hdr + 24 > b->size) return -1;
    const uint32_t names_off = rd32(b->data + strtab_hdr + 16);

    for (uint32_t i = 0; i < e->shnum; i++) {
        const size_t so = (size_t)e->shoff + (size_t)i * e->shentsize;
        if (so + 24 > b->size) break;
        const uint32_t name_idx = rd32(b->data + so);
        const size_t   np = (size_t)names_off + name_idx;
        if (np >= b->size) continue;
        if (strcmp((const char *)b->data + np, name) != 0) continue;
        out->addr   = rd32(b->data + so + 12);
        out->offset = rd32(b->data + so + 16);
        out->size   = rd32(b->data + so + 20);
        return 0;
    }
    return -1;
}

/* $gp, from the module info the PRX carries in `.rodata.sceModuleInfo`.
 *
 * A module built with a small-data area addresses it as an offset from $gp,
 * and nothing in the instruction stream says what $gp should be -- the value
 * lives only in this header. Leave it zero and every such access reads from
 * around address 0 instead: an `lw -0x7FF4($gp)` lands at 0xFFFF800C, which
 * looks like a wild pointer and is really a register nobody set.
 *
 * Armored Core is built -G0 and never names $gp, so this stayed invisible; the
 * first module that used it was a test binary, and the oracle could not have
 * caught it either, since it excludes $gp from comparison.
 *
 * Read after relocation, not before: gp_value is one of the five relocated
 * words in this header, so the image already holds the final address. */
static uint32_t module_gp(const psp_blob *b, const elf_info *e) {
    /* flags(4) + name(28) puts gp_value at 0x20. */
    if (e->modinfo_size < 0x24 || (size_t)e->modinfo_offset + 0x24 > b->size) return 0;
    return rd32(b->data + e->modinfo_offset + 0x20);
}

int psp_load_module(psp_blob *b, const elf_info *e, psp_load_info *out) {
    if (psp_relocate_image(b->data, b->size, e, out) != 0) return -1;
    out->gp = module_gp(b, e);

    if (psp_mem_map_module(out->lo, out->hi - out->lo) != 0) return -1;
    for (int i = 0; i < e->nsegments; i++) {
        const elf_segment *s = &e->seg[i];
        if (!s->filesz) continue;
        if ((size_t)s->offset + s->filesz > b->size) return -1;
        psp_mem_write_block(s->addr, b->data + s->offset, s->filesz);
    }
    return 0;
}
