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
 * moves it, which it does at every firmware call (clock.c), so while a thread
 * runs, a firmware call is the only moment at which a deadline can be
 * observed to have passed. psp_ktimer_tick runs there, and it is the same
 * trade the scheduler already makes for sleeping threads: the instant is not
 * exact, but it arrives, and it arrives in the same place on every run.
 *
 * The other moment is when no thread runs at all. The scheduler then moves
 * the clock to the next deadline, and when that is a timer's it stops there
 * and runs the handler (psp_ktimer_next_due, psp_ktimer_fire_idle), as the
 * interrupt would have: threadprobe step 115 (fw 6.60) has a 2ms alarm fire
 * inside a 10ms delay, where waiting for the next firmware call ran it at the
 * delay's end.
 *
 * The handler runs through psp_dispatch, so it is recompiled code in the boot
 * host and interpreted under the test harness, with no special case for either.
 */

#include "psprecomp/hle.h"
#include "psprecomp/clock.h"
#include "psprecomp/mem.h"
#include "psprecomp/sched.h"
#include "psprecomp/interrupt.h"

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

int psp_ktimer_in_handler(void) { return g_firing; }

/* Both halves of this file are reset and ticked together; the vtimer side is
 * defined below. */
static void vtimer_reset(void);

void psp_ktimer_reset(void) {
    memset(g_alarm, 0, sizeof g_alarm);
    vtimer_reset();
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
    /* At least a microsecond out. The handler is an interrupt, and it cannot
     * arrive before the call that armed it has returned: threadprobe step 119
     * (fw 6.60) reads the hit count straight after SetAlarm(0) as 0, and 1
     * after a 1ms delay. Due at once, the tick at the end of this very call
     * fired it. */
    a->schedule = psp_clock_peek() + (usec ? usec : 1);
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

/* Fire whatever is due, and say how many handlers ran.
 *
 * The handler's return value is a *rescheduling* interval: non-zero re-arms the
 * alarm that many microseconds later, zero retires it. That is what lets a
 * guest write a periodic timer with one call and no thread. Later than the
 * moment it was *due*, not the moment it ran: threadprobe step 116 (fw 6.60)
 * has a handler return 1000 twice, and the first gap between its runs comes
 * out under 1000us -- a late first run does not push the next one back. */
static int vtimer_tick(void);

static int fire_due(void) {
    /* Not inside another interrupt's handler, and not while the CPU has
     * interrupts suspended: the timer's interrupt waits for the resume. */
    if (g_firing || psp_interrupt_in_handler() || !psp_interrupt_enabled()) return 0;
    const uint64_t now = psp_clock_peek();
    int fired = 0;

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
        const uint32_t again = psp_call_guest(handler, &common, 1);

        g_firing = 0;
        fired++;

        a = find_alarm(uid);
        if (!a) continue;                 /* cancelled itself */
        if (again) a->schedule += again;
        else       a->alive = 0;
    }
    return fired + vtimer_tick();
}

/* Called from the firmware-call path. A handler that readied a thread more
 * urgent than the one it interrupted hands it the CPU on the way out, as the
 * end of an interrupt does; the reschedule that ran just before this call's
 * timers could not have known. */
void psp_ktimer_tick(void) {
    if (fire_due()) psp_sched_tick();
}

/* For the scheduler's idle path: nothing is runnable and the clock has just
 * been moved to psp_ktimer_next_due(). */
int psp_ktimer_fire_idle(void) { return fire_due(); }

static void alarm_list(int type, uint32_t out, int max, int *count) {
    if (type != PSP_TMID_ALARM) return;
    for (int i = 0; i < MAX_ALARMS; i++) {
        if (!g_alarm[i].alive) continue;
        psp_threadman_list_put(g_alarm[i].uid, out, max, count);
    }
}

void psp_ktimer_register(void) {
    psp_threadman_add_lister(alarm_list);
    psp_hle_register(0x6652B8CA, "ThreadManForUser", "sceKernelSetAlarm",         hle_SetAlarm);
    psp_hle_register(0xB2C25152, "ThreadManForUser", "sceKernelSetSysClockAlarm", hle_SetSysClockAlarm);
    psp_hle_register(0x7E65B999, "ThreadManForUser", "sceKernelCancelAlarm",      hle_CancelAlarm);
    psp_hle_register(0xDAA3F564, "ThreadManForUser", "sceKernelReferAlarmStatus", hle_ReferAlarmStatus);
    psp_ktimer_register_vtimer();
}

