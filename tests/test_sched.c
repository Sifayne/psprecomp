/* Scheduler tests — the handoff token, with no game data involved.
 *
 * The PSP is single-core and never runs two threads at once, so this layer has
 * exactly one invariant worth the name: **at most one slot is RUNNING, and it
 * is the one holding the token.** Everything else here is a corollary.
 *
 * That invariant was broken for a while, and the way it broke is why these
 * tests are shaped the way they are. A handoff that found nobody runnable
 * reported it by setting a sticky global and broadcasting on the condition
 * variable. Two things followed. Every parked thread woke, and — because the
 * wait loop exited on the flag rather than on the token — returned to its
 * caller as though it had been scheduled, so several host threads ran guest
 * code at once against the single global `psp_cpu`. And the flag was never
 * cleared, so a *later* handoff that succeeded was still read as a deadlock by
 * whoever looked next, and that caller took the token back from the thread the
 * handoff had just marked RUNNING. Two slots were left RUNNING, one of them
 * holding nothing — and since only READY slots are ever selected, that thread
 * could never be scheduled again. The game stopped, and the scheduler called it
 * a deadlock.
 *
 * So the checks below are not about internals. They pin the observable
 * contract, which is what makes the whole class of bug detectable rather than
 * just the one instance: the RUNNING count after every transition, the
 * documented return value of an unsatisfiable wait, and thread identity.
 *
 * Guest thread bodies are ordinary C functions registered in the dispatch
 * table. Nothing here reads guest memory, so no module and no image is needed.
 */

#include "psprecomp/sched.h"
#include "psprecomp/dispatch.h"
#include "psprecomp/cpu.h"

#include <stdio.h>
#include <string.h>

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

/* Synthetic guest addresses. Any value the dispatch table can hold will do —
 * they are never fetched from, only looked up. */
#define ENTRY_TRIVIAL  0x00001000u
#define ENTRY_WAITER   0x00002000u
#define ENTRY_WAKER    0x00003000u
#define ENTRY_STRANDED 0x00004000u

#define UID_WAITER     0x00040001u
#define UID_WAKER      0x00040002u
#define UID_TRIVIAL    0x00040003u
#define UID_STRANDED   0x00040004u

/* A guest stack pointer. Never dereferenced — thread_main only copies it into
 * $sp, and none of these bodies touch memory. */
#define FAKE_SP        0x09FF0000u

/* ---- the invariant ---------------------------------------------------------
 *
 * Asked through the public dump rather than by reaching into the slot table,
 * so the test constrains the interface a host actually sees. */
static int running_slots(void) {
    FILE *f = tmpfile();
    if (!f) return -1;
    psp_sched_dump_threads(f);

    char buf[4096];
    rewind(f);
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = '\0';
    fclose(f);

    int count = 0;
    for (const char *p = buf; (p = strstr(p, "running")) != NULL; p += 7) count++;
    return count;
}

/* Checked after every transition below. One token, one RUNNING slot. */
static void check_one_running(const char *where) {
    const int n = running_slots();
    CHECK(n == 1, "%s: %d slots RUNNING, expected exactly 1", where, n);
}

/* ---- guest thread bodies --------------------------------------------------- */

static int  waiter_ran, waiter_resumed, stranded_ran;
static int  trivial_ran, waker_ran;
static int  waiter_block_rc, stranded_block_rc;
static uint32_t waiter_uid_before, waiter_uid_after, trivial_uid;

static void body_trivial(void) {
    trivial_ran = 1;
    trivial_uid = psp_sched_current();
    check_one_running("inside trivial thread");
}

/* Blocks on something the waker will satisfy. */
static void body_waiter(void) {
    waiter_ran = 1;
    waiter_uid_before = psp_sched_current();
    check_one_running("inside waiter, before block");

    waiter_block_rc = psp_sched_block(psp_sched_current(),
                                      PSP_SCHED_BLOCKED, "test-object");

    waiter_resumed = 1;
    waiter_uid_after = psp_sched_current();
    check_one_running("inside waiter, after resume");
}

static void body_waker(void) {
    waker_ran = 1;
    check_one_running("inside waker");
    psp_sched_wake(UID_WAITER);
}

/* Blocks on something nothing will ever satisfy. */
static void body_stranded(void) {
    stranded_ran = 1;
    stranded_block_rc = psp_sched_block(psp_sched_current(),
                                        PSP_SCHED_BLOCKED, "test-unsatisfiable");
    check_one_running("inside stranded thread, after refused wait");
}

/* ---- tests ----------------------------------------------------------------- */

/* The contract in sched.h: a wait that would leave nothing runnable is refused
 * with -1 rather than parked, and the caller comes back running.
 *
 * A caller that blocks in a loop until its condition holds would otherwise spin
 * against a scheduler handing the token straight back — a busy hang, and worse
 * than the fabricated timeout this replaced. */
static void test_unsatisfiable_wait_is_refused(void) {
    psp_sched_reset();
    psp_sched_set_threading(1);

    check_one_running("before a refused wait");

    const int rc = psp_sched_block(0, PSP_SCHED_BLOCKED, "test-nothing-can-wake-this");
    CHECK(rc == -1, "block with no other thread returned %d, expected -1", rc);

    check_one_running("after a refused wait");
    CHECK(psp_sched_current() == 0,
          "main context reports uid 0x%08X after a refused wait, expected 0",
          psp_sched_current());
}

