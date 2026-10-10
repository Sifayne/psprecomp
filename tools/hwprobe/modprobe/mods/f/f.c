/* mod_f -- loaded and started before mod_a, with an import of mod_a's
 * ProbeLibA that nothing exports yet; calls it from module_stop, once mod_a
 * has started. Whether that reaches mod_a is whether the firmware links a
 * library to stubs loaded before it. */
#include "../report.h"

PSP_MODULE_INFO("ProbeModF", PSP_MODULE_USER, 1, 1);

int probe_a_add(int a, int b);

int module_start(SceSize args, void *argp) {
    mod_started(args, argp, NULL);
    return 0;
}

int module_stop(SceSize args, void *argp) {
    mod_report *r = mod_stopped(args, argp);
    if (r) {
        r->reached = 1;
        r->value = probe_a_add(2, 3);
        r->value_set = 1;
    }
    return 0;
}
