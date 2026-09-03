/* psprecomp — the utility dialogs.
 *
 * The savedata dialog is implemented for real (M4): InitStart performs the
 * requested mode against ms0:/PSP/SAVEDATA/<game><save>/ immediately, and
 * the status ratchet reports the conversation's shape. The dialog has no UI
 * -- list modes act directly instead of showing a list -- but every byte it
 * reads and writes is on the host where a relaunch finds it.
 *
 * What is deliberately not here: PGD encryption (secure modes store
 * plaintext and round-trip it; secureversion measures the crypto and stays
 * red), the overwrite refusal (SAVE always writes), and LIST UI. Each is
 * noted where it bites. pspautotests' savedata suite is the oracle: every
 * mode below, every result code, and the file-exists lines come out of its
 * .expected files, and the [r]/[x] prefix is timing noise as everywhere.
 */

#include "psprecomp/hle.h"
#include "psprecomp/cpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/statvfs.h>
#endif
#include <stdlib.h>
#include <string.h>

/* PspUtilityDialogState, from PSPSDK's psputility.h -- the values a caller
 * compares GetStatus against. NONE is "no dialog is currently active" and QUIT
 * is "the dialog has been cancelled and should be shut down"; those two are the
 * only ones a dialog that never appears can honestly be in. INIT, VISIBLE and
 * FINISHED are named here because the enum is only half an answer without them.
 * https://pspdev.github.io/pspsdk/psputility_8h_source.html */
#define PSP_UTILITY_DIALOG_NONE     0
#define PSP_UTILITY_DIALOG_INIT     1
#define PSP_UTILITY_DIALOG_VISIBLE  2
#define PSP_UTILITY_DIALOG_QUIT     3
#define PSP_UTILITY_DIALOG_FINISHED 4

static int g_savedata_state;
static uint32_t g_savedata_param;
static int g_savedata_done;

void psp_utility_init(void) {
    g_savedata_state = PSP_UTILITY_DIALOG_NONE;
    g_savedata_param = 0;
    g_savedata_done = 0;
}

/* Said once. A title that offers to load a save on every screen would otherwise
 * repeat this for as long as it runs. */
static void no_savedata(void) {
    static int said;
    if (!said++)
        fprintf(stderr,
            "psprecomp: sceUtilitySavedata runs headless: list modes act\n"
            "  directly instead of showing a list, and secure modes store\n"
            "  plaintext. Saves load and store; nothing is shown.\n");
}

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

/* Offsets into SceUtilitySavedataParam, from PSPSDK's psputility_savedata.h
 * (BSD): 48-byte pspUtilityDialogCommon (result at 28), then mode at 48. */
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
 * base.result is written on every InitStart, including success -- the
 * suite reads it back unconditionally.
 *
 * Deliberate approximations, each load-bearing somewhere and each stated:
 * SAVE always writes (the overwrite refusal has no oracle); LISTLOAD and
 * LISTSAVE act directly on param.saveName instead of showing a list, while
 * LISTDELETE without a UI selection deletes nothing (347, as the automated
 * hardware run reports); file categories go by creation record
 * (secure-made files travel secure, PARAM.SFO system, the rest normal --
 * the real distinction is encryption, which is the paragraph above);
 * neededKB is floor(dataSize/1024); free space is the host's, so free*
 * lines track this machine, not a 16GB stick; LISTALLDELETE reports how
 * many data-having saves it removed. */

