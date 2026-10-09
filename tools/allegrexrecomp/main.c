/* allegrexrecomp — command-line front end.
 *
 *   info    <file>                    identify and describe any layer of the stack
 *   ls      <file.iso>                list a disc image
 *   extract <file.iso> <match> <dir>  pull matching files out of a disc image
 *   dis     <file> [addr] [count]     disassemble Allegrex code
 *   cover   <file>                    decode-coverage report over an image
 *
 * Phase 1 stops at `dis` and `cover`; `emit` (the C generator) is phase 3.
 * See ROADMAP.md.
 */

#include "container.h"
#include "decode.h"
#include "analyze.h"
#include "emit.h"
#include "interp.h"
#include "loader.h"
#include "keys.h"
#include "crypto/kirk.h"

/* The `interp` verb runs guest code, so the CLI reaches into the runtime the
 * recompiled game also links against — same CPU state, same memory model. */
#include "psprecomp/cpu.h"
#include "psprecomp/dispatch.h"
#include "psprecomp/hle.h"
#include "psprecomp/mem.h"
#include "psprecomp/sched.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_ISO_ENTRIES 4096

static int usage(void) {
    fprintf(stderr,
        "allegrexrecomp — Allegrex static-recompilation toolkit\n"
        "\n"
        "  allegrexrecomp info    <file>\n"
        "  allegrexrecomp ls      <file.iso>\n"
        "  allegrexrecomp extract <file.iso> <substring> <outdir>\n"
        "  allegrexrecomp dis     <file> [start-addr] [count]\n"
        "  allegrexrecomp cover   <file>\n"
        "  allegrexrecomp funcs   <file> [--list]\n"
        "  allegrexrecomp emit    <file> <outdir> [prefix] [--replace <addrs>|@<file>] [--no-resume]\n"
        "  allegrexrecomp interp  <file> [--from <addr>] [--budget <n>] [--trace] [--regs] [--dispatch] [--drain <s>]\n"
        "                         [--argv0 <guest path>] [--base <addr>]\n"
        "  allegrexrecomp decrypt <file> [--keys <path>]\n"
        "  allegrexrecomp kirk1   <file> [out] [--keys <path>]\n"
        "\n"
        "Accepts .iso disc images, PBP containers, ~PSP / ~SCE wrappers,\n"
        "ELF/PRX modules, and raw binaries.\n"
        "\n"
        "--base loads a relocatable PRX at <addr> (as the PSP's loader would,\n"
        "0x08804000) instead of where it was linked. For probe runs: the emitted\n"
        "C uses the linked addresses, so a moved module is not comparable with it.\n"
        "\n"
        "--replace names functions (hex addresses, comma-separated, or @file with\n"
        "one per line and # comments) that the host will implement itself. Their\n"
        "translated bodies are emitted as psp_func_<addr>__orig and the public\n"
        "psp_func_<addr> is left for the host to define, so a native version wins\n"
        "at link time and can still call the original.\n"
        "\n"
        "Every call's return site is an entry of its function, with a table\n"
        "psp_resume_chain uses to continue a restored thread natively: what a\n"
        "save state needs (docs/PLAYER-LAYER.md, section 5). --no-resume leaves\n"
        "them out, which saves about 6%% of compile time and 7%% of code, and\n"
        "every save is then refused.\n"
        "\n"
        "Key material is never bundled. Supply it via --keys, $PSPRECOMP_KEYS,\n"
        "or ./keys/psp_keys.txt — see docs/DECRYPT.md.\n");
    return 2;
}

/* ---- info ---------------------------------------------------------------- */

static void print_iso_info(const char *path, FILE *f) {
    printf("format:   ISO9660 disc image\n");

    iso_entry *entries = (iso_entry *)calloc(MAX_ISO_ENTRIES, sizeof *entries);
    if (!entries) return;

    int n = iso_list(f, entries, MAX_ISO_ENTRIES);
    if (n < 0) { printf("  (could not read the volume descriptor)\n"); free(entries); return; }
    printf("entries:  %d\n", n);
    if (n > MAX_ISO_ENTRIES) {
        printf("  (listing truncated to %d)\n", MAX_ISO_ENTRIES);
        n = MAX_ISO_ENTRIES;
    }

    /* PARAM.SFO is the disc's identity: DISC_ID, TITLE, firmware requirement. */
    for (int i = 0; i < n; i++) {
        if (entries[i].is_dir) continue;
        if (!strstr(entries[i].path, "PSP_GAME/PARAM.SFO")) continue;

        uint8_t *buf = (uint8_t *)malloc(entries[i].size);
        if (!buf) break;
        if (fseek(f, (long)entries[i].lba * ISO_SECTOR, SEEK_SET) == 0 &&
            fread(buf, 1, entries[i].size, f) == entries[i].size) {
            printf("\nPARAM.SFO:\n");
            sfo_dump(buf, entries[i].size, stdout);
        }
        free(buf);
        break;
    }

    /* The boot chain is what a recomp run actually cares about. */
    printf("\nboot chain:\n");
    for (int i = 0; i < n; i++) {
        const char *p = entries[i].path;
        if (entries[i].is_dir) continue;
        if (strstr(p, "EBOOT.BIN") || strstr(p, "BOOT.BIN") ||
            strstr(p, "bootbin")   || strstr(p, ".prx")) {
            printf("  %10u  %s\n", entries[i].size, p);
        }
    }
    free(entries);
}

static void print_psp_info(const uint8_t *d, size_t len);
static void print_elf_info(const uint8_t *d, size_t len);

static void print_pbp_info(const uint8_t *d, size_t len) {
    pbp_info pbp;
    if (pbp_parse(d, len, &pbp) != 0) { printf("  (malformed PBP)\n"); return; }

    printf("format:   PBP container (version 0x%X)\n", pbp.version);
    for (int i = 0; i < PBP_NUM_SEGMENTS; i++) {
        if (!pbp.size[i]) continue;
        printf("  %-10s  offset 0x%08X  size %10u",
               PBP_SEGMENT_NAMES[i], pbp.offset[i], pbp.size[i]);
        /* Name the inner format so you can see at a glance whether the
         * executable payload is encrypted before you try to decode it. */
        if (pbp.offset[i] + 16 <= len) {
            psp_format inner = psp_sniff(d + pbp.offset[i], 16);
            if (inner != FMT_UNKNOWN) printf("  [%s]", psp_format_name(inner));
        }
        printf("\n");
    }
    if (pbp.size[0] && pbp.offset[0] + pbp.size[0] <= len) {
        printf("\nPARAM.SFO:\n");
        sfo_dump(d + pbp.offset[0], pbp.size[0], stdout);
    }

    /* Recurse one level into the executable payload. DATA.PSP is where the
     * actual module lives, and its header names the module and the key tag —
     * the two things you need before you can decrypt or decode it. */
    if (pbp.size[6] && pbp.offset[6] + pbp.size[6] <= len) {
        const uint8_t *inner = d + pbp.offset[6];
        printf("\nDATA.PSP:\n");
        switch (psp_sniff(inner, 16)) {
        case FMT_PSP: print_psp_info(inner, pbp.size[6]); break;
        case FMT_ELF: print_elf_info(inner, pbp.size[6]); break;
        default:      printf("  (unrecognised payload)\n"); break;
        }
    }
}

static void print_psp_info(const uint8_t *d, size_t len) {
    psp_header h;
    if (psp_header_parse(d, len, &h) != 0) { printf("  (malformed ~PSP)\n"); return; }

    printf("format:   ~PSP encrypted PRX\n");
    printf("  module        %s\n", h.modname);
    printf("  version       %u.%u\n", h.mod_ver_hi, h.mod_ver_lo);
    printf("  attributes    mod=0x%04X comp=0x%04X\n", h.mod_attr, h.comp_attr);
    printf("  segments      %u\n", h.nsegments);
    printf("  elf size      %u bytes (decrypted)\n", h.elf_size);
    printf("  psp size      %u bytes (this file)\n", h.psp_size);
    printf("  entry         0x%08X\n", h.boot_entry);
    printf("  modinfo       0x%08X\n", h.modinfo_offset);
    printf("  bss           %u bytes\n", h.bss_size);
    printf("  devkit        0x%08X\n", h.devkit_version);
    printf("  decrypt mode  %u\n", h.decrypt_mode);
    printf("  tag           0x%08X\n", h.tag);
    for (int i = 0; i < h.nsegments && i < 4; i++)
        printf("  segment %d     addr 0x%08X size %u\n", i, h.seg_address[i], h.seg_size[i]);
    printf("\n  This module is encrypted. Decryption is phase 2 — see docs/DECRYPT.md.\n");
}

