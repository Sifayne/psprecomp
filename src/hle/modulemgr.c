/* psprecomp — ModuleMgrForUser: the executable, and the modules a game loads
 * at run time (docs/MODULES.md).
 *
 * A module the game loads was recompiled into the program when the game was
 * prepared. The host's loader recognises the file sceKernelLoadModule reads,
 * maps that module's image in at the address it was recompiled for and
 * registers its code; everything after that -- ids, starting, stopping, the
 * exports the other modules call -- is here.
 *
 * Semantics follow uofw's modulemgr (MIT; src/kd/modulemgr) and PSPSDK's
 * pspmodulemgr.h (BSD). None of it is measured on the console yet: that is
 * MODULES.md's M6 probe. */
#include "psprecomp/modules.h"
#include "psprecomp/hle.h"
#include "psprecomp/cpu.h"
#include "psprecomp/mem.h"
#include "psprecomp/interrupt.h"
#include "psprecomp/state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCE_ERROR_ERRNO_FILE_NOT_FOUND          0x80010002u
#define SCE_ERROR_KERNEL_UNKNOWN_MODULE         0x8002012Eu
#define SCE_ERROR_KERNEL_UNKNOWN_MODULE_FILE    0x8002012Fu
#define SCE_ERROR_KERNEL_MODULE_ALREADY_STARTED 0x80020133u
#define SCE_ERROR_KERNEL_MODULE_NOT_STARTED     0x80020134u
#define SCE_ERROR_KERNEL_MODULE_ALREADY_STOPPED 0x80020135u
#define SCE_ERROR_KERNEL_MODULE_NOT_STOPPED     0x80020137u

/* module_start's answer: stay loaded, or not. module_stop's: stopped, or
 * refused. */
#define SCE_KERNEL_RESIDENT     0
#define SCE_KERNEL_NO_RESIDENT  1
#define SCE_KERNEL_STOP_SUCCESS 0
#define SCE_KERNEL_STOP_FAIL    1

/* The thread a module's start or stop runs on, unless the module or the
 * caller says otherwise (uofw threadman_kernel.h). */
#define MODULE_INIT_PRIORITY    32
#define USER_DEFAULT_STACKSIZE  (256 * 1024)

/* ThreadManForUser, called on the guest's behalf. */
#define NID_CREATE_THREAD       0x446D8DE6u
#define NID_START_THREAD        0xF475845Du
#define NID_WAIT_THREAD_END     0x278C0DF5u
#define NID_DELETE_THREAD       0x9FA03CD3u
#define NID_EXIT_DELETE_THREAD  0x809CE29Bu

/* A game-sharing microgame is self-contained and does not load further
 * modules, so the executable's own calls report a plausible identity rather
 * than doing anything.
 *
 * These report the id of the one loaded module.
 *
 * A previous note here recorded the opposite -- that returning an id was a
 * "plausible lie" and an error was the truthful answer -- on the strength of a
 * test showing byte-identical output either way. **That test was run while
 * `jal` never assigned `$ra`**, so every non-leaf function in the program was
 * returning through a stale register. A falsification obtained under broken
 * codegen is not a falsification.
 *
 * Re-run after that fix, the two answers differ clearly: returning an id makes
 * the game's own "libc:_getmodreent: no reent structure" diagnostic disappear
 * and drops bad memory accesses from 3 to 0.
 *
 * And an id is the *truthful* answer. The question is "which module owns this
 * address", a self-contained microgame is exactly one module, and the host has
 * loaded it. Reporting 1 states that; reporting UNKNOWN_MODULE denies a module
 * that demonstrably exists. Addresses outside every module a game loaded keep
 * that answer. */
#define PSP_MAIN_MODULE_ID 1u

#define MODULES_MAX 32

typedef struct {
    int      used;
    uint32_t id;
    psp_module_image im;
    uint32_t block;      /* what the load took from the user partition */
    int      started;    /* module_start has run, or there is none; exports live */
    int      stopped;
} module;

static module g_mod[MODULES_MAX];
/* Threads inside a module's start or stop, waiting on it from a host frame
 * that a save state could not hold. */
static int g_busy;
static psp_module_loader g_loader;
static psp_module_image  g_main;
static int               g_have_main;

void psp_modules_set_loader(psp_module_loader loader) { g_loader = loader; }

