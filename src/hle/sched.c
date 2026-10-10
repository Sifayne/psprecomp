/* psprecomp — thread scheduling. See include/psprecomp/sched.h. */

#include "psprecomp/sched.h"
#include "psprecomp/cpu.h"
#include "psprecomp/dispatch.h"
#include "psprecomp/clock.h"
#include "psprecomp/interrupt.h"
#include "psprecomp/hle.h"          /* psp_ktimer_in_handler */

#include "psprecomp/os.h"
#include "census.h"
#include "psprecomp/safepoint.h"
#include "psprecomp/state.h"

#include <setjmp.h>
#include <stdio.h>
#include <string.h>

/* Threads alive at once, plus the main context. Dead slots are reused (see
 * psp_sched_spawn), so this bounds what is running, not what has run; the
 * thread manager's table is the same size. */
#define MAX_SCHED_THREADS 1026
#define MAIN_SLOT         0        /* the context module_start runs on */
/* Timer handlers one idle handoff may run before it gives up; see there. */
#define IDLE_TIMER_CAP    100000
#define IDLE_GE_CAP       100000
#define PSP_HOST_STACK_SIZE (16u * 1024u * 1024u)

typedef struct {
    int             used;
    uint32_t        uid;
    uint32_t        entry, sp, k0, a0, a1;
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
    /* What the waker said, for waits that can end more than one way. See
     * psp_sched_wake_as. */
    int             wake_reason;
    /* Where this slot stands in the ready queue of its priority: the guest
     * microsecond at which it became READY, then a counter for everything
     * that happened in the same microsecond. The handoff runs the most urgent
     * priority and, within it, the lowest stamp -- which is one FIFO queue per
     * priority, the rule threadprobe measured on fw 6.60 (steps 16-83, all 29
     * ordering lines). It replaced a round-robin scan of the slot table, which
     * ordered equals by slot index rather than by when they became ready.
     *
     * Two stamps are not "now". A thread displaced by a more urgent one goes
     * back to the *head* of its queue (steps 59, 71, 72): it never stopped
     * being the one that should run at that priority. It gets a stamp below
     * every other. And a timed wait that expires is stamped with its deadline,
     * not with the moment somebody noticed: expired delays line up in
     * deadline order (step 77: 30, 20 and 10 ms delays run C B A). */
    int64_t         rq_time;
    int64_t         rq_seq;
    /* The counter value when the current wait began, which orders two waits
     * with the same deadline the way they were entered. */
    int64_t         park_seq;
    /* sceKernelSuspendThread. A flag over the state, not a state: a thread
     * suspended in a wait is still in that wait, and the wait can still end
     * underneath it. threadprobe (fw 6.60) reports status 0x0C (WAITING |
     * SUSPEND) with waitType and waitId intact for a suspended waiter (step
     * 36); a suspended sleeper is still asleep after its resume (step 53,
     * status 4); and one woken while suspended has had its wakeup delivered
     * and reports 8, merely SUSPEND (step 54). So the handoff skips a flagged
     * slot, and everything else treats it as whatever its state says. */
    int             suspended;
    /* What sceKernelReferThreadStatus reports it has used; see charge_locked. */
    uint64_t        run_us, run_since;
    uint32_t        releases, thread_preempts, intr_preempts;
    psp_cpu_state   ctx;           /* valid whenever this slot is not running */
    psp_park        park;          /* how it last parked: the census (census.h) */
    uint8_t         step;          /* psp_sched_set_step */
    psp_os_thread   host;
    int             started;
    int             joined;        /* host thread reaped by psp_sched_join_all */
} sched_slot;

/* The slots and the queue order are a save state's (psprecomp/state.h):
 * kept, with the host's fields -- the host thread, the diagnostic strings --
 * cleared when a state is loaded. */
static sched_slot      g_slot[MAX_SCHED_THREADS];
/* One past the highest slot ever handed out. Every scan stops here: the
 * reschedule after each firmware call walks the table twice, and a walk of
 * all of it would cost that on every call a game makes. */
static int             g_slot_hi = 1;
static psp_os_mutex g_lock = PSP_OS_MUTEX_INIT;
static psp_os_cond  g_turn = PSP_OS_COND_INIT;
static int             g_running = MAIN_SLOT;
static void          (*g_end_hook)(uint32_t uid, uint32_t status);
static void          (*g_thread_hook)(void);
static void          (*g_expire_hook)(uint32_t uid);
static int             g_threading = 1;
/* Set by loading a save state, until the main context's drain takes it: the
 * token is the saving thread's, held back until the run is ready, and no
 * handoff is to choose anyone else. */
static int             g_token_restored;
static int             g_restored_running;
/* How each slot continues from a loaded state; see "save states" below. */
enum { RESUME_NONE, RESUME_FRESH, RESUME_SAFEPOINT, RESUME_WAIT };
static struct { uint8_t kind; uint32_t nid, site; } g_resume[MAX_SCHED_THREADS];
/* The tie-break counter behind every ready stamp; see sched_slot.rq_time. */
static int64_t         g_rq_seq;
/* sceKernelSuspendDispatchThread. See psp_sched_set_dispatch. */
static int             g_dispatch = 1;
static const char     *g_stop_reason;
/* Set by psp_sched_stop_all and read without the lock by whoever is running
 * guest code: a plain flag is all a poll needs. */
static volatile int    g_stopping;
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
static PSP_THREAD_LOCAL int g_self = MAIN_SLOT;

/* ---- slots ---------------------------------------------------------------- */

/* A live slot wins over a dead one carrying the same uid.
 *
 * A dead slot stays until psp_sched_spawn reuses it, so a thread that is
 * terminated and started again can own *two* slots with one uid, and this
 * returned the corpse: its priority, its state, and its position in
 * every scan. threads/change restarts a thread after each priority change and
 * reads the priority back, which is where it showed
 * (`Before: Current=18` against hardware's `30`).
 *
 * The dead slot is still the answer when it is the only one, because a finished
 * thread is a thing callers legitimately ask about. */
static int slot_of(uint32_t uid) {
    int dead = -1;
    for (int i = 0; i < g_slot_hi; i++) {
        if (!g_slot[i].used || g_slot[i].uid != uid) continue;
        if (g_slot[i].state != PSP_SCHED_DEAD) return i;
        if (dead < 0) dead = i;
    }
    return dead;
}

/* The caller's slot. An explicit uid wins when it names a live slot; otherwise
 * the caller is whoever this host thread is. */
static int self_slot(uint32_t uid) {
    const int s = slot_of(uid);
    return s >= 0 ? s : g_self;
}

void psp_sched_init(void) {
    /* The drain's timed wait is armed against psp_os_mono_ns, so the condition
     * variable has to be measuring the same clock. Here rather than in
     * psp_sched_reset: it re-initialises the variable, which is only safe
     * before anything can be waiting on it. */
    psp_os_cond_use_monotonic(&g_turn);
    psp_sched_reset();
    psp_census_init();
    psp_safepoint_init();
    PSP_STATE_KEEP(g_slot);
    PSP_STATE_KEEP(g_slot_hi);
    PSP_STATE_KEEP(g_running);
    PSP_STATE_KEEP(g_rq_seq);
    PSP_STATE_KEEP(g_dispatch);
    PSP_STATE_KEEP(g_resume);
    psp_clock_keep();
}

void psp_sched_set_threading(int on) { g_threading = on; }

static void (*g_cancel_hook)(uint32_t uid);

void psp_sched_set_cancel_hook(void (*fn)(uint32_t uid)) { g_cancel_hook = fn; }
void psp_sched_cancel_spawn(uint32_t uid) { if (g_cancel_hook) g_cancel_hook(uid); }

void psp_sched_set_spawn_hook(int (*fn)(uint32_t uid, uint32_t entry, uint32_t sp,
                                        uint32_t a0, uint32_t a1, int priority)) {
    g_spawn_hook = fn;
}

