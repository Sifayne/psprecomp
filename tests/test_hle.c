/* HLE tests — the firmware layer, with no game data involved.
 *
 * The headline check is the first one: **every registered NID is verified
 * against the SHA-1 of its own declared name.** A PSP NID is defined as the
 * first four bytes of SHA-1(name), so this is not a convention we are choosing
 * to follow — it is the identity the hardware uses, and it makes the whole
 * table self-verifying. A mistyped NID or a wrong function name cannot get
 * past this, which matters because such a mistake produces a game that runs
 * and misbehaves in a way indistinguishable from a codegen bug.
 */

#include "psprecomp/hle.h"
#include "psprecomp/dispatch.h"
#include "psprecomp/clock.h"
#include "psprecomp/sched.h"
#include "crypto/sha1.h"

#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

static int failures;

#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);        \
            printf(__VA_ARGS__);                               \
            printf("\n");                                      \
            failures++;                                        \
        }                                                      \
    } while (0)

/* Invoke a registered firmware call with o32 arguments. */
/* A NUL-terminated name in guest RAM.
 *
 * The create calls reject a null name pointer, as hardware does, so a test
 * that wants a semaphore has to supply one. Address chosen to sit in user RAM
 * clear of the stack the harness sets up. */
static uint32_t guest_name(const char *s) {
    const uint32_t at = 0x08804000u;
    for (uint32_t i = 0; ; i++) {
        psp_write8(at + i, (uint8_t)s[i]);
        if (!s[i]) break;
    }
    return at;
}

static uint32_t call(uint32_t nid, uint32_t a0, uint32_t a1, uint32_t a2,
                     uint32_t a3) {
    psp_cpu.r[PSP_REG_A0] = a0;
    psp_cpu.r[PSP_REG_A1] = a1;
    psp_cpu.r[PSP_REG_A2] = a2;
    psp_cpu.r[PSP_REG_A3] = a3;
    psp_hle_call(nid);
    return psp_cpu.r[PSP_REG_V0];
}

/* The fifth argument goes in $t0, not at $sp+16.
 *
 * This wrote it to the stack, which is plain o32 and not what a PSP firmware
 * stub does -- they load $t0-$t3 and branch, and hle.h carries the disassembly
 * and the bug that established it. So every call5 here was putting the argument
 * somewhere psp_arg(4) does not read, and it went unnoticed because the only
 * value ever passed was 0 and $t0 happens to start at 0 too. The first caller
 * to pass something else found it immediately. */
static uint32_t call5(uint32_t nid, uint32_t a0, uint32_t a1, uint32_t a2,
                      uint32_t a3, uint32_t a4) {
    psp_cpu.r[PSP_REG_T0] = a4;
    return call(nid, a0, a1, a2, a3);
}

static uint32_t call7(uint32_t nid, uint32_t a0, uint32_t a1, uint32_t a2,
                      uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6) {
    psp_cpu.r[PSP_REG_T1] = a5;
    psp_cpu.r[PSP_REG_T2] = a6;
    return call5(nid, a0, a1, a2, a3, a4);
}

static void test_sha1_vectors(void) {
    /* FIPS 180-4 examples, so the hash itself is trusted before anything is
     * built on it. */
    uint8_t d[20];
    char hex[41];

    sha1("abc", 3, d);
    for (int i = 0; i < 20; i++) sprintf(hex + i * 2, "%02x", d[i]);
    CHECK(strcmp(hex, "a9993e364706816aba3e25717850c26c9cd0d89d") == 0,
          "SHA-1(\"abc\"): got %s", hex);

    sha1("", 0, d);
    for (int i = 0; i < 20; i++) sprintf(hex + i * 2, "%02x", d[i]);
    CHECK(strcmp(hex, "da39a3ee5e6b4b0d3255bfef95601890afd80709") == 0,
          "SHA-1(\"\"): got %s", hex);

    sha1("The quick brown fox jumps over the lazy dog", 43, d);
    for (int i = 0; i < 20; i++) sprintf(hex + i * 2, "%02x", d[i]);
    CHECK(strcmp(hex, "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12") == 0,
          "SHA-1(\"The quick brown fox...\"): got %s", hex);

    /* 56 bytes is the boundary case: the 0x80 terminator fits in the first
     * block but the 8-byte length does not, so padding spills into a second
     * block. An implementation that gets this wrong passes every shorter
     * vector. */
    const char *m = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    CHECK(strlen(m) == 56, "the two-block padding case is 56 bytes");
    sha1(m, 56, d);
    for (int i = 0; i < 20; i++) sprintf(hex + i * 2, "%02x", d[i]);
    CHECK(strcmp(hex, "c2db330f6083854c99d4b5bfb6e8f29f201be699") == 0,
          "SHA-1 of 56 'a's (two-block padding): got %s", hex);
}

/* THE important test: the registered table describes itself correctly. */
static void test_nids_match_names(void) {
    int n = 0;
    const psp_hle_entry *e = psp_hle_entries(&n);
    CHECK(n > 0, "something is registered");

    int checked = 0, unnamed = 0;
    for (int i = 0; i < n; i++) {
        /* A NID may be observed without being identified: the game calls it,
         * the NID is exact, and no plausible name hashes to it. Those are
         * registered unnamed rather than under an invented name, and are
         * skipped here by construction. Inventing a name to satisfy this check
         * would defeat the only thing that makes the table trustworthy. */
        if (!e[i].name) { unnamed++; continue; }        uint32_t want = psp_nid(e[i].name);
        if (want != e[i].nid) {
            printf("FAIL %s::%s\n  registered 0x%08X but SHA-1(name) gives 0x%08X\n",
                   e[i].lib, e[i].name, e[i].nid, want);
            failures++;
        }
        checked++;
    }
    printf("  verified %d NIDs against SHA-1 of their names\n", checked);
}

static void test_sysmem(void) {
    psp_sysmem_reset();

    const uint32_t NID_ALLOC = psp_nid("sceKernelAllocPartitionMemory");
    const uint32_t NID_FREE  = psp_nid("sceKernelFreePartitionMemory");
    const uint32_t NID_HEAD  = psp_nid("sceKernelGetBlockHeadAddr");

    uint32_t before = psp_sysmem_free();
    const uint32_t NM = guest_name("blk");

    /* Low placement. */
    uint32_t uid = call5(NID_ALLOC, 2, NM, 0 /*Low*/, 0x1000, 0);
    CHECK(uid >= 0x00010000u, "low alloc returns a UID, got 0x%08X", uid);
    uint32_t lo = call(NID_HEAD, uid, 0, 0, 0);
    CHECK(lo != 0, "block has an address");

    /* High placement must land above the low one -- games depend on the
     * distinction, so it is not enough that both merely succeed. */
    uint32_t uid2 = call5(NID_ALLOC, 2, NM, 1 /*High*/, 0x1000, 0);
    uint32_t hi = call(NID_HEAD, uid2, 0, 0, 0);
    CHECK(hi > lo, "high allocation sits above the low one (lo=0x%08X hi=0x%08X)", lo, hi);

    /* Blocks must not overlap. */
    CHECK(lo + 0x1000 <= hi, "blocks do not overlap");

    /* Sizes round up to the hardware's 256-byte granule. */
    uint32_t uid3 = call5(NID_ALLOC, 2, NM, 0, 100, 0);
    CHECK(uid3 >= 0x00010000u, "a 100-byte request succeeds");
    CHECK(psp_sysmem_free() % 0x100 == 0, "allocations are granule-rounded");

    /* Freeing returns the memory, and leaves a hole: total counts it, the
     * largest run does not (threadprobe step 141, fw 6.60: total > max). */
    uint32_t mid = psp_sysmem_free();
    const uint32_t TOTAL = psp_nid("sceKernelTotalFreeMemSize");
    const uint32_t MAX   = psp_nid("sceKernelMaxFreeMemSize");
    CHECK(call(NID_FREE, uid, 0, 0, 0) == 0, "free succeeds");
    CHECK(psp_sysmem_free() > mid, "freeing returns memory");
    CHECK(call(TOTAL, 0, 0, 0, 0) > call(MAX, 0, 0, 0, 0),
          "a hole below a live block: total %08X, max %08X",
          call(TOTAL, 0, 0, 0, 0), call(MAX, 0, 0, 0, 0));

    /* The allocator's own codes (threadprobe steps 143-147, fw 6.60). */
    CHECK(call(NID_FREE, 0xDEADBEEF, 0, 0, 0) == 0x800200CBu,
          "freeing an unknown UID is refused");
    CHECK(call(NID_FREE, uid, 0, 0, 0) == 0x800200CBu, "freeing twice is refused");
    CHECK(call(NID_HEAD, 0xDEADBEEF, 0, 0, 0) == 0,
          "an unknown UID has no address");
    CHECK(call5(NID_ALLOC, 2, 0, 0, 0x100, 0) == SCE_KERNEL_ERROR_ERROR, "NULL name");
    CHECK(call5(NID_ALLOC, 2, NM, 0, 0, 0) == 0x800200D9u, "size 0");
    CHECK(call5(NID_ALLOC, 2, NM, 5, 0x100, 0) == 0x800200D8u, "type 5");
    CHECK(call5(NID_ALLOC, 2, NM, 3, 0x100, 3) == 0x800200E4u, "alignment 3");
    CHECK(call5(NID_ALLOC, 2, NM, 3, 0x100, 0) == 0x800200E4u, "alignment 0");
    CHECK(call5(NID_ALLOC, 1, NM, 0, 0x100, 0) == 0x800200D6u, "partition 1");
    CHECK(call5(NID_ALLOC, 7, NM, 0, 0x100, 0) == 0x800200D2u, "partition 7");

    /* Addr placement starts at the granule the address falls in (step 145)
     * and runs to the end of the one holding want + size (step 169: 0x1000
     * at +0x80 took 0x1100). */
    const uint32_t free_before = call(TOTAL, 0, 0, 0, 0);
    const uint32_t at = call5(NID_ALLOC, 2, NM, 2 /*Addr*/, 0x100, lo + 0x80);
    CHECK((int32_t)at > 0 && call(NID_HEAD, at, 0, 0, 0) == lo,
          "Addr at +0x80 of a free granule: head %08X, expected %08X",
          call(NID_HEAD, at, 0, 0, 0), lo);
    CHECK(free_before - call(TOTAL, 0, 0, 0, 0) == 0x200,
          "Addr 0x100 at +0x80 took %X, expected 0x200",
          free_before - call(TOTAL, 0, 0, 0, 0));
    call(NID_FREE, at, 0, 0, 0);

    /* Partition 5 gives a block and 9 is not the caller's (step 170). */
    const uint32_t p5 = call5(NID_ALLOC, 5, NM, 0, 0x100, 0);
    CHECK((int32_t)p5 > 0, "partition 5: %08X", p5);
    call(NID_FREE, p5, 0, 0, 0);
    CHECK(call5(NID_ALLOC, 9, NM, 0, 0x100, 0) == 0x800200D6u, "partition 9");

    /* An impossible request fails rather than returning a bogus block. */
    uint32_t huge = call5(NID_ALLOC, 2, NM, 0, 0x7F000000u, 0);
    CHECK(huge == 0x800200D9u, "an oversized request fails, got 0x%08X", huge);

    call(NID_FREE, uid2, 0, 0, 0);
    call(NID_FREE, uid3, 0, 0, 0);
    CHECK(psp_sysmem_free() == before, "everything freed restores the heap");
    CHECK(call(MAX, 0, 0, 0, 0) == before, "and one run again");

    /* The SDK version reads back what was set, 0 before (saveprobe step
     * 114, fw 6.60; 0x358CA1BB is sceKernelSetCompiledSdkVersion660). */
    const uint32_t NID_GETSDK = psp_nid("sceKernelGetCompiledSdkVersion");
    CHECK(call(NID_GETSDK, 0, 0, 0, 0) == 0, "no SDK version before one is set");
    CHECK(call(0x358CA1BB, 0x06060010, 0, 0, 0) == 0, "the 6.60 variant sets it");
    CHECK(call(NID_GETSDK, 0, 0, 0, 0) == 0x06060010, "and it reads back");
    psp_sysmem_reset();
    CHECK(call(NID_GETSDK, 0, 0, 0, 0) == 0, "a reset clears it");
}

