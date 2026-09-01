/* psprecomp — ThreadManForUser.
 *
 * Threads, semaphores and event flags. This is the largest single dependency
 * any PSP game has: `module_start` typically does nothing but create a thread,
 * start it, and return — so until this works, a recompiled module runs about
 * forty instructions and stops.
 *
 * ## The execution model
 *
 * Threads are real: each guest thread gets a host thread, and a handoff token
 * keeps exactly one of them running at a time, the way a single-core PSP does.
 * The scheduler is src/hle/sched.c; the header there explains why saving the
 * register file is not enough to park a thread.
 *
 * What this module owns is what a thread *is* -- its stack, priority, exit
 * status, and the kernel objects it waits on. Waits park the caller and signals
 * release it.
 *
 * The one place the model still shows through: with no preemption and no clock,
 * a wait that nothing could ever satisfy -- because no other thread is runnable
 * -- cannot be waited out. A caller that supplied a timeout is told it elapsed;
 * one that did not is not lied to, because a fabricated timeout is something a
 * game acts on. See wait_deadlock. sceKernelDelayThread yields rather than
 * sleeping, for the same want of a clock.
 */

#include "psprecomp/hle.h"
#include "psprecomp/dispatch.h"
#include "psprecomp/clock.h"
#include "psprecomp/sched.h"
#include "psprecomp/mem.h"
#include "waitq.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_THREADS 128
#define MAX_SEMAS   128
#define MAX_FLAGS   128
#define MAX_CBS     64
#define UID_BASE    0x00040000u

enum { TH_DORMANT = 0, TH_READY, TH_RUNNING, TH_SUSPENDED };

#define MAX_SEMA_WAITERS 32

typedef struct {
    uint32_t uid;
    char     name[32];
    uint32_t entry;
    uint32_t priority;
    uint32_t stack_size;
    uint32_t stack_base;   /* low address of the allocation */
    uint32_t attr;
    int      state;
    uint32_t exit_status;
    /* Threads parked in sceKernelWaitThreadEnd on this one. */
    uint32_t enders[MAX_SEMA_WAITERS];
    int      nenders;
    int      used;
    jmp_buf  unwind;       /* where sceKernelExitThread returns to */
    int      unwind_set;
} psp_thread;

typedef struct {
    uint32_t uid;
    char     name[32];
    int32_t  count;
    int32_t  max_count;
    int32_t  init_count;      /* what it was created with; ReferSemaStatus reports it */
    uint32_t attr;
    int      used;
    /* Who is parked on it, in the order the attribute says to release them.
     * The signaller deducts the count on a waiter's behalf and wakes only the
     * ones it satisfied -- see waitq.h for why waking everyone to re-test is
     * not a conservative version of that but a different, measurable rule. */
    psp_waitq q;
    /* "sceKernelWaitSema(<name>)", built once at creation.
     *
     * The scheduler stores the string it is handed and prints it for every
     * parked thread, so naming the *object* here is the difference between
     * knowing a thread waits on a semaphore and knowing which one nobody is
     * signalling. Held in the semaphore because the slot only keeps a pointer,
     * and it has to outlive the call that blocked. */
    char     waitdesc[64];
} psp_sema;

typedef struct {
    uint32_t  uid;
    char      name[32];
    uint32_t  pattern;
    uint32_t  init_pattern;
    uint32_t  attr;
    int       used;
    psp_waitq q;
    char      waitdesc[64];
} psp_evflag;

typedef struct {
    uint32_t uid;
    char     name[32];
    uint32_t func;
    uint32_t arg;
    uint32_t thread;          /* the thread that created it */
    uint32_t notify_count;
    uint32_t notify_arg;
    int      used;
} psp_callback;

static psp_thread   g_thread[MAX_THREADS];
static psp_sema     g_sema[MAX_SEMAS];
static psp_evflag   g_flag[MAX_FLAGS];
static psp_callback g_cb[MAX_CBS];
static uint32_t     g_next_uid;
/* The thread the scheduler says is running, as a thread-manager object.
 *
 * There is no separate notion of "current" any more: the scheduler owns that,
 * and a second copy maintained here would be a second thing to keep in step.
 * Returns NULL on the main context, which is not a guest thread. */
static psp_thread *current_thread(void);
static int          g_warned_block;

void psp_threadman_reset(void) {
    memset(g_thread, 0, sizeof g_thread);
    memset(g_sema, 0, sizeof g_sema);
    memset(g_flag, 0, sizeof g_flag);
    memset(g_cb, 0, sizeof g_cb);
    g_next_uid = UID_BASE;
    g_warned_block = 0;
    /* The clock first: the scheduler stamps the main context's timeslice from
     * it, so resetting time afterwards would leave that stamp in the future. */
    psp_clock_reset();
    psp_sched_reset();
}

static void on_thread_end(uint32_t uid, uint32_t status);

void psp_threadman_init(void) {
    psp_sched_set_end_hook(on_thread_end);
    psp_threadman_reset();
}

/* Typed lookups rather than one generic macro. A macro taking a parameter
 * named `uid` also rewrites every `.uid` member access it expands around,
 * which is exactly the kind of subtlety not worth inviting to save twelve
 * lines. */
static psp_thread *find_thread(uint32_t id) {
    for (int i = 0; i < MAX_THREADS; i++)
        if (g_thread[i].used && g_thread[i].uid == id) return &g_thread[i];
    return NULL;
}
static psp_thread *current_thread(void) {
    const uint32_t uid = psp_sched_current();
    return uid ? find_thread(uid) : NULL;
}

static psp_sema *find_sema(uint32_t id) {
    for (int i = 0; i < MAX_SEMAS; i++)
        if (g_sema[i].used && g_sema[i].uid == id) return &g_sema[i];
    return NULL;
}
static psp_evflag *find_flag(uint32_t id) {
    for (int i = 0; i < MAX_FLAGS; i++)
        if (g_flag[i].used && g_flag[i].uid == id) return &g_flag[i];
    return NULL;
}

/* Report a wait that nothing could satisfy, exactly once. Repeating it for
 * every frame of a game that polls a semaphore drowns out everything else. */
/* A wait that can never complete, made by a caller that asked for no timeout.
 *
 * Reporting a timeout here is a lie, and a load-bearing one: this game's
 * user_main waits on its game thread with no timeout, and on being told the
 * wait timed out it deletes that thread -- still live, mid-frame -- and quits.
 * The whole run ended in a screen clear because of it.
 *
 * So the run stops and says why. A caller that *did* pass a timeout still gets
 * one, because for that caller a timeout is a true answer. */
