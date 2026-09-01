/* psprecomp — thread scheduling. See include/psprecomp/sched.h. */

#include "psprecomp/sched.h"
#include "psprecomp/cpu.h"
#include "psprecomp/dispatch.h"
#include "psprecomp/clock.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define MAX_SCHED_THREADS 130      /* the kernel's 128, plus the main context */
#define MAIN_SLOT         0        /* the context module_start runs on */
#define PSP_HOST_STACK_SIZE (16u * 1024u * 1024u)

typedef struct {
    int             used;
    uint32_t        uid;
    uint32_t        entry, sp, a0, a1;
    uint32_t        gp;            /* the starter's $gp; see psp_sched_spawn */
    int             priority;      /* PSP: lower number is more urgent */
    psp_sched_state state;
    const char     *waiting_on;    /* diagnostics only */
    uint64_t        wake_at;       /* guest microseconds; 0 when not sleeping */
    /* Set by psp_sched_wake, cleared when a wait begins. A deadline that
     * expires readies a slot without setting it, which is how a timed wait
     * tells "somebody signalled me" from "my time ran out" -- the two need
     * opposite answers and the token alone cannot distinguish them. */
    int             woken;
    psp_cpu_state   ctx;           /* valid whenever this slot is not running */
    pthread_t       host;
    int             started;
} sched_slot;