static void test_semaphores(void) {
    psp_threadman_reset();

    const uint32_t CREATE = psp_nid("sceKernelCreateSema");
    const uint32_t WAIT   = psp_nid("sceKernelWaitSema");
    const uint32_t SIGNAL = psp_nid("sceKernelSignalSema");
    const uint32_t DELETE = psp_nid("sceKernelDeleteSema");

    uint32_t sem = call5(CREATE, guest_name("sema"), 0, 2 /*init*/, 4 /*max*/, 0);
    CHECK(sem != 0, "semaphore created");

    CHECK(call(WAIT, sem, 1, 0, 0) == 0, "wait succeeds while the count allows");
    CHECK(call(WAIT, sem, 1, 0, 0) == 0, "and again, down to zero");
    /* The third argument is the timeout pointer. Only a caller that supplies
     * one may be told a timeout elapsed: fabricating one for a caller that
     * asked to wait indefinitely is something a game acts on -- Armored Core
     * deletes the thread it was waiting for and quits. */
    const uint32_t TMO = 0x08802000u;
    CHECK(call(WAIT, sem, 1, TMO, 0) == SCE_KERNEL_ERROR_WAIT_TIMEOUT,
          "a wait that would block reports a timeout when one was asked for");

    CHECK(call(SIGNAL, sem, 1, 0, 0) == 0, "signal succeeds");
    CHECK(call(WAIT, sem, 1, 0, 0) == 0, "and the signalled count is available");

    /* The count must saturate at max, not run away. */
    call(SIGNAL, sem, 100, 0, 0);
    CHECK(call(WAIT, sem, 4, 0, 0) == 0, "count saturates at max (4 available)");
    CHECK(call(WAIT, sem, 1, TMO, 0) == SCE_KERNEL_ERROR_WAIT_TIMEOUT,
          "and no more than max");

    CHECK(call(DELETE, sem, 0, 0, 0) == 0, "delete succeeds");
    /* The semaphore's own code, not the allocator's. A wrong-typed handle is
     * refused with the *asking* type's error -- semaphores/wait.expected
     * answers NULL, invalid and deleted alike with 0x80020199. */
    CHECK(call(WAIT, sem, 1, 0, 0) == SCE_KERNEL_ERROR_UNKNOWN_SEMID,
          "a deleted semaphore is gone");

    /* And the count is range-checked before anything waits. */
    const uint32_t s2 = call5(CREATE, guest_name("counts"), 0, 0, 2, 0);
    CHECK(call(WAIT, s2, 3, TMO, 0) == SCE_KERNEL_ERROR_ILLEGAL_COUNT,
          "waiting for more than the maximum is refused");
    CHECK(call(WAIT, s2, 0, TMO, 0) == SCE_KERNEL_ERROR_ILLEGAL_COUNT,
          "waiting for zero is refused");
    CHECK(call(WAIT, s2, 0xFFFFFFFFu, TMO, 0) == SCE_KERNEL_ERROR_ILLEGAL_COUNT,
          "waiting for a negative amount is refused");
}

/* The attribute rules, one object type at a time.
 *
 * These two disagree with each other, and the whole reason this test exists is
 * that one of them was measured and then applied to both. A semaphore takes the
 * low nine bits; an event flag refuses bit 0x100 and accepts bit 0x200, which is
 * PSP_EVENT_WAITMULTIPLE and entirely ordinary. Armored Core creates one with
 * exactly 0x200, was refused, and the run went to 0 GE lists and 123,606 bad
 * memory accesses.
 *
 * Both tables below are transcribed from the pspautotests hardware captures,
 * semaphores/create.expected and events/create/create.expected. */
static void test_create_attributes(void) {
    psp_threadman_reset();

    const uint32_t SEMA = psp_nid("sceKernelCreateSema");
    const uint32_t FLAG = psp_nid("sceKernelCreateEventFlag");
    const uint32_t nm   = guest_name("create");

    static const struct { uint32_t attr; int ok; } sema[] = {
        { 0x00001, 1 }, { 0x00100, 1 }, { 0x001FF, 1 },
        { 0x00200, 0 }, { 0x00400, 0 }, { 0x00800, 0 }, { 0x00900, 0 },
        { 0x01000, 0 }, { 0x02000, 0 }, { 0x04000, 0 }, { 0x08000, 0 },
        { 0x10000, 0 },
    };
    for (unsigned i = 0; i < sizeof sema / sizeof *sema; i++) {
        const uint32_t r = call5(SEMA, nm, sema[i].attr, 0, 2, 0);
        const int accepted = r != SCE_KERNEL_ERROR_ILLEGAL_ATTR;
        CHECK(accepted == sema[i].ok,
              "sema attr 0x%X: %s, hardware %s it (got 0x%08X)",
              sema[i].attr, accepted ? "accepted" : "refused",
              sema[i].ok ? "accepts" : "refuses", r);
    }

    static const struct { uint32_t attr; int ok; } flag[] = {
        { 0x0000, 1 }, { 0x0001, 1 }, { 0x0010, 1 },
        { 0x0100, 0 }, { 0x0122, 0 },
        { 0x0200, 1 }, { 0x0222, 1 },
        { 0x0300, 0 }, { 0x0900, 0 }, { 0x1200, 0 },
    };
    for (unsigned i = 0; i < sizeof flag / sizeof *flag; i++) {
        const uint32_t r = call(FLAG, nm, flag[i].attr, 0, 0);
        const int accepted = r != SCE_KERNEL_ERROR_ILLEGAL_ATTR;
        CHECK(accepted == flag[i].ok,
              "event flag attr 0x%X: %s, hardware %s it (got 0x%08X)",
              flag[i].attr, accepted ? "accepted" : "refused",
              flag[i].ok ? "accepts" : "refuses", r);
    }
}

static void test_event_flags(void) {
    psp_threadman_reset();

    const uint32_t CREATE = psp_nid("sceKernelCreateEventFlag");
    const uint32_t SET    = psp_nid("sceKernelSetEventFlag");
    const uint32_t CLEAR  = psp_nid("sceKernelClearEventFlag");
    const uint32_t WAIT   = psp_nid("sceKernelWaitEventFlag");

    uint32_t ef = call(CREATE, guest_name("evflag"), 0, 0x0000, 0);
    CHECK(ef != 0, "event flag created");

    call(SET, ef, 0x0005, 0, 0);

    /* A timeout pointer, because an unsatisfiable wait now *blocks*.
     *
     * It used to return a fabricated WAIT_TIMEOUT to everyone, so these checks
     * could pass NULL and still get an answer. They now have to say what a
     * caller on hardware says: with no timeout the wait is indefinite, and with
     * nothing else runnable that stops the run rather than inventing a result.
     * The timeout is the fifth argument -- $t0, see hle.h -- so call5. */
    const uint32_t TMO = 0x08802000u;
    psp_write32(TMO, 1000);

    /* WAITOR is satisfied by any bit; WAITAND needs all of them. */
    CHECK(call5(WAIT, ef, 0x0004, 0x01 /*OR*/, 0, TMO) == 0, "OR wait on a set bit");
    psp_write32(TMO, 1000);
    CHECK(call5(WAIT, ef, 0x0003, 0x00 /*AND*/, 0, TMO) == SCE_KERNEL_ERROR_WAIT_TIMEOUT,
          "AND wait fails when only some bits are set");
    CHECK(psp_read32(TMO) == 0, "and the timeout word is spent, got %u",
          psp_read32(TMO));
    psp_write32(TMO, 1000);
    CHECK(call5(WAIT, ef, 0x0005, 0x00 /*AND*/, 0, TMO) == 0,
          "AND wait succeeds when all bits are set");
    CHECK(psp_read32(TMO) == 1000,
          "an immediate success spends none of the timeout, got %u",
          psp_read32(TMO));

    /* clear takes a mask of bits to KEEP. Getting that backwards leaves a game
     * waiting on a flag that never clears, so it is pinned explicitly. */
    call(CLEAR, ef, ~0x0004u, 0, 0);
    psp_write32(TMO, 1000);
    CHECK(call5(WAIT, ef, 0x0004, 0x01, 0, TMO) == SCE_KERNEL_ERROR_WAIT_TIMEOUT,
          "the cleared bit is gone");
    psp_write32(TMO, 1000);
    CHECK(call5(WAIT, ef, 0x0001, 0x01, 0, TMO) == 0,
          "the kept bit survives");

    /* And the out-parameter: the pattern the wait woke on. */
    const uint32_t OUT = 0x08802010u;
    psp_write32(OUT, 0xDEADBEEF);
    psp_write32(TMO, 1000);
    CHECK(call5(WAIT, ef, 0x0001, 0x01, OUT, TMO) == 0, "wait with an out pointer");
    CHECK(psp_read32(OUT) == 0x0001,
          "the pattern is reported: got 0x%08X", psp_read32(OUT));
}

/* A recompiled thread entry: writes a marker so the test can prove it ran on
 * the stack the thread manager gave it. */
static uint32_t g_thread_sp;
static uint32_t g_thread_arg;
static uint32_t g_thread_argp, g_thread_argword;
static int      g_thread_ran;

static void fake_thread_entry(void) {
    g_thread_ran++;
    g_thread_sp   = psp_cpu.r[PSP_REG_SP];
    g_thread_arg  = psp_cpu.r[PSP_REG_A0];
    g_thread_argp = psp_cpu.r[PSP_REG_A1];
    g_thread_argword = g_thread_argp ? psp_read32(g_thread_argp) : 0;
    psp_cpu.r[PSP_REG_V0] = 0x1234;      /* exit status */
}

/* What a thread is started with: the length and the pointer cancel each other.
 *
 * threads/semaphores/semaphores measures both directions. A NULL pointer with a
 * non-zero length arrives as length 0, and a zero length with a real pointer
 * arrives as a NULL pointer -- neither is what passing them through gives.
 *
 * The third rule that file measures, that the block is copied onto the thread's
 * own stack, is checked at the end (threadprobe step 19 confirms it on fw 6.60). */
static void test_thread_argument_block(void) {
    psp_sysmem_reset();
    psp_threadman_reset();
    psp_dispatch_reset();

    const uint32_t CREATE  = psp_nid("sceKernelCreateThread");
    const uint32_t START   = psp_nid("sceKernelStartThread");
    const uint32_t WAITEND = psp_nid("sceKernelWaitThreadEnd");

    const uint32_t ENTRY = 0x08801000u;
    const uint32_t SRC   = 0x08802100u;
    psp_register(ENTRY, fake_thread_entry);

    struct { uint32_t len, argp, want_len; int want_argp; const char *what; } cases[] = {
        { 4, SRC, 4, 1, "a length and a pointer arrive as given" },
        { 2, 0,   0, 0, "a NULL pointer zeroes the length" },
        { 0, SRC, 0, 0, "a zero length nulls the pointer" },
    };

    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        psp_write32(SRC, 0x00004567);
        const uint32_t thid = call5(CREATE, guest_name("arg"), ENTRY, 32, 0x4000, 0);
        g_thread_ran = 0; g_thread_arg = 0xDEAD; g_thread_argp = 0xDEAD;

        CHECK(call(START, thid, cases[i].len, cases[i].argp, 0) == 0,
              "%s: start", cases[i].what);
        call(WAITEND, thid, 0, 0, 0);

        CHECK(g_thread_ran == 1, "%s: the thread ran", cases[i].what);
        CHECK(g_thread_arg == cases[i].want_len,
              "%s: arglen %u, expected %u",
              cases[i].what, g_thread_arg, cases[i].want_len);
        CHECK((g_thread_argp != 0) == cases[i].want_argp,
              "%s: argp 0x%08X, expected %s",
              cases[i].what, g_thread_argp, cases[i].want_argp ? "non-NULL" : "NULL");
    }

    /* The block is copied onto the thread's own stack and the thread is handed
     * the copy, so the pointer it sees is not the one the caller passed and the
     * bytes behind it are the same. Held back until now because it took the
     * game from 633 GE lists to 3; see the comment in threadman.c. */
    psp_write32(SRC, 0x00004567);
    const uint32_t cid = call5(CREATE, guest_name("copy"), ENTRY, 32, 0x4000, 0);
    g_thread_argp = 0xDEAD;
    CHECK(call(START, cid, 4, SRC, 0) == 0, "copy: start");
    call(WAITEND, cid, 0, 0, 0);
    CHECK(g_thread_argp != SRC && g_thread_argp != 0,
          "the thread is handed a copy, not the caller's pointer (0x%08X)",
          g_thread_argp);
    CHECK(psp_read32(g_thread_argp) == 0x00004567,
          "the copy holds the caller's bytes, got 0x%08X",
          psp_read32(g_thread_argp));
    /* $sp starts 0x40 below the copy, and the top 16 words of the stack are the
     * kernel's: uid, 0, base, 0 x 11, then two 0xFFFFFFFF words (threadprobe
     * steps 19-25, fw 6.60). The copy of 4 bytes sits at top - 0x110. */
    CHECK(g_thread_sp == g_thread_argp - 0x40,
          "$sp 0x%08X, expected 0x40 below the copy at 0x%08X",
          g_thread_sp, g_thread_argp);
    const uint32_t top = g_thread_argp + 0x110;
    CHECK(psp_read32(top - 0x40) == cid, "top-0x40 holds the uid");
    CHECK(psp_read32(top - 0x3C) == 0 && psp_read32(top - 0x0C) == 0,
          "the words between the kernel's are zero, got %08X %08X",
          psp_read32(top - 0x3C), psp_read32(top - 0x0C));
    CHECK(psp_read32(top - 8) == 0xFFFFFFFFu && psp_read32(top - 4) == 0xFFFFFFFFu,
          "the last two words are 0xFFFFFFFF");

    /* And the stack the thread ran on is 0xFF, not whatever was there. Read
     * below the thread's own $sp, which it never wrote. */
    CHECK(psp_read32(g_thread_sp - 64) == 0xFFFFFFFFu,
          "a fresh thread's stack is filled with 0xFF, got 0x%08X",
          psp_read32(g_thread_sp - 64));
}

