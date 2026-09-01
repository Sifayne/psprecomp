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
#include "psprecomp/mem.h"
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

static void lw_reset(void);

void psp_kernlock_reset(void) { memset(g_mutex, 0, sizeof g_mutex); lw_reset(); }

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
    if (may_block && !psp_sched_can_wait()) {
        psp_ret(SCE_KERNEL_ERROR_CAN_NOT_WAIT); return;
    }
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

    /* Cancelled rather than deleted: the object is still there, so looking it
     * up says nothing, and only the waker knew. */
    if (rc == PSP_SCHED_WOKEN && psp_sched_wake_reason() == PSP_WAIT_WOKE_CANCELLED) {
        psp_wait_writeback(tmo_ptr, deadline);
        psp_ret(SCE_KERNEL_ERROR_WAIT_CANCEL);
        return;
    }
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
    /* And held by somebody else, which reads as the same mistake: you do not
     * have it. mutex/unlock has one thread lock a free mutex and then unlocks
     * it from main -- `Locked 0 => 1: ... main=800201C5` -- against the line
     * above it where main created the mutex locked and unlocking works. Same
     * call, same count, and only the owner differs. */
    if (m->owner != psp_sched_current()) {
        psp_ret(SCE_KERNEL_ERROR_MUTEX_UNLOCKED);
        return;
    }
    if (count > m->count)  { psp_ret(SCE_KERNEL_ERROR_MUTEX_UNLOCK_UNDERFLOW); return; }

    m->count -= count;
    if (m->count == 0) m->owner = 0;
    const int urgent = mutex_release(m);
    psp_ret(SCE_KERNEL_ERROR_OK);
    if (urgent) psp_sched_yield();
}

/* Free it outright and report how many were waiting, which is the only way a
 * caller can find that out. */
/* sceKernelCancelMutex(uid, count, waitThreadsOut)
 *
 * It does not merely release the waiters: it re-arms the mutex at a count of
 * the caller's choosing, and mutex/cancel sweeps that count.
 *
 *     Normal (1):           OK          current=1, lockThread=1
 *     Greater than max (3): 800201BD    current=1, lockThread=1   (unchanged)
 *     Zero (0):             OK          current=0, lockThread=0
 *     Negative -3, -1:      OK          current=0, lockThread=0
 *
 * Three rules in four lines. The ceiling is the one a lock obeys -- one, for a
 * mutex that is not recursive -- and exceeding it leaves the object *and* the
 * caller's wait-count word untouched, which the second half of the file checks
 * by pre-seeding that word with 99 and finding it still there. A negative count
 * is not a count at all; it means unlocked, exactly as zero does. We assigned
 * whatever we were handed, so the mutex read back `current=-3`. */
