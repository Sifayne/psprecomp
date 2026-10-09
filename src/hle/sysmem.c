/* psprecomp — SysMemUserForUser.
 *
 * The PSP's user-memory allocator. A game asks for a block by size and
 * placement policy, gets back a UID, and turns that UID into an address with
 * sceKernelGetBlockHeadAddr. Nothing runs before this works: it is the first
 * thing almost every module does after start-up.
 *
 * Placement matters and is not a detail. A game that asks for PSP_SMEM_High
 * expects its block at the *top* of user memory, and engines rely on that to
 * keep long-lived allocations away from a low-end scratch heap. Treating every
 * request as "any free block" appears to work and then fragments in a way that
 * only shows up hours in.
 */

#include "psprecomp/hle.h"
#include "psprecomp/state.h"

#include <stdio.h>
#include <string.h>

/* Placement policies, as passed in the `type` argument. */
#define PSP_SMEM_Low          0
#define PSP_SMEM_High         1
#define PSP_SMEM_Addr         2
#define PSP_SMEM_LowAligned   3
#define PSP_SMEM_HighAligned  4

/* A block per allocation, and the ceiling is reached by tests rather than by
 * games: msgpipe/create makes 1024 pipes in a loop, each with a buffer, and
 * expects every one to succeed. 512 ran out at 507. */
#define MAX_BLOCKS 2048
#define UID_BASE   0x00010000u

typedef struct {
    uint32_t uid;
    uint32_t addr;
    uint32_t size;
    char     name[32];
    int      used;
} mem_block;

static mem_block g_block[MAX_BLOCKS];
static uint32_t  g_next_uid;
static uint32_t  g_heap_lo, g_heap_hi;
static uint32_t  g_sdk_version;   /* see hle_SetCompiledSdkVersion */

uint32_t psp_sysmem_compiled_sdk(void) { return g_sdk_version; }

/* Default user heap. Modules load at 0x08800000 and are a few megabytes, so
 * starting above them keeps the allocator from handing out memory the module
 * itself occupies. A host that knows its real layout should narrow this. */
#define DEFAULT_HEAP_LO 0x08C00000u
#define DEFAULT_HEAP_HI (PSP_RAM_BASE + PSP_RAM_SIZE)

/* User memory starts here; below it belongs to the kernel. */
#define PSP_USER_BASE   0x08800000u

void psp_sysmem_reset(void) {
    memset(g_block, 0, sizeof g_block);
    g_next_uid = UID_BASE;
    g_heap_lo = DEFAULT_HEAP_LO;
    g_heap_hi = DEFAULT_HEAP_HI;
    g_sdk_version = 0;
}

void psp_sysmem_init(void) { psp_sysmem_reset(); }

void psp_sysmem_reserve_module(uint32_t lo, uint32_t hi) {
    g_heap_hi = DEFAULT_HEAP_HI;

    if (hi <= PSP_USER_BASE || lo >= DEFAULT_HEAP_HI) {
        /* The module is nowhere near user RAM. A PRX linked at address 0 --
         * which is what every pspautotests binary is -- lands outside the
         * partition entirely, so there is nothing here to step around.
         *
         * Hardware's loader still puts it in the partition, though, and where
         * the *rest* of user memory begins is therefore observable. Three tls
         * tests print a block address, and each one says the same thing: the
         * bottom of the partition holds the module's memory image rounded to a
         * granule, plus 0x4000. delete.prx and priority.prx have images of
         * 0x31700 and answer 0x09D35700; free.prx's is 0x319C0 and it answers
         * 0x09D35A00. The 16K is the loader's own bookkeeping, and it is the
         * same 16K in all three.
         *
         * What must *not* happen is rounding this up to something comfortable.
         * The old floor here was a flat megabyte and that megabyte was the
         * difference: gum.prx asks for a single 0x01500000 block, the heap was
         * 0x01400000 wide, and the allocation failed. The guest does not check
         * -- it formats into the null it got back, over its own code at address
         * zero -- so it presented as a wild pointer rather than as an
         * out-of-memory, which is a long way from the cause. */
        const uint32_t image = hi > lo ? ((hi - lo) + 0xFFu) & ~0xFFu : 0;
        g_heap_lo = PSP_USER_BASE + image + 0x4000u;
        return;
    }

    /* The module *is* in user RAM. Start above it, rounded up, but never below
     * the old default: the game's measurements are taken against that floor and
     * moving it would shift every address the guest allocates for no gain
     * here. */
    const uint32_t end = (hi + 0xFFFFu) & ~0xFFFFu;
    g_heap_lo = end > DEFAULT_HEAP_LO ? end : DEFAULT_HEAP_LO;
}

