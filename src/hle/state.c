/* Save states: the file, guest memory, the kept variables and the parts.
 * See psprecomp/state.h.
 *
 * The file is a header, then tagged chunks, then an end tag:
 *
 *   header   "PSPSTAT1", format version, the executable's hash, the counts
 *            of registered functions and resume sites, guest time, polls,
 *            when it was saved
 *   chunks   tag (8 bytes), size (8), bytes; the thumbnail first, so a slot
 *            list reads only the start of each file
 *
 * Guest memory goes as pages, the ones that are all zero skipped: a 4 KB
 * page map, then the pages it marks. That is the size measure the plan asked
 * for before any compression is considered. The kept variables go the same
 * way, end to end, after a list of their names, sizes and addresses. */
#include "psprecomp/state.h"
#include "census.h"

#include "psprecomp/clock.h"
#include "psprecomp/dispatch.h"
#include "psprecomp/hle.h"
#include "psprecomp/mem.h"
#include "psprecomp/os.h"
#include "psprecomp/render.h"
#include "psprecomp/sched.h"

#include <errno.h>
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif

enum { FORMAT = 2, PAGE = 4096, PARTS_MAX = 32, KEPT_MAX = 256, KEPT_NAME = 48 };
static const char MAGIC[8] = { 'P', 'S', 'P', 'S', 'T', 'A', 'T', '1' };

typedef struct {
    char magic[8];
    uint32_t format, header_size;
    uint64_t exe_hash;
    uint32_t functions, resume_sites;
    uint64_t guest_us;
    uint32_t polls, reserved;
    int64_t  saved;
} header;

struct psp_state_writer { FILE *f; int failed; };
struct psp_state_reader { uint8_t *data; size_t size; };

static const psp_state_part *g_parts[PARTS_MAX];
static int g_nparts;
static struct { const char *name; void *data; size_t size; } g_kept[KEPT_MAX];
static int g_nkept;
static int g_loaded;
static intptr_t g_delta;

intptr_t psp_state_delta(void) { return g_delta; }

void psp_state_keep(const char *name, void *data, size_t size) {
    for (int i = 0; i < g_nkept; i++) if (g_kept[i].data == data) return;
    if (g_nkept == KEPT_MAX) { fprintf(stderr, "state: too many kept variables; %s is not saved\n", name); return; }
    g_kept[g_nkept].name = name;
    g_kept[g_nkept].data = data;
    g_kept[g_nkept].size = size;
    g_nkept++;
}

int psp_state_is_kept(const void *p) {
    const char *c = (const char *)p;
    for (int i = 0; i < g_nkept; i++)
        if (c >= (const char *)g_kept[i].data && c < (const char *)g_kept[i].data + g_kept[i].size) return 1;
    return 0;
}

void psp_state_register(const psp_state_part *part) {
    for (int i = 0; i < g_nparts; i++) if (g_parts[i] == part) return;
    if (g_nparts < PARTS_MAX) g_parts[g_nparts++] = part;
    else fprintf(stderr, "state: too many parts; %s is not saved\n", part->name);
}

int psp_state_loaded(void) { return g_loaded; }

/* ---- identity ---------------------------------------------------------------- */

/* The build: FNV-1a over the executable's own bytes. A state holds this
 * build's layout -- its variables, its structures -- and nothing else can
 * read it. */
static uint64_t exe_hash(void) {
    static uint64_t cached;
    if (cached) return cached;
    uint64_t h = 1469598103934665603ull;
#ifdef _WIN32
    char self[4096];
    const DWORD len = GetModuleFileNameA(NULL, self, sizeof self);
    FILE *f = len && len < sizeof self ? fopen(self, "rb") : NULL;
#else
    FILE *f = fopen("/proc/self/exe", "rb");
#endif
    if (f) {
        static unsigned char buf[1 << 16];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0)
            for (size_t i = 0; i < n; i++) { h ^= buf[i]; h *= 1099511628211ull; }
        fclose(f);
    }
    cached = h ? h : 1;
    return cached;
}

/* ---- chunks ------------------------------------------------------------------- */

static void tag_of(char t[8], const char *tag) {
    memset(t, 0, 8);
    memcpy(t, tag, strnlen(tag, 8));
}

