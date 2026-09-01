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
#include <stdlib.h>
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

#define MAX_VPLS 2048
#define VPL_ALIGN     8u
#define VPL_HEADER    8u
#define VPL_OVERHEAD 32u

/* Fifth object type, fifth attribute rule: 0x43FF. create.expected accepts
 * 0x1, 0x100, 0x200 and 0x4000 and refuses 0x400, 0x800, 0x1000, 0x2000,
 * 0x8000 and 0x10000 -- so bit 14 is legal with a gap of four illegal bits
 * below it. */
#define VPL_ATTR_KNOWN 0x43FFu

/* ## The pool's bookkeeping lives *in the pool*, and the guest can read it
 *
 * threads/vpl/order does not check totals. It casts a pointer into the pool and
 * walks the kernel's own structures, printing every node's address, `next` and
 * size -- so the layout, the placement and the order of operations are all
 * observable. Its own header declares them:
 *
 *     struct VplBlock      { VplBlock *next; u32 sizeDiv8; };            //  8
 *     struct VplAccounting { void *start, *start2, *startPlusSeven;      // 12
 *                            u32 totalSizeMinus8, allocatedInBlocks;     // 20
 *                            VplBlock *nextFreeBlock;                    // 24
 *                            VplBlock bottomBlock; };                    // 32
 *
 * **That 32-byte accounting struct is the 32 bytes of pool overhead** the
 * create test reports as `poolSize = round_up(size, 8) - 32`. The two facts
 * were measured separately and are the same fact.
 *
 * The block chain runs from `bottomBlock`, which is *inside* the accounting at
 * offset 24, to a size-zero terminator in the last eight bytes of the
 * allocation. Free nodes are linked in a **circular** list through `next`, and
 * the terminator is permanently one of them.
 *
 * The test also pins where the pools *are*: it reaches the middle pool's
 * accounting as `addr3 + 0x18`, which only resolves if three pools created in
 * order descend in memory. So a vpl's pool is allocated from the **high** end.
 */
#define VPL_ACCT_SIZE  32u
#define VPL_BOTTOM     24u    /* bottomBlock's offset within the accounting */

/* Offsets inside the accounting struct. */
enum { VA_START = 0, VA_START2 = 4, VA_START7 = 8, VA_TOTAL_M8 = 12,
       VA_ALLOCED = 16, VA_NEXTFREE = 20 };

typedef struct {
    uint32_t  uid;
    char      name[32];
    uint32_t  attr;
    uint32_t  base;          /* the whole allocation; the accounting is at [0] */
    uint32_t  total;         /* round_up(requested, 8) */
    uint32_t  pool_size;     /* total - 32, what ReferVplStatus reports */
    int       used;
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

/* tlspl does not share it. Partitions 8 and 9 are ILLEGAL_PERM to a vpl and an
 * fpl -- vpl/create.expected and fpl/create.expected both say so -- and
 * ILLEGAL_PARTITION to a tlspl, which is the answer everything out of range
 * gets. So the two services validate the same argument against different
 * tables, and only the 1..7 window is common. Partition 5 is the one value no
 * tlspl test covers; it keeps vpl's answer for want of any evidence. */
static uint32_t tlspl_partition_error(int32_t part) {
    switch (part) {
        case 2: case 6:                      return SCE_KERNEL_ERROR_OK;
        case 1: case 3: case 4: case 5:      return SCE_KERNEL_ERROR_ILLEGAL_PERM;
        default:                             return SCE_KERNEL_ERROR_ILLEGAL_PARTITION;
    }
}

static void vpl_init(psp_vpl *v);

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

    const uint32_t total = round_up(size, VPL_ALIGN);
    const uint32_t pool  = vpl_pool_size(size);
    /* A request the heap cannot meet is NO_MEMORY rather than a bad size --
     * 0x10000000 and 0x02000000 are refused where 0x01800000 succeeds, so the
     * boundary is what is actually free and not a constant.
     *
     * From the high end, which vpl/order pins: it reaches the middle of three
     * pools as `addr3 + 0x18`, and that only resolves if they descend. */
    const uint32_t base = pool ? psp_sysmem_alloc(total, 1) : 0;
    if (pool && !base) { psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }

    psp_vpl *v = NULL;
    for (int i = 0; i < MAX_VPLS; i++) if (!g_vpl[i].used) { v = &g_vpl[i]; break; }
    if (!v) { if (base) psp_sysmem_release(base);
              psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }

    memset(v, 0, sizeof *v);
    psp_str(name, v->name, sizeof v->name);
    v->attr      = attr;
    v->base      = base;
    v->total     = total;
    v->pool_size = pool;
    v->uid       = psp_threadman_next_uid();
    if (base) vpl_init(v);
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

/* ---- the block chain, in guest memory ------------------------------------ */

static uint32_t blk_next(uint32_t b)          { return psp_read32(b); }
static uint32_t blk_size(uint32_t b)          { return psp_read32(b + 4) * 8; }
static void set_next(uint32_t b, uint32_t n)  { psp_write32(b, n); }
static void set_size(uint32_t b, uint32_t sz) { psp_write32(b + 4, sz / 8); }

static uint32_t vpl_bottom(const psp_vpl *v) { return v->base + VPL_BOTTOM; }
static uint32_t vpl_term(const psp_vpl *v)   { return v->base + v->total - 8; }
static uint32_t vpl_head(const psp_vpl *v)   { return psp_read32(v->base + VA_NEXTFREE); }
static void set_head(const psp_vpl *v, uint32_t b) { psp_write32(v->base + VA_NEXTFREE, b); }

/* An allocated block's `next` is the pool's own `start`, which is what lets a
 * free tell one apart from a free block -- and `start` is the accounting plus
 * eight, because every allocated node in order.expected prints `next->08`. */
static uint32_t vpl_start(const psp_vpl *v) { return v->base + 8; }

/* One free block spanning the pool, a size-zero terminator, and the two linked
 * to each other. This is the state the empty-pool lines describe exactly:
 * `bottom at 0x18, next->0xf8, 0xe0` and `0xf8, next->0x18, 0`. */
static void vpl_init(psp_vpl *v) {
    const uint32_t bottom = vpl_bottom(v), term = vpl_term(v);
    psp_write32(v->base + VA_START,   vpl_start(v));
    psp_write32(v->base + VA_START2,  vpl_start(v));
    psp_write32(v->base + VA_START7,  vpl_start(v) + 7);
    psp_write32(v->base + VA_TOTAL_M8, v->total - 8);
    psp_write32(v->base + VA_ALLOCED, 0);
    set_next(bottom, term);  set_size(bottom, term - bottom);
    set_next(term, bottom);  set_size(term, 0);
    set_head(v, bottom);
}

static uint32_t vpl_free_size(const psp_vpl *v) {
    if (!v->base) return 0;
    return v->pool_size - psp_read32(v->base + VA_ALLOCED) * 8;
}

/* Carve from the *top* of the first free block that fits, walking the circular
 * list from the head.
 *
 * Top, not bottom: order.expected has the bottom block shrink from 0xe0 to
 * 0xc8 while the new allocation appears at 0xe0 -- immediately above what is
 * left of the free block. And the head advances to that block's `next`
 * afterwards, which is why a second allocation from the same block leaves the
 * head where it already was. */
/* Open, and the last line threads/vpl/free differs by. That test allocates
 * three adjacent blocks, frees all three, and allocates again: hardware reuses
 * the merged hole where the first one was, and we take the other free region
 * instead. The reason is the head this search starts from -- vpl_give_back
 * leaves it at the free node *preceding* the returned block, which after a
 * forward merge is the low block rather than the merged one.
 *
 * Do not change the head rule on that one observation. order.expected matches
 * today and is what pins it, and its three frees agree with the current rule
 * and with nothing simpler. Whatever replaces it has to satisfy both tests. */
static uint32_t vpl_alloc(psp_vpl *v, uint32_t bytes) {
    if (!v->base) return 0;
    const uint32_t need = round_up(bytes, VPL_ALIGN) + VPL_HEADER;

    uint32_t b = vpl_head(v), prev = 0;
    for (uint32_t guard = 0; guard < 4096; guard++) {
        const uint32_t sz = blk_size(b);
        if (sz >= need) break;
        prev = b;
        b = blk_next(b);
        if (b == vpl_head(v)) return 0;        /* all the way round */
    }
    const uint32_t sz = blk_size(b);
    if (sz < need) return 0;

    const uint32_t at = b + (sz - need);       /* the new block's header */
    if (sz == need) {
        /* The free block is consumed whole, so it leaves the list. Its
         * predecessor has to be found the long way round when the head is the
         * block itself. */
        if (!prev) { prev = b; while (blk_next(prev) != b) prev = blk_next(prev); }
        set_next(prev, blk_next(b));
        set_head(v, blk_next(b));
    } else {
        set_size(b, sz - need);
        set_head(v, blk_next(b));
    }
    set_next(at, vpl_start(v));
    set_size(at, need);
    psp_write32(v->base + VA_ALLOCED,
                psp_read32(v->base + VA_ALLOCED) + need / 8);
    return at + VPL_HEADER;
}

/* Put a block back: into the circular list in address order, coalescing with
 * the neighbour on each side when it is adjacent *and* free.
 *
 * The head ends up at the free node preceding the returned block, which is the
 * merged block itself whenever a backward merge happened. All three frees in
 * order.expected agree with that and with nothing simpler. */
static void vpl_give_back(psp_vpl *v, uint32_t at) {
    const uint32_t size = blk_size(at);
    psp_write32(v->base + VA_ALLOCED,
                psp_read32(v->base + VA_ALLOCED) - size / 8);

    /* The free list is kept in ascending address order and closes on itself
     * through the terminator, which is always its highest node. So the
     * predecessor of a returned block is the greatest free node below it -- or
     * the terminator, when there is none, because that is where the list
     * wraps.
     *
     * Searching only for a node *below* `at` and giving up otherwise is what a
     * non-circular list would want, and it put every block freed beneath the
     * whole free list in the wrong place. */
    uint32_t prev = vpl_term(v);
    {
        uint32_t b = vpl_head(v);
        for (uint32_t guard = 0; guard < 4096; guard++) {
            if (b < at && (prev == vpl_term(v) || b > prev)) prev = b;
            b = blk_next(b);
            if (b == vpl_head(v)) break;
        }
    }
    const uint32_t next = blk_next(prev);

    set_next(at, next);
    set_next(prev, at);

    uint32_t merged = at;
    /* Forward first, so a three-way merge lands on the earliest node. */
    if (at + size == next && blk_size(next) != 0) {
        set_size(at, size + blk_size(next));
        set_next(at, blk_next(next));
    }
    if (prev + blk_size(prev) == at && blk_size(prev) != 0) {
        set_size(prev, blk_size(prev) + blk_size(at));
        set_next(prev, blk_next(at));
        merged = prev;
    }
    set_head(v, merged == at ? prev : merged);
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
    if (may_block && !psp_sched_can_wait()) {
        psp_ret(SCE_KERNEL_ERROR_CAN_NOT_WAIT); return;
    }
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
    /* Cancelled rather than deleted: the object is still there, so looking it
     * up says nothing, and only the waker knew. */
    if (rc == PSP_SCHED_WOKEN && psp_sched_wake_reason() == PSP_WAIT_WOKE_CANCELLED) {
        psp_wait_writeback(tmo_ptr, deadline);
        psp_ret(SCE_KERNEL_ERROR_WAIT_CANCEL);
        return;
    }
    v = find_vpl(id);
    if (!v) { psp_wait_writeback(tmo_ptr, deadline);
              psp_ret(SCE_KERNEL_ERROR_WAIT_DELETE); return; }

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

    /* A pointer that is not exactly the start of one of *this* pool's live
     * blocks is refused, and the tests are thorough about which is which:
     * freeing twice, a NULL, a stack address, a pointer one block into the
     * pool, and another pool's pointer are each ILLEGAL_MEMBLOCK, while a
     * pointer that is not mapped memory at all is 0x800200D3.
     *
     * And that one is decided before the uid is. vpl/free refuses a null uid
     * with a good pointer as UNKNOWN_VPLID and a null uid with 0xDEADBEEF as
     * 0x800200D3, so the pointer is what the kernel objects to first. */
    if (ptr && !psp_mem_ptr(ptr, 4)) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_SIZE); return; }

    psp_vpl *v = find_vpl(id);
    if (!v) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_VPLID); return; }

    /* Walk the chain and require an exact hit on an *allocated* node. That
     * rejects, in one test, all five shapes free.expected tries: a second free
     * (the node's `next` is a free-list pointer by then, not `start`), a NULL,
     * a stack address, a pointer part-way into a block, and another pool's. */
    if (v->base && ptr > v->base && ptr < v->base + v->total) {
        for (uint32_t b = vpl_bottom(v); blk_size(b); b += blk_size(b))
            if (b + VPL_HEADER == ptr && blk_next(b) == vpl_start(v)) {
                vpl_give_back(v, b);
                const int urgent = vpl_release(v);
                psp_ret(SCE_KERNEL_ERROR_OK);
                if (urgent) psp_sched_yield();
                return;
            }
    }
    psp_ret(SCE_KERNEL_ERROR_ILLEGAL_MEMBLOCK_PTR);
}