static void print_elf_info(const uint8_t *d, size_t len) {
    elf_info e;
    if (elf_parse(d, len, &e) != 0) { printf("  (malformed ELF)\n"); return; }

    printf("format:   ELF32 MIPS LE (%s)\n",
           e.type == ET_PSP_PRX ? "PRX, relocatable" : "executable");
    printf("  entry         0x%08X\n", e.entry);
    printf("  PT_LOAD       %d\n", e.nsegments);
    for (int i = 0; i < e.nsegments; i++) {
        const elf_segment *s = &e.seg[i];
        printf("  segment %d     addr 0x%08X  file %8u  mem %8u  flags %c%c%c\n",
               i, s->addr, s->filesz, s->memsz,
               (s->flags & 4) ? 'r' : '-',
               (s->flags & 2) ? 'w' : '-',
               (s->flags & 1) ? 'x' : '-');
    }
    if (e.nsections) printf("  sections      %d\n", e.nsections);
    if (e.text_size)
        printf("  code          0x%08X + %u bytes  (from %s)\n",
               e.text_addr, e.text_size,
               e.text_from_section ? ".text section" : "PT_LOAD segment");
    if (e.stub_size)
        printf("  import stubs  0x%08X + %u bytes\n", e.stub_addr, e.stub_size);

    if (e.modinfo_size) {
        psp_module_info mi;
        if (psp_modinfo_parse(d, len, e.modinfo_offset, &mi) == 0) {
            printf("  module info   0x%08X\n", e.modinfo_addr);
            printf("    name        %s\n", mi.name);
            printf("    attribute   0x%08X\n", mi.attribute);
            printf("    gp          0x%08X\n", mi.gp_value);
            printf("    exports     0x%08X..0x%08X\n", mi.ent_top, mi.ent_end);
            printf("    imports     0x%08X..0x%08X\n", mi.stub_top, mi.stub_end);

            /* load_bias converts a module virtual address to a file offset.
             * A PRX links at 0, so the bias is just the segment's file
             * offset. */
            uint32_t bias = e.nsegments ? e.seg[0].offset - e.seg[0].addr : 0;
            uint32_t exp[512];
            int n = psp_collect_exports(d, len, &mi, bias, exp,
                                        (int)(sizeof exp / sizeof exp[0]));
            printf("    exported fn %d\n", n);
        }
    }
}

static int cmd_info(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 1; }

    uint8_t head[16];
    size_t got = fread(head, 1, sizeof head, f);

    /* ISO is the only format whose magic is not at offset 0. Probe sector 16
     * directly rather than reading the whole (multi-gigabyte) image. */
    uint8_t pvd[8];
    int is_iso = 0;
    if (fseek(f, 16 * ISO_SECTOR, SEEK_SET) == 0 &&
        fread(pvd, 1, sizeof pvd, f) == sizeof pvd &&
        memcmp(pvd + 1, "CD001", 5) == 0) {
        is_iso = 1;
    }
    rewind(f);

    printf("file:     %s\n", path);
    if (is_iso) {
        print_iso_info(path, f);
        fclose(f);
        return 0;
    }
    fclose(f);

    psp_blob b;
    if (psp_blob_read(path, &b) != 0) { fprintf(stderr, "cannot read %s\n", path); return 1; }
    printf("size:     %zu bytes\n", b.size);

    switch (psp_sniff(b.data, got < 16 ? b.size : 16)) {
    case FMT_PBP: print_pbp_info(b.data, b.size); break;
    case FMT_PSP: print_psp_info(b.data, b.size); break;
    case FMT_ELF: print_elf_info(b.data, b.size); break;
    case FMT_SCE:
        /* ~SCE is a 0x40-byte shim; whatever follows is the real payload. */
        printf("format:   ~SCE wrapper\n");
        if (b.size > 0x40) {
            printf("  inner       %s\n", psp_format_name(psp_sniff(b.data + 0x40, 16)));
            if (psp_sniff(b.data + 0x40, 16) == FMT_PSP)
                print_psp_info(b.data + 0x40, b.size - 0x40);
        }
        break;
    default:
        printf("format:   unrecognised — treating as raw Allegrex code\n");
        break;
    }

    psp_blob_free(&b);
    return 0;
}

/* ---- ls / extract -------------------------------------------------------- */

static int cmd_ls(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 1; }

    iso_entry *entries = (iso_entry *)calloc(MAX_ISO_ENTRIES, sizeof *entries);
    if (!entries) { fclose(f); return 1; }

    int n = iso_list(f, entries, MAX_ISO_ENTRIES);
    if (n < 0) { fprintf(stderr, "%s is not an ISO9660 image\n", path); free(entries); fclose(f); return 1; }
    if (n > MAX_ISO_ENTRIES) n = MAX_ISO_ENTRIES;

    for (int i = 0; i < n; i++)
        printf("%c %12u  lba=%-8u %s\n",
               entries[i].is_dir ? 'd' : ' ',
               entries[i].size, entries[i].lba, entries[i].path);

    free(entries);
    fclose(f);
    return 0;
}

static int cmd_extract(const char *path, const char *match, const char *outdir) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 1; }

    iso_entry *entries = (iso_entry *)calloc(MAX_ISO_ENTRIES, sizeof *entries);
    if (!entries) { fclose(f); return 1; }

    int n = iso_list(f, entries, MAX_ISO_ENTRIES);
    if (n < 0) { fprintf(stderr, "%s is not an ISO9660 image\n", path); free(entries); fclose(f); return 1; }
    if (n > MAX_ISO_ENTRIES) n = MAX_ISO_ENTRIES;

    int found = 0;
    for (int i = 0; i < n; i++) {
        if (entries[i].is_dir || !strstr(entries[i].path, match)) continue;

        /* Flatten the ISO path into a filename so nested entries with the same
         * basename (SYSDIR/EBOOT.BIN vs SYSDIR/UPDATE/EBOOT.BIN) do not collide. */
        char flat[256];
        snprintf(flat, sizeof flat, "%s", entries[i].path + 1);
        for (char *p = flat; *p; p++) if (*p == '/') *p = '_';

        char dst[512];
        snprintf(dst, sizeof dst, "%s/%s", outdir, flat);
        if (iso_extract(f, &entries[i], dst) == 0) {
            printf("extracted %s -> %s (%u bytes)\n", entries[i].path, dst, entries[i].size);
            found++;
        } else {
            fprintf(stderr, "failed to extract %s\n", entries[i].path);
        }
    }
    if (!found) fprintf(stderr, "nothing matched \"%s\"\n", match);

    free(entries);
    fclose(f);
    return found ? 0 : 1;
}

/* ---- dis / cover --------------------------------------------------------- */

/* Locate the code to decode: an ELF's .text, or the whole blob if it is raw. */
static int find_code(const psp_blob *b, const uint8_t **code, uint32_t *size, uint32_t *base) {
    psp_format fmt = psp_sniff(b->data, b->size < 16 ? b->size : 16);

    if (fmt == FMT_ELF) {
        elf_info e;
        if (elf_parse(b->data, b->size, &e) == 0 && e.text_size) {
            *code = b->data + e.text_offset;
            *size = e.text_size;
            *base = e.text_addr;
            return 0;
        }
        return -1;
    }
    if (fmt == FMT_PSP || fmt == FMT_SCE) {
        fprintf(stderr,
            "this module is encrypted (%s) — decryption is phase 2, see docs/DECRYPT.md\n",
            psp_format_name(fmt));
        return -1;
    }
    if (fmt == FMT_PBP) {
        pbp_info p;
        if (pbp_parse(b->data, b->size, &p) == 0 && p.size[6]) {
            /* DATA.PSP is segment 6. Recurse one level by re-sniffing it. */
            const uint8_t *inner = b->data + p.offset[6];
            psp_format ifmt = psp_sniff(inner, 16);
            if (ifmt == FMT_ELF) {
                elf_info e;
                if (elf_parse(inner, p.size[6], &e) == 0 && e.text_size) {
                    *code = inner + e.text_offset;
                    *size = e.text_size;
                    *base = e.text_addr;
                    return 0;
                }
            }
            fprintf(stderr, "PBP DATA.PSP is %s — cannot decode yet\n",
                    psp_format_name(ifmt));
            return -1;
        }
        return -1;
    }

    /* Raw code. Default the load base to the standard user-memory entry. */
    *code = b->data;
    *size = (uint32_t)b->size;
    *base = 0x08804000u;
    return 0;
}

