/* psprecomp — HLE dispatch. See include/psprecomp/hle.h. */

#include "psprecomp/hle.h"
#include "psprecomp/clock.h"
#include "psprecomp/sched.h"
#include "psprecomp/dispatch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HLE_MAX 512

static psp_hle_entry g_entry[HLE_MAX];
static psp_hle_fn    g_fn[HLE_MAX];
static int           g_count;

void psp_hle_register(uint32_t nid, const char *lib, const char *name, psp_hle_fn fn) {
    /* Re-registering replaces, so a game repo can override one function
     * without forking the table. */
    for (int i = 0; i < g_count; i++) {
        if (g_entry[i].nid == nid) { g_fn[i] = fn; g_entry[i].name = name; return; }
    }
    if (g_count >= HLE_MAX) return;
    g_entry[g_count].nid = nid;
    g_entry[g_count].lib = lib;
    g_entry[g_count].name = name;
    g_fn[g_count] = fn;
    g_count++;
}

/* Register a NID whose name is genuinely unknown.
 *
 * Every named entry is verified against SHA-1 of its own name, which is what
 * makes the table self-checking -- and which correctly refuses a made-up name.
 * But a firmware entry point can be *observed* without being *identified*: the
 * game calls it, the NID is exact, and no plausible name hashes to it.
 *
 * The wrong response is to invent a name so the entry can be registered. That
 * produces a table which runs, looks right, and lies about what it implements.
 * The right response is to make "unidentified" representable: registered and
 * callable, marked as unnamed, and skipped by the NID check by construction
 * rather than by exception. */
void psp_hle_register_unnamed(uint32_t nid, const char *lib, psp_hle_fn fn) {
    psp_hle_register(nid, lib, NULL, fn);
}

int psp_hle_is_named(int index) {
    return index >= 0 && index < g_count && g_entry[index].name != NULL;
}

const char *psp_hle_name(uint32_t nid) {
    for (int i = 0; i < g_count; i++)
        if (g_entry[i].nid == nid) return g_entry[i].name;
    return NULL;
}

int psp_hle_count(void) { return g_count; }

const psp_hle_entry *psp_hle_entries(int *count) {
    if (count) *count = g_count;
    return g_entry;
}

/* A short history of firmware calls that returned zero.
 *
 * Zero is the value a game most often treats as an address, so a stubbed or
 * failing call returning it tends to surface far away as a wild pointer --
 * typically a table walk starting near address 0. When that happens the useful
 * question is "which call handed the game a null?", and by then the call is
 * long gone. Keeping the last few makes the answer immediate instead of
 * requiring a second run with different instrumentation. */
#define ZERO_HISTORY 16
static struct { uint32_t nid; const char *name; int handled; } g_zero[ZERO_HISTORY];
static int g_zero_n;

/* Suppresses the unimplemented-call message. Two reasons a batch caller wants
 * this, and the second one matters more than it looks:
 *
 *   Volume. A differential run makes millions of firmware calls. One line of
 *   stderr each is slower than the work being measured.
 *
 *   Deadlock. A harness that puts a wall-clock watchdog around recompiled code
 *   has to longjmp out of a signal handler. If that handler interrupts an
 *   fprintf, stdio's stream lock is still held, and the next fprintf blocks on
 *   it forever -- a hang no timeout escapes, because the timeout is the thing
 *   that caused it.
 *
 * The history behind psp_hle_dump_recent() is still recorded either way, so
 * quiet mode loses the noise and keeps the diagnosis. */
static int g_quiet;

void psp_hle_set_quiet(int quiet) { g_quiet = quiet; }

/* `handled` distinguishes "we ran a handler that returned zero" from "no
 * handler existed". A NULL name used to stand for both, so a function
 * registered without a known name -- psp_hle_register_unnamed, for the NIDs
 * whose exported symbol is not known -- was reported as unimplemented. That is
 * actively misleading during bring-up: it sends you looking for a handler that
 * is already there. */
static void note_zero(uint32_t nid, const char *name, int handled) {
    g_zero[g_zero_n % ZERO_HISTORY].nid = nid;
    g_zero[g_zero_n % ZERO_HISTORY].name = name;
    g_zero[g_zero_n % ZERO_HISTORY].handled = handled;
    g_zero_n++;
}

