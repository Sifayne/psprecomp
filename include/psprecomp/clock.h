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
 * reads never return the same value. */
uint64_t psp_clock_read(void);

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

#ifdef __cplusplus
}
#endif

#endif /* PSPRECOMP_CLOCK_H */
