/* psprecomp — IoFileMgrForUser.
 *
 * The PSP's file API, mapped onto a host directory. Games address the UMD as
 * `disc0:/` and the Memory Stick as `ms0:/`, so those prefixes are rewritten to
 * subdirectories of a root the host chooses.
 *
 * Reads go straight into guest memory, which means a game loading assets is
 * doing the real thing -- and a texture or model that arrives byte-correct is
 * strong evidence the recompiled code around it is behaving too.
 */

#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE                 /* statx, for a file's birth time */
#endif

#include "psprecomp/hle.h"
#include "psprecomp/sched.h"
#include "io_observed.h"
#include "psprecomp/state.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <strings.h>
#else
#define strcasecmp _stricmp
#endif

#ifdef _WIN32
#  include <io.h>
#  include <direct.h>
#else
#  include <dirent.h>
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <time.h>
#  include <unistd.h>
#endif

#define MAX_FILES 64
#define MAX_DIRS  16

/* Open flags, as the guest passes them. */
#define PSP_O_RDONLY 0x0001
#define PSP_O_WRONLY 0x0002
#define PSP_O_RDWR   0x0003
#define PSP_O_APPEND 0x0100
#define PSP_O_CREAT  0x0200
#define PSP_O_TRUNC  0x0400

/* An open file. `base`/`len` describe a window inside `f`, which is what makes
 * a file inside a disc image indistinguishable from a host file to everything
 * downstream: the ISO is one host file and every asset is a range within it.
 * A plain host file is the whole of `f`, so len is its size and base is 0. */
typedef struct {
    FILE    *f;
    int      used;
    uint64_t base, len, pos;
    /* The async result of the last operation on this descriptor, waiting to be
     * collected by poll or wait. See the async block near the bottom. */
    int      has_result;
    int      close_pending;
    int      running;       /* the first poll after queueing reports it running */
    int64_t  result;
    /* The raw UMD block device counts in 2048-byte sectors, not bytes: both the
     * offset given to sceIoLseek and the length given to sceIoRead are LBAs.
     * A game streaming from the disc opens the device this way, stats each file
     * to get its start sector, and seeks straight there. Treating those numbers
     * as bytes reads 64 bytes from offset 58144 where the game asked for 64
     * sectors from sector 58144 -- a plausible-looking read of entirely the
     * wrong thing.
     *
     * So the mode is a property of the *handle*, set at open by which device
     * name was used, and not something a read can work out from its arguments.
     */
    int      sector_mode;
    /* Written since it was opened, so closing it flushes (see io_park). */
    int      dirty;
    /* How to open it again in another process, for a save state: the disc
     * image, or this host path read-only. A writable file is not saved. */
    int      image, writable;
    char     host[1024];
} io_file;

/* One entry of a directory listing, as sceIoDopen took it. */
typedef struct {
    char name[256];
    uint64_t born;       /* creation order key: host birth time, in ns */
    int is_dir;          /* from the listing itself on Windows */
    uint64_t size;
} io_dirent;

typedef struct {
    int used;
    io_dirent *ent;      /* the whole listing, read and ordered at Dopen */
    int n, pos;
    char host[1024];     /* the directory's host path, to stat its entries */
    int fat;             /* on the Memory Stick: entries stat the FAT way */
    /* A listing served from the disc image: the directory extent's records,
     * walked in stored order. NULL, with iso clear, for a host directory. */
    uint8_t *records;
    uint32_t bytes, off;
    int iso;
} io_dir;

static io_file g_file[MAX_FILES];
static io_dir  g_dir[MAX_DIRS];
static char    g_root[512];
static char    g_cwd[512];
static char    g_umd_image[512];
static uint64_t g_bytes_read;

void psp_io_set_root(const char *root) {
    snprintf(g_root, sizeof g_root, "%s", root ? root : ".");
}

void psp_io_set_umd_image(const char *path) {
    snprintf(g_umd_image, sizeof g_umd_image, "%s", path ? path : "");
}

/* Is this the raw block device rather than a path on the filesystem?
 *
 * `umd0:` and `umd1:` name the device itself. Anything after the colon makes
 * it a path again (the disc probe opened umd0:/ALPHA.BIN and umd1:/ALPHA.BIN
 * as files), and `disc0:` is always the filesystem view, so neither is
 * treated as raw. Getting that distinction wrong would turn every asset load
 * into an attempt to open the whole disc. Both names exist for a program
 * booted from its disc, which is what a game is; the same probe loaded from
 * the host with the image mounted saw no umd0: at all, so that is not the
 * context this layer models. */
static int is_raw_umd(const char *guest) {
    const char *p;
    if      (!strncmp(guest, "umd0:", 5)) p = guest + 5;
    else if (!strncmp(guest, "umd1:", 5)) p = guest + 5;
    else return 0;
    while (*p == '/' || *p == '\\') p++;
    return *p == '\0';
}

void psp_io_reset(void) {
    for (int i = 0; i < MAX_FILES; i++) {
        if (g_file[i].used && g_file[i].f) fclose(g_file[i].f);
        g_file[i].used = 0;
        g_file[i].f = NULL;
        g_file[i].base = g_file[i].len = g_file[i].pos = 0;
        g_file[i].has_result = g_file[i].close_pending = g_file[i].running = 0;
        g_file[i].result = 0;
        g_file[i].sector_mode = 0;
        g_file[i].dirty = 0;
    }
    for (int i = 0; i < MAX_DIRS; i++) { free(g_dir[i].ent); free(g_dir[i].records); }
    memset(g_dir, 0, sizeof g_dir);
    g_cwd[0] = '\0';
    g_bytes_read = 0;
    if (!g_root[0]) psp_io_set_root(".");
}

void psp_io_init(void) { g_root[0] = '\0'; psp_io_reset(); }

uint64_t psp_io_bytes_read(void) { return g_bytes_read; }

/* Rewrite a PSP path into a host path. The device prefix becomes a
 * subdirectory so a disc image and a memory-stick image can coexist under one
 * root without colliding. A path without a device goes through the current
 * directory, which starts as the root. */
static void map_path(const char *guest, char *out, size_t cap) {
    const char *p = guest;
    const char *sub = "disc";
    int have_dev = 1;

    if      (!strncmp(p, "disc0:", 6)) { p += 6; sub = "disc"; }
    else if (!strncmp(p, "umd0:",  5)) { p += 5; sub = "disc"; }
    else if (!strncmp(p, "ms0:",   4)) { p += 4; sub = "ms";   }
    else if (!strncmp(p, "flash0:",7)) { p += 7; sub = "flash";}
    else if (!strncmp(p, "host0:", 6)) { p += 6; sub = "host"; }
    /* Seen from real titles and previously unmapped, which left the colon in
     * the host path and made every such open fail in a way that looked like a
     * missing file rather than a missing prefix. With a path after the colon
     * both umd names are the same filesystem view as disc0: (disc probe,
     * opens 7 and 14). */
    else if (!strncmp(p, "umd1:",  5)) { p += 5; sub = "disc"; }
    else if (!strncmp(p, "msstor0p1:", 10)) { p += 10; sub = "ms"; }
    else if (!strncmp(p, "msstor0:",   8)) { p += 8;  sub = "ms"; }
    else if (strchr(p, ':') == NULL) have_dev = 0;
    else { p = guest; sub = "disc"; }

    while (*p == '/' || *p == '\\') p++;
    if (have_dev) snprintf(out, cap, "%s/%s/%s", g_root, sub, p);
    else if (g_cwd[0]) snprintf(out, cap, "%s/%s", g_cwd, p);
    else snprintf(out, cap, "%s/%s", g_root, p);
}

/* A Memory Stick path: FAT underneath, which shows in what open and stat
 * report (see write_fat_stat). */
static int is_ms_path(const char *guest) {
    return !strncmp(guest, "ms0:", 4) || !strncmp(guest, "msstor0:", 8) ||
           !strncmp(guest, "msstor0p1:", 10);
}

/* One ISO 9660 sector. Up here rather than with the reader below because
 * sceIoGetstat reports a file's start sector and needs it too. */
#define ISO_SECTOR 2048u

/* Defined with the directories below; declared here because CREAT opens
 * make their parents. */
static void mkdir_parents(const char *host);

/* Defined with the rest of the ISO 9660 reader below; declared here because
 * opening and stat-ing both consult the disc image before the host tree. */
typedef struct {
    FILE    *f;             /* the image, left open for the caller */
    uint64_t base, len;     /* the byte window of what the path names */
    int      is_dir, is_root, is_device;
} iso_entry;
static int iso_lookup(const char *guest, iso_entry *e);

static void io_park(void);
static void hle_open_body(void);
/* See io_park: an open gives up the CPU whether or not it finds the file. */
static void hle_Open(void) { hle_open_body(); io_park(); }