void psp_hle_dump_recent(FILE *out) {
    if (!g_zero_n) {
        fprintf(out, "  (no firmware call returned zero)\n");
        return;
    }
    int n = g_zero_n < ZERO_HISTORY ? g_zero_n : ZERO_HISTORY;
    /* Read this list with care: SCE_KERNEL_ERROR_OK is also zero, so a
     * successful call that returns nothing appears here exactly like a
     * handle-returning call that failed. The entries worth suspecting are the
     * ones whose name implies an address or an id -- GetBlockHeadAddr,
     * GetModuleId, the Create/Alloc family. The rest are noise.
     *
     * That ambiguity is a limitation of this instrument, not of the runtime:
     * distinguishing the two needs per-function knowledge of what the return
     * value means, which the table does not currently carry. */
    fprintf(out, "  last %d firmware calls that returned zero, newest first\n"
                 "  (note: success is also zero -- see the comment in hle.c):\n", n);
    for (int i = 1; i <= n; i++) {
        int k = (g_zero_n - i) % ZERO_HISTORY;
        fprintf(out, "    0x%08X  %s\n", g_zero[k].nid,
                g_zero[k].name ? g_zero[k].name
                               : (g_zero[k].handled ? "(handled, name unknown)"
                                                    : "(unimplemented)"));
    }
}

/* How many times each registered function was called, plus the unimplemented
 * ones lumped together.
 *
 * The zero-history above answers "what handed the game a null?". This answers a
 * different question that comes up just as often: "what is the game *doing*?"
 * A run that stops making progress has a shape -- a hundred thousand
 * __sceSasCore and no sceDisplayWaitVblank is an audio loop with no frame loop,
 * and that is visible here in one line and nowhere else. */
static uint64_t g_calls[HLE_MAX];
static uint64_t g_calls_unimpl;

/* An ordered log of every firmware call, enabled by PSPRECOMP_HLE_LOG=1.
 *
 * The histogram says what a run did a lot of; this says what it did, in order,
 * with arguments and results. That is what identifies the call a game gave up
 * after -- a question the totals cannot answer, because the interesting call
 * happened exactly once. Off by default: it is one line per call. */
static int g_log = -1;

static int logging(void);

int psp_hle_logging(void) { return logging(); }

static int logging(void) {
    if (g_log < 0) {
        const char *v = getenv("PSPRECOMP_HLE_LOG");
        g_log = (v && *v && *v != '0') ? 1 : 0;
    }
    return g_log;
}

/* PSPRECOMP_HLE_TRACE=<name> dumps the function trace at every call to that
 * firmware function.
 *
 * Knowing a game called sceIoGetstat is rarely the question; knowing *which* of
 * its loaders called it, and therefore what it does with the answer, is. The
 * call is the one moment where guest code is stopped at a known point with its
 * whole chain still on the trace ring. */
static const char *trace_name(void) {
    static const char *n;
    static int looked;
    if (!looked) { looked = 1; n = getenv("PSPRECOMP_HLE_TRACE"); if (n && !*n) n = NULL; }
    return n;
}

void psp_hle_dump_calls(FILE *out, int top) {
    /* Selection sort over indices: g_count is a couple of hundred and this runs
     * once, at exit. */
    int order[HLE_MAX];
    int n = 0;
    for (int i = 0; i < g_count; i++) if (g_calls[i]) order[n++] = i;
    for (int a = 0; a < n; a++)
        for (int b = a + 1; b < n; b++)
            if (g_calls[order[b]] > g_calls[order[a]]) {
                const int t = order[a]; order[a] = order[b]; order[b] = t;
            }

    if (!n) { fprintf(out, "  (no firmware calls)\n"); return; }
    if (top > n) top = n;
    for (int i = 0; i < top; i++) {
        const int e = order[i];
        fprintf(out, "  %10llu  %s\n", (unsigned long long)g_calls[e],
                g_entry[e].name ? g_entry[e].name : "(unnamed)");
    }
    if (g_calls_unimpl)
        fprintf(out, "  %10llu  (unimplemented)\n",
                (unsigned long long)g_calls_unimpl);
}