static mem_block *find_uid(uint32_t uid) {
    for (int i = 0; i < MAX_BLOCKS; i++)
        if (g_block[i].used && g_block[i].uid == uid) return &g_block[i];
    return NULL;
}

static mem_block *alloc_slot(void) {
    for (int i = 0; i < MAX_BLOCKS; i++)
        if (!g_block[i].used) return &g_block[i];
    return NULL;
}

/* Does [addr, addr+size) overlap anything already handed out? */
static int overlaps(uint32_t addr, uint32_t size) {
    for (int i = 0; i < MAX_BLOCKS; i++) {
        if (!g_block[i].used) continue;
        uint32_t b = g_block[i].addr, e = b + g_block[i].size;
        if (addr < e && b < addr + size) return 1;
    }
    return 0;
}

/* First fit walking up from the bottom of the heap. Blocks are few (a few
 * hundred at most) so a linear probe per candidate is fine and keeps the
 * bookkeeping to one array. */
static uint32_t place_low(uint32_t size, uint32_t align) {
    uint32_t a = (g_heap_lo + align - 1) & ~(align - 1);
    while (a + size <= g_heap_hi) {
        if (!overlaps(a, size)) return a;
        /* Skip past whatever is in the way rather than stepping by `align`,
         * which on a full heap would be O(heap/align) probes per allocation. */
        uint32_t next = g_heap_hi;
        for (int i = 0; i < MAX_BLOCKS; i++) {
            if (!g_block[i].used) continue;
            uint32_t b = g_block[i].addr, e = b + g_block[i].size;
            if (a < e && b < a + size && e < next) next = e;
        }
        a = (next + align - 1) & ~(align - 1);
    }
    return 0;
}

/* First fit walking down from the top. */
static uint32_t place_high(uint32_t size, uint32_t align) {
    if (size > g_heap_hi - g_heap_lo) return 0;
    uint32_t a = (g_heap_hi - size) & ~(align - 1);
    while (a >= g_heap_lo) {
        if (!overlaps(a, size)) return a;
        uint32_t prev = g_heap_lo;
        for (int i = 0; i < MAX_BLOCKS; i++) {
            if (!g_block[i].used) continue;
            uint32_t b = g_block[i].addr, e = b + g_block[i].size;
            if (a < e && b < a + size && b > prev) prev = b;
        }
        if (prev <= g_heap_lo || prev < size) return 0;
        a = (prev - size) & ~(align - 1);
    }
    return 0;
}

uint32_t psp_sysmem_free(void) {
    uint32_t used = 0;
    for (int i = 0; i < MAX_BLOCKS; i++)
        if (g_block[i].used) used += g_block[i].size;
    return (g_heap_hi - g_heap_lo) - used;
}

uint32_t psp_sysmem_alloc(uint32_t size, int from_high) {
    /* A request big enough that rounding it up wraps has to be refused here,
     * before the arithmetic loses it. 0xFFFFFFFF + 0xFF truncates to 0xFE,
     * masks to 0, and then the zero-size guard below turns it into a *256-byte*
     * allocation that succeeds -- so a caller asking for four gigabytes got a
     * handle and no error. msgpipe/create asks for exactly that and expects
     * `Failed (80020190)`. */
    if (size > 0xFFFFFF00u) return 0;
    uint32_t rounded = (size + 0xFF) & ~0xFFu;
    if (!rounded) rounded = 0x100;

    uint32_t addr = from_high ? place_high(rounded, 0x100) : place_low(rounded, 0x100);
    if (!addr) return 0;

    mem_block *b = alloc_slot();
    if (!b) return 0;
    b->uid = g_next_uid++;
    b->addr = addr;
    b->size = rounded;
    fprintf(stderr, "AllocPartition -> uid 0x%X at 0x%08X size %u (ends 0x%08X)\n",
            b->uid, b->addr, b->size, b->addr + b->size);
    b->used = 1;
    snprintf(b->name, sizeof b->name, "internal");
    return addr;
}

void psp_sysmem_release(uint32_t addr) {
    for (int i = 0; i < MAX_BLOCKS; i++)
        if (g_block[i].used && g_block[i].addr == addr) { g_block[i].used = 0; return; }
}

/* ---- the calls ----------------------------------------------------------- */