/* ---- vtimer: a stopwatch that can also fire ---------------------------------
 *
 * A vtimer counts guest microseconds, but only while it is *running*, and the
 * count is the guest's to set. So there are two quantities and they are not the
 * same one twice: `base` is the system time the timer was last started or had
 * its value set from, and `current` is the value it reads now -- base plus the
 * time since, or a frozen number while stopped.
 *
 * It can also carry a handler with a schedule, which fires the same way an
 * alarm does and through the same tick.
 */

/* Every `Create 1024` case in the suite builds a thousand objects in a loop and
 * expects the thousandth to succeed, so a cap below that is not a resource
 * limit being modelled -- it is ours, and it shows up as `Failed at 128`. The
 * headroom above 1024 is because the process already holds some: callbacks
 * stopped at 1023 with the cap at exactly 1024. Hardware's real ceiling is
 * higher and is measured nowhere here. */
#define MAX_VTIMERS 2048

typedef struct {
    uint32_t uid;
    char     name[32];
    int      active;
    uint64_t value;        /* the count as of `since` */
    uint64_t since;        /* guest time the count was last anchored */
    /* What `base` reports, which is *not* the anchor. A timer that has never
     * been started reports 0 -- create.expected and sethandler.expected both
     * print `base=0` for one -- so this is the timer's own zero point rather
     * than a reading of the system clock. */
    uint64_t base;
    uint64_t schedule;     /* timer value at which the handler fires, 0 = none */
    uint32_t handler, common;
    int      alive;
} psp_vtimer;

static psp_vtimer g_vtimer[MAX_VTIMERS];
/* One past the highest slot ever used. vtimer_tick runs at every firmware call
 * -- that is the whole design, see the note at the top of this file -- so the
 * scan is on the hottest path there is, and sizing the table for the thousand
 * vtimers threads/vtimers/create makes turned it into 2048 iterations per
 * call. utility/msgdialog stopped finishing at all. */
static int g_vtimer_hi;

static void vtimer_reset(void) { memset(g_vtimer, 0, sizeof g_vtimer); g_vtimer_hi = 0; }

static psp_vtimer *find_vtimer(uint32_t uid) {
    for (int i = 0; i < g_vtimer_hi; i++)
        if (g_vtimer[i].alive && g_vtimer[i].uid == uid) return &g_vtimer[i];
    return NULL;
}

/* What the timer reads now: frozen while stopped, running on otherwise. */
static uint64_t vtimer_now(const psp_vtimer *v) {
    return v->active ? v->value + (psp_clock_peek() - v->since) : v->value;
}

/* Setting the count re-anchors a running timer. A stopped one keeps base 0:
 * threadprobe step 124 (fw 6.60) sets the time of a stopped timer and reads
 * BaseWide 0 and a refer with base=0; this used to move base to "now". */
static void vtimer_set(psp_vtimer *v, uint64_t to) {
    v->value = to;
    v->since = psp_clock_peek();
    if (v->active) v->base = v->since;
}

static void hle_CreateVTimer(void) {
    if (!psp_arg(0)) { psp_ret(SCE_KERNEL_ERROR_ERROR); return; }
    psp_vtimer *v = NULL;
    int vi = 0;
    for (; vi < MAX_VTIMERS; vi++) if (!g_vtimer[vi].alive) { v = &g_vtimer[vi]; break; }
    if (v && vi >= g_vtimer_hi) g_vtimer_hi = vi + 1;
    if (!v) { psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }
    memset(v, 0, sizeof *v);
    psp_str(psp_arg(0), v->name, sizeof v->name);
    v->uid   = psp_threadman_next_uid();
    v->alive = 1;
    v->since = psp_clock_peek();
    psp_ret(v->uid);
}

