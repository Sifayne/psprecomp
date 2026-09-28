/* syncprobe -- what a real PSP's kernel does with its synchronisation objects
 * and memory pools, measured so that psprecomp's src/hle rests on our own
 * captures rather than on someone else's.
 *
 * Written against PSPSDK (BSD) only; imports.S declares the few calls PSPSDK
 * has no stubs for. It covers semaphores, event flags, mutexes, lwmutexes,
 * mailboxes, message pipes and the variable- and fixed-size pools: the
 * argument checks and error codes, what the Refer*Status calls write, the
 * lwmutex workarea, the mailbox's message links, the pool's block layout, and
 * above all the ORDER in which waiting threads are released.
 *
 * ## How an order is observed
 *
 * Waiters are worker threads at chosen priorities, all MORE urgent than the
 * main thread (0x20). Handing one its job therefore runs it at once, up to the
 * point where it blocks, so the order the workers queue in is the order main
 * hands the jobs out. (The workers are made once and reused; see "The worker
 * pool" below.) When the object releases one, it outranks main and runs immediately:
 * the first thing it does is append its letter to a list in memory. Main
 * releases one unit at a time, so the list reads back the order the firmware
 * chose. Where one call releases several at once, the workers share a
 * priority, so they run in the order they were made ready -- again the
 * firmware's order.
 *
 * The one exception is the "release, then delete" section, whose workers are
 * LESS urgent than main so that nothing runs between the two calls.
 *
 * ## Rules the log follows
 *
 * A line holds only what the firmware decided: return codes, counts, orders,
 * out-parameters. UIDs are logged as valid or not, thread ids by name (main,
 * WA, WB, ...), addresses as offsets from something this probe owns, and a
 * timeout's write-back only as full / part / zero. Every wait has a timeout,
 * and every section deletes what it made, so one failure does not spread.
 * Steps that hand the firmware a NULL or bad pointer are at the end of their
 * section or in the last section.
 *
 * Each claim this checks has a comment naming the psprecomp file:line that
 * makes it.
 */
#include <pspkernel.h>
#include <pspthreadman.h>
#include <pspintrman.h>
#include <pspsysmem.h>

#include <stdio.h>
#include <string.h>

#include "probe.h"

/* PSPSDK's u32 is `long unsigned` on psp-gcc; logged words use this. */
typedef unsigned int w32;

