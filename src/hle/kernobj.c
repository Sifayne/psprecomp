/* psprecomp — the kernel's memory and message objects.
 *
 * Variable-size pools (vpl) here; fixed pools, mailboxes and message pipes to
 * follow. What they have in common is that their *internals* are observable:
 * pspautotests reports a pool's free size after every operation, so the header
 * size, the alignment and the per-pool overhead are all pinned by the captures
 * rather than being ours to choose. An allocator that merely worked would
 * disagree with hardware on every line.
 */

#include "psprecomp/hle.h"
#include "psprecomp/clock.h"
#include "psprecomp/mem.h"
#include "psprecomp/sched.h"
#include "waitq.h"

#include <stdio.h>
#include <string.h>

/* ---- vpl: the variable-size pool ------------------------------------------
 *
 * Three numbers decide every `freeSize` the tests print, and all three are
 * measured from threads/vpl/{create,allocate}.expected:
 *
 *   - **32 bytes of pool overhead.** A pool asked for 0x1000 reports
 *     `poolSize=00000FE0`, and one asked for 0x10000 reports `0000FFE0`.
 *   - **The pool size is rounded *up* to 8 first.** 0x31, 0x32, 0x36 and 0x38
 *     all give 0x18, while 0x39 and 0x3A give 0x20 -- so the rule is
 *     `round_up(size, 8) - 32`, which reproduces all eleven values the create
 *     test prints above its noise floor (see the note at vpl_pool_size).
 *   - **An 8-byte header per allocation, and 8-byte alignment.** From a 0x10000
 *     pool: 1 byte takes 16, 16 bytes take 24, 8 bytes take 16. That is
 *     `round_up(n, 8) + 8` and nothing else fits all three.
 */

#define MAX_VPLS 128
#define VPL_ALIGN     8u
#define VPL_HEADER    8u
#define VPL_OVERHEAD 32u

/* Fifth object type, fifth attribute rule: 0x43FF. create.expected accepts
 * 0x1, 0x100, 0x200 and 0x4000 and refuses 0x400, 0x800, 0x1000, 0x2000,
 * 0x8000 and 0x10000 -- so bit 14 is legal with a gap of four illegal bits
 * below it. */
#define VPL_ATTR_KNOWN 0x43FFu

#define MAX_VPL_BLOCKS 64

typedef struct { uint32_t addr, size; } vpl_block;   /* addr is the header */

typedef struct {
    uint32_t  uid;
    char      name[32];
    uint32_t  attr;
    uint32_t  base;          /* host allocation backing the pool */
    uint32_t  pool_size;
    uint32_t  free_size;
    int       used;
    vpl_block block[MAX_VPL_BLOCKS];   /* live allocations, address order */
    int       nblocks;
    psp_waitq q;
    char      waitdesc[64];
} psp_vpl;

static psp_vpl g_vpl[MAX_VPLS];

static psp_vpl *find_vpl(uint32_t id) {
    for (int i = 0; i < MAX_VPLS; i++)
        if (g_vpl[i].used && g_vpl[i].uid == id) return &g_vpl[i];
    return NULL;
}

static uint32_t round_up(uint32_t n, uint32_t to) { return (n + to - 1) & ~(to - 1); }

/* What a pool of `size` bytes reports as its poolSize.
 *
 * Exact for every value the create test prints from 0x31 upward. Below that the
 * capture cannot be read: `schedfVpl` refers into an **uninitialised stack
 * struct** and prints whatever the previous iteration left there, so sizes
 * 1..0x30 all show the 0x0FE0 of the 0x1000 pool created before them rather
 * than anything about themselves. Recorded rather than modelled around. */
static uint32_t vpl_pool_size(uint32_t size) {
    const uint32_t up = round_up(size, VPL_ALIGN);
    return up > VPL_OVERHEAD ? up - VPL_OVERHEAD : 0;
}

/* Which memory partitions a vpl may be created in.
 *
 * Neither a range nor a bitmask: 2 and 6 are permitted, 1, 3, 4, 8 and 9 are
 * refused as not-permitted, and -5, -1, 0, 7 and 10 are refused as
 * out-of-range -- so 7 is out of range while 8 and 9 are merely forbidden.
 * Transcribed. Partition 5 is not swept by the test (its comment says it
 * crashes hardware) and is grouped with the forbidden ones. */
static uint32_t vpl_partition_error(int32_t part) {
    switch (part) {
        case 2: case 6:                      return SCE_KERNEL_ERROR_OK;
        case 1: case 3: case 4: case 5:
        case 8: case 9:                      return SCE_KERNEL_ERROR_ILLEGAL_PERM;
        default:                             return SCE_KERNEL_ERROR_ILLEGAL_PARTITION;
    }
}

