/* Shared subinterrupt delivery. All calls require the guest scheduler token. */
#ifndef PSPRECOMP_INTERRUPT_H
#define PSPRECOMP_INTERRUPT_H

#include <stdint.h>

#define PSP_INTERRUPT_VBLANK 30u

#ifdef __cplusplus
extern "C" {
#endif

void psp_interrupt_reset(void);
void psp_interrupt_register(void);
/* Host module metadata, supplied after reset. Ranges are half-open. Handler
 * GP comes from its owning module, not the interrupted/registration thread. */
int psp_interrupt_set_module(uint32_t lo, uint32_t hi, uint32_t gp);
uint32_t psp_interrupt_handler_gp(uint32_t handler);
/* Queue events without entering guest code; safe before an HLE mutation. */
void psp_interrupt_raise(unsigned intr, uint64_t count);
/* Deliver after an HLE result is complete, never under the scheduler lock. */
void psp_interrupt_run_pending(void);
int psp_interrupt_in_handler(void);
int psp_interrupt_enabled(void);
/* Borrow the current CPU for a device interrupt. Returns zero if delivery is
 * masked or already inside an interrupt; the device must retain the event. */
int psp_interrupt_call(uint32_t handler, uint32_t gp,
                       uint32_t a0, uint32_t a1, uint32_t a2);
/* The same, whatever the mask: for a device that has decided the moment
 * itself (the GE's handlers, src/hle/ge.c). Nests inside a handler. */
void psp_interrupt_call_now(uint32_t entry, uint32_t gp,
                            uint32_t a0, uint32_t a1, uint32_t a2);
/* Next deliverable display event, or zero if no handler can wake the CPU. */
uint64_t psp_interrupt_next_event(void);

#ifdef __cplusplus
}
#endif

#endif
