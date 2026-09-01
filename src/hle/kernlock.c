/* psprecomp — the mutual-exclusion kernel objects.
 *
 * A mutex is not a semaphore with a count of one. It has an *owner*, so it can
 * be relocked by the thread that holds it and cannot be unlocked by one that
 * does not, and the whole error surface below exists to distinguish those
 * cases. Every rule here is transcribed from a hardware capture --
 * threads/mutex/{create,lock,unlock}.expected -- rather than inferred from what
 * a mutex usually is, because two of them are not what a mutex usually is.
 *
 * The waiter queue is the shared one; see waitq.h for why the signaller decides
 * who gets the object rather than waking everyone to re-test.
 */

#include "psprecomp/hle.h"
#include "psprecomp/clock.h"
#include "psprecomp/sched.h"
#include "waitq.h"

#include <stdio.h>
#include <string.h>

#define MAX_MUTEXES 128

/* Attribute bits a mutex accepts, measured rather than assumed.
 *
 * create.expected sweeps twelve values: 0x1, 0x100, 0x200, 0x800, 0xB00 and
 * 0xBFF are accepted; 0x400, 0xC00, 0x1000, 0x2000, 0x4000, 0x8000 and 0x10000
 * are refused. So bit 10 is illegal where bit 11 is legal, which no round mask
 * expresses -- the legal set is exactly 0xBFF, and this is a third object type
 * with a third attribute rule. */
#define MUTEX_ATTR_KNOWN   0xBFFu
#define MUTEX_ATTR_RECURSE 0x200u

typedef struct {
    uint32_t  uid;
    char      name[32];
    uint32_t  attr;
    int32_t   count;        /* how many times it is currently held */
    int32_t   init_count;
    uint32_t  owner;        /* thread uid holding it, 0 when free */
    int       used;
    psp_waitq q;
    char      waitdesc[64];
} psp_mutex;

static psp_mutex g_mutex[MAX_MUTEXES];

void psp_kernlock_reset(void) { memset(g_mutex, 0, sizeof g_mutex); }

static psp_mutex *find_mutex(uint32_t id) {
    for (int i = 0; i < MAX_MUTEXES; i++)
        if (g_mutex[i].used && g_mutex[i].uid == id) return &g_mutex[i];
    return NULL;
}

/* How much a lock may ask for.
 *
 * A non-recursive mutex takes exactly one; a recursive one takes any positive
 * number that does not carry the total past INT_MAX. Both halves are measured:
 * `Lock 0 => 2` is refused with ILLEGAL_COUNT without the attribute and
 * accepted with it, and `Lock 1 => INT_MAX` is refused with LOCK_OVERFLOW where
 * `Lock 1 => INT_MAX - 1` succeeds at exactly INT_MAX. */
static uint32_t lock_count_error(const psp_mutex *m, int32_t count) {
    if (count <= 0) return SCE_KERNEL_ERROR_ILLEGAL_COUNT;
    if (count > 1 && !(m->attr & MUTEX_ATTR_RECURSE))
        return SCE_KERNEL_ERROR_ILLEGAL_COUNT;
    if (m->count > INT32_MAX - count) return SCE_KERNEL_ERROR_MUTEX_LOCK_OVERFLOW;
    return SCE_KERNEL_ERROR_OK;
}

static void hle_CreateMutex(void) {
    /* (name, attr, initCount, option) */
    const uint32_t name = psp_arg(0);
    const uint32_t attr = psp_arg(1);
    const int32_t  init = (int32_t)psp_arg(2);

    if (!name) { psp_ret(SCE_KERNEL_ERROR_ERROR); return; }
    if (attr & ~MUTEX_ATTR_KNOWN) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_ATTR); return; }
    /* A mutex may be created already held -- `Positive count` reports
     * `init=1,current=1,lockThread=1`, so the creator owns it. More than one is
     * refused unless it is recursive, on the same rule a lock follows. */
    if (init < 0 || (init > 1 && !(attr & MUTEX_ATTR_RECURSE))) {
        psp_ret(SCE_KERNEL_ERROR_ILLEGAL_COUNT);
        return;
    }

    psp_mutex *m = NULL;
    for (int i = 0; i < MAX_MUTEXES; i++) if (!g_mutex[i].used) { m = &g_mutex[i]; break; }
    if (!m) { psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }

    memset(m, 0, sizeof *m);
    psp_str(name, m->name, sizeof m->name);
    m->attr       = attr;
    m->count      = init;
    m->init_count = init;
    m->owner      = init > 0 ? psp_sched_current() : 0;
    m->uid        = psp_threadman_next_uid();
    m->used       = 1;
    char nm[sizeof m->name];
    memcpy(nm, m->name, sizeof nm);
    snprintf(m->waitdesc, sizeof m->waitdesc, "sceKernelLockMutex(%s)", nm);
    psp_ret(m->uid);
}