void psp_modules_set_main(const psp_module_image *main) {
    g_main = *main;
    g_main.nexports = 0;
    g_main.export_nid = g_main.export_addr = NULL;
    g_have_main = 1;
}

int psp_modules_loaded(void) {
    int n = 0;
    for (int i = 0; i < MODULES_MAX; i++) n += g_mod[i].used;
    return n;
}

uint32_t psp_modules_export(uint32_t nid) {
    for (int i = 0; i < MODULES_MAX; i++) {
        const module *m = &g_mod[i];
        if (!m->used || !m->started || m->stopped) continue;
        for (int k = 0; k < m->im.nexports; k++)
            if (m->im.export_nid[k] == nid) return m->im.export_addr[k];
    }
    return 0;
}

static module *by_id(uint32_t id) {
    for (int i = 0; i < MODULES_MAX; i++)
        if (g_mod[i].used && g_mod[i].id == id) return &g_mod[i];
    return NULL;
}

static module *by_address(uint32_t addr) {
    for (int i = 0; i < MODULES_MAX; i++)
        if (g_mod[i].used && addr >= g_mod[i].im.lo && addr < g_mod[i].im.hi) return &g_mod[i];
    return NULL;
}

/* An entry's memory, without touching the guest's: a state restores the
 * allocator itself. */
static void clear(module *m) {
    free(m->im.export_nid);
    free(m->im.export_addr);
    memset(m, 0, sizeof *m);
}

static void forget(module *m) {
    if (m->block) psp_sysmem_release(m->block);
    clear(m);
}

/* A firmware call made on the guest's behalf, from inside another. The
 * caller's argument registers are put back afterwards; $v0 is the answer. */
static uint32_t nested(uint32_t nid, uint32_t a0, uint32_t a1, uint32_t a2,
                       uint32_t a3, uint32_t t0, uint32_t t1) {
    static const int regs[] = { PSP_REG_A0, PSP_REG_A1, PSP_REG_A2, PSP_REG_A3,
                                PSP_REG_T0, PSP_REG_T1 };
    uint32_t keep[6];
    const uint32_t args[6] = { a0, a1, a2, a3, t0, t1 };
    for (int i = 0; i < 6; i++) { keep[i] = psp_cpu.r[regs[i]]; psp_cpu.r[regs[i]] = args[i]; }
    psp_hle_call(nid);
    const uint32_t v0 = psp_cpu.r[PSP_REG_V0];
    for (int i = 0; i < 6; i++) psp_cpu.r[regs[i]] = keep[i];
    return v0;
}

/* Run module_start or module_stop on a thread of its own and wait for it, as
 * the module manager does: named, prioritised and sized by the caller's
 * SceKernelSMOption {size, mpidstack, stacksize, priority, attribute} if it
 * gave one, else by the module's thread parameter, else the defaults, and
 * created under the module's $gp. Returns 0 with the function's answer in
 * *status, or the error that kept it from running. */
static uint32_t run_thread_busy(const module *m, uint32_t entry, const char *name,
                                uint32_t argsize, uint32_t argp, uint32_t opt, uint32_t *status);

static uint32_t run_thread(const module *m, uint32_t entry, const char *name,
                           uint32_t argsize, uint32_t argp, uint32_t opt, uint32_t *status) {
    g_busy++;
    const uint32_t rc = run_thread_busy(m, entry, name, argsize, argp, opt, status);
    g_busy--;
    return rc;
}

