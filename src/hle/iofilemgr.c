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

#include "psprecomp/hle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <io.h>
#else
#  include <dirent.h>
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
    int64_t  result;
    /* The raw UMD block device counts in 2048-byte sectors, not bytes: both the
     * offset given to sceIoLseek and the length given to sceIoRead are LBAs.
     * A game streaming from the disc opens the device this way, stats each file
     * to get its start sector, and seeks straight there. Treating those numbers
     * as bytes reads 64 bytes from offset 58144 where the game asked for 64
     * sectors from sector 58144 -- a plausible-looking read of entirely the
     * wrong thing.
     *
     * PPSSPP marks the same distinction, in Core/FileSystems/ISOFileSystem.cpp:
     *   "when open as umd1:... the param in sceIoLseek and sceIoRead is lba
     *    mode. we must mark it." */
    int      sector_mode;
} io_file;

typedef struct {
    int used;
#ifdef _WIN32
    intptr_t handle;
    struct _finddata_t data;
    int first;
    int done;
#else
    DIR *dir;
#endif
} io_dir;

static io_file g_file[MAX_FILES];
static io_dir  g_dir[MAX_DIRS];
static char    g_root[512];
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
 * `umd0:` and `umd1:` name the device itself. Anything after the colon makes it
 * a path again, and `disc0:` is always the filesystem view, so neither is
 * treated as raw. Getting that distinction wrong would turn every asset load
 * into an attempt to open the whole disc. */
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
        g_file[i].has_result = g_file[i].close_pending = 0;
        g_file[i].result = 0;
        g_file[i].sector_mode = 0;
    }
    memset(g_dir, 0, sizeof g_dir);
    g_bytes_read = 0;
    if (!g_root[0]) psp_io_set_root(".");
}

void psp_io_init(void) { g_root[0] = '\0'; psp_io_reset(); }

uint64_t psp_io_bytes_read(void) { return g_bytes_read; }

/* Rewrite a PSP path into a host path. The device prefix becomes a
 * subdirectory so a disc image and a memory-stick image can coexist under one
 * root without colliding. */
static void map_path(const char *guest, char *out, size_t cap) {
    const char *p = guest;
    const char *sub = "disc";

    if      (!strncmp(p, "disc0:", 6)) { p += 6; sub = "disc"; }
    else if (!strncmp(p, "umd0:",  5)) { p += 5; sub = "disc"; }
    else if (!strncmp(p, "ms0:",   4)) { p += 4; sub = "ms";   }
    else if (!strncmp(p, "flash0:",7)) { p += 7; sub = "flash";}
    else if (!strncmp(p, "host0:", 6)) { p += 6; sub = "host"; }
    /* Seen from real titles and previously unmapped, which left the colon in
     * the host path and made every such open fail in a way that looked like a
     * missing file rather than a missing prefix. */
    else if (!strncmp(p, "umd1:",  5)) { p += 5; sub = "disc"; }
    else if (!strncmp(p, "msstor0p1:", 10)) { p += 10; sub = "ms"; }
    else if (!strncmp(p, "msstor0:",   8)) { p += 8;  sub = "ms"; }

    while (*p == '/' || *p == '\\') p++;
    snprintf(out, cap, "%s/%s/%s", g_root, sub, p);
}

/* One ISO 9660 sector. Up here rather than with the reader below because
 * sceIoGetstat reports a file's start sector and needs it too. */
#define ISO_SECTOR 2048u

/* Defined with the rest of the ISO 9660 reader below; declared here because
 * opening and stat-ing both consult the disc image before the host tree. */
static int iso_lookup(const char *guest, uint64_t *base, uint64_t *len,
                      int *is_dir, FILE **out);

