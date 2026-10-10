/* allegrexrecomp — a PRX's relocations. See reloc.h. */

#include "reloc.h"

#define SHT_REL       9u
#define PRXRELOC      0x700000A0u   /* section type and segment type alike */
#define PRXRELOC2     0x700000A1u

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | (rd16(p + 2) << 16); }

static void put(psp_reloc *out, int max, int *n, psp_reloc r) {
    if (*n < max) out[*n] = r;
    (*n)++;
}

/* An Elf32_Rel's r_info: the MIPS type, and two PT_LOAD indices in place of a
 * symbol. */
static void put_rel(psp_reloc *out, int max, int *n, uint32_t r_offset, uint32_t r_info) {
    psp_reloc r = { r_offset, PSP_RELOC_OTHER, (uint8_t)(r_info >> 8), (uint8_t)(r_info >> 16), 0 };
    switch (r_info & 0xFF) {
    case 0: r.kind = PSP_RELOC_NONE;    break;
    case 2: r.kind = PSP_RELOC_32;      break;
    case 4: r.kind = PSP_RELOC_26;      break;
    case 5: r.kind = PSP_RELOC_HI16;    break;
    case 6: r.kind = PSP_RELOC_LO16;    break;
    case 7: r.kind = PSP_RELOC_GPREL16; break;
    }
    put(out, max, n, r);
}

/* The packed form. A two-byte zero, then the field widths, then two tables,
 * then a stream of 16-bit commands:
 *
 *   bytes 2, 3   part1s, part2s: how many low bits of a command index the
 *                first table and, above the segment bits, the second
 *   byte 4..     the first table, its own size first; then the second table,
 *                likewise
 *
 * A command's low part1s bits pick a first-table entry whose bit 0 says what
 * the command is. Clear, it names the segment the following offsets are in,
 * with the offset either above the segment bits or in the 32 bits after.
 * Set, it is a relocation: the next nbits are the segment its value is
 * relative to, the next part2s pick its kind from the second table, and the
 * offset advances by the command's signed high bits, or by those and 16 more,
 * or is replaced by the 32 bits after. Bits 3..5 of the entry say whether a
 * 16-bit addend follows, or the previous one stands. nbits is just wide
 * enough to number the PT_LOADs. */
static int read_packed(const uint8_t *p, uint32_t size, const elf_info *e,
                       psp_reloc *out, int max, int *n) {
    if (size < 6 || p[0] || p[1]) return -1;
    const uint32_t part1s = p[2], part2s = p[3];
    const uint8_t *block1 = p + 4;
    const uint32_t block1s = block1[0];
    if (4 + block1s >= size) return -1;
    const uint8_t *block2 = block1 + block1s;
    const uint32_t block2s = block2[0];
    if (4 + block1s + block2s > size || !block1s || !block2s) return -1;
    const uint8_t *pos = block2 + block2s, *end = p + size;

    uint32_t nbits = 1;
    while ((1 << nbits) < e->nsegments) nbits++;
    if (part1s + nbits + part2s > 16) return -1;

    uint32_t ofs_seg = (uint32_t)e->nsegments, offset = 0, last_part2 = block2s;
    int16_t addend = 0;
    while (pos + 2 <= end) {
        const uint32_t cmd = rd16(pos);
        pos += 2;
        const uint32_t i1 = cmd & ((1u << part1s) - 1);
        if (i1 >= block1s) return -1;
        const uint32_t part1 = block1[i1];
        const uint32_t seg = (cmd >> part1s) & ((1u << nbits) - 1);

        if (!(part1 & 1)) {
            if (seg >= (uint32_t)e->nsegments) return -1;
            ofs_seg = seg;
            if ((part1 & 6) == 0) {
                offset = cmd >> (part1s + nbits);
            } else if ((part1 & 6) == 4) {
                if (pos + 4 > end) return -1;
                offset = rd32(pos);
                pos += 4;
            } else {
                return -1;
            }
            continue;
        }

        const uint32_t i2 = (cmd >> (part1s + nbits)) & ((1u << part2s) - 1);
        if (i2 >= block2s || seg >= (uint32_t)e->nsegments || ofs_seg >= (uint32_t)e->nsegments)
            return -1;
        const uint32_t part2 = block2[i2];
        const int32_t high = (int32_t)(int16_t)cmd >> (part1s + part2s + nbits);
        switch (part1 & 6) {
        case 0:
            offset += (uint32_t)high;
            break;
        case 2:
            if (pos + 2 > end) return -1;
            offset += ((uint32_t)high << 16) | rd16(pos);
            pos += 2;
            break;
        case 4:
            if (pos + 4 > end) return -1;
            offset = rd32(pos);
            pos += 4;
            break;
        default:
            return -1;
        }
        if (offset >= e->seg[ofs_seg].filesz) return -1;

        switch (part1 & 0x38) {
        case 0x00: addend = 0; break;
        case 0x08: if (last_part2 != 4) addend = 0; break;
        case 0x10:
            if (pos + 2 > end) return -1;
            addend = (int16_t)rd16(pos);
            pos += 2;
            break;
        default: return -1;
        }
        last_part2 = part2;

        psp_reloc r = { offset, PSP_RELOC_NONE, (uint8_t)ofs_seg, (uint8_t)seg, 0 };
        switch (part2) {
        case 0: continue;
        case 1: case 5: r.kind = PSP_RELOC_LO16; break;
        case 2: r.kind = PSP_RELOC_32; break;
        case 3: case 6: case 7: r.kind = PSP_RELOC_26; break;
        case 4: r.kind = PSP_RELOC_HI16_ADDEND; r.addend = addend; break;
        default: return -1;
        }
        put(out, max, n, r);
    }
    return 0;   /* a byte short of a command is padding */
}

int psp_relocs_read(const uint8_t *d, size_t len, const elf_info *e, int flags,
                    psp_reloc *out, int max) {
    int n = 0;

    for (uint32_t i = 0; e->shoff && i < e->shnum; i++) {
        const size_t sh = (size_t)e->shoff + (size_t)i * e->shentsize;
        if (sh + 24 > len) break;
        const uint32_t type = rd32(d + sh + 4);
        if (type != PRXRELOC && !(type == SHT_REL && (flags & PSP_RELOCS_SHT_REL))) continue;
        const uint32_t off = rd32(d + sh + 16), size = rd32(d + sh + 20);
        if ((size_t)off + size > len) continue;
        for (uint32_t r = 0; r + 8 <= size; r += 8)
            put_rel(out, max, &n, rd32(d + off + r), rd32(d + off + r + 4));
    }
    if (n || len < 52) return n;

    /* No relocation sections: the module's relocation segment, if it has one. */
    const uint32_t phoff = rd32(d + 28), phentsize = rd16(d + 42), phnum = rd16(d + 44);
    for (uint32_t i = 0; i < phnum; i++) {
        const size_t ph = (size_t)phoff + (size_t)i * phentsize;
        if (ph + 20 > len) break;
        const uint32_t type = rd32(d + ph), off = rd32(d + ph + 4), size = rd32(d + ph + 16);
        if (type != PRXRELOC && type != PRXRELOC2) continue;
        if ((size_t)off + size > len) return -1;
        if (type == PRXRELOC) {
            for (uint32_t r = 0; r + 8 <= size; r += 8)
                put_rel(out, max, &n, rd32(d + off + r), rd32(d + off + r + 4));
        } else if (read_packed(d + off, size, e, out, max, &n) != 0) {
            return -1;
        }
    }
    return n;
}
