/* modprobe -- what the module manager does with a game's own modules.
 *
 * Written against PSPSDK (BSD) only. Nothing here needs input. It loads the
 * small modules in mods/ (built beside it, copied into its folder) and asks
 * the firmware about each step: the ids and error codes it answers, the
 * thread module_start and module_stop run on, the free memory a load takes,
 * where the module goes, what QueryModuleInfo reports, whether a library a
 * module exports is linked to stubs loaded before it, and what a self-unload
 * and an import nothing exports do.
 *
 * psprecomp's module manager (src/hle/modulemgr.c, docs/MODULES.md) follows
 * uofw's modulemgr and PSPSDK's pspmodulemgr.h; none of it was measured. This
 * is the measurement (MODULES.md, M6).
 *
 * The modules report through the argument block sceKernelStartModule and
 * sceKernelStopModule pass them (mods/report.h), never through a library, so
 * the report does not depend on the linking it measures. As everywhere in
 * these probes, a log line holds what the firmware decided: an id is logged
 * as valid or not, or as equal to another, an address as an offset from one
 * the probe knows. */
#include <pspkernel.h>
#include <pspmodulemgr.h>
#include <pspsysmem.h>
#include <pspthreadman.h>
#include <pspiofilemgr.h>

#include <stdio.h>
#include <string.h>
#include <malloc.h>

#include "probe.h"
#include "mods/report.h"