static void test_threads(void) {
    psp_sysmem_reset();
    psp_threadman_reset();
    psp_dispatch_reset();

    const uint32_t CREATE = psp_nid("sceKernelCreateThread");
    const uint32_t START  = psp_nid("sceKernelStartThread");
    const uint32_t WAITEND= psp_nid("sceKernelWaitThreadEnd");
    const uint32_t DELETE = psp_nid("sceKernelDeleteThread");
    const uint32_t GETID  = psp_nid("sceKernelGetThreadId");

    const uint32_t ENTRY = 0x08801000u;
    psp_register(ENTRY, fake_thread_entry);

    /* A NULL name is refused (threadprobe step 150, fw 6.60). */
    CHECK(call5(CREATE, 0, ENTRY, 32, 0x4000, 0) == SCE_KERNEL_ERROR_ERROR,
          "a thread with a NULL name is refused");
    uint32_t thid = call5(CREATE, guest_name("t"), ENTRY, 32 /*prio*/, 0x4000 /*stack*/, 0);
    CHECK(thid != 0, "thread created, got 0x%08X", thid);

    /* The caller's context must survive the thread running. */
    psp_cpu.r[PSP_REG_S0] = 0xC0FFEE;
    uint32_t caller_sp = psp_cpu.r[PSP_REG_SP];

    g_thread_ran = 0;
    /* A real address: the argument block is copied out of it, so it has to be
     * memory that exists. This passed 0xAAAA, which the start now refuses. */
    uint32_t rc = call(START, thid, 7 /*arglen*/, 0x08802100u, 0);
    CHECK(rc == 0, "start succeeds");

    /* Starting a thread makes it runnable; it does not run it. The PSP is
     * single-core, and the caller keeps the CPU until it gives it up. */
    CHECK(g_thread_ran == 0, "start does not run the thread inline");

    /* Waiting for it is the caller giving the CPU up, so the thread runs here.
     * sceKernelWaitThreadEnd *returns* the exit status -- its second argument
     * is a timeout pointer, not somewhere to write the status. */
    CHECK(call(WAITEND, thid, 0, 0, 0) == 0x1234,
          "wait-for-end returns the thread's exit status");
    CHECK(g_thread_ran == 1, "the thread ran once the caller waited on it");
    CHECK(g_thread_arg == 7, "argument reached the thread, got %u", g_thread_arg);
    CHECK(g_thread_sp != caller_sp && g_thread_sp != 0,
          "the thread ran on its own stack (0x%08X vs caller 0x%08X)",
          g_thread_sp, caller_sp);
    CHECK((g_thread_sp & 15) == 0, "the thread stack pointer is 16-byte aligned");

    CHECK(psp_cpu.r[PSP_REG_S0] == 0xC0FFEE, "caller's registers restored");
    CHECK(psp_cpu.r[PSP_REG_SP] == caller_sp, "caller's stack pointer restored");


    /* Deleting frees the stack. */
    uint32_t before_delete = psp_sysmem_free();
    CHECK(call(DELETE, thid, 0, 0, 0) == 0, "delete succeeds");
    CHECK(psp_sysmem_free() > before_delete, "deleting a thread frees its stack");

    /* An id that names nothing is UNKNOWN_THID, not ILLEGAL_THID -- that one is
     * reserved for an id of zero. This asserted the wrong code, which is what
     * threads/start.expected keeps apart on consecutive lines:
     * `NULL: 80020197`, `Deleted: 80020198`, `Invalid: 80020198`. */
    CHECK(call(START, 0xDEADBEEF, 0, 0, 0) == SCE_KERNEL_ERROR_UNKNOWN_THID,
          "starting an unknown thread is refused");
    CHECK(call(START, 0, 0, 0, 0) == SCE_KERNEL_ERROR_ILLEGAL_THID,
          "starting thread id zero is refused differently");
    CHECK(call(GETID, 0, 0, 0, 0) == 0, "no current thread outside one");
}

/* Argument checks and small rules threadprobe measured on fw 6.60. */
static void test_thread_rules(void) {
    psp_sysmem_reset();
    psp_threadman_reset();
    psp_dispatch_reset();

    /* Dispatch suspend nests by returning the previous state (step 83). */
    const uint32_t SUSP = psp_nid("sceKernelSuspendDispatchThread");
    const uint32_t RES  = psp_nid("sceKernelResumeDispatchThread");
    const uint32_t d1 = call(SUSP, 0, 0, 0, 0);
    const uint32_t d2 = call(SUSP, 0, 0, 0, 0);
    CHECK(d1 == 1 && d2 == 0, "suspend dispatch twice: %08X %08X, expected 1 0", d1, d2);
    CHECK(call(psp_nid("sceKernelDelayThread"), 1000, 0, 0, 0) ==
          SCE_KERNEL_ERROR_CAN_NOT_WAIT, "a delay while dispatch is off");
    CHECK(call(RES, d2, 0, 0, 0) == 0, "resume(0)");
    CHECK(call(RES, d1, 0, 0, 0) == 0, "resume(1)");
    CHECK(call(SUSP, 0, 0, 0, 0) == 1, "dispatch is back on after resume(1)");
    call(RES, 1, 0, 0, 0);

    /* Any nonzero state turns it back on (step 97). */
    call(SUSP, 0, 0, 0, 0);
    CHECK(call(RES, 2, 0, 0, 0) == 0, "resume(2)");
    CHECK(call(SUSP, 0, 0, 0, 0) == 1, "dispatch is on after resume(2)");
    CHECK(call(RES, 0xFFFFFFFFu, 0, 0, 0) == 0, "resume(-1)");
    CHECK(call(SUSP, 0, 0, 0, 0) == 1, "dispatch is on after resume(-1)");
    call(RES, 1, 0, 0, 0);

    /* With interrupts off nothing waits and dispatch cannot be suspended
     * (steps 97, 98). */
    const uint32_t INTR_OFF = psp_nid("sceKernelCpuSuspendIntr");
    const uint32_t INTR_ON  = psp_nid("sceKernelCpuResumeIntr");
    const uint32_t cookie = call(INTR_OFF, 0, 0, 0, 0);
    CHECK(call(SUSP, 0, 0, 0, 0) == SCE_KERNEL_ERROR_CPUDI, "suspend dispatch, interrupts off");
    CHECK(call(psp_nid("sceKernelDelayThread"), 1000, 0, 0, 0) ==
          SCE_KERNEL_ERROR_CAN_NOT_WAIT, "a delay with interrupts off");
    call(INTR_ON, cookie, 0, 0, 0);
    CHECK(call(SUSP, 0, 0, 0, 0) == 1, "dispatch still on after the refused suspend");
    call(RES, 1, 0, 0, 0);

    /* CreateThread's argument checks (steps 2, 3, 5) and the stack rounding. */
    const uint32_t CREATE = psp_nid("sceKernelCreateThread");
    const uint32_t REFER  = psp_nid("sceKernelReferThreadStatus");
    const uint32_t ENTRY  = 0x08801000u;
    struct { uint32_t prio, size, attr, want; } bad[] = {
        { 0x00, 0x1000, 0, SCE_KERNEL_ERROR_ILLEGAL_PRIORITY },
        { 0x07, 0x1000, 0, SCE_KERNEL_ERROR_ILLEGAL_PRIORITY },
        { 0x78, 0x1000, 0, SCE_KERNEL_ERROR_ILLEGAL_PRIORITY },
        { 0xFFFFFFFFu, 0x1000, 0, SCE_KERNEL_ERROR_ILLEGAL_PRIORITY },
        { 0x20, 0x1FF,  0, SCE_KERNEL_ERROR_ILLEGAL_STACK_SIZE },
        { 0x20, 0,      0, SCE_KERNEL_ERROR_ILLEGAL_STACK_SIZE },
        { 0x20, 0xFFFFFFFFu, 0, SCE_KERNEL_ERROR_NO_MEMORY },
        { 0x20, 0x1000, 0x00000100u, SCE_KERNEL_ERROR_ILLEGAL_ATTR },
        { 0x20, 0x1000, 0x00008000u, SCE_KERNEL_ERROR_ILLEGAL_ATTR },
        { 0x20, 0x1000, 0x04000000u, SCE_KERNEL_ERROR_ILLEGAL_ATTR },
    };
    for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) {
        const uint32_t r = call5(CREATE, guest_name("bad"), ENTRY, bad[i].prio,
                                 bad[i].size, bad[i].attr);
        CHECK(r == bad[i].want, "create(prio %X, size %X, attr %X) = %08X, expected %08X",
              bad[i].prio, bad[i].size, bad[i].attr, r, bad[i].want);
    }
    /* Which check wins (step 9): a kernel entry, then attr, priority, stack
     * size, and the NULL name last. */
    struct { uint32_t name, entry, prio, size, attr, want; } order[] = {
        { 0, ENTRY, 0x00, 0x1000, 0, SCE_KERNEL_ERROR_ILLEGAL_PRIORITY },
        { 0, ENTRY, 0x20, 0x100,  0, SCE_KERNEL_ERROR_ILLEGAL_STACK_SIZE },
        { 0, ENTRY, 0x20, 0x1000, 0x100, SCE_KERNEL_ERROR_ILLEGAL_ATTR },
        { 1, ENTRY, 0x00, 0x100,  0, SCE_KERNEL_ERROR_ILLEGAL_PRIORITY },
        { 1, ENTRY, 0x00, 0x1000, 0x100, SCE_KERNEL_ERROR_ILLEGAL_ATTR },
        { 1, ENTRY, 0x20, 0x100,  0x100, SCE_KERNEL_ERROR_ILLEGAL_ATTR },
        { 1, 0x88000000u, 0x20, 0x1000, 0, 0x800200D3u },
        { 1, 0x88000000u, 0x00, 0x1000, 0, 0x800200D3u },
        { 0, 0x88000000u, 0x20, 0x1000, 0, 0x800200D3u },
    };
    for (unsigned i = 0; i < sizeof order / sizeof *order; i++) {
        const uint32_t r = call5(CREATE, order[i].name ? guest_name("bad") : 0,
                                 order[i].entry, order[i].prio, order[i].size, order[i].attr);
        CHECK(r == order[i].want, "create #%u = %08X, expected %08X", i, r, order[i].want);
    }

    const uint32_t ok = call5(CREATE, guest_name("ok"), ENTRY, 0x08, 0x201,
                              0x80804001u);
    CHECK((int32_t)ok > 0, "create(8, 0x201, 0x80804001) = %08X", ok);
    const uint32_t info = 0x08806000u;
    psp_write32(info, 104);
    CHECK(call(REFER, ok, info, 0, 0) == 0, "refer");
    CHECK(psp_read32(info + 52) == 0x300, "stack 0x201 rounds to 0x%X, expected 0x300",
          psp_read32(info + 52));
    CHECK(psp_read32(info + 36) == 0x800040FFu, "attr reported %08X, expected 800040FF",
          psp_read32(info + 36));

    /* WakeupThread: 0 is ILLEGAL_THID, a thread never started is DORMANT
     * (steps 29, 30). */
    const uint32_t WAKE = psp_nid("sceKernelWakeupThread");
    CHECK(call(WAKE, 0, 0, 0, 0) == SCE_KERNEL_ERROR_ILLEGAL_THID, "wakeup(0)");
    CHECK(call(WAKE, ok, 0, 0, 0) == SCE_KERNEL_ERROR_DORMANT, "wakeup(never started)");

    /* GetThreadmanIdType answers the id-list type (step 100); ReleaseWaitThread
     * refuses 0 and a thread that is not waiting (step 70). */
    const uint32_t IDTYPE = psp_nid("sceKernelGetThreadmanIdType");
    const uint32_t sema = call5(psp_nid("sceKernelCreateSema"), guest_name("s"), 0, 0, 1, 0);
    CHECK(call(IDTYPE, ok, 0, 0, 0) == 1, "type of a thread");
    CHECK(call(IDTYPE, sema, 0, 0, 0) == 2, "type of a semaphore");
    CHECK(call(IDTYPE, 0, 0, 0, 0) == 0x800200D2u, "type of 0");
    call(psp_nid("sceKernelDeleteSema"), sema, 0, 0, 0);
    CHECK(call(IDTYPE, sema, 0, 0, 0) == 0x800200D2u, "type of a deleted semaphore");
    const uint32_t RELEASE = psp_nid("sceKernelReleaseWaitThread");
    CHECK(call(RELEASE, 0, 0, 0, 0) == SCE_KERNEL_ERROR_ILLEGAL_THID, "release(0)");
    CHECK(call(RELEASE, ok, 0, 0, 0) == SCE_KERNEL_ERROR_NOT_WAIT, "release(dormant)");

    /* GetThreadStackFreeSize (step 85): a thread never started answers its
     * size less 0x10; an id that names nothing is UNKNOWN_THID. */
    const uint32_t FREE = psp_nid("sceKernelGetThreadStackFreeSize");
    CHECK(call(FREE, ok, 0, 0, 0) == 0x300 - 0x10, "free size of a created thread: %08X",
          call(FREE, ok, 0, 0, 0));
    CHECK(call(FREE, 0xDEADBEEFu, 0, 0, 0) == SCE_KERNEL_ERROR_UNKNOWN_THID,
          "free size of an unknown id");
}

static void test_guest_strings(void) {
    /* Names come out of guest memory, so the reader has to terminate and must
     * not run past the buffer. */
    const uint32_t at = 0x08803000u;
    const char *s = "sceThreadName";
    for (uint32_t i = 0; i <= strlen(s); i++) psp_write8(at + i, (uint8_t)s[i]);

    char buf[32];
    CHECK(strcmp(psp_str(at, buf, sizeof buf), s) == 0, "string round-trips");

    char small[6];
    psp_str(at, small, sizeof small);
    CHECK(strlen(small) == 5 && strncmp(small, s, 5) == 0,
          "an over-long string is truncated, not overflowed: \"%s\"", small);
}

