/* psprecomp — the queue of threads parked on a kernel object. See waitq.h. */

#include "waitq.h"
#include "psprecomp/hle.h"
#include "psprecomp/sched.h"
#include "psprecomp/clock.h"
#include "psprecomp/mem.h"
#include "psprecomp/state.h"

#include <stdio.h>
#include <string.h>

/* Which queue each parked thread is in; see psp_waitq_leave. One entry per
 * waiting thread, kept packed, with room for every thread the scheduler can
 * hold. At 512 the 513th thread to park got no entry, and psp_waitq_leave
 * then missed it on a timeout or a release. */
#define WAITQ_PARKED_MAX PSP_SCHED_THREADS_MAX
static struct { uint32_t uid; psp_waitq *q; } g_parked[WAITQ_PARKED_MAX];
static int g_nparked;

static int parked_find(uint32_t uid) {
    for (int i = 0; i < g_nparked; i++) if (g_parked[i].uid == uid) return i;
    return -1;
}

static void parked_set(uint32_t uid, psp_waitq *q) {
    int i = parked_find(uid);
    if (i < 0) {
        if (g_nparked >= WAITQ_PARKED_MAX) return;
        i = g_nparked++;
    }
    g_parked[i].uid = uid;
    g_parked[i].q   = q;
}

static void parked_clear(uint32_t uid, const psp_waitq *q) {
    const int i = parked_find(uid);
    if (i < 0 || g_parked[i].q != q) return;
    g_parked[i] = g_parked[--g_nparked];
}

int psp_waitq_add(psp_waitq *q, uint32_t uid, uint32_t need, uint32_t mode,
                  uint32_t out) {
    if (q->n >= PSP_WAITQ_MAX) return -1;
    q->w[q->n++] = (psp_waiter){ uid, need, mode, out, 0, 0 };
    if (uid == psp_sched_current()) parked_set(uid, q);
    return 0;
}

int psp_waitq_find(const psp_waitq *q, uint32_t uid) {
    for (int i = 0; i < q->n; i++) if (q->w[i].uid == uid) return i;
    return -1;
}

int psp_waitq_drop(psp_waitq *q, uint32_t uid) {
    const int i = psp_waitq_find(q, uid);
    if (i < 0) return 0;
    (void)psp_waitq_take(q, i);
    return 1;
}

void psp_waitq_reset(void) { g_nparked = 0; }

/* A save state keeps the table as it is. Every queue is inside an object the
 * state keeps too, so a load moves each pointer to where that object is now
 * (psp_state_moved). */
static const char *waitq_refuse(void) {
    for (int i = 0; i < g_nparked; i++)
        if (!psp_state_is_kept(g_parked[i].q)) return "a thread waits on an object the state does not keep";
    return NULL;
}

static int waitq_load(psp_state_reader *r, char *why, size_t size) {
    (void)r;
    for (int i = 0; i < g_nparked; i++) {
        psp_waitq *q = (psp_waitq *)psp_state_moved(g_parked[i].q);
        if (!q) { snprintf(why, size, "a parked thread's queue is in no kept variable"); return -1; }
        g_parked[i].q = q;
    }
    return 0;
}

void psp_waitq_keep(void) {
    static const psp_state_part part = { .name = "waitq", .refuse = waitq_refuse, .load = waitq_load };
    PSP_STATE_KEEP(g_parked);
    PSP_STATE_KEEP(g_nparked);
    psp_state_register(&part);
}

/* `answered` is whether the thread's call will return, and so whether it is
 * told what it was handed. */
static int leave(uint32_t uid, int answered) {
    const int p = parked_find(uid);
    if (p < 0) return 0;
    psp_waitq *q = g_parked[p].q;
    const int i = psp_waitq_find(q, uid);
    if (i < 0) {
        g_parked[p] = g_parked[--g_nparked];
        return 0;
    }
    if (answered && q->w[i].nout) psp_write32(q->w[i].nout, q->w[i].done);
    (void)psp_waitq_take(q, i);
    if (q->mirror) psp_write32(q->mirror, (uint32_t)q->n);
    return 1;
}

int psp_waitq_leave(uint32_t uid) { return leave(uid, 1); }

int psp_waitq_leave_terminated(uint32_t uid) { return leave(uid, 0); }

int psp_waitq_pick(const psp_waitq *q, uint32_t attr) {
    if (q->n == 0) return -1;
    /* First-come is the array order, because add appends and take closes the
     * gap -- so the queue is its own FIFO and needs no arrival stamp. */
    if (!(attr & PSP_WAITQ_PRIORITY)) return 0;

    int best = 0;
    int best_pri = psp_sched_priority(q->w[0].uid);
    for (int i = 1; i < q->n; i++) {
        const int pri = psp_sched_priority(q->w[i].uid);
        /* Strictly more urgent, so equal priorities keep arrival order. */
        if (pri < best_pri) { best = i; best_pri = pri; }
    }
    return best;
}

psp_waiter psp_waitq_take(psp_waitq *q, int i) {
    const psp_waiter w = q->w[i];
    memmove(&q->w[i], &q->w[i + 1], (size_t)(q->n - i - 1) * sizeof q->w[0]);
    q->n--;
    parked_clear(w.uid, q);
    return w;
}

int psp_waitq_count(const psp_waitq *q) { return q->n; }