/* Result codes the modes below can produce. */
#define SD_OK              0u
#define SD_LOAD_NO_DATA    0x80110307u
#define SD_LOAD_FILE       0x80110309u
#define SD_RW_NO_DATA      0x80110327u
#define SD_RW_FILE         0x80110329u
#define SD_DELETE_NO_DATA  0x80110347u
#define SD_LOAD_BROKEN     0x80110306u
#define SD_RW_BROKEN       0x80110326u

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
    while (n < cap && (c = fgetc(f)) != EOF) psp_write8(dst + n++, (uint8_t)c);
    fclose(f);
    return (int64_t)n;
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
    FILE *f = fopen(host, "wb");
    if (!f) return;
    for (uint32_t i = 0; i < cap; i++) fputc(psp_read8(src + i), f);
    fclose(f);
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
                      k4[] = "SAVEDATA_TITLE", k5[] = "DETAIL";
    const char ms[] = "MS";
    struct ent { const char *key; uint16_t fmt; const char *s; uint32_t v; uint32_t max; };
    struct ent ents[] = {
        { k0, 2, ms, 0, 4 },
        { k1, 2, dirname, 0, 32 },
        { k2, 4, NULL, parental, 4 },
        { k3, 2, title, 0, 128 },
        { k4, 2, savetitle, 0, 128 },
        { k5, 2, detail, 0, 1024 },
    };
    const int n = (int)(sizeof ents / sizeof ents[0]);

    uint32_t klen = 0, dlen = 0;
    for (int i = 0; i < n; i++) {
        klen += (uint32_t)strlen(ents[i].key) + 1;
        uint32_t len = ents[i].fmt == 4 ? 4 : (uint32_t)strlen(ents[i].s) + 1;
        dlen = (dlen + 3) & ~3u;
        dlen += (len + 3) & ~3u;
    }
    uint32_t kstart = 20 + (uint32_t)n * 16, dstart = kstart + klen;
    uint32_t total = dstart + dlen;
    uint8_t *buf = (uint8_t *)calloc(1, total ? total : 1);
    if (!buf) return;
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
        dout = (dout + 3) & ~3u;
        PUT16(20 + (uint32_t)i * 16, ko - kstart);
        PUT16(22 + (uint32_t)i * 16, ents[i].fmt);
        PUT32(24 + (uint32_t)i * 16, len);
        PUT32(28 + (uint32_t)i * 16, max);
        PUT32(32 + (uint32_t)i * 16, dout - dstart);
        memcpy(buf + ko, ents[i].key, strlen(ents[i].key) + 1);
        ko += (uint32_t)strlen(ents[i].key) + 1;
        if (ents[i].fmt == 4) { PUT32(dout, ents[i].v); }
        else memcpy(buf + dout, ents[i].s, len);
        dout += (len + 3) & ~3u;
    }
#undef PUT32
#undef PUT16

    char guest[512];
    snprintf(guest, sizeof guest, "%s/PARAM.SFO", dir);
    sd_make_parents(guest);
    char host[1024];
    psp_io_host_path(guest, host, sizeof host);
    FILE *f = fopen(host, "wb");
    if (f) { fwrite(buf, 1, total, f); fclose(f); }
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
    uint32_t msfree = psp_read32(param + SD_MSFREE);
    uint32_t msdata = psp_read32(param + SD_MSDATA);
    uint32_t utild  = psp_read32(param + SD_UTILDATA);
    uint32_t sinfo  = psp_read32(param + SD_SIZEINFO);
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
    uint32_t fl = psp_read32(param + SD_FILELIST);
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
    uint32_t il = psp_read32(param + SD_IDLIST);
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