/* The 27 networking imports fail instead of answering 0 (M6). A 0 here
 * would read as "multiplayer is up" and send the menu down a flow whose
 * wakeups never arrive -- the success-lie. Out-parameters stay untouched on
 * the failure paths, the Refer*Status rule. */
static void test_net(void) {
    static const char *const refused[] = {
        "sceNetAdhocInit",          "sceNetAdhocctlInit",
        "sceNetAdhocTerm",          "sceNetAdhocPdpCreate",
        "sceNetAdhocPdpSend",       "sceNetAdhocPdpRecv",
        "sceNetAdhocPdpDelete",     "sceNetAdhocPtpClose",
        "sceNetAdhocPtpSend",       "sceNetAdhocPtpOpen",
        "sceNetAdhocPtpRecv",       "sceNetAdhocPtpAccept",
        "sceNetAdhocPtpListen",     "sceNetAdhocPtpConnect",
        "sceNetAdhocPtpFlush",      "sceNetAdhocctlTerm",
        "sceNetAdhocctlAddHandler", "sceNetAdhocctlDelHandler",
        "sceNetAdhocctlDisconnect", "sceNetAdhocctlConnect",
        "sceNetAdhocctlGetState",   "sceNetAdhocctlGetPeerList",
        "sceNetGetLocalEtherAddr",
    };
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        uint32_t got = call(psp_nid(refused[i]), 0, 0, 0, 0);
        CHECK(got == SCE_KERNEL_ERROR_NOTIMPLEMENTED, "%s: got 0x%08X, want 0x%08X",
              refused[i], got, SCE_KERNEL_ERROR_NOTIMPLEMENTED);
    }

    /* sceNetInit manages a pool, not the radio: vacuous success. */
    CHECK(call(psp_nid("sceNetInit"), 65536, 30, 0x1000, 30) == 0,
          "net init succeeds");
    CHECK(call(psp_nid("sceNetTerm"), 0, 0, 0, 0) == 0, "net term succeeds");

    /* Failure writes nothing: seed guards around the two calls that take
     * out-parameters. */
    const uint32_t OUT = 0x08830000u;
    psp_write32(OUT, 0x13371337u);
    call(psp_nid("sceNetAdhocctlGetState"), OUT, 0, 0, 0);
    CHECK(psp_read32(OUT) == 0x13371337u, "getstate leaves its out-param alone");
    call(psp_nid("sceNetGetLocalEtherAddr"), OUT, 0, 0, 0);
    CHECK(psp_read32(OUT) == 0x13371337u, "getether leaves its out-param alone");

    /* The two value-returning calls. Ntostr is void: only the string is
     * checked, not $v0. An invalid pointer is skipped, not faulted. */
    const uint32_t MAC = 0x08830100u, BUF = 0x08830200u;
    call(psp_nid("sceNetEtherNtostr"), MAC, BUF, 0, 0);
    char got[32];
    CHECK(strcmp(psp_str(BUF, got, sizeof got), "00:00:00:00:00:00") == 0,
          "ntostr formats the zero MAC, got \"%s\"", got);
    call(psp_nid("sceNetEtherNtostr"), 0xDEADBEEFu, BUF, 0, 0);
    CHECK(strcmp(psp_str(BUF, got, sizeof got), "00:00:00:00:00:00") == 0,
          "ntostr with a bad source leaves the buffer");
    CHECK(call(psp_nid("sceWlanGetSwitchState"), 0, 0, 0, 0) == 0,
          "the WLAN switch reads off");
}

/* M4's directory calls, under a root of their own. A save flow makes
 * ms0:/PSP/SAVEDATA/<id> and enumerates it back; the tree starts empty, so
 * the parents have to come from somewhere. Ends with the root restored and
 * the scratch tree removed. */
static void test_io_dirs(void) {
    psp_io_set_root("test-io-root");
    const uint32_t MKDIR  = psp_nid("sceIoMkdir");
    const uint32_t RMDIR  = psp_nid("sceIoRmdir");
    const uint32_t REMOVE = psp_nid("sceIoRemove");
    const uint32_t CHDIR  = psp_nid("sceIoChdir");
    const uint32_t CHSTAT = psp_nid("sceIoChstat");
    const uint32_t OPEN   = psp_nid("sceIoOpen");
    const uint32_t CLOSE  = psp_nid("sceIoClose");
    const uint32_t WRITE  = psp_nid("sceIoWrite");
    const uint32_t READ   = psp_nid("sceIoRead");
    const uint32_t GETSTAT= psp_nid("sceIoGetstat");
    const uint32_t DOPEN  = psp_nid("sceIoDopen");
    const uint32_t DREAD  = psp_nid("sceIoDread");
    const uint32_t DCLOSE = psp_nid("sceIoDclose");

    /* Start clean no matter how the last run ended. */
    call(REMOVE, guest_name("ms0:/PSP/SAVEDATA/ZZZ/DATA.BIN"), 0, 0, 0);
    call(RMDIR, guest_name("ms0:/PSP/SAVEDATA/ZZZ"), 0, 0, 0);
    call(RMDIR, guest_name("ms0:/PSP/SAVEDATA"), 0, 0, 0);
    call(RMDIR, guest_name("ms0:/PSP"), 0, 0, 0);

    CHECK(call(MKDIR, guest_name("ms0:/PSP/SAVEDATA/ZZZ"), 0777, 0, 0) == 0,
          "mkdir makes the whole chain");
    CHECK(call(MKDIR, guest_name("ms0:/PSP/SAVEDATA/ZZZ"), 0777, 0, 0) == 0x80010011,
          "mkdir of an existing dir says EEXIST");
    CHECK(call(CHDIR, guest_name("ms0:/PSP"), 0, 0, 0) == 0, "chdir in");
    CHECK(call(CHDIR, guest_name("ms0:/NOPE"), 0, 0, 0) == 0x80010002,
          "chdir to a missing dir fails");
    CHECK(call(CHSTAT, guest_name("ms0:/PSP/SAVEDATA/ZZZ"), 0, 0, 0) == 0,
          "chstat of a dir succeeds");
    CHECK(call(CHSTAT, guest_name("ms0:/PSP/NOPE"), 0, 0, 0) == 0x80010002,
          "chstat of a missing path fails");

    /* A file round trip through a CREAT open, which makes parents too. */
    const uint32_t SRC = 0x08805000u, DST = 0x08806000u, DIR = 0x08807000u;
    for (uint32_t i = 0; i < 64; i++) psp_write8(SRC + i, (uint8_t)(i * 3 + 1));
    uint32_t fd = call(OPEN, guest_name("ms0:/PSP/SAVEDATA/ZZZ/DATA.BIN"),
                       0x602, 0777, 0);
    CHECK((int32_t)fd >= 3, "creat open hands a descriptor, got 0x%08X", fd);
    CHECK(call(WRITE, fd, SRC, 64, 0) == 64, "short write writes short");
    CHECK(call(CLOSE, fd, 0, 0, 0) == 0, "close succeeds");
    CHECK(call(GETSTAT, guest_name("ms0:/PSP/SAVEDATA/ZZZ/DATA.BIN"), DST, 0, 0) == 0,
          "stat succeeds");
    CHECK(psp_read32(DST + 8) == 64, "stat reports the size, got %u", psp_read32(DST + 8));
#ifndef _WIN32
    /* The Memory Stick stats the FAT way (saveprobe steps 105-106, fw 6.60):
     * modes 0x21FF and 0x11FF, attrs 0x20 and 0x10, a directory's size 0,
     * dates filled with the access date at 00:00:00, st_private untouched.
     * A directory does not open as a file (steps 103-104). */
    for (uint32_t i = 0; i < 88; i++) psp_write8(DST + i, 0xEE);
    CHECK(call(GETSTAT, guest_name("ms0:/PSP/SAVEDATA/ZZZ/DATA.BIN"), DST, 0, 0) == 0 &&
          psp_read32(DST) == 0x21FF && psp_read32(DST + 4) == 0x20 && psp_read32(DST + 8) == 64 &&
          psp_read16(DST + 16) >= 1980 && psp_read16(DST + 38) == 0 && psp_read32(DST + 60) == 0 &&
          psp_read32(DST + 64) == 0xEEEEEEEEu && psp_read32(DST + 84) == 0xEEEEEEEEu,
          "an ms0 file stats as FAT, mode 0x%08X", psp_read32(DST));
    CHECK(call(GETSTAT, guest_name("ms0:/PSP/SAVEDATA/ZZZ"), DST, 0, 0) == 0 &&
          psp_read32(DST) == 0x11FF && psp_read32(DST + 4) == 0x10 &&
          psp_read32(DST + 8) == 0 && psp_read32(DST + 12) == 0,
          "an ms0 directory stats as a directory of size 0, mode 0x%08X size high 0x%08X",
          psp_read32(DST), psp_read32(DST + 12));
    CHECK(call(OPEN, guest_name("ms0:/PSP/SAVEDATA/ZZZ"), 0x0001, 0, 0) == 0x8001000Du,
          "open of an ms0 directory is EACCES");
    CHECK(call(OPEN, guest_name("ms0:/PSP/SAVEDATA/ZZZ/"), 0x0001, 0, 0) == 0x80010016u,
          "open of an ms0 directory with a trailing slash is EINVAL");
#endif
    fd = call(OPEN, guest_name("ms0:/PSP/SAVEDATA/ZZZ/DATA.BIN"), 0x0001, 0, 0);
    CHECK((int32_t)fd >= 3, "reopen reads");
    for (uint32_t i = 0; i < 64; i++) psp_write8(DST + i, 0);
    CHECK(call(READ, fd, DST, 64, 0) == 64, "read back what was written");
    int same = 1;
    for (uint32_t i = 0; i < 64; i++) same &= psp_read8(DST + i) == (uint8_t)(i * 3 + 1);
    CHECK(same, "round trip is byte-exact");
    CHECK(call(CLOSE, fd, 0, 0, 0) == 0, "close succeeds");

    /* Enumeration finds the file by name. SceIoDirent is the 88-byte
     * SceIoStat and then d_name, so the name is at +88 and the size at +8. */
    uint32_t dd = call(DOPEN, guest_name("ms0:/PSP/SAVEDATA/ZZZ"), 0, 0, 0);
    CHECK((int32_t)dd >= 1, "dopen succeeds");
    int found = 0;
    uint32_t found_size = 0;
    while (call(DREAD, dd, DIR, 0, 0) == 1) {
        char name[260];
        psp_str(DIR + 88, name, sizeof name);
        if (!strcmp(name, "DATA.BIN")) { found = 1; found_size = psp_read32(DIR + 8); }
    }
    CHECK(found, "dread enumerates DATA.BIN");
    CHECK(found_size == 64, "dread reports the entry's size, got %u", found_size);
    CHECK(call(DCLOSE, dd, 0, 0, 0) == 0, "dclose succeeds");

    CHECK(call(REMOVE, guest_name("ms0:/PSP/SAVEDATA/ZZZ/DATA.BIN"), 0, 0, 0) == 0,
          "remove deletes");
    CHECK(call(REMOVE, guest_name("ms0:/PSP/SAVEDATA/ZZZ/DATA.BIN"), 0, 0, 0) == 0x80010002,
          "remove of a missing file fails");
    CHECK(call(RMDIR, guest_name("ms0:/PSP/SAVEDATA/ZZZ"), 0, 0, 0) == 0, "rmdir leaf");
    CHECK(call(RMDIR, guest_name("ms0:/PSP/SAVEDATA/ZZZ"), 0, 0, 0) == 0x80010002,
          "rmdir of a missing dir fails");
    CHECK(call(RMDIR, guest_name("ms0:/PSP/SAVEDATA"), 0, 0, 0) == 0, "rmdir middle");
    CHECK(call(RMDIR, guest_name("ms0:/PSP"), 0, 0, 0) == 0, "rmdir top");
    CHECK(call(RMDIR, guest_name("ms0:/"), 0, 0, 0) == 0, "rmdir ms itself");
#ifdef _WIN32
    _rmdir("test-io-root");
#else
    rmdir("test-io-root");
#endif
    psp_io_set_root(".");
}

/* Build a display list in guest memory and check the walk follows control flow
 * rather than merely counting words. The list deliberately contains a PRIM
 * that a JUMP skips over: if it gets counted, the walk is not following jumps. */