int psp_state_put(psp_state_writer *w, const char *tag, const void *data, size_t size) {
    char t[8];
    tag_of(t, tag);
    const uint64_t n = size;
    if (fwrite(t, 1, 8, w->f) != 8 || fwrite(&n, sizeof n, 1, w->f) != 1 ||
        (size && fwrite(data, 1, size, w->f) != size))
        w->failed = 1;
    return w->failed ? -1 : 0;
}

const void *psp_state_get(psp_state_reader *r, const char *tag, size_t *size) {
    char t[8];
    tag_of(t, tag);
    size_t at = sizeof(header);
    while (at + 16 <= r->size) {
        uint64_t n;
        memcpy(&n, r->data + at + 8, sizeof n);
        if (n > r->size - at - 16) return NULL;
        if (!memcmp(r->data + at, t, 8)) {
            if (size) *size = (size_t)n;
            return r->data + at + 16;
        }
        at += 16 + (size_t)n;
    }
    return NULL;
}

/* ---- guest memory ---------------------------------------------------------------- */

/* A region as a page map and the pages it marks. */
static int put_region(psp_state_writer *w, const char *tag, const uint8_t *bytes, uint32_t size) {
    const uint32_t pages = (size + PAGE - 1) / PAGE;
    const size_t map_bytes = (pages + 7) / 8;
    uint8_t *map = calloc(1, map_bytes ? map_bytes : 1);
    if (!map) return -1;
    size_t kept = 0;
    for (uint32_t p = 0; p < pages; p++) {
        const uint32_t len = p == pages - 1 && size % PAGE ? size % PAGE : PAGE;
        for (uint32_t i = 0; i < len; i++)
            if (bytes[(size_t)p * PAGE + i]) { map[p / 8] |= (uint8_t)(1u << (p % 8)); kept++; break; }
    }
    const size_t total = 8 + map_bytes + kept * PAGE;
    uint8_t *out = malloc(total);
    if (!out) { free(map); return -1; }
    memcpy(out, &size, 4);
    memcpy(out + 4, &pages, 4);
    memcpy(out + 8, map, map_bytes);
    uint8_t *at = out + 8 + map_bytes;
    for (uint32_t p = 0; p < pages; p++) {
        if (!(map[p / 8] & (1u << (p % 8)))) continue;
        const uint32_t len = p == pages - 1 && size % PAGE ? size % PAGE : PAGE;
        memset(at, 0, PAGE);
        memcpy(at, bytes + (size_t)p * PAGE, len);
        at += PAGE;
    }
    const int rc = psp_state_put(w, tag, out, total);
    free(out);
    free(map);
    return rc;
}

static int get_region(psp_state_reader *r, const char *tag, uint8_t *bytes, uint32_t size) {
    size_t n;
    const uint8_t *in = psp_state_get(r, tag, &n);
    if (!in || n < 8) return -1;
    uint32_t saved, pages;
    memcpy(&saved, in, 4);
    memcpy(&pages, in + 4, 4);
    if (saved != size) return -1;
    const size_t map_bytes = (pages + 7) / 8;
    if (n < 8 + map_bytes) return -1;
    const uint8_t *map = in + 8, *at = in + 8 + map_bytes;
    memset(bytes, 0, size);
    for (uint32_t p = 0; p < pages; p++) {
        if (!(map[p / 8] & (1u << (p % 8)))) continue;
        if ((size_t)(at + PAGE - in) > n) return -1;
        const uint32_t len = p == pages - 1 && size % PAGE ? size % PAGE : PAGE;
        memcpy(bytes + (size_t)p * PAGE, at, len);
        at += PAGE;
    }
    return 0;
}

/* ---- the kept variables --------------------------------------------------------- */

typedef struct { char name[KEPT_NAME]; uint64_t size, addr; } kept_entry;

static size_t kept_total(void) {
    size_t n = 0;
    for (int i = 0; i < g_nkept; i++) n += g_kept[i].size;
    return n;
}

