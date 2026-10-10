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
#include "psprecomp/clock.h"
#include "psprecomp/os.h"

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
#define ENTRY_TIMED    0x00005000u
#define ENTRY_TWOKEN   0x00006000u
#define ENTRY_TWAKER   0x00007000u
#define ENTRY_SLEEPER  0x0000D000u
#define ENTRY_SPINNER  0x0000E000u

#define UID_WAITER     0x00040001u
#define UID_WAKER      0x00040002u
#define UID_TRIVIAL    0x00040003u
#define UID_STRANDED   0x00040004u
#define UID_TIMED      0x00040005u
#define UID_TWOKEN     0x00040006u
#define UID_SLEEPER    0x00040007u
#define UID_SPINNER    0x00040008u

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
static uint32_t trivial_gp;

static void body_trivial(void) {
    trivial_ran = 1;
    trivial_uid = psp_sched_current();
    trivial_gp  = psp_cpu.r[PSP_REG_GP];
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

static void body_waker_timed(void) {
    psp_sched_wake(UID_TWOKEN);
}

/* A frame loop that delays, and a worker that never blocks. */
#define SPIN_LIMIT 50000

static volatile int spin_count, spin_at_wake, sleeper_woke;

static void body_sleeper(void) {
    psp_sched_delay(2000);                /* 2 ms: expires inside one 5 ms slice */
    spin_at_wake = spin_count;
    sleeper_woke = 1;
    check_one_running("inside sleeper, after its delay");
}

static void body_spinner(void) {
    for (spin_count = 0; spin_count < SPIN_LIMIT && !sleeper_woke; spin_count++) {
        psp_clock_tick();                 /* what every firmware call costs */
        psp_sched_tick();
    }
}

/* A timed wait nothing will ever satisfy: released by its own deadline. */
static int      timed_rc;
static uint64_t timed_clock_after;

static void body_timed(void) {
    timed_rc = psp_sched_block_until(psp_sched_current(), PSP_SCHED_BLOCKED,
                                     "test-timed", psp_clock_peek() + 5000);
    timed_clock_after = psp_clock_peek();
    check_one_running("inside timed waiter, after its deadline");
}

/* A timed wait that a signal reaches first. */
static int timed_woken_rc;

static void body_timed_woken(void) {
    timed_woken_rc = psp_sched_block_until(psp_sched_current(), PSP_SCHED_BLOCKED,
                                           "test-timed-woken",
                                           psp_clock_peek() + 1000000);
    check_one_running("inside signalled timed waiter");
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

    CHECK(psp_sched_spawn(UID_TRIVIAL, ENTRY_TRIVIAL, FAKE_SP, 0, 0, 0, 32) == 0,
          "spawn after a refused wait failed");

    const int live = psp_sched_drain(5);
    CHECK(live == 0, "%d thread(s) still alive after a refused wait", live);
    CHECK(trivial_ran, "the thread spawned after a refused wait never ran");
    check_one_running("after draining a thread spawned post-refusal");
}

/* A thread inherits its module's $gp, and nothing else can supply it.
 *
 * Every other register a fresh thread starts with comes from sceKernelStartThread
 * -- the two arguments, the stack it was given, the zeroed $ra that ends it. $gp
 * comes from neither the call nor the instruction stream: it is per *module*,
 * declared once in the module info, and a module with a small-data area reads
 * that area as an offset from it. thread_main memset the whole register file and
 * never put it back, so every such access from a spawned thread went to around
 * address 0. Armored Core is built -G0 and never names $gp, which is why this
 * survived the whole bring-up; a pspautotests module names it constantly. */
static void test_thread_inherits_gp(void) {
    psp_sched_reset();
    psp_sched_set_threading(1);
    trivial_ran = 0;
    trivial_gp  = 0;

    const uint32_t gp = 0x08812340u;
    psp_cpu.r[PSP_REG_GP] = gp;

    CHECK(psp_sched_spawn(UID_TRIVIAL, ENTRY_TRIVIAL, FAKE_SP, 0, 0, 0, 32) == 0,
          "spawning the trivial thread failed");

    const int live = psp_sched_drain(5);
    CHECK(live == 0, "%d thread(s) still alive", live);
    CHECK(trivial_ran, "the thread never ran");
    CHECK(trivial_gp == gp,
          "the thread started with $gp = 0x%08X, expected the starter's 0x%08X",
          trivial_gp, gp);
}

/* A timed wait expires, and a plain one in the same situation is refused.
 *
 * This is the whole difference a deadline makes, and it is not a convenience.
 * An undated wait with nothing else runnable has to be refused -- nobody could
 * ever satisfy it, and parking would be a hang. A dated one in exactly the same
 * situation is satisfiable *by time*: the handoff finds no runnable thread,
 * looks for the earliest sleeping deadline, moves the virtual clock to it and
 * releases the sleeper. Guest time only advances because the guest advanced it,
 * so the moment has to be jumped to rather than waited for.
 *
 * Without this, every kernel wait with a timeout is a deadlock report. */
static void test_timed_wait_expires_when_nothing_can_satisfy_it(void) {
    psp_sched_reset();
    psp_sched_set_threading(1);
    psp_clock_reset();
    timed_rc = -99;
    timed_clock_after = 0;

    const uint64_t before = psp_clock_peek();
    CHECK(psp_sched_spawn(UID_TIMED, ENTRY_TIMED, FAKE_SP, 0, 0, 0, 32) == 0,
          "spawning the timed waiter failed");

    const int live = psp_sched_drain(5);
    CHECK(live == 0, "%d thread(s) still alive", live);
    CHECK(timed_rc == PSP_SCHED_EXPIRED,
          "a timed wait with nothing to satisfy it returned %d, expected "
          "PSP_SCHED_EXPIRED (%d)", timed_rc, PSP_SCHED_EXPIRED);
    CHECK(timed_clock_after >= before + 5000,
          "the clock moved to the deadline: %llu -> %llu, expected +5000us",
          (unsigned long long)before, (unsigned long long)timed_clock_after);
}

/* And a signal that arrives first wins, reported as such.
 *
 * The two outcomes need opposite handling by the caller -- one means the wait
 * succeeded and one means it did not -- so the scheduler has to distinguish
 * them. Being running again does not: both routes end there. */
static void test_timed_wait_prefers_a_signal(void) {
    psp_sched_reset();
    psp_sched_set_threading(1);
    psp_clock_reset();
    timed_woken_rc = -99;

    CHECK(psp_sched_spawn(UID_TWOKEN, ENTRY_TWOKEN, FAKE_SP, 0, 0, 0, 32) == 0,
          "spawning the timed waiter failed");
    CHECK(psp_sched_spawn(UID_WAKER, ENTRY_TWAKER, FAKE_SP, 0, 0, 0, 32) == 0,
          "spawning the waker failed");

    const int live = psp_sched_drain(5);
    CHECK(live == 0, "%d thread(s) still alive", live);
    CHECK(timed_woken_rc == PSP_SCHED_WOKEN,
          "a timed wait released by a signal returned %d, expected "
          "PSP_SCHED_WOKEN (%d)", timed_woken_rc, PSP_SCHED_WOKEN);
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

    CHECK(psp_sched_spawn(UID_WAITER, ENTRY_WAITER, FAKE_SP, 0, 0, 0, 32) == 0,
          "spawning the waiter failed");
    CHECK(psp_sched_spawn(UID_WAKER, ENTRY_WAKER, FAKE_SP, 0, 0, 0, 32) == 0,
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

    CHECK(psp_sched_spawn(UID_STRANDED, ENTRY_STRANDED, FAKE_SP, 0, 0, 0, 32) == 0,
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

/* A force-stopped run and a finished run both leave zero live threads —
 * stop_all marks every slot dead — so the stop reason is what tells the boot
 * summary apart. It has to survive until read, and reset has to clear it, or
 * the next run inherits the last run's ending. */
static void test_stop_reason_reports_and_resets(void) {
    psp_sched_reset();
    psp_sched_set_threading(1);
    CHECK(psp_sched_stop_reason() == NULL,
          "a fresh run already has a stop reason");

    CHECK(psp_sched_spawn(UID_TRIVIAL, ENTRY_TRIVIAL, FAKE_SP, 0, 0, 0, 32) == 0,
          "spawn failed");
    psp_sched_stop_all("test-stop");
    /* Joined before the reset below forgets it: a host thread that had not yet
     * reached its first wait would otherwise take its slot index into the next
     * test and run whatever that test puts there. */
    psp_sched_join_all();
    CHECK(psp_sched_live() == 0, "stop_all left threads alive");
    CHECK(psp_sched_stop_reason() != NULL &&
          strcmp(psp_sched_stop_reason(), "test-stop") == 0,
          "stop reason came back as \"%s\"",
          psp_sched_stop_reason() ? psp_sched_stop_reason() : "(null)");

    psp_sched_reset();
    CHECK(psp_sched_stop_reason() == NULL, "reset did not clear the stop reason");
}

/* ---- the ready-queue policy ------------------------------------------------
 *
 * One FIFO queue per priority and no timeslice, as threadprobe measured on
 * fw 6.60. Each test records the order its threads ran in as a string. */
#define ENTRY_Q_A      0x00008000u
#define ENTRY_Q_B      0x00009000u
#define ENTRY_Q_C      0x0000A000u
#define ENTRY_Q_D      0x0000B000u
#define ENTRY_Q_HI     0x0000C000u
#define UID_Q_A        0x00041001u
#define UID_Q_B        0x00041002u
#define UID_Q_C        0x00041003u
#define UID_Q_D        0x00041004u
#define UID_Q_HI       0x00041005u

static char q_order[64];
static int  q_mode;
static void q_tag(const char *t) { strncat(q_order, t, sizeof q_order - strlen(q_order) - 1); }

enum { Q_DEADLINE, Q_NOSLICE, Q_ROTATE, Q_HEAD };

static void body_q_a(void) {
    if (q_mode == Q_DEADLINE) { psp_sched_delay(30000); q_tag("A"); return; }
    if (q_mode == Q_NOSLICE) {
        q_tag("A0");
        psp_clock_advance_to(psp_clock_peek() + 20000);
        psp_sched_tick();
        q_tag("A1");
        return;
    }
    if (q_mode == Q_HEAD) {
        q_tag("A0");
        psp_sched_spawn(UID_Q_HI, ENTRY_Q_HI, FAKE_SP, 0, 0, 0, 16);
        q_tag("A1");
        return;
    }
    q_tag("A");
}
static void body_q_b(void) {
    if (q_mode == Q_DEADLINE) psp_sched_delay(20000);
    q_tag("B");
}
static void body_q_c(void) {
    if (q_mode == Q_DEADLINE) psp_sched_delay(10000);
    q_tag("C");
}
/* Step 77's main: busy past every deadline without giving up the CPU. */
static void body_q_d(void) {
    psp_clock_advance_to(psp_clock_peek() + 45000);
    psp_sched_tick();
    q_tag("D");
}
static void body_q_hi(void) { q_tag("H"); }

static void q_run(int mode) {
    psp_sched_join_all();      /* the previous test's host threads, as above */
    psp_sched_reset();
    psp_sched_set_threading(1);
    psp_clock_reset();
    q_order[0] = '\0';
    q_mode = mode;
}

/* Timed waits that have all expired run in deadline order, not in the order
 * the scan happens to find them (threadprobe step 77: delays of 30, 20 and
 * 10 ms, main busy for 45, run C B A). */
static void test_expired_delays_run_in_deadline_order(void) {
    q_run(Q_DEADLINE);
    psp_sched_spawn(UID_Q_A, ENTRY_Q_A, FAKE_SP, 0, 0, 0, 32);
    psp_sched_spawn(UID_Q_B, ENTRY_Q_B, FAKE_SP, 0, 0, 0, 32);
    psp_sched_spawn(UID_Q_C, ENTRY_Q_C, FAKE_SP, 0, 0, 0, 32);
    psp_sched_spawn(UID_Q_D, ENTRY_Q_D, FAKE_SP, 0, 0, 0, 32);
    CHECK(psp_sched_drain(5) == 0, "threads still alive");
    CHECK(strcmp(q_order, "DCBA") == 0, "ran %s, expected DCBA", q_order);
}

/* No timeslice: a thread that stays runnable keeps the CPU against an equal
 * however long it runs (threadprobe step 75: `A0 A1 B` across 20 ms). */
static void test_no_timeslice_between_equals(void) {
    q_run(Q_NOSLICE);
    psp_sched_spawn(UID_Q_A, ENTRY_Q_A, FAKE_SP, 0, 0, 0, 32);
    psp_sched_spawn(UID_Q_B, ENTRY_Q_B, FAKE_SP, 0, 0, 0, 32);
    CHECK(psp_sched_drain(5) == 0, "threads still alive");
    CHECK(strcmp(q_order, "A0A1B") == 0, "ran %s, expected A0A1B", q_order);
}

/* Rotating another priority moves its head to its tail and leaves the caller
 * running (threadprobe step 66: W1 W2 W3 ready, rotate, run W2 W3 W1). */
static void test_rotate_other_level(void) {
    q_run(Q_ROTATE);
    psp_sched_spawn(UID_Q_A, ENTRY_Q_A, FAKE_SP, 0, 0, 0, 48);
    psp_sched_spawn(UID_Q_B, ENTRY_Q_B, FAKE_SP, 0, 0, 0, 48);
    psp_sched_spawn(UID_Q_C, ENTRY_Q_C, FAKE_SP, 0, 0, 0, 48);
    CHECK(psp_sched_rotate(48) == 1, "nothing to rotate at 48");
    CHECK(q_order[0] == '\0', "the rotation ran %s before the caller finished", q_order);
    CHECK(psp_sched_drain(5) == 0, "threads still alive");
    CHECK(strcmp(q_order, "BCA") == 0, "ran %s, expected BCA", q_order);
}

/* A thread displaced by a more urgent one goes back to the head of its queue,
 * ahead of an equal that was waiting (threadprobe steps 59, 71, 72). */
static void test_displaced_thread_keeps_its_place(void) {
    q_run(Q_HEAD);
    psp_sched_spawn(UID_Q_A, ENTRY_Q_A, FAKE_SP, 0, 0, 0, 32);
    psp_sched_spawn(UID_Q_B, ENTRY_Q_B, FAKE_SP, 0, 0, 0, 32);
    CHECK(psp_sched_drain(5) == 0, "threads still alive");
    CHECK(strcmp(q_order, "A0HA1B") == 0, "ran %s, expected A0HA1B", q_order);
}

/* Suspension is a flag over the wait, not a replacement for it (threadprobe
 * steps 36, 52-54, fw 6.60): resuming a waiter leaves it waiting, a wake
 * delivered while suspended completes the wait without running the thread,
 * and a resume of a ready, more urgent thread says so, for the caller to
 * switch to it. */
static int s_wait_rc;

static void body_s_waiter(void) {
    s_wait_rc = psp_sched_block(psp_sched_current(), PSP_SCHED_BLOCKED, "test-suspend");
    q_tag("W");
}

static void body_s_main(void) {
    CHECK(psp_sched_suspend(UID_Q_A) == 1, "suspend of a waiter failed");
    CHECK(psp_sched_resume(UID_Q_A) == 0, "resuming a waiter asked for a switch");
    CHECK(psp_sched_state_of(UID_Q_A) == PSP_SCHED_BLOCKED,
          "resuming a waiter ended its wait (state %d)", psp_sched_state_of(UID_Q_A));
    psp_sched_suspend(UID_Q_A);
    CHECK(psp_sched_wake(UID_Q_A) == 0, "a suspended thread counted as urgent");
    CHECK(psp_sched_state_of(UID_Q_A) == PSP_SCHED_READY,
          "a wake while suspended did not end the wait");
    q_tag("M1");
    psp_sched_yield();                     /* must come straight back */
    CHECK(psp_sched_resume(UID_Q_A) == 1, "resuming a ready, more urgent thread "
                                          "did not ask for a switch");
    psp_sched_preempt();
    q_tag("M2");
}

static void test_suspend_is_a_flag_over_the_wait(void) {
    q_run(Q_ROTATE);
    s_wait_rc = -99;
    psp_sched_spawn(UID_Q_A, ENTRY_Q_C + 0x10, FAKE_SP, 0, 0, 0, 32);
    psp_sched_spawn(UID_Q_B, ENTRY_Q_C + 0x20, FAKE_SP, 0, 0, 0, 48);
    CHECK(psp_sched_drain(5) == 0, "threads still alive");
    CHECK(strcmp(q_order, "M1WM2") == 0, "ran %s, expected M1WM2", q_order);
    CHECK(s_wait_rc == PSP_SCHED_WOKEN, "the suspended waiter's wait returned %d",
          s_wait_rc);
}

/* A finished thread's slot is reused once its host thread is joined, so the
 * table bounds threads alive at once rather than starts per run (threadprobe
 * made 115 starts; the 130th of any run used to fail). */
static void test_dead_slots_are_reused(void) {
    q_run(Q_ROTATE);
    int ran = 0;
    for (int i = 0; i < 1500; i++) {
        trivial_ran = 0;
        if (psp_sched_spawn(UID_TRIVIAL + 0x100 + (uint32_t)i, ENTRY_TRIVIAL,
                            FAKE_SP, 0, 0, 0, 32) != 0) break;
        psp_sched_drain(5);
        ran += trivial_ran;
    }
    CHECK(ran == 1500, "%d of 1500 sequential starts ran", ran);
}

/* ReferThreadStatus's counters (threadprobe steps 1, 9-13, fw 6.60): run time
 * is nonzero during a first turn, a delay is one release, and a thread that
 * starts a more urgent one has been preempted once. */
static psp_sched_stats st_first, st_after;

static void body_st_hi(void) { }

static void body_st(void) {
    psp_sched_stats_of(psp_sched_current(), &st_first);
    psp_sched_delay(1000);
    psp_sched_spawn(UID_Q_HI, ENTRY_Q_C + 0x40, FAKE_SP, 0, 0, 0, 16);
    psp_sched_stats_of(psp_sched_current(), &st_after);
}

static void test_thread_counters(void) {
    q_run(Q_ROTATE);
    psp_sched_spawn(UID_Q_A, ENTRY_Q_C + 0x30, FAKE_SP, 0, 0, 0, 32);
    psp_sched_stats none;
    CHECK(psp_sched_stats_of(UID_Q_B, &none) == 0 && none.run_us == 0,
          "a uid with no slot has counters");
    CHECK(psp_sched_drain(5) == 0, "threads still alive");
    CHECK(st_first.run_us > 0 && st_first.releases == 0,
          "first turn: run %llu, releases %u",
          (unsigned long long)st_first.run_us, st_first.releases);
    CHECK(st_after.releases == 1 && st_after.thread_preempts == 1 &&
          st_after.intr_preempts == 0,
          "after a delay and a preempting start: releases %u, thread %u, intr %u",
          st_after.releases, st_after.thread_preempts, st_after.intr_preempts);
}

/* An expired delay beats a thread that never blocks.
 *
 * Ticks are the only place a spinner can give way, and a tick only rotates
 * when somebody else could run. A sleeper whose deadline has passed still
 * reads SLEEPING until a handoff promotes it, so a tick that counted only
 * READY threads let the spinner keep the CPU through every expired delay.
 * Armored Core 3 Portable's movie decoder did exactly that at the end of the
 * prologue: it spun on sceMpegRingbufferAvailableSize while the frame loop sat
 * in a 16.9 ms sceKernelDelayThread, and the game never polled the pad again.
 *
 * The sleeper outranks the spinner, as a frame loop outranks a worker, so once
 * it is runnable it is also the one that runs. */
static void test_expired_delay_preempts_a_spinner(void) {
    psp_sched_reset();
    psp_sched_set_threading(1);
    psp_clock_reset();
    spin_count = spin_at_wake = sleeper_woke = 0;

    CHECK(psp_sched_spawn(UID_SLEEPER, ENTRY_SLEEPER, FAKE_SP, 0, 0, 0, 30) == 0,
          "spawning the sleeper failed");
    CHECK(psp_sched_spawn(UID_SPINNER, ENTRY_SPINNER, FAKE_SP, 0, 0, 0, 40) == 0,
          "spawning the spinner failed");

    const int live = psp_sched_drain(5);
    CHECK(live == 0, "%d thread(s) still alive", live);
    CHECK(sleeper_woke, "the sleeper never woke");
    CHECK(spin_at_wake > 0 && spin_at_wake < SPIN_LIMIT,
          "the sleeper ran after %d spinner ticks; it should have been let in "
          "before the spinner gave up at %d", spin_at_wake, SPIN_LIMIT);
}

/* psp_sched_run_on: a function run on a parked thread's own host thread while
 * the caller waits, as the GE's walk is handed to the thread whose host thread
 * holds the GL context (src/hle/ge.c). Host-thread-local state tells the two
 * host threads apart; the function also sees the parked thread's identity. */
#define ENTRY_HOST_OWNER 0x0000F000u
#define ENTRY_HOST_ASKER 0x0000F100u
#define UID_HOST_OWNER   0x00040009u
#define UID_HOST_ASKER   0x0004000Au

static PSP_THREAD_LOCAL int t_owner_host;
static int served_on_owner, served_as_owner, served_here, served_missing, owner_resumed;

static void serve_on_owner(void) {
    served_on_owner = t_owner_host;
    served_as_owner = psp_sched_current() == UID_HOST_OWNER;
}
static void serve_here(void) { served_here = !t_owner_host && psp_sched_current() == UID_HOST_ASKER; }
static void serve_missing(void) { served_missing = !t_owner_host; }

static void body_host_owner(void) {
    t_owner_host = 1;
    psp_sched_block(psp_sched_current(), PSP_SCHED_BLOCKED, "test-owner");
    owner_resumed = 1;
}

static void body_host_asker(void) {
    psp_sched_run_on(UID_HOST_OWNER, serve_on_owner);
    check_one_running("after a function ran on a parked thread");
    psp_sched_run_on(UID_HOST_ASKER, serve_here);     /* the caller: here */
    psp_sched_run_on(0x0004FFFFu, serve_missing);     /* no such thread: here */
    psp_sched_wake(UID_HOST_OWNER);
}

static void test_run_on_a_parked_thread(void) {
    psp_sched_reset();
    psp_sched_set_threading(1);
    served_on_owner = served_as_owner = served_here = served_missing = owner_resumed = 0;

    CHECK(psp_sched_spawn(UID_HOST_OWNER, ENTRY_HOST_OWNER, FAKE_SP, 0, 0, 0, 30) == 0,
          "spawning the owner failed");
    CHECK(psp_sched_spawn(UID_HOST_ASKER, ENTRY_HOST_ASKER, FAKE_SP, 0, 0, 0, 40) == 0,
          "spawning the asker failed");

    const int live = psp_sched_drain(5);
    CHECK(live == 0, "%d thread(s) still alive", live);
    CHECK(served_on_owner, "the function did not run on the parked thread's host thread");
    CHECK(served_as_owner, "the function did not see the parked thread's identity");
    CHECK(served_here, "a function asked of the caller itself did not run on its thread");
    CHECK(served_missing, "a function asked of no thread did not run on the caller's");
    CHECK(owner_resumed, "the parked thread did not carry on after serving");
}

int main(void) {
    psp_register(ENTRY_HOST_OWNER, body_host_owner);
    psp_register(ENTRY_HOST_ASKER, body_host_asker);
    psp_register(ENTRY_Q_C + 0x30, body_st);
    psp_register(ENTRY_Q_C + 0x40, body_st_hi);
    psp_register(ENTRY_Q_C + 0x10, body_s_waiter);
    psp_register(ENTRY_Q_C + 0x20, body_s_main);
    psp_register(ENTRY_Q_A,      body_q_a);
    psp_register(ENTRY_Q_B,      body_q_b);
    psp_register(ENTRY_Q_C,      body_q_c);
    psp_register(ENTRY_Q_D,      body_q_d);
    psp_register(ENTRY_Q_HI,     body_q_hi);
    psp_register(ENTRY_TRIVIAL,  body_trivial);
    psp_register(ENTRY_WAITER,   body_waiter);
    psp_register(ENTRY_WAKER,    body_waker);
    psp_register(ENTRY_STRANDED, body_stranded);
    psp_register(ENTRY_TIMED,    body_timed);
    psp_register(ENTRY_TWOKEN,   body_timed_woken);
    psp_register(ENTRY_TWAKER,   body_waker_timed);
    psp_register(ENTRY_SLEEPER,  body_sleeper);
    psp_register(ENTRY_SPINNER,  body_spinner);

    psp_sched_init();

    test_unsatisfiable_wait_is_refused();
    test_scheduling_survives_a_refused_wait();
    test_thread_inherits_gp();
    test_timed_wait_expires_when_nothing_can_satisfy_it();
    test_timed_wait_prefers_a_signal();
    test_expired_delay_preempts_a_spinner();
    test_block_and_wake_round_trip();
    test_thread_identity();
    test_guest_thread_unsatisfiable_wait();
    test_stop_reason_reports_and_resets();
    test_expired_delays_run_in_deadline_order();
    test_no_timeslice_between_equals();
    test_rotate_other_level();
    test_displaced_thread_keeps_its_place();
    test_suspend_is_a_flag_over_the_wait();
    test_dead_slots_are_reused();
    test_thread_counters();
    test_run_on_a_parked_thread();

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("all scheduler checks passed\n");
    return 0;
}