static void hle_CancelVpl(void) {
    psp_vpl *v = find_vpl(psp_arg(0));
    if (!v) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_VPLID); return; }
    const uint32_t out = psp_arg(1);
    if (out) psp_write32(out, (uint32_t)psp_waitq_count(&v->q));
    const int urgent = psp_waitq_cancel_all(&v->q);
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
    psp_write32(info + 44, vpl_free_size(v));
    psp_write32(info + 48, (uint32_t)psp_waitq_count(&v->q));
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void mpp_reset(void);
static void mbx_reset(void);
static void fpl_reset(void);
static void tls_reset(void);

void psp_kernobj_reset(void) {
    for (int i = 0; i < MAX_VPLS; i++)
        if (g_vpl[i].used && g_vpl[i].base) psp_sysmem_release(g_vpl[i].base);
    memset(g_vpl, 0, sizeof g_vpl);
    mpp_reset();
    mbx_reset();
    fpl_reset();
    tls_reset();
}

static void vpl_list(int type, uint32_t out, int max, int *count) {
    if (type != PSP_TMID_VPL) return;
    for (int i = 0; i < MAX_VPLS; i++) {
        if (!g_vpl[i].used) continue;
        if (out && *count < max) psp_write32(out + (uint32_t)*count * 4, g_vpl[i].uid);
        (*count)++;
    }
}

/* Same for the pools and the queues: deliver, then do the ordinary thing. */
static void hle_AllocateVplCB(void) { psp_threadman_cb_begin(); hle_AllocateVpl(); psp_threadman_cb_end(); }

void psp_kernobj_register(void) {
    psp_threadman_add_lister(vpl_list);
    psp_hle_register(0x56C039B5, "ThreadManForUser", "sceKernelCreateVpl",      hle_CreateVpl);
    psp_hle_register(0x89B3D48C, "ThreadManForUser", "sceKernelDeleteVpl",      hle_DeleteVpl);
    psp_hle_register(0xBED27435, "ThreadManForUser", "sceKernelAllocateVpl",    hle_AllocateVpl);
    psp_hle_register(0xEC0A693F, "ThreadManForUser", "sceKernelAllocateVplCB",  hle_AllocateVplCB);
    psp_hle_register(0xAF36D708, "ThreadManForUser", "sceKernelTryAllocateVpl", hle_TryAllocateVpl);
    psp_hle_register(0xB736E9FF, "ThreadManForUser", "sceKernelFreeVpl",        hle_FreeVpl);
    psp_hle_register(0x1D371B8A, "ThreadManForUser", "sceKernelCancelVpl",      hle_CancelVpl);
    psp_hle_register(0x39810265, "ThreadManForUser", "sceKernelReferVplStatus", hle_ReferVplStatus);
    psp_kernobj_register_mpp();
    psp_kernobj_register_mbx();
    psp_kernobj_register_fpl();
    psp_kernobj_register_tls();
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

/* msgpipe/create makes 1024 of them in a loop and expects every one to
 * succeed, so the table is sized for the test rather than for a guess about
 * what a game needs. */
#define MAX_PIPES 1024
#define MPP_MODE_ASAP 1u
/* Two queues, two attribute bits. Unlike every other object type, a message
 * pipe orders its senders and its receivers separately. */
#define MPP_ATTR_SEND_PRIORITY 0x0100u
#define MPP_ATTR_RECV_PRIORITY 0x1000u
#define MPP_ATTR_MASK          0x51FFu

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
    /* The seventh attribute mask among seven object types, and msgpipe/create's
     * sweep settles it in two lines: 0x3FF is refused and 0x51FF is accepted,
     * so the legal set is the low nine bits plus the two queue-order bits --
     * 0x1FF | 0x1000 | 0x4000, which is 0x51FF exactly. */
    if (attr & ~MPP_ATTR_MASK) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_ATTR); return; }

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

/* Turning a queue out because the pipe is going away. Each waiter still gets
 * told how many bytes it moved before it was abandoned -- msgpipe/data deletes
 * a pipe under a receiver that got nothing and reads back
 * `received = 00000000`, where an untouched word would still hold the 0x1337
 * the test seeded. */
static int mpp_abandon(psp_waitq *q, int reason) {
    int urgent = 0;
    while (q->n > 0) {
        const psp_waiter w = psp_waitq_take(q, 0);
        if (w.nout) psp_write32(w.nout, w.done);
        urgent |= psp_sched_wake_as(w.uid, reason);
    }
    return urgent;
}

static void hle_DeleteMsgPipe(void) {
    psp_msgpipe *p = find_pipe(psp_arg(0));
    if (!p) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_MPPID); return; }
    int urgent = mpp_abandon(&p->send_q, 0);
    urgent |= mpp_abandon(&p->recv_q, 0);
    if (p->base) psp_sysmem_release(p->base);
    p->alive = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
    if (urgent) psp_sched_yield();
}