static int put_kept(psp_state_writer *w) {
    kept_entry dir[KEPT_MAX];
    memset(dir, 0, sizeof dir);
    for (int i = 0; i < g_nkept; i++) {
        snprintf(dir[i].name, sizeof dir[i].name, "%s", g_kept[i].name);
        dir[i].size = g_kept[i].size;
        dir[i].addr = (uint64_t)(uintptr_t)g_kept[i].data;
    }
    const size_t total = kept_total();
    uint8_t *all = malloc(total ? total : 1);
    if (!all) return -1;
    size_t at = 0;
    for (int i = 0; i < g_nkept; i++) { memcpy(all + at, g_kept[i].data, g_kept[i].size); at += g_kept[i].size; }
    const int rc = psp_state_put(w, "keptdir", dir, (size_t)g_nkept * sizeof *dir) ||
                   put_region(w, "kept", all, (uint32_t)total);
    free(all);
    return rc ? -1 : 0;
}

/* Every variable back where this process keeps it. They must be the ones
 * that were saved, in the same order; and one distance must separate every
 * one from where it was, or no kept pointer could be moved by it. */
static int get_kept(psp_state_reader *r) {
    size_t n;
    const kept_entry *dir = psp_state_get(r, "keptdir", &n);
    if (!dir || n != (size_t)g_nkept * sizeof *dir) return -1;
    for (int i = 0; i < g_nkept; i++) {
        kept_entry e;
        memcpy(&e, &dir[i], sizeof e);
        if (strncmp(e.name, g_kept[i].name, sizeof e.name - 1) || e.size != g_kept[i].size) return -1;
        const intptr_t delta = (intptr_t)(uintptr_t)g_kept[i].data - (intptr_t)e.addr;
        if (i && delta != g_delta) return -1;
        g_delta = delta;
    }
    const size_t total = kept_total();
    uint8_t *all = malloc(total ? total : 1);
    if (!all) return -1;
    if (get_region(r, "kept", all, (uint32_t)total)) { free(all); return -1; }
    size_t at = 0;
    for (int i = 0; i < g_nkept; i++) { memcpy(g_kept[i].data, all + at, g_kept[i].size); at += g_kept[i].size; }
    free(all);
    return 0;
}

/* ---- saving --------------------------------------------------------------------------- */

const char *psp_state_refusal(void) {
    if (!psp_resume_count()) return "the game was compiled without resume entries";
    const char *why = psp_sched_state_refuse();
    if (why) return why;
    for (int i = 0; i < g_nparts; i++) {
        why = g_parts[i]->refuse ? g_parts[i]->refuse() : NULL;
        if (why) return why;
    }
    return NULL;
}

int psp_state_save(const char *path, char *why, size_t size) {
    const char *refused = psp_state_refusal();
    if (refused) { snprintf(why, size, "%s", refused); return -1; }
    char tmp[4096];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) {
        snprintf(why, size, "path too long");
        return -1;
    }
    psp_state_writer w = { fopen(tmp, "wb"), 0 };
    if (!w.f) { snprintf(why, size, "%s: %s", tmp, strerror(errno)); return -1; }
    header h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, MAGIC, sizeof MAGIC);
    h.format = FORMAT;
    h.header_size = sizeof h;
    h.exe_hash = exe_hash();
    h.functions = (uint32_t)psp_dispatch_count();
    h.resume_sites = (uint32_t)psp_resume_count();
    h.guest_us = psp_clock_peek();
    h.polls = psp_ctrl_polls();
    h.saved = (int64_t)time(NULL);
    if (fwrite(&h, sizeof h, 1, w.f) != 1) w.failed = 1;
    /* What the player last saw. A backend that draws away from guest memory
     * puts it there first, so the thumbnail and VRAM are both the frame. */
    const psp_render_backend *be = psp_render_current();
    if (be && be->to_memory) be->to_memory();
    static uint8_t thumb[PSP_STATE_THUMB_W * PSP_STATE_THUMB_H * 4];
    if (!psp_display_thumbnail(thumb, PSP_STATE_THUMB_W, PSP_STATE_THUMB_H))
        psp_state_put(&w, "thumb", thumb, sizeof thumb);

    if (put_region(&w, "ram", psp_mem.ram, PSP_RAM_SIZE)) w.failed = 1;
    if (put_region(&w, "vram", psp_mem.vram, PSP_VRAM_SIZE)) w.failed = 1;
    if (put_region(&w, "scratch", psp_mem.scratch, PSP_SCRATCH_SIZE)) w.failed = 1;
    uint32_t mbase = 0, msize = 0;
    psp_mem_module_region(&mbase, &msize);
    if (msize) {
        uint8_t *image = psp_mem_ptr(mbase, msize);
        uint32_t where[2] = { mbase, msize };
        psp_state_put(&w, "modbase", where, sizeof where);
        if (image && put_region(&w, "module", image, msize)) w.failed = 1;
    }
    psp_sched_state_prepare();
    if (put_kept(&w)) w.failed = 1;
    for (int i = 0; i < g_nparts && !w.failed; i++)
        if (g_parts[i]->save && g_parts[i]->save(&w)) w.failed = 1;
    psp_state_put(&w, "end", NULL, 0);
    if (fclose(w.f)) w.failed = 1;
    if (w.failed || rename(tmp, path)) {
        snprintf(why, size, "%s: could not be written", path);
        remove(tmp);
        return -1;
    }
    return 0;
}