static void wait_deadlock(const char *what) {
    fprintf(stderr,
        "psprecomp: %s cannot be satisfied -- no thread is runnable, so nothing\n"
        "  can ever signal it, and the caller passed no timeout. Reporting a\n"
        "  timeout would make the guest act on a falsehood, so the run stops\n"
        "  here instead. Live threads:\n", what);
    psp_sched_dump_threads(stderr);
    /* The reason rides along so the boot summary can tell a stopped run from
     * one that finished -- both leave zero threads alive. */
    psp_sched_stop_all(what);
}

/* warn_block used to sit here: a wait that could not block reported a timeout
 * and said so once. Nothing reaches it now. A caller that supplies a timeout
 * gets a real one -- the wait parks with a deadline and the scheduler moves the
 * clock to it -- and a caller that supplies none is not lied to, it stops the
 * run through wait_deadlock. There is no third case left to warn about. */

/* Defined down with the waits, but needed by the signal above them. */
static int sema_release(psp_sema *s);

/* ---- threads ------------------------------------------------------------- */

static void hle_CreateThread(void) {
    /* (name, entry, priority, stackSize, attr, option) */
    psp_thread *t = NULL;
    for (int i = 0; i < MAX_THREADS; i++) if (!g_thread[i].used) { t = &g_thread[i]; break; }
    if (!t) { psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }

    memset(t, 0, sizeof *t);
    psp_str(psp_arg(0), t->name, sizeof t->name);
    t->entry      = psp_arg(1);
    t->priority   = psp_arg(2);
    t->stack_size = psp_arg(3);
    t->attr       = psp_arg(4);

    if (t->stack_size < 0x1000) t->stack_size = 0x1000;
    /* Stacks grow down, so allocate from the top of the heap: a stack that
     * overflows then runs into free space rather than into another block. */
    t->stack_base = psp_sysmem_alloc(t->stack_size, 1);
    if (!t->stack_base) { psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }

    t->uid = g_next_uid++;
    t->state = TH_DORMANT;
    t->used = 1;
    psp_ret(t->uid);
}

/* What a thread is started with, and the one part of it that is held back.
 *
 * threads/semaphores/semaphores measures three rules and this implements two.
 *
 * The two: a NULL pointer with a non-zero length arrives as length **0**, and a
 * zero length with a real pointer arrives as a **NULL pointer**. Each cancels
 * the other out, in both directions, and neither is what passing the arguments
 * straight through gives.
 *
 * The third is that the block is *copied* onto the thread's own stack, and the
 * evidence for it is not in doubt. A one-byte start of the global 0x4567 reads
 * back on hardware as **0xFFFFFF67** -- one byte of data with this stack's 0xFF
 * fill above it -- which no reading of the original address can produce. A
 * variable holding 7, handed to a thread that writes 3 through the pointer,
 * still reads 7 afterwards.
 *
 * **It is not implemented, because it takes Armored Core from 633 GE lists to
 * 3.** Measured directly, and narrowed: performing the copy is harmless, and
 * handing the thread the copy's *address* is what breaks it. Copying 256 bytes
 * instead of four does not help, so the game is not merely reading past the
 * length it declared. What it does do is start three workers in a row from one
 * shared slot, rewriting the word between each -- so with the original pointer
 * all three read the last value, and with copies each reads its own, which is
 * the correct behaviour and the one the game does not survive.
 *
 * That points at something else being wrong upstream rather than at the rule,
 * and shipping a rule that is right in principle and breaks the only real
 * program available is the wrong trade. See docs/findings/autotests.md. */
static uint32_t start_arg_block(uint32_t *arglen, uint32_t argp) {
    if (!argp || !*arglen) { *arglen = 0; return 0; }
    return argp;
}

static void hle_StartThread(void) {
    /* (thid, arglen, argp) */
    psp_thread *t = find_thread(psp_arg(0));
    if (!t) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_THID); return; }

    /* A fresh thread's stack is filled with 0xFF, not left as it was found.
     *
     * That is the pattern sceKernelGetThreadStackFreeSize walks looking for --
     * this file already noted that we do not paint one -- and it is directly
     * observable: a short argument block on hardware reads back with 0xFF above
     * it. Verified against the game before shipping, which is not idle: filling
     * a stack changes what every uninitialised local reads. */
    if (t->stack_base) {
        void *p = psp_mem_ptr(t->stack_base, t->stack_size);
        if (p) memset(p, 0xFF, t->stack_size);
    }

    uint32_t arglen = psp_arg(1);
    const uint32_t argp = start_arg_block(&arglen, psp_arg(2));

    /* Stack pointer starts at the top of the allocation, 16-byte aligned, with
     * a little headroom so a callee storing below $sp cannot run off the end. */
    const uint32_t sp = (t->stack_base + t->stack_size - 64) & ~15u;

    /* The thread becomes runnable; it does not run here.
     *
     * It used to run to completion inside this call, on the caller's register
     * file, which worked for the module_start -> create -> start -> return
     * shape and for nothing else. A thread that blocks part-way has to be able
     * to stop and let another run, and its position in the host call stack is
     * its state -- so it needs a stack of its own. See sched.c.
     *
     * Starting a thread does not hand it the token. A PSP thread of higher
     * priority would preempt its starter, which cannot happen without
     * preemption; what does happen is that it runs as soon as the starter
     * blocks or yields. */
    if (psp_sched_spawn(t->uid, t->entry, sp, arglen, argp,
                        (int)t->priority) != 0) {
        psp_ret(SCE_KERNEL_ERROR_NO_MEMORY);
        return;
    }
    t->state = TH_READY;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_ExitThread(void) {
    const uint32_t status = psp_arg(0);

    /* A thread ending is the last chance to see how it got there, and a thread
     * that ends with a nonzero status is usually reporting a failure its caller
     * will act on -- by which point the code that decided is long gone. Costs
     * nothing unless the generated code was built with PSPRECOMP_TRACE. */
    if (psp_hle_logging()) {
        fprintf(stderr, "hle: thread 0x%08X exiting with status %u\n",
                psp_sched_current(), status);
        psp_trace_dump();
    }
    psp_thread *t = find_thread(psp_sched_current());
    if (t) {
        t->exit_status = status;
        t->state       = TH_DORMANT;
        for (int i = 0; i < t->nenders && i < MAX_SEMA_WAITERS; i++)
            psp_sched_wake(t->enders[i]);
        t->nenders = 0;
    }
    /* Does not return when called from a guest thread: the scheduler ends the
     * host thread underneath it. From the main context there is nothing to
     * unwind, so it falls through. */
    psp_sched_exit(psp_sched_current());
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_TerminateThread(void);

static void hle_DeleteThread(void) {
    psp_thread *t = find_thread(psp_arg(0));
    if (!t) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_THID); return; }
    if (t->stack_base) psp_sysmem_release(t->stack_base);
    t->used = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* With one thread running to completion there is nothing to schedule during a
 * delay, so it returns immediately. Time still advances for anything reading
 * the clock. */