PSP_MODULE_INFO("syncprobe", PSP_MODULE_USER, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
/* A small fixed heap: the pools below are allocated by the kernel from what is
 * left of the user partition. */
PSP_HEAP_SIZE_KB(256);

#define PROBE_VERSION 2

/* ---- imports PSPSDK has no prototype or stub for (imports.S) ------------- */

int sceKernelCreateMutex(const char *name, SceUInt attr, int initCount, void *opt);
int sceKernelDeleteMutex(SceUID id);
int sceKernelLockMutex(SceUID id, int count, SceUInt *timeout);
int sceKernelTryLockMutex(SceUID id, int count);
int sceKernelUnlockMutex(SceUID id, int count);
int sceKernelCancelMutex(SceUID id, int newCount, int *numWaitThreads);
int sceKernelReferMutexStatus(SceUID id, void *info);
int sceKernelReferLwMutexStatusByID(SceUID id, void *info);
int sceKernelReferLwMutexStatus(SceLwMutexWorkarea *wa, void *info);
int sceKernelTryLockLwMutex_600(SceLwMutexWorkarea *wa, int count);
/* These two have stubs in libpspuser but no prototype in pspthreadman.h. */
int sceKernelCancelSema(SceUID id, int newCount, int *numWaitThreads);
int sceKernelCancelEventFlag(SceUID id, u32 newPattern, int *numWaitThreads);

/* ---- small helpers -------------------------------------------------------- */

#define JOB_TMO   3000000u   /* a worker's wait: far longer than any test */
#define NJOB      8
#define WAITOR    0x01
#define WAITCLEAR 0x20
#define WAITCLEARALL 0x10

static SceUID g_main;

/* A delay long enough for every thread that can run to reach its next wait. */
static void settle(void) { sceKernelDelayThread(2000); }

static const char *hex(w32 v) {
    static char b[8][12];
    static int k;
    char *s = b[k++ & 7];
    snprintf(s, 12, "%08X", (unsigned)v);
    return s;
}

/* A create's return: "uid" or the error. */
static const char *uidstr(int r) { return r > 0 ? "uid" : hex((w32)r); }
static void uidret(int r) { out("  = %s\n", uidstr(r)); }

/* The timeout word after a wait, against what it was set to. */
static const char *tcls(SceUInt left, SceUInt req) {
    if (left == req) return "full";
    if (left == 0) return "zero";
    if (left < req) return "part";
    return "grew";
}

/* ---- worker threads ------------------------------------------------------- */

typedef struct job job_t;
typedef int (*jobfn)(job_t *);
struct job {
    job_t  *self;          /* what the thread is started with */
    char    tag;
    volatile int state;    /* 0 not started, 1 in its wait, 2 wait returned, 3 ended */
    jobfn   fn, post;
    SceUID  obj, thid;
    int     a, b;
    void   *p;
    SceUInt tmo, tmo_req;
    w32     out;
    int     result, post_result;
    int     oneshot;       /* ran on a thread of its own */
    unsigned char buf[16];
};

static job_t  g_job[NJOB];
static SceUID g_gate = -1;           /* post actions that hold wait on this */

static char g_ord[64];
static int  g_nord;

static void order_reset(void) { g_nord = 0; g_ord[0] = 0; }

static void order_add(char c) {
    unsigned int f = sceKernelCpuSuspendIntr();
    if (g_nord < 63) { g_ord[g_nord++] = c; g_ord[g_nord] = 0; }
    sceKernelCpuResumeIntr(f);
}

static void show_order(void) { out("  order: %s\n", g_nord ? g_ord : "(none)"); }

/* One job, run by whichever thread was handed it. */
static void run_job(job_t *j) {
    j->state = 1;
    j->result = j->fn(j);
    order_add(j->tag);
    j->state = 2;
    if (j->post) j->post_result = j->post(j);
    j->state = 3;
}

/* ## The worker pool
 *
 * Worker i runs job i (letter 'A'+i). The eight workers are made once and
 * kept: each sleeps on its own command semaphore and, handed a job, runs it,
 * signals its done semaphore and sleeps again. Starting a job is a priority
 * change followed by a signal, so a worker more urgent than main runs at once
 * up to its wait, exactly as a freshly started thread would. (Keeping them
 * also bounds the run to a dozen thread starts; see the README.)
 *
 * The idle wait has a timeout like every other wait, far longer than a run;
 * deleting the command semaphores at the end is what makes the workers
 * return. */
#define IDLE_TMO 600000000u
static SceUID g_wthid[NJOB], g_wcmd[NJOB], g_wdone[NJOB];
static job_t *volatile g_wjob[NJOB];

static int worker_loop(SceSize len, void *argp) {
    (void)len;
    int i = *(int *)argp;
    for (;;) {
        SceUInt t = IDLE_TMO;
        int r = sceKernelWaitSema(g_wcmd[i], 1, &t);
        if (r == (int)0x800201A8) continue;           /* WAIT_TIMEOUT: idle on */
        if (r < 0) return 0;                          /* deleted: the run is over */
        run_job(g_wjob[i]);
        sceKernelSignalSema(g_wdone[i], 1);
    }
}

static int worker_make(int i) {
    char nm[4] = { 'w', (char)('A' + i), 0, 0 };
    g_wthid[i] = sceKernelCreateThread(nm, worker_loop, 0x30, 0x2000,
                                       PSP_THREAD_ATTR_USER, NULL);
    if (g_wthid[i] < 0) return g_wthid[i];
    return sceKernelStartThread(g_wthid[i], sizeof i, &i);
}

static int pool_open(void) {
    int bad = 0;
    for (int i = 0; i < NJOB; i++) {
        g_wcmd[i] = sceKernelCreateSema("wcmd", 0, 0, 1, NULL);
        g_wdone[i] = sceKernelCreateSema("wdone", 0, 0, 1, NULL);
        if (g_wcmd[i] < 0 || g_wdone[i] < 0 || worker_make(i) < 0) bad = 1;
    }
    settle();
    return bad ? -1 : 0;
}

static void pool_close(void) {
    for (int i = 0; i < NJOB; i++) {
        sceKernelDeleteSema(g_wcmd[i]);
        sceKernelDeleteSema(g_wdone[i]);
    }
    for (int i = 0; i < NJOB; i++) {
        SceUInt t = 1000000;
        if (g_wthid[i] <= 0) continue;
        if (sceKernelWaitThreadEnd(g_wthid[i], &t) < 0) sceKernelTerminateDeleteThread(g_wthid[i]);
        else sceKernelDeleteThread(g_wthid[i]);
    }
}

static job_t *job(int i, jobfn fn, SceUID obj, int a, int b) {
    job_t *j = &g_job[i];
    memset(j, 0, sizeof *j);
    j->self = j;
    j->tag  = (char)('A' + i);
    j->fn   = fn;
    j->obj  = obj;
    j->a    = a;
    j->b    = b;
    j->tmo  = j->tmo_req = JOB_TMO;
    j->out  = 0xDEADBEEF;
    return j;
}

/* Hand job j to its worker at priority prio, and let it reach its wait. */
static void spawn(job_t *j, int prio) {
    int i = (int)(j - g_job);
    j->thid = g_wthid[i];
    int r = sceKernelChangeThreadPriority(g_wthid[i], prio);
    if (r < 0) { out("  (worker %c not prioritised: %08X)\n", j->tag, (unsigned)r); j->thid = 0; return; }
    g_wjob[i] = j;
    sceKernelSignalSema(g_wcmd[i], 1);
    settle();
}

/* The one test that needs a thread to END holding something runs its job on
 * a thread of its own, made for it and deleted by reap_all. */
static int oneshot_entry(SceSize len, void *argp) {
    (void)len;
    run_job(*(job_t **)argp);
    return 0;
}

static void spawn_oneshot(job_t *j, int prio) {
    char nm[4] = { 'o', j->tag, 0, 0 };
    j->oneshot = 1;
    j->thid = sceKernelCreateThread(nm, oneshot_entry, prio, 0x2000,
                                    PSP_THREAD_ATTR_USER, NULL);
    if (j->thid < 0) { out("  (thread %c not created: %08X)\n", j->tag, (unsigned)j->thid); j->thid = 0; return; }
    int r = sceKernelStartThread(j->thid, sizeof(job_t *), &j->self);
    if (r < 0) out("  (thread %c not started: %08X)\n", j->tag, (unsigned)r);
    settle();
}

static void gate_open(void) { g_gate = sceKernelCreateSema("gate", 0, 0, NJOB, NULL); }

/* Delete the gate and wait for every job to finish. A worker still blocked
 * here is itself an observation, so it is logged, and the worker replaced. */
static void reap_all(void) {
    if (g_gate > 0) { sceKernelDeleteSema(g_gate); g_gate = -1; }
    for (int i = 0; i < NJOB; i++) {
        job_t *j = &g_job[i];
        if (j->thid <= 0) continue;
        SceUInt t = JOB_TMO + 1000000;
        int r;
        if (j->oneshot) {
            r = sceKernelWaitThreadEnd(j->thid, &t);
            if (r < 0) sceKernelTerminateDeleteThread(j->thid);
            else sceKernelDeleteThread(j->thid);
        } else {
            r = sceKernelWaitSema(g_wdone[i], 1, &t);
            if (r < 0) {
                sceKernelTerminateDeleteThread(g_wthid[i]);
                if (worker_make(i) < 0) out("  (worker %c could not be replaced)\n", j->tag);
                settle();
            }
        }
        if (r < 0) out("  (worker %c had not finished: %08X; terminated)\n", j->tag, (unsigned)r);
        j->thid = 0;
    }
}

static const char *tid_name(w32 t) {
    static char b[4][12];
    static int k;
    if (t == 0) return "0";
    if (t == 0xFFFFFFFFu) return "-1";
    if (t == (w32)g_main) return "main";
    for (int i = 0; i < NJOB; i++)
        if ((g_wthid[i] > 0 && (w32)g_wthid[i] == t) ||
            (g_job[i].thid > 0 && (w32)g_job[i].thid == t)) {
            char *s = b[k++ & 3];
            snprintf(s, 12, "W%c", 'A' + i);
            return s;
        }
    if ((int)t < 0) return hex(t);
    return "other";
}

/* How one worker came out. `kind` says what its out word is. */
static const char *(*g_ptrname)(w32);
static void show_job(const job_t *j, char kind) {
    if (j->state == 0) { out("  %c: never ran\n", j->tag); return; }
    if (j->state == 1) { out("  %c: still waiting\n", j->tag); return; }
    out("  %c: ret=%08X tmo=%s", j->tag, (unsigned)j->result, tcls(j->tmo, j->tmo_req));
    switch (kind) {
    case 'x': out(" out=%08X", (unsigned)j->out); break;
    case 'b': out(" bytes=%08X data=\"%.8s\"", (unsigned)j->out, (const char *)j->buf); break;
    case 'p': out(" got=%s", g_ptrname ? g_ptrname(j->out) : hex(j->out)); break;
    }
    if (j->post && j->state == 3) out(" post=%08X", (unsigned)j->post_result);
    out("\n");
}

static void show_jobs(int n, char kind) {
    show_order();
    for (int i = 0; i < n; i++) show_job(&g_job[i], kind);
}

/* The worker bodies: one wait each, with its timeout in the job. */
static int jf_waitsema(job_t *j) { return sceKernelWaitSema(j->obj, j->a, &j->tmo); }
static int jf_waitevf(job_t *j)  { return sceKernelWaitEventFlag(j->obj, j->a, j->b, (u32 *)&j->out, &j->tmo); }
static int jf_lockmtx(job_t *j)  { return sceKernelLockMutex(j->obj, j->a, &j->tmo); }
static int jf_locklw(job_t *j)   { return sceKernelLockLwMutex(j->p, j->a, &j->tmo); }
static int jf_recvmbx(job_t *j) {
    void *m = (void *)0xDEADBEEF;
    int r = sceKernelReceiveMbx(j->obj, &m, &j->tmo);
    j->out = (w32)m;
    return r;
}
/* The data is j->p when set, else the job's own 16-byte buffer. */
static int jf_sendmpp(job_t *j) {
    return sceKernelSendMsgPipe(j->obj, j->p ? j->p : j->buf, j->a, j->b, &j->out, &j->tmo);
}
static int jf_recvmpp(job_t *j) {
    return sceKernelReceiveMsgPipe(j->obj, j->p ? j->p : j->buf, j->a, j->b, &j->out, &j->tmo);
}
static int jf_allocvpl(job_t *j) {
    void *p = (void *)0xDEADBEEF;
    int r = sceKernelAllocateVpl(j->obj, j->a, &p, &j->tmo);
    j->out = (w32)p;
    return r;
}
static int jf_allocfpl(job_t *j) {
    void *p = (void *)0xDEADBEEF;
    int r = sceKernelAllocateFpl(j->obj, &p, &j->tmo);
    j->out = (w32)p;
    return r;
}

/* Post actions, run after the letter is recorded. */
static int gate_wait(void) { SceUInt t = JOB_TMO; return sceKernelWaitSema(g_gate, 1, &t); }
static int pf_unlockmtx(job_t *j)      { return sceKernelUnlockMutex(j->obj, j->a); }
static int pf_hold_unlockmtx(job_t *j) { gate_wait(); return sceKernelUnlockMutex(j->obj, j->a); }
static int pf_unlocklw(job_t *j)       { return sceKernelUnlockLwMutex(j->p, j->a); }
static int pf_hold_unlocklw(job_t *j)  { gate_wait(); return sceKernelUnlockLwMutex(j->p, j->a); }
static int pf_freefpl(job_t *j) {
    return j->result == 0 ? sceKernelFreeFpl(j->obj, (void *)j->out) : 0;
}

/* Arrival priorities for the four-waiter order tests: A, B, C, D queue in
 * that order, so first-come reads ABCD and most-urgent-first reads BDAC. */
static const int ARRIVE[4] = { 0x16, 0x13, 0x18, 0x14 };

/* ---- Refer*Status: every word, the name as text --------------------------- */

#define INFO_WORDS 48
#define INFO_FILL  0xA5A5A5A5u
static w32 g_info[INFO_WORDS] __attribute__((aligned(16)));
#define INFO ((void *)g_info)
static w32 g_uidref;     /* a uid word equal to this prints as "uid=obj" */

static void *info_prep(w32 size) {
    for (int i = 0; i < INFO_WORDS; i++) g_info[i] = INFO_FILL;
    g_info[0] = size;
    return g_info;
}

static void show_word(char kind, w32 v) {
    switch (kind) {
    case 'd': out("%d", (int)v); break;
    case 'u':
        if (v == INFO_FILL) out("%08X", (unsigned)v);
        else if (v && v == g_uidref) out("uid=obj");
        else if ((int)v > 0) out("uid");
        else out("%s", hex(v));
        break;
    case 't': out("%s", v == INFO_FILL ? hex(v) : tid_name(v)); break;
    case 'p': out("%s", v == INFO_FILL ? hex(v) : g_ptrname ? g_ptrname(v) : "ptr"); break;
    default:  out("%08X", (unsigned)v); break;
    }
}

/* `fields` names the words after the 32-byte name, "label:kind" each, where
 * kind is x hex, d decimal, u uid, t thread id, p pointer (via g_ptrname).
 * Words the firmware did not write still hold A5A5A5A5 and print as such. */
static void show_info(const char *fields) {
    char nm[33];
    memcpy(nm, &g_info[1], 32);
    nm[32] = 0;
    for (int i = 0; i < 32 && nm[i]; i++)
        if (nm[i] < 32 || nm[i] > 126) nm[i] = '.';
    out("  size=%X name=\"%s\"", (unsigned)g_info[0], nm);
    int w = 9;
    for (const char *f = fields; *f; ) {
        char label[16];
        int n = 0;
        while (*f && *f != ':' && n < 15) label[n++] = *f++;
        label[n] = 0;
        if (*f == ':') f++;
        char kind = *f ? *f++ : 'x';
        while (*f == ' ') f++;
        out(" %s=", label);
        show_word(kind, g_info[w++]);
    }
    out("\n");
    int last = -1;
    for (int i = w; i < INFO_WORDS; i++) if (g_info[i] != INFO_FILL) last = i;
    if (last >= 0) out("  (written past the struct, up to +%02X)\n", last * 4);
}

#define SEMA_FIELDS  "attr:x init:d cur:d max:d wait:d"
#define EVF_FIELDS   "attr:x init:x cur:x wait:d"
#define MTX_FIELDS   "attr:x init:d cur:d owner:t wait:d"
#define LW_FIELDS    "attr:x uid:u wa:p init:d cur:d owner:t wait:d"
#define MBX_FIELDS   "attr:x wait:d count:d first:p"
#define MPP_FIELDS   "attr:x buf:x free:x sendwait:d recvwait:d"
#define VPL_FIELDS   "attr:x pool:x free:x wait:d"
#define FPL_FIELDS   "attr:x bsize:x blocks:x free:d wait:d"

/* Compact status lines for use in the middle of a test. */
static void sema_state(SceUID s) {
    SceKernelSemaInfo i;
    i.size = sizeof i;
    int r = sceKernelReferSemaStatus(s, &i);
    if (r < 0) out("  refer: %08X\n", (unsigned)r);
    else out("  cur=%d wait=%d\n", i.currentCount, i.numWaitThreads);
}
static void evf_state(SceUID f) {
    SceKernelEventFlagInfo i;
    i.size = sizeof i;
    int r = sceKernelReferEventFlagStatus(f, &i);
    if (r < 0) out("  refer: %08X\n", (unsigned)r);
    else out("  cur=%08X wait=%d\n", (unsigned)i.currentPattern, i.numWaitThreads);
}
static void mtx_state(SceUID m) {
    w32 i[14];
    i[0] = 56;
    int r = sceKernelReferMutexStatus(m, i);
    if (r < 0) out("  refer: %08X\n", (unsigned)r);
    else out("  cur=%d owner=%s wait=%d\n", (int)i[11], tid_name(i[12]), (int)i[13]);
}
static void mbx_state(SceUID m);
static void mpp_state(SceUID p) {
    SceKernelMppInfo i;
    i.size = sizeof i;
    int r = sceKernelReferMsgPipeStatus(p, &i);
    if (r < 0) out("  refer: %08X\n", (unsigned)r);
    else out("  free=%X sendwait=%d recvwait=%d\n", i.freeSize,
             i.numSendWaitThreads, i.numReceiveWaitThreads);
}
static void vpl_state(SceUID v) {
    SceKernelVplInfo i;
    i.size = sizeof i;
    int r = sceKernelReferVplStatus(v, &i);
    if (r < 0) out("  refer: %08X\n", (unsigned)r);
    else out("  pool=%X free=%X wait=%d\n", i.poolSize, i.freeSize, i.numWaitThreads);
}
static int vpl_free_size(SceUID v) {
    SceKernelVplInfo i;
    i.size = sizeof i;
    return sceKernelReferVplStatus(v, &i) < 0 ? -1 : i.freeSize;
}
static void fpl_state(SceUID f) {
    SceKernelFplInfo i;
    i.size = sizeof i;
    int r = sceKernelReferFplStatus(f, &i);
    if (r < 0) out("  refer: %08X\n", (unsigned)r);
    else out("  free=%d wait=%d\n", i.freeBlocks, i.numWaitThreads);
}

/* ======================================================================== */
/* Semaphores                                                               */
/* ======================================================================== */

static void sec_sema_create(void) {
    section("semaphore: create");

    /* threadman.c:1102-1104 -- semaphores/create.expected: 1, 0x100 and
     * 0x1ff accepted; 0x200, 0x400, 0x800, 0x900, 0x1000 ... 0x10000 refused
     * with ILLEGAL_ATTR (80020191). */
    step("sema: CreateSema attr sweep, init 0 max 1");
    static const w32 attrs[] = { 0x0, 0x1, 0x100, 0x1FF, 0x200, 0x400, 0x800,
                                 0x900, 0x1000, 0x2000, 0x4000, 0x8000, 0x10000 };
    for (unsigned i = 0; i < sizeof attrs / sizeof attrs[0]; i++) {
        SceUID s = sceKernelCreateSema("attr", attrs[i], 0, 1, NULL);
        out("  attr %05X: %s\n", (unsigned)attrs[i], uidstr(s));
        if (s > 0) sceKernelDeleteSema(s);
    }

    /* threadman.c:1128-1153 -- psprecomp's CreateSema checks neither the
     * initial count nor the maximum: whatever hardware refuses here, it
     * accepts and stores as given. */
    step("sema: CreateSema init/max combinations, ReferSemaStatus of each");
    static const int im[][2] = { {0, 0}, {1, 1}, {2, 1}, {-1, 1}, {0, -1},
                                 {1, -1}, {5, 5}, {0, 0x7FFFFFFF}, {-5, -1} };
    for (unsigned i = 0; i < sizeof im / sizeof im[0]; i++) {
        SceUID s = sceKernelCreateSema("initmax", 0, im[i][0], im[i][1], NULL);
        out("  init %d max %d: %s", im[i][0], im[i][1], uidstr(s));
        if (s > 0) {
            SceKernelSemaInfo inf;
            inf.size = sizeof inf;
            sceKernelReferSemaStatus(s, &inf);
            out(" -> init=%d cur=%d max=%d", inf.initCount, inf.currentCount, inf.maxCount);
            sceKernelDeleteSema(s);
        }
        out("\n");
    }

    /* threadman.c:127-133 (psp_threadman_write_name) -- names are reported
     * truncated to 31 characters and NUL-terminated. */
    step("sema: CreateSema with a 40-character name, ReferSemaStatus");
    {
        SceUID s = sceKernelCreateSema("0123456789abcdefghijklmnopqrstuvwxyzABCD",
                                       0x1FF, 3, 7, NULL);
        uidret(s);
        if (s > 0) {
            info_prep(56);
            out("  refer: %08X\n", (unsigned)sceKernelReferSemaStatus(s, INFO));
            show_info(SEMA_FIELDS);
            sceKernelDeleteSema(s);
        }
    }

    /* threadman.c:1080-1087 -- a NULL name is ERROR (80020001). */
    step("sema: CreateSema with NULL name");
    {
        SceUID s = sceKernelCreateSema(NULL, 0, 0, 1, NULL);
        uidret(s);
        if (s > 0) sceKernelDeleteSema(s);
    }
}

static void sec_sema_ops(void) {
    section("semaphore: signal, wait, poll, refer (one thread)");
    SceUInt tmo;
    int r;

    step("sema: CreateSema init 1 max 3");
    SceUID s = sceKernelCreateSema("ops", 0, 1, 3, NULL);
    uidret(s);

    /* waitq.h:48-56 -- an immediate success leaves the whole timeout
     * ("Signaled: OK (500ms left)"). */
    step("sema: WaitSema 1 on count 1, timeout 500ms");
    tmo = 500000; r = sceKernelWaitSema(s, 1, &tmo);
    out("  = %08X tmo=%s\n", (unsigned)r, tcls(tmo, 500000));

    /* threadman.c:1321-1328 -- more than the maximum is ILLEGAL_COUNT
     * (800201BD) and the timeout word is left alone ("500ms left"). */
    step("sema: WaitSema 4 on max 3, timeout 500ms");
    tmo = 500000; r = sceKernelWaitSema(s, 4, &tmo);
    out("  = %08X tmo=%s\n", (unsigned)r, tcls(tmo, 500000));

    step("sema: WaitSema 0 and -1, timeout 500ms");
    tmo = 500000; r = sceKernelWaitSema(s, 0, &tmo);
    out("  0: %08X tmo=%s\n", (unsigned)r, tcls(tmo, 500000));
    tmo = 500000; r = sceKernelWaitSema(s, -1, &tmo);
    out("  -1: %08X tmo=%s\n", (unsigned)r, tcls(tmo, 500000));

    /* waitq.h:50-51 -- never signalled: WAIT_TIMEOUT (800201A8), 0 left. */
    step("sema: WaitSema 1 on count 0, timeout 5ms");
    tmo = 5000; r = sceKernelWaitSema(s, 1, &tmo);
    out("  = %08X tmo=%s\n", (unsigned)r, tcls(tmo, 5000));

    /* waitq.c:81-90 -- a zero timeout is a deadline already passed. */
    step("sema: WaitSema 1 on count 0, timeout 0");
    tmo = 0; r = sceKernelWaitSema(s, 1, &tmo);
    out("  = %08X tmo=%08X\n", (unsigned)r, (unsigned)tmo);

    step("sema: SignalSema 1 on count 0");
    out("  = %08X\n", (unsigned)sceKernelSignalSema(s, 1));
    sema_state(s);

    /* threadman.c:1168-1176 -- past the maximum is SEMA_OVF (800201AE), not
     * clamped, and the count does not move. */
    step("sema: SignalSema 3 on count 1 max 3 (would overflow)");
    out("  = %08X\n", (unsigned)sceKernelSignalSema(s, 3));
    sema_state(s);

    step("sema: SignalSema 2 on count 1 max 3 (reaches max)");
    out("  = %08X\n", (unsigned)sceKernelSignalSema(s, 2));
    sema_state(s);

    step("sema: SignalSema 1 on a full semaphore");
    out("  = %08X\n", (unsigned)sceKernelSignalSema(s, 1));
    sema_state(s);

    /* threadman.c:1172-1177 -- psprecomp adds whatever it is given, so a
     * negative signal lowers the count. */
    step("sema: SignalSema 0, then -1");
    out("  0: %08X\n", (unsigned)sceKernelSignalSema(s, 0));
    sema_state(s);
    out("  -1: %08X\n", (unsigned)sceKernelSignalSema(s, -1));
    sema_state(s);

    /* The polls use a semaphore of their own, so the counts they start
     * from do not depend on what the signals above did. */
    step("sema: CreateSema init 3 max 3; PollSema 2");
    SceUID sp = sceKernelCreateSema("poll", 0, 3, 3, NULL);
    uidret(sp);
    out("  = %08X\n", (unsigned)sceKernelPollSema(sp, 2));
    sema_state(sp);

    step("sema: PollSema 2 and 4 (above max) on a count of 1");
    out("  2: %08X\n", (unsigned)sceKernelPollSema(sp, 2));
    out("  4: %08X\n", (unsigned)sceKernelPollSema(sp, 4));
    sema_state(sp);

    /* threadman.c:1202-1204 -- polling for 0 while signalled is
     * ILLEGAL_COUNT (800201BD). */
    step("sema: PollSema 0 and -1 while signalled");
    out("  0: %08X\n", (unsigned)sceKernelPollSema(sp, 0));
    out("  -1: %08X\n", (unsigned)sceKernelPollSema(sp, -1));
    sema_state(sp);

    step("sema: PollSema 1, three times");
    for (int i = 0; i < 3; i++) out("  %08X", (unsigned)sceKernelPollSema(sp, 1));
    out("\n");
    sema_state(sp);

    /* threadman.c:1202-1203 -- empty: SEMA_ZERO (800201AD) whatever it is
     * asked for, 0 and negative included. */
    step("sema: PollSema 0, 1, -1 while empty");
    out("  0: %08X\n", (unsigned)sceKernelPollSema(sp, 0));
    out("  1: %08X\n", (unsigned)sceKernelPollSema(sp, 1));
    out("  -1: %08X\n", (unsigned)sceKernelPollSema(sp, -1));
    sceKernelDeleteSema(sp);

    /* threadman.c:1206-1208 -- the count is checked before the uid:
     * PollSema(NULL, 0) is ILLEGAL_COUNT, PollSema(NULL, 1) UNKNOWN_SEMID. */
    step("sema: PollSema uid 0 with 0 and with 1");
    out("  0: %08X\n", (unsigned)sceKernelPollSema(0, 0));
    out("  1: %08X\n", (unsigned)sceKernelPollSema(0, 1));

    /* threadman.c:1325-1330 -- WaitSema looks the uid up before the count. */
    step("sema: WaitSema uid 0 with 0 and 1, SignalSema uid 0, timeout 1ms");
    tmo = 1000; out("  wait 0: %08X\n", (unsigned)sceKernelWaitSema(0, 0, &tmo));
    tmo = 1000; out("  wait 1: %08X\n", (unsigned)sceKernelWaitSema(0, 1, &tmo));
    out("  signal: %08X\n", (unsigned)sceKernelSignalSema(0, 1));

    /* threadman.c:1636-1638 -- a size field of 0 gets nothing written, and
     * psprecomp writes all 56 bytes for any other size. A fresh semaphore,
     * init 2 max 3, so every field is known. */
    SceUID sr = sceKernelCreateSema("refer", 0, 2, 3, NULL);
    step("sema: ReferSemaStatus with size field 0 (init 2 max 3)");
    info_prep(0);
    out("  = %08X\n", (unsigned)sceKernelReferSemaStatus(sr, INFO));
    show_info(SEMA_FIELDS);

    step("sema: ReferSemaStatus with size field 4");
    info_prep(4);
    out("  = %08X\n", (unsigned)sceKernelReferSemaStatus(sr, INFO));
    show_info(SEMA_FIELDS);

    /* threadman.c:1631-1646 -- 56 bytes: size, name, attr, init, cur, max,
     * waiters. */
    step("sema: ReferSemaStatus with size field 56");
    info_prep(56);
    out("  = %08X\n", (unsigned)sceKernelReferSemaStatus(sr, INFO));
    show_info(SEMA_FIELDS);

    step("sema: ReferSemaStatus with size field 100");
    info_prep(100);
    out("  = %08X\n", (unsigned)sceKernelReferSemaStatus(sr, INFO));
    show_info(SEMA_FIELDS);
    sceKernelDeleteSema(sr);

    /* hle.h:177-181 -- the uid space is per type, and a wrong-typed handle
     * is refused with the asking type's code (UNKNOWN_SEMID 80020199). */
    step("sema: SignalSema, PollSema, ReferSemaStatus on an event flag's uid");
    {
        SceUID f = sceKernelCreateEventFlag("notsema", 0, 0, NULL);
        out("  signal: %08X\n", (unsigned)sceKernelSignalSema(f, 1));
        out("  poll: %08X\n", (unsigned)sceKernelPollSema(f, 1));
        info_prep(56);
        out("  refer: %08X\n", (unsigned)sceKernelReferSemaStatus(f, INFO));
        sceKernelDeleteEventFlag(f);
    }

    step("sema: WaitSemaCB 1 after SignalSema 1, timeout 500ms");
    sceKernelSignalSema(s, 1);
    tmo = 500000; r = sceKernelWaitSemaCB(s, 1, &tmo);
    out("  = %08X tmo=%s\n", (unsigned)r, tcls(tmo, 500000));

    step("sema: DeleteSema, then every call on the deleted uid");
    out("  delete: %08X\n", (unsigned)sceKernelDeleteSema(s));
    out("  delete again: %08X\n", (unsigned)sceKernelDeleteSema(s));
    out("  signal: %08X\n", (unsigned)sceKernelSignalSema(s, 1));
    out("  poll: %08X\n", (unsigned)sceKernelPollSema(s, 1));
    tmo = 1000; out("  wait: %08X\n", (unsigned)sceKernelWaitSema(s, 1, &tmo));
    info_prep(56);
    out("  refer: %08X\n", (unsigned)sceKernelReferSemaStatus(s, INFO));
    out("  cancel: %08X\n", (unsigned)sceKernelCancelSema(s, 0, NULL));
}

/* Four waiters wanting 1 each, released by four signals of 1. */
static void sema_order(w32 attr) {
    order_reset();
    step("sema: attr %X, 4 waiters for 1 queue A(16) B(13) C(18) D(14)", (unsigned)attr);
    SceUID s = sceKernelCreateSema("order", attr, 0, 10, NULL);
    uidret(s);
    for (int i = 0; i < 4; i++) spawn(job(i, jf_waitsema, s, 1, 0), ARRIVE[i]);
    sema_state(s);

    step("sema: SignalSema 1, four times, settling after each");
    for (int i = 0; i < 4; i++) {
        out("  signal: %08X\n", (unsigned)sceKernelSignalSema(s, 1));
        settle();
    }
    show_jobs(4, 0);
    sema_state(s);
    sceKernelDeleteSema(s);
    reap_all();
}

static void sec_sema_threads(void) {
    section("semaphore: waiters");
    int r, n;
    SceUID s;

    /* waitq.h:3-9 -- bit 0x100 selects most-urgent-first, clear is
     * first-come; waitq.c:28-43 -- ties keep arrival order. */
    sema_order(0);
    sema_order(0x100);

    /* threadman.c:1270-1283 -- threads/semaphores/fifo: the head blocks the
     * queue. With a count of 1, A asking for 5 parks; B asking for 1 parks
     * behind it rather than taking what is there; only when A times out does
     * B get it. */
    order_reset();
    step("sema: FIFO, count 1; A(15) waits for 5 with a 100ms timeout, then B(14) for 1");
    s = sceKernelCreateSema("hol", 0, 1, 10, NULL);
    job_t *ja = job(0, jf_waitsema, s, 5, 0);
    ja->tmo = ja->tmo_req = 100000;
    spawn(ja, 0x15);
    spawn(job(1, jf_waitsema, s, 1, 0), 0x14);
    sema_state(s);
    step("sema: main sleeps 300ms so A's timeout runs out");
    sceKernelDelayThread(300000);
    show_jobs(2, 0);
    sema_state(s);
    sceKernelDeleteSema(s);
    reap_all();

    /* threadman.c:1263-1283 via waitq.c:28-43 -- with priority order the
     * head is the most urgent waiter, and psprecomp stops at it when it
     * cannot be satisfied, so the less urgent B is not served either. */
    order_reset();
    step("sema: priority, count 1; A(14) waits for 5, B(16) waits for 1");
    s = sceKernelCreateSema("prihol", 0x100, 1, 10, NULL);
    spawn(job(0, jf_waitsema, s, 5, 0), 0x14);
    spawn(job(1, jf_waitsema, s, 1, 0), 0x16);
    sema_state(s);
    show_order();
    step("sema: SignalSema 4 (count 5), then SignalSema 1");
    out("  signal 4: %08X\n", (unsigned)sceKernelSignalSema(s, 4));
    settle();
    sema_state(s);
    out("  signal 1: %08X\n", (unsigned)sceKernelSignalSema(s, 1));
    settle();
    show_jobs(2, 0);
    sema_state(s);
    sceKernelDeleteSema(s);
    reap_all();

    /* threadman.c:1284-1289 -- semaphores/signal: +2 on an idle 0/1
     * semaphore is refused, the same +2 with a thread queued for 1 is
     * allowed because the waiter takes one of them first. */
    order_reset();
    step("sema: max 1 with A waiting for 1: SignalSema 2; then idle max 1: SignalSema 2");
    s = sceKernelCreateSema("absorb", 0, 0, 1, NULL);
    spawn(job(0, jf_waitsema, s, 1, 0), 0x18);
    r = sceKernelSignalSema(s, 2);
    settle();
    out("  with waiter: %08X\n", (unsigned)r);
    show_jobs(1, 0);
    sema_state(s);
    sceKernelDeleteSema(s);
    reap_all();
    s = sceKernelCreateSema("idle", 0, 0, 1, NULL);
    out("  idle: %08X\n", (unsigned)sceKernelSignalSema(s, 2));
    sema_state(s);
    sceKernelDeleteSema(s);

    /* threadman.c:1303-1314 -- a waiter wanting more than is there stays
     * queued while the count builds up, and is released when it suffices. */
    order_reset();
    step("sema: max 5, A waits for 3; SignalSema 1 twice, then 1 more");
    s = sceKernelCreateSema("build", 0, 0, 5, NULL);
    spawn(job(0, jf_waitsema, s, 3, 0), 0x18);
    sceKernelSignalSema(s, 1);
    sceKernelSignalSema(s, 1);
    settle();
    sema_state(s);
    show_order();
    out("  third: %08X\n", (unsigned)sceKernelSignalSema(s, 1));
    settle();
    show_jobs(1, 0);
    sema_state(s);
    sceKernelDeleteSema(s);
    reap_all();

    /* threadman.c:1210-1213 -- psprecomp's least certain rule: at count 0
     * *with a waiter*, PollSema(0) is ILLEGAL_COUNT rather than SEMA_ZERO.
     * threadman.c:1214-1227 -- and a poll takes what is there even with a
     * waiter queued ahead of it. */
    order_reset();
    step("sema: count 0 with A waiting for 2: PollSema 0, 1, 2");
    s = sceKernelCreateSema("pollw", 0, 0, 5, NULL);
    spawn(job(0, jf_waitsema, s, 2, 0), 0x18);
    out("  0: %08X\n", (unsigned)sceKernelPollSema(s, 0));
    out("  1: %08X\n", (unsigned)sceKernelPollSema(s, 1));
    out("  2: %08X\n", (unsigned)sceKernelPollSema(s, 2));
    step("sema: SignalSema 1 (count 1, A still short), then PollSema 1");
    sceKernelSignalSema(s, 1);
    settle();
    sema_state(s);
    out("  poll 1: %08X\n", (unsigned)sceKernelPollSema(s, 1));
    sema_state(s);

    /* threadman.c:1334-1341 -- a WaitSema that could be satisfied at once
     * still queues behind an earlier waiter. */
    step("sema: SignalSema 1 again, then main WaitSema 1 behind A, timeout 10ms");
    sceKernelSignalSema(s, 1);
    SceUInt tmo = 10000;
    r = sceKernelWaitSema(s, 1, &tmo);
    out("  = %08X tmo=%s\n", (unsigned)r, tcls(tmo, 10000));
    sema_state(s);
    show_jobs(1, 0);
    sceKernelDeleteSema(s);
    reap_all();

    /* hle.h:185-187, threadman.c:1353-1358 -- waiters of a deleted
     * semaphore get WAIT_DELETE (800201B5). Equal priorities, so the order
     * is the order the firmware released them in. */
    order_reset();
    step("sema: DeleteSema with three waiters A B C, all priority 18");
    s = sceKernelCreateSema("del", 0, 0, 5, NULL);
    for (int i = 0; i < 3; i++) spawn(job(i, jf_waitsema, s, 1, 0), 0x18);
    out("  = %08X\n", (unsigned)sceKernelDeleteSema(s));
    settle();
    show_jobs(3, 0);
    reap_all();

    /* hle.h:137-142, waitq.h:61-68 -- every object's Cancel wakes its
     * waiters with WAIT_CANCEL (800201A9). threadman.c:2194-2218 registers
     * no sceKernelCancelSema, so psprecomp answers 0 and releases no one. */
    order_reset();
    step("sema: CancelSema(new count 2) with three waiters A B C (priority 18), max 5");
    s = sceKernelCreateSema("cancel", 0, 0, 5, NULL);
    for (int i = 0; i < 3; i++) spawn(job(i, jf_waitsema, s, 1, 0), 0x18);
    n = 99;
    r = sceKernelCancelSema(s, 2, &n);
    settle();
    out("  = %08X waiters=%d\n", (unsigned)r, n);
    show_jobs(3, 0);
    sema_state(s);
    step("sema: CancelSema with new count -1, 0, 6 (above max), and a NULL count pointer");
    n = 99; r = sceKernelCancelSema(s, -1, &n);
    out("  -1: %08X waiters=%d", (unsigned)r, n); sema_state(s);
    n = 99; r = sceKernelCancelSema(s, 0, &n);
    out("  0: %08X waiters=%d", (unsigned)r, n); sema_state(s);
    n = 99; r = sceKernelCancelSema(s, 6, &n);
    out("  6: %08X waiters=%d", (unsigned)r, n); sema_state(s);
    r = sceKernelCancelSema(s, 1, NULL);
    out("  1, NULL: %08X", (unsigned)r); sema_state(s);
    sceKernelDeleteSema(s);
    reap_all();
}

/* ======================================================================== */
/* Event flags                                                              */
/* ======================================================================== */

static void sec_evf_create(void) {
    section("event flag: create");

    /* threadman.c:1106-1116 -- events/create: 0, 1, 0x10, 0x200 and 0x222
     * accepted; 0x100, 0x122, 0x300, 0x900 and 0x1200 refused. */
    step("evf: CreateEventFlag attr sweep");
    static const w32 attrs[] = { 0x0, 0x1, 0x10, 0x100, 0x122, 0x200, 0x222,
                                 0x2FF, 0x300, 0x400, 0x900, 0x1200 };
    for (unsigned i = 0; i < sizeof attrs / sizeof attrs[0]; i++) {
        SceUID f = sceKernelCreateEventFlag("attr", attrs[i], 0, NULL);
        out("  attr %04X: %s\n", (unsigned)attrs[i], uidstr(f));
        if (f > 0) sceKernelDeleteEventFlag(f);
    }

    /* threadman.c:1703-1718 -- 52 bytes: size, name, attr, init, cur,
     * waiters. */
    step("evf: CreateEventFlag attr 200 init 12345678, ReferEventFlagStatus");
    {
        SceUID f = sceKernelCreateEventFlag("init", 0x200, 0x12345678, NULL);
        uidret(f);
        info_prep(52);
        out("  refer: %08X\n", (unsigned)sceKernelReferEventFlagStatus(f, INFO));
        show_info(EVF_FIELDS);
        info_prep(0);
        out("  refer size 0: %08X\n", (unsigned)sceKernelReferEventFlagStatus(f, INFO));
        show_info(EVF_FIELDS);
        sceKernelDeleteEventFlag(f);
    }

    /* threadman.c:1080-1087 -- a NULL name is ERROR (80020001). */
    step("evf: CreateEventFlag with NULL name");
    {
        SceUID f = sceKernelCreateEventFlag(NULL, 0, 0, NULL);
        uidret(f);
        if (f > 0) sceKernelDeleteEventFlag(f);
    }
}

static void poll_line(const char *what, SceUID f, w32 bits, w32 mode) {
    u32 o = 0xDEADBEEF;
    int r = sceKernelPollEventFlag(f, bits, mode, &o);
    out("  %s: %08X out=%08X\n", what, (unsigned)r, (unsigned)o);
}

static void sec_evf_ops(void) {
    section("event flag: set, clear, poll, wait (one thread)");
    SceUInt tmo;
    u32 o;
    int r;

    step("evf: CreateEventFlag attr 0 init 0000FFFF");
    SceUID f = sceKernelCreateEventFlag("ops", 0, 0x0000FFFF, NULL);
    uidret(f);

    /* threadman.c:1650-1662 -- a poll that does not match still writes the
     * current pattern ("Failed (800201AF, bits=0000FFFF)"). */
    step("evf: PollEventFlag FFFFFFFF AND, 1 OR, 10000 OR");
    poll_line("FFFFFFFF and", f, 0xFFFFFFFF, 0);
    poll_line("1 or", f, 1, WAITOR);
    poll_line("10000 or", f, 0x10000, WAITOR);

    /* threadman.c:1435-1444 -- only WAITOR, WAITCLEAR and WAITCLEARALL, and
     * not both clears: 0x02 0x04 0x08 0x40 0x80 0xFF and 0x30 are
     * ILLEGAL_MODE (80020195), out word untouched. */
    step("evf: PollEventFlag mode sweep for bit 0");
    static const w32 modes[] = { 0x02, 0x04, 0x08, 0x10, 0x11, 0x20, 0x21,
                                 0x30, 0x31, 0x40, 0x80, 0xFF };
    for (unsigned i = 0; i < sizeof modes / sizeof modes[0]; i++) {
        char what[16];
        snprintf(what, sizeof what, "mode %02X", (unsigned)modes[i]);
        sceKernelSetEventFlag(f, 0x0000FFFF);
        poll_line(what, f, 1, modes[i]);
    }

    /* threadman.c:1531-1541 -- the mode, then the pattern, then the uid:
     * no bits is EVF_ILPAT (800201B1) even on a bad uid, and no bits with a
     * bad mode is ILLEGAL_MODE. */
    step("evf: PollEventFlag with no bits: good uid, uid 0, bad mode; uid 0 with bits");
    poll_line("bits 0", f, 0, WAITOR);
    poll_line("bits 0 uid 0", 0, 0, WAITOR);
    poll_line("bits 0 mode 04", f, 0, 0x04);
    poll_line("bits 1 uid 0", 0, 1, WAITOR);

    /* threadman.c:1391-1397 -- events/poll: from FFFFFFFF, CLEAR|OR for
     * bit 0 leaves FFFFFFFE, CLEARALL|OR leaves 00000000. */
    step("evf: from FFFFFFFF, poll bit 0 OR|CLEAR, then OR|CLEARALL");
    sceKernelSetEventFlag(f, 0xFFFFFFFF);
    poll_line("or|clear", f, 1, WAITOR | WAITCLEAR);
    evf_state(f);
    sceKernelSetEventFlag(f, 0xFFFFFFFF);
    poll_line("or|clearall", f, 1, WAITOR | WAITCLEARALL);
    evf_state(f);

    step("evf: pattern 0F, poll 03 AND|CLEAR, then 30 AND|CLEAR, then NULL out");
    sceKernelClearEventFlag(f, 0);
    sceKernelSetEventFlag(f, 0x0F);
    poll_line("03 and|clear", f, 0x03, WAITCLEAR);
    evf_state(f);
    poll_line("30 and|clear", f, 0x30, WAITCLEAR);
    evf_state(f);
    out("  NULL out: %08X\n", (unsigned)sceKernelPollEventFlag(f, 0x04, WAITOR, NULL));

    /* threadman.c:1504-1505 -- the argument is the bits to KEEP. */
    step("evf: SetEventFlag FF, ClearEventFlag F0, SetEventFlag 0");
    out("  set FF: %08X\n", (unsigned)sceKernelSetEventFlag(f, 0xFF));
    out("  clear F0: %08X\n", (unsigned)sceKernelClearEventFlag(f, 0xF0));
    evf_state(f);
    out("  set 0: %08X\n", (unsigned)sceKernelSetEventFlag(f, 0));
    evf_state(f);

    /* threadman.c:1600-1605 -- a zero-timeout wait that fails leaves the out
     * word untouched (DEADBEEF); one that ran out writes the pattern. */
    step("evf: WaitEventFlag for bit 0, pattern F0, timeout 0");
    o = 0xDEADBEEF; tmo = 0;
    r = sceKernelWaitEventFlag(f, 1, WAITOR, &o, &tmo);
    out("  = %08X out=%08X tmo=%08X\n", (unsigned)r, (unsigned)o, (unsigned)tmo);

    step("evf: WaitEventFlag for bit 0, pattern F0, timeout 5ms");
    o = 0xDEADBEEF; tmo = 5000;
    r = sceKernelWaitEventFlag(f, 1, WAITOR, &o, &tmo);
    out("  = %08X out=%08X tmo=%s\n", (unsigned)r, (unsigned)o, tcls(tmo, 5000));

    step("evf: WaitEventFlag for 30 AND|CLEAR, pattern F0, timeout 500ms");
    o = 0xDEADBEEF; tmo = 500000;
    r = sceKernelWaitEventFlag(f, 0x30, WAITCLEAR, &o, &tmo);
    out("  = %08X out=%08X tmo=%s\n", (unsigned)r, (unsigned)o, tcls(tmo, 500000));
    evf_state(f);

    /* threadman.c:1531-1541 -- argument errors in the same order as poll,
     * and the timeout word left alone. */
    step("evf: WaitEventFlag bad mode 04, bits 0, bits 0 on uid 0, bits 1 on uid 0");
    o = 0xDEADBEEF; tmo = 500000;
    r = sceKernelWaitEventFlag(f, 1, 0x04, &o, &tmo);
    out("  mode 04: %08X out=%08X tmo=%s\n", (unsigned)r, (unsigned)o, tcls(tmo, 500000));
    o = 0xDEADBEEF; tmo = 500000;
    r = sceKernelWaitEventFlag(f, 0, WAITOR, &o, &tmo);
    out("  bits 0: %08X out=%08X tmo=%s\n", (unsigned)r, (unsigned)o, tcls(tmo, 500000));
    o = 0xDEADBEEF; tmo = 500000;
    r = sceKernelWaitEventFlag(0, 0, WAITOR, &o, &tmo);
    out("  bits 0 uid 0: %08X out=%08X tmo=%s\n", (unsigned)r, (unsigned)o, tcls(tmo, 500000));
    o = 0xDEADBEEF; tmo = 500000;
    r = sceKernelWaitEventFlag(0, 0, 0x04, &o, &tmo);
    out("  bits 0 uid 0 mode 04: %08X out=%08X\n", (unsigned)r, (unsigned)o);
    o = 0xDEADBEEF; tmo = 500000;
    r = sceKernelWaitEventFlag(0, 1, WAITOR, &o, &tmo);
    out("  bits 1 uid 0: %08X out=%08X tmo=%s\n", (unsigned)r, (unsigned)o, tcls(tmo, 500000));

    /* hle.h:177-181 -- wrong-typed uid: UNKNOWN_EVFID (8002019A). */
    step("evf: SetEventFlag, PollEventFlag, ReferEventFlagStatus on a semaphore's uid");
    {
        SceUID s = sceKernelCreateSema("notevf", 0, 0, 1, NULL);
        out("  set: %08X\n", (unsigned)sceKernelSetEventFlag(s, 1));
        poll_line("poll", s, 1, WAITOR);
        info_prep(52);
        out("  refer: %08X\n", (unsigned)sceKernelReferEventFlagStatus(s, INFO));
        sceKernelDeleteSema(s);
    }

    step("evf: DeleteEventFlag, then every call on the deleted uid");
    out("  delete: %08X\n", (unsigned)sceKernelDeleteEventFlag(f));
    out("  delete again: %08X\n", (unsigned)sceKernelDeleteEventFlag(f));
    out("  set: %08X\n", (unsigned)sceKernelSetEventFlag(f, 1));
    out("  clear: %08X\n", (unsigned)sceKernelClearEventFlag(f, 0));
    poll_line("poll", f, 1, WAITOR);
    o = 0xDEADBEEF; tmo = 1000;
    r = sceKernelWaitEventFlag(f, 1, WAITOR, &o, &tmo);
    out("  wait: %08X out=%08X\n", (unsigned)r, (unsigned)o);
    out("  cancel: %08X\n", (unsigned)sceKernelCancelEventFlag(f, 0, NULL));
}

static void sec_evf_threads(void) {
    section("event flag: waiters");
    SceUID f;
    int r, n;
    u32 o;
    SceUInt tmo;

    /* threadman.c:1560-1566 -- without WAITMULTIPLE (0x200) a second waiter
     * is refused with EVF_MULTI (800201B0); threadman.c:1689-1694 -- and so
     * is a poll while someone waits. */
    order_reset();
    step("evf: attr 0, A waits bit 1; then B waits bit 2, main polls and waits for bit 2");
    f = sceKernelCreateEventFlag("single", 0, 0, NULL);
    spawn(job(0, jf_waitevf, f, 1, WAITOR), 0x18);
    spawn(job(1, jf_waitevf, f, 2, WAITOR), 0x17);
    poll_line("main poll 2", f, 2, WAITOR);
    o = 0xDEADBEEF; tmo = 1000;
    r = sceKernelWaitEventFlag(f, 2, WAITOR, &o, &tmo);
    out("  main wait 2: %08X out=%08X tmo=%s\n", (unsigned)r, (unsigned)o, tcls(tmo, 1000));
    evf_state(f);
    step("evf: SetEventFlag 1");
    out("  = %08X\n", (unsigned)sceKernelSetEventFlag(f, 1));
    settle();
    show_jobs(2, 'x');
    evf_state(f);
    sceKernelDeleteEventFlag(f);
    reap_all();

    /* threadman.c:1456-1466 -- with WAITCLEAR, who is considered first gets
     * the bit; waitq.h:3-9 -- in arrival order for an event flag (0x100 is
     * not a legal event flag attribute, threadman.c:1106-1116). */
    order_reset();
    step("evf: attr 200, A(16) B(13) C(18) wait bit 1 OR|CLEAR; SetEventFlag 1 three times");
    f = sceKernelCreateEventFlag("fifo", 0x200, 0, NULL);
    for (int i = 0; i < 3; i++) spawn(job(i, jf_waitevf, f, 1, WAITOR | WAITCLEAR), ARRIVE[i]);
    evf_state(f);
    for (int i = 0; i < 3; i++) {
        out("  set: %08X\n", (unsigned)sceKernelSetEventFlag(f, 1));
        settle();
        evf_state(f);
    }
    show_jobs(3, 'x');
    sceKernelDeleteEventFlag(f);
    reap_all();

    /* threadman.c:1467-1489 -- one set releases every waiter it satisfies,
     * each told the pattern at its release. Equal priorities, so the run
     * order is the release order. */
    order_reset();
    step("evf: attr 200, prio 18: A 1 AND, B 2 AND, C 3 AND, D 4 OR; SetEventFlag 3, then 4");
    f = sceKernelCreateEventFlag("multi", 0x200, 0, NULL);
    spawn(job(0, jf_waitevf, f, 1, 0), 0x18);
    spawn(job(1, jf_waitevf, f, 2, 0), 0x18);
    spawn(job(2, jf_waitevf, f, 3, 0), 0x18);
    spawn(job(3, jf_waitevf, f, 4, WAITOR), 0x18);
    out("  set 3: %08X\n", (unsigned)sceKernelSetEventFlag(f, 3));
    settle();
    evf_state(f);
    show_order();
    out("  set 4: %08X\n", (unsigned)sceKernelSetEventFlag(f, 4));
    settle();
    show_jobs(4, 'x');
    evf_state(f);
    sceKernelDeleteEventFlag(f);
    reap_all();

    /* threadman.c:1456-1460 -- no head-of-line blocking: a waiter that
     * cannot be satisfied does not hold up one behind it that can. */
    order_reset();
    step("evf: attr 200, A waits 3 AND, B waits 1 OR|CLEAR; SetEventFlag 1, then 2");
    f = sceKernelCreateEventFlag("hol", 0x200, 0, NULL);
    spawn(job(0, jf_waitevf, f, 3, 0), 0x18);
    spawn(job(1, jf_waitevf, f, 1, WAITOR | WAITCLEAR), 0x18);
    sceKernelSetEventFlag(f, 1);
    settle();
    evf_state(f);
    show_order();
    sceKernelSetEventFlag(f, 3);
    settle();
    show_jobs(2, 'x');
    evf_state(f);
    sceKernelDeleteEventFlag(f);
    reap_all();

    /* threadman.c:1467-1489 -- a released waiter's clear happens before the
     * next waiter is considered, so a CLEAR ahead of a plain waiter starves
     * it and a plain waiter ahead of a CLEAR does not. */
    order_reset();
    step("evf: attr 200, A waits 1 OR|CLEAR then B waits 1 OR; SetEventFlag 1");
    f = sceKernelCreateEventFlag("clr1", 0x200, 0, NULL);
    spawn(job(0, jf_waitevf, f, 1, WAITOR | WAITCLEAR), 0x18);
    spawn(job(1, jf_waitevf, f, 1, WAITOR), 0x18);
    sceKernelSetEventFlag(f, 1);
    settle();
    show_jobs(2, 'x');
    evf_state(f);
    sceKernelDeleteEventFlag(f);
    reap_all();

    order_reset();
    step("evf: attr 200, A waits 1 OR then B waits 1 OR|CLEAR; SetEventFlag 1");
    f = sceKernelCreateEventFlag("clr2", 0x200, 0, NULL);
    spawn(job(0, jf_waitevf, f, 1, WAITOR), 0x18);
    spawn(job(1, jf_waitevf, f, 1, WAITOR | WAITCLEAR), 0x18);
    sceKernelSetEventFlag(f, 1);
    settle();
    show_jobs(2, 'x');
    evf_state(f);
    sceKernelDeleteEventFlag(f);
    reap_all();

    order_reset();
    step("evf: A waits 1 OR|CLEARALL; SetEventFlag F1");
    f = sceKernelCreateEventFlag("clrall", 0, 0, NULL);
    spawn(job(0, jf_waitevf, f, 1, WAITOR | WAITCLEARALL), 0x18);
    sceKernelSetEventFlag(f, 0xF1);
    settle();
    show_jobs(1, 'x');
    evf_state(f);
    sceKernelDeleteEventFlag(f);
    reap_all();

    /* threadman.c:1578-1590 -- a waiter whose flag is deleted gets
     * WAIT_DELETE (800201B5) and a pattern of 0. */
    order_reset();
    step("evf: DeleteEventFlag with waiters A B (prio 18) on pattern 0C");
    f = sceKernelCreateEventFlag("del", 0x200, 0x0C, NULL);
    spawn(job(0, jf_waitevf, f, 1, WAITOR), 0x18);
    spawn(job(1, jf_waitevf, f, 3, 0), 0x18);
    out("  = %08X\n", (unsigned)sceKernelDeleteEventFlag(f));
    settle();
    show_jobs(2, 'x');
    reap_all();

    /* hle.h:137-142 -- WAIT_CANCEL (800201A9); threadman.c:2194-2218 has no
     * sceKernelCancelEventFlag, so psprecomp answers 0 and releases no one. */
    order_reset();
    step("evf: CancelEventFlag(new pattern 55) with waiters A B (prio 18) on pattern 0C");
    f = sceKernelCreateEventFlag("cancel", 0x200, 0x0C, NULL);
    spawn(job(0, jf_waitevf, f, 1, WAITOR), 0x18);
    spawn(job(1, jf_waitevf, f, 3, 0), 0x18);
    n = 99;
    r = sceKernelCancelEventFlag(f, 0x55, &n);
    settle();
    out("  = %08X waiters=%d\n", (unsigned)r, n);
    show_jobs(2, 'x');
    evf_state(f);
    sceKernelDeleteEventFlag(f);
    reap_all();
}

/* ======================================================================== */
/* Mutexes                                                                  */
/* ======================================================================== */

static void sec_mutex_create(void) {
    section("mutex: create");

    /* kernlock.c:31-38 -- mutex/create: 0x1, 0x100, 0x200, 0x800, 0xB00
     * and 0xBFF accepted; 0x400, 0xC00, 0x1000 ... 0x10000 refused. */
    step("mutex: CreateMutex attr sweep");
    static const w32 attrs[] = { 0x0, 0x1, 0x100, 0x200, 0x400, 0x800, 0xB00,
                                 0xBFF, 0xC00, 0x1000, 0x2000, 0x4000, 0x8000, 0x10000 };
    for (unsigned i = 0; i < sizeof attrs / sizeof attrs[0]; i++) {
        SceUID m = sceKernelCreateMutex("attr", attrs[i], 0, NULL);
        out("  attr %05X: %s\n", (unsigned)attrs[i], uidstr(m));
        if (m > 0) sceKernelDeleteMutex(m);
    }

    /* kernlock.c:88-94 -- created held: init 1 is owned by the creator;
     * more than 1 needs the recursive attribute; negative is ILLEGAL_COUNT. */
    step("mutex: CreateMutex init counts, ReferMutexStatus of each");
    static const int ic[][2] = { {0, 1}, {0, 2}, {0, -1}, {0x200, 2}, {0x200, -1}, {0x200, 0x7FFFFFFF} };
    for (unsigned i = 0; i < sizeof ic / sizeof ic[0]; i++) {
        SceUID m = sceKernelCreateMutex("init", ic[i][0], ic[i][1], NULL);
        out("  attr %03X init %d: %s\n", ic[i][0], ic[i][1], uidstr(m));
        if (m > 0) { mtx_state(m); sceKernelDeleteMutex(m); }
    }

    /* kernlock.c:299-321 -- 56 bytes; kernlock.c:314-320 -- lockThread is
     * -1 when free. */
    step("mutex: ReferMutexStatus of a free mutex (attr 0x100), sizes 56 and 0");
    {
        SceUID m = sceKernelCreateMutex("refer", 0x100, 0, NULL);
        info_prep(56);
        out("  = %08X\n", (unsigned)sceKernelReferMutexStatus(m, INFO));
        show_info(MTX_FIELDS);
        info_prep(0);
        out("  size 0: %08X\n", (unsigned)sceKernelReferMutexStatus(m, INFO));
        show_info(MTX_FIELDS);
        sceKernelDeleteMutex(m);
    }

    /* kernlock.c:86 -- a NULL name is ERROR (80020001). */
    step("mutex: CreateMutex with NULL name");
    {
        SceUID m = sceKernelCreateMutex(NULL, 0, 0, NULL);
        uidret(m);
        if (m > 0) sceKernelDeleteMutex(m);
    }
}

static void sec_mutex_ops(void) {
    section("mutex: lock, unlock (one thread)");
    SceUInt tmo;
    int r;

    step("mutex: CreateMutex attr 0 init 0");
    SceUID m = sceKernelCreateMutex("ops", 0, 0, NULL);
    uidret(m);

    step("mutex: LockMutex 1, timeout 500ms");
    tmo = 500000; r = sceKernelLockMutex(m, 1, &tmo);
    out("  = %08X tmo=%s\n", (unsigned)r, tcls(tmo, 500000));
    info_prep(56);
    sceKernelReferMutexStatus(m, INFO);
    show_info(MTX_FIELDS);

    /* kernlock.c:152-163 -- relocking without the recursive attribute is
     * MUTEX_RECURSIVE (800201C8), refused before any wait. */
    step("mutex: LockMutex 1 again, timeout 500ms; TryLockMutex 1");
    tmo = 500000; r = sceKernelLockMutex(m, 1, &tmo);
    out("  lock: %08X tmo=%s\n", (unsigned)r, tcls(tmo, 500000));
    out("  trylock: %08X\n", (unsigned)sceKernelTryLockMutex(m, 1));

    /* kernlock.c:65-77 -- 0 or negative, or more than 1 without the
     * recursive attribute: ILLEGAL_COUNT (800201BD). */
    step("mutex: LockMutex 0, 2, -1 and TryLockMutex 0, 2 while held");
    tmo = 500000; out("  lock 0: %08X\n", (unsigned)sceKernelLockMutex(m, 0, &tmo));
    tmo = 500000; out("  lock 2: %08X\n", (unsigned)sceKernelLockMutex(m, 2, &tmo));
    tmo = 500000; out("  lock -1: %08X\n", (unsigned)sceKernelLockMutex(m, -1, &tmo));
    out("  trylock 0: %08X\n", (unsigned)sceKernelTryLockMutex(m, 0));
    out("  trylock 2: %08X\n", (unsigned)sceKernelTryLockMutex(m, 2));

    /* kernlock.c:234-251 -- the lock's count rules, then MUTEX_UNLOCKED
     * (800201C5) when not held. */
    step("mutex: UnlockMutex 2, 0, then 1, then 1 again");
    out("  unlock 2: %08X\n", (unsigned)sceKernelUnlockMutex(m, 2));
    out("  unlock 0: %08X\n", (unsigned)sceKernelUnlockMutex(m, 0));
    out("  unlock 1: %08X\n", (unsigned)sceKernelUnlockMutex(m, 1));
    mtx_state(m);
    out("  unlock 1: %08X\n", (unsigned)sceKernelUnlockMutex(m, 1));

    step("mutex: TryLockMutex 1 on a free mutex, UnlockMutex 1");
    out("  trylock: %08X\n", (unsigned)sceKernelTryLockMutex(m, 1));
    mtx_state(m);
    out("  unlock: %08X\n", (unsigned)sceKernelUnlockMutex(m, 1));
    sceKernelDeleteMutex(m);

    step("mutex: CreateMutex attr 200 (recursive) init 0; LockMutex 1, then 2");
    SceUID rm = sceKernelCreateMutex("rec", 0x200, 0, NULL);
    uidret(rm);
    tmo = 500000; out("  lock 1: %08X\n", (unsigned)sceKernelLockMutex(rm, 1, &tmo));
    tmo = 500000; out("  lock 2: %08X\n", (unsigned)sceKernelLockMutex(rm, 2, &tmo));
    mtx_state(rm);

    /* kernlock.c:251 -- giving back more than is held is
     * MUTEX_UNLOCK_UNDERFLOW (800201C7). */
    step("mutex: UnlockMutex 4 on a count of 3, then 3");
    out("  unlock 4: %08X\n", (unsigned)sceKernelUnlockMutex(rm, 4));
    mtx_state(rm);
    out("  unlock 3: %08X\n", (unsigned)sceKernelUnlockMutex(rm, 3));
    mtx_state(rm);

    /* kernlock.c:65-77 -- `Lock 1 => INT_MAX` is MUTEX_LOCK_OVERFLOW
     * (800201C6) where `Lock 1 => INT_MAX - 1` reaches INT_MAX. */
    step("mutex: recursive, count 1: LockMutex 7FFFFFFF, then 7FFFFFFE, then 1");
    tmo = 500000; sceKernelLockMutex(rm, 1, &tmo);
    tmo = 500000; out("  lock 7FFFFFFF: %08X\n", (unsigned)sceKernelLockMutex(rm, 0x7FFFFFFF, &tmo));
    mtx_state(rm);
    tmo = 500000; out("  lock 7FFFFFFE: %08X\n", (unsigned)sceKernelLockMutex(rm, 0x7FFFFFFE, &tmo));
    mtx_state(rm);
    tmo = 500000; out("  lock 1: %08X\n", (unsigned)sceKernelLockMutex(rm, 1, &tmo));
    out("  trylock 1: %08X\n", (unsigned)sceKernelTryLockMutex(rm, 1));
    out("  unlock 7FFFFFFF: %08X\n", (unsigned)sceKernelUnlockMutex(rm, 0x7FFFFFFF));
    mtx_state(rm);

    /* kernlock.c:262-297 -- CancelMutex re-arms the mutex at the count it is
     * given: 1 -> held by the caller; above the ceiling (3 on a plain
     * mutex) -> ILLEGAL_COUNT with the object and the waiter-count word
     * (seeded 99) untouched; 0 or negative -> free. */
    step("mutex: CancelMutex on a free non-recursive mutex with 1, 3, 0, -1, -3");
    {
        SceUID cm = sceKernelCreateMutex("cancel", 0, 0, NULL);
        static const int cc[] = { 1, 3, 0, -1, -3 };
        for (unsigned i = 0; i < sizeof cc / sizeof cc[0]; i++) {
            int w = 99;
            r = sceKernelCancelMutex(cm, cc[i], &w);
            out("  %d: %08X waiters=%d", cc[i], (unsigned)r, w);
            mtx_state(cm);
        }
        step("mutex: CancelMutex on a recursive mutex with 5, and with a NULL count pointer");
        int w = 99;
        r = sceKernelCancelMutex(rm, 5, &w);
        out("  5: %08X waiters=%d", (unsigned)r, w);
        mtx_state(rm);
        r = sceKernelCancelMutex(rm, 0, NULL);
        out("  0, NULL: %08X", (unsigned)r);
        mtx_state(rm);
        sceKernelDeleteMutex(cm);
    }
    sceKernelDeleteMutex(rm);

    /* kernlock.c:115-116 -- NOT_FOUND_MUTEX (800201C3). */
    step("mutex: every call on uid 0 and on a deleted mutex");
    tmo = 1000;
    out("  lock 0: %08X\n", (unsigned)sceKernelLockMutex(0, 1, &tmo));
    out("  trylock 0: %08X\n", (unsigned)sceKernelTryLockMutex(0, 1));
    out("  unlock 0: %08X\n", (unsigned)sceKernelUnlockMutex(0, 1));
    tmo = 1000;
    out("  lock deleted: %08X\n", (unsigned)sceKernelLockMutex(rm, 1, &tmo));
    out("  unlock deleted: %08X\n", (unsigned)sceKernelUnlockMutex(rm, 1));
    out("  cancel deleted: %08X\n", (unsigned)sceKernelCancelMutex(rm, 0, NULL));
    info_prep(56);
    out("  refer deleted: %08X\n", (unsigned)sceKernelReferMutexStatus(rm, INFO));
    out("  delete deleted: %08X\n", (unsigned)sceKernelDeleteMutex(rm));
    out("  lock count 0 on uid 0: %08X\n", (unsigned)sceKernelTryLockMutex(0, 0));
}

static void mutex_order(w32 attr) {
    order_reset();
    step("mutex: attr %X held by main; A(16) B(13) C(18) D(14) lock 1, each unlocking at once", (unsigned)attr);
    SceUInt tmo = 1000;
    SceUID m = sceKernelCreateMutex("order", attr, 0, NULL);
    sceKernelLockMutex(m, 1, &tmo);
    for (int i = 0; i < 4; i++) {
        job_t *j = job(i, jf_lockmtx, m, 1, 0);
        j->post = pf_unlockmtx;
        spawn(j, ARRIVE[i]);
    }
    mtx_state(m);
    step("mutex: main UnlockMutex 1");
    out("  = %08X\n", (unsigned)sceKernelUnlockMutex(m, 1));
    settle();
    show_jobs(4, 0);
    mtx_state(m);
    sceKernelDeleteMutex(m);
    reap_all();
}

static void sec_mutex_threads(void) {
    section("mutex: owners and waiters");
    SceUID m;
    SceUInt tmo;
    int r, n;

    /* kernlock.c:242-250 -- unlocking a mutex another thread holds is
     * MUTEX_UNLOCKED (800201C5); kernlock.c:183 -- TryLock on it is
     * MUTEX_LOCKED (800201C4). */
    order_reset();
    step("mutex: A locks a free mutex and holds it; main unlocks, trylocks, locks with 5ms");
    gate_open();
    m = sceKernelCreateMutex("owned", 0, 0, NULL);
    {
        job_t *j = job(0, jf_lockmtx, m, 1, 0);
        j->post = pf_hold_unlockmtx;
        spawn(j, 0x18);
    }
    info_prep(56);
    sceKernelReferMutexStatus(m, INFO);
    show_info(MTX_FIELDS);
    out("  unlock: %08X\n", (unsigned)sceKernelUnlockMutex(m, 1));
    out("  trylock: %08X\n", (unsigned)sceKernelTryLockMutex(m, 1));
    tmo = 5000; r = sceKernelLockMutex(m, 1, &tmo);
    out("  lock: %08X tmo=%s\n", (unsigned)r, tcls(tmo, 5000));
    mtx_state(m);
    step("mutex: B(17) locks and waits; A lets go");
    spawn(job(1, jf_lockmtx, m, 1, 0), 0x17);
    mtx_state(m);
    sceKernelSignalSema(g_gate, 1);
    settle();
    show_jobs(2, 0);
    mtx_state(m);
    sceKernelDeleteMutex(m);
    reap_all();

    /* kernlock.c:47 -- a mutex is owned by a thread; what a thread that
     * ends while holding one leaves behind is not modelled anywhere. */
    order_reset();
    step("mutex: A locks a free mutex and exits holding it; main refers, trylocks, unlocks");
    m = sceKernelCreateMutex("orphan", 0, 0, NULL);
    spawn_oneshot(job(0, jf_lockmtx, m, 1, 0), 0x18);
    show_jobs(1, 0);
    info_prep(56);
    sceKernelReferMutexStatus(m, INFO);
    show_info(MTX_FIELDS);
    out("  trylock: %08X\n", (unsigned)sceKernelTryLockMutex(m, 1));
    out("  unlock: %08X\n", (unsigned)sceKernelUnlockMutex(m, 1));
    reap_all();
    step("mutex: after the thread is deleted: refer, trylock");
    mtx_state(m);
    out("  trylock: %08X\n", (unsigned)sceKernelTryLockMutex(m, 1));
    mtx_state(m);
    sceKernelDeleteMutex(m);

    /* waitq.h:3-9 -- first-come without 0x100, most-urgent with it. */
    mutex_order(0);
    mutex_order(0x100);

    /* kernlock.c:123-133 -- the waiter is handed the mutex at the count it
     * asked for. */
    order_reset();
    step("mutex: recursive, held by main; A locks 3 and holds; main unlocks");
    gate_open();
    m = sceKernelCreateMutex("handoff", 0x200, 1, NULL);
    {
        job_t *j = job(0, jf_lockmtx, m, 3, 0);
        j->post = pf_hold_unlockmtx;
        spawn(j, 0x18);
    }
    mtx_state(m);
    out("  unlock: %08X\n", (unsigned)sceKernelUnlockMutex(m, 1));
    settle();
    show_order();
    info_prep(56);
    sceKernelReferMutexStatus(m, INFO);
    show_info(MTX_FIELDS);
    sceKernelSignalSema(g_gate, 1);
    settle();
    show_jobs(1, 0);
    mtx_state(m);
    sceKernelDeleteMutex(m);
    reap_all();

    /* kernlock.c:262-297, hle.h:137-142 -- CancelMutex reports the waiters,
     * wakes them with WAIT_CANCEL and re-arms the mutex for the caller. */
    order_reset();
    step("mutex: A holds; B C (prio 18) wait; main CancelMutex 1");
    gate_open();
    m = sceKernelCreateMutex("cancel", 0, 0, NULL);
    {
        job_t *j = job(0, jf_lockmtx, m, 1, 0);
        j->post = pf_hold_unlockmtx;
        spawn(j, 0x17);
    }
    spawn(job(1, jf_lockmtx, m, 1, 0), 0x18);
    spawn(job(2, jf_lockmtx, m, 1, 0), 0x18);
    mtx_state(m);
    n = 99;
    r = sceKernelCancelMutex(m, 1, &n);
    settle();
    out("  = %08X waiters=%d\n", (unsigned)r, n);
    mtx_state(m);
    step("mutex: A lets go of the cancelled mutex; main unlocks");
    sceKernelSignalSema(g_gate, 1);
    settle();
    show_jobs(3, 0);
    mtx_state(m);
    out("  main unlock: %08X\n", (unsigned)sceKernelUnlockMutex(m, 1));
    mtx_state(m);
    sceKernelDeleteMutex(m);
    reap_all();

    /* kernlock.c:114-121, hle.h:185-187 -- WAIT_DELETE (800201B5). */
    order_reset();
    step("mutex: held by main, A B (prio 18) wait; DeleteMutex");
    m = sceKernelCreateMutex("del", 0, 1, NULL);
    spawn(job(0, jf_lockmtx, m, 1, 0), 0x18);
    spawn(job(1, jf_lockmtx, m, 1, 0), 0x18);
    mtx_state(m);
    out("  = %08X\n", (unsigned)sceKernelDeleteMutex(m));
    settle();
    show_jobs(2, 0);
    reap_all();
}

/* ======================================================================== */
/* LwMutexes                                                                */
/* ======================================================================== */

static SceLwMutexWorkarea g_wa[3] __attribute__((aligned(16)));
static SceLwMutexWorkarea g_forged __attribute__((aligned(16)));
static SceLwMutexWorkarea g_copy __attribute__((aligned(16)));
/* The uid each workarea was last created with, to name uid words by. */
static SceLwMutexWorkarea *const g_wa_all[5] = { &g_wa[0], &g_wa[1], &g_wa[2], &g_forged, &g_copy };
static const char *const g_wa_names[5] = { "wa0", "wa1", "wa2", "forged", "copy" };
static w32 g_wa_uidv[5];

static int wa_idx(const void *w) {
    for (int i = 0; i < 5; i++) if ((const void *)g_wa_all[i] == w) return i;
    return -1;
}
static w32 wa_uid(const SceLwMutexWorkarea *w) { int i = wa_idx(w); return i < 0 ? 0 : g_wa_uidv[i]; }

static const char *wa_name(w32 p) {
    int i = wa_idx((const void *)p);
    if (i >= 0) return g_wa_names[i];
    if (p == 0) return "NULL";
    return "other";
}

/* kernlock.c:381-382 -- count, thread, attr, waiting, uid, then three pad
 * words; kernlock.c:370-372 -- thread is 0 when free. The uid word prints as
 * "uid" when it is the one this workarea was created with, "uid of waN" when
 * it is another workarea's. */
static void show_wa(const char *label, const SceLwMutexWorkarea *w) {
    static char ub[20];
    const w32 *v = (const w32 *)w;
    const char *uid;
    int own = wa_idx(w), other = -1;
    for (int i = 0; i < 5; i++) if (i != own && g_wa_uidv[i] && v[4] == g_wa_uidv[i]) other = i;
    if (v[4] == 0) uid = "0";
    else if (own >= 0 && v[4] == g_wa_uidv[own]) uid = "uid";
    else if (other >= 0) { snprintf(ub, sizeof ub, "uid of %s", g_wa_names[other]); uid = ub; }
    else uid = (int)v[4] > 0 ? "other uid" : hex(v[4]);
    out("  %s: count=%d thread=%s attr=%X wait=%d uid=%s pad=%08X %08X %08X\n",
        label, (int)v[0], tid_name(v[1]), (unsigned)v[2], (int)v[3], uid,
        (unsigned)v[5], (unsigned)v[6], (unsigned)v[7]);
}

static int lw_create(SceLwMutexWorkarea *w, const char *name, w32 attr, int init) {
    memset(w, 0xCD, sizeof *w);
    int r = sceKernelCreateLwMutex(w, name, attr, init, NULL);
    int i = wa_idx(w);
    if (i >= 0) g_wa_uidv[i] = r == 0 ? ((w32 *)w)[4] : 0;
    return r;
}

static void lw_refer(SceLwMutexWorkarea *w) {
    g_ptrname = wa_name;
    g_uidref = wa_uid(w);
    info_prep(64);
    out("  refer: %08X\n", (unsigned)sceKernelReferLwMutexStatus(w, INFO));
    show_info(LW_FIELDS);
}

static void sec_lw_create(void) {
    section("lwmutex: create");

    /* kernlock.c:375-378 -- lwmutex/create: 0x300 and 0x3FF accepted, 0x400
     * and 0x800 refused. kernlock.c:476-487 -- the whole workarea is written,
     * pad words included (it is pre-filled CD here). */
    step("lw: CreateLwMutex attr sweep, workarea after each");
    static const w32 attrs[] = { 0x0, 0x100, 0x200, 0x300, 0x3FF, 0x400, 0x800, 0x1000 };
    for (unsigned i = 0; i < sizeof attrs / sizeof attrs[0]; i++) {
        int r = lw_create(&g_wa[0], "attr", attrs[i], 0);
        out("  attr %04X: %08X\n", (unsigned)attrs[i], (unsigned)r);
        if (r == 0) {
            if (i == 0) show_wa("wa", &g_wa[0]);
            sceKernelDeleteLwMutex(&g_wa[0]);
        } else {
            show_wa("wa", &g_wa[0]);
        }
    }

    /* kernlock.c:457-460 -- same init rules as a mutex; kernlock.c:480 --
     * created held, the workarea names the creator. */
    step("lw: CreateLwMutex init counts, workarea after each");
    static const int ic[][2] = { {0, 1}, {0, 2}, {0, -1}, {0x200, 2}, {0x200, -1} };
    for (unsigned i = 0; i < sizeof ic / sizeof ic[0]; i++) {
        int r = lw_create(&g_wa[0], "init", ic[i][0], ic[i][1]);
        out("  attr %03X init %d: %08X\n", ic[i][0], ic[i][1], (unsigned)r);
        show_wa("wa", &g_wa[0]);
        if (r == 0) sceKernelDeleteLwMutex(&g_wa[0]);
    }

    /* kernlock.c:651-665 -- 64 bytes, and kernlock.c:683-688 -- by uid the
     * same bytes as by workarea. */
    step("lw: ReferLwMutexStatus and ReferLwMutexStatusByID of a free lwmutex (attr 100)");
    lw_create(&g_wa[0], "refer", 0x100, 0);
    lw_refer(&g_wa[0]);
    {
        static w32 by_wa[INFO_WORDS];
        memcpy(by_wa, g_info, sizeof by_wa);
        info_prep(64);
        out("  by id: %08X\n", (unsigned)sceKernelReferLwMutexStatusByID(wa_uid(&g_wa[0]), INFO));
        out("  same bytes as by workarea: %s\n", memcmp(by_wa, g_info, sizeof by_wa) ? "no" : "yes");
        info_prep(0);
        out("  size 0: %08X\n", (unsigned)sceKernelReferLwMutexStatus(&g_wa[0], INFO));
        show_info(LW_FIELDS);
    }
    sceKernelDeleteLwMutex(&g_wa[0]);

    /* kernlock.c:455 -- a NULL name is ERROR (80020001). */
    step("lw: CreateLwMutex with NULL name");
    {
        memset(&g_wa[0], 0xCD, sizeof g_wa[0]);
        int r = sceKernelCreateLwMutex(&g_wa[0], NULL, 0, 0, NULL);
        out("  = %08X\n", (unsigned)r);
        show_wa("wa", &g_wa[0]);
        if (r == 0) sceKernelDeleteLwMutex(&g_wa[0]);
    }
}

static void sec_lw_ops(void) {
    section("lwmutex: lock, unlock (one thread)");
    SceLwMutexWorkarea *w = &g_wa[0];
    SceUInt tmo;
    int r;

    step("lw: CreateLwMutex attr 0 init 0");
    out("  = %08X\n", (unsigned)lw_create(w, "ops", 0, 0));
    show_wa("wa", w);

    step("lw: LockLwMutex 1, timeout 500ms");
    tmo = 500000; r = sceKernelLockLwMutex(w, 1, &tmo);
    out("  = %08X tmo=%s\n", (unsigned)r, tcls(tmo, 500000));
    show_wa("wa", w);
    lw_refer(w);

    /* kernlock.c:561-562 -- LWMUTEX_RECURSIVE (800201CF); kernlock.c:520-526
     * -- the old TryLock answers 800201C4 for every failure, the _600 one
     * the specific code. */
    step("lw: LockLwMutex 1 again (timeout 500ms), TryLockLwMutex 1, TryLockLwMutex_600 1");
    tmo = 500000; r = sceKernelLockLwMutex(w, 1, &tmo);
    out("  lock: %08X tmo=%s\n", (unsigned)r, tcls(tmo, 500000));
    out("  try: %08X\n", (unsigned)sceKernelTryLockLwMutex(w, 1));
    out("  try600: %08X\n", (unsigned)sceKernelTryLockLwMutex_600(w, 1));

    /* kernlock.c:509-518 -- ILLEGAL_COUNT (800201BD) for 0, negative, or
     * above 1 without the recursive attribute. */
    step("lw: counts 0, 2, -1 through Lock, TryLock and TryLock_600 while held");
    static const int cc[] = { 0, 2, -1 };
    for (int i = 0; i < 3; i++) {
        tmo = 500000;
        out("  %d: lock %08X", cc[i], (unsigned)sceKernelLockLwMutex(w, cc[i], &tmo));
        out(" try %08X", (unsigned)sceKernelTryLockLwMutex(w, cc[i]));
        out(" try600 %08X\n", (unsigned)sceKernelTryLockLwMutex_600(w, cc[i]));
    }

    /* kernlock.c:618-627 -- LWMUTEX_UNLOCKED (800201CC) when not held. */
    step("lw: UnlockLwMutex 2, 0, then 1, then 1 again");
    out("  unlock 2: %08X\n", (unsigned)sceKernelUnlockLwMutex(w, 2));
    out("  unlock 0: %08X\n", (unsigned)sceKernelUnlockLwMutex(w, 0));
    out("  unlock 1: %08X\n", (unsigned)sceKernelUnlockLwMutex(w, 1));
    show_wa("wa", w);
    out("  unlock 1: %08X\n", (unsigned)sceKernelUnlockLwMutex(w, 1));
    lw_refer(w);

    step("lw: TryLockLwMutex 1 and TryLockLwMutex_600 1 on a free lwmutex");
    out("  try: %08X\n", (unsigned)sceKernelTryLockLwMutex(w, 1));
    show_wa("wa", w);
    out("  unlock: %08X\n", (unsigned)sceKernelUnlockLwMutex(w, 1));
    out("  try600: %08X\n", (unsigned)sceKernelTryLockLwMutex_600(w, 1));
    show_wa("wa", w);
    out("  unlock: %08X\n", (unsigned)sceKernelUnlockLwMutex(w, 1));

    SceLwMutexWorkarea *rw = &g_wa[1];
    step("lw: CreateLwMutex attr 200 (recursive); LockLwMutex 1, then 2");
    out("  = %08X\n", (unsigned)lw_create(rw, "rec", 0x200, 0));
    tmo = 500000; out("  lock 1: %08X\n", (unsigned)sceKernelLockLwMutex(rw, 1, &tmo));
    tmo = 500000; out("  lock 2: %08X\n", (unsigned)sceKernelLockLwMutex(rw, 2, &tmo));
    show_wa("wa", rw);

    /* kernlock.c:627 -- LWMUTEX_UNLOCK_UNDERFLOW (800201CE). */
    step("lw: UnlockLwMutex 4 on a count of 3, then 3");
    out("  unlock 4: %08X\n", (unsigned)sceKernelUnlockLwMutex(rw, 4));
    out("  unlock 3: %08X\n", (unsigned)sceKernelUnlockLwMutex(rw, 3));
    show_wa("wa", rw);

    /* kernlock.c:515-516 -- LWMUTEX_LOCK_OVERFLOW (800201CD). */
    step("lw: recursive, count 1: LockLwMutex 7FFFFFFF, then 7FFFFFFE; TryLock 1 both kinds");
    tmo = 500000; sceKernelLockLwMutex(rw, 1, &tmo);
    tmo = 500000; out("  lock 7FFFFFFF: %08X\n", (unsigned)sceKernelLockLwMutex(rw, 0x7FFFFFFF, &tmo));
    tmo = 500000; out("  lock 7FFFFFFE: %08X\n", (unsigned)sceKernelLockLwMutex(rw, 0x7FFFFFFE, &tmo));
    show_wa("wa", rw);
    out("  try: %08X\n", (unsigned)sceKernelTryLockLwMutex(rw, 1));
    out("  try600: %08X\n", (unsigned)sceKernelTryLockLwMutex_600(rw, 1));
    out("  unlock 7FFFFFFF: %08X\n", (unsigned)sceKernelUnlockLwMutex(rw, 0x7FFFFFFF));
    show_wa("wa", rw);
    sceKernelDeleteLwMutex(rw);

    /* kernlock.c:500-503 -- the uid stays in the workarea after delete;
     * kernlock.c:421-422 -- so a lock on it is NOT_FOUND (800201CA), not
     * RECURSIVE. */
    step("lw: DeleteLwMutex, then every call on the deleted workarea");
    out("  delete: %08X\n", (unsigned)sceKernelDeleteLwMutex(w));
    show_wa("wa", w);
    tmo = 1000; out("  lock: %08X\n", (unsigned)sceKernelLockLwMutex(w, 1, &tmo));
    out("  try: %08X\n", (unsigned)sceKernelTryLockLwMutex(w, 1));
    out("  try600: %08X\n", (unsigned)sceKernelTryLockLwMutex_600(w, 1));
    out("  unlock: %08X\n", (unsigned)sceKernelUnlockLwMutex(w, 1));
    info_prep(64);
    out("  refer: %08X\n", (unsigned)sceKernelReferLwMutexStatus(w, INFO));
    info_prep(64);
    out("  refer by id: %08X\n", (unsigned)sceKernelReferLwMutexStatusByID(wa_uid(w), INFO));
    out("  delete again: %08X\n", (unsigned)sceKernelDeleteLwMutex(w));
    show_wa("wa", w);

    /* kernlock.c:363-368 -- a hand-forged workarea (uid 0, pads DEADBEEF)
     * locks and unlocks, and ReferLwMutexStatus on it is NOT_FOUND. */
    step("lw: forged workarea (uid 0, pad DEADBEEF): lock, unlock, try, refer");
    memset(&g_forged, 0, sizeof g_forged);
    g_forged.pad[0] = g_forged.pad[1] = g_forged.pad[2] = 0xDEADBEEF;
    tmo = 1000; out("  lock: %08X\n", (unsigned)sceKernelLockLwMutex(&g_forged, 1, &tmo));
    show_wa("forged", &g_forged);
    out("  unlock: %08X\n", (unsigned)sceKernelUnlockLwMutex(&g_forged, 1));
    show_wa("forged", &g_forged);
    out("  try600: %08X\n", (unsigned)sceKernelTryLockLwMutex_600(&g_forged, 1));
    out("  unlock: %08X\n", (unsigned)sceKernelUnlockLwMutex(&g_forged, 1));
    info_prep(64);
    out("  refer: %08X\n", (unsigned)sceKernelReferLwMutexStatus(&g_forged, INFO));

    /* kernlock.c:429-435 -- a copy of a live workarea: locking it works,
     * asking about or deleting it is NOT_FOUND. */
    step("lw: copy of a live workarea: lock, unlock, refer, delete");
    lw_create(&g_wa[2], "orig", 0, 0);
    memcpy(&g_copy, &g_wa[2], sizeof g_copy);
    tmo = 1000; out("  lock copy: %08X\n", (unsigned)sceKernelLockLwMutex(&g_copy, 1, &tmo));
    show_wa("copy", &g_copy);
    show_wa("orig", &g_wa[2]);
    out("  unlock copy: %08X\n", (unsigned)sceKernelUnlockLwMutex(&g_copy, 1));
    info_prep(64);
    out("  refer copy: %08X\n", (unsigned)sceKernelReferLwMutexStatus(&g_copy, INFO));
    out("  delete copy: %08X\n", (unsigned)sceKernelDeleteLwMutex(&g_copy));
    out("  delete orig: %08X\n", (unsigned)sceKernelDeleteLwMutex(&g_wa[2]));
}

static void lw_order(w32 attr) {
    SceLwMutexWorkarea *w = &g_wa[0];
    SceUInt tmo = 1000;
    order_reset();
    step("lw: attr %X held by main; A(16) B(13) C(18) D(14) lock 1, each unlocking at once", (unsigned)attr);
    lw_create(w, "order", attr, 0);
    sceKernelLockLwMutex(w, 1, &tmo);
    for (int i = 0; i < 4; i++) {
        job_t *j = job(i, jf_locklw, 0, 1, 0);
        j->p = w;
        j->post = pf_unlocklw;
        spawn(j, ARRIVE[i]);
    }
    show_wa("wa", w);
    step("lw: main UnlockLwMutex 1");
    out("  = %08X\n", (unsigned)sceKernelUnlockLwMutex(w, 1));
    settle();
    show_jobs(4, 0);
    show_wa("wa", w);
    sceKernelDeleteLwMutex(w);
    reap_all();
}

static void sec_lw_threads(void) {
    section("lwmutex: owners and waiters");
    SceLwMutexWorkarea *w = &g_wa[0];
    SceUInt tmo;
    int r;

    /* kernlock.c:618-621 -- unlocking one another thread holds is
     * LWMUTEX_UNLOCKED (800201CC); kernlock.c:569 -- trylock is
     * LWMUTEX_LOCKED (800201CB), or 800201C4 from the old TryLock. */
    order_reset();
    step("lw: A locks and holds; main unlocks, trys, locks with 5ms");
    gate_open();
    lw_create(w, "owned", 0, 0);
    {
        job_t *j = job(0, jf_locklw, 0, 1, 0);
        j->p = w;
        j->post = pf_hold_unlocklw;
        spawn(j, 0x18);
    }
    show_wa("wa", w);
    out("  unlock: %08X\n", (unsigned)sceKernelUnlockLwMutex(w, 1));
    out("  try: %08X\n", (unsigned)sceKernelTryLockLwMutex(w, 1));
    out("  try600: %08X\n", (unsigned)sceKernelTryLockLwMutex_600(w, 1));
    tmo = 5000; r = sceKernelLockLwMutex(w, 1, &tmo);
    out("  lock: %08X tmo=%s\n", (unsigned)r, tcls(tmo, 5000));
    show_wa("wa", w);

    /* kernlock.c:581 -- the workarea's waiting count follows the queue. */
    step("lw: B(17) waits; workarea and status; A lets go");
    {
        job_t *j = job(1, jf_locklw, 0, 1, 0);
        j->p = w;
        spawn(j, 0x17);
    }
    show_wa("wa", w);
    lw_refer(w);
    sceKernelSignalSema(g_gate, 1);
    settle();
    show_jobs(2, 0);
    show_wa("wa", w);
    sceKernelDeleteLwMutex(w);
    reap_all();

    lw_order(0);
    lw_order(0x100);

    /* kernlock.c:490-505, hle.h:185-187 -- WAIT_DELETE (800201B5) for the
     * waiters of a deleted lwmutex. */
    order_reset();
    step("lw: held by main, A B (prio 18) wait; DeleteLwMutex");
    lw_create(w, "del", 0, 1);
    for (int i = 0; i < 2; i++) {
        job_t *j = job(i, jf_locklw, 0, 1, 0);
        j->p = w;
        spawn(j, 0x18);
    }
    show_wa("wa", w);
    out("  = %08X\n", (unsigned)sceKernelDeleteLwMutex(w));
    settle();
    show_jobs(2, 0);
    show_wa("wa", w);
    reap_all();
}

/* ======================================================================== */
/* Mailboxes                                                                */
/* ======================================================================== */

typedef struct { w32 next; unsigned char prio, pad[3]; w32 id; } msg_t;
static msg_t g_msg[6] __attribute__((aligned(16)));

static const char *msgname(w32 p) {
    static char b[4][8];
    static int k;
    if (p == 0) return "NULL";
    if (p == 0xDEADBEEF) return "DEAD";
    for (int i = 0; i < 6; i++)
        if (p == (w32)&g_msg[i]) {
            char *s = b[k++ & 3];
            snprintf(s, 8, "m%d", i + 1);
            return s;
        }
    return "other";
}

static void msg_prep(int i, int prio) {
    g_msg[i].next = 0xDEADBEEF;
    g_msg[i].prio = (unsigned char)prio;
    g_msg[i].pad[0] = g_msg[i].pad[1] = g_msg[i].pad[2] = 0;
    g_msg[i].id = (w32)(i + 1);
}

/* Each message's `next`, for the ones named. */
static void show_links(int n) {
    out("  links:");
    for (int i = 0; i < n; i++) out(" m%d->%s", i + 1, msgname(g_msg[i].next));
    out("\n");
}

static void mbx_state(SceUID m) {
    SceKernelMbxInfo i;
    i.size = sizeof i;
    int r = sceKernelReferMbxStatus(m, &i);
    if (r < 0) out("  refer: %08X\n", (unsigned)r);
    else out("  count=%d first=%s wait=%d\n", i.numMessages,
             msgname((w32)i.firstMessage), i.numWaitThreads);
}

static void poll_mbx(const char *what, SceUID m) {
    void *p = (void *)0xDEADBEEF;
    int r = sceKernelPollMbx(m, &p);
    out("  %s: %08X got=%s\n", what, (unsigned)r, msgname((w32)p));
}

static void sec_mbx_create(void) {
    section("mailbox: create");

    /* kernobj.c:1084 -- the legal mask is 0x5FF. */
    step("mbx: CreateMbx attr sweep");
    static const w32 attrs[] = { 0x0, 0x100, 0x200, 0x400, 0x500, 0x5FF, 0x600,
                                 0x800, 0x1000, 0x4000 };
    for (unsigned i = 0; i < sizeof attrs / sizeof attrs[0]; i++) {
        SceUID m = sceKernelCreateMbx("attr", attrs[i], NULL);
        out("  attr %04X: %s\n", (unsigned)attrs[i], uidstr(m));
        if (m > 0) sceKernelDeleteMbx(m);
    }

    /* kernobj.c:1212 -- a NULL name is ERROR (80020001). */
    step("mbx: CreateMbx with NULL name");
    {
        SceUID m = sceKernelCreateMbx(NULL, 0, NULL);
        uidret(m);
        if (m > 0) sceKernelDeleteMbx(m);
    }
}

static void sec_mbx_ops(void) {
    section("mailbox: send, receive, links (one thread)");
    g_ptrname = msgname;
    SceUInt tmo;
    int r;
    void *p;

    step("mbx: CreateMbx attr 0");
    SceUID m = sceKernelCreateMbx("ops", 0, NULL);
    uidret(m);

    /* kernobj.c:1310 -- MBOX_NOMSG (800201B2) from an empty box. */
    step("mbx: PollMbx on an empty mailbox");
    poll_mbx("poll", m);

    /* kernobj.c:1061-1074 -- the queue is a circular list through the
     * packets' own `next`: one message points at itself; with two, the
     * first points at the second and the second at the first. */
    step("mbx: SendMbx m1 (next poisoned DEADBEEF)");
    for (int i = 0; i < 6; i++) msg_prep(i, 0);
    out("  = %08X\n", (unsigned)sceKernelSendMbx(m, &g_msg[0]));
    show_links(2);
    mbx_state(m);

    /* kernobj.c:1245-1247 -- a packet already in the queue is refused
     * (MBX_CORRUPT 800201C9) and the count stays 1. */
    step("mbx: SendMbx m1 a second time");
    out("  = %08X\n", (unsigned)sceKernelSendMbx(m, &g_msg[0]));
    show_links(2);
    mbx_state(m);

    step("mbx: SendMbx m2, then m3");
    out("  m2: %08X\n", (unsigned)sceKernelSendMbx(m, &g_msg[1]));
    show_links(3);
    mbx_state(m);
    out("  m3: %08X\n", (unsigned)sceKernelSendMbx(m, &g_msg[2]));
    show_links(3);

    /* kernobj.c:1353-1370 -- 52 bytes, and kernobj.c:1112-1129 -- `first`
     * is whatever the last message points at. */
    step("mbx: ReferMbxStatus with size 52, then size 0");
    info_prep(52);
    out("  = %08X\n", (unsigned)sceKernelReferMbxStatus(m, INFO));
    show_info(MBX_FIELDS);
    info_prep(0);
    out("  size 0: %08X\n", (unsigned)sceKernelReferMbxStatus(m, INFO));
    show_info(MBX_FIELDS);

    /* kernobj.c:1194-1205 -- a received packet's `next` is left as it was
     * in the ring. */
    step("mbx: PollMbx three times, links after each");
    for (int i = 0; i < 3; i++) {
        poll_mbx("poll", m);
        show_links(3);
        mbx_state(m);
    }

    step("mbx: ReceiveMbx on empty, timeout 5ms; SendMbx m4, ReceiveMbx timeout 500ms");
    p = (void *)0xDEADBEEF; tmo = 5000;
    r = sceKernelReceiveMbx(m, &p, &tmo);
    out("  empty: %08X got=%s tmo=%s\n", (unsigned)r, msgname((w32)p), tcls(tmo, 5000));
    sceKernelSendMbx(m, &g_msg[3]);
    p = (void *)0xDEADBEEF; tmo = 500000;
    r = sceKernelReceiveMbx(m, &p, &tmo);
    out("  full: %08X got=%s tmo=%s\n", (unsigned)r, msgname((w32)p), tcls(tmo, 500000));
    sceKernelDeleteMbx(m);

    /* kernobj.c:1151-1169 -- attr 0x400 orders messages by the packet's
     * priority byte, lowest first; psprecomp puts a new message after the
     * ones of equal priority. */
    step("mbx: attr 400, send m1 prio 3, m2 prio 1, m3 prio 2, m4 prio 1, m5 prio 0");
    m = sceKernelCreateMbx("prio", 0x400, NULL);
    uidret(m);
    static const int pr[5] = { 3, 1, 2, 1, 0 };
    for (int i = 0; i < 5; i++) {
        msg_prep(i, pr[i]);
        r = sceKernelSendMbx(m, &g_msg[i]);
        if (r) out("  m%d: %08X\n", i + 1, (unsigned)r);
    }
    show_links(5);
    mbx_state(m);
    step("mbx: PollMbx five times");
    out("  received:");
    for (int i = 0; i < 6; i++) {
        p = (void *)0xDEADBEEF;
        r = sceKernelPollMbx(m, &p);
        out(" %s", r ? hex(r) : msgname((w32)p));
    }
    out("\n");
    sceKernelDeleteMbx(m);

    /* hle.h:177-181 -- UNKNOWN_MBXID (8002019B). */
    step("mbx: every call on uid 0 and on a deleted mailbox");
    out("  send 0: %08X\n", (unsigned)sceKernelSendMbx(0, &g_msg[5]));
    poll_mbx("poll 0", 0);
    out("  send deleted: %08X\n", (unsigned)sceKernelSendMbx(m, &g_msg[5]));
    poll_mbx("poll deleted", m);
    p = NULL; tmo = 1000;
    out("  receive deleted: %08X\n", (unsigned)sceKernelReceiveMbx(m, &p, &tmo));
    out("  cancel deleted: %08X\n", (unsigned)sceKernelCancelReceiveMbx(m, NULL));
    info_prep(52);
    out("  refer deleted: %08X\n", (unsigned)sceKernelReferMbxStatus(m, INFO));
    out("  delete deleted: %08X\n", (unsigned)sceKernelDeleteMbx(m));
}

/* Four receivers, four messages sent one at a time. */
static void mbx_order(w32 attr) {
    order_reset();
    g_ptrname = msgname;
    step("mbx: attr %X, A(16) B(13) C(18) D(14) receive; send m1..m4 one at a time", (unsigned)attr);
    SceUID m = sceKernelCreateMbx("order", attr, NULL);
    for (int i = 0; i < 4; i++) spawn(job(i, jf_recvmbx, m, 0, 0), ARRIVE[i]);
    mbx_state(m);
    for (int i = 0; i < 4; i++) {
        msg_prep(i, 0);
        out("  send m%d: %08X\n", i + 1, (unsigned)sceKernelSendMbx(m, &g_msg[i]));
        settle();
    }
    show_jobs(4, 'p');
    mbx_state(m);
    sceKernelDeleteMbx(m);
    reap_all();
}

static void sec_mbx_threads(void) {
    section("mailbox: receivers");
    g_ptrname = msgname;
    SceUID m;
    int r, n;

    /* kernobj.c:1254-1260 -- a waiting receiver takes the packet without it
     * ever being queued, so its `next` is left as the sender left it. */
    order_reset();
    step("mbx: A waits in ReceiveMbx; SendMbx m1 (next DEADBEEF)");
    m = sceKernelCreateMbx("direct", 0, NULL);
    spawn(job(0, jf_recvmbx, m, 0, 0), 0x18);
    mbx_state(m);
    msg_prep(0, 0);
    out("  = %08X\n", (unsigned)sceKernelSendMbx(m, &g_msg[0]));
    settle();
    show_jobs(1, 'p');
    show_links(1);
    mbx_state(m);
    sceKernelDeleteMbx(m);
    reap_all();

    mbx_order(0);
    mbx_order(0x100);

    /* hle.h:137-142 -- WAIT_CANCEL (800201A9), and the waiter count. */
    order_reset();
    step("mbx: A B (prio 18) wait; CancelReceiveMbx");
    m = sceKernelCreateMbx("cancel", 0, NULL);
    spawn(job(0, jf_recvmbx, m, 0, 0), 0x18);
    spawn(job(1, jf_recvmbx, m, 0, 0), 0x18);
    n = 99;
    r = sceKernelCancelReceiveMbx(m, &n);
    settle();
    out("  = %08X waiters=%d\n", (unsigned)r, n);
    show_jobs(2, 'p');
    mbx_state(m);
    sceKernelDeleteMbx(m);
    reap_all();

    /* kernobj.c:1230-1237, hle.h:185-187 -- WAIT_DELETE (800201B5). */
    order_reset();
    step("mbx: A B (prio 18) wait; DeleteMbx");
    m = sceKernelCreateMbx("del", 0, NULL);
    spawn(job(0, jf_recvmbx, m, 0, 0), 0x18);
    spawn(job(1, jf_recvmbx, m, 0, 0), 0x18);
    out("  = %08X\n", (unsigned)sceKernelDeleteMbx(m));
    settle();
    show_jobs(2, 'p');
    reap_all();
}

/* ======================================================================== */
/* Message pipes                                                            */
/* ======================================================================== */

static unsigned char g_tx[0x200], g_rx[0x200];

static SceUID mpp_create(const char *name, int part, w32 attr, w32 size) {
    return sceKernelCreateMsgPipe(name, part, attr, (void *)size, NULL);
}

/* One transfer, logged as ret and the byte count. */
static void mpp_line(const char *what, int r, w32 bytes) {
    out("  %s: %08X bytes=%08X\n", what, (unsigned)r, (unsigned)bytes);
}

static void sec_mpp_create(void) {
    section("message pipe: create");

    /* kernobj.c:115-128 via kernobj.c:613 -- partitions 2 and 6 permitted;
     * 1, 3, 4, 8, 9 ILLEGAL_PERM (800200D1); 0, 7, 10, -1 out of range
     * (800200D2). Partition 5 is left out: vpl_partition_error's comment
     * says the test that swept it crashes hardware. */
    step("mpp: CreateMsgPipe partition sweep, size 0x100");
    static const int parts[] = { 2, 6, 1, 3, 4, 8, 9, 0, 7, 10, -1 };
    for (unsigned i = 0; i < sizeof parts / sizeof parts[0]; i++) {
        SceUID p = mpp_create("part", parts[i], 0, 0x100);
        out("  partition %d: %s\n", parts[i], uidstr(p));
        if (p > 0) sceKernelDeleteMsgPipe(p);
    }

    /* kernobj.c:615-619 -- msgpipe/create: 0x3FF refused, 0x51FF accepted,
     * so the legal set is 0x1FF | 0x1000 | 0x4000. */
    step("mpp: CreateMsgPipe attr sweep");
    static const w32 attrs[] = { 0x0, 0x100, 0x1FF, 0x200, 0x3FF, 0x400, 0x800,
                                 0x1000, 0x2000, 0x4000, 0x51FF, 0x8000 };
    for (unsigned i = 0; i < sizeof attrs / sizeof attrs[0]; i++) {
        SceUID p = mpp_create("attr", 2, attrs[i], 0x100);
        out("  attr %04X: %s\n", (unsigned)attrs[i], uidstr(p));
        if (p > 0) sceKernelDeleteMsgPipe(p);
    }

    /* kernobj.c:579-582 -- a pipe with no buffer is legal. */
    step("mpp: CreateMsgPipe sizes 0, 1, 0x1001, 0x10000; ReferMsgPipeStatus");
    static const w32 sizes[] = { 0, 1, 0x1001, 0x10000 };
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        SceUID p = mpp_create("size", 2, 0, sizes[i]);
        out("  size %X: %s", (unsigned)sizes[i], uidstr(p));
        if (p > 0) { mpp_state(p); sceKernelDeleteMsgPipe(p); }
        else out("\n");
    }

    /* kernobj.c:609-612 -- a NULL name is NO_MEMORY (80020190). */
    step("mpp: CreateMsgPipe with NULL name");
    {
        SceUID p = mpp_create(NULL, 2, 0, 0x100);
        uidret(p);
        if (p > 0) sceKernelDeleteMsgPipe(p);
    }
}