static uint32_t run_thread_busy(const module *m, uint32_t entry, const char *name,
                                uint32_t argsize, uint32_t argp, uint32_t opt, uint32_t *status) {
    uint32_t priority = m->im.start_priority ? m->im.start_priority : MODULE_INIT_PRIORITY;
    uint32_t stack = m->im.start_stack;
    uint32_t attr = m->im.start_attr;
    if (opt && psp_read32(opt) >= 20) {
        priority = psp_read32(opt + 12);
        if (psp_read32(opt + 8)) stack = psp_read32(opt + 8);
        attr |= psp_read32(opt + 16);
    }
    if (!stack) stack = USER_DEFAULT_STACKSIZE;

    /* The name only has to last the call: below the caller's stack, which
     * MIPS code does not keep anything in. */
    const uint32_t name_at = (psp_cpu.r[PSP_REG_SP] - 64) & ~3u;
    psp_mem_write_block(name_at, name, (uint32_t)strlen(name) + 1);

    /* The console gives the thread $gp when it creates it; the runtime takes
     * it from the starter when it starts (sched.c), so the module's covers
     * both. */
    const uint32_t gp = psp_cpu.r[PSP_REG_GP];
    if (m->im.gp) psp_cpu.r[PSP_REG_GP] = m->im.gp;
    const uint32_t thid = nested(NID_CREATE_THREAD, name_at, entry, priority, stack, attr, 0);
    uint32_t rc = thid;
    if ((int32_t)thid >= 0) rc = nested(NID_START_THREAD, thid, argsize, argp, 0, 0, 0);
    psp_cpu.r[PSP_REG_GP] = gp;
    if ((int32_t)thid < 0) return thid;
    if ((int32_t)rc < 0) { nested(NID_DELETE_THREAD, thid, 0, 0, 0, 0, 0); return rc; }
    *status = nested(NID_WAIT_THREAD_END, thid, 0, 0, 0, 0, 0);
    nested(NID_DELETE_THREAD, thid, 0, 0, 0, 0, 0);
    return SCE_KERNEL_ERROR_OK;
}

/* ---- loading -------------------------------------------------------------- */

static uint32_t load(const uint8_t *file, size_t len, const char *what) {
    if (!g_loader) return SCE_ERROR_KERNEL_UNKNOWN_MODULE_FILE;
    module *m = NULL;
    for (int i = 0; i < MODULES_MAX && !m; i++) if (!g_mod[i].used) m = &g_mod[i];
    if (!m) return SCE_KERNEL_ERROR_NO_MEMORY;

    psp_module_image im;
    memset(&im, 0, sizeof im);
    const int rc = g_loader(file, len, &im);
    if (rc != 0) {
        if ((uint32_t)rc == SCE_ERROR_KERNEL_UNKNOWN_MODULE_FILE)
            fprintf(stderr, "psprecomp: %s is not a module this game was prepared with\n", what);
        return (uint32_t)rc;
    }

    /* The load takes the module's size from the user partition, as the
     * console's does, although the module itself lives beside the executable:
     * games report and check free memory (docs/MODULES.md, decided 9 Oct). */
    memset(m, 0, sizeof *m);
    m->im = im;
    m->block = psp_sysmem_alloc(im.hi - im.lo, 0);
    if (!m->block) { m->used = 0; forget(m); return SCE_KERNEL_ERROR_NO_MEMORY; }
    m->used = 1;
    m->id = psp_threadman_next_uid();
    psp_interrupt_set_module(im.lo, im.hi, im.gp);
    fprintf(stderr, "psprecomp: loaded module %.28s (%s) at 0x%08X..0x%08X as 0x%X\n",
            im.name, what, im.lo, im.hi, m->id);
    return m->id;
}

/* sceKernelLoadModule(path, flags, option) */
static void hle_LoadModule(void) {
    char path[512];
    psp_str(psp_arg(0), path, sizeof path);
    size_t len = 0;
    uint8_t *file = psp_io_read_whole(path, &len);
    if (!file) { psp_ret(SCE_ERROR_ERRNO_FILE_NOT_FOUND); return; }
    psp_ret(load(file, len, path));
    free(file);
}

/* sceKernelLoadModuleByID(fd, flags, option) */
static void hle_LoadModuleByID(void) {
    size_t len = 0;
    uint8_t *file = psp_io_read_fd(psp_arg(0), &len);
    if (!file) { psp_ret(SCE_ERROR_ERRNO_FILE_NOT_FOUND); return; }
    psp_ret(load(file, len, "an open file"));
    free(file);
}

/* 0xF9275D98 is sceKernelLoadModuleBufferUsbWlan: PSPSDK's import stub
 * (src/user/ModuleMgrForUser.S; BSD) names it, and SHA-1 of the name is the
 * NID. It was registered unnamed, after eighteen ModuleMgr names guessed
 * against SHA-1 missed it. WTF's microgame calls it three times on its
 * heap-setup path, where the unregistered 0 failed heap establishment; a load
 * answers the loaded module's id, and the one module's id is what it went on
 * answering. Nothing is loaded. */
static void hle_LoadModuleBufferUsbWlan(void) { psp_ret(PSP_MAIN_MODULE_ID); }

