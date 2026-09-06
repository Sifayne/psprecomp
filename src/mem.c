/* psprecomp — PSP memory map. See include/psprecomp/mem.h. */

#include "psprecomp/mem.h"
#include "psprecomp/dispatch.h"
#include "psprecomp/cpu.h"

#include <stdio.h>
#include <stdlib.h>

psp_memory psp_mem;
uint64_t   psp_mem_bad_access;

/* A relocatable module can live outside all three fixed memory windows. Keep
 * its backing and write-generation table beside the fixed regions so the
 * cache contract below covers every address psp_mem_ptr can return. */
static uint8_t *g_module;
static uint32_t g_module_base, g_module_size;
static uint64_t *g_module_write;

/* ---- write generations ---------------------------------------------------
 *
 * A GPU texture cache cannot use an address as the identity of its contents:
 * games overwrite VRAM in place, both with CPU stores and GE transfers. Keep
 * one timestamp per small guest-memory granule so a cache can ask whether the
 * bytes it decoded have changed since its upload. 256 bytes is fine enough not
 * to tie neighbouring PSP textures together, while the complete RAM + VRAM +
 * scratch tables cost a little over one MiB of zero-filled static storage.
 *
 * Guest execution is serialised by the scheduler's run token. The GE renderer
 * runs under that same token, so ordinary integers are sufficient here; making
 * every recompiled store atomic would buy no additional ordering and charge
 * the hottest path in the runtime for it. */
enum { WRITE_GRANULE_SHIFT = 8, WRITE_GRANULE = 1 << WRITE_GRANULE_SHIFT };
static uint64_t g_ram_write[(PSP_RAM_SIZE + WRITE_GRANULE - 1) / WRITE_GRANULE];
static uint64_t g_vram_write[(PSP_VRAM_SIZE + WRITE_GRANULE - 1) / WRITE_GRANULE];
static uint64_t g_scratch_write[(PSP_SCRATCH_SIZE + WRITE_GRANULE - 1) / WRITE_GRANULE];
static uint64_t g_write_serial;

static uint64_t *write_table(uint32_t addr, uint32_t size, uint32_t *off) {
    const uint32_t a = addr & PSP_ADDR_MASK;
    const uint64_t end = (uint64_t)a + size;
    if (g_module_size && g_module_write && a >= g_module_base &&
        end <= (uint64_t)g_module_base + g_module_size) {
        if (off) *off = a - g_module_base;
        return g_module_write;
    }
    if (a >= PSP_RAM_BASE && end <= (uint64_t)PSP_RAM_BASE + PSP_RAM_SIZE) {
        if (off) *off = a - PSP_RAM_BASE;
        return g_ram_write;
    }
    if (a >= PSP_VRAM_BASE && end <= (uint64_t)PSP_VRAM_BASE + PSP_VRAM_SIZE) {
        if (off) *off = a - PSP_VRAM_BASE;
        return g_vram_write;
    }
    if (a >= PSP_SCRATCH_BASE &&
        end <= (uint64_t)PSP_SCRATCH_BASE + PSP_SCRATCH_SIZE) {
        if (off) *off = a - PSP_SCRATCH_BASE;
        return g_scratch_write;
    }
    return NULL;
}

uint64_t psp_mem_write_serial(void) { return g_write_serial; }

uint64_t psp_mem_range_generation(uint32_t addr, uint32_t size) {
    if (!size) return 0;
    uint32_t off = 0;
    uint64_t *table = write_table(addr, size, &off);
    if (!table) return 0;
    const uint32_t first = off >> WRITE_GRANULE_SHIFT;
    const uint32_t last = (uint32_t)(((uint64_t)off + size - 1u) >>
                                     WRITE_GRANULE_SHIFT);
    uint64_t newest = 0;
    for (uint32_t i = first; i <= last; i++)
        if (table[i] > newest) newest = table[i];
    return newest;
}

