/* psprecomp — the guest clock. See include/psprecomp/clock.h. */

#include "psprecomp/clock.h"
#include "psprecomp/state.h"
#include "census.h"
#include "psprecomp/os.h"

#include <stdatomic.h>

/* How far a firmware call moves the clock.
 *
 * It exists only so that time cannot stop: a thread spinning on calls that neither
 * read the clock nor wait for a vblank keeps something runnable forever, so the
 * scheduler's idle jump never fires, and any thread sleeping on a deadline
 * would sleep through the rest of the run. Any non-zero value does that.
 *
 * Its size, though, decides how much guest time a piece of ordinary kernel work
 * appears to take -- and at 100us a *kernel call* cost as much as a hundred
 * microseconds of computation, which is roughly two orders of magnitude more
 * than one takes. That is not a harmless overestimate. pspautotests asks for
 * timeouts of a few hundred microseconds and then does three or four kernel
 * calls, and every one of those timeouts expired part-way through the work it
 * was meant to outlast. threads/semaphores/fifo shows it directly: hardware
 * finishes a checkpoint and *then* sees a 200us wait expire, where this saw the
 * expiry arrive between a line's text and its newline.
 *
 * One microsecond is both small enough to stay out of the way and closer to
 * what a PSP kernel call actually costs, so this is a fidelity fix rather than
 * a fudge. It still cannot stop: a million calls is a second, and the decode
 * loop this was introduced for makes 562 million of them in a run.
 *
 * ## And a clock read is a firmware call like any other
 *
 * Reads used to charge a hundred times this, on the argument that a guest
 * polling the clock in a loop should finish in a sane number of iterations.
 * That argument was about *our* throughput, and it was paid for in fidelity:
 * pspautotests' checkpoint() reads the clock once, so a checkpoint cost 100us
 * of guest time, and a test that delays 200us and prints two lines had its
 * delay expire inside the printing. threads/terminate, threads/threadmanidlist
 * and msgpipe/create each sat two lines from matching on that alone.
 *
 * The throughput it was protecting turns out not to need protecting. With
 * reads charging one microsecond the game's output is *byte-identical* --
 * same 633 GE lists, same 93,354,668 pixels -- so nothing it does polls the
 * clock in a loop long enough to notice, and no test in the suite started
 * timing out. So the separate read tick is gone rather than retuned: the two
 * questions had different answers only for as long as one of them was
 * unmeasured.
 *
 * ## It is not the checkpoint column, and cannot be
 *
 * It was the open suspect for the `[x]`/`[r]` differences in the threads suite,
 * on the theory that a call costing two or three microseconds would leave
 * msgpipe's `1us:` and `2us:` waits with a deadline already passed, taking the
 * no-wait path in sched.c -- `[x]` without a switch. No value of this number
 * does that, and the reason is the order the two happen in. The tick is charged
 * here at *call entry*; the handler then computes its deadline as
 * `psp_clock_peek() + usec` from the already-advanced clock, and compares it
 * against that same unchanged value. Raising the cost moves the deadline and
 * the comparison point together. It is a relative offset, so the guard cannot
 * fire for a nonzero timeout however expensive a call is made.
 *
 * The column was the wake path handing the CPU over as a yield rather than a
 * preemption; see psp_sched_preempt in sched.c. Left here because the theory is
 * a natural one to arrive at twice, and because a *rule* of the same shape --
 * "do not park below 3us" -- was implemented once and took forty tests to no
 * output at all while matching one. */
#define PSP_CALL_TICK_US 1u

static uint64_t g_us;

/* ---- real-time mode -------------------------------------------------------
 *
 * A second way for the clock to exist. Virtual time only moves when the guest
 * moves it, which is right for measurement and useless for presentation: a
 * frame loop paced on vblank runs as fast as the rasterizer manages, and
 * audio queued for playback at 44.1kHz arrives at whatever rate the host CPU
 * manages. Real-time mode anchors the same counter to CLOCK_MONOTONIC, so
 * guest microseconds and wall microseconds track one to one and the pacers --
 * vblank, idle sleep, audio backlog -- all become waits out of real time
 * rather than jumps over it.
 *
 * The mapping needs an origin, captured when the mode is enabled. Every
 * sleeper then targets `origin + guest_us`, and every path that would have
 * advanced virtual time calls wall_sync() first, so the two clocks cannot
 * drift: a jump forward is preceded by waiting out the same duration. A run
 * that falls behind never waits at all, which is what keeps a compute-bound
 * run from silently turning into a slide show of sleeps.
 *
 * What the mode must NOT do is keep the artificial advances. A read's tick
 * and a firmware call's tick would run guest time ahead of the wall it is
 * anchored to, and the pacing waits would never wait. Both instead adopt
 * elapsed wall time -- which reaches the threads that need it, because the
 * spin loops that charge ticks call in constantly, and each call pulls the
 * clock up to the wall. */

static int      g_realtime;
static uint64_t g_origin_ns;
static uint64_t g_start_ns;        /* when the mode was enabled; pauses never move it */

/* Host pauses. Written by the holding thread, read by any (the audio thread
 * measures its gaps with psp_clock_run_ns). A hold publishes the frozen run
 * time before it starts and clears it only after the total includes it, so a
 * reader on another thread never sees run time jump either way. */
static _Atomic uint64_t g_held_ns, g_hold_since_ns, g_frozen_ns;
static _Atomic int      g_holding;

static uint64_t wall_ns(void) { return psp_os_mono_ns(); }

/* Sleep until wall moment `origin + us`. Returns immediately when the run is
 * already there or past it. The 2us slack keeps a sync that lands on the
 * boundary from costing a syscall for nothing. */