/* ---- moving bytes through the pipe ----------------------------------------
 *
 * A message pipe is a byte stream, not a queue of messages, and threads/msgpipe
 * measures that distinction directly. Three rules follow from it, and none of
 * them survives an all-or-nothing transfer:
 *
 *   - **A waiting receiver is filled in pieces.** msgpipe/data has three
 *     receivers wanting four bytes each and senders offering three at a time;
 *     the partial bytes land in the *receiver's own buffer* and it keeps
 *     waiting for the rest. There is nowhere else for them to go, and the test
 *     proves they went somewhere by printing what each receiver finally read:
 *     `msg1`, `msg2`, `msg3`, assembled across separate sends.
 *
 *   - **With the buffer empty, a sender hands bytes straight to a receiver.**
 *     That is the only path on a pipe created with no buffer, and the same
 *     test runs the whole scenario twice -- once with 0x100 bytes of buffer and
 *     once with none -- and gets identical output.
 *
 *   - **Blocked senders top the buffer up as it drains.** A receiver that
 *     empties the buffer pulls the next sender's bytes into it.
 *
 * The three are one loop, run after any change: senders fill, the buffer feeds
 * receivers, repeat while anything moved.
 *
 * The two queues also take their release order from *different* attribute bits.
 * 0x100 orders senders and 0x1000 orders receivers, and msgpipe/data pins the
 * second on its own: a pipe created with attr 0x1000 serves three receivers at
 * priorities 0x33, 0x32 and 0x31 in the order 0x31, 0x32, 0x33 while its
 * senders stay first-come. Passing the raw attribute to both queues gave
 * receivers the sender's rule. */

static uint32_t mpp_min(uint32_t a, uint32_t b) { return a < b ? a : b; }

/* Copy between two guest addresses a byte at a time; the pipe never moves
 * enough at once for anything cleverer to matter. */
static void mpp_copy(uint32_t dst, uint32_t src, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        psp_write8(dst + i, src ? psp_read8(src + i) : 0);
}

/* A waiter is finished when it has all it asked for -- or, in ASAP mode, as
 * soon as it has anything at all. */
static int mpp_satisfied(const psp_waiter *w) {
    return w->done >= w->need || ((w->mode & MPP_MODE_ASAP) && w->done > 0);
}

/* A waiter released because it got what it asked for, as against one turned out
 * because the pipe is being destroyed. Both arrive at the waiter as a wake, and
 * it cannot tell them apart by looking: the queue entry is gone and so is the
 * pipe. It matters -- msgpipe/tryreceive deletes a pipe immediately after a
 * poll that satisfied two senders, and hardware answers those senders
 * `00000000`, not `800201b5`. Bytes already moved are moved. */
/* Done with this waiter: report how much moved, take it out, wake it. */
static void mpp_release(psp_waitq *q, int i, int *urgent) {
    const psp_waiter w = psp_waitq_take(q, i);
    if (w.nout) psp_write32(w.nout, w.done);
    *urgent |= psp_sched_wake_as(w.uid, PSP_WAIT_WOKE_SATISFIED);
}

static uint32_t mpp_send_order(const psp_msgpipe *p) {
    return (p->attr & MPP_ATTR_SEND_PRIORITY) ? PSP_WAITQ_PRIORITY : 0;
}
static uint32_t mpp_recv_order(const psp_msgpipe *p) {
    return (p->attr & MPP_ATTR_RECV_PRIORITY) ? PSP_WAITQ_PRIORITY : 0;
}

/* What a transfer could move right now, counting the buffer *and* what the
 * threads blocked on the other side are holding. A poll moves all-or-nothing
 * and cannot take bytes back once a waiting thread has been handed them, so it
 * has to know the answer before it starts rather than by trying.
 *
 * The two are not symmetric only because the buffer is not: a receiver takes
 * what is stored and then reaches past it into the senders, while a sender
 * hands to receivers first and stores the rest. After a pump either the buffer
 * is empty or nobody is waiting to receive, so the send side can add the two
 * without double-counting. */
static uint32_t mpp_recv_capacity(const psp_msgpipe *p) {
    uint32_t n = p->used;
    for (int i = 0; i < p->send_q.n; i++)
        n += p->send_q.w[i].need - p->send_q.w[i].done;
    return n;
}

static uint32_t mpp_send_capacity(const psp_msgpipe *p) {
    uint32_t n = mpp_free(p);
    for (int i = 0; i < p->recv_q.n; i++)
        n += p->recv_q.w[i].need - p->recv_q.w[i].done;
    return n;
}

/* Move whatever can move, and keep going until nothing does.
 *
 * `exclude` is the caller's own uid: it is in the queue so that it sits in the
 * right place in the release order, but it is not blocked yet, so it must not
 * be woken or removed here. Its caller reads its progress out of the queue
 * afterwards.
 *
 * Receiver-driven, which is the part that took measuring. A receiver takes from
 * the buffer and then reaches into the blocked senders for the rest; a sender
 * only *stores* when its whole remainder fits, or when it is in ASAP mode and
 * will settle for whatever room there is. That asymmetry is what
 * msgpipe/data's send-priority block reads back: a full-wait sender arriving at
 * a pipe with one byte free does not leave that byte behind it, so the receiver
 * that comes next gets the byte from a *different*, more urgent sender --
 * `msgs`, not `msg1`. */
static void mpp_pump(psp_msgpipe *p, uint32_t exclude, int *urgent) {
    for (int spinning = 1; spinning; ) {
        spinning = 0;

        /* Receivers pull: the buffer first, then straight from blocked senders,
         * which is the only path on a pipe with no buffer at all. */
        for (;;) {
            const int ri = psp_waitq_pick(&p->recv_q, mpp_recv_order(p));
            if (ri < 0) break;
            psp_waiter *r = &p->recv_q.w[ri];
            const uint32_t fromBuf = mpp_min(p->used, r->need - r->done);
            if (fromBuf) {
                mpp_take(p, r->out + r->done, fromBuf);
                r->done += fromBuf;
                spinning = 1;
            }

            /* Direct handoff only with the buffer drained: bytes still in it
             * came first and must be delivered first. */
            while (!p->used && r->done < r->need) {
                const int si = psp_waitq_pick(&p->send_q, mpp_send_order(p));
                if (si < 0) break;
                psp_waiter *s = &p->send_q.w[si];
                const uint32_t n = mpp_min(s->need - s->done, r->need - r->done);
                if (!n) break;
                mpp_copy(r->out + r->done, s->out + s->done, n);
                s->done += n; r->done += n;
                spinning = 1;
                if (!mpp_satisfied(s) || s->uid == exclude) break;
                mpp_release(&p->send_q, si, urgent);
            }

            if (!mpp_satisfied(r)) break;
            /* The caller is in the queue for its position in the order, not to
             * be woken: it is still running. It also stops the scan, because
             * anyone behind it is behind it. Its own code takes it out and
             * pumps again. */
            if (r->uid == exclude) break;
            mpp_release(&p->recv_q, ri, urgent);
        }

        /* Senders store what the buffer will hold. */
        for (;;) {
            const uint32_t room = mpp_free(p);
            if (!room) break;
            const int si = psp_waitq_pick(&p->send_q, mpp_send_order(p));
            if (si < 0) break;
            psp_waiter *s = &p->send_q.w[si];
            const uint32_t left = s->need - s->done;
            if (!left) break;
            /* All of it, or nothing -- unless ASAP, which takes what there is. */
            if (left > room && !(s->mode & MPP_MODE_ASAP)) break;
            const uint32_t n = mpp_min(room, left);
            mpp_put(p, s->out + s->done, n);
            s->done += n;
            spinning = 1;
            if (!mpp_satisfied(s)) break;
            if (s->uid == exclude) break;
            mpp_release(&p->send_q, si, urgent);
        }
    }
}

/* send and receive are the same shape with the direction reversed, so they
 * share a body: `sending` picks which queue is ours and which is theirs, which
 * error a full-or-empty poll gives, and which way the bytes go.
 *
 * The caller joins its own queue before anything moves, rather than trying the
 * transfer first and queueing only on failure. That is what puts it in the
 * right place in the order relative to threads already waiting, and it means
 * the immediate case and the blocking case are the same code. */
