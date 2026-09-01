/* psprecomp — the guest clock. See include/psprecomp/clock.h. */

#include "psprecomp/clock.h"

#include <errno.h>
#include <time.h>

/* 59.94Hz, the PSP's refresh rate, rounded to whole microseconds. */
#define PSP_FRAME_US 16667u

/* How far a read moves the clock.
 *
 * Big enough that a poll loop counting microseconds finishes in a sane number
 * of iterations, small enough that a game measuring a real interval does not
 * see it pass instantly. At 100us a millisecond costs ten reads. */
#define PSP_READ_TICK_US 100u

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

static uint64_t wall_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/* Sleep until wall moment `origin + us`. Returns immediately when the run is
 * already there or past it. The 2us slack keeps a sync that lands on the
 * boundary from costing a syscall for nothing. */
static void wall_sync(uint64_t us) {
    const uint64_t target = g_origin_ns + us * 1000u;
    if (wall_ns() + 2000u >= target) return;
    const struct timespec t = {
        .tv_sec  = (time_t)(target / 1000000000u),
        .tv_nsec = (long)(target % 1000000000u),
    };
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL) == EINTR) {}
}

void psp_clock_realtime(int enable) {
    if (enable && !g_realtime) g_origin_ns = wall_ns();
    g_realtime = enable;
}

int psp_clock_is_realtime(void) { return g_realtime; }

void psp_clock_reset(void) {
    g_us = 0;
    /* Re-anchor if the mode is already on: reset means a new run, and the
     * origin is what makes "microseconds since the module started" mean the
     * same thing on both sides of the mapping. */
    if (g_realtime) g_origin_ns = wall_ns();
}

uint64_t psp_clock_peek(void) { return g_us; }

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
    g_us += PSP_READ_TICK_US;
    return now;
}

void psp_clock_frame(void) {
    g_us += PSP_FRAME_US;
    if (g_realtime) wall_sync(g_us);
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
    g_us += PSP_READ_TICK_US;
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
