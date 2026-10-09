/* ModuleMgrForUser with a module of the game's own (docs/MODULES.md, M3),
 * answering as modprobe v1 measured (probe set 25, fw 6.60).
 *
 * The loader here stands in for the boot host's: it recognises the bytes
 * "~PSPMODA" and "maps" a module whose code is a few C functions registered
 * at its addresses, as a recompiled module's registration function would. A
 * guest thread then loads, starts, calls, stops and unloads it. Synthetic: no
 * game data. */
#include "psprecomp/hle.h"
#include "psprecomp/dispatch.h"
#include "psprecomp/modules.h"
#include "psprecomp/sched.h"
#include "crypto/sha1.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; fprintf(stderr, "FAIL %d: ", __LINE__); \
                           fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

#define DRIVER   0x08801000u
#define PATHS    0x08802000u
#define STATUS   0x08803000u
#define INFO     0x08803100u
#define ARGS     0x08803200u

#define MOD_LO     0x08900000u
#define MOD_HI     0x08901000u
#define MOD_GP     0x08908000u
#define MOD_START  0x08900010u
#define MOD_STOP   0x08900020u
#define MOD_FN     0x08900030u
#define MOD_SHADOW 0x08900040u   /* exports a NID the runtime answers itself */
#define FN_NID     0x11111111u

static uint32_t driver_thread, start_thread;
static uint32_t start_gp, start_argsize, start_arg0, start_call;
static int stopped, fn_calls, shadow_calls;

static uint32_t call(const char *name, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    psp_cpu.r[PSP_REG_A0] = a; psp_cpu.r[PSP_REG_A1] = b;
    psp_cpu.r[PSP_REG_A2] = c; psp_cpu.r[PSP_REG_A3] = d;
    psp_cpu.r[PSP_REG_T0] = e;
    psp_hle_call(psp_nid(name));
    return psp_cpu.r[PSP_REG_V0];
}

static uint32_t path(const char *p) {
    psp_mem_write_block(PATHS, p, (uint32_t)strlen(p) + 1);
    return PATHS;
}

/* ---- the module's code ---- */

static void mod_fn(void) { fn_calls++; psp_cpu.r[PSP_REG_V0] = 42; }
static void mod_shadow(void) { shadow_calls++; psp_cpu.r[PSP_REG_V0] = 7; }

static void mod_start(void) {
    start_thread = psp_sched_current();
    start_gp = psp_cpu.r[PSP_REG_GP];
    start_argsize = psp_cpu.r[PSP_REG_A0];
    start_arg0 = psp_read32(psp_cpu.r[PSP_REG_A1]);
    /* Its own export, through a stub, while it starts. */
    psp_hle_call(FN_NID);
    start_call = psp_cpu.r[PSP_REG_V0];
    psp_cpu.r[PSP_REG_V0] = 0;        /* resident */
}

static void mod_stop(void) { stopped++; psp_cpu.r[PSP_REG_V0] = 0; }

static int loader(const uint8_t *file, size_t len, psp_module_image *out) {
    if (len != 8 || memcmp(file, "~PSPMODA", 8)) return (int)0x8002012F;
    out->lo = MOD_LO; out->hi = MOD_HI; out->gp = MOD_GP;
    out->start = MOD_START; out->stop = MOD_STOP;
    out->attribute = 0; out->version[0] = 1; out->version[1] = 2;
    strcpy(out->name, "sceTest_Module");
    out->text_addr = MOD_LO; out->text_size = 0x100;
    out->nsegments = 1; out->seg_addr[0] = MOD_LO; out->seg_size[0] = MOD_HI - MOD_LO;
    out->nexports = 2;
    out->export_nid = malloc(2 * sizeof(uint32_t));
    out->export_addr = malloc(2 * sizeof(uint32_t));
    out->export_nid[0] = FN_NID;                              out->export_addr[0] = MOD_FN;
    out->export_nid[1] = psp_nid("sceKernelGetThreadId");     out->export_addr[1] = MOD_SHADOW;
    psp_register(MOD_START, mod_start);
    psp_register(MOD_STOP, mod_stop);
    psp_register(MOD_FN, mod_fn);
    psp_register(MOD_SHADOW, mod_shadow);
    return 0;
}

