/* psprecomp — the smaller firmware libraries.
 *
 * Kernel_Library, UtilsForUser, StdioForUser, sceSuspendForUser,
 * LoadExecForUser, ModuleMgrForUser, sceCtrl, sceRtc, sceAudio, scePower,
 * sceImpose and sceOpenPSID.
 * Individually small,
 * but collectively they are what a game's C runtime needs before main() gets
 * anywhere -- newlib's reentrancy setup alone wants interrupt masking, a
 * clock, and the standard file descriptors.
 */

#include "psprecomp/hle.h"
#include "psprecomp/mem.h"
#include "psprecomp/sched.h"
#include "psprecomp/clock.h"
#include "psprecomp/os.h"
#include "psprecomp/interrupt.h"
#include "census.h"
#include "psprecomp/state.h"

#include <stdio.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>

/* ---- scePower -------------------------------------------------------------
 *
 * scePowerRegisterCallback(slot, cb) is the first half of the single most
 * common idiom in a PSP program: register a callback, then sleep in
 * sceKernelSleepThreadCB waiting for the home button.
 *
 * Registering *fires* it. That is not a detail of ours -- callbacks/notify
 * says so on the line that calls it, `scePowerRegisterCallback (causes
 * notify)`, and proves it two lines later by reading the accumulated count
 * back as 2 after a single manual notify. The guest learns the current power
 * state without having to ask for it separately.
 *
 * The slot argument is ignored: nothing here ever raises a second power event,
 * so there is no bookkeeping a slot would serve. */
static void hle_PowerRegisterCallback(void) {
    const uint32_t cb = psp_arg(1);
    psp_ret(psp_threadman_notify_callback(cb, 0));
}

/* ---- UtilsForUser -------------------------------------------------------- */

/* Cache maintenance. There is no cache to write back -- the recompiled code
 * and the GE share one flat backing store -- so these are genuinely no-ops
 * rather than unimplemented. */