/* ---- starting and stopping ------------------------------------------------- */

/* sceKernelStartModule(modid, argsize, argp, status, option): the module's id
 * if it stays resident, 0 if module_start said it would not, else the error. */
static void hle_StartModule(void) {
    const uint32_t id = psp_arg(0), argsize = psp_arg(1), argp = psp_arg(2);
    const uint32_t statusp = psp_arg(3), opt = psp_arg(4);
    module *m = by_id(id);
    if (!m) {
        /* The executable is started already; nothing else answers to an id
         * the table does not hold. */
        psp_ret(id == PSP_MAIN_MODULE_ID ? SCE_KERNEL_ERROR_OK : SCE_ERROR_KERNEL_UNKNOWN_MODULE);
        return;
    }
    if (m->started) { psp_ret(SCE_ERROR_KERNEL_MODULE_ALREADY_STARTED); return; }
    /* Its libraries are registered before module_start runs, which may call
     * them through its own stubs. */
    m->started = 1;
    uint32_t status = SCE_KERNEL_RESIDENT;
    if (m->im.start) {
        const uint32_t rc = run_thread(m, m->im.start, "SceModmgrStart", argsize, argp, opt, &status);
        if (rc) { m->started = 0; psp_ret(rc); return; }
    }
    if (statusp) psp_write32(statusp, status);
    if (status == SCE_KERNEL_RESIDENT) { psp_ret(id); return; }
    if (status == SCE_KERNEL_NO_RESIDENT) { forget(m); psp_ret(SCE_KERNEL_ERROR_OK); return; }
    psp_ret(status);
}

/* module_stop, if the module has one. 0 if it stopped, else its answer or
 * the error that kept it from running. */
static uint32_t stop(module *m, uint32_t argsize, uint32_t argp, uint32_t opt, uint32_t *status) {
    *status = SCE_KERNEL_STOP_SUCCESS;
    if (m->im.stop) {
        const uint32_t rc = run_thread(m, m->im.stop, "SceKernelModmgrStop", argsize, argp, opt, status);
        if (rc) return rc;
    }
    if (*status == SCE_KERNEL_STOP_SUCCESS) m->stopped = 1;
    return *status;
}

/* sceKernelStopModule(modid, argsize, argp, status, option): 0, or the
 * module's id if module_stop refused. */
static void hle_StopModule(void) {
    module *m = by_id(psp_arg(0));
    if (!m) {
        psp_ret(psp_arg(0) == PSP_MAIN_MODULE_ID ? SCE_KERNEL_ERROR_OK : SCE_ERROR_KERNEL_UNKNOWN_MODULE);
        return;
    }
    if (!m->started) { psp_ret(SCE_ERROR_KERNEL_MODULE_NOT_STARTED); return; }
    if (m->stopped) { psp_ret(SCE_ERROR_KERNEL_MODULE_ALREADY_STOPPED); return; }
    uint32_t status;
    const uint32_t rc = stop(m, psp_arg(1), psp_arg(2), psp_arg(4), &status);
    if (psp_arg(3)) psp_write32(psp_arg(3), status);
    if (rc == SCE_KERNEL_STOP_FAIL) { psp_ret(m->id); return; }
    psp_ret(rc);
}

/* sceKernelUnloadModule(modid): its id. */
static void hle_UnloadModule(void) {
    module *m = by_id(psp_arg(0));
    if (!m) {
        psp_ret(psp_arg(0) == PSP_MAIN_MODULE_ID ? SCE_KERNEL_ERROR_OK : SCE_ERROR_KERNEL_UNKNOWN_MODULE);
        return;
    }
    if (m->started && !m->stopped) { psp_ret(SCE_ERROR_KERNEL_MODULE_NOT_STOPPED); return; }
    const uint32_t id = m->id;
    forget(m);
    psp_ret(id);
}

/* A module stopping and unloading itself: the module the call came from,
 * whose thread then ends. From the executable it is the game's own exit,
 * which the boot host answers for StopUnloadSelfModuleWithStatus; these two
 * forms answer 0 there, as they did before any module could be loaded. */