/* The regression the sticky flag caused: a refused wait must leave no residue.
 *
 * Under the old code the refusal path was also the only place the deadlock flag
 * was cleared, which made "did a previous wait fail?" change the outcome of the
 * next one. Scheduling after a refusal has to work exactly as it would have
 * before it. */
static void test_scheduling_survives_a_refused_wait(void) {
    psp_sched_reset();
    psp_sched_set_threading(1);
    trivial_ran = 0;
    trivial_uid = 0;

    const int rc = psp_sched_block(0, PSP_SCHED_BLOCKED, "test-nothing-can-wake-this");
    CHECK(rc == -1, "setup: expected a refused wait, got %d", rc);

    CHECK(psp_sched_spawn(UID_TRIVIAL, ENTRY_TRIVIAL, FAKE_SP, 0, 0, 32) == 0,
          "spawn after a refused wait failed");

    const int live = psp_sched_drain(5);
    CHECK(live == 0, "%d thread(s) still alive after a refused wait", live);
    CHECK(trivial_ran, "the thread spawned after a refused wait never ran");
    check_one_running("after draining a thread spawned post-refusal");
}

/* A full round trip: block, hand the token over, wake, resume.
 *
 * The waker is spawned at a lower priority number so the spawn-time reschedule
 * does not run it before the waiter has parked — the waiter has to be blocked
 * for the wake to be the thing that releases it. */
static void test_block_and_wake_round_trip(void) {
    psp_sched_reset();
    psp_sched_set_threading(1);
    waiter_ran = waiter_resumed = waker_ran = 0;
    waiter_block_rc = -99;
    waiter_uid_before = waiter_uid_after = 0;

    CHECK(psp_sched_spawn(UID_WAITER, ENTRY_WAITER, FAKE_SP, 0, 0, 32) == 0,
          "spawning the waiter failed");
    CHECK(psp_sched_spawn(UID_WAKER, ENTRY_WAKER, FAKE_SP, 0, 0, 32) == 0,
          "spawning the waker failed");

    const int live = psp_sched_drain(5);

    CHECK(waiter_ran,     "the waiter never ran");
    CHECK(waker_ran,      "the waker never ran");
    CHECK(waiter_resumed, "the waiter blocked but was never resumed");
    CHECK(waiter_block_rc == 0,
          "the waiter's block returned %d, expected 0 — it was satisfiable",
          waiter_block_rc);
    CHECK(live == 0, "%d thread(s) still alive after a satisfiable wait", live);
    check_one_running("after a completed block/wake round trip");
}

/* Identity comes from the host thread, not from the token.
 *
 * psp_sched_current() used to read whoever held the token, which is the same
 * answer while a thread runs — but it returned 0 whenever the token was held by
 * nobody, and 0 is the main context's uid. Callers hand the result straight
 * back to psp_sched_block, so a guest thread parked the *main context's* slot
 * and left its own marked RUNNING. Checked on both sides of a block, since the
 * gap either side of a handoff is where it went wrong. */
static void test_thread_identity(void) {
    CHECK(waiter_uid_before == UID_WAITER,
          "waiter reports uid 0x%08X before blocking, expected 0x%08X",
          waiter_uid_before, UID_WAITER);
    CHECK(waiter_uid_after == UID_WAITER,
          "waiter reports uid 0x%08X after resuming, expected 0x%08X",
          waiter_uid_after, UID_WAITER);
    CHECK(trivial_uid == UID_TRIVIAL,
          "trivial thread reports uid 0x%08X, expected 0x%08X",
          trivial_uid, UID_TRIVIAL);
}

/* A guest thread whose wait nothing can satisfy is refused, exactly as the main
 * context is — and the scheduler stays coherent afterwards rather than being
 * left with a slot marked RUNNING that holds no token. */
static void test_guest_thread_unsatisfiable_wait(void) {
    psp_sched_reset();
    psp_sched_set_threading(1);
    stranded_ran = 0;
    stranded_block_rc = -99;

    CHECK(psp_sched_spawn(UID_STRANDED, ENTRY_STRANDED, FAKE_SP, 0, 0, 32) == 0,
          "spawning the stranded thread failed");

    const int live = psp_sched_drain(5);

    CHECK(stranded_ran, "the stranded thread never ran");
    CHECK(stranded_block_rc == -1,
          "the stranded thread's block returned %d, expected -1",
          stranded_block_rc);
    CHECK(live == 0, "%d thread(s) still alive; the refusal did not let it finish",
          live);
    check_one_running("after a guest thread's wait was refused");
}

int main(void) {
    psp_register(ENTRY_TRIVIAL,  body_trivial);
    psp_register(ENTRY_WAITER,   body_waiter);
    psp_register(ENTRY_WAKER,    body_waker);
    psp_register(ENTRY_STRANDED, body_stranded);

    psp_sched_init();

    test_unsatisfiable_wait_is_refused();
    test_scheduling_survives_a_refused_wait();
    test_block_and_wake_round_trip();
    test_thread_identity();
    test_guest_thread_unsatisfiable_wait();

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("all scheduler checks passed\n");
    return 0;
}