static void hle_DeleteVTimer(void) {
    /* Not from inside a handler. vtimers/delete has a handler delete the very
     * timer it is running for and gets ILLEGAL_CONTEXT -- which is the code
     * for a call made where it is not allowed to be made, so the context is
     * what disqualifies it rather than the target. Deleting some *other*
     * vtimer from a handler is not measured anywhere. */
    if (g_firing) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_CONTEXT); return; }
    psp_vtimer *v = find_vtimer(psp_arg(0));
    if (!v) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_VTID); return; }
    v->alive = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* Start and stop report whether they *changed* anything, not whether they
 * succeeded: starting an already-running timer answers 1. */
static void hle_StartVTimer(void) {
    if (!psp_arg(0)) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_VTID); return; }
    psp_vtimer *v = find_vtimer(psp_arg(0));
    if (!v) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_VTID); return; }
    const int was = v->active;
    if (!was) { v->since = psp_clock_peek(); v->base = v->since; v->active = 1; }
    psp_ret((uint32_t)was);
}

static void hle_StopVTimer(void) {
    if (!psp_arg(0)) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_VTID); return; }
    psp_vtimer *v = find_vtimer(psp_arg(0));
    if (!v) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_VTID); return; }
    const int was = v->active;
    /* Stopping clears the base. It is set at the start and only means anything
     * while the timer runs: sethandler prints `base=0` beside every `active=0`
     * and a real reading beside every `active=1`, and vtimers/stop starts and
     * stops three times and then reads a base of 0. */
    if (was) { v->value = vtimer_now(v); v->base = 0; v->active = 0; }
    psp_ret((uint32_t)was);
}

/* The 64-bit reads come back in $v0:$v1, low word first -- the o32 convention
 * the caller was compiled against, the same as sceKernelGetSystemTimeWide. */
static void ret64(uint64_t v) {
    psp_cpu.r[PSP_REG_V0] = (uint32_t)v;
    psp_cpu.r[PSP_REG_V1] = (uint32_t)(v >> 32);
}

/* The wide reads have no room for an error code -- their whole return value is
 * the number -- so failure is all-ones. getbase.expected prints
 * `Wrong value - ffffffffffffffff` for a NULL timer, where returning the
 * ordinary 0x800201BE in the low word reads as a plausible time. */
#define VTIMER_WIDE_FAIL 0xFFFFFFFFFFFFFFFFull

static void hle_GetVTimerTimeWide(void) {
    const psp_vtimer *v = find_vtimer(psp_arg(0));
    if (!v) { ret64(VTIMER_WIDE_FAIL); return; }
    ret64(vtimer_now(v));
}

static void hle_GetVTimerTime(void) {
    const psp_vtimer *v = find_vtimer(psp_arg(0));
    const uint32_t out = psp_arg(1);
    if (!v) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_VTID); return; }
    const uint64_t t = vtimer_now(v);
    if (out) { psp_write32(out, (uint32_t)t); psp_write32(out + 4, (uint32_t)(t >> 32)); }
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* base is where the count was last anchored, which start and set both move. */
static void hle_GetVTimerBaseWide(void) {
    const psp_vtimer *v = find_vtimer(psp_arg(0));
    if (!v) { ret64(VTIMER_WIDE_FAIL); return; }
    ret64(v->base);
}

static void hle_GetVTimerBase(void) {
    const psp_vtimer *v = find_vtimer(psp_arg(0));
    const uint32_t out = psp_arg(1);
    if (!v) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_VTID); return; }
    if (out) { psp_write32(out, (uint32_t)v->base);
               psp_write32(out + 4, (uint32_t)(v->base >> 32)); }
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_SetVTimerTimeWide(void) {
    psp_vtimer *v = find_vtimer(psp_arg(0));
    if (!v) { ret64(VTIMER_WIDE_FAIL); return; }
    const uint64_t was = vtimer_now(v);
    /* The 64-bit argument arrives in the pair $a2:$a3, not $a1: an o32 long
     * long is aligned to an even register, so $a1 is skipped. */
    vtimer_set(v, (uint64_t)psp_arg(2) | ((uint64_t)psp_arg(3) << 32));
    ret64(was);
}