static void sec_mpp_ops(void) {
    section("message pipe: send, receive (one thread)");
    SceUInt tmo;
    w32 b;
    int r;
    for (int i = 0; i < (int)sizeof g_tx; i++) g_tx[i] = (unsigned char)(i * 7 + 1);

    step("mpp: CreateMsgPipe size 0x100 attr 0");
    SceUID p = mpp_create("ops", 2, 0, 0x100);
    uidret(p);

    /* kernobj.c:525-531 -- a byte ring: no per-message header, free space
     * drops by exactly what was sent. */
    step("mpp: SendMsgPipe 0x40 bytes (full), then 1 byte");
    b = 0x1337; tmo = 500000;
    r = sceKernelSendMsgPipe(p, g_tx, 0x40, 0, &b, &tmo);
    out("  0x40: %08X bytes=%08X tmo=%s\n", (unsigned)r, (unsigned)b, tcls(tmo, 500000));
    mpp_state(p);
    b = 0x1337; tmo = 500000;
    r = sceKernelSendMsgPipe(p, g_tx + 0x40, 1, 0, &b, &tmo);
    out("  1: %08X bytes=%08X\n", (unsigned)r, (unsigned)b);
    mpp_state(p);

    step("mpp: ReceiveMsgPipe 0x41, compare the bytes");
    memset(g_rx, 0, sizeof g_rx);
    b = 0x1337; tmo = 500000;
    r = sceKernelReceiveMsgPipe(p, g_rx, 0x41, 0, &b, &tmo);
    out("  = %08X bytes=%08X tmo=%s match=%s\n", (unsigned)r, (unsigned)b,
        tcls(tmo, 500000), memcmp(g_rx, g_tx, 0x41) ? "no" : "yes");
    mpp_state(p);

    /* kernobj.c:533-535 -- only modes 0 and 1: -2, -1, 2..9, 257, 4097 are
     * ILLEGAL_MODE (80020195). */
    step("mpp: TrySendMsgPipe 1 byte with modes -1, 2, 3, 9, 0x101, 0x1001");
    static const int modes[] = { -1, 2, 3, 9, 0x101, 0x1001 };
    for (unsigned i = 0; i < sizeof modes / sizeof modes[0]; i++) {
        b = 0x1337;
        r = sceKernelTrySendMsgPipe(p, g_tx, 1, modes[i], &b);
        out("  mode %X: %08X bytes=%08X\n", (unsigned)modes[i], (unsigned)r, (unsigned)b);
    }

    /* kernobj.c:860-871 -- a negative length is 800200D3, one larger than
     * the pipe 800201BC. kernobj.c:875-882 -- zero is OK. */
    step("mpp: TrySendMsgPipe lengths -1, 0x101, 0; TryReceiveMsgPipe -1, 0x101, 0");
    b = 0x1337; r = sceKernelTrySendMsgPipe(p, g_tx, (unsigned)-1, 0, &b); mpp_line("send -1", r, b);
    b = 0x1337; r = sceKernelTrySendMsgPipe(p, g_tx, 0x101, 0, &b); mpp_line("send 0x101", r, b);
    b = 0x1337; r = sceKernelTrySendMsgPipe(p, g_tx, 0, 0, &b); mpp_line("send 0", r, b);
    b = 0x1337; r = sceKernelTryReceiveMsgPipe(p, g_rx, (unsigned)-1, 0, &b); mpp_line("recv -1", r, b);
    b = 0x1337; r = sceKernelTryReceiveMsgPipe(p, g_rx, 0x101, 0, &b); mpp_line("recv 0x101", r, b);
    b = 0x1337; r = sceKernelTryReceiveMsgPipe(p, g_rx, 0, 0, &b); mpp_line("recv 0", r, b);

    /* kernobj.c:901-913 -- a full-wait poll that fails leaves the byte word
     * alone (0x1337); an ASAP one that moves nothing writes 0. */
    step("mpp: fill: TrySend 0xC0; TrySend 0x80 full-wait, then 0x80 ASAP, then 1 ASAP");
    b = 0x1337; r = sceKernelTrySendMsgPipe(p, g_tx, 0xC0, 0, &b); mpp_line("0xC0", r, b);
    b = 0x1337; r = sceKernelTrySendMsgPipe(p, g_tx + 0xC0, 0x80, 0, &b); mpp_line("0x80 full", r, b);
    b = 0x1337; r = sceKernelTrySendMsgPipe(p, g_tx + 0xC0, 0x80, 1, &b); mpp_line("0x80 asap", r, b);
    b = 0x1337; r = sceKernelTrySendMsgPipe(p, g_tx, 1, 1, &b); mpp_line("1 asap", r, b);
    mpp_state(p);

    step("mpp: SendMsgPipe 1 byte to a full pipe, timeout 5ms");
    b = 0x1337; tmo = 5000;
    r = sceKernelSendMsgPipe(p, g_tx, 1, 0, &b, &tmo);
    out("  = %08X bytes=%08X tmo=%s\n", (unsigned)r, (unsigned)b, tcls(tmo, 5000));

    /* The ring wraps: 0x80 out, 0x60 in at the start of the buffer, then
     * everything out in order. */
    step("mpp: wrap: receive 0x80, send 0x60, receive 0xE0 and compare");
    memset(g_rx, 0, sizeof g_rx);
    b = 0x1337; r = sceKernelTryReceiveMsgPipe(p, g_rx, 0x80, 0, &b); mpp_line("recv 0x80", r, b);
    out("  first 0x80 match=%s\n", memcmp(g_rx, g_tx, 0x80) ? "no" : "yes");
    b = 0x1337; r = sceKernelTrySendMsgPipe(p, g_tx + 0x100, 0x60, 0, &b); mpp_line("send 0x60", r, b);
    mpp_state(p);
    memset(g_rx, 0, sizeof g_rx);
    b = 0x1337; r = sceKernelTryReceiveMsgPipe(p, g_rx, 0xE0, 0, &b); mpp_line("recv 0xE0", r, b);
    out("  match=%s\n", (!memcmp(g_rx, g_tx + 0x80, 0x80) && !memcmp(g_rx + 0x80, g_tx + 0x100, 0x60))
        ? "yes" : "no");
    mpp_state(p);

    step("mpp: empty: TryReceive 1 full-wait, 1 ASAP; Receive 1 full-wait timeout 5ms");
    b = 0x1337; r = sceKernelTryReceiveMsgPipe(p, g_rx, 1, 0, &b); mpp_line("try full", r, b);
    b = 0x1337; r = sceKernelTryReceiveMsgPipe(p, g_rx, 1, 1, &b); mpp_line("try asap", r, b);
    /* kernobj.c:991-993 -- a full-wait that ran out reports bytes=0. */
    b = 0x1337; tmo = 5000;
    r = sceKernelReceiveMsgPipe(p, g_rx, 1, 0, &b, &tmo);
    out("  wait: %08X bytes=%08X tmo=%s\n", (unsigned)r, (unsigned)b, tcls(tmo, 5000));

    step("mpp: 3 bytes in: Receive 10 ASAP (timeout 500ms), TryReceive 10 full-wait");
    sceKernelTrySendMsgPipe(p, g_tx, 3, 0, &b);
    b = 0x1337; tmo = 500000;
    r = sceKernelReceiveMsgPipe(p, g_rx, 10, 1, &b, &tmo);
    out("  asap: %08X bytes=%08X tmo=%s\n", (unsigned)r, (unsigned)b, tcls(tmo, 500000));
    sceKernelTrySendMsgPipe(p, g_tx, 3, 0, &b);
    b = 0x1337; r = sceKernelTryReceiveMsgPipe(p, g_rx, 10, 0, &b); mpp_line("try full", r, b);
    mpp_state(p);

    /* kernobj.c:1019-1033 -- 56 bytes. */
    step("mpp: ReferMsgPipeStatus with size 56, then size 0");
    info_prep(56);
    out("  = %08X\n", (unsigned)sceKernelReferMsgPipeStatus(p, INFO));
    show_info(MPP_FIELDS);
    info_prep(0);
    out("  size 0: %08X\n", (unsigned)sceKernelReferMsgPipeStatus(p, INFO));
    show_info(MPP_FIELDS);

    /* kernobj.c:1006-1017 -- psprecomp's cancel also empties the buffer. */
    step("mpp: CancelMsgPipe with 3 bytes buffered and no waiters");
    {
        int ns = 99, nr = 99;
        r = sceKernelCancelMsgPipe(p, &ns, &nr);
        out("  = %08X send=%d recv=%d\n", (unsigned)r, ns, nr);
        mpp_state(p);
    }
    sceKernelDeleteMsgPipe(p);

    /* kernobj.c:863-867 -- on a pipe with no buffer every request is FULL,
     * even one obviously too big; kernobj.c:875-876 -- zero bytes is OK. */
    step("mpp: no buffer: TrySend 1, TrySend 0x1000, TrySend 0, TryReceive 1, Send 1 timeout 5ms");
    p = mpp_create("nobuf", 2, 0, 0);
    b = 0x1337; r = sceKernelTrySendMsgPipe(p, g_tx, 1, 0, &b); mpp_line("send 1", r, b);
    b = 0x1337; r = sceKernelTrySendMsgPipe(p, g_tx, 0x1000, 0, &b); mpp_line("send 0x1000", r, b);
    b = 0x1337; r = sceKernelTrySendMsgPipe(p, g_tx, 0, 0, &b); mpp_line("send 0", r, b);
    b = 0x1337; r = sceKernelTryReceiveMsgPipe(p, g_rx, 1, 0, &b); mpp_line("recv 1", r, b);
    b = 0x1337; tmo = 5000;
    r = sceKernelSendMsgPipe(p, g_tx, 1, 0, &b, &tmo);
    out("  send wait: %08X bytes=%08X tmo=%s\n", (unsigned)r, (unsigned)b, tcls(tmo, 5000));
    sceKernelDeleteMsgPipe(p);

    /* hle.h:177-181 -- UNKNOWN_MPPID (8002019E). */
    step("mpp: every call on uid 0 and on a deleted pipe");
    b = 0x1337;
    out("  send 0: %08X\n", (unsigned)sceKernelTrySendMsgPipe(0, g_tx, 1, 0, &b));
    out("  send deleted: %08X\n", (unsigned)sceKernelTrySendMsgPipe(p, g_tx, 1, 0, &b));
    out("  recv deleted: %08X\n", (unsigned)sceKernelTryReceiveMsgPipe(p, g_rx, 1, 0, &b));
    out("  cancel deleted: %08X\n", (unsigned)sceKernelCancelMsgPipe(p, NULL, NULL));
    info_prep(56);
    out("  refer deleted: %08X\n", (unsigned)sceKernelReferMsgPipeStatus(p, INFO));
    out("  delete deleted: %08X\n", (unsigned)sceKernelDeleteMsgPipe(p));
}