static int cmd_dis(const char *path, uint32_t start, uint32_t count) {
    psp_blob b;
    if (psp_blob_read(path, &b) != 0) { fprintf(stderr, "cannot read %s\n", path); return 1; }

    const uint8_t *code; uint32_t size, base;
    if (find_code(&b, &code, &size, &base) != 0) { psp_blob_free(&b); return 1; }

    uint32_t addr = start ? start : base;
    if (addr < base || addr >= base + size) {
        fprintf(stderr, "0x%08X is outside the code extent 0x%08X..0x%08X\n",
                addr, base, base + size);
        psp_blob_free(&b);
        return 1;
    }

    uint32_t off = addr - base;
    uint32_t emitted = 0;
    char text[128];

    while (off + 4 <= size && (count == 0 || emitted < count)) {
        uint32_t word = (uint32_t)code[off] | ((uint32_t)code[off + 1] << 8) |
                        ((uint32_t)code[off + 2] << 16) | ((uint32_t)code[off + 3] << 24);
        a_insn in;
        a_decode(word, addr, &in);
        a_format(&in, text, sizeof text);
        printf("%08X  %08X  %s\n", addr, word, text);
        off += 4;
        addr += 4;
        emitted++;
    }

    psp_blob_free(&b);
    return 0;
}

/* Decode coverage over a whole image. This is the honest measure of how far
 * the decoder is from handling a real game: every unrecognised word is a
 * concrete gap, and VFPU words are counted separately because they are a known
 * partial area rather than an unknown one. */
static int cmd_cover(const char *path) {
    psp_blob b;
    if (psp_blob_read(path, &b) != 0) { fprintf(stderr, "cannot read %s\n", path); return 1; }

    const uint8_t *code; uint32_t size, base;
    if (find_code(&b, &code, &size, &base) != 0) { psp_blob_free(&b); return 1; }

    uint64_t total = 0, ok = 0, vfpu = 0, bad = 0;
    uint32_t hist[A_OP_COUNT];
    memset(hist, 0, sizeof hist);

    for (uint32_t off = 0; off + 4 <= size; off += 4) {
        uint32_t word = (uint32_t)code[off] | ((uint32_t)code[off + 1] << 8) |
                        ((uint32_t)code[off + 2] << 16) | ((uint32_t)code[off + 3] << 24);
        a_insn in;
        a_decode(word, base + off, &in);
        total++;
        hist[in.op]++;
        if (in.op == A_INVALID)           bad++;
        else if (in.op == A_VFPU_UNKNOWN) vfpu++;
        else                              ok++;
    }

    printf("image:    %s\n", path);
    printf("extent:   0x%08X + %u bytes (%llu words)\n",
           base, size, (unsigned long long)total);
    if (!total) { psp_blob_free(&b); return 0; }

    printf("decoded:  %llu (%.2f%%)\n", (unsigned long long)ok, 100.0 * (double)ok / (double)total);
    printf("vfpu:     %llu (%.2f%%)  — recognised as VFPU, not yet named\n",
           (unsigned long long)vfpu, 100.0 * (double)vfpu / (double)total);
    printf("unknown:  %llu (%.2f%%)  — decoder gaps, or data misread as code\n",
           (unsigned long long)bad, 100.0 * (double)bad / (double)total);

    /* Top opcodes: a sanity check that we are looking at code at all. Real
     * MIPS text is dominated by lw/sw/addiu/nop; a flat histogram means the
     * extent is data. */
    printf("\ntop opcodes:\n");
    for (int rank = 0; rank < 12; rank++) {
        int best = -1;
        uint32_t bestn = 0;
        for (int i = 1; i < A_OP_COUNT; i++)
            if (hist[i] > bestn) { bestn = hist[i]; best = i; }
        if (best < 0) break;
        printf("  %-10s %8u  (%.2f%%)\n", a_mnemonic((a_op)best), bestn,
               100.0 * (double)bestn / (double)total);
        hist[best] = 0;
    }

    psp_blob_free(&b);
    return 0;
}

/* ---- function discovery -------------------------------------------------- */

/* Load a decrypted module and run discovery over it. On success the caller
 * owns both `b` (psp_blob_free) and `an` (a_analysis_free). Shared by `funcs`
 * and `emit`, which differ only in what they do with the result. */
static int load_and_discover(const char *path, psp_blob *b, elf_info *e,
                             a_analysis *an, int *out_nseeds, int *out_nexports,
                             int *out_nptr, int *out_scanned) {
    if (psp_blob_read(path, b) != 0) { fprintf(stderr, "cannot read %s\n", path); return -1; }

    if (psp_sniff(b->data, b->size < 16 ? b->size : 16) != FMT_ELF ||
        elf_parse(b->data, b->size, e) != 0 || !e->text_size) {
        fprintf(stderr, "%s is not a decrypted ELF/PRX with code in it\n", path);
        fprintf(stderr, "(encrypted modules must be decrypted first — see docs/DECRYPT.md)\n");
        psp_blob_free(b);
        return -1;
    }

    /* Relocate before anything reads the code.
     *
     * A PRX stores cross-segment addresses relative to the segment they point
     * into, so an unrelocated pointer into .data reads as a small number --
     * usually zero. Code that builds one with a lui/addiu pair then computes
     * address 0 and walks into .text, and a table scan started there never
     * finds its terminator.
     *
     * It has to happen here rather than at load time. The interpreter fetches
     * instructions from memory, so patching memory would fix it; the emitter
     * bakes address literals in when it reads the file, so patching memory
     * would not. Relocating the image up front is what keeps the two agreeing.
     *
     * Both segments stay at their linked addresses, so no code address moves. */
    {
        psp_load_info li;
        if (psp_relocate_image(b->data, b->size, e, &li) != 0) {
            fprintf(stderr, "cannot apply relocations to %s\n", path);
            psp_blob_free(b);
            return -1;
        }
        if (li.nrelocs)
            fprintf(stderr, "relocs:     %d applied%s\n", li.nrelocs,
                    li.nreloc_skipped ? " (some skipped -- see loader.c)" : "");
    }

    memset(an, 0, sizeof *an);
    /* Walk the whole loaded image, not just .text: a module can place
     * executable code outside it (C++ static initialisers, notably), and
     * restricting the extent to .text makes that code invisible. The linear
     * jal harvest stays confined to .text via scan_base/scan_size. */
    if (e->nsegments) {
        an->code = b->data + e->seg[0].offset;
        an->base = e->seg[0].addr;
        an->size = e->seg[0].filesz;
    } else {
        an->code = b->data + e->text_offset;
        an->base = e->text_addr;
        an->size = e->text_size;
    }
    an->scan_base = e->text_addr;
    an->scan_size = e->text_size;
    an->stub_addr = e->stub_addr;
    an->stub_size = e->stub_size;
    an->scan_calls = 1;
    /* The whole loaded segment, so jump tables in .rodata/.data can be read. */
    if (e->nsegments) {
        an->image      = b->data + e->seg[0].offset;
        an->image_base = e->seg[0].addr;
        an->image_size = e->seg[0].filesz;
    }

    /* Seeds come from three places, in increasing order of how much they find:
     *
     *   1. the module entry point
     *   2. the export table — a library entry nothing internal calls is
     *      unreachable from the entry point alone
     *   3. R_MIPS_32 relocations pointing into .text — the stored function
     *      pointers behind thread entries, callbacks and vtables, which no
     *      control-flow scan can see
     *
     * (`jal` targets are harvested separately, inside discovery itself.) */
    const uint32_t bias = e->nsegments ? e->seg[0].offset - e->seg[0].addr : 0;

    /* Size the buffer from an actual count rather than a guessed maximum. */
    int nptr_avail = psp_collect_pointer_seeds(b->data, b->size, e, bias, NULL, 0);
    int cap = 1 + 512 + nptr_avail;
    uint32_t *seeds = (uint32_t *)malloc((size_t)cap * sizeof *seeds);
    if (!seeds) { psp_blob_free(b); return -1; }

    int nseeds = 0;
    seeds[nseeds++] = e->entry;

    psp_module_info mi;
    int nexports = 0;
    if (e->modinfo_size && psp_modinfo_parse(b->data, b->size, e->modinfo_offset, &mi) == 0) {
        nexports = psp_collect_exports(b->data, b->size, &mi, bias, seeds + nseeds, 512);
        if (nexports > 512) nexports = 512;
        nseeds += nexports;
    }

    int nptr = psp_collect_pointer_seeds(b->data, b->size, e, bias,
                                         seeds + nseeds, cap - nseeds);
    if (nptr > cap - nseeds) nptr = cap - nseeds;
    nseeds += nptr;

    /* A statically linked module (ET_EXEC) has empty relocation sections — its
     * addresses are absolute, so there is nothing for the loader to patch and
     * nothing to enumerate. Fall back to recognising pointers by shape, which
     * is a heuristic rather than a fact and is only used when enumeration
     * genuinely had nothing to offer. */
    if (nptr == 0 && e->nsegments) {
        const elf_segment *s = &e->seg[0];
        uint32_t data_off = e->text_offset + e->text_size;
        uint32_t seg_end  = s->offset + s->filesz;

        if (data_off < seg_end && (size_t)seg_end <= b->size) {
            uint32_t region_len = seg_end - data_off;
            int avail = a_scan_data_pointers(an, b->data + data_off, region_len, NULL, 0);

            uint32_t *bigger = (uint32_t *)realloc(seeds, (size_t)(nseeds + avail) * sizeof *seeds);
            if (bigger) {
                seeds = bigger;
                nptr = a_scan_data_pointers(an, b->data + data_off, region_len,
                                            seeds + nseeds, avail);
                if (nptr > avail) nptr = avail;
                nseeds += nptr;
                if (out_scanned) *out_scanned = 1;
            }
        }
    }

    /* The entry point and the exports are hard; everything appended after them
     * is pointer-derived and therefore a guess. See A_ENTRY_SOFT in analyze.h. */
    int rc = a_discover(an, seeds, nseeds, 1 + nexports);
    free(seeds);
    if (rc != 0) {
        fprintf(stderr, "discovery failed (out of memory)\n");
        psp_blob_free(b);
        return -1;
    }

    if (out_nseeds)   *out_nseeds = nseeds;
    if (out_nexports) *out_nexports = nexports;
    if (out_nptr)     *out_nptr = nptr;
    (void)out_scanned;
    return 0;
}