static void hle_DeleteMutex(void) {
    psp_mutex *m = find_mutex(psp_arg(0));
    if (!m) { psp_ret(SCE_KERNEL_ERROR_NOT_FOUND_MUTEX); return; }
    const int urgent = psp_waitq_release_all(&m->q);
    m->used = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
    if (urgent) psp_sched_yield();
}

/* Hand the mutex to the next in line once it is free. Only one waiter can be
 * satisfied, because a mutex has one owner. */
static int mutex_release(psp_mutex *m) {
    if (m->count > 0) return 0;
    const int i = psp_waitq_pick(&m->q, m->attr);
    if (i < 0) return 0;
    const psp_waiter w = psp_waitq_take(&m->q, i);
    m->count = (int32_t)w.need;
    m->owner = w.uid;
    return psp_sched_wake(w.uid);
}

/* The common body of the two blocking locks and the non-blocking one. */
static void mutex_lock(int may_block, int has_timeout) {
    const uint32_t id      = psp_arg(0);
    const int32_t  count   = (int32_t)psp_arg(1);
    const uint32_t tmo_ptr = has_timeout ? psp_arg(2) : 0;

    psp_mutex *m = find_mutex(id);
    if (!m) { psp_ret(SCE_KERNEL_ERROR_NOT_FOUND_MUTEX); return; }

    const uint32_t bad = lock_count_error(m, count);
    if (bad) { psp_ret(bad); return; }

    const uint64_t deadline = psp_wait_deadline(tmo_ptr);
    const uint32_t me = psp_sched_current();

    /* Free, and nobody ahead of us. */
    if (m->count == 0 && psp_waitq_count(&m->q) == 0) {
        m->count = count;
        m->owner = me;
        psp_wait_writeback(tmo_ptr, deadline);
        psp_ret(SCE_KERNEL_ERROR_OK);
        return;
    }

    /* Ours already. Relocking is what the recursive attribute is for, and
     * without it hardware refuses rather than blocking -- a thread waiting for
     * a lock it already holds would never be released. */
    if (m->owner == me) {
        if (!(m->attr & MUTEX_ATTR_RECURSE)) {
            psp_ret(SCE_KERNEL_ERROR_MUTEX_RECURSIVE);
            return;
        }
        m->count += count;
        psp_wait_writeback(tmo_ptr, deadline);
        psp_ret(SCE_KERNEL_ERROR_OK);
        return;
    }

    if (!may_block) { psp_ret(SCE_KERNEL_ERROR_MUTEX_LOCKED); return; }

    if (psp_waitq_add(&m->q, me, (uint32_t)count, 0, 0) != 0) {
        psp_ret(SCE_KERNEL_ERROR_NO_MEMORY);
        return;
    }
    const int rc = psp_sched_block_until(me, PSP_SCHED_BLOCKED, m->waitdesc,
                                         deadline);

    m = find_mutex(id);
    if (!m) { psp_ret(SCE_KERNEL_ERROR_WAIT_DELETE); return; }

    if (rc == PSP_SCHED_WOKEN) {
        /* mutex_release took it on our behalf. */
        psp_wait_writeback(tmo_ptr, deadline);
        psp_ret(SCE_KERNEL_ERROR_OK);
        return;
    }

    psp_waitq_drop(&m->q, me);
    if (rc == PSP_SCHED_EXPIRED) {
        psp_wait_writeback(tmo_ptr, deadline);
        psp_ret(SCE_KERNEL_ERROR_WAIT_TIMEOUT);
        return;
    }
    psp_ret(SCE_KERNEL_ERROR_WAIT_TIMEOUT);
}

static void hle_LockMutex(void)    { mutex_lock(1, 1); }
static void hle_TryLockMutex(void) { mutex_lock(0, 0); }