/* There is no clock, so a delay cannot be timed -- but it is nearly always a
 * thread being polite, and returning immediately starves everything else in a
 * game whose main loop delays. Yielding gives the other threads the token,
 * which is the useful half of the semantics. */
static void hle_DelayThread(void) {
    psp_sched_delay(psp_arg(0));
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* A thread died by returning from its entry point. Records what it returned and
 * releases anyone waiting for it. */
static void on_thread_end(uint32_t uid, uint32_t status) {
    psp_thread *t = find_thread(uid);
    if (!t) return;
    t->exit_status = status;
    t->state       = TH_DORMANT;
    for (int i = 0; i < t->nenders && i < MAX_SEMA_WAITERS; i++)
        psp_sched_wake(t->enders[i]);
    t->nenders = 0;
}

/* sceKernelWaitThreadEnd(SceUID thid, SceUInt *timeout)
 *
 * The second argument is a *timeout pointer*, not somewhere to put the exit
 * status -- writing the status there corrupted whatever the guest kept at that
 * address. The status is the return value, as PPSSPP's implementation shows. */
static void hle_WaitThreadEnd(void) {
    const uint32_t thid    = psp_arg(0);
    const uint32_t timeout = psp_arg(1);
    psp_thread *t = find_thread(thid);
    if (!t) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_THID); return; }

    /* Park until it ends. This is what drives a freshly started thread: nothing
     * runs it until the thread holding the token gives it up, and a caller
     * waiting for its result is the usual moment that happens. */
    while (t->state != TH_DORMANT) {
        const uint32_t me = psp_sched_current();
        t->enders[t->nenders++ % MAX_SEMA_WAITERS] = me;
        if (psp_sched_block(me, PSP_SCHED_BLOCKED, "sceKernelWaitThreadEnd") != 0) {
            if (timeout) { psp_ret(SCE_KERNEL_ERROR_WAIT_TIMEOUT); return; }
            wait_deadlock("sceKernelWaitThreadEnd");
            psp_ret(SCE_KERNEL_ERROR_OK);
            return;
        }
        t = find_thread(thid);
        if (!t) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_THID); return; }
    }

    psp_ret(t->exit_status);
}

static void hle_GetThreadId(void) { psp_ret(psp_sched_current()); }

/* ---- the clock ------------------------------------------------------------
 *
 * Microseconds since the module started. See include/psprecomp/clock.h for why
 * it is virtual rather than the host's. A 64-bit result comes back in $v0:$v1,
 * low word first, which is the o32 convention the compiler emitted the caller
 * against. */
static void hle_GetSystemTimeWide(void) {
    const uint64_t us = psp_clock_read();
    psp_cpu.r[PSP_REG_V0] = (uint32_t)us;
    psp_cpu.r[PSP_REG_V1] = (uint32_t)(us >> 32);
}

static void hle_GetSystemTimeLow(void) {
    psp_ret((uint32_t)psp_clock_read());
}

/* Takes a SceKernelSysClock * to fill in rather than returning the value. */
static void hle_GetSystemTime(void) {
    const uint32_t out = psp_arg(0);
    const uint64_t us  = psp_clock_read();
    if (out) {
        psp_write32(out,     (uint32_t)us);
        psp_write32(out + 4, (uint32_t)(us >> 32));
    }
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_SuspendThread(void) {
    psp_thread *t = find_thread(psp_arg(0));
    if (!t) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_THID); return; }
    t->state = TH_SUSPENDED;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_ResumeThread(void) {
    psp_thread *t = find_thread(psp_arg(0));
    if (!t) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_THID); return; }
    t->state = TH_READY;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_ChangeThreadPriority(void) {
    psp_thread *t = find_thread(psp_arg(0));
    if (!t) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_THID); return; }
    t->priority = psp_arg(1);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* A module's entry point is not running on "no thread".
 *
 * On hardware the loader creates a thread to call module_start and that thread
 * has a priority like any other -- 0x20 for a user module. Here module_start
 * is called directly, so psp_sched_current() is zero until the guest creates
 * threads of its own, and answering 0 says "priority zero", which is a real
 * and very high priority rather than an absence.
 *
 * pspautotests threads/mutex/unlock2 opens by checking exactly this and
 * refuses to run at all when it is wrong. */
#define PSP_MAIN_THREAD_PRIORITY 0x20

uint32_t psp_threadman_current_priority(void) {
    const psp_thread *c = current_thread();
    return c ? c->priority : PSP_MAIN_THREAD_PRIORITY;
}

static void hle_GetThreadCurrentPriority(void) {
    psp_ret(psp_threadman_current_priority());
}

/* Terminate stops a thread without freeing it. Unimplemented until now, which
 * mattered more than it looks: pspautotests' checkpoint helper terminates its
 * rescheduler thread between every pair of checks, and a terminate that does
 * nothing leaves that thread to run later. */
