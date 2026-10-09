/* Subinterrupt host implementation reconstructed from BSD PSPSDK declarations
 * and our independently authored instruction/API probes. See
 * tests/provenance/intr/ for recorded observations and limitations. Historical
 * source exposure remains in the parent project's provenance audit. */
#include "psprecomp/interrupt.h"
#include "psprecomp/hle.h"
#include "psprecomp/clock.h"
#include "psprecomp/dispatch.h"
#include "psprecomp/mem.h"
#include "psprecomp/vfpu.h"
#include "psprecomp/state.h"
#include <string.h>

/* SDK IDs run through 66; the probe checks both ends and subindices 31/32.
 * Module capacity and delivery budget below are host resource policies. */
#define IRQ_COUNT 67u
#define SUB_COUNT 32u
#define MODULE_CAPACITY 32u
#define DELIVERY_BUDGET 64u

typedef struct {
    uint32_t entry, common;
    uint64_t generation, pending;
    int enabled;
} irq_subscription;
typedef struct { uint32_t lo, hi, gp; } irq_module;
static irq_subscription subscriptions[IRQ_COUNT][SUB_COUNT];
static irq_module modules[MODULE_CAPACITY];
static unsigned module_count;
static uint64_t generation;
static int cpu_enabled = 1, in_handler, servicing;

/* Numeric categories independently documented in PSPSDK pspkerror.h. */
enum {
    IRQ_INVALID = 0x80020065u,
    IRQ_EXISTS = 0x80020067u,
    IRQ_MISSING = 0x80020068u
};

void psp_interrupt_reset(void) {
    memset(subscriptions, 0, sizeof subscriptions);
    memset(modules, 0, sizeof modules);
    module_count = 0;
    generation = 0;
    cpu_enabled = 1;
    in_handler = servicing = 0;
}

int psp_interrupt_set_module(uint32_t lo, uint32_t hi, uint32_t gp) {
    if (hi <= lo) return -1;
    for (unsigned i = 0; i < module_count; i++) {
        if (modules[i].lo == lo && modules[i].hi == hi) {
            modules[i].gp = gp;
            return 0;
        }
    }
    if (module_count == MODULE_CAPACITY) return -1;
    modules[module_count++] = (irq_module){lo, hi, gp};
    return 0;
}

uint32_t psp_interrupt_handler_gp(uint32_t handler) {
    for (unsigned i = module_count; i > 0; i--)
        if (handler >= modules[i-1].lo && handler < modules[i-1].hi)
            return modules[i-1].gp;
    return 0;
}

int psp_interrupt_enabled(void) { return cpu_enabled; }
/* The name the rest of the runtime asks by (hle.h). */
int psp_intr_enabled(void) { return cpu_enabled; }
int psp_interrupt_in_handler(void) { return in_handler; }

static irq_subscription *argument_slot(void) {
    unsigned irq = psp_arg(0), sub = psp_arg(1);
    if (irq >= IRQ_COUNT || sub >= SUB_COUNT) {
        psp_ret(IRQ_INVALID);
        return NULL;
    }
    return &subscriptions[irq][sub];
}
static void register_handler(void) {
    irq_subscription *slot = argument_slot();
    if (!slot) return;
    if (slot->entry) { psp_ret(IRQ_EXISTS); return; }
    *slot = (irq_subscription){.entry = psp_arg(2), .common = psp_arg(3),
                               .generation = ++generation};
    psp_ret(0);
}
static void release_handler(void) {
    irq_subscription *slot = argument_slot();
    if (!slot) return;
    if (!slot->entry) { psp_ret(IRQ_MISSING); return; }
    memset(slot, 0, sizeof *slot);
    psp_ret(0);
}
static void enable_handler(void) {
    irq_subscription *slot = argument_slot();
    if (!slot) return;
    slot->enabled = 1;
    psp_ret(0);
}
static void disable_handler(void) {
    irq_subscription *slot = argument_slot();
    if (!slot) return;
    slot->enabled = 0;
    slot->pending = 0;
    psp_ret(0);
}
static void suspend_cpu(void) {
    const int previous = cpu_enabled;
    cpu_enabled = 0;
    psp_ret(previous);
}
static void resume_cpu(void) { cpu_enabled = psp_arg(0) != 0; psp_ret(0); }
static void query_enabled(void) { psp_ret(cpu_enabled); }
static void query_suspended(void) { psp_ret(psp_arg(0) == 0); }
static void query_context(void) { psp_ret(in_handler || psp_ktimer_in_handler()); }

/* The probe's masked interval does not replay missed Vblanks on resume.
 * Events observed while a subscription is inactive therefore do not become
 * a backlog for a later subscription. */
void psp_interrupt_raise(unsigned irq, uint64_t count) {
    if (irq >= IRQ_COUNT || !count || !cpu_enabled) return;
    for (unsigned sub = 0; sub < SUB_COUNT; sub++) {
        irq_subscription *slot = &subscriptions[irq][sub];
        if (!slot->entry || !slot->enabled) continue;
        if (UINT64_MAX - slot->pending < count) slot->pending = UINT64_MAX;
        else slot->pending += count;
    }
}