static void hle_UnlockMutex(void) {
    const uint32_t id    = psp_arg(0);
    const int32_t  count = (int32_t)psp_arg(1);

    psp_mutex *m = find_mutex(id);
    if (!m) { psp_ret(SCE_KERNEL_ERROR_NOT_FOUND_MUTEX); return; }

    /* The count rules are the lock's, minus the overflow check. */
    if (count <= 0 || (count > 1 && !(m->attr & MUTEX_ATTR_RECURSE))) {
        psp_ret(SCE_KERNEL_ERROR_ILLEGAL_COUNT);
        return;
    }
    /* Not held at all, and holding it fewer times than is being given back:
     * two different mistakes with two different codes, both measured. */
    if (m->count == 0)     { psp_ret(SCE_KERNEL_ERROR_MUTEX_UNLOCKED); return; }
    if (count > m->count)  { psp_ret(SCE_KERNEL_ERROR_MUTEX_UNLOCK_UNDERFLOW); return; }

    m->count -= count;
    if (m->count == 0) m->owner = 0;
    const int urgent = mutex_release(m);
    psp_ret(SCE_KERNEL_ERROR_OK);
    if (urgent) psp_sched_yield();
}

/* Free it outright and report how many were waiting, which is the only way a
 * caller can find that out. */
static void hle_CancelMutex(void) {
    psp_mutex *m = find_mutex(psp_arg(0));
    if (!m) { psp_ret(SCE_KERNEL_ERROR_NOT_FOUND_MUTEX); return; }
    const uint32_t out = psp_arg(2);
    if (out) psp_write32(out, (uint32_t)psp_waitq_count(&m->q));
    m->count = (int32_t)psp_arg(1);
    m->owner = 0;
    const int urgent = psp_waitq_release_all(&m->q);
    psp_ret(SCE_KERNEL_ERROR_OK);
    if (urgent) psp_sched_yield();
}

static void hle_ReferMutexStatus(void) {
    const psp_mutex *m = find_mutex(psp_arg(0));
    const uint32_t info = psp_arg(1);
    if (!m)    { psp_ret(SCE_KERNEL_ERROR_NOT_FOUND_MUTEX); return; }
    if (!info) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
    /* The size field is in and out: the caller sets how much room it has and
     * hardware reports how much it filled. A caller offering *zero* gets zero
     * back and nothing written -- `0: 00000000 => 0` in threads/refer.expected,
     * against `=> 104` for both -1 and 1, so it is not a min. */
    if (psp_read32(info) == 0) { psp_ret(SCE_KERNEL_ERROR_OK); return; }
    psp_write32(info +  0, 56);
    psp_threadman_write_name(info + 4, m->name);
    psp_write32(info + 36, m->attr);
    psp_write32(info + 40, (uint32_t)m->init_count);
    psp_write32(info + 44, (uint32_t)m->count);
    psp_write32(info + 48, m->owner);
    psp_write32(info + 52, (uint32_t)psp_waitq_count(&m->q));
    psp_ret(SCE_KERNEL_ERROR_OK);
}

void psp_kernlock_register(void) {
    psp_hle_register(0xB7D098C6, "ThreadManForUser", "sceKernelCreateMutex",      hle_CreateMutex);
    psp_hle_register(0xF8170FBE, "ThreadManForUser", "sceKernelDeleteMutex",      hle_DeleteMutex);
    psp_hle_register(0xB011B11F, "ThreadManForUser", "sceKernelLockMutex",        hle_LockMutex);
    /* The CB form additionally pumps the thread's callbacks while it waits, and
     * callbacks are delivered by sceKernelCheckCallback here, so the two differ
     * only in that -- the same split sceKernelWaitSemaCB already has. */
    psp_hle_register(0x5BF4DD27, "ThreadManForUser", "sceKernelLockMutexCB",      hle_LockMutex);
    psp_hle_register(0x0DDCD2C9, "ThreadManForUser", "sceKernelTryLockMutex",     hle_TryLockMutex);
    psp_hle_register(0x6B30100F, "ThreadManForUser", "sceKernelUnlockMutex",      hle_UnlockMutex);
    psp_hle_register(0x87D9223C, "ThreadManForUser", "sceKernelCancelMutex",      hle_CancelMutex);
    psp_hle_register(0xA9C2CB9A, "ThreadManForUser", "sceKernelReferMutexStatus", hle_ReferMutexStatus);
}
