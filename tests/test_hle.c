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

    /* Low placement. */
    uint32_t uid = call5(NID_ALLOC, 2, 0, 0 /*Low*/, 0x1000, 0);
    CHECK(uid >= 0x00010000u, "low alloc returns a UID, got 0x%08X", uid);
    uint32_t lo = call(NID_HEAD, uid, 0, 0, 0);
    CHECK(lo != 0, "block has an address");

    /* High placement must land above the low one -- games depend on the
     * distinction, so it is not enough that both merely succeed. */
    uint32_t uid2 = call5(NID_ALLOC, 2, 0, 1 /*High*/, 0x1000, 0);
    uint32_t hi = call(NID_HEAD, uid2, 0, 0, 0);
    CHECK(hi > lo, "high allocation sits above the low one (lo=0x%08X hi=0x%08X)", lo, hi);

    /* Blocks must not overlap. */
    CHECK(lo + 0x1000 <= hi, "blocks do not overlap");

    /* Sizes round up to the hardware's 256-byte granule. */
    uint32_t uid3 = call5(NID_ALLOC, 2, 0, 0, 100, 0);
    CHECK(uid3 >= 0x00010000u, "a 100-byte request succeeds");
    CHECK(psp_sysmem_free() % 0x100 == 0, "allocations are granule-rounded");

    /* Freeing returns the memory. */
    uint32_t mid = psp_sysmem_free();
    CHECK(call(NID_FREE, uid, 0, 0, 0) == 0, "free succeeds");
    CHECK(psp_sysmem_free() > mid, "freeing returns memory");

    CHECK(call(NID_FREE, 0xDEADBEEF, 0, 0, 0) == SCE_KERNEL_ERROR_UNKNOWN_UID,
          "freeing an unknown UID is refused");
    CHECK(call(NID_HEAD, 0xDEADBEEF, 0, 0, 0) == 0,
          "an unknown UID has no address");

    /* An impossible request fails rather than returning a bogus block. */
    uint32_t huge = call5(NID_ALLOC, 2, 0, 0, 0x7F000000u, 0);
    CHECK(huge == SCE_KERNEL_ERROR_NO_MEMORY, "an oversized request fails, got 0x%08X", huge);

    call(NID_FREE, uid2, 0, 0, 0);
    call(NID_FREE, uid3, 0, 0, 0);
    CHECK(psp_sysmem_free() == before, "everything freed restores the heap");
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
 * own stack, is deliberately *not* implemented; threadman.c says why and what
 * it costs. There is no test for it here, because a test for behaviour that was
 * knowingly left out is a test that has to be wrong. */
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

    uint32_t thid = call5(CREATE, 0 /*name*/, ENTRY, 32 /*prio*/, 0x4000 /*stack*/, 0);
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
    static const struct { const char *name; uint32_t want; } refused[] = {
        { "sceNetAdhocInit",         SCE_KERNEL_ERROR_NOTIMPLEMENTED  },
        { "sceNetAdhocctlInit",      SCE_KERNEL_ERROR_NOTIMPLEMENTED  },
        { "sceNetAdhocTerm",         SCE_NET_ADHOC_ERROR_NOT_INITIALIZED    },
        { "sceNetAdhocPdpCreate",    SCE_NET_ADHOC_ERROR_NOT_INITIALIZED    },
        { "sceNetAdhocPdpSend",      SCE_NET_ADHOC_ERROR_NOT_INITIALIZED    },
        { "sceNetAdhocPdpRecv",      SCE_NET_ADHOC_ERROR_NOT_INITIALIZED    },
        { "sceNetAdhocPdpDelete",    SCE_NET_ADHOC_ERROR_NOT_INITIALIZED    },
        { "sceNetAdhocPtpClose",     SCE_NET_ADHOC_ERROR_NOT_INITIALIZED    },
        { "sceNetAdhocPtpSend",      SCE_NET_ADHOC_ERROR_NOT_INITIALIZED    },
        { "sceNetAdhocPtpOpen",      SCE_NET_ADHOC_ERROR_NOT_INITIALIZED    },
        { "sceNetAdhocPtpRecv",      SCE_NET_ADHOC_ERROR_NOT_INITIALIZED    },
        { "sceNetAdhocPtpAccept",    SCE_NET_ADHOC_ERROR_NOT_INITIALIZED    },
        { "sceNetAdhocPtpListen",    SCE_NET_ADHOC_ERROR_NOT_INITIALIZED    },
        { "sceNetAdhocPtpConnect",   SCE_NET_ADHOC_ERROR_NOT_INITIALIZED    },
        { "sceNetAdhocPtpFlush",     SCE_NET_ADHOC_ERROR_NOT_INITIALIZED    },
        { "sceNetAdhocctlTerm",      SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED },
        { "sceNetAdhocctlAddHandler",SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED },
        { "sceNetAdhocctlDelHandler",SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED },
        { "sceNetAdhocctlDisconnect",SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED },
        { "sceNetAdhocctlConnect",   SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED },
        { "sceNetAdhocctlGetState",  SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED },
        { "sceNetAdhocctlGetPeerList", SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED },
        { "sceNetGetLocalEtherAddr", SCE_NET_ERROR_NO_ADDRESS },
    };
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        uint32_t got = call(psp_nid(refused[i].name), 0, 0, 0, 0);
        CHECK(got == refused[i].want, "%s: got 0x%08X, want 0x%08X",
              refused[i].name, got, refused[i].want);
    }

    /* sceNetInit manages a pool, not the radio: real argument validation,
     * then vacuous success -- the two rules PPSSPP pins against hardware. */
    CHECK(call(psp_nid("sceNetInit"), 65536, 30, 0x1000, 30) == 0,
          "net init succeeds");
    CHECK(call(psp_nid("sceNetInit"), 0, 30, 0x1000, 30) == SCE_KERNEL_ERROR_ILLEGAL_MEMSIZE,
          "net init refuses a zero pool");
    CHECK(call(psp_nid("sceNetInit"), 65536, 0x07, 0x1000, 30) == SCE_KERNEL_ERROR_ILLEGAL_PRIORITY,
          "net init refuses a bad callout priority");
    CHECK(call(psp_nid("sceNetInit"), 65536, 30, 0x1000, 0x78) == SCE_KERNEL_ERROR_ILLEGAL_PRIORITY,
          "net init refuses a bad netintr priority");
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
    fd = call(OPEN, guest_name("ms0:/PSP/SAVEDATA/ZZZ/DATA.BIN"), 0x0001, 0, 0);
    CHECK((int32_t)fd >= 3, "reopen reads");
    for (uint32_t i = 0; i < 64; i++) psp_write8(DST + i, 0);
    CHECK(call(READ, fd, DST, 64, 0) == 64, "read back what was written");
    int same = 1;
    for (uint32_t i = 0; i < 64; i++) same &= psp_read8(DST + i) == (uint8_t)(i * 3 + 1);
    CHECK(same, "round trip is byte-exact");
    CHECK(call(CLOSE, fd, 0, 0, 0) == 0, "close succeeds");

    /* Enumeration finds the file by name. */
    uint32_t dd = call(DOPEN, guest_name("ms0:/PSP/SAVEDATA/ZZZ"), 0, 0, 0);
    CHECK((int32_t)dd >= 1, "dopen succeeds");
    int found = 0;
    while (call(DREAD, dd, DIR, 0, 0) == 1) {
        char name[260];
        psp_str(DIR + 52, name, sizeof name);
        if (!strcmp(name, "DATA.BIN")) found = 1;
    }
    CHECK(found, "dread enumerates DATA.BIN");
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
    /* sceGeListEnQueue(list, stall=0, cbid, arg), then DrawSync(WAIT):
     * the GE is deferred, so nothing executes until the sync drains it. */
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
    call(psp_nid("__sceSasCore"), 0, OUT, 0, 0);
    CHECK(psp_sas_nonzero() == 0, "silence before key-on");

    /* An envelope, explicitly. A voice with no attack rate stays at zero
     * height and is silent, on hardware as here -- this used to lean on a
     * default rate invented by the key-on, which adsrcurve's rate-0 sweeps
     * showed was not hardware's (findings item 45). */
    CHECK(call7(psp_nid("__sceSasSetADSR"), 0, 0, 15,
                0x40000000 / 64, 0x40000000 / 512, 0x30000000, 0x40000000 / 256) == 0,
          "adsr set");

    CHECK(call(psp_nid("__sceSasSetKeyOn"), 0, 0, 0, 0) == 0, "key on");
    call(psp_nid("__sceSasCore"), 0, OUT, 0, 0);

    CHECK(psp_sas_frames() == 2, "two frames rendered, got %llu",
          (unsigned long long)psp_sas_frames());
    CHECK(psp_sas_nonzero() > 0, "audio was actually produced after key-on");

    /* The end flag is how a game knows a sound finished; a voice that never
     * reports ended is a common way for audio to stall after the first sound. */
    uint32_t ended = call(psp_nid("__sceSasGetEndFlag"), 0, 0, 0, 0);
    CHECK((ended & ~1u) != 0, "unused voices report ended, got 0x%08X", ended);
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
    psp_display_reset();

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
    test_guest_strings();
    test_io_dirs();
    test_net();
    test_ge_display_list();
    test_ge_infinite_list();
    test_sas_adpcm();
    test_stdio_async();
    test_display();

    psp_mem_free();

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("all HLE checks passed\n");
    return 0;
}