static void hle_Open(void) {
    char guest[512], host[1024];
    psp_str(psp_arg(0), guest, sizeof guest);
    uint32_t flags = psp_arg(1);

    /* A disc path is served from the image if there is one. The raw device
     * falls out of this as the case with nothing after the colon. */
    if (!(flags & (PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC))) {
        uint64_t base, len; int isdir; FILE *img;
        if (iso_lookup(guest, &base, &len, &isdir, &img) == 0) {
            if (isdir) { fclose(img); psp_ret(0x80010014); return; }   /* EISDIR */
            for (int i = 0; i < MAX_FILES; i++) {
                if (g_file[i].used) continue;
                g_file[i].f = img; g_file[i].used = 1;
                g_file[i].base = base; g_file[i].len = len; g_file[i].pos = 0;
                g_file[i].has_result = g_file[i].close_pending = 0;
                g_file[i].sector_mode = is_raw_umd(guest);
                psp_ret((uint32_t)(i + 3));
                return;
            }
            fclose(img);
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
            psp_ret(0x80010002);
            return;
        }
        FILE *f = fopen(g_umd_image, "rb");
        if (!f) {
            fprintf(stderr, "psprecomp: cannot open disc image %s\n", g_umd_image);
            psp_ret(0x80010002);
            return;
        }
        for (int i = 0; i < MAX_FILES; i++) {
            if (g_file[i].used) continue;
            g_file[i].f = f;
            g_file[i].used = 1;
            g_file[i].base = 0;
            g_file[i].pos  = 0;
            g_file[i].len  = (fseeko(f, 0, SEEK_END) == 0 && ftello(f) > 0)
                             ? (uint64_t)ftello(f) : 0;
            psp_ret((uint32_t)(i + 3));
            return;
        }
        fclose(f);
        psp_ret(0x80010018);
        return;
    }

    map_path(guest, host, sizeof host);

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
            fprintf(stderr, "psprecomp: sceIoOpen failed: \"%s\" -> \"%s\"\n", guest, host);
        }
        psp_ret(0x80010002);          /* ENOENT */
        return;
    }

    for (int i = 0; i < MAX_FILES; i++) {
        if (g_file[i].used) continue;
        g_file[i].f = f;
        g_file[i].used = 1;
        g_file[i].base = 0;
        g_file[i].pos  = 0;
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

static io_file *fd_arg(void) {
    int32_t fd = (int32_t)psp_arg(0) - 3;
    if (fd < 0 || fd >= MAX_FILES || !g_file[fd].used) return NULL;
    return &g_file[fd];
}

static void hle_Close(void) {
    io_file *h = fd_arg();
    if (!h) { psp_ret(0x80020323); return; }
    fclose(h->f);
    h->f = NULL;
    h->used = 0;
    psp_ret(0);
}

static void hle_Read(void) {
    io_file *h = fd_arg();
    uint32_t dst = psp_arg(1), size = psp_arg(2);
    if (!h) { psp_ret(0x80020323); return; }
    if (!size) { psp_ret(0); return; }
    if (h->sector_mode) size *= ISO_SECTOR;   /* the count is in sectors */

    /* Read through a host buffer and then place it, so a read that straddles
     * the end of a guest region is rejected by the memory layer rather than
     * writing past it. */
    /* Clamp to the file's window: an asset inside a disc image is followed by
     * the next asset, not by end-of-file, so reading past it would quietly
     * return a neighbour's bytes instead of a short read. */
    if (h->pos >= h->len) { psp_ret(0); return; }
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
    if (!h) {
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
        psp_ret(0x80020323);
        return;
    }

    uint8_t *tmp = (uint8_t *)malloc(size ? size : 1);
    if (!tmp) { psp_ret(0x80020190); return; }
    for (uint32_t i = 0; i < size; i++) tmp[i] = psp_read8(src + i);
    size_t put = fwrite(tmp, 1, size, h->f);
    free(tmp);
    psp_ret((uint32_t)put);
}

/* sceIoLseek takes a 64-bit offset and returns one. Under o32 a 64-bit
 * argument is register-aligned, so it lands in $a2:$a3 rather than $a1:$a2 --
 * and the result comes back in $v0:$v1. Getting either wrong makes every seek
 * land somewhere plausible but wrong. */
static void hle_Lseek(void) {
    io_file *h = fd_arg();
    uint64_t off = (uint64_t)psp_arg(2) | ((uint64_t)psp_arg(3) << 32);
    uint32_t whence = psp_arg(4);
    if (!h) { psp_ret(0x80020323); return; }

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
    if ((uint64_t)want > h->len) want = (int64_t)h->len;
    h->pos = (uint64_t)want;

    const uint64_t shown = h->sector_mode ? h->pos / ISO_SECTOR : h->pos;
    psp_cpu.r[PSP_REG_V0] = (uint32_t)shown;
    psp_cpu.r[PSP_REG_V1] = (uint32_t)(shown >> 32);
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
     * PPSSPP does the same, in Core/HLE/sceIo.cpp __IoGetStat:
     *     stat->st_private[0] = info.startSector; */
    psp_write32(out + 64, lba);

    /* The three timestamps stay zero. Nothing in a game's load path reads them,
     * and inventing a date would be less honest than reporting none. */
}

static void hle_Getstat(void) {
    char guest[512], host[1024];
    psp_str(psp_arg(0), guest, sizeof guest);
    const uint32_t out = psp_arg(1);

    /* The disc image answers for disc paths, including the raw device. */
    {
        uint64_t base, len; int isdir; FILE *img;
        if (iso_lookup(guest, &base, &len, &isdir, &img) == 0) {
            fclose(img);
            if (out) write_stat(out, isdir, len, (uint32_t)(base / ISO_SECTOR));
            psp_ret(SCE_KERNEL_ERROR_OK);
            return;
        }
    }

    const char *path = host;

    if (is_raw_umd(guest)) {
        if (!g_umd_image[0]) { psp_ret(0x80010002); return; }
        path = g_umd_image;
    } else {
        map_path(guest, host, sizeof host);
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        /* Could be a directory, which fopen refuses. Say so rather than
         * reporting a missing file: a loader that walks a tree checks. */
        DIR *d = opendir(path);
        if (d) { closedir(d); if (out) write_stat(out, 1, 0, 0); psp_ret(SCE_KERNEL_ERROR_OK); return; }
        fprintf(stderr, "psprecomp: sceIoGetstat failed: \"%s\" -> \"%s\"\n", guest, path);
        psp_ret(0x80010002);       /* ENOENT */
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
 * colon is the raw block device -- the image itself, read by sector. Everything
 * else is a *path*, and on hardware the kernel resolves it through the
 * filesystem on the disc. Mapping those paths to a host directory only works if
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
 * ISO 9660 stores file names uppercased and suffixed with a version -- "AC.BIN"
 * is written "AC.BIN;1" -- while directories carry no suffix. Both sides are
 * folded to uppercase because a game's compiled-in path is not required to
 * match the disc's case, and the version is ignored. */
static int iso_name_eq(const uint8_t *rec_name, unsigned rec_len, const char *want) {
    unsigned n = rec_len;
    for (unsigned i = 0; i < rec_len; i++)
        if (rec_name[i] == ';') { n = i; break; }

    unsigned i = 0;
    for (; i < n && want[i]; i++) {
        int a = rec_name[i], b = (unsigned char)want[i];
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return 0;
    }
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

/* Resolve a guest path inside the disc image. Returns 0 and the byte range of
 * the file, or -1 if there is no image or no such path. */
static int iso_lookup(const char *guest, uint64_t *base, uint64_t *len,
                      int *is_dir, FILE **out)
{
    const char *p = guest;
    if      (!strncmp(p, "disc0:", 6)) p += 6;
    else if (!strncmp(p, "umd0:",  5)) p += 5;
    else if (!strncmp(p, "umd1:",  5)) p += 5;
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

    while (*p == '/' || *p == '\\') p++;
    if (!*p) {                            /* the device itself */
        if (fseeko(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
        *base = 0; *len = (uint64_t)ftello(f); *is_dir = 0; *out = f;
        return 0;
    }

    uint32_t lba = 0, blen = 0;
    int isdir = 0;
    if (iso_lookup_in(f, root_lba, root_len, p, &lba, &blen, &isdir) != 0) {
        fclose(f);
        return -1;
    }
    *base = (uint64_t)lba * ISO_SECTOR;
    *len = blen;
    *is_dir = isdir;
    *out = f;
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

static void hle_Dopen(void) {
    char guest[512], host[1024];
    psp_str(psp_arg(0), guest, sizeof guest);
    map_path(guest, host, sizeof host);

    for (int i = 0; i < MAX_DIRS; i++) {
        if (g_dir[i].used) continue;
#ifdef _WIN32
        char pattern[1088];
        snprintf(pattern, sizeof pattern, "%s/*", host);
        g_dir[i].handle = _findfirst(pattern, &g_dir[i].data);
        if (g_dir[i].handle == -1) { psp_ret(0x80010002); return; }
        g_dir[i].first = 1;
        g_dir[i].done = 0;
#else
        g_dir[i].dir = opendir(host);
        if (!g_dir[i].dir) { psp_ret(0x80010002); return; }
#endif
        g_dir[i].used = 1;
        psp_ret((uint32_t)(i + 1));
        return;
    }
    psp_ret(0x80010018);
}

/* Fill a SceIoDirent. Only the name is populated: games use Dread to enumerate
 * save slots and asset directories by name, and a wrong stat block would be
 * worse than an empty one. */
static void hle_Dread(void) {
    int32_t id = (int32_t)psp_arg(0) - 1;
    uint32_t dirent = psp_arg(1);
    if (id < 0 || id >= MAX_DIRS || !g_dir[id].used) { psp_ret(0x80020323); return; }

    const char *name = NULL;
#ifdef _WIN32
    if (g_dir[id].done) { psp_ret(0); return; }
    if (g_dir[id].first) {
        g_dir[id].first = 0;
        name = g_dir[id].data.name;
    } else if (_findnext(g_dir[id].handle, &g_dir[id].data) == 0) {
        name = g_dir[id].data.name;
    } else {
        g_dir[id].done = 1;
        psp_ret(0);
        return;
    }
#else
    struct dirent *de = readdir(g_dir[id].dir);
    if (!de) { psp_ret(0); return; }
    name = de->d_name;
#endif

    /* SceIoDirent: a 52-byte SceIoStat, then char d_name[256]. */
    for (int i = 0; i < 52; i++) psp_write8(dirent + (uint32_t)i, 0);
    uint32_t at = dirent + 52;
    for (uint32_t i = 0; i < 255 && name[i]; i++) psp_write8(at + i, (uint8_t)name[i]);
    psp_write8(at + (uint32_t)strlen(name), 0);

    psp_ret(1);                       /* more entries may follow */
}

static void hle_Dclose(void) {
    int32_t id = (int32_t)psp_arg(0) - 1;
    if (id < 0 || id >= MAX_DIRS || !g_dir[id].used) { psp_ret(0x80020323); return; }
#ifdef _WIN32
    if (g_dir[id].handle != -1) _findclose(g_dir[id].handle);
#else
    if (g_dir[id].dir) closedir(g_dir[id].dir);
#endif
    g_dir[id].used = 0;
    psp_ret(0);
}

/* ---- asynchronous I/O -------------------------------------------------------
 *
 * This is how a game actually streams from the disc. It queues an operation,
 * carries on doing something else, and collects the answer later with poll or
 * wait -- so the load never stalls the frame. Armored Core does all of its
 * asset loading this way; the synchronous calls above are only used for
 * metadata, which is why the disc was opened and never read.
 *
 * Here the "queue" completes before it returns: our reads are host reads from a
 * file that is already in the page cache, and inventing a delay would only add
 * a way to be wrong. So every operation stores its result immediately and poll
 * reports it on the first ask.
 *
 * Behaviour follows PPSSPP's Core/HLE/sceIo.cpp:
 *   - an operation still running    -> poll returns 1, wait blocks
 *   - a result waiting              -> written as a 64-bit value, poll returns 0
 *   - nothing outstanding           -> SCE_KERNEL_ERROR_NOASYNC
 * The first case cannot arise here, which is the one place this differs from
 * hardware: a game that depends on a read *not* having finished yet sees it
 * finished. That is the safe direction -- the data is there either way.
 */
#define SCE_ERROR_BADF     0x80020323u
#define SCE_ERROR_NOASYNC  0x80020321u

static void async_done(io_file *h, int64_t value) {
    h->result = value;
    h->has_result = 1;
}

/* The async calls reuse the synchronous handlers rather than repeating them:
 * the work is identical and the only difference is where the answer goes. A
 * second copy of the read path would be a second thing to keep correct. */
static void hle_OpenAsync(void) {
    hle_Open();
    const int32_t fd = (int32_t)psp_cpu.r[PSP_REG_V0] - 3;
    if (fd >= 0 && fd < MAX_FILES && g_file[fd].used)
        async_done(&g_file[fd], (int64_t)(int32_t)psp_cpu.r[PSP_REG_V0]);
    /* Returns the descriptor, not a status: the caller needs it to poll on. */
}

static void hle_ReadAsync(void) {
    io_file *h = fd_arg();
    if (!h) { psp_ret(SCE_ERROR_BADF); return; }
    hle_Read();
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
    io_file *h = fd_arg();
    if (!h) { psp_ret(SCE_ERROR_BADF); return; }
    hle_Write();
    async_done(h, (int64_t)(int32_t)psp_cpu.r[PSP_REG_V0]);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_LseekAsync(void) {
    io_file *h = fd_arg();
    if (!h) { psp_ret(SCE_ERROR_BADF); return; }
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
    io_file *h = fd_arg();
    if (!h) { psp_ret(SCE_ERROR_BADF); return; }
    async_done(h, 0);
    h->close_pending = 1;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void collect_async(io_file *h) {
    const uint32_t out = psp_arg(1);
    if (out) {
        psp_write32(out,     (uint32_t)(uint64_t)h->result);
        psp_write32(out + 4, (uint32_t)((uint64_t)h->result >> 32));
    }
    h->has_result = 0;
    if (h->close_pending) {
        if (h->f) fclose(h->f);
        h->f = NULL;
        h->used = 0;
        h->close_pending = 0;
    }
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_PollAsync(void) {
    io_file *h = fd_arg();
    if (!h) { psp_ret(SCE_ERROR_BADF); return; }
    if (!h->has_result) { psp_ret(SCE_ERROR_NOASYNC); return; }
    collect_async(h);
}

/* Nothing is ever still running, so waiting is collecting. */
static void hle_WaitAsync(void) { hle_PollAsync(); }

void psp_io_register(void) {
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
    psp_hle_register(0xFF5940B6, "IoFileMgrForUser", "sceIoCloseAsync",  hle_CloseAsync);
    psp_hle_register(0x3251EA56, "IoFileMgrForUser", "sceIoPollAsync",   hle_PollAsync);
    psp_hle_register(0xE23EEC33, "IoFileMgrForUser", "sceIoWaitAsync",   hle_WaitAsync);
    psp_hle_register(0x35DBD746, "IoFileMgrForUser", "sceIoWaitAsyncCB", hle_WaitAsync);
    psp_hle_register(0xB29DDF9C, "IoFileMgrForUser", "sceIoDopen",  hle_Dopen);
    psp_hle_register(0xE3EB004C, "IoFileMgrForUser", "sceIoDread",  hle_Dread);
    psp_hle_register(0xEB092469, "IoFileMgrForUser", "sceIoDclose", hle_Dclose);
}