static void self_stop_unload(uint32_t exit_status, uint32_t argsize, uint32_t argp,
                             uint32_t statusp, uint32_t opt) {
    module *m = by_address(psp_cpu.r[PSP_REG_RA]);
    if (!m) { psp_ret(SCE_KERNEL_ERROR_OK); return; }
    uint32_t status;
    const uint32_t rc = stop(m, argsize, argp, opt, &status);
    if (statusp) psp_write32(statusp, status);
    if (rc != SCE_KERNEL_STOP_SUCCESS) { psp_ret(rc == SCE_KERNEL_STOP_FAIL ? m->id : rc); return; }
    forget(m);
    nested(NID_EXIT_DELETE_THREAD, exit_status, 0, 0, 0, 0, 0);
}

/* sceKernelSelfStopUnloadModule(exitStatus, argsize, argp) */
static void hle_SelfStopUnloadModule(void) {
    self_stop_unload(psp_arg(0), psp_arg(1), psp_arg(2), 0, 0);
}

/* sceKernelStopUnloadSelfModule(argsize, argp, status, option) */
static void hle_StopUnloadSelfModule(void) {
    self_stop_unload(SCE_KERNEL_ERROR_OK, psp_arg(0), psp_arg(1), psp_arg(2), psp_arg(3));
}

/* ---- which module -------------------------------------------------------- */

static void hle_GetModuleId(void) {
    const module *m = by_address(psp_cpu.r[PSP_REG_RA]);
    psp_ret(m ? m->id : PSP_MAIN_MODULE_ID);
}

static void hle_GetModuleIdByAddress(void) {
    const module *m = by_address(psp_arg(0));
    psp_ret(m ? m->id : PSP_MAIN_MODULE_ID);
}

/* sceKernelQueryModuleInfo(modid, info): PSPSDK's SceKernelModuleInfo, as
 * much of it as the caller's size field says it has room for. */
