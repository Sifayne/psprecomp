/* mod_d -- imports a library nothing exports, and calls it from
 * module_start. modprobe runs it last: the call may not come back. */
#include "../report.h"

PSP_MODULE_INFO("ProbeModD", PSP_MODULE_USER, 1, 1);

int probe_none(void);

int module_start(SceSize args, void *argp) {
    mod_report *r = mod_started(args, argp, NULL);
    if (r) {
        r->reached = 1;
        r->value = probe_none();
        r->value_set = 1;
    }
    return 0;
}