/* Receivers A(16) B(13) C(18) want 4 bytes; main sends 4 at a time. */
static void mpp_recv_order(w32 attr) {
    order_reset();
    step("mpp: attr %X, receivers A(16) B(13) C(18) want 4; send 1111, 2222, 3333", (unsigned)attr);
    SceUID p = mpp_create("rorder", 2, attr, 0x100);
    for (int i = 0; i < 3; i++) {
        job_t *j = job(i, jf_recvmpp, p, 4, 0);
        j->out = 0x1337;
        spawn(j, ARRIVE[i]);
    }
    mpp_state(p);
    for (int i = 0; i < 3; i++) {
        unsigned char d[4];
        memset(d, '1' + i, 4);
        w32 b = 0x1337;
        int r = sceKernelTrySendMsgPipe(p, d, 4, 0, &b);
        if (r) mpp_line("send", r, b);
        settle();
    }
    show_jobs(3, 'b');
    mpp_state(p);
    sceKernelDeleteMsgPipe(p);
    reap_all();
}

/* A 4-byte pipe, full; senders A(16) B(13) C(18) each block sending 4. */
static void mpp_send_order(w32 attr) {
    order_reset();
    step("mpp: attr %X, 4-byte pipe full; senders A(16) B(13) C(18) send 4; receive 4 at a time", (unsigned)attr);
    SceUID p = mpp_create("sorder", 2, attr, 4);
    w32 b;
    sceKernelTrySendMsgPipe(p, "0000", 4, 0, &b);
    for (int i = 0; i < 3; i++) {
        job_t *j = job(i, jf_sendmpp, p, 4, 0);
        memset(j->buf, 'A' + i, 4);
        j->out = 0x1337;
        spawn(j, ARRIVE[i]);
    }
    mpp_state(p);
    out("  received:");
    for (int i = 0; i < 4; i++) {
        char d[5] = { 0 };
        b = 0x1337;
        int r = sceKernelTryReceiveMsgPipe(p, d, 4, 0, &b);
        if (r) out(" %08X", (unsigned)r);
        else out(" %s", d);
        settle();
    }
    out("\n");
    show_jobs(3, 'x');
    mpp_state(p);
    sceKernelDeleteMsgPipe(p);
    reap_all();
}

