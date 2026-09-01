/* psprecomp — the queue of threads parked on a kernel object. See waitq.h. */

#include "waitq.h"
#include "psprecomp/sched.h"

#include <string.h>

int psp_waitq_add(psp_waitq *q, uint32_t uid, uint32_t need, uint32_t mode,
                  uint32_t out) {
    if (q->n >= PSP_WAITQ_MAX) return -1;
    q->w[q->n++] = (psp_waiter){ uid, need, mode, out };
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

int psp_waitq_release_all(psp_waitq *q) {
    int urgent = 0;
    for (int i = 0; i < q->n; i++) urgent |= psp_sched_wake(q->w[i].uid);
    q->n = 0;
    return urgent;
}
