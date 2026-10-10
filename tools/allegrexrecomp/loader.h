/* allegrexrecomp — module loading.
 *
 * Maps a decrypted module into guest memory and applies its relocations.
 *
 * The relocation step is not optional, and the reason is worth stating because
 * skipping it looks harmless. A PRX links each segment at its own base and
 * stores cross-segment addresses *relative to the segment they point into*, so
 * an unrelocated pointer into the data segment reads as a small number — very
 * often plain zero. Code built from `lui`/`addiu` pairs then computes address 0
 * and walks off into .text.
 *
 * That does not surface as a crash or as a difference between the interpreter
 * and the recompiled C, because both are equally wrong. It surfaces as loops
 * that never terminate: a table walk looking for a zero entry, started at
 * address 0, scanning code that is nonzero everywhere. On Armored Core that
 * single shape accounted for 58% of all non-terminating runs.
 *
 * Applying relocations does *not* move code. Both segments are mapped at the
 * addresses they were linked at, so a relocation against segment 0 adds zero
 * and every function stays where the emitter put it. Only pointers into the
 * data segment change, which is exactly the intent.
 */
#ifndef ALLEGREX_LOADER_H
#define ALLEGREX_LOADER_H

#include "container.h"
#include "psprecomp/modules.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t lo, hi;        /* extent mapped, in guest addresses */
    uint32_t gp;            /* $gp for this module, 0 if it declares none */
    int      nsegments;
    int      nrelocs;       /* relocations applied */
    int      nreloc_skipped;/* entries with a type or base we do not handle */
} psp_load_info;

/* Apply the module's relocations to the file image, in place.
 *
 * Call this before analysis or emission, not after loading. Patching guest
 * memory instead would fix only the interpreter -- the recompiled C has its
 * address literals baked in when the emitter reads the file, so the two would
 * compute different addresses from the same instruction and disagree for a
 * reason the loader invented. Relocating the image means both consume the same
 * bytes. */
int psp_relocate_image(uint8_t *data, size_t len, const elf_info *e,
                       psp_load_info *out);

/* Relocate, then map every PT_LOAD at its linked address.
 * psp_mem_init() must already have been called. Returns 0 on success. */
int psp_load_module(psp_blob *b, const elf_info *e, psp_load_info *out);

/* A relocated module's segments into guest memory where they now are, each
 * zero past its file size: a module loaded again after an unload starts as
 * clean as the first time. For memory that exists already (RAM, or a module
 * window the host has grown). */
void psp_module_write(const psp_blob *b, const elf_info *e);

/* A relocated module, as the runtime's module table takes it
 * (psprecomp/modules.h): its extent and $gp; module_start, module_stop and
 * module_start's thread parameter from its syslib; the functions of its named
 * libraries, in malloc'd arrays the caller hands over or frees; and what
 * sceKernelQueryModuleInfo reports -- its loadable segments, its entry, and
 * the text, data and bss sizes as the PSP's loader counts them
 * (elf_exec_sizes). The thread parameter is read from guest memory, so after
 * psp_module_write. */
int psp_module_describe(const psp_blob *b, const elf_info *e, const psp_load_info *li,
                        psp_module_image *out);

/* Move a relocatable module so its lowest segment starts at `base`, as the
 * PSP's loader places one in user memory (the first module at 0x08804000):
 * every segment, the entry point and .text shift by the same amount, and
 * psp_relocate_image then adds the new segment bases to every relocated
 * word. Call before relocating. Opt-in, for `interp --base`: the recompiled
 * C is emitted at the linked addresses, so a moved module is no longer
 * comparable with it address for address.
 *
 * Returns the shift applied (base minus the old lowest address), or sets
 * *err: -1 for a module that is not a relocatable PRX (an ET_EXEC is
 * linked where it runs and cannot move), -2 for a base not 256-byte
 * aligned. */
uint32_t psp_rebase_image(elf_info *e, uint32_t base, int *err);

/* Look a section up by name.
 *
 * Worth preferring over a heuristic scan wherever the module names what it
 * wants. `psp_ctors_find` looks for the longest null-terminated run of code
 * pointers, which is a guess that vtables also satisfy; `.cplinit` says so
 * outright. The same lesson as the relocation tables: the metadata is in the
 * file, so read it. */
typedef struct { uint32_t addr, offset, size; } psp_section;
int psp_find_section(const psp_blob *b, const elf_info *e, const char *name,
                     psp_section *out);

#ifdef __cplusplus
}
#endif

#endif /* ALLEGREX_LOADER_H */