static void hle_open_body(void) {
    char guest[512], host[1024];
    psp_str(psp_arg(0), guest, sizeof guest);
    uint32_t flags = psp_arg(1);

    /* A disc path is served from the image if there is one. The raw device
     * falls out of this as the case with nothing after the colon. A directory
     * opens too: the disc probe opened disc0:/SUB as a file and read its raw
     * directory records back, with the extent's size at SEEK_END. */
    if (!(flags & (PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC))) {
        iso_entry e;
        if (iso_lookup(guest, &e) == 0) {
            for (int i = 0; i < MAX_FILES; i++) {
                if (g_file[i].used) continue;
                g_file[i].f = e.f; g_file[i].used = 1; g_file[i].dirty = 0;
                g_file[i].image = 1; g_file[i].writable = 0; g_file[i].host[0] = '\0';
                g_file[i].base = e.base; g_file[i].len = e.len; g_file[i].pos = 0;
                g_file[i].has_result = g_file[i].close_pending = g_file[i].running = 0;
                g_file[i].sector_mode = e.is_device;
                psp_ret((uint32_t)(i + 3));
                return;
            }
            fclose(e.f);
            psp_ret(0x80010018);
            return;
        }
    }

    if (is_raw_umd(guest)) {
        if (!g_umd_image[0]) {
            static int complained;
            if (!complained++)
                fprintf(stderr, "psprecomp: %s opened as a raw device, but no disc "
                                "image is set (psp_io_set_umd_image)\n", guest);
            psp_ret(IO_ERROR_NOENT);
            return;
        }
        FILE *f = fopen(g_umd_image, "rb");
        if (!f) {
            fprintf(stderr, "psprecomp: cannot open disc image %s\n", g_umd_image);
            psp_ret(IO_ERROR_NOENT);
            return;
        }
        for (int i = 0; i < MAX_FILES; i++) {
            if (g_file[i].used) continue;
            g_file[i].f = f;
            g_file[i].used = 1;
            g_file[i].dirty = 0;
            g_file[i].image = 1; g_file[i].writable = 0; g_file[i].host[0] = '\0';
            g_file[i].base = 0;
            g_file[i].pos  = 0;
            g_file[i].len  = (fseeko(f, 0, SEEK_END) == 0 && ftello(f) > 0)
                             ? (uint64_t)ftello(f) : 0;
            g_file[i].has_result = g_file[i].close_pending = g_file[i].running = 0;
            g_file[i].sector_mode = 1;
            psp_ret((uint32_t)(i + 3));
            return;
        }
        fclose(f);
        psp_ret(0x80010018);
        return;
    }

    map_path(guest, host, sizeof host);

    /* FAT-illegal names fail at open with INVALID_ARG (the savedata suite's
     * A?C probe): ? * < > | " and controls never name a real file. Only the
     * host branch -- disc names are fixed. Checked on the guest subpath so
     * no host root can smuggle a character in. */
    {
        const char *colon = strchr(guest, ':');
        const char *sub = colon ? colon + 1 : guest;
        for (const char *c = sub; *c; c++) {
            unsigned char u = (unsigned char)*c;
            if (u < 32 || u == '?' || u == '*' || u == '<' || u == '>' ||
                u == '|' || u == '"') {
                psp_ret(0x80010016);
                return;
            }
        }
    }

#ifndef _WIN32
    /* A Memory Stick directory does not open as a file: EACCES, or EINVAL
     * when the path ends in a slash (saveprobe steps 103-104, fw 6.60).
     * Host fopen opens a directory for reading on Linux, and the read that
     * followed returned 0 bytes. */
    if (is_ms_path(guest)) {
        struct stat st;
        if (stat(host, &st) == 0 && S_ISDIR(st.st_mode)) {
            const size_t n = strlen(guest);
            psp_ret(n && (guest[n - 1] == '/' || guest[n - 1] == '\\') ? 0x80010016u : 0x8001000Du);
            return;
        }
    }
#endif

    /* Creating a file creates its parents: a save flow makes
     * ms0:/PSP/SAVEDATA/<id> under a tree that starts empty, and failing on
     * the absent middle is the same empty-tree deviation mkdir_parents
     * documents. */
    if (flags & PSP_O_CREAT) {
        char dir[1024];
        snprintf(dir, sizeof dir, "%s", host);
        char *slash = strrchr(dir, '/');
        if (slash) { *slash = '\0'; mkdir_parents(dir); }
    }

    const char *mode = "rb";
    if (flags & PSP_O_TRUNC)                  mode = (flags & PSP_O_RDWR) == PSP_O_RDWR ? "w+b" : "wb";
    else if (flags & PSP_O_APPEND)            mode = "ab";
    else if ((flags & PSP_O_RDWR) == PSP_O_RDWR) mode = "r+b";
    else if (flags & PSP_O_WRONLY)            mode = (flags & PSP_O_CREAT) ? "wb" : "r+b";

    FILE *f = fopen(host, mode);
    if (!f && (flags & PSP_O_CREAT)) f = fopen(host, "w+b");
    if (!f) {
        /* A failed open is often normal -- a game probing for a save file --
         * so this is rate-limited rather than loud. But a game that cannot find
         * its assets retries forever, and then the *first* few failures are the
         * whole story: they name the file it wanted and the host path that was
         * searched, which is usually a root that was never configured. */
        static int complained;
        if (complained < 8) {
            complained++;
            fprintf(stderr, "psprecomp: sceIoOpen failed: \"%s\" (flags 0x%x) -> \"%s\"\n", guest, flags, host);
        }
        psp_ret(IO_ERROR_NOENT);
        return;
    }

    for (int i = 0; i < MAX_FILES; i++) {
        if (g_file[i].used) continue;
        g_file[i].f = f;
        g_file[i].used = 1;
        g_file[i].dirty = 0;
        g_file[i].image = 0;
        g_file[i].writable = strcmp(mode, "rb") != 0;
        snprintf(g_file[i].host, sizeof g_file[i].host, "%s", host);
        g_file[i].base = 0;
        g_file[i].pos  = 0;
        g_file[i].has_result = g_file[i].close_pending = g_file[i].running = 0;
        g_file[i].sector_mode = 0;
        /* A host file's window is the whole file. Writable files start empty
         * and grow, so the length is refreshed on write rather than fixed. */
        g_file[i].len = (fseeko(f, 0, SEEK_END) == 0 && ftello(f) > 0)
                        ? (uint64_t)ftello(f) : 0;
        rewind(f);
        psp_ret((uint32_t)(i + 3));   /* 0-2 are reserved for the std streams */
        return;
    }
    fclose(f);
    psp_ret(0x80010018);              /* too many open files */
}

/* A file system call gives up the CPU while its driver works.
 *
 * threadprobe step 86 (fw 6.60) makes Memory Stick calls from main (0x20)
 * with a ready 0x20 thread W, and W runs inside open (for writing and for
 * reading), write, read (32K and 16 bytes), getstat and remove, and inside a
 * close that follows a write; main's releaseCount rises across each (51, 2,
 * 50, 1, 1, 50 and 53, and 4 for that close). An lseek and a close with
 * nothing written give nothing up (release+0, W runs after). Step 1's
 * release=333 for main is the same thing during start-up I/O. psprecomp's
 * calls never let go of the CPU, so a thread doing I/O kept it from every
 * thread of its own priority and below until it next blocked.
 *
 * Modelled as the shortest delay: the caller stops being runnable, every
 * ready thread gets its turn -- equal and lower priorities too, as they do
 * while a real driver waits -- and the caller is ready again as soon as any
 * firmware call notices a microsecond has passed. The hardware's counts are
 * the driver's own waits and depend on the stick's directory layout; this
 * counts one release per call. The disc (disc0:, umd0:) is not measured: it
 * is parked the same way, on the grounds that a UMD read waits for a slower
 * drive than a stick read does. Directory calls, mkdir, rmdir, rename and
 * chstat are unmeasured and left as they were.
 *
 * $v0/$v1 are the call's answer and are kept across the park. Nothing is
 * given up with dispatch or interrupts off, where a PSP could not wait, nor
 * by the async calls, which reuse these handlers but return at once, nor
 * inside an alarm or vtimer handler, which runs on no thread and has nothing
 * to park. */
static int g_io_async;

static void io_park(void) {
    if (g_io_async || !psp_sched_can_wait() || psp_ktimer_in_handler()) return;
    const uint32_t v0 = psp_cpu.r[PSP_REG_V0], v1 = psp_cpu.r[PSP_REG_V1];
    (void)psp_sched_delay(1);
    psp_cpu.r[PSP_REG_V0] = v0;
    psp_cpu.r[PSP_REG_V1] = v1;
}

static io_file *fd_arg(void) {
    int32_t fd = (int32_t)psp_arg(0) - 3;
    if (fd < 0 || fd >= MAX_FILES || !g_file[fd].used) return NULL;
    return &g_file[fd];
}

static void hle_Close(void) {
    io_file *h = fd_arg();
    if (!h) { psp_ret(IO_ERROR_BADF); return; }
    const int flush = h->dirty;
    if (h->f) fclose(h->f);
    h->f = NULL;
    h->used = 0;
    h->dirty = 0;
    psp_ret(0);
    if (flush) io_park();
}

static void hle_read_body(io_file *h, uint32_t dst, uint32_t size);

static void hle_Read(void) {
    io_file *h = fd_arg();
    uint32_t dst = psp_arg(1), size = psp_arg(2);
    if (!h || !h->f) { psp_ret(IO_ERROR_BADF); return; }
    if (!size) { psp_ret(0); return; }
    const uint32_t asked = size;
    if (h->sector_mode) size *= ISO_SECTOR;   /* the count is in sectors */
    hle_read_body(h, dst, size);
    io_park();
}

static void hle_read_body(io_file *h, uint32_t dst, uint32_t size) {
    /* What the caller asked for, in its own unit: sectors on the raw device. */
    const uint32_t asked = h->sector_mode ? size / ISO_SECTOR : size;

    /* Read through a host buffer and then place it, so a read that straddles
     * the end of a guest region is rejected by the memory layer rather than
     * writing past it. */
    /* Clamp to the file's window: an asset inside a disc image is followed by
     * the next asset, not by end-of-file, so reading past it would quietly
     * return a neighbour's bytes instead of a short read. */
    /* The raw device past its last sector answers the count it was asked for
     * and writes nothing (disc probe: four sectors asked at sector 100 of a
     * 40-sector image returned 4 and left the buffer untouched). A file at
     * its end reads nothing. */
    if (h->pos >= h->len) {
        if (h->sector_mode) { h->pos += size; psp_ret(asked); return; }
        psp_ret(0);
        return;
    }
    if (size > h->len - h->pos) size = (uint32_t)(h->len - h->pos);

    uint8_t *tmp = (uint8_t *)malloc(size);
    if (!tmp) { psp_ret(0x80020190); return; }

    size_t got = 0;
    if (fseeko(h->f, (off_t)(h->base + h->pos), SEEK_SET) == 0)
        got = fread(tmp, 1, size, h->f);
    h->pos += got;
    if (got && psp_mem_write_block(dst, tmp, (uint32_t)got) != 0) {
        /* Fall back to byte-at-a-time so a partially mapped destination still
         * gets what fits, and the bad-access counter records the rest. */
        for (size_t i = 0; i < got; i++) psp_write8(dst + (uint32_t)i, tmp[i]);
    }
    free(tmp);
    g_bytes_read += got;
    /* Reported in whatever unit the caller asked in. */
    psp_ret((uint32_t)(h->sector_mode ? got / ISO_SECTOR : got));
}

