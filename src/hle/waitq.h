/* psprecomp — the queue of threads parked on a kernel object.
 *
 * Every waitable PSP object releases its waiters in one of two orders, and
 * which one is bit 0x100 of the attribute it was created with: clear means
 * first-come-first-served, set means most-urgent-first. That bit is 0x100 for
 * semaphores, event flags, mutexes, fixed and variable pools, mailboxes and
 * message pipes alike -- PSP_FPL_ATTR_PRIORITY, PSP_VPL_ATTR_PRIORITY and
 * PSP_MBX_ATTR_PRIORITY are the same number. One queue therefore serves all of
 * them, and the order is a property of the object rather than of the code that
 * signals it.
 *
 * ## Why the order is worth implementing rather than approximating
 *
 * The obvious shortcut is to wake every waiter and let each re-test its own
 * condition. It is simple, it cannot miss a wakeup, and it is wrong in a way the
 * tests measure directly: the thread that gets the object is then whichever the
 * *scheduler* picks, which is always the most urgent one, so a FIFO object
 * behaves like a priority object and a late high-priority arrival barges past
 * threads that have been waiting. threads/semaphores/fifo and threads/vpl/fifo
 * exist to catch exactly that.
 *
 * So the signaller decides. It walks the queue in release order, satisfies
 * whoever it can, and wakes only those -- which also means a released waiter
 * does not have to re-check anything, because the thing it waited for has
 * already been handed to it.
 *
 * ## What the entries mean
 *
 * `need`, `mode` and `out` are three words the object gives meaning to: a
 * semaphore's count, an event flag's bit pattern and wait mode and where to
 * write the pattern it woke on, a pool's requested size. The queue does not
 * interpret them; it only keeps them with the waiter, because the signaller
 * needs them to decide and the waiter's own frame is not reachable from there.
 */
#ifndef PSPRECOMP_WAITQ_H
#define PSPRECOMP_WAITQ_H

#include <stdint.h>

/* The attribute bit that selects most-urgent-first over first-come. */
#define PSP_WAITQ_PRIORITY 0x100u

/* A PSP kernel allows far fewer than this to wait on one object, and a queue
 * that fills refuses to grow rather than dropping its oldest: losing the head
 * of a FIFO is the one failure that would be invisible in the output. */
#define PSP_WAITQ_MAX 32

typedef struct {
    uint32_t uid;
    uint32_t need, mode, out;
} psp_waiter;

typedef struct {
    psp_waiter w[PSP_WAITQ_MAX];   /* kept in arrival order */
    int        n;
} psp_waitq;

/* Append a waiter. Returns 0, or -1 if the queue is full. */
int  psp_waitq_add(psp_waitq *q, uint32_t uid, uint32_t need, uint32_t mode,
                   uint32_t out);

/* Remove a waiter by uid, wherever it sits. For a thread that stopped waiting
 * without being released -- its timeout elapsed, the object was deleted under
 * it, or the guest terminated it. Returns 1 if it was there. */
int  psp_waitq_drop(psp_waitq *q, uint32_t uid);

/* The index of the waiter to release next under `attr`, or -1 when empty.
 * Ties among equal priorities go to whoever arrived first, which is what makes
 * a priority queue still fair within a level. */
int  psp_waitq_pick(const psp_waitq *q, uint32_t attr);

/* Remove the waiter at `i` and return it. */
psp_waiter psp_waitq_take(psp_waitq *q, int i);

int  psp_waitq_count(const psp_waitq *q);

/* Wake everyone and empty the queue, for delete and cancel -- where no waiter
 * is being *satisfied* and each has to discover for itself that its object is
 * gone. Returns nonzero if any of them outranks the caller. */
int  psp_waitq_release_all(psp_waitq *q);

#endif /* PSPRECOMP_WAITQ_H */