static void hle_QueryModuleInfo(void) {
    const uint32_t id = psp_arg(0), info = psp_arg(1);
    const module *m = by_id(id);
    const psp_module_image *im = m ? &m->im : (id == PSP_MAIN_MODULE_ID && g_have_main) ? &g_main : NULL;
    if (!im) { psp_ret(SCE_ERROR_KERNEL_UNKNOWN_MODULE); return; }

    uint8_t out[0x60];
    memset(out, 0, sizeof out);
#define PUT32(o, v) do { const uint32_t v_ = (v); memcpy(out + (o), &v_, 4); } while (0)
    PUT32(0x00, 0x60);
    out[0x04] = (uint8_t)(im->nsegments > 4 ? 4 : im->nsegments);
    for (int i = 0; i < 4 && i < im->nsegments; i++) {
        PUT32(0x08 + 4 * i, im->seg_addr[i]);
        PUT32(0x18 + 4 * i, im->seg_size[i]);
    }
    PUT32(0x28, im->start ? im->start : 0xFFFFFFFFu);
    PUT32(0x2C, im->gp);
    PUT32(0x30, im->text_addr);
    PUT32(0x34, im->text_size);
    PUT32(0x38, im->data_size);
    PUT32(0x3C, im->bss_size);
    memcpy(out + 0x40, &im->attribute, 2);
    out[0x42] = im->version[0];
    out[0x43] = im->version[1];
    memcpy(out + 0x44, im->name, 28);
#undef PUT32
    uint32_t room = psp_read32(info);
    if (room > sizeof out) room = sizeof out;
    /* The size stays as the caller wrote it. */
    if (room > 4) psp_mem_write_block(info + 4, out + 4, room - 4);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* ---- state ----------------------------------------------------------------- */

/* The module table in a save state (docs/MODULES.md, M5). Guest memory, the
 * allocator and the interrupt contexts come back with the state; the modules'
 * code is registered at boot. What is left is this table: each entry as it
 * stands, then every entry's exports, in order. A state is the build's own,
 * so the entry is written as it is laid out, its export pointers aside. */
static const char *refuse(void) {
    return g_busy ? "a module of the game's is starting or stopping" : NULL;
}

static int state_save(psp_state_writer *w) {
    module entries[MODULES_MAX];
    uint32_t exports[2 * 4096];
    int n = 0, nx = 0;
    for (int i = 0; i < MODULES_MAX; i++) {
        if (!g_mod[i].used) continue;
        entries[n] = g_mod[i];
        entries[n].im.export_nid = entries[n].im.export_addr = NULL;
        for (int k = 0; k < g_mod[i].im.nexports; k++) {
            if (nx + 2 > (int)(sizeof exports / sizeof *exports)) return -1;
            exports[nx++] = g_mod[i].im.export_nid[k];
            exports[nx++] = g_mod[i].im.export_addr[k];
        }
        n++;
    }
    /* Nothing at all for a game that has loaded none: its states stay as
     * they were before modules could be loaded. */
    if (!n) return 0;
    return psp_state_put(w, "modules", entries, (size_t)n * sizeof *entries) ||
           psp_state_put(w, "modexps", exports, (size_t)nx * sizeof *exports) ? -1 : 0;
}

static int state_load(psp_state_reader *r, char *why, size_t size) {
    for (int i = 0; i < MODULES_MAX; i++) clear(&g_mod[i]);
    size_t bytes = 0, xbytes = 0;
    const module *in = psp_state_get(r, "modules", &bytes);
    const uint32_t *x = psp_state_get(r, "modexps", &xbytes);
    if (!in) return 0;   /* a state from before any module: none loaded */
    const size_t n = bytes / sizeof *in, nx = x ? xbytes / sizeof *x : 0;
    size_t at = 0;
    for (size_t i = 0; i < n && i < MODULES_MAX; i++) {
        module *m = &g_mod[i];
        *m = in[i];
        const int k = m->im.nexports;
        m->im.export_nid = (uint32_t *)malloc((size_t)(k ? k : 1) * sizeof(uint32_t));
        m->im.export_addr = (uint32_t *)malloc((size_t)(k ? k : 1) * sizeof(uint32_t));
        if (!m->im.export_nid || !m->im.export_addr || at + 2 * (size_t)k > nx) {
            snprintf(why, size, "the state's module table is incomplete");
            return -1;
        }
        for (int e = 0; e < k; e++, at += 2) {
            m->im.export_nid[e] = x[at];
            m->im.export_addr[e] = x[at + 1];
        }
        fprintf(stderr, "psprecomp: restored module %.28s at 0x%08X..0x%08X as 0x%X\n",
                m->im.name, m->im.lo, m->im.hi, m->id);
    }
    return 0;
}

static void state_drop(void) {
    for (int i = 0; i < MODULES_MAX; i++) clear(&g_mod[i]);
}

static const psp_state_part g_part = { .name = "modules", .refuse = refuse, .save = state_save,
                                       .load = state_load, .drop = state_drop };

void psp_modulemgr_init(void) {
    for (int i = 0; i < MODULES_MAX; i++) if (g_mod[i].used) forget(&g_mod[i]);
}

void psp_modulemgr_register(void) {
    psp_state_register(&g_part);
    psp_hle_register(0x977DE386, "ModuleMgrForUser", "sceKernelLoadModule",              hle_LoadModule);
    psp_hle_register(0xB7F46618, "ModuleMgrForUser", "sceKernelLoadModuleByID",          hle_LoadModuleByID);
    psp_hle_register(0xF9275D98, "ModuleMgrForUser", "sceKernelLoadModuleBufferUsbWlan", hle_LoadModuleBufferUsbWlan);
    psp_hle_register(0x50F0C1EC, "ModuleMgrForUser", "sceKernelStartModule",             hle_StartModule);
    psp_hle_register(0xD1FF982A, "ModuleMgrForUser", "sceKernelStopModule",              hle_StopModule);
    psp_hle_register(0x2E0911AA, "ModuleMgrForUser", "sceKernelUnloadModule",            hle_UnloadModule);
    psp_hle_register(0xD675EBB8, "ModuleMgrForUser", "sceKernelSelfStopUnloadModule",    hle_SelfStopUnloadModule);
    psp_hle_register(0xCC1D3699, "ModuleMgrForUser", "sceKernelStopUnloadSelfModule",    hle_StopUnloadSelfModule);
    psp_hle_register(0xF0A26395, "ModuleMgrForUser", "sceKernelGetModuleId",             hle_GetModuleId);
    psp_hle_register(0xD8B73127, "ModuleMgrForUser", "sceKernelGetModuleIdByAddress",    hle_GetModuleIdByAddress);
    psp_hle_register(0x748CBED9, "ModuleMgrForUser", "sceKernelQueryModuleInfo",         hle_QueryModuleInfo);
}