static void hle_CreateVpl(void) {
    /* (name, partition, attr, size, option) */
    const uint32_t name = psp_arg(0);
    const int32_t  part = (int32_t)psp_arg(1);
    const uint32_t attr = psp_arg(2);
    const uint32_t size = psp_arg(3);

    if (!name) { psp_ret(SCE_KERNEL_ERROR_ERROR); return; }
    const uint32_t pe = vpl_partition_error(part);
    if (pe) { psp_ret(pe); return; }
    if (attr & ~VPL_ATTR_KNOWN) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_ATTR); return; }
    if (size == 0) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_MEMSIZE); return; }

    const uint32_t pool = vpl_pool_size(size);
    /* A request the heap cannot meet is NO_MEMORY rather than a bad size --
     * 0x10000000 and 0x02000000 are refused where 0x01800000 succeeds, so the
     * boundary is what is actually free and not a constant. */
    const uint32_t base = pool ? psp_sysmem_alloc(pool, 0) : 0;
    if (pool && !base) { psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }

    psp_vpl *v = NULL;
    for (int i = 0; i < MAX_VPLS; i++) if (!g_vpl[i].used) { v = &g_vpl[i]; break; }
    if (!v) { if (base) psp_sysmem_release(base);
              psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }

    memset(v, 0, sizeof *v);
    psp_str(name, v->name, sizeof v->name);
    v->attr      = attr;
    v->base      = base;
    v->pool_size = pool;
    v->free_size = pool;
    v->uid       = psp_threadman_next_uid();
    v->used      = 1;
    char nm[sizeof v->name];
    memcpy(nm, v->name, sizeof nm);
    snprintf(v->waitdesc, sizeof v->waitdesc, "sceKernelAllocateVpl(%s)", nm);
    psp_ret(v->uid);
}

static void hle_DeleteVpl(void) {
    psp_vpl *v = find_vpl(psp_arg(0));
    if (!v) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_VPLID); return; }
    const int urgent = psp_waitq_release_all(&v->q);
    if (v->base) psp_sysmem_release(v->base);
    v->used = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
    if (urgent) psp_sched_yield();
}

/* First fit over the gaps between live blocks, which are kept in address
 * order. The *placement* is not observable -- the tests print free sizes and
 * check that a pointer round-trips -- but the accounting is, exactly. */
static uint32_t vpl_alloc(psp_vpl *v, uint32_t bytes) {
    const uint32_t need = round_up(bytes, VPL_ALIGN) + VPL_HEADER;
    if (need > v->free_size || v->nblocks >= MAX_VPL_BLOCKS) return 0;

    uint32_t at = v->base;
    int i = 0;
    for (; i < v->nblocks; i++) {
        if (v->block[i].addr - at >= need) break;
        at = v->block[i].addr + v->block[i].size;
    }
    if (at + need > v->base + v->pool_size) return 0;

    memmove(&v->block[i + 1], &v->block[i],
            (size_t)(v->nblocks - i) * sizeof v->block[0]);
    v->block[i].addr = at;
    v->block[i].size = need;
    v->nblocks++;
    v->free_size -= need;
    return at + VPL_HEADER;
}

/* Hand the pool to whoever is next in line and can now be satisfied. */
static int vpl_release(psp_vpl *v) {
    int urgent = 0;
    for (;;) {
        const int i = psp_waitq_pick(&v->q, v->attr);
        if (i < 0) break;
        const uint32_t got = vpl_alloc(v, v->q.w[i].need);
        if (!got) break;
        const psp_waiter w = psp_waitq_take(&v->q, i);
        if (w.out) psp_write32(w.out, got);
        urgent |= psp_sched_wake(w.uid);
    }
    return urgent;
}

static void vpl_allocate(int may_block, int has_timeout) {
    const uint32_t id      = psp_arg(0);
    const uint32_t bytes   = psp_arg(1);
    const uint32_t out     = psp_arg(2);
    const uint32_t tmo_ptr = has_timeout ? psp_arg(3) : 0;

    psp_vpl *v = find_vpl(id);
    if (!v) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_VPLID); return; }
    /* Zero, negative, or more than the pool could ever hold: all one code, and
     * all decided before anything waits -- a request the pool cannot satisfy
     * even when empty is not something waiting would fix. */
    if (bytes == 0 || (int32_t)bytes < 0 || bytes > v->pool_size) {
        psp_ret(SCE_KERNEL_ERROR_ILLEGAL_MEMSIZE);
        return;
    }

    const uint64_t deadline = psp_wait_deadline(tmo_ptr);
    const uint32_t me = psp_sched_current();

    /* A new request takes what it can even while others wait, unlike a
     * semaphore. threads/vpl/priority shows four allocations succeeding with
     * `wait=2` reported throughout: two threads are parked for sizes the pool
     * cannot meet, and smaller requests walk straight past them. There is no
     * head-of-line blocking here because the waiters are not queued for the
     * *same* resource -- each wants a different number of bytes. */
    const uint32_t got = vpl_alloc(v, bytes);
    if (got) {
        if (out) psp_write32(out, got);
        psp_wait_writeback(tmo_ptr, deadline);
        psp_ret(SCE_KERNEL_ERROR_OK);
        return;
    }

    if (!may_block) { psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }

    if (psp_waitq_add(&v->q, me, bytes, 0, out) != 0) {
        psp_ret(SCE_KERNEL_ERROR_NO_MEMORY);
        return;
    }
    const int rc = psp_sched_block_until(me, PSP_SCHED_BLOCKED, v->waitdesc,
                                         deadline);
    v = find_vpl(id);
    if (!v) { psp_ret(SCE_KERNEL_ERROR_WAIT_DELETE); return; }

    if (rc == PSP_SCHED_WOKEN) {
        /* vpl_release allocated for us and wrote the pointer. */
        psp_wait_writeback(tmo_ptr, deadline);
        psp_ret(SCE_KERNEL_ERROR_OK);
        return;
    }
    psp_waitq_drop(&v->q, me);
    psp_wait_writeback(tmo_ptr, deadline);
    psp_ret(SCE_KERNEL_ERROR_WAIT_TIMEOUT);
}

