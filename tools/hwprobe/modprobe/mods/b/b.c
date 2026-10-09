/* mod_b -- a module whose module_start declines to stay resident, and which
 * names its start thread's priority and stack in
 * module_start_thread_parameter: {count, priority, stack size, attributes}. */
#include "../report.h"

PSP_MODULE_INFO("ProbeModB", PSP_MODULE_USER, 1, 1);

int module_start_thread_parameter[4] = { 3, 0x31, 0x4000, 0 };

int module_start(SceSize args, void *argp) {
    mod_started(args, argp, NULL);
    return 1;   /* not resident */
}