static void hle_CancelMutex(void) {
    psp_mutex *m = find_mutex(psp_arg(0));
    if (!m) { psp_ret(SCE_KERNEL_ERROR_NOT_FOUND_MUTEX); return; }
    const int32_t  count = (int32_t)psp_arg(1);
    const uint32_t out   = psp_arg(2);

    /* Only the non-recursive ceiling is measured here; a recursive mutex is
     * left unbounded rather than given a number no capture supports. */
    if (!(m->attr & MUTEX_ATTR_RECURSE) && count > 1) {
        psp_ret(SCE_KERNEL_ERROR_ILLEGAL_COUNT);
        return;
    }

    if (out) psp_write32(out, (uint32_t)psp_waitq_count(&m->q));
    m->count = count > 0 ? count : 0;
    m->owner = m->count > 0 ? psp_sched_current() : 0;
    const int urgent = psp_waitq_cancel_all(&m->q);
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
    /* "Nobody holds it" is **-1**, not zero, and the tests read that field
     * through `info.lockThread == -1 ? 0 : 1` -- so a zero we wrote for an
     * unlocked mutex printed as *locked*. Which also means the owner cannot be
     * derived from the uid alone here: the main context's uid is 0, so a mutex
     * it holds would be indistinguishable from a free one. The count is what
     * says whether it is held. */
    psp_write32(info + 48, m->count > 0 ? m->owner : 0xFFFFFFFFu);
    psp_write32(info + 52, (uint32_t)psp_waitq_count(&m->q));
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void mutex_list(int type, uint32_t out, int max, int *count) {
    if (type != PSP_TMID_MUTEX) return;
    for (int i = 0; i < MAX_MUTEXES; i++) {
        if (!g_mutex[i].used) continue;
        if (out && *count < max) psp_write32(out + (uint32_t)*count * 4, g_mutex[i].uid);
        (*count)++;
    }
}

/* A CB wait delivers the calling thread's callbacks and then does exactly what
 * its plain counterpart does. These were registered straight to those
 * counterparts, which waits correctly and delivers nothing. */
static void hle_LockMutexCB(void) { psp_threadman_run_callbacks(); hle_LockMutex(); }

void psp_kernlock_register(void) {
    psp_threadman_add_lister(mutex_list);
    psp_hle_register(0xB7D098C6, "ThreadManForUser", "sceKernelCreateMutex",      hle_CreateMutex);
    psp_hle_register(0xF8170FBE, "ThreadManForUser", "sceKernelDeleteMutex",      hle_DeleteMutex);
    psp_hle_register(0xB011B11F, "ThreadManForUser", "sceKernelLockMutex",        hle_LockMutex);
    /* The CB form additionally pumps the thread's callbacks while it waits, and
     * callbacks are delivered by sceKernelCheckCallback here, so the two differ
     * only in that -- the same split sceKernelWaitSemaCB already has. */
    psp_hle_register(0x5BF4DD27, "ThreadManForUser", "sceKernelLockMutexCB",      hle_LockMutexCB);
    psp_hle_register(0x0DDCD2C9, "ThreadManForUser", "sceKernelTryLockMutex",     hle_TryLockMutex);
    psp_hle_register(0x6B30100F, "ThreadManForUser", "sceKernelUnlockMutex",      hle_UnlockMutex);
    psp_hle_register(0x87D9223C, "ThreadManForUser", "sceKernelCancelMutex",      hle_CancelMutex);
    psp_hle_register(0xA9C2CB9A, "ThreadManForUser", "sceKernelReferMutexStatus", hle_ReferMutexStatus);
    psp_kernlock_register_lw();
}

/* ---- lwmutex ---------------------------------------------------------------
 *
 * The "lightweight" in the name is not about size, it is about *where the state
 * lives*: an lwmutex keeps its count, owner and attributes in a 32-byte
 * workarea the **guest** owns, and the uncontended lock and unlock are done
 * there without the kernel being involved at all. Only blocking, and only
 * asking about the object by name, needs a kernel record.
 *
 * That is not an inference. threads/lwmutex/{lock,unlock}.expected run every
 * case twice, once against a real workarea and once against a hand-forged one
 * whose uid is zero and whose pad words are 0xDEADBEEF -- and the forged one
 * *locks and unlocks successfully* while `sceKernelReferLwMutexStatus` on it
 * answers NOT_FOUND. So the arithmetic reads and writes guest memory and the
 * lookup is a separate question, which is what the code below does.
 *
 * Two conventions inside one object, both measured, and neither derivable from
 * the other: the workarea's `thread` field is **0** when the mutex is free,
 * while the info block's `lockThread` is **-1**.
 */

/* Fourth object type, fourth attribute rule: 0x3FF here against the mutex's
 * 0xBFF, so bit 11 is legal for one and not the other. Measured from
 * create.expected, which accepts 0x300 and 0x3FF and refuses 0x400 and 0x800. */
#define LWMUTEX_ATTR_KNOWN   0x3FFu
#define LWMUTEX_ATTR_RECURSE 0x200u

/* Offsets into the guest's SceLwMutexWorkarea. */
enum { LW_COUNT = 0, LW_THREAD = 4, LW_ATTR = 8, LW_WAITING = 12, LW_UID = 16 };

/* create.expected creates 1024 in a row *after* the ones its earlier sections
 * made, and expects every one to succeed -- so this is a measured floor with
 * headroom rather than a guess at what is reasonable. */
#define MAX_LWMUTEXES 2048

/* What the kernel keeps, which is only what the workarea cannot hold: the name,
 * what it was created with, and the queue of threads parked on it. */
typedef struct {
    uint32_t  uid;
    char      name[32];
    uint32_t  workarea;
    int32_t   init_count;
    int       used;
    psp_waitq q;
    char      waitdesc[64];
} psp_lwmutex;

static psp_lwmutex g_lw[MAX_LWMUTEXES];

static void lw_reset(void) { memset(g_lw, 0, sizeof g_lw); }

static psp_lwmutex *find_lw(uint32_t uid) {
    for (int i = 0; i < MAX_LWMUTEXES; i++)
        if (g_lw[i].used && g_lw[i].uid == uid) return &g_lw[i];
    return NULL;
}

/* Resolving a workarea has three answers, not two, and the tests separate them.
 *
 *   uid == 0            no kernel object at all, and that is legal -- the
 *                       hand-forged workarea locks and unlocks perfectly well
 *                       on its own fields. *out is NULL and this returns 0.
 *   uid names a record  the ordinary case, provided the record was made for
 *                       *this* address: a memcpy of a live workarea answers
 *                       NOT_FOUND, so the address is part of the identity.
 *   anything else       deleted, or never valid. NOT_FOUND.
 *
 * Collapsing the first and third -- both "no record" -- is what made a lock on
 * a deleted workarea report RECURSIVE instead. */
static uint32_t lw_resolve(uint32_t wa, psp_lwmutex **out, int by_address) {
    *out = NULL;
    const uint32_t uid = psp_read32(wa + LW_UID);
    if (uid == 0) return SCE_KERNEL_ERROR_OK;
    psp_lwmutex *m = find_lw(uid);
    if (!m) return SCE_KERNEL_ERROR_NOT_FOUND_LWMUTEX;
    /* A *copy* of a live workarea carries a uid that still resolves, and the
     * two callers want opposite answers about it. Locking one succeeds -- the
     * arithmetic is on the copy's own fields and never reaches the kernel --
     * while deleting or asking about one answers NOT_FOUND, because those are
     * questions about the registered object and the address is part of its
     * identity. `Lock copy #2: OK` against `Copy: Failed (800201CA)`. */
    if (by_address && m->workarea != wa) return SCE_KERNEL_ERROR_NOT_FOUND_LWMUTEX;
    *out = m;
    return SCE_KERNEL_ERROR_OK;
}

/* A workarea pointer that does not name 32 readable bytes. Distinct from a
 * *deleted* one, and reported as ILLEGAL_SIZE -- delete.expected's `Invalid`
 * case, which passes a garbage pointer rather than a stale one. */
static int lw_bad_pointer(uint32_t wa) {
    return wa == 0 || psp_mem_ptr(wa, 32) == NULL;
}

static void hle_CreateLwMutex(void) {
    /* (workarea, name, attr, count, options) -- workarea first, unlike every
     * other create in the kernel. */
    const uint32_t wa   = psp_arg(0);
    const uint32_t name = psp_arg(1);
    const uint32_t attr = psp_arg(2);
    const int32_t  init = (int32_t)psp_arg(3);

    if (!name) { psp_ret(SCE_KERNEL_ERROR_ERROR); return; }
    if (attr & ~LWMUTEX_ATTR_KNOWN) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_ATTR); return; }
    if (init < 0 || (init > 1 && !(attr & LWMUTEX_ATTR_RECURSE))) {
        psp_ret(SCE_KERNEL_ERROR_ILLEGAL_COUNT);
        return;
    }

    psp_lwmutex *m = NULL;
    for (int i = 0; i < MAX_LWMUTEXES; i++) if (!g_lw[i].used) { m = &g_lw[i]; break; }
    if (!m) { psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }

    memset(m, 0, sizeof *m);
    psp_str(name, m->name, sizeof m->name);
    m->workarea   = wa;
    m->init_count = init;
    m->uid        = psp_threadman_next_uid();
    m->used       = 1;
    char nm[sizeof m->name];
    memcpy(nm, m->name, sizeof nm);
    snprintf(m->waitdesc, sizeof m->waitdesc, "sceKernelLockLwMutex(%s)", nm);

    /* The workarea is the object. Everything the guest can see about the lock
     * is written here, including the three pad words -- the tests print them,
     * so leaving them as they were found is observable. */
    psp_write32(wa + LW_COUNT,   (uint32_t)init);
    psp_write32(wa + LW_THREAD,  init > 0 ? psp_sched_current() : 0);
    psp_write32(wa + LW_ATTR,    attr);
    psp_write32(wa + LW_WAITING, 0);
    psp_write32(wa + LW_UID,     m->uid);
    psp_write32(wa + 20, 0);
    psp_write32(wa + 24, 0);
    psp_write32(wa + 28, 0);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_DeleteLwMutex(void) {
    const uint32_t wa = psp_arg(0);
    if (lw_bad_pointer(wa)) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_SIZE); return; }
    psp_lwmutex *m = NULL;
    if (lw_resolve(wa, &m, 1) != SCE_KERNEL_ERROR_OK || !m) {
        psp_ret(SCE_KERNEL_ERROR_NOT_FOUND_LWMUTEX);
        return;
    }
    const int urgent = psp_waitq_release_all(&m->q);
    m->used = 0;
    /* The uid is left in the workarea on purpose. It is what makes a *deleted*
     * lwmutex distinguishable from a never-registered one, and the two get
     * different answers to everything afterwards. */
    psp_ret(SCE_KERNEL_ERROR_OK);
    if (urgent) psp_sched_yield();
}