static void (*g_write_observer)(uint32_t, uint32_t);
void psp_mem_set_write_observer(void (*observer)(uint32_t, uint32_t)) {
    g_write_observer = observer;
}

void psp_mem_mark_write(uint32_t addr, uint32_t size) {
    if (!size) return;
    uint32_t off = 0;
    uint64_t *table = write_table(addr, size, &off);
    if (!table) return;
    uint64_t stamp = ++g_write_serial;
    /* Zero is the pristine-memory generation. In practice wrapping a 64-bit
     * write count is unreachable; retaining the invariant still costs one
     * branch and keeps the API honest. */
    if (!stamp) stamp = ++g_write_serial;
    const uint32_t first = off >> WRITE_GRANULE_SHIFT;
    const uint32_t last = (uint32_t)(((uint64_t)off + size - 1u) >>
                                     WRITE_GRANULE_SHIFT);
    for (uint32_t i = first; i <= last; i++) table[i] = stamp;
    if (g_write_observer) g_write_observer(addr & PSP_ADDR_MASK, size);
}

/* Watch writes to one address. "Which code writes this word" is a question
 * that came up repeatedly and could only be answered by guessing; the write
 * path is the one place that can answer it directly. */
static uint32_t g_wwatch;
static int g_whits;
/* An optional value filter. A hot address is written far more often than it is
 * written *interestingly* -- a matrix element is cleared to zero every frame by
 * the identity load -- so an unfiltered watch spends its report budget on the
 * writes nobody asked about and never reaches the one that matters. */
static uint32_t g_wvalue;
static int g_wfilter;
void psp_mem_watch_write(uint32_t addr) { g_wwatch = addr; g_whits = 0; g_wfilter = 0; }
void psp_mem_watch_write_value(uint32_t addr, uint32_t val) {
    g_wwatch = addr; g_whits = 0; g_wvalue = val; g_wfilter = 1;
}
int psp_mem_watch_hits(void) { return g_whits; }

/* Armed by default. The report budget is 32 hits, and a word that is cleared
 * at mission start and rewritten every frame spends all 32 on the clears --
 * the write that matters, minutes later, is never seen. The sceCtrl HLE
 * disarms this at init when PSPRECOMP_WATCHMEM_FROM names a poll and re-arms
 * it when that poll arrives, so the budget starts where the question does. */
static int g_warmed = 1;
void psp_mem_watch_arm(int armed) { g_warmed = armed; }

/* The value as well as the writer.
 *
 * Naming the function that wrote a word is only half an answer, and reporting
 * the half without the other half is actively misleading: pointed at a matrix
 * element this caught sceGumLoadIdentity zeroing the slot and read as "here is
 * your culprit", when the write that mattered was a different one carrying a
 * different value. A watch that cannot say *what* was written cannot be
 * filtered, and an unfilterable watch on a hot address reports the wrong
 * write. */
static void note_write_val(uint32_t addr, uint32_t width, uint32_t val) {
    if (!g_wwatch || addr + width <= g_wwatch || addr > g_wwatch) return;
    if (!g_warmed) return;
    if (g_wfilter && val != g_wvalue) return;
    if (g_whits++ < 32) {
        union { uint32_t u; float f; } c; c.u = val;
        /* The registers too: for a memcpy-like writer the entry arguments
         * are long gone (reused as loop cursors), but survivors like a
         * saved dst/end-marker name the call. Which register matters is
         * specific to the writer; printing all of the plausibly-useful
         * ones beats a second run per hypothesis. */
        fprintf(stderr, "write%u to 0x%08X = 0x%08X (%.4g) from fn 0x%08X\n"
                        "        a0=%08X a1=%08X a2=%08X a3=%08X t0=%08X t1=%08X t2=%08X t3=%08X sp=%08X ra=%08X\n",
                width * 8, addr, val, (double)c.f, psp_trace_last(),
                psp_cpu.r[PSP_REG_A0], psp_cpu.r[PSP_REG_A1],
                psp_cpu.r[PSP_REG_A2], psp_cpu.r[PSP_REG_A3],
                psp_cpu.r[PSP_REG_T0], psp_cpu.r[PSP_REG_T1],
                psp_cpu.r[PSP_REG_T2], psp_cpu.r[PSP_REG_T3],
                psp_cpu.r[PSP_REG_SP], psp_cpu.r[PSP_REG_RA]);
        /* The functions entered before the writer, newest first. "from fn"
         * names the last *entry*, and for a memcpy called through a wrapper
         * that is the memcpy: the registers hold the wrapper's return address,
         * the wrapper's own is on the guest stack, and the function that
         * actually decided the value is two entries back. */
        fprintf(stderr, "        entered before it: 0x%08X <- 0x%08X <- 0x%08X <- 0x%08X\n",
                psp_trace_recent(1), psp_trace_recent(2),
                psp_trace_recent(3), psp_trace_recent(4));
    }
}