void psp_sched_reset(void) {
    psp_os_lock(&g_lock);
    /* Threads from a previous run are not joined: they are parked inside guest
     * code that will never be resumed, and there is nowhere for them to return
     * to. Reset is a bring-up convenience between runs in one process, not a
     * teardown. */
    memset(g_slot, 0, sizeof g_slot);
    g_stop_reason = NULL;
    g_stopping    = 0;
    g_slot[MAIN_SLOT].used     = 1;
    g_slot[MAIN_SLOT].uid      = 0;
    g_slot[MAIN_SLOT].state    = PSP_SCHED_RUNNING;
    g_slot[MAIN_SLOT].priority = 32;
    g_rq_seq     = 0;
    g_slot_hi    = MAIN_SLOT + 1;
    g_running    = MAIN_SLOT;
    psp_os_unlock(&g_lock);
}

/* ---- the ready queues --------------------------------------------------------
 *
 * There is no queue structure: a slot is in the ready queue of its priority
 * when it is READY, and its stamp is its place there. Called with the lock
 * held, like everything below that ends in _locked. */

/* To the tail of its priority's queue, as of guest moment `t`. */
static void ready_tail_locked(int i, uint64_t t) {
    g_slot[i].state   = PSP_SCHED_READY;
    g_slot[i].rq_time = (int64_t)t;
    g_slot[i].rq_seq  = ++g_rq_seq;
}

/* To the head: ahead of every stamp there is, including an earlier head. */
static void ready_head_locked(int i) {
    g_slot[i].state   = PSP_SCHED_READY;
    g_slot[i].rq_time = -1;
    g_slot[i].rq_seq  = -(++g_rq_seq);
}

static int runnable(int i) {
    return g_slot[i].used && g_slot[i].state == PSP_SCHED_READY &&
           !g_slot[i].suspended;
}

/* Whether slot a runs before slot b: priority first, then queue position. */
static int ahead_of(int a, int b) {
    if (g_slot[a].priority != g_slot[b].priority)
        return g_slot[a].priority < g_slot[b].priority;
    if (g_slot[a].rq_time != g_slot[b].rq_time)
        return g_slot[a].rq_time < g_slot[b].rq_time;
    return g_slot[a].rq_seq < g_slot[b].rq_seq;
}

/* The slot the handoff would run now, or -1. */
static int pick_locked(void) {
    int best = -1;
    for (int i = 0; i < g_slot_hi; i++)
        if (runnable(i) && (best < 0 || ahead_of(i, best))) best = i;
    return best;
}

/* Every timed wait whose moment is `now` or earlier becomes READY, stamped
 * with its own deadline -- so the order they join the queue in is the order
 * their timers would have fired on hardware, however late this runs. */
static void expire_locked(uint64_t now) {
    for (int i = 0; i < g_slot_hi; i++) {
        sched_slot *t = &g_slot[i];
        if (!t->used || t->state != PSP_SCHED_SLEEPING || !t->wake_at ||
            t->wake_at > now) continue;
        t->state   = PSP_SCHED_READY;
        t->rq_time = (int64_t)t->wake_at;
        t->rq_seq  = t->park_seq;
        t->wake_at = 0;
        if (g_expire_hook) g_expire_hook(t->uid);
    }
}

/* The earliest deadline any timed wait has, or 0 for none. */
/* Whether a guest thread is waiting or sleeping: something an interrupt
 * handler could still release. The main context counts only while it waits
 * on a guest object, not while it merely drains. */
static int has_waiter_locked(void) {
    for (int i = 0; i < MAX_SCHED_THREADS; i++)
        if (g_slot[i].used &&
            (g_slot[i].state == PSP_SCHED_BLOCKED || g_slot[i].state == PSP_SCHED_SLEEPING) &&
            (i != MAIN_SLOT || g_slot[i].waiting_on)) return 1;
    return 0;
}

static uint64_t soonest_locked(void) {
    uint64_t soonest = 0;
    for (int i = 0; i < g_slot_hi; i++)
        if (g_slot[i].used && g_slot[i].state == PSP_SCHED_SLEEPING &&
            g_slot[i].wake_at && (!soonest || g_slot[i].wake_at < soonest))
            soonest = g_slot[i].wake_at;
    return soonest;
}

/* ---- what a thread has used ------------------------------------------------
 *
 * sceKernelReferThreadStatus reports a thread's run time and three counts, and
 * threadprobe (fw 6.60) shows all of them moving: runClocks is nonzero for
 * any thread that has run, even one still in its first turn (step 13), and 0
 * for one that never has; releaseCount is 1 after one block (steps 9-12, 34)
 * and 0 for a thread that has not yet given up the CPU (step 13);
 * threadPreemptCount is 1 for main after starting a more urgent thread (step
 * 1). They were written as zeros.
 *
 * Run time is charged when a slot stops holding the token, and a turn is
 * worth at least 1us: guest time only moves at firmware calls, and a thread
 * that ran without making one did still run. */
static void charge_locked(int i, uint64_t now) {
    if (i < 0) return;
    const uint64_t d = now > g_slot[i].run_since ? now - g_slot[i].run_since : 0;
    g_slot[i].run_us += d ? d : 1;
}

/* The one place the token changes hands, so the one place run time is kept.
 * `stopped` is when the outgoing thread stopped running. */