static void sec_mpp_threads(void) {
    section("message pipe: senders and receivers");
    SceUID p;
    SceUInt tmo;
    w32 b;
    int r;

    /* kernobj.c:694-699 -- senders are ordered by 0x100, receivers by
     * 0x1000, separately. */
    mpp_recv_order(0);
    mpp_recv_order(0x1000);
    mpp_recv_order(0x100);
    mpp_send_order(0);
    mpp_send_order(0x100);
    mpp_send_order(0x1000);

    /* kernobj.c:676-690 -- msgpipe/data: receivers wanting 4 bytes are
     * filled in pieces by 3-byte sends, with a buffer and without one, and
     * read "msg1" "msg2" "msg3". */
    for (int pass = 0; pass < 2; pass++) {
        const w32 size = pass ? 0 : 0x100;
        order_reset();
        step("mpp: buffer %X, A B C (prio 18) receive 4; main sends msg 1ms g2m sg3 (3 each, 50ms)", (unsigned)size);
        p = mpp_create("pieces", 2, 0, size);
        for (int i = 0; i < 3; i++) {
            job_t *j = job(i, jf_recvmpp, p, 4, 0);
            j->out = 0x1337;
            spawn(j, 0x18);
        }
        static const char *chunk[4] = { "msg", "1ms", "g2m", "sg3" };
        for (int i = 0; i < 4; i++) {
            b = 0x1337; tmo = 50000;
            r = sceKernelSendMsgPipe(p, (void *)chunk[i], 3, 0, &b, &tmo);
            out("  send %s: %08X bytes=%08X\n", chunk[i], (unsigned)r, (unsigned)b);
            settle();
        }
        show_jobs(3, 'b');
        mpp_state(p);
        sceKernelDeleteMsgPipe(p);
        reap_all();
    }

    /* kernobj.c:767-774 -- a full-wait sender arriving at a pipe with one
     * byte free does not store part of its message. */
    order_reset();
    step("mpp: 4-byte pipe holding xyz; A sends 2 bytes full-wait; then main receives");
    p = mpp_create("partial", 2, 0, 4);
    sceKernelTrySendMsgPipe(p, "xyz", 3, 0, &b);
    {
        job_t *j = job(0, jf_sendmpp, p, 2, 0);
        memcpy(j->buf, "AA", 2);
        j->out = 0x1337;
        spawn(j, 0x18);
    }
    mpp_state(p);
    {
        char d[8] = { 0 };
        b = 0x1337; r = sceKernelTryReceiveMsgPipe(p, d, 3, 0, &b);
        out("  recv 3: %08X bytes=%08X data=\"%s\"\n", (unsigned)r, (unsigned)b, d);
        settle();
        memset(d, 0, sizeof d);
        b = 0x1337; r = sceKernelTryReceiveMsgPipe(p, d, 4, 1, &b);
        out("  recv 4 asap: %08X bytes=%08X data=\"%s\"\n", (unsigned)r, (unsigned)b, d);
    }
    show_jobs(1, 'x');
    sceKernelDeleteMsgPipe(p);
    reap_all();
    step("mpp: 4-byte pipe holding xyz: TrySend 2 bytes ASAP");
    p = mpp_create("asap", 2, 0, 4);
    sceKernelTrySendMsgPipe(p, "xyz", 3, 0, &b);
    b = 0x1337; r = sceKernelTrySendMsgPipe(p, "AA", 2, 1, &b); mpp_line("asap", r, b);
    mpp_state(p);
    sceKernelDeleteMsgPipe(p);

    /* kernobj.c:892-900 -- msgpipe/tryreceive: on a pipe with no buffer, a
     * poll reaches into a blocked sender ("Partial packet: OK (bytes=128)",
     * "Complete packet: OK (bytes=256)"). */
    order_reset();
    step("mpp: no buffer, A sends 0x100 full-wait; main TryReceive 0x80 twice");
    p = mpp_create("reach", 2, 0, 0);
    {
        job_t *j = job(0, jf_sendmpp, p, 0x100, 0);
        j->p = g_tx;
        j->out = 0x1337;
        spawn(j, 0x18);
    }
    mpp_state(p);
    b = 0x1337; r = sceKernelTryReceiveMsgPipe(p, g_rx, 0x80, 0, &b); mpp_line("recv 0x80", r, b);
    settle();
    b = 0x1337; r = sceKernelTryReceiveMsgPipe(p, g_rx, 0x80, 0, &b); mpp_line("recv 0x80", r, b);
    settle();
    show_jobs(1, 'x');
    mpp_state(p);
    sceKernelDeleteMsgPipe(p);
    reap_all();

    order_reset();
    step("mpp: no buffer, A receives 8 full-wait; main TrySend 8");
    p = mpp_create("hand", 2, 0, 0);
    {
        job_t *j = job(0, jf_recvmpp, p, 8, 0);
        j->out = 0x1337;
        spawn(j, 0x18);
    }
    b = 0x1337; r = sceKernelTrySendMsgPipe(p, "handoff!", 8, 0, &b); mpp_line("send 8", r, b);
    settle();
    show_jobs(1, 'b');
    sceKernelDeleteMsgPipe(p);
    reap_all();

    /* hle.h:137-142 -- WAIT_CANCEL (800201A9), with the per-queue counts. */
    order_reset();
    step("mpp: A B (prio 18) receive 4 from an empty 0x10 pipe; CancelMsgPipe");
    p = mpp_create("cancel", 2, 0, 0x10);
    for (int i = 0; i < 2; i++) {
        job_t *j = job(i, jf_recvmpp, p, 4, 0);
        j->out = 0x1337;
        spawn(j, 0x18);
    }
    {
        int ns = 99, nr = 99;
        r = sceKernelCancelMsgPipe(p, &ns, &nr);
        settle();
        out("  = %08X send=%d recv=%d\n", (unsigned)r, ns, nr);
    }
    show_jobs(2, 'x');
    mpp_state(p);
    sceKernelDeleteMsgPipe(p);
    reap_all();

    /* kernobj.c:644-657 -- msgpipe/data: a receiver whose pipe is deleted
     * is told it got 0 bytes (the 0x1337 is overwritten), WAIT_DELETE. */
    order_reset();
    step("mpp: A receives 4 from an empty pipe, B sends 4 to a full 4-byte one; DeleteMsgPipe both");
    p = mpp_create("delr", 2, 0, 0x10);
    SceUID p2 = mpp_create("dels", 2, 0, 4);
    sceKernelTrySendMsgPipe(p2, "full", 4, 0, &b);
    {
        job_t *j = job(0, jf_recvmpp, p, 4, 0);
        j->out = 0x1337;
        spawn(j, 0x18);
        j = job(1, jf_sendmpp, p2, 4, 0);
        memcpy(j->buf, "BBBB", 4);
        j->out = 0x1337;
        spawn(j, 0x18);
    }
    out("  delete recv pipe: %08X\n", (unsigned)sceKernelDeleteMsgPipe(p));
    out("  delete send pipe: %08X\n", (unsigned)sceKernelDeleteMsgPipe(p2));
    settle();
    show_jobs(2, 'x');
    reap_all();
}