static void hle_TerminateThread(void) {
    psp_thread *t = find_thread(psp_arg(0));
    if (!t) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_THID); return; }
    psp_sched_cancel_spawn(t->uid);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_ChangeCurrentThreadAttr(void) {
    psp_thread *c = current_thread();
    if (c) c->attr = (c->attr & ~psp_arg(0)) | psp_arg(1);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_GetThreadStackFreeSize(void) {
    /* Real firmware walks the stack looking for the fill pattern. We do not
     * paint one, so report the whole stack: it is used for "am I close to
     * overflowing", and claiming plenty of room is the safe direction. */
    psp_thread *t = find_thread(psp_arg(0));
    const psp_thread *c = current_thread();
    psp_ret(t ? t->stack_size : (c ? c->stack_size : 0));
}

/* ---- semaphores ---------------------------------------------------------- */

/* PSPRECOMP_SEMA=<substring> narrates the traffic on the semaphores whose name
 * contains it -- who waits, who signals, and what the count was each time.
 *
 * "Nobody signals this semaphore" is the shape of several bugs here, and the
 * thread dump can only say a thread is parked on one. It cannot say whether
 * the signal never came or came too early, and those need opposite fixes. A
 * signal also dumps the guest function trace, which names the code that sent
 * it; a wait does not, because waits are the common case and the trace is long.
 *
 * Off unless the variable is set, and matched by substring so PSPRECOMP_SEMA=Movie
 * covers a whole subsystem at once. */
static const char *sema_watch(void) {
    static int done;
    static const char *v;
    if (!done) { v = getenv("PSPRECOMP_SEMA"); done = 1; }
    return v;
}

static int sema_watched(const psp_sema *s) {
    const char *w = sema_watch();
    return w && *w && strstr(s->name, w) != NULL;
}

static void sema_log(const psp_sema *s, const char *op, int32_t arg, int trace) {
    if (!sema_watched(s)) return;
    fprintf(stderr, "sema: thread 0x%08X  %-7s uid 0x%08X %-20s count=%d arg=%d\n",
            psp_sched_current(), op, s->uid, s->name, s->count, arg);
    if (trace) psp_trace_dump();
}

/* Every distinct semaphore uid ever handed to sceKernelSignalSema.
 *
 * "Nobody signals this one" is a claim about code that was never executed, and
 * the watch above can only report signals that happened. This records the whole
 * set instead, so the claim can be checked against it rather than inferred from
 * silence -- including the case where the name the watch matches on is not the
 * object the waiter is actually parked on. */
#define MAX_SIGNALLED_UIDS 64
static uint32_t g_sig_uid[MAX_SIGNALLED_UIDS];
static int      g_sig_uids;

static void note_signalled(uint32_t uid) {
    for (int i = 0; i < g_sig_uids; i++) if (g_sig_uid[i] == uid) return;
    if (g_sig_uids < MAX_SIGNALLED_UIDS) g_sig_uid[g_sig_uids++] = uid;
}

/* Every thread the game ever created, alive or not.
 *
 * The scheduler's list is of threads that still exist, which cannot answer what
 * became of one that is missing -- created and never started looks identical to
 * never created at all, and both look identical to started and long since
 * finished. This keeps the record instead. */
void psp_threadman_dump_threads(FILE *out) {
    static const char *const ST[] = { "dormant", "ready", "running", "suspended" };
    fprintf(out, "  threads created:\n");
    for (int i = 0; i < MAX_THREADS; i++) {
        const psp_thread *t = &g_thread[i];
        if (!t->uid) continue;          /* never allocated */
        fprintf(out, "    uid 0x%08X  entry 0x%08X  prio %-3u  %-9s  exit %u  %s%s\n",
                t->uid, t->entry, t->priority,
                (unsigned)t->state < 4 ? ST[t->state] : "?",
                t->exit_status, t->name,
                t->used ? "" : "  (deleted)");
    }
}

void psp_threadman_dump_signalled(FILE *out) {
    fprintf(out, "  semaphore uids ever signalled (%d):", g_sig_uids);
    for (int i = 0; i < g_sig_uids; i++) fprintf(out, " 0x%08X", g_sig_uid[i]);
    fprintf(out, "\n");
    for (int i = 0; i < MAX_SEMAS; i++) {
        if (!g_sema[i].used) continue;
        int seen = 0;
        for (int k = 0; k < g_sig_uids; k++) if (g_sig_uid[k] == g_sema[i].uid) seen = 1;
        fprintf(out, "    uid 0x%08X  %-24s %s\n", g_sema[i].uid, g_sema[i].name,
                seen ? "signalled" : "NEVER SIGNALLED");
    }
}

/* A NULL name is rejected, and the attribute word is range-checked.
 *
 * Both are observable and neither was done. The pspautotests create tests
 * pass a null pointer as the name and expect SCE_KERNEL_ERROR_ERROR back;
 * we accepted it, read a string from guest address 0, and reported success.
 * The two objects that answer NO_MEMORY instead of ERROR (fpl, msgpipe) are
 * not implemented here, so the single code is right for everything that is.
 *
 * **The attribute rule is per object type, and sharing one check broke the
 * game.** A semaphore takes the low nine bits and refuses anything above them;
 * that was measured from semaphores/create.expected, and then applied to event
 * flags too, where it is simply false. Armored Core creates an event flag with
 * attribute 0x200 -- PSP_EVENT_WAITMULTIPLE, an ordinary and documented flag
 * attribute -- was told ILLEGAL_ATTR, used the error code as a uid, and the run
 * went from 633 GE lists to 0 with 123,606 bad memory accesses.
 *
 * The lesson is the one state.md keeps recording in other forms: a measurement
 * about one object is not a fact about its neighbours. Both rules below come
 * from a hardware capture, and the two capture files disagree with each other. */
static int name_ok(uint32_t name_ptr) { return name_ptr != 0; }

/* semaphores/create.expected: 1, 0x100 and 0x1ff are accepted; 0x200, 0x400,
 * 0x800, 0x900, 0x1000, 0x2000, 0x4000, 0x8000 and 0x10000 are all refused. */
static int sema_attr_ok(uint32_t attr) { return attr < 0x200u; }

/* events/create/create.expected, all ten of its cases:
 *
 *     0x000 ok   0x001 ok   0x010 ok   0x100 FAIL  0x122 FAIL
 *     0x200 ok   0x222 ok   0x300 FAIL 0x900 FAIL  0x1200 FAIL
 *
 * So bit 0x100 is illegal here where it is legal for a semaphore, bit 0x200 is
 * legal here where it is illegal for a semaphore, and nothing above 0x2FF is
 * allowed at all. Every one of the ten fits this and nothing narrower does. */
static int flag_attr_ok(uint32_t attr) {
    return (attr & ~0x2FFu) == 0 && (attr & 0x100u) == 0;
}

/* Copy a name into a guest SceKernel*Info block: 32 bytes, truncated to 31
 * characters and NUL-terminated, which is what hardware reports back for the
 * 31-character name the create tests hand it. */
static void write_info_name(uint32_t dst, const char *name) {
    char buf[32];
    memset(buf, 0, sizeof buf);
    for (int i = 0; i < 31 && name[i]; i++) buf[i] = name[i];
    for (int i = 0; i < 32; i++) psp_write8(dst + (uint32_t)i, (uint8_t)buf[i]);
}

static void hle_CreateSema(void) {
    /* (name, attr, initVal, maxVal, option) */
    if (!name_ok(psp_arg(0))) { psp_ret(SCE_KERNEL_ERROR_ERROR); return; }
    if (!sema_attr_ok(psp_arg(1))) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_ATTR); return; }
    psp_sema *s = NULL;
    for (int i = 0; i < MAX_SEMAS; i++) if (!g_sema[i].used) { s = &g_sema[i]; break; }
    if (!s) { psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }

    memset(s, 0, sizeof *s);
    psp_str(psp_arg(0), s->name, sizeof s->name);
    s->attr       = psp_arg(1);
    s->count      = (int32_t)psp_arg(2);
    s->init_count = (int32_t)psp_arg(2);
    s->max_count  = (int32_t)psp_arg(3);
    s->uid = g_next_uid++;
    s->used = 1;
    /* Through a local: source and destination are both inside g_sema, and the
     * compiler cannot see that two distinct members never overlap. */
    char nm[sizeof s->name];
    memcpy(nm, s->name, sizeof nm);
    snprintf(s->waitdesc, sizeof s->waitdesc, "sceKernelWaitSema(%s)", nm);
    sema_log(s, "create", s->count, 0);
    psp_ret(s->uid);
}

