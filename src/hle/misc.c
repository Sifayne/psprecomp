/* psprecomp — the smaller firmware libraries.
 *
 * Kernel_Library, UtilsForUser, StdioForUser, sceSuspendForUser,
 * LoadExecForUser, ModuleMgrForUser, sceCtrl, sceAudio and scePower.
 * Individually small,
 * but collectively they are what a game's C runtime needs before main() gets
 * anywhere -- newlib's reentrancy setup alone wants interrupt masking, a
 * clock, and the standard file descriptors.
 */

#include "psprecomp/hle.h"
#include "psprecomp/sched.h"
#include "psprecomp/clock.h"

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

/* ---- Kernel_Library ------------------------------------------------------
 * Interrupt masking. With no interrupts to mask, the pair only has to be
 * *consistent*: suspend returns a cookie that resume accepts. Games use them
 * to bracket short critical sections, and libc's lightweight mutexes are built
 * on them -- which is why a game stalls in its own startup without these. */

static uint32_t g_intr_enabled = 1;

static void hle_CpuSuspendIntr(void) {
    uint32_t prev = g_intr_enabled;
    g_intr_enabled = 0;
    psp_ret(prev);                    /* the cookie resume expects */
}

static void hle_CpuResumeIntr(void) {
    g_intr_enabled = psp_arg(0);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* ---- UtilsForUser -------------------------------------------------------- */

/* Cache maintenance. There is no cache to write back -- the recompiled code
 * and the GE share one flat backing store -- so these are genuinely no-ops
 * rather than unimplemented. */
static void hle_CacheOp(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

static void hle_LibcTime(void) {
    time_t t = time(NULL);
    uint32_t out = psp_arg(0);
    if (out) psp_write32(out, (uint32_t)t);
    psp_ret((uint32_t)t);
}

static void hle_LibcClock(void) {
    /* Microseconds since start. A game that uses this for frame pacing needs
     * it to advance, so it is derived from the host clock rather than being a
     * constant -- a frozen clock makes a game either spin or run at infinite
     * speed, both of which look like a hang. */
    psp_ret((uint32_t)((uint64_t)clock() * 1000000ull / CLOCKS_PER_SEC));
}

static void hle_LibcGettimeofday(void) {
    uint32_t tv = psp_arg(0);
    if (tv) {
        time_t t = time(NULL);
        psp_write32(tv, (uint32_t)t);
        psp_write32(tv + 4, 0);
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

/* Volatile memory: the 4MB between the kernel area and user RAM.
 *
 * It belongs to the UMD cache, and a game may borrow it -- typically as the
 * scratch buffer it decompresses an archive into. The call reports the block
 * back through two out-parameters, and a stub that returned "success" while
 * writing neither left the game holding a null pointer and a length of zero. It
 * then walked a table through that pointer, which is how this surfaced: an
 * endless run of bad accesses just past the end of .bss, in a structure whose
 * two neighbouring fields were the very pointers passed in here.
 *
 * Where the block sits is this file's choice rather than a fact to look up:
 * the call reports address and size through out-parameters and the guest uses
 * what it is handed. Two things constrain the choice. It has to be mapped,
 * which it is -- PSP_RAM_BASE is 0x08000000 and the RAM is 32MB -- and it has
 * to sit below the user heap so that psp_sysmem_alloc can never hand the same
 * bytes out twice. User memory begins at 0x08800000 (uofw documents the map),
 * so the 4MB immediately under it is free for this and is where hardware keeps
 * the UMD cache the call is borrowing. */
#define PSP_VOLATILE_BASE 0x08400000u
#define PSP_VOLATILE_SIZE 0x00400000u

static int g_volatile_held;

static void volatile_grant(void) {
    const uint32_t ptr_out = psp_arg(1), size_out = psp_arg(2);
    if (ptr_out)  psp_write32(ptr_out,  PSP_VOLATILE_BASE);
    if (size_out) psp_write32(size_out, PSP_VOLATILE_SIZE);
    g_volatile_held = 1;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* Lock blocks until the block is free; nothing else here ever takes it, so it
 * is always free and the two differ only in what they would do under
 * contention. TryLock is the one games actually call. */
static void hle_VolatileMemLock(void)    { volatile_grant(); }
static void hle_VolatileMemTryLock(void) { volatile_grant(); }

static void hle_VolatileMemUnlock(void) {
    g_volatile_held = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
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

/* The SceCtrlData timestamp field, one per *sample* written. Games do
 * arithmetic on it, so it counts what it has always counted. */
static uint32_t         g_ctrl_frame;
/* One per *call*, which is the unit a scripted input has to be keyed on: it
 * is "one thing the guest did", and unlike g_ctrl_frame it does not move when
 * a game changes how many samples it asks for per poll. */
static uint32_t         g_ctrl_polls;

uint32_t psp_ctrl_polls(void)   { return g_ctrl_polls; }
uint32_t psp_ctrl_samples(void) { return g_ctrl_frame; }

void psp_ctrl_set(uint32_t buttons, uint8_t ax, uint8_t ay) {
    atomic_store(&g_host_buttons, buttons);
    atomic_store(&g_host_ax, ax);
    atomic_store(&g_host_ay, ay);
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

/* SceCtrlData: u32 timestamp, u32 buttons, u8 lx, u8 ly, then padding to 16. */
static void hle_ReadBufferPositive(void) {
    const uint64_t us = psp_clock_peek();
    g_ctrl_polls++;
    pad_press_step(us);
    psp_ctrl_replay_step(g_ctrl_polls, us);

    /* The merge. Buttons OR; the stick belongs to whoever last claimed it. */
    const uint32_t host = atomic_load(&g_host_buttons);
    const uint8_t  hax  = atomic_load(&g_host_ax);
    const uint8_t  hay  = atomic_load(&g_host_ay);
    const uint32_t buttons = g_hold_buttons | g_script_buttons | host;
    const uint8_t  ax = g_script_analog ? g_script_ax : hax;
    const uint8_t  ay = g_script_analog ? g_script_ay : hay;

    /* A scenario driving a run that also has a hand on the pad is still a
     * useful run -- it is how you take over one that is stuck -- but it is no
     * longer the scenario's run, and that has to be impossible to miss. */
    if (host || hax != 128 || hay != 128) psp_ctrl_replay_taint(g_ctrl_polls);
    psp_ctrl_replay_record(g_ctrl_polls, us, buttons, ax, ay);

    uint32_t buf = psp_arg(0), count = psp_arg(1);
    if (!count) count = 1;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t at = buf + i * 16;
        psp_write32(at, g_ctrl_frame++);
        psp_write32(at + 4, buttons);
        psp_write8(at + 8, ax);
        psp_write8(at + 9, ay);
        for (int k = 10; k < 16; k++) psp_write8(at + (uint32_t)k, 0);
    }
    psp_ret(count);
}

/* ---- sceAudio ------------------------------------------------------------ */

#define AUDIO_CHANNELS 8

typedef struct { int reserved; uint32_t samples; uint32_t format; } audio_ch;
static audio_ch g_audio[AUDIO_CHANNELS];
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
                              uint32_t buf);

void psp_audio_set_output(int64_t (*fn)(int ch, uint32_t samples, uint32_t fmt,
                                        uint32_t buf)) {
    g_audio_out = fn;
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

static int64_t audio_output_common(int buf_arg) {
    const uint32_t ch = psp_arg(0);
    g_audio_blocks++;
    if (ch >= AUDIO_CHANNELS || !g_audio[ch].reserved || !g_audio[ch].samples)
        return 0;
    if (g_audio_out)
        return g_audio_out((int)ch, g_audio[ch].samples, g_audio[ch].format,
                           psp_arg(buf_arg));

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
     * harmless just because nothing has outranked it yet. */
    return (int64_t)g_audio[ch].samples * 1000000 / PSP_AUDIO_RATE;
}

static uint32_t audio_ret(void) {
    const uint32_t ch = psp_arg(0);
    return ch < AUDIO_CHANNELS ? g_audio[ch].samples : 0;
}

/* sceAudioOutputBlocking(ch, vol, buf) -- on hardware this blocks until the
 * previous buffer drains, which is what paces a game's audio thread. The
 * backlog is that wait, in guest microseconds. */
static void hle_OutputBlocking(void) {
    const int64_t backlog = audio_output_common(2);
    if (backlog > 0) psp_sched_delay((uint64_t)backlog);
    psp_ret(audio_ret());
}

static void hle_OutputPanned(void) {
    (void)audio_output_common(3);
    psp_ret(audio_ret());
}

static void hle_OutputPannedBlocking(void) {
    const int64_t backlog = audio_output_common(3);
    if (backlog > 0) psp_sched_delay((uint64_t)backlog);
    psp_ret(audio_ret());
}

/* Zero remaining means "ready for more", so a game's audio loop keeps going. */
static void hle_GetChannelRestLength(void) { psp_ret(0); }

void psp_misc_reset(void) {
    g_intr_enabled = 1;
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
    memset(&g_press, 0, sizeof g_press);
    memset(g_audio, 0, sizeof g_audio);
    g_audio_blocks = 0;
    psp_ctrl_replay_reset();
}

void psp_misc_init(void) {
    psp_misc_reset();
    g_hold_buttons = parse_pad();
    parse_pad_press();
    psp_ctrl_replay_init();
}

void psp_misc_register(void) {
    psp_hle_register(0x04B7766E, "scePower", "scePowerRegisterCallback", hle_PowerRegisterCallback);

    psp_hle_register(0x092968F4, "Kernel_Library", "sceKernelCpuSuspendIntr", hle_CpuSuspendIntr);
    psp_hle_register(0x5F10D406, "Kernel_Library", "sceKernelCpuResumeIntr",  hle_CpuResumeIntr);

    psp_hle_register(0x79D1C3FA, "UtilsForUser", "sceKernelDcacheWritebackAll",           hle_CacheOp);
    psp_hle_register(0xB435DEC5, "UtilsForUser", "sceKernelDcacheWritebackInvalidateAll", hle_CacheOp);
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

    /* Observed but unidentified. The game calls this three times on its
     * heap-setup path, and an unimplemented call returns 0 -- which for a
     * module query means "no module" and makes heap establishment fail.
     * Eighteen plausible ModuleMgr names were tried against SHA-1 with no
     * match, so the name is genuinely unknown and is not invented here. */
    psp_hle_register_unnamed(0xF9275D98, "ModuleMgrForUser", hle_GetModuleId);
    psp_hle_register(0xF0A26395, "ModuleMgrForUser", "sceKernelGetModuleId",          hle_GetModuleId);
    psp_hle_register(0xD8B73127, "ModuleMgrForUser", "sceKernelGetModuleIdByAddress", hle_GetModuleIdByAddress);
    psp_hle_register(0x50F0C1EC, "ModuleMgrForUser", "sceKernelStartModule",          hle_ModuleOk);
    psp_hle_register(0xD1FF982A, "ModuleMgrForUser", "sceKernelStopModule",           hle_ModuleOk);
    psp_hle_register(0x2E0911AA, "ModuleMgrForUser", "sceKernelUnloadModule",         hle_ModuleOk);

    psp_hle_register(0x1F4011E6, "sceCtrl", "sceCtrlSetSamplingMode",     hle_CtrlSet);
    psp_hle_register(0x6A2774F3, "sceCtrl", "sceCtrlSetSamplingCycle",    hle_CtrlSet);
    psp_hle_register(0x1F803938, "sceCtrl", "sceCtrlReadBufferPositive",  hle_ReadBufferPositive);
    /* Peek differs only in not waiting for the next sample. Nothing samples
     * here, so the two are the same call. */
    psp_hle_register(0x3A622550, "sceCtrl", "sceCtrlPeekBufferPositive",  hle_ReadBufferPositive);

    psp_hle_register(0x5EC81C55, "sceAudio", "sceAudioChReserve",            hle_ChReserve);
    psp_hle_register(0x6FC46853, "sceAudio", "sceAudioChRelease",            hle_ChRelease);
    psp_hle_register(0x136CAF51, "sceAudio", "sceAudioOutputBlocking",       hle_OutputBlocking);
    psp_hle_register(0x13F592BC, "sceAudio", "sceAudioOutputPannedBlocking", hle_OutputPannedBlocking);
    psp_hle_register(0xE2D56B2D, "sceAudio", "sceAudioOutputPanned",         hle_OutputPanned);
    psp_hle_register(0xB011922F, "sceAudio", "sceAudioGetChannelRestLength", hle_GetChannelRestLength);
    psp_hle_register(0xCB2E439E, "sceAudio", "sceAudioSetChannelDataLen",    hle_ok);
    psp_hle_register(0x95FD0C2D, "sceAudio", "sceAudioChangeChannelConfig",  hle_ok);
    psp_hle_register(0xB7E1D8E7, "sceAudio", "sceAudioChangeChannelVolume",  hle_ok);
}