/* ======================================================================== */
/* Variable-size pools                                                      */
/* ======================================================================== */

/* Offsets are from the first block this test allocated (`g_vref`). */
static w32 g_vref;
static char g_offbuf[4][16];
static int  g_offk;
static const char *voff(w32 p) {
    char *s = g_offbuf[g_offk++ & 3];
    if (p == 0) return "NULL";
    if (p == 0xDEADBEEF) return "DEAD";
    int d = (int)(p - g_vref);
    if (d < 0) snprintf(s, 16, "-%X", (unsigned)-d);
    else snprintf(s, 16, "+%X", (unsigned)d);
    return s;
}

static int user_ptr_ok(w32 p) {
    return (p & 3) == 0 && p >= 0x08800000u && p < 0x0A000000u;
}

/* kernobj.c:48-73, 213-231 -- the pool's bookkeeping sits in the pool: an
 * allocated block's header is {next = the pool's start (accounting + 8),
 * size / 8}, and the 32-byte accounting holds start, start, start + 7, total
 * - 8, allocated / 8, the next free block, and the bottom block. Printed as
 * offsets from the first block; the accounting is read only if the header
 * points at a plausible address inside the user partition near the block. */
static void vpl_hdr(const char *label, w32 blk) {
    if (!user_ptr_ok(blk)) { out("  %s: %s\n", label, voff(blk)); return; }
    const w32 *h = (const w32 *)(blk - 8);
    out("  %s: at %s hdr next=%s size/8=%X\n", label, voff(blk), voff(h[0]), (unsigned)h[1]);
}

static void vpl_acct(w32 blk, w32 span) {
    if (!user_ptr_ok(blk)) { out("  acct: no block\n"); return; }
    const w32 start = ((const w32 *)(blk - 8))[0];
    const w32 base = start - 8;
    if (!user_ptr_ok(base) || base > blk || blk - base > span) {
        out("  acct: header next %s is not a pool start\n", voff(start));
        return;
    }
    const w32 *a = (const w32 *)base;
    out("  acct at %s:", voff(base));
    for (int i = 0; i < 8; i++) {
        w32 v = a[i];
        if (v >= base && v < base + span) out(" %s", voff(v));
        else out(" %08X", (unsigned)v);
    }
    out("\n");
}

static void *vtry(SceUID v, w32 n, int *rp) {
    void *p = (void *)0xDEADBEEF;
    *rp = sceKernelTryAllocateVpl(v, n, &p);
    return p;
}