static void hle_Write(void) {
    io_file *h = fd_arg();
    uint32_t src = psp_arg(1), size = psp_arg(2);

    /* fd 1 and 2 are stdout/stderr: a game writing there is talking to us. */
    int32_t fd = (int32_t)psp_arg(0);
    if (fd == 1 || fd == 2) {
        for (uint32_t i = 0; i < size; i++) fputc(psp_read8(src + i), stderr);
        psp_ret(size);
        return;
    }
    if (!h || !h->f) {
        /* A write to a descriptor nothing opened. Returning the error silently
         * is right for a shipped port and wrong during bring-up: a game's panic
         * path is usually "get the stdout fd, write the message, abort", so the
         * one write most worth seeing is exactly the one most likely to land on
         * a descriptor this layer does not know about. Dropping it costs the
         * message that would have named the failure.
         *
         * Rate-limited, because a game that logs in a loop should not drown
         * the run it is trying to explain. */
        static int complained;
        if (complained < 8) {
            complained++;
            fprintf(stderr, "psprecomp: sceIoWrite to unopened fd %d, %u bytes: \"", fd, size);
            for (uint32_t i = 0; i < size && i < 200; i++) {
                const int c = psp_read8(src + i);
                fputc((c >= 32 && c < 127) ? c : '.', stderr);
            }
            fprintf(stderr, "%s\"\n", size > 200 ? "..." : "");
        }
        psp_ret(IO_ERROR_BADF);
        return;
    }

    uint8_t *tmp = (uint8_t *)malloc(size ? size : 1);
    if (!tmp) { psp_ret(0x80020190); return; }
    for (uint32_t i = 0; i < size; i++) tmp[i] = psp_read8(src + i);
    size_t put = fwrite(tmp, 1, size, h->f);
    free(tmp);
    if (size) h->dirty = 1;
    psp_ret((uint32_t)put);
    if (size) io_park();
}

/* sceIoLseek takes a 64-bit offset and returns one. Under o32 a 64-bit
 * argument is register-aligned, so it lands in $a2:$a3 rather than $a1:$a2 --
 * and the result comes back in $v0:$v1. Getting either wrong makes every seek
 * land somewhere plausible but wrong. */
static void hle_Lseek(void) {
    io_file *h = fd_arg();
    uint64_t off = (uint64_t)psp_arg(2) | ((uint64_t)psp_arg(3) << 32);
    uint32_t whence = psp_arg(4);
    if (!h || !h->f) { psp_ret(IO_ERROR_BADF); return; }

    /* Seeks are tracked against the file's own window rather than the host
     * file, so a file inside a disc image seeks within itself. It also makes
     * SEEK_END mean the end of the *asset*, which is what a game seeking to
     * find a length expects, and the 64-bit offset is no longer truncated to
     * long on the way through. */
    int64_t rel = (int64_t)off;
    if (h->sector_mode) rel *= ISO_SECTOR;
    int64_t want;
    if      (whence == 1) want = (int64_t)h->pos + rel;   /* SEEK_CUR */
    else if (whence == 2) want = (int64_t)h->len + rel;   /* SEEK_END */
    else                  want = rel;                     /* SEEK_SET */

    if (want < 0) { psp_ret(0xFFFFFFFFu); return; }
    /* The raw device seeks past its end (disc probe: sector 100 of a
     * 40-sector image came back as 100); a file's window is clamped. */
    if (!h->sector_mode && (uint64_t)want > h->len) want = (int64_t)h->len;
    h->pos = (uint64_t)want;

    const uint64_t shown = h->sector_mode ? h->pos / ISO_SECTOR : h->pos;
    psp_cpu.r[PSP_REG_V0] = (uint32_t)shown;
    psp_cpu.r[PSP_REG_V1] = (uint32_t)(shown >> 32);
}

/* sceIoLseek32 is the same seek with a 32-bit offset and a 32-bit result, so
 * its arguments are *not* register-aligned: the offset is $a1 and the whence
 * $a2. It had never been registered, which meant a call to it returned
 * whatever was in $v0 -- zero, in practice. A test that seeks to the end to
 * size a file then read it got a length of zero and went on to decode an
 * empty buffer, which is why sascore's vag output was silence with no error
 * anywhere to explain it. */
static void hle_Lseek32(void) {
    io_file *h = fd_arg();
    if (!h || !h->f) { psp_ret(IO_ERROR_BADF); return; }
    const int64_t off = (int32_t)psp_arg(1);
    const uint32_t whence = psp_arg(2);
    /* Reuse the 64-bit seek so the window, sector mode and clamping stay in
     * one place: put the offset where that one looks for it. */
    psp_cpu.r[PSP_REG_A2] = (uint32_t)(uint64_t)off;
    psp_cpu.r[PSP_REG_A3] = (uint32_t)((uint64_t)off >> 32);
    psp_cpu.r[PSP_REG_T0] = whence;
    hle_Lseek();
    /* 32-bit result: $v1 is not part of it. */
    psp_cpu.r[PSP_REG_V1] = 0;
}

/* SceIoStat, laid out exactly as pspiofilemgr_stat.h declares it:
 *
 *    0  SceMode        st_mode
 *    4  unsigned int   st_attr
 *    8  SceOff         st_size        64-bit, hence the padding at 12
 *   16  ScePspDateTime st_ctime       six u16 then a u32 -- 16 bytes
 *   32  ScePspDateTime st_atime
 *   48  ScePspDateTime st_mtime
 *   64  unsigned int   st_private[6]
 *
 * The size field is the one that matters. A game stats a file to find out how
 * much to allocate and how much to read, and a stat that reports success while
 * writing nothing tells it the file is zero bytes long -- so it allocates
 * nothing, reads nothing, and then parses whatever was already in the buffer.
 * That is precisely how this ran before: sceIoOpen("umd1:") succeeded, the
 * stat returned 0 for "no handler", and the disc was never read. */
#define FIO_S_IFDIR   0x1000u
#define FIO_S_IFREG   0x2000u
#define FIO_S_IRWXU   0x01C0u
#define FIO_SO_IFDIR  0x0010u
#define FIO_SO_IFREG  0x0020u
#define PSP_STAT_LEN  88u

static void write_stat(uint32_t out, int is_dir, uint64_t size, uint32_t lba) {
    for (uint32_t i = 0; i < PSP_STAT_LEN; i += 4) psp_write32(out + i, 0);
    psp_write32(out + 0, (is_dir ? FIO_S_IFDIR : FIO_S_IFREG) | FIO_S_IRWXU);
    psp_write32(out + 4, is_dir ? FIO_SO_IFDIR : FIO_SO_IFREG);
    psp_write32(out + 8,  (uint32_t)size);
    psp_write32(out + 12, (uint32_t)(size >> 32));

    /* st_private[0] is the file's first sector on the disc.
     *
     * Undocumented by Sony and load-bearing: a game that wants to stream from
     * the UMD opens the raw device once, stats each file to find out *where* it
     * is, and seeks there itself rather than paying for the kernel's path
     * lookup on every read. Reporting zero tells it every file begins at sector
     * zero, so it never reads anything meaningful -- which is exactly what this
     * one did, opening umd1: and then never issuing a read.
     *
     * So st_private[0] carries the file's starting sector, which is the one
     * field of the stat block this game reads, and which is what the
     * firmware's ISO filesystem puts there (uofw src/kd/isofs/isofs.c:
     * `stat->st_private[0] = isoDir->lbn`). */
    psp_write32(out + 64, lba);

    /* The three timestamps stay zero. Nothing in a game's load path reads them,
     * and inventing a date would be less honest than reporting none. */
}

#ifndef _WIN32
/* ScePspDateTime: year, month, day, hour, minute, second as u16, then
 * microsecond as u32. */
static void write_date(uint32_t at, time_t t, int time_of_day) {
    struct tm tm;
    if (!localtime_r(&t, &tm)) return;
    psp_write16(at + 0, (uint16_t)(tm.tm_year + 1900));
    psp_write16(at + 2, (uint16_t)(tm.tm_mon + 1));
    psp_write16(at + 4, (uint16_t)tm.tm_mday);
    psp_write16(at + 6, (uint16_t)(time_of_day ? tm.tm_hour : 0));
    psp_write16(at + 8, (uint16_t)(time_of_day ? tm.tm_min : 0));
    psp_write16(at + 10, (uint16_t)(time_of_day ? tm.tm_sec : 0));
    psp_write32(at + 12, 0);
}

/* A Memory Stick file or directory as a PSP on firmware 6.60 stats it
 * (saveprobe steps 105-107, sceIoGetstat and each sceIoDread d_stat alike):
 * mode 0x21FF for a file and 0x11FF for a directory, attr 0x20 and 0x10, a
 * directory's size 0, the creation and modification dates with their time
 * of day and the access date alone at 00:00:00 (FAT keeps no access time),
 * microseconds 0, and st_private left as the caller had it. The dates are
 * the host file's: status change for creation, which Linux does not keep. */
static void write_fat_stat(uint32_t out, const struct stat *st) {
    const int dir = S_ISDIR(st->st_mode);
    const uint64_t size = dir ? 0 : (uint64_t)st->st_size;
    /* Read-only (sysprobe step 3, after Chstat): mode 0x016D, r-x for all,
     * and attr bit 0x01. The host keeps it as a file with no write bit. */
    const int ro = !(st->st_mode & 0222);
    psp_write32(out + 0, (dir ? FIO_S_IFDIR : FIO_S_IFREG) | (ro ? 0x016Du : 0x01FFu));
    psp_write32(out + 4, (dir ? FIO_SO_IFDIR : FIO_SO_IFREG) | (ro ? 0x01u : 0));
    psp_write32(out + 8,  (uint32_t)size);
    psp_write32(out + 12, (uint32_t)(size >> 32));
    write_date(out + 16, st->st_ctime, 1);
    write_date(out + 32, st->st_atime, 0);
    write_date(out + 48, st->st_mtime, 1);
}
#endif