/* The count rules, applied to the workarea's own attr and count so that a
 * workarea the kernel never made is judged by exactly the same arithmetic. */
static uint32_t lw_count_error(uint32_t wa, int32_t count, int locking) {
    const uint32_t attr = psp_read32(wa + LW_ATTR);
    const int32_t  cur  = (int32_t)psp_read32(wa + LW_COUNT);
    if (count <= 0) return SCE_KERNEL_ERROR_ILLEGAL_COUNT;
    if (count > 1 && !(attr & LWMUTEX_ATTR_RECURSE))
        return SCE_KERNEL_ERROR_ILLEGAL_COUNT;
    if (locking && cur > INT32_MAX - count)
        return SCE_KERNEL_ERROR_LWMUTEX_LOCK_OVERFLOW;
    return SCE_KERNEL_ERROR_OK;
}

/* The pre-6.00 sceKernelTryLockLwMutex reports one code for every failure.
 *
 * Not a simplification on our part: try.expected has fifteen failing cases and
 * every one of them answers 0x800201C4, where try600.expected gives the precise
 * code for the same call on the same inputs. The `_600` suffix is a firmware
 * revision that made the error vocabulary specific, and both exports are still
 * present with their own NIDs. */
static void lw_lock(int may_block, int has_timeout, int flatten) {
    if (may_block && !psp_sched_can_wait()) {
        psp_ret(SCE_KERNEL_ERROR_CAN_NOT_WAIT); return;
    }
    const uint32_t wa      = psp_arg(0);
    const int32_t  count   = (int32_t)psp_arg(1);
    const uint32_t tmo_ptr = has_timeout ? psp_arg(2) : 0;

#define LW_FAIL(code) do { \
        psp_ret(flatten ? SCE_KERNEL_ERROR_LWMUTEX_TRY_FAILED : (code)); \
        return; \
    } while (0)

    if (lw_bad_pointer(wa)) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_SIZE); return; }
    const uint32_t bad = lw_count_error(wa, count, 1);
    if (bad) LW_FAIL(bad);

    psp_lwmutex *rec = NULL;
    const uint32_t gone = lw_resolve(wa, &rec, 0);
    if (gone) LW_FAIL(gone);

    const uint64_t deadline = psp_wait_deadline(tmo_ptr);
    const uint32_t me   = psp_sched_current();
    const int32_t  cur  = (int32_t)psp_read32(wa + LW_COUNT);
    const uint32_t attr = psp_read32(wa + LW_ATTR);

    if (cur == 0) {
        psp_write32(wa + LW_COUNT,  (uint32_t)count);
        psp_write32(wa + LW_THREAD, me);
        psp_wait_writeback(tmo_ptr, deadline);
        psp_ret(SCE_KERNEL_ERROR_OK);
        return;
    }

    if (psp_read32(wa + LW_THREAD) == me) {
        if (!(attr & LWMUTEX_ATTR_RECURSE)) LW_FAIL(SCE_KERNEL_ERROR_LWMUTEX_RECURSIVE);
        psp_write32(wa + LW_COUNT, (uint32_t)(cur + count));
        psp_wait_writeback(tmo_ptr, deadline);
        psp_ret(SCE_KERNEL_ERROR_OK);
        return;
    }

    if (!may_block) LW_FAIL(SCE_KERNEL_ERROR_LWMUTEX_LOCKED);

    /* Only here does the kernel need to exist: a thread cannot be parked
     * against a workarea alone. */
    psp_lwmutex *m = rec;
    if (!m) { psp_ret(SCE_KERNEL_ERROR_NOT_FOUND_LWMUTEX); return; }
    const uint32_t uid = m->uid;

    if (psp_waitq_add(&m->q, me, (uint32_t)count, 0, 0) != 0) {
        psp_ret(SCE_KERNEL_ERROR_NO_MEMORY);
        return;
    }
    psp_write32(wa + LW_WAITING, (uint32_t)psp_waitq_count(&m->q));

    const int rc = psp_sched_block_until(me, PSP_SCHED_BLOCKED, m->waitdesc,
                                         deadline);

    m = find_lw(uid);
    if (!m) { psp_ret(SCE_KERNEL_ERROR_WAIT_DELETE); return; }

    if (rc == PSP_SCHED_WOKEN) {
        psp_wait_writeback(tmo_ptr, deadline);
        psp_ret(SCE_KERNEL_ERROR_OK);
        return;
    }
    psp_waitq_drop(&m->q, me);
    psp_write32(wa + LW_WAITING, (uint32_t)psp_waitq_count(&m->q));
    psp_wait_writeback(tmo_ptr, deadline);
    psp_ret(SCE_KERNEL_ERROR_WAIT_TIMEOUT);