static void hle_AllocateVpl(void)    { vpl_allocate(1, 1); }
static void hle_TryAllocateVpl(void) { vpl_allocate(0, 0); }

static void hle_FreeVpl(void) {
    const uint32_t id  = psp_arg(0);
    const uint32_t ptr = psp_arg(1);

    psp_vpl *v = find_vpl(id);
    if (!v) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_VPLID); return; }

    /* A pointer that is not exactly the start of one of *this* pool's live
     * blocks is refused, and the tests are thorough about which is which:
     * freeing twice, a NULL, a stack address, a pointer one block into the
     * pool, and another pool's pointer are each ILLEGAL_MEMBLOCK, while a
     * pointer that is not mapped memory at all is ILLEGAL_SIZE. */
    if (ptr && !psp_mem_ptr(ptr, 4)) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_SIZE); return; }
    for (int i = 0; i < v->nblocks; i++) {
        if (v->block[i].addr + VPL_HEADER != ptr) continue;
        v->free_size += v->block[i].size;
        memmove(&v->block[i], &v->block[i + 1],
                (size_t)(v->nblocks - i - 1) * sizeof v->block[0]);
        v->nblocks--;
        const int urgent = vpl_release(v);
        psp_ret(SCE_KERNEL_ERROR_OK);
        if (urgent) psp_sched_yield();
        return;
    }
    psp_ret(SCE_KERNEL_ERROR_ILLEGAL_MEMBLOCK);
}

static void hle_CancelVpl(void) {
    psp_vpl *v = find_vpl(psp_arg(0));
    if (!v) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_VPLID); return; }
    const uint32_t out = psp_arg(1);
    if (out) psp_write32(out, (uint32_t)psp_waitq_count(&v->q));
    const int urgent = psp_waitq_release_all(&v->q);
    psp_ret(SCE_KERNEL_ERROR_OK);
    if (urgent) psp_sched_yield();
}

static void hle_ReferVplStatus(void) {
    const psp_vpl *v = find_vpl(psp_arg(0));
    const uint32_t info = psp_arg(1);
    if (!v)    { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_VPLID); return; }
    if (!info) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
    if (psp_read32(info) == 0) { psp_ret(SCE_KERNEL_ERROR_OK); return; }
    psp_write32(info +  0, 52);
    psp_threadman_write_name(info + 4, v->name);
    psp_write32(info + 36, v->attr);
    psp_write32(info + 40, v->pool_size);
    psp_write32(info + 44, v->free_size);
    psp_write32(info + 48, (uint32_t)psp_waitq_count(&v->q));
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void mpp_reset(void);
static void mbx_reset(void);

void psp_kernobj_reset(void) {
    for (int i = 0; i < MAX_VPLS; i++)
        if (g_vpl[i].used && g_vpl[i].base) psp_sysmem_release(g_vpl[i].base);
    memset(g_vpl, 0, sizeof g_vpl);
    mpp_reset();
    mbx_reset();
}

void psp_kernobj_register(void) {
    psp_hle_register(0x56C039B5, "ThreadManForUser", "sceKernelCreateVpl",      hle_CreateVpl);
    psp_hle_register(0x89B3D48C, "ThreadManForUser", "sceKernelDeleteVpl",      hle_DeleteVpl);
    psp_hle_register(0xBED27435, "ThreadManForUser", "sceKernelAllocateVpl",    hle_AllocateVpl);
    psp_hle_register(0xEC0A693F, "ThreadManForUser", "sceKernelAllocateVplCB",  hle_AllocateVpl);
    psp_hle_register(0xAF36D708, "ThreadManForUser", "sceKernelTryAllocateVpl", hle_TryAllocateVpl);
    psp_hle_register(0xB736E9FF, "ThreadManForUser", "sceKernelFreeVpl",        hle_FreeVpl);
    psp_hle_register(0x1D371B8A, "ThreadManForUser", "sceKernelCancelVpl",      hle_CancelVpl);
    psp_hle_register(0x39810265, "ThreadManForUser", "sceKernelReferVplStatus", hle_ReferVplStatus);
    psp_kernobj_register_mpp();
    psp_kernobj_register_mbx();
}