/* ---- loading ----------------------------------------------------------------------------- */

/* The whole file, its header checked against this build. */
static int read_state(const char *path, psp_state_reader *r, char *why, size_t size) {
    r->data = NULL;
    r->size = 0;
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(why, size, "%s: %s", path, strerror(errno)); return -1; }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n > (long)sizeof(header)) r->data = malloc((size_t)n);
    if (r->data && fread(r->data, 1, (size_t)n, f) == (size_t)n) r->size = (size_t)n;
    fclose(f);
    header h;
    if (!r->size) { snprintf(why, size, "%s: unreadable", path); goto bad; }
    memcpy(&h, r->data, sizeof h);
    if (memcmp(h.magic, MAGIC, sizeof MAGIC) || h.format != FORMAT || h.header_size != sizeof h) {
        snprintf(why, size, "%s: not a state this player writes", path);
        goto bad;
    }
    if (h.exe_hash != exe_hash() || h.functions != (uint32_t)psp_dispatch_count() ||
        h.resume_sites != (uint32_t)psp_resume_count()) {
        snprintf(why, size, "%s: written by another build of the game", path);
        goto bad;
    }
    size_t dir;
    if (!psp_state_get(r, "keptdir", &dir) || dir != (size_t)g_nkept * sizeof(kept_entry)) {
        snprintf(why, size, "%s: the runtime's saved variables do not match", path);
        goto bad;
    }
    return 0;
bad:
    free(r->data);
    r->data = NULL;
    return -1;
}

/* Memory, the kept variables and the parts, into this process. */
static int apply_state(psp_state_reader *r, const char *path, char *why, size_t size) {
    if (get_region(r, "ram", psp_mem.ram, PSP_RAM_SIZE) ||
        get_region(r, "vram", psp_mem.vram, PSP_VRAM_SIZE) ||
        get_region(r, "scratch", psp_mem.scratch, PSP_SCRATCH_SIZE)) {
        snprintf(why, size, "%s: guest memory is damaged", path);
        return -1;
    }
    size_t where_size;
    const uint32_t *where = psp_state_get(r, "modbase", &where_size);
    if (where && where_size == 8) {
        uint32_t mbase = 0, msize = 0;
        psp_mem_module_region(&mbase, &msize);
        uint8_t *image = psp_mem_ptr(mbase, msize);
        if (mbase != where[0] || msize != where[1] || !image || get_region(r, "module", image, msize)) {
            snprintf(why, size, "%s: the module image does not match", path);
            return -1;
        }
    }
    if (get_kept(r)) {
        snprintf(why, size, "%s: the runtime's saved variables do not match", path);
        return -1;
    }
    for (int i = 0; i < g_nparts; i++)
        if (g_parts[i]->load && g_parts[i]->load(r, why, size)) return -1;
    /* Every write generation moves on, so nothing cached from memory before
     * the load is believed. */
    psp_mem_mark_write(PSP_RAM_BASE, PSP_RAM_SIZE);
    psp_mem_mark_write(PSP_VRAM_BASE, PSP_VRAM_SIZE);
    psp_mem_mark_write(PSP_SCRATCH_BASE, PSP_SCRATCH_SIZE);
    uint32_t mbase = 0, msize = 0;
    psp_mem_module_region(&mbase, &msize);
    if (msize) psp_mem_mark_write(mbase, msize);
    return 0;
}