static int cmd_funcs(const char *path, int list) {
    psp_blob b;
    elf_info e;
    a_analysis an;
    int nseeds = 0, nexports = 0, nptr = 0, scanned = 0;

    if (load_and_discover(path, &b, &e, &an, &nseeds, &nexports, &nptr, &scanned) != 0) return 1;

    printf("module:     %s\n", path);
    printf("code:       0x%08X + %u bytes  (%u instructions)\n",
           an.base, an.size, an.size / 4);
    printf("seeds:      %d (entry 0x%08X + %d exports + %d %s)\n",
           nseeds, e.entry, nexports, nptr,
           scanned ? "scanned data pointers (heuristic)" : "relocation pointers");
    printf("\n");
    printf("functions:  %d  (%d merged away)\n", an.nfuncs, an.nmerged);
    /* Shared blocks the merge refused because the target was already an entry.
     * The soft share is the interesting number: those are loops cut in half on
     * the evidence of a data word that happened to decode. */
    printf("suppressed: %d shared blocks left unmerged, %d of them on a soft seed\n",
           an.nsuppressed, an.nsuppressed_soft);
    /* `an.size` is the whole loaded image -- code plus .data plus .bss -- so
     * dividing by it and calling the result "of .text" understates coverage by
     * roughly six times. This module reported 14.18% while actually covering
     * 92.2% of its executable range, which reads as a glaring hole in
     * discovery and is not one. Report the executable extent when it is known,
     * and say plainly which denominator was used. */
    if (an.text_size)
        printf("reached:    %u bytes (%.1f%% of .text, %u bytes)\n",
               an.bytes_reached, 100.0 * an.bytes_reached / an.text_size,
               an.text_size);
    else
        printf("reached:    %u bytes (%.2f%% of the loaded image; .text extent unknown)\n",
               an.bytes_reached, an.size ? 100.0 * an.bytes_reached / an.size : 0.0);
    printf("imports:    %d distinct firmware calls\n", an.nimports);

    /* Which firmware libraries this module actually needs. This is the HLE
     * work list: everything here has to exist before the game runs, and
     * nothing outside it does. */
    if (e.modinfo_size) {
        psp_module_info mi2;
        if (psp_modinfo_parse(b.data, b.size, e.modinfo_offset, &mi2) == 0) {
            uint32_t bias = e.nsegments ? e.seg[0].offset - e.seg[0].addr : 0;
            int nimp = psp_collect_imports(b.data, b.size, &mi2, bias, NULL, 0);
            psp_import_entry *imp = (psp_import_entry *)malloc((size_t)(nimp ? nimp : 1) * sizeof *imp);
            if (imp) {
                nimp = psp_collect_imports(b.data, b.size, &mi2, bias, imp, nimp);

                /* Count functions per library, preserving first-seen order. */
                char libs[64][32];
                int counts[64], nlibs = 0;
                for (int i = 0; i < nimp; i++) {
                    int k = 0;
                    for (; k < nlibs; k++) if (!strcmp(libs[k], imp[i].lib)) break;
                    if (k == nlibs && nlibs < 64) {
                        snprintf(libs[nlibs], sizeof libs[0], "%s", imp[i].lib);
                        counts[nlibs] = 0;
                        nlibs++;
                    }
                    if (k < 64) counts[k]++;
                }
                printf("\nfirmware libraries needed (%d functions across %d libraries):\n",
                       nimp, nlibs);
                for (int k = 0; k < nlibs; k++)
                    printf("  %-24s %d\n", libs[k], counts[k]);

                if (list) {
                    printf("\nimports by NID:\n");
                    for (int k = 0; k < nlibs; k++)
                        for (int i = 0; i < nimp; i++)
                            if (!strcmp(imp[i].lib, libs[k]))
                                printf("  %-24s 0x%08X  thunk 0x%08X\n",
                                       imp[i].lib, imp[i].nid, imp[i].addr);
                }
                free(imp);
            }
        }
    }

    printf("indirect:   %d computed-jump sites; %d tables resolved -> %d targets\n",
           an.nindirects, an.ntables, an.ntable_targets);

    /* The honest VFPU cost for this title: measured over discovered code, not
     * over a segment that is mostly data. */
    printf("\nover discovered code:\n");
    printf("  instructions %llu\n", (unsigned long long)an.insns);
    printf("  vfpu         %llu (%.2f%%)\n", (unsigned long long)an.vfpu,
           an.insns ? 100.0 * (double)an.vfpu / (double)an.insns : 0.0);
    printf("  invalid      %llu (%.2f%%)\n", (unsigned long long)an.invalid,
           an.insns ? 100.0 * (double)an.invalid / (double)an.insns : 0.0);

    int with_vfpu = 0, no_return = 0, with_indirect = 0, scattered = 0;
    uint32_t biggest = 0, biggest_addr = 0;
    for (int i = 0; i < an.nfuncs; i++) {
        const a_func *f = &an.funcs[i];
        if (f->has_vfpu) with_vfpu++;
        if (!f->has_return) no_return++;
        if (f->has_indirect) with_indirect++;

        /* A function's real size is the code in it. Its *extent* is the span
         * from entry to the furthest address reached, which is larger when
         * blocks are not contiguous. When extent runs far ahead of size the
         * boundary is probably wrong, so those are counted rather than
         * averaged away. */
        uint32_t code = f->insns * 4;
        uint32_t extent = f->end - f->start;
        if (extent > code * 2 + 64) scattered++;
        if (code > biggest) { biggest = code; biggest_addr = f->addr; }
    }
    printf("\nfunction shape:\n");
    printf("  touch vfpu       %d (%.1f%%)\n", with_vfpu,
           an.nfuncs ? 100.0 * with_vfpu / an.nfuncs : 0.0);
    printf("  no `jr $ra`      %d   (tail calls, or traced into data)\n", no_return);
    printf("  computed jumps   %d\n", with_indirect);
    printf("  scattered        %d   (extent >> code size; suspect boundaries)\n", scattered);
    printf("  largest          0x%08X, %u bytes of code\n", biggest_addr, biggest);

    if (list) {
        printf("\n%-12s %-10s %-8s %s\n", "addr", "size", "insns", "flags");
        for (int i = 0; i < an.nfuncs; i++) {
            const a_func *f = &an.funcs[i];
            printf("0x%08X  %-10u %-8u %s%s%s\n",
                   f->addr, f->end - f->addr, f->insns,
                   f->has_return   ? "ret "      : "",
                   f->has_indirect ? "indirect " : "",
                   f->has_vfpu     ? "vfpu"      : "");
        }
    }

    a_analysis_free(&an);
    psp_blob_free(&b);
    return 0;
}

