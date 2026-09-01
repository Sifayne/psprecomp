/* psprecomp — the guest clock. See include/psprecomp/clock.h. */

#include "psprecomp/clock.h"

/* 59.94Hz, the PSP's refresh rate, rounded to whole microseconds. */
#define PSP_FRAME_US 16667u

/* How far a read moves the clock.
 *
 * Big enough that a poll loop counting microseconds finishes in a sane number
 * of iterations, small enough that a game measuring a real interval does not
 * see it pass instantly. At 100us a millisecond costs ten reads. */
#define PSP_READ_TICK_US 100u

static uint64_t g_us;

void psp_clock_reset(void) { g_us = 0; }

uint64_t psp_clock_peek(void) { return g_us; }

uint64_t psp_clock_read(void) {
    const uint64_t now = g_us;
    g_us += PSP_READ_TICK_US;
    return now;
}

void psp_clock_frame(void) { g_us += PSP_FRAME_US; }

void psp_clock_advance_to(uint64_t us) { if (us > g_us) g_us = us; }
