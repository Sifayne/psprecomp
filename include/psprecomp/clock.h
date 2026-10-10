/* psprecomp — the guest clock.
 *
 * A virtual microsecond clock, not the host's. Two reasons:
 *
 *   - Determinism. The differential oracle compares an interpreter against
 *     recompiled C and attributes any disagreement to a codegen bug. A clock
 *     that reported real elapsed time would make the two disagree for reasons
 *     that are not bugs, and the oracle would have to learn to ignore it.
 *   - Speed. Recompiled code does not run at PSP speed, so host time is not the
 *     game's time in any case.
 *
 * It advances two ways. A vblank moves it a frame, which is what makes elapsed
 * time roughly track frames the way a game expects. A *read* also moves it a
 * little, because nothing here advances on its own, and
 *
 *     start = sceKernelGetSystemTimeLow();
 *     while (sceKernelGetSystemTimeLow() - start < 1000) { }
 *
 * against a clock that only moved on vblank would never leave the loop. This is
 * the same trade src/hle/display.c already makes for the scanline counters, and
 * it is made for the same reason: unfaithful timing beats a hang.
 */
#ifndef PSPRECOMP_CLOCK_H
#define PSPRECOMP_CLOCK_H

#include <stdint.h>

/* 59.94 Hz, rounded to whole microseconds.  Display counters and vblank waits
 * must share the same grid or a game comparing them observes two clocks. */
#define PSP_CLOCK_FRAME_US 16667u

#ifdef __cplusplus
extern "C" {
#endif

void psp_clock_reset(void);

/* Microseconds since the module started. Advances the clock by a tick, so two
 * reads never return the same value. In real-time mode the tick is replaced by
 * elapsed wall time, which passes on its own; two reads inside one instant can
 * then agree, which is the honest report rather than an invented step. */
uint64_t psp_clock_read(void);

/* Advance the clock by a tick without reading it. Firmware calls charge one,
 * so a thread spinning on calls that neither read the clock nor wait for a
 * vblank still lets time pass -- otherwise it starves every sleeping thread by
 * staying runnable forever. Real-time mode adopts wall time here rather than
 * going no-op: wall time reaches only threads that sleep or ask. */
void psp_clock_tick(void);

/* Microseconds since the module started, without advancing it. For diagnostics
 * that should not perturb what they measure. */
uint64_t psp_clock_peek(void);

/* The absolute moment of the next vblank, strictly in the future. Every thread
 * waiting for a frame is waiting for the same one, so the vblank handlers park
 * on this rather than each adding a frame to the clock. Does not advance it;
 * psp_clock_advance_to does that once the wait is over. */
uint64_t psp_clock_next_frame(void);

/* Move the clock forward to `us` if it is not already past it. Never moves it
 * back. The scheduler uses this when every thread is asleep: time here only
 * advances because the guest asked it to, so a run where nothing is runnable
 * would otherwise wait forever for a moment that cannot arrive on its own. */
void psp_clock_advance_to(uint64_t us);

/* Run the clock against wall time instead of guest events.
 *
 * Off (the default) the clock is the deterministic virtual clock the oracle
 * depends on, and everything above stands. On, the clock anchors to
 * CLOCK_MONOTONIC at the moment this is called: a vblank lasts a frame of
 * wall time (psp_clock_advance_to sleeps out the remainder), the idle scheduler
 * waits out the sleep it would otherwise skip, and a read reports elapsed
 * wall time instead of inventing a tick. Guest time and wall time then track
 * one to one, which is what audio output and a paced frame loop need.
 *
 * A compute-bound run never sleeps at all -- falling behind the wall is the
 * ordinary case and degrades to running flat out, exactly as with the mode
 * off. The mode is off unless the host asks for it, which keeps the
 * differential oracle on the deterministic clock it depends on. */
void psp_clock_realtime(int enable);
int  psp_clock_is_realtime(void);

/* Paced mode: the deterministic virtual clock, presented at the wall's pace.
 *
 * Real-time mode makes the guest see the host: every read adopts wall time,
 * every wait ends when a wall deadline passes, and audio waits on the host
 * device. Two runs of the same input then differ wherever the host's timing
 * did, which is why a recorded session drifted off its line within seconds
 * of replaying. Paced mode keeps everything the guest sees on the virtual
 * clock -- reads tick, idle jumps to the next deadline -- and only the jump
 * waits for the wall to catch up, so a session played in it is a function of
 * its input alone and a recording of it replays exactly in another paced run.
 * (A headless run keeps the per-call vblank count its gate baselines were
 * made with, so it is not one.) When the host falls more than
 * PSP_CLOCK_PACED_SLIP_US behind, the anchor moves rather than the guest
 * racing to catch up: the game slows down instead of dropping time. Enabling
 * it turns real-time mode off. */
#define PSP_CLOCK_PACED_SLIP_US 100000u
void psp_clock_paced(int enable);
int  psp_clock_is_paced(void);
/* Whether this run asked for paced mode: PSPRECOMP_PACED=1 or 0 decides; a
 * run that plays or records a pad scenario (PSPRECOMP_REPLAY,
 * PSPRECOMP_REPLAY_REC) is paced, so its timing is the scenario's; and
 * otherwise the player's preference (its TIMING setting, given to
 * psp_clock_paced_preference before the window opens) decides. The host
 * consults this where it would otherwise turn real-time mode on. */
int  psp_clock_paced_requested(void);
void psp_clock_paced_preference(int paced);

/* A non-perturbing end-of-run measurement of the real-time (or paced)
 * mapping. Returns zero while the unpaced virtual clock is selected. `guest_us` is the
 * latest time adopted by guest execution and `wall_us` is elapsed monotonic
 * time since the mode was enabled, host pauses included; less
 * psp_clock_held_us, their difference exposes pacing drift without making a
 * clock read change the value being measured. */
int psp_clock_realtime_stats(uint64_t *guest_us, uint64_t *wall_us);

/* A host pause (psprecomp/safepoint.h). While held, guest time does not
 * move; on release the real-time origin moves forward by the time held, so
 * neither vblank nor the timers catch up, and the run is exactly as far
 * ahead of or behind the wall as it was. Called by the holding thread. */
void     psp_clock_hold(void);
void     psp_clock_release(void);
/* Wall time spent held, in microseconds, the current hold included. */
uint64_t psp_clock_held_us(void);
/* Monotonic nanoseconds less every hold, frozen while held: the wall time
 * the guest has had. For host measurements of guest-facing intervals --
 * gaps between audio outputs, a movie's fetch rate -- which a pause would
 * otherwise read as a stall. Any thread. */
uint64_t psp_clock_run_ns(void);
/* Wall nanoseconds the real-time and paced pacers have slept in all. */
uint64_t psp_clock_slept_ns(void);

#ifdef __cplusplus
}
#endif

#endif /* PSPRECOMP_CLOCK_H */