static void give_token_locked(int to, uint64_t stopped) {
    if (to != g_running) {
        charge_locked(g_running, stopped);
        if (to >= 0) g_slot[to].run_since = psp_clock_peek();
    }
    g_running = to;
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
    for (int i = 0; i < g_slot_hi; i++) {
        if (!g_slot[i].used || g_slot[i].state != PSP_SCHED_RUNNING) continue;
        if (cur < 0) cur = i; else { dup = i; break; }
    }
    if (dup >= 0)
        fprintf(stderr, "psprecomp: scheduler invariant broken -- uid 0x%08X and "
                        "uid 0x%08X are both RUNNING\n",
                g_slot[cur].uid, g_slot[dup].uid);

    /* The moment the outgoing thread stopped, before any idle time below. */
    const uint64_t t0 = psp_clock_peek();

    /* Anything whose delay has expired is runnable again. psp_sched_tick
     * notices most expiries at the firmware call where they fall due; this
     * catches the rest. */
    expire_locked(t0);
    int best = pick_locked();

    /* Nothing is runnable, but something may be sleeping on a deadline that has
     * not arrived. Waiting for it is not idling, it is hanging: the clock only
     * advances because a thread advanced it, and no thread is running. So move
     * time to the earliest deadline and let that thread go -- which is what a
     * kernel with a real timer would do, arriving at the same instant by a
     * different route. */
    /* An alarm or vtimer falling due first is an interrupt arriving while the
     * CPU idles, and its handler runs at that moment rather than at the next
     * firmware call some thread makes: threadprobe step 115 (fw 6.60) sets a
     * 2ms alarm, delays 10ms, and the handler has run before the delay ends.
     * It runs here, on the outgoing thread's host thread, whose registers are
     * already saved and which still holds the token; the lock is dropped for
     * it because the handler makes firmware calls. Whatever it readies is
     * picked up below. Without this a program whose threads all wait on
     * something only a timer handler provides was declared stranded. The cap
     * only stops a periodic handler that never readies anyone from spinning
     * guest time on for ever. */
    int timer_runs = 0, ge_runs = 0, intr_runs = 0;
    while (best < 0) {
        const uint64_t soonest = soonest_locked();
        const uint64_t timer = timer_runs < IDLE_TIMER_CAP && !psp_ktimer_in_handler()
                             ? psp_ktimer_next_due() : 0;
        /* The GE, likewise, goes on with what it was given while the CPU
         * idles (src/hle/ge.c, "When the GE runs"), up to whichever comes
         * first, the next deadline or the next timer; a handler it reaches
         * by then runs at its moment and may ready a thread. The cap only
         * stops a list that raises handlers without end. */
        if (ge_runs < IDLE_GE_CAP) {
            const uint64_t next = timer && (!soonest || timer < soonest) ? timer : soonest;
            psp_os_unlock(&g_lock);
            const int ran = psp_ge_idle_run(next);
            psp_os_lock(&g_lock);
            if (ran) {
                ge_runs++;
                expire_locked(psp_clock_peek());
                best = pick_locked();
                continue;
            }
        }
        /* A Vblank subinterrupt handler, likewise, runs at its moment while
         * the CPU idles, if some thread is waiting for something it might
         * provide (src/hle/interrupt.c). The host drain is not such a waiter:
         * once every guest thread has exited, a leftover registration must
         * not keep the drain going. The cap is the timers'. */
        const uint64_t intr = intr_runs < IDLE_TIMER_CAP && has_waiter_locked()
                            ? psp_interrupt_next_event() : 0;
        if (intr && (!soonest || intr <= soonest) && (!timer || intr < timer)) {
            psp_clock_advance_to(intr);
            psp_os_unlock(&g_lock);
            psp_display_tick();
            psp_interrupt_run_pending();
            psp_os_lock(&g_lock);
            intr_runs++;
            expire_locked(psp_clock_peek());
            best = pick_locked();
            continue;
        }
        if (timer && (!soonest || timer <= soonest)) {
            psp_clock_advance_to(timer);
            psp_os_unlock(&g_lock);
            const int ran = psp_ktimer_fire_idle();
            psp_os_lock(&g_lock);
            timer_runs = ran ? timer_runs + ran : IDLE_TIMER_CAP;
            expire_locked(psp_clock_peek());
            best = pick_locked();
            continue;
        }
        if (!soonest) break;
        psp_clock_advance_to(soonest);
        expire_locked(psp_clock_peek());
        best = pick_locked();
    }

    if (best < 0) {
        /* Nothing can run. Every thread is blocked on something no running
         * thread will ever provide. The token is left with nobody, and the
         * broadcast is what lets the main context notice inside its drain --
         * a guest thread that wakes here finds g_running still not its own and
         * goes back to waiting, which is the point. */
        give_token_locked(-1, t0);
        psp_os_cond_broadcast(&g_turn);
        return -1;
    }

    g_slot[best].state = PSP_SCHED_RUNNING;
    give_token_locked(best, t0);
    psp_os_cond_broadcast(&g_turn);
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
 * psp_cpu is a single global. Nothing may run without g_running == me.
 *
 * While it waits it can be asked to run something on its host thread
 * (psp_sched_run_on), first, so that a request is answered even by a slot
 * that has just been killed. */
static uint8_t g_parked[MAX_SCHED_THREADS];  /* waiting here: can take a request */
static int     g_serve = -1;                 /* the slot asked to run g_serve_fn */
static void  (*g_serve_fn)(void);

/* A request for this slot's host thread: run it, with the lock let go, and
 * tell the caller. Returns whether there was one. */
static int serve_locked(int me) {
    if (g_serve != me) return 0;
    void (*fn)(void) = g_serve_fn;
    psp_os_unlock(&g_lock);
    fn();
    psp_os_lock(&g_lock);
    g_serve = -1;
    psp_os_cond_broadcast(&g_turn);
    return 1;
}

static int await_turn_locked(int me) {
    while (g_running != me) {
        if (serve_locked(me)) continue;
        if (g_slot[me].state == PSP_SCHED_DEAD) return -1;
        g_parked[me] = 1;
        psp_os_cond_wait(&g_turn, &g_lock);
        g_parked[me] = 0;
    }
    psp_cpu = g_slot[me].ctx;
    return 0;
}

/* A guest thread's host thread is ending: the host lets go of what is that
 * thread's alone first (the GL context, src/host/render_gl.c), since nothing
 * can make it another thread's once the thread holding it has gone. Called
 * on that host thread, without the lock. */
static void (*g_host_exit_hook)(void);
void psp_sched_set_host_exit_hook(void (*fn)(void)) { g_host_exit_hook = fn; }

static void host_exit(void) {
    if (g_host_exit_hook) g_host_exit_hook();
    psp_os_thread_exit();
}

void psp_sched_run_on(uint32_t uid, void (*fn)(void)) {
    psp_os_lock(&g_lock);
    const int s = slot_of(uid);
    if (s < 0 || s == g_self || !g_parked[s] || g_serve >= 0) {
        psp_os_unlock(&g_lock);
        fn();
        return;
    }
    g_serve = s;
    g_serve_fn = fn;
    psp_os_cond_broadcast(&g_turn);
    while (g_serve >= 0) psp_os_cond_wait(&g_turn, &g_lock);
    psp_os_unlock(&g_lock);
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
    if (psp_interrupt_in_handler()) return -1;
    psp_os_lock(&g_lock);
    g_slot[me].ctx        = psp_cpu;
    psp_census_note_park(&g_slot[me].park, PSP_PARK_BLOCK, what, deadline_us);
    /* With a deadline the wait *is* a sleep as far as the handoff is concerned:
     * SLEEPING plus wake_at is the state it already knows how to expire, and
     * teaching it a second one would be two mechanisms for one thing. What the
     * caller was waiting on is still recorded, so the thread dump reads
     * "sleeping on sceKernelWaitSema(x)" rather than losing the object. */
    g_slot[me].state      = deadline_us ? PSP_SCHED_SLEEPING : why;
    g_slot[me].waiting_on = what;
    g_slot[me].wake_at    = deadline_us;
    g_slot[me].woken      = 0;
    g_slot[me].park_seq   = ++g_rq_seq;

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
        give_token_locked(me, psp_clock_peek());
        psp_os_unlock(&g_lock);
        return PSP_SCHED_STRANDED;
    }
    /* Parked: the thread gave up the CPU (releaseCount; see charge_locked). */
    g_slot[me].releases++;

    if (await_turn_locked(me) != 0) {          /* killed by psp_sched_stop_all */
        g_slot[me].waiting_on = NULL;
        psp_os_unlock(&g_lock);
        if (me != MAIN_SLOT) host_exit();
        return PSP_SCHED_STRANDED;
    }
    const int woken = g_slot[me].woken;
    const int released = woken && g_slot[me].wake_reason == PSP_SCHED_WAKE_RELEASE;
    g_slot[me].waiting_on = NULL;
    g_slot[me].wake_at    = 0;
    psp_os_unlock(&g_lock);
    /* Running again, and only the flag says why. A signal that arrived after
     * the deadline still counts as a signal: it was delivered. A forced
     * release is neither, and has its own answer, so that a wait which does
     * not know about it still leaves its queue rather than taking it for a
     * signal. */
    if (released) return PSP_SCHED_RELEASED;
    if (woken || !deadline_us) return PSP_SCHED_WOKEN;
    return PSP_SCHED_EXPIRED;
}

/* ---- guest thread bodies --------------------------------------------------- */

static void thread_end(sched_slot *t);

static void thread_run(sched_slot *t) {
    const int me = (int)(t - g_slot);
    psp_os_lock(&g_lock);
    if (await_turn_locked(me) != 0) { psp_os_unlock(&g_lock); return; }
    psp_os_unlock(&g_lock);

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
    psp_cpu_reset_thread();  /* DEADBEEF GPRs, NaN float/vector registers */
    psp_cpu.r[PSP_REG_A0] = t->a0;
    psp_cpu.r[PSP_REG_A1] = t->a1;
    psp_cpu.r[PSP_REG_SP] = t->sp;
    psp_cpu.r[PSP_REG_FP] = t->sp;
    psp_cpu.r[PSP_REG_K0] = t->k0;
    psp_cpu.r[PSP_REG_GP] = t->gp;
    psp_cpu.r[PSP_REG_RA] = 0;

    psp_dispatch(t->entry);
    thread_end(t);
}

/* Fell off the end of the entry point. Report it before taking the lock:
 * the hook runs thread-manager code that may take locks of its own, and it
 * still holds the token, so nothing else can be running. */
static void thread_end(sched_slot *t) {
    if (g_end_hook) g_end_hook(t->uid, psp_cpu.r[PSP_REG_V0]);

    psp_os_lock(&g_lock);
    t->state = PSP_SCHED_DEAD;
    /* A failed handoff here means this was the last thread that could run. The
     * token is left with nobody and the main context's drain reports it; there
     * is nothing for a dead thread to do about it. */
    (void)handoff_locked();
    psp_os_unlock(&g_lock);
}

/* Where this host thread's guest thread began. A save state loaded while the
 * game runs brings the thread that loads it back here, its host frames
 * abandoned, to continue as the state's saving thread (psp_sched_state_jump). */
static PSP_THREAD_LOCAL jmp_buf *t_base;

static void resume_slot(sched_slot *t);

static void thread_main(void *arg) {
    sched_slot *t = (sched_slot *)arg;
    /* Before anything else: this host thread's identity. Everything that asks
     * "which guest thread am I" reads it, including the firmware calls made
     * from the hook below. */
    g_self = (int)(t - g_slot);
    if (g_thread_hook) g_thread_hook();
    jmp_buf base;
    t_base = &base;
    if (setjmp(base)) { resume_slot(&g_slot[g_self]); }
    else thread_run(t);
    if (g_host_exit_hook) g_host_exit_hook();
}

