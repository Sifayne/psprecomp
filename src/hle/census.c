/* The park census; see census.h and docs/PLAYER-LAYER.md §5. */
#include "census.h"

#include "psprecomp/clock.h"
#include "psprecomp/cpu.h"
#include "psprecomp/dispatch.h"
#include "psprecomp/hle.h"
#include "psprecomp/os.h"
#include "psprecomp/sched.h"

#include <stdlib.h>
#include <string.h>

/* Per host thread, so per guest thread: the firmware calls in progress and
 * the host frames that called back into guest code. Plain stacks; a depth
 * past their size is still counted, only its entry is not kept. */
enum { CALLS_MAX = 8, NEST_MAX = 8 };
static PSP_THREAD_LOCAL struct { uint32_t nid, site; } t_call[CALLS_MAX];
static PSP_THREAD_LOCAL unsigned t_ncalls;
static PSP_THREAD_LOCAL struct { uint8_t kind; uint32_t addr; } t_nest[NEST_MAX];
static PSP_THREAD_LOCAL unsigned t_nnest;

void psp_census_call_enter(uint32_t nid, uint32_t site) {
    if (t_ncalls < CALLS_MAX) { t_call[t_ncalls].nid = nid; t_call[t_ncalls].site = site; }
    t_ncalls++;
}
void psp_census_call_leave(void) { if (t_ncalls) t_ncalls--; }

void psp_nest_enter(int kind, uint32_t addr) {
    if (t_nnest < NEST_MAX) { t_nest[t_nnest].kind = (uint8_t)kind; t_nest[t_nnest].addr = addr; }
    t_nnest++;
}
void psp_nest_leave(void) { if (t_nnest) t_nnest--; }
unsigned psp_nest_depth(void) { return t_nnest; }

void psp_census_note_park(psp_park *out, int kind, const char *what, uint64_t deadline) {
    out->kind = (uint8_t)kind;
    out->what = what;
    out->deadline = deadline;
    out->calls = (uint8_t)(t_ncalls > 255 ? 255 : t_ncalls);
    const unsigned top = t_ncalls > CALLS_MAX ? CALLS_MAX : t_ncalls;
    out->nid  = top ? t_call[top - 1].nid : 0;
    out->site = top ? t_call[top - 1].site : 0;
    out->nest = (uint8_t)(t_nnest > 255 ? 255 : t_nnest);
    const unsigned keep = t_nnest > PSP_PARK_NEST_MAX ? PSP_PARK_NEST_MAX : t_nnest;
    for (unsigned i = 0; i < keep; i++) {
        out->nest_kind[i] = t_nest[i].kind;
        out->nest_addr[i] = t_nest[i].addr;
    }
}

static const char *nest_name(int kind) {
    switch (kind) {
    case PSP_NEST_CALL_GUEST: return "guest call";
    case PSP_NEST_INTERRUPT:  return "interrupt handler";
    case PSP_NEST_REPLACED:   return "replaced function";
    default:                  return "host frame";
    }
}

void psp_census_print_park(FILE *out, const psp_park *p) {
    static const char *const KIND[] = { "never parked", "blocked", "delayed", "yielded", "preempted" };
    const char *call = p->nid ? psp_hle_name(p->nid) : NULL;
    fprintf(out, "%s", KIND[p->kind < 5 ? p->kind : 0]);
    if (p->nid) fprintf(out, " in %s", call ? call : "(unnamed)");
    else if (p->kind) fprintf(out, " outside a firmware call");
    if (p->what && (!call || strcmp(p->what, call))) fprintf(out, " on \"%s\"", p->what);
    if (p->deadline) fprintf(out, ", until %.3f s", p->deadline / 1e6);
    if (p->site) fprintf(out, ", resumes at 0x%08X", p->site);
    if (p->calls > 1) fprintf(out, ", %u firmware calls deep", p->calls);
    for (unsigned i = 0; i < p->nest && i < PSP_PARK_NEST_MAX; i++)
        fprintf(out, ", under a %s 0x%08X", nest_name(p->nest_kind[i]), p->nest_addr[i]);
    if (p->nest > PSP_PARK_NEST_MAX) fprintf(out, ", %u host frames in all", p->nest);
}

/* PSPRECOMP_PARK_CENSUS: the polls to take a census at, ascending. */
uint32_t psp_census_next;
static uint32_t g_polls[32];
static int g_npolls, g_at;