/* ---- msgpipe: a byte ring, not a message queue -----------------------------
 *
 * The name says messages and the accounting says bytes: a 0x1000 pipe sent 256
 * bytes reports `free=f00`, and one sent a single unaligned byte reports
 * `free=fff`. There is no per-message header and no framing at all -- a
 * receiver takes whatever is there, in order, in whatever quantity it asked
 * for. Both halves of that are printed by threads/msgpipe/{send,receive}.
 *
 * **Only two modes exist.** send.expected sweeps -2, -1, 2..9, 257 and 4097 and
 * refuses every one with ILLEGAL_MODE; 0 waits for the whole transfer and 1
 * takes what it can. Fifty-two of the file's lines are that sweep.
 */

#define MAX_PIPES 64
#define MPP_MODE_ASAP 1u

typedef struct {
    uint32_t  uid;
    char      name[32];
    uint32_t  attr;
    uint32_t  base;        /* the ring, in guest memory */
    uint32_t  buf_size;
    uint32_t  head, used;  /* head is the read cursor */
    int       alive;
    psp_waitq send_q, recv_q;
    char      senddesc[64], recvdesc[64];
} psp_msgpipe;

static psp_msgpipe g_pipe[MAX_PIPES];

static void mpp_reset(void) {
    for (int i = 0; i < MAX_PIPES; i++)
        if (g_pipe[i].alive && g_pipe[i].base) psp_sysmem_release(g_pipe[i].base);
    memset(g_pipe, 0, sizeof g_pipe);
}

static psp_msgpipe *find_pipe(uint32_t id) {
    for (int i = 0; i < MAX_PIPES; i++)
        if (g_pipe[i].alive && g_pipe[i].uid == id) return &g_pipe[i];
    return NULL;
}

static uint32_t mpp_free(const psp_msgpipe *p) { return p->buf_size - p->used; }

/* The ring, byte at a time. Small transfers, and the wrap is the only thing
 * worth being careful about. */
/* Both guard on the buffer size before taking a remainder. A pipe created with
 * no buffer is legal -- create.expected has several -- and a zero-length
 * transfer on one is legal too, so the cursor update runs with a modulus of
 * zero and takes the process down with SIGFPE. */
static void mpp_put(psp_msgpipe *p, uint32_t src, uint32_t n) {
    if (!p->buf_size || !n) return;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t at = (p->head + p->used + i) % p->buf_size;
        psp_write8(p->base + at, src ? psp_read8(src + i) : 0);
    }
    p->used += n;
}

static void mpp_take(psp_msgpipe *p, uint32_t dst, uint32_t n) {
    if (!p->buf_size || !n) return;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t b = psp_read8(p->base + (p->head + i) % p->buf_size);
        if (dst) psp_write8(dst + i, b);
    }
    p->head = (p->head + n) % p->buf_size;
    p->used -= n;
}

static void hle_CreateMsgPipe(void) {
    /* (name, partition, attr, size, option) */
    const uint32_t name = psp_arg(0);
    const int32_t  part = (int32_t)psp_arg(1);
    const uint32_t attr = psp_arg(2);
    const uint32_t size = psp_arg(3);

    /* NO_MEMORY for a null name, where a semaphore answers ERROR and an
     * lwmutex answers ERROR too. threadman.c predicted this one before the
     * type existed: fpl and msgpipe are the two that differ. */
    if (!name) { psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }
    const uint32_t pe = vpl_partition_error(part);
    if (pe) { psp_ret(pe); return; }

    const uint32_t base = size ? psp_sysmem_alloc(size, 0) : 0;
    if (size && !base) { psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }

    psp_msgpipe *p = NULL;
    for (int i = 0; i < MAX_PIPES; i++) if (!g_pipe[i].alive) { p = &g_pipe[i]; break; }
    if (!p) { if (base) psp_sysmem_release(base);
              psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }

    memset(p, 0, sizeof *p);
    psp_str(name, p->name, sizeof p->name);
    p->attr     = attr;
    p->base     = base;
    p->buf_size = size;
    p->uid      = psp_threadman_next_uid();
    p->alive    = 1;
    char nm[sizeof p->name];
    memcpy(nm, p->name, sizeof nm);
    snprintf(p->senddesc, sizeof p->senddesc, "sceKernelSendMsgPipe(%s)", nm);
    memcpy(nm, p->name, sizeof nm);
    snprintf(p->recvdesc, sizeof p->recvdesc, "sceKernelReceiveMsgPipe(%s)", nm);
    psp_ret(p->uid);
}