static void test_ge_display_list(void) {
    psp_ge_reset();

    const uint32_t LIST = 0x08820000u;
    uint32_t w[16], n = 0;

    /* GE addresses are 24-bit; BASE supplies the high byte. Setting it here
     * also exercises that path -- a list whose BASE is ignored jumps to the
     * wrong place and silently executes garbage. */
    w[n++] = (0x10u << 24) | 0x080000;              /* BASE  -> 0x08000000 */
    w[n++] = (0x12u << 24) | 0x000123;              /* VTYPE */
    w[n++] = (0x04u << 24) | (3u << 16) | 6;        /* PRIM triangles, 6 verts */
    w[n++] = (0x08u << 24) | 0x820020;              /* JUMP -> LIST + 0x20 */
    w[n++] = (0x04u << 24) | (0u << 16) | 99;       /* PRIM points -- SKIPPED */
    w[n++] = (0x00u << 24);
    w[n++] = (0x00u << 24);
    w[n++] = (0x00u << 24);
    /* word 8 == LIST + 0x20 */
    w[n++] = (0x04u << 24) | (6u << 16) | 2;        /* PRIM sprites, 2 verts */
    w[n++] = (0x0Fu << 24);                          /* FINISH */
    w[n++] = (0x0Cu << 24);                          /* END */

    for (uint32_t i = 0; i < n; i++) psp_write32(LIST + i * 4, w[i]);

    uint64_t before = psp_ge_command_count();
    /* sceGeListEnQueue(list, stall=0, cbid, arg), then DrawSync(WAIT): a
     * stall-less list with nothing ahead of it runs inside EnQueue, and the
     * sync finds nothing left to drain. */
    uint32_t qid = call(psp_nid("sceGeListEnQueue"), LIST, 0, 0, 0);
    CHECK(qid != 0, "list enqueued, got 0x%08X", qid);
    CHECK(call(psp_nid("sceGeDrawSync"), 0, 0, 0, 0) == 0, "draw sync drains");

    uint64_t executed = psp_ge_command_count() - before;
    CHECK(executed == 6, "walked BASE,VTYPE,PRIM,JUMP,PRIM,FINISH = 6, got %llu",
          (unsigned long long)executed);
    CHECK(psp_ge_vertex_count() == 8,
          "counted 6+2 vertices and skipped the jumped-over PRIM, got %llu",
          (unsigned long long)psp_ge_vertex_count());

    CHECK(call(psp_nid("sceGeDrawSync"), 0, 0, 0, 0) == 0, "draw sync succeeds");
    CHECK(call(psp_nid("sceGeEdramGetAddr"), 0, 0, 0, 0) == PSP_VRAM_BASE,
          "eDRAM is the VRAM window");
    CHECK(call(psp_nid("sceGeEdramGetSize"), 0, 0, 0, 0) == PSP_VRAM_SIZE,
          "eDRAM is 2 MB");
}

/* GU_SIGNAL_PAUSE is followed by a FINISH/END pair, and the list stops there
 * until sceGeContinue: geprobe 5 step 57 (fw 6.60) reads ListSync(peek) 4
 * and DrawSync(peek) 2 while it waits, and the rest of the list runs inside
 * sceGeContinue. */
static void test_ge_signal_pause(void) {
    psp_ge_reset();

    const uint32_t LIST = 0x0882C000u;
    psp_write32(LIST + 0x00, (0x0Eu << 24) | (0x03u << 16) | 1u); /* PAUSE */
    psp_write32(LIST + 0x04, (0x0Cu << 24));                      /* END */
    psp_write32(LIST + 0x08, (0x0Fu << 24));                      /* pause FINISH */
    psp_write32(LIST + 0x0C, (0x0Cu << 24));                      /* pause END */
    psp_write32(LIST + 0x10, (0x00u << 24));                      /* after resume */
    psp_write32(LIST + 0x14, (0x0Fu << 24));                      /* final FINISH */
    psp_write32(LIST + 0x18, (0x0Cu << 24));                      /* final END */

    uint64_t before = psp_ge_command_count();
    uint32_t qid = call(psp_nid("sceGeListEnQueue"), LIST, 0, 0, 0);
    CHECK(qid != 0, "signal PAUSE list enqueued, got 0x%08X", qid);
    CHECK(psp_ge_command_count() - before == 3, "stops at the pause's FINISH: %llu command(s)",
          (unsigned long long)(psp_ge_command_count() - before));
    uint32_t ls = call(psp_nid("sceGeListSync"), qid, 1, 0, 0);
    uint32_t ds = call(psp_nid("sceGeDrawSync"), 1, 0, 0, 0);
    CHECK(ls == 4 && ds == 2, "paused: ListSync(peek) 0x%08X DrawSync(peek) 0x%08X, hardware 4 and 2", ls, ds);
    CHECK(psp_ge_command_count() - before == 3, "still paused after two firmware calls");
    CHECK(call(psp_nid("sceGeContinue"), 0, 0, 0, 0) == 0, "sceGeContinue reads 0");
    CHECK(psp_ge_command_count() - before == 6,
          "the rest runs inside sceGeContinue: %llu command(s), want 6",
          (unsigned long long)(psp_ge_command_count() - before));
    CHECK(call(psp_nid("sceGeListSync"), qid, 1, 0, 0) == 0, "done after Continue");
    CHECK(call(psp_nid("sceGeDrawSync"), 0, 0, 0, 0) == 0, "signal PAUSE list drains");
}

/* GE callbacks, geprobe step 26 (fw 6.60): sceGeSetCallback with signal_arg
 * 0x5A and finish_arg 0xA5, then a raw list SIGNAL 0x44 (behaviour 1), SIGNAL
 * 0x55 (behaviour 2), FINISH 0x66 with no stall. All three handlers have run
 * when EnQueue returns, with (id, arg); ListSync(wait) and DrawSync(wait)
 * read 0, and ListSync(peek) after them 0x80000100. */
static uint32_t g_gecb[8][3];
static int g_gecb_n;
static void gecb_note(uint32_t kind) {
    if (g_gecb_n < 8) {
        g_gecb[g_gecb_n][0] = kind;
        g_gecb[g_gecb_n][1] = psp_cpu.r[PSP_REG_A0];
        g_gecb[g_gecb_n][2] = psp_cpu.r[PSP_REG_A1];
        g_gecb_n++;
    }
    psp_cpu.r[PSP_REG_A0] = 0xDEAD;     /* the caller's registers come back */
}
static void gecb_signal(void) { gecb_note(1); }
static void gecb_finish(void) { gecb_note(2); }

static void test_ge_callbacks(void) {
    psp_ge_reset();
    g_gecb_n = 0;
    psp_register(0x08A00000u, gecb_signal);
    psp_register(0x08A00100u, gecb_finish);
    const uint32_t CB = 0x08834000u, LIST = 0x08834100u;
    psp_write32(CB + 0, 0x08A00000u); psp_write32(CB + 4, 0x5A);
    psp_write32(CB + 8, 0x08A00100u); psp_write32(CB + 12, 0xA5);
    const uint32_t cbid = call(psp_nid("sceGeSetCallback"), CB, 0, 0, 0);
    CHECK((int32_t)cbid >= 0, "sceGeSetCallback gives an id, got 0x%08X", cbid);

    static const uint32_t w[] = { 0x0E010044, 0x0C000000, 0x0E020055, 0x0C000000,
                                  0x0F000066, 0x0C000000, 0 };
    for (uint32_t i = 0; i < 7; i++) psp_write32(LIST + i * 4, w[i]);
    const uint32_t qid = call(psp_nid("sceGeListEnQueue"), LIST, 0, cbid, 0);
    CHECK(g_gecb_n == 3 &&
          g_gecb[0][0] == 1 && g_gecb[0][1] == 0x44 && g_gecb[0][2] == 0x5A &&
          g_gecb[1][0] == 1 && g_gecb[1][1] == 0x55 && g_gecb[1][2] == 0x5A &&
          g_gecb[2][0] == 2 && g_gecb[2][1] == 0x66 && g_gecb[2][2] == 0xA5,
          "signal 44, signal 55, finish 66 inside EnQueue: %d call(s)", g_gecb_n);
    CHECK(psp_cpu.r[PSP_REG_A0] == LIST, "the handlers leave EnQueue's registers: a0 0x%08X",
          psp_cpu.r[PSP_REG_A0]);
    CHECK(call(psp_nid("sceGeListSync"), qid, 0, 0, 0) == 0, "ListSync(wait) on the done list reads 0");
    CHECK(call(psp_nid("sceGeDrawSync"), 0, 0, 0, 0) == 0, "DrawSync(wait) reads 0");
    const uint32_t after = call(psp_nid("sceGeListSync"), qid, 1, 0, 0);
    CHECK(after == 0x80000100u, "ListSync(peek) after DrawSync: 0x%08X, hardware 80000100", after);
    CHECK(call(psp_nid("sceGeUnsetCallback"), cbid, 0, 0, 0) == 0, "sceGeUnsetCallback reads 0");

    /* Once unset, the same list calls nobody. */
    g_gecb_n = 0;
    call(psp_nid("sceGeListEnQueue"), LIST, 0, cbid, 0);
    call(psp_nid("sceGeDrawSync"), 0, 0, 0, 0);
    CHECK(g_gecb_n == 0, "an unset callback is not called: %d call(s)", g_gecb_n);
}

/* A list that keeps the GE busy is still running when EnQueue returns:
 * geprobe 5 step 58 (fw 6.60) enqueues SIGNAL 0x01, 100 full-screen sprites,
 * SIGNAL 0x02, 100 more and FINISH 0x03 with no stall; only the first
 * handler has run when EnQueue returns, ListSync(peek) reads 2, and the rest
 * come later. Here 40 sprites (5.2 million pixels) do the same. */
static void test_ge_long_list(void) {
    psp_ge_reset();
    g_gecb_n = 0;
    psp_register(0x08A00000u, gecb_signal);
    psp_register(0x08A00100u, gecb_finish);
    const uint32_t CB = 0x08836000u, LIST = 0x08836100u, V = 0x08838000u;
    psp_write32(CB + 0, 0x08A00000u); psp_write32(CB + 4, 0x5A);
    psp_write32(CB + 8, 0x08A00100u); psp_write32(CB + 12, 0xA5);
    const uint32_t cbid = call(psp_nid("sceGeSetCallback"), CB, 0, 0, 0);
    /* Two corners of a full-screen sprite: 8888 colour, 16-bit position. */
    psp_write32(V + 0, 0xFF203040u); psp_write16(V + 4, 0);   psp_write16(V + 6, 0);   psp_write16(V + 8, 0);
    psp_write32(V + 12, 0xFF203040u); psp_write16(V + 16, 480); psp_write16(V + 18, 272); psp_write16(V + 20, 0);
    uint32_t n = 0;
#define W_(x) psp_write32(LIST + 4 * n++, (x))
    W_(0x10000000u | ((V >> 8) & 0xFF0000));            /* BASE */
    W_(0x9C000000u | 0x000000);                          /* FBP: VRAM start */
    W_(0x9D000000u | 0x040000 | 512);                    /* FBW, address bits 24-31 */
    W_(0x12000000u | (7u << 2) | (2u << 7) | (1u << 23)); /* VTYPE: 8888, s16, through */
    W_(0x0E020001u); W_(0x0C000000u);                    /* SIGNAL 0x01, END */
    for (int i = 0; i < 40; i++) {
        W_(0x01000000u | (V & 0xFFFFFF));                /* VADDR */
        W_(0x04000000u | (6u << 16) | 2);                /* PRIM sprites */
    }
    W_(0x0E020002u); W_(0x0C000000u);                    /* SIGNAL 0x02, END */
    W_(0x0F000003u); W_(0x0C000000u);                    /* FINISH 0x03, END */
#undef W_
    const uint32_t qid = call(psp_nid("sceGeListEnQueue"), LIST, 0, cbid, 0);
    CHECK(g_gecb_n == 1 && g_gecb[0][1] == 0x01, "only SIGNAL 0x01 inside EnQueue: %d handler(s)", g_gecb_n);
    const uint32_t peek = call(psp_nid("sceGeListSync"), qid, 1, 0, 0);
    CHECK(peek == 2, "ListSync(peek) as EnQueue returns: 0x%08X, hardware 2", peek);
    CHECK(call(psp_nid("sceGeDrawSync"), 1, 0, 0, 0) == 2, "DrawSync(peek) while it runs: 2");
    CHECK(call(psp_nid("sceGeDrawSync"), 0, 0, 0, 0) == 0, "DrawSync(wait) reads 0");
    CHECK(g_gecb_n == 3 && g_gecb[1][1] == 0x02 && g_gecb[2][0] == 2 && g_gecb[2][1] == 0x03,
          "SIGNAL 0x02 and FINISH 0x03 by the end of the wait: %d handler(s)", g_gecb_n);
    call(psp_nid("sceGeUnsetCallback"), cbid, 0, 0, 0);
}

/* A list that jumps to itself must terminate rather than hang the host -- this
 * is a normal transient state while the CPU is still writing the list. */
static void test_ge_infinite_list(void) {
    psp_ge_reset();
    const uint32_t LIST = 0x08830000u;
    psp_write32(LIST + 0, (0x10u << 24) | 0x080000);   /* BASE */
    psp_write32(LIST + 4, (0x08u << 24) | 0x830000);   /* JUMP to itself */

    uint32_t qid = call(psp_nid("sceGeListEnQueue"), LIST, 0, 0, 0);
    CHECK(qid != 0, "a self-jumping list still returns");
}

