/* mod_e -- a module that stops and unloads itself: module_start starts a
 * thread, which calls sceKernelSelfStopUnloadModule. */
#include "../report.h"

PSP_MODULE_INFO("ProbeModE", PSP_MODULE_USER, 1, 1);

static mod_report *g_report;

static int quit(SceSize args, void *argp) {
    g_report->reached = 1;
    sceKernelDelayThread(10000);
    g_report->value = sceKernelSelfStopUnloadModule(1, 0, NULL);
    g_report->value_set = 1;   /* only if the call came back */
    sceKernelExitDeleteThread(0);
    return 0;
}

int module_start(SceSize args, void *argp) {
    g_report = mod_started(args, argp, NULL);
    if (!g_report) return 1;
    const SceUID t = sceKernelCreateThread("ProbeModEQuit", quit, 0x30, 0x1000, 0, NULL);
    if (t >= 0) sceKernelStartThread(t, 0, NULL);
    return 0;
}

int module_stop(SceSize args, void *argp) {
    /* The self forms pass no arguments of ours: count it on the report kept
     * from module_start. */
    if (!mod_stopped(args, argp) && g_report) {
        g_report->stops++;
        g_report->stop_argsize = (int)args;
        mod_thread(g_report->stop_name, &g_report->stop_prio, &g_report->stop_stack, NULL, NULL);
    }
    return 0;
}