/* ---- the emitter --------------------------------------------------------- */

/* --replace <addr>[,<addr>...] or --replace @<file>.
 *
 * The file form exists because this list grows by hand and a build script
 * carrying a dozen addresses on one command line is a quoting accident waiting
 * to happen. One address per line, `#` to end-of-line is a comment, so the file
 * can say *why* each function is replaced -- which is the part that would
 * otherwise live only in somebody's memory.
 *
 * Returns the count, or -1 with a message. */
static int parse_replace(const char *spec, uint32_t **out) {
    char *buf = NULL;
    const char *text = spec;

    if (*spec == '@') {
        FILE *fp = fopen(spec + 1, "r");
        if (!fp) { fprintf(stderr, "cannot read %s\n", spec + 1); return -1; }
        size_t cap = 4096, len = 0;
        buf = (char *)malloc(cap);
        if (!buf) { fclose(fp); return -1; }
        int ch;
        while ((ch = fgetc(fp)) != EOF) {
            if (len + 2 > cap) {
                char *nb = (char *)realloc(buf, cap * 2);
                if (!nb) { free(buf); fclose(fp); return -1; }
                buf = nb; cap *= 2;
            }
            /* Comments and newlines both just become separators. */
            buf[len++] = (char)ch;
        }
        buf[len] = '\0';
        fclose(fp);
        for (char *p = buf; *p; p++)
            if (*p == '#') while (*p && *p != '\n') *p++ = ' ';
        text = buf;
    }

    int n = 0, cap = 8;
    uint32_t *list = (uint32_t *)malloc((size_t)cap * sizeof *list);
    if (!list) { free(buf); return -1; }

    for (const char *p = text; *p; ) {
        while (*p == ',' || *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (!*p) break;
        char *end;
        const unsigned long v = strtoul(p, &end, 16);   /* addresses are hex */
        if (end == p) {
            fprintf(stderr, "--replace: not an address: %s\n", p);
            free(list); free(buf); return -1;
        }
        if (n == cap) {
            uint32_t *nl = (uint32_t *)realloc(list, (size_t)cap * 2 * sizeof *list);
            if (!nl) { free(list); free(buf); return -1; }
            list = nl; cap *= 2;
        }
        list[n++] = (uint32_t)v;
        p = end;
    }

    free(buf);
    *out = list;
    return n;
}

static int cmd_emit(const char *path, const char *outdir, const char *prefix,
                    const uint32_t *replace, int nreplace, int resume) {
    psp_blob b;
    elf_info e;
    a_analysis an;

    if (load_and_discover(path, &b, &e, &an, NULL, NULL, NULL, NULL) != 0) return 1;

    psp_module_info mi;
    const char *module = "(unknown)";
    if (e.modinfo_size && psp_modinfo_parse(b.data, b.size, e.modinfo_offset, &mi) == 0)
        module = mi.name;


    /* The import table lets each generated thunk dispatch to the HLE layer by
     * NID instead of merely trapping. */
    psp_import_entry *imp = NULL;
    int nimp = 0;
    if (e.modinfo_size) {
        uint32_t bias = e.nsegments ? e.seg[0].offset - e.seg[0].addr : 0;
        nimp = psp_collect_imports(b.data, b.size, &mi, bias, NULL, 0);
        if (nimp > 0) {
            imp = (psp_import_entry *)malloc((size_t)nimp * sizeof *imp);
            if (imp) nimp = psp_collect_imports(b.data, b.size, &mi, bias, imp, nimp);
            else nimp = 0;
        }
    }

    emit_opts o;
    o.outdir = outdir;
    o.prefix = prefix ? prefix : "recomp";
    o.module = module;
    o.imports = imp;
    o.nimports = nimp;
    o.replace = replace;
    o.nreplace = nreplace;
    o.resume = resume;

    printf("module:     %s\n", module);
    printf("functions:  %d\n", an.nfuncs);
    printf("imports:    %d\n", an.nimports);

    /* An address that names no discovered function emits nothing, declares
     * nothing, and fails at link time with an undefined psp_func_<addr> that
     * the host does define -- which reads as a build system problem rather
     * than as the typo it is. Say so here instead. */
    if (nreplace) {
        printf("replacing:  %d function(s) left to the host\n", nreplace);
        for (int i = 0; i < nreplace; i++) {
            int found = 0;
            for (int j = 0; j < an.nfuncs && !found; j++)
                if (an.funcs[j].addr == replace[i]) found = 1;
            printf("  0x%08X  %s\n", replace[i],
                   found ? "ok" : "*** NOT A DISCOVERED FUNCTION -- nothing will be replaced");
        }
    }
    printf("\n");

    int rc = a_emit(&an, &o);

    if (rc == 0) {
        /* The instructions the emitter could not translate are the honest
         * measure of how far the output is from running. They are traps, not
         * silence, so they will announce themselves — but knowing the count
         * up front is better than discovering it one crash at a time. */
        printf("\n%llu of %llu instructions are VFPU and emit as traps (%.2f%%).\n",
               (unsigned long long)an.vfpu, (unsigned long long)an.insns,
               an.insns ? 100.0 * (double)an.vfpu / (double)an.insns : 0.0);
    }

    free(imp);
    a_analysis_free(&an);
    psp_blob_free(&b);
    return rc == 0 ? 0 : 1;
}

/* ---- decryption ---------------------------------------------------------- */

static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Narrow a blob down to the ~PSP module inside it, whatever it is wrapped in.
 * Returns NULL if there is no ~PSP layer to be found. */
static const uint8_t *find_psp_module(const uint8_t *d, size_t len, size_t *out_len) {
    switch (psp_sniff(d, len < 16 ? len : 16)) {
    case FMT_PSP:
        *out_len = len;
        return d;
    case FMT_SCE:
        /* ~SCE is a 0x40-byte shim over the real payload. */
        if (len > 0x40 && psp_sniff(d + 0x40, 16) == FMT_PSP) {
            *out_len = len - 0x40;
            return d + 0x40;
        }
        return NULL;
    case FMT_PBP: {
        pbp_info p;
        if (pbp_parse(d, len, &p) == 0 && p.size[6] &&
            p.offset[6] + p.size[6] <= len &&
            psp_sniff(d + p.offset[6], 16) == FMT_PSP) {
            *out_len = p.size[6];
            return d + p.offset[6];
        }
        return NULL;
    }
    default:
        return NULL;
    }
}

/* Does a 0x30-byte block look like KIRK CMD1 metadata?
 *
 * The metadata block is highly constrained — a mode of exactly 1, a signature
 * flag of 0 or 1, a retail/devkit word that is 0 or all-ones, and 0x18 bytes
 * of mandatory zeros. That is roughly 100 bits of structure, so a false
 * positive in a few hundred bytes of header is very unlikely.
 *
 * This exists because the transform from a ~PSP header to a CMD1 header is the
 * one part of the chain we do not yet have a trustworthy description of. Rather
 * than guess an offset and silently decrypt garbage, we look for the block and
 * report where it actually is — measuring the layout instead of assuming it.
 */
static int looks_like_cmd1_meta(const uint8_t *m, size_t avail, uint32_t file_size) {
    if (avail < KIRK1_META_SIZE) return 0;

    if (rd32le(m + 0x00) != 1)   return 0;          /* mode */
    if (rd32le(m + 0x04) > 1)    return 0;          /* 0 = CMAC, 1 = ECDSA */
    if (rd32le(m + 0x08) != 0)   return 0;          /* reserved */

    uint32_t devkit = rd32le(m + 0x0C);
    if (devkit != 0 && devkit != 0xFFFFFFFFu) return 0;

    uint32_t data_size = rd32le(m + 0x10);
    uint32_t pad       = rd32le(m + 0x14);
    if (data_size == 0 || data_size > file_size) return 0;
    if (pad > 0x10000u) return 0;

    for (int i = 0x18; i < 0x30; i++) if (m[i]) return 0;
    return 1;
}

/* ---- interp ---------------------------------------------------------------
 *
 * Run a module under the interpreter oracle. See docs/ORACLE.md and interp.h.
 *
 * The module is mapped at the address it was *linked* at, not somewhere inside
 * the console's RAM window. A PRX links at 0, and the recompiled C has those
 * addresses baked in as literals, so the oracle has to use the same address
 * space as the code it is meant to be compared against. psp_mem_map_module()
 * exists for exactly this. --base is the exception, for running a probe the
 * way the PSP would: it moves the module into user memory (see cmd_interp).
 *
 * Relocations are deliberately not applied. Doing so would move every address
 * away from the ones the emitter used and make traces from the two sides
 * incomparable — which is the only thing this command is for. The consequence
 * is that running from the module entry does not get far: real bring-up starts
 * at a single leaf function, not at _start. */

/* The main context's guest stack, taken from the allocator rather than picked.
 *
 * It used to be the top of RAM, and the allocator's high end is *also* the top
 * of RAM -- so the first `psp_sysmem_alloc(size, from_high)` returned a block
 * containing it. Every guest thread's stack comes from exactly that call, so
 * the module entry and the first thread it started shared their stacks.
 *
 * That was invisible for as long as nothing wrote a whole stack at once. It
 * stopped being invisible when sceKernelStartThread began painting a fresh
 * stack with 0xFF: 125 of the 127 threads tests went from "returned" to
 * "invalid instruction" in one step, because starting a thread wiped the
 * context that had started it.
 *
 * Asking the allocator removes the overlap by construction instead of moving
 * the collision somewhere else. */
#define INTERP_STACK_SIZE 0x10000u
#define INTERP_RA_DONE    0x0DEAD000u

/* Guest re-entry attempted from inside an HLE handler. Recorded rather than
 * fatal — see where this is installed. */
static uint64_t g_reentry_count;
static uint32_t g_reentry_first;

static void interp_note_reentry(uint32_t addr) {
    if (!g_reentry_count) g_reentry_first = addr;
    g_reentry_count++;
}

/* Hand the interpreter the thunk-to-NID map, so firmware calls reach HLE
 * instead of running the unlinked `jr $ra` the linker left in the module.
 * Returns the number of thunks registered; the table is owned by the caller
 * and must outlive the run, so it is returned through `out_tbl`. */
static int interp_bind_imports(const psp_blob *b, const elf_info *e,
                               psp_interp_import **out_tbl) {
    *out_tbl = NULL;
    if (!e->modinfo_size) return 0;

    psp_module_info mi;
    if (psp_modinfo_parse(b->data, b->size, e->modinfo_offset, &mi) != 0) return 0;

    const uint32_t bias = e->nsegments ? e->seg[0].offset - e->seg[0].addr : 0;
    int n = psp_collect_imports(b->data, b->size, &mi, bias, NULL, 0);
    if (n <= 0) return 0;

    psp_import_entry *imp = (psp_import_entry *)malloc((size_t)n * sizeof *imp);
    if (!imp) return 0;
    n = psp_collect_imports(b->data, b->size, &mi, bias, imp, n);

    psp_interp_import *tbl = (psp_interp_import *)malloc((size_t)n * sizeof *tbl);
    if (!tbl) { free(imp); return 0; }
    for (int i = 0; i < n; i++) {
        tbl[i].addr = imp[i].addr;
        tbl[i].nid  = imp[i].nid;
    }
    free(imp);

    const int bound = psp_interp_set_imports(tbl, n);
    if (bound <= 0) { free(tbl); return 0; }
    *out_tbl = tbl;
    return bound;
}   /* never mapped: the run stops here */

/* The path a PSP would pass as the module's argument when it started it from
 * the Memory Stick: ms0:/PSP/GAME/<name>/EBOOT.PBP, with <name> the host
 * file's name without its extension, or its directory's name when the file
 * is itself an EBOOT.PBP. */
static void interp_default_argv0(const char *host, char *out, size_t cap) {
    const char *base = strrchr(host, '/');
    base = base ? base + 1 : host;
    const char *name = base;
    size_t len = strlen(base);
    const char *dot = strrchr(base, '.');
    if (dot && dot != base) len = (size_t)(dot - base);
    if (!strcasecmp(base, "EBOOT.PBP") && base > host) {
        const char *end = base - 1;              /* the '/' before EBOOT.PBP */
        const char *start = end;
        while (start > host && start[-1] != '/') start--;
        if (end > start) { name = start; len = (size_t)(end - start); }
    }
    snprintf(out, cap, "ms0:/PSP/GAME/%.*s/EBOOT.PBP", (int)len, name);
}

static int cmd_interp(const char *path, uint32_t from, int have_from,
                      uint64_t budget, int trace, int trace_regs, int dispatch,
                      int drain_s, const char *argv0, uint32_t base, int have_base) {
    psp_blob b;
    if (psp_blob_read(path, &b) != 0) { fprintf(stderr, "cannot read %s\n", path); return 1; }

    elf_info e;
    if (elf_parse(b.data, b.size, &e) != 0) {
        fprintf(stderr, "not an ELF/PRX: %s\n"
                        "  interp needs a decrypted module — run `decrypt` first\n", path);
        psp_blob_free(&b);
        return 1;
    }

    if (psp_mem_init() != 0) {
        fprintf(stderr, "cannot allocate guest memory\n");
        psp_blob_free(&b); return 1;
    }

    /* --base: load a PRX where the PSP's loader would rather than where it
     * was linked, so the module's own buffers sit in user memory as they do
     * on the console. A probe tells its buffers apart from constants by that
     * range: sasprobe prints a word pointing into it as "vag+0", where a
     * module at its link address 0 prints 0003AE40. Not for a differential
     * run: the emitter writes the linked addresses into the C, and a moved
     * module no longer lines up with it. --from names a linked address, as
     * `dis` and `funcs` print them, and moves with the module. */
    uint32_t shift = 0;
    if (have_base) {
        int err = 0;
        shift = psp_rebase_image(&e, base, &err);
        if (err == -1) {
            fprintf(stderr, "--base: %s is not a relocatable PRX; it runs where it was linked\n", path);
            goto fail;
        }
        if (err == -2) { fprintf(stderr, "--base: 0x%08X is not 256-byte aligned\n", base); goto fail; }
        if (have_from) from += shift;
    }

    psp_load_info li;
    if (psp_load_module(&b, &e, &li) != 0) {
        fprintf(stderr, "cannot load module image\n"); goto fail;
    }
    const uint32_t lo = li.lo, hi = li.hi;

    psp_hle_init();
    /* After psp_hle_init, which resets the allocator. */
    psp_sysmem_reserve_module(li.lo, li.hi);
    psp_interp_import *imports = NULL;
    const int nimports = interp_bind_imports(&b, &e, &imports);

    /* Some HLE handlers call back into guest code — a thread entry point, a
     * registered callback — and they do it through psp_dispatch(), which only
     * knows about *recompiled* functions. Under the interpreter nothing is
     * registered, so every such re-entry is a miss, and the default handler
     * aborts. Counting them and carrying on says how much of the module is out
     * of reach this way, which is the useful answer; crashing says nothing. */
    psp_set_miss_handler(interp_note_reentry);

    /* --dispatch: serve re-entries instead of counting them.
     *
     * The counting handler above answers the oracle's question -- which
     * functions reach firmware callbacks, and so are uncomparable -- and is
     * the right default for a differential run. Running a *program* is the
     * other use: its thread starts and callbacks have to happen for anything
     * after the first instruction to execute at all. Serving them runs the
     * target interpreted, nested, charged against this run's budget. */
    if (dispatch) {
        psp_interp_service_dispatch(1);
        /* And give each guest thread a host thread, so a wait can park and be
         * resumed. Running a *program* is what this verb is for, and a program
         * that blocks needs somewhere to block; the differential oracle, which
         * shares the dispatch hook, deliberately does not ask for this. */
        psp_interp_service_threads(1, budget);
    }

    const uint32_t entry = have_from ? from : e.entry;

    memset(&psp_cpu, 0, sizeof psp_cpu);
    psp_cpu_reset_fp();
    const uint32_t stack = psp_sysmem_alloc(INTERP_STACK_SIZE, 1);
    if (!stack) { fprintf(stderr, "cannot allocate a guest stack\n"); goto fail; }
    psp_cpu.r[PSP_REG_SP] = (stack + INTERP_STACK_SIZE - 64) & ~15u;
    /* The module's argument block: its own path. threadprobe step 2 (fw 6.60)
     * has main() see argc=1 and argv[0]="ms0:/PSP/GAME/threadprobe/EBOOT.PBP"
     * -- the loader hands module_start (length with the NUL, pointer) and
     * crt0 passes the pair on to StartThread, which copies it to the top of
     * the main thread's stack; that copy is what puts main's stack 0x30 below
     * where it was here (step 92). This started the module with $a0 = $a1 = 0.
     * Not for --from, which starts at an arbitrary function. */
    if (!have_from) {
        char def[512];
        if (!argv0) { interp_default_argv0(path, def, sizeof def); argv0 = def; }
        const uint32_t len = (uint32_t)strlen(argv0) + 1;
        const uint32_t at = (stack + INTERP_STACK_SIZE - 64 - len) & ~15u;
        for (uint32_t i = 0; i < len; i++) psp_write8(at + i, (uint8_t)argv0[i]);
        psp_cpu.r[PSP_REG_A0] = len;
        psp_cpu.r[PSP_REG_A1] = at;
        psp_cpu.r[PSP_REG_SP] = at - 64;
    }
    /* A module with a small-data area reads it through $gp and never loads the
     * register itself; the value comes from the module info. */
    psp_cpu.r[PSP_REG_GP] = li.gp;

    psp_interp it;
    psp_interp_init(&it, entry, INTERP_RA_DONE, budget);
    it.trace      = trace ? stdout : NULL;
    it.trace_regs = trace_regs;

    printf("module:   %s\n", path);
    printf("mapped:   0x%08X + %u bytes (%d segments)%s\n", lo, hi - lo, e.nsegments,
           have_base ? " (--base)" : "");
    printf("relocs:   %d applied%s\n", li.nrelocs,
           li.nreloc_skipped ? " (some skipped -- see loader.c)" : "");
    printf("gp:       0x%08X%s\n", li.gp,
           li.gp ? "" : " (module declares none)");
    printf("imports:  %d thunks routed to HLE\n", nimports);
    printf("entry:    0x%08X%s\n", entry, have_from ? " (--from)" : " (module entry)");
    printf("budget:   %llu instructions\n", (unsigned long long)budget);
    printf("---\n");

    psp_interp_run(&it);

    /* The entry point has returned; the threads it started have not.
     *
     * The main context steps aside and lets the scheduler run them until they
     * are all done, which is also when a thread that parked gets woken and
     * resumed -- the drain is not a mop-up pass but the rest of the program.
     *
     * Except after sceKernelExitGame, which is a program saying it is finished.
     * Draining then would run stragglers past the end and let them print, so
     * the run stops instead. */
    int live = 0;
    if (dispatch) {
        if (it.status == I_EXIT) psp_sched_stop_all("sceKernelExitGame");
        else                     live = psp_sched_drain(drain_s);
    }

    printf("---\n");
    printf("stopped:  %s\n", psp_interp_status_str(it.status));
    printf("executed: %llu instructions\n", (unsigned long long)it.executed);
    if (dispatch)
        printf("re-entry: served interpreted; nest refused %llu, failed %llu\n",
               (unsigned long long)psp_interp_nest_refused(),
               psp_interp_nest_failed());
    /* No duration is claimed here: the drain gives up either on the deadline or
     * the moment nothing is runnable, and its return value does not say which.
     * It has already printed the distinction itself, with the reason. */
    if (live) {
        printf("threads:  %d still alive\n", live);
        psp_sched_dump_threads(stdout);
    }
    else if (g_reentry_count)
        printf("re-entry: %llu HLE callbacks into guest code, first 0x%08X\n"
               "          (needs the interpreter to service psp_dispatch)\n",
               (unsigned long long)g_reentry_count, g_reentry_first);
    if (it.status != I_OK_RETURN && it.status != I_BUDGET && it.status != I_EXIT) {
        a_insn in;
        a_decode(psp_read32(it.fault_pc), it.fault_pc, &in);
        char buf[96];
        a_format(&in, buf, sizeof buf);
        printf("at:       0x%08X  %s\n", it.fault_pc, buf);
    }
    printf("bad mem:  %llu accesses\n", (unsigned long long)psp_mem_bad_access);

    /* No guest thread may outlive this point. The drain that gives up on its
     * deadline leaves the token holder running -- between firmware calls it
     * never looks at the token -- and the free below would pull the RAM out
     * from under it: cpu/vfpu/vector and utility/msgdialog both died in
     * psp_read32 that way, the first with its output cut mid-line at 4452 of
     * 5329 and reported for a week as an unimplemented instruction. Stop
     * everything and wait for the host threads to actually exit; the
     * interpreter polls the stop flag, so the wait is one instruction long.
     * After the summary, so the thread list above still names who was alive. */
    if (dispatch) {
        if (live) psp_sched_stop_all("drain deadline");
        psp_sched_join_all();
    }

    psp_interp_free_imports();
    free(imports);
    psp_mem_free();
    psp_blob_free(&b);
    /* A clean return is success, and so is the guest asking to exit -- a
     * program that runs to its own sceKernelExitGame finished, it just had
     * nowhere to return to. Anything else is a finding, not a crash, so report
     * it through the exit status without a scary message. */
    return (it.status == I_OK_RETURN || it.status == I_EXIT) ? 0 : 1;

fail:
    psp_mem_free();
    psp_blob_free(&b);
    return 1;
}

static int cmd_decrypt(const char *path, const char *keypath) {
    psp_blob b;
    if (psp_blob_read(path, &b) != 0) { fprintf(stderr, "cannot read %s\n", path); return 1; }

    size_t mod_len = 0;
    const uint8_t *mod = find_psp_module(b.data, b.size, &mod_len);
    if (!mod) {
        fprintf(stderr, "no ~PSP module found in %s (format: %s)\n",
                path, psp_format_name(psp_sniff(b.data, 16)));
        psp_blob_free(&b);
        return 1;
    }

    psp_header h;
    if (psp_header_parse(mod, mod_len, &h) != 0) {
        fprintf(stderr, "malformed ~PSP header\n");
        psp_blob_free(&b);
        return 1;
    }

    printf("module:   %s\n", h.modname);
    printf("tag:      0x%08X   decrypt mode %u\n", h.tag, h.decrypt_mode);
    printf("sizes:    %u encrypted -> %u decrypted\n", h.psp_size, h.elf_size);

    /* Probe the header for an embedded CMD1 metadata block. */
    printf("\nprobing for a KIRK CMD1 metadata block...\n");
    int found = 0;
    const size_t scan_limit = mod_len < 0x400 ? mod_len : 0x400;
    for (size_t off = 0; off + KIRK1_META_SIZE <= scan_limit; off += 4) {
        if (!looks_like_cmd1_meta(mod + off, mod_len - off, (uint32_t)mod_len)) continue;

        /* The metadata sits at +0x60 within a CMD1 header, so the header
         * itself begins 0x60 earlier. */
        printf("  found at module offset 0x%zX", off);
        if (off >= KIRK1_META_OFFSET) {
            size_t hdr = off - KIRK1_META_OFFSET;
            printf("  => CMD1 header at 0x%zX\n", hdr);
            kirk1_info info;
            if (kirk_cmd1_peek(mod + hdr, mod_len - hdr, &info) == KIRK_OK) {
                printf("    data size    %u\n", info.data_size);
                printf("    padding      0x%X\n", info.data_offset);
                printf("    signature    %s\n", info.use_ecdsa ? "ECDSA" : "AES-CMAC");
                printf("    target       %s\n", info.is_devkit ? "devkit" : "retail");
                if (info.data_size == h.elf_size)
                    printf("    *** data size matches the header's declared ELF size ***\n");
            }
        } else {
            printf("  (too close to the start to hold a full CMD1 header)\n");
        }
        found++;
    }
    if (!found) printf("  none found in the first 0x%zX bytes\n", scan_limit);

    /* Only attempt an actual decrypt if we have a key to attempt it with. */
    key_store ks;
    keys_load(&ks, keypath);

    char err[256];
    const uint8_t *kirk1 = keys_require(&ks, "kirk1", 16, err, sizeof err);
    if (!kirk1) {
        printf("\nno decryption attempted: %s\n", err);
        printf("See docs/DECRYPT.md for the key file format.\n");
        psp_blob_free(&b);
        return found ? 0 : 1;
    }

    printf("\nkeys loaded from %s\n", ks.path);
    printf("(the ~PSP tag -> CMD1 transform is not implemented yet; see\n"
           " docs/DECRYPT.md phase 2b. `kirk1` decrypts a raw CMD1 blob today.)\n");

    psp_blob_free(&b);
    return 0;
}

/* Decrypt a raw KIRK CMD1 blob. Complete and tested — the layer above it is
 * what is still missing. */
static int cmd_kirk1(const char *path, const char *outpath, const char *keypath) {
    psp_blob b;
    if (psp_blob_read(path, &b) != 0) { fprintf(stderr, "cannot read %s\n", path); return 1; }

    key_store ks;
    keys_load(&ks, keypath);

    char err[256];
    const uint8_t *kirk1 = keys_require(&ks, "kirk1", 16, err, sizeof err);
    if (!kirk1) {
        fprintf(stderr, "%s\n", err);
        fprintf(stderr, "See docs/DECRYPT.md for the key file format.\n");
        psp_blob_free(&b);
        return 1;
    }

    kirk1_info info;
    uint8_t *out = NULL;
    uint32_t out_len = 0;
    kirk_result r = kirk_cmd1_decrypt(kirk1, b.data, b.size, &info, &out, &out_len);

    /* Structural failures happen before any CMAC is computed, so reporting
     * "CMAC: FAILED" for them would point at the key when the real problem is
     * that this is not a CMD1 blob at all. */
    if (r == KIRK_ERR_TOO_SMALL || r == KIRK_ERR_BAD_MODE ||
        r == KIRK_ERR_ECDSA     || r == KIRK_ERR_SIZE_OVERFLOW) {
        fprintf(stderr, "not a usable KIRK CMD1 blob: %s\n", kirk_strerror(r));
        psp_blob_free(&b);
        return 1;
    }

    printf("mode:         %u\n", info.mode);
    printf("signature:    %s\n", info.use_ecdsa ? "ECDSA" : "AES-CMAC");
    printf("data size:    %u\n", info.data_size);
    printf("padding:      0x%X\n", info.data_offset);
    printf("header CMAC:  %s\n", info.header_cmac_ok ? "OK" : "FAILED");
    printf("body CMAC:    %s\n", info.data_cmac_ok ? "OK" : "FAILED");

    if (r != KIRK_OK) {
        fprintf(stderr, "\ndecryption failed: %s\n", kirk_strerror(r));
        psp_blob_free(&b);
        return 1;
    }

    if (outpath) {
        FILE *f = fopen(outpath, "wb");
        if (!f) { fprintf(stderr, "cannot write %s\n", outpath); psp_blob_free(&b); return 1; }
        fwrite(out, 1, out_len, f);
        fclose(f);
        printf("\nwrote %u bytes to %s (%s)\n", out_len, outpath,
               psp_format_name(psp_sniff(out, out_len < 16 ? out_len : 16)));
    }

    psp_blob_free(&b);
    return 0;
}

/* ---- main ---------------------------------------------------------------- */

int main(int argc, char **argv) {
    if (argc < 3) return usage();

    const char *cmd = argv[1];

    if (!strcmp(cmd, "info"))    return cmd_info(argv[2]);
    if (!strcmp(cmd, "ls"))      return cmd_ls(argv[2]);
    if (!strcmp(cmd, "cover"))   return cmd_cover(argv[2]);
    if (!strcmp(cmd, "funcs"))   return cmd_funcs(argv[2], argc > 3 && !strcmp(argv[3], "--list"));
    if (!strcmp(cmd, "emit")) {
        if (argc < 4) return usage();
        /* prefix stays positional and optional, so it is only argv[4] when
         * argv[4] is not the start of the flags. */
        const char *prefix = (argc > 4 && strncmp(argv[4], "--", 2)) ? argv[4] : NULL;
        uint32_t *replace = NULL;
        int nreplace = 0, resume = 1;
        for (int i = prefix ? 5 : 4; i < argc; i++) {
            if (!strcmp(argv[i], "--resume")) {
                resume = 1;                 /* the default; accepted as before */
            } else if (!strcmp(argv[i], "--no-resume")) {
                resume = 0;
            } else if (!strcmp(argv[i], "--replace") && i + 1 < argc) {
                uint32_t *add = NULL;
                const int n = parse_replace(argv[++i], &add);
                if (n < 0) { free(replace); return 2; }
                uint32_t *merged = (uint32_t *)realloc(replace,
                                       (size_t)(nreplace + n) * sizeof *merged);
                if (!merged) { free(replace); free(add); return 1; }
                replace = merged;
                for (int k = 0; k < n; k++) replace[nreplace + k] = add[k];
                nreplace += n;
                free(add);
            } else {
                fprintf(stderr, "unknown option: %s\n", argv[i]);
                free(replace);
                return usage();
            }
        }
        const int rc = cmd_emit(argv[2], argv[3], prefix, replace, nreplace, resume);
        free(replace);
        return rc;
    }
    if (!strcmp(cmd, "extract")) {
        if (argc < 5) return usage();
        return cmd_extract(argv[2], argv[3], argv[4]);
    }
    if (!strcmp(cmd, "dis")) {
        uint32_t start = (argc > 3) ? (uint32_t)strtoul(argv[3], NULL, 0) : 0;
        uint32_t count = (argc > 4) ? (uint32_t)strtoul(argv[4], NULL, 0) : 64;
        return cmd_dis(argv[2], start, count);
    }

    /* --keys may follow any of the crypto subcommands. */
    const char *keypath = NULL;
    for (int i = 3; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--keys")) keypath = argv[i + 1];

    if (!strcmp(cmd, "interp")) {
        uint32_t from = 0; int have_from = 0, trace = 0, regs = 0, dispatch = 0;
        uint64_t budget = 1000000;
        /* Wall-clock, not guest time, and deliberately short: the sweep already
         * wraps each test in `timeout 25`, and a drain that outlasts it turns a
         * diagnosable "still alive, parked on X" report into a killed process
         * with no output at all. psp_sched_drain's own default of 60 is for the
         * game, which is supposed to still be going. */
        int drain_s = 10;
        const char *argv0 = NULL;
        uint32_t base = 0; int have_base = 0;
        for (int i = 3; i < argc; i++) {
            if (!strcmp(argv[i], "--argv0") && i + 1 < argc) {
                argv0 = argv[++i];
            } else if (!strcmp(argv[i], "--base") && i + 1 < argc) {
                base = (uint32_t)strtoul(argv[++i], NULL, 0); have_base = 1;
            } else if (!strcmp(argv[i], "--from") && i + 1 < argc) {
                from = (uint32_t)strtoul(argv[++i], NULL, 0); have_from = 1;
            } else if (!strcmp(argv[i], "--budget") && i + 1 < argc) {
                budget = strtoull(argv[++i], NULL, 0);
            } else if (!strcmp(argv[i], "--drain") && i + 1 < argc) {
                drain_s = (int)strtol(argv[++i], NULL, 0);
            } else if (!strcmp(argv[i], "--trace")) {
                trace = 1;
            } else if (!strcmp(argv[i], "--regs")) {
                trace = 1; regs = 1;   /* --regs implies --trace */
            } else if (!strcmp(argv[i], "--dispatch")) {
                dispatch = 1;
            } else {
                fprintf(stderr, "interp: unknown option %s\n", argv[i]);
                return usage();
            }
        }
        return cmd_interp(argv[2], from, have_from, budget, trace, regs,
                          dispatch, drain_s, argv0, base, have_base);
    }
    if (!strcmp(cmd, "decrypt")) return cmd_decrypt(argv[2], keypath);
    if (!strcmp(cmd, "kirk1")) {
        const char *out = (argc > 3 && strcmp(argv[3], "--keys")) ? argv[3] : NULL;
        return cmd_kirk1(argv[2], out, keypath);
    }

    return usage();
}