static void hle_SetVTimerTime(void) {
    psp_vtimer *v = find_vtimer(psp_arg(0));
    const uint32_t in = psp_arg(1);
    if (!v) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_VTID); return; }
    if (!in) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
    /* In and out, like the Wide form's return value: the clock comes back
     * holding the time it replaced (threadprobe step 124, fw 6.60). */
    const uint64_t was = vtimer_now(v);
    vtimer_set(v, (uint64_t)psp_read32(in) | ((uint64_t)psp_read32(in + 4) << 32));
    psp_write32(in, (uint32_t)was);
    psp_write32(in + 4, (uint32_t)(was >> 32));
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_SetVTimerHandler(void) {
    psp_vtimer *v = find_vtimer(psp_arg(0));
    const uint32_t sched = psp_arg(1);
    if (!v) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_VTID); return; }
    v->schedule = sched ? ((uint64_t)psp_read32(sched) |
                           ((uint64_t)psp_read32(sched + 4) << 32)) : 0;
    v->handler  = psp_arg(2);
    v->common   = psp_arg(3);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_SetVTimerHandlerWide(void) {
    psp_vtimer *v = find_vtimer(psp_arg(0));
    if (!v) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_VTID); return; }
    v->schedule = (uint64_t)psp_arg(2) | ((uint64_t)psp_arg(3) << 32);
    v->handler  = psp_arg(4);
    v->common   = psp_arg(5);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* Only the handler goes: threadprobe step 128 (fw 6.60) cancels a handler set
 * for 0x7FFFFFFF and the refer still reads that schedule and the common
 * pointer. This used to clear all three. */
static void hle_CancelVTimerHandler(void) {
    psp_vtimer *v = find_vtimer(psp_arg(0));
    if (!v) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_VTID); return; }
    v->handler = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_ReferVTimerStatus(void) {
    const psp_vtimer *v = find_vtimer(psp_arg(0));
    const uint32_t info = psp_arg(1);
    if (!v)    { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_VTID); return; }
    if (!info) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
    if (psp_read32(info) == 0) { psp_ret(SCE_KERNEL_ERROR_OK); return; }
    const uint64_t cur = vtimer_now(v);
    psp_write32(info +  0, 72);
    psp_threadman_write_name(info + 4, v->name);
    psp_write32(info + 36, (uint32_t)v->active);
    psp_write32(info + 40, (uint32_t)v->base);
    psp_write32(info + 44, (uint32_t)(v->base >> 32));
    psp_write32(info + 48, (uint32_t)cur);
    psp_write32(info + 52, (uint32_t)(cur >> 32));
    psp_write32(info + 56, (uint32_t)v->schedule);
    psp_write32(info + 60, (uint32_t)(v->schedule >> 32));
    psp_write32(info + 64, v->handler);
    psp_write32(info + 68, v->common);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* Guest memory for the two clocks a handler is handed. One block, reused every
 * firing: a handler cannot be running twice at once, and the structures are
 * only alive for the length of the call. */
static uint32_t vtimer_clock_scratch(void) {
    static uint32_t at;
    if (!at) at = psp_sysmem_alloc(16, 0);
    return at;
}

/* Fire any vtimer whose count has reached its schedule. Same contract as an
 * alarm's: the handler's return value re-arms it, zero retires it -- and
 * retiring clears the handler but leaves the schedule and the common pointer
 * where they were: threadprobe steps 126-127 (fw 6.60) read back the last
 * schedule, handler 0 and the common pointer after a handler returned 0. */
static int vtimer_tick(void) {
    int fired = 0;
    for (int i = 0; i < g_vtimer_hi; i++) {
        psp_vtimer *v = &g_vtimer[i];
        if (!v->alive || !v->active || !v->handler || !v->schedule) continue;
        if (vtimer_now(v) < v->schedule) continue;

        const uint32_t uid = v->uid, handler = v->handler, common = v->common;
        const uint64_t sched = v->schedule;
        const uint64_t real  = vtimer_now(v);

        /* The handler takes two SceKernelSysClock *pointers*, not the halves of
         * one clock:
         *
         *   SceUInt handler(SceUID, SceKernelSysClock *elapsedScheduled,
         *                   SceKernelSysClock *elapsedReal, void *common)
         *
         * vtimers/vtimer dereferences the second argument and prints it, so
         * passing the schedule's low and high words put a small integer where
         * an address belonged and the test read whatever that addressed. The
         * two structures need to live in guest memory for the length of the
         * call, which is what this block is for. */
        const uint32_t clocks = vtimer_clock_scratch();
        if (clocks) {
            psp_write32(clocks + 0, (uint32_t)sched);
            psp_write32(clocks + 4, (uint32_t)(sched >> 32));
            psp_write32(clocks + 8, (uint32_t)real);
            psp_write32(clocks + 12, (uint32_t)(real >> 32));
        }

        g_firing = 1;
        const uint32_t args[4] = { uid, clocks, clocks + 8, common };
        const uint32_t again = psp_call_guest(handler, args, 4);
        g_firing = 0;
        fired++;

        v = find_vtimer(uid);
        if (!v) continue;
        if (again) v->schedule = sched + again;
        else       v->handler = 0;
    }
    return fired;
}

