/* psprecomp — address → function dispatch. See include/psprecomp/dispatch.h. */

#include "psprecomp/dispatch.h"
#include "psprecomp/cpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* An open-addressed hash table. A module has a few thousand functions and
 * lookups happen on every indirect call, so this wants to be cheap; addresses
 * are 4-aligned and densely clustered, which a power-of-two table with a
 * multiplicative hash handles well. */

typedef struct {
    uint32_t addr;
    psp_fn_t fn;
    int      used;
} slot;

static slot   *g_table;
static uint32_t g_cap;      /* always a power of two */
static uint32_t g_count;
static uint64_t g_misses;
static psp_miss_fn_t g_miss;
static psp_miss_ctx_fn_t g_miss_ctx;

void psp_set_miss_context(psp_miss_ctx_fn_t fn) { g_miss_ctx = fn; }
void psp_miss_context(void) { if (g_miss_ctx) g_miss_ctx(); }

static uint32_t hash_addr(uint32_t a) {
    /* Knuth multiplicative. The low two bits are always zero, so shift them
     * out first or a quarter of the table would never be used. */
    return (a >> 2) * 2654435761u;
}

static void grow(void) {
    uint32_t ncap = g_cap ? g_cap * 2 : 4096;
    slot *nt = (slot *)calloc(ncap, sizeof *nt);
    if (!nt) return;

    for (uint32_t i = 0; i < g_cap; i++) {
        if (!g_table[i].used) continue;
        uint32_t m = ncap - 1;
        uint32_t j = hash_addr(g_table[i].addr) & m;
        while (nt[j].used) j = (j + 1) & m;
        nt[j] = g_table[i];
    }
    free(g_table);
    g_table = nt;
    g_cap = ncap;
}

void psp_register(uint32_t addr, psp_fn_t fn) {
    if (!g_table || (g_count + 1) * 4 >= g_cap * 3) grow();   /* keep load < 0.75 */
    if (!g_table) return;

    uint32_t m = g_cap - 1;
    uint32_t j = hash_addr(addr) & m;
    while (g_table[j].used) {
        if (g_table[j].addr == addr) { g_table[j].fn = fn; return; }  /* replace */
        j = (j + 1) & m;
    }
    g_table[j].addr = addr;
    g_table[j].fn = fn;
    g_table[j].used = 1;
    g_count++;
}

/* Register an interior label -- a block in the middle of a function that some
 * computed jump can land on. Unlike psp_register this never overwrites: a real
 * function entry runs the whole function, a label thunk enters partway through,
 * and if an address is both then the entry is the correct answer. */
void psp_register_label(uint32_t addr, psp_fn_t fn) {
    if (!g_table || (g_count + 1) * 4 >= g_cap * 3) grow();
    if (!g_table) return;

    uint32_t m = g_cap - 1;
    uint32_t j = hash_addr(addr) & m;
    while (g_table[j].used) {
        if (g_table[j].addr == addr) return;      /* already known: keep it */
        j = (j + 1) & m;
    }
    g_table[j].addr = addr;
    g_table[j].fn = fn;
    g_table[j].used = 1;
    g_count++;
}

psp_fn_t psp_lookup(uint32_t addr) {
    if (!g_table) return NULL;
    uint32_t m = g_cap - 1;
    uint32_t j = hash_addr(addr) & m;
    while (g_table[j].used) {
        if (g_table[j].addr == addr) return g_table[j].fn;
        j = (j + 1) & m;
    }
    return NULL;
}

/* ---- function-entry trace ------------------------------------------------ */

/* Deep enough that a failure still has its cause in the ring.
 *
 * 32 was too short to be useful for the question it kept being asked: when a
 * game prints a diagnostic, the printf and its string handling alone fill the
 * ring, so the code that decided to print has already been pushed out. The
 * cost is one array; the whole point of the instrument is to reach past the
 * reporting and into the deciding. */
#define TRACE_DEPTH 512
static uint32_t g_trace[TRACE_DEPTH];
static uint64_t g_trace_n;

/* A one-shot hook on entry to a specific function. Bring-up repeatedly needs
 * to see the arguments to one function out of thousands, and rebuilding the
 * generated code with a printf in it is both slow and easy to leave behind. */
static uint32_t g_watch_addr;
static void (*g_watch_fn)(uint32_t);

void psp_trace_watch(uint32_t addr, void (*fn)(uint32_t)) {
    g_watch_addr = addr;
    g_watch_fn = fn;
}

