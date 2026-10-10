/* psprecomp — the boot host's half of a game's own modules. See
 * module_loader.h and docs/MODULES.md. */
#include "module_loader.h"

#include "psprecomp/host/title.h"
#include "psprecomp/modules.h"
#include "psprecomp/mem.h"
#include "crypto/sha1.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCE_ERROR_KERNEL_ILLEGAL_OBJECT_FORMAT 0x8002012D
#define SCE_ERROR_KERNEL_UNKNOWN_MODULE_FILE   0x8002012F
#define SCE_ERROR_KERNEL_FILE_READ_ERROR       0x80020130
#define SCE_ERROR_KERNEL_NO_MEMORY             0x80020190

static char g_folder[1024];

/* The executable, described as a module is (psp_module_describe), for
 * sceKernelQueryModuleInfo. */
void psp_host_modules_main(const psp_blob *b, const elf_info *e, const psp_load_info *li) {
    psp_module_image im;
    if (psp_module_describe(b, e, li, &im) != 0) return;
    psp_modules_set_main(&im);
    free(im.export_nid);
    free(im.export_addr);
}

static const psp_title_module *known(const uint8_t *file, size_t len) {
    uint8_t digest[SHA1_DIGEST_SIZE];
    sha1(file, len, digest);
    char hex[2 * SHA1_DIGEST_SIZE + 1];
    for (int i = 0; i < SHA1_DIGEST_SIZE; i++) snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    for (unsigned i = 0; i < psp_title_info.module_count; i++)
        if (!strcmp(psp_title_info.modules[i].sha1, hex)) return &psp_title_info.modules[i];
    return NULL;
}

/* The runtime's loader: the module a file is, mapped in where its code was
 * recompiled to run, relocated by the same code the recompiler moved it with
 * (loader.c), so the image and the C agree. Its code is registered already
 * (psp_host_modules_start). */
static int load(const uint8_t *file, size_t len, psp_module_image *out) {
    const psp_title_module *k = known(file, len);
    if (!k) return (int)SCE_ERROR_KERNEL_UNKNOWN_MODULE_FILE;

    char path[1200];
    snprintf(path, sizeof path, "%s/%s", g_folder, k->image);
    psp_blob b;
    if (psp_blob_read(path, &b) != 0) {
        fprintf(stderr, "psprecomp: cannot read the module image %s\n", path);
        return (int)SCE_ERROR_KERNEL_FILE_READ_ERROR;
    }
    elf_info e;
    int err = 0;
    const int parsed = elf_parse(b.data, b.size, &e) == 0;
    psp_load_info li;
    if (!parsed || (psp_rebase_image(&e, k->base, &err), err) ||
        psp_relocate_image(b.data, b.size, &e, &li) != 0 ||
        li.lo < k->base || li.hi > k->base + k->size) {
        fprintf(stderr, "psprecomp: %s is not the module this game was prepared with\n", path);
        psp_blob_free(&b);
        return (int)SCE_ERROR_KERNEL_ILLEGAL_OBJECT_FORMAT;
    }

    psp_module_write(&b, &e);
    const int rc = psp_module_describe(&b, &e, &li, out);
    psp_blob_free(&b);
    return rc ? (int)SCE_ERROR_KERNEL_NO_MEMORY : 0;
}

int psp_host_modules_start(const char *module_path) {
    if (!psp_title_info.module_count) return 0;
    snprintf(g_folder, sizeof g_folder, "%s", module_path);
    char *slash = strrchr(g_folder, '/');
    if (slash) *slash = '\0'; else snprintf(g_folder, sizeof g_folder, ".");

    /* One window, the executable's, reaching past every module's place. The
     * console puts a module wherever the partition has room; here it is where
     * its code was recompiled for, so the room is made now. */
    uint32_t end = 0;
    for (unsigned i = 0; i < psp_title_info.module_count; i++) {
        const psp_title_module *m = &psp_title_info.modules[i];
        if (m->base + m->size > end) end = m->base + m->size;
    }
    if (psp_mem_grow_module(end) != 0) {
        fprintf(stderr, "psprecomp: no room for the game's modules up to 0x%08X beside its executable\n", end);
        return -1;
    }
    /* Every module's code is registered now rather than when the game loads
     * it. The set is then the same for the whole run, which is what a save
     * state counts on: it records how many resume sites the build has, and a
     * fresh process loading it has loaded no module yet (docs/MODULES.md, M5).
     * A jump into a module not loaded runs its code on zeroed memory where
     * the console would fault -- only a game already lost would see it. */
    for (unsigned i = 0; i < psp_title_info.module_count; i++) {
        int seen = 0;
        for (unsigned j = 0; j < i; j++)
            seen |= psp_title_info.modules[j].register_code == psp_title_info.modules[i].register_code;
        if (!seen) psp_title_info.modules[i].register_code();
    }
    psp_modules_set_loader(load);
    return 0;
}