void psp_census_init(void) {
    const char *spec = getenv("PSPRECOMP_PARK_CENSUS");
    g_npolls = g_at = 0;
    psp_census_next = 0;
    while (spec && *spec && g_npolls < 32) {
        char *end;
        const unsigned long v = strtoul(spec, &end, 10);
        if (end == spec) break;
        if (v) g_polls[g_npolls++] = (uint32_t)v;
        spec = *end ? end + 1 : end;
    }
    /* Sorted, so the next due is always the first left. */
    for (int i = 1; i < g_npolls; i++)
        for (int j = i; j > 0 && g_polls[j] < g_polls[j - 1]; j--) {
            const uint32_t t = g_polls[j]; g_polls[j] = g_polls[j - 1]; g_polls[j - 1] = t;
        }
    if (g_npolls) psp_census_next = g_polls[0];
}

/* The safe point, as stage 5 will define it: a firmware call from the
 * thread's own guest code has completed, on the thread that drives the GE
 * (the one that owns the GL context), or on any thread before the GE has
 * run. Every other thread is parked, since only one runs at a time. */
void psp_census_check(void) {
    const uint32_t polls = psp_ctrl_polls();
    if (!psp_census_next || polls < psp_census_next) return;
    const uint32_t self = psp_sched_current(), owner = psp_ge_owner();
    if (owner && owner != self) return;
    const uint32_t wanted = psp_census_next;
    while (g_at < g_npolls && g_polls[g_at] <= polls) g_at++;
    psp_census_next = g_at < g_npolls ? g_polls[g_at] : 0;

    FILE *out = stderr;
    const uint32_t nid = t_ncalls < CALLS_MAX && t_ncalls ? t_call[t_ncalls - 1].nid : 0;
    const char *call = nid ? psp_hle_name(nid) : NULL;
    fprintf(out, "census: poll %u (asked %u), guest time %.3f s, safe point on 0x%08X \"%s\" after %s,"
                 " resumes at 0x%08X%s\n",
            polls, wanted, psp_clock_peek() / 1e6, self, psp_threadman_thread_name(self),
            call ? call : "(unnamed)", psp_cpu.r[PSP_REG_RA],
            t_nnest ? ", UNDER HOST FRAMES" : "");
    for (unsigned i = 0; i < t_nnest && i < NEST_MAX; i++)
        fprintf(out, "census:   safe point is under a %s 0x%08X\n",
                nest_name(t_nest[i].kind), t_nest[i].addr);
    psp_sched_census(out, self);
    int fed = 0;
    const int movies = psp_mpeg_census(&fed);
    /* 1..3 are a dialog in progress; 4, finished, is only a status word
     * the game has not shut down yet, which a state carries as data. */
    const int dialog = psp_utility_census();
    fprintf(out, "census: movie contexts %d (%d holding stream data), savedata dialog status %d (%s)\n",
            movies, fed, dialog, dialog >= 1 && dialog <= 3 ? "open" : dialog == 4 ? "finished" : "none");
}

/* What resuming this thread would take, in the plan's terms (§5, "What can
 * be resumed, and what is refused"). */
static const char *verdict(const psp_park *p) {
    if (!p->kind) return "not-started";
    if (p->nest) return "refused-host-frames";
    if (!p->nid) return "outside-call";
    if (p->kind == PSP_PARK_YIELD || p->kind == PSP_PARK_PREEMPT) return "switched-in-call";
    return "wait-in-call";
}

void psp_census_row(FILE *out, uint32_t uid, const char *name, const char *state,
                    const psp_park *p, uint32_t entry) {
    static const char *const KIND[] = { "none", "block", "delay", "yield", "preempt" };
    const char *call = p->nid ? psp_hle_name(p->nid) : NULL;
    fprintf(out, "census-row\t0x%08X\t%s\t%s\t%s\t%s\t%s\t0x%08X\t%u",
            uid, name, state, KIND[p->kind < 5 ? p->kind : 0],
            p->nid ? (call ? call : "(unnamed)") : "-", p->what ? p->what : "-",
            p->kind ? p->site : entry, p->nest);
    for (unsigned i = 0; i < p->nest && i < PSP_PARK_NEST_MAX; i++)
        fprintf(out, "%s%s 0x%08X", i ? "; " : "\t", nest_name(p->nest_kind[i]), p->nest_addr[i]);
    if (!p->nest) fprintf(out, "\t-");
    fprintf(out, "\t%s\n", verdict(p));
}