static void driver(void) {
    driver_thread = psp_sched_current();
    const uint32_t free_before = psp_sysmem_free();

    CHECK(call("sceKernelLoadModule", path("ms0:/PSP/none.prx"), 0, 0, 0, 0) == 0x80010002u,
          "a missing file is not found");
    CHECK(call("sceKernelLoadModule", path("ms0:/PSP/other.prx"), 0, 0, 0, 0) == 0x8002012Fu,
          "a module the game was not prepared with is an unknown module file");
    CHECK(call("sceKernelLoadModule", path("ms0:/PSP/junk.prx"), 0, 0, 0, 0) == 0x80020148u,
          "a file that is no module is an unsupported type (modprobe step 2)");
    {
        const uint32_t fd = call("sceIoOpen", path("ms0:/PSP/mod.prx"), 1, 0, 0, 0);
        CHECK((int32_t)fd > 0 && call("sceKernelLoadModuleByID", fd, 0, 0, 0, 0) == 0x80020146u,
              "LoadModuleByID of a memory stick file is refused for its device (step 11)");
        call("sceIoClose", fd, 0, 0, 0, 0);
    }

    const uint32_t id = call("sceKernelLoadModule", path("ms0:/PSP/mod.prx"), 0, 0, 0, 0);
    CHECK((int32_t)id > 1, "the module loads, got %08X", id);
    CHECK(psp_sysmem_free() == free_before - (MOD_HI - MOD_LO),
          "a load takes the module's size from the user partition");
    CHECK(psp_modules_export(FN_NID) == 0, "nothing is exported before the module starts");

    psp_write32(ARGS, 0xCAFEF00D);
    CHECK(call("sceKernelStartModule", id, 4, ARGS, STATUS, 0) == id,
          "a resident module's start answers its id");
    CHECK(psp_read32(STATUS) == 0, "module_start's answer is the status");
    CHECK(start_thread && start_thread != driver_thread, "module_start runs on a thread of its own");
    CHECK(start_gp == MOD_GP, "under the module's $gp, got %08X", start_gp);
    CHECK(start_argsize == 4 && start_arg0 == 0xCAFEF00D, "with the caller's arguments");
    CHECK(start_call == 42, "module_start can call its own exports");
    CHECK(call("sceKernelStartModule", id, 0, 0, 0, 0) == 0x80020001u, "a module starts once (step 7)");
    {
        const uint32_t twin = call("sceKernelLoadModule", path("ms0:/PSP/mod.prx"), 0, 0, 0, 0);
        const int starts = (int)start_argsize;
        start_argsize = 0;
        CHECK((int32_t)twin > 0 && twin != id, "the same file loads again, as another module");
        CHECK(call("sceKernelStartModule", twin, 0, 0, 0, 0) == 0x8002013Bu && start_argsize == 0,
              "but its library is there already: it does not start (step 10)");
        CHECK(call("sceKernelUnloadModule", twin, 0, 0, 0, 0) == twin, "and unloads, never started");
        start_argsize = (uint32_t)starts;
    }

    psp_cpu.r[PSP_REG_V0] = 0;
    psp_hle_call(FN_NID);
    CHECK(psp_cpu.r[PSP_REG_V0] == 42 && fn_calls == 2, "an import the runtime does not answer reaches the export");
    const uint32_t self = call("sceKernelGetThreadId", 0, 0, 0, 0, 0);
    CHECK(self == driver_thread && shadow_calls == 0, "the runtime's own answer wins over an export");

    CHECK(call("sceKernelGetModuleIdByAddress", MOD_FN, 0, 0, 0, 0) == id, "an address in the module is its");
    CHECK(call("sceKernelGetModuleIdByAddress", 0x08800000u, 0, 0, 0, 0) == 1,
          "with no executable described, any other is the executable's");
    {
        psp_module_image main;
        memset(&main, 0, sizeof main);
        main.lo = 0x08804000u; main.hi = 0x08840000u;
        psp_modules_set_main(&main);
        CHECK(call("sceKernelGetModuleIdByAddress", 0x08804100u, 0, 0, 0, 0) == 1, "the executable's code is its");
        CHECK(call("sceKernelGetModuleIdByAddress", 0x08880000u, 0, 0, 0, 0) == 0x8002012Eu &&
              call("sceKernelGetModuleIdByAddress", 0, 0, 0, 0, 0) == 0x8002012Eu,
              "a stack or 0 is no module's (step 4)");
    }

    psp_write32(INFO, 0x60);
    CHECK(call("sceKernelQueryModuleInfo", id, INFO, 0, 0, 0) == 0, "the module can be queried");
    char name[29] = {0};
    for (int i = 0; i < 28; i++) name[i] = (char)psp_read8(INFO + 0x44 + i);
    CHECK(psp_read32(INFO) == 0x60 && psp_read8(INFO + 4) == 1 && psp_read32(INFO + 8) == MOD_LO &&
          psp_read32(INFO + 0x28) == MOD_START && psp_read32(INFO + 0x2C) == MOD_GP &&
          psp_read8(INFO + 0x42) == 1 && psp_read8(INFO + 0x43) == 2 && !strcmp(name, "sceTest_Module"),
          "and reports its segments, entry, gp, version and name");

    CHECK(call("sceKernelUnloadModule", id, 0, 0, 0, 0) == 0x80020138u,
          "a started module cannot be removed until it stops (step 7)");
    CHECK(call("sceKernelStopModule", id, 0, 0, STATUS, 0) == 0 && stopped == 1 && psp_read32(STATUS) == 0,
          "it stops, running module_stop");
    CHECK(call("sceKernelStopModule", id, 0, 0, 0, 0) == 0x80020135u, "once");
    CHECK(psp_modules_export(FN_NID) == 0, "a stopped module exports nothing");
    CHECK(call("sceKernelUnloadModule", id, 0, 0, 0, 0) == id, "it unloads, answering its id");
    CHECK(psp_sysmem_free() == free_before, "and gives its memory back");
    CHECK(call("sceKernelQueryModuleInfo", id, INFO, 0, 0, 0) == 0x8002012Eu, "then it is unknown");
    CHECK(!psp_modules_loaded(), "and the table is empty");
}