int psp_sched_spawn(uint32_t uid, uint32_t entry, uint32_t sp, uint32_t k0,
                    uint32_t a0, uint32_t a1, int priority) {
    /* The spawn hook is consulted first, before threading: an interpreter run
     * that services thread starts wants them synchronously, nested, whatever
     * the host's threading setting is. */
    if (g_spawn_hook && g_spawn_hook(uid, entry, sp, a0, a1, priority)) return 0;

    /* Threading off: the thread exists as far as the guest is concerned and
     * never runs. Reporting failure instead would send a game down its
     * out-of-memory path, which is a different and less useful lie. */
    if (!g_threading) { (void)uid; (void)entry; (void)sp; (void)k0;
                        (void)a0; (void)a1; (void)priority; return 0; }

    psp_os_lock(&g_lock);

    /* A dead slot is reused, but only once its host thread has been joined.
     *
     * Without the join it is unsafe. The dead thread's host thread may still be
     * parked in await_turn_locked, which refuses to proceed only while the slot
     * reads DEAD; hand that slot to a new thread and the old one's wait
     * *succeeds*, so two host threads run guest code at once against the single
     * global psp_cpu. Measured: threads/create went to 131,929,071 bad memory
     * accesses the moment reuse was first allowed, without a join.
     *
     * And it has to happen. With no reuse the table was a count of every start
     * in the run, and threadprobe alone makes 115 of them; the 130th start of
     * any run failed with NO_MEMORY however few threads were alive (findings
     * G8). A dead slot is preferred to a fresh one so that the host threads,
     * and the range every scan here walks, stay the size of what is alive. */
    int idx = -1, dead = -1;
    for (int i = 1; i < g_slot_hi; i++) {
        if (!g_slot[i].used) { if (idx < 0) idx = i; }
        else if (g_slot[i].state == PSP_SCHED_DEAD && dead < 0) dead = i;
    }
    if (dead >= 0) {
        sched_slot *d = &g_slot[dead];
        /* Claimed before the lock is dropped, so nothing else takes it; still
         * DEAD, so its host thread, woken by the broadcast, leaves. */
        d->used = 0;
        if (d->started && !d->joined) {
            psp_os_cond_broadcast(&g_turn);
            psp_os_unlock(&g_lock);
            psp_os_thread_join(&d->host);
            psp_os_lock(&g_lock);
            d->joined = 1;
        }
        idx = dead;
    }
    if (idx < 0 && g_slot_hi < MAX_SCHED_THREADS) idx = g_slot_hi++;
    if (idx < 0) { psp_os_unlock(&g_lock); return -1; }

    sched_slot *t = &g_slot[idx];
    memset(t, 0, sizeof *t);
    t->used = 1; t->uid = uid; t->entry = entry; t->sp = sp; t->k0 = k0;
    t->a0 = a0;  t->a1 = a1;  t->priority = priority;
    ready_tail_locked(idx, psp_clock_peek());
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
    const int rc = psp_os_thread_start(&t->host, thread_main, t,
                                       PSP_HOST_STACK_SIZE);
    if (rc != 0) {
        t->used = 0;
        psp_os_unlock(&g_lock);
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
    psp_os_unlock(&g_lock);
    if (preempts) psp_sched_preempt();
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
    /* A deadline that has already arrived is not a wait, and must not become a
     * handoff. The zero-timeout case is the one that shows it: pspautotests
     * asks for a resource that is not there with a timeout of 0 and tags the
     * line `[x]` -- no reschedule happened -- where switching away and coming
     * back tags it `[r]`. There is no time in which anything could change, so
     * there is nothing to schedule around.
     *
     * It does *not* generalise to short-but-nonzero timeouts, though the
     * captures look like it should. msgpipe/receive sweeps the timeout by the
     * microsecond and the column changes between 2us and 3us, which reads as a
     * threshold -- and refusing to park below it takes forty tests to no output
     * at all. The reason is that pspautotests' own workers spin on 1us waits at
     * a priority above the main thread: a wait that never parks never yields,
     * and the test that was going to release them never runs again. So the
     * `[x]` at 1us is not "did not park"; it is "parked, and nothing else was
     * able to use the microsecond". Which of those it is, is not settled here. */
    if (deadline_us && deadline_us <= psp_clock_peek()) return PSP_SCHED_EXPIRED;
    return switch_away(self_slot(uid), why, what, deadline_us);
}

enum { YIELD = 0, PREEMPT_THREAD = 1, PREEMPT_INTR = 2 };
static void yield_as(int displaced);

/* Give way to a thread that outranks us, staying at the head of our own
 * priority queue. Everything else about it is a yield.
 *
 * Both ways a thread comes to outrank the running one arrive here: starting it,
 * just above, and waking it -- which is what the `urgent` return from
 * psp_sched_wake means, since it is set on a strict `priority <` and on nothing
 * else. The wake sites called psp_sched_yield until they were measured, which
 * sent the *caller* to the tail of its own priority queue for a switch it never
 * asked for; an equal-priority thread already waiting there then overtook it on
 * the way back. pspautotests creates its resched thread at exactly the main
 * thread's priority, so that overtaking is precisely what its checkpoints
 * report, and it reported it as `[r]` where hardware says `[x]`.
 * threads/mutex/unlock2 is the whole difference in one line, `Unlocked, ran: 4`,
 * and matches with this. */
void psp_sched_preempt(void) { yield_as(PREEMPT_THREAD); }

void psp_sched_yield(void) { yield_as(YIELD); }

static void yield_as(int displaced) {
    if (!g_threading) return;
    /* Dispatch suspended: the guest asked not to be switched away from. */
    if (!g_dispatch) return;
    /* A timer handler runs on no thread and returns to whatever it
     * interrupted; a thread it readied gets the CPU when it has returned (see
     * psp_ktimer_tick), not from inside it. */
    if (psp_ktimer_in_handler()) return;
    /* Nor does an interrupt handler, and with interrupts suspended nothing
     * interrupts the running thread. */
    if (psp_interrupt_in_handler() || !psp_interrupt_enabled()) return;
    /* A yield differs from a block only in that the caller stays runnable --
     * so a lone thread that yields simply gets the token straight back. */
    psp_os_lock(&g_lock);
    const int me = g_running;
    if (me < 0) { psp_os_unlock(&g_lock); return; }
    g_slot[me].ctx = psp_cpu;
    psp_census_note_park(&g_slot[me].park, displaced ? PSP_PARK_PREEMPT : PSP_PARK_YIELD, NULL, 0);
    /* Displaced goes to the head of its queue, a yield to the tail; see
     * sched_slot.rq_time for the measurements. */
    if (displaced) ready_head_locked(me);
    else           ready_tail_locked(me, psp_clock_peek());

    /* This handoff cannot fail: the caller was just marked READY, so the scan
     * finds at least the caller. Handled rather than assumed, because the cost
     * is three lines and the failure mode it guards against -- running on with
     * the token held by nobody -- is the bug this file was fixed for. */
    const int to = handoff_locked();
    if (to < 0) {
        g_slot[me].state = PSP_SCHED_RUNNING;
        give_token_locked(me, psp_clock_peek());
        psp_os_unlock(&g_lock);
        return;
    }
    /* Counted only when another thread did run. A displacement at a timer's
     * expiry happens at the end of an interrupt on hardware, so it is an
     * interrupt preemption; one caused by a system call is a thread
     * preemption; a yield is the thread giving the CPU up. Only the thread
     * preemption is measured (threadprobe step 1). */
    if (to != me) {
        if (displaced == PREEMPT_INTR)        g_slot[me].intr_preempts++;
        else if (displaced == PREEMPT_THREAD) g_slot[me].thread_preempts++;
        else                                  g_slot[me].releases++;
    }

    if (await_turn_locked(me) != 0) {          /* killed by psp_sched_stop_all */
        psp_os_unlock(&g_lock);
        if (me != MAIN_SLOT) host_exit();
        return;
    }
    psp_os_unlock(&g_lock);
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
int psp_sched_delay(uint64_t usec) {
    if (!g_threading || psp_interrupt_in_handler()) return PSP_SCHED_EXPIRED;
    psp_os_lock(&g_lock);
    const int me = g_self;

    g_slot[me].ctx      = psp_cpu;
    psp_census_note_park(&g_slot[me].park, PSP_PARK_DELAY, NULL,
                         psp_clock_peek() + (usec ? usec : 1));
    g_slot[me].state    = PSP_SCHED_SLEEPING;
    g_slot[me].woken    = 0;
    g_slot[me].park_seq = ++g_rq_seq;
    /* sceKernelDelayThread(0) never gets here: on hardware it returns without
     * standing aside (threadprobe step 80, fw 6.60), and threadman answers it
     * itself. A zero from the runtime's own callers (a vblank wait that is
     * already due, say) still gets the shortest deadline there is rather than
     * none, which would never wake. */
    g_slot[me].wake_at = psp_clock_peek() + (usec ? usec : 1);

    if (handoff_locked() < 0) {                /* nobody else at all: carry on */
        g_slot[me].state   = PSP_SCHED_RUNNING;
        g_slot[me].wake_at = 0;
        give_token_locked(me, psp_clock_peek());
        psp_os_unlock(&g_lock);
        return PSP_SCHED_EXPIRED;
    }
    g_slot[me].releases++;                     /* see switch_away */

    if (await_turn_locked(me) != 0) {
        psp_os_unlock(&g_lock);
        if (me != MAIN_SLOT) host_exit();
        return PSP_SCHED_EXPIRED;
    }
    const int woken = g_slot[me].woken, reason = g_slot[me].wake_reason;
    psp_os_unlock(&g_lock);
    if (woken && reason == PSP_SCHED_WAKE_RELEASE) return PSP_SCHED_RELEASED;
    return woken ? PSP_SCHED_WOKEN : PSP_SCHED_EXPIRED;
}

/* The reschedule every firmware call ends with.
 *
 * There is no timeslice. threadprobe step 75 (fw 6.60) has two ready threads
 * at one priority, the first spinning for 20 ms on GetSystemTimeLow, and the
 * second does not run until the first gives the CPU up: `A0 A1 B`. So an equal
 * gets no turn at a firmware call, nor on a timer, within 20 ms. The 5 ms
 * slice that used to live here put B between A's two lines.
 *
 * What is left is the one reason a PSP switches threads without the running
 * one asking: something more urgent became runnable. Here that means a timed
 * wait whose deadline has passed -- a timer interrupt readies it on hardware,
 * and a firmware call is the nearest point this runtime has to an interrupt,
 * the guest being between instructions with its register file coherent. An
 * equal or less urgent thread readied that way only joins its queue, by its
 * deadline (step 77's expired delays run C B A once main gives up the CPU).
 *
 * Not while dispatch is suspended, which is what suspending it is for, and
 * not inside an alarm or vtimer handler, which runs on no thread and has to
 * return to the one it interrupted.
 *
 * For a game: Armored Core posts its disc reads to equal-priority workers and
 * carries on, and the slice was what let those workers run. Hardware gives up
 * the CPU inside file I/O instead (threadprobe step 86, fw 6.60), which
 * iofilemgr.c's io_park models; that, not a slice, is what lets them run. */
void psp_sched_tick(void) {
    if (!g_threading || !g_dispatch || psp_ktimer_in_handler() ||
        psp_interrupt_in_handler() || !psp_interrupt_enabled()) return;

    psp_os_lock(&g_lock);
    const int me = g_running;
    int urgent = 0;
    if (me >= 0 && me == g_self) {
        const int before = pick_locked();
        expire_locked(psp_clock_peek());
        const int best = pick_locked();
        if (best >= 0 && g_slot[best].priority < g_slot[me].priority)
            urgent = best != before ? PREEMPT_INTR : PREEMPT_THREAD;
    }
    psp_os_unlock(&g_lock);

    /* A thread whose delay just ran out preempts as a timer interrupt would;
     * one readied earlier, while it could not (dispatch was off), as the
     * system call that readied it would have. */
    if (urgent) yield_as(urgent);
}

static int wake_slot(uint32_t uid, int reason);

int psp_sched_wake(uint32_t uid) { return wake_slot(uid, 0); }

int psp_sched_wake_as(uint32_t uid, int reason) { return wake_slot(uid, reason); }

int psp_sched_wake_reason(void) {
    psp_os_lock(&g_lock);
    const int r = g_slot[g_self].wake_reason;
    psp_os_unlock(&g_lock);
    return r;
}

static int wake_slot(uint32_t uid, int reason) {
    if (!g_threading) return 0;
    psp_os_lock(&g_lock);
    int urgent = 0;
    const int s = slot_of(uid);
    if (s >= 0 && (g_slot[s].state == PSP_SCHED_BLOCKED ||
                   g_slot[s].state == PSP_SCHED_SLEEPING)) {
        ready_tail_locked(s, psp_clock_peek());
        g_slot[s].waiting_on = NULL;
        /* Released by a signal rather than by its deadline, and a timed waiter
         * needs to know which. The deadline is dropped with it: the wait is
         * over, and leaving wake_at set would make the next handoff consider
         * this slot's stale moment when it looks for the earliest one. */
        g_slot[s].woken       = 1;
        g_slot[s].wake_at     = 0;
        g_slot[s].wake_reason = reason;
        /* The token is not handed over here, because a waker usually has more
         * to do -- it may be releasing several waiters at once, and switching
         * part-way through would leave the rest for later. The caller is told
         * instead, and switches when it is finished. A suspended thread's wait
         * ends all the same (threadprobe step 54), but it cannot run. */
        urgent = !g_slot[s].suspended && g_running >= 0 &&
                 g_slot[s].priority < g_slot[g_running].priority;
    }
    psp_os_unlock(&g_lock);
    return urgent;
}

void psp_sched_set_thread_hook(void (*fn)(void)) { g_thread_hook = fn; }

void psp_sched_set_end_hook(void (*fn)(uint32_t uid, uint32_t status)) { g_end_hook = fn; }
void psp_sched_set_expire_hook(void (*fn)(uint32_t uid)) { g_expire_hook = fn; }

void psp_sched_exit(uint32_t uid) {
    if (!g_threading) return;         /* returns to the caller, as it used to */
    const int me = self_slot(uid);
    psp_os_lock(&g_lock);
    g_slot[me].state = PSP_SCHED_DEAD;
    /* As in thread_main: a failed handoff leaves the token with nobody, which
     * is the drain's to report. This thread is leaving either way. */
    (void)handoff_locked();
    psp_os_unlock(&g_lock);
    if (me != MAIN_SLOT) host_exit();
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
static int await_turn_deadline_locked(int me, uint64_t deadline_ns) {
    while (g_running != me) {
        /* The main context waits here, and may be asked too: the boot
         * thread ran the game's entry, and may hold what is its alone. */
        if (serve_locked(me)) continue;
        if (g_running < 0) return -2;
        g_parked[me] = 1;
        const int timed_out = psp_os_cond_wait_until(&g_turn, &g_lock, deadline_ns);
        g_parked[me] = 0;
        if (timed_out && g_running != me && g_serve != me)
            return g_running < 0 ? -2 : -1;
    }
    psp_cpu = g_slot[me].ctx;
    return 0;
}

/* One line per live thread: its state, whether it is suspended on top of it,
 * and what it is parked on. `from` skips the main context. */
static void dump_locked(FILE *out, int from) {
    static const char *const ST[] = {
        "ready", "running", "blocked", "sleeping", "dead" };
    for (int i = from; i < g_slot_hi; i++) {
        if (!g_slot[i].used || g_slot[i].state == PSP_SCHED_DEAD) continue;
        fprintf(out, "    uid 0x%08X  entry 0x%08X  prio %d  %s%s%s%s\n",
                g_slot[i].uid, g_slot[i].entry, g_slot[i].priority,
                ST[g_slot[i].state],
                g_slot[i].suspended ? " (suspended)" : "",
                g_slot[i].waiting_on ? " on " : "",
                g_slot[i].waiting_on ? g_slot[i].waiting_on : "");
    }
}

static int live_locked(void) {
    int live = 0;
    for (int i = 1; i < g_slot_hi; i++)
        if (g_slot[i].used && g_slot[i].state != PSP_SCHED_DEAD) live++;
    return live;
}

void psp_sched_stop_all(const char *why) {
    /* Recorded before anything is killed: the caller is a guest thread that
     * will not exist past the thread exit below. */
    g_stop_reason = why;
    g_stopping    = 1;
    psp_os_lock(&g_lock);
    const int me = g_running;
    for (int i = 1; i < g_slot_hi; i++)
        if (g_slot[i].used) g_slot[i].state = PSP_SCHED_DEAD;
    g_slot[MAIN_SLOT].state = PSP_SCHED_RUNNING;
    give_token_locked(MAIN_SLOT, psp_clock_peek());
    psp_os_cond_broadcast(&g_turn);
    psp_os_unlock(&g_lock);
    if (me != MAIN_SLOT) host_exit();
}

const char *psp_sched_stop_reason(void) {
    return g_stop_reason;
}

int psp_sched_stopping(void) { return g_stopping; }

/* Not under the lock: the threads take it on their way out, and a join that
 * held it would deadlock against every one of them. `started` is set once,
 * before the thread exists, and never cleared while it does, so it is safe to
 * read here. Slots are walked whether or not they are still `used`: a
 * cancelled thread's slot may have been released while its host thread is
 * still parked, and it still has to be waited for. */
void psp_sched_join_all(void) {
    for (int i = 1; i < g_slot_hi; i++) {
        if (!g_slot[i].started || g_slot[i].joined) continue;
        psp_os_thread_join(&g_slot[i].host);
        g_slot[i].joined = 1;
    }
}

int psp_sched_drain(int timeout_s) {
    if (!g_threading) return 0;

    /* A timeout of zero or less means no limit -- someone is at the window
     * and will decide when to stop. It is expressed as a deadline a year out
     * rather than as a special case inside the wait: psp_os_cond_wait_until
     * takes an absolute time, and a saturated one is not portably safe --
     * pthread_cond_timedwait may reject an out-of-range timespec with EINVAL,
     * which reports as "not a timeout" and would turn waiting forever into a
     * busy loop against the lock. A year is not forever, and is long enough
     * to be. */
    const uint64_t span_ns = timeout_s > 0
        ? (uint64_t)timeout_s * 1000000000ull
        : 365ull * 24 * 3600 * 1000000000ull;
    uint64_t deadline_ns = psp_os_mono_ns() + span_ns;
    /* Time the guest spends held by a host pause (psprecomp/safepoint.h) is
     * not run time: the limit moves out by it, so a hold does not cut a
     * timed run short. */
    uint64_t held_us = psp_clock_held_us();

    /* The main context steps aside so the guest threads can run. It becomes
     * runnable again only when they are all finished -- or when none of them
     * can proceed, which handoff_locked reports as a deadlock. */
    psp_os_lock(&g_lock);
    int timed_out = 0, stalled = 0;
    while (live_locked()) {
        /* A loaded state's token is already its saving thread's; the main
         * context was waiting here when it was saved. */
        if (g_token_restored) {
            g_token_restored = 0;
            g_running = g_restored_running;
            psp_os_cond_broadcast(&g_turn);
        } else {
            g_slot[MAIN_SLOT].ctx   = psp_cpu;
            g_slot[MAIN_SLOT].state = PSP_SCHED_BLOCKED;
            if (handoff_locked() < 0) { stalled = 1; break; }
        }
        int rc;
        while ((rc = await_turn_deadline_locked(MAIN_SLOT, deadline_ns)) == -1) {
            const uint64_t held = psp_clock_held_us();
            if (held == held_us) break;
            deadline_ns += (held - held_us) * 1000u;
            held_us = held;
        }
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
        dump_locked(stderr, 0);
    } else if (stalled && live) {
        fprintf(stderr, "psprecomp: deadlock -- %d thread(s) alive, none runnable:\n", live);
        dump_locked(stderr, 1);
    }

    g_slot[MAIN_SLOT].state = PSP_SCHED_RUNNING;
    give_token_locked(MAIN_SLOT, psp_clock_peek());
    psp_os_unlock(&g_lock);
    return live;
}

/* Every live thread and what it is parked on. The drain prints this when it
 * gives up; a wait that cannot be satisfied needs exactly the same list, and
 * printing it there is the difference between "deadlock" and knowing which
 * semaphore nobody is going to signal. */
/* The census's thread lines: copied under the lock, printed without it,
 * since the names come from the thread manager. */
void psp_sched_census(FILE *out, uint32_t self) {
    static const char *const ST[] = { "ready", "running", "blocked", "sleeping", "dead" };
    static struct { uint32_t uid, entry; int slot, priority, suspended; psp_sched_state state; psp_park park; }
        copy[MAX_SCHED_THREADS];
    int n = 0;
    psp_os_lock(&g_lock);
    for (int i = 0; i < g_slot_hi; i++) {
        const sched_slot *t = &g_slot[i];
        if (!t->used || t->state == PSP_SCHED_DEAD) continue;
        copy[n].uid = t->uid; copy[n].entry = t->entry; copy[n].slot = i; copy[n].priority = t->priority;
        copy[n].suspended = t->suspended; copy[n].state = t->state; copy[n].park = t->park;
        n++;
    }
    psp_os_unlock(&g_lock);
    for (int i = 0; i < n; i++) {
        const int me = copy[i].uid == self;
        /* The host's own context, which ran module_start and now waits for
         * the threads to end: not a guest thread, and re-created on a load. */
        if (copy[i].slot == MAIN_SLOT && !me) {
            fprintf(out, "census:   (host main context: ran module_start, waits for the threads)\n");
            continue;
        }
        const char *name = psp_threadman_thread_name(copy[i].uid);
        fprintf(out, "census:   0x%08X %-24s prio %3d %-8s ", copy[i].uid, name,
                copy[i].priority, me ? "SAFE" : ST[copy[i].state]);
        if (me) fprintf(out, "at the safe point");
        else if (!copy[i].park.kind) fprintf(out, "not started: entry 0x%08X", copy[i].entry);
        else psp_census_print_park(out, &copy[i].park);
        if (copy[i].suspended) fprintf(out, " (suspended)");
        fprintf(out, "\n");
        if (!me) psp_census_row(out, copy[i].uid, name, ST[copy[i].state], &copy[i].park, copy[i].entry);
    }
}

void psp_sched_dump_threads(FILE *out) {
    psp_os_lock(&g_lock);
    dump_locked(out, 0);
    psp_os_unlock(&g_lock);
}

int psp_sched_live(void) {
    psp_os_lock(&g_lock);
    const int live = live_locked();
    psp_os_unlock(&g_lock);
    return live;
}

int psp_sched_terminate(uint32_t uid) {
    if (!g_threading) return 0;
    psp_os_lock(&g_lock);
    const int s = slot_of(uid);
    if (s < 0 || g_slot[s].state == PSP_SCHED_DEAD) {
        psp_os_unlock(&g_lock);
        return 0;
    }
    const int self = (s == g_self);
    const int self_is_main = (s == MAIN_SLOT);
    g_slot[s].state      = PSP_SCHED_DEAD;
    g_slot[s].waiting_on = NULL;
    g_slot[s].wake_at    = 0;
    /* Wake it wherever it is parked. await_turn_locked returns -1 for a slot
     * that has gone DEAD, and its caller leaves through psp_os_thread_exit. */
    psp_os_cond_broadcast(&g_turn);
    if (self) {
        (void)handoff_locked();
        psp_os_unlock(&g_lock);
        /* The main context is not a guest thread and has nowhere to exit to --
         * exiting it ends the thread the process was started on and
         * leaves the runner waiting for threads that will never finish. The
         * same guard psp_sched_exit carries, for the same reason. */
        if (!self_is_main) host_exit();
        return 1;
    }
    psp_os_unlock(&g_lock);
    return 1;
}

int psp_sched_can_wait(void) {
    return g_dispatch && !psp_interrupt_in_handler() && psp_intr_enabled();
}

int psp_sched_set_dispatch(int on) {
    const int was = g_dispatch;
    g_dispatch = on;
    return was;
}

void psp_sched_set_priority(uint32_t uid, int priority) {
    psp_os_lock(&g_lock);
    const int s = slot_of(uid);
    if (s >= 0) {
        g_slot[s].priority = priority;
        /* A ready thread whose priority is set joins the tail of its new
         * queue, as a thread that yields does. Measured for the caller itself
         * (threadprobe steps 61-62, fw 6.60: the tail even when the value is
         * unchanged); another ready thread is assumed to move the same way. */
        if (g_slot[s].state == PSP_SCHED_READY)
            ready_tail_locked(s, psp_clock_peek());
    }
    psp_os_unlock(&g_lock);
}

int psp_sched_rotate(int priority) {
    if (!g_threading) return 0;
    psp_os_lock(&g_lock);
    int head = -1;
    for (int i = 0; i < g_slot_hi; i++)
        if (runnable(i) && g_slot[i].priority == priority &&
            (head < 0 || ahead_of(i, head))) head = i;
    if (head >= 0) ready_tail_locked(head, psp_clock_peek());
    psp_os_unlock(&g_lock);
    return head >= 0;
}

int psp_sched_suspend(uint32_t uid) {
    if (!g_threading) return 0;
    psp_os_lock(&g_lock);
    const int s = slot_of(uid);
    if (s < 0) { psp_os_unlock(&g_lock); return 0; }
    /* A thread that has finished stays finished, and is not flagged. */
    if (g_slot[s].state == PSP_SCHED_DEAD) { psp_os_unlock(&g_lock); return 1; }
    /* The flag and nothing else. The wait it may be in -- what it is parked
     * on, its deadline, its place in the object's queue -- is left alone,
     * because on hardware it is still in that wait (threadprobe steps 36, 53;
     * see sched_slot.suspended). This used to replace the wait with a
     * SUSPENDED state and forget it, so a resume ended a sleep that only a
     * wakeup should have. */
    g_slot[s].suspended = 1;
    const int running = (s == g_running && s == g_self);
    psp_os_unlock(&g_lock);
    /* Ourselves (sceKernelSuspendThread refuses this, so only the runtime can
     * ask): stay READY, and the handoff will not pick us until resumed. */
    if (running) yield_as(YIELD);
    return 1;
}

int psp_sched_resume(uint32_t uid) {
    if (!g_threading) return 0;
    psp_os_lock(&g_lock);
    const int s = slot_of(uid);
    int urgent = 0;
    if (s >= 0 && g_slot[s].suspended) {
        g_slot[s].suspended = 0;
        /* Back into its ready queue at the tail, if its wait is over (or it
         * never had one); still waiting otherwise. Resuming a more urgent
         * thread runs it inside the call (threadprobe steps 52 and 54: `m1 W
         * m2`, `m2 Ws m3`), which the caller does with the answer. */
        if (g_slot[s].state == PSP_SCHED_READY) {
            ready_tail_locked(s, psp_clock_peek());
            urgent = g_running >= 0 &&
                     g_slot[s].priority < g_slot[g_running].priority;
        }
    }
    psp_os_unlock(&g_lock);
    return urgent;
}

int psp_sched_stats_of(uint32_t uid, psp_sched_stats *out) {
    memset(out, 0, sizeof *out);
    psp_os_lock(&g_lock);
    const int s = slot_of(uid);
    if (s >= 0) {
        out->run_us          = g_slot[s].run_us;
        out->releases        = g_slot[s].releases;
        out->thread_preempts = g_slot[s].thread_preempts;
        out->intr_preempts   = g_slot[s].intr_preempts;
        /* The turn in progress counts too (threadprobe step 13 reads its own
         * runClocks as nonzero during its first turn). */
        if (s == g_running) {
            const uint64_t now = psp_clock_peek();
            const uint64_t d = now > g_slot[s].run_since ? now - g_slot[s].run_since : 0;
            out->run_us += d ? d : 1;
        }
    }
    psp_os_unlock(&g_lock);
    return s >= 0;
}

psp_sched_state psp_sched_state_of(uint32_t uid) {
    psp_os_lock(&g_lock);
    const int s = slot_of(uid);
    const psp_sched_state st = s >= 0 ? g_slot[s].state : PSP_SCHED_DEAD;
    psp_os_unlock(&g_lock);
    return st;
}

int psp_sched_priority(uint32_t uid) {
    psp_os_lock(&g_lock);
    const int s = slot_of(uid);
    /* PSP priorities run 0..0x7F with 0 the most urgent, so anything past the
     * range sorts last. An unknown uid is a waiter whose thread no longer
     * exists, and it must never be picked ahead of one that does. */
    const int pri = s >= 0 ? g_slot[s].priority : 0x7FFFFFFF;
    psp_os_unlock(&g_lock);
    return pri;
}

uint32_t psp_sched_current(void) {
    /* From this host thread's own slot, not from whoever holds the token. The
     * two agree while a thread is running, which is why reading g_running here
     * looked correct -- but it answered 0 whenever the token was held by
     * nobody, and 0 is the main context's uid. Callers pass the result straight
     * back to psp_sched_block, so a guest thread parked the main context's slot
     * and left its own marked RUNNING. */
    psp_os_lock(&g_lock);
    const uint32_t uid = g_slot[g_self].uid;
    psp_os_unlock(&g_lock);
    return uid;
}

/* ---- save states (psprecomp/state.h) ----------------------------------------- */

/* How each slot continues from a loaded state (g_resume), decided when it is
 * saved:
 *
 *   FRESH      started and never run: it begins at its entry point, as ever;
 *   SAFEPOINT  the thread that saved, at the safe point inside its call;
 *   WAIT       parked in a firmware call that knows how to finish a wait it
 *              did not begin in this process (psp_hle_register_resume).
 *
 * The last two finish their call -- the rest of the handler, then what every
 * call ends with -- and then climb their guest stack (psp_resume_chain). */

/* A host frame between guest frames is fatal to resuming, unless it is a
 * replaced function a title resumes itself (psp_resume_override). */
static int nest_resumes(int n, const uint8_t *kind, const uint32_t *addr) {
    for (int i = 0; i < n && i < PSP_PARK_NEST_MAX; i++)
        if (kind[i] != PSP_NEST_REPLACED || !psp_resume_overridden(addr[i])) return 0;
    return 1;
}

static const char *sched_refuse(void) {
    static char why[160];
    const int me = g_self;
    if (!psp_safepoint_nid() || g_running != me || me == MAIN_SLOT) return "not at the safe point";
    if (g_slot[MAIN_SLOT].state != PSP_SCHED_BLOCKED) return "the module is still starting";
    uint8_t kinds[PSP_PARK_NEST_MAX];
    uint32_t addrs[PSP_PARK_NEST_MAX];
    const int nest = psp_census_self_nest(kinds, addrs, PSP_PARK_NEST_MAX);
    if (!nest_resumes(nest, kinds, addrs)) return "the safe point is inside a callback or a handler";
    for (int i = 1; i < g_slot_hi; i++) {
        const sched_slot *t = &g_slot[i];
        if (i == me || !t->used || t->state == PSP_SCHED_DEAD) continue;
        const psp_park *p = &t->park;
        if (!p->kind) continue;                                   /* never run */
        if (!nest_resumes(p->nest, p->nest_kind, p->nest_addr)) {
            snprintf(why, sizeof why, "thread 0x%08X is inside a callback or a handler", t->uid);
            return why;
        }
        if (p->kind == PSP_PARK_YIELD || p->kind == PSP_PARK_PREEMPT) {
            snprintf(why, sizeof why, "thread 0x%08X was switched away inside a firmware call", t->uid);
            return why;
        }
        if (p->calls != 1 || !psp_hle_resumable(p->nid)) {
            snprintf(why, sizeof why, "thread 0x%08X waits in %s, which cannot be resumed yet", t->uid,
                     p->nid && psp_hle_name(p->nid) ? psp_hle_name(p->nid) : "an unnamed call");
            return why;
        }
    }
    return NULL;
}

/* Called by psp_state_save before the kept variables are written: the
 * saving thread's registers into its slot, and every slot's way back. */
static int sched_prepare(void) {
    g_slot[g_running].ctx = psp_cpu;
    memset(g_resume, 0, sizeof g_resume);
    for (int i = 1; i < g_slot_hi; i++) {
        const sched_slot *t = &g_slot[i];
        if (!t->used || t->state == PSP_SCHED_DEAD) continue;
        if (i == g_running) {
            g_resume[i].kind = RESUME_SAFEPOINT;
            g_resume[i].nid = psp_safepoint_nid();
            g_resume[i].site = psp_cpu.r[PSP_REG_RA];
        } else if (!t->park.kind) {
            g_resume[i].kind = RESUME_FRESH;
        } else {
            g_resume[i].kind = RESUME_WAIT;
            g_resume[i].nid = t->park.nid;
            g_resume[i].site = t->park.site;
        }
    }
    return 0;
}

/* A restored thread's way back: finish the call it was in, then climb its
 * guest stack to the entry point, then end as a thread ends. */
static void resume_main(void *arg) {
    sched_slot *t = (sched_slot *)arg;
    g_self = (int)(t - g_slot);
    if (g_thread_hook) g_thread_hook();
    jmp_buf base;
    t_base = &base;
    if (setjmp(base)) { resume_slot(&g_slot[g_self]); return; }
    resume_slot(t);
}

static void resume_slot(sched_slot *t) {
    const int me = (int)(t - g_slot);
    if (g_resume[me].kind == RESUME_FRESH) { thread_run(t); return; }
    psp_hle_resume(g_resume[me].nid, g_resume[me].site, g_resume[me].kind == RESUME_SAFEPOINT);
    uint32_t missing = 0;
    if (psp_resume_chain(psp_cpu.r[PSP_REG_RA], 0, &missing)) {
        fprintf(stderr, "state: thread 0x%08X cannot continue: 0x%08X is no return site\n", t->uid, missing);
        psp_sched_stop_all("a loaded thread could not continue");
        return;
    }
    thread_end(t);
}

/* The first wait of a restored thread is the saved one: the slot already
 * says how it is parked. These wait for the token and answer as switch_away
 * and psp_sched_delay would have, had the wait begun in this process. */
void psp_sched_set_step(uint8_t step) { g_slot[g_self].step = step; }
uint8_t psp_sched_step(void) { return g_slot[g_self].step; }

int psp_sched_resume_block(uint64_t *deadline) {
    const int me = g_self;
    psp_os_lock(&g_lock);
    if (await_turn_locked(me) != 0) {
        g_slot[me].waiting_on = NULL;
        psp_os_unlock(&g_lock);
        host_exit();
        return PSP_SCHED_STRANDED;
    }
    const uint64_t deadline_us = g_slot[me].park.deadline;
    if (deadline) *deadline = deadline_us;
    const int woken = g_slot[me].woken;
    const int released = woken && g_slot[me].wake_reason == PSP_SCHED_WAKE_RELEASE;
    g_slot[me].waiting_on = NULL;
    g_slot[me].wake_at    = 0;
    psp_os_unlock(&g_lock);
    if (released) return PSP_SCHED_RELEASED;
    if (woken || !deadline_us) return PSP_SCHED_WOKEN;
    return PSP_SCHED_EXPIRED;
}

int psp_sched_resume_delay(uint64_t *deadline) {
    const int me = g_self;
    psp_os_lock(&g_lock);
    if (deadline) *deadline = g_slot[me].park.deadline;
    if (await_turn_locked(me) != 0) {
        psp_os_unlock(&g_lock);
        host_exit();
        return PSP_SCHED_EXPIRED;
    }
    const int woken = g_slot[me].woken, reason = g_slot[me].wake_reason;
    psp_os_unlock(&g_lock);
    if (woken && reason == PSP_SCHED_WAKE_RELEASE) return PSP_SCHED_RELEASED;
    return woken ? PSP_SCHED_WOKEN : PSP_SCHED_EXPIRED;
}

/* The saving thread: the token is its own. */
void psp_sched_resume_token(void) {
    psp_os_lock(&g_lock);
    (void)await_turn_locked(g_self);
    psp_os_unlock(&g_lock);
}

/* The restored table's host side, which belonged to the saving process:
 * every slot is without a host thread until one is started or adopted. */
static void forget_hosts(void) {
    for (int i = 0; i < MAX_SCHED_THREADS; i++) {
        sched_slot *t = &g_slot[i];
        memset(&t->host, 0, sizeof t->host);
        t->started = 0;
        t->joined = 1;
        /* The saving process's strings: what a restored wait is on is now
         * only the call it is in, which is enough for the thread dump. */
        const int parked = i && t->used && (t->state == PSP_SCHED_BLOCKED || t->state == PSP_SCHED_SLEEPING);
        t->park.what = parked && g_resume[i].kind == RESUME_WAIT ? psp_hle_name(g_resume[i].nid) : NULL;
        t->waiting_on = t->park.what;
    }
}

/* A host thread for every live slot, but `adopt`, whose host thread is
 * `self`: the loading thread, which becomes it. -1 for none. */
static int start_hosts(int adopt, psp_os_thread self, char *why, size_t size) {
    for (int i = 1; i < g_slot_hi; i++) {
        sched_slot *t = &g_slot[i];
        if (!t->used || t->state == PSP_SCHED_DEAD) continue;
        if (g_resume[i].kind == RESUME_NONE) {
            snprintf(why, size, "thread 0x%08X has no way back", t->uid);
            return -1;
        }
        t->joined = 0;
        if (i == adopt) t->host = self;
        else if (psp_os_thread_start(&t->host, resume_main, t, PSP_HOST_STACK_SIZE) != 0) {
            snprintf(why, size, "cannot start a host thread for 0x%08X", t->uid);
            return -1;
        }
        t->started = 1;
    }
    return 0;
}

static int sched_load(char *why, size_t size) {
    /* Nobody runs until the main context drains: the boot finishes first. */
    g_restored_running = g_running;
    g_running = -1;
    forget_hosts();
    psp_os_thread none;
    memset(&none, 0, sizeof none);
    if (start_hosts(-1, none, why, size)) return -1;
    g_token_restored = 1;
    return 0;
}

/* ---- a load while the game runs ---------------------------------------------- */

/* The thread at the safe point loads. First every other guest thread ends:
 * each is parked, waiting for a turn it will now never get -- a dead slot's
 * wait returns and its host thread exits -- and is joined, so none is left
 * behind however many loads a run makes. The main context goes on waiting
 * in its drain. Then the state replaces the table under the lock; the
 * loading thread takes over the state's saving thread, and every other live
 * slot gets a new host thread. Last, the loading thread drops its host
 * frames and continues as the saving thread did (psp_sched_state_jump), so
 * the GL context, which belongs to it, never changes thread. */
static psp_os_thread g_loader;

static const char *sched_live_refuse(void) {
    psp_os_lock(&g_lock);
    const int me = g_self, ok = me != MAIN_SLOT && g_running == me && t_base && psp_safepoint_nid();
    psp_os_unlock(&g_lock);
    return ok ? NULL : "not at the safe point";
}

static void sched_retire(void) {
    psp_os_lock(&g_lock);
    const int me = g_self;
    g_loader = g_slot[me].host;
    for (int i = 1; i < g_slot_hi; i++)
        if (i != me && g_slot[i].used) g_slot[i].state = PSP_SCHED_DEAD;
    psp_os_cond_broadcast(&g_turn);
    for (int i = 1; i < g_slot_hi; i++) {
        sched_slot *t = &g_slot[i];
        if (i == me || !t->started || t->joined) continue;
        psp_os_unlock(&g_lock);
        psp_os_thread_join(&t->host);
        psp_os_lock(&g_lock);
        t->joined = 1;
    }
    psp_os_unlock(&g_lock);
}

static int sched_reload(char *why, size_t size) {
    const int saver = g_running;
    forget_hosts();
    if (saver < 1 || saver >= g_slot_hi || g_resume[saver].kind != RESUME_SAFEPOINT) {
        snprintf(why, size, "the state has no thread at its safe point");
        return -1;
    }
    if (start_hosts(saver, g_loader, why, size)) return -1;
    g_self = saver;
    return 0;
}

void psp_sched_state_jump(void) {
    psp_hle_thread_reset();
    longjmp(*t_base, 1);
}

const char *psp_sched_state_refuse(void) { return sched_refuse(); }
int psp_sched_state_prepare(void) { return sched_prepare(); }
int psp_sched_state_load(char *why, size_t size) { return sched_load(why, size); }
const char *psp_sched_state_live_refuse(void) { return sched_live_refuse(); }
void psp_sched_state_retire(void) { sched_retire(); }
int psp_sched_state_reload(char *why, size_t size) { return sched_reload(why, size); }
void psp_sched_state_lock(int on) { if (on) psp_os_lock(&g_lock); else psp_os_unlock(&g_lock); }