static void test_sas_adpcm(void) {
    psp_sas_reset();

    /* One VAG block: shift 8, filter 0, then 28 nibbles. With filter 0 there is
     * no prediction, so each output sample is just the sign-extended nibble
     * scaled -- which makes a decode bug obvious rather than merely quieter. */
    const uint32_t VAG = 0x08840000u;
    psp_write8(VAG + 0, 0x08);        /* shift 8, filter 0 */
    psp_write8(VAG + 1, 0x00);        /* flags: not the end */
    for (uint32_t i = 0; i < 14; i++)
        psp_write8(VAG + 2 + i, 0x7Fu);   /* nibbles 0xF and 0x7 */

    /* A real, 64-byte-aligned core: hardware refuses a null or unaligned one
     * with 80420005 (audio/sascore/sascore.expected, "NULL" and "Unaligned"),
     * and so does hle_Init now. */
    const uint32_t CORE = 0x08860000u;
    CHECK(call5(psp_nid("__sceSasInit"), CORE, 64 /*grain*/, 32, 0, 44100) == 0,
          "SAS init");

    /* sceSasSetVoice(core, voice, addr, size, loop) */
    CHECK(call5(psp_nid("__sceSasSetVoice"), 0, 0, VAG, 16, 0) == 0, "voice set");
    CHECK(call(psp_nid("__sceSasSetVolume"), 0, 0, 0x1000, 0x1000) == 0, "volume set");
    CHECK(call(psp_nid("__sceSasSetPitch"), 0, 0, 0x1000, 0) == 0, "pitch set");

    /* Before key-on nothing should be playing. */
    const uint32_t OUT = 0x08850000u;
    for (uint32_t i = 0; i < 64 * 4; i += 4) psp_write32(OUT + i, 0);
    call(psp_nid("__sceSasCore"), CORE, OUT, 0, 0);
    CHECK(psp_sas_nonzero() == 0, "silence before key-on");

    /* An envelope, explicitly. A voice with no attack rate stays at zero
     * height and is silent, on hardware as here -- this used to lean on a
     * default rate invented by the key-on, which adsrcurve's rate-0 sweeps
     * showed was not hardware's (findings item 45). */
    CHECK(call7(psp_nid("__sceSasSetADSR"), 0, 0, 15,
                0x40000000 / 64, 0x40000000 / 512, 0x30000000, 0x40000000 / 256) == 0,
          "adsr set");

    CHECK(call(psp_nid("__sceSasSetKeyOn"), 0, 0, 0, 0) == 0, "key on");
    call(psp_nid("__sceSasCore"), CORE, OUT, 0, 0);

    CHECK(psp_sas_frames() == 2, "two frames rendered, got %llu",
          (unsigned long long)psp_sas_frames());
    CHECK(psp_sas_nonzero() > 0, "audio was actually produced after key-on");

    /* The end flag is how a game knows a sound finished; a voice that never
     * reports ended is a common way for audio to stall after the first sound. */
    uint32_t ended = call(psp_nid("__sceSasGetEndFlag"), 0, 0, 0, 0);
    CHECK((ended & ~1u) != 0, "unused voices report ended, got 0x%08X", ended);
}

/* sasprobe's rendering setup: grain 256, one voice at full volume with no
 * sends, and a flat envelope -- full height from its second sample on. */
enum { SAS_CORE = 0x08860000u, SAS_OUT = 0x08850000u, SAS_DATA = 0x08870000u };

static void sas_flat_voice(uint32_t v) {
    call7(psp_nid("__sceSasSetVolume"), SAS_CORE, v, 0x1000, 0x1000, 0, 0, 0);
    call7(psp_nid("__sceSasSetADSRmode"), SAS_CORE, v, 0xF, 0, 1, 1, 1);
    call7(psp_nid("__sceSasSetADSR"), SAS_CORE, v, 0xF, 0x7FFFFFFF, 0, 0, 0);
    call(psp_nid("__sceSasSetSL"), SAS_CORE, v, 0x40000000u, 0);
}

static void sas_core(void) { call(psp_nid("__sceSasCore"), SAS_CORE, SAS_OUT, 0, 0); }
static int sas_left(uint32_t i) { return (int16_t)psp_read16(SAS_OUT + i * 4u); }

static void sas_fresh(void) {
    psp_sas_reset();
    CHECK(call5(psp_nid("__sceSasInit"), SAS_CORE, 256, 32, 0, 44100) == 0, "SAS init");
}

/* The rules sasprobe measured on firmware 6.60; each check names its step. */
static void test_sas_hardware_rules(void) {
    const uint32_t PCM = SAS_DATA;
    for (uint32_t i = 0; i < 64; i++) psp_write16(PCM + i * 2u, (uint16_t)(4 * (i + 1)));

    /* Every pitch interpolates, rounding up (step 181): at 0x800 a ramp of
     * 4, 8, 12 ... plays 0 10 8 14 12 18 from L[32]. */
    sas_fresh();
    call5(psp_nid("__sceSasSetVoicePCM"), SAS_CORE, 0, PCM, 64, 0xFFFFFFFFu);
    sas_flat_voice(0);
    call(psp_nid("__sceSasSetPitch"), SAS_CORE, 0, 0x800, 0);
    call(psp_nid("__sceSasSetKeyOn"), SAS_CORE, 0, 0, 0);
    sas_core();
    {
        static const int want[6] = { 0, 10, 8, 14, 12, 18 };
        for (uint32_t i = 0; i < 6; i++)
            CHECK(sas_left(32 + i) == want[i], "pitch 0x800: L[%u] = %d, want %d",
                  32 + i, sas_left(32 + i), want[i]);
    }

    /* A PCM voice's pitch stops at 0x1000 (step 182): 0x2000 plays the ramp
     * one sample per output. */
    sas_fresh();
    call5(psp_nid("__sceSasSetVoicePCM"), SAS_CORE, 0, PCM, 64, 0xFFFFFFFFu);
    sas_flat_voice(0);
    call(psp_nid("__sceSasSetPitch"), SAS_CORE, 0, 0x2000, 0);
    call(psp_nid("__sceSasSetKeyOn"), SAS_CORE, 0, 0, 0);
    sas_core();
    CHECK(sas_left(33) == 8 && sas_left(34) == 12 && sas_left(35) == 16,
          "PCM pitch 0x2000 plays as 0x1000: L[33..35] = %d %d %d, want 8 12 16",
          sas_left(33), sas_left(34), sas_left(35));

    /* The key-on waits for the next core (steps 73, 75), and a one-shot's
     * last sample is never heard (step 172): 4 samples play 8, 12 and stop. */
    sas_fresh();
    call5(psp_nid("__sceSasSetVoicePCM"), SAS_CORE, 0, PCM, 4, 0xFFFFFFFFu);
    sas_flat_voice(0);
    CHECK(call(psp_nid("__sceSasSetKeyOn"), SAS_CORE, 0, 0, 0) == 0, "key on");
    CHECK(call(psp_nid("__sceSasGetEndFlag"), SAS_CORE, 0, 0, 0) & 1u,
          "the end flag is still up straight after KeyOn");
    CHECK(call(psp_nid("__sceSasSetKeyOn"), SAS_CORE, 0, 0, 0) == 0x80420016u,
          "but the key is down: a second KeyOn is refused");
    sas_core();
    CHECK(sas_left(33) == 8 && sas_left(34) == 12 && sas_left(35) == 0,
          "one-shot of 4: L[33..35] = %d %d %d, want 8 12 0",
          sas_left(33), sas_left(34), sas_left(35));
    CHECK(call(psp_nid("__sceSasGetEndFlag"), SAS_CORE, 0, 0, 0) & 1u,
          "the one-shot has ended after its core");

    /* VAG flag 7 ends the stream before its block (step 162): block 0 plays
     * 256 for 27 samples, block 1 is never heard. */
    sas_fresh();
    for (uint32_t b = 0; b < 2; b++) {
        psp_write8(SAS_DATA + 16 * b, 0x04);              /* filter 0, shift 4 */
        psp_write8(SAS_DATA + 16 * b + 1, b ? 7 : 0);
        for (uint32_t i = 2; i < 16; i++) psp_write8(SAS_DATA + 16 * b + i, b ? 0x22 : 0x11);
    }
    call5(psp_nid("__sceSasSetVoice"), SAS_CORE, 0, SAS_DATA, 32, 0);
    sas_flat_voice(0);
    call(psp_nid("__sceSasSetKeyOn"), SAS_CORE, 0, 0, 0);
    sas_core();
    CHECK(sas_left(33) == 256 && sas_left(59) == 256 && sas_left(60) == 0,
          "VAG flag 7: L[33] %d L[59] %d L[60] %d, want 256 256 0",
          sas_left(33), sas_left(59), sas_left(60));

    /* The noise register (step 198): frequency 63 ticks every 2 samples from
     * the voice's second, shifting in ones from 0. */
    sas_fresh();
    call(psp_nid("__sceSasSetNoise"), SAS_CORE, 0, 63, 0);
    sas_flat_voice(0);
    call(psp_nid("__sceSasSetKeyOn"), SAS_CORE, 0, 0, 0);
    sas_core();
    {
        static const int want[8] = { 0, 1, 1, 3, 3, 7, 7, 15 };
        for (uint32_t i = 0; i < 8; i++)
            CHECK(sas_left(32 + i) == want[i], "noise 63: L[%u] = %d, want %d",
                  32 + i, sas_left(32 + i), want[i]);
    }

    /* A direct decay sets the height to its rate (step 110). */
    sas_fresh();
    call5(psp_nid("__sceSasSetVoicePCM"), SAS_CORE, 0, PCM, 64, 0);
    call7(psp_nid("__sceSasSetADSRmode"), SAS_CORE, 0, 0xF, 0, 5, 1, 1);
    call7(psp_nid("__sceSasSetADSR"), SAS_CORE, 0, 0xF, 0x7FFFFFFF, 0x20000000, 0, 0);
    call(psp_nid("__sceSasSetSL"), SAS_CORE, 0, 0x10000000, 0);
    call(psp_nid("__sceSasSetKeyOn"), SAS_CORE, 0, 0, 0);
    sas_core();
    CHECK(call(psp_nid("__sceSasGetEnvelopeHeight"), SAS_CORE, 0, 0, 0) == 0x20000000u,
          "direct decay 0x20000000 holds there, got %08X",
          call(psp_nid("__sceSasGetEnvelopeHeight"), SAS_CORE, 0, 0, 0));
}

/* What sasprobe 3 measured on firmware 6.60 (run 4); each check names its
 * step. */