/* The earliest guest moment at which an alarm or a running vtimer's handler
 * is due, or 0 for none. A vtimer's schedule is on its own count, which runs
 * at the system clock's rate from `since`. */
uint64_t psp_ktimer_next_due(void) {
    uint64_t due = 0;
    for (int i = 0; i < MAX_ALARMS; i++)
        if (g_alarm[i].alive && (!due || g_alarm[i].schedule < due))
            due = g_alarm[i].schedule;
    for (int i = 0; i < g_vtimer_hi; i++) {
        const psp_vtimer *v = &g_vtimer[i];
        if (!v->alive || !v->active || !v->handler || !v->schedule) continue;
        const uint64_t at = v->schedule > v->value ? v->since + (v->schedule - v->value)
                                                   : v->since;
        if (!due || at < due) due = at;
    }
    return due;
}

static void vtimer_list(int type, uint32_t out, int max, int *count) {
    if (type != PSP_TMID_VTIMER) return;
    for (int i = 0; i < g_vtimer_hi; i++) {
        if (!g_vtimer[i].alive) continue;
        psp_threadman_list_put(g_vtimer[i].uid, out, max, count);
    }
}

void psp_ktimer_register_vtimer(void) {
    psp_threadman_add_lister(vtimer_list);
    psp_hle_register(0x20FFF560, "ThreadManForUser", "sceKernelCreateVTimer",        hle_CreateVTimer);
    psp_hle_register(0x328F9E52, "ThreadManForUser", "sceKernelDeleteVTimer",        hle_DeleteVTimer);
    psp_hle_register(0xC68D9437, "ThreadManForUser", "sceKernelStartVTimer",         hle_StartVTimer);
    psp_hle_register(0xD0AEEE87, "ThreadManForUser", "sceKernelStopVTimer",          hle_StopVTimer);
    psp_hle_register(0xB3A59970, "ThreadManForUser", "sceKernelGetVTimerBase",       hle_GetVTimerBase);
    psp_hle_register(0xB7C18B77, "ThreadManForUser", "sceKernelGetVTimerBaseWide",   hle_GetVTimerBaseWide);
    psp_hle_register(0x034A921F, "ThreadManForUser", "sceKernelGetVTimerTime",       hle_GetVTimerTime);
    psp_hle_register(0xC0B3FFD2, "ThreadManForUser", "sceKernelGetVTimerTimeWide",   hle_GetVTimerTimeWide);
    psp_hle_register(0x542AD630, "ThreadManForUser", "sceKernelSetVTimerTime",       hle_SetVTimerTime);
    psp_hle_register(0xFB6425C3, "ThreadManForUser", "sceKernelSetVTimerTimeWide",   hle_SetVTimerTimeWide);
    psp_hle_register(0xD8B299AE, "ThreadManForUser", "sceKernelSetVTimerHandler",    hle_SetVTimerHandler);
    psp_hle_register(0x53B00E9A, "ThreadManForUser", "sceKernelSetVTimerHandlerWide",hle_SetVTimerHandlerWide);
    psp_hle_register(0xD2D615EF, "ThreadManForUser", "sceKernelCancelVTimerHandler", hle_CancelVTimerHandler);
    psp_hle_register(0x5F32BEAA, "ThreadManForUser", "sceKernelReferVTimerStatus",   hle_ReferVTimerStatus);
}