/* A census, because "few enough to simply print" stopped being true.
 *
 * Printing the first 32 addresses answered the question while a bad access
 * meant a botched initialiser. It does not survive a run that makes 1.2
 * billion of them: the 32 lines are all cascade from one wild pointer, the
 * count is the only other number, and neither says where it came from.
 *
 * Two tables, and the second is the one that matters. Addresses tell you the
 * shape of the walk -- a stride of 68 through a vertex array reads very
 * differently from one address hit repeatedly. `$ra` tells you the call site,
 * and *it works without a TRACE build*, because unlike psp_trace_last() it is
 * just a register. In the run this was written for, `ra` was the whole
 * diagnosis: one value, 0x0002E2F8, for all 1.2 billion. */
#define BAD_ADDRS 256
#define BAD_SITES 32

typedef struct { uint32_t key; uint64_t count; uint32_t ra, fn, first; int w, width; } bad_slot;
static bad_slot g_bad_addr[BAD_ADDRS];
static bad_slot g_bad_site[BAD_SITES];
static int      g_bad_addr_n, g_bad_site_n;
static uint64_t g_bad_addr_lost, g_bad_site_lost;

/* Populating the tables costs a hash probe per access, and at a billion
 * accesses that is the run's whole budget. Past this many the counter keeps
 * counting and the tables stop learning -- stated in the report, because a
 * distribution over a sample read as a census is exactly the kind of wrong
 * number this codebase keeps a list of. */
static uint64_t g_bad_sample = 1000000;

static void bad_note(bad_slot *tab, int cap, int *n, uint64_t *lost,
                     uint32_t key, uint32_t addr, int write, int width) {
    uint32_t h = (key * 2654435761u) % (uint32_t)cap;
    for (int i = 0; i < cap; i++) {
        bad_slot *s = &tab[(h + (uint32_t)i) % (uint32_t)cap];
        if (s->count && s->key != key) continue;
        if (!s->count) {
            s->key = key; s->first = addr; s->w = write; s->width = width;
            s->ra = psp_cpu.r[PSP_REG_RA]; s->fn = psp_trace_last();
            (*n)++;
        }
        s->count++;
        return;
    }
    (*lost)++;
}

/* Fire once at the Nth bad access, with the faulting frame still live.
 *
 * The policy -- stop the run, trap for a debugger, do nothing -- is the
 * host's, not this layer's. mem.c is a leaf: it includes mem.h, dispatch.h
 * and cpu.h, and test_runtime links it with no scheduler behind it. Calling
 * psp_sched_stop_all from here would drag the whole HLE in to serve one
 * diagnostic. */
static uint64_t g_bad_at;
static void (*g_bad_hook)(uint64_t nth, uint32_t addr, int write, int width);

void psp_mem_set_bad_hook(uint64_t nth,
                          void (*fn)(uint64_t, uint32_t, int, int)) {
    g_bad_at = nth; g_bad_hook = fn;
}
void psp_mem_set_bad_sample(uint64_t n) { g_bad_sample = n; }