static void mpp_transfer(int sending, int may_block, int has_timeout) {
    if (may_block && !psp_sched_can_wait()) {
        psp_ret(SCE_KERNEL_ERROR_CAN_NOT_WAIT); return;
    }
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

    /* Nothing asked for is nothing to wait for: a zero-length transfer
     * succeeds even on a pipe with no buffer at all. */
    if (len == 0) {
        if (out) psp_write32(out, 0);
        psp_wait_writeback(tmo_ptr, deadline);
        psp_ret(SCE_KERNEL_ERROR_OK);
        return;
    }

    int urgent = 0;
    /* Settle whoever is already waiting before looking at our own request: a
     * thread that arrived earlier is ahead of us in the order, and what it
     * takes changes what is left. */
    mpp_pump(p, 0, &urgent);

    const uint32_t me = psp_sched_current();

    /* A poll reaches past the buffer exactly as a blocking call does -- a
     * try-receive on a pipe with no buffer at all still succeeds when a sender
     * is blocked on the other side, and msgpipe/tryreceive says so twice:
     * `Partial packet: OK (bytes=128)` and `Complete packet: OK (bytes=256)`,
     * both against `buffer=0`.
     *
     * What it may not do is half-succeed. So it asks how much could move,
     * decides, and only then joins the queue -- by which point completing is
     * certain, and the ordinary pump does the work. */
    if (!may_block) {
        const uint32_t cap = sending ? mpp_send_capacity(p) : mpp_recv_capacity(p);
        const uint32_t now = (mode & MPP_MODE_ASAP) ? mpp_min(cap, len) : len;
        if (!now || now > cap) {
            /* ASAP moved nothing, and says so: `ASAP: Failed (800201b3,
             * bytes=0)`. A full-wait failure leaves the word alone, which is
             * how the tests tell the two apart -- they pre-seed it 0x1337. */
            if ((mode & MPP_MODE_ASAP) && out) psp_write32(out, 0);
            psp_ret(sending ? SCE_KERNEL_ERROR_MSGPIPE_FULL
                            : SCE_KERNEL_ERROR_MSGPIPE_EMPTY);
            if (urgent) psp_sched_yield();
            return;
        }
        psp_waitq *pq = sending ? &p->send_q : &p->recv_q;
        /* Asking for exactly what is there, as a full-wait: the pump then fills
         * it precisely rather than stopping at the first byte. */
        if (psp_waitq_add(pq, me, now, 0, buf) != 0) {
            psp_ret(SCE_KERNEL_ERROR_NO_MEMORY);
            return;
        }
        mpp_pump(p, me, &urgent);
        for (int k = 0; k < pq->n; k++)
            if (pq->w[k].uid == me) { psp_waitq_take(pq, k); break; }
        mpp_pump(p, 0, &urgent);
        if (out) psp_write32(out, now);
        psp_ret(SCE_KERNEL_ERROR_OK);
        if (urgent) psp_sched_yield();
        return;
    }

    psp_waitq *q = sending ? &p->send_q : &p->recv_q;
    if (psp_waitq_add(q, me, len, mode, buf) != 0) {
        psp_ret(SCE_KERNEL_ERROR_NO_MEMORY);
        return;
    }
    q->w[q->n - 1].nout = out;
    mpp_pump(p, me, &urgent);

    int i = -1;
    for (int k = 0; k < q->n; k++) if (q->w[k].uid == me) { i = k; break; }
    const uint32_t done = i >= 0 ? q->w[i].done : len;

    if (i < 0 || mpp_satisfied(&q->w[i])) {
        if (i >= 0) {
            psp_waitq_take(q, i);
            /* Out of the way: whoever was behind us can move now. */
            mpp_pump(p, 0, &urgent);
        }
        if (out) psp_write32(out, done);
        psp_wait_writeback(tmo_ptr, deadline);
        psp_ret(SCE_KERNEL_ERROR_OK);
        if (urgent) psp_sched_yield();
        return;
    }

    if (urgent) psp_sched_yield();
    const int rc = psp_sched_block_until(me, PSP_SCHED_BLOCKED,
                                         sending ? p->senddesc : p->recvdesc,
                                         deadline);
    /* Checked before the pipe is looked up again, because a pipe deleted
     * between our release and our turn on the CPU does not undo the transfer.
     * The other side moved the bytes on our behalf, took us out of the queue,
     * and wrote how many -- it is the only one that knew, since an ASAP waiter
     * can be released with less than it asked for. */
    if (rc == PSP_SCHED_WOKEN) {
        const int why = psp_sched_wake_reason();
        if (why == PSP_WAIT_WOKE_SATISFIED) {
            psp_wait_writeback(tmo_ptr, deadline);
            psp_ret(SCE_KERNEL_ERROR_OK);
            return;
        }
        /* Cancelled rather than deleted. The pipe is still there, so looking it
         * up would say nothing; only the waker knew. */
        if (why == PSP_WAIT_WOKE_CANCELLED) {
            psp_wait_writeback(tmo_ptr, deadline);
            psp_ret(SCE_KERNEL_ERROR_WAIT_CANCEL);
            return;
        }
    }

    p = find_pipe(id);
    if (!p) { psp_wait_writeback(tmo_ptr, deadline);
              psp_ret(SCE_KERNEL_ERROR_WAIT_DELETE); return; }
    q = sending ? &p->send_q : &p->recv_q;

    if (rc == PSP_SCHED_WOKEN) {
        psp_wait_writeback(tmo_ptr, deadline);
        psp_ret(SCE_KERNEL_ERROR_OK);
        return;
    }
    /* A wait that ran out reports what it did get, which for a full-wait is
     * nothing: `bytes=0`, where an *argument* failure leaves the caller's
     * 0x1337 in place because it never waited. */
    for (int k = 0; k < q->n; k++)
        if (q->w[k].uid == me && out) { psp_write32(out, q->w[k].done); break; }
    psp_waitq_drop(q, me);
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
    int urgent = mpp_abandon(&p->send_q, PSP_WAIT_WOKE_CANCELLED);
    urgent |= mpp_abandon(&p->recv_q, PSP_WAIT_WOKE_CANCELLED);
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

static void mpp_list(int type, uint32_t out, int max, int *count) {
    if (type != PSP_TMID_MSGPIPE) return;
    for (int i = 0; i < MAX_PIPES; i++) {
        if (!g_pipe[i].alive) continue;
        if (out && *count < max) psp_write32(out + (uint32_t)*count * 4, g_pipe[i].uid);
        (*count)++;
    }
}

static void hle_SendMsgPipeCB(void)    { psp_threadman_cb_begin(); hle_SendMsgPipe(); psp_threadman_cb_end(); }
static void hle_ReceiveMsgPipeCB(void) { psp_threadman_cb_begin(); hle_ReceiveMsgPipe(); psp_threadman_cb_end(); }

void psp_kernobj_register_mpp(void) {
    psp_threadman_add_lister(mpp_list);
    psp_hle_register(0x7C0DC2A0, "ThreadManForUser", "sceKernelCreateMsgPipe",     hle_CreateMsgPipe);
    psp_hle_register(0xF0B7DA1C, "ThreadManForUser", "sceKernelDeleteMsgPipe",     hle_DeleteMsgPipe);
    psp_hle_register(0x876DBFAD, "ThreadManForUser", "sceKernelSendMsgPipe",       hle_SendMsgPipe);
    psp_hle_register(0x7C41F2C2, "ThreadManForUser", "sceKernelSendMsgPipeCB",     hle_SendMsgPipeCB);
    psp_hle_register(0x884C9F90, "ThreadManForUser", "sceKernelTrySendMsgPipe",    hle_TrySendMsgPipe);
    psp_hle_register(0x74829B76, "ThreadManForUser", "sceKernelReceiveMsgPipe",    hle_ReceiveMsgPipe);
    psp_hle_register(0xFBFA697D, "ThreadManForUser", "sceKernelReceiveMsgPipeCB",  hle_ReceiveMsgPipeCB);
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

/* Every `Create 1024` case in the suite builds a thousand objects in a loop and
 * expects the thousandth to succeed, so a cap below that is not a resource
 * limit being modelled -- it is ours, and it shows up as `Failed at 128`. The
 * headroom above 1024 is because the process already holds some: callbacks
 * stopped at 1023 with the cap at exactly 1024. Hardware's real ceiling is
 * higher and is measured nowhere here. */
#define MAX_MBXES 2048
#define MBX_ATTR_KNOWN    0x5FFu   /* sixth object type, sixth rule */
#define MBX_ATTR_MSG_PRIO 0x400u

enum { MSG_NEXT = 0, MSG_PRIO = 4 };   /* offsets in SceKernelMsgPacket */

typedef struct {
    uint32_t  uid;
    char      name[32];
    uint32_t  attr;
    /* Guest address of the *last* message, 0 when empty. The head is derived
     * from it -- see mbx_first for why that is not an internal choice. */
    uint32_t  last;
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

/* The queue is circular and the mailbox holds its **last** message, not its
 * first. `firstMessage` is derived: it is whatever the last one points at.
 *
 * That is not an implementation choice, it is what mbx/send measures. The test
 * sends two messages, then reaches into the guest-owned packet header of the
 * *second* one and rewrites its `next`, and reads the mailbox back:
 *
 *     next = itself   ->  first=OTHER, and the walk starts at that message
 *     next = NULL     ->  first=NULL, with count still 2
 *
 * Neither is possible if the kernel kept a head pointer -- the guest cannot
 * reach it. Both fall out of `first = last->next`. */
static uint32_t mbx_first(const psp_mbx *m) {
    /* Not gated on the count. mbx/send drains a tampered box to zero and still
     * reads `count=0, first=OTHER` with a walkable message behind it, so the
     * head is whatever the last one points at and nothing else. */
    return m->last ? psp_read32(m->last + MSG_NEXT) : 0;
}

/* Is this packet already in the ring? A message may be in one queue at a time,
 * and mbx/send sends the same packet twice to find out what happens: the second
 * send is refused and the count stays at 1. Bounded by the count, because the
 * ring being walked may be one the guest has edited. */
static int mbx_holds(const psp_mbx *m, uint32_t msg) {
    uint32_t at = mbx_first(m);
    for (uint32_t i = 0; i < m->count && at; i++) {
        if (at == msg) return 1;
        at = psp_read32(at + MSG_NEXT);
    }
    return 0;
}

static void mbx_insert(psp_mbx *m, uint32_t msg) {
    if (!m->count) {
        psp_write32(msg + MSG_NEXT, msg);     /* a ring of one */
        m->last  = msg;
        m->count = 1;
        return;
    }
    if (m->attr & MBX_ATTR_MSG_PRIO) {
        /* Ordered by the packet's own priority byte, ahead of equals. */
        const uint32_t pri = psp_read8(msg + MSG_PRIO);
        const uint32_t head = mbx_first(m);
        uint32_t prev = m->last, at = head, i = 0;
        for (; i < m->count; i++) {
            if (psp_read8(at + MSG_PRIO) > pri) break;
            prev = at;
            at = psp_read32(at + MSG_NEXT);
        }
        psp_write32(msg + MSG_NEXT, at);
        psp_write32(prev + MSG_NEXT, msg);
        /* Going in front of the head and going after the last produce the same
         * two links -- in a ring, `last -> msg -> head` is both -- so the links
         * cannot say which happened and the walk has to. Only a search that ran
         * out of messages appended. */
        if (i == m->count) m->last = msg;
        m->count++;
        return;
    }
    /* The head is read before the link it is derived from is overwritten:
     * `first` is `last->next`, so appending destroys it. */
    const uint32_t head = mbx_first(m);
    psp_write32(m->last + MSG_NEXT, msg);
    psp_write32(msg + MSG_NEXT, head);
    m->last = msg;
    m->count++;
}

static uint32_t mbx_pop(psp_mbx *m) {
    if (!m->count) return 0;
    const uint32_t head = mbx_first(m);
    /* The box is empty when the message leaving *is* the last one -- which is
     * not the same as the count reaching zero, and mbx/send separates them. Its
     * "evil" case leaves a ring whose head is not the last, and draining that
     * to a count of zero still leaves a last behind, which the status then
     * reports as a non-null first. */
    if (head == m->last) { m->last = 0; m->count = 0; }
    else {
        /* The last one closes the ring over the head that is leaving. */
        psp_write32(m->last + MSG_NEXT, psp_read32(head + MSG_NEXT));
        m->count--;
    }
    /* And its `next` is left exactly as it was. A received packet keeps
     * pointing wherever it pointed in the ring, which mbx/receive reads back
     * for every case in turn:
     *
     *     Single standard:      next=ITSELF     ring of one
     *     Multiple standard #1: next=FIRST      the other message, now first
     *     Multiple standard #2: next=ITSELF     ring of one again
     *
     * Overwriting it with the packet's own address gets the single-message
     * cases right for the wrong reason -- a ring of one already points at
     * itself -- and the middle case wrong, which is what it did. */
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
    /* A packet belongs to one queue at a time. mbx/send sends the same one
     * twice and the second is refused with the count left at 1. */
    if (mbx_holds(m, msg)) { psp_ret(SCE_KERNEL_ERROR_MBX_CORRUPT); return; }
    /* And nothing is appended to a ring the guest has already broken. Where a
     * *receive* from such a mailbox has two distinguishable failures, a send
     * has one observable effect: mbx/refer breaks the ring, sends another
     * message, and reads the count back unchanged. */
    if (m->count && !mbx_first(m)) { psp_ret(SCE_KERNEL_ERROR_MBX_CORRUPT); return; }

    /* A waiting receiver takes it without it ever joining the queue -- and
     * "never joining" is observable, because the packet's `next` is the
     * guest's own memory and it is left exactly as the guest left it.
     * mbx/priority poisons every packet with 0xDEADBEEF before sending and
     * reads that value back out of the delivered message: `GOT: "hi 2"
     * (next=DEAD)`. Writing a ring of one over it, which is what a message
     * that had been queued would carry, reports ITSELF instead. */
    const int i = psp_waitq_pick(&m->q, m->attr);
    if (i >= 0) {
        const psp_waiter w = psp_waitq_take(&m->q, i);
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
    if (may_block && !psp_sched_can_wait()) {
        psp_ret(SCE_KERNEL_ERROR_CAN_NOT_WAIT); return;
    }
    const uint32_t id      = psp_arg(0);
    const uint32_t out     = psp_arg(1);
    const uint32_t tmo_ptr = has_timeout ? psp_arg(2) : 0;

    psp_mbx *m = find_mbx(id);
    if (!m) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_MBXID); return; }

    const uint64_t deadline = psp_wait_deadline(tmo_ptr);

    if (m->count) {
        /* Two ways the guest can have broken the ring since the send, both
         * measured by mbx/send tampering with a packet it already handed over,
         * and each with its own code:
         *
         *   next = NULL    ->  there is no head to take        800200D3
         *   count says more than one, and the head *is* the last, so taking it
         *   would empty a box holding two                      800201C9
         *
         * Its third case -- a two-node ring where the head is not the last --
         * is left alone, because that one hardware simply serves. */
        const uint32_t head = mbx_first(m);
        if (!head) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_SIZE); return; }
        if (m->count > 1 && head == m->last) {
            psp_ret(SCE_KERNEL_ERROR_MBX_CORRUPT);
            return;
        }
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
    /* Cancelled rather than deleted: the object is still there, so looking it
     * up says nothing, and only the waker knew. */
    if (rc == PSP_SCHED_WOKEN && psp_sched_wake_reason() == PSP_WAIT_WOKE_CANCELLED) {
        psp_wait_writeback(tmo_ptr, deadline);
        psp_ret(SCE_KERNEL_ERROR_WAIT_CANCEL);
        return;
    }
    m = find_mbx(id);
    if (!m) { psp_wait_writeback(tmo_ptr, deadline);
              psp_ret(SCE_KERNEL_ERROR_WAIT_DELETE); return; }

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
    const int urgent = psp_waitq_cancel_all(&m->q);
    psp_ret(SCE_KERNEL_ERROR_OK);
    if (urgent) psp_sched_yield();
}

static void hle_ReferMbxStatus(void) {
    const psp_mbx *m = find_mbx(psp_arg(0));
    const uint32_t info = psp_arg(1);
    if (!m)    { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_MBXID); return; }
    if (!info) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
    /* A caller offering zero bytes gets zero back and nothing written -- the
     * rule the other Refer calls already follow, which this one did not.
     * mbx/refer sweeps the size field: `Size 00000000 => 00000000`, against
     * `=> 00000034` for every other value it tries, including -1. */
    if (psp_read32(info) == 0) { psp_ret(SCE_KERNEL_ERROR_OK); return; }
    psp_write32(info +  0, 52);
    psp_threadman_write_name(info + 4, m->name);
    psp_write32(info + 36, m->attr);
    psp_write32(info + 40, (uint32_t)psp_waitq_count(&m->q));
    psp_write32(info + 44, m->count);
    psp_write32(info + 48, mbx_first(m));
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void mbx_list(int type, uint32_t out, int max, int *count) {
    if (type != PSP_TMID_MBX) return;
    for (int i = 0; i < MAX_MBXES; i++) {
        if (!g_mbx[i].alive) continue;
        if (out && *count < max) psp_write32(out + (uint32_t)*count * 4, g_mbx[i].uid);
        (*count)++;
    }
}

static void hle_ReceiveMbxCB(void) { psp_threadman_cb_begin(); hle_ReceiveMbx(); psp_threadman_cb_end(); }

void psp_kernobj_register_mbx(void) {
    psp_threadman_add_lister(mbx_list);
    psp_hle_register(0x8125221D, "ThreadManForUser", "sceKernelCreateMbx",        hle_CreateMbx);
    psp_hle_register(0x86255ADA, "ThreadManForUser", "sceKernelDeleteMbx",        hle_DeleteMbx);
    psp_hle_register(0xE9B3061E, "ThreadManForUser", "sceKernelSendMbx",          hle_SendMbx);
    psp_hle_register(0x18260574, "ThreadManForUser", "sceKernelReceiveMbx",       hle_ReceiveMbx);
    psp_hle_register(0xF3986382, "ThreadManForUser", "sceKernelReceiveMbxCB",     hle_ReceiveMbxCB);
    psp_hle_register(0x0D81716A, "ThreadManForUser", "sceKernelPollMbx",          hle_PollMbx);
    psp_hle_register(0x87D4DD36, "ThreadManForUser", "sceKernelCancelReceiveMbx", hle_CancelReceiveMbx);
    psp_hle_register(0xA8E8C846, "ThreadManForUser", "sceKernelReferMbxStatus",   hle_ReferMbxStatus);
}

/* ---- fpl: the fixed-size block pool ---------------------------------------
 *
 * The simple one, and simple in a way worth stating: **nothing is rounded and
 * nothing is reserved**. A pool created with a block size of 0x2F reports
 * `blockSize=0000002f`, a count of 0x2F reports `numBlocks=0000002f`, and
 * consecutive allocations from a 16-byte pool are exactly 16 bytes apart --
 * the test says so in words, `Alloc #2 is 16 bytes after #1`. So there is no
 * per-block header and no per-pool overhead, which is the opposite of the vpl
 * above and had to be checked rather than assumed from the neighbour.
 */

/* Same reasoning as the caps in threadman.c: the tests build a thousand and
 * expect the thousandth to work, with headroom for what the process already
 * holds. Missing this one showed up as `Failed at 0` -- the earlier sections
 * of fpl/create leave their pools alive, so the loop had no slot to start
 * from and the failure looked like memory rather than bookkeeping. */
#define MAX_FPLS 2048
/* create.expected creates pools of 0x131, 0x136 and 0x139 blocks and expects
 * each to succeed. A count of 0x04000000 is refused, but for want of memory
 * rather than a table limit. */
#define MAX_FPL_BLOCKS 1024

/* Seventh object type, seventh attribute rule: 0x41FF. create.expected accepts
 * 0x1, 0x100, 0x4000 and 0x41FF and refuses 0x200, 0x300, 0x400, 0x800,
 * 0x1000, 0x2000, 0x8000, 0x10000, 0x20000, 0x40000 and 0x80000 -- so bit 9 is
 * illegal here where a vpl takes it, and the two differ by exactly that bit. */
#define FPL_ATTR_KNOWN 0x41FFu

typedef struct {
    uint32_t  uid;
    char      name[32];
    uint32_t  attr;
    uint32_t  base, block_size, nblocks;
    /* What a block costs, which is what it holds rounded up to the option
     * struct's alignment. Reported blockSize stays what was asked for. */
    uint32_t  stride;
    /* The free list is a *queue*, not a lowest-first search.
     *
     * threads/fpl/allocate says so in words: after freeing the first block, the
     * next allocation lands *above* the second rather than back in the hole --
     * `Alloc #2 is 16 bytes before #3`, where reusing the lowest free block
     * would put #3 below #2 and print "after". A freed block goes to the back
     * of the queue and allocations keep climbing. */
    uint16_t  freelist[MAX_FPL_BLOCKS];
    uint32_t  head, free_blocks;
    int       alive;
    psp_waitq q;
    char      waitdesc[64];
} psp_fpl;

static psp_fpl g_fpl[MAX_FPLS];

static void fpl_reset(void) {
    for (int i = 0; i < MAX_FPLS; i++)
        if (g_fpl[i].alive && g_fpl[i].base) psp_sysmem_release(g_fpl[i].base);
    memset(g_fpl, 0, sizeof g_fpl);
}

static psp_fpl *find_fpl(uint32_t id) {
    for (int i = 0; i < MAX_FPLS; i++)
        if (g_fpl[i].alive && g_fpl[i].uid == id) return &g_fpl[i];
    return NULL;
}

static void hle_CreateFpl(void) {
    /* (name, partition, attr, blockSize, numBlocks, option) */
    const uint32_t name  = psp_arg(0);
    const int32_t  part  = (int32_t)psp_arg(1);
    const uint32_t attr  = psp_arg(2);
    const uint32_t bsize = psp_arg(3);
    const uint32_t count = psp_arg(4);

    /* NO_MEMORY for a null name, as for a message pipe and unlike everything
     * else -- the two threadman.c predicted before either type existed. */
    if (!name) { psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }
    const uint32_t pe = vpl_partition_error(part);
    if (pe) { psp_ret(pe); return; }
    if (attr & ~FPL_ATTR_KNOWN) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_ATTR); return; }
    if (bsize == 0 || (int32_t)bsize < 0) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_MEMSIZE); return; }
    if (count == 0 || (int32_t)count < 0) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_MEMSIZE); return; }
    /* A pool whose blocks would not fit in an address space is a *size* error,
     * not a memory one: `Count 0x04000000` with a 0x100 block is refused with
     * ILLEGAL_MEMSIZE where merely asking for too much answers NO_MEMORY. */
    if (count > 0xFFFFFFFFu / bsize) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_MEMSIZE); return; }
    if (count > MAX_FPL_BLOCKS) { psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }

    /* The option block's second word is an alignment, and it must be zero or a
     * power of two. create.expected sweeps it: 0, 1, 2, 4 and 8 are accepted;
     * -1, 3, 5, 6 and 7 are refused -- and refused with the *partition* code,
     * which is the one thing about it that could not have been guessed. */
    const uint32_t opt = psp_arg(5);
    uint32_t align = 4;
    if (opt && psp_mem_ptr(opt, 8)) {
        const uint32_t a = psp_read32(opt + 4);
        if (a && (a & (a - 1))) {
            psp_ret(SCE_KERNEL_ERROR_ILLEGAL_PARTITION);
            return;
        }
        if (a > align) align = a;
    }
    /* And having been checked, it spaces the blocks out, the same way a
     * tlspl's does. fpl/tryallocate creates 0x10-byte blocks with alignment 32
     * and measures 32 bytes between two of them; every other section of that
     * test leaves the options null and measures 16. */
    const uint32_t stride = (bsize + align - 1) & ~(align - 1);

    const uint32_t base = psp_sysmem_alloc(stride * count, 0);
    if (!base) { psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }

    psp_fpl *f = NULL;
    for (int i = 0; i < MAX_FPLS; i++) if (!g_fpl[i].alive) { f = &g_fpl[i]; break; }
    if (!f) { psp_sysmem_release(base); psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }

    memset(f, 0, sizeof *f);
    psp_str(name, f->name, sizeof f->name);
    f->attr = attr; f->base = base; f->block_size = bsize; f->nblocks = count;
    f->stride = stride;
    f->free_blocks = count;
    for (uint32_t i = 0; i < count; i++) f->freelist[i] = (uint16_t)i;
    f->uid   = psp_threadman_next_uid();
    f->alive = 1;
    char nm[sizeof f->name];
    memcpy(nm, f->name, sizeof nm);
    snprintf(f->waitdesc, sizeof f->waitdesc, "sceKernelAllocateFpl(%s)", nm);
    psp_ret(f->uid);
}