/* The allocator's own error codes, all measured by threadprobe steps 143-147
 * (fw 6.60). The numbers are in PSPSDK's pspkerror.h (UNKNOWN_UID 800200CB,
 * ILLEGAL_PARTITION 800200D6, ILLEGAL_MEMBLOCKTYPE 800200D8,
 * MEMBLOCK_ALLOC_FAILED 800200D9; 800200E4 is not listed there). They are
 * named here by what they mean to this allocator instead: hle.h spells
 * ILLEGAL_PARTITION and UNKNOWN_UID with other numbers (see its note on
 * misnamed codes), and a second #define of a name silently wins. A partition
 * out of range is hle.h's SCE_KERNEL_ERROR_ILLEGAL_PARTITION, 800200D2. */
#define SYSMEM_ERR_NOT_USER_PART 0x800200D6u  /* a partition, not the caller's */
#define SYSMEM_ERR_BAD_TYPE      0x800200D8u
#define SYSMEM_ERR_NO_ROOM       0x800200D9u
#define SYSMEM_ERR_BAD_ALIGN     0x800200E4u
#define SYSMEM_ERR_NOT_A_BLOCK   0x800200CBu

/* threadprobe step 146 (fw 6.60): 2 and 6 are user partitions; 1, 3, 4 and 8
 * exist but are not the caller's; 0, 7 and -1 are out of range. Step 170: 5
 * gives a block and 9 is not the caller's. This refused 5, following the vpl
 * table in kernobj.c. Where a partition-5 block lies on a PSP is unmeasured;
 * here it comes from the same heap as 2 and 6. */
static uint32_t partition_error(int32_t part) {
    switch (part) {
        case 2: case 5: case 6:              return 0;
        case 1: case 3: case 4:
        case 8: case 9:                      return SYSMEM_ERR_NOT_USER_PART;
        default:                             return SCE_KERNEL_ERROR_ILLEGAL_PARTITION;
    }
}

static void hle_AllocPartitionMemory(void) {

    /* (partitionid, name, type, size, addr) */
    int32_t  part     = (int32_t)psp_arg(0);
    uint32_t name_ptr = psp_arg(1);
    uint32_t type     = psp_arg(2);
    uint32_t size     = psp_arg(3);
    uint32_t want     = psp_arg(4);

    /* threadprobe step 150: a NULL name is 80020001. */
    if (!name_ptr) { psp_ret(SCE_KERNEL_ERROR_ERROR); return; }
    const uint32_t pe = partition_error(part);
    if (pe) { psp_ret(pe); return; }
    /* Step 143: a type outside Low..HighAligned is ILLEGAL_MEMBLOCKTYPE (5 and
     * -1), and size 0 is refused rather than rounded up to a granule. */
    if (type > PSP_SMEM_HighAligned) { psp_ret(SYSMEM_ERR_BAD_TYPE); return; }
    if (size == 0) { psp_ret(SYSMEM_ERR_NO_ROOM); return; }

    /* The hardware allocator works in 256-byte granules. Rounding up matters:
     * a game that allocates 100 bytes and then writes 256 is relying on it. */
    uint32_t rounded = (size + 0xFF) & ~0xFFu;
    if (rounded == 0) { psp_ret(SYSMEM_ERR_NO_ROOM); return; }

    uint32_t align = 0x100;
    if (type == PSP_SMEM_LowAligned || type == PSP_SMEM_HighAligned) {
        /* Step 144: 0x1000 and 0x10000 are honoured; 3 and 0 are refused. */
        align = want;
        if (!align || (align & (align - 1))) { psp_ret(SYSMEM_ERR_BAD_ALIGN); return; }
        if (align < 0x100) align = 0x100;
    }

    uint32_t addr = 0;
    switch (type) {
    case PSP_SMEM_Low:
    case PSP_SMEM_LowAligned:  addr = place_low(rounded, align);  break;
    case PSP_SMEM_High:
    case PSP_SMEM_HighAligned: addr = place_high(rounded, align); break;
    case PSP_SMEM_Addr:
        /* Step 145: asked for 0x80 into a free granule, the block starts at
         * the granule. Step 169 (fw 6.60): and it runs to the end of the
         * granule holding want + size -- 0x1000 bytes asked for at +0x80
         * took 0x1100 from the total free. This kept the rounded size, 0x1000,
         * which left the last 0x80 bytes the caller asked for outside it. */
        rounded = ((want + size + 0xFFu) & ~0xFFu) - (want & ~0xFFu);
        want &= ~0xFFu;
        if (want >= g_heap_lo && want + rounded <= g_heap_hi && !overlaps(want, rounded))
            addr = want;
        break;
    }

    /* Step 147: more than the largest free run is MEMBLOCK_ALLOC_FAILED. */
    if (!addr) { psp_ret(SYSMEM_ERR_NO_ROOM); return; }

    mem_block *b = alloc_slot();
    if (!b) { psp_ret(SYSMEM_ERR_NO_ROOM); return; }

    b->uid = g_next_uid++;
    b->addr = addr;
    b->size = rounded;
    fprintf(stderr, "AllocPartition -> uid 0x%X at 0x%08X size %u (ends 0x%08X)\n",
            b->uid, b->addr, b->size, b->addr + b->size);
    b->used = 1;
    psp_str(name_ptr, b->name, sizeof b->name);

    psp_ret(b->uid);
}