int psp_state_load(const char *path, char *why, size_t size) {
    psp_state_reader r;
    if (read_state(path, &r, why, size)) return -1;
    /* Last: the threads, which start running once the main context drains. */
    const int rc = apply_state(&r, path, why, size) || psp_sched_state_load(why, size) ? -1 : 0;
    free(r.data);
    if (!rc) g_loaded = 1;
    return rc;
}

/* The process's threads and resident memory, for the checks that repeated
 * loads leave nothing behind: Linux's /proc, and nothing elsewhere. */
static void process_size(int *threads, long *rss_kb, long *heap_kb) {
    *threads = 0;
    *rss_kb = 0;
    *heap_kb = 0;
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
    *heap_kb = (long)(mallinfo2().uordblks / 1024);
#endif
#ifdef __linux__
    FILE *f = fopen("/proc/self/status", "r");
    char line[256];
    while (f && fgets(line, sizeof line, f)) {
        if (!strncmp(line, "Threads:", 8)) *threads = atoi(line + 8);
        else if (!strncmp(line, "VmRSS:", 6)) *rss_kb = atol(line + 6);
    }
    if (f) fclose(f);
#endif
}

/* The host request a load in progress answers, for its report. */
static unsigned g_loading;
static void finish(unsigned seq, int ok, const char *message);

int psp_state_load_here(const char *path, char *why, size_t size) {
    const char *refused = psp_sched_state_live_refuse();
    if (!refused && psp_utility_census() >= 1 && psp_utility_census() <= 3) refused = "the savedata dialog is open";
    if (refused) { snprintf(why, size, "%s", refused); return -1; }
    const uint64_t t0 = psp_os_mono_ns();
    psp_state_reader r;
    if (read_state(path, &r, why, size)) return -2;
    static unsigned loads;
    const uint32_t from = psp_ctrl_polls();
    /* From here the running game is given up. */
    psp_sched_state_retire();
    for (int i = 0; i < g_nparts; i++) if (g_parts[i]->drop) g_parts[i]->drop();
    psp_sched_state_lock(1);
    const int rc = apply_state(&r, path, why, size) || psp_sched_state_reload(why, size);
    psp_sched_state_lock(0);
    free(r.data);
    if (rc) {
        fprintf(stderr, "state: %s\n", why);
        psp_sched_stop_all("a state could not be loaded");
        return -1;
    }
    g_loaded = 1;
    int threads;
    long rss, heap;
    process_size(&threads, &rss, &heap);
    fprintf(stderr, "state: loaded %s at poll %u, now poll %u, guest time %.6f s, in %.0f ms (load %u: %d host threads, %ld kB resident, %ld kB allocated)\n",
            path, from, psp_ctrl_polls(), psp_clock_peek() / 1e6, (psp_os_mono_ns() - t0) / 1e6, ++loads, threads, rss, heap);
    if (g_loading) {
        char message[96];
        snprintf(message, sizeof message, "Loaded, at %.0f s of play.", psp_clock_peek() / 1e6);
        finish(g_loading, 1, message);
        g_loading = 0;
    }
    psp_safepoint_leave();
    psp_sched_state_jump();
    return -1;
}

const char *psp_state_dir(void) {
    const char *dir = getenv("PSPRECOMP_STATE_DIR");
    return dir && *dir ? dir : "states";
}

void psp_state_file(const char *name, char *out, size_t size) {
    snprintf(out, size, "%s/%s.state", psp_state_dir(), name);
}