void psp_trace_enter(uint32_t addr) {
    if (addr == g_watch_addr && g_watch_fn) g_watch_fn(addr);
    g_trace[g_trace_n % TRACE_DEPTH] = addr;
    g_trace_n++;
}

void psp_trace_reset(void) { g_trace_n = 0; }

/* The most recent traced function entry -- who was running when something else
 * went wrong. */
uint32_t psp_trace_last(void) {
    return g_trace_n ? g_trace[(g_trace_n - 1) % TRACE_DEPTH] : 0;
}

/* The entry `back` places before the most recent one: 1 is the function
 * entered just before psp_trace_last(). Entries, not a call stack -- a leaf
 * that returned is still there -- but for "who called the wrapper that called
 * this memcpy" that is exactly the order wanted. 0 for anything past what the
 * ring holds. */
uint32_t psp_trace_recent(int back) {
    if (back < 0 || (uint64_t)back >= g_trace_n || back >= TRACE_DEPTH) return 0;
    return g_trace[(g_trace_n - 1 - (uint64_t)back) % TRACE_DEPTH];
}

void psp_trace_dump(void) {
    if (!g_trace_n) {
        fprintf(stderr, "  (no function trace -- build the generated code with "
                        "PSPRECOMP_TRACE to enable it)\n");
        return;
    }
    uint64_t n = g_trace_n < TRACE_DEPTH ? g_trace_n : TRACE_DEPTH;
    fprintf(stderr, "  last %llu functions entered, newest first:\n",
            (unsigned long long)n);
    for (uint64_t i = 1; i <= n; i++) {
        uint64_t k = (g_trace_n - i) % TRACE_DEPTH;
        fprintf(stderr, "    psp_func_%08X\n", g_trace[k]);
    }
    fprintf(stderr, "  (%llu function entries total)\n",
            (unsigned long long)g_trace_n);
}

static void default_miss(uint32_t addr) {
    fprintf(stderr,
            "psprecomp: indirect call to 0x%08X, which is not a recompiled "
            "function.\n"
            "  Either discovery missed it, or it is data being called as code.\n",
            addr);
    psp_miss_context();
    abort();
}

void psp_set_miss_handler(psp_miss_fn_t fn) { g_miss = fn; }

static psp_dispatch_hook_t g_hook;
void psp_set_dispatch_hook(psp_dispatch_hook_t fn) { g_hook = fn; }

/* A budget on dispatched calls.
 *
 * A game's main loop does not return, and during bring-up it is just as likely
 * to be spinning on a condition nothing will ever satisfy. Both look identical
 * from outside: the process sits there. Killing it from the shell loses every
 * statistic that would say which one it is.
 *
 * With a budget the run ends on its own and the host prints its report, so a
 * hang becomes readable evidence instead of a stopped terminal. */
static uint64_t g_calls, g_budget;
static void (*g_over)(void);

void psp_dispatch_set_budget(uint64_t calls, void (*on_exceeded)(void)) {
    g_budget = calls;
    g_over = on_exceeded;
    g_calls = 0;
}

uint64_t psp_dispatch_calls(void) { return g_calls; }

void psp_dispatch(uint32_t addr) {
    if (g_budget && ++g_calls >= g_budget) {
        g_budget = 0;                 /* fire once */
        if (g_over) g_over();
    }
    /* Before the table: a hook may want to run this itself. */
    if (g_hook && g_hook(addr)) return;

    psp_fn_t fn = psp_lookup(addr);
    if (fn) {
        /* Log the first few *successful* indirect calls. Every session so far
         * has examined only the failures; a working virtual call sitting next
         * to a broken one says where vptrs come from. */
        extern int psp_log_indirect;
        static int logged;
        if (psp_log_indirect && logged < 12) {
            logged++;
            fprintf(stderr, "indirect ok: target=0x%08X from fn 0x%08X\n",
                    addr, psp_trace_last());
        }
        fn();
        return;
    }

    g_misses++;
    /* Aborting by default is deliberate. A silently-ignored indirect call
     * produces a program that runs and is wrong, which is far more expensive
     * to debug than one that stops and names the address. A host that wants to
     * survive misses during bring-up installs its own handler. */
    (g_miss ? g_miss : default_miss)(addr);
}

uint32_t psp_dispatch_count(void)  { return g_count; }
uint64_t psp_dispatch_misses(void) { return g_misses; }

void psp_dispatch_reset(void) {
    free(g_table);
    g_table = NULL;
    g_cap = g_count = 0;
    g_misses = 0;
    g_miss = NULL;
}

