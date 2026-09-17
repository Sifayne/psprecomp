/* PSP savedata: file operations and an input-driven utility session.
 * Secure modes still store plaintext; encryption is a separate capability.
 * Interactive requests never write or delete before explicit confirmation. */
#include "psprecomp/hle.h"
#include "psprecomp/cpu.h"
#include "psprecomp/os.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <sys/statvfs.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#else
#include <direct.h>
#include <io.h>
#include <fcntl.h>
#include <share.h>
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif
#endif

#define PSP_UTILITY_DIALOG_NONE 0
#define PSP_UTILITY_DIALOG_INIT 1
#define PSP_UTILITY_DIALOG_VISIBLE 2
#define PSP_UTILITY_DIALOG_QUIT 3
#define PSP_UTILITY_DIALOG_FINISHED 4
static int g_savedata_state, g_savedata_done, g_savedata_interactive;
static uint32_t g_savedata_param;
static unsigned char sd_request[1536];
static int sd_request_valid(void);
static int sd_io_error;

/* PSPRECOMP_SAVEDATA_LOG=1 narrates InitStart's parameter block: mode,
 * game/save/file names and buffer sizes. That is how the modes a title
 * actually uses get measured rather than enumerated. */
static int savedata_log_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("PSPRECOMP_SAVEDATA_LOG");
        on = (v && *v && *v != '0');
    }
    return on;
}

/* Offsets into SceUtilitySavedataParam; the layout is described in the M4
 * section below. */
#define SD_MODE      48u
#define SD_BIND      52u
#define SD_OVERWRITE 56u
#define SD_GAMENAME  60u
#define SD_SAVENAME  76u
#define SD_SAVENAMELIST 96u
#define SD_FILENAME 100u
#define SD_DATABUF  116u
#define SD_DATABUFSZ 120u
#define SD_DATASZ   124u
#define SD_RESULT    28u
#define SD_SFO_TITLE 128u
#define SD_SFO_SAVETITLE 256u
#define SD_SFO_DETAIL 384u
#define SD_SFO_PARENTAL 1408u
#define SD_ICON0 1412u
#define SD_ICON1 1428u
#define SD_PIC1  1444u
#define SD_SND0  1460u
#define SD_FOCUS 1480u
#define SD_MSFREE 1488u
#define SD_MSDATA 1492u
#define SD_UTILDATA 1496u
#define SD_IDLIST 1524u
#define SD_FILELIST 1528u
#define SD_SIZEINFO 1532u

static uint32_t sd_optional(uint32_t p, uint32_t off) {
    return psp_read32(p) >= off+4 ? psp_read32(p+off) : 0;
}

static void savedata_log(uint32_t param) {
    if (!savedata_log_on()) return;
    char game[14], save[21], file[14];
    for (int i = 0; i < 13; i++) game[i] = (char)psp_read8(param + SD_GAMENAME + (uint32_t)i);
    game[13] = '\0';
    for (int i = 0; i < 20; i++) save[i] = (char)psp_read8(param + SD_SAVENAME + (uint32_t)i);
    save[20] = '\0';
    for (int i = 0; i < 13; i++) file[i] = (char)psp_read8(param + SD_FILENAME + (uint32_t)i);
    file[13] = '\0';
    fprintf(stderr, "savedata: mode=%u game=%.13s save=%.20s file=%.13s databuf=%08X bufsz=%u datasz=%u\n",
            psp_read32(param + SD_MODE), game, save, file,
            psp_read32(param + SD_DATABUF),
            psp_read32(param + SD_DATABUFSZ), psp_read32(param + SD_DATASZ));
}

/* ---- M4: savedata data paths ------------------------------------------------
 *
 * Offsets into SceUtilitySavedataParam, from PSPSDK's psputility_savedata.h
 * (BSD): a 48-byte pspUtilityDialogCommon (result at 28), then mode at 48.
 * pspautotests' savedata suite carries the same layout and pins, per mode,
 * the result code, every field the dialog touches, and which files exist
 * afterwards -- that is the whole contract below.
 *
 * Layout on the card is hardware's: ms0:/PSP/SAVEDATA/<game><save>/ holds
 * the data file under param.fileName, PARAM.SFO, and the icon/sound files.
 * The game name and save name concatenate with no separator (TEST99901 +
 * ABC, per checkpointExists). Secure modes store plaintext and round-trip
 * it: without Kirk PGD there is no honest ciphertext, and bytes the caller
 * gets back are worth more than bytes shaped like hardware's.
 *
 * Result codes (values from PPSSPP's ErrorCodes.h, meanings confirmed by
 * the suite): 0 success; LOAD_NO_DATA when no save dir matches at all;
 * LOAD_FILE_NOT_FOUND when the dir is there but the data file is not;
 * RW_FILE_NOT_FOUND / RW_NO_DATA are the read/write family's pair for the
 * same two situations; DELETE_NO_DATA when a list delete matches nothing.
 * Interactive results are written only after a decision or an error.
 * Secure-file classification is tracked in memory; free space is the host's.
 */

/* Result codes the modes below can produce. */
#define SD_OK              0u
#define SD_LOAD_NO_DATA    0x80110307u
#define SD_LOAD_FILE       0x80110309u
#define SD_RW_NO_DATA      0x80110327u
#define SD_RW_FILE         0x80110329u
#define SD_DELETE_NO_DATA  0x80110347u
#define SD_LOAD_BROKEN     0x80110306u
#define SD_RW_BROKEN       0x80110326u
#define SD_LOAD_ACCESS     0x80110305u   /* read failed */
#define SD_SAVE_ACCESS     0x80110385u   /* write or list failed */
#define SD_DELETE_ACCESS   0x80110345u
#define SD_ERASE_ACCESS    0x80110325u
#define SD_BAD_PARAM       0x80110004u   /* rejected parameter block */
#define SD_BUSY            0x80110001u   /* a utility is already running */
/* Interactive outcomes written to the result word: not PSP status codes. */
#define SD_RESULT_CANCEL   1u
#define SD_RESULT_ABORT    2u            /* scripted responses ran out or mismatched */

/* Modes (SceUtilitySavedataParam2 in the suite's shared.h). */
#define SD_AUTOLOAD   0u
#define SD_AUTOSAVE   1u
#define SD_LOAD       2u
#define SD_SAVE       3u
#define SD_LISTLOAD   4u
#define SD_LISTSAVE   5u
#define SD_LISTDELETE 6u
#define SD_LISTALLDEL 7u
#define SD_SIZES      8u
#define SD_AUTODELETE 9u
#define SD_DELETE    10u
#define SD_LIST      11u
#define SD_FILES     12u
#define SD_MAKEDATA  14u
#define SD_READDATA  16u
#define SD_WRITEDATA 18u
#define SD_ERASE     20u
#define SD_DELETEDATA 21u
#define SD_GETSIZE   22u
/* Secure twins, stored plaintext (see above): */
#define SD_MAKEDATASECURE 13u
#define SD_READDATASECURE 15u
#define SD_WRITEDATASECURE 17u
#define SD_ERASESECURE 19u

static void sd_getstr(uint32_t param, uint32_t off, uint32_t max, char *dst, size_t cap) {
    uint32_t i;
    for (i = 0; i < max && i + 1 < cap; i++) {
        char c = (char)psp_read8(param + off + i);
        if (!c) break;
        dst[i] = c;
    }
    dst[i] = '\0';
}

/* ms0:/PSP/SAVEDATA/<game><save> for a candidate save name. */
static void sd_dir(const char *game, const char *save, char *out, size_t cap) {
    snprintf(out, cap, "ms0:/PSP/SAVEDATA/%s%s", game, save);
}

static int sd_exists(const char *guest) {
    uint64_t size; int is_dir;
    if (psp_io_path_info(guest, &size, &is_dir) != 0) return 0;
    return 1;
}

static uint64_t sd_fsize(const char *guest) {
    uint64_t size = 0; int is_dir = 0;
    if (psp_io_path_info(guest, &size, &is_dir) != 0 || is_dir) return 0;
    return size;
}

/* A save dir holding anything but PARAM.SFO and the icon/sound sidecars.
 * ERASE keys off this: SFO-only fixtures end RW_FILE_NOT_FOUND. (DELETE
 * itself takes any existing dir -- the suite deletes SFO-only fixtures
 * with result 0.) */
static int sd_has_data(const char *dir) {
    char names[256][64];
    int n = psp_io_list_names(dir, names, 256);
    if (n < 0) return 0;
    for (int i = 0; i < n; i++) {
        if (!strcmp(names[i], "PARAM.SFO") ||
            !strcmp(names[i], "ICON0.PNG") || !strcmp(names[i], "ICON1.PMF") ||
            !strcmp(names[i], "PIC1.PNG") || !strcmp(names[i], "SND0.AT3"))
            continue;
        return 1;
    }
    return 0;
}

