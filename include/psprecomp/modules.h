/* The modules a game loads at run time (docs/MODULES.md).
 *
 * The runtime keeps the module table and answers ModuleMgrForUser: ids,
 * starting and stopping, the exports other modules call. Which modules exist
 * is the game's business: each was recompiled into the program when the game
 * was prepared, and the host's loader (psp_modules_set_loader) recognises a
 * file sceKernelLoadModule reads as one of them, maps its image in and
 * registers its code. */
#ifndef PSPRECOMP_MODULES_H
#define PSPRECOMP_MODULES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A module mapped into guest memory, as the loader reports it. */
typedef struct {
    uint32_t lo, hi;                 /* its extent */
    /* The user-partition block the loader placed it in, or 0: the runtime
     * then takes the module's size from the partition itself. */
    uint32_t block;
    uint32_t gp;
    uint32_t entry;                  /* the ELF's entry, which QueryModuleInfo reports */
    uint32_t start, stop;            /* module_start and module_stop, or 0 */
    /* module_start_thread_parameter: priority, stack size, attributes, or 0 */
    uint32_t start_priority, start_stack, start_attr;
    uint16_t attribute;
    uint8_t  version[2];
    char     name[28];
    uint32_t text_addr, text_size, data_size, bss_size;
    int      nsegments;
    uint32_t seg_addr[4], seg_size[4];
    /* The functions its libraries export, the syslib's aside. malloc'd; the
     * runtime takes them over. */
    int       nexports;
    uint32_t *export_nid, *export_addr;
} psp_module_image;

/* Recognise `file` as one of the game's modules, map it in and register its
 * code. Returns 0 with `out` filled, or a firmware error code: the runtime
 * answers sceKernelLoadModule with it. */
typedef int (*psp_module_loader)(const uint8_t *file, size_t len, psp_module_image *out);
void psp_modules_set_loader(psp_module_loader loader);

/* The executable, so that sceKernelQueryModuleInfo can describe it too. Its
 * export arrays are not taken over. */
void psp_modules_set_main(const psp_module_image *main);

/* Loaded modules, and the address a started one exports `nid` at, or 0.
 * psp_hle_call asks, for a call the runtime does not answer itself. */
int      psp_modules_loaded(void);
uint32_t psp_modules_export(uint32_t nid);

void psp_modulemgr_init(void);
void psp_modulemgr_register(void);

#ifdef __cplusplus
}
#endif

#endif /* PSPRECOMP_MODULES_H */