PSP_MODULE_INFO("modprobe", PSP_MODULE_USER, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(1024);

#define PROBE_VERSION 1

typedef unsigned int w32;

int sceKernelGetModuleId(void);

static mod_report g_report;

/* ---- helpers ---------------------------------------------------------------- */

static const char *path(const char *name) {
    static char p[256];
    snprintf(p, sizeof p, "%s%s", probe_dir(), name);
    return p;
}

/* An id the firmware answered: valid, or the error. */
static void id_of(const char *what, int r) {
    if (r > 0) out("  %s: a valid id\n", what);
    else out("  %s = %08X\n", what, (w32)r);
}

/* An answer that should be an id we know, 0, or an error. */
static void answer(const char *what, int r, int id) {
    if (id > 0 && r == id) out("  %s: the module's id\n", what);
    else if (r > 0) out("  %s: a valid id, not the module's\n", what);
    else out("  %s = %08X\n", what, (w32)r);
}

static w32 g_total, g_max;
static void memory(const char *when) {
    const w32 t = sceKernelTotalFreeMemSize(), m = sceKernelMaxFreeMemSize();
    out("  free memory %s: total %+d, largest block %+d bytes against the start\n",
        when, (int)(t - g_total), (int)(m - g_max));
}

/* The lowest address the user partition has free, by asking for it. */
static w32 lowest_free(void) {
    const SceUID b = sceKernelAllocPartitionMemory(2, "modprobe low", PSP_SMEM_Low, 256, NULL);
    if (b < 0) return 0;
    const w32 a = (w32)sceKernelGetBlockHeadAddr(b);
    sceKernelFreePartitionMemory(b);
    return a;
}

static mod_args args_for(void) {
    mod_args a;
    a.magic = MOD_MAGIC;
    a.report = &g_report;
    a.caller_thid = sceKernelGetThreadId();
    a.caller_gp = mod_gp();
    return a;
}

static void report_start(void) {
    const mod_report *r = &g_report;
    out("  module_start ran %d time(s); args %d bytes, ours %d, copied %d\n",
        r->starts, r->start_argsize, r->start_magic, r->start_copied);
    out("  its thread: \"%s\", priority %X, stack %X, attr %08X, the caller's %d\n",
        r->start_name, r->start_prio, r->start_stack, (w32)r->start_attr, r->start_on_caller);
    out("  its $gp: the module's %d, the caller's %d; the thread's gp field the module's %d\n",
        r->start_gp_own, r->start_gp_main, r->start_thread_gp_own);
}

static void report_stop(void) {
    const mod_report *r = &g_report;
    out("  module_stop ran %d time(s); args %d bytes, ours %d\n", r->stops, r->stop_argsize, r->stop_magic);
    out("  its thread: \"%s\", priority %X, stack %X, the caller's %d\n",
        r->stop_name, r->stop_prio, r->stop_stack, r->stop_on_caller);
}

/* QueryModuleInfo, every field, addresses from its text. */
static w32 query(const char *what, SceUID id) {
    SceKernelModuleInfo info;
    memset(&info, 0xCD, sizeof info);
    info.size = sizeof info;
    const int r = sceKernelQueryModuleInfo(id, &info);
    out("  QueryModuleInfo(%s) = %08X\n", what, (w32)r);
    if (r < 0) return 0;
    const w32 text = info.text_addr;
    out("    size %X, %d segment(s)", (w32)info.size, info.nsegment);
    for (int i = 0; i < info.nsegment && i < 4; i++)
        out(" [text%+d, %X]", (int)((w32)info.segmentaddr[i] - text), (w32)info.segmentsize[i]);
    out("\n    entry text%+d, gp text%+d, text %X, data %X, bss %X\n",
        (int)(info.entry_addr - text), (int)(info.gp_value - text),
        (w32)info.text_size, (w32)info.data_size, (w32)info.bss_size);
    out("    attribute %04X, version %d.%d, name \"%.28s\"\n",
        info.attribute, info.version[0], info.version[1], info.name);
    return text;
}

static SceUID load(const char *what, const char *file) {
    const SceUID id = sceKernelLoadModule(path(file), 0, NULL);
    id_of(what, id);
    return id;
}

static int start(const char *what, SceUID id) {
    memset(&g_report, 0, sizeof g_report);
    mod_args a = args_for();
    int status = 0x5A5A5A5A;
    const int r = sceKernelStartModule(id, sizeof a, &a, &status, NULL);
    answer(what, r, id);
    out("  status %08X\n", (w32)status);
    return r;
}

static int stop(const char *what, SceUID id) {
    mod_args a = args_for();
    int status = 0x5A5A5A5A;
    const int r = sceKernelStopModule(id, sizeof a, &a, &status, NULL);
    answer(what, r, id);
    out("  status %08X\n", (w32)status);
    return r;
}

static void main_function(void) {}

/* ---- the steps ---------------------------------------------------------------- */

static void errors(void) {
    section("1. errors and the executable");
    step("LoadModule of a file that is not there");
    id_of("LoadModule(missing.prx)", sceKernelLoadModule(path("missing.prx"), 0, NULL));
    step("LoadModule of a file that is not a module");
    {
        static char junk[1024];
        for (int i = 0; i < (int)sizeof junk; i++) junk[i] = (char)(i * 13 + 5);
        probe_write_file("junk.prx", junk, sizeof junk);
        id_of("LoadModule(junk.prx)", sceKernelLoadModule(path("junk.prx"), 0, NULL));
    }
    step("an id nothing has");
    {
        int status = 0x5A5A5A5A;
        out("  StartModule = %08X, status %08X\n",
            (w32)sceKernelStartModule(0x7FFFFFF1, 0, NULL, &status, NULL), (w32)status);
        out("  StopModule = %08X\n", (w32)sceKernelStopModule(0x7FFFFFF1, 0, NULL, &status, NULL));
        out("  UnloadModule = %08X\n", (w32)sceKernelUnloadModule(0x7FFFFFF1));
        query("an id nothing has", 0x7FFFFFF1);
    }
    step("which module: GetModuleId, and GetModuleIdByAddress of the executable's code, stack, heap, 0");
    {
        const int self = sceKernelGetModuleId();
        id_of("GetModuleId", self);
        int local = 0;
        void *heap = malloc(64);
        answer("ByAddress(a function of its own)", sceKernelGetModuleIdByAddress((void *)main_function), self);
        answer("ByAddress(its stack)", sceKernelGetModuleIdByAddress(&local), self);
        answer("ByAddress(its heap)", sceKernelGetModuleIdByAddress(heap), self);
        answer("ByAddress(0)", sceKernelGetModuleIdByAddress(NULL), self);
        free(heap);
        query("the executable", self);
    }
}

static void module_a(void) {
    section("2. mod_a: loaded, started, queried, stopped, unloaded");

    step("mod_f first: loaded and started before mod_a, with an import of mod_a's library");
    const SceUID idf = load("LoadModule(mod_f.prx)", "mod_f.prx");
    if (idf > 0) start("StartModule(mod_f)", idf);

    step("LoadModule(mod_a.prx): its id, its place, the memory it takes");
    const w32 low = lowest_free();
    const w32 total = sceKernelTotalFreeMemSize(), max = sceKernelMaxFreeMemSize();
    const SceUID ida = load("LoadModule(mod_a.prx)", "mod_a.prx");
    if (ida <= 0) return;
    out("  free memory: total %+d, largest block %+d bytes\n",
        (int)(sceKernelTotalFreeMemSize() - total), (int)(sceKernelMaxFreeMemSize() - max));
    const w32 text = query("mod_a", ida);
    out("  its text against the lowest free address before: %+d\n", (int)(text - low));
    answer("ByAddress(its text)", sceKernelGetModuleIdByAddress((void *)text), ida);
    out("  StopModule before StartModule = %08X\n", (w32)sceKernelStopModule(ida, 0, NULL, NULL, NULL));

    step("StartModule(mod_a): the answer, the status, module_start's thread");
    const w32 before = sceKernelTotalFreeMemSize();
    start("StartModule(mod_a)", ida);
    report_start();
    answer("GetModuleId in its module_start", g_report.value_set ? g_report.value : 0, ida);
    out("  free memory across the start: %+d\n", (int)(sceKernelTotalFreeMemSize() - before));
    out("  StartModule again = %08X\n", (w32)sceKernelStartModule(ida, 0, NULL, NULL, NULL));
    out("  UnloadModule while started = %08X\n", (w32)sceKernelUnloadModule(ida));

    step("mod_c: loaded after mod_a started; calls its library from module_start");
    const SceUID idc = load("LoadModule(mod_c.prx)", "mod_c.prx");
    if (idc > 0) {
        start("StartModule(mod_c)", idc);
        out("  reached %d; ProbeLibA add(2, 3) = %08X (0x105 is mod_a's answer)\n",
            g_report.reached, (w32)g_report.value);
        answer("mod_a's GetModuleId called from mod_c", g_report.value2, ida);
        answer("mod_c's own GetModuleId", g_report.value3, idc);
        stop("StopModule(mod_c)", idc);
        answer("UnloadModule(mod_c)", sceKernelUnloadModule(idc), idc);
    }

    step("mod_f: stopped now; its module_stop calls mod_a's library through stubs loaded before it");
    if (idf > 0) {
        memset(&g_report, 0, sizeof g_report);
        stop("StopModule(mod_f)", idf);
        out("  reached %d, value set %d; add(2, 3) = %08X (0x105 is mod_a's answer)\n",
            g_report.reached, g_report.value_set, (w32)g_report.value);
        answer("UnloadModule(mod_f)", sceKernelUnloadModule(idf), idf);
    }

    step("the same file again while mod_a is loaded");
    {
        const SceUID again = load("LoadModule(mod_a.prx) again", "mod_a.prx");
        if (again > 0) {
            out("  the same id as the first: %d\n", again == ida);
            start("StartModule(the second mod_a)", again);
            out("  module_start ran %d time(s)\n", g_report.starts);
            const int st = sceKernelStopModule(again, 0, NULL, NULL, NULL);
            out("  StopModule = %08X\n", (w32)st);
            answer("UnloadModule(the second)", sceKernelUnloadModule(again), again);
        }
    }

    step("LoadModuleByID: mod_a's file open");
    {
        const SceUID fd = sceIoOpen(path("mod_a.prx"), PSP_O_RDONLY, 0);
        const SceUID idb = fd >= 0 ? sceKernelLoadModuleByID(fd, 0, NULL) : fd;
        id_of("LoadModuleByID", idb);
        if (fd >= 0) sceIoClose(fd);
        if (idb > 0) answer("UnloadModule(never started)", sceKernelUnloadModule(idb), idb);
    }

    step("StopModule(mod_a), then UnloadModule");
    memset(&g_report, 0, sizeof g_report);
    stop("StopModule(mod_a)", ida);
    report_stop();
    out("  StopModule again = %08X\n", (w32)sceKernelStopModule(ida, 0, NULL, NULL, NULL));
    answer("UnloadModule(mod_a)", sceKernelUnloadModule(ida), ida);
    out("  free memory against before the load: total %+d, largest block %+d\n",
        (int)(sceKernelTotalFreeMemSize() - total), (int)(sceKernelMaxFreeMemSize() - max));
    query("mod_a after unloading", ida);
    answer("ByAddress(its old text)", sceKernelGetModuleIdByAddress((void *)text), ida);
}

static void module_b(void) {
    section("3. mod_b: not resident, with module_start_thread_parameter {3, 0x31, 0x4000, 0}");
    step("LoadModule(mod_b.prx), StartModule");
    const w32 total = sceKernelTotalFreeMemSize();
    const SceUID id = load("LoadModule(mod_b.prx)", "mod_b.prx");
    if (id <= 0) return;
    start("StartModule(mod_b)", id);
    report_start();
    out("  free memory against before the load: %+d\n", (int)(sceKernelTotalFreeMemSize() - total));
    query("mod_b after its start", id);
    out("  UnloadModule = %08X\n", (w32)sceKernelUnloadModule(id));
}

static void module_e(void) {
    section("4. mod_e: stops and unloads itself from a thread of its own");
    step("LoadModule(mod_e.prx), StartModule, then its thread calls SelfStopUnloadModule");
    const w32 total = sceKernelTotalFreeMemSize();
    const SceUID id = load("LoadModule(mod_e.prx)", "mod_e.prx");
    if (id <= 0) return;
    start("StartModule(mod_e)", id);
    sceKernelDelayThread(300000);
    out("  its thread reached the call %d; the call came back %d (= %08X)\n",
        g_report.reached, g_report.value_set, (w32)g_report.value);
    report_stop();
    query("mod_e afterwards", id);
    out("  free memory against before the load: %+d\n", (int)(sceKernelTotalFreeMemSize() - total));
}

static void module_d(void) {
    section("5. mod_d: an import nothing exports, called from module_start (last: it may not come back)");
    if (step("LoadModule(mod_d.prx), StartModule; module_start calls the import")) return;
    const SceUID id = load("LoadModule(mod_d.prx)", "mod_d.prx");
    if (id <= 0) return;
    start("StartModule(mod_d)", id);
    out("  reached %d, the call came back %d (= %08X)\n",
        g_report.reached, g_report.value_set, (w32)g_report.value);
}

int main(int argc, char **argv) {
    probe_init("modprobe", PROBE_VERSION, argc, argv);
    g_total = sceKernelTotalFreeMemSize();
    g_max = sceKernelMaxFreeMemSize();
    memory("at the start");
    errors();
    module_a();
    module_b();
    module_e();
    memory("before mod_d");
    module_d();
    probe_done();
    return 0;
}