static sched_slot      g_slot[MAX_SCHED_THREADS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_turn = PTHREAD_COND_INITIALIZER;
static int             g_running = MAIN_SLOT;
static void          (*g_end_hook)(uint32_t uid, uint32_t status);
static void          (*g_thread_hook)(void);
static int             g_threading = 1;
static const char     *g_stop_reason;
static int           (*g_spawn_hook)(uint32_t uid, uint32_t entry, uint32_t sp,
                                      uint32_t a0, uint32_t a1, int priority);

/* Which slot this host thread *is*, as opposed to which slot holds the token.
 *
 * Identity has to come from the host thread. Deriving it from g_running instead
 * answers with whoever currently holds the token -- and g_running is -1 while a
 * handoff has found nobody, which read back as uid 0, which is the main
 * context's uid. A guest thread then blocked the main context's slot rather
 * than its own, and the main context sat marked RUNNING with no token.
 *
 * MAIN_SLOT is 0, so the main context needs no initialisation. */
static _Thread_local int g_self = MAIN_SLOT;

/* ---- slots ---------------------------------------------------------------- */

static int slot_of(uint32_t uid) {
    for (int i = 0; i < MAX_SCHED_THREADS; i++)
        if (g_slot[i].used && g_slot[i].uid == uid) return i;
    return -1;
}

/* The caller's slot. An explicit uid wins when it names a live slot; otherwise
 * the caller is whoever this host thread is. */
static int self_slot(uint32_t uid) {
    const int s = slot_of(uid);
    return s >= 0 ? s : g_self;
}

void psp_sched_init(void) { psp_sched_reset(); }

void psp_sched_set_threading(int on) { g_threading = on; }

static void (*g_cancel_hook)(uint32_t uid);

void psp_sched_set_cancel_hook(void (*fn)(uint32_t uid)) { g_cancel_hook = fn; }
void psp_sched_cancel_spawn(uint32_t uid) { if (g_cancel_hook) g_cancel_hook(uid); }

void psp_sched_set_spawn_hook(int (*fn)(uint32_t uid, uint32_t entry, uint32_t sp,
                                        uint32_t a0, uint32_t a1, int priority)) {
    g_spawn_hook = fn;
}

void psp_sched_reset(void) {
    pthread_mutex_lock(&g_lock);
    /* Threads from a previous run are not joined: they are parked inside guest
     * code that will never be resumed, and there is nowhere for them to return
     * to. Reset is a bring-up convenience between runs in one process, not a
     * teardown. */
    memset(g_slot, 0, sizeof g_slot);
    g_stop_reason = NULL;
    g_slot[MAIN_SLOT].used     = 1;
    g_slot[MAIN_SLOT].uid      = 0;
    g_slot[MAIN_SLOT].state    = PSP_SCHED_RUNNING;
    g_slot[MAIN_SLOT].priority = 32;
    g_running    = MAIN_SLOT;
    pthread_mutex_unlock(&g_lock);
}

/* ---- handing the token over ------------------------------------------------
 *
 * Called with the lock held. Picks the most urgent ready thread and wakes
 * everyone; each sleeper re-checks whether it is the one. A broadcast is
 * wasteful in principle and irrelevant here -- a PSP title runs a handful of
 * threads, and the alternative is a condition variable per slot for no
 * measurable gain.
 *
 * Returns the slot that now holds the token, or -1 if nothing could run.
 *
 * The result is returned rather than left in a global on purpose. It used to be
 * a sticky `g_deadlocked` flag, which conflated two different things -- "this
 * handoff found nobody" and "the scheduler has stopped" -- and no path that set
 * it cleared it. A later handoff that succeeded would then be read as a
 * deadlock by whoever looked at the flag next, and that caller would take the
 * token back from the thread this function had just marked RUNNING. Two slots
 * were left RUNNING, one of them holding nothing, and since only READY slots
 * are ever selected that thread could never be scheduled again. */
static int handoff_locked(void) {
    /* One token, one RUNNING slot. Cheap -- the selection scan below already
     * walks the same array -- and it turns a silent corruption into a named
     * one, which is the whole reason the bug above took as long as it did. */
    int cur = -1, dup = -1;
    for (int i = 0; i < MAX_SCHED_THREADS; i++) {
        if (!g_slot[i].used || g_slot[i].state != PSP_SCHED_RUNNING) continue;
        if (cur < 0) cur = i; else { dup = i; break; }
    }
    if (dup >= 0)
        fprintf(stderr, "psprecomp: scheduler invariant broken -- uid 0x%08X and "
                        "uid 0x%08X are both RUNNING\n",
                g_slot[cur].uid, g_slot[dup].uid);

    /* Anything whose delay has expired is runnable again. Checked here rather
     * than by a timer because there is no timer: guest time only moves when the
     * guest moves it, so the moment to notice is the moment somebody asks who
     * runs next. */
    for (int i = 0; i < MAX_SCHED_THREADS; i++)
        if (g_slot[i].used && g_slot[i].state == PSP_SCHED_SLEEPING &&
            g_slot[i].wake_at && g_slot[i].wake_at <= psp_clock_peek()) {
            g_slot[i].state   = PSP_SCHED_READY;
            g_slot[i].wake_at = 0;
        }

    int best = -1;
    /* Scanned starting *after* the current thread, so that among threads of
     * equal priority the next one round-robins in rather than the lowest slot
     * index winning every time. Priority still decides first -- this only
     * settles ties, which is what a PSP does: equal-priority threads share the
     * CPU in turn, and one of them cannot monopolise it. */
    const int start = g_running >= 0 ? g_running : 0;
    for (int k = 1; k <= MAX_SCHED_THREADS; k++) {
        const int i = (start + k) % MAX_SCHED_THREADS;
        if (!g_slot[i].used || g_slot[i].state != PSP_SCHED_READY) continue;
        if (best < 0 || g_slot[i].priority < g_slot[best].priority) best = i;
    }

    if (best < 0) {
        /* Nothing is runnable, but something may be sleeping on a deadline that
         * has not arrived. Waiting for it is not idling, it is hanging: the
         * clock only advances because a thread advanced it, and no thread is
         * running. So move time to the earliest deadline and let that thread go
         * -- which is what a kernel with a real timer would do, arriving at the
         * same instant by a different route. */
        uint64_t soonest = 0;
        for (int i = 0; i < MAX_SCHED_THREADS; i++)
            if (g_slot[i].used && g_slot[i].state == PSP_SCHED_SLEEPING &&
                g_slot[i].wake_at && (!soonest || g_slot[i].wake_at < soonest))
                soonest = g_slot[i].wake_at;

        if (soonest) {
            psp_clock_advance_to(soonest);
            for (int i = 0; i < MAX_SCHED_THREADS; i++)
                if (g_slot[i].used && g_slot[i].state == PSP_SCHED_SLEEPING &&
                    g_slot[i].wake_at && g_slot[i].wake_at <= soonest) {
                    g_slot[i].state   = PSP_SCHED_READY;
                    g_slot[i].wake_at = 0;
                }
            const int start2 = g_running >= 0 ? g_running : 0;
            for (int k = 1; k <= MAX_SCHED_THREADS; k++) {
                const int i = (start2 + k) % MAX_SCHED_THREADS;
                if (!g_slot[i].used || g_slot[i].state != PSP_SCHED_READY) continue;
                if (best < 0 || g_slot[i].priority < g_slot[best].priority) best = i;
            }
        }
    }

    if (best < 0) {
        /* Nothing can run. Every thread is blocked on something no running
         * thread will ever provide. The token is left with nobody, and the
         * broadcast is what lets the main context notice inside its drain --
         * a guest thread that wakes here finds g_running still not its own and
         * goes back to waiting, which is the point. */
        g_running = -1;
        pthread_cond_broadcast(&g_turn);
        return -1;
    }

    g_slot[best].state = PSP_SCHED_RUNNING;
    g_running = best;
    pthread_cond_broadcast(&g_turn);
    return best;
}

/* Wait until this slot holds the token, then adopt its register state.
 * Called with the lock held.
 *
 * Returns 0 holding the token, or -1 if this slot was killed while waiting --
 * psp_sched_stop_all is the only thing that does that, and a thread it killed
 * must not carry on running guest code.
 *
 * Waking is not the same as being scheduled. This used to break out of its wait
 * on a deadlock flag and return regardless, so every parked thread resumed at
 * once, none of them holding the token and none with psp_cpu restored -- and
 * psp_cpu is a single global. Nothing may run without g_running == me. */
static int await_turn_locked(int me) {
    while (g_running != me) {
        if (g_slot[me].state == PSP_SCHED_DEAD) return -1;
        pthread_cond_wait(&g_turn, &g_lock);
    }
    psp_cpu = g_slot[me].ctx;
    return 0;
}

/* Give up the token: save state, change our own, hand over, wait to come back.
 *
 * Returns 0 once the caller is running again, or -1 if parking would have
 * stopped everything -- no other thread was runnable, so nobody could ever
 * deliver what the caller is waiting for.
 *
 * That second case has to be reported rather than waited out. A caller that
 * blocks in a loop until its condition holds would otherwise spin against a
 * scheduler that keeps handing the token straight back, which is a busy hang
 * and worse than the timeout it replaced. The caller undoes its wait and fails
 * it instead. */
static int switch_away(int me, psp_sched_state why, const char *what,
                       uint64_t deadline_us) {
    pthread_mutex_lock(&g_lock);
    g_slot[me].ctx        = psp_cpu;
    /* With a deadline the wait *is* a sleep as far as the handoff is concerned:
     * SLEEPING plus wake_at is the state it already knows how to expire, and
     * teaching it a second one would be two mechanisms for one thing. What the
     * caller was waiting on is still recorded, so the thread dump reads
     * "sleeping on sceKernelWaitSema(x)" rather than losing the object. */
    g_slot[me].state      = deadline_us ? PSP_SCHED_SLEEPING : why;
    g_slot[me].waiting_on = what;
    g_slot[me].wake_at    = deadline_us;
    g_slot[me].woken      = 0;

    if (handoff_locked() < 0) {
        /* This handoff found nobody, so undo the wait and say so. Decided from
         * the handoff's own result, never from a flag some earlier handoff may
         * have left behind -- acting on a stale flag here is what used to take
         * the token back from a thread that had just been given it.
         *
         * A dated wait cannot reach here: handoff_locked's own fallback finds
         * this slot's deadline, moves the clock to it and releases us. */
        g_slot[me].state      = PSP_SCHED_RUNNING;
        g_slot[me].waiting_on = NULL;
        g_slot[me].wake_at    = 0;
        g_running             = me;
        pthread_mutex_unlock(&g_lock);
        return PSP_SCHED_STRANDED;
    }

    if (await_turn_locked(me) != 0) {          /* killed by psp_sched_stop_all */
        g_slot[me].waiting_on = NULL;
        pthread_mutex_unlock(&g_lock);
        if (me != MAIN_SLOT) pthread_exit(NULL);
        return PSP_SCHED_STRANDED;
    }
    const int woken = g_slot[me].woken;
    g_slot[me].waiting_on = NULL;
    g_slot[me].wake_at    = 0;
    pthread_mutex_unlock(&g_lock);
    /* Running again, and only the flag says why. A signal that arrived after
     * the deadline still counts as a signal: it was delivered. */
    if (woken || !deadline_us) return PSP_SCHED_WOKEN;
    return PSP_SCHED_EXPIRED;
}

/* ---- guest thread bodies --------------------------------------------------- */

static void *thread_main(void *arg) {
    sched_slot *t = (sched_slot *)arg;
    const int me = (int)(t - g_slot);

    /* Before anything else: this host thread's identity. Everything that asks
     * "which guest thread am I" reads it, including the firmware calls made
     * from the hook below. */
    g_self = me;

    if (g_thread_hook) g_thread_hook();

    pthread_mutex_lock(&g_lock);
    if (await_turn_locked(me) != 0) { pthread_mutex_unlock(&g_lock); return NULL; }
    pthread_mutex_unlock(&g_lock);

    /* The register file the thread starts with. $ra is zero: returning from the
     * entry point is how a PSP thread ends when it does not call
     * sceKernelExitThread, and the dispatcher treats a return here as the end
     * of the thread rather than a jump to address zero.
     *
     * $gp is not zero, and it is the one register here that cannot be derived
     * from the thread's own arguments -- it is per *module*, not per thread, and
     * nothing in the instruction stream ever names it. A module built with a
     * small-data area addresses that area as an offset from $gp, so a thread
     * starting with zero sends every such access to around address 0. */
    memset(&psp_cpu, 0, sizeof psp_cpu);
    psp_cpu_reset_fp();      /* a fresh thread's float/vector registers are NaN */
    psp_cpu.r[PSP_REG_A0] = t->a0;
    psp_cpu.r[PSP_REG_A1] = t->a1;
    psp_cpu.r[PSP_REG_SP] = t->sp;
    psp_cpu.r[PSP_REG_GP] = t->gp;
    psp_cpu.r[PSP_REG_RA] = 0;

    psp_dispatch(t->entry);

    /* Fell off the end of the entry point. Report it before taking the lock:
     * the hook runs thread-manager code that may take locks of its own, and it
     * still holds the token, so nothing else can be running. */
    if (g_end_hook) g_end_hook(t->uid, psp_cpu.r[PSP_REG_V0]);

    pthread_mutex_lock(&g_lock);
    t->state = PSP_SCHED_DEAD;
    /* A failed handoff here means this was the last thread that could run. The
     * token is left with nobody and the main context's drain reports it; there
     * is nothing for a dead thread to do about it. */
    (void)handoff_locked();
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

int psp_sched_spawn(uint32_t uid, uint32_t entry, uint32_t sp,
                    uint32_t a0, uint32_t a1, int priority) {
    /* The spawn hook is consulted first, before threading: an interpreter run
     * that services thread starts wants them synchronously, nested, whatever
     * the host's threading setting is. */
    if (g_spawn_hook && g_spawn_hook(uid, entry, sp, a0, a1, priority)) return 0;

    /* Threading off: the thread exists as far as the guest is concerned and
     * never runs. Reporting failure instead would send a game down its
     * out-of-memory path, which is a different and less useful lie. */
    if (!g_threading) { (void)uid; (void)entry; (void)sp;
                        (void)a0; (void)a1; (void)priority; return 0; }

    pthread_mutex_lock(&g_lock);

    int idx = -1;
    for (int i = 1; i < MAX_SCHED_THREADS; i++) if (!g_slot[i].used) { idx = i; break; }
    if (idx < 0) { pthread_mutex_unlock(&g_lock); return -1; }

    sched_slot *t = &g_slot[idx];
    memset(t, 0, sizeof *t);
    t->used = 1; t->uid = uid; t->entry = entry; t->sp = sp;
    t->a0 = a0;  t->a1 = a1;  t->priority = priority;
    t->state = PSP_SCHED_READY;
    /* Captured here rather than passed in, because the caller does not have it
     * to pass: sceKernelStartThread's arguments say nothing about $gp. The
     * starter is running now and is in the same module as the thread it starts,
     * so its own $gp is the module's -- which is what hardware gives the new
     * thread, from the module's entry in the thread control block. */
    t->gp = psp_cpu.r[PSP_REG_GP];

    /* A generous host stack, unrelated to the guest stack size the game asked
     * for. A recompiled frame is much larger than the MIPS one it came from --
     * every local the original kept in a register becomes a variable, and the
     * dispatcher adds a frame per guest call -- so a call chain that fits a
     * PSP thread's 16K can still run a host thread out of its default 8MB. The
     * same reasoning already applies to the boot host's entry stack.
     *
     * It is headroom, not a fix. A guest loop emitted as recursion exhausts any
     * stack; raising this to 64MB did not save MovieReadThread, which is how
     * that was ruled out as the cause. */
    pthread_attr_t attr;
    pthread_attr_t *attrp = NULL;
    if (pthread_attr_init(&attr) == 0) {
        pthread_attr_setstacksize(&attr, PSP_HOST_STACK_SIZE);
        attrp = &attr;
    }
    const int rc = pthread_create(&t->host, attrp, thread_main, t);
    if (attrp) pthread_attr_destroy(attrp);
    if (rc != 0) {
        t->used = 0;
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    t->started = 1;

    /* Starting a more urgent thread switches to it here.
     *
     * This is not preemption -- there is still no timer and no interrupt. It is
     * the reschedule a kernel performs *at the system call*: sceKernelStartThread
     * makes the new thread runnable, and if it outranks the caller the caller
     * stops running at that instruction. Games depend on it. This one starts a
     * priority-16 worker from a priority-32 context, sets a quit flag, and waits
     * for the worker to end; if the worker does not get to run before the flag
     * is set, it sees the flag on its first instruction and exits immediately,
     * and the game shuts down instead of booting.
     *
     * Strictly more urgent, not equal: threads of equal priority run in the
     * order they became ready, so a starter is not displaced by its own equal. */
    const int preempts = g_running >= 0 && priority < g_slot[g_running].priority;
    pthread_mutex_unlock(&g_lock);
    if (preempts) psp_sched_yield();
    return 0;
}

/* ---- the operations threadman drives -------------------------------------- */

int psp_sched_block(uint32_t uid, psp_sched_state why, const char *what) {
    return psp_sched_block_until(uid, why, what, 0);
}

int psp_sched_block_until(uint32_t uid, psp_sched_state why, const char *what,
                          uint64_t deadline_us) {
    /* Threading off is the oracle's configuration: there are no other threads,
     * so nothing could ever wake this and no clock is being driven towards the
     * deadline either. Stranded is the honest answer and the one callers
     * already handle. */
    if (!g_threading) return PSP_SCHED_STRANDED;
    return switch_away(self_slot(uid), why, what, deadline_us);
}

void psp_sched_yield(void) {
    if (!g_threading) return;
    /* A yield differs from a block only in that the caller stays runnable --
     * so a lone thread that yields simply gets the token straight back. */
    pthread_mutex_lock(&g_lock);
    const int me = g_running;
    if (me < 0) { pthread_mutex_unlock(&g_lock); return; }
    g_slot[me].ctx   = psp_cpu;
    g_slot[me].state = PSP_SCHED_READY;

    /* This handoff cannot fail: the caller was just marked READY, so the scan
     * finds at least the caller. Handled rather than assumed, because the cost
     * is three lines and the failure mode it guards against -- running on with
     * the token held by nobody -- is the bug this file was fixed for. */
    if (handoff_locked() < 0) {
        g_slot[me].state = PSP_SCHED_RUNNING;
        g_running        = me;
        pthread_mutex_unlock(&g_lock);
        return;
    }

    if (await_turn_locked(me) != 0) {          /* killed by psp_sched_stop_all */
        pthread_mutex_unlock(&g_lock);
        if (me != MAIN_SLOT) pthread_exit(NULL);
        return;
    }
    pthread_mutex_unlock(&g_lock);
}

/* Give up the CPU for one scheduling round, to a thread of *any* priority.
 *
 * A yield cannot express this. It marks the caller READY and hands off, and the
 * handoff picks the most urgent READY thread -- which is the caller again
 * whenever it is the most urgent thing runnable. So a yield from the top
 * priority is a no-op, and a game whose main loop is `render(); delay();` at
 * priority 16 starves its own priority-17 and -18 workers forever. Observed
 * exactly that: the movie threads sat READY, never blocked, never run, while
 * the frame loop went round two million times.
 *
 * A delay on hardware makes the thread genuinely not runnable for its duration,
 * which is what lets anything below it run. There is no clock here, so the
 * duration cannot be honoured -- but the *ineligibility* can, for one round.
 * That is the half of the semantics that matters. */
void psp_sched_delay(uint64_t usec) {
    if (!g_threading) return;
    pthread_mutex_lock(&g_lock);
    const int me = g_self;

    g_slot[me].ctx     = psp_cpu;
    g_slot[me].state   = PSP_SCHED_SLEEPING;
    g_slot[me].woken   = 0;
    /* A zero delay is still a request to stand aside, so it gets the shortest
     * deadline that exists rather than none -- otherwise it would never wake. */
    g_slot[me].wake_at = psp_clock_peek() + (usec ? usec : 1);

    if (handoff_locked() < 0) {                /* nobody else at all: carry on */
        g_slot[me].state   = PSP_SCHED_RUNNING;
        g_slot[me].wake_at = 0;
        g_running          = me;
        pthread_mutex_unlock(&g_lock);
        return;
    }

    if (await_turn_locked(me) != 0) {
        pthread_mutex_unlock(&g_lock);
        if (me != MAIN_SLOT) pthread_exit(NULL);
        return;
    }
    pthread_mutex_unlock(&g_lock);
}

/* The timeslice, counted in firmware calls rather than microseconds.
 *
 * A PSP preempts on a timer, so a thread that never blocks still gives way to
 * its equals. Nothing here can interrupt recompiled C part-way -- it is an
 * ordinary host call stack -- so the closest honest approximation is to
 * reschedule every so many kernel calls. Every firmware call is already a point
 * where the guest is between instructions and its register file is coherent,
 * which is exactly what a switch needs.
 *
 * This is what stops one thread starving the rest. Armored Core posts its disc
 * reads to a pool of equal-priority workers and then carries on; without a
 * timeslice the poster keeps the CPU, the workers never run, and nothing is
 * ever read from the disc. */
#define PSP_SCHED_SLICE 64

static unsigned g_slice;

void psp_sched_tick(void) {
    if (!g_threading) return;
    if (++g_slice < PSP_SCHED_SLICE) return;
    g_slice = 0;

    /* Only worth a switch if somebody else could actually run. */
    pthread_mutex_lock(&g_lock);
    int other = 0;
    for (int i = 0; i < MAX_SCHED_THREADS; i++)
        if (i != g_running && g_slot[i].used && g_slot[i].state == PSP_SCHED_READY)
            { other = 1; break; }
    pthread_mutex_unlock(&g_lock);

    if (other) psp_sched_yield();
}

int psp_sched_wake(uint32_t uid) {
    if (!g_threading) return 0;
    pthread_mutex_lock(&g_lock);
    int urgent = 0;
    const int s = slot_of(uid);
    if (s >= 0 && (g_slot[s].state == PSP_SCHED_BLOCKED ||
                   g_slot[s].state == PSP_SCHED_SLEEPING)) {
        g_slot[s].state = PSP_SCHED_READY;
        g_slot[s].waiting_on = NULL;
        /* Released by a signal rather than by its deadline, and a timed waiter
         * needs to know which. The deadline is dropped with it: the wait is
         * over, and leaving wake_at set would make the next handoff consider
         * this slot's stale moment when it looks for the earliest one. */
        g_slot[s].woken   = 1;
        g_slot[s].wake_at = 0;
        /* The token is not handed over here, because a waker usually has more
         * to do -- it may be releasing several waiters at once, and switching
         * part-way through would leave the rest for later. The caller is told
         * instead, and switches when it is finished. */
        urgent = g_running >= 0 && g_slot[s].priority < g_slot[g_running].priority;
    }
    pthread_mutex_unlock(&g_lock);
    return urgent;
}

void psp_sched_set_thread_hook(void (*fn)(void)) { g_thread_hook = fn; }

void psp_sched_set_end_hook(void (*fn)(uint32_t uid, uint32_t status)) { g_end_hook = fn; }

void psp_sched_exit(uint32_t uid) {
    if (!g_threading) return;         /* returns to the caller, as it used to */
    const int me = self_slot(uid);
    pthread_mutex_lock(&g_lock);
    g_slot[me].state = PSP_SCHED_DEAD;
    /* As in thread_main: a failed handoff leaves the token with nobody, which
     * is the drain's to report. This thread is leaving either way. */
    (void)handoff_locked();
    pthread_mutex_unlock(&g_lock);
    if (me != MAIN_SLOT) pthread_exit(NULL);
}

/* Called with the lock held. Like await_turn_locked, but gives up at `deadline`.
 * Returns 0 if the token arrived, -1 on timeout, -2 if the scheduler stopped --
 * some other thread's handoff found nobody runnable and left the token with no
 * one, which only the main context is entitled to notice and report.
 *
 * A timeout is how a runaway guest thread is escaped. It cannot be interrupted:
 * unwinding a host thread from outside means longjmp-ing across threads, which
 * is undefined. So the main context stops waiting for it instead, reports it,
 * and the process ends with the thread still running. */
static int await_turn_deadline_locked(int me, const struct timespec *deadline) {
    while (g_running != me) {
        if (g_running < 0) return -2;
        if (pthread_cond_timedwait(&g_turn, &g_lock, deadline) == ETIMEDOUT &&
            g_running != me)
            return g_running < 0 ? -2 : -1;
    }
    psp_cpu = g_slot[me].ctx;
    return 0;
}

static int live_locked(void) {
    int live = 0;
    for (int i = 1; i < MAX_SCHED_THREADS; i++)
        if (g_slot[i].used && g_slot[i].state != PSP_SCHED_DEAD) live++;
    return live;
}

void psp_sched_stop_all(const char *why) {
    /* Recorded before anything is killed: the caller is a guest thread that
     * will not exist past the pthread_exit below. */
    g_stop_reason = why;
    pthread_mutex_lock(&g_lock);
    const int me = g_running;
    for (int i = 1; i < MAX_SCHED_THREADS; i++)
        if (g_slot[i].used) g_slot[i].state = PSP_SCHED_DEAD;
    g_slot[MAIN_SLOT].state = PSP_SCHED_RUNNING;
    g_running    = MAIN_SLOT;
    pthread_cond_broadcast(&g_turn);
    pthread_mutex_unlock(&g_lock);
    if (me != MAIN_SLOT) pthread_exit(NULL);
}

const char *psp_sched_stop_reason(void) {
    return g_stop_reason;
}

int psp_sched_drain(int timeout_s) {
    if (!g_threading) return 0;

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_s;

    /* The main context steps aside so the guest threads can run. It becomes
     * runnable again only when they are all finished -- or when none of them
     * can proceed, which handoff_locked reports as a deadlock. */
    pthread_mutex_lock(&g_lock);
    int timed_out = 0, stalled = 0;
    while (live_locked()) {
        g_slot[MAIN_SLOT].ctx   = psp_cpu;
        g_slot[MAIN_SLOT].state = PSP_SCHED_BLOCKED;
        if (handoff_locked() < 0) { stalled = 1; break; }
        const int rc = await_turn_deadline_locked(MAIN_SLOT, &deadline);
        if (rc == -2) { stalled = 1; break; }
        if (rc != 0)  { timed_out = 1; break; }
    }

    const int live = live_locked();

    if (timed_out) {
        /* The list matters more here than in the stall below, not less: a run
         * that times out is one where something is still going round, and which
         * threads are parked on what is the whole question. It was omitted, so
         * the common case printed a count and nothing else.
         *
         * Printed inline rather than through psp_sched_dump_threads because the
         * lock is already held. */
        fprintf(stderr, "psprecomp: guest threads still running after %ds; "
                        "%d alive, not waiting further:\n", timeout_s, live);
        for (int i = 0; i < MAX_SCHED_THREADS; i++) {
            if (!g_slot[i].used || g_slot[i].state == PSP_SCHED_DEAD) continue;
            static const char *const ST[] = {
                "ready", "running", "blocked", "sleeping", "dead" };
            fprintf(stderr, "    uid 0x%08X  entry 0x%08X  prio %d  %s%s%s\n",
                    g_slot[i].uid, g_slot[i].entry, g_slot[i].priority,
                    ST[g_slot[i].state],
                    g_slot[i].waiting_on ? " on " : "",
                    g_slot[i].waiting_on ? g_slot[i].waiting_on : "");
        }
    } else if (stalled && live) {
        fprintf(stderr, "psprecomp: deadlock -- %d thread(s) alive, none runnable:\n", live);
        for (int i = 1; i < MAX_SCHED_THREADS; i++) {
            if (!g_slot[i].used || g_slot[i].state == PSP_SCHED_DEAD) continue;
            fprintf(stderr, "    uid 0x%08X  entry 0x%08X  %s%s%s\n",
                    g_slot[i].uid, g_slot[i].entry,
                    g_slot[i].state == PSP_SCHED_SLEEPING ? "sleeping" : "blocked",
                    g_slot[i].waiting_on ? " on " : "",
                    g_slot[i].waiting_on ? g_slot[i].waiting_on : "");
        }
    }

    g_slot[MAIN_SLOT].state = PSP_SCHED_RUNNING;
    g_running = MAIN_SLOT;
    pthread_mutex_unlock(&g_lock);
    return live;
}

/* Every live thread and what it is parked on. The drain prints this when it
 * gives up; a wait that cannot be satisfied needs exactly the same list, and
 * printing it there is the difference between "deadlock" and knowing which
 * semaphore nobody is going to signal. */
void psp_sched_dump_threads(FILE *out) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_SCHED_THREADS; i++) {
        if (!g_slot[i].used || g_slot[i].state == PSP_SCHED_DEAD) continue;
        static const char *const ST[] = {
            "ready", "running", "blocked", "sleeping", "dead" };
        fprintf(out, "    uid 0x%08X  entry 0x%08X  prio %d  %s%s%s\n",
                g_slot[i].uid, g_slot[i].entry, g_slot[i].priority,
                ST[g_slot[i].state],
                g_slot[i].waiting_on ? " on " : "",
                g_slot[i].waiting_on ? g_slot[i].waiting_on : "");
    }
    pthread_mutex_unlock(&g_lock);
}

int psp_sched_live(void) {
    pthread_mutex_lock(&g_lock);
    const int live = live_locked();
    pthread_mutex_unlock(&g_lock);
    return live;
}

uint32_t psp_sched_current(void) {
    /* From this host thread's own slot, not from whoever holds the token. The
     * two agree while a thread is running, which is why reading g_running here
     * looked correct -- but it answered 0 whenever the token was held by
     * nobody, and 0 is the main context's uid. Callers pass the result straight
     * back to psp_sched_block, so a guest thread parked the main context's slot
     * and left its own marked RUNNING. */
    pthread_mutex_lock(&g_lock);
    const uint32_t uid = g_slot[g_self].uid;
    pthread_mutex_unlock(&g_lock);
    return uid;
}