static void wall_sync(uint64_t us) {
    const uint64_t target = g_origin_ns + us * 1000u;
    if (wall_ns() + 2000u >= target) return;
    psp_os_sleep_until_ns(target);
}

/* Anchored so that the guest's present is the wall's: at start-up that maps
 * guest 0 to now, and after a save state loads, the saved guest time. The
 * arithmetic is modular, so a guest that has run longer than the host has
 * been up still maps exactly. */
static void anchor(void) { g_origin_ns = g_start_ns = wall_ns() - g_us * 1000u; }

void psp_clock_realtime(int enable) {
    if (enable && !g_realtime) anchor();
    g_realtime = enable;
}

int psp_clock_is_realtime(void) { return g_realtime; }

int psp_clock_realtime_stats(uint64_t *guest_us, uint64_t *wall_us) {
    if (!g_realtime) return 0;
    if (guest_us) *guest_us = g_us;
    if (wall_us) *wall_us = (wall_ns() - g_start_ns) / 1000u;
    return 1;
}

void psp_clock_hold(void) {
    if (g_holding) return;
    const uint64_t now = wall_ns();
    g_hold_since_ns = now;
    g_frozen_ns = now - g_held_ns;
    g_holding = 1;
}

void psp_clock_release(void) {
    if (!g_holding) return;
    const uint64_t held = wall_ns() - g_hold_since_ns;
    if (g_realtime) g_origin_ns += held;
    g_held_ns += held;
    g_holding = 0;
}

uint64_t psp_clock_held_us(void) {
    return (g_held_ns + (g_holding ? wall_ns() - g_hold_since_ns : 0)) / 1000u;
}

/* A reader that sampled the wall just as a hold began can have seen a
 * nanosecond or two past the frozen time; the latest value returned keeps
 * run time from stepping back to it. */
static _Atomic uint64_t g_run_seen_ns;

uint64_t psp_clock_run_ns(void) {
    const uint64_t t = g_holding ? g_frozen_ns : wall_ns() - g_held_ns;
    uint64_t seen = g_run_seen_ns;
    while (t > seen && !atomic_compare_exchange_weak(&g_run_seen_ns, &seen, t)) {}
    return t > seen ? t : seen;
}

void psp_clock_reset(void) {
    g_us = 0;
    /* Re-anchor if the mode is already on: reset means a new run, and the
     * origin is what makes "microseconds since the module started" mean the
     * same thing on both sides of the mapping. */
    if (g_realtime) anchor();
}

uint64_t psp_clock_peek(void) { return g_us; }

static int clock_load(psp_state_reader *r, char *why, size_t size) {
    (void)r; (void)why; (void)size;
    if (g_realtime) anchor();
    return 0;
}

void psp_clock_keep(void) {
    static const psp_state_part part = { "clock", NULL, NULL, clock_load };
    PSP_STATE_KEEP(g_us);
    psp_state_register(&part);
}

uint64_t psp_clock_read(void) {
    if (g_realtime) {
        /* Adopt elapsed wall time rather than adding a tick: the poll loop
         * the tick serves terminates because the wall moves, and a tick would
         * break the one-to-one mapping the pacers depend on. Two reads inside
         * one instant can agree, which is the honest report. */
        const uint64_t wall = (wall_ns() - g_origin_ns) / 1000u;
        if (wall > g_us) g_us = wall;
        return g_us;
    }
    const uint64_t now = g_us;
    g_us += PSP_CALL_TICK_US;
    return now;
}

/* The next vblank, as an absolute moment rather than a duration.
 *
 * Vblanks are a grid the whole machine shares -- one scanout, so every thread
 * waiting for "the next frame" is waiting for the *same* moment. Returning that
 * moment is what lets them all park on it and be released together, which a
 * per-caller duration cannot express: three threads each adding a frame to
 * their own clock is three frames of guest time for one frame of scanout.
 *
 * Strictly future, so a caller already standing exactly on a boundary waits for
 * the next one rather than returning immediately. */
uint64_t psp_clock_next_frame(void) {
    return (g_us / PSP_CLOCK_FRAME_US + 1) * PSP_CLOCK_FRAME_US;
}

/* One tick, for a caller that is doing work rather than reading the time.
 * Separate from psp_clock_read so that "time passed" and "somebody asked what
 * time it is" stay distinguishable at the call site.
 *
 * In real-time mode this adopts elapsed wall time rather than charging an
 * invented tick -- but it must not be a no-op, which is what it was first
 * written as, and the difference is the whole movie. The decode loop paces
 * the old-fashioned way: it spins on the ring, charging a tick per call, and
 * those ticks are what let the sleeping update thread's deadline arrive. A
 * no-op froze guest time behind exactly that spin -- the pre-0030 stall,
 * reproduced in real time to the pixel. Wall time passes on its own, but only
 * a clock read reaches a thread that never sleeps and never asks; the spin
 * now asks every call. */
void psp_clock_tick(void) {
    if (g_realtime) {
        const uint64_t wall = (wall_ns() - g_origin_ns) / 1000u;
        if (wall > g_us) g_us = wall;
        return;
    }
    g_us += PSP_CALL_TICK_US;
}

void psp_clock_advance_to(uint64_t us) {
    if (us > g_us) {
        /* The idle scheduler's jump becomes a wait. Sleeping here is safe in
         * a way it rarely is: the caller holds the scheduler lock and nothing
         * is runnable -- that is why this path exists -- so the wait cannot
         * delay anyone who could have made progress. */
        if (g_realtime) wall_sync(us);
        g_us = us;
    }
}