/* A save without PARAM.SFO is broken: reads through it fail even when the
 * data file is there (the suite's broken-2 fixtures), and FILES reports
 * RW_DATA_BROKEN with an empty listing. */
static int sd_has_sfo(const char *dir) {
    char guest[512];
    snprintf(guest, sizeof guest, "%s/PARAM.SFO", dir);
    return sd_exists(guest);
}

/* Read a whole host file into guest memory, up to cap bytes. Returns bytes
 * placed, or -1 when missing. */
static int64_t sd_read_file(const char *guest, uint32_t dst, uint32_t cap) {
    char host[1024];
    psp_io_host_path(guest, host, sizeof host);
    FILE *f = fopen(host, "rb");
    if (!f) return -1;
    uint32_t n = 0;
    int c;
    if (cap && !psp_mem_ptr(dst, cap)) { fclose(f); return -1; }
    while (n < cap && (c = fgetc(f)) != EOF) psp_write8(dst + n++, (uint8_t)c);
    int bad = ferror(f);
    if (fclose(f)) bad = 1;
    return bad ? -1 : (int64_t)n;
}

/* Write cap bytes of guest memory to a host file, making the parent chain
 * (not the leaf: mkdir_parents would happily create the filename as a
 * directory). Cap 0 still creates the (empty) file. */
static void sd_make_parents(const char *guest) {
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", guest);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        if (dir[0]) psp_io_mkdir_all(dir);
    }
}

static void sd_write_file(const char *guest, uint32_t src, uint32_t cap) {
    sd_make_parents(guest);
    char host[1024];
    psp_io_host_path(guest, host, sizeof host);
    const void *bytes = cap ? psp_mem_ptr(src, cap) : NULL;
    if (cap && !bytes) { sd_io_error = 1; return; }
    FILE *f = fopen(host, "wb");
    if (!f) { sd_io_error = 1; return; }
    if (cap && fwrite(bytes, 1, cap, f) != cap) sd_io_error = 1;
    if (fflush(f)) sd_io_error = 1;
#ifndef _WIN32
    if (fsync(fileno(f))) sd_io_error = 1;
#endif
    if (fclose(f)) sd_io_error = 1;
}

/* Minimal valid PARAM.SFO: the VSH-facing fields plus the directory name.
 * Nothing here reads it back -- the layout is for anything outside that
 * lists the card -- so minimal and well-formed beats complete and guessed. */
static void sd_write_sfo(const char *dir, const char *dirname, uint32_t param) {
    char title[129], savetitle[129], detail[1025];
    sd_getstr(param, SD_SFO_TITLE, 0x80, title, sizeof title);
    sd_getstr(param, SD_SFO_SAVETITLE, 0x80, savetitle, sizeof savetitle);
    sd_getstr(param, SD_SFO_DETAIL, 0x400, detail, sizeof detail);
    uint32_t parental = psp_read32(param + SD_SFO_PARENTAL);

    static const char k0[] = "CATEGORY", k1[] = "SAVEDATA_DIRECTORY",
                      k2[] = "PARENTAL_LEVEL", k3[] = "TITLE",
                      k4[] = "SAVEDATA_TITLE", k5[] = "SAVEDATA_DETAIL";
    const char ms[] = "MS";
    struct ent { const char *key; uint16_t fmt; const char *s; uint32_t v; uint32_t max; };
    struct ent ents[] = {
        { k0, 2, ms, 0, 4 },
        { k1, 2, dirname, 0, 64 },
        { k2, 4, NULL, parental, 4 },
        { k3, 2, title, 0, 128 },
        { k4, 2, savetitle, 0, 128 },
        { k5, 2, detail, 0, 1024 },
    };
    const int n = (int)(sizeof ents / sizeof ents[0]);

    uint32_t klen = 0, dlen = 0;
    for (int i = 0; i < n; i++) {
        klen += (uint32_t)strlen(ents[i].key) + 1;
        dlen = (dlen + 3) & ~3u;
        dlen += (ents[i].max + 3) & ~3u;
    }
    uint32_t kstart = 20 + (uint32_t)n * 16, dstart = (kstart + klen + 3) & ~3u;
    uint32_t total = dstart + dlen;
    uint8_t *buf = (uint8_t *)calloc(1, total ? total : 1);
    if (!buf) { sd_io_error = 1; return; }
#define PUT32(o, v) do { buf[o] = (uint8_t)(v); buf[(o)+1] = (uint8_t)((v) >> 8); \
                         buf[(o)+2] = (uint8_t)((v) >> 16); buf[(o)+3] = (uint8_t)((v) >> 24); } while (0)
#define PUT16(o, v) do { buf[o] = (uint8_t)(v); buf[(o)+1] = (uint8_t)((v) >> 8); } while (0)
    PUT32(0, 0x46535000u);
    PUT32(4, 0x101u);
    PUT32(8, kstart);
    PUT32(12, dstart);
    PUT32(16, (uint32_t)n);
    uint32_t ko = kstart, dout = dstart;
    for (int i = 0; i < n; i++) {
        uint32_t len = ents[i].fmt == 4 ? 4 : (uint32_t)strlen(ents[i].s) + 1;
        uint32_t max = ents[i].max;
        if (len>max) len=max;
        dout = (dout + 3) & ~3u;
        PUT16(20 + (uint32_t)i * 16, ko - kstart);
        PUT16(22 + (uint32_t)i * 16, (uint16_t)(ents[i].fmt | 0x0200u));
        PUT32(24 + (uint32_t)i * 16, len);
        PUT32(28 + (uint32_t)i * 16, max);
        PUT32(32 + (uint32_t)i * 16, dout - dstart);
        memcpy(buf + ko, ents[i].key, strlen(ents[i].key) + 1);
        ko += (uint32_t)strlen(ents[i].key) + 1;
        if (ents[i].fmt == 4) { PUT32(dout, ents[i].v); }
        else memcpy(buf + dout, ents[i].s, len-1);
        dout += (max + 3) & ~3u;
    }
#undef PUT32
#undef PUT16

    char guest[512];
    snprintf(guest, sizeof guest, "%s/PARAM.SFO", dir);
    sd_make_parents(guest);
    char host[1024];
    psp_io_host_path(guest, host, sizeof host);
    FILE *f = fopen(host, "wb");
    if (!f) sd_io_error = 1;
    else {
        if (fwrite(buf, 1, total, f) != total || fflush(f)) sd_io_error = 1;
#ifndef _WIN32
        if (fsync(fileno(f))) sd_io_error = 1;
#endif
        if (fclose(f)) sd_io_error = 1;
    }
    free(buf);
}

/* Icon/sound sidecars: ICON0.PNG, ICON1.PMF, PIC1.PNG, SND0.AT3. Written
 * when the param carries a buffer, skipped when it does not -- the tests
 * pass NULL throughout, the game may not. */
static void sd_write_sidecars(const char *dir, uint32_t param) {
    static const struct { uint32_t off; const char *name; } side[] = {
        { SD_ICON0, "ICON0.PNG" }, { SD_ICON1, "ICON1.PMF" },
        { SD_PIC1, "PIC1.PNG" },   { SD_SND0, "SND0.AT3" },
    };
    for (size_t i = 0; i < sizeof side / sizeof side[0]; i++) {
        uint32_t buf = psp_read32(param + side[i].off);
        uint32_t bsz = psp_read32(param + side[i].off + 4);
        uint32_t sz  = psp_read32(param + side[i].off + 8);
        if (!buf || (!sz && !bsz)) continue;
        char guest[512];
        snprintf(guest, sizeof guest, "%s/%s", dir, side[i].name);
        sd_write_file(guest, buf, sz ? sz : bsz);
    }
}

/* 32KB clusters, after the suite's free/used arithmetic: three files read
 * 96KB, files plus their directory read 128. */
#define SD_CLUSTER 32768u

static uint32_t sd_clusters(uint64_t bytes) {
    uint32_t c = (uint32_t)(bytes / SD_CLUSTER) + (bytes % SD_CLUSTER ? 1 : 0);
    return c ? c : 1;
}

static void sd_write_u32(uint32_t addr, uint32_t v) { if (addr) psp_write32(addr, v); }

static void sd_write_str(uint32_t addr, const char *s, uint32_t max) {
    if (!addr) return;
    uint32_t i;
    for (i = 0; i < max && s[i]; i++) psp_write8(addr + i, (uint8_t)s[i]);
    if (i < max) psp_write8(addr + i, 0);
}