static void put_file(const char *guest, const char *bytes) {
    char host[1024];
    psp_io_mkdir_all("ms0:/PSP");
    psp_io_host_path(guest, host, sizeof host);
    FILE *f = fopen(host, "wb");
    if (!f) { failures++; fprintf(stderr, "cannot write %s\n", host); return; }
    fwrite(bytes, 1, strlen(bytes), f);
    fclose(f);
}

int main(void) {
    CHECK(psp_mem_init() == 0, "memory");
    psp_hle_init();
    psp_cpu_reset(); psp_threadman_reset(); psp_sched_reset();
    psp_sysmem_reset(); psp_sched_set_threading(1);
    psp_io_set_root("./modules-root");
    put_file("ms0:/PSP/mod.prx", "~PSPMODA");
    put_file("ms0:/PSP/other.prx", "~PSPMODB");
    put_file("ms0:/PSP/junk.prx", "JUNKJUNK");
    psp_modules_set_loader(loader);

    CHECK(psp_nid("sceKernelLoadModule") == 0x977DE386u && psp_nid("sceKernelCreateThread") == 0x446D8DE6u &&
          psp_nid("sceKernelStartThread") == 0xF475845Du && psp_nid("sceKernelWaitThreadEnd") == 0x278C0DF5u &&
          psp_nid("sceKernelDeleteThread") == 0x9FA03CD3u && psp_nid("sceKernelExitDeleteThread") == 0x809CE29Bu,
          "the module manager calls the thread manager by the right NIDs");

    psp_register(DRIVER, driver);
    CHECK(psp_sched_spawn(0x70000, DRIVER, 0x08880000, 0, 0, 0, 32) == 0, "driver spawns");
    CHECK(psp_sched_drain(10) == 0, "the run drains");
    psp_sched_join_all();
    printf("modules: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