static void hle_DeleteFpl(void) {
    psp_fpl *f = find_fpl(psp_arg(0));
    if (!f) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_FPLID); return; }
    const int urgent = psp_waitq_release_all(&f->q);
    if (f->base) psp_sysmem_release(f->base);
    f->alive = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
    if (urgent) psp_sched_yield();
}

/* Lowest free block first, which the test checks by address: `Alloc #2 is 16
 * bytes after #1`. */
static uint32_t fpl_take(psp_fpl *f) {
    if (!f->free_blocks) return 0;
    const uint32_t i = f->freelist[f->head];
    f->head = (f->head + 1) % f->nblocks;
    f->free_blocks--;
    return f->base + i * f->stride;
}

/* Is this block index currently handed out? The queue holds the free ones from
 * `head` for `free_blocks` entries, so anything not in that window is live. */
static int fpl_is_taken(const psp_fpl *f, uint32_t idx) {
    for (uint32_t k = 0; k < f->free_blocks; k++)
        if (f->freelist[(f->head + k) % f->nblocks] == idx) return 0;
    return 1;
}

static int fpl_release(psp_fpl *f) {
    int urgent = 0;
    for (;;) {
        const int i = psp_waitq_pick(&f->q, f->attr);
        if (i < 0 || !f->free_blocks) break;
        const psp_waiter w = psp_waitq_take(&f->q, i);
        const uint32_t got = fpl_take(f);
        if (w.out) psp_write32(w.out, got);
        /* Woken as *satisfied*, not merely woken. Whether the pool still
         * exists when the waiter next runs says nothing about whether it got
         * a block, and fpl/allocate deletes the pool immediately after the
         * free that hands one over. Same reason a message pipe does it. */
        urgent |= psp_sched_wake_as(w.uid, PSP_WAIT_WOKE_SATISFIED);
    }
    return urgent;
}