static void hle_DeleteMsgPipe(void) {
    psp_msgpipe *p = find_pipe(psp_arg(0));
    if (!p) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_MPPID); return; }
    int urgent = psp_waitq_release_all(&p->send_q);
    urgent |= psp_waitq_release_all(&p->recv_q);
    if (p->base) psp_sysmem_release(p->base);
    p->alive = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
    if (urgent) psp_sched_yield();
}

/* Whoever is waiting on the other side may now be able to move. Each waiter
 * carries the buffer it was given and how much it wanted; `mode` says whether
 * a partial transfer will do. */
static int mpp_wake_receivers(psp_msgpipe *p) {
    int urgent = 0;
    for (;;) {
        const int i = psp_waitq_pick(&p->recv_q, p->attr);
        if (i < 0) break;
        const psp_waiter w = p->recv_q.w[i];
        const uint32_t want = (w.mode & MPP_MODE_ASAP)
                            ? (p->used < w.need ? p->used : w.need) : w.need;
        if (!want || want > p->used) break;
        psp_waitq_take(&p->recv_q, i);
        mpp_take(p, w.out, want);
        urgent |= psp_sched_wake(w.uid);
    }
    return urgent;
}

static int mpp_wake_senders(psp_msgpipe *p) {
    int urgent = 0;
    for (;;) {
        const int i = psp_waitq_pick(&p->send_q, p->attr);
        if (i < 0) break;
        const psp_waiter w = p->send_q.w[i];
        const uint32_t room = mpp_free(p);
        const uint32_t give = (w.mode & MPP_MODE_ASAP)
                            ? (room < w.need ? room : w.need) : w.need;
        if (!give || give > room) break;
        psp_waitq_take(&p->send_q, i);
        mpp_put(p, w.out, give);
        urgent |= psp_sched_wake(w.uid);
    }
    return urgent;
}

/* send and receive are the same shape with the direction reversed, so they
 * share a body: `sending` picks which queue is ours and which is theirs, which
 * error a full-or-empty poll gives, and which way the bytes go. */
static void mpp_transfer(int sending, int may_block, int has_timeout) {
    const uint32_t id      = psp_arg(0);
    const uint32_t buf     = psp_arg(1);
    const uint32_t len     = psp_arg(2);
    const uint32_t mode    = psp_arg(3);
    const uint32_t out     = psp_arg(4);
    const uint32_t tmo_ptr = has_timeout ? psp_arg(5) : 0;

    psp_msgpipe *p = find_pipe(id);
    if (!p) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_MPPID); return; }
    if (mode & ~MPP_MODE_ASAP) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_MODE); return; }
    /* A length with the sign bit set is a different mistake from one merely
     * bigger than the pipe, and gets a different code. */
    if ((int32_t)len < 0) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_SIZE); return; }
    /* Bigger than the pipe -- but only when the pipe has a size at all. On a
     * pipe created with no buffer *every* request is answered FULL rather than
     * too-big, including ones that are obviously too big: with nowhere to put
     * anything, "there is no room" is the more specific truth and it is what
     * hardware says. */
    if (p->buf_size && len > p->buf_size) {
        psp_ret(SCE_KERNEL_ERROR_ILLEGAL_SIZE_MPP);
        return;
    }

    const uint64_t deadline = psp_wait_deadline(tmo_ptr);
    const uint32_t avail = sending ? mpp_free(p) : p->used;
    uint32_t now = (mode & MPP_MODE_ASAP) ? (avail < len ? avail : len) : len;

    /* Nothing asked for is nothing to wait for: a zero-length transfer
     * succeeds even on a pipe with no buffer at all. */
    if (len == 0 || (now && now <= avail)) {
        if (sending) { mpp_put(p, buf, now); }
        else         { mpp_take(p, buf, now); }
        if (out) psp_write32(out, now);
        const int urgent = sending ? mpp_wake_receivers(p) : mpp_wake_senders(p);
        psp_wait_writeback(tmo_ptr, deadline);
        psp_ret(SCE_KERNEL_ERROR_OK);
        if (urgent) psp_sched_yield();
        return;
    }

    if (!may_block) {
        /* ASAP moved nothing, and says so: `ASAP: Failed (800201b3, bytes=0)`.
         * A full-wait failure leaves the word alone, which is how the tests
         * tell the two apart -- they pre-seed it with 0x1337. */
        if ((mode & MPP_MODE_ASAP) && out) psp_write32(out, 0);
        psp_ret(sending ? SCE_KERNEL_ERROR_MSGPIPE_FULL
                        : SCE_KERNEL_ERROR_MSGPIPE_EMPTY);
        return;
    }

    const uint32_t me = psp_sched_current();
    psp_waitq *q = sending ? &p->send_q : &p->recv_q;
    if (psp_waitq_add(q, me, len, mode, buf) != 0) {
        psp_ret(SCE_KERNEL_ERROR_NO_MEMORY);
        return;
    }
    const int rc = psp_sched_block_until(me, PSP_SCHED_BLOCKED,
                                         sending ? p->senddesc : p->recvdesc,
                                         deadline);
    p = find_pipe(id);
    if (!p) { psp_ret(SCE_KERNEL_ERROR_WAIT_DELETE); return; }

    if (rc == PSP_SCHED_WOKEN) {
        /* The other side moved the bytes on our behalf. */
        if (out) psp_write32(out, len);
        psp_wait_writeback(tmo_ptr, deadline);
        psp_ret(SCE_KERNEL_ERROR_OK);
        return;
    }
    psp_waitq_drop(sending ? &p->send_q : &p->recv_q, me);
    /* A wait that ran out moved nothing, and reports that: `bytes=0`, where an
     * argument failure leaves the caller's 0x1337 in place. */
    if (out) psp_write32(out, 0);
    psp_wait_writeback(tmo_ptr, deadline);
    psp_ret(SCE_KERNEL_ERROR_WAIT_TIMEOUT);
}