/* The last loop back-edge taken.
 *
 * Entry tracing cannot see a body that loops after its last call: no function
 * is entered, so the trace simply stops with the newest entry being some
 * innocent function that already returned. That blind spot cost three rounds of
 * disassembling the wrong code. Recording back-edges closes it -- a spinning
 * loop keeps writing here even though nothing else moves. */
static uint32_t g_loop_addr;
static uint64_t g_loop_hits;

void psp_trace_loop(uint32_t addr) { g_loop_addr = addr; g_loop_hits++; }
uint32_t psp_trace_loop_addr(void) { return g_loop_addr; }
uint64_t psp_trace_loop_hits(void) { return g_loop_hits; }

int psp_log_indirect;


/* Label-level reachability.
 *
 * psp_trace_watch hooks PSP_ENTER, which is emitted once per function body. A
 * watch on any address that is not a function entry therefore never fires, and
 * reports "not reached" for something it simply cannot observe -- a negative
 * indistinguishable from a real one. That flaw produced a confidently wrong
 * conclusion about an allocation failing.
 *
 * Marking every label closes it for any address the emitter gave a label to --
 * branch and jump targets, function entries, split entries, fall-through
 * targets.
 *
 * It does not close it for anything else, and the distinction is not visible
 * from in here. psp_trace_was_marked returns -1 only for an address outside the
 * marked range; an in-module address that never got a label returns 0, which
 * reads as "not reached" when it means "cannot be seen". Observability is a
 * property of the emitted C, and this layer holds no label table to check it
 * against, so the caller has to map an address to its covering label first --
 * see PSPRECOMP_REACHED in the boot host, which documents the grep. */
static uint32_t g_mark_addr;
static void (*g_mark_fn)(uint32_t);
static uint8_t *g_marked;
static uint32_t g_marked_lo, g_marked_n;

void psp_trace_mark(uint32_t addr) {
    if (g_marked && addr >= g_marked_lo && addr - g_marked_lo < g_marked_n * 4)
        g_marked[(addr - g_marked_lo) >> 2] = 1;
    if (addr == g_mark_addr && g_mark_fn) g_mark_fn(addr);
}

void psp_trace_watch_label(uint32_t addr, void (*fn)(uint32_t)) {
    g_mark_addr = addr;
    g_mark_fn = fn;
}

void psp_trace_marks_init(uint32_t lo, uint32_t words) {
    free(g_marked);
    g_marked = (uint8_t *)calloc(words ? words : 1, 1);
    g_marked_lo = lo;
    g_marked_n = words;
}

int psp_trace_was_marked(uint32_t addr) {
    if (!g_marked || addr < g_marked_lo || addr - g_marked_lo >= g_marked_n * 4) return -1;
    return g_marked[(addr - g_marked_lo) >> 2];
}

/* Stack-balance violations.
 *
 * A recompiled function must leave $sp as it found it. One that does not
 * corrupts every callee-saved register its caller restores afterwards: the
 * restores read `sp + offset`, so a shifted $sp reads a neighbouring slot and
 * loads a plausible wrong value instead of failing. That is invisible until
 * the wrong value is dereferenced, far from the cause.
 *
 * Checking the invariant at every return finds the culprit directly. */
static uint64_t g_sp_bad;
static uint64_t g_sp_leak;      /* the negative-delta subset */
static uint32_t g_sp_first;
static int32_t  g_sp_delta;

/* Per-site tallies.
 *
 * The raw total is a poor measure on its own: it counts *returns*, so one hot
 * function in a frame loop contributes millions and buries everything else.
 * Two runs whose structure differs substantially can still report nearly the
 * same total. What distinguishes them is which sites are involved and how many
 * there are, so the sites are aggregated rather than merely counted.
 *
 * A fixed open-addressed table, no growth and no allocation: this runs on the
 * return path of every recompiled function in a TRACE build, and a table that
 * reallocated there would change the thing it is measuring. Sites beyond
 * capacity are dropped and reported as such -- an undercount that says so is
 * worth more than a number that quietly stops being true. */
#define SP_SITES 4096
typedef struct { uint32_t fn; int32_t delta; uint64_t hits; } sp_site;
static sp_site  g_sp_site[SP_SITES];
static unsigned g_sp_nsites;
static uint64_t g_sp_dropped;