#undef LW_FAIL
}

static void hle_LockLwMutex(void)        { lw_lock(1, 1, 0); }
static void hle_TryLockLwMutex(void)     { lw_lock(0, 0, 1); }
static void hle_TryLockLwMutex600(void)  { lw_lock(0, 0, 0); }

static void hle_UnlockLwMutex(void) {
    const uint32_t wa    = psp_arg(0);
    const int32_t  count = (int32_t)psp_arg(1);

    if (lw_bad_pointer(wa)) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_SIZE); return; }
    const uint32_t bad = lw_count_error(wa, count, 0);
    if (bad) { psp_ret(bad); return; }

    psp_lwmutex *rec = NULL;
    const uint32_t gone = lw_resolve(wa, &rec, 0);
    if (gone) { psp_ret(gone); return; }

    /* Not held, *or held by somebody else*: both are UNLOCKED. The second half
     * is the one that is not obvious -- `Locked 0 => 1` has one thread lock it
     * and another give it back, and hardware refuses rather than transferring
     * ownership. A mutex you do not own is, to you, unlocked. */
    const int32_t cur = (int32_t)psp_read32(wa + LW_COUNT);
    if (cur == 0 || psp_read32(wa + LW_THREAD) != psp_sched_current()) {
        psp_ret(SCE_KERNEL_ERROR_LWMUTEX_UNLOCKED);
        return;
    }
    if (count > cur) { psp_ret(SCE_KERNEL_ERROR_LWMUTEX_UNLOCK_UNDERFLOW); return; }

    psp_write32(wa + LW_COUNT, (uint32_t)(cur - count));
    if (cur - count > 0) { psp_ret(SCE_KERNEL_ERROR_OK); return; }
    psp_write32(wa + LW_THREAD, 0);

    /* Free now, so whoever is next in line takes it -- if there is a kernel
     * record to hold a queue at all. */
    int urgent = 0;
    psp_lwmutex *m = rec;
    if (m) {
        const int i = psp_waitq_pick(&m->q, psp_read32(wa + LW_ATTR));
        if (i >= 0) {
            const psp_waiter w = psp_waitq_take(&m->q, i);
            psp_write32(wa + LW_COUNT,   w.need);
            psp_write32(wa + LW_THREAD,  w.uid);
            psp_write32(wa + LW_WAITING, (uint32_t)psp_waitq_count(&m->q));
            urgent = psp_sched_wake(w.uid);
        }
    }
    psp_ret(SCE_KERNEL_ERROR_OK);
    if (urgent) psp_sched_yield();
}

