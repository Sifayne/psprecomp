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

/* Move the clock on by one frame. Called from the vblank handlers. */
void psp_clock_frame(void);

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
 * wall time (psp_clock_frame sleeps out the remainder), the idle scheduler
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

#ifdef __cplusplus
}
#endif

#endif /* PSPRECOMP_CLOCK_H */