static void hle_SendMsgPipe(void)       { mpp_transfer(1, 1, 1); }
static void hle_TrySendMsgPipe(void)    { mpp_transfer(1, 0, 0); }
static void hle_ReceiveMsgPipe(void)    { mpp_transfer(0, 1, 1); }
static void hle_TryReceiveMsgPipe(void) { mpp_transfer(0, 0, 0); }

static void hle_CancelMsgPipe(void) {
    psp_msgpipe *p = find_pipe(psp_arg(0));
    if (!p) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_MPPID); return; }
    const uint32_t nsend = psp_arg(1), nrecv = psp_arg(2);
    if (nsend) psp_write32(nsend, (uint32_t)psp_waitq_count(&p->send_q));
    if (nrecv) psp_write32(nrecv, (uint32_t)psp_waitq_count(&p->recv_q));
    int urgent = psp_waitq_release_all(&p->send_q);
    urgent |= psp_waitq_release_all(&p->recv_q);
    p->head = p->used = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
    if (urgent) psp_sched_yield();
}

static void hle_ReferMsgPipeStatus(void) {
    const psp_msgpipe *p = find_pipe(psp_arg(0));
    const uint32_t info = psp_arg(1);
    if (!p)    { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_MPPID); return; }
    if (!info) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
    if (psp_read32(info) == 0) { psp_ret(SCE_KERNEL_ERROR_OK); return; }
    psp_write32(info +  0, 56);
    psp_threadman_write_name(info + 4, p->name);
    psp_write32(info + 36, p->attr);
    psp_write32(info + 40, p->buf_size);
    psp_write32(info + 44, mpp_free(p));
    psp_write32(info + 48, (uint32_t)psp_waitq_count(&p->send_q));
    psp_write32(info + 52, (uint32_t)psp_waitq_count(&p->recv_q));
    psp_ret(SCE_KERNEL_ERROR_OK);
}

void psp_kernobj_register_mpp(void) {
    psp_hle_register(0x7C0DC2A0, "ThreadManForUser", "sceKernelCreateMsgPipe",     hle_CreateMsgPipe);
    psp_hle_register(0xF0B7DA1C, "ThreadManForUser", "sceKernelDeleteMsgPipe",     hle_DeleteMsgPipe);
    psp_hle_register(0x876DBFAD, "ThreadManForUser", "sceKernelSendMsgPipe",       hle_SendMsgPipe);
    psp_hle_register(0x7C41F2C2, "ThreadManForUser", "sceKernelSendMsgPipeCB",     hle_SendMsgPipe);
    psp_hle_register(0x884C9F90, "ThreadManForUser", "sceKernelTrySendMsgPipe",    hle_TrySendMsgPipe);
    psp_hle_register(0x74829B76, "ThreadManForUser", "sceKernelReceiveMsgPipe",    hle_ReceiveMsgPipe);
    psp_hle_register(0xFBFA697D, "ThreadManForUser", "sceKernelReceiveMsgPipeCB",  hle_ReceiveMsgPipe);
    psp_hle_register(0xDF52098F, "ThreadManForUser", "sceKernelTryReceiveMsgPipe", hle_TryReceiveMsgPipe);
    psp_hle_register(0x349B864D, "ThreadManForUser", "sceKernelCancelMsgPipe",     hle_CancelMsgPipe);
    psp_hle_register(0x33BE4024, "ThreadManForUser", "sceKernelReferMsgPipeStatus",hle_ReferMsgPipeStatus);
}

