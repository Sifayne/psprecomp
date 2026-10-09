/* psprecomp — the queue of threads parked on a kernel object. See waitq.h. */

#include "waitq.h"
#include "psprecomp/hle.h"
#include "psprecomp/sched.h"
#include "psprecomp/clock.h"
#include "psprecomp/mem.h"
#include "psprecomp/state.h"

#include <string.h>

/* Which queue each parked thread is in; see psp_waitq_leave. One entry per
 * waiting thread, kept packed. */
#define WAITQ_PARKED_MAX 512
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

int psp_waitq_drop(psp_waitq *q, uint32_t uid) {
    for (int i = 0; i < q->n; i++) {
        if (q->w[i].uid != uid) continue;
        memmove(&q->w[i], &q->w[i + 1],
                (size_t)(q->n - i - 1) * sizeof q->w[0]);
        q->n--;
        parked_clear(uid, q);
        return 1;
    }
    return 0;
}

void psp_waitq_reset(void) { g_nparked = 0; }

/* A save state keeps the table as it is. Every queue is inside an object the
 * state keeps too, so a load moves each pointer by the distance they all
 * moved (psp_state_delta). */
static const char *waitq_refuse(void) {
    for (int i = 0; i < g_nparked; i++)
        if (!psp_state_is_kept(g_parked[i].q)) return "a thread waits on an object the state does not keep";
    return NULL;
}

static int waitq_load(psp_state_reader *r, char *why, size_t size) {
    (void)r; (void)why; (void)size;
    for (int i = 0; i < g_nparked; i++)
        g_parked[i].q = (psp_waitq *)((char *)g_parked[i].q + psp_state_delta());
    return 0;
}

void psp_waitq_keep(void) {
    static const psp_state_part part = { "waitq", waitq_refuse, NULL, waitq_load };
    PSP_STATE_KEEP(g_parked);
    PSP_STATE_KEEP(g_nparked);
    psp_state_register(&part);
}

int psp_waitq_leave(uint32_t uid) {
    const int p = parked_find(uid);
    if (p < 0) return 0;
    psp_waitq *q = g_parked[p].q;
    for (int i = 0; i < q->n; i++) {
        if (q->w[i].uid != uid) continue;
        if (q->w[i].nout) psp_write32(q->w[i].nout, q->w[i].done);
        (void)psp_waitq_drop(q, uid);
        if (q->mirror) psp_write32(q->mirror, (uint32_t)q->n);
        return 1;
    }
    g_parked[p] = g_parked[--g_nparked];
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