static void hle_DeleteSema(void) {
    psp_sema *s = find_sema(psp_arg(0));
    if (!s) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_SEMID); return; }
    /* Everyone parked on it has to be let go, not left parked on an object that
     * no longer exists -- each discovers for itself that its lookup now fails. */
    const int urgent = psp_waitq_release_all(&s->q);
    s->used = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
    if (urgent) psp_sched_yield();
}

static void hle_SignalSema(void) {
    psp_sema *s = find_sema(psp_arg(0));
    if (!s) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_SEMID); return; }
    s->count += (int32_t)psp_arg(1);
    if (s->max_count > 0 && s->count > s->max_count) s->count = s->max_count;
    note_signalled(s->uid);
    sema_log(s, "signal", (int32_t)psp_arg(1), 1);

    /* Release in the object's own order, deducting on each waiter's behalf.
     *
     * This used to wake everyone and let them re-test, which is not a
     * conservative version of the same thing: the winner was then whichever
     * thread the *scheduler* picked, always the most urgent, so a FIFO
     * semaphore behaved like a priority one. See waitq.h. */
    const int urgent = sema_release(s);

    psp_ret(SCE_KERNEL_ERROR_OK);
    /* Released a thread that outranks us, so it runs now. */
    if (urgent) psp_sched_yield();
}

/* Take the semaphore if it can be taken, and never block. The distinction from
 * WaitSema is the whole point of the call: a caller uses it precisely because
 * it has something else to do when the answer is no. */
static void hle_PollSema(void) {
    psp_sema *s = find_sema(psp_arg(0));
    if (!s) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_SEMID); return; }
    const int32_t need = (int32_t)psp_arg(1);
    if (s->count < need) { psp_ret(SCE_KERNEL_ERROR_SEMA_ZERO); return; }
    s->count -= need;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* Give the rest of this priority level a turn. With round-robin handoff a
 * plain yield already does exactly that, and the priority argument only
 * selects a level we would reach anyway. */
static void hle_RotateReadyQueue(void) {
    psp_sched_yield();
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_DeleteCallback(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

/* ---- the shape every kernel wait has ---------------------------------------
 *
 * The timeout argument is a *pointer*, and it is in and out. Hardware reads how
 * long to wait and, when the wait ends, writes back how much of it was left --
 * which the tests print, so it is observable rather than a nicety.
 * semaphores/wait.expected pins all three cases:
 *
 *     Signaled: OK (500ms left)                 immediate, nothing spent
 *     Wait timeout: ... remaining=4             blocked, woken with 4ms of 5000 left
 *     Never signaled: Failed (800201A8, 0ms)    ran out
 *
 * and a fourth, which is why the write-back is conditional: a call that fails
 * on its *arguments* -- `Greater than max: Failed (800201BD, 500ms left)` --
 * leaves the word alone, because it never waited.
 *
 * This was previously read only for its non-NULL-ness, to decide whether a
 * fabricated timeout was allowed. The duration itself was discarded. */
static uint64_t wait_deadline(uint32_t tmo_ptr) {
    if (!tmo_ptr) return 0;                       /* wait forever */
    const uint32_t usec = psp_read32(tmo_ptr);
    /* A zero timeout is a real timeout, the shortest one there is -- and
     * hardware answers it with WAIT_TIMEOUT rather than waiting
     * ("Zero timeout: Failed (800201A8, 0ms left)"). Giving it the earliest
     * deadline that exists produces exactly that, through the ordinary path. */
    return psp_clock_peek() + (usec ? usec : 1);
}

static void wait_writeback(uint32_t tmo_ptr, uint64_t deadline) {
    if (!tmo_ptr) return;
    const uint64_t now = psp_clock_peek();
    psp_write32(tmo_ptr, now < deadline ? (uint32_t)(deadline - now) : 0);
}

/* Hand the count to whoever is next in line, as far as it will go.
 *
 * The head blocks the queue -- a waiter the count cannot satisfy is not skipped
 * over, and a *later* caller does not take what it is waiting for.
 * threads/semaphores/fifo measures both halves in eight lines: with a count of
 * 1, a thread asking for 5 parks; a second thread asking for 1 parks behind it
 * rather than taking what is there; and only when the first times out and
 * leaves the queue does the second get it.
 *
 * That is also why this runs whenever the queue *changes*, not only on a
 * signal. A waiter leaving on its own timeout can unblock the one behind it. */
static int sema_release(psp_sema *s) {
    int urgent = 0;
    for (;;) {
        const int i = psp_waitq_pick(&s->q, s->attr);
        if (i < 0 || s->count < (int32_t)s->q.w[i].need) break;
        const psp_waiter w = psp_waitq_take(&s->q, i);
        s->count -= (int32_t)w.need;
        sema_log(s, "taken", (int32_t)w.need, 0);
        urgent |= psp_sched_wake(w.uid);
    }
    return urgent;
}

static void hle_WaitSema(void) {
    const uint32_t id      = psp_arg(0);
    const int32_t  need    = (int32_t)psp_arg(1);
    const uint32_t tmo_ptr = psp_arg(2);
    /* Zero, negative, or more than the semaphore could ever hold. Answered
     * before the count is consulted and without touching the timeout word --
     * semaphores/wait.expected reports `Greater than max: Failed (800201BD,
     * 500ms left)`, so the call never waited. */
    psp_sema *s = find_sema(id);
    if (!s) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_SEMID); return; }
    if (need <= 0 || (s->max_count > 0 && need > s->max_count)) {
        psp_ret(SCE_KERNEL_ERROR_ILLEGAL_COUNT);
        return;
    }

    const uint64_t deadline = wait_deadline(tmo_ptr);

    /* Available, and nobody ahead of us. Both conditions: see sema_release. */
    if (psp_waitq_count(&s->q) == 0 && s->count >= need) {
        s->count -= need;
        sema_log(s, "taken", need, 0);
        wait_writeback(tmo_ptr, deadline);
        psp_ret(SCE_KERNEL_ERROR_OK);
        return;
    }

    sema_log(s, "park", need, 0);
    const uint32_t me = psp_sched_current();
    if (psp_waitq_add(&s->q, me, (uint32_t)need, 0, 0) != 0) {
        psp_ret(SCE_KERNEL_ERROR_NO_MEMORY);
        return;
    }

    const int rc = psp_sched_block_until(me, PSP_SCHED_BLOCKED, s->waitdesc,
                                         deadline);

    /* Gone while we were parked, which is what sceKernelDeleteSema releasing
     * its waiters looks like from in here -- and a different answer from asking
     * about a semaphore that was already gone before the call. */
    s = find_sema(id);
    if (!s) { psp_ret(SCE_KERNEL_ERROR_WAIT_DELETE); return; }

    if (rc == PSP_SCHED_WOKEN) {
        /* The signaller already took the count on our behalf, so there is
         * nothing to re-test: being woken *is* the semaphore. */
        wait_writeback(tmo_ptr, deadline);
        psp_ret(SCE_KERNEL_ERROR_OK);
        return;
    }

    /* Not released, so we are still queued. Leaving may unblock the thread
     * behind us -- that is the second half of what fifo.expected measures. */
    psp_waitq_drop(&s->q, me);
    const int urgent = sema_release(s);

    if (rc == PSP_SCHED_EXPIRED) {
        wait_writeback(tmo_ptr, deadline);
        psp_ret(SCE_KERNEL_ERROR_WAIT_TIMEOUT);
        if (urgent) psp_sched_yield();
        return;
    }

    /* Stranded: nothing is runnable and no deadline can release us, so nobody
     * could ever signal this. A caller that asked for no timeout is not told
     * one elapsed -- a fabricated timeout is something a game acts on. */
    wait_deadlock("sceKernelWaitSema");
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* ---- event flags --------------------------------------------------------- */

#define PSP_EVENT_WAITAND   0x00
#define PSP_EVENT_WAITOR    0x01
#define PSP_EVENT_WAITCLEAR 0x20
/* The creation attribute that lets more than one thread wait at once. It is
 * also the bit an event flag accepts where a semaphore refuses it -- see the
 * two attribute rules above. */
#define PSP_EVENT_WAITMULTIPLE 0x200

static void hle_CreateEventFlag(void) {
    /* (name, attr, bits, option) */
    if (!name_ok(psp_arg(0))) { psp_ret(SCE_KERNEL_ERROR_ERROR); return; }
    if (!flag_attr_ok(psp_arg(1))) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_ATTR); return; }
    psp_evflag *f = NULL;
    for (int i = 0; i < MAX_FLAGS; i++) if (!g_flag[i].used) { f = &g_flag[i]; break; }
    if (!f) { psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }

    memset(f, 0, sizeof *f);
    psp_str(psp_arg(0), f->name, sizeof f->name);
    f->attr         = psp_arg(1);
    f->pattern      = psp_arg(2);
    f->init_pattern = psp_arg(2);
    f->uid = g_next_uid++;
    f->used = 1;
    char nm[sizeof f->name];
    memcpy(nm, f->name, sizeof nm);
    snprintf(f->waitdesc, sizeof f->waitdesc, "sceKernelWaitEventFlag(%s)", nm);
    psp_ret(f->uid);
}

