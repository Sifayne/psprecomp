#ifndef PSPRECOMP_HOST_MODULE_LOADER_H
#define PSPRECOMP_HOST_MODULE_LOADER_H
/* The boot host's half of a game's own modules (docs/MODULES.md): which of
 * the title's recompiled modules a file is, and mapping it in. The runtime's
 * module manager (src/hle/modulemgr.c) does the rest. */
#include "container.h"
#include "loader.h"

/* The executable, as sceKernelQueryModuleInfo describes it. */
void psp_host_modules_main(const psp_blob *b, const elf_info *e, const psp_load_info *li);

/* Make room for the title's modules beside the executable and offer the
 * runtime the loader. After psp_hle_init, before any guest code runs. 0, or
 * -1 with the reason on stderr. */
int psp_host_modules_start(const char *module_path);

#endif