static void sp_record(uint32_t fn, int32_t delta) {
    unsigned h = (unsigned)((fn * 2654435761u) >> 13) & (SP_SITES - 1);
    for (unsigned n = 0; n < SP_SITES; n++) {
        sp_site *s = &g_sp_site[(h + n) & (SP_SITES - 1)];
        if (s->hits && s->fn != fn) continue;
        if (!s->hits) { s->fn = fn; s->delta = delta; g_sp_nsites++; }
        s->hits++;
        return;
    }
    g_sp_dropped++;
}

void psp_trace_sp(uint32_t fn, uint32_t sp_in, uint32_t sp_out) {
    if (sp_in == sp_out) return;
    const int32_t delta = (int32_t)(sp_out - sp_in);
    /* A *positive* delta usually means a split continuation that contains an
     * epilogue but not its matching prologue -- discovery makes those separate
     * bodies, so the invariant does not hold for them and they are noise. A
     * *negative* delta is the dangerous case: stack consumed and never
     * returned. Counted apart, because lumping them together makes a run full
     * of harmless splits look identical to one that is leaking. */
    if (delta < 0) g_sp_leak++;
    if (g_sp_bad < 24)
        fprintf(stderr, "sp UNBALANCED in 0x%08X: %+d%s\n", fn, (int)delta,
                delta < 0 ? "   <-- leak" : "");
    if (!g_sp_bad) { g_sp_first = fn; g_sp_delta = delta; }
    g_sp_bad++;
    sp_record(fn, delta);
}

uint64_t psp_sp_violations(void) { return g_sp_bad; }
uint64_t psp_sp_leaks(void)      { return g_sp_leak; }
unsigned psp_sp_sites(void)      { return g_sp_nsites; }
uint32_t psp_sp_first_bad(void)  { return g_sp_first; }
int32_t  psp_sp_first_delta(void){ return g_sp_delta; }

/* The busiest offenders, worst first. `top` is how many to print. */
void psp_sp_dump(FILE *out, int top) {
    if (!g_sp_nsites) return;
    fprintf(out, "  %u distinct site(s), %llu leak(s) of %llu unbalanced return(s)%s\n",
            g_sp_nsites, (unsigned long long)g_sp_leak,
            (unsigned long long)g_sp_bad,
            g_sp_dropped ? "  (site table full; some sites dropped)" : "");

    /* Selection sort over the top few. The table is small, this runs once at
     * exit, and sorting 4096 entries to show 12 is not worth the code. */
    for (int rank = 0; rank < top; rank++) {
        sp_site *best = NULL;
        for (unsigned i = 0; i < SP_SITES; i++) {
            sp_site *s = &g_sp_site[i];
            if (!s->hits) continue;
            if (!best || s->hits > best->hits) best = s;
        }
        if (!best) break;
        fprintf(out, "    %12llu  0x%08X  %+d%s\n",
                (unsigned long long)best->hits, best->fn, (int)best->delta,
                best->delta < 0 ? "   <-- leak" : "");
        best->hits = 0;   /* consumed */
    }
}

/* Stack imbalance observed across a call. Complements psp_trace_sp: that one
 * checks a body against its own entry and is blind to leaks that straddle a
 * body boundary; this one names the callee directly. */
static uint64_t g_spc_bad;
void psp_trace_sp_call(uint32_t callee, uint32_t sp_before, uint32_t sp_after) {
    if (sp_before == sp_after) return;
    if (g_spc_bad < 16)
        fprintf(stderr, "sp NOT RESTORED by callee 0x%08X: %+d\n",
                callee, (int)(sp_after - sp_before));
    g_spc_bad++;
}
uint64_t psp_sp_call_violations(void) { return g_spc_bad; }

/* ---- resuming a guest call chain ------------------------------------------ */

static const psp_resume_site *g_resume;
static int g_nresume;

void psp_resume_register(const psp_resume_site *sites, int count) {
    g_resume = sites;
    g_nresume = count;
}

int psp_resume_count(void) { return g_nresume; }

psp_resume_fn psp_resume_lookup(uint32_t site) {
    int lo = 0, hi = g_nresume - 1;
    while (lo <= hi) {
        const int mid = lo + (hi - lo) / 2;
        if (g_resume[mid].site == site) return g_resume[mid].fn;
        if (g_resume[mid].site < site) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

int psp_resume_chain(uint32_t site, uint32_t stop, uint32_t *missing) {
    while (site != stop) {
        const psp_resume_fn fn = psp_resume_lookup(site);
        if (!fn) { if (missing) *missing = site; return -1; }
        fn(site);
        site = psp_cpu.r[PSP_REG_RA];
    }
    return 0;
}
