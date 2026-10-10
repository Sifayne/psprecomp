/* mod_a -- the module modprobe loads, starts, queries, stops and unloads.
 * Resident; exports ProbeLibA, which mod_c and modprobe itself import. */
#include "../report.h"

PSP_MODULE_INFO("ProbeModA", PSP_MODULE_USER, 1, 2);

int sceKernelGetModuleId(void);

int probe_a_add(int a, int b) { return a + b + 0x100; }
int probe_a_id(void) { return sceKernelGetModuleId(); }

int module_start(SceSize args, void *argp) {
    mod_report *r = mod_started(args, argp, NULL);
    if (r) { r->value = sceKernelGetModuleId(); r->value_set = 1; }   /* its own id */
    return 0;   /* resident */
}

int module_stop(SceSize args, void *argp) {
    mod_stopped(args, argp);
    return 0;   /* stopped */
}