static void hle_FreePartitionMemory(void) {
    mem_block *b = find_uid(psp_arg(0));
    /* Step 147: freeing a block twice is 800200CB (PSPSDK's UNKNOWN_UID). */
    if (!b) { psp_ret(SYSMEM_ERR_NOT_A_BLOCK); return; }
    b->used = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_GetBlockHeadAddr(void) {
    mem_block *b = find_uid(psp_arg(0));
    fprintf(stderr, "GetBlockHeadAddr(uid=0x%X) -> 0x%08X (size %u, ends 0x%08X)\n",
            psp_arg(0), b ? b->addr : 0, b ? b->size : 0,
            b ? b->addr + b->size : 0);
    psp_ret(b ? b->addr : 0);
}

/* Version-reporting calls. The firmware records the SDK version, and
 * sceKernelGetCompiledSdkVersion reads it back: 0 before any Set, then the
 * value set (saveprobe step 114, fw 6.60: 0, then 06060010 after
 * sceKernelSetCompiledSdkVersion660(0x06060010)). Only that variant and a
 * valid value are measured; the other variants are taken to record theirs
 * the same way, and no value is checked. The compiler version is not read
 * back by anything. */
static void hle_SetCompiledSdkVersion(void) { g_sdk_version = psp_arg(0); psp_ret(SCE_KERNEL_ERROR_OK); }
static void hle_GetCompiledSdkVersion(void) { psp_ret(g_sdk_version); }
static void hle_SetCompilerVersion(void)    { psp_ret(SCE_KERNEL_ERROR_OK); }

/* sceKernelPrintf is a game's own debug output, which makes it one of the most
 * valuable things to have working during bring-up -- it is the game telling you
 * what it thinks it is doing. Formatting is done here rather than passed to the
 * host printf because the arguments live in guest registers and, for %s, guest
 * memory. */
static void hle_Printf(void) {
    char fmt[256];
    psp_str(psp_arg(0), fmt, sizeof fmt);

    int argi = 1;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { fputc(*p, stderr); continue; }
        p++;
        if (!*p) break;
        /* Skip flags/width/precision; we do not reproduce padding. */
        while (*p && strchr("-+ #0123456789.lhz", *p)) p++;
        switch (*p) {
        case 'd': case 'i': fprintf(stderr, "%d", (int32_t)psp_arg(argi++)); break;
        case 'u':           fprintf(stderr, "%u", psp_arg(argi++)); break;
        case 'x':           fprintf(stderr, "%x", psp_arg(argi++)); break;
        case 'X':           fprintf(stderr, "%X", psp_arg(argi++)); break;
        case 'p':           fprintf(stderr, "0x%08X", psp_arg(argi++)); break;
        case 'c':           fputc((int)psp_arg(argi++), stderr); break;
        case 's': {
            char s[256];
            psp_str(psp_arg(argi++), s, sizeof s);
            fputs(s, stderr);
            break;
        }
        case '%': fputc('%', stderr); break;
        default:  fputc('%', stderr); fputc(*p, stderr); break;
        }
    }
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* How much of the user partition is left.
 *
 * Unimplemented until now, which meant it returned zero -- and a *pair* of
 * zeros is a delta of zero, so a test measuring what a call consumed measured
 * nothing. threads/tls/create prints `(allocated N bytes)` around every one of
 * its creates and read 0 for all of them.
 *
 * The two differ only in fragmentation: total is everything free, max is the
 * largest single run. They used to be one number here, on the argument that
 * allocating from both ends of one span leaves a single run; threadprobe step
 * 141 (fw 6.60) reads total > max, and a freed stack or a block between two
 * live ones is a hole here as well. Step 147 allocates exactly max. */
static uint32_t largest_free_run(void) {
    uint32_t best = 0, at = g_heap_lo;
    while (at < g_heap_hi) {
        /* The first block at or above `at`, and the end of any that covers it. */
        uint32_t next = g_heap_hi, cover = 0;
        for (int i = 0; i < MAX_BLOCKS; i++) {
            if (!g_block[i].used) continue;
            const uint32_t b = g_block[i].addr, e = b + g_block[i].size;
            if (b <= at && e > at) { if (e > cover) cover = e; }
            else if (b > at && b < next) next = b;
        }
        if (cover) { at = cover; continue; }
        if (next - at > best) best = next - at;
        at = next;
    }
    return best;
}

static void hle_TotalFreeMemSize(void) { psp_ret(psp_sysmem_free()); }
static void hle_MaxFreeMemSize(void)   { psp_ret(largest_free_run()); }

/* The firmware version as 0xMMmmrr10: a PSP on 6.60 answers 06060010 (the
 * header line of every hwprobe log, 2026-09-28). psprecomp stands in for
 * that firmware. Unregistered, it returned 0. */
static void hle_DevkitVersion(void) { psp_ret(0x06060010u); }

void psp_sysmem_register(void) {
    PSP_STATE_KEEP(g_block);            /* a save state's (psprecomp/state.h) */
    PSP_STATE_KEEP(g_next_uid);
    PSP_STATE_KEEP(g_heap_lo);
    PSP_STATE_KEEP(g_heap_hi);
    PSP_STATE_KEEP(g_sdk_version);
    /* NIDs are SHA-1(name)[0:4] little-endian; tests/test_hle.c verifies every
     * pair below, so a mistyped NID cannot survive. */
    psp_hle_register(0x237DBD4F, "SysMemUserForUser", "sceKernelAllocPartitionMemory", hle_AllocPartitionMemory);
    psp_hle_register(0xB6D61D02, "SysMemUserForUser", "sceKernelFreePartitionMemory",  hle_FreePartitionMemory);
    psp_hle_register(0x9D9A5BA1, "SysMemUserForUser", "sceKernelGetBlockHeadAddr",     hle_GetBlockHeadAddr);
    psp_hle_register(0x7591C7DB, "SysMemUserForUser", "sceKernelSetCompiledSdkVersion",hle_SetCompiledSdkVersion);
    /* ULUS10567 (The 3rd Birthday) imports this ID and supplies 0x06030010.
     * Its GE probe observes that it updates the SDK getter and the callback
     * ABI. Unnamed for the same reason as the two below. */
    psp_hle_register_unnamed(0x1B4217BC, "SysMemUserForUser", hle_SetCompiledSdkVersion);
    /* A firmware-specific variant of the call above: same effect, different
     * NID per SDK generation, and a module imports exactly the one matching
     * what it was built against. This module's ~PSP header reports devkit
     * 0x05050010, and this is the NID it imports.
     *
     * Registered *unnamed* on purpose, and the reason is checkable rather than
     * a matter of taste. uofw's sysmem exports label this NID
     * `sceKernelSetCompiledSdkVersion500_550`, and SHA-1 of that string is
     * 0xA503C960, not 0x91DE343C -- so whatever that label is, it is not the
     * exported symbol, and the real name is not known. Registering it as named
     * would put a false name in the diagnostics and fail the SHA-1 check in
     * test_hle.c, which is right to reject it. */
    psp_hle_register_unnamed(0x91DE343C, "SysMemUserForUser", hle_SetCompiledSdkVersion);
    /* The 6.60 variant, unnamed for the same reason: uofw's label
     * `sceKernelSetCompiledSdkVersion660` hashes to 0x6E69DB0E. */
    psp_hle_register_unnamed(0x358CA1BB, "SysMemUserForUser", hle_SetCompiledSdkVersion);
    /* The 3.70 generation's, unnamed for the same reason: UCUS98712 (WipEout
     * Pulse) imports this ID and supplies 0x03070010. */
    psp_hle_register_unnamed(0x342061E5, "SysMemUserForUser", hle_SetCompiledSdkVersion);
    psp_hle_register(0xFC114573, "SysMemUserForUser", "sceKernelGetCompiledSdkVersion",hle_GetCompiledSdkVersion);
    psp_hle_register(0xF77D77CB, "SysMemUserForUser", "sceKernelSetCompilerVersion",   hle_SetCompilerVersion);
    psp_hle_register(0x13A5ABEF, "SysMemUserForUser", "sceKernelPrintf",               hle_Printf);
    psp_hle_register(0xF919F628, "SysMemUserForUser", "sceKernelTotalFreeMemSize",     hle_TotalFreeMemSize);
    psp_hle_register(0xA291F107, "SysMemUserForUser", "sceKernelMaxFreeMemSize",       hle_MaxFreeMemSize);
    psp_hle_register(0x3FC9AE6A, "SysMemUserForUser", "sceKernelDevkitVersion",        hle_DevkitVersion);
}