/* 64 bytes, from create.expected's own `size=64`. `lockThread` is -1 when the
 * mutex is free, where the workarea says 0 for the same state. */
static void lw_write_info(const psp_lwmutex *m, uint32_t info) {
    const uint32_t wa  = m->workarea;
    const uint32_t thr = psp_read32(wa + LW_THREAD);
    psp_write32(info +  0, 64);
    psp_threadman_write_name(info + 4, m->name);
    psp_write32(info + 36, psp_read32(wa + LW_ATTR));
    psp_write32(info + 40, m->uid);
    psp_write32(info + 44, wa);
    psp_write32(info + 48, (uint32_t)m->init_count);
    psp_write32(info + 52, psp_read32(wa + LW_COUNT));
    psp_write32(info + 56, thr ? thr : 0xFFFFFFFFu);
    psp_write32(info + 60, (uint32_t)psp_waitq_count(&m->q));
}

static void lw_refer(const psp_lwmutex *m, uint32_t info) {
    if (!m)    { psp_ret(SCE_KERNEL_ERROR_NOT_FOUND_LWMUTEX); return; }
    if (!info) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
    if (psp_read32(info) == 0) { psp_ret(SCE_KERNEL_ERROR_OK); return; }
    lw_write_info(m, info);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_ReferLwMutexStatus(void) {
    const uint32_t wa = psp_arg(0);
    if (!wa) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_SIZE); return; }
    psp_lwmutex *m = NULL;
    (void)lw_resolve(wa, &m, 1);
    lw_refer(m, psp_arg(1));
}

