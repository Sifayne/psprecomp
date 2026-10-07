/* The safe point and the host pause (docs/PLAYER-LAYER.md §2).
 *
 * The safe point is a firmware call that marks a frame boundary -- by
 * default the controller reads, which each title makes once a frame --
 * completed by the thread that drives the GE, from its own guest code, not
 * nested inside a callback or an interrupt handler. It is taken after the
 * call's handler and before the scheduling, interrupts and timers that
 * follow it. There the calling thread has finished its call and holds the
 * scheduler token, and every other guest thread is parked inside a firmware
 * call, since a thread only gives the token up in one.
 *
 * A pause holds the guest there: the thread keeps the token, so nothing of
 * the guest runs; the guest clock is held (psp_clock_hold), so on release
 * neither vblank nor the timers catch up; and a redraw hook keeps the window
 * alive. The host silences its audio output meanwhile without draining it.
 * Save states (stages 8 and 9) and the park census are taken at the same
 * point. */
#ifndef PSPRECOMP_SAFEPOINT_H
#define PSPRECOMP_SAFEPOINT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The frame-boundary calls, by firmware name, NULL-terminated; NULL restores
 * the default, the controller reads. A title whose frame does not read the
 * controller names its own. Before the guest starts. */
void psp_safepoint_set_calls(const char *const *names);

/* Ask the guest to be held at the next safe point, or let it go. Any
 * thread: the SDL thread asks for a pause, the overlay will. */
void psp_pause_request(int hold);
int  psp_pause_requested(void);
/* Whether the guest is held now. */
int  psp_paused(void);
/* How many holds have ended, which a host that redraws or reports can poll. */
uint64_t psp_pause_count(void);

/* Called on the holding thread about once a display refresh while held: a
 * GL backend recomposes the last presented frame there, since its context
 * belongs to that thread. */
void psp_pause_set_redraw(void (*redraw)(void));

/* PSPRECOMP_PAUSE_AT=<poll>:<ms>[,<poll>:<ms>...] holds the guest for <ms>
 * of wall time at the first safe point at or after each <poll>, for checks
 * that a pause is invisible. Read when the scheduler starts. */
void psp_safepoint_init(void);

#ifdef __cplusplus
}
#endif

#endif