/* ---- mbx: a message box whose queue lives in the guest's own messages -------
 *
 * The kernel stores no copy of anything. A message is a guest structure whose
 * first eight bytes are a `SceKernelMsgPacket` header, and the queue is a
 * **circular** singly-linked list threaded through those headers -- so the
 * topology is user-visible and the tests check it directly. They poison
 * `header.next` with 0xDEADBEEF before sending and then classify whatever the
 * kernel wrote as NULL / DEAD / ITSELF / FIRST / OTHER.
 *
 * Circular, not NULL-terminated: one message reports `next=ITSELF`, and with
 * two the first reports `next=OTHER` and the second `next=FIRST`. sub_shared.h
 * carries the observation as a comment -- "Seems they loop" -- and this
 * implements it, because a NULL-terminated list would print DEAD or NULL there
 * and every line would differ.
 */

#define MAX_MBXES 64
#define MBX_ATTR_KNOWN    0x5FFu   /* sixth object type, sixth rule */
#define MBX_ATTR_MSG_PRIO 0x400u

enum { MSG_NEXT = 0, MSG_PRIO = 4 };   /* offsets in SceKernelMsgPacket */

typedef struct {
    uint32_t  uid;
    char      name[32];
    uint32_t  attr;
    uint32_t  first;       /* guest address of the head message, 0 when empty */
    uint32_t  count;
    int       alive;
    psp_waitq q;
    char      waitdesc[64];
} psp_mbx;

static psp_mbx g_mbx[MAX_MBXES];

static void mbx_reset(void) { memset(g_mbx, 0, sizeof g_mbx); }

static psp_mbx *find_mbx(uint32_t id) {
    for (int i = 0; i < MAX_MBXES; i++)
        if (g_mbx[i].alive && g_mbx[i].uid == id) return &g_mbx[i];
    return NULL;
}

/* Walk to the message before `first` -- the tail, since the list closes on
 * itself. Bounded by the count so a corrupt `next` cannot spin forever. */
static uint32_t mbx_tail(const psp_mbx *m) {
    uint32_t at = m->first;
    for (uint32_t i = 1; i < m->count; i++) at = psp_read32(at + MSG_NEXT);
    return at;
}

static void mbx_insert(psp_mbx *m, uint32_t msg) {
    if (!m->count) {
        psp_write32(msg + MSG_NEXT, msg);     /* ITSELF */
        m->first = msg;
        m->count = 1;
        return;
    }
    if (m->attr & MBX_ATTR_MSG_PRIO) {
        /* Ordered by the packet's own priority byte, ahead of equals. */
        const uint32_t pri = psp_read8(msg + MSG_PRIO);
        uint32_t prev = mbx_tail(m), at = m->first;
        for (uint32_t i = 0; i < m->count; i++) {
            if (psp_read8(at + MSG_PRIO) > pri) break;
            prev = at;
            at = psp_read32(at + MSG_NEXT);
        }
        psp_write32(msg + MSG_NEXT, at);
        psp_write32(prev + MSG_NEXT, msg);
        if (at == m->first && prev == mbx_tail(m)) { /* new head */ }
        if (psp_read8(m->first + MSG_PRIO) > pri) m->first = msg;
        m->count++;
        return;
    }
    const uint32_t tail = mbx_tail(m);
    psp_write32(tail + MSG_NEXT, msg);
    psp_write32(msg + MSG_NEXT, m->first);
    m->count++;
}

static uint32_t mbx_pop(psp_mbx *m) {
    if (!m->count) return 0;
    const uint32_t head = m->first;
    if (m->count == 1) { m->first = 0; m->count = 0; }
    else {
        const uint32_t next = psp_read32(head + MSG_NEXT);
        psp_write32(mbx_tail(m) + MSG_NEXT, next);
        m->first = next;
        m->count--;
    }
    /* The message leaves pointing at itself, which is what a receiver sees:
     * `GOT: "hi 0" (next=ITSELF)`. */
    psp_write32(head + MSG_NEXT, head);
    return head;
}

static void hle_CreateMbx(void) {
    /* (name, attr, option) -- no partition, unlike the pools. */
    const uint32_t name = psp_arg(0);
    const uint32_t attr = psp_arg(1);
    if (!name) { psp_ret(SCE_KERNEL_ERROR_ERROR); return; }
    if (attr & ~MBX_ATTR_KNOWN) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_ATTR); return; }

    psp_mbx *m = NULL;
    for (int i = 0; i < MAX_MBXES; i++) if (!g_mbx[i].alive) { m = &g_mbx[i]; break; }
    if (!m) { psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }

    memset(m, 0, sizeof *m);
    psp_str(name, m->name, sizeof m->name);
    m->attr  = attr;
    m->uid   = psp_threadman_next_uid();
    m->alive = 1;
    char nm[sizeof m->name];
    memcpy(nm, m->name, sizeof nm);
    snprintf(m->waitdesc, sizeof m->waitdesc, "sceKernelReceiveMbx(%s)", nm);
    psp_ret(m->uid);
}

static void hle_DeleteMbx(void) {
    psp_mbx *m = find_mbx(psp_arg(0));
    if (!m) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_MBXID); return; }
    const int urgent = psp_waitq_release_all(&m->q);
    m->alive = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
    if (urgent) psp_sched_yield();
}