static int release_all_as(psp_waitq *q, int reason) {
    int urgent = 0;
    for (int i = 0; i < q->n; i++) {
        parked_clear(q->w[i].uid, q);
        urgent |= psp_sched_wake_as(q->w[i].uid, reason);
    }
    q->n = 0;
    return urgent;
}

int psp_waitq_release_all(psp_waitq *q) {
    return release_all_as(q, PSP_WAIT_WOKE_NORMAL);
}

int psp_waitq_cancel_all(psp_waitq *q) {
    return release_all_as(q, PSP_WAIT_WOKE_CANCELLED);
}

/* In a function of its own, apart from the object both strings live in: the
 * compiler cannot see there that two distinct members never overlap. */
void psp_wait_describe(char *desc, size_t size, const char *call, const char *name) {
    snprintf(desc, size, "%s(%s)", call, name);
}

static int g_cb_call;

void psp_wait_cb_pending(int on) { g_cb_call = on; }

uint64_t psp_wait_deadline(uint32_t tmo_ptr) {
    /* Past the caller's argument checks and not yet blocked: the one moment a
     * CB wait delivers. Cleared first, so a handler that itself waits does not
     * recurse here. */
    if (g_cb_call) { g_cb_call = 0; psp_threadman_run_callbacks(); }
    if (!tmo_ptr) return 0;
    const uint32_t usec = psp_read32(tmo_ptr);
    if (usec) return psp_clock_peek() + usec;
    /* A zero timeout is a deadline that has *already* arrived, not the shortest
     * future one. The difference is observable: a zero-timeout lock that
     * succeeds immediately reports `0ms left`, and giving it a deadline one
     * microsecond out reported 1. The handoff promotes a sleeper whose moment
     * has passed on its very next pass, so this expires through the ordinary
     * path either way. Guarded against a clock still reading zero, where the
     * scheduler would read the deadline as "none". */
    const uint64_t now = psp_clock_peek();
    return now ? now : 1;
}

void psp_wait_writeback(uint32_t tmo_ptr, uint64_t deadline) {
    if (!tmo_ptr) return;
    const uint64_t now = psp_clock_peek();
    psp_write32(tmo_ptr, now < deadline ? (uint32_t)(deadline - now) : 0);
}

int psp_wait_end_woken(int rc, uint32_t tmo_ptr, uint64_t deadline) {
    if (rc != PSP_SCHED_WOKEN) return 0;
    const int why = psp_sched_wake_reason();
    if (why != PSP_WAIT_WOKE_SATISFIED && why != PSP_WAIT_WOKE_CANCELLED) return 0;
    psp_wait_writeback(tmo_ptr, deadline);
    psp_ret(why == PSP_WAIT_WOKE_SATISFIED ? SCE_KERNEL_ERROR_OK
                                           : SCE_KERNEL_ERROR_WAIT_CANCEL);
    return 1;
}

int psp_wait_end_found(int rc, const void *object, uint32_t tmo_ptr,
                       uint64_t deadline) {
    if (object && rc != PSP_SCHED_WOKEN) return 0;
    /* A wait ended by the object's deletion still reports how much of the
     * timeout was left. It is easy to miss because the object is gone, but the
     * caller's timeout word is the caller's, not the object's: eight tests
     * across fpl and msgpipe print `timeout = 8ms remaining` where the
     * unwritten word still said 10. */
    psp_wait_writeback(tmo_ptr, deadline);
    psp_ret(object ? SCE_KERNEL_ERROR_OK : SCE_KERNEL_ERROR_WAIT_DELETE);
    return 1;
}

/* Forced out by sceKernelReleaseWaitThread it is RELEASE_WAIT (threadprobe
 * step 70, fw 6.60), not a timeout. */
void psp_wait_give_up(int rc, uint32_t tmo_ptr, uint64_t deadline,
                      const char *call) {
    if (rc == PSP_SCHED_STRANDED && !tmo_ptr) {
        psp_wait_deadlock(call);
        psp_ret(SCE_KERNEL_ERROR_OK);
        return;
    }
    /* A stranded wait is told its timeout ran out, so none of it is left,
     * though the clock never came to the deadline. */
    if (rc == PSP_SCHED_STRANDED) psp_write32(tmo_ptr, 0);
    else psp_wait_writeback(tmo_ptr, deadline);
    psp_ret(rc == PSP_SCHED_RELEASED ? SCE_KERNEL_ERROR_RELEASE_WAIT
                                     : SCE_KERNEL_ERROR_WAIT_TIMEOUT);
}

/* Reporting a timeout here is a lie, and a load-bearing one: Armored Core's
 * user_main waits on its game thread with no timeout, and on being told the
 * wait timed out it deletes that thread -- still live, mid-frame -- and quits.
 * The whole run ended in a screen clear because of it.
 *
 * So the run stops and says why. A caller that *did* pass a timeout gets one,
 * because for that caller a timeout is a true answer. */
void psp_wait_deadlock(const char *call) {
    fprintf(stderr,
        "psprecomp: %s cannot be satisfied -- no thread is runnable, so nothing\n"
        "  can ever signal it, and the caller passed no timeout. Reporting a\n"
        "  timeout would make the guest act on a falsehood, so the run stops\n"
        "  here instead. Live threads:\n", call);
    psp_sched_dump_threads(stderr);
    /* The reason rides along so the boot summary can tell a stopped run from
     * one that finished -- both leave zero threads alive. */
    psp_sched_stop_all(call);
}