static void hle_getstat_body(void);
static void hle_Getstat(void) { hle_getstat_body(); io_park(); }   /* io_park */

/* The stat block of a disc entry as the disc probe recorded it: mode
 * IO_DISC_FILE_MODE or IO_DISC_DIR_MODE, attribute IO_DISC_FILE_ATTR or
 * IO_DISC_DIR_ATTR, all three times the same constant (year 1900, month 1)
 * whatever the image's records say -- the probe's image dates its records
 * 2026-09-19 -- the start sector in st_private[0] and IO_DISC_PRIVATE_FILL
 * in the other five words. */
static void write_disc_stat(uint32_t out, int is_dir, uint64_t size, uint32_t lba) {
    psp_write32(out + 0, is_dir ? IO_DISC_DIR_MODE : IO_DISC_FILE_MODE);
    psp_write32(out + 4, is_dir ? IO_DISC_DIR_ATTR : IO_DISC_FILE_ATTR);
    psp_write32(out + 8,  (uint32_t)size);
    psp_write32(out + 12, (uint32_t)(size >> 32));
    for (uint32_t t = 16; t < 64; t += 16) {
        psp_write32(out + t, IO_DISC_TIME_WORD0);
        psp_write32(out + t + 4, 0);
        psp_write32(out + t + 8, 0);
        psp_write32(out + t + 12, 0);
    }
    psp_write32(out + 64, lba);
    for (uint32_t i = 68; i < PSP_STAT_LEN; i += 4) psp_write32(out + i, IO_DISC_PRIVATE_FILL);
}