int psp_state_peek(const char *path, psp_state_info *info, uint8_t *thumb) {
    memset(info, 0, sizeof *info);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    header h;
    int rc = -1;
    if (fread(&h, sizeof h, 1, f) == 1 && !memcmp(h.magic, MAGIC, sizeof MAGIC)) {
        rc = 0;
        info->usable = h.format == FORMAT && h.header_size == sizeof h && h.exe_hash == exe_hash() &&
                       h.functions == (uint32_t)psp_dispatch_count() &&
                       h.resume_sites == (uint32_t)psp_resume_count();
        if (h.format == FORMAT && h.header_size == sizeof h) {
            info->saved = h.saved;
            info->guest_us = h.guest_us;
            info->polls = h.polls;
            char tag[8];
            uint64_t n;
            const size_t want = (size_t)PSP_STATE_THUMB_W * PSP_STATE_THUMB_H * 4;
            if (fread(tag, 1, 8, f) == 8 && fread(&n, sizeof n, 1, f) == 1 &&
                !memcmp(tag, "thumb\0\0\0", 8) && n == want) {
                info->has_thumb = !thumb || fread(thumb, 1, want, f) == want;
            }
        }
    }
    fclose(f);
    return rc;
}

/* ---- scripted saves and loads, and the host's requests ---------------------------------------- */

/* PSPRECOMP_SAVE_STATE: each save waits for the first safe point at or after
 * its poll where nothing refuses it, and says when it was taken, and each
 * different reason it waited for, once. PSPRECOMP_LOAD_STATE: the same for
 * loads into the running game, as many times as asked. */
static struct { uint32_t poll; char path[512]; } g_script[8];
static int g_nscript, g_script_at;
static struct { uint32_t poll; unsigned times; char path[512]; } g_reload;
static char g_said[8][160];
static int g_nsaid;

void psp_state_init(void) {
    g_nscript = g_script_at = 0;
    const char *spec = getenv("PSPRECOMP_SAVE_STATE");
    while (spec && *spec && g_nscript < 8) {
        char *end;
        const unsigned long poll = strtoul(spec, &end, 10);
        if (end == spec || *end != ':') break;
        const char *file = end + 1;
        const size_t len = strcspn(file, ",");
        snprintf(g_script[g_nscript].path, sizeof g_script[g_nscript].path, "%.*s", (int)len, file);
        g_script[g_nscript].poll = (uint32_t)poll;
        g_nscript++;
        spec = file[len] ? file + len + 1 : file + len;
    }
    memset(&g_reload, 0, sizeof g_reload);
    spec = getenv("PSPRECOMP_LOAD_STATE");
    if (spec && *spec) {
        char *end;
        const unsigned long poll = strtoul(spec, &end, 10);
        if (end != spec && *end == ':') {
            const char *file = end + 1, *colon = strrchr(file, ':');
            const unsigned long times = colon ? strtoul(colon + 1, &end, 10) : 1;
            const int counted = colon && end != colon + 1 && !*end;
            snprintf(g_reload.path, sizeof g_reload.path, "%.*s",
                     (int)(counted ? (size_t)(colon - file) : strlen(file)), file);
            g_reload.poll = (uint32_t)poll;
            g_reload.times = counted ? (unsigned)times : 1;
        }
    }
}

/* Each different reason a scripted step waited, once. */
static void say_refused(const char *what, const char *refused) {
    for (int i = 0; i < g_nsaid; i++) if (!strncmp(g_said[i], refused, sizeof g_said[i] - 1)) return;
    fprintf(stderr, "state: not %s at poll %u yet: %s\n", what, psp_ctrl_polls(), refused);
    if (g_nsaid < 8) snprintf(g_said[g_nsaid++], sizeof g_said[0], "%.159s", refused);
}

/* The host's request, and the last result, shared with any thread. */
static psp_os_mutex g_req_lock = PSP_OS_MUTEX_INIT;
static struct { unsigned seq; int kind; char path[1024]; uint64_t since; } g_req;
static unsigned g_req_seq, g_done_seq;
static _Atomic int g_req_pending;
static int g_done_ok;
static char g_done_msg[384];

unsigned psp_state_request(int kind, const char *path) {
    psp_os_lock(&g_req_lock);
    g_req.seq = ++g_req_seq;
    g_req.kind = kind;
    g_req.since = 0;
    snprintf(g_req.path, sizeof g_req.path, "%s", path ? path : "");
    const unsigned seq = g_req.seq;
    g_req_pending = 1;
    psp_os_unlock(&g_req_lock);
    psp_safepoint_rearm();
    return seq;
}

