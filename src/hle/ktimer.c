/* psprecomp — the kernel objects that fire a handler at a moment.
 *
 * An alarm is not a wait. Nothing blocks on it: the guest hands the kernel a
 * deadline and a function, and the kernel calls that function *from no thread
 * at all* when the deadline passes. threads/alarm/alarm prints
 * `alarmHandler called: 00001234 on thread -1`, and the -1 is the point --
 * the handler asks which thread it is on and the answer is neither of the two
 * that exist.
 *
 * ## Where a deadline is noticed
 *
 * There is no timer and no interrupt. Guest time only moves when the guest
 * moves it, which it does at every firmware call (clock.c), so a firmware call
 * is also the only moment at which a deadline can be observed to have passed.
 * psp_ktimer_tick runs there, and it is the same trade the scheduler already
 * makes for sleeping threads: the instant is not exact, but it arrives, and it
 * arrives in the same place on every run.
 *
 * The handler runs through psp_dispatch, so it is recompiled code in the boot
 * host and interpreted under the test harness, with no special case for either.
 */

#include "psprecomp/hle.h"
#include "psprecomp/clock.h"
#include "psprecomp/dispatch.h"
#include "psprecomp/mem.h"
#include "psprecomp/sched.h"

#include <string.h>

#define MAX_ALARMS 32

typedef struct {
    uint32_t uid;
    uint64_t schedule;    /* guest microsecond at which it fires */
    uint32_t handler;
    uint32_t common;
    int      alive;
} psp_alarm;

static psp_alarm g_alarm[MAX_ALARMS];
/* Guards against an alarm handler that sets or cancels alarms re-entering the
 * scan it was called from. One pass at a time; anything armed inside a handler
 * is picked up by the next firmware call. */
static int g_firing;

void psp_ktimer_reset(void) {
    memset(g_alarm, 0, sizeof g_alarm);
    g_firing = 0;
}

static psp_alarm *find_alarm(uint32_t uid) {
    for (int i = 0; i < MAX_ALARMS; i++)
        if (g_alarm[i].alive && g_alarm[i].uid == uid) return &g_alarm[i];
    return NULL;
}

static uint32_t alarm_arm(uint64_t usec, uint32_t handler, uint32_t common) {
    /* A null handler is refused, and with the *size* code rather than an
     * address one -- `NULL handler: Alarm: Failed (800200D3)`. */
    if (!handler) { return 0; }
    psp_alarm *a = NULL;
    for (int i = 0; i < MAX_ALARMS; i++) if (!g_alarm[i].alive) { a = &g_alarm[i]; break; }
    if (!a) return 0;
    memset(a, 0, sizeof *a);
    a->schedule = psp_clock_peek() + usec;
    a->handler  = handler;
    a->common   = common;
    a->uid      = psp_threadman_next_uid();
    a->alive    = 1;
    return a->uid;
}

static void hle_SetAlarm(void) {
    /* (usec, handler, common) */
    if (!psp_arg(1)) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_SIZE); return; }
    const uint32_t uid = alarm_arm(psp_arg(0), psp_arg(1), psp_arg(2));
    psp_ret(uid ? uid : SCE_KERNEL_ERROR_NO_MEMORY);
}

/* The same call with the delay as a 64-bit SceKernelSysClock rather than a
 * microsecond count in a register. */
static void hle_SetSysClockAlarm(void) {
    const uint32_t clk = psp_arg(0);
    if (!psp_arg(1)) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_SIZE); return; }
    if (!clk || !psp_mem_ptr(clk, 8)) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_SIZE); return; }
    const uint64_t usec = (uint64_t)psp_read32(clk) |
                          ((uint64_t)psp_read32(clk + 4) << 32);
    const uint32_t uid = alarm_arm(usec, psp_arg(1), psp_arg(2));
    psp_ret(uid ? uid : SCE_KERNEL_ERROR_NO_MEMORY);
}

static void hle_CancelAlarm(void) {
    psp_alarm *a = find_alarm(psp_arg(0));
    if (!a) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_ALMID); return; }
    a->alive = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* Writes exactly as many bytes as the caller says it has room for, not the
 * whole structure and not nothing.
 *
 * refer.expected sweeps the size field one byte at a time and prints what
 * arrived: at 1 the `size` field alone reads back as 20, because one byte of
 * the little-endian 0x14 is the whole of it; at 5 the first byte of the
 * schedule has landed too. A refer that wrote all 20 bytes regardless would
 * differ on every line of that sweep, and one that wrote none would differ on
 * the rest. */
static void hle_ReferAlarmStatus(void) {
    const psp_alarm *a = find_alarm(psp_arg(0));
    const uint32_t info = psp_arg(1);
    if (!a)    { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_ALMID); return; }
    if (!info) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }

    uint8_t buf[20];
    const uint32_t words[] = {
        20, (uint32_t)a->schedule, (uint32_t)(a->schedule >> 32),
        a->handler, a->common,
    };
    for (int w = 0; w < 5; w++)
        for (int b = 0; b < 4; b++) buf[w * 4 + b] = (uint8_t)(words[w] >> (b * 8));

    uint32_t room = psp_read32(info);
    if (room > sizeof buf) room = sizeof buf;
    for (uint32_t i = 0; i < room; i++) psp_write8(info + i, buf[i]);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* Fire whatever is due. Called from the firmware-call path.
 *
 * The handler's return value is a *rescheduling* interval: non-zero re-arms the
 * alarm that many microseconds later, zero retires it. That is what lets a
 * guest write a periodic timer with one call and no thread. */
void psp_ktimer_tick(void) {
    if (g_firing) return;
    const uint64_t now = psp_clock_peek();

    for (int i = 0; i < MAX_ALARMS; i++) {
        psp_alarm *a = &g_alarm[i];
        if (!a->alive || a->schedule > now) continue;

        const uint32_t handler = a->handler, common = a->common;
        const uint32_t uid = a->uid;
        g_firing = 1;

        /* The handler is guest code and gets a guest register file. Saved and
         * restored around it because it runs *between* two instructions of
         * whatever thread happened to make the firmware call, and that thread
         * must not be able to tell. */
        const psp_cpu_state saved = psp_cpu;
        psp_cpu.r[PSP_REG_A0] = common;
        psp_cpu.r[PSP_REG_RA] = 0;
        psp_dispatch(handler);
        const uint32_t again = psp_cpu.r[PSP_REG_V0];
        psp_cpu = saved;

        g_firing = 0;

        a = find_alarm(uid);
        if (!a) continue;                 /* cancelled itself */
        if (again) a->schedule = psp_clock_peek() + again;
        else       a->alive = 0;
    }
}

void psp_ktimer_register(void) {
    psp_hle_register(0x6652B8CA, "ThreadManForUser", "sceKernelSetAlarm",         hle_SetAlarm);
    psp_hle_register(0xB2C25152, "ThreadManForUser", "sceKernelSetSysClockAlarm", hle_SetSysClockAlarm);
    psp_hle_register(0x7E65B999, "ThreadManForUser", "sceKernelCancelAlarm",      hle_CancelAlarm);
    psp_hle_register(0xDAA3F564, "ThreadManForUser", "sceKernelReferAlarmStatus", hle_ReferAlarmStatus);
}