/* "15 GB" shape: GB past a GB, MB past a MB, else KB. */
static void sd_kb_str(uint64_t kb, uint32_t addr) {
    char tmp[16];
    if (kb >= 1048576) snprintf(tmp, sizeof tmp, "%u GB", (unsigned)(kb / 1048576));
    else if (kb >= 1024) snprintf(tmp, sizeof tmp, "%u MB", (unsigned)(kb / 1024));
    else snprintf(tmp, sizeof tmp, "%u KB", (unsigned)kb);
    sd_write_str(addr, tmp, 8);
}

/* Sum of file clusters under a save dir, +1 for the directory itself when
 * asked: msData counts files-plus-dir (4 for three files), utilityData
 * counts files alone (3). Read off sizes.expected, where both appear. */
static uint32_t sd_dir_clusters(const char *dir, int with_dir, uint64_t *bytes_out) {
    char names[256][64];
    int n = psp_io_list_names(dir, names, 256);
    if (n < 0) { if (bytes_out) *bytes_out = 0; return 0; }
    uint32_t clusters = with_dir ? 1 : 0;
    uint64_t bytes = 0;
    for (int i = 0; i < n; i++) {
        char child[512];
        snprintf(child, sizeof child, "%s/%s", dir, names[i]);
        uint64_t sz = 0; int is_dir = 0;
        if (psp_io_path_info(child, &sz, &is_dir) == 0 && !is_dir) {
            clusters += sd_clusters(sz);
            bytes += sz;
        }
    }
    if (bytes_out) *bytes_out = bytes;
    return clusters;
}