/* By uid rather than by workarea, and the tests memcmp the two results against
 * each other -- so they have to agree field for field, which is why both go
 * through one writer. */
static void hle_ReferLwMutexStatusByID(void) {
    lw_refer(find_lw(psp_arg(0)), psp_arg(1));
}

static void hle_LockLwMutexCB(void) { psp_threadman_run_callbacks(); hle_LockLwMutex(); }

void psp_kernlock_register_lw(void) {
    psp_hle_register(0x19CFF145, "ThreadManForUser", "sceKernelCreateLwMutex",   hle_CreateLwMutex);
    psp_hle_register(0x60107536, "ThreadManForUser", "sceKernelDeleteLwMutex",   hle_DeleteLwMutex);
    psp_hle_register(0xBEA46419, "ThreadManForUser", "sceKernelLockLwMutex",     hle_LockLwMutex);
    psp_hle_register(0x1FC64E09, "ThreadManForUser", "sceKernelLockLwMutexCB",   hle_LockLwMutexCB);
    psp_hle_register(0xDC692EE3, "ThreadManForUser", "sceKernelTryLockLwMutex",  hle_TryLockLwMutex);
    /* The _600 suffix is part of the exported name, not a version we choose;
     * it hashes to its own NID and the tests import both. */
    psp_hle_register(0x37431849, "ThreadManForUser", "sceKernelTryLockLwMutex_600", hle_TryLockLwMutex600);
    psp_hle_register(0x15B6446B, "ThreadManForUser", "sceKernelUnlockLwMutex",   hle_UnlockLwMutex);
    psp_hle_register(0xC1734599, "ThreadManForUser", "sceKernelReferLwMutexStatus", hle_ReferLwMutexStatus);
    psp_hle_register(0x4C145944, "ThreadManForUser", "sceKernelReferLwMutexStatusByID", hle_ReferLwMutexStatusByID);
}