static void sec_vpl_create(void) {
    section("vpl: create");

    /* kernobj.c:115-128 -- partitions 2 and 6 permitted; 1, 3, 4, 8, 9
     * ILLEGAL_PERM (800200D1); 0, 7, 10, -1, -5 out of range (800200D2). */
    step("vpl: CreateVpl partition sweep, size 0x100");
    static const int parts[] = { 2, 6, 1, 3, 4, 8, 9, 0, 7, 10, -1, -5 };
    for (unsigned i = 0; i < sizeof parts / sizeof parts[0]; i++) {
        SceUID v = sceKernelCreateVpl("part", parts[i], 0, 0x100, NULL);
        out("  partition %d: %s\n", parts[i], uidstr(v));
        if (v > 0) sceKernelDeleteVpl(v);
    }

    /* kernobj.c:42-46 -- vpl/create: 0x1, 0x100, 0x200 and 0x4000 accepted;
     * 0x400, 0x800, 0x1000, 0x2000, 0x8000, 0x10000 refused. */
    step("vpl: CreateVpl attr sweep");
    static const w32 attrs[] = { 0x0, 0x1, 0x100, 0x200, 0x300, 0x3FF, 0x400, 0x800,
                                 0x1000, 0x2000, 0x4000, 0x43FF, 0x8000, 0x10000 };
    for (unsigned i = 0; i < sizeof attrs / sizeof attrs[0]; i++) {
        SceUID v = sceKernelCreateVpl("attr", 2, attrs[i], 0x100, NULL);
        out("  attr %05X: %s\n", (unsigned)attrs[i], uidstr(v));
        if (v > 0) sceKernelDeleteVpl(v);
    }

    /* kernobj.c:23-31, 103-113 -- poolSize = round_up(size, 8) - 32:
     * 0x1000 -> FE0, 0x10000 -> FFE0; 0x31 0x32 0x36 0x38 -> 18; 0x39 0x3A
     * -> 20. kernobj.c:158 -- size 0 is ILLEGAL_MEMSIZE (800201B7). */
    step("vpl: CreateVpl size sweep, ReferVplStatus of each");
    static const w32 sizes[] = { 0, 1, 0x20, 0x21, 0x28, 0x31, 0x32, 0x36, 0x38, 0x39,
                                 0x3A, 0x40, 0x100, 0x1000, 0x10000 };
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        SceUID v = sceKernelCreateVpl("size", 2, 0, sizes[i], NULL);
        out("  size %X: %s", (unsigned)sizes[i], uidstr(v));
        if (v > 0) { vpl_state(v); sceKernelDeleteVpl(v); }
        else out("\n");
    }

    /* kernobj.c:162-169 -- a request the heap cannot meet is NO_MEMORY
     * (80020190): 0x10000000 and 0x02000000 are refused. */
    step("vpl: CreateVpl sizes 0x02000000, 0x10000000, 0x80000000, 0xFFFFFFF8");
    static const w32 big[] = { 0x02000000, 0x10000000, 0x80000000, 0xFFFFFFF8 };
    for (unsigned i = 0; i < sizeof big / sizeof big[0]; i++) {
        SceUID v = sceKernelCreateVpl("big", 2, 0, big[i], NULL);
        out("  size %08X: %s\n", (unsigned)big[i], uidstr(v));
        if (v > 0) sceKernelDeleteVpl(v);
    }

    step("vpl: ReferVplStatus of a 0x1000 pool (attr 100), sizes 52 and 0");
    {
        SceUID v = sceKernelCreateVpl("refer", 2, 0x100, 0x1000, NULL);
        info_prep(52);
        out("  = %08X\n", (unsigned)sceKernelReferVplStatus(v, INFO));
        show_info(VPL_FIELDS);
        info_prep(0);
        out("  size 0: %08X\n", (unsigned)sceKernelReferVplStatus(v, INFO));
        show_info(VPL_FIELDS);
        sceKernelDeleteVpl(v);
    }

    /* kernobj.c:154 -- a NULL name is ERROR (80020001). */
    step("vpl: CreateVpl with NULL name");
    {
        SceUID v = sceKernelCreateVpl(NULL, 2, 0, 0x100, NULL);
        uidret(v);
        if (v > 0) sceKernelDeleteVpl(v);
    }
}

static void sec_vpl_ops(void) {
    section("vpl: allocate, free, layout (one thread)");
    int r, f0, f1;
    SceUInt tmo;

    step("vpl: CreateVpl 0x10000");
    SceUID v = sceKernelCreateVpl("ops", 2, 0, 0x10000, NULL);
    uidret(v);
    vpl_state(v);

    /* kernobj.c:32-34 -- 1 byte costs 16, 16 bytes 24, 8 bytes 16: an 8-byte
     * header and 8-byte rounding. */
    step("vpl: TryAllocateVpl 1, 16, 8 and 0x100; free space used by each");
    w32 blk[4];
    static const w32 req[4] = { 1, 16, 8, 0x100 };
    for (int i = 0; i < 4; i++) {
        f0 = vpl_free_size(v);
        blk[i] = (w32)vtry(v, req[i], &r);
        if (i == 0) g_vref = blk[0];
        f1 = vpl_free_size(v);
        out("  %X: %08X at %s cost %X\n", (unsigned)req[i], (unsigned)r, voff(blk[i]), (unsigned)(f0 - f1));
    }
    /* kernobj.c:213-216, 238-245 -- carved from the top of the free block,
     * so successive blocks descend; each header's next is the pool start. */
    for (int i = 0; i < 4; i++) vpl_hdr("blk", blk[i]);
    vpl_acct(blk[0], 0x10000);

    /* kernobj.c:364-370 -- 0, negative, or more than the pool: all
     * ILLEGAL_MEMSIZE (800201B7), decided before waiting. */
    step("vpl: TryAllocateVpl 0, -1, 0x10000; AllocateVpl 0x10000 timeout 500ms");
    vtry(v, 0, &r); out("  0: %08X\n", (unsigned)r);
    vtry(v, (w32)-1, &r); out("  -1: %08X\n", (unsigned)r);
    vtry(v, 0x10000, &r); out("  0x10000: %08X\n", (unsigned)r);
    {
        void *p = (void *)0xDEADBEEF;
        tmo = 500000;
        r = sceKernelAllocateVpl(v, 0x10000, &p, &tmo);
        out("  wait 0x10000: %08X tmo=%s\n", (unsigned)r, tcls(tmo, 500000));
    }

    step("vpl: TryAllocateVpl 0xFF00 (does not fit); AllocateVpl 0xFF00 timeout 5ms");
    vtry(v, 0xFF00, &r); out("  try: %08X\n", (unsigned)r);
    {
        void *p = (void *)0xDEADBEEF;
        tmo = 5000;
        r = sceKernelAllocateVpl(v, 0xFF00, &p, &tmo);
        out("  wait: %08X got=%s tmo=%s\n", (unsigned)r, voff((w32)p), tcls(tmo, 5000));
    }

    /* kernobj.c:426-443 -- vpl/free: a second free, a stack address, a
     * pointer into a block and another pool's pointer are all
     * ILLEGAL_MEMBLOCK_PTR (800201B6); a null uid with a good pointer is
     * UNKNOWN_VPLID. */
    step("vpl: FreeVpl bad pointers: twice, stack, block+8, another pool's, uid 0");
    {
        int local = 0;
        out("  free blk1: %08X\n", (unsigned)sceKernelFreeVpl(v, (void *)blk[1]));
        out("  free blk1 again: %08X\n", (unsigned)sceKernelFreeVpl(v, (void *)blk[1]));
        out("  free stack: %08X\n", (unsigned)sceKernelFreeVpl(v, &local));
        out("  free blk3+8: %08X\n", (unsigned)sceKernelFreeVpl(v, (void *)(blk[3] + 8)));
        SceUID v2 = sceKernelCreateVpl("other", 2, 0, 0x100, NULL);
        void *o = vtry(v2, 8, &r);
        out("  free other pool's: %08X\n", (unsigned)sceKernelFreeVpl(v, o));
        out("  free with uid 0: %08X\n", (unsigned)sceKernelFreeVpl(0, (void *)blk[2]));
        sceKernelDeleteVpl(v2);
        vpl_state(v);
    }
    sceKernelDeleteVpl(v);

    /* kernobj.c:48-73, 218-336 -- vpl/order walks the block chain of a
     * 0x100 pool: carve from the top of the first fit, coalesce on free,
     * head at the free node before the returned block. */
    step("vpl: 0x100 pool: allocate a b c of 0x10, accounting");
    v = sceKernelCreateVpl("order", 2, 0, 0x100, NULL);
    w32 a = (w32)vtry(v, 0x10, &r);
    g_vref = a;
    w32 b = (w32)vtry(v, 0x10, &r);
    w32 c = (w32)vtry(v, 0x10, &r);
    vpl_hdr("a", a); vpl_hdr("b", b); vpl_hdr("c", c);
    vpl_acct(a, 0x100);
    vpl_state(v);
    step("vpl: free b, accounting; free a, accounting; allocate 8");
    out("  free b: %08X\n", (unsigned)sceKernelFreeVpl(v, (void *)b));
    vpl_acct(a, 0x100);
    out("  free a: %08X\n", (unsigned)sceKernelFreeVpl(v, (void *)a));
    vpl_acct(c, 0x100);
    w32 d = (w32)vtry(v, 8, &r);
    out("  alloc 8: %08X at %s\n", (unsigned)r, voff(d));
    vpl_acct(c, 0x100);
    step("vpl: free c, allocate 0x20, allocate 0x10, free all");
    out("  free c: %08X\n", (unsigned)sceKernelFreeVpl(v, (void *)c));
    w32 e = (w32)vtry(v, 0x20, &r);
    out("  alloc 0x20: %08X at %s\n", (unsigned)r, voff(e));
    w32 f = (w32)vtry(v, 0x10, &r);
    out("  alloc 0x10: %08X at %s\n", (unsigned)r, voff(f));
    vpl_acct(d, 0x100);
    sceKernelFreeVpl(v, (void *)d);
    sceKernelFreeVpl(v, (void *)e);
    sceKernelFreeVpl(v, (void *)f);
    vpl_state(v);
    sceKernelDeleteVpl(v);

    /* kernobj.c:246-255 -- the open question: three adjacent blocks freed
     * in order, then an allocation: hardware reuses the merged hole where
     * the first one was; psprecomp takes the other free region. */
    step("vpl: 0x1000 pool: allocate x y z of 0x20, free x y z, allocate 0x20");
    v = sceKernelCreateVpl("merge", 2, 0, 0x1000, NULL);
    w32 x = (w32)vtry(v, 0x20, &r);
    g_vref = x;
    w32 y = (w32)vtry(v, 0x20, &r);
    w32 z = (w32)vtry(v, 0x20, &r);
    out("  x at %s, y at %s, z at %s\n", voff(x), voff(y), voff(z));
    sceKernelFreeVpl(v, (void *)x);
    sceKernelFreeVpl(v, (void *)y);
    sceKernelFreeVpl(v, (void *)z);
    w32 w = (w32)vtry(v, 0x20, &r);
    out("  alloc 0x20: %08X at %s\n", (unsigned)r, voff(w));
    sceKernelFreeVpl(v, (void *)w);
    step("vpl: same pool: allocate x y z of 0x20 again, free z y x, allocate 0x20");
    x = (w32)vtry(v, 0x20, &r);
    y = (w32)vtry(v, 0x20, &r);
    z = (w32)vtry(v, 0x20, &r);
    out("  x at %s, y at %s, z at %s\n", voff(x), voff(y), voff(z));
    sceKernelFreeVpl(v, (void *)z);
    sceKernelFreeVpl(v, (void *)y);
    sceKernelFreeVpl(v, (void *)x);
    w = (w32)vtry(v, 0x20, &r);
    out("  alloc 0x20: %08X at %s\n", (unsigned)r, voff(w));
    sceKernelDeleteVpl(v);

    /* kernobj.c:238-245 -- first fit from the head: two holes, which one a
     * small request lands in. */
    step("vpl: 0x1000 pool: A 0x100, B 0x10, C 0x100, D 0x10; free A and C; allocate 0x80 twice");
    v = sceKernelCreateVpl("fit", 2, 0, 0x1000, NULL);
    {
        w32 pa = (w32)vtry(v, 0x100, &r);
        g_vref = pa;
        w32 pb = (w32)vtry(v, 0x10, &r);
        w32 pc = (w32)vtry(v, 0x100, &r);
        w32 pd = (w32)vtry(v, 0x10, &r);
        out("  A at %s, B at %s, C at %s, D at %s\n", voff(pa), voff(pb), voff(pc), voff(pd));
        sceKernelFreeVpl(v, (void *)pa);
        sceKernelFreeVpl(v, (void *)pc);
        w32 p1 = (w32)vtry(v, 0x80, &r);
        out("  alloc 0x80: %08X at %s\n", (unsigned)r, voff(p1));
        w32 p2 = (w32)vtry(v, 0x80, &r);
        out("  alloc 0x80: %08X at %s\n", (unsigned)r, voff(p2));
        vpl_acct(pb, 0x1000);
    }
    sceKernelDeleteVpl(v);

    /* kernobj.c:70-73, 166-168 -- vpl/order reaches the middle of three
     * pools as `addr3 + 0x18`: pools are allocated from the high end, so
     * three created in order descend. */
    step("vpl: three 0x100 pools created in order; where their first blocks are");
    {
        SceUID pv[3];
        w32 pb[3];
        for (int i = 0; i < 3; i++) {
            pv[i] = sceKernelCreateVpl("desc", 2, 0, 0x100, NULL);
            pb[i] = (w32)vtry(pv[i], 8, &r);
        }
        g_vref = pb[0];
        out("  pool2 %s, pool3 %s (from pool1's block)\n", voff(pb[1]), voff(pb[2]));
        for (int i = 0; i < 3; i++) sceKernelDeleteVpl(pv[i]);
    }
}

/* Fill a 0x1000 pool with 0x100 blocks; what is left is less than one. */
static int vpl_fill(SceUID v, w32 *blk, int max) {
    int n = 0, r;
    while (n < max) {
        void *p = vtry(v, 0x100, &r);
        if (r) break;
        blk[n++] = (w32)p;
    }
    return n;
}

static void vpl_order(w32 attr) {
    order_reset();
    step("vpl: attr %X, 0x1000 pool full of 0x100 blocks; A(16) B(13) C(18) D(14) want 0x100", (unsigned)attr);
    SceUID v = sceKernelCreateVpl("order", 2, attr, 0x1000, NULL);
    w32 blk[32];
    int n = vpl_fill(v, blk, 32);
    g_vref = n ? blk[0] : 0;
    out("  blocks: %d\n", n);
    vpl_state(v);
    for (int i = 0; i < 4; i++) spawn(job(i, jf_allocvpl, v, 0x100, 0), ARRIVE[i]);
    vpl_state(v);
    step("vpl: free one block at a time, four times");
    for (int i = 0; i < 4 && i < n; i++) {
        out("  free: %08X\n", (unsigned)sceKernelFreeVpl(v, (void *)blk[i]));
        settle();
    }
    g_ptrname = voff;
    show_jobs(4, 'p');
    vpl_state(v);
    sceKernelDeleteVpl(v);
    reap_all();
}

static void sec_vpl_threads(void) {
    section("vpl: waiters");
    SceUID v;
    w32 blk[32];
    int n, r;

    /* waitq.h:3-9 -- 0x100 selects most-urgent-first. */
    vpl_order(0);
    vpl_order(0x100);

    /* kernobj.c:338-351 -- releasing stops at the first waiter in order it
     * cannot satisfy; kernobj.c:375-380 -- but a new request walks past the
     * waiters and takes what fits. */
    order_reset();
    step("vpl: FIFO, full pool; A wants 0x200, then B 0x80; free one 0x100 block");
    v = sceKernelCreateVpl("hol", 2, 0, 0x1000, NULL);
    n = vpl_fill(v, blk, 32);
    g_vref = n ? blk[0] : 0;
    spawn(job(0, jf_allocvpl, v, 0x200, 0), 0x16);
    spawn(job(1, jf_allocvpl, v, 0x80, 0), 0x15);
    out("  free: %08X\n", (unsigned)sceKernelFreeVpl(v, (void *)blk[0]));
    settle();
    vpl_state(v);
    show_order();
    step("vpl: main TryAllocateVpl 0x10 with the waiters queued");
    {
        void *p = vtry(v, 0x10, &r);
        out("  = %08X at %s\n", (unsigned)r, voff((w32)p));
    }
    vpl_state(v);
    step("vpl: free two more blocks");
    sceKernelFreeVpl(v, (void *)blk[1]);
    settle();
    sceKernelFreeVpl(v, (void *)blk[2]);
    settle();
    g_ptrname = voff;
    show_jobs(2, 'p');
    vpl_state(v);
    sceKernelDeleteVpl(v);
    reap_all();

    /* hle.h:137-142 -- WAIT_CANCEL (800201A9). */
    order_reset();
    step("vpl: full pool, A B (prio 18) want 0x100; CancelVpl");
    v = sceKernelCreateVpl("cancel", 2, 0, 0x1000, NULL);
    vpl_fill(v, blk, 32);
    spawn(job(0, jf_allocvpl, v, 0x100, 0), 0x18);
    spawn(job(1, jf_allocvpl, v, 0x100, 0), 0x18);
    {
        int w = 99;
        r = sceKernelCancelVpl(v, &w);
        settle();
        out("  = %08X waiters=%d\n", (unsigned)r, w);
    }
    show_jobs(2, 'p');
    vpl_state(v);
    sceKernelDeleteVpl(v);
    reap_all();

    /* kernobj.c:191-199, hle.h:185-187 -- WAIT_DELETE (800201B5). */
    order_reset();
    step("vpl: full pool, A B (prio 18) want 0x100; DeleteVpl");
    v = sceKernelCreateVpl("del", 2, 0, 0x1000, NULL);
    vpl_fill(v, blk, 32);
    spawn(job(0, jf_allocvpl, v, 0x100, 0), 0x18);
    spawn(job(1, jf_allocvpl, v, 0x100, 0), 0x18);
    out("  = %08X\n", (unsigned)sceKernelDeleteVpl(v));
    settle();
    show_jobs(2, 'p');
    reap_all();
}

/* ======================================================================== */
/* Fixed-size pools                                                         */
/* ======================================================================== */

static void *ftry(SceUID f, int *rp) {
    void *p = (void *)0xDEADBEEF;
    *rp = sceKernelTryAllocateFpl(f, &p);
    return p;
}

struct fplopt { SceSize size; w32 align; };

static void sec_fpl_create(void) {
    section("fpl: create");

    /* kernobj.c:1470-1471 -- the vpl's partition table. */
    step("fpl: CreateFpl partition sweep");
    static const int parts[] = { 2, 6, 1, 3, 8, 0, 7, -1 };
    for (unsigned i = 0; i < sizeof parts / sizeof parts[0]; i++) {
        SceUID f = sceKernelCreateFpl("part", parts[i], 0, 0x10, 4, NULL);
        out("  partition %d: %s\n", parts[i], uidstr(f));
        if (f > 0) sceKernelDeleteFpl(f);
    }

    /* kernobj.c:1417-1421 -- fpl/create: 0x1, 0x100, 0x4000, 0x41FF
     * accepted; 0x200, 0x300, 0x400, 0x800, 0x1000, 0x2000, 0x8000 ...
     * refused. */
    step("fpl: CreateFpl attr sweep");
    static const w32 attrs[] = { 0x0, 0x1, 0x100, 0x1FF, 0x200, 0x300, 0x400, 0x800,
                                 0x1000, 0x2000, 0x4000, 0x41FF, 0x8000, 0x10000 };
    for (unsigned i = 0; i < sizeof attrs / sizeof attrs[0]; i++) {
        SceUID f = sceKernelCreateFpl("attr", 2, attrs[i], 0x10, 4, NULL);
        out("  attr %05X: %s\n", (unsigned)attrs[i], uidstr(f));
        if (f > 0) sceKernelDeleteFpl(f);
    }

    /* kernobj.c:1473-1479 -- zero or negative size or count is
     * ILLEGAL_MEMSIZE (800201B7), and so is a pool whose size overflows a
     * word (0x04000000 blocks of 0x100). kernobj.c:1415, 1479 -- psprecomp
     * refuses more than 1024 blocks as NO_MEMORY, a limit of its own. */
    step("fpl: CreateFpl block size / count combinations");
    static const w32 sc[][2] = { {0, 4}, {0x10, 0}, {(w32)-1, 4}, {0x10, (w32)-1},
                                 {0x100, 0x04000000}, {0x10, 0x400}, {0x10, 0x2000},
                                 {0x2F, 0x2F}, {1, 1} };
    for (unsigned i = 0; i < sizeof sc / sizeof sc[0]; i++) {
        SceUID f = sceKernelCreateFpl("size", 2, 0, sc[i][0], sc[i][1], NULL);
        out("  bsize %X count %X: %s", (unsigned)sc[i][0], (unsigned)sc[i][1], uidstr(f));
        if (f > 0) {
            SceKernelFplInfo inf;
            inf.size = sizeof inf;
            sceKernelReferFplStatus(f, &inf);
            out(" -> bsize=%X blocks=%X free=%X", inf.blockSize, inf.numBlocks, inf.freeBlocks);
            sceKernelDeleteFpl(f);
        }
        out("\n");
    }

    /* kernobj.c:1481-1494 -- the option's second word is an alignment:
     * 0, 1, 2, 4, 8 accepted; -1, 3, 5, 6, 7 refused with 800200D2. */
    step("fpl: CreateFpl option alignment sweep (block 0x10, 4 blocks)");
    static const w32 al[] = { 0, 1, 2, 4, 8, 32, 0x1000, 3, 5, 6, 7, 12, (w32)-1 };
    for (unsigned i = 0; i < sizeof al / sizeof al[0]; i++) {
        struct fplopt o = { sizeof o, al[i] };
        SceUID f = sceKernelCreateFpl("align", 2, 0, 0x10, 4, (void *)&o);
        out("  align %X: %s\n", (unsigned)al[i], uidstr(f));
        if (f > 0) sceKernelDeleteFpl(f);
    }

    step("fpl: ReferFplStatus of a pool (attr 100, 0x2F x 3), sizes 56 and 0");
    {
        SceUID f = sceKernelCreateFpl("refer", 2, 0x100, 0x2F, 3, NULL);
        info_prep(56);
        out("  = %08X\n", (unsigned)sceKernelReferFplStatus(f, INFO));
        show_info(FPL_FIELDS);
        info_prep(0);
        out("  size 0: %08X\n", (unsigned)sceKernelReferFplStatus(f, INFO));
        show_info(FPL_FIELDS);
        sceKernelDeleteFpl(f);
    }

    /* kernobj.c:1467-1469 -- a NULL name is NO_MEMORY (80020190). */
    step("fpl: CreateFpl with NULL name");
    {
        SceUID f = sceKernelCreateFpl(NULL, 2, 0, 0x10, 4, NULL);
        uidret(f);
        if (f > 0) sceKernelDeleteFpl(f);
    }
}