void psp_hle_call(uint32_t nid) {
    /* Every firmware call costs a tick of guest time.
     *
     * The clock advanced three ways and every one of them could stop. A vblank
     * moves it a frame, but only the thread that waits on vblank calls that. A
     * *read* moves it a little, for the spin-on-GetSystemTime loop. And the
     * scheduler moves it to the earliest deadline when nothing is runnable.
     *
     * A thread that spins on firmware calls which are neither -- this game's
     * decode loop runs 562M sceMpegRingbufferAvailableSize and 281M
     * sceKernelSignalSema without touching the clock -- keeps *something*
     * runnable forever, so the scheduler's fallback never fires, while calling
     * nothing that moves time itself. Guest time then stops dead, and any
     * thread sleeping on a deadline sleeps through the rest of the run. That is
     * what parked the movie's own frame loop in a 16.9ms delay it could never
     * leave.
     *
     * Charging a tick per call is the same trade clock.h already makes for
     * reads, for the same reason: unfaithful timing beats a hang. It keeps the
     * clock deterministic, which is what the oracle needs, because both sides
     * make the same calls in the same order. */
    psp_clock_tick();

    for (int i = 0; i < g_count; i++) {
        if (g_entry[i].nid == nid) {
            g_calls[i]++;
            /* Logged before the call, not after: sceKernelExitThread and the
             * exit-the-game calls never return, so a line emitted afterwards
             * would omit exactly the call that ended the run. The result comes
             * back on its own line for the calls that do return. */
            if (logging())
                fprintf(stderr, "hle: [%05X] %-36s(0x%08X, 0x%08X, 0x%08X, 0x%08X)\n",
                        psp_sched_current(),
                        g_entry[i].name ? g_entry[i].name : "(unnamed)",
                        psp_arg(0), psp_arg(1), psp_arg(2), psp_arg(3));
            {
                const char *tn = trace_name();
                if (tn && g_entry[i].name && !strcmp(tn, g_entry[i].name)) {
                    fprintf(stderr, "hle: --- trace at %s ---\n", g_entry[i].name);
                    psp_trace_dump();
                }
            }
            g_fn[i]();
            if (logging())
                fprintf(stderr, "hle: [%05X] %-36s  = 0x%08X\n",
                        psp_sched_current(), "", psp_cpu.r[PSP_REG_V0]);
            if (psp_cpu.r[PSP_REG_V0] == 0) note_zero(nid, g_entry[i].name, 1);
            /* After the handler, not before: the call has to finish before the
             * thread can be switched away from, or its result is written into
             * whoever runs next. */
            psp_sched_tick();
            /* After the handler and after the reschedule: a timer handler is
             * guest code, and running it before the call it interrupted has
             * finished would write its result into the caller's $v0. */
            psp_ktimer_tick();
            return;
        }
    }
    note_zero(nid, NULL, 0);
    g_calls_unimpl++;
    if (logging())
        fprintf(stderr, "hle: [%05X] 0x%08X <unimplemented>(0x%08X, 0x%08X, 0x%08X, 0x%08X)\n",
                psp_sched_current(), nid,
                psp_arg(0), psp_arg(1), psp_arg(2), psp_arg(3));

    /* Unimplemented. Naming the function is the whole point — bringing a game
     * up is largely the process of watching this message stop appearing, and
     * "0x237DBD4F" is far less use than "sceKernelAllocPartitionMemory".
     *
     * Returning 0 rather than aborting is deliberate: many firmware calls are
     * advisory (version reporting, profiling hooks) and a game will run past
     * them happily. One that genuinely needed the result will fail visibly
     * soon after, with this line already in the log. */
    if (!g_quiet)
        fprintf(stderr, "psprecomp: unimplemented firmware call 0x%08X\n", nid);
    psp_ret(0);
}

const char *psp_str(uint32_t addr, char *dst, size_t cap) {
    size_t n = 0;
    if (cap == 0) return dst;
    while (n + 1 < cap) {
        uint8_t c = psp_read8(addr + (uint32_t)n);
        if (!c) break;
        dst[n++] = (char)c;
    }
    dst[n] = '\0';
    return dst;
}

static void miss_context(void) { psp_trace_dump(); psp_hle_dump_recent(stderr); }

void psp_hle_init(void) {
    psp_set_miss_context(miss_context);
    psp_sysmem_init();
    psp_sysmem_register();
    psp_sched_init();
    psp_threadman_init();
    psp_kernlock_reset();
    psp_kernobj_reset();
    psp_ktimer_reset();
    psp_mpeg_register();
    psp_mpeg_reset();
    psp_threadman_register();
    psp_kernlock_register();
    psp_kernobj_register();
    psp_ktimer_register();
    psp_display_init();
    psp_display_register();
    psp_ge_init();
    psp_ge_register();
    psp_sas_init();
    psp_sas_register();
    psp_io_init();
    psp_io_register();
    psp_misc_init();
    psp_misc_register();
    psp_umd_init();
    psp_umd_register();
    psp_utility_init();
    psp_utility_register();
}