static void fpl_allocate(int may_block, int has_timeout) {
    if (may_block && !psp_sched_can_wait()) {
        psp_ret(SCE_KERNEL_ERROR_CAN_NOT_WAIT); return;
    }
    const uint32_t id      = psp_arg(0);
    const uint32_t out     = psp_arg(1);
    const uint32_t tmo_ptr = has_timeout ? psp_arg(2) : 0;

    psp_fpl *f = find_fpl(id);
    if (!f) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_FPLID); return; }

    const uint64_t deadline = psp_wait_deadline(tmo_ptr);

    if (f->free_blocks && psp_waitq_count(&f->q) == 0) {
        const uint32_t got = fpl_take(f);
        if (out) psp_write32(out, got);
        psp_wait_writeback(tmo_ptr, deadline);
        psp_ret(SCE_KERNEL_ERROR_OK);
        return;
    }
    if (!may_block) { psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }

    const uint32_t me = psp_sched_current();
    if (psp_waitq_add(&f->q, me, 0, 0, out) != 0) {
        psp_ret(SCE_KERNEL_ERROR_NO_MEMORY);
        return;
    }
    const int rc = psp_sched_block_until(me, PSP_SCHED_BLOCKED, f->waitdesc,
                                         deadline);
    /* Both of these are decided before the pool is looked up again, because
     * neither answer depends on whether it is still there. A block already
     * handed over is not taken back by the delete that follows, and a cancel
     * leaves the pool in place so finding it would prove nothing. */
    if (rc == PSP_SCHED_WOKEN) {
        const int why = psp_sched_wake_reason();
        if (why == PSP_WAIT_WOKE_SATISFIED) {
            psp_wait_writeback(tmo_ptr, deadline);
            psp_ret(SCE_KERNEL_ERROR_OK);
            return;
        }
        if (why == PSP_WAIT_WOKE_CANCELLED) {
            psp_wait_writeback(tmo_ptr, deadline);
            psp_ret(SCE_KERNEL_ERROR_WAIT_CANCEL);
            return;
        }
    }
    f = find_fpl(id);
    if (!f) { psp_wait_writeback(tmo_ptr, deadline);
              psp_ret(SCE_KERNEL_ERROR_WAIT_DELETE); return; }
    if (rc == PSP_SCHED_WOKEN) {
        psp_wait_writeback(tmo_ptr, deadline);
        psp_ret(SCE_KERNEL_ERROR_OK);
        return;
    }
    psp_waitq_drop(&f->q, me);
    psp_wait_writeback(tmo_ptr, deadline);
    psp_ret(SCE_KERNEL_ERROR_WAIT_TIMEOUT);
}

static void hle_AllocateFpl(void)    { fpl_allocate(1, 1); }
static void hle_TryAllocateFpl(void) { fpl_allocate(0, 0); }