/* The spacing between two consecutive blocks of a new pool. */
static void fpl_spacing(const char *what, w32 bsize, w32 align) {
    struct fplopt o = { sizeof o, align };
    SceUID f = sceKernelCreateFpl("space", 2, 0, bsize, 4, align == 0xFFFFFFFFu ? NULL : (void *)&o);
    int r;
    w32 a = (w32)ftry(f, &r);
    w32 b = (w32)ftry(f, &r);
    g_vref = a;
    out("  %s: %s, second block at %s\n", what, uidstr(f), voff(b));
    sceKernelDeleteFpl(f);
}

static void sec_fpl_ops(void) {
    section("fpl: allocate, free, layout (one thread)");
    int r;
    SceUInt tmo;

    /* kernobj.c:1395-1403 -- no header and no rounding: consecutive blocks
     * of a 16-byte pool are 16 bytes apart. kernobj.c:1431-1437, 1532-1540
     * -- the free list is a queue: a freed block goes to the back, so the
     * next allocation keeps climbing rather than reusing the hole. */
    step("fpl: 0x10 x 4 pool: allocate #1 #2 #3, free #1, allocate #4 #5, then one too many");
    SceUID f = sceKernelCreateFpl("ops", 2, 0, 0x10, 4, NULL);
    uidret(f);
    w32 p1 = (w32)ftry(f, &r);
    g_vref = p1;
    w32 p2 = (w32)ftry(f, &r);
    w32 p3 = (w32)ftry(f, &r);
    out("  #1 at %s, #2 at %s, #3 at %s\n", voff(p1), voff(p2), voff(p3));
    out("  free #1: %08X\n", (unsigned)sceKernelFreeFpl(f, (void *)p1));
    w32 p4 = (w32)ftry(f, &r);
    out("  #4: %08X at %s\n", (unsigned)r, voff(p4));
    w32 p5 = (w32)ftry(f, &r);
    out("  #5: %08X at %s\n", (unsigned)r, voff(p5));
    w32 p6 = (w32)ftry(f, &r);
    out("  #6: %08X at %s\n", (unsigned)r, voff(p6));
    fpl_state(f);
    step("fpl: free #3 then #2, allocate twice");
    sceKernelFreeFpl(f, (void *)p3);
    sceKernelFreeFpl(f, (void *)p2);
    w32 p7 = (w32)ftry(f, &r);
    out("  #7: %08X at %s\n", (unsigned)r, voff(p7));
    w32 p8 = (w32)ftry(f, &r);
    out("  #8: %08X at %s\n", (unsigned)r, voff(p8));

    step("fpl: AllocateFpl on an empty pool, timeout 5ms");
    {
        void *p = (void *)0xDEADBEEF;
        tmo = 5000;
        r = sceKernelAllocateFpl(f, &p, &tmo);
        out("  = %08X got=%s tmo=%s\n", (unsigned)r, voff((w32)p), tcls(tmo, 5000));
    }

    /* kernobj.c:1629-1650 -- fpl: the uid is checked first; a pointer that
     * is not the start of a live block of this pool is ILLEGAL_MEMBLOCK_PTR
     * (800201B6). */
    step("fpl: FreeFpl bad pointers: twice, stack, block+4, another pool's, uid 0");
    {
        int local = 0;
        out("  free #7: %08X\n", (unsigned)sceKernelFreeFpl(f, (void *)p7));
        out("  free #7 again: %08X\n", (unsigned)sceKernelFreeFpl(f, (void *)p7));
        out("  free stack: %08X\n", (unsigned)sceKernelFreeFpl(f, &local));
        out("  free #8+4: %08X\n", (unsigned)sceKernelFreeFpl(f, (void *)(p8 + 4)));
        SceUID f2 = sceKernelCreateFpl("other", 2, 0, 0x10, 2, NULL);
        void *o = ftry(f2, &r);
        out("  free other pool's: %08X\n", (unsigned)sceKernelFreeFpl(f, o));
        out("  free with uid 0: %08X\n", (unsigned)sceKernelFreeFpl(0, (void *)p8));
        sceKernelDeleteFpl(f2);
        fpl_state(f);
    }
    sceKernelDeleteFpl(f);

    /* kernobj.c:1495-1499 -- fpl/tryallocate: alignment 32 spaces 0x10
     * blocks 32 apart; kernobj.c:1486 -- without options blocks are rounded
     * to 4. */
    step("fpl: spacing of two blocks for block sizes and alignments");
    fpl_spacing("0x10 no opt", 0x10, 0xFFFFFFFFu);
    fpl_spacing("0x10 align 32", 0x10, 32);
    fpl_spacing("0x10 align 0", 0x10, 0);
    fpl_spacing("1 no opt", 1, 0xFFFFFFFFu);
    fpl_spacing("1 align 1", 1, 1);
    fpl_spacing("0x11 no opt", 0x11, 0xFFFFFFFFu);
    fpl_spacing("0x11 align 8", 0x11, 8);
    fpl_spacing("0x2F no opt", 0x2F, 0xFFFFFFFFu);
}

static void fpl_order(w32 attr) {
    order_reset();
    step("fpl: attr %X, 1-block pool held by main; A(16) B(13) C(18) D(14) allocate, free at once", (unsigned)attr);
    SceUID f = sceKernelCreateFpl("order", 2, attr, 0x10, 1, NULL);
    int r;
    void *held = ftry(f, &r);
    g_vref = (w32)held;
    for (int i = 0; i < 4; i++) {
        job_t *j = job(i, jf_allocfpl, f, 0, 0);
        j->post = pf_freefpl;
        spawn(j, ARRIVE[i]);
    }
    fpl_state(f);
    step("fpl: main FreeFpl");
    out("  = %08X\n", (unsigned)sceKernelFreeFpl(f, held));
    settle();
    g_ptrname = voff;
    show_jobs(4, 'p');
    fpl_state(f);
    sceKernelDeleteFpl(f);
    reap_all();
}

static void sec_fpl_threads(void) {
    section("fpl: waiters");
    SceUID f;
    int r;

    fpl_order(0);
    fpl_order(0x100);

    /* hle.h:137-142 -- WAIT_CANCEL (800201A9). */
    order_reset();
    step("fpl: empty pool, A B (prio 18) allocate; CancelFpl");
    f = sceKernelCreateFpl("cancel", 2, 0, 0x10, 1, NULL);
    ftry(f, &r);
    spawn(job(0, jf_allocfpl, f, 0, 0), 0x18);
    spawn(job(1, jf_allocfpl, f, 0, 0), 0x18);
    {
        int w = 99;
        r = sceKernelCancelFpl(f, &w);
        settle();
        out("  = %08X waiters=%d\n", (unsigned)r, w);
    }
    g_ptrname = voff;
    show_jobs(2, 'p');
    fpl_state(f);
    sceKernelDeleteFpl(f);
    reap_all();

    /* kernobj.c:1522-1530, hle.h:185-187 -- WAIT_DELETE (800201B5). */
    order_reset();
    step("fpl: empty pool, A B (prio 18) allocate; DeleteFpl");
    f = sceKernelCreateFpl("del", 2, 0, 0x10, 1, NULL);
    ftry(f, &r);
    spawn(job(0, jf_allocfpl, f, 0, 0), 0x18);
    spawn(job(1, jf_allocfpl, f, 0, 0), 0x18);
    out("  = %08X\n", (unsigned)sceKernelDeleteFpl(f));
    settle();
    show_jobs(2, 'p');
    reap_all();
}

/* ======================================================================== */
/* Release, then delete at once                                             */
/* ======================================================================== */

/* waitq.h:79-92 -- psprecomp's own note: a waiter handed the object and then
 * deleted before it runs is told OK by hardware in fpl/allocate
 * (kernobj.c:1558-1562) and msgpipe/tryreceive (kernobj.c:716-721), and
 * psprecomp says WAIT_DELETE for the other types. Here the waiter is LESS
 * urgent than main (0x30), so nothing runs between the release and the
 * delete, and nothing is logged between them either. */
static void sec_release_delete(void) {
    section("release, then delete before the waiter runs");
    int r1, r2;
    SceUID o;
    w32 b;

    order_reset();
    step("rd: sema, A(30) waits; SignalSema 1 then DeleteSema");
    o = sceKernelCreateSema("rd", 0, 0, 1, NULL);
    spawn(job(0, jf_waitsema, o, 1, 0), 0x30);
    r1 = sceKernelSignalSema(o, 1);
    r2 = sceKernelDeleteSema(o);
    settle();
    out("  signal=%08X delete=%08X\n", (unsigned)r1, (unsigned)r2);
    show_jobs(1, 0);
    reap_all();

    order_reset();
    step("rd: event flag, A(30) waits bit 1; SetEventFlag 1 then DeleteEventFlag");
    o = sceKernelCreateEventFlag("rd", 0, 0, NULL);
    spawn(job(0, jf_waitevf, o, 1, WAITOR), 0x30);
    r1 = sceKernelSetEventFlag(o, 1);
    r2 = sceKernelDeleteEventFlag(o);
    settle();
    out("  set=%08X delete=%08X\n", (unsigned)r1, (unsigned)r2);
    show_jobs(1, 'x');
    reap_all();

    order_reset();
    step("rd: mutex held by main, A(30) waits; UnlockMutex then DeleteMutex");
    o = sceKernelCreateMutex("rd", 0, 1, NULL);
    spawn(job(0, jf_lockmtx, o, 1, 0), 0x30);
    r1 = sceKernelUnlockMutex(o, 1);
    r2 = sceKernelDeleteMutex(o);
    settle();
    out("  unlock=%08X delete=%08X\n", (unsigned)r1, (unsigned)r2);
    show_jobs(1, 0);
    reap_all();

    order_reset();
    step("rd: lwmutex held by main, A(30) waits; UnlockLwMutex then DeleteLwMutex");
    lw_create(&g_wa[0], "rd", 0, 1);
    {
        job_t *j = job(0, jf_locklw, 0, 1, 0);
        j->p = &g_wa[0];
        spawn(j, 0x30);
    }
    r1 = sceKernelUnlockLwMutex(&g_wa[0], 1);
    r2 = sceKernelDeleteLwMutex(&g_wa[0]);
    settle();
    out("  unlock=%08X delete=%08X\n", (unsigned)r1, (unsigned)r2);
    show_jobs(1, 0);
    show_wa("wa", &g_wa[0]);
    reap_all();

    order_reset();
    g_ptrname = msgname;
    step("rd: mailbox, A(30) receives; SendMbx m1 then DeleteMbx");
    o = sceKernelCreateMbx("rd", 0, NULL);
    spawn(job(0, jf_recvmbx, o, 0, 0), 0x30);
    msg_prep(0, 0);
    r1 = sceKernelSendMbx(o, &g_msg[0]);
    r2 = sceKernelDeleteMbx(o);
    settle();
    out("  send=%08X delete=%08X\n", (unsigned)r1, (unsigned)r2);
    show_jobs(1, 'p');
    reap_all();

    order_reset();
    step("rd: full vpl, A(30) wants 0x100; FreeVpl then DeleteVpl");
    {
        w32 blk[32];
        o = sceKernelCreateVpl("rd", 2, 0, 0x1000, NULL);
        int n = vpl_fill(o, blk, 32);
        g_vref = n ? blk[0] : 0;
        spawn(job(0, jf_allocvpl, o, 0x100, 0), 0x30);
        r1 = sceKernelFreeVpl(o, (void *)blk[0]);
        r2 = sceKernelDeleteVpl(o);
        settle();
        out("  free=%08X delete=%08X\n", (unsigned)r1, (unsigned)r2);
        g_ptrname = voff;
        show_jobs(1, 'p');
    }
    reap_all();

    order_reset();
    step("rd: empty fpl, A(30) allocates; FreeFpl then DeleteFpl");
    {
        int r;
        o = sceKernelCreateFpl("rd", 2, 0, 0x10, 1, NULL);
        void *held = ftry(o, &r);
        g_vref = (w32)held;
        spawn(job(0, jf_allocfpl, o, 0, 0), 0x30);
        r1 = sceKernelFreeFpl(o, held);
        r2 = sceKernelDeleteFpl(o);
        settle();
        out("  free=%08X delete=%08X\n", (unsigned)r1, (unsigned)r2);
        g_ptrname = voff;
        show_jobs(1, 'p');
    }
    reap_all();

    order_reset();
    step("rd: no-buffer pipe, A(30) B(30) send 4; TryReceive 8 then DeleteMsgPipe");
    o = mpp_create("rd", 2, 0, 0);
    for (int i = 0; i < 2; i++) {
        job_t *j = job(i, jf_sendmpp, o, 4, 0);
        memset(j->buf, 'A' + i, 4);
        j->out = 0x1337;
        spawn(j, 0x30);
    }
    {
        char d[9] = { 0 };
        b = 0x1337;
        r1 = sceKernelTryReceiveMsgPipe(o, d, 8, 0, &b);
        r2 = sceKernelDeleteMsgPipe(o);
        settle();
        out("  receive=%08X bytes=%08X data=\"%s\" delete=%08X\n", (unsigned)r1,
            (unsigned)b, d, (unsigned)r2);
    }
    show_jobs(2, 'x');
    reap_all();
}

/* ======================================================================== */
/* NULL and bad pointers, and a tampered mailbox: last, so that a crash     */
/* here loses nothing else.                                                 */
/* ======================================================================== */

static void sec_pointers(void) {
    section("NULL and bad pointers");
    int r;

    /* threadman.c:1634-1635 and the other Refer*Status -- a NULL info
     * pointer is ILLEGAL_ADDR (80020005) in psprecomp. On firmware 6.60 this
     * step switched the PSP off (syncprobe 1, 2026-09-28); which of the eight
     * calls did it is not known, since the log is written per step. */
    if (!step("ptr: Refer*Status with a NULL info pointer, every type") &&
        !KNOWN_CRASH("switched the PSP off on firmware 6.60 (syncprobe 1)")) {
        SceUID s = sceKernelCreateSema("p", 0, 0, 1, NULL);
        out("  sema: %08X\n", (unsigned)sceKernelReferSemaStatus(s, NULL));
        sceKernelDeleteSema(s);
        s = sceKernelCreateEventFlag("p", 0, 0, NULL);
        out("  evf: %08X\n", (unsigned)sceKernelReferEventFlagStatus(s, NULL));
        sceKernelDeleteEventFlag(s);
        s = sceKernelCreateMutex("p", 0, 0, NULL);
        out("  mutex: %08X\n", (unsigned)sceKernelReferMutexStatus(s, NULL));
        sceKernelDeleteMutex(s);
        s = sceKernelCreateMbx("p", 0, NULL);
        out("  mbx: %08X\n", (unsigned)sceKernelReferMbxStatus(s, NULL));
        sceKernelDeleteMbx(s);
        s = mpp_create("p", 2, 0, 0x10);
        out("  mpp: %08X\n", (unsigned)sceKernelReferMsgPipeStatus(s, NULL));
        sceKernelDeleteMsgPipe(s);
        s = sceKernelCreateVpl("p", 2, 0, 0x100, NULL);
        out("  vpl: %08X\n", (unsigned)sceKernelReferVplStatus(s, NULL));
        sceKernelDeleteVpl(s);
        s = sceKernelCreateFpl("p", 2, 0, 0x10, 1, NULL);
        out("  fpl: %08X\n", (unsigned)sceKernelReferFplStatus(s, NULL));
        sceKernelDeleteFpl(s);
        lw_create(&g_wa[0], "p", 0, 0);
        out("  lwmutex: %08X\n", (unsigned)sceKernelReferLwMutexStatusByID(wa_uid(&g_wa[0]), NULL));
        sceKernelDeleteLwMutex(&g_wa[0]);
    }

    /* kernobj.c:426-435 -- FreeVpl(NULL) is ILLEGAL_MEMBLOCK_PTR; a pointer
     * that is not mapped memory is 800200D3. kernobj.c:1635-1638 -- the
     * same split for FreeFpl. One call per step, so a call that switches
     * the PSP off costs only itself: the next start skips it. */
    {
        static const struct { const char *what; int fpl, uid0; w32 ptr; } fr[] = {
            { "FreeVpl(vpl, NULL)", 0, 0, 0 },    { "FreeVpl(vpl, 0x10)", 0, 0, 0x10 },
            { "FreeVpl(0, 0x10)", 0, 1, 0x10 },   { "FreeFpl(fpl, NULL)", 1, 0, 0 },
            { "FreeFpl(fpl, 0x10)", 1, 0, 0x10 }, { "FreeFpl(0, 0x10)", 1, 1, 0x10 },
        };
        for (int i = 0; i < (int)(sizeof fr / sizeof fr[0]); i++) {
            if (step("ptr: %s", fr[i].what)) continue;
            if (fr[i].fpl) {
                SceUID f = sceKernelCreateFpl("p", 2, 0, 0x10, 1, NULL);
                out("  = %08X\n", (unsigned)sceKernelFreeFpl(fr[i].uid0 ? 0 : f, (void *)fr[i].ptr));
                sceKernelDeleteFpl(f);
            } else {
                SceUID v = sceKernelCreateVpl("p", 2, 0, 0x100, NULL);
                out("  = %08X\n", (unsigned)sceKernelFreeVpl(fr[i].uid0 ? 0 : v, (void *)fr[i].ptr));
                sceKernelDeleteVpl(v);
            }
        }
    }

    /* kernobj.c:1244 -- SendMbx(NULL) is ILLEGAL_ADDR (80020005). Sending
     * writes the message's link word, the same kind of kernel access through
     * a NULL pointer as the Refer*Status step above. */
    if (!step("ptr: SendMbx with a NULL message") &&
        !KNOWN_CRASH("the kernel would write through a NULL message, like the Refer*Status crash")) {
        SceUID m = sceKernelCreateMbx("p", 0, NULL);
        out("  = %08X\n", (unsigned)sceKernelSendMbx(m, NULL));
        mbx_state(m);
        sceKernelDeleteMbx(m);
    }

    /* kernlock.c:440-445, 490-492 -- lwmutex/delete's "Invalid" case:
     * DeleteLwMutex(NULL) is 800200D3. */
    if (!step("ptr: DeleteLwMutex(NULL)"))
        out("  = %08X\n", (unsigned)sceKernelDeleteLwMutex(NULL));

    /* kernobj.c:1112-1129 -- mbx/send rewrites the last message's `next`
     * and reads the box back: next = itself gives first = that message. */
    g_ptrname = msgname;
    if (!step("ptr: mailbox m1 m2; set m2.next = m2; refer, poll twice")) {
        SceUID m = sceKernelCreateMbx("tamper", 0, NULL);
        msg_prep(0, 0);
        msg_prep(1, 0);
        sceKernelSendMbx(m, &g_msg[0]);
        sceKernelSendMbx(m, &g_msg[1]);
        g_msg[1].next = (w32)&g_msg[1];
        mbx_state(m);
        poll_mbx("poll", m);
        show_links(2);
        mbx_state(m);
        poll_mbx("poll", m);
        show_links(2);
        mbx_state(m);
        sceKernelDeleteMbx(m);
    }

    /* kernobj.c:1288-1302 -- next = NULL: count still 2, first NULL, and a
     * receive answers 800200D3. */
    if (!step("ptr: mailbox m1 m2; set m2.next = NULL; refer, poll")) {
        SceUID m = sceKernelCreateMbx("tamper", 0, NULL);
        msg_prep(0, 0);
        msg_prep(1, 0);
        sceKernelSendMbx(m, &g_msg[0]);
        sceKernelSendMbx(m, &g_msg[1]);
        g_msg[1].next = 0;
        mbx_state(m);
        poll_mbx("poll", m);
        mbx_state(m);
        msg_prep(2, 0);
        r = sceKernelSendMbx(m, &g_msg[2]);
        out("  send m3: %08X\n", (unsigned)r);
        mbx_state(m);
        sceKernelDeleteMbx(m);
    }
}

int main(int argc, char **argv) {
    probe_init("syncprobe", PROBE_VERSION, argc, argv);
    g_main = sceKernelGetThreadId();
    out("main thread priority %X\n", (unsigned)sceKernelGetThreadCurrentPriority());
    if (pool_open() < 0) out("(worker pool incomplete)\n");

    sec_sema_create();
    sec_sema_ops();
    sec_sema_threads();
    sec_evf_create();
    sec_evf_ops();
    sec_evf_threads();
    sec_mutex_create();
    sec_mutex_ops();
    sec_mutex_threads();
    sec_lw_create();
    sec_lw_ops();
    sec_lw_threads();
    sec_mbx_create();
    sec_mbx_ops();
    sec_mbx_threads();
    sec_mpp_create();
    sec_mpp_ops();
    sec_mpp_threads();
    sec_vpl_create();
    sec_vpl_ops();
    sec_vpl_threads();
    sec_fpl_create();
    sec_fpl_ops();
    sec_fpl_threads();
    sec_release_delete();
    sec_pointers();

    pool_close();
    probe_done();
    return 0;
}