static void hle_getstat_body(void) {
    char guest[512], host[1024];
    psp_str(psp_arg(0), guest, sizeof guest);
    const uint32_t out = psp_arg(1);

    /* The disc image answers for disc paths, including the raw device. */
    {
        iso_entry e;
        if (iso_lookup(guest, &e) == 0) {
            fclose(e.f);
            if (out) {
                /* The root reports no size and no sector; the device is a
                 * file whose size is in sectors (disc probe stats 4 and 6). */
                if (e.is_root) write_disc_stat(out, 1, 0, 0);
                else if (e.is_device) write_disc_stat(out, 0, e.len / ISO_SECTOR, 0);
                else write_disc_stat(out, e.is_dir, e.len, (uint32_t)(e.base / ISO_SECTOR));
            }
            psp_ret(SCE_KERNEL_ERROR_OK);
            return;
        }
    }

    const char *path = host;

    if (is_raw_umd(guest)) {
        if (!g_umd_image[0]) { psp_ret(IO_ERROR_NOENT); return; }
        path = g_umd_image;
    } else {
        map_path(guest, host, sizeof host);
    }

#ifndef _WIN32
    /* stat before fopen: Linux opens a directory for reading, and seeking to
     * its end read as a size of 0x7FFFFFFFFFFFFFFF with the file's mode. */
    {
        struct stat st;
        if (stat(path, &st) == 0) {
            const int dir = S_ISDIR(st.st_mode);
            if (out) {
                if (is_ms_path(guest)) write_fat_stat(out, &st);
                else write_stat(out, dir, dir ? 0 : (uint64_t)st.st_size, 0);
            }
            psp_ret(SCE_KERNEL_ERROR_OK);
            return;
        }
    }
#endif

    FILE *f = fopen(path, "rb");
    if (!f) {
        /* Could be a directory, which fopen refuses. Say so rather than
         * reporting a missing file: a loader that walks a tree checks. */
        DIR *d = opendir(path);
        if (d) { closedir(d); if (out) write_stat(out, 1, 0, 0); psp_ret(SCE_KERNEL_ERROR_OK); return; }
        fprintf(stderr, "psprecomp: sceIoGetstat failed: \"%s\" -> \"%s\"\n", guest, path);
        psp_ret(IO_ERROR_NOENT);
        return;
    }

    uint64_t size = 0;
    if (fseeko(f, 0, SEEK_END) == 0) { const off_t n = ftello(f); if (n > 0) size = (uint64_t)n; }
    fclose(f);

    if (out) write_stat(out, 0, size, 0);   /* a host file has no disc sector */
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* ---- ISO 9660 ---------------------------------------------------------------
 *
 * A game reaches the disc two ways. `umd0:`/`umd1:` with nothing after the
 * colon is the raw block device -- the image itself, read by sector.
 * Everything else is a *path*, and on hardware the kernel resolves it through
 * the filesystem on the disc. The rules a game can observe -- exact names,
 * what a directory or the device reports, the async states -- are the disc
 * probe's (tests/provenance/disc, run as a game booted from its own image)
 * and its derived io_observed.h. Mapping those paths to a host directory only works if
 * someone has already extracted the disc; backing them with the image directly
 * is both less setup and closer to what the console does.
 *
 * Only what a game needs to load a file is implemented: the primary volume
 * descriptor, and a walk down the directory records. No Joliet, no Rock Ridge,
 * no multi-extent files. PSP discs are plain ISO 9660 and the names a game
 * compiles in are the 8.3-style ones this reads.
 */
#define ISO_PVD_SECTOR  16u
#define ISO_DIR_IS_DIR  0x02u   /* flags byte in a directory record */

static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int iso_read_at(FILE *f, uint64_t off, void *dst, size_t n) {
    return (fseeko(f, (off_t)off, SEEK_SET) == 0 && fread(dst, 1, n, f) == n) ? 0 : -1;
}

/* Compare one path component against a directory record name.
 *
 * ISO 9660 stores file names suffixed with a version -- "AC.BIN" is written
 * "AC.BIN;1" -- while directories carry no suffix. The version is dropped
 * from the record's side only and the comparison is exact: the disc probe's
 * opens of disc0:/alpha.bin and disc0:/ALPHA.BIN;1 both failed with NOENT. */
static unsigned iso_name_len(const uint8_t *rec_name, unsigned rec_len) {
    for (unsigned i = 0; i < rec_len; i++)
        if (rec_name[i] == ';') return i;
    return rec_len;
}

static int iso_name_eq(const uint8_t *rec_name, unsigned rec_len, const char *want) {
    const unsigned n = iso_name_len(rec_name, rec_len);
    unsigned i = 0;
    for (; i < n && want[i]; i++)
        if (rec_name[i] != (unsigned char)want[i]) return 0;
    return i == n && want[i] == '\0';
}

/* Find `path` under the directory extent at `lba`, `bytes` long. On success
 * fills the extent and length of what it found. */
static int iso_lookup_in(FILE *f, uint32_t lba, uint32_t bytes,
                         const char *path, uint32_t *out_lba, uint32_t *out_len,
                         int *out_is_dir) {
    /* Split off the first component. */
    while (*path == '/' || *path == '\\') path++;
    if (!*path) return -1;
    char comp[256];
    size_t c = 0;
    while (path[c] && path[c] != '/' && path[c] != '\\' && c + 1 < sizeof comp) {
        comp[c] = path[c]; c++;
    }
    comp[c] = '\0';
    const char *rest = path + c;

    uint8_t *dir = (uint8_t *)malloc(bytes ? bytes : 1);
    if (!dir) return -1;
    if (iso_read_at(f, (uint64_t)lba * ISO_SECTOR, dir, bytes) != 0) { free(dir); return -1; }

    int rc = -1;
    for (uint32_t off = 0; off + 33 <= bytes; ) {
        const uint32_t rec = dir[off];
        if (!rec) {
            /* A directory record never straddles a sector, so a zero length is
             * padding to the end of this one. */
            off = (off / ISO_SECTOR + 1) * ISO_SECTOR;
            continue;
        }
        if (off + rec > bytes) break;
        const uint8_t *r = dir + off;
        const unsigned nlen = r[32];
        if (off + 33 + nlen <= bytes && iso_name_eq(r + 33, nlen, comp)) {
            const uint32_t elba = rd32le(r + 2), elen = rd32le(r + 10);
            const int isdir = (r[25] & ISO_DIR_IS_DIR) != 0;
            while (*rest == '/' || *rest == '\\') rest++;
            if (!*rest) {
                *out_lba = elba; *out_len = elen; *out_is_dir = isdir;
                rc = 0;
            } else if (isdir) {
                rc = iso_lookup_in(f, elba, elen, rest, out_lba, out_len, out_is_dir);
            }
            break;
        }
        off += rec;
    }
    free(dir);
    return rc;
}

/* One number of an extent name: hexadecimal after `0x`, else decimal. Moves
 * *s past it; 0 if there are no digits. */
static int lbn_number(const char **s, uint32_t *out) {
    const char *p = *s;
    const int hex = (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'));
    if (hex) p += 2;
    uint64_t v = 0;
    const char *digits = p;
    for (;; p++) {
        int d;
        if (*p >= '0' && *p <= '9') d = *p - '0';
        else if (hex && *p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
        else if (hex && *p >= 'A' && *p <= 'F') d = *p - 'A' + 10;
        else break;
        v = v * (hex ? 16 : 10) + (uint64_t)d;
        if (v > 0xFFFFFFFFu) return 0;
    }
    if (p == digits) return 0;
    *s = p; *out = (uint32_t)v;
    return 1;
}

/* `sce_lbn<sector>_size<bytes>`: the bytes starting at that sector of the
 * disc, as a file. UCUS98712 (WipEout Pulse) opens its archives this way:
 * `disc0:/sce_lbn0xf100_size0x12c6f640` is Data.wad's extent exactly, sector
 * 61696 and 315,029,056 bytes. That form is the one observed; decimal is
 * accepted too, and nothing past the size. */
static int lbn_name(const char *p, uint32_t *lbn, uint32_t *bytes) {
    if (strncmp(p, "sce_lbn", 7) != 0) return 0;
    p += 7;
    if (!lbn_number(&p, lbn) || strncmp(p, "_size", 5) != 0) return 0;
    p += 5;
    return lbn_number(&p, bytes) && *p == '\0';
}

/* Resolve a guest path inside the disc image. Returns 0 and fills `e` with
 * the byte range of what the path names, the image left open in e->f, or -1
 * if there is no image or no such path. */
static int iso_lookup(const char *guest, iso_entry *e)
{
    const char *p = guest;
    int device = 0;
    if      (!strncmp(p, "disc0:", 6)) p += 6;
    else if (!strncmp(p, "umd0:",  5)) { p += 5; device = 1; }
    else if (!strncmp(p, "umd1:",  5)) { p += 5; device = 1; }
    else return -1;                       /* not a disc path */
    if (!g_umd_image[0]) return -1;

    FILE *f = fopen(g_umd_image, "rb");
    if (!f) return -1;

    uint8_t pvd[ISO_SECTOR];
    if (iso_read_at(f, (uint64_t)ISO_PVD_SECTOR * ISO_SECTOR, pvd, sizeof pvd) != 0 ||
        memcmp(pvd + 1, "CD001", 5) != 0) {
        fclose(f);
        return -1;
    }

    /* The root directory record sits at offset 156 of the descriptor. */
    const uint8_t *root = pvd + 156;
    const uint32_t root_lba = rd32le(root + 2), root_len = rd32le(root + 10);

    memset(e, 0, sizeof *e);
    e->f = f;
    while (*p == '/' || *p == '\\') p++;
    if (!*p) {
        if (device) {                     /* umd0: or umd1: is the device itself */
            if (fseeko(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
            e->len = (uint64_t)ftello(f); e->is_device = 1;
            return 0;
        }
        e->base = (uint64_t)root_lba * ISO_SECTOR;   /* disc0:/ is the root */
        e->len = root_len; e->is_dir = e->is_root = 1;
        return 0;
    }

    /* A file named by its extent rather than its path. */
    uint32_t lbn = 0, bytes = 0;
    if (lbn_name(p, &lbn, &bytes)) {
        e->base = (uint64_t)lbn * ISO_SECTOR;
        e->len = bytes;
        return 0;
    }

    uint32_t lba = 0, blen = 0;
    int isdir = 0;
    if (iso_lookup_in(f, root_lba, root_len, p, &lba, &blen, &isdir) != 0) {
        fclose(f);
        return -1;
    }
    e->base = (uint64_t)lba * ISO_SECTOR;
    e->len = blen;
    e->is_dir = isdir;
    return 0;
}

static void hle_Rename(void) {
    char a[512], b[512], ha[1024], hb[1024];
    psp_str(psp_arg(0), a, sizeof a);
    psp_str(psp_arg(1), b, sizeof b);
    map_path(a, ha, sizeof ha);
    map_path(b, hb, sizeof hb);
    psp_ret(rename(ha, hb) == 0 ? 0 : 0x80010002);
}

/* ---- directories --------------------------------------------------------- */

#ifdef _WIN32
#include <direct.h>
#define MKDIR_ONE(p) _mkdir(p)
#define RMDIR_ONE(p) _rmdir(p)
#define UNLINK_ONE(p) _unlink(p)
#define STAT_ISDIR(st) (((st).st_mode & _S_IFDIR) != 0)
#else
#define MKDIR_ONE(p) mkdir(p, 0777)
#define RMDIR_ONE(p) rmdir(p)
#define UNLINK_ONE(p) unlink(p)
#define STAT_ISDIR(st) S_ISDIR((st).st_mode)
#endif

/* Create every missing level of `host`. Hardware Mkdir is single-level, but
 * the host tree starts empty -- hardware always has PSP/SAVEDATA/ -- so a
 * save flow that makes ms0:/PSP/SAVEDATA/<id> would fail on the absent
 * parents rather than on anything the game did. The deviation is this
 * function, in one place, not spread across callers. */
static void mkdir_parents(const char *host) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", host);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            MKDIR_ONE(tmp);
            *p = '/';
        }
    }
    MKDIR_ONE(tmp);
}

#ifndef _WIN32
/* When a host file was made, in ns, for ordering a Memory Stick listing:
 * its birth time where the filesystem keeps one, its modification time
 * where it does not. */
static uint64_t host_born(const char *path) {
#if defined(__linux__) && defined(STATX_BTIME)
    struct statx sx;
    if (statx(AT_FDCWD, path, 0, STATX_BTIME | STATX_MTIME, &sx) == 0) {
        const struct statx_timestamp *t = (sx.stx_mask & STATX_BTIME) ? &sx.stx_btime : &sx.stx_mtime;
        return (uint64_t)t->tv_sec * 1000000000u + t->tv_nsec;
    }
#endif
    struct stat st;
    if (stat(path, &st) != 0) return 0;
#if defined(__APPLE__)
    return (uint64_t)st.st_birthtimespec.tv_sec * 1000000000u + (uint64_t)st.st_birthtimespec.tv_nsec;
#else
    return (uint64_t)st.st_mtime * 1000000000u;
#endif
}
#endif

static int g_dir_sort_fat;

/* "." and ".." first, as both the PSP and the hosts list them. Then a
 * Memory Stick directory goes in the order its files were made, which is
 * the order of their FAT slots: saveprobe v2 (fw 6.60) lists DATA.BIN before
 * PARAM.SFO in every save (step 107's sceIoDread too), ICON0.PNG before both
 * (step 90), and after a WRITEDATASECURE of DATA2.BIN, DATA.BIN DATA2.BIN
 * PARAM.SFO (step 86), where the host's readdir gave whatever its filesystem
 * keeps (tmpfs: newest first). A slot freed by a delete and reused is not
 * modelled. Anywhere else the order is the name's, as an ISO 9660
 * directory's records are sorted, rather than the host's. */
static int dirent_order(const void *pa, const void *pb) {
    const io_dirent *a = pa, *b = pb;
    const int ra = !strcmp(a->name, ".") ? 0 : !strcmp(a->name, "..") ? 1 : 2;
    const int rb = !strcmp(b->name, ".") ? 0 : !strcmp(b->name, "..") ? 1 : 2;
    if (ra != rb) return ra - rb;
    if (g_dir_sort_fat && a->born != b->born) return a->born < b->born ? -1 : 1;
    return strcmp(a->name, b->name);
}

/* Append one entry to d's listing. 0 when out of memory. */
static int dir_add(io_dir *d, int *cap, const char *name) {
    if (d->n == *cap) {
        const int nc = *cap ? 2 * *cap : 32;
        io_dirent *ne = realloc(d->ent, (size_t)nc * sizeof *ne);
        if (!ne) return 0;
        d->ent = ne;
        *cap = nc;
    }
    io_dirent *e = &d->ent[d->n++];
    memset(e, 0, sizeof *e);
    snprintf(e->name, sizeof e->name, "%s", name);
    return 1;
}

/* sceIoDopen takes the whole listing at once and orders it (dirent_order);
 * sceIoDread then hands it out an entry at a time. The Windows half has not
 * been through a Windows build. */
static void hle_Dopen(void) {
    char guest[512], host[1024];
    psp_str(psp_arg(0), guest, sizeof guest);
    /* A disc path lists from the image. A file opens as an empty listing
     * (disc probe: sceIoDopen("disc0:/SUB/GAMMA.BIN") succeeded and its first
     * sceIoDread returned 0). */
    {
        iso_entry e;
        if (iso_lookup(guest, &e) == 0) {
            for (int i = 0; i < MAX_DIRS; i++) {
                if (g_dir[i].used) continue;
                io_dir *d = &g_dir[i];
                d->ent = NULL; d->n = d->pos = 0; d->fat = 0; d->host[0] = 0;
                d->records = NULL; d->bytes = d->off = 0;
                if (e.is_dir && e.len) {
                    d->records = malloc(e.len);
                    if (!d->records || iso_read_at(e.f, e.base, d->records, e.len) != 0) {
                        free(d->records); d->records = NULL;
                        fclose(e.f); psp_ret(IO_ERROR_NOENT); return;
                    }
                    d->bytes = (uint32_t)e.len;
                }
                fclose(e.f);
                d->iso = 1; d->used = 1;
                psp_ret((uint32_t)(i + 1));
                return;
            }
            fclose(e.f);
            psp_ret(0x80010018);
            return;
        }
    }
    map_path(guest, host, sizeof host);

    for (int i = 0; i < MAX_DIRS; i++) {
        if (g_dir[i].used) continue;
        io_dir *d = &g_dir[i];
        int cap = 0, ok = 1;
        d->ent = NULL;
        d->n = d->pos = 0;
        d->iso = 0; d->records = NULL; d->bytes = d->off = 0;
#ifdef _WIN32
        char pattern[1088];
        struct _finddata_t fd;
        snprintf(pattern, sizeof pattern, "%s/*", host);
        intptr_t h = _findfirst(pattern, &fd);
        if (h == -1) { psp_ret(0x80010002); return; }
        do {
            if (!(ok = dir_add(d, &cap, fd.name))) break;
            io_dirent *e = &d->ent[d->n - 1];
            e->born = (uint64_t)fd.time_create * 1000000000u;
            e->is_dir = (fd.attrib & _A_SUBDIR) != 0;
            e->size = e->is_dir ? 0 : (uint64_t)fd.size;
        } while (_findnext(h, &fd) == 0);
        _findclose(h);
#else
        DIR *dd = opendir(host);
        if (!dd) { psp_ret(0x80010002); return; }
        for (struct dirent *de; (de = readdir(dd)) != NULL;) {
            if (!(ok = dir_add(d, &cap, de->d_name))) break;
            char path[1300];
            snprintf(path, sizeof path, "%s/%s", host, de->d_name);
            d->ent[d->n - 1].born = host_born(path);
        }
        closedir(dd);
#endif
        if (!ok) { free(d->ent); d->ent = NULL; psp_ret(0x8001000C); return; }   /* ENOMEM */
        d->used = 1;
        d->fat = is_ms_path(guest);
        g_dir_sort_fat = d->fat;
        if (d->n > 1) qsort(d->ent, (size_t)d->n, sizeof *d->ent, dirent_order);
        snprintf(d->host, sizeof d->host, "%s", host);
        psp_ret((uint32_t)(i + 1));
        return;
    }
    psp_ret(0x80010018);
}

/* Fill a SceIoDirent: the entry's SceIoStat (88 bytes, as sceIoGetstat
 * writes it) and then d_name at +88. The name used to go at +52, from a
 * wrong idea of the stat's size, so every name read back empty: saveprobe
 * on a PSP (firmware 6.60) listed its save files by name where psprecomp
 * listed blanks. */
static void hle_Dread(void) {
    int32_t id = (int32_t)psp_arg(0) - 1;
    uint32_t dirent = psp_arg(1);
    if (id < 0 || id >= MAX_DIRS || !g_dir[id].used) { psp_ret(0x80020323); return; }
    /* An entry is an 88-byte stat and a 256-byte name. A buffer that is not
     * there is refused (PSPSDK's ILLEGAL_ADDR; not measured) rather than
     * written through, and the listing does not advance. */
    if (!psp_mem_ptr(dirent, PSP_STAT_LEN + 256)) { psp_ret(0x800200D3u); return; }

    if (g_dir[id].iso) {
        io_dir *entry = &g_dir[id];
        const uint32_t destination = dirent;
        /* Walk the extent's records in stored order, skipping the self and
         * parent records: the probe's root listing was SUB, ALPHA.BIN,
         * BETA.TXT, with no "." or "..". */
        while (entry->off + 33 <= entry->bytes) {
            const uint8_t *r = entry->records + entry->off;
            const uint32_t rec = r[0];
            if (!rec) {                   /* padding to the end of this sector */
                entry->off = (entry->off / ISO_SECTOR + 1) * ISO_SECTOR;
                continue;
            }
            if (entry->off + rec > entry->bytes) break;
            entry->off += rec;
            const unsigned nlen = r[32];
            if (33 + nlen > rec || (nlen == 1 && r[33] <= 1)) continue;
            write_disc_stat(destination, (r[25] & ISO_DIR_IS_DIR) != 0, rd32le(r + 10), rd32le(r + 2));
            char output[256] = {0};
            const unsigned length = iso_name_len(r + 33, nlen);
            memcpy(output, r + 33, length < 255 ? length : 255);
            psp_mem_write_block(destination + PSP_STAT_LEN, output, sizeof output);
            psp_ret(1);
            return;
        }
        /* The end of the listing writes only the name's first byte (disc
         * probe: every other word of the block kept its fill). */
        psp_write8(destination + PSP_STAT_LEN, 0);
        psp_ret(0);
        return;
    }

    if (g_dir[id].pos >= g_dir[id].n) { psp_ret(0); return; }
    const io_dirent *e = &g_dir[id].ent[g_dir[id].pos++];
    const char *name = e->name;

    int is_dir = 0, fat_done = 0;
    uint64_t size = 0;
#ifdef _WIN32
    is_dir = e->is_dir;
    size = e->size;
#else
    {
        char path[1300];
        struct stat st;
        snprintf(path, sizeof path, "%s/%s", g_dir[id].host, name);
        if (stat(path, &st) == 0) {
            is_dir = S_ISDIR(st.st_mode);
            size = is_dir ? 0 : (uint64_t)st.st_size;
            /* d_stat is what sceIoGetstat gives (saveprobe step 107). */
            if (g_dir[id].fat) { write_fat_stat(dirent, &st); fat_done = 1; }
        }
    }
#endif

    /* SceIoDirent: SceIoStat d_stat, char d_name[256], then d_private. */
    if (!fat_done) write_stat(dirent, is_dir, size, 0);
    uint32_t at = dirent + PSP_STAT_LEN;
    uint32_t n = 0;
    for (; n < 255 && name[n]; n++) psp_write8(at + n, (uint8_t)name[n]);
    psp_write8(at + n, 0);

    psp_ret(1);                       /* more entries may follow */
}

/* sceIoDevctl(dev, cmd, indata, inlen, outdata, outlen).
 *
 * Answering "no such device" is not a stub here, it is the answer. Homebrew
 * and test binaries probe for emulators by devctl'ing a name only an emulator
 * claims -- pspautotests uses "kemulator:" -- and a real PSP fails the call.
 * Failing it is how the caller learns it is on hardware, which is the mode we
 * are trying to be faithful to. Claiming success would opt every such program
 * into an emulator-specific path we do not implement.
 *
 * Left unimplemented it printed once per call and the caller kept asking. */
/* 0x80010000 | errno, with ENODEV = 19. */
#define SCE_ERROR_ENODEV   0x80010013u

static void hle_Devctl(void) {
    psp_ret(SCE_ERROR_ENODEV);
}

static void hle_Dclose(void) {
    int32_t id = (int32_t)psp_arg(0) - 1;
    if (id < 0 || id >= MAX_DIRS || !g_dir[id].used) { psp_ret(0x80020323); return; }
    free(g_dir[id].ent);
    g_dir[id].ent = NULL;
    g_dir[id].n = g_dir[id].pos = 0;
    free(g_dir[id].records);
    g_dir[id].records = NULL;
    g_dir[id].iso = 0;
    g_dir[id].used = 0;
    psp_ret(0);
}

/* The five directory calls M1 named and M4 needs. The game stats
 * ms0:/PSP/SAVEDATA/<id> to probe for saves and makes the tree when it
 * writes; nothing on a measured path calls these yet, so there is no capture
 * to shape them. NIDs and names are PSPSDK's IoFileMgrForUser stubs
 * (src/user/IoFileMgrForUser.S; BSD). They behave as the host filesystem
 * does, and the failures are uofw's errno codes (include/common/errors.h):
 * 0x80010002 FILE_NOT_FOUND, 0x80010011 FILE_ALREADY_EXISTS. */

/* The process-wide current directory for paths without a device prefix.
 * The game only ever passes absolute device paths (no measured open names
 * anything else), so this starts as the root and is recorded, not resolved,
 * until something relative arrives. */
/* Single-level, as sysprobe steps 1-2 measured on a Memory Stick (fw 6.60): a
 * missing parent is FILE_NOT_FOUND and nothing is made, an existing name
 * FILE_ALREADY_EXISTS, and a trailing slash INVALID_ARGUMENT (0x80010016).
 * A missing parent that is part of the tree a stick always has -- PSP,
 * PSP/SAVEDATA, PSP/GAME -- is made first (mkdir_parents explains why). */
static void hle_Mkdir(void) {
    char guest[512], host[1024];
    psp_str(psp_arg(0), guest, sizeof guest);
    map_path(guest, host, sizeof host);

    const size_t n = strlen(guest);
    if (n >= 2 && (guest[n - 1] == '/' || guest[n - 1] == '\\') && guest[n - 2] != ':') {
        psp_ret(0x80010016);
        return;
    }
    struct stat st;
    if (stat(host, &st) == 0) { psp_ret(0x80010011); return; }  /* EEXIST */
    {
        static const char *const always[] = { "ms0:/PSP", "ms0:/PSP/SAVEDATA", "ms0:/PSP/GAME" };
        char up[512];
        snprintf(up, sizeof up, "%s", guest);
        char *cut = strrchr(up, '/');
        if (cut) *cut = '\0';
        for (size_t i = 0; i < sizeof always / sizeof *always; i++)
            if (!strcasecmp(up, always[i])) psp_io_mkdir_all(up);
    }
    char parent[1024];
    snprintf(parent, sizeof parent, "%s", host);
    char *slash = strrchr(parent, '/');
    if (slash) *slash = '\0';
    if (slash && (stat(parent, &st) != 0 || !STAT_ISDIR(st))) { psp_ret(0x80010002); return; }
    psp_ret(MKDIR_ONE(host) == 0 ? SCE_KERNEL_ERROR_OK : 0x80010002);
}

/* A directory with something in it is ACCESS_DENIED (0x8001000D, sysprobe
 * step 2), not FILE_NOT_FOUND. */
static void hle_Rmdir(void) {
    char guest[512], host[1024];
    psp_str(psp_arg(0), guest, sizeof guest);
    map_path(guest, host, sizeof host);
    if (RMDIR_ONE(host) == 0) { psp_ret(SCE_KERNEL_ERROR_OK); return; }
    psp_ret(errno == ENOTEMPTY || errno == EEXIST ? 0x8001000D : 0x80010002);
}

/* A read-only file is ACCESS_DENIED (sysprobe step 4, after Chstat made it
 * so); a host would remove it, its directory being writable. */
static void hle_Remove(void) {
    char guest[512], host[1024];
    psp_str(psp_arg(0), guest, sizeof guest);
    map_path(guest, host, sizeof host);
    struct stat st;
    if (stat(host, &st) == 0 && !STAT_ISDIR(st) && !(st.st_mode & 0222)) {
        psp_ret(0x8001000D);
        io_park();
        return;
    }
    psp_ret(UNLINK_ONE(host) == 0 ? SCE_KERNEL_ERROR_OK : 0x80010002);
    io_park();
}

static void hle_Chdir(void) {
    char guest[512], host[1024];
    psp_str(psp_arg(0), guest, sizeof guest);
    map_path(guest, host, sizeof host);
    struct stat st;
    if (stat(host, &st) != 0 || !STAT_ISDIR(st)) { psp_ret(0x80010002); return; }
    snprintf(g_cwd, sizeof g_cwd, "%s", host);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* sceIoChstat(file, stat, bits), bit by bit as sysprobe step 3 measured on
 * a Memory Stick (fw 6.60), each answering 0:
 *   MODE (0x01)  read-only when the mode gives no write: 0444 makes it so,
 *                0777 undoes it. Getstat then shows mode r-x and attr 0x01.
 *   ATTR (0x02)  the FAT attributes as given: 0x01 read-only.
 *   CT (0x08), MT (0x20)  set, to the second (FAT's modification time is
 *                kept to two seconds; the probe's even second came back).
 *   SIZE, AT, PRVT, 0  nothing.
 * Two deviations, for what the host keeps no place for: ATTR's other bits
 * (the archive bit went with ATTR 0x01), and the creation time, which Linux
 * cannot set. Getstat reports the host's status-change time there. */
#define CST_MODE 0x01u
#define CST_ATTR 0x02u
#define CST_CT   0x08u
#define CST_MT   0x20u
static void hle_Chstat(void) {
    char guest[512], host[1024];
    psp_str(psp_arg(0), guest, sizeof guest);
    map_path(guest, host, sizeof host);
    struct stat st;
    if (stat(host, &st) != 0) { psp_ret(0x80010002); return; }
    const uint32_t in = psp_arg(1), bits = psp_arg(2);
#ifndef _WIN32
    int ro = !(st.st_mode & 0222);
    if (bits & CST_MODE) ro = !(psp_read32(in + 0) & 0222);
    if (bits & CST_ATTR) ro = (psp_read32(in + 4) & 0x01) != 0;
    if (bits & (CST_MODE | CST_ATTR))
        chmod(host, ro ? (st.st_mode & 07555) : (st.st_mode | 0200));
    if (bits & CST_MT) {
        struct tm tm;
        memset(&tm, 0, sizeof tm);
        tm.tm_year = psp_read16(in + 48) - 1900;
        tm.tm_mon  = psp_read16(in + 50) - 1;
        tm.tm_mday = psp_read16(in + 52);
        tm.tm_hour = psp_read16(in + 54);
        tm.tm_min  = psp_read16(in + 56);
        tm.tm_sec  = psp_read16(in + 58);
        tm.tm_isdst = -1;
        const struct timespec times[2] = { { 0, UTIME_OMIT }, { mktime(&tm), 0 } };
        utimensat(AT_FDCWD, host, times, 0);
    }
#else
    (void)in; (void)bits;
#endif
    psp_ret(SCE_KERNEL_ERROR_OK);
}

void psp_io_host_path(const char *guest, char *out, size_t cap) {
    map_path(guest, out, cap);
}

void psp_io_mkdir_all(const char *guest) {
    char host[1024];
    map_path(guest, host, sizeof host);
    mkdir_parents(host);
}

int psp_io_path_info(const char *guest, uint64_t *size, int *is_dir) {
    char host[1024];
    map_path(guest, host, sizeof host);
    struct stat st;
    if (stat(host, &st) != 0) return -1;
    if (size) *size = STAT_ISDIR(st) ? 0 : (uint64_t)st.st_size;
    if (is_dir) *is_dir = STAT_ISDIR(st) ? 1 : 0;
    return 0;
}

/* A window of an open host file, whole. */
static uint8_t *read_window(FILE *f, uint64_t base, uint64_t len, size_t *out_len) {
    if (len > 256u * 1024 * 1024) return NULL;
    uint8_t *buf = (uint8_t *)malloc(len ? (size_t)len : 1);
    if (!buf) return NULL;
    if (iso_read_at(f, base, buf, (size_t)len) != 0) { free(buf); return NULL; }
    *out_len = (size_t)len;
    return buf;
}

uint8_t *psp_io_read_whole(const char *guest, size_t *len) {
    iso_entry e;
    if (iso_lookup(guest, &e) == 0) {
        uint8_t *buf = e.is_dir || e.is_device ? NULL : read_window(e.f, e.base, e.len, len);
        fclose(e.f);
        return buf;
    }
    char host[1024];
    map_path(guest, host, sizeof host);
    FILE *f = fopen(host, "rb");
    if (!f) return NULL;
    uint8_t *buf = NULL;
    if (fseeko(f, 0, SEEK_END) == 0) {
        const off_t size = ftello(f);
        if (size >= 0) buf = read_window(f, 0, (uint64_t)size, len);
    }
    fclose(f);
    return buf;
}

uint8_t *psp_io_read_fd(uint32_t fd, size_t *len, int *on_disc) {
    if (fd < 3 || fd - 3 >= MAX_FILES || !g_file[fd - 3].used || !g_file[fd - 3].f) return NULL;
    const io_file *o = &g_file[fd - 3];
    if (on_disc) *on_disc = o->image;
    return read_window(o->f, o->base, o->len, len);
}

int psp_io_list_names(const char *guest, char names[][64], int cap) {
    char host[1024];
    map_path(guest, host, sizeof host);
    int n = 0;
#ifdef _WIN32
    char pattern[1088];
    snprintf(pattern, sizeof pattern, "%s/*", host);
    struct _finddata_t data;
    intptr_t h = _findfirst(pattern, &data);
    if (h == -1) return -1;
    do {
        if (!strcmp(data.name, ".") || !strcmp(data.name, "..")) continue;
        if (n < cap) {
            snprintf(names[n], 64, "%s", data.name);
            n++;
        }
    } while (_findnext(h, &data) == 0);
    _findclose(h);
    return n;
#else
    DIR *d = opendir(host);
    if (!d) return -1;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (n < cap) {
            snprintf(names[n], 64, "%s", de->d_name);
            n++;
        }
    }
    closedir(d);
    return n;
#endif
}

int psp_io_remove_tree(const char *guest) {
    char host[1024];
    map_path(guest, host, sizeof host);
    struct stat st;
    if (stat(host, &st) != 0) return -1;
    if (!STAT_ISDIR(st)) return UNLINK_ONE(host) == 0 ? 0 : -1;
#ifdef _WIN32
    char pattern[1088];
    snprintf(pattern, sizeof pattern, "%s/*", host);
    struct _finddata_t data;
    intptr_t h = _findfirst(pattern, &data);
    if (h != -1) {
        do {
            if (!strcmp(data.name, ".") || !strcmp(data.name, "..")) continue;
            char gchild[1024];
            snprintf(gchild, sizeof gchild, "%s/%s", guest, data.name);
            psp_io_remove_tree(gchild);
        } while (_findnext(h, &data) == 0);
        _findclose(h);
    }
    return RMDIR_ONE(host) == 0 ? 0 : -1;
#else
    DIR *d = opendir(host);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            char gchild[1024];
            snprintf(gchild, sizeof gchild, "%s/%s", guest, de->d_name);
            psp_io_remove_tree(gchild);
        }
        closedir(d);
    }
    return RMDIR_ONE(host) == 0 ? 0 : -1;
#endif
}

/* ---- asynchronous I/O -------------------------------------------------------
 *
 * This is how a game actually streams from the disc. It queues an operation,
 * carries on doing something else, and collects the answer later with poll or
 * wait -- so the load never stalls the frame. Armored Core does all of its
 * asset loading this way; the synchronous calls above are only used for
 * metadata, which is why the disc was opened and never read.
 *
 * Here the operation completes before the call returns: our reads are host
 * reads from a file that is already in the page cache, and inventing a delay
 * would only add a way to be wrong. What the caller sees follows the disc
 * probe (tests/provenance/disc, kind 5):
 *   - one poll after queueing reports the operation running (1); the probe's
 *     polling loop saw that state before the result came, so a game that
 *     polls gets one "not yet" and then the answer;
 *   - a wait, or the next poll, writes the 64-bit result and returns 0;
 *   - with nothing outstanding, poll, wait and sceIoGetAsyncStat return
 *     IO_ERROR_NO_ASYNC and leave the result alone;
 *   - a second operation before the first is collected returns
 *     IO_ERROR_ASYNC_BUSY and does nothing;
 *   - a failed sceIoOpenAsync still hands out a descriptor, whose result is
 *     the error sign-extended; collecting it releases the descriptor;
 *   - collecting sceIoCloseAsync releases the descriptor, and a bad or
 *     released descriptor is IO_ERROR_BADF.
 */
/* uofw include/common/errors.h: BAD_FILE_DESCRIPTOR and NO_ASYNC_OP. */
#define SCE_ERROR_BADF     0x80020323u
#define SCE_ERROR_NOASYNC  0x8002032Au

static void async_done(io_file *h, int64_t value) {
    h->result = value;
    h->has_result = 1;
    h->running = 1;
}

/* The descriptor a queueing call works on, or NULL with the error returned. */
static io_file *async_arg(void) {
    io_file *h = fd_arg();
    if (!h) { psp_ret(IO_ERROR_BADF); return NULL; }
    if (h->has_result) { psp_ret(IO_ERROR_ASYNC_BUSY); return NULL; }
    return h;
}

/* The async calls reuse the synchronous handlers rather than repeating them:
 * the work is identical and the only difference is where the answer goes. A
 * second copy of the read path would be a second thing to keep correct. */
static void hle_OpenAsync(void) {
    g_io_async = 1;
    hle_Open();
    g_io_async = 0;
    const uint32_t opened = psp_cpu.r[PSP_REG_V0];
    const int32_t fd = (int32_t)opened - 3;
    if (fd >= 0 && fd < MAX_FILES && g_file[fd].used) {
        async_done(&g_file[fd], (int64_t)(int32_t)opened);
        return;                           /* the descriptor, to poll on */
    }
    /* The open failed: the error is what the new descriptor's wait delivers,
     * and collecting it frees the slot again (close_pending). */
    for (int i = 0; i < MAX_FILES; i++) {
        if (g_file[i].used) continue;
        g_file[i].f = NULL; g_file[i].used = 1;
        g_file[i].image = g_file[i].writable = 0; g_file[i].host[0] = '\0';
        g_file[i].base = g_file[i].len = g_file[i].pos = 0;
        g_file[i].sector_mode = 0;
        g_file[i].close_pending = 1;
        async_done(&g_file[i], (int64_t)(int32_t)opened);
        psp_ret((uint32_t)(i + 3));
        return;
    }
    /* No free slot: the error itself is all there is to return. */
}

static void hle_ReadAsync(void) {
    io_file *h = async_arg();
    if (!h) return;
    g_io_async = 1;
    hle_Read();
    g_io_async = 0;
    async_done(h, (int64_t)(int32_t)psp_cpu.r[PSP_REG_V0]);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_WriteAsync(void) {
    /* stdout and stderr have no descriptor-table slot, so fd_arg() answers
     * NULL for them and the fall-through below returns BADF. The synchronous
     * hle_Write handles those two fds itself, and the async variant must too:
     * a game's panic path is "get the stdout fd, write the message, abort",
     * and when the write goes through the async variant the message is lost
     * exactly where it matters most -- the one write most worth seeing. There
     * is no slot to record an async result into, but the path that writes here
     * aborts without polling, so the text arriving is the part that matters. */
    const int32_t fd = (int32_t)psp_arg(0);
    if (fd == 1 || fd == 2) {
        hle_Write();
        psp_ret(SCE_KERNEL_ERROR_OK);
        return;
    }
    io_file *h = async_arg();
    if (!h) return;
    g_io_async = 1;
    hle_Write();
    g_io_async = 0;
    async_done(h, (int64_t)(int32_t)psp_cpu.r[PSP_REG_V0]);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_Lseek32Async(void) {
    io_file *h = async_arg();
    if (!h) return;
    hle_Lseek32();
    async_done(h, (int64_t)(int32_t)psp_cpu.r[PSP_REG_V0]);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_LseekAsync(void) {
    io_file *h = async_arg();
    if (!h) return;
    hle_Lseek();
    /* The seek result is 64-bit, returned in $v0:$v1. */
    async_done(h, (int64_t)(((uint64_t)psp_cpu.r[PSP_REG_V1] << 32) |
                            psp_cpu.r[PSP_REG_V0]));
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* The descriptor stays alive until the result is collected -- the close is
 * the operation being waited on, and freeing the slot now would leave nothing
 * to poll. */
static void hle_CloseAsync(void) {
    io_file *h = async_arg();
    if (!h) return;
    async_done(h, 0);
    h->close_pending = 1;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void collect_async(io_file *h, uint32_t out) {
    if (out) {
        psp_write32(out,     (uint32_t)(uint64_t)h->result);
        psp_write32(out + 4, (uint32_t)((uint64_t)h->result >> 32));
    }
    h->has_result = h->running = 0;
    if (h->close_pending) {
        if (h->f) fclose(h->f);
        h->f = NULL;
        h->used = 0;
        h->dirty = 0;
        h->close_pending = 0;
    }
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void async_poll(io_file *h, uint32_t out) {
    if (!h) { psp_ret(IO_ERROR_BADF); return; }
    if (!h->has_result) { psp_ret(IO_ERROR_NO_ASYNC); return; }
    if (h->running) { h->running = 0; psp_ret(1); return; }
    collect_async(h, out);
}

static void async_wait(io_file *h, uint32_t out) {
    if (!h) { psp_ret(IO_ERROR_BADF); return; }
    if (!h->has_result) { psp_ret(IO_ERROR_NO_ASYNC); return; }
    collect_async(h, out);
}

static void hle_PollAsync(void) { async_poll(fd_arg(), psp_arg(1)); }
static void hle_WaitAsync(void) { async_wait(fd_arg(), psp_arg(1)); }

/* sceIoGetAsyncStat(fd, poll, result): a poll when `poll` is nonzero, a wait
 * otherwise (disc probe cases 5, 9). */
static void hle_GetAsyncStat(void) {
    if (psp_arg(1)) async_poll(fd_arg(), psp_arg(2));
    else async_wait(fd_arg(), psp_arg(2));
}

/* ---- save states (psprecomp/state.h) ----------------------------------------
 *
 * A descriptor is saved as how to open it again and where it was: the disc
 * image, or a host path read-only. Paths under the root are saved relative to
 * it, so a state outlives the data directory moving. A writable file or an
 * open directory refuses the save; a game holds neither across frames. */
typedef struct {
    int32_t  used, open, image, sector_mode, has_result, close_pending, running, rooted;
    uint64_t base, len, pos;
    int64_t  result;
    char     path[1024];
} saved_file;

static const char *io_refuse(void) {
    for (int i = 0; i < MAX_FILES; i++)
        if (g_file[i].used && g_file[i].f && g_file[i].writable) return "a file is open for writing";
    for (int i = 0; i < MAX_DIRS; i++)
        if (g_dir[i].used) return "a directory is open";
    return NULL;
}

/* A host path under the root, as the part after it; else as it is. */
static int unroot(const char *host, char *out, size_t cap) {
    const size_t n = strlen(g_root);
    if (n && !strncmp(host, g_root, n) && (host[n] == '/' || host[n] == '\\' || !host[n])) {
        snprintf(out, cap, "%s", host + n);
        return 1;
    }
    snprintf(out, cap, "%s", host);
    return 0;
}

static int io_save(psp_state_writer *w) {
    static saved_file out[MAX_FILES];
    memset(out, 0, sizeof out);
    for (int i = 0; i < MAX_FILES; i++) {
        const io_file *f = &g_file[i];
        if (!f->used) continue;
        saved_file *s = &out[i];
        s->used = 1;
        s->open = f->f != NULL;
        s->image = f->image;
        s->sector_mode = f->sector_mode;
        s->has_result = f->has_result;
        s->close_pending = f->close_pending;
        s->running = f->running;
        s->base = f->base; s->len = f->len; s->pos = f->pos;
        s->result = f->result;
        s->rooted = unroot(f->host, s->path, sizeof s->path);
    }
    char cwd[sizeof g_cwd + 1];
    cwd[0] = (char)unroot(g_cwd, cwd + 1, sizeof cwd - 1);
    return psp_state_put(w, "iofiles", out, sizeof out) || psp_state_put(w, "iocwd", cwd, sizeof cwd) ? -1 : 0;
}

static int io_load(psp_state_reader *r, char *why, size_t size) {
    size_t n, m;
    const saved_file *in = psp_state_get(r, "iofiles", &n);
    const char *cwd = psp_state_get(r, "iocwd", &m);
    if (!in || n != MAX_FILES * sizeof *in || !cwd || m != sizeof g_cwd + 1) {
        snprintf(why, size, "the open files are missing");
        return -1;
    }
    psp_io_reset();
    if (cwd[0]) snprintf(g_cwd, sizeof g_cwd, "%s%s", g_root, cwd + 1);
    else snprintf(g_cwd, sizeof g_cwd, "%s", cwd + 1);
    for (int i = 0; i < MAX_FILES; i++) {
        saved_file s;
        memcpy(&s, &in[i], sizeof s);
        if (!s.used) continue;
        io_file *f = &g_file[i];
        memset(f, 0, sizeof *f);
        f->used = 1;
        f->image = s.image;
        s.path[sizeof s.path - 1] = '\0';
        if (s.rooted) snprintf(f->host, sizeof f->host, "%s%.511s", g_root, s.path);
        else snprintf(f->host, sizeof f->host, "%s", s.path);
        if (s.open) {
            f->f = fopen(s.image ? g_umd_image : f->host, "rb");
            if (!f->f) {
                snprintf(why, size, "%.200s cannot be opened again", s.image ? g_umd_image : f->host);
                return -1;
            }
        }
        f->sector_mode = s.sector_mode;
        f->has_result = s.has_result;
        f->close_pending = s.close_pending;
        f->running = s.running;
        f->base = s.base; f->len = s.len; f->pos = s.pos;
        f->result = s.result;
    }
    return 0;
}

void psp_io_register(void) {
    static const psp_state_part part = { .name = "io", .refuse = io_refuse, .save = io_save, .load = io_load };
    psp_state_register(&part);
    psp_hle_register(0x109F50BC, "IoFileMgrForUser", "sceIoOpen",   hle_Open);
    psp_hle_register(0x810C4BC3, "IoFileMgrForUser", "sceIoClose",  hle_Close);
    psp_hle_register(0x6A638D83, "IoFileMgrForUser", "sceIoRead",   hle_Read);
    psp_hle_register(0x42EC03AC, "IoFileMgrForUser", "sceIoWrite",  hle_Write);
    psp_hle_register(0x27EB27B8, "IoFileMgrForUser", "sceIoLseek",  hle_Lseek);
    psp_hle_register(0x779103A0, "IoFileMgrForUser", "sceIoRename", hle_Rename);
    psp_hle_register(0xACE946E8, "IoFileMgrForUser", "sceIoGetstat", hle_Getstat);

    psp_hle_register(0x89AA9906, "IoFileMgrForUser", "sceIoOpenAsync",   hle_OpenAsync);
    psp_hle_register(0xA0B5A7C2, "IoFileMgrForUser", "sceIoReadAsync",   hle_ReadAsync);
    psp_hle_register(0x0FACAB19, "IoFileMgrForUser", "sceIoWriteAsync",  hle_WriteAsync);
    psp_hle_register(0x71B19E77, "IoFileMgrForUser", "sceIoLseekAsync",  hle_LseekAsync);
    psp_hle_register(0x68963324, "IoFileMgrForUser", "sceIoLseek32",  hle_Lseek32);
    psp_hle_register(0x1B385D8F, "IoFileMgrForUser", "sceIoLseek32Async", hle_Lseek32Async);
    psp_hle_register(0xFF5940B6, "IoFileMgrForUser", "sceIoCloseAsync",  hle_CloseAsync);
    psp_hle_register(0x3251EA56, "IoFileMgrForUser", "sceIoPollAsync",   hle_PollAsync);
    psp_hle_register(0xE23EEC33, "IoFileMgrForUser", "sceIoWaitAsync",   hle_WaitAsync);
    psp_hle_register(0x35DBD746, "IoFileMgrForUser", "sceIoWaitAsyncCB", hle_WaitAsync);
    psp_hle_register(0xCB05F8D6, "IoFileMgrForUser", "sceIoGetAsyncStat", hle_GetAsyncStat);
    psp_hle_register(0xB29DDF9C, "IoFileMgrForUser", "sceIoDopen",  hle_Dopen);
    psp_hle_register(0xE3EB004C, "IoFileMgrForUser", "sceIoDread",  hle_Dread);
    psp_hle_register(0xEB092469, "IoFileMgrForUser", "sceIoDclose", hle_Dclose);
    psp_hle_register(0x54F5FB11, "IoFileMgrForUser", "sceIoDevctl", hle_Devctl);
    psp_hle_register(0x06A70004, "IoFileMgrForUser", "sceIoMkdir",  hle_Mkdir);
    psp_hle_register(0x1117C65F, "IoFileMgrForUser", "sceIoRmdir",  hle_Rmdir);
    psp_hle_register(0xF27A9C51, "IoFileMgrForUser", "sceIoRemove", hle_Remove);
    psp_hle_register(0x55F4717D, "IoFileMgrForUser", "sceIoChdir",  hle_Chdir);
    psp_hle_register(0xB8A740F4, "IoFileMgrForUser", "sceIoChstat", hle_Chstat);
}