/* qsort hook: deterministic enumeration (readdir order is not). */
static int sd_cmp_str(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

/* Secure-created files, by "game+save/filename". The on-card distinction is
 * encryption, which this implementation does not do (secure modes store
 * plaintext); without a record, FILES could not tell a secure-made DATA.BIN
 * from a WRITEDATA-made OTHER.BIN, which is exactly what the fileList and
 * emptyfilename suites pin. In-memory only: across a relaunch everything
 * reads back either way, which is what the game needs. */
#define SD_SECURE_MAX 256
static char sd_secure[SD_SECURE_MAX][80];
static int sd_secure_n;

static void sd_mark_secure(const char *dir, const char *file) {
    char key[80];
    snprintf(key, sizeof key, "%s/%s", dir, file);
    for (int i = 0; i < sd_secure_n; i++)
        if (!strcmp(sd_secure[i], key)) return;
    if (sd_secure_n < SD_SECURE_MAX) {
        snprintf(sd_secure[sd_secure_n], sizeof sd_secure[0], "%s", key);
        sd_secure_n++;
    }
}

static int sd_is_secure(const char *dir, const char *file) {
    char key[80];
    snprintf(key, sizeof key, "%s/%s", dir, file);
    for (int i = 0; i < sd_secure_n; i++)
        if (!strcmp(sd_secure[i], key)) return 1;
    return 0;
}

/* A plaintext overwrite clears the record: what is on the card now reads
 * through either mode. */
static void sd_unmark_secure(const char *dir, const char *file) {
    char key[80];
    snprintf(key, sizeof key, "%s/%s", dir, file);
    for (int i = 0; i < sd_secure_n; i++) {
        if (!strcmp(sd_secure[i], key)) {
            sd_secure_n--;
            snprintf(sd_secure[i], sizeof sd_secure[0], "%s", sd_secure[sd_secure_n]);
            return;
        }
    }
}

/* 0 secure, 1 normal, 2 system. PARAM.SFO is always system; otherwise the
 * creation record decides. */
static int sd_classify(const char *dir, const char *file) {
    if (!strcmp(file, "PARAM.SFO")) return 2;
    return sd_is_secure(dir, file) ? 0 : 1;
}

/* Fill the SIZES-family structs. Free space is the host's (statvfs where
 * available, a big stick otherwise), so free* lines track this machine the
 * way the suite's track a 16GB card -- environmental either way. Used space
 * is measured out of our own files, which is exact. */
static void sd_fill_sizes(uint32_t param, const char *dir) {
    uint32_t msfree = sd_optional(param, SD_MSFREE);
    uint32_t msdata = sd_optional(param, SD_MSDATA);
    uint32_t utild  = sd_optional(param, SD_UTILDATA);
    uint32_t sinfo  = sd_optional(param, SD_SIZEINFO);
    if (!msfree && !msdata && !utild && !sinfo) return;

    uint64_t freebytes = 0;
#ifndef _WIN32
    {
        /* Read-only: no mkdir here (unlike the write paths) -- a size query
         * must not create the tree it measures. Missing tree reads as an
         * empty card on top of the fallback constants. */
        char host[1024];
        psp_io_host_path("ms0:/PSP/SAVEDATA", host, sizeof host);
        struct statvfs sv;
        uint64_t sz = 0; int is_dir = 0;
        if (psp_io_path_info("ms0:/PSP/SAVEDATA", &sz, &is_dir) == 0 && is_dir &&
            statvfs(host, &sv) == 0)
            freebytes = (uint64_t)sv.f_bavail * (uint64_t)sv.f_frsize;
    }
#endif
    uint32_t freecl = freebytes ? (uint32_t)(freebytes / SD_CLUSTER) : 0x7FFFFu;
    uint64_t freekb = freebytes ? freebytes / 1024 : 0xFFFFFBu;
    if (msfree) {
        sd_write_u32(msfree + 0, 0x8000u);
        sd_write_u32(msfree + 4, freecl);
        sd_write_u32(msfree + 8, (uint32_t)freekb);
        sd_kb_str(freekb, msfree + 12);
    }
    /* msData measures one save: its own struct names when set, else the
     * param's (the suite points msData at ABC while asking about ASDF). A
     * missing dir leaves the whole struct alone (not even "0 KB" strings
     * -- the suite's missing-dir checks show every field untouched). */
    if (msdata) {
        char mgame[17], msave[21], mdir[512];
        sd_getstr(msdata, 0, 16, mgame, sizeof mgame);
        sd_getstr(msdata, 16, 20, msave, sizeof msave);
        if (!mgame[0] || !msave[0]) {
            char game[14], save[21];
            sd_getstr(param, SD_GAMENAME, 13, game, sizeof game);
            sd_getstr(param, SD_SAVENAME, 20, save, sizeof save);
            if (!mgame[0]) snprintf(mgame, sizeof mgame, "%s", game);
            if (!msave[0]) snprintf(msave, sizeof msave, "%s", save);
        }
        sd_dir(mgame, msave, mdir, sizeof mdir);
        if (sd_exists(mdir)) {
            uint64_t bytes = 0;
            uint32_t cl = sd_dir_clusters(mdir, 1, &bytes);
            sd_write_u32(msdata + 36, cl);
            sd_write_u32(msdata + 40, cl * 32u);
            sd_kb_str((uint64_t)cl * 32u, msdata + 44);
            sd_write_u32(msdata + 52, cl * 32u);
            sd_kb_str((uint64_t)cl * 32u, msdata + 56);
        }
    }
    if (utild) {
        char names[256][64];
        int n = psp_io_list_names("ms0:/PSP/SAVEDATA", names, 256);
        uint32_t cl = 0;
        if (n > 0) {
            for (int i = 0; i < n; i++) {
                char child[512];
                snprintf(child, sizeof child, "ms0:/PSP/SAVEDATA/%s", names[i]);
                uint64_t bytes = 0;
                cl += sd_dir_clusters(child, 0, &bytes);
            }
        }
        sd_write_u32(utild + 0, cl);
        sd_write_u32(utild + 4, cl * 32u);
        sd_kb_str((uint64_t)cl * 32u, utild + 8);
        sd_write_u32(utild + 16, cl * 32u);
        sd_kb_str((uint64_t)cl * 32u, utild + 20);
    }
    if (sinfo) {
        uint32_t datasz = psp_read32(param + SD_DATASZ);
        sd_write_u32(sinfo + 16, 0x8000u);
        sd_write_u32(sinfo + 20, freecl);
        sd_write_u32(sinfo + 24, (uint32_t)freekb);
        sd_kb_str(freekb, sinfo + 28);
        /* Needed in whole KB for this save; a 16-byte fixture needs none. */
        sd_write_u32(sinfo + 36, datasz / 1024u);
        sd_write_str(sinfo + 40, "", 8);
        sd_write_u32(sinfo + 48, datasz / 1024u);
        sd_write_str(sinfo + 52, "", 8);
        /* Per-file entries mirror FILES below; sizes only, names included.
         * Unpinned by the suite (no entries exist in its size checks) but
         * read by anything sizing a real save. numSecure/numNormal double
         * as capacities here -- there is no separate result count -- so
         * they are read, never written. Entry layout matches sd_do_files
         * (80 bytes). */
        uint32_t maxsec = psp_read32(sinfo + 0), maxnor = psp_read32(sinfo + 4);
        uint32_t psec = psp_read32(sinfo + 8), pnor = psp_read32(sinfo + 12);
        if ((psec || pnor) && dir && sd_exists(dir)) {
            char names[256][64];
            int n = psp_io_list_names(dir, names, 256);
            uint32_t nsec = 0, nnor = 0;
            if (n > 0) {
                qsort(names, (size_t)n, sizeof names[0], sd_cmp_str);
                for (int i = 0; i < n; i++) {
                    char child[512];
                    snprintf(child, sizeof child, "%s/%s", dir, names[i]);
                    uint64_t sz = 0; int is_dir = 0;
                    if (psp_io_path_info(child, &sz, &is_dir) != 0 || is_dir) continue;
                    int cls = sd_classify(dir, names[i]);
                    uint32_t dst = 0;
                    if (cls == 0 && nsec < maxsec && psec) dst = psec + nsec++ * 80u;
                    else if (cls == 2) continue;
                    else if (cls == 1 && nnor < maxnor && pnor) dst = pnor + nnor++ * 80u;
                    else continue;
                    sd_write_u32(dst + 0, 0x21FFu);
                    sd_write_u32(dst + 8, (uint32_t)sz);
                    sd_write_u32(dst + 12, (uint32_t)(sz >> 32));
                    sd_write_str(dst + 64, names[i], 16);
                }
            }
        }
    }
}

/* FILES(12): secure/normal/system buckets by creation record -- files a
 * secure mode made travel secure, WRITEDATA-made travel normal, PARAM.SFO
 * travels system. Secure-creation is tracked because the on-card
 * distinction is encryption, which this implementation does not do. A dir
 * without PARAM.SFO is broken: report RW_DATA_BROKEN with an empty
 * listing, and touch nothing (not even the counts). */
static uint32_t sd_do_files(uint32_t param, const char *dir) {
    uint32_t fl = sd_optional(param, SD_FILELIST);
    char names[256][64];
    int n = psp_io_list_names(dir, names, 256);
    if (n < 0) return SD_RW_NO_DATA;
    if (!sd_has_sfo(dir)) return SD_RW_BROKEN;
    qsort(names, (size_t)n, sizeof names[0], sd_cmp_str);
    if (fl) {
        uint32_t maxsec = psp_read32(fl + 0), maxnor = psp_read32(fl + 4),
                 maxsys = psp_read32(fl + 8);
        uint32_t psec = psp_read32(fl + 24), pnor = psp_read32(fl + 28),
                 psys = psp_read32(fl + 32);
        uint32_t nsec = 0, nnor = 0, nsys = 0;
        for (int i = 0; i < n; i++) {
            char child[512];
            snprintf(child, sizeof child, "%s/%s", dir, names[i]);
            uint64_t sz = 0; int is_dir = 0;
            if (psp_io_path_info(child, &sz, &is_dir) != 0 || is_dir) continue;
            int sys = !strcmp(names[i], "PARAM.SFO");
            int sec = !sys && sd_is_secure(dir, names[i]);
            uint32_t dst = 0;
            if (sys && nsys < maxsys && psys) {
                dst = psys + nsys * 80u; nsys++;
            } else if (sec && nsec < maxsec && psec) {
                dst = psec + nsec * 80u; nsec++;
            } else if (!sys && !sec && nnor < maxnor && pnor) {
                dst = pnor + nnor * 80u; nnor++;
            } else continue;
            /* FileListEntry: st_mode, st_attr, st_size u64, three 16-byte
             * datetimes, name[16] -- 80 bytes. Only mode and name are
             * suite-pinned; size is written for real readers. */
            sd_write_u32(dst + 0, 0x21FFu);
            sd_write_u32(dst + 8, (uint32_t)sz);
            sd_write_u32(dst + 12, (uint32_t)(sz >> 32));
            sd_write_str(dst + 64, names[i], 16);
        }
        sd_write_u32(fl + 12, nsec);
        sd_write_u32(fl + 16, nnor);
        sd_write_u32(fl + 20, nsys);
    }
    return SD_OK;
}

/* LIST(11): one entry per save dir under the game prefix, names reported
 * bare (TEST99901ABC reads ABC). Sorted, capped at maxCount. */
static uint32_t sd_do_list(uint32_t param, const char *game) {
    uint32_t il = sd_optional(param, SD_IDLIST);
    char names[256][64];
    int n = psp_io_list_names("ms0:/PSP/SAVEDATA", names, 256);
    if (n < 0 || !il) return SD_OK;
    qsort(names, (size_t)n, sizeof names[0], sd_cmp_str);
    size_t glen = strlen(game);
    uint32_t maxc = psp_read32(il + 0), pent = psp_read32(il + 8);
    uint32_t count = 0;
    for (int i = 0; i < n && count < maxc; i++) {
        if (strncmp(names[i], game, glen) != 0) continue;
        if (pent) {
            /* IdListEntry: st_mode, three 16-byte datetimes, name[20] --
             * 72 bytes. Mode 0x11FF and the bare save name are suite-pinned. */
            uint32_t dst = pent + count * 72u;
            sd_write_u32(dst + 0, 0x11FFu);
            sd_write_str(dst + 52, names[i] + glen, 20);
        }
        count++;
    }
    sd_write_u32(il + 4, count);
    return SD_OK;
}

/* Write the data file, SFO and sidecars for a save-shaped mode. Empty
 * fileName writes everything but the data file (the suite's empty-filename
 * trials pin exactly that: dir plus PARAM.SFO, result 0). */
static void sd_write_save(uint32_t param, const char *dir, const char *file) {
    char guest[512], leaf[64];
    snprintf(leaf, sizeof leaf, "%s", file);
    if (leaf[0]) {
        snprintf(guest, sizeof guest, "%s/%s", dir, leaf);
        uint32_t buf = psp_read32(param + SD_DATABUF);
        uint32_t sz = psp_read32(param + SD_DATASZ);
        sd_write_file(guest, buf, (buf && sz) ? sz : 0);
    } else {
        psp_io_mkdir_all(dir);
    }
    const char *slash = strrchr(dir, '/');
    sd_write_sfo(dir, slash ? slash + 1 : dir, param);
    sd_write_sidecars(dir, param);
}

#include "savedata_io.h"

/* Execute on the guest thread after a decision, or for a noninteractive mode. */
static uint32_t sd_do_mode(uint32_t param) {
    char game[14], save[21], file[14];
    sd_getstr(param, SD_GAMENAME, 13, game, sizeof game);
    sd_getstr(param, SD_SAVENAME, 20, save, sizeof save);
    sd_getstr(param, SD_FILENAME, 13, file, sizeof file);
    uint32_t mode = psp_read32(param + SD_MODE);
    uint32_t buf = psp_read32(param + SD_DATABUF);
    uint32_t bufsz = psp_read32(param + SD_DATABUFSZ);
    uint32_t datasz = psp_read32(param + SD_DATASZ);
    char dir[512], guest[512];
    char cand[21];
    sd_io_error = 0;

    switch (mode) {
    case SD_SAVE:
    case SD_AUTOSAVE:
    case SD_LISTSAVE:
    case SD_MAKEDATA:
    case SD_MAKEDATASECURE: {
        /* Target: param.saveName when it names a free slot, else the first
         * free list entry, else the first entry (AUTOSAVE flow, matching the
         * suite: ABC free beats F1/M2/L3). */
        const char *target = save;
        sd_dir(game, save, dir, sizeof dir);
        if (mode == SD_AUTOSAVE && save[0] && sd_exists(dir)) {
            uint32_t list = psp_read32(param + SD_SAVENAMELIST);
            target = NULL;
            if (list) {
                for (int i = 0; i < 100; i++) {
                    if ((uint64_t)list+(uint32_t)i*20u>UINT32_MAX || !psp_mem_ptr(list+(uint32_t)i*20u,20)) return SD_SAVE_ACCESS;
                    sd_getstr(list + (uint32_t)i * 20u, 0, 20, cand, sizeof cand);
                    if (!cand[0]) break;
                    if (!psp_mem_ptr(list+(uint32_t)i*20u,20) || !sd_component(cand,0) || strlen(cand)>19) return SD_SAVE_ACCESS;
                    sd_dir(game, cand, dir, sizeof dir);
                    if (!sd_exists(dir)) { target = cand; break; }
                }
                if (!target) {
                    sd_getstr(list, 0, 20, cand, sizeof cand);
                    if (!sd_component(cand,1) || strlen(cand)>19) return SD_SAVE_ACCESS;
                    if (cand[0]) { target = cand; sd_dir(game, cand, dir, sizeof dir); }
                    else if (!save[0]) return SD_RW_NO_DATA;
                    else { target = save; sd_dir(game, save, dir, sizeof dir); }
                }
            } else if (!save[0]) return SD_RW_NO_DATA;
        }
        if (!target) { target = save; sd_dir(game, save, dir, sizeof dir); }
        sd_transaction_write(param, dir, file);
        if (sd_io_error) return SD_SAVE_ACCESS;
        if (file[0]) {
            if (mode == SD_MAKEDATASECURE) sd_mark_secure(dir, file);
            else sd_unmark_secure(dir, file);
        }
        return sd_io_error ? SD_SAVE_ACCESS : SD_OK;
    }

    /* The load family reads saveName directly -- an empty name denotes the
     * bare game dir, and the list plays no role on these paths. Missing dir
     * is NO_DATA, SFO-less is BROKEN, missing file is FILE_NOT_FOUND.
     * Interactive selection has already written the chosen saveName. */
    case SD_LOAD:
    case SD_LISTLOAD:
    case SD_AUTOLOAD: {
        int rc;
        sd_dir(game, save, dir, sizeof dir);
        if (!sd_exists(dir)) rc = SD_LOAD_NO_DATA;
        else if (!sd_has_sfo(dir)) rc = SD_LOAD_BROKEN;
        else if (!file[0]) rc = SD_OK;
        else {
            snprintf(guest, sizeof guest, "%s/%s", dir, file);
            if (!sd_exists(guest)) rc = SD_LOAD_FILE;
            else {
                if (buf && bufsz) {
                    int64_t n = sd_read_file(guest, buf, bufsz);
                    if (n < 0) return SD_LOAD_ACCESS;
                    psp_write32(param + SD_DATASZ, (uint32_t)n);
                } else {
                    psp_write32(param + SD_DATASZ, (uint32_t)sd_fsize(guest));
                }
                rc = SD_OK;
            }
        }
        return rc;
    }

    case SD_DELETE:
    case SD_AUTODELETE:
        sd_dir(game, save, dir, sizeof dir);
        if (!sd_exists(dir)) return SD_DELETE_NO_DATA;
        return sd_remove_tree(dir,0) == 0 ? SD_OK : SD_DELETE_ACCESS;

    case SD_DELETEDATA:
        sd_dir(game, save, dir, sizeof dir);
        if (!sd_exists(dir)) return SD_DELETE_NO_DATA;
        return sd_remove_tree(dir,0) == 0 ? SD_OK : SD_DELETE_ACCESS;

    case SD_ERASE:
    case SD_ERASESECURE: {
        /* The named data file goes, PARAM.SFO and the dir stay (the suite's
         * erase blocks list a lone System:PARAM.SFO afterwards). With no
         * data file at all there is nothing to erase: RW_FILE_NOT_FOUND,
         * which is what SFO-only fixtures report. */
        sd_dir(game, save, dir, sizeof dir);
        if (!sd_exists(dir)) return SD_RW_NO_DATA;
        if (!sd_has_sfo(dir)) return SD_RW_BROKEN;
        if (!sd_has_data(dir)) return SD_RW_FILE;
        const char *df = file[0] ? file : "DATA.BIN";
        snprintf(guest, sizeof guest, "%s/%s", dir, df);
        if (sd_exists(guest) && sd_remove_tree(guest,0)) return SD_ERASE_ACCESS;
        return SD_OK;
    }

    case SD_READDATA:
    case SD_READDATASECURE: {
        int secmode = (mode == SD_READDATASECURE);
        /* An empty filename reads nothing and succeeds (the emptyfilename
         * trials leave loaddata untouched with result 0); a named file
         * goes through the dir/file/broken checks. */
        if (!file[0]) return SD_OK;
        sd_dir(game, save, dir, sizeof dir);
        if (!sd_exists(dir)) return SD_RW_NO_DATA;
        if (!sd_has_sfo(dir)) return SD_RW_BROKEN;
        snprintf(guest, sizeof guest, "%s/%s", dir, file);
        /* A secure-made file that is gone reads as FILE_NOT_FOUND; a file
         * never made at all reads as NO_DATA. The suite removes DATA.BIN
         * from one fixture and never creates it in the other. */
        if (!sd_exists(guest))
            return sd_is_secure(dir, file) ? SD_RW_FILE : SD_RW_NO_DATA;
        /* Secure-made files only open through secure modes and vice versa:
         * the suite reads both pairings and gets RW_DATA_BROKEN for each. */
        if (sd_is_secure(dir, file) != secmode) return SD_RW_BROKEN;
        if (buf && bufsz) {
            int64_t n = sd_read_file(guest, buf, bufsz);
            if (n<0) return SD_ERASE_ACCESS;
            psp_write32(param + SD_DATASZ, (uint32_t)n);
        } else {
            psp_write32(param + SD_DATASZ, (uint32_t)sd_fsize(guest));
        }
        return SD_OK;
    }

    case SD_WRITEDATA:
    case SD_WRITEDATASECURE:
        if (!file[0]) return SD_RW_NO_DATA;
        sd_dir(game, save, dir, sizeof dir);
        if (!sd_exists(dir)) return SD_RW_NO_DATA;
        snprintf(guest, sizeof guest, "%s/%s", dir, file);
        sd_write_file(guest,buf,datasz);
        if (sd_io_error) return SD_ERASE_ACCESS;
        if (mode == SD_WRITEDATASECURE) sd_mark_secure(dir, file);
        else sd_unmark_secure(dir, file);
        return SD_OK;

    case SD_SIZES:
    case SD_GETSIZE: {
        char sizedir[512];
        sd_dir(game, save, sizedir, sizeof sizedir);
        if (!sd_exists(sizedir)) sizedir[0] = '\0';
        sd_fill_sizes(param, sizedir[0] ? sizedir : NULL);
        return SD_OK;
    }

    case SD_FILES:
        sd_dir(game, save, dir, sizeof dir);
        return sd_do_files(param, dir);

    case SD_LIST:
        return sd_do_list(param, game);

    default:
        return SCE_KERNEL_ERROR_NOTIMPLEMENTED;
    }
}

/* ---- interactive savedata session --------------------------------------- */
static psp_savedata_view sd_view, sd_published;
static psp_os_mutex sd_bridge_lock = PSP_OS_MUTEX_INIT;
static uint64_t sd_serial, sd_ordinal;
static int sd_host, sd_pending;
static struct { uint64_t session, revision; int action, index; } sd_response;
static int (*sd_redraw)(void);
static int sd_status_logged=-1;
static FILE *sd_script;
static int sd_script_checked, sd_script_failed;
static unsigned sd_script_line;

static int sd_interactive(uint32_t mode) {
    return (mode>=SD_LOAD && mode<=SD_LISTALLDEL) || mode==SD_DELETE;
}
static int sd_saving(void) { return sd_view.mode==SD_SAVE || sd_view.mode==SD_LISTSAVE; }
static int sd_loading(void) { return sd_view.mode==SD_LOAD || sd_view.mode==SD_LISTLOAD; }
static int sd_list_mode(void) {
    return sd_view.mode==SD_LISTLOAD || sd_view.mode==SD_LISTSAVE ||
           sd_view.mode==SD_LISTDELETE || sd_view.mode==SD_LISTALLDEL;
}
static void sd_publish(void) {
    sd_view.revision++;
    psp_os_lock(&sd_bridge_lock);
    sd_published=sd_view;
    psp_os_unlock(&sd_bridge_lock);
}
int psp_savedata_snapshot(psp_savedata_view *out) {
    if (!out) return 0;
    psp_os_lock(&sd_bridge_lock);
    int changed=out->revision!=sd_published.revision || out->session!=sd_published.session;
    if (changed) *out=sd_published;
    int result=sd_published.active ? (changed?2:1) : 0;
    psp_os_unlock(&sd_bridge_lock);
    return result;
}
int psp_savedata_respond(uint64_t session, uint64_t revision, int action, int index) {
    psp_os_lock(&sd_bridge_lock);
    int ok=sd_published.active && !sd_pending && session==sd_published.session &&
        revision==sd_published.revision && action>=PSP_SAVEDATA_SELECT && action<=PSP_SAVEDATA_CANCEL;
    if (action==PSP_SAVEDATA_SELECT && (sd_published.stage!=PSP_SAVEDATA_LIST ||
        index<0 || index>=sd_published.count)) ok=0;
    if (ok) {
        sd_response.session=session; sd_response.revision=revision;
        sd_response.action=action; sd_response.index=index; sd_pending=1;
    }
    psp_os_unlock(&sd_bridge_lock);
    return ok;
}
void psp_savedata_set_host(int available) {
    psp_os_lock(&sd_bridge_lock); sd_host=available; psp_os_unlock(&sd_bridge_lock);
}
void psp_savedata_set_redraw(int (*redraw)(void)) {
    psp_os_lock(&sd_bridge_lock); sd_redraw=redraw; psp_os_unlock(&sd_bridge_lock);
}
int psp_savedata_set_script(const char *path) {
    if (sd_script) fclose(sd_script);
    sd_script=NULL; sd_script_checked=1; sd_script_line=0; sd_script_failed=0;
    if (path && *path) {
        sd_script=fopen(path,"r");
        if (!sd_script) { sd_script_failed=1; return -1; }
    }
    return 0;
}
void psp_utility_init(void) {
    g_savedata_state=PSP_UTILITY_DIALOG_NONE;
    g_savedata_param=0; g_savedata_done=0; g_savedata_interactive=0;
    memset(&sd_view,0,sizeof sd_view); sd_view.session=++sd_serial;
    sd_ordinal=0; sd_secure_n=0;
    /* The dialog host and its redraw hook belong to the host's lifetime,
     * not to a guest reset: they stay registered. */
    psp_os_lock(&sd_bridge_lock); sd_pending=0; psp_os_unlock(&sd_bridge_lock);
    sd_status_logged=-1;
    psp_savedata_set_script(NULL); sd_script_checked=0;
    sd_publish();
}
static void sd_finish(uint32_t result) {
    sd_view.result=result; sd_view.active=0;
    psp_write32(g_savedata_param+SD_RESULT,result);
    g_savedata_done=1; g_savedata_state=PSP_UTILITY_DIALOG_QUIT;
    sd_publish();
}
static void sd_outcome(uint32_t result, const char *message) {
    sd_view.result=result;
    sd_view.stage=result ? PSP_SAVEDATA_ERROR : PSP_SAVEDATA_DONE;
    snprintf(sd_view.message,sizeof sd_view.message,"%s",message);
    psp_write32(g_savedata_param+SD_RESULT,result);
    g_savedata_done=1;
    sd_publish();
}
static unsigned sd_le16(const unsigned char *b) { return b[0] | (unsigned)b[1]<<8; }
static uint32_t sd_le32(const unsigned char *b) {
    return b[0] | (uint32_t)b[1]<<8 | (uint32_t)b[2]<<16 | (uint32_t)b[3]<<24;
}
/* Read metadata, including saves written by the old minimal SFO writer. */
static int sd_metadata(const char *dir, psp_savedata_slot *slot) {
    char guest[512],host[1024]; snprintf(guest,sizeof guest,"%s/PARAM.SFO",dir);
    if (sd_plain_path(guest,0)!=1) return -1;
    psp_io_host_path(guest,host,sizeof host);
    FILE *f=fopen(host,"rb"); if (!f) return -1;
    unsigned char b[16384]; size_t n=fread(b,1,sizeof b,f); int bad=ferror(f);
    if (fclose(f)) bad=1;
    if (bad || n<20 || sd_le32(b)!=0x46535000u) return -1;
    uint32_t keys=sd_le32(b+8),data=sd_le32(b+12),count=sd_le32(b+16);
    if (count>(n-20)/16 || keys>=n || data>=n) return -1;
    for (uint32_t i=0;i<count;i++) {
        const unsigned char *e=b+20+i*16;
        uint64_t k=(uint64_t)keys+sd_le16(e), d=(uint64_t)data+sd_le32(e+12);
        uint32_t len=sd_le32(e+4);
        if (k>=n || d>=n || len>n-d || !memchr(b+k,0,n-k)) return -1;
        if (!len || !memchr(b+d,0,len)) continue;
        const char *key=(const char *)b+k, *value=(const char *)b+d;
        if (!strcmp(key,"SAVEDATA_TITLE") || (!strcmp(key,"TITLE") && !slot->title[0]))
            snprintf(slot->title,sizeof slot->title,"%s",value);
        if (!strcmp(key,"SAVEDATA_DETAIL") || !strcmp(key,"DETAIL"))
            snprintf(slot->detail,sizeof slot->detail,"%s",value);
    }
    return 0;
}
static void sd_candidate_dir(int index, char *dir, size_t cap) {
    if (sd_view.mode==SD_LISTALLDEL)
        snprintf(dir,cap,"ms0:/PSP/SAVEDATA/%s",sd_view.slots[index].name);
    else sd_dir(sd_view.game,sd_view.slots[index].name,dir,cap);
}
static int sd_add_candidate(const char *name) {
    const size_t max=sd_view.mode==SD_LISTALLDEL ? 33 : 19;
    if (!sd_component(name,sd_view.mode!=SD_LISTALLDEL) || strlen(name)>max) return -1;
    for (int i=0;i<sd_view.count;i++) if (!strcmp(sd_view.slots[i].name,name)) return 0;
    if (sd_view.count==PSP_SAVEDATA_MAX_SLOTS) return -1;
    psp_savedata_slot *slot=&sd_view.slots[sd_view.count];
    memset(slot,0,sizeof *slot); snprintf(slot->name,sizeof slot->name,"%s",name);
    char dir[512],host[1024],guest[512]; sd_candidate_dir(sd_view.count,dir,sizeof dir);
    int exists=sd_plain_path(dir,1);
    if (exists<0) return -1;
    if (!exists && !sd_saving() && sd_list_mode()) return 0;
    slot->exists=exists;
    if (exists) {
        psp_io_host_path(dir,host,sizeof host);
        struct stat st; if (!stat(host,&st)) slot->modified=(int64_t)st.st_mtime;
        sd_dir_clusters(dir,0,&slot->bytes);
        slot->broken=sd_metadata(dir,slot)!=0;
        snprintf(guest,sizeof guest,"%s/ICON0.PNG",dir);
        if (sd_plain_path(guest,0)==1) psp_io_host_path(guest,slot->icon_path,sizeof slot->icon_path);
        snprintf(guest,sizeof guest,"%s/PIC1.PNG",dir);
        if (sd_plain_path(guest,0)==1) psp_io_host_path(guest,slot->pic1_path,sizeof slot->pic1_path);
    } else {
        snprintf(slot->title,sizeof slot->title,"New save data");
        uint32_t nd=sd_optional(g_savedata_param,1476);
        if (nd && psp_mem_ptr(nd,20)) {
            uint32_t title=psp_read32(nd+16);
            if (title && psp_mem_ptr(title,128)) sd_getstr(title,0,127,slot->title,sizeof slot->title);
        }
    }
    if (!slot->title[0]) snprintf(slot->title,sizeof slot->title,"%s",slot->name[0]?slot->name:"Save data");
    sd_view.count++;
    return 0;
}
static int sd_expand_candidates(void) {
    char names[1024][64]; int n=psp_io_list_names("ms0:/PSP/SAVEDATA",names,1024);
    if (n<0) return 0;
    if (n==1024) fprintf(stderr,"savedata: more than 1023 entries under PSP/SAVEDATA; listing the first 1024\n");
    qsort(names,(size_t)n,sizeof names[0],sd_cmp_str);
    size_t prefix=strlen(sd_view.game);
    for (int i=0;i<n;i++) {
        if (names[i][0]=='.') continue;
        if (sd_view.mode!=SD_LISTALLDEL && strncmp(names[i],sd_view.game,prefix)) continue;
        char dir[512]; snprintf(dir,sizeof dir,"ms0:/PSP/SAVEDATA/%s",names[i]);
        if (sd_plain_path(dir,1)!=1) continue;
        /* A foreign or unrepresentable directory is not one of this game's
         * slots: skip it rather than refusing the whole list. */
        if (sd_add_candidate(names[i]+(sd_view.mode==SD_LISTALLDEL?0:prefix))) continue;
    }
    return 0;
}
static void sd_initial_focus(const char *name) {
    uint32_t focus=sd_optional(g_savedata_param,SD_FOCUS);
    int selected=0,found=0;
    for (int i=0;i<sd_view.count;i++) {
        psp_savedata_slot *s=&sd_view.slots[i]; int match=0;
        switch (focus) {
        case 0: match=!strcmp(name,s->name); break;
        case 1: match=i==0; break;
        case 2: match=1; break;
        case 3: case 4:
            match=s->exists && (!found || (focus==3 ? s->modified>sd_view.slots[selected].modified :
                                                         s->modified<sd_view.slots[selected].modified)); break;
        case 5: case 6: match=s->exists && (!found || focus==6); break;
        case 7: case 8: match=!s->exists && (!found || focus==8); break;
        default: break;
        }
        if (match) { selected=i; found=1; }
    }
    sd_view.selected=selected;
}
static int sd_build_candidates(void) {
    char name[21]; sd_getstr(g_savedata_param,SD_SAVENAME,20,name,sizeof name);
    if (sd_view.mode==SD_LISTALLDEL) { if (sd_expand_candidates()) return -1; }
    else if (sd_list_mode()) {
        uint32_t list=psp_read32(g_savedata_param+SD_SAVENAMELIST);
        if (list) {
            int terminated=0;
            for (unsigned i=0;i<=PSP_SAVEDATA_MAX_SLOTS;i++) {
                uint64_t addr=(uint64_t)list+i*20;
                if (addr>UINT32_MAX || !psp_mem_ptr((uint32_t)addr,20)) return -1;
                char next[21]; sd_getstr((uint32_t)addr,0,20,next,sizeof next);
                if (!*next) { terminated=1; break; }
                if (!strcmp(next,"<>")) { if (sd_expand_candidates()) return -1; }
                else if (sd_add_candidate(next)) return -1;
            }
            if (!terminated) return -1;
        } else if (!strcmp(name,"<>")) { if (sd_expand_candidates()) return -1; }
        else if (sd_add_candidate(name)) return -1;
    } else if (sd_add_candidate(name)) return -1;
    sd_initial_focus(name);
    uint32_t nd=sd_optional(g_savedata_param,1476);
    if (nd && psp_mem_ptr(nd,20)) {
        uint32_t addr=psp_read32(nd),size=psp_read32(nd+8);
        if (size && size<=PSP_SAVEDATA_ICON_MAX && psp_mem_ptr(addr,size)) {
            memcpy(sd_view.new_icon,psp_mem_ptr(addr,size),size); sd_view.new_icon_size=size;
        }
    }
    /* The utility shows artwork behind an unused slot too: the PIC1 the game
     * would write there, taken from the request's own sidecar field. */
    if (sd_saving()) {
        uint32_t addr=psp_read32(g_savedata_param+SD_PIC1),size=psp_read32(g_savedata_param+SD_PIC1+8);
        if (addr && size && size<=PSP_SAVEDATA_PIC1_MAX && psp_mem_ptr(addr,size)) {
            memcpy(sd_view.new_pic1,psp_mem_ptr(addr,size),size); sd_view.new_pic1_size=size;
        }
    }
    return 0;
}
static void sd_confirm(void) {
    psp_savedata_slot *slot=&sd_view.slots[sd_view.selected];
    sd_view.stage=PSP_SAVEDATA_CONFIRM;
    snprintf(sd_view.message,sizeof sd_view.message,"%s",
        sd_saving() ? (slot->exists ? "Overwrite this save data?" : "Save to this slot?") :
        sd_loading() ? "Load this save data?" : "Delete this save data? This cannot be undone.");
    sd_publish();
}
static void sd_execute_selection(void) {
    if (!sd_request_valid()) {
        sd_outcome(SD_BAD_PARAM,"Save request changed while the dialog was open."); return;
    }
    psp_savedata_slot *slot=&sd_view.slots[sd_view.selected];
    char dir[512]; sd_candidate_dir(sd_view.selected,dir,sizeof dir);
    uint32_t result;
    if (sd_saving() || sd_loading()) {
        sd_write_str(g_savedata_param+SD_SAVENAME,slot->name,20);
        if (sd_loading() && slot->broken) result=SD_LOAD_BROKEN;
        else result=sd_do_mode(g_savedata_param);
    } else {
        int lock=sd_card_lock();
        if (lock<0) result=SD_DELETE_ACCESS;
        else {
            result=sd_plain_path(dir,1)==1 ?
                (sd_remove_tree(dir,0)==0 ? 0u : SD_DELETE_ACCESS) : SD_DELETE_NO_DATA;
            sd_card_unlock(lock);
        }
        if (sd_view.mode!=SD_LISTALLDEL) sd_write_str(g_savedata_param+SD_SAVENAME,slot->name,20);
    }
    if (savedata_log_on()) fprintf(stderr,"savedata: session=%llu slot=%s result=%08X\n",
        (unsigned long long)sd_ordinal,slot->name,result);
    const char *message=result ? (sd_loading() ? "Unable to load this save data." :
        sd_saving() ? "Unable to save. Previous data has been retained where possible." : "Unable to delete this save data.") :
        sd_saving() ? "Save completed." : sd_loading() ? "Load completed." : "Save data deleted.";
    sd_outcome(result,message);
}
static void sd_apply_response(int action,int index) {
    if (action==PSP_SAVEDATA_SELECT) {
        if (sd_view.stage==PSP_SAVEDATA_LIST && index>=0 && index<sd_view.count) {
            sd_view.selected=index; sd_publish();
        }
    } else if (sd_view.stage==PSP_SAVEDATA_DONE || sd_view.stage==PSP_SAVEDATA_ERROR) {
        sd_finish(sd_view.result);
    } else if (action==PSP_SAVEDATA_CANCEL) {
        if (sd_view.stage==PSP_SAVEDATA_CONFIRM && sd_list_mode()) {
            sd_view.stage=PSP_SAVEDATA_LIST; sd_view.message[0]=0; sd_publish();
        } else sd_finish(SD_RESULT_CANCEL);
    } else if (action==PSP_SAVEDATA_ACCEPT && sd_view.count) {
        if (sd_view.stage==PSP_SAVEDATA_CONFIRM) sd_execute_selection();
        else if (sd_view.stage==PSP_SAVEDATA_LIST) {
            if (sd_loading() || (sd_saving() && !sd_view.slots[sd_view.selected].exists)) sd_execute_selection();
            else sd_confirm();
        }
    }
}
static void sd_script_step(void) {
    char line[256],game[32],name[80],action[24],extra;
    unsigned long long ordinal; unsigned mode;
    while (fgets(line,sizeof line,sd_script)) {
        sd_script_line++;
        if (line[0]=='#' || line[0]=='\n' || line[0]=='\r') continue;
        if (sscanf(line,"%llu %u %31s %79s %23s %c",&ordinal,&mode,game,name,action,&extra)!=5 ||
            ordinal!=sd_ordinal || mode!=sd_view.mode || strcmp(game,sd_view.game)) break;
        int index=-1;
        const char *wanted=!strcmp(name,"-") ? "" : name;
        for (int i=0;i<sd_view.count;i++) if (!strcmp(wanted,sd_view.slots[i].name)) { index=i; break; }
        if (!strcmp(action,"select") && index>=0 && sd_view.stage==PSP_SAVEDATA_LIST) {
            sd_apply_response(PSP_SAVEDATA_SELECT,index); return;
        }
        if (strcmp(action,"accept") && strcmp(action,"cancel")) break;
        if (sd_view.count && index!=sd_view.selected) break;
        sd_apply_response(!strcmp(action,"accept")?PSP_SAVEDATA_ACCEPT:PSP_SAVEDATA_CANCEL,index);
        return;
    }
    fprintf(stderr,"savedata: script mismatch/EOF at line %u (dialog %llu, mode %u, game %s)\n",
        sd_script_line,(unsigned long long)sd_ordinal,sd_view.mode,sd_view.game);
    sd_script_failed=1;
    sd_finish(g_savedata_done ? sd_view.result : SD_RESULT_ABORT);
}
static int sd_validate(uint32_t p) {
    if (!p || (p&3) || !psp_mem_ptr(p,4)) return 0;
    uint32_t size=psp_read32(p);
    if ((size!=1480 && size!=1500 && size!=1536) || !psp_mem_ptr(p,size)) return 0;
    char game[14],save[21],file[14];
    sd_getstr(p,SD_GAMENAME,13,game,sizeof game); sd_getstr(p,SD_SAVENAME,20,save,sizeof save);
    sd_getstr(p,SD_FILENAME,13,file,sizeof file);
    uint32_t mode=psp_read32(p+SD_MODE);
    char pattern[21]; snprintf(pattern,sizeof pattern,"%s",save);
    if (mode==SD_LIST) for (char *c=pattern;*c;c++) if (*c=='*' || *c=='?') *c='x';
    int wildcard=!strcmp(save,"<>") && (mode==SD_LISTLOAD || mode==SD_LISTSAVE || mode==SD_LISTDELETE);
    if (mode>SD_GETSIZE || !sd_component(game,mode==SD_LISTALLDEL || mode==SD_LIST) ||
        (!sd_component(pattern,1) && !wildcard) || !sd_component(file,1)) return 0;
    uint32_t data=psp_read32(p+SD_DATABUF),cap=psp_read32(p+SD_DATABUFSZ),n=psp_read32(p+SD_DATASZ);
    if (data && cap && !psp_mem_ptr(data,cap)) return 0;
    int writing=mode==SD_AUTOSAVE || mode==SD_SAVE || mode==SD_LISTSAVE ||
        mode==SD_MAKEDATA || mode==SD_MAKEDATASECURE || mode==SD_WRITEDATA || mode==SD_WRITEDATASECURE;
    if (writing && n && (!data || n>cap || !psp_mem_ptr(data,n))) return 0;
    for (unsigned off=SD_ICON0;off<=SD_SND0;off+=16) {
        uint32_t a=psp_read32(p+off),len=psp_read32(p+off+8),space=psp_read32(p+off+4);
        if (a && ((writing && len>space) || (space && !psp_mem_ptr(a,space)))) return 0;
    }
    /* Optional ABI fields must fit both the parameter version and guest RAM. */
    const unsigned offsets[]={SD_MSFREE,SD_MSDATA,SD_UTILDATA,SD_IDLIST,SD_FILELIST,SD_SIZEINFO};
    const unsigned sizes[]={20,64,28,12,36,60};
    for (unsigned i=0;i<sizeof offsets/sizeof offsets[0];i++) {
        uint32_t ptr=sd_optional(p,offsets[i]);
        if (ptr && !psp_mem_ptr(ptr,sizes[i])) return 0;
    }
    return 1;
}
static int sd_request_valid(void) {
    if (!sd_validate(g_savedata_param)) return 0;
    const unsigned char *now=psp_mem_ptr(g_savedata_param,psp_read32(g_savedata_param));
    return !memcmp(now,sd_request,28) &&
        !memcmp(now+32,sd_request+32,psp_read32(g_savedata_param)-32);
}
static void hle_SavedataInitStart(void) {
    /* Shutdown is synchronous here. Some games start the next utility without
     * polling FINISHED/NONE after ShutdownStart; a completed shutdown must not
     * prevent that next request. GetStatus still exposes FINISHED to pollers. */
    if (g_savedata_state==PSP_UTILITY_DIALOG_FINISHED)
        g_savedata_state=PSP_UTILITY_DIALOG_NONE;
    if (g_savedata_state!=PSP_UTILITY_DIALOG_NONE) {
        if (savedata_log_on()) fprintf(stderr,"savedata: InitStart rejected while status=%d\n",g_savedata_state);
        psp_ret(SD_BUSY); return;
    }
    uint32_t p=psp_arg(0);
    if (!sd_validate(p)) {
        if (savedata_log_on() && psp_mem_ptr(p,1480)) {
            fprintf(stderr,"savedata: rejected parameter size=%u\n",psp_read32(p)); savedata_log(p);
            for (unsigned off=SD_ICON0;off<=SD_SND0;off+=16)
                fprintf(stderr,"savedata: sidecar %u buf=%08X cap=%u size=%u\n",off,
                    psp_read32(p+off),psp_read32(p+off+4),psp_read32(p+off+8));
        }
        psp_ret(SD_BAD_PARAM); return;
    }
    g_savedata_param=p; g_savedata_done=0;
    memcpy(sd_request,psp_mem_ptr(p,psp_read32(p)),psp_read32(p));
    uint32_t mode=psp_read32(p+SD_MODE);
    g_savedata_interactive=sd_interactive(mode);
    g_savedata_state=PSP_UTILITY_DIALOG_INIT;
    savedata_log(p);
    if (g_savedata_interactive) {
        memset(&sd_view,0,sizeof sd_view);
        sd_view.session=++sd_serial; sd_view.active=1; sd_view.mode=mode;
        sd_view.confirm_circle=psp_read32(p+8)==0;
        sd_getstr(p,SD_GAMENAME,13,sd_view.game,sizeof sd_view.game);
        sd_getstr(p,SD_SFO_TITLE,128,sd_view.title,sizeof sd_view.title);
        sd_ordinal++;
        if (!sd_script_checked) {
            const char *path=getenv("PSPRECOMP_SAVEDATA_SCRIPT");
            if (psp_savedata_set_script(path)) fprintf(stderr,"savedata: cannot open script %s\n",path);
        }
        sd_view.stage=sd_list_mode()?PSP_SAVEDATA_LIST:PSP_SAVEDATA_CONFIRM;
        if (sd_recover_card() || sd_build_candidates()) {
            sd_outcome(sd_loading()?SD_LOAD_ACCESS:sd_saving()?SD_SAVE_ACCESS:SD_DELETE_ACCESS,
                       "Unable to read the save list. Check the memory stick directory.");
        } else if (!sd_view.count || (!sd_saving() && !sd_view.slots[0].exists && !sd_list_mode())) {
            sd_outcome(sd_loading()?SD_LOAD_NO_DATA:sd_saving()?SD_SAVE_ACCESS:SD_DELETE_NO_DATA,
                       sd_saving()?"No save slot is available.":"No save data is available.");
        } else if (!sd_list_mode()) sd_confirm();
        else sd_publish();
    }
    psp_ret(0);
}
static void hle_SavedataGetStatus(void) {
    int now=g_savedata_state;
    if (savedata_log_on() && now!=sd_status_logged) { fprintf(stderr,"savedata: status=%d\n",now); sd_status_logged=now; }
    if (now==PSP_UTILITY_DIALOG_INIT) g_savedata_state=PSP_UTILITY_DIALOG_VISIBLE;
    else if (now==PSP_UTILITY_DIALOG_FINISHED) g_savedata_state=PSP_UTILITY_DIALOG_NONE;
    psp_ret((uint32_t)now);
}
static void hle_SavedataUpdate(void) {
    if (!g_savedata_param || g_savedata_state!=PSP_UTILITY_DIALOG_VISIBLE) { psp_ret(0); return; }
    if (!g_savedata_interactive) {
        if (!g_savedata_done) {
            uint32_t mode=psp_read32(g_savedata_param+SD_MODE);
            uint32_t result=!sd_request_valid() ? SD_BAD_PARAM :
                (mode==SD_AUTOLOAD && sd_recover_card()) ? SD_LOAD_ACCESS : sd_do_mode(g_savedata_param);
            psp_write32(g_savedata_param+SD_RESULT,result);
            g_savedata_done=1;
        }
        g_savedata_state=PSP_UTILITY_DIALOG_QUIT;
    } else if (sd_view.active) {
        int host,action=0,index=0; int (*redraw)(void);
        psp_os_lock(&sd_bridge_lock);
        host=sd_host; redraw=sd_redraw;
        if (sd_pending && sd_response.session==sd_view.session && sd_response.revision==sd_view.revision) {
            action=sd_response.action; index=sd_response.index;
        }
        sd_pending=0; psp_os_unlock(&sd_bridge_lock);
        if (sd_script_failed) sd_finish(g_savedata_done?sd_view.result:SD_RESULT_ABORT);
        else if (sd_script) sd_script_step();
        else if (!host) {
            fprintf(stderr,"savedata: interactive mode %u cancelled: no dialog host or script\n",sd_view.mode);
            sd_finish(g_savedata_done?sd_view.result:SD_RESULT_CANCEL);
        } else if (action) sd_apply_response(action,index);
        /* A negative redraw means presentation is gone for good; a backend
         * that merely cannot draw from this thread returns 0 and skips. */
        if (redraw && redraw()<0 && sd_view.active) {
            fprintf(stderr,"savedata: dialog presentation unavailable; cancelling\n");
            sd_finish(g_savedata_done?sd_view.result:SD_RESULT_CANCEL);
        }
    }
    psp_ret(0);
}
static void hle_SavedataShutdownStart(void) {
    if (g_savedata_state==PSP_UTILITY_DIALOG_NONE || g_savedata_state==PSP_UTILITY_DIALOG_FINISHED) {
        psp_ret(SD_BUSY); return;
    }
    if (g_savedata_interactive) {
        if (sd_view.active) sd_finish(g_savedata_done?sd_view.result:SD_RESULT_CANCEL);
    } else if (g_savedata_param && !g_savedata_done) {
        psp_write32(g_savedata_param+SD_RESULT,sd_request_valid()?sd_do_mode(g_savedata_param):SD_BAD_PARAM); g_savedata_done=1;
    }
    g_savedata_state=PSP_UTILITY_DIALOG_FINISHED;
    psp_ret(0);
}
void psp_utility_register(void) {
    psp_hle_register(0x50C4CD57,"sceUtility","sceUtilitySavedataInitStart",hle_SavedataInitStart);
    psp_hle_register(0x8874DBE0,"sceUtility","sceUtilitySavedataGetStatus",hle_SavedataGetStatus);
    psp_hle_register(0xD4B95FFB,"sceUtility","sceUtilitySavedataUpdate",hle_SavedataUpdate);
    psp_hle_register(0x9790B33C,"sceUtility","sceUtilitySavedataShutdownStart",hle_SavedataShutdownStart);
}
