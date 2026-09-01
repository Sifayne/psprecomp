/* psprecomp — the queue of threads parked on a kernel object. See waitq.h. */

#include "waitq.h"
#include "psprecomp/hle.h"
#include "psprecomp/sched.h"
#include "psprecomp/clock.h"
#include "psprecomp/mem.h"

#include <string.h>

int psp_waitq_add(psp_waitq *q, uint32_t uid, uint32_t need, uint32_t mode,
                  uint32_t out) {
    if (q->n >= PSP_WAITQ_MAX) return -1;
    q->w[q->n++] = (psp_waiter){ uid, need, mode, out, 0, 0 };
    return 0;
}

int psp_waitq_drop(psp_waitq *q, uint32_t uid) {
    for (int i = 0; i < q->n; i++) {
        if (q->w[i].uid != uid) continue;
        memmove(&q->w[i], &q->w[i + 1],
                (size_t)(q->n - i - 1) * sizeof q->w[0]);
        q->n--;
        return 1;
    }
    return 0;
}

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
    return w;
}

int psp_waitq_count(const psp_waitq *q) { return q->n; }

static int release_all_as(psp_waitq *q, int reason) {
    int urgent = 0;
    for (int i = 0; i < q->n; i++) urgent |= psp_sched_wake_as(q->w[i].uid, reason);
    q->n = 0;
    return urgent;
}

int psp_waitq_release_all(psp_waitq *q) {
    return release_all_as(q, PSP_WAIT_WOKE_NORMAL);
}

int psp_waitq_cancel_all(psp_waitq *q) {
    return release_all_as(q, PSP_WAIT_WOKE_CANCELLED);
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
