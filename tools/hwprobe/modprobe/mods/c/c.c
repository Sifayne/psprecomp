/* mod_c -- loaded after mod_a has started: calls ProbeLibA from module_start,
 * and asks mod_a's code which module it is. */
#include "../report.h"

PSP_MODULE_INFO("ProbeModC", PSP_MODULE_USER, 1, 1);

int sceKernelGetModuleId(void);
int probe_a_add(int a, int b);
int probe_a_id(void);

int module_start(SceSize args, void *argp) {
    mod_report *r = mod_started(args, argp, NULL);
    if (r) {
        r->reached = 1;
        r->value = probe_a_add(2, 3);
        r->value2 = probe_a_id();             /* mod_a's code, called from here */
        r->value3 = sceKernelGetModuleId();   /* its own */
        r->value_set = 1;
    }
    return 0;
}

int module_stop(SceSize args, void *argp) {
    mod_stopped(args, argp);
    return 0;
}
