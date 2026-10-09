/* allegrexrecomp — a PRX's relocations, in whichever form it carries them.
 *
 * Three forms are in use:
 *
 *   SHT_PRXRELOC sections (0x700000A0)   Elf32_Rel entries, one section per
 *                                        relocated section. Every executable
 *                                        so far.
 *   a PT_PRXRELOC segment (0x700000A0)   the same entries in a program header,
 *                                        with no section headers at all.
 *                                        WipEout Pulse's libmp3.prx.
 *   a PT_PRXRELOC2 segment (0x700000A1)  a packed form, again with no section
 *                                        headers. Its libfont.prx and its
 *                                        network-dialog stub.
 *
 * The packed form is undocumented. Its layout here follows two independent
 * readings that agree on it: prxtool's LoadRelocsTypeB (pspdev, Academic Free
 * License 2.0) and uofw's sceKernelApplyPspRelSegment2 (MIT, a transcription
 * of the firmware's loader). They disagree on what kinds 6 and 7 patch.
 * WipEout's modules settle it: every kind 6 lies on a `j` and every kind 7 on
 * a `jal` (274 and 309 of them), as uofw has it, so both are 26-bit jump
 * targets. */
#ifndef ALLEGREX_RELOC_H
#define ALLEGREX_RELOC_H

#include "container.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PSP_RELOC_NONE,
    PSP_RELOC_32,           /* a word holding an address */
    PSP_RELOC_26,           /* a j/jal target */
    PSP_RELOC_HI16,         /* an upper half whose lower half is the next LO16 */
    PSP_RELOC_HI16_ADDEND,  /* an upper half that carries its lower half */
    PSP_RELOC_LO16,         /* a lower half, signed */
    PSP_RELOC_GPREL16,      /* $gp-relative: moves with the module, no patch */
    PSP_RELOC_OTHER         /* a type no PSP module has been seen to use */
};

typedef struct {
    uint32_t offset;     /* where to patch, from the start of segment ofs_seg */
    uint8_t  kind;       /* PSP_RELOC_* */
    uint8_t  ofs_seg;    /* the PT_LOAD the patch lies in */
    uint8_t  val_seg;    /* the PT_LOAD whose base the value is relative to */
    int16_t  addend;     /* PSP_RELOC_HI16_ADDEND: the lower half */
} psp_reloc;

/* Also read SHT_REL (9) sections. Only the seed harvest wants them: a static
 * executable's are absolute, and applying them would move it. */
#define PSP_RELOCS_SHT_REL 1

/* Every relocation of the module, in table order. The relocation sections
 * when the module has any, else its relocation segment. Returns how many there
 * are (which may exceed `max`: only `max` are written), or -1 if a packed
 * segment is malformed. */
int psp_relocs_read(const uint8_t *d, size_t len, const elf_info *e, int flags,
                    psp_reloc *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* ALLEGREX_RELOC_H */