/* Run the mode: read the param block, do the file work, answer the result
 * code. Runs synchronously inside InitStart -- files here are small and
 * instant, and the status ratchet (unchanged) is what paces the caller. */
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
                    sd_getstr(list + (uint32_t)i * 20u, 0, 20, cand, sizeof cand);
                    if (!cand[0]) break;
                    sd_dir(game, cand, dir, sizeof dir);
                    if (!sd_exists(dir)) { target = cand; break; }
                }
                if (!target) {
                    sd_getstr(list, 0, 20, cand, sizeof cand);
                    if (cand[0]) { target = cand; sd_dir(game, cand, dir, sizeof dir); }
                    else if (!save[0]) return SD_RW_NO_DATA;
                    else { target = save; sd_dir(game, save, dir, sizeof dir); }
                }
            } else if (!save[0]) return SD_RW_NO_DATA;
        } else if (!save[0] && mode == SD_LISTSAVE) {
            uint32_t list = psp_read32(param + SD_SAVENAMELIST);
            if (!list) return SD_RW_NO_DATA;
            sd_getstr(list, 0, 20, cand, sizeof cand);
            if (!cand[0]) return SD_RW_NO_DATA;
            target = cand;
            sd_dir(game, cand, dir, sizeof dir);
        }
        if (!target) { target = save; sd_dir(game, save, dir, sizeof dir); }
        sd_write_save(param, dir, file);
        if (file[0]) {
            if (mode == SD_MAKEDATASECURE) sd_mark_secure(dir, file);
            else sd_unmark_secure(dir, file);
        }
        return SD_OK;
    }

    /* The load family reads saveName directly -- an empty name denotes the
     * bare game dir, and the list plays no role on these paths. Missing dir
     * is NO_DATA, SFO-less is BROKEN, missing file is FILE_NOT_FOUND.
     * LISTLOAD shows a list UI: with nothing to show it dismisses to 0
     * instead of failing, but a broken save still reports its real code. */
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
                    psp_write32(param + SD_DATASZ, n > 0 ? (uint32_t)n : 0);
                } else {
                    psp_write32(param + SD_DATASZ, (uint32_t)sd_fsize(guest));
                }
                rc = SD_OK;
            }
        }
        if (mode == SD_LISTLOAD && rc == SD_LOAD_NO_DATA)
            rc = SD_OK;
        return rc;
    }

    case SD_DELETE:
    case SD_AUTODELETE:
        sd_dir(game, save, dir, sizeof dir);
        if (!sd_exists(dir)) return SD_DELETE_NO_DATA;
        psp_io_remove_tree(dir);
        return SD_OK;

    case SD_DELETEDATA:
        sd_dir(game, save, dir, sizeof dir);
        if (!sd_exists(dir)) return SD_DELETE_NO_DATA;
        psp_io_remove_tree(dir);
        return SD_OK;

    case SD_LISTDELETE: {
        /* Deletes every listed save that exists, then reports DELETE_NO_DATA
         * anyway: without a UI no selection exists, and the automated
         * hardware run agrees -- 347 with the save gone (FILES ends empty).
         * Deleting nothing also ends 347. */
        uint32_t list = psp_read32(param + SD_SAVENAMELIST);
        if (list) {
            for (int i = 0; i < 100; i++) {
                sd_getstr(list + (uint32_t)i * 20u, 0, 20, cand, sizeof cand);
                if (!cand[0]) break;
                sd_dir(game, cand, dir, sizeof dir);
                if (sd_exists(dir)) psp_io_remove_tree(dir);
            }
        }
        return SD_DELETE_NO_DATA;
    }

    case SD_LISTALLDEL: {
        char names[256][64];
        int n = psp_io_list_names("ms0:/PSP/SAVEDATA", names, 256);
        int deleted = 0;
        if (n > 0) {
            size_t glen = strlen(game);
            for (int i = 0; i < n; i++) {
                if (game[0] && strncmp(names[i], game, glen) != 0) continue;
                char child[512];
                snprintf(child, sizeof child, "ms0:/PSP/SAVEDATA/%s", names[i]);
                uint64_t sz = 0; int is_dir = 0;
                if (psp_io_path_info(child, &sz, &is_dir) != 0 || !is_dir) continue;
                psp_io_remove_tree(child);
                deleted++;
            }
        }
        return (uint32_t)deleted;
    }

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
        if (sd_exists(guest)) psp_io_remove_tree(guest);
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
            psp_write32(param + SD_DATASZ, n > 0 ? (uint32_t)n : 0);
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
        if (buf && datasz) sd_write_file(guest, buf, datasz);
        else {
            char host[1024];
            psp_io_host_path(guest, host, sizeof host);
            FILE *f = fopen(host, "wb");
            if (f) fclose(f);
        }
        if (mode == SD_WRITEDATASECURE) sd_mark_secure(dir, file);
        else sd_unmark_secure(dir, file);
        return SD_OK;

    case SD_SIZES:
    case SD_GETSIZE: {
        char sizedir[512];
        sd_dir(game, save, sizedir, sizeof sizedir);
        if (!sd_exists(sizedir)) sizedir[0] = '\0';
        sd_fill_sizes(param, sizedir[0] ? sizedir : NULL);
        if (mode == SD_GETSIZE && sizedir[0]) {
            /* GETSIZE also enumerates handled inside sd_fill_sizes. */
        }
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

/* Accepted: the dialog opens (silently). The mode runs on the first Update
 * (or a workless ShutdownStart) -- see hle_SavedataUpdate for why not here.
 * base.result carries the outcome, including success: the suite reads it
 * back unconditionally. */
static void hle_SavedataInitStart(void) {
    no_savedata();
    uint32_t param = psp_arg(0);
    if (!param) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
    savedata_log(param);
    g_savedata_param = param;
    g_savedata_done = 0;
    g_savedata_state = PSP_UTILITY_DIALOG_INIT;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* Reports where the dialog is, then moves it on.
 *
 * The lifecycle is a ratchet rather than a state machine with inputs, and it
 * has to be: the only thing that could drive it here is being asked. A caller
 * polls until the dialog is done with, so a status that only changed when
 * something else changed it would never change at all -- the same trade
 * hle_GetVcount already makes for the scanline counter, and for the same
 * reason.
 *
 * The sequence is hardware's, read off savedata/autosave: INIT, then VISIBLE
 * while the caller drives it, then QUIT to say it is finished with, and after
 * ShutdownStart a FINISHED that settles to NONE. A dialog that does nothing
 * still passes through all of them, which is why they are all here -- stopping
 * at QUIT skipped three of the statuses the tests print. */
static void hle_SavedataGetStatus(void) {
    const int now = g_savedata_state;
    switch (now) {
    case PSP_UTILITY_DIALOG_INIT:     g_savedata_state = PSP_UTILITY_DIALOG_VISIBLE; break;
    case PSP_UTILITY_DIALOG_VISIBLE:  g_savedata_state = PSP_UTILITY_DIALOG_QUIT;    break;
    case PSP_UTILITY_DIALOG_FINISHED: g_savedata_state = PSP_UTILITY_DIALOG_NONE;    break;
    /* QUIT waits for ShutdownStart, and NONE is the resting state. */
    default: break;
    }
    psp_ret((uint32_t)now);
}

/* The caller drives the dialog a frame at a time, and the first drive is
 * when the work happens: files land and result/entries are written here,
 * not at InitStart. The suite pins this ordering -- its CHANGE lines come
 * after the Update line, never between InitStart and the first status --
 * which is exactly what doing everything up front gets wrong. ShutdownStart
 * runs anything an Update-less flow skipped, so direct Init-to-Shutdown
 * callers still complete. */
static void hle_SavedataUpdate(void) {
    if (g_savedata_param && !g_savedata_done) {
        g_savedata_done = 1;
        psp_write32(g_savedata_param + SD_RESULT, sd_do_mode(g_savedata_param));
    }
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_SavedataShutdownStart(void) {
    if (g_savedata_param && !g_savedata_done) {
        g_savedata_done = 1;
        psp_write32(g_savedata_param + SD_RESULT, sd_do_mode(g_savedata_param));
    }
    g_savedata_state = PSP_UTILITY_DIALOG_FINISHED;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

void psp_utility_register(void) {
    psp_hle_register(0x50C4CD57, "sceUtility", "sceUtilitySavedataInitStart",
                     hle_SavedataInitStart);
    psp_hle_register(0x8874DBE0, "sceUtility", "sceUtilitySavedataGetStatus",
                     hle_SavedataGetStatus);
    psp_hle_register(0xD4B95FFB, "sceUtility", "sceUtilitySavedataUpdate",
                     hle_SavedataUpdate);
    psp_hle_register(0x9790B33C, "sceUtility", "sceUtilitySavedataShutdownStart",
                     hle_SavedataShutdownStart);
}
