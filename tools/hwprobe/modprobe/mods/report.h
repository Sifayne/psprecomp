/* report.h -- what a test module tells modprobe.
 *
 * modprobe hands each module a mod_args through sceKernelStartModule's and
 * sceKernelStopModule's argument block; the module writes what it saw into
 * the mod_report that names. Nothing here goes through a library the module
 * exports, so the report does not depend on the linking it measures. */
#ifndef MODPROBE_REPORT_H
#define MODPROBE_REPORT_H

#include <pspkernel.h>
#include <string.h>

#define MOD_MAGIC 0x504D4F44u   /* "DOMP" */

typedef struct mod_report {
    int starts, stops;
    /* module_start: its arguments, and the thread it ran on */
    int start_argsize, start_magic, start_copied;
    char start_name[32];
    int start_prio, start_stack, start_attr, start_on_caller;
    int start_gp_own, start_gp_main, start_thread_gp_own;
    /* module_stop */
    int stop_argsize, stop_magic;
    char stop_name[32];
    int stop_prio, stop_stack, stop_on_caller;
    /* what the module under test did besides */
    int value, value_set, reached;
    int value2, value3;
} mod_report;

typedef struct {
    unsigned magic;
    mod_report *report;
    int caller_thid;
    unsigned caller_gp;
} mod_args;

extern char _gp[];

static inline unsigned mod_gp(void) {
    unsigned g;
    __asm__ volatile("move %0, $gp" : "=r"(g));
    return g;
}

/* What modprobe passed, or NULL if the block is not ours. */
static inline mod_args *mod_args_of(SceSize args, void *argp) {
    mod_args *a = (mod_args *)argp;
    return args == sizeof *a && a && a->magic == MOD_MAGIC ? a : NULL;
}

static inline void mod_thread(char *name, int *prio, int *stack, int *attr, int *gp_own) {
    SceKernelThreadInfo t;
    memset(&t, 0, sizeof t);
    t.size = sizeof t;
    if (sceKernelReferThreadStatus(sceKernelGetThreadId(), &t) < 0) return;
    memcpy(name, t.name, 32);
    name[31] = 0;
    *prio = t.initPriority;
    *stack = t.stackSize;
    if (attr) *attr = (int)t.attr;
    if (gp_own) *gp_own = t.gpReg == (void *)_gp;
}

/* module_start's half: everything but the module's own value. */
static inline mod_report *mod_started(SceSize args, void *argp, const void *passed) {
    mod_args *a = mod_args_of(args, argp);
    if (!a) return NULL;
    mod_report *r = a->report;
    r->starts++;
    r->start_argsize = (int)args;
    r->start_magic = 1;
    r->start_copied = argp != passed;
    mod_thread(r->start_name, &r->start_prio, &r->start_stack, &r->start_attr, &r->start_thread_gp_own);
    r->start_on_caller = sceKernelGetThreadId() == a->caller_thid;
    r->start_gp_own = mod_gp() == (unsigned)_gp;
    r->start_gp_main = mod_gp() == a->caller_gp;
    return r;
}

static inline mod_report *mod_stopped(SceSize args, void *argp) {
    mod_args *a = mod_args_of(args, argp);
    if (!a) return NULL;
    mod_report *r = a->report;
    r->stops++;
    r->stop_argsize = (int)args;
    r->stop_magic = 1;
    mod_thread(r->stop_name, &r->stop_prio, &r->stop_stack, NULL, NULL);
    r->stop_on_caller = sceKernelGetThreadId() == a->caller_thid;
    return r;
}

#endif