/* Bad accesses were only ever counted, which says an initialiser went wrong
 * without saying which one. The addresses are what identify it, and there are
 * few enough of them (28 in the current run) to simply print. */
static void bad_access(uint32_t addr, int write, int width) {
    /* The first one matters most: everything after it may be cascade from a
     * register that was already wrong. Dump the whole file once. */
    if (psp_mem_bad_access == 0) {
        static const char *N[32] = {
            "zero","at","v0","v1","a0","a1","a2","a3","t0","t1","t2","t3",
            "t4","t5","t6","t7","s0","s1","s2","s3","s4","s5","s6","s7",
            "t8","t9","k0","k1","gp","sp","fp","ra" };
        fprintf(stderr, "\n--- first bad access: %s%d at 0x%08X (last fn 0x%08X) ---\n",
                write ? "write" : "read", width * 8, addr, psp_trace_last());
        for (int i = 0; i < 32; i += 4)
            fprintf(stderr, "  %-3s=0x%08X  %-3s=0x%08X  %-3s=0x%08X  %-3s=0x%08X\n",
                    N[i], psp_cpu.r[i], N[i+1], psp_cpu.r[i+1],
                    N[i+2], psp_cpu.r[i+2], N[i+3], psp_cpu.r[i+3]);
        /* The registers say what the access was made from; the trace says how
         * the code got there, which is the half that identifies the caller
         * that supplied the bad pointer. Empty unless built -DPSPRECOMP_TRACE. */
        psp_trace_dump();
    }
    /* Eight, not thirty-two. When these were all the report had, more was
     * better; beside the census they are only a sample of the walk's shape,
     * and thirty-two lines of the same cascade buries the register dump
     * above them. */
    if (psp_mem_bad_access < 8)
        fprintf(stderr, "psprecomp: bad %s%d at 0x%08X (last fn 0x%08X)\n",
                write ? "write" : "read", width * 8, addr, psp_trace_last());

    if (psp_mem_bad_access < g_bad_sample) {
        bad_note(g_bad_addr, BAD_ADDRS, &g_bad_addr_n, &g_bad_addr_lost,
                 addr, addr, write, width);
        bad_note(g_bad_site, BAD_SITES, &g_bad_site_n, &g_bad_site_lost,
                 psp_cpu.r[PSP_REG_RA], addr, write, width);
    }

    psp_mem_bad_access++;
    /* Fires at the first, so a host can capture state that the rest of the
     * run destroys -- the HLE's zero-return ring is sixteen deep and a
     * cascade this size scrolls it away long before the summary prints -- and
     * again at whichever one the host armed, to stop or trap there. */
    if (g_bad_hook && (psp_mem_bad_access == 1 || psp_mem_bad_access == g_bad_at))
        g_bad_hook(psp_mem_bad_access, addr, write, width);
}

static int bad_cmp(const void *a, const void *b) {
    const bad_slot *x = a, *y = b;
    return x->count < y->count ? 1 : x->count > y->count ? -1 : 0;
}

static void bad_table(FILE *out, const char *what, bad_slot *tab, int cap,
                      int n, uint64_t lost, int top, int by_site) {
    if (!n) return;
    bad_slot *s = malloc((size_t)cap * sizeof *s);
    if (!s) return;
    memcpy(s, tab, (size_t)cap * sizeof *s);
    qsort(s, (size_t)cap, sizeof *s, bad_cmp);

    fprintf(out, "  %d distinct %s%s:\n", n, what, n > top ? ", top by count" : "");
    for (int i = 0; i < cap && i < top; i++) {
        if (!s[i].count) break;
        if (by_site)
            fprintf(out, "    ra=0x%08X  x%-12llu  (first: %s%d at 0x%08X",
                    s[i].key, (unsigned long long)s[i].count,
                    s[i].w ? "write" : "read", s[i].width * 8, s[i].first);
        else
            fprintf(out, "    0x%08X  x%-12llu  (%s%d",
                    s[i].key, (unsigned long long)s[i].count,
                    s[i].w ? "write" : "read", s[i].width * 8);
        if (s[i].fn) fprintf(out, ", fn 0x%08X", s[i].fn);
        fprintf(out, ")\n");
    }
    if (lost) fprintf(out, "    ... and %llu more, past the table's %d slots\n",
                      (unsigned long long)lost, cap);
    free(s);
}

