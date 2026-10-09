/* The safe point and the host pause; see psprecomp/safepoint.h. */
#include "psprecomp/safepoint.h"
#include "census.h"

#include "psprecomp/clock.h"
#include "psprecomp/hle.h"
#include "psprecomp/os.h"
#include "psprecomp/sched.h"
#include "psprecomp/state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Atomic int psp_safepoint_armed;

/* ---- the frame-boundary calls -------------------------------------------- */

static const char *const DEFAULT_CALLS[] = {
    "sceCtrlReadBufferPositive", "sceCtrlPeekBufferPositive",
    "sceCtrlReadBufferNegative", "sceCtrlPeekBufferNegative", NULL,
};
static const char *const *g_names = DEFAULT_CALLS;
enum { NIDS_MAX = 16 };
static uint32_t g_nids[NIDS_MAX];
static int g_nnids = -1;            /* resolved on first use, once registered */

void psp_safepoint_set_calls(const char *const *names) {
    g_names = names ? names : DEFAULT_CALLS;
    g_nnids = -1;
}

static void resolve(void) {
    int count = 0;
    const psp_hle_entry *e = psp_hle_entries(&count);
    g_nnids = 0;
    for (int n = 0; g_names[n] && g_nnids < NIDS_MAX; n++)
        for (int i = 0; i < count; i++)
            if (e[i].name && !strcmp(e[i].name, g_names[n])) { g_nids[g_nnids++] = e[i].nid; break; }
}

static int boundary(uint32_t nid) {
    if (g_nnids < 0) resolve();
    for (int i = 0; i < g_nnids; i++) if (g_nids[i] == nid) return 1;
    return 0;
}

/* ---- holding ---------------------------------------------------------------- */

static _Atomic int g_request, g_held;
static _Atomic uint64_t g_holds;
static void (*g_redraw)(void);
static struct { uint32_t poll, ms; } g_script[32];
static int g_nscript, g_script_at;

void psp_pause_request(int hold) {
    g_request = hold;
    if (hold) psp_safepoint_armed = 1;
}
int psp_pause_requested(void) { return g_request; }
int psp_paused(void) { return g_held; }
uint64_t psp_pause_count(void) { return g_holds; }
void psp_pause_set_redraw(void (*redraw)(void)) { g_redraw = redraw; }

/* Armed while anything wants the safe point. A request arriving between the
 * computation and the store is caught by the second look: it stored its arm
 * before this re-reads the request, or this re-reads it after. */
void psp_safepoint_rearm(void) {
    psp_safepoint_armed = g_request || psp_census_next || g_script_at < g_nscript ||
                          psp_state_scripted_pending();
    if (g_request) psp_safepoint_armed = 1;
}

void psp_safepoint_init(void) {
    psp_state_init();
    const char *spec = getenv("PSPRECOMP_PAUSE_AT");
    g_nscript = g_script_at = 0;
    while (spec && *spec && g_nscript < 32) {
        char *end;
        const unsigned long poll = strtoul(spec, &end, 10);
        if (end == spec || *end != ':') break;
        const unsigned long ms = strtoul(end + 1, &end, 10);
        g_script[g_nscript].poll = (uint32_t)poll;
        g_script[g_nscript].ms = (uint32_t)ms;
        g_nscript++;
        spec = *end ? end + 1 : end;
    }
    psp_safepoint_rearm();
}

/* Hold the guest: for `ms` of wall time, or (0) until the request is
 * withdrawn. This thread keeps the scheduler token, so nothing of the guest
 * runs; the redraw keeps the window alive at about display rate. A host
 * that is stopping the run ends the hold at once. */
static void hold(uint32_t ms, const char *why) {
    const uint64_t start = psp_os_mono_ns();
    fprintf(stderr, "pause: %s at poll %u, guest time %.3f s\n", why, psp_ctrl_polls(),
            psp_clock_peek() / 1e6);
    psp_clock_hold();
    g_held = 1;
    unsigned redraws = 0;
    while (!psp_sched_stopping()) {
        const uint64_t now = psp_os_mono_ns();
        if (ms ? now - start >= (uint64_t)ms * 1000000u : !g_request) break;
        psp_state_held();
        if (g_redraw) { g_redraw(); redraws++; }
        uint64_t next = now + 16666667u;
        if (ms && next > start + (uint64_t)ms * 1000000u) next = start + (uint64_t)ms * 1000000u;
        psp_os_sleep_until_ns(next);
    }
    g_held = 0;
    psp_clock_release();
    g_holds++;
    fprintf(stderr, "pause: released after %.3f s, guest time %.3f s, %u redraws\n",
            (psp_os_mono_ns() - start) / 1e9, psp_clock_peek() / 1e6, redraws);
}

static uint32_t g_at;     /* the call the safe point is in, while it is */
uint32_t psp_safepoint_nid(void) { return g_at; }
void psp_safepoint_leave(void) { g_at = 0; }

void psp_safepoint(uint32_t nid) {
    if (!boundary(nid)) return;
    /* On the thread that owns the GL context -- the last to run a display
     * list -- or on any before the GE has run. */
    const uint32_t owner = psp_ge_owner();
    if (owner && owner != psp_sched_current()) return;
    g_at = nid;
    if (psp_census_next) psp_census_check();
    if (psp_state_scripted_pending()) psp_state_scripted();
    const uint32_t polls = psp_ctrl_polls();
    while (g_script_at < g_nscript && polls >= g_script[g_script_at].poll) {
        char why[48];
        snprintf(why, sizeof why, "scripted hold of %u ms", g_script[g_script_at].ms);
        hold(g_script[g_script_at].ms, why);
        g_script_at++;
    }
    if (g_request) hold(0, "held");
    g_at = 0;
    psp_safepoint_rearm();
}