static void hle_CacheOp(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

/* sceDmacMemcpy(dst, src, size) -- the DMA engine as a memcpy. It is what
 * pspautotests' GPU tests read the framebuffer back with, so while it was
 * unimplemented -- returning zero and copying nothing -- every textured test in
 * the corpus reported its own preset fill at every pixel, whatever the GE had
 * drawn: 44444444 on all of gpu/texfunc, texcolors, filtering, clut, texmtx.
 * The suite was blind to texturing by the readback, not by the rasterizer.
 *
 * The contract is dmac/dmactest.expected: the size is checked before the
 * pointers (zero size is 80000104 even with null pointers), a null pointer
 * with a length is 80000103, and a copy answers 0. The try variant is the same
 * call that refuses with 80000021 while another copy is in flight; copies here
 * are synchronous, so nothing is ever in flight and it never refuses -- one
 * line of that test, and an honest one. */
#define SCE_ERROR_INVALID_POINTER 0x80000103u
#define SCE_ERROR_INVALID_SIZE    0x80000104u

/* Guest-to-guest copy and fill, marking what they write. A range the flat map
 * cannot hand over whole goes byte by byte, so what is mapped is written and
 * the bad-access counter records the rest. */
static void guest_copy(uint32_t dst, uint32_t src, uint32_t size) {
    void *d = psp_mem_ptr(dst, size);
    const void *sp = psp_mem_ptr(src, size);
    if (d && sp) {
        memmove(d, sp, size);
        psp_mem_mark_write(dst, size);
    } else {
        for (uint32_t i = 0; i < size; i++) psp_write8(dst + i, psp_read8(src + i));
    }
}
static void guest_set(uint32_t dst, uint8_t val, uint32_t size) {
    void *d = psp_mem_ptr(dst, size);
    if (d) {
        memset(d, val, size);
        psp_mem_mark_write(dst, size);
    } else {
        for (uint32_t i = 0; i < size; i++) psp_write8(dst + i, val);
    }
}

static void dmac_copy(void) {
    const uint32_t dst = psp_arg(0), src = psp_arg(1), size = psp_arg(2);
    if (!size)       { psp_ret(SCE_ERROR_INVALID_SIZE); return; }
    if (!dst || !src){ psp_ret(SCE_ERROR_INVALID_POINTER); return; }
    guest_copy(dst, src, size);
    psp_ret(SCE_KERNEL_ERROR_OK);
}
static void hle_DmacMemcpy(void)    { dmac_copy(); }
static void hle_DmacTryMemcpy(void) { dmac_copy(); }

/* ---- Kernel_Library: memory ------------------------------------------------
 *
 * sceKernelMemcpy(dst, src, size) and sceKernelMemset(dst, s8 val, size),
 * each answering dst (uofw include/usersystemlib_kernel.h; MIT). They are the
 * user-mode libc's, with no argument checks documented. What an overlapping
 * memcpy does is not measured; this copies as memmove does. The 3rd Birthday
 * calls Memset; unregistered it answered 0 and filled nothing. */
static void hle_KernelMemcpy(void) {
    const uint32_t dst = psp_arg(0);
    guest_copy(dst, psp_arg(1), psp_arg(2));
    psp_ret(dst);
}
static void hle_KernelMemset(void) {
    const uint32_t dst = psp_arg(0);
    guest_set(dst, (uint8_t)psp_arg(1), psp_arg(2));
    psp_ret(dst);
}

/* ---- sceImpose, sceOpenPSID -------------------------------------------------
 *
 * Both games import one call from each and test neither result beyond < 0.
 * sceImposeSetLanguageMode(lang, button) is "< 0 on error" (PSPSDK
 * src/impose/pspimpose.h); nothing here reads the mode back, so it is
 * accepted and not kept. sceOpenPSIDGetOpenPSID(PspOpenPSID *) fills a
 * 16-byte console id (PSPSDK src/openpsid/pspopenpsid.h); there is no
 * console, and any value invented here could end up tied to a save, so the
 * buffer is left as the game passed it -- what the unregistered call did. */
static void hle_ImposeSetLanguageMode(void) { psp_ret(SCE_KERNEL_ERROR_OK); }
static void hle_OpenPSIDGetOpenPSID(void)   { psp_ret(SCE_KERNEL_ERROR_OK); }

/* ---- the wall clock --------------------------------------------------------
 *
 * A date at guest time 0, and the guest clock from there: every difference
 * between two readings is guest time -- a 20ms DelayThread moves it 20ms, as
 * on the PSP (threadprobe steps 133-134, fw 6.60), however long the host took.
 * It used to be host time(NULL) per call, whole seconds, with LibcClock on the
 * host's CPU clock, which does not move while a guest thread waits.
 *
 * The date is the host's, read once, only when the clock runs against wall
 * time (psp_clock_realtime, clock.h). On the deterministic clock
 * it is a fixed day, so a game that reads the date still makes the same calls
 * on every run and the differential oracle's two sides agree; the sceRtc
 * tick, which this now feeds, was deterministic before. */
#define WALL_FIXED_UNIX 1767225600u   /* 2026-01-01 00:00:00 UTC */

static uint64_t wall_us(void) {
    static int have;
    static uint64_t base;          /* microseconds since 1970 at guest time 0 */
    if (!have) {
        base = psp_clock_is_realtime()
             ? (uint64_t)time(NULL) * 1000000u - psp_clock_peek()
             : (uint64_t)WALL_FIXED_UNIX * 1000000u;
        have = 1;
    }
    return base + psp_clock_peek();
}

static void hle_LibcTime(void) {
    const uint32_t t = (uint32_t)(wall_us() / 1000000u);
    uint32_t out = psp_arg(0);
    if (out) psp_write32(out, t);
    psp_ret(t);
}

/* Microseconds of guest time. threadprobe step 133 (fw 6.60): it advances at
 * least 20000 across DelayThread(20000). */
static void hle_LibcClock(void) {
    psp_ret((uint32_t)psp_clock_peek());
}

/* threadprobe step 133 (fw 6.60) reads a microsecond part below 1000000 and
 * not zero, which whole seconds never gave.
 *
 * Step 154 (fw 6.60): the seconds are under 86400, not within 2 s of LibcTime
 * nor of the uptime in seconds, and two reads 20 ms apart advance by 20 ms to
 * 1 s, so seconds and microseconds are one clock. The timezone struct, when
 * given, gets two zero words. What the seconds count is not settled beyond
 * "under a day"; taken here as the seconds since midnight (UTC) of the wall
 * clock LibcTime reads, which fits all three relations. This returned the
 * Unix time, which is after 2001, and left the timezone struct untouched. */
static void hle_LibcGettimeofday(void) {
    const uint32_t tv = psp_arg(0), tz = psp_arg(1);
    if (tv) {
        const uint64_t us = wall_us();
        psp_write32(tv, (uint32_t)((us / 1000000u) % 86400u));
        psp_write32(tv + 4, (uint32_t)(us % 1000000u));
    }
    if (tz) {
        psp_write32(tz, 0);
        psp_write32(tz + 4, 0);
    }
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* General-purpose I/O pins, wired to the debug board. Nothing is connected. */
static void hle_GetGPI(void) { psp_ret(0); }

/* ---- StdioForUser -------------------------------------------------------- */
/* These return the file descriptors, which sceIoWrite then recognises. */
static void hle_Stdin(void)  { psp_ret(0); }
static void hle_Stdout(void) { psp_ret(1); }
static void hle_Stderr(void) { psp_ret(2); }

/* ---- sceSuspendForUser --------------------------------------------------- */

/* Volatile memory: a block the firmware lends a game on request, typically
 * as the scratch buffer it decompresses an archive into. The call reports the
 * block back through two out-parameters, and a stub that returned "success"
 * while writing neither left the game holding a null pointer and a length of
 * zero. It then walked a table through that pointer, which is how this
 * surfaced: an endless run of bad accesses just past the end of .bss, in a
 * structure whose two neighbouring fields were the very pointers passed in
 * here.
 *
 * Where the block sits is this file's choice rather than a fact to look up:
 * the call reports address and size through out-parameters and the guest uses
 * what it is handed. Two things constrain the choice. It has to be mapped,
 * which it is -- PSP_RAM_BASE is 0x08000000 and the RAM is 32MB -- and it has
 * to sit below the user heap so that psp_sysmem_alloc can never hand the same
 * bytes out twice. User memory begins at 0x08800000 (uofw's
 * include/common/memory.h, SCE_USERSPACE_ADDR_KU0), so the 4MB immediately
 * under it is free for this.
 *
 * Under contention Lock blocks and TryLock refuses, an unlock wakes the first
 * waiter, and a non-zero type is refused: tests/provenance/kernel records
 * that, together with the address and size, under an emulator (not a PSP).
 * FIFO selection among several waiters is a host policy; the probe measures
 * one waiter. */
#define VOLATILE_ADDRESS 0x08400000u
#define VOLATILE_BYTES   0x00400000u
#define VOLATILE_BAD_TYPE 0x80000107u
#define VOLATILE_BUSY     0x802b0200u
static struct {
    int acquired;
    unsigned count;
    uint32_t waiting[128]; /* host thread-table capacity */
} volatile_memory;

static void volatile_acquire(int nonblocking) {
    if (psp_arg(0)) { psp_ret(VOLATILE_BAD_TYPE); return; }
    const uint32_t address_out=psp_arg(1), bytes_out=psp_arg(2);
    /* Invalid mapped outputs are a host validation policy. The SDK permits
     * null outputs; the probe exercises all four null/non-null combinations. */
    if ((address_out && !psp_mem_ptr(address_out,4)) ||
        (bytes_out && !psp_mem_ptr(bytes_out,4))) {
        psp_ret(0x800200d3u); return; /* SDK ILLEGAL_ADDR */
    }
    while (volatile_memory.acquired) {
        if (nonblocking) { psp_ret(VOLATILE_BUSY); return; }
        if (!psp_sched_can_wait()) { psp_ret(SCE_KERNEL_ERROR_CAN_NOT_WAIT); return; }
        if (volatile_memory.count==128) { psp_ret(SCE_KERNEL_ERROR_NOTIMPLEMENTED); return; }
        uint32_t self=psp_sched_current();
        volatile_memory.waiting[volatile_memory.count++]=self;
        int result=psp_sched_block(self,PSP_SCHED_BLOCKED,"volatile memory");
        /* Remove a cancelled/stranded wait without leaving a stale UID. */
        for (unsigned i=0;i<volatile_memory.count;i++) {
            if (volatile_memory.waiting[i]!=self) continue;
            memmove(&volatile_memory.waiting[i],&volatile_memory.waiting[i+1],
                    (--volatile_memory.count-i)*sizeof(uint32_t));
            break;
        }
        if (result!=0) {
            psp_sched_stop_all("volatile memory wait has no runnable releaser");
            psp_ret(SCE_KERNEL_ERROR_NOTIMPLEMENTED); return;
        }
    }
    volatile_memory.acquired=1;
    if (address_out) psp_write32(address_out,VOLATILE_ADDRESS);
    if (bytes_out) psp_write32(bytes_out,VOLATILE_BYTES);
    psp_ret(0);
}
static void hle_VolatileMemLock(void) { volatile_acquire(0); }
static void hle_VolatileMemTryLock(void) { volatile_acquire(1); }
static void hle_VolatileMemUnlock(void) {
    if (psp_arg(0)) { psp_ret(VOLATILE_BAD_TYPE); return; }
    if (!volatile_memory.acquired) { psp_ret(SCE_KERNEL_ERROR_SEMA_OVF); return; }
    volatile_memory.acquired=0;
    int urgent=0;
    if (volatile_memory.count) {
        const uint32_t next=volatile_memory.waiting[0];
        memmove(volatile_memory.waiting,volatile_memory.waiting+1,
                --volatile_memory.count*sizeof(uint32_t));
        urgent=psp_sched_wake(next);
    }
    psp_ret(0);
    if (urgent) psp_sched_preempt();
}
/* Power management around suspend. Nothing suspends here. */
static void hle_ok(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

/* ---- LoadExecForUser ----------------------------------------------------- */

static int g_exit_requested;
int psp_exit_requested(void) { return g_exit_requested; }

static void hle_ExitGame(void) {
    /* A game calling this is finished. Recording it rather than terminating
     * the process lets the host report what happened and dump its counters. */
    g_exit_requested = 1;
    fprintf(stderr, "psprecomp: the game called sceKernelExitGame\n");
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_RegisterExitCallback(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

/* ---- ModuleMgrForUser ---------------------------------------------------- */
/* A game-sharing microgame is self-contained and does not load further
 * modules, so these report a plausible identity rather than doing anything.
 * A title that genuinely loads PRXs at run time will need real ones. */
/* These report the id of the one loaded module.
 *
 * A previous note here recorded the opposite -- that returning an id was a
 * "plausible lie" and an error was the truthful answer -- on the strength of a
 * test showing byte-identical output either way. **That test was run while
 * `jal` never assigned `$ra`**, so every non-leaf function in the program was
 * returning through a stale register. A falsification obtained under broken
 * codegen is not a falsification.
 *
 * Re-run after that fix, the two answers differ clearly: returning an id makes
 * the game's own "libc:_getmodreent: no reent structure" diagnostic disappear
 * and drops bad memory accesses from 3 to 0.
 *
 * And an id is the *truthful* answer. The question is "which module owns this
 * address", a self-contained microgame is exactly one module, and the host has
 * loaded it. Reporting 1 states that; reporting UNKNOWN_MODULE denies a module
 * that demonstrably exists. */
#define SCE_KERNEL_ERROR_UNKNOWN_MODULE 0x80020139u
#define PSP_MAIN_MODULE_ID 1u

static void hle_GetModuleId(void)          { psp_ret(PSP_MAIN_MODULE_ID); }
static void hle_GetModuleIdByAddress(void) { psp_ret(PSP_MAIN_MODULE_ID); }
static void hle_ModuleOk(void)             { psp_ret(SCE_KERNEL_ERROR_OK); }

/* 0xF9275D98 is sceKernelLoadModuleBufferUsbWlan: PSPSDK's import stub
 * (src/user/ModuleMgrForUser.S; BSD) names it, and SHA-1 of the name is the
 * NID. It was registered unnamed, after eighteen ModuleMgr names guessed
 * against SHA-1 missed it. WTF's microgame calls it three times on its
 * heap-setup path, where the unregistered 0 failed heap establishment; a load
 * answers the loaded module's id, and the one module's id is what it went on
 * answering. Nothing is loaded. */
static void hle_LoadModuleBufferUsbWlan(void) { psp_ret(PSP_MAIN_MODULE_ID); }

/* ---- sceCtrl ------------------------------------------------------------- */

/* Three lanes, merged only at the guest's read.
 *
 * One shared word had four writers fighting over it, and they did not compose:
 * psp_ctrl_set stores, so a windowed host erased whatever else was set between
 * two SDL events; the timed press cleared its bit with fetch_and, so pressing
 * a button PSPRECOMP_PAD was holding *released* the hold. Each writer was
 * correct alone and wrong beside another.
 *
 * Separating them makes the merge explicit and each lane single-writer:
 *
 *   hold    PSPRECOMP_PAD. Written once at init, read-only thereafter.
 *   script  the timed press, and later the replay player. Guest threads only,
 *           and only one guest thread runs at a time, so it needs no atomics.
 *   host    psp_ctrl_set, from the SDL thread. Atomic; genuinely concurrent.
 *
 * Buttons OR together, which is what a player pressing a second button while
 * holding the first does. The stick cannot OR, so the script takes ownership
 * of it when it sets it and hands it back on `neutral`. */
static uint32_t         g_hold_buttons;
static uint32_t         g_script_buttons;
static uint8_t          g_script_ax, g_script_ay;
static int              g_script_analog;
static _Atomic uint32_t g_host_buttons;
static _Atomic uint8_t  g_host_ax, g_host_ay;

/* The look channel, in the same lanes. A second stick, centred like the
 * first, and mouse motion as a running sum: the host adds to it at whatever
 * rate the mouse reports, the poll takes it, and the script lane's delta is
 * delivered once. Nothing here reaches SceCtrlData -- the PSP has neither --
 * so a game that has not been changed cannot see any of it. */
static uint8_t          g_script_rx = 128, g_script_ry = 128;
static int              g_script_mdx, g_script_mdy;
static int              g_script_look;
static _Atomic uint8_t  g_host_rx, g_host_ry;      /* centred at init */
static _Atomic int      g_host_mdx, g_host_mdy;

/* The SceCtrlData timestamp field, one per *sample* written. Games do
 * arithmetic on it, so it counts what it has always counted. */
static uint32_t         g_ctrl_frame;
/* One per *call*, which is the unit a scripted input has to be keyed on: it
 * is "one thing the guest did", and unlike g_ctrl_frame it does not move when
 * a game changes how many samples it asks for per poll. */
static uint32_t         g_ctrl_polls;
static uint32_t         g_ctrl_last_buttons, g_ctrl_pressed_buttons;

uint32_t psp_ctrl_polls(void)   { return g_ctrl_polls; }
uint32_t psp_ctrl_pressed_buttons(void) { return g_ctrl_pressed_buttons; }

/* The stick as the guest last saw it: the merged lane, after the script took
 * or returned it. For native code that wants the magnitude the game's own
 * control path throws away -- it thresholds the stick into button bits. Read
 * from the same value ctrl_fill wrote, so it is what a recording holds and a
 * replay reproduces. */
static uint8_t g_ctrl_last_ax = 128, g_ctrl_last_ay = 128;
void psp_ctrl_last_stick(uint8_t *ax, uint8_t *ay) {
    if (ax) *ax = g_ctrl_last_ax;
    if (ay) *ay = g_ctrl_last_ay;
}

static uint8_t g_ctrl_last_rx = 128, g_ctrl_last_ry = 128;
static int     g_ctrl_last_mdx, g_ctrl_last_mdy;
void psp_ctrl_last_look(uint8_t *rx, uint8_t *ry, int *mdx, int *mdy) {
    if (rx)  *rx  = g_ctrl_last_rx;
    if (ry)  *ry  = g_ctrl_last_ry;
    if (mdx) *mdx = g_ctrl_last_mdx;
    if (mdy) *mdy = g_ctrl_last_mdy;
}
uint32_t psp_ctrl_samples(void) { return g_ctrl_frame; }

void psp_ctrl_set(uint32_t buttons, uint8_t ax, uint8_t ay) {
    atomic_store(&g_host_buttons, buttons);
    atomic_store(&g_host_ax, ax);
    atomic_store(&g_host_ay, ay);
}

void psp_ctrl_set_look(uint8_t rx, uint8_t ry) {
    atomic_store(&g_host_rx, rx);
    atomic_store(&g_host_ry, ry);
}

void psp_ctrl_add_mouse(int dx, int dy) {
    atomic_fetch_add(&g_host_mdx, dx);
    atomic_fetch_add(&g_host_mdy, dy);
}
void psp_ctrl_clear_mouse(void) {
    atomic_store(&g_host_mdx, 0);
    atomic_store(&g_host_mdy, 0);
}

void psp_ctrl_script_set_look(int owned, uint8_t rx, uint8_t ry, int mdx, int mdy) {
    g_script_look = owned;
    if (owned) { g_script_rx = rx; g_script_ry = ry; }
    g_script_mdx = mdx;
    g_script_mdy = mdy;
}

/* Set by the script lane's owner; read here so the composed value can say so.
 * Defined in this file so the lane stays private to it. */
void psp_ctrl_script_set(uint32_t buttons, int analog_owned,
                         uint8_t ax, uint8_t ay) {
    g_script_buttons = buttons;
    g_script_analog  = analog_owned;
    if (analog_owned) { g_script_ax = ax; g_script_ay = ay; }
}

static void hle_CtrlSet(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

/* One button table, three callers.
 *
 * PSPRECOMP_PAD and PSPRECOMP_PAD_PRESS each carried their own copy, and a
 * scenario file would have made three. Two copies of a name table drift, and
 * the drift is silent: a name one of them accepts and another rejects reads
 * as "that button does nothing" rather than as a typo. */
static const struct { const char *name; uint32_t bit; } PSP_PAD_NAMES[] = {
    { "select",   0x000001 }, { "start",    0x000008 },
    { "up",       0x000010 }, { "right",    0x000020 },
    { "down",     0x000040 }, { "left",     0x000080 },
    { "ltrigger", 0x000100 }, { "rtrigger", 0x000200 },
    { "l",        0x000100 }, { "r",        0x000200 },
    { "triangle", 0x001000 }, { "circle",   0x002000 },
    { "cross",    0x004000 }, { "square",   0x008000 },
};

/* A button name, or 0x<hex> for a bit the table does not name -- the PSP has
 * bits here that no game maps to a face button (hold, note, screen, disc) and
 * a scenario should be able to reach them without this table growing names
 * nobody checked. Returns 0 for anything unrecognised; the caller reports. */
uint32_t psp_pad_bit(const char *s, size_t n) {
    if (n > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        char buf[16];
        if (n >= sizeof buf) return 0;
        memcpy(buf, s, n); buf[n] = 0;
        return (uint32_t)strtoul(buf, NULL, 16);
    }
    for (size_t i = 0; i < sizeof PSP_PAD_NAMES / sizeof PSP_PAD_NAMES[0]; i++)
        if (strlen(PSP_PAD_NAMES[i].name) == n &&
            !strncasecmp(PSP_PAD_NAMES[i].name, s, n))
            return PSP_PAD_NAMES[i].bit;
    return 0;
}

/* PSPRECOMP_PAD=start,cross holds those buttons for the whole run.
 *
 * There is no window and no gamepad here, so the pad reads neutral and a game
 * sits on its title screen forever waiting for a press. Being able to hold a
 * button is what gets bring-up past that -- it is the difference between
 * "renders a menu" and "renders whatever is behind the menu".
 *
 * Read at init rather than on the first poll: it is a constant for the run,
 * and reading it once here means the read point does not need a guard. */
static uint32_t parse_pad(void) {
    const char *v = getenv("PSPRECOMP_PAD");
    if (!v || !*v) return 0;

    uint32_t held = 0;
    for (const char *p = v; *p; ) {
        while (*p == ',' || *p == ' ') p++;
        if (!*p) break;
        size_t n = 0;
        while (p[n] && p[n] != ',' && p[n] != ' ') n++;

        uint32_t bit = psp_pad_bit(p, n);
        if (bit) held |= bit;
        else fprintf(stderr, "psprecomp: PSPRECOMP_PAD: unknown button \"%.*s\"\n",
                     (int)n, p);
        p += n;
    }
    if (held) fprintf(stderr, "psprecomp: holding pad buttons 0x%06X\n", held);
    return held;
}

/* PSPRECOMP_PAD_PRESS=<button>,<delay>[,<duration>]
 *
 * PSPRECOMP_PAD cannot express a press. A game reads "pressed" as a
 * transition -- down where it was up at the previous poll -- and a button
 * held from before the first poll never transitions: it is a button that was
 * already down, forever. So holding start at a title screen does nothing,
 * while a player tapping it skips.
 *
 * BEHAVIOUR CHANGE: `delay` and `duration` were wall-clock seconds, slept out
 * on a detached pthread. They are now *guest* seconds, evaluated at the read
 * point. On the virtual clock those are different quantities, and the same
 * command line will land the press somewhere else.
 *
 * The thread had to go. It raced the guest, so where the press landed in the
 * instruction stream depended on how fast the host was that minute, which
 * makes a run that reproduces a bug not reproduce it again. Guest time is a
 * quantity the run owns; wall time is a quantity the machine owns. */
typedef struct {
    uint32_t bit;
    uint64_t down_us, up_us;
    int      armed, down_done, up_done;
} pad_press;

static pad_press g_press;

static void parse_pad_press(void) {
    const char *v = getenv("PSPRECOMP_PAD_PRESS");
    if (!v || !*v) return;

    char spec[256];
    snprintf(spec, sizeof spec, "%s", v);
    char *save = NULL;
    char *name     = strtok_r(spec, ",", &save);
    char *delay_s  = strtok_r(NULL, ",", &save);
    char *dur_s    = strtok_r(NULL, ",", &save);
    if (!name || !delay_s) {
        fprintf(stderr, "psprecomp: PSPRECOMP_PAD_PRESS: want <button>,<delay>[,<duration>]\n");
        return;
    }

    uint32_t bit = psp_pad_bit(name, strlen(name));
    if (!bit) {
        fprintf(stderr, "psprecomp: PSPRECOMP_PAD_PRESS: unknown button \"%s\"\n", name);
        return;
    }

    double delay = atof(delay_s);
    double dur   = dur_s ? atof(dur_s) : 0.5;
    if (delay < 0) delay = 0;
    if (dur <= 0)  dur   = 0.5;

    g_press.bit     = bit;
    g_press.down_us = (uint64_t)(delay * 1e6);
    g_press.up_us   = (uint64_t)((delay + dur) * 1e6);
    g_press.armed   = 1;
    fprintf(stderr, "psprecomp: pad press 0x%06X at %.3fs for %.3fs of guest time\n",
            bit, delay, dur);
}

/* One edge per poll, for the same reason the replay player obeys that rule:
 * applying down and up in the same poll shows the guest no transition at all,
 * and a press the guest could not observe is a press that did not happen. */
static void pad_press_step(uint64_t us) {
    if (!g_press.armed) return;
    if (!g_press.down_done && us >= g_press.down_us) {
        g_press.down_done = 1;
        g_script_buttons |= g_press.bit;
        fprintf(stderr, "psprecomp: pad press 0x%06X down (poll %u, t=%.3fs)\n",
                g_press.bit, g_ctrl_polls, (double)us / 1e6);
        return;
    }
    if (g_press.down_done && !g_press.up_done && us >= g_press.up_us) {
        g_press.up_done = 1;
        g_script_buttons &= ~g_press.bit;
        fprintf(stderr, "psprecomp: pad press 0x%06X up (poll %u, t=%.3fs)\n",
                g_press.bit, g_ctrl_polls, (double)us / 1e6);
    }
}

/* The controller is sampled once per vblank, and Read is the call that waits
 * for a sample it has not already been given. That is the whole difference
 * between Read and Peek, and it is measurable: ctrl/ctrl times five calls of
 * each and asks whether more than 5000us went by. Five reads span four vblanks
 * -- about 67ms -- so hardware answers 1, 0, 1 for Read, ReadLatch and Peek.
 *
 * Waiting unconditionally would be wrong in the direction that matters most. A
 * frame loop is `WaitVblank(); ReadBufferPositive();`, and the sample it wants
 * arrived at the vblank it just waited out -- charging it another frame would
 * halve the frame rate of every game that reads the pad. So what is tracked is
 * when the *next* unread sample appears: a read that has one takes it and does
 * not wait.
 *
 * `g_sample_due` starts at zero, which is "a sample is available now" -- the
 * first read of a run does not wait, because the pad has a position before
 * anyone asks. */
static uint64_t g_sample_due;

static void ctrl_wait_sample(void) {
    const uint64_t now = psp_clock_peek();
    if (now < g_sample_due) {
        psp_sched_delay(g_sample_due - now);
        /* As in hle_WaitVblank: with threading off nothing parks, and the
         * sample still has to become due. Monotonic, so it is a no-op when the
         * wait already arrived. */
        psp_clock_advance_to(g_sample_due);
    }
    g_sample_due = psp_clock_next_frame();
}

/* How many samples a buffer call hands back. Observed from an external
 * executable (tests/provenance/ctrl, not a physical PSP), and consistent with
 * the hardware-recorded ctrl/sampling and ctrl/vblank expectations:
 *
 *   - Read returns the samples taken since the previous Read -- one per
 *     vblank -- newest last, at most 63 of them, and at most the room the
 *     caller gave. With none unread it waits for the next one. A title that
 *     reads every other vblank gets two per call; one that reads every vblank
 *     gets one. Room 0 behaves as room 1.
 *   - Peek returns `count` samples of history without consuming anything;
 *     room 0 returns 0 and writes nothing.
 *   - Room above 64 is rejected by both with SCE_ERROR_INVALID_SIZE, and
 *     nothing is written.
 *
 * Handing back as many samples as the room allowed (the old behaviour) made
 * The 3rd Birthday, which reads with room for ten every other vblank, count
 * ten samples per poll into its per-sample menu repeat, and a short tap
 * skipped entries. One sample per call halved the real rate instead.
 *
 * This provider has one merged snapshot per call, not a sample history, so
 * every entry carries the current state. The unread count comes from the
 * guest clock's vblank grid; scenarios replay in virtual time, where it is
 * deterministic. In live play a late frame can deliver more than two, as
 * the hardware would. */
enum { CTRL_HISTORY = 64, CTRL_UNREAD_MAX = 63 };
static uint64_t g_read_frame = UINT64_MAX;  /* vblank of the last Read's newest sample */

static uint32_t ctrl_unread_samples(uint32_t room) {
    const uint64_t frame = psp_clock_peek() / PSP_CLOCK_FRAME_US;
    uint64_t unread = g_read_frame == UINT64_MAX || frame <= g_read_frame
                    ? 1 : frame - g_read_frame;
    g_read_frame = frame;
    if (unread > CTRL_UNREAD_MAX) unread = CTRL_UNREAD_MAX;
    if (!room) room = 1;
    return unread < room ? (uint32_t)unread : room;
}

/* ---- PSPRECOMP_RAMSNAP -- whole-RAM snapshots at chosen polls -------------
 *
 * PSPRECOMP_RAMSNAP=<prefix> with PSPRECOMP_RAMSNAP_POLLS=<n>[,<n>...] writes
 * the guest's 32 MB of RAM to <prefix>-<poll>.ram at each named poll, before
 * that poll's pad state is written.
 *
 * This exists for one question FINDPTR cannot ask: "which word changes by a
 * constant each frame while the stick is held". FINDPTR needs a value to look
 * for, and a heading's representation -- float radians, degrees, a 16-bit
 * binary angle -- is exactly what is not known. Three snapshots a poll apart
 * during a hold settle it offline without guessing: a word whose successive
 * differences are equal and non-zero is an integrator's output, and its step
 * says what the unit is. See scripts/ram-diff.py in last-raven.
 *
 * Taken at the poll, the timebase scenarios are keyed on, so a snapshot lines
 * up with the input that produced it. Capped at 16 -- half a gigabyte --
 * stated here rather than discovered from a full disk. */
enum { RAMSNAP_MAX = 16 };
static const char *g_ramsnap_prefix;
static uint32_t    g_ramsnap_polls[RAMSNAP_MAX];
static int         g_ramsnap_n, g_ramsnap_next;

/* PSPRECOMP_WATCHMEM_FROM=<poll>: hold PSPRECOMP_WATCHMEM's report budget until
 * that poll. Lives here because the poll count does; mem.c only knows about
 * writes. */
static uint32_t g_watch_from;

static void parse_watch_from(void) {
    const char *v = getenv("PSPRECOMP_WATCHMEM_FROM");
    if (!v || !*v) return;
    g_watch_from = (uint32_t)strtoul(v, NULL, 0);
    if (g_watch_from) {
        psp_mem_watch_arm(0);
        printf("      watchmem  reporting from poll %u\n", g_watch_from);
    }
}

static void parse_ramsnap(void) {
    const char *p = getenv("PSPRECOMP_RAMSNAP");
    if (!p || !*p) return;
    g_ramsnap_prefix = p;
    const char *v = getenv("PSPRECOMP_RAMSNAP_POLLS");
    for (const char *s = v ? v : ""; *s && g_ramsnap_n < RAMSNAP_MAX; ) {
        char *end;
        const uint32_t n = (uint32_t)strtoul(s, &end, 10);
        if (end == s) break;
        g_ramsnap_polls[g_ramsnap_n++] = n;
        s = (*end == ',') ? end + 1 : end;
    }
    printf("      ramsnap   %d snapshot(s) to %s-<poll>.ram\n", g_ramsnap_n, p);
}

static void ramsnap_step(uint32_t poll) {
    /* Listed ascending. >= rather than ==, so a poll listed below the current
     * count fires late instead of blocking every one after it. */
    if (g_ramsnap_next >= g_ramsnap_n || poll < g_ramsnap_polls[g_ramsnap_next]) return;
    g_ramsnap_next++;
    char path[1024];
    snprintf(path, sizeof path, "%s-%u.ram", g_ramsnap_prefix, poll);
    const void *ram = psp_mem_ptr(PSP_RAM_BASE, PSP_RAM_SIZE);
    FILE *f = ram ? fopen(path, "wb") : NULL;
    if (!f) { fprintf(stderr, "ramsnap: cannot write %s\n", path); return; }
    const size_t got = fwrite(ram, 1, PSP_RAM_SIZE, f);
    fclose(f);
    fprintf(stderr, "ramsnap: poll %u -> %s (%zu bytes)\n", poll, path, got);

    /* The module image too, as <prefix>-<poll>.mod. This runtime maps the
     * module at guest address 0, and its data and BSS are not in RAM at all --
     * which is where this game keeps its player object, so a RAM-only diff
     * looked straight past the heading it was hunting. The extent is not known
     * here; probe upward in 64K steps until the mapping ends. Best effort: a
     * process with no module loaded writes no .mod and says nothing. */
    uint32_t mod = 0;
    while (mod < 0x01000000u && psp_mem_ptr(0, mod + 0x10000u)) mod += 0x10000u;
    if (mod) {
        snprintf(path, sizeof path, "%s-%u.mod", g_ramsnap_prefix, poll);
        f = fopen(path, "wb");
        if (!f) { fprintf(stderr, "ramsnap: cannot write %s\n", path); return; }
        const size_t gotm = fwrite(psp_mem_ptr(0, mod), 1, mod, f);
        fclose(f);
        fprintf(stderr, "ramsnap: poll %u -> %s (%zu bytes, module at 0)\n",
                poll, path, gotm);
    }
}

/* SceCtrlData: u32 timestamp, u32 buttons, u8 lx, u8 ly, then padding to 16.
 * Delivers `samples` entries of the merged state and returns that count. */
static void ctrl_fill(uint32_t samples) {
    const uint64_t us = psp_clock_peek();
    g_ctrl_polls++;
    if (g_watch_from && g_ctrl_polls == g_watch_from) psp_mem_watch_arm(1);
    pad_press_step(us);
    psp_ctrl_replay_step(g_ctrl_polls, us);
    ramsnap_step(g_ctrl_polls);

    /* The merge. Buttons OR; the stick belongs to whoever last claimed it.
     * PSPRECOMP_REPLAY_LIVE=0 keeps the host lane out of a scenario's run
     * altogether: a controller left plugged in idles a few counts off centre,
     * and a measurement run must not depend on what is on the desk. */
    static int live = -1;
    if (live < 0) { const char *e = getenv("PSPRECOMP_REPLAY_LIVE"); live = !(e && *e && *e == '0'); }
    const int scripted = !live && psp_ctrl_replay_active();
    const uint32_t host = scripted ? 0 : atomic_load(&g_host_buttons);
    const uint8_t  hax  = scripted ? 128 : atomic_load(&g_host_ax);
    const uint8_t  hay  = scripted ? 128 : atomic_load(&g_host_ay);
    const uint32_t buttons = g_hold_buttons | g_script_buttons | host;
    g_ctrl_pressed_buttons = buttons & ~g_ctrl_last_buttons;
    g_ctrl_last_buttons = buttons;
    const uint8_t  ax = g_script_analog ? g_script_ax : hax;
    const uint8_t  ay = g_script_analog ? g_script_ay : hay;
    g_ctrl_last_ax = ax;
    g_ctrl_last_ay = ay;

    /* The look channel merges the same way, except that the mouse is taken
     * rather than read -- the sum restarts at every poll -- and the script's
     * delta is spent here, so a later poll does not deliver it again. */
    const uint8_t hrx  = scripted ? 128 : atomic_load(&g_host_rx);
    const uint8_t hry  = scripted ? 128 : atomic_load(&g_host_ry);
    const int     hmdx = scripted ? (atomic_exchange(&g_host_mdx, 0), 0) : atomic_exchange(&g_host_mdx, 0);
    const int     hmdy = scripted ? (atomic_exchange(&g_host_mdy, 0), 0) : atomic_exchange(&g_host_mdy, 0);
    g_ctrl_last_rx  = g_script_look ? g_script_rx  : hrx;
    g_ctrl_last_ry  = g_script_look ? g_script_ry  : hry;
    g_ctrl_last_mdx = g_script_look ? g_script_mdx : hmdx;
    g_ctrl_last_mdy = g_script_look ? g_script_mdy : hmdy;
    g_script_mdx = g_script_mdy = 0;

    /* A scenario driving a run that also has a hand on the pad is still a
     * useful run -- it is how you take over one that is stuck -- but it is no
     * longer the scenario's run, and that has to be impossible to miss. */
    if (host || hax != 128 || hay != 128 || hrx != 128 || hry != 128 || hmdx || hmdy)
        psp_ctrl_replay_taint(g_ctrl_polls);
    psp_ctrl_replay_record(g_ctrl_polls, us, buttons, ax, ay,
                           g_ctrl_last_rx, g_ctrl_last_ry,
                           g_ctrl_last_mdx, g_ctrl_last_mdy);

    /* Room past the delivered samples is left untouched. */
    const uint32_t buf = psp_arg(0);
    for (uint32_t i = 0; i < samples; i++) {
        const uint32_t at = buf + i * 16;
        psp_write32(at, g_ctrl_frame++);
        psp_write32(at + 4, buttons);
        psp_write8(at + 8, ax);
        psp_write8(at + 9, ay);
        for (int k = 10; k < 16; k++) psp_write8(at + (uint32_t)k, 0);
    }
    psp_ret(samples);
}

static void hle_ReadBufferPositive(void) {
    const uint32_t room = psp_arg(1);
    if (room > CTRL_HISTORY) { psp_ret(SCE_ERROR_INVALID_SIZE); return; }
    ctrl_wait_sample();
    ctrl_fill(ctrl_unread_samples(room));
}

/* A thread a save state restored inside the read's wait for a sample
 * (psp_hle_register_resume): the rest of ctrl_wait_sample, then the read. */
static void read_buffer_resume(void) {
    (void)psp_sched_resume_delay(NULL);
    psp_clock_advance_to(g_sample_due);
    g_sample_due = psp_clock_next_frame();
    ctrl_fill(ctrl_unread_samples(psp_arg(1)));
}

static void hle_PeekBufferPositive(void) {
    const uint32_t room = psp_arg(1);
    if (room > CTRL_HISTORY) { psp_ret(SCE_ERROR_INVALID_SIZE); return; }
    if (!room) { psp_ret(0); return; }
    ctrl_fill(room);
}

/* ---- sceRtc ----------------------------------------------------------------
 *
 * A tick is a microsecond counted from 0001-01-01 00:00:00 UTC, proleptic
 * Gregorian. threadprobe (fw 6.60) pins it: resolution 1000000 (step 134),
 * GetTick of 2000-01-01 is 0x00E01D00_3A63A000, 730119 days of them (step
 * 138), and a current tick is past 1970 (step 134). The tick counter used to
 * start at module start; differences, which rtc/rtc and ctrl/ctrl measure,
 * are unchanged, since the date is the wall clock above plus guest time.
 *
 * The calendar calls were not here at all (unimplemented, answering 0 and
 * writing nothing). Each one below cites the step that measured it. */

#define RTC_US_PER_DAY   86400000000ull
/* Days from 0001-01-01 to 1970-01-01. */
#define RTC_DAYS_TO_1970 719162

/* A ScePspDateTime: six u16 (year, month, day, hour, minute, second) and a
 * u32 microsecond -- 16 bytes. */
typedef struct { int64_t year, month, day, hour, minute, second, us; } rtc_date;

static rtc_date rtc_read_date(uint32_t at) {
    rtc_date d;
    d.year   = psp_read16(at + 0);  d.month  = psp_read16(at + 2);
    d.day    = psp_read16(at + 4);  d.hour   = psp_read16(at + 6);
    d.minute = psp_read16(at + 8);  d.second = psp_read16(at + 10);
    d.us     = psp_read32(at + 12);
    return d;
}

static void rtc_write_date(uint32_t at, const rtc_date *d) {
    psp_write16(at + 0, (uint16_t)d->year);   psp_write16(at + 2, (uint16_t)d->month);
    psp_write16(at + 4, (uint16_t)d->day);    psp_write16(at + 6, (uint16_t)d->hour);
    psp_write16(at + 8, (uint16_t)d->minute); psp_write16(at + 10, (uint16_t)d->second);
    psp_write32(at + 12, (uint32_t)d->us);
}

static int64_t floor_div(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

/* Days since 1970-01-01 of a civil date. A month outside 1..12 carries into
 * the year and a day past the month's end runs on into the next, which is
 * what GetDayOfWeek does with 2023-02-30 (a Thursday, 4) and 2023-13-01 (a
 * Monday, 1) in step 136. */
static int64_t rtc_days(int64_t y, int64_t m, int64_t d) {
    y += floor_div(m - 1, 12);
    m = m - 1 - floor_div(m - 1, 12) * 12 + 1;
    y -= m <= 2;
    const int64_t era = floor_div(y, 400);
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void rtc_civil(int64_t z, int64_t *y, int64_t *m, int64_t *d) {
    z += 719468;
    const int64_t era = floor_div(z, 146097);
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp  = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = yoe + era * 400 + (*m <= 2);
}

static uint64_t rtc_tick_of(const rtc_date *t) {
    const int64_t days = rtc_days(t->year, t->month, t->day) + RTC_DAYS_TO_1970;
    return (uint64_t)days * RTC_US_PER_DAY +
           (uint64_t)((t->hour * 3600 + t->minute * 60 + t->second) * 1000000 + t->us);
}

static rtc_date rtc_date_of(uint64_t tick) {
    rtc_date t;
    const uint64_t day = tick / RTC_US_PER_DAY, rest = tick % RTC_US_PER_DAY;
    rtc_civil((int64_t)day - RTC_DAYS_TO_1970, &t.year, &t.month, &t.day);
    t.hour   = (int64_t)(rest / 3600000000ull);
    t.minute = (int64_t)(rest / 60000000ull % 60);
    t.second = (int64_t)(rest / 1000000ull % 60);
    t.us     = (int64_t)(rest % 1000000ull);
    return t;
}

static int rtc_leap(int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static int rtc_month_days(int64_t y, int64_t m) {
    static const int n[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return m == 2 && rtc_leap(y) ? 29 : n[m - 1];
}

static uint64_t rtc_now(void) {
    return wall_us() + (uint64_t)RTC_DAYS_TO_1970 * RTC_US_PER_DAY;
}

static void rtc_write_tick(uint32_t at, uint64_t t) {
    psp_write32(at, (uint32_t)t);
    psp_write32(at + 4, (uint32_t)(t >> 32));
}

static uint64_t rtc_read_tick(uint32_t at) {
    return (uint64_t)psp_read32(at) | ((uint64_t)psp_read32(at + 4) << 32);
}

static void hle_RtcGetCurrentTick(void) {
    const uint32_t out = psp_arg(0);
    if (out) rtc_write_tick(out, rtc_now());
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* Ticks per second, and the unit above is what makes it this number. */
static void hle_RtcGetTickResolution(void) { psp_ret(1000000u); }

/* sceRtcGetAccumulativeTime(void). uofw's version (src/kd/rtc/rtc.c; MIT)
 * answers SCE_ERROR_OK with its system-time read commented out and the
 * function marked unfinished, so 0 is all a source gives; what firmware 6.60
 * answers is not measured. The 3rd Birthday calls it once. */
static void hle_RtcGetAccumulativeTime(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

/* (date, tz minutes): the date now, tz minutes east of UTC (step 134). */
static void hle_RtcGetCurrentClock(void) {
    const uint32_t out = psp_arg(0);
    const int32_t tz = (int32_t)psp_arg(1);
    const rtc_date d = rtc_date_of(rtc_now() + (uint64_t)((int64_t)tz * 60000000));
    if (out) rtc_write_date(out, &d);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* Step 135: 1900 0, 2000 1, 2004 1, 2023 0. */
static void hle_RtcIsLeapYear(void) { psp_ret((uint32_t)rtc_leap((int32_t)psp_arg(0))); }

/* Step 135: 2000/2 29, 1900/2 28, 2023/4 30, and months 13 and 0 are
 * 0x800001FF -- not a kernel code, but the one the PSP gives. */
static void hle_RtcGetDaysInMonth(void) {
    const int32_t y = (int32_t)psp_arg(0), m = (int32_t)psp_arg(1);
    if (m < 1 || m > 12) { psp_ret(0x800001FFu); return; }
    psp_ret((uint32_t)rtc_month_days(y, m));
}

/* Step 136: 0 is Sunday; 2000-01-01 6, 1970-01-01 4, 2026-09-27 0. */
static void hle_RtcGetDayOfWeek(void) {
    const int64_t days = rtc_days((int32_t)psp_arg(0), (int32_t)psp_arg(1), (int32_t)psp_arg(2));
    psp_ret((uint32_t)(days - floor_div(days + 4, 7) * 7 + 4));
}

/* Step 137: 0, or the first bad field in this order, as -1 (year) down to -7
 * (microsecond). February 30 is a bad day. A year above 9999 is unmeasured
 * and taken as bad. */
static void hle_RtcCheckValid(void) {
    const rtc_date d = rtc_read_date(psp_arg(0));
    int32_t r = 0;
    if (d.year < 1 || d.year > 9999)                          r = -1;
    else if (d.month < 1 || d.month > 12)                     r = -2;
    else if (d.day < 1 || d.day > rtc_month_days(d.year, d.month)) r = -3;
    else if (d.hour > 23)                                     r = -4;
    else if (d.minute > 59)                                   r = -5;
    else if (d.second > 59)                                   r = -6;
    else if (d.us > 999999)                                   r = -7;
    psp_ret((uint32_t)r);
}

/* (date, &tick) and (&date, &tick), step 138.
 *
 * An invalid date: threadprobe step 161 (fw 6.60) has 2023-02-30 give
 * 2023-03-02's tick, so a day past the month's end runs on, but 2023-13-01
 * gives 2023-12-01's (31 days before 2024-01-01), so a month above 12 is
 * taken as 12 rather than carried into the year the way GetDayOfWeek does
 * (step 136). This carried it, and gave 2024-01-01. Months above 13 are
 * unmeasured and taken as 12 too; month 0 is unmeasured and still borrows
 * from the year. */
static void hle_RtcGetTick(void) {
    const uint32_t date = psp_arg(0), out = psp_arg(1);
    rtc_date d = rtc_read_date(date);
    if (d.month > 12) d.month = 12;
    if (out) rtc_write_tick(out, rtc_tick_of(&d));
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_RtcSetTick(void) {
    const uint32_t date = psp_arg(0), in = psp_arg(1);
    const rtc_date d = rtc_date_of(rtc_read_tick(in));
    rtc_write_date(date, &d);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* time_t is 32 bits to the firmware: GetTime_t writes one word (step 138,
 * 2000-01-01 is 386D4380), and SetTime_t takes its time in $a1. The probe
 * was built with a 64-bit time_t and passed 0 in $a2:$a3; the PSP read $a1,
 * which held 1, and answered 1970-01-01 00:00:01. */
static void hle_RtcGetTime_t(void) {
    const rtc_date d = rtc_read_date(psp_arg(0));
    const uint64_t t = rtc_tick_of(&d);
    const uint64_t epoch = (uint64_t)RTC_DAYS_TO_1970 * RTC_US_PER_DAY;
    if (psp_arg(1)) psp_write32(psp_arg(1), (uint32_t)((t - epoch) / 1000000u));
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_RtcSetTime_t(void) {
    const uint64_t t = (uint64_t)RTC_DAYS_TO_1970 * RTC_US_PER_DAY +
                       (uint64_t)psp_arg(1) * 1000000u;
    const rtc_date d = rtc_date_of(t);
    rtc_write_date(psp_arg(0), &d);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* Step 140: -1, 0, 1. */
static void hle_RtcCompareTick(void) {
    const uint64_t a = rtc_read_tick(psp_arg(0)), b = rtc_read_tick(psp_arg(1));
    psp_ret(a < b ? 0xFFFFFFFFu : a > b ? 1u : 0u);
}

/* The TickAdd family (step 139, from 2000-01-31): (dest, src, n), with n a
 * u64 in $a2:$a3 for ticks, microseconds, seconds and minutes and an int in
 * $a2 for the rest. The fixed units are plain multiples, wrapping in 64 bits
 * (Minutes(-1) is -60 s). Months and years move the calendar date and keep
 * the time of day, clamping the day to the new month: Jan 31 + 1 month is
 * 2000-02-29, and 2000-01-31 + 1 year is 366 days on. */
static uint64_t arg64(int i) { return (uint64_t)psp_arg(i) | ((uint64_t)psp_arg(i + 1) << 32); }

static void rtc_add(uint64_t delta) {
    const uint32_t dst = psp_arg(0);
    const uint64_t t = rtc_read_tick(psp_arg(1)) + delta;
    if (dst) rtc_write_tick(dst, t);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_RtcTickAddTicks(void)        { rtc_add(arg64(2)); }
static void hle_RtcTickAddMicroseconds(void) { rtc_add(arg64(2)); }
static void hle_RtcTickAddSeconds(void)      { rtc_add(arg64(2) * 1000000ull); }
static void hle_RtcTickAddMinutes(void)      { rtc_add(arg64(2) * 60000000ull); }
static void hle_RtcTickAddHours(void)  { rtc_add((uint64_t)((int64_t)(int32_t)psp_arg(2) * 3600000000ll)); }
static void hle_RtcTickAddDays(void)   { rtc_add((uint64_t)((int64_t)(int32_t)psp_arg(2) * (int64_t)RTC_US_PER_DAY)); }
static void hle_RtcTickAddWeeks(void)  { rtc_add((uint64_t)((int64_t)(int32_t)psp_arg(2) * 7 * (int64_t)RTC_US_PER_DAY)); }

static void rtc_add_months(int64_t months) {
    const uint32_t dst = psp_arg(0);
    const uint64_t src = rtc_read_tick(psp_arg(1));
    rtc_date d = rtc_date_of(src);
    const int64_t m0 = d.year * 12 + (d.month - 1) + months;
    d.year  = floor_div(m0, 12);
    d.month = m0 - d.year * 12 + 1;
    const int last = rtc_month_days(d.year, d.month);
    if (d.day > last) d.day = last;
    if (dst) rtc_write_tick(dst, rtc_tick_of(&d));
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_RtcTickAddMonths(void) { rtc_add_months((int32_t)psp_arg(2)); }
static void hle_RtcTickAddYears(void)  { rtc_add_months((int64_t)(int32_t)psp_arg(2) * 12); }

/* (buffer, &utc, tz minutes), step 140: "2000-01-01T00:00:00.00Z" at +0 and
 * "2000-01-01T09:00:00.00+09:00" at +540 -- the local time, two digits of
 * fraction, and the offset. */
static void hle_RtcFormatRFC3339(void) {
    const uint32_t buf = psp_arg(0), in = psp_arg(1);
    const int32_t tz = (int32_t)psp_arg(2);
    const rtc_date d = rtc_date_of(rtc_read_tick(in) + (uint64_t)((int64_t)tz * 60000000));
    char s[48], z[8];
    if (tz == 0) snprintf(z, sizeof z, "Z");
    else snprintf(z, sizeof z, "%c%02d:%02d", tz < 0 ? '-' : '+',
                  (tz < 0 ? -tz : tz) / 60 % 100, (tz < 0 ? -tz : tz) % 60);
    snprintf(s, sizeof s, "%04d-%02d-%02dT%02d:%02d:%02d.%02d%s", (int)d.year, (int)d.month,
             (int)d.day, (int)d.hour, (int)d.minute, (int)d.second, (int)(d.us / 10000), z);
    for (size_t i = 0; i <= strlen(s); i++) psp_write8(buf + (uint32_t)i, (uint8_t)s[i]);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* ---- sceAudio ------------------------------------------------------------ */

#define AUDIO_CHANNELS 8
#define AUDIO_OUTPUT2_CHANNEL AUDIO_CHANNELS
#define AUDIO_OUTPUTS (AUDIO_CHANNELS + 1)

typedef struct { int reserved; uint32_t samples; uint32_t format; uint64_t play_until_ns; } audio_ch;
static audio_ch g_audio[AUDIO_OUTPUTS];
static uint64_t g_output2_until_ns;
static uint64_t g_audio_blocks;

uint64_t psp_audio_blocks(void) { return g_audio_blocks; }

/* The host side of audio output.
 *
 * The samples a game passes to sceAudioOutput are real PCM, and until now
 * they were counted and dropped: with nothing consuming them the output calls
 * returned immediately, which un-paces the audio thread and shows up
 * downstream as hundreds of millions of spins. The hook, when a host
 * registers one, receives the buffer and reports how far the playback queue
 * is backed up in *microseconds* -- the amount a blocking output would have
 * waited on hardware. Policy stays here: the blocking calls pay the backlog
 * with psp_sched_delay, the non-blocking ones never wait. */
static int64_t (*g_audio_out)(int ch, uint32_t samples, uint32_t fmt,
                              uint32_t buf, uint32_t lvol, uint32_t rvol);
static uint32_t (*g_audio_pending)(int ch);

void psp_audio_set_output(int64_t (*fn)(int ch, uint32_t samples, uint32_t fmt,
                                        uint32_t buf, uint32_t lvol, uint32_t rvol)) {
    g_audio_out = fn;
    g_audio_pending = NULL;
}

void psp_audio_set_pending(uint32_t (*fn)(int ch)) { g_audio_pending = fn; }

/* PSPRECOMP_AUDIO_DUMP=<prefix> appends every output buffer, raw signed
 * 16-bit as the game wrote it, to <prefix>.chN.raw -- one file per channel,
 * with a line on stderr naming its shape. A headless run has no speaker, and
 * "is there sound" is otherwise a question only a windowed run can answer. */
static FILE *g_audio_dump[AUDIO_OUTPUTS];
/* A load into the running game starts the dumps again, so each holds the
 * game's sound from the last load on. */
static void audio_dump_drop(void) {
    for (int ch = 0; ch < AUDIO_OUTPUTS; ch++)
        if (g_audio_dump[ch]) { fclose(g_audio_dump[ch]); g_audio_dump[ch] = NULL; }
}
static const char *audio_dump_prefix(void) {
    static const char *p; static int looked;
    if (!looked) { looked = 1; p = getenv("PSPRECOMP_AUDIO_DUMP"); if (p && !*p) p = NULL; }
    return p;
}
static void audio_dump(uint32_t ch, uint32_t samples, uint32_t fmt, uint32_t buf) {
    const char *prefix = audio_dump_prefix();
    if (!prefix) return;
    if (!g_audio_dump[ch]) {
        char path[512];
        snprintf(path, sizeof path, "%s.ch%u.raw", prefix, ch);
        g_audio_dump[ch] = fopen(path, "wb");
        if (!g_audio_dump[ch]) return;
        fprintf(stderr, "audio dump: ch %u  %s  %u samples per call  -> %s\n",
                ch, (fmt & 0x10) ? "mono" : "stereo", samples, path);
    }
    const uint32_t bytes = samples * ((fmt & 0x10) ? 2u : 4u);
    const void *p = psp_mem_ptr(buf, bytes);
    if (p) fwrite(p, 1, bytes, g_audio_dump[ch]);
}

static void hle_ChReserve(void) {
    /* (channel, samplecount, format) -- channel -1 means "any". */
    int32_t ch = (int32_t)psp_arg(0);
    if (ch < 0) {
        for (int i = 0; i < AUDIO_CHANNELS; i++)
            if (!g_audio[i].reserved) { ch = i; break; }
    }
    if (ch < 0 || ch >= AUDIO_CHANNELS) { psp_ret(0x80000001); return; }
    g_audio[ch].reserved = 1;
    g_audio[ch].samples = psp_arg(1);
    g_audio[ch].format  = psp_arg(2);
    psp_ret((uint32_t)ch);
}

static void hle_ChRelease(void) {
    int32_t ch = (int32_t)psp_arg(0);
    if (ch < 0 || ch >= AUDIO_CHANNELS) { psp_ret(0x80000001); return; }
    g_audio[ch].reserved = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* Common body of the four output calls. `buf_arg` is where the sample buffer
 * sits: the plain pair pass (channel, volume, buffer), the panned pair pass
 * (channel, leftvol, rightvol, buffer). Returns the playback backlog in
 * microseconds, zero when there is nothing to wait for. */
/* The PSP's audio output rate. A buffer of N samples takes N/44100 seconds to
 * play, and that is how long a blocking output waits. */
#define PSP_AUDIO_RATE 44100u

/* Host-clock gaps between a channel's outputs, for the summary. A speaker
 * drains at 44.1kHz whatever the host does, so an output that arrives later
 * than the previous buffer's length is a gap in the sound; this counts them
 * per channel and keeps the longest, which is the number a windowed run's
 * popping comes down to. Only meaningful when the run is paced against the
 * wall clock -- a window, or PSPRECOMP_REALTIME -- and reported then. */
typedef struct { uint64_t first_ns, last_ns, max_gap_ns; uint32_t late, outputs; } audio_gap;
static audio_gap g_audio_gap[AUDIO_OUTPUTS];
static void audio_note_gap(uint32_t ch, uint32_t samples) {
    audio_gap *g = &g_audio_gap[ch];
    /* Less host pauses: a held guest outputs nothing, and the speaker is
     * silenced rather than starved meanwhile. */
    const uint64_t now = psp_clock_run_ns();
    if (g->first_ns) {
        const uint64_t gap = now - g->last_ns;
        const uint64_t buf = (uint64_t)samples * 1000000000ull / PSP_AUDIO_RATE;
        if (gap > g->max_gap_ns) g->max_gap_ns = gap;
        if (gap > buf + buf / 2) g->late++;
    } else g->first_ns = now;
    g->last_ns = now;
    g->outputs++;
}
void psp_audio_dump_gaps(FILE *out) {
    for (uint32_t ch = 0; ch < AUDIO_OUTPUTS; ch++) {
        const audio_gap *g = &g_audio_gap[ch];
        if (g->outputs < 2) continue;
        fprintf(out, "    audio ch %u: %u outputs over %.1f s, longest wait %.0f ms, %u arrived later than 1.5 buffers\n",
                ch, g->outputs, (g->last_ns - g->first_ns) / 1e9, g->max_gap_ns / 1e6, g->late);
    }
}

static int64_t audio_output_common(int buf_arg, int lvol_arg, int rvol_arg) {
    const uint32_t ch = psp_arg(0);
    g_audio_blocks++;
    if (ch >= AUDIO_CHANNELS || !g_audio[ch].reserved || !g_audio[ch].samples)
        return 0;
    audio_note_gap(ch, g_audio[ch].samples);
    audio_dump(ch, g_audio[ch].samples, g_audio[ch].format, psp_arg(buf_arg));
    if (g_audio_out)
        return g_audio_out((int)ch, g_audio[ch].samples, g_audio[ch].format,
                           psp_arg(buf_arg), psp_arg(lvol_arg), psp_arg(rvol_arg));

    /* No host sink, but a blocking output still takes the time the samples
     * take. Returning zero here made a call with "Blocking" in its name return
     * instantly, so the game's audio thread ran flat out instead of at 44.1kHz
     * -- 11 million outputs in a 60-second run, against the ~2,600 a paced
     * thread would make.
     *
     * That was survivable only for as long as the audio thread shared its
     * priority with everything else and the timeslice rotated between equals.
     * The moment sceKernelChangeThreadPriority became real, the game lowered
     * its own main thread from 16 to 40, the audio thread outranked it, and a
     * yield cannot give way to a lower priority -- so it starved the whole game
     * and the run went from 633 GE lists to 3. A spinning thread is not
     * harmless just because nothing has outranked it yet.
     *
     * The wait is hardware's, not a buffer's length: a virtual speaker drains
     * what has been queued at 44.1kHz, and the call waits only for the excess
     * over two buffers in flight -- the one playing and the one queued. A flat
     * wait of one buffer per call ran the audio at 91% of real time under
     * real-time pacing (the wait plus the time to get the token), which read
     * as the picture running away from the sound. */
    audio_ch *a = &g_audio[ch];
    const uint64_t now = psp_clock_peek() * 1000ull;   /* guest microseconds; wall time when paced */
    const uint64_t buf_ns = (uint64_t)a->samples * 1000000000ull / PSP_AUDIO_RATE;
    if (a->play_until_ns < now) a->play_until_ns = now;
    a->play_until_ns += buf_ns;
    const uint64_t ahead = a->play_until_ns - now;
    return ahead > 2 * buf_ns ? (int64_t)((ahead - 2 * buf_ns) / 1000) : 0;
}

static uint32_t audio_ret(void) {
    const uint32_t ch = psp_arg(0);
    return ch < AUDIO_CHANNELS ? g_audio[ch].samples : 0;
}

/* sceAudioOutputBlocking(ch, vol, buf) -- on hardware this blocks until the
 * previous buffer drains, which is what paces a game's audio thread. The
 * backlog is that wait, in guest microseconds. */
static void hle_OutputBlocking(void) {
    const int64_t backlog = audio_output_common(2, 1, 1);
    if (backlog > 0) psp_sched_delay((uint64_t)backlog);
    psp_ret(audio_ret());
}

static void hle_OutputPanned(void) {
    (void)audio_output_common(3, 1, 2);
    psp_ret(audio_ret());
}

static void hle_OutputPannedBlocking(void) {
    const int64_t backlog = audio_output_common(3, 1, 2);
    if (backlog > 0) psp_sched_delay((uint64_t)backlog);
    psp_ret(audio_ret());
}

/* Restored inside the backlog's wait: the buffer was taken before it. */
static void output_blocking_resume(void) {
    (void)psp_sched_resume_delay(NULL);
    psp_ret(audio_ret());
}

/* Zero remaining means "ready for more", so a game's audio loop keeps going. */
static void hle_GetChannelRestLength(void) { psp_ret(0); }

/* (channel, samplecount): the per-call sample count, changed after the
 * reserve. This game calls it thousands of times a run, and it used to be
 * accepted and ignored -- so a channel whose count the game had changed was
 * read at its original length, the wrong number of bytes from every buffer. */
static void hle_SetChannelDataLen(void) {
    const uint32_t ch = psp_arg(0), n = psp_arg(1);
    if (ch >= AUDIO_CHANNELS || !g_audio[ch].reserved) { psp_ret(0x80260008); return; }  /* NOT_RESERVED */
    if (n == 0 || n > 65472 || (n & 63)) { psp_ret(0x80260006); return; }                 /* INVALID_SIZE */
    g_audio[ch].samples = n;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* (channel, samplecount, format): both at once. */
static void hle_ChangeChannelConfig(void) {
    const uint32_t ch = psp_arg(0);
    if (ch >= AUDIO_CHANNELS || !g_audio[ch].reserved) { psp_ret(0x80260008); return; }
    g_audio[ch].format = psp_arg(1);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* Output2 contract: PSPSDK 654ac51 src/audio/pspaudio.h and sceAudio.S.
 * This implementation uses a host queue query, or a single timed transfer
 * without a device. The latter is our scheduling policy, not a claim about
 * firmware FIFO depth or undocumented reservation/status quirks.
 * Error categories below are SDK constants. Exact errors for unspecified
 * edge cases still require independent hardware observations. */
enum {
    O2_BUSY = 0x80260002u, O2_SIZE = 0x80260006u,
    O2_NOT_RESERVED = 0x80260008u, O2_VOLUME = 0x8026000Bu,
    O2_ADDRESS = 0x800200D3u /* PSPSDK src/user/pspkerror.h */
};

/* Which of Output2Blocking's waits a thread is in (psp_sched_set_step). */
enum { O2_STEP_DRAIN = 1, O2_STEP_PLAY };
static void output2_submit(uint32_t volume, uint32_t pcm);

static uint32_t output2_remaining(void) {
    if (g_audio_out)
        return g_audio_pending ? g_audio_pending(AUDIO_OUTPUT2_CHANNEL) : 0;
    const uint64_t now = psp_clock_peek() * 1000ull;
    if (now >= g_output2_until_ns) return 0;
    return (uint32_t)(((g_output2_until_ns - now) * PSP_AUDIO_RATE
                      + 999999999ull) / 1000000000ull);
}

static int output2_length_ok(uint32_t samples) {
    return samples >= 17 && samples <= 4111;
}

static void hle_Output2Reserve(void) {
    audio_ch *channel = &g_audio[AUDIO_OUTPUT2_CHANNEL];
    const uint32_t samples = psp_arg(0);
    if (!output2_length_ok(samples)) { psp_ret(O2_SIZE); return; }
    if (channel->reserved) { psp_ret(O2_BUSY); return; }
    *channel = (audio_ch){.reserved = 1, .samples = samples};
    g_output2_until_ns = 0;
    psp_ret(0);
}

static void hle_Output2ChangeLength(void) {
    audio_ch *channel = &g_audio[AUDIO_OUTPUT2_CHANNEL];
    if (!channel->reserved) { psp_ret(O2_NOT_RESERVED); return; }
    if (!output2_length_ok(psp_arg(0))) { psp_ret(O2_SIZE); return; }
    channel->samples = psp_arg(0);
    psp_ret(0);
}

static void hle_Output2Rest(void) {
    if (!g_audio[AUDIO_OUTPUT2_CHANNEL].reserved) {
        psp_ret(O2_NOT_RESERVED); return;
    }
    psp_ret(output2_remaining());
}

static void hle_Output2Release(void) {
    audio_ch *channel = &g_audio[AUDIO_OUTPUT2_CHANNEL];
    if (!channel->reserved) { psp_ret(O2_NOT_RESERVED); return; }
    if (output2_remaining()) { psp_ret(O2_BUSY); return; }
    *channel = (audio_ch){0};
    g_output2_until_ns = 0;
    psp_ret(0);
}

static void hle_Output2Blocking(void) {
    audio_ch *channel = &g_audio[AUDIO_OUTPUT2_CHANNEL];
    const uint32_t volume = psp_arg(0), pcm = psp_arg(1);
    if (!channel->reserved) { psp_ret(O2_NOT_RESERVED); return; }
    if (volume > 0x8000) { psp_ret(O2_VOLUME); return; }
    if (!pcm || !psp_mem_ptr(pcm, channel->samples * 4)) {
        psp_ret(O2_ADDRESS); return;
    }
    /* No device: finish the previous transfer before accepting the next one.
     * If scheduling is disabled (unit probes), report busy without losing it. */
    if (!g_audio_out && output2_remaining()) {
        const uint64_t now = psp_clock_peek() * 1000ull;
        if (g_output2_until_ns > now) {
            psp_sched_set_step(O2_STEP_DRAIN);
            psp_sched_delay((g_output2_until_ns - now + 999) / 1000);
        }
    }
    output2_submit(volume, pcm);
}

/* The transfer, once the previous one has drained: here, and on a thread a
 * save state restored inside that drain (output2_resume). */
static void output2_submit(uint32_t volume, uint32_t pcm) {
    audio_ch *channel = &g_audio[AUDIO_OUTPUT2_CHANNEL];
    if (!g_audio_out && output2_remaining()) { psp_ret(O2_BUSY); return; }
    const uint32_t samples = channel->samples;
    g_audio_blocks++;
    audio_note_gap(AUDIO_OUTPUT2_CHANNEL, samples);
    audio_dump(AUDIO_OUTPUT2_CHANNEL, samples, 0, pcm);
    int64_t wait_us;
    if (g_audio_out) {
        wait_us = g_audio_out(AUDIO_OUTPUT2_CHANNEL, samples, 0, pcm, volume, volume);
    } else {
        wait_us = ((uint64_t)samples * 1000000ull + PSP_AUDIO_RATE - 1) / PSP_AUDIO_RATE;
        g_output2_until_ns = psp_clock_peek() * 1000ull
                          + (uint64_t)samples * 1000000000ull / PSP_AUDIO_RATE;
    }
    psp_sched_set_step(O2_STEP_PLAY);
    if (wait_us > 0) psp_sched_delay((uint64_t)wait_us);
    psp_ret(0);
}

/* Restored inside one of the call's two waits, which its step tells apart. */
static void output2_resume(void) {
    (void)psp_sched_resume_delay(NULL);
    if (psp_sched_step() == O2_STEP_DRAIN) output2_submit(psp_arg(0), psp_arg(1));
    else psp_ret(0);
}

void psp_misc_reset(void) {
    memset(&volatile_memory,0,sizeof volatile_memory);
    psp_interrupt_reset();
    g_exit_requested = 0;
    g_hold_buttons   = 0;
    g_script_buttons = 0;
    g_script_analog  = 0;
    g_script_ax = g_script_ay = 128;
    atomic_store(&g_host_buttons, 0);
    atomic_store(&g_host_ax, 128);
    atomic_store(&g_host_ay, 128);
    g_ctrl_frame = 0;
    g_ctrl_polls = 0;
    g_read_frame = UINT64_MAX;
    g_ctrl_last_buttons = g_ctrl_pressed_buttons = 0;
    memset(&g_press, 0, sizeof g_press);
    memset(g_audio, 0, sizeof g_audio);
    g_output2_until_ns = 0;
    memset(g_audio_gap, 0, sizeof g_audio_gap);
    g_audio_blocks = 0;
    psp_ctrl_replay_reset();
}

void psp_misc_init(void) {
    psp_misc_reset();
    g_hold_buttons = parse_pad();
    parse_pad_press();
    parse_ramsnap();
    parse_watch_from();
    atomic_store(&g_host_rx, 128);
    atomic_store(&g_host_ry, 128);
    atomic_store(&g_host_mdx, 0);
    atomic_store(&g_host_mdy, 0);
    psp_ctrl_replay_init();
}

/* What a save state keeps here (psprecomp/state.h): the controller's
 * counters and lanes, the volatile-memory lock, the audio channels and their
 * pacing. Not the RTC's base, which a load works out again: fixed when the
 * clock is virtual, as before; today's when it is real time. */
static void misc_keep(void) {
    PSP_STATE_KEEP(volatile_memory);
    PSP_STATE_KEEP(g_script_buttons);
    PSP_STATE_KEEP(g_script_ax);
    PSP_STATE_KEEP(g_script_ay);
    PSP_STATE_KEEP(g_script_analog);
    PSP_STATE_KEEP(g_script_rx);
    PSP_STATE_KEEP(g_script_ry);
    PSP_STATE_KEEP(g_script_mdx);
    PSP_STATE_KEEP(g_script_mdy);
    PSP_STATE_KEEP(g_script_look);
    PSP_STATE_KEEP(g_ctrl_frame);
    PSP_STATE_KEEP(g_ctrl_polls);
    PSP_STATE_KEEP(g_ctrl_last_buttons);
    PSP_STATE_KEEP(g_ctrl_pressed_buttons);
    PSP_STATE_KEEP(g_ctrl_last_ax);
    PSP_STATE_KEEP(g_ctrl_last_ay);
    PSP_STATE_KEEP(g_ctrl_last_rx);
    PSP_STATE_KEEP(g_ctrl_last_ry);
    PSP_STATE_KEEP(g_ctrl_last_mdx);
    PSP_STATE_KEEP(g_ctrl_last_mdy);
    PSP_STATE_KEEP(g_press);
    PSP_STATE_KEEP(g_sample_due);
    PSP_STATE_KEEP(g_read_frame);
    PSP_STATE_KEEP(g_audio);
    PSP_STATE_KEEP(g_output2_until_ns);
    static const psp_state_part part = { .name = "audiodump", .drop = audio_dump_drop };
    psp_state_register(&part);
    psp_ctrl_replay_keep();
}

void psp_misc_register(void) {
    misc_keep();
    psp_hle_register(0x04B7766E, "scePower", "scePowerRegisterCallback", hle_PowerRegisterCallback);

    psp_interrupt_register();

    psp_hle_register(0x79D1C3FA, "UtilsForUser", "sceKernelDcacheWritebackAll",           hle_CacheOp);
    psp_hle_register(0xB435DEC5, "UtilsForUser", "sceKernelDcacheWritebackInvalidateAll", hle_CacheOp);
    psp_hle_register(0x34B9FA9E, "UtilsForUser", "sceKernelDcacheWritebackInvalidateRange", hle_CacheOp);
    psp_hle_register(0x3EE30821, "UtilsForUser", "sceKernelDcacheWritebackRange",         hle_CacheOp);
    psp_hle_register(0xBFA98062, "UtilsForUser", "sceKernelDcacheInvalidateRange",        hle_CacheOp);
    psp_hle_register(0x617F3FE6, "sceDmac", "sceDmacMemcpy",    hle_DmacMemcpy);
    psp_hle_register(0xD97F94D8, "sceDmac", "sceDmacTryMemcpy", hle_DmacTryMemcpy);
    psp_hle_register(0x1839852A, "Kernel_Library", "sceKernelMemcpy", hle_KernelMemcpy);
    psp_hle_register(0xA089ECA4, "Kernel_Library", "sceKernelMemset", hle_KernelMemset);
    psp_hle_register(0x36AA6E91, "sceImpose", "sceImposeSetLanguageMode", hle_ImposeSetLanguageMode);
    psp_hle_register(0xC69BEBCE, "sceOpenPSID", "sceOpenPSIDGetOpenPSID", hle_OpenPSIDGetOpenPSID);
    psp_hle_register(0x27CC57F0, "UtilsForUser", "sceKernelLibcTime",         hle_LibcTime);
    psp_hle_register(0x91E4F6A7, "UtilsForUser", "sceKernelLibcClock",        hle_LibcClock);
    psp_hle_register(0x71EC4271, "UtilsForUser", "sceKernelLibcGettimeofday", hle_LibcGettimeofday);
    psp_hle_register(0x37FB5C42, "UtilsForUser", "sceKernelGetGPI",           hle_GetGPI);

    psp_hle_register(0x172D316E, "StdioForUser", "sceKernelStdin",  hle_Stdin);
    psp_hle_register(0xA6BAB2E9, "StdioForUser", "sceKernelStdout", hle_Stdout);
    psp_hle_register(0xF78BA90A, "StdioForUser", "sceKernelStderr", hle_Stderr);

    psp_hle_register(0xEADB1BD7, "sceSuspendForUser", "sceKernelPowerLock",   hle_ok);
    psp_hle_register(0x3AEE7261, "sceSuspendForUser", "sceKernelPowerUnlock", hle_ok);
    psp_hle_register(0x3E0271D3, "sceSuspendForUser", "sceKernelVolatileMemLock",
                     hle_VolatileMemLock);
    psp_hle_register(0xA14F40B2, "sceSuspendForUser", "sceKernelVolatileMemTryLock",
                     hle_VolatileMemTryLock);
    psp_hle_register(0xA569E425, "sceSuspendForUser", "sceKernelVolatileMemUnlock",
                     hle_VolatileMemUnlock);
    psp_hle_register(0x090CCB3F, "sceSuspendForUser", "sceKernelPowerTick",   hle_ok);

    psp_hle_register(0x05572A5F, "LoadExecForUser", "sceKernelExitGame",             hle_ExitGame);
    psp_hle_register(0x4AC57943, "LoadExecForUser", "sceKernelRegisterExitCallback", hle_RegisterExitCallback);

    psp_hle_register(0xF9275D98, "ModuleMgrForUser", "sceKernelLoadModuleBufferUsbWlan", hle_LoadModuleBufferUsbWlan);
    psp_hle_register(0xF0A26395, "ModuleMgrForUser", "sceKernelGetModuleId",          hle_GetModuleId);
    psp_hle_register(0xD8B73127, "ModuleMgrForUser", "sceKernelGetModuleIdByAddress", hle_GetModuleIdByAddress);
    psp_hle_register(0x50F0C1EC, "ModuleMgrForUser", "sceKernelStartModule",          hle_ModuleOk);
    psp_hle_register(0xD1FF982A, "ModuleMgrForUser", "sceKernelStopModule",           hle_ModuleOk);
    psp_hle_register(0x2E0911AA, "ModuleMgrForUser", "sceKernelUnloadModule",         hle_ModuleOk);

    psp_hle_register(0x1F4011E6, "sceCtrl", "sceCtrlSetSamplingMode",     hle_CtrlSet);
    psp_hle_register(0x6A2774F3, "sceCtrl", "sceCtrlSetSamplingCycle",    hle_CtrlSet);
    psp_hle_register(0x1F803938, "sceCtrl", "sceCtrlReadBufferPositive",  hle_ReadBufferPositive);
    /* Peek differs in not waiting for the next sample, which is a real
     * difference now that the vblank grid exists to sample against. */
    psp_hle_register(0x3A622550, "sceCtrl", "sceCtrlPeekBufferPositive",  hle_PeekBufferPositive);

    psp_hle_register(0x3F7AD767, "sceRtc", "sceRtcGetCurrentTick",         hle_RtcGetCurrentTick);
    psp_hle_register(0xC41C2853, "sceRtc", "sceRtcGetTickResolution",      hle_RtcGetTickResolution);
    psp_hle_register(0x011F03C1, "sceRtc", "sceRtcGetAccumulativeTime",    hle_RtcGetAccumulativeTime);
    psp_hle_register(0x4CFA57B0, "sceRtc", "sceRtcGetCurrentClock",        hle_RtcGetCurrentClock);
    psp_hle_register(0x42307A17, "sceRtc", "sceRtcIsLeapYear",             hle_RtcIsLeapYear);
    psp_hle_register(0x05EF322C, "sceRtc", "sceRtcGetDaysInMonth",         hle_RtcGetDaysInMonth);
    psp_hle_register(0x57726BC1, "sceRtc", "sceRtcGetDayOfWeek",           hle_RtcGetDayOfWeek);
    psp_hle_register(0x4B1B5E82, "sceRtc", "sceRtcCheckValid",             hle_RtcCheckValid);
    psp_hle_register(0x6FF40ACC, "sceRtc", "sceRtcGetTick",                hle_RtcGetTick);
    psp_hle_register(0x7ED29E40, "sceRtc", "sceRtcSetTick",                hle_RtcSetTick);
    psp_hle_register(0x27C4594C, "sceRtc", "sceRtcGetTime_t",              hle_RtcGetTime_t);
    psp_hle_register(0x3A807CC8, "sceRtc", "sceRtcSetTime_t",              hle_RtcSetTime_t);
    psp_hle_register(0x9ED0AE87, "sceRtc", "sceRtcCompareTick",            hle_RtcCompareTick);
    psp_hle_register(0x44F45E05, "sceRtc", "sceRtcTickAddTicks",           hle_RtcTickAddTicks);
    psp_hle_register(0x26D25A5D, "sceRtc", "sceRtcTickAddMicroseconds",    hle_RtcTickAddMicroseconds);
    psp_hle_register(0xF2A4AFE5, "sceRtc", "sceRtcTickAddSeconds",         hle_RtcTickAddSeconds);
    psp_hle_register(0xE6605BCA, "sceRtc", "sceRtcTickAddMinutes",         hle_RtcTickAddMinutes);
    psp_hle_register(0x26D7A24A, "sceRtc", "sceRtcTickAddHours",           hle_RtcTickAddHours);
    psp_hle_register(0xE51B4B7A, "sceRtc", "sceRtcTickAddDays",            hle_RtcTickAddDays);
    psp_hle_register(0xCF3A2CA8, "sceRtc", "sceRtcTickAddWeeks",           hle_RtcTickAddWeeks);
    psp_hle_register(0xDBF74F1B, "sceRtc", "sceRtcTickAddMonths",          hle_RtcTickAddMonths);
    psp_hle_register(0x42842C77, "sceRtc", "sceRtcTickAddYears",           hle_RtcTickAddYears);
    psp_hle_register(0x0498FB3C, "sceRtc", "sceRtcFormatRFC3339",          hle_RtcFormatRFC3339);

    psp_hle_register(0x5EC81C55, "sceAudio", "sceAudioChReserve",            hle_ChReserve);
    psp_hle_register(0x6FC46853, "sceAudio", "sceAudioChRelease",            hle_ChRelease);
    psp_hle_register(0x136CAF51, "sceAudio", "sceAudioOutputBlocking",       hle_OutputBlocking);
    psp_hle_register(0x13F592BC, "sceAudio", "sceAudioOutputPannedBlocking", hle_OutputPannedBlocking);
    psp_hle_register(0xE2D56B2D, "sceAudio", "sceAudioOutputPanned",         hle_OutputPanned);
    psp_hle_register(0xB011922F, "sceAudio", "sceAudioGetChannelRestLength", hle_GetChannelRestLength);
    psp_hle_register(0xCB2E439E, "sceAudio", "sceAudioSetChannelDataLen",    hle_SetChannelDataLen);
    psp_hle_register(0x95FD0C2D, "sceAudio", "sceAudioChangeChannelConfig",  hle_ChangeChannelConfig);
    psp_hle_register(0xB7E1D8E7, "sceAudio", "sceAudioChangeChannelVolume",  hle_ok);
    psp_hle_register(0x01562BA3, "sceAudio", "sceAudioOutput2Reserve", hle_Output2Reserve);
    psp_hle_register(0x2D53F36E, "sceAudio", "sceAudioOutput2OutputBlocking", hle_Output2Blocking);
    psp_hle_register(0x63F2889C, "sceAudio", "sceAudioOutput2ChangeLength", hle_Output2ChangeLength);
    psp_hle_register(0x647CEF33, "sceAudio", "sceAudioOutput2GetRestSample", hle_Output2Rest);
    psp_hle_register(0x43196845, "sceAudio", "sceAudioOutput2Release", hle_Output2Release);

    /* The waits a thread can be parked in when a state is saved, finished
     * on a thread the state restored (psprecomp/state.h). */
    psp_hle_register_resume(0x1F803938, read_buffer_resume);       /* sceCtrlReadBufferPositive */
    psp_hle_register_resume(0x136CAF51, output_blocking_resume);   /* sceAudioOutputBlocking */
    psp_hle_register_resume(0x13F592BC, output_blocking_resume);   /* sceAudioOutputPannedBlocking */
    psp_hle_register_resume(0x2D53F36E, output2_resume);           /* sceAudioOutput2OutputBlocking */
}