unsigned psp_state_result(int *ok, char *message, size_t size) {
    psp_os_lock(&g_req_lock);
    const unsigned seq = g_done_seq;
    if (ok) *ok = g_done_ok;
    if (message && size) snprintf(message, size, "%s", g_done_msg);
    psp_os_unlock(&g_req_lock);
    return seq;
}

static void finish(unsigned seq, int ok, const char *message) {
    psp_os_lock(&g_req_lock);
    g_done_seq = seq;
    g_done_ok = ok;
    snprintf(g_done_msg, sizeof g_done_msg, "%s", message);
    if (g_req.seq == seq) g_req_pending = 0;
    psp_os_unlock(&g_req_lock);
    fprintf(stderr, "state: %s\n", message);
}

int psp_state_scripted_pending(void) {
    return g_script_at < g_nscript || g_reload.times || g_req_pending;
}

/* The host's request, at a safe point: a load only where the guest is not
 * held (`may_load`), since the load does not come back. */
static void serve_request(int may_load) {
    if (!g_req_pending) return;
    psp_os_lock(&g_req_lock);
    const unsigned seq = g_req.seq;
    const int kind = g_req.kind;
    char path[sizeof g_req.path];
    snprintf(path, sizeof path, "%s", g_req.path);
    if (!g_req.since) g_req.since = psp_clock_peek() + 1;
    const int expired = psp_clock_peek() + 1 - g_req.since > 1000000u;
    psp_os_unlock(&g_req_lock);
    if (kind == PSP_STATE_LOAD && !may_load) return;
    char why[300], message[384];
    if (kind == PSP_STATE_SAVE) {
        const char *refused = psp_state_refusal();
        if (refused) {
            if (!expired) return;
            snprintf(message, sizeof message, "Not saved: %s.", refused);
            finish(seq, 0, message);
            return;
        }
        const uint64_t t0 = psp_os_mono_ns();
        if (psp_state_save(path, why, sizeof why)) {
            snprintf(message, sizeof message, "Not saved: %s.", why);
            finish(seq, 0, message);
        } else {
            fprintf(stderr, "state: written in %.0f ms\n", (psp_os_mono_ns() - t0) / 1e6);
            snprintf(message, sizeof message, "Saved at %.0f s of play.", psp_clock_peek() / 1e6);
            finish(seq, 1, message);
        }
        return;
    }
    /* A load that cannot begin here is refused for good when the file is the
     * problem, and tried again for a moment when the game is. One that does
     * begin does not come back: it reports itself (g_loading). */
    g_loading = seq;
    const int rc = psp_state_load_here(path, why, sizeof why);
    g_loading = 0;
    const int retry = rc == -1 && !expired;
    psp_os_lock(&g_req_lock);
    if (g_req.seq == seq && retry) g_req_pending = 1;
    psp_os_unlock(&g_req_lock);
    if (!retry) {
        snprintf(message, sizeof message, "Not loaded: %s.", why);
        finish(seq, 0, message);
    }
}

void psp_state_scripted(void) {
    serve_request(1);
    char why[256];
    if (g_script_at < g_nscript && psp_ctrl_polls() >= g_script[g_script_at].poll) {
        const char *refused = psp_state_refusal();
        if (refused) { say_refused("saving", refused); return; }
        g_nsaid = 0;
        const uint64_t t0 = psp_os_mono_ns();
        if (psp_state_save(g_script[g_script_at].path, why, sizeof why))
            fprintf(stderr, "state: save at poll %u failed: %s\n", psp_ctrl_polls(), why);
        else
            fprintf(stderr, "state: saved at poll %u, guest time %.6f s -> %s (%.0f ms)\n", psp_ctrl_polls(),
                    psp_clock_peek() / 1e6, g_script[g_script_at].path, (psp_os_mono_ns() - t0) / 1e6);
        g_script_at++;
    }
    if (g_reload.times && psp_ctrl_polls() >= g_reload.poll) {
        g_reload.times--;
        const int rc = psp_state_load_here(g_reload.path, why, sizeof why);
        if (rc == -1) { say_refused("loading", why); g_reload.times++; }
        else if (rc == -2) { fprintf(stderr, "state: not loading: %s\n", why); g_reload.times = 0; }
    }
}

void psp_state_held(void) { serve_request(0); }