static void test_sas_round3_rules(void) {
    const uint32_t RAMP = SAS_DATA, CONST = SAS_DATA + 0x1000u;
    for (uint32_t i = 0; i < 64; i++) {
        psp_write16(RAMP + i * 2u, (uint16_t)(4 * (i + 1)));
        psp_write16(CONST + i * 2u, 16384);
    }

    /* The noise clock (step 264): frequency 53 moves the register after the
     * voice's sample 3 and then 6, 6, 3, 3, 6 samples apart. */
    sas_fresh();
    call(psp_nid("__sceSasSetNoise"), SAS_CORE, 0, 53, 0);
    sas_flat_voice(0);
    call(psp_nid("__sceSasSetKeyOn"), SAS_CORE, 0, 0, 0);
    sas_core();
    {
        static const int at[9][2] = { { 35, 0 }, { 36, 1 }, { 41, 1 }, { 42, 3 }, { 47, 3 },
                                      { 48, 7 }, { 50, 7 }, { 51, 15 }, { 54, 31 } };
        for (int i = 0; i < 9; i++)
            CHECK(sas_left((uint32_t)at[i][0]) == at[i][1], "noise 53: L[%d] = %d, want %d",
                  at[i][0], sas_left((uint32_t)at[i][0]), at[i][1]);
    }

    /* SetNoise(63) on a voice playing 48 (step 279): the tick already due
     * at 2, then one every 4 samples -- 63's spacing, 48's table. */
    sas_fresh();
    call(psp_nid("__sceSasSetNoise"), SAS_CORE, 0, 48, 0);
    sas_flat_voice(0);
    call(psp_nid("__sceSasSetKeyOn"), SAS_CORE, 0, 0, 0);
    sas_core();
    call(psp_nid("__sceSasSetNoise"), SAS_CORE, 0, 63, 0);
    sas_core();
    {
        static const int at[7][2] = { { 1, -2948 }, { 2, -5895 }, { 5, -5895 }, { 6, -11789 },
                                      { 9, -11789 }, { 10, -23578 }, { 14, 18380 } };
        for (int i = 0; i < 7; i++)
            CHECK(sas_left((uint32_t)at[i][0]) == at[i][1], "noise 48 to 63: L[%d] = %d, want %d",
                  at[i][0], sas_left((uint32_t)at[i][0]), at[i][1]);
    }

    /* The re-key fade (steps 177, 275): KeyOff and KeyOn with no core
     * between. The old voice plays sample 0, then fades over 20 samples by
     * 0.625 a sample; the new one starts at 32 as ever. */
    sas_fresh();
    call5(psp_nid("__sceSasSetVoicePCM"), SAS_CORE, 0, CONST, 64, 0);
    sas_flat_voice(0);
    call(psp_nid("__sceSasSetKeyOn"), SAS_CORE, 0, 0, 0);
    sas_core();
    call(psp_nid("__sceSasSetKeyOff"), SAS_CORE, 0, 0, 0);
    call(psp_nid("__sceSasSetKeyOn"), SAS_CORE, 0, 0, 0);
    sas_core();
    {
        static const int at[7][2] = { { 0, 16384 }, { 1, 10240 }, { 2, 6400 }, { 10, 149 },
                                      { 20, 1 }, { 21, 0 }, { 33, 16384 } };
        for (int i = 0; i < 7; i++)
            CHECK(sas_left((uint32_t)at[i][0]) == at[i][1], "re-key: L[%d] = %d, want %d",
                  at[i][0], sas_left((uint32_t)at[i][0]), at[i][1]);
    }

    /* The pause fade (steps 305, 310-312): the first paused core plays the
     * sample due and then fades the *next* source sample; nothing advances,
     * so the resumed voice plays that first sample again. The looping ramp
     * is at its sample 32 (132) when the second core starts. */
    sas_fresh();
    call5(psp_nid("__sceSasSetVoicePCM"), SAS_CORE, 0, RAMP, 64, 0);
    sas_flat_voice(0);
    call(psp_nid("__sceSasSetKeyOn"), SAS_CORE, 0, 0, 0);
    sas_core();
    call(psp_nid("__sceSasSetPause"), SAS_CORE, 1, 1, 0);
    sas_core();
    CHECK(sas_left(0) == 132 && sas_left(1) == 85 && sas_left(2) == 53 && sas_left(21) == 0,
          "pause: L[0..2] %d %d %d L[21] %d, want 132 85 53 0",
          sas_left(0), sas_left(1), sas_left(2), sas_left(21));
    sas_core();
    CHECK(sas_left(0) == 0 && sas_left(1) == 0, "a second paused core is silent");
    call(psp_nid("__sceSasSetPause"), SAS_CORE, 1, 0, 0);
    sas_core();
    CHECK(sas_left(0) == 132 && sas_left(1) == 136, "resumed: L[0..1] %d %d, want 132 136",
          sas_left(0), sas_left(1));

    /* Waves at pitch 441, a 100-sample period (steps 338, 344, 346, 347). */
    {
        static const struct { const char *call; uint32_t duty; uint32_t n; int want; } w[] = {
            { "__sceSasSetSteepWave", 25, 24, 8192 },   { "__sceSasSetSteepWave", 25, 25, -8192 },
            { "__sceSasSetTrianglarWave", 0, 1, 16057 }, { "__sceSasSetTrianglarWave", 0, 99, -16057 },
            { "__sceSasSetTrianglarWave", 75, 50, -1 },  { "__sceSasSetTrianglarWave", 100, 99, 31527 },
        };
        for (unsigned i = 0; i < sizeof w / sizeof w[0]; i++) {
            sas_fresh();
            call(psp_nid(w[i].call), SAS_CORE, 0, w[i].duty, 0);
            sas_flat_voice(0);
            call(psp_nid("__sceSasSetPitch"), SAS_CORE, 0, 441, 0);
            call(psp_nid("__sceSasSetKeyOn"), SAS_CORE, 0, 0, 0);
            sas_core();
            CHECK(sas_left(32 + w[i].n) == w[i].want, "%s(%u): L[32+%u] = %d, want %d", w[i].call,
                  w[i].duty, w[i].n, sas_left(32 + w[i].n), w[i].want);
        }
    }

    /* A volume of 0x80000000 is accepted and kept as 16 bits: silence
     * (step 291). */
    sas_fresh();
    call5(psp_nid("__sceSasSetVoicePCM"), SAS_CORE, 0, CONST, 64, 0);
    sas_flat_voice(0);
    call7(psp_nid("__sceSasSetVolume"), SAS_CORE, 0, 0x80000000u, 0x1000, 0, 0, 0);
    call(psp_nid("__sceSasSetKeyOn"), SAS_CORE, 0, 0, 0);
    sas_core();
    CHECK(sas_left(100) == 0 && (int16_t)psp_read16(SAS_OUT + 100 * 4 + 2) == 16384,
          "volume 0x80000000: L[100] %d R[100] %d, want 0 16384", sas_left(100),
          (int16_t)psp_read16(SAS_OUT + 100 * 4 + 2));

    /* A sustain that has fallen below 0x8000 ends at the next core (step
     * 136): exponent-rev 0x1000000 from 0x20000000 reads 0x6A2A after ten
     * cores and 0 after eleven. */
    sas_fresh();
    call5(psp_nid("__sceSasSetVoicePCM"), SAS_CORE, 0, CONST, 64, 0);
    call7(psp_nid("__sceSasSetVolume"), SAS_CORE, 0, 0x1000, 0x1000, 0, 0, 0);
    call7(psp_nid("__sceSasSetADSRmode"), SAS_CORE, 0, 0xF, 0, 5, 3, 1);
    call7(psp_nid("__sceSasSetADSR"), SAS_CORE, 0, 0xF, 0x7FFFFFFF, 0x20000000, 0x1000000, 0);
    call(psp_nid("__sceSasSetSL"), SAS_CORE, 0, 0x20000000, 0);
    call(psp_nid("__sceSasSetKeyOn"), SAS_CORE, 0, 0, 0);
    for (int c = 0; c < 10; c++) sas_core();
    CHECK(call(psp_nid("__sceSasGetEnvelopeHeight"), SAS_CORE, 0, 0, 0) == 0x6A2Au,
          "sustain after 10 cores: %08X, want 00006A2A",
          call(psp_nid("__sceSasGetEnvelopeHeight"), SAS_CORE, 0, 0, 0));
    sas_core();
    CHECK(call(psp_nid("__sceSasGetEnvelopeHeight"), SAS_CORE, 0, 0, 0) == 0 &&
          (call(psp_nid("__sceSasGetEndFlag"), SAS_CORE, 0, 0, 0) & 1u),
          "and ended after 11");

    /* Refusals: a null core to __sceSasCore (step 355), feedback 128 (319). */
    CHECK(call(psp_nid("__sceSasCore"), 0, SAS_OUT, 0, 0) == 0x80420005u,
          "__sceSasCore(NULL) is refused");
    CHECK(call(psp_nid("__sceSasRevParam"), SAS_CORE, 0, 128, 0) == 0x80420021u,
          "RevParam feedback 128 is refused");
}

/* sasprobe 3's reverb captures (steps 321-331, fw 6.60): voice 0 plays 32767
 * at frames 33 and 34 into the sends only, the effect at EVOL 0x1000 and
 * VON(1, 1); frames count from the key-on's core. Each `at` row is (frame,
 * 0 for L or 1 for R, the value the PSP wrote), in frame order. */
static void sas_rev_pulse(const char *name, int type, uint32_t delay, uint32_t fb,
                          const int (*at)[3], int n) {
    sas_fresh();
    for (uint32_t i = 0; i < 4; i++) psp_write16(SAS_DATA + i * 2u, i == 1 || i == 2 ? 32767 : 0);
    CHECK(call(psp_nid("__sceSasRevType"), SAS_CORE, (uint32_t)type, 0, 0) == 0, "%s: RevType", name);
    call(psp_nid("__sceSasRevParam"), SAS_CORE, delay, fb, 0);
    call(psp_nid("__sceSasRevEVOL"), SAS_CORE, 0x1000, 0x1000, 0);
    call(psp_nid("__sceSasRevVON"), SAS_CORE, 1, 1, 0);
    call5(psp_nid("__sceSasSetVoicePCM"), SAS_CORE, 0, SAS_DATA, 4, 0xFFFFFFFFu);
    sas_flat_voice(0);
    call7(psp_nid("__sceSasSetVolume"), SAS_CORE, 0, 0, 0, 0x1000, 0x1000, 0);
    call(psp_nid("__sceSasSetKeyOn"), SAS_CORE, 0, 0, 0);
    int k = 0;
    for (uint32_t c = 0; k < n && c < 64; c++) {
        sas_core();
        for (; k < n && (uint32_t)at[k][0] < (c + 1) * 256u; k++) {
            const uint32_t i = (uint32_t)at[k][0] - c * 256u;
            const int got = (int16_t)psp_read16(SAS_OUT + i * 4u + (at[k][1] ? 2u : 0u));
            CHECK(got == at[k][2], "%s: %c[%d] = %d, want %d", name, at[k][1] ? 'R' : 'L',
                  at[k][0], got, at[k][2]);
        }
    }
}

static void test_sas_reverb(void) {
    /* Echo: the pulse back at 16d + 7 + D1 steps, then again 16d + 4 later
     * at feedback / -128 of it; nothing on the odd frames. */
    static const int echo[][3] = { { 558, 0, 1 }, { 1110, 1, 15372 }, { 1118, 0, 15372 },
                                   { 1119, 0, 0 }, { 1638, 0, -7688 }, { 2158, 0, 3844 } };
    sas_rev_pulse("echo (16, 64)", 6, 16, 64, echo, 6);
    sas_rev_pulse("delay (16, 64)", 7, 16, 64, echo, 6);
    static const int echo_d8[][3] = { { 598, 1, 15372 }, { 606, 0, 15372 }, { 870, 0, -7688 } };
    sas_rev_pulse("echo (8, 64)", 6, 8, 64, echo_d8, 3);
    static const int echo_fb0[][3] = { { 1118, 0, 15372 }, { 1638, 0, 0 }, { 1678, 0, -2 } };
    sas_rev_pulse("echo (16, 0)", 6, 16, 0, echo_fb0, 3);

    /* The first arrivals and the loudest sample of four of the others. */
    static const int room[][3] = { { 492, 0, 29 }, { 568, 1, 29 }, { 1188, 1, 5642 },
                                   { 1364, 0, 5642 } };
    sas_rev_pulse("room", 0, 16, 64, room, 4);
    static const int hall[][3] = { { 528, 0, 98 }, { 540, 1, 98 }, { 2640, 1, 5175 },
                                   { 3472, 0, 5175 } };
    sas_rev_pulse("hall", 4, 16, 64, hall, 4);
    static const int space[][3] = { { 624, 1, -216 }, { 932, 0, 211 }, { 4532, 1, -6202 },
                                    { 6996, 0, -6202 } };
    sas_rev_pulse("space", 5, 16, 64, space, 4);
    static const int pipe[][3] = { { 250, 1, 3 }, { 312, 0, 5 }, { 378, 1, 4843 },
                                   { 592, 1, -8584 }, { 682, 0, 7255 } };
    sas_rev_pulse("pipe", 8, 16, 64, pipe, 5);
}

/* stdout and stderr are not in the descriptor table, and an async write to
 * them used to return BADF -- which dropped the message a panic path writes
 * right before abort(). The synchronous path handled those fds; this pins the
 * async one to the same behaviour. */
static void test_stdio_async(void) {
    const uint32_t NID = psp_nid("sceIoWriteAsync");

    const uint32_t MSG = 0x08840000u;
    const char *text = "panic: test message\n";
    for (size_t i = 0; i <= strlen(text); i++) psp_write8(MSG + i, text[i]);

    CHECK(call(NID, 1, MSG, (uint32_t)strlen(text), 0) == SCE_KERNEL_ERROR_OK,
          "async write to stdout succeeds, not BADF");
    CHECK(call(NID, 2, MSG, (uint32_t)strlen(text), 0) == SCE_KERNEL_ERROR_OK,
          "async write to stderr succeeds, not BADF");
    /* A real descriptor still behaves as before. */
    CHECK(call(NID, 0x3F, MSG, (uint32_t)strlen(text), 0) == 0x80020323,
          "async write to an unopened fd is still BADF");
}

static void test_display(void) {
    psp_clock_realtime(0);
    psp_display_reset();

    /* Headless runs retain the advancing-counter fallback: without a real
     * scanout clock, a guest busy-waiting on either value must still make
     * progress.  Windowed runs exercise the wall-time branch in the replay
     * integration tests. */
    CHECK(call(psp_nid("sceDisplayGetVcount"), 0, 0, 0, 0) == 0,
          "virtual vcount starts at zero");
    CHECK(call(psp_nid("sceDisplayGetAccumulatedHcount"), 0, 0, 0, 0) == 286,
          "virtual hcount shares vcount's 286-line grid");
    CHECK(call(psp_nid("sceDisplayGetVcount"), 0, 0, 0, 0) == 2,
          "virtual counter reads continue to advance");

    CHECK(call(psp_nid("sceDisplaySetMode"), 0, 480, 272, 0) == 0, "set mode");
    /* (topaddr, bufferwidth, pixelformat, sync) */
    CHECK(call(psp_nid("sceDisplaySetFrameBuf"), PSP_VRAM_BASE, 512, 3, 0) == 0,
          "set framebuffer");
    CHECK(psp_display_framebuffer() == PSP_VRAM_BASE, "framebuffer recorded");

    uint64_t v0 = psp_display_vblanks();
    call(psp_nid("sceDisplayWaitVblank"), 0, 0, 0, 0);
    call(psp_nid("sceDisplayWaitVblankStartCB"), 0, 0, 0, 0);
    CHECK(psp_display_vblanks() == v0 + 2,
          "vblank waits advance the frame counter (the bring-up heartbeat)");
}

/* A ScePspDateTime: six u16 and a u32 microsecond. */
static void put_date(uint32_t at, int y, int mo, int d, int h, int mi, int s, uint32_t us) {
    const int f[6] = { y, mo, d, h, mi, s };
    for (int i = 0; i < 6; i++) psp_write16(at + 2u * (uint32_t)i, (uint16_t)f[i]);
    psp_write32(at + 12, us);
}

/* The SysClock conversions and the sceRtc calendar, against what a 6.60 PSP
 * answered (threadprobe steps 132-140). */