static void hle_SendMbx(void) {
    const uint32_t id  = psp_arg(0);
    const uint32_t msg = psp_arg(1);
    psp_mbx *m = find_mbx(id);
    if (!m) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_MBXID); return; }
    if (!msg || !psp_mem_ptr(msg, 8)) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }

    /* A waiting receiver takes it without it ever joining the queue. */
    const int i = psp_waitq_pick(&m->q, m->attr);
    if (i >= 0) {
        const psp_waiter w = psp_waitq_take(&m->q, i);
        psp_write32(msg + MSG_NEXT, msg);
        if (w.out) psp_write32(w.out, msg);
        const int urgent = psp_sched_wake(w.uid);
        psp_ret(SCE_KERNEL_ERROR_OK);
        if (urgent) psp_sched_yield();
        return;
    }
    mbx_insert(m, msg);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void mbx_receive(int may_block, int has_timeout) {
    const uint32_t id      = psp_arg(0);
    const uint32_t out     = psp_arg(1);
    const uint32_t tmo_ptr = has_timeout ? psp_arg(2) : 0;

    psp_mbx *m = find_mbx(id);
    if (!m) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_MBXID); return; }

    const uint64_t deadline = psp_wait_deadline(tmo_ptr);

    if (m->count) {
        const uint32_t got = mbx_pop(m);
        if (out) psp_write32(out, got);
        psp_wait_writeback(tmo_ptr, deadline);
        psp_ret(SCE_KERNEL_ERROR_OK);
        return;
    }
    if (!may_block) { psp_ret(SCE_KERNEL_ERROR_MBOX_NOMSG); return; }

    const uint32_t me = psp_sched_current();
    if (psp_waitq_add(&m->q, me, 0, 0, out) != 0) {
        psp_ret(SCE_KERNEL_ERROR_NO_MEMORY);
        return;
    }
    const int rc = psp_sched_block_until(me, PSP_SCHED_BLOCKED, m->waitdesc,
                                         deadline);
    m = find_mbx(id);
    if (!m) { psp_ret(SCE_KERNEL_ERROR_WAIT_DELETE); return; }

    if (rc == PSP_SCHED_WOKEN) {
        psp_wait_writeback(tmo_ptr, deadline);
        psp_ret(SCE_KERNEL_ERROR_OK);
        return;
    }
    psp_waitq_drop(&m->q, me);
    psp_wait_writeback(tmo_ptr, deadline);
    psp_ret(SCE_KERNEL_ERROR_WAIT_TIMEOUT);
}

static void hle_ReceiveMbx(void) { mbx_receive(1, 1); }
static void hle_PollMbx(void)    { mbx_receive(0, 0); }

static void hle_CancelReceiveMbx(void) {
    psp_mbx *m = find_mbx(psp_arg(0));
    if (!m) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_MBXID); return; }
    const uint32_t out = psp_arg(1);
    if (out) psp_write32(out, (uint32_t)psp_waitq_count(&m->q));
    const int urgent = psp_waitq_release_all(&m->q);
    psp_ret(SCE_KERNEL_ERROR_OK);
    if (urgent) psp_sched_yield();
}

static void hle_ReferMbxStatus(void) {
    const psp_mbx *m = find_mbx(psp_arg(0));
    const uint32_t info = psp_arg(1);
    if (!m)    { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_MBXID); return; }
    if (!info) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
    psp_write32(info +  0, 52);
    psp_threadman_write_name(info + 4, m->name);
    psp_write32(info + 36, m->attr);
    psp_write32(info + 40, (uint32_t)psp_waitq_count(&m->q));
    psp_write32(info + 44, m->count);
    psp_write32(info + 48, m->first);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

void psp_kernobj_register_mbx(void) {
    psp_hle_register(0x8125221D, "ThreadManForUser", "sceKernelCreateMbx",        hle_CreateMbx);
    psp_hle_register(0x86255ADA, "ThreadManForUser", "sceKernelDeleteMbx",        hle_DeleteMbx);
    psp_hle_register(0xE9B3061E, "ThreadManForUser", "sceKernelSendMbx",          hle_SendMbx);
    psp_hle_register(0x18260574, "ThreadManForUser", "sceKernelReceiveMbx",       hle_ReceiveMbx);
    psp_hle_register(0xF3986382, "ThreadManForUser", "sceKernelReceiveMbxCB",     hle_ReceiveMbx);
    psp_hle_register(0x0D81716A, "ThreadManForUser", "sceKernelPollMbx",          hle_PollMbx);
    psp_hle_register(0x87D4DD36, "ThreadManForUser", "sceKernelCancelReceiveMbx", hle_CancelReceiveMbx);
    psp_hle_register(0xA8E8C846, "ThreadManForUser", "sceKernelReferMbxStatus",   hle_ReferMbxStatus);
}