void psp_mem_dump_bad(FILE *out, int top) {
    if (!psp_mem_bad_access) return;
    if (top <= 0) top = 16;
    if (psp_mem_bad_access > g_bad_sample)
        fprintf(out, "  (tables cover the first %llu of %llu accesses -- a sample, "
                     "not a census; raise PSPRECOMP_BAD_SAMPLE to widen it)\n",
                (unsigned long long)g_bad_sample,
                (unsigned long long)psp_mem_bad_access);
    bad_table(out, "call site", g_bad_site, BAD_SITES, g_bad_site_n,
              g_bad_site_lost, top, 1);
    bad_table(out, "address", g_bad_addr, BAD_ADDRS, g_bad_addr_n,
              g_bad_addr_lost, top, 0);
}

int psp_mem_init(void) {
    g_write_observer = NULL;
    psp_mem.ram     = (uint8_t *)calloc(1, PSP_RAM_SIZE);
    psp_mem.vram    = (uint8_t *)calloc(1, PSP_VRAM_SIZE);
    psp_mem.scratch = (uint8_t *)calloc(1, PSP_SCRATCH_SIZE);
    if (!psp_mem.ram || !psp_mem.vram || !psp_mem.scratch) {
        psp_mem_free();
        return -1;
    }
    psp_mem_bad_access = 0;
    memset(g_bad_addr, 0, sizeof g_bad_addr);
    memset(g_bad_site, 0, sizeof g_bad_site);
    g_bad_addr_n = g_bad_site_n = 0;
    g_bad_addr_lost = g_bad_site_lost = 0;
    memset(g_ram_write, 0, sizeof g_ram_write);
    memset(g_vram_write, 0, sizeof g_vram_write);
    memset(g_scratch_write, 0, sizeof g_scratch_write);
    /* A host may reuse the runtime in one process. Force any external cache
     * that survived the old allocation to revalidate against the now-pristine
     * page tables rather than taking its global-serial fast path. */
    if (++g_write_serial == 0) g_write_serial++;
    return 0;
}

void psp_mem_free(void) {
    g_write_observer = NULL;
    free(psp_mem.ram);
    free(psp_mem.vram);
    free(psp_mem.scratch);
    free(g_module);
    free(g_module_write);
    psp_mem.ram = psp_mem.vram = psp_mem.scratch = NULL;
    g_module = NULL;
    g_module_write = NULL;
    g_module_base = g_module_size = 0;
}

int psp_mem_map_module(uint32_t base, uint32_t size) {
    free(g_module);
    free(g_module_write);
    g_module = (uint8_t *)calloc(1, size ? size : 1);
    const size_t pages = ((size_t)size + WRITE_GRANULE - 1) / WRITE_GRANULE;
    g_module_write = (uint64_t *)calloc(pages ? pages : 1, sizeof(uint64_t));
    if (!g_module || !g_module_write) {
        free(g_module);
        free(g_module_write);
        g_module = NULL;
        g_module_write = NULL;
        g_module_size = 0;
        return -1;
    }
    g_module_base = base;
    g_module_size = size;
    if (++g_write_serial == 0) g_write_serial++;
    return 0;
}

/* Where the module image is mapped, for anything that has to snapshot guest
 * memory: this region is checked before RAM and VRAM and is not part of
 * either, so a snapshot that omits it is missing whatever the module holds --
 * for this game, its display lists. */
void psp_mem_module_region(uint32_t *base, uint32_t *size) {
    if (base) *base = g_module_base;
    if (size) *size = g_module_size;
}