static void test_time_calls(void) {
    const uint32_t B = 0x08805000u;   /* scratch in user RAM */

    CHECK(call(psp_nid("sceKernelUSec2SysClock"), 1234567, B, 0, 0) == 0 &&
          psp_read32(B) == 0x0012D687u && psp_read32(B + 4) == 0, "USec2SysClock(1234567)");
    psp_write32(B, 0x12345678u);
    psp_write32(B + 4, 9);
    call(psp_nid("sceKernelSysClock2USec"), B, B + 8, B + 12, 0);
    CHECK(psp_read32(B + 8) == 0x9830 && psp_read32(B + 12) == 0x1EA78,
          "SysClock2USec(0x9_12345678) = %X s %X us", psp_read32(B + 8), psp_read32(B + 12));

    /* A tick is a microsecond since 0001-01-01. */
    put_date(B, 2000, 1, 1, 0, 0, 0, 0);
    CHECK(call(psp_nid("sceRtcGetTick"), B, B + 16, 0, 0) == 0, "GetTick");
    CHECK(psp_read32(B + 16) == 0x3A63A000u && psp_read32(B + 20) == 0x00E01D00u,
          "tick of 2000-01-01 = %08X_%08X", psp_read32(B + 20), psp_read32(B + 16));

    const uint32_t DOW = psp_nid("sceRtcGetDayOfWeek");
    CHECK(call(DOW, 2000, 1, 1, 0) == 6, "2000-01-01 is a Saturday");
    CHECK(call(DOW, 2023, 2, 30, 0) == 4, "2023-02-30 runs on into March");
    CHECK(call(DOW, 2023, 13, 1, 0) == 1, "month 13 carries into the year");
    CHECK(call(psp_nid("sceRtcGetDaysInMonth"), 2023, 13, 0, 0) == 0x800001FFu, "month 13");
    CHECK(call(psp_nid("sceRtcGetDaysInMonth"), 2000, 2, 0, 0) == 29, "February 2000");

    put_date(B + 32, 2023, 2, 30, 0, 0, 0, 0);
    CHECK(call(psp_nid("sceRtcCheckValid"), B + 32, 0, 0, 0) == (uint32_t)-3, "Feb 30 is a bad day");

    /* GetTick takes month 13 as 12 (step 161): 2023-13-01 is 2023-12-01. */
    put_date(B, 2023, 13, 1, 0, 0, 0, 0);
    put_date(B + 32, 2023, 12, 1, 0, 0, 0, 0);
    call(psp_nid("sceRtcGetTick"), B, B + 16, 0, 0);
    call(psp_nid("sceRtcGetTick"), B + 32, B + 48, 0, 0);
    CHECK(psp_read32(B + 16) == psp_read32(B + 48) && psp_read32(B + 20) == psp_read32(B + 52),
          "GetTick of 2023-13-01 is not 2023-12-01's");

    /* Gettimeofday: seconds under a day and a zeroed timezone (step 154). */
    for (int i = 0; i < 4; i++) psp_write32(B + 4u * (uint32_t)i, 0xEEEEEEEEu);
    CHECK(call(psp_nid("sceKernelLibcGettimeofday"), B, B + 8, 0, 0) == 0, "Gettimeofday");
    CHECK(psp_read32(B) < 86400u && psp_read32(B + 4) < 1000000u,
          "Gettimeofday seconds %u usec %u", psp_read32(B), psp_read32(B + 4));
    CHECK(psp_read32(B + 8) == 0 && psp_read32(B + 12) == 0, "timezone %08X %08X",
          psp_read32(B + 8), psp_read32(B + 12));

    /* Months clamp the day: 2000-01-31 + 1 month is 2000-02-29. */
    put_date(B, 2000, 1, 31, 0, 0, 0, 0);
    call(psp_nid("sceRtcGetTick"), B, B + 16, 0, 0);
    CHECK(call(psp_nid("sceRtcTickAddMonths"), B + 24, B + 16, 1, 0) == 0, "TickAddMonths");
    call(psp_nid("sceRtcSetTick"), B + 32, B + 24, 0, 0);
    CHECK(psp_read16(B + 32) == 2000 && psp_read16(B + 34) == 2 && psp_read16(B + 36) == 29,
          "Jan 31 + 1 month = %u-%u-%u", psp_read16(B + 32), psp_read16(B + 34),
          psp_read16(B + 36));

    put_date(B, 2000, 1, 1, 0, 0, 0, 0);
    call(psp_nid("sceRtcGetTick"), B, B + 16, 0, 0);
    call(psp_nid("sceRtcFormatRFC3339"), B + 64, B + 16, 540, 0);
    char s[40];
    CHECK(strcmp(psp_str(B + 64, s, sizeof s), "2000-01-01T09:00:00.00+09:00") == 0,
          "RFC3339 at +540: \"%s\"", s);
}

/* Freeing a pointer that is no memory at all (syncprobe steps 226-231, fw
 * 6.60): 0x10 is a bad block, not a bad address, and uid 0 is still an
 * unknown pool; only a kernel-space pointer is 0x800200D3. */
static void test_pool_free_pointers(void) {
    const uint32_t name = guest_name("pool");
    const uint32_t vpl = call5(psp_nid("sceKernelCreateVpl"), name, 2, 0, 0x100, 0);
    const uint32_t fpl = call7(psp_nid("sceKernelCreateFpl"), name, 2, 0, 0x10, 2, 0, 0);
    CHECK((int32_t)vpl > 0 && (int32_t)fpl > 0, "vpl %08X fpl %08X", vpl, fpl);

    const uint32_t FV = psp_nid("sceKernelFreeVpl"), FF = psp_nid("sceKernelFreeFpl");
    CHECK(call(FV, vpl, 0, 0, 0) == 0x800201B6u, "FreeVpl(vpl, NULL)");
    CHECK(call(FV, vpl, 0x10, 0, 0) == 0x800201B6u, "FreeVpl(vpl, 0x10)");
    CHECK(call(FV, 0, 0x10, 0, 0) == 0x8002019Cu, "FreeVpl(0, 0x10)");
    CHECK(call(FV, 0, 0xDEADBEEFu, 0, 0) == 0x800200D3u, "FreeVpl(0, 0xDEADBEEF)");
    CHECK(call(FF, fpl, 0, 0, 0) == 0x800201B6u, "FreeFpl(fpl, NULL)");
    CHECK(call(FF, fpl, 0x10, 0, 0) == 0x800201B6u, "FreeFpl(fpl, 0x10)");
    CHECK(call(FF, 0, 0x10, 0, 0) == 0x8002019Du, "FreeFpl(0, 0x10)");
    CHECK(call(FF, fpl, 0xDEADBEEFu, 0, 0) == 0x800200D3u, "FreeFpl(fpl, 0xDEADBEEF)");

    call(psp_nid("sceKernelDeleteVpl"), vpl, 0, 0, 0);
    call(psp_nid("sceKernelDeleteFpl"), fpl, 0, 0, 0);

    /* A tlspl may live in partition 5 (threadprobe step 131, fw 6.60). */
    const uint32_t tls = call7(psp_nid("sceKernelCreateTlspl"), name, 5, 0, 4, 1, 0, 0);
    CHECK((int32_t)tls > 0, "tlspl in partition 5: %08X", tls);
    call(psp_nid("sceKernelDeleteTlspl"), tls, 0, 0, 0);
}

/* ---- waits with real threads --------------------------------------------
 *
 * Thread bodies are C functions in the dispatch table, started through the
 * firmware calls, with the host's main context as the module's main thread
 * (priority 0x20, like the probe's). */
#define TW_SEMA  0x08802000u
#define TW_MBX   0x08802100u
#define TW_NEG   0x08802200u
#define TW_FLAG  0x08802300u
#define TW_INFO  0x08806000u
#define TW_TMO   0x08806100u
#define TW_MSG   0x08806200u

static uint32_t tw_obj, tw_tmo, tw_rc;
static int      tw_ran;

static void body_tw_sema(void) { tw_rc = call(psp_nid("sceKernelWaitSema"), tw_obj, 1, tw_tmo, 0); }
static void body_tw_mbx(void)  { tw_rc = call(psp_nid("sceKernelReceiveMbx"), tw_obj, TW_MSG, 0, 0); }
static void body_tw_neg(void)  { psp_cpu.r[PSP_REG_V0] = (uint32_t)-5; }
static void body_tw_flag(void) { tw_ran = 1; }

static uint32_t tw_start(uint32_t entry) {
    const uint32_t th = call5(psp_nid("sceKernelCreateThread"), guest_name("tw"), entry,
                              0x20, 0x1000, 0);
    CHECK((int32_t)th > 0, "create a thread: %08X", th);
    call(psp_nid("sceKernelStartThread"), th, 0, 0, 0);
    return th;
}

static uint32_t tw_waiters(void) {
    psp_write32(TW_INFO, 56);
    call(psp_nid("sceKernelReferSemaStatus"), tw_obj, TW_INFO, 0, 0);
    return psp_read32(TW_INFO + 52);
}

static void test_waits_with_threads(void) {
    psp_register(TW_SEMA, body_tw_sema);
    psp_register(TW_MBX, body_tw_mbx);
    psp_register(TW_NEG, body_tw_neg);
    psp_register(TW_FLAG, body_tw_flag);
    psp_sysmem_reset();
    psp_threadman_reset();
    psp_sched_set_threading(1);
    const uint32_t DELAY = psp_nid("sceKernelDelayThread");

    /* ReleaseWaitThread takes the thread out of the object's queue then and
     * there: the waiter count reads 1 -> 0 across the call (step 78). */
    tw_obj = call5(psp_nid("sceKernelCreateSema"), guest_name("tws"), 0, 0, 1, 0);
    tw_tmo = 0; tw_rc = 0xEEEEEEEEu;
    uint32_t th = tw_start(TW_SEMA);
    call(DELAY, 1000, 0, 0, 0);
    CHECK(tw_waiters() == 1, "waiting: %u waiters", tw_waiters());
    CHECK(call(psp_nid("sceKernelReleaseWaitThread"), th, 0, 0, 0) == 0, "release");
    CHECK(tw_waiters() == 0, "released: %u waiters, expected 0", tw_waiters());
    call(DELAY, 1000, 0, 0, 0);
    CHECK(tw_rc == SCE_KERNEL_ERROR_RELEASE_WAIT, "released wait returned %08X", tw_rc);

    /* A timeout that runs out while the thread is suspended takes it out of
     * the queue at the deadline, and the wait answers WAIT_TIMEOUT when it
     * is resumed (step 59). */
    psp_write32(TW_TMO, 1000);
    tw_tmo = TW_TMO; tw_rc = 0xEEEEEEEEu;
    th = tw_start(TW_SEMA);
    call(DELAY, 100, 0, 0, 0);
    CHECK(call(psp_nid("sceKernelSuspendThread"), th, 0, 0, 0) == 0, "suspend the waiter");
    call(DELAY, 5000, 0, 0, 0);
    CHECK(tw_waiters() == 0, "timed out while suspended: %u waiters, expected 0", tw_waiters());
    call(psp_nid("sceKernelResumeThread"), th, 0, 0, 0);
    call(DELAY, 1000, 0, 0, 0);
    CHECK(tw_rc == SCE_KERNEL_ERROR_WAIT_TIMEOUT, "timed-out wait returned %08X", tw_rc);
    call(psp_nid("sceKernelDeleteSema"), tw_obj, 0, 0, 0);

    /* ReferThreadStatus names the wait: a mailbox is waitType 5, waitId the
     * mailbox (step 17). */
    tw_obj = call(psp_nid("sceKernelCreateMbx"), guest_name("twm"), 0, 0, 0);
    th = tw_start(TW_MBX);
    call(DELAY, 100, 0, 0, 0);
    psp_write32(TW_INFO, 104);
    call(psp_nid("sceKernelReferThreadStatus"), th, TW_INFO, 0, 0);
    CHECK(psp_read32(TW_INFO + 68) == PSP_WAITTYPE_MBX && psp_read32(TW_INFO + 72) == tw_obj,
          "mbx wait: waitType %X waitId %08X", psp_read32(TW_INFO + 68),
          psp_read32(TW_INFO + 72));
    call(psp_nid("sceKernelDeleteMbx"), tw_obj, 0, 0, 0);
    call(DELAY, 100, 0, 0, 0);

    /* An entry point that returns a negative value ends with 800200D2 as its
     * exit status (step 54). */
    th = tw_start(TW_NEG);
    call(DELAY, 100, 0, 0, 0);
    CHECK(call(psp_nid("sceKernelGetThreadExitStatus"), th, 0, 0, 0) == 0x800200D2u,
          "exit status after returning -5: %08X",
          call(psp_nid("sceKernelGetThreadExitStatus"), th, 0, 0, 0));

    /* A file call gives up the CPU, even to a thread of the caller's own
     * priority, and keeps its answer (step 86). */
    tw_ran = 0;
    tw_start(TW_FLAG);
    const uint32_t fd = call(psp_nid("sceIoOpen"), guest_name("ms0:/no/such/file.bin"), 1, 0, 0);
    CHECK(tw_ran == 1, "an equal-priority thread did not run inside sceIoOpen");
    CHECK(fd == 0x80010002u, "open of a missing file answered %08X", fd);

    psp_sched_drain(5);
    psp_sched_join_all();
    psp_sched_set_threading(0);
    psp_threadman_reset();
}

int main(void) {
    CHECK(psp_mem_init() == 0, "memory init");
    psp_cpu_reset();
    psp_cpu.r[PSP_REG_SP] = 0x08810000u;   /* a stack for the "caller" */
    psp_hle_init();

    printf("registered %d firmware functions\n", psp_hle_count());

    test_sha1_vectors();
    test_nids_match_names();
    test_sysmem();
    test_semaphores();
    test_create_attributes();
    test_event_flags();
    test_threads();
    test_thread_argument_block();
    test_thread_rules();
    test_guest_strings();
    test_io_dirs();
    test_net();
    test_ge_display_list();
    test_ge_signal_pause();
    test_ge_callbacks();
    test_ge_long_list();
    test_ge_infinite_list();
    test_sas_adpcm();
    test_sas_hardware_rules();
    test_sas_round3_rules();
    test_sas_reverb();
    test_stdio_async();
    test_display();
    test_time_calls();
    test_pool_free_pointers();
    test_waits_with_threads();

    psp_mem_free();

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("all HLE checks passed\n");
    return 0;
}