static void hle_FreeFpl(void) {
    const uint32_t id  = psp_arg(0);
    const uint32_t ptr = psp_arg(1);
    psp_fpl *f = find_fpl(id);
    if (!f) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_FPLID); return; }

    /* The same two-way split the vpl has: a pointer that is not mapped memory
     * at all is ILLEGAL_SIZE, while one that is real but is not the start of a
     * live block of *this* pool is ILLEGAL_MEMBLOCK. */
    if (ptr && !psp_mem_ptr(ptr, 1)) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_SIZE); return; }
    if (ptr >= f->base && ptr < f->base + f->nblocks * f->stride) {
        const uint32_t i = (ptr - f->base) / f->stride;
        if (f->base + i * f->stride == ptr && fpl_is_taken(f, i)) {
            f->freelist[(f->head + f->free_blocks) % f->nblocks] = (uint16_t)i;
            f->free_blocks++;
            const int urgent = fpl_release(f);
            psp_ret(SCE_KERNEL_ERROR_OK);
            if (urgent) psp_sched_yield();
            return;
        }
    }
    psp_ret(SCE_KERNEL_ERROR_ILLEGAL_MEMBLOCK_PTR);
}

static void hle_CancelFpl(void) {
    psp_fpl *f = find_fpl(psp_arg(0));
    if (!f) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_FPLID); return; }
    const uint32_t out = psp_arg(1);
    if (out) psp_write32(out, (uint32_t)psp_waitq_count(&f->q));
    const int urgent = psp_waitq_cancel_all(&f->q);
    psp_ret(SCE_KERNEL_ERROR_OK);
    if (urgent) psp_sched_yield();
}

static void hle_ReferFplStatus(void) {
    const psp_fpl *f = find_fpl(psp_arg(0));
    const uint32_t info = psp_arg(1);
    if (!f)    { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_FPLID); return; }
    if (!info) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
    if (psp_read32(info) == 0) { psp_ret(SCE_KERNEL_ERROR_OK); return; }
    psp_write32(info +  0, 56);
    psp_threadman_write_name(info + 4, f->name);
    psp_write32(info + 36, f->attr);
    psp_write32(info + 40, f->block_size);
    psp_write32(info + 44, f->nblocks);
    psp_write32(info + 48, f->free_blocks);
    psp_write32(info + 52, (uint32_t)psp_waitq_count(&f->q));
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void fpl_list(int type, uint32_t out, int max, int *count) {
    if (type != PSP_TMID_FPL) return;
    for (int i = 0; i < MAX_FPLS; i++) {
        if (!g_fpl[i].alive) continue;
        if (out && *count < max) psp_write32(out + (uint32_t)*count * 4, g_fpl[i].uid);
        (*count)++;
    }
}

static void hle_AllocateFplCB(void) { psp_threadman_cb_begin(); hle_AllocateFpl(); psp_threadman_cb_end(); }

void psp_kernobj_register_fpl(void) {
    psp_threadman_add_lister(fpl_list);
    psp_hle_register(0xC07BB470, "ThreadManForUser", "sceKernelCreateFpl",      hle_CreateFpl);
    psp_hle_register(0xED1410E0, "ThreadManForUser", "sceKernelDeleteFpl",      hle_DeleteFpl);
    psp_hle_register(0xD979E9BF, "ThreadManForUser", "sceKernelAllocateFpl",    hle_AllocateFpl);
    psp_hle_register(0xE7282CB6, "ThreadManForUser", "sceKernelAllocateFplCB",  hle_AllocateFplCB);
    psp_hle_register(0x623AE665, "ThreadManForUser", "sceKernelTryAllocateFpl", hle_TryAllocateFpl);
    psp_hle_register(0xF6414A71, "ThreadManForUser", "sceKernelFreeFpl",        hle_FreeFpl);
    psp_hle_register(0xA8AA591F, "ThreadManForUser", "sceKernelCancelFpl",      hle_CancelFpl);
    psp_hle_register(0xD8199E4C, "ThreadManForUser", "sceKernelReferFplStatus", hle_ReferFplStatus);
}

/* ---- tlspl: a block pool indexed by thread --------------------------------
 *
 * A fixed pool again, but the caller never names a block: `sceKernelGetTlsAddr`
 * hands the calling thread *its* block, allocating one the first time and
 * returning the same address on every later call. threads/tls/get measures both
 * halves -- `Twice: OK (+0000)` with `freeBlocks` unchanged.
 *
 * A tlspl also carries an **index**, assigned at creation and reported in its
 * status, which is what a thread control block would key on. The second pool
 * created reports `index=1`.
 *
 * Its attribute mask is 0x41FF, which is the first one in seven object types
 * that agrees with another -- fpl's. Checked rather than assumed: this was the
 * seventh capture read and the first that could have been guessed.
 */

/* Sixteen, because that is how many hardware has: tls/create makes them in a
 * loop and the seventeenth fails. The index a pool reports is its slot here,
 * so the cap is observable twice over -- the last success reports index 15. */
#define MAX_TLSPLS 16
#define TLSPL_ATTR_KNOWN 0x41FFu

typedef struct {
    uint32_t  uid;
    char      name[32];
    uint32_t  attr, index;
    uint32_t  base, block_size, nblocks;
    /* What one block costs in the pool, which is not what it holds: the
     * option struct's alignment rounds it up. Reported size stays block_size. */
    uint32_t  stride;
    uint32_t  cursor;                  /* where the next search starts */
    /* Thread uid holding each block, 0 free. One entry per block and the
     * count is the guest's: tls/create asks for 0x1000 blocks of 0x100 bytes
     * and hardware allocates the megabyte, so what bounds a pool is the memory
     * it needs and not a bound of ours. */
    uint32_t *owner;
    int       alive;
    psp_waitq q;
} psp_tlspl;

static psp_tlspl g_tls[MAX_TLSPLS];

static void tls_reset(void) {
    for (int i = 0; i < MAX_TLSPLS; i++) {
        if (g_tls[i].alive && g_tls[i].base) psp_sysmem_release(g_tls[i].base);
        free(g_tls[i].owner);
    }
    memset(g_tls, 0, sizeof g_tls);
}

static psp_tlspl *find_tls(uint32_t id) {
    for (int i = 0; i < MAX_TLSPLS; i++)
        if (g_tls[i].alive && g_tls[i].uid == id) return &g_tls[i];
    return NULL;
}

static uint32_t tls_free_blocks(const psp_tlspl *t) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < t->nblocks; i++) if (!t->owner[i]) n++;
    return n;
}

static void hle_CreateTlspl(void) {
    /* (name, partition, attr, blockSize, count, option) */
    const uint32_t name  = psp_arg(0);
    const int32_t  part  = (int32_t)psp_arg(1);
    const uint32_t attr  = psp_arg(2);
    const uint32_t bsize = psp_arg(3);
    const uint32_t count = psp_arg(4);

    if (!name) { psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }
    const uint32_t pe = tlspl_partition_error(part);
    if (pe) { psp_ret(pe); return; }
    if (attr & ~TLSPL_ATTR_KNOWN) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_ATTR); return; }
    if (bsize == 0 || (int32_t)bsize < 0) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_MEMSIZE); return; }
    if (count == 0 || (int32_t)count < 0) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_MEMSIZE); return; }

    /* The option block's second word is an alignment, and it rounds the block
     * up rather than merely placing the pool. tls/get creates one-byte blocks
     * three times over, with alignment 0x100, 1 and 0, and reads the spacing
     * back as 0x100, 4 and 4 -- so anything below four is four, which is also
     * what a pool created without options gets. */
    const uint32_t opt = psp_arg(5);
    uint32_t align = 4;
    if (opt && psp_mem_ptr(opt, 8)) {
        const uint32_t a = psp_read32(opt + 4);
        /* And it must be a power of two, refused with the *partition* code --
         * which is what an fpl does with the same field, and the one thing
         * about either that could not have been guessed. */
        if (a && (a & (a - 1))) {
            psp_ret(SCE_KERNEL_ERROR_ILLEGAL_PARTITION);
            return;
        }
        if (a > align) align = a;
    }
    const uint32_t stride = (bsize + align - 1) & ~(align - 1);
    /* A pool whose size does not fit in a word is an illegal *size*, where one
     * that merely does not fit in memory is out of memory: 0x1000 blocks of
     * 0x100 is a megabyte and succeeds, 0x10000 of them answers NO_MEMORY, and
     * 0x1000000 -- which is exactly 2^32 bytes -- answers ILLEGAL_MEMSIZE. */
    if (count > 0xFFFFFFFFu / stride) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_MEMSIZE); return; }

    const uint32_t base = psp_sysmem_alloc(stride * count, 0);
    if (!base) { psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }

    psp_tlspl *t = NULL;
    /* The index is a *slot*, not a count of what is alive. tls/get creates two
     * pools (indices 0 and 1), deletes the first and creates a third: hardware
     * gives it index 0, reusing the slot the deleted one vacated. Counting the
     * survivors would have said 1. */
    uint32_t index = 0;
    for (;; index++) {
        int taken = 0;
        for (int i = 0; i < MAX_TLSPLS; i++)
            if (g_tls[i].alive && g_tls[i].index == index) { taken = 1; break; }
        if (!taken) break;
    }
    for (int i = 0; i < MAX_TLSPLS; i++) if (!g_tls[i].alive) { t = &g_tls[i]; break; }
    if (!t) { psp_sysmem_release(base); psp_ret(SCE_KERNEL_ERROR_TLSPL_FULL); return; }

    uint32_t *owner = (uint32_t *)calloc(count, sizeof *owner);
    if (!owner) { psp_sysmem_release(base); psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }

    memset(t, 0, sizeof *t);
    t->owner = owner;
    psp_str(name, t->name, sizeof t->name);
    t->attr = attr; t->index = index;
    t->base = base; t->block_size = bsize; t->nblocks = count;
    t->stride = stride;
    t->uid   = psp_threadman_next_uid();
    t->alive = 1;
    psp_ret(t->uid);
}