static void hle_DeleteEventFlag(void) {
    psp_evflag *f = find_flag(psp_arg(0));
    if (!f) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_EVFID); return; }
    const int urgent = psp_waitq_release_all(&f->q);
    f->used = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
    if (urgent) psp_sched_yield();
}

static int flag_satisfied(const psp_evflag *f, uint32_t bits, uint32_t mode) {
    return (mode & PSP_EVENT_WAITOR) ? (f->pattern & bits) != 0
                                     : (f->pattern & bits) == bits;
}

/* Release every waiter the pattern now satisfies, in the object's order.
 *
 * Unlike a semaphore there is no head-of-line blocking here: waiters ask for
 * different bit patterns, so one that cannot be satisfied says nothing about
 * the next. What does have to stay ordered is WAITCLEAR -- a released waiter
 * consumes the bits it woke on, so who is considered first decides who gets
 * them, and that is the whole content of the attribute.
 *
 * The pattern each waiter woke on goes to the address it supplied, which is why
 * the queue carries that address: by the time the release happens the waiter's
 * own frame is not reachable from here. */
static int flag_release(psp_evflag *f) {
    int urgent = 0;
    for (;;) {
        /* The satisfiable waiters, in the queue's own arrival order, so that
         * psp_waitq_pick stays the single place that knows what the attribute
         * means. Copying them out is what lets an unsatisfiable waiter be
         * passed over without being removed. */
        psp_waitq eligible = { 0 };
        for (int i = 0; i < psp_waitq_count(&f->q); i++) {
            const psp_waiter *w = &f->q.w[i];
            if (flag_satisfied(f, w->need, w->mode))
                psp_waitq_add(&eligible, w->uid, w->need, w->mode, w->out);
        }
        const int i = psp_waitq_pick(&eligible, f->attr);
        if (i < 0) break;

        const psp_waiter w = eligible.w[i];
        psp_waitq_drop(&f->q, w.uid);
        if (w.out) psp_write32(w.out, f->pattern);
        if (w.mode & PSP_EVENT_WAITCLEAR) f->pattern &= ~w.need;
        urgent |= psp_sched_wake(w.uid);
    }
    return urgent;
}

static void hle_SetEventFlag(void) {
    psp_evflag *f = find_flag(psp_arg(0));
    if (!f) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_EVFID); return; }
    f->pattern |= psp_arg(1);
    const int urgent = flag_release(f);
    psp_ret(SCE_KERNEL_ERROR_OK);
    if (urgent) psp_sched_yield();
}

static void hle_ClearEventFlag(void) {
    psp_evflag *f = find_flag(psp_arg(0));
    if (!f) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_EVFID); return; }
    /* The argument is a mask of bits to KEEP, not bits to clear. Getting this
     * backwards leaves a game waiting on a flag that never clears. */
    f->pattern &= psp_arg(1);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* sceKernelWaitEventFlag(evfid, bits, mode, outBits, timeout)
 *
 * The fifth argument is psp_arg(4), and that question is settled rather than
 * open. A previous version of this function returned a fabricated timeout
 * instead of blocking, on the grounds that o32 spills argument five to sp+16
 * while psp_arg(4) reads $t0 -- so which one the guest used was unknown, and
 * guessing had already been wrong once.
 *
 * hle.h answers it, with a disassembly and a bug that turned on it: PSP
 * firmware stubs do *not* spill. They load $t0-$t3 and branch, visible in the
 * delay slot of every such call, and reading sp+16 instead returned whatever
 * the stack happened to hold -- which is what once made the allocator refuse a
 * 15.9 MB request. So the fifth argument is $t0, the ordinary rule applies, and
 * this can block like every other wait. */