/* Borrow the native runtime's CPU at a completed HLE boundary. A handler
 * must not corrupt a suspended C activation's guest registers or switch its
 * scheduler token. The scheduler tests in_handler before switching. A
 * handler outside every module the host registered keeps the interrupted
 * thread's gp, which is right for a game of one module. */
static void deliver(uint32_t entry, uint32_t gp, uint32_t a0, uint32_t a1, uint32_t a2) {
    const psp_cpu_state saved = psp_cpu;
    uint32_t controls[16];
    for (unsigned i = 0; i < 16; i++) controls[i] = psp_mfvc(i);
    const int saved_enabled = cpu_enabled, saved_in = in_handler;
    in_handler = 1;
    psp_cpu.r[PSP_REG_A0] = a0;
    psp_cpu.r[PSP_REG_A1] = a1;
    psp_cpu.r[PSP_REG_A2] = a2;
    if (gp) psp_cpu.r[PSP_REG_GP] = gp;
    psp_cpu.r[PSP_REG_RA] = 0;
    if (psp_cpu.r[PSP_REG_SP] >= 16)
        psp_cpu.r[PSP_REG_SP] = (psp_cpu.r[PSP_REG_SP] - 16) & ~15u;
    psp_nest_enter(PSP_NEST_INTERRUPT, entry);
    psp_dispatch(entry);
    psp_nest_leave();
    psp_cpu = saved;
    for (unsigned i = 0; i < 16; i++) psp_mtvc(i, controls[i]);
    cpu_enabled = saved_enabled;
    in_handler = saved_in;
}

int psp_interrupt_call(uint32_t entry, uint32_t gp,
                       uint32_t a0, uint32_t a1, uint32_t a2) {
    if (!cpu_enabled || in_handler) return 0;
    if (entry) deliver(entry, gp, a0, a1, a2);
    return 1;
}

void psp_interrupt_call_now(uint32_t entry, uint32_t gp,
                            uint32_t a0, uint32_t a1, uint32_t a2) {
    if (entry) deliver(entry, gp, a0, a1, a2);
}

void psp_interrupt_run_pending(void) {
    if (!cpu_enabled || in_handler || servicing) return;
    servicing = 1;
    unsigned budget = DELIVERY_BUDGET;
    for (unsigned irq = 0; irq < IRQ_COUNT && budget; irq++) {
        for (unsigned sub = 0; sub < SUB_COUNT && budget; sub++) {
            irq_subscription *slot = &subscriptions[irq][sub];
            /* Snapshot this subscription's work: events raised by a handler
             * cannot recursively re-enter it; replaced handlers cannot inherit
             * work from the old generation. */
            const uint64_t serial = slot->generation;
            uint64_t remaining = slot->pending;
            while (remaining && budget && slot->entry && slot->enabled && cpu_enabled
                   && slot->generation == serial) {
                remaining--;
                slot->pending--;
                budget--;
                psp_interrupt_call(slot->entry, psp_interrupt_handler_gp(slot->entry),
                                   sub, slot->common, 0);
            }
        }
    }
    /* Device queues retain their own completion events while CPU delivery is
     * masked. Resume them at the same completed HLE boundary as subscriptions. */
    psp_ge_run_pending_callbacks();
    servicing = 0;
}

uint64_t psp_interrupt_next_event(void) {
    if (!cpu_enabled || in_handler) return 0;
    for (unsigned sub = 0; sub < SUB_COUNT; sub++)
        if (subscriptions[PSP_INTERRUPT_VBLANK][sub].entry &&
            subscriptions[PSP_INTERRUPT_VBLANK][sub].enabled)
            return psp_clock_next_frame();
    return 0;
}

void psp_interrupt_register(void) {
    /* A save state's (psprecomp/state.h): the modules too, since a title
     * can load more of them than the boot does. */
    PSP_STATE_KEEP(subscriptions);
    PSP_STATE_KEEP(modules);
    PSP_STATE_KEEP(module_count);
    PSP_STATE_KEEP(generation);
    PSP_STATE_KEEP(cpu_enabled);
#define BIND(nid, library, name, function) psp_hle_register(nid, library, name, function)
    BIND(0xCA04A2B9, "InterruptManager", "sceKernelRegisterSubIntrHandler", register_handler);
    BIND(0xD61E6961, "InterruptManager", "sceKernelReleaseSubIntrHandler", release_handler);
    BIND(0xFB8E22EC, "InterruptManager", "sceKernelEnableSubIntr", enable_handler);
    BIND(0x8A389411, "InterruptManager", "sceKernelDisableSubIntr", disable_handler);
    BIND(0x092968F4, "Kernel_Library", "sceKernelCpuSuspendIntr", suspend_cpu);
    BIND(0x5F10D406, "Kernel_Library", "sceKernelCpuResumeIntr", resume_cpu);
    BIND(0x3B84732D, "Kernel_Library", "sceKernelCpuResumeIntrWithSync", resume_cpu);
    BIND(0x47A0B729, "Kernel_Library", "sceKernelIsCpuIntrSuspended", query_suspended);
    BIND(0xB55249D2, "Kernel_Library", "sceKernelIsCpuIntrEnable", query_enabled);
    BIND(0xFE28C6D9, "InterruptManagerForKernel", "sceKernelIsIntrContext", query_context);
#undef BIND
}