static uint32_t tls_me(void);

static void hle_DeleteTlspl(void) {
    psp_tlspl *t = find_tls(psp_arg(0));
    if (!t) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_TLSPLID); return; }
    /* A block still out with another thread refuses the delete. Waiters do
     * not: tls/delete deletes a one-block pool with two threads queued behind
     * it and gets OK, then fails on a two- and a three-block pool where the
     * extra blocks went to threads that had not given them back. So what the
     * kernel counts is blocks lent, and the caller's own does not count. */
    const uint32_t me = tls_me();
    for (uint32_t i = 0; i < t->nblocks; i++)
        if (t->owner[i] && t->owner[i] != me) {
            psp_ret(SCE_KERNEL_ERROR_TLSPL_BUSY);
            return;
        }
    const int urgent = psp_waitq_release_all(&t->q);
    if (t->base) psp_sysmem_release(t->base);
    free(t->owner);
    t->owner = NULL;
    t->alive = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
    if (urgent) psp_sched_yield();
}

/* The calling thread's block, allocated on first ask and kept thereafter. */
/* The uid a block is recorded against. The main context's is 0, which is also
 * "free", so it gets a sentinel. */
static uint32_t tls_me(void) {
    const uint32_t me = psp_sched_current();
    return me ? me : 0xFFFFFFFFu;
}

static uint32_t tls_block_of(const psp_tlspl *t, uint32_t owner) {
    for (uint32_t i = 0; i < t->nblocks; i++)
        if (t->owner[i] == owner) return t->base + i * t->stride;
    return 0;
}

/* sceKernelGetTlsAddr(uid)
 *
 * A pool with no free block does not answer NULL -- it *waits*. tls/priority
 * makes a pool of one block, takes it from the main thread, starts three
 * threads that ask for one, and reads the pool back: `freeBlocks=00000000,
 * wait=2`. Two of the three are parked in this call. Returning NULL instead
 * gave three threads an immediate answer and a queue that was never used.
 *
 * The release order is the usual attribute bit, and the same test proves it is
 * read: with 0x100 set, threads of priority 0x30, 0x34 and 0x31 come back in
 * the order 1, 3, 2. */
static void hle_GetTlsAddr(void) {
    psp_tlspl *t = find_tls(psp_arg(0));
    if (!t) { psp_ret(0); return; }
    const uint32_t id = t->uid, me = tls_me();

    const uint32_t mine = tls_block_of(t, me);
    if (mine) { psp_ret(mine); return; }
    /* The six lines threads/tls/get still differs by are here, and are left
     * alone on purpose: hardware does not validate this uid at all. It indexes
     * the object table by `uid >> 3`, so with two pools alive at slots 0 and 1,
     * uid 0 and uid 1 both answer the first pool's base and 0xF answers the
     * second's, while 0x10 and above fail. sceKernelReferTlsplStatus on the
     * same uids answers 800201D0, so the laxity is this one call's.
     *
     * Reproducing it needs uids that encode their slot in a shared object
     * table, which is a change to every object type at once. A fallback that
     * tries `index == uid >> 3` when the exact match misses would fit this
     * test and is not the mechanism -- it would be dead code the moment uids
     * did encode a slot. Worth doing for a better reason than six lines. */

    /* The search starts where the last one stopped rather than at block zero,
     * so a block that has just been freed is not the one handed straight back.
     * tls/get takes and frees a block from a pool of three, four times over,
     * and reads the offsets: +0000, +0010, +0020, +0000. First-free would have
     * answered +0000 every time. */
    for (uint32_t n = 0; n < t->nblocks; n++) {
        const uint32_t i = (t->cursor + n) % t->nblocks;
        if (t->owner[i]) continue;
        t->owner[i] = me;
        t->cursor = (i + 1) % t->nblocks;
        const uint32_t at = t->base + i * t->stride;
        /* Handed out clean. tls/get scribbles 0xCC over a block, frees it and
         * takes it again: the read comes back zero. Asking a *second* time
         * without freeing gives the same pointer untouched, still 0xCC -- and
         * that is the already-ours path above, which is why the clearing
         * belongs here rather than at the top of the call. */
        void *p = psp_mem_ptr(at, t->stride);
        if (p) memset(p, 0, t->stride);
        psp_ret(at);
        return;
    }

    if (!psp_sched_can_wait()) { psp_ret(0); return; }
    if (psp_waitq_add(&t->q, me, 0, 0, 0) != 0) { psp_ret(0); return; }
    const int rc = psp_sched_block_until(me, PSP_SCHED_BLOCKED,
                                         "sceKernelGetTlsAddr", 0);
    t = find_tls(id);
    if (!t) { psp_ret(0); return; }              /* deleted under us */
    if (rc != PSP_SCHED_WOKEN) { psp_waitq_drop(&t->q, me); psp_ret(0); return; }
    /* Whoever released us assigned the block on our behalf. */
    psp_ret(tls_block_of(t, me));
}

/* A thread's blocks go back to the pool when the thread ends. That is what
 * makes the storage thread-local, and tls/priority measures it without ever
 * calling free successfully: its workers take a block, delay, and then free it
 * with sceKernelFreeFpl -- the wrong call for the type, which fails with
 * 8002019d every time. The block still reaches the next waiter, so what
 * returned it was the worker exiting. */
void psp_kernobj_thread_ended(uint32_t uid) {
    for (int i = 0; i < MAX_TLSPLS; i++) {
        psp_tlspl *t = &g_tls[i];
        if (!t->alive) continue;
        for (uint32_t b = 0; b < t->nblocks; b++) {
            if (t->owner[b] != uid) continue;
            t->owner[b] = 0;
            const int wi = psp_waitq_pick(&t->q, t->attr);
            if (wi < 0) continue;
            const psp_waiter w = psp_waitq_take(&t->q, wi);
            t->owner[b] = w.uid;
            (void)psp_sched_wake(w.uid);
        }
    }
}

static void hle_FreeTlspl(void) {
    psp_tlspl *t = find_tls(psp_arg(0));
    if (!t) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_TLSPLID); return; }
    const uint32_t me = tls_me();
    for (uint32_t i = 0; i < t->nblocks; i++)
        if (t->owner[i] == me) {
            t->owner[i] = 0;
            /* Wiped on the way out as well as on the way in. tls/free writes
             * 0xCC over its block, frees it and reads zero back without
             * asking for it again -- so the free did it, not the next
             * allocation. */
            void *p = psp_mem_ptr(t->base + i * t->stride, t->stride);
            if (p) memset(p, 0, t->stride);
            /* The block goes straight to the next thread waiting for one,
             * rather than being left free for whoever asks next: the queue's
             * order is the point of having one. */
            int urgent = 0;
            const int wi = psp_waitq_pick(&t->q, t->attr);
            if (wi >= 0) {
                const psp_waiter w = psp_waitq_take(&t->q, wi);
                t->owner[i] = w.uid;
                urgent = psp_sched_wake(w.uid);
            }
            psp_ret(SCE_KERNEL_ERROR_OK);
            if (urgent) psp_sched_yield();
            return;
        }
    /* Freeing when the caller holds nothing is not an error. tls/free calls it
     * twice in a row and gets OK both times, and calls it from a thread that
     * has never asked while another thread holds the pool's only block -- also
     * OK, with freeBlocks still zero, so it did not take anyone else's. The
     * uid is checked; what the caller owns is not. */
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_ReferTlsplStatus(void) {
    const psp_tlspl *t = find_tls(psp_arg(0));
    const uint32_t info = psp_arg(1);
    if (!t)    { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_TLSPLID); return; }
    if (!info) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
    if (psp_read32(info) == 0) { psp_ret(SCE_KERNEL_ERROR_OK); return; }
    psp_write32(info +  0, 60);
    psp_threadman_write_name(info + 4, t->name);
    psp_write32(info + 36, t->attr);
    psp_write32(info + 40, t->index);
    psp_write32(info + 44, t->block_size);
    psp_write32(info + 48, t->nblocks);
    psp_write32(info + 52, tls_free_blocks(t));
    psp_write32(info + 56, (uint32_t)psp_waitq_count(&t->q));
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void tls_list(int type, uint32_t out, int max, int *count) {
    if (type != PSP_TMID_TLSPL) return;
    for (int i = 0; i < MAX_TLSPLS; i++) {
        if (!g_tls[i].alive) continue;
        if (out && *count < max) psp_write32(out + (uint32_t)*count * 4, g_tls[i].uid);
        (*count)++;
    }
}

void psp_kernobj_register_tls(void) {
    psp_threadman_add_lister(tls_list);
    psp_hle_register(0x8DAFF657, "ThreadManForUser", "sceKernelCreateTlspl",      hle_CreateTlspl);
    psp_hle_register(0x32BF938E, "ThreadManForUser", "sceKernelDeleteTlspl",      hle_DeleteTlspl);
    psp_hle_register(0xFA835CDE, "ThreadManForUser", "sceKernelGetTlsAddr",       hle_GetTlsAddr);
    psp_hle_register(0x4A719FB2, "ThreadManForUser", "sceKernelFreeTlspl",        hle_FreeTlspl);
    psp_hle_register(0x721067F3, "ThreadManForUser", "sceKernelReferTlsplStatus", hle_ReferTlsplStatus);
}