static void hle_WaitEventFlag(void) {
    const uint32_t id      = psp_arg(0);
    const uint32_t bits    = psp_arg(1);
    const uint32_t mode    = psp_arg(2);
    const uint32_t out     = psp_arg(3);
    const uint32_t tmo_ptr = psp_arg(4);

    /* Arguments before the handle, which is the order hardware uses and is
     * observable. events/wait/wait.expected answers a wait for *no bits* on a
     * NULL, invalid or deleted flag with EVF_ILPAT rather than UNKNOWN_EVFID,
     * and a wrong mode on a NULL flag with ILLEGAL_MODE -- so both checks see
     * the arguments before anything has looked the object up. */
    if (bits == 0) { psp_ret(SCE_KERNEL_ERROR_EVF_ILPAT); return; }
    /* WAITOR and WAITCLEAR and nothing else: 0x02, 0x04, 0x08, 0x40, 0x80 and
     * 0xFF are each refused in that same file. */
    if (mode & ~(uint32_t)(PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR)) {
        psp_ret(SCE_KERNEL_ERROR_ILLEGAL_MODE);
        return;
    }

    psp_evflag *f = find_flag(id);
    if (!f) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_EVFID); return; }

    const uint64_t deadline = wait_deadline(tmo_ptr);

    if (flag_satisfied(f, bits, mode)) {
        if (out) psp_write32(out, f->pattern);
        if (mode & PSP_EVENT_WAITCLEAR) f->pattern &= ~bits;
        wait_writeback(tmo_ptr, deadline);
        psp_ret(SCE_KERNEL_ERROR_OK);
        return;
    }

    /* One waiter, unless the flag was created to allow more. That is what
     * attribute 0x200 is -- PSP_EVENT_WAITMULTIPLE -- and it is the same bit
     * the create test showed an event flag accepting where a semaphore refuses
     * it. A second waiter without it is refused rather than queued. */
    if (psp_waitq_count(&f->q) > 0 && !(f->attr & PSP_EVENT_WAITMULTIPLE)) {
        psp_ret(SCE_KERNEL_ERROR_EVF_MULTI);
        return;
    }

    const uint32_t me = psp_sched_current();
    if (psp_waitq_add(&f->q, me, bits, mode, out) != 0) {
        psp_ret(SCE_KERNEL_ERROR_NO_MEMORY);
        return;
    }

    const int rc = psp_sched_block_until(me, PSP_SCHED_BLOCKED, f->waitdesc,
                                         deadline);

    /* Gone while we were parked, which is what sceKernelDeleteEventFlag
     * releasing its waiters looks like from in here -- and a different answer
     * from asking about a flag that was already gone before the call. */
    f = find_flag(id);
    if (!f) { psp_ret(SCE_KERNEL_ERROR_WAIT_DELETE); return; }

    if (rc == PSP_SCHED_WOKEN) {
        /* flag_release already wrote the pattern we woke on and applied our
         * clear, on our behalf and in the object's order. */
        wait_writeback(tmo_ptr, deadline);
        psp_ret(SCE_KERNEL_ERROR_OK);
        return;
    }

    psp_waitq_drop(&f->q, me);
    if (out) psp_write32(out, f->pattern);

    if (rc == PSP_SCHED_EXPIRED) {
        wait_writeback(tmo_ptr, deadline);
        psp_ret(SCE_KERNEL_ERROR_WAIT_TIMEOUT);
        return;
    }

    wait_deadlock("sceKernelWaitEventFlag");
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* ---- callbacks ----------------------------------------------------------- */

/* The Refer*Status calls: what an object currently is, written into a struct
 * the caller supplies.
 *
 * None of the three existed. An unregistered firmware call returns without
 * touching its out-parameter, so the caller prints its own uninitialised
 * stack -- the create tests reported `attr=167767488, init=1308, cur=43`
 * where hardware writes zeros. Same failure as the missing
 * sceDisplayGetFrameBuf: success reported, nothing written.
 *
 * `size` is written rather than left alone. A caller sets it before the call
 * and hardware fills what fits, so echoing the real size is right for the
 * common case and better than leaving a field the caller may not have set. */
static void hle_ReferSemaStatus(void) {
    const psp_sema *sm = find_sema(psp_arg(0));
    const uint32_t info = psp_arg(1);
    if (!sm)   { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_SEMID); return; }
    if (!info) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
    psp_write32(info +  0, 56);
    write_info_name(info + 4, sm->name);
    psp_write32(info + 36, sm->attr);
    psp_write32(info + 40, (uint32_t)sm->init_count);
    psp_write32(info + 44, (uint32_t)sm->count);
    psp_write32(info + 48, (uint32_t)sm->max_count);
    psp_write32(info + 52, (uint32_t)psp_waitq_count(&sm->q));
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_ReferEventFlagStatus(void) {
    const psp_evflag *f = find_flag(psp_arg(0));
    const uint32_t info = psp_arg(1);
    if (!f)    { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_EVFID); return; }
    if (!info) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
    psp_write32(info +  0, 52);
    write_info_name(info + 4, f->name);
    psp_write32(info + 36, f->attr);
    psp_write32(info + 40, f->init_pattern);
    psp_write32(info + 44, f->pattern);
    psp_write32(info + 48, (uint32_t)psp_waitq_count(&f->q));
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* No lookup existed: callbacks are only ever created here, never resolved. */
static psp_callback *find_cb(uint32_t id) {
    for (int i = 0; i < MAX_CBS; i++)
        if (g_cb[i].used && g_cb[i].uid == id) return &g_cb[i];
    return NULL;
}

static void hle_ReferCallbackStatus(void) {
    const psp_callback *c = find_cb(psp_arg(0));
    const uint32_t info = psp_arg(1);
    if (!c)    { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_UID); return; }
    if (!info) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
    psp_write32(info +  0, 56);
    write_info_name(info + 4, c->name);
    psp_write32(info + 36, c->thread);
    psp_write32(info + 40, c->func);
    psp_write32(info + 44, c->arg);
    psp_write32(info + 48, c->notify_count);
    psp_write32(info + 52, c->notify_arg);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_CreateCallback(void) {
    if (!name_ok(psp_arg(0))) { psp_ret(SCE_KERNEL_ERROR_ERROR); return; }
    psp_callback *c = NULL;
    for (int i = 0; i < MAX_CBS; i++) if (!g_cb[i].used) { c = &g_cb[i]; break; }
    if (!c) { psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }

    memset(c, 0, sizeof *c);
    psp_str(psp_arg(0), c->name, sizeof c->name);
    c->func = psp_arg(1);
    c->arg  = psp_arg(2);
    c->thread = psp_sched_current();
    c->uid  = g_next_uid++;
    c->used = 1;
    /* Registered but never fired: callbacks are delivered from the scheduler,
     * which does not exist yet. A game that only registers them (the common
     * case -- exit and power callbacks) is unaffected. */
    psp_ret(c->uid);
}