void *psp_mem_ptr(uint32_t addr, uint32_t size) {
    /* Collapse the three cache-behaviour mirrors onto one backing store. */
    const uint32_t a = addr & PSP_ADDR_MASK;

    /* Checked first: a module linked at 0 would otherwise fall through every
     * region test and read as unmapped. */
    if (g_module_size && a >= g_module_base && a < g_module_base + g_module_size) {
        uint32_t off = a - g_module_base;
        if (off + size > g_module_size) return NULL;
        return g_module + off;
    }

    if (a >= PSP_RAM_BASE && a < PSP_RAM_BASE + PSP_RAM_SIZE) {
        uint32_t off = a - PSP_RAM_BASE;
        if (off + size > PSP_RAM_SIZE) return NULL;   /* straddles the end */
        return psp_mem.ram + off;
    }
    if (a >= PSP_VRAM_BASE && a < PSP_VRAM_BASE + PSP_VRAM_SIZE) {
        uint32_t off = a - PSP_VRAM_BASE;
        if (off + size > PSP_VRAM_SIZE) return NULL;
        return psp_mem.vram + off;
    }
    if (a >= PSP_SCRATCH_BASE && a < PSP_SCRATCH_BASE + PSP_SCRATCH_SIZE) {
        uint32_t off = a - PSP_SCRATCH_BASE;
        if (off + size > PSP_SCRATCH_SIZE) return NULL;
        return psp_mem.scratch + off;
    }
    return NULL;
}

/* Reads of unmapped memory return 0 and are counted. Silently returning 0 is
 * what hardware roughly does, but a recompiled game should never be doing it
 * in a steady state — the counter is how you notice. */
#define READ_BODY(TYPE)                              \
    void *p = psp_mem_ptr(addr, (uint32_t)sizeof(TYPE)); \
    if (!p) { bad_access(addr, 0, (int)sizeof(TYPE)); return 0; }      \
    TYPE v;                                          \
    memcpy(&v, p, sizeof v);                         \
    return v;

uint8_t  psp_read8 (uint32_t addr) { READ_BODY(uint8_t)  }
uint16_t psp_read16(uint32_t addr) { READ_BODY(uint16_t) }
uint32_t psp_read32(uint32_t addr) { READ_BODY(uint32_t) }
float    psp_read_f32(uint32_t addr) { READ_BODY(float)  }

#define WRITE_BODY(TYPE)                             \
    void *p = psp_mem_ptr(addr, (uint32_t)sizeof(TYPE)); \
    if (!p) { bad_access(addr, 1, (int)sizeof(TYPE)); return; }         \
    { uint32_t _v = 0; memcpy(&_v, &val, sizeof(TYPE) > 4 ? 4 : sizeof(TYPE)); \
      note_write_val(addr, (uint32_t)sizeof(TYPE), _v); }                       \
    memcpy(p, &val, sizeof(TYPE));                   \
    psp_mem_mark_write(addr, (uint32_t)sizeof(TYPE));

void psp_write8 (uint32_t addr, uint8_t  val) { WRITE_BODY(uint8_t)  }
void psp_write16(uint32_t addr, uint16_t val) { WRITE_BODY(uint16_t) }
void psp_write32(uint32_t addr, uint32_t val) { WRITE_BODY(uint32_t) }
void psp_write_f32(uint32_t addr, float  val) { WRITE_BODY(float)    }

int psp_mem_write_block(uint32_t addr, const void *src, uint32_t len) {
    void *p = psp_mem_ptr(addr, len);
    if (!p) return -1;
    memcpy(p, src, len);
    psp_mem_mark_write(addr, len);
    return 0;
}

int psp_mem_read_block(void *dst, uint32_t addr, uint32_t len) {
    void *p = psp_mem_ptr(addr, len);
    if (!p) return -1;
    memcpy(dst, p, len);
    return 0;
}
