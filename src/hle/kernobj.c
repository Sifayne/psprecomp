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

void psp_kernobj_reset(void) {
    for (int i = 0; i < MAX_VPLS; i++)
        if (g_vpl[i].used && g_vpl[i].base) psp_sysmem_release(g_vpl[i].base);
    memset(g_vpl, 0, sizeof g_vpl);
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
}