/* Deliver any callbacks pending for the current thread; the return value is
 * how many ran.
 *
 * None ever are: callbacks are raised by things that do not happen here -- a
 * disc being ejected, a power button, a timer expiring. Reporting zero is
 * therefore accurate rather than a stub.
 *
 * It matters that this exists at all. A game waiting on an event pumps
 * callbacks while it waits, and an unimplemented call still returns zero, so
 * the loop looks identical either way -- except that the surrounding wait never
 * ends, and the whole thing reads as a hang with no cause. */
static void hle_CheckCallback(void) { psp_ret(0); }

void psp_threadman_register(void) {
    /* NIDs are SHA-1(name)[0:4] little-endian; tests/test_hle.c verifies every
     * pair below. */
    psp_hle_register(0x349D6D6C, "ThreadManForUser", "sceKernelCheckCallback",           hle_CheckCallback);
    psp_hle_register(0x446D8DE6, "ThreadManForUser", "sceKernelCreateThread",            hle_CreateThread);
    psp_hle_register(0xF475845D, "ThreadManForUser", "sceKernelStartThread",             hle_StartThread);
    psp_hle_register(0xAA73C935, "ThreadManForUser", "sceKernelExitThread",              hle_ExitThread);
    psp_hle_register(0x9FA03CD3, "ThreadManForUser", "sceKernelDeleteThread",            hle_DeleteThread);
    psp_hle_register(0x616403BA, "ThreadManForUser", "sceKernelTerminateThread",         hle_TerminateThread);
    psp_hle_register(0xCEADEB47, "ThreadManForUser", "sceKernelDelayThread",             hle_DelayThread);
    psp_hle_register(0x68DA9E36, "ThreadManForUser", "sceKernelDelayThreadCB",           hle_DelayThread);
    psp_hle_register(0x278C0DF5, "ThreadManForUser", "sceKernelWaitThreadEnd",           hle_WaitThreadEnd);
    psp_hle_register(0x82BC5777, "ThreadManForUser", "sceKernelGetSystemTimeWide",        hle_GetSystemTimeWide);
    psp_hle_register(0x369ED59D, "ThreadManForUser", "sceKernelGetSystemTimeLow",         hle_GetSystemTimeLow);
    psp_hle_register(0xDB738F35, "ThreadManForUser", "sceKernelGetSystemTime",            hle_GetSystemTime);
    psp_hle_register(0x293B45B8, "ThreadManForUser", "sceKernelGetThreadId",             hle_GetThreadId);
    psp_hle_register(0x9944F31F, "ThreadManForUser", "sceKernelSuspendThread",           hle_SuspendThread);
    psp_hle_register(0x75156E8F, "ThreadManForUser", "sceKernelResumeThread",            hle_ResumeThread);
    psp_hle_register(0x71BC9871, "ThreadManForUser", "sceKernelChangeThreadPriority",    hle_ChangeThreadPriority);
    psp_hle_register(0x94AA61EE, "ThreadManForUser", "sceKernelGetThreadCurrentPriority",hle_GetThreadCurrentPriority);
    psp_hle_register(0xEA748E31, "ThreadManForUser", "sceKernelChangeCurrentThreadAttr", hle_ChangeCurrentThreadAttr);
    psp_hle_register(0x52089CA1, "ThreadManForUser", "sceKernelGetThreadStackFreeSize",  hle_GetThreadStackFreeSize);

    psp_hle_register(0xD6DA4BA1, "ThreadManForUser", "sceKernelCreateSema",              hle_CreateSema);
    psp_hle_register(0x28B6489C, "ThreadManForUser", "sceKernelDeleteSema",              hle_DeleteSema);
    psp_hle_register(0x3F53E640, "ThreadManForUser", "sceKernelSignalSema",              hle_SignalSema);
    psp_hle_register(0x4E3A1105, "ThreadManForUser", "sceKernelWaitSema",                hle_WaitSema);
    /* The CB form additionally runs the thread's pending callbacks while it
     * waits. Callbacks are delivered by sceKernelCheckCallback here, so the
     * two differ only in that -- and unimplemented was much worse than
     * imperfect: it returned zero, and zero means "you have the semaphore",
     * so a thread carried on holding a lock it had never taken. */
    psp_hle_register(0x6D212BAC, "ThreadManForUser", "sceKernelWaitSemaCB",              hle_WaitSema);
    psp_hle_register(0x58B1F937, "ThreadManForUser", "sceKernelPollSema",                hle_PollSema);
    psp_hle_register(0x912354A7, "ThreadManForUser", "sceKernelRotateThreadReadyQueue",  hle_RotateReadyQueue);
    psp_hle_register(0xEDBA5844, "ThreadManForUser", "sceKernelDeleteCallback",          hle_DeleteCallback);

    psp_hle_register(0x55C20A00, "ThreadManForUser", "sceKernelCreateEventFlag",         hle_CreateEventFlag);
    psp_hle_register(0xEF9E4C70, "ThreadManForUser", "sceKernelDeleteEventFlag",         hle_DeleteEventFlag);
    psp_hle_register(0x1FB15A32, "ThreadManForUser", "sceKernelSetEventFlag",            hle_SetEventFlag);
    psp_hle_register(0x812346E4, "ThreadManForUser", "sceKernelClearEventFlag",          hle_ClearEventFlag);
    psp_hle_register(0x402FCF22, "ThreadManForUser", "sceKernelWaitEventFlag",           hle_WaitEventFlag);

    psp_hle_register(0xE81CAF8F, "ThreadManForUser", "sceKernelCreateCallback",          hle_CreateCallback);
    psp_hle_register(0xBC6FEBC5, "ThreadManForUser", "sceKernelReferSemaStatus",         hle_ReferSemaStatus);
    psp_hle_register(0xA66B0120, "ThreadManForUser", "sceKernelReferEventFlagStatus",    hle_ReferEventFlagStatus);
    psp_hle_register(0x730ED8BC, "ThreadManForUser", "sceKernelReferCallbackStatus",     hle_ReferCallbackStatus);
}
