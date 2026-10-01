/* saveprobe -- what the savedata utility writes and answers on a real PSP.
 *
 * Written against PSPSDK (BSD) only. Nothing here needs input: it makes a set
 * of small test saves under the made-up game id PRCP00000, reads each back
 * with the right key, a wrong key and no key, runs the list, files and size
 * queries over them, and copies every file the firmware wrote into the
 * probe's own folder, so one folder holds the whole result.
 *
 * psprecomp stores secure saves as plaintext (src/hle/utility.c:2), so a save
 * made on a PSP cannot be read by a port today. The files here are known
 * plaintext encrypted under known keys and secure versions -- the test data
 * an implementation of the save encryption needs -- and the result codes are
 * the contract src/hle/utility.c:100 transcribes from pspautotests.
 *
 * Version 2 keeps version 1's 67 steps and their titles, so the logs of the
 * two runs line up, and adds after them the steps that the analysis of the
 * first hardware run asked for (hwresults/fw660-run1/findings/saveprobe.md,
 * section 6). The comments name the analysis item each one settles: P1 (the
 * PLAIN save was an AUTOSAVE with an all-zero key) and R1-R9 (the root causes
 * of the differences from psprecomp). Line numbers are psprecomp's at c12701f.
 *
 * The saves stay on the memory stick afterwards, listed in the XMB as
 * "psprecomp saveprobe"; they can be deleted there. */
#include <pspkernel.h>
#include <pspdisplay.h>
#include <psputility.h>

#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include "probe.h"

PSP_MODULE_INFO("saveprobe", PSP_MODULE_USER, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(4096);

#define PROBE_VERSION 2

typedef unsigned int w32;   /* PSPSDK's u32 is uint32_t, a long here, which %X does not take */

#define GAME "PRCP00000"

/* SysMemUserForUser, NID 0x358CA1BB; the stub is in imports.S. */
int sceKernelSetCompiledSdkVersion660(unsigned int version);

/* step() with a count of the steps so far, which names the copies collect()
 * makes. It counts exactly as probe.c does: every step() goes through here. */
static int g_nstep;
#define STEP(...) (g_nstep++, step(__VA_ARGS__))

static w32 crc32(const void *p, int n) {
    const unsigned char *b = p;
    w32 c = 0xFFFFFFFFu;
    for (int i = 0; i < n; i++) {
        c ^= b[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static w32 le32(const unsigned char *p) {
    return p[0] | (w32)p[1] << 8 | (w32)p[2] << 16 | (w32)p[3] << 24;
}

static const unsigned char KEY_A[16] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F };
static const unsigned char KEY_B[16] = {
    0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xFA, 0xF9, 0xF8,
    0xF7, 0xF6, 0xF5, 0xF4, 0xF3, 0xF2, 0xF1, 0xF0 };
static const unsigned char KEY_0[16];

/* A 144x80 RGB PNG (a dark field with a lighter band), for the ICON0 steps. */
static const unsigned char ICON0[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x90, 0x00, 0x00, 0x00, 0x50,
    0x08, 0x02, 0x00, 0x00, 0x00, 0x79, 0xCC, 0x6B, 0x5B, 0x00, 0x00, 0x00,
    0x9E, 0x49, 0x44, 0x41, 0x54, 0x78, 0xDA, 0xED, 0xDA, 0x41, 0x11, 0x00,
    0x20, 0x0C, 0x03, 0xC1, 0x4A, 0x40, 0x02, 0x52, 0x2A, 0x0D, 0x69, 0x48,
    0x43, 0x45, 0x1F, 0x19, 0x76, 0xE6, 0x14, 0x64, 0xBF, 0xA9, 0xB5, 0x5B,
    0x41, 0x95, 0x09, 0x80, 0x09, 0x98, 0x80, 0x01, 0x13, 0x30, 0x01, 0x03,
    0x26, 0x60, 0x02, 0x06, 0x4C, 0xC0, 0x04, 0x0C, 0x98, 0x80, 0x09, 0x18,
    0x30, 0x01, 0x13, 0x30, 0x60, 0x02, 0x26, 0x60, 0xC0, 0x04, 0x4C, 0xC0,
    0x80, 0x09, 0x98, 0x80, 0x01, 0xB3, 0x42, 0x14, 0x58, 0x9F, 0xAB, 0xA0,
    0x80, 0x01, 0x13, 0x30, 0x01, 0x03, 0x26, 0x60, 0x02, 0x06, 0x4C, 0xC0,
    0x04, 0x0C, 0x98, 0x80, 0x09, 0x18, 0x30, 0x01, 0x13, 0x30, 0x60, 0x02,
    0x26, 0x60, 0x7F, 0x80, 0xB9, 0xB5, 0x78, 0x4D, 0x09, 0x98, 0x80, 0x01,
    0x13, 0x30, 0x01, 0x03, 0x26, 0x60, 0x02, 0x06, 0x4C, 0xC0, 0x04, 0x0C,
    0x98, 0x80, 0x09, 0x18, 0x30, 0x01, 0x13, 0x30, 0x60, 0x02, 0x26, 0x60,
    0xC0, 0x04, 0x4C, 0xC0, 0x80, 0x09, 0x98, 0x80, 0x01, 0xB3, 0x02, 0x30,
    0x8D, 0xF5, 0x00, 0xA8, 0x3F, 0xA5, 0xD2, 0xCC, 0x09, 0x6A, 0x02, 0x00,
    0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
};

/* The plaintext every save carries: byte i is (i * 7 + 3) & 0xFF, so any
 * prefix of it is known from its length alone. */
static unsigned char *g_plain;
static unsigned char *g_read;
static unsigned char *g_file;
static unsigned char *g_icon;
#define DATA_MAX 4096               /* dataBufSize of every request but the 64 KB ones */
#define BIG_SIZE (64 * 1024 + 1)
#define BUF_MAX  (80 * 1024)        /* g_plain and g_read; dataBufSize for BIG_SIZE */
#define FILE_MAX (128 * 1024)

/* ---- the utility --------------------------------------------------------- */

static SceUtilitySavedataParam g_p;

static void param_init(int mode, const char *save, const unsigned char *key, int secure_version,
                       int size) {
    memset(&g_p, 0, sizeof g_p);
    g_p.base.size = sizeof g_p;
    g_p.base.language = 1;              /* English */
    g_p.base.buttonSwap = 1;
    g_p.base.graphicsThread = 0x11;
    g_p.base.accessThread = 0x13;
    g_p.base.fontThread = 0x12;
    g_p.base.soundThread = 0x10;
    g_p.mode = mode;
    g_p.overwrite = 1;
    strcpy(g_p.gameName, GAME);
    strcpy(g_p.saveName, save);
    strcpy(g_p.fileName, "DATA.BIN");
    g_p.dataBuf = g_read;
    g_p.dataBufSize = DATA_MAX;
    g_p.dataSize = size;
    strcpy(g_p.sfoParam.title, "psprecomp saveprobe");
    snprintf(g_p.sfoParam.savedataTitle, sizeof g_p.sfoParam.savedataTitle, "%s", save);
    strcpy(g_p.sfoParam.detail, "Test data from tools/hwprobe/saveprobe. Safe to delete.");
    g_p.sfoParam.parentalLevel = 1;
    g_p.focus = PSP_UTILITY_SAVEDATA_FOCUS_LATEST;
    if (key) memcpy(g_p.key, key, 16);
    g_p.secureVersion = secure_version;
}

/* InitStart, then Update until it finishes, then shut it down. Every wait is
 * bounded, so a utility that never finishes costs this step, not the run. */
static int run_utility(void) {
    int r = sceUtilitySavedataInitStart(&g_p);
    if (r < 0) { out("  InitStart = %08X\n", (w32)r); return r; }
    int st = 0, last = -1, shut = 0;
    for (int frame = 0; frame < 1200; frame++) {
        st = sceUtilitySavedataGetStatus();
        if (st != last) { out("  status %d\n", st); last = st; }
        if (st == 2) sceUtilitySavedataUpdate(1);
        else if (st == 3 && !shut) { sceUtilitySavedataShutdownStart(); shut = 1; }
        else if ((st == 4 || st == 0) && shut) break;
        sceDisplayWaitVblankStart();
    }
    if (!shut) out("  did not finish: status %d after 20 s\n", st);
    for (int frame = 0; frame < 120 && sceUtilitySavedataGetStatus() != 0; frame++)
        sceDisplayWaitVblankStart();
    out("  result %08X\n", (w32)g_p.base.result);
    return g_p.base.result;
}

/* One line out of a PARAM.SFO: byte 0 of SAVEDATA_PARAMS (the flags: 0x21 or
 * 0x01 in run 1) and the names in SAVEDATA_FILE_LIST (32-byte entries, name
 * first; run 1 found only the secure files there). */
static int sfo_key_is(const unsigned char *b, int n, w32 off, const char *key) {
    const w32 len = strlen(key) + 1;
    return off + len <= (w32)n && !memcmp(b + off, key, len);
}

static void sfo_summary(const unsigned char *b, int n) {
    if (n < 20 || memcmp(b, "\0PSF", 4)) { out("  sfo: no PSF header\n"); return; }
    const w32 ks = le32(b + 8), ds = le32(b + 12), cnt = le32(b + 16);
    char flags[8] = "absent", list[200] = "absent";
    for (w32 i = 0; i < cnt && 20 + 16 * (i + 1) <= (w32)n; i++) {
        const unsigned char *e = b + 20 + 16 * i;
        const w32 ko = ks + (e[0] | (w32)e[1] << 8), at = ds + le32(e + 12);
        w32 max = le32(e + 8);
        if (at >= (w32)n) continue;
        if (max > (w32)n - at) max = (w32)n - at;
        if (sfo_key_is(b, n, ko, "SAVEDATA_PARAMS") && max > 0) {
            snprintf(flags, sizeof flags, "%02X", b[at]);
        } else if (sfo_key_is(b, n, ko, "SAVEDATA_FILE_LIST")) {
            int len = 0;
            list[0] = '\0';
            for (w32 j = 0; j + 32 <= max; j += 32) {
                if (!b[at + j]) continue;
                char name[14];
                memcpy(name, b + at + j, 13);
                name[13] = '\0';
                len += snprintf(list + len, sizeof list - len, "%s%s", len ? " " : "", name);
                if (len >= (int)sizeof list - 16) break;
            }
            if (!list[0]) snprintf(list, sizeof list, "empty");
        }
    }
    out("  sfo: SAVEDATA_PARAMS flags %s, SAVEDATA_FILE_LIST %s\n", flags, list);
}

/* Saves whose files were copied earlier in this run. */
static char g_copied[64][24];
static int g_ncopied;

/* List the save's directory, log each file, and copy it beside the EBOOT as
 * <save>__<file>. A save copied before in this run (DSEC after its write,
 * KEYPAIR after its rewrite) is copied as <save>_s<step>__<file>, so no copy
 * overwrites another. With `sfo`, PARAM.SFO's flags and file list are logged
 * too; only version 2's steps ask for that, so version 1's lines stay as they
 * were. */
static void collect_sfo(const char *save, int sfo) {
    char dir[96];
    snprintf(dir, sizeof dir, "ms0:/PSP/SAVEDATA/" GAME "%s", save);
    SceUID d = sceIoDopen(dir);
    if (d < 0) { out("  dir %s: open %08X\n", save, (w32)d); return; }
    int again = 0, copied = 0;
    for (int i = 0; i < g_ncopied; i++)
        if (!strcmp(g_copied[i], save)) again = 1;
    char tag[48];
    if (again) snprintf(tag, sizeof tag, "%s_s%d", save, g_nstep);
    else snprintf(tag, sizeof tag, "%s", save);
    SceIoDirent e;
    for (;;) {
        memset(&e, 0, sizeof e);
        if (sceIoDread(d, &e) <= 0) break;
        if (e.d_name[0] == '.') continue;
        char path[400];
        snprintf(path, sizeof path, "%s/%s", dir, e.d_name);
        SceUID f = sceIoOpen(path, PSP_O_RDONLY, 0);
        int n = f >= 0 ? sceIoRead(f, g_file, FILE_MAX) : f;
        if (f >= 0) sceIoClose(f);
        out("  file %-12s size %6d crc %08X", e.d_name, n, n > 0 ? crc32(g_file, n) : 0);
        if (n > 0) {
            out("  first 16:");
            for (int i = 0; i < 16 && i < n; i++) out(" %02X", g_file[i]);
            char name[400];
            snprintf(name, sizeof name, "%s__%s", tag, e.d_name);
            out("  -> %s", name);
            probe_write_file(name, g_file, n);
            copied = 1;
        }
        out("\n");
        if (sfo && n > 0 && !strcmp(e.d_name, "PARAM.SFO")) sfo_summary(g_file, n);
    }
    sceIoDclose(d);
    if (copied && !again && g_ncopied < 64)
        snprintf(g_copied[g_ncopied++], sizeof g_copied[0], "%s", save);
}

static void collect(const char *save) { collect_sfo(save, 0); }

static void check_read_cap(int size, int cap) {
    const int n = g_p.dataSize;
    out("  read back: dataSize %d, matches the plaintext: %s\n", n,
        (n == size && memcmp(g_read, g_plain, size) == 0) ? "yes" :
        (n > 0 && n <= cap) ? "no" : "n/a");
    if (n > 0 && n <= cap && memcmp(g_read, g_plain, n < size ? n : size) != 0) {
        out("  first 16 read:");
        for (int i = 0; i < 16 && i < n; i++) out(" %02X", g_read[i]);
        out("\n");
    }
}

static void check_read(int size) { check_read_cap(size, DATA_MAX); }

/* One save: write it with AUTOSAVE, collect its files, read it back with the
 * right key, a wrong key and no key. */
static void save_and_read(const char *save, const unsigned char *key, int ver, int size) {
    STEP("save %s: AUTOSAVE %d bytes, key %s, secureVersion %d", save, size,
         key == KEY_A ? "A" : key == KEY_B ? "B" : "none", ver);
    param_init(PSP_UTILITY_SAVEDATA_AUTOSAVE, save, key, ver, size);
    memcpy(g_read, g_plain, size);
    run_utility();
    collect(save);

    static const struct { const char *name; const unsigned char *key; } R[] = {
        { "same key", NULL }, { "key B", KEY_B }, { "no key", KEY_0 } };
    for (int i = 0; i < 3; i++) {
        const unsigned char *k = R[i].key ? R[i].key : key;
        if (i == 1 && key == KEY_B) k = KEY_A;
        STEP("load %s: AUTOLOAD with %s", save, i == 1 && key == KEY_B ? "key A" : R[i].name);
        param_init(PSP_UTILITY_SAVEDATA_AUTOLOAD, save, k, ver, 0);
        memset(g_read, 0xEE, DATA_MAX);
        run_utility();
        check_read(size);
    }
}

/* ---- queries ------------------------------------------------------------- */

static void log_sizes(const SceUtilitySavedataMsFreeInfo *fr, const SceUtilitySavedataMsDataInfo *md,
                      const SceUtilitySavedataUsedDataInfo *ud) {
    /* Free space depends on the card, so only its shape is logged. */
    out("  msFree: clusterSize %d, freeClusters %s, freeSpaceKB %s\n",
        fr->clusterSize, fr->freeClusters > 0 ? ">0" : "0", fr->freeSpaceKB > 0 ? ">0" : "0");
    out("  msData: usedClusters %d usedSpaceKB %d '%s' used32KB %d '%s'\n",
        md->info.usedClusters, md->info.usedSpaceKB, md->info.usedSpaceStr,
        md->info.usedSpace32KB, md->info.usedSpace32Str);
    out("  utilityData: usedClusters %d usedSpaceKB %d '%s' used32KB %d '%s'\n",
        ud->usedClusters, ud->usedSpaceKB, ud->usedSpaceStr, ud->usedSpace32KB, ud->usedSpace32Str);
}

static void query_sizes(void) {
    STEP("SIZES for PLAIN (needed space for a save of this shape)");
    static SceUtilitySavedataMsFreeInfo fr;
    static SceUtilitySavedataMsDataInfo md;
    static SceUtilitySavedataUsedDataInfo ud;
    memset(&fr, 0, sizeof fr); memset(&md, 0, sizeof md); memset(&ud, 0, sizeof ud);
    strcpy(md.gameName, GAME); strcpy(md.saveName, "PLAIN");
    param_init(SCE_UTILITY_SAVEDATA_SIZES, "PLAIN", NULL, 0, 256);
    memcpy(g_read, g_plain, 256);
    g_p.msFree = &fr; g_p.msData = &md; g_p.utilityData = &ud;
    run_utility();
    log_sizes(&fr, &md, &ud);
}

static void query_list(const char *pattern, int max) {
    STEP("LIST of " GAME " with saveName '%s'", pattern);
    static SceUtilitySavedataIdListEntry ents[64];
    static SceUtilitySavedataIdListInfo il;
    memset(ents, 0, sizeof ents);
    il.maxCount = max; il.resultCount = -1; il.entries = ents;
    param_init(SCE_UTILITY_SAVEDATA_LIST, pattern, NULL, 0, 0);
    g_p.idList = &il;
    run_utility();
    out("  resultCount %d\n", il.resultCount);
    for (int i = 0; i < il.resultCount && i < max; i++)
        out("  entry st_mode %04X name '%s'\n", (w32)ents[i].st_mode, ents[i].name);
}

static void query_files(const char *save, const unsigned char *key) {
    STEP("FILES of %s", save);
    static SceUtilitySavedataFileListEntry sec[8], nor[8], sys[8];
    static SceUtilitySavedataFileListInfo fl;
    memset(&fl, 0, sizeof fl);
    fl.maxSecureEntries = fl.maxNormalEntries = fl.maxSystemEntries = 8;
    fl.secureEntries = sec; fl.normalEntries = nor; fl.systemEntries = sys;
    param_init(SCE_UTILITY_SAVEDATA_FILES, save, key, 0, 0);
    g_p.fileList = &fl;
    run_utility();
    out("  secure %d normal %d system %d\n", (int)fl.resultNumSecureEntries,
        (int)fl.resultNumNormalEntries, (int)fl.resultNumSystemEntries);
    for (w32 i = 0; i < fl.resultNumSecureEntries && i < 8; i++)
        out("  secure %-12s mode %04X size %d\n", sec[i].name, (w32)sec[i].st_mode, (int)sec[i].st_size);
    for (w32 i = 0; i < fl.resultNumNormalEntries && i < 8; i++)
        out("  normal %-12s mode %04X size %d\n", nor[i].name, (w32)nor[i].st_mode, (int)nor[i].st_size);
    for (w32 i = 0; i < fl.resultNumSystemEntries && i < 8; i++)
        out("  system %-12s mode %04X size %d\n", sys[i].name, (w32)sys[i].st_mode, (int)sys[i].st_size);
}

static void query_getsize(const char *save) {
    STEP("GETSIZE of %s", save);
    /* Room for 8 entries although the request names 1 of each: psprecomp
     * writes 80-byte records into these 24-byte entries (utility.c:587-594),
     * and they must land in the probe's own array. */
    static PspUtilitySavedataSizeEntry se[8], ne[8];
    static PspUtilitySavedataSizeInfo si;
    memset(&si, 0, sizeof si);
    memset(se, 0, sizeof se); memset(ne, 0, sizeof ne);
    si.numSecureEntries = 1; si.secureEntries = se;
    si.numNormalEntries = 1; si.normalEntries = ne;
    strcpy(se[0].name, "DATA.BIN"); se[0].size = 1000;
    strcpy(ne[0].name, "OTHER.BIN"); ne[0].size = 5000;
    param_init(SCE_UTILITY_SAVEDATA_GETSIZE, save, NULL, 0, 0);
    g_p.sizeInfo = &si;
    run_utility();
    out("  sectorSize %d neededKB %d '%s' overwriteKB %d '%s'\n", si.sectorSize,
        si.neededKB, si.neededString, si.overwriteKB, si.overwriteString);
}

/* The data-mode family: MAKEDATA(SECURE), then WRITEDATA(SECURE) into it,
 * READDATA(SECURE) back, and the no-data codes. */
static void data_modes(void) {
    static const struct { const char *save; int make, write, read; const unsigned char *key; } D[] = {
        { "DSEC",   SCE_UTILITY_SAVEDATA_MAKEDATASECURE, SCE_UTILITY_SAVEDATA_WRITEDATASECURE,
                    SCE_UTILITY_SAVEDATA_READDATASECURE, KEY_A },
        { "DPLAIN", SCE_UTILITY_SAVEDATA_MAKEDATA, SCE_UTILITY_SAVEDATA_WRITEDATA,
                    SCE_UTILITY_SAVEDATA_READDATA, KEY_0 },
        { "DPLAINK", SCE_UTILITY_SAVEDATA_MAKEDATA, SCE_UTILITY_SAVEDATA_WRITEDATA,
                    SCE_UTILITY_SAVEDATA_READDATA, KEY_A },
    };
    for (int i = 0; i < 3; i++) {
        STEP("%s: make (mode %d)", D[i].save, D[i].make);
        param_init(D[i].make, D[i].save, D[i].key, 0, 100);
        memcpy(g_read, g_plain, 100);
        run_utility();
        collect(D[i].save);
        STEP("%s: write 100 bytes (mode %d)", D[i].save, D[i].write);
        param_init(D[i].write, D[i].save, D[i].key, 0, 100);
        memcpy(g_read, g_plain, 100);
        run_utility();
        collect(D[i].save);
        STEP("%s: read (mode %d)", D[i].save, D[i].read);
        param_init(D[i].read, D[i].save, D[i].key, 0, 0);
        memset(g_read, 0xEE, DATA_MAX);
        run_utility();
        check_read(100);
    }
    STEP("READDATA of a save that does not exist");
    param_init(SCE_UTILITY_SAVEDATA_READDATA, "NOSUCH", KEY_0, 0, 0);
    run_utility();
    STEP("AUTOLOAD of a save that does not exist");
    param_init(PSP_UTILITY_SAVEDATA_AUTOLOAD, "NOSUCH", KEY_0, 0, 0);
    run_utility();
    STEP("READDATASECURE of a non-secure save (DPLAIN)");
    param_init(SCE_UTILITY_SAVEDATA_READDATASECURE, "DPLAIN", KEY_A, 0, 0);
    memset(g_read, 0xEE, DATA_MAX);
    run_utility();
    check_read(100);
    STEP("READDATA of a secure save (DSEC)");
    param_init(SCE_UTILITY_SAVEDATA_READDATA, "DSEC", KEY_A, 0, 0);
    memset(g_read, 0xEE, DATA_MAX);
    run_utility();
    check_read(100);
}

/* ---- version 2 ----------------------------------------------------------- */

/* AUTOSAVE `size` bytes and collect the files with the PARAM.SFO summary.
 * `base_size` 0 is the full 1536-byte block; 1480 and 1500 are the older
 * layouts, which end before the key field. */
static void v2_save(const char *save, const unsigned char *key, int ver, int size, int base_size) {
    param_init(PSP_UTILITY_SAVEDATA_AUTOSAVE, save, key, ver, size);
    if (base_size) g_p.base.size = base_size;
    if (size > DATA_MAX) g_p.dataBufSize = BUF_MAX;
    memcpy(g_read, g_plain, size);
    run_utility();
    collect_sfo(save, 1);
}

/* AUTOLOAD `file` of `save` and compare it with the first `size` bytes of the
 * plaintext. */
static void v2_load(const char *save, const char *file, const unsigned char *key, int ver,
                    int size, int base_size) {
    const int cap = size > DATA_MAX ? BUF_MAX : DATA_MAX;
    param_init(PSP_UTILITY_SAVEDATA_AUTOLOAD, save, key, ver, 0);
    strcpy(g_p.fileName, file);
    if (base_size) g_p.base.size = base_size;
    g_p.dataBufSize = cap;
    memset(g_read, 0xEE, cap);
    run_utility();
    check_read_cap(size, cap);
}

/* A data mode (MAKEDATA ... READDATASECURE) on one file of a save. */
static void v2_data(int mode, const char *save, const char *file, const unsigned char *key, int size) {
    param_init(mode, save, key, 0, size);
    strcpy(g_p.fileName, file);
    if (size) memcpy(g_read, g_plain, size);
    else memset(g_read, 0xEE, DATA_MAX);
    run_utility();
}

/* R1: run_utility, except that after ShutdownStart GetStatus is polled back
 * to back and every status it passes through is logged, with when it came.
 * utility.c:1219-1221 says GetStatus "still exposes FINISHED" (4) once after
 * ShutdownStart; run 1, polling once per vblank, saw 3 and then 0 in every
 * step. How long 4 lasts is the question here, so the durations are logged,
 * on a line of their own because they differ from run to run. Bounded: the
 * back-to-back polling gives up after 2 s. */
static void run_utility_poll(void) {
    int r = sceUtilitySavedataInitStart(&g_p);
    if (r < 0) { out("  InitStart = %08X\n", (w32)r); return; }
    int st = 0, last = -1, at3 = 0;
    for (int frame = 0; frame < 1200; frame++) {
        st = sceUtilitySavedataGetStatus();
        if (st != last) { out("  status %d\n", st); last = st; }
        if (st == 2) sceUtilitySavedataUpdate(1);
        else if (st == 3) { at3 = 1; break; }
        sceDisplayWaitVblankStart();
    }
    if (!at3) {
        out("  did not reach status 3: status %d after 20 s\n", st);
    } else {
        enum { MAXSEQ = 8 };
        int seq[MAXSEQ], nseq = 0, polls = 0;
        w32 at[MAXSEQ], vb[MAXSEQ];
        const w32 v0 = sceDisplayGetVcount();
        const w32 t0 = sceKernelGetSystemTimeLow();
        r = sceUtilitySavedataShutdownStart();
        last = -1;
        for (;;) {
            st = sceUtilitySavedataGetStatus();
            const w32 dt = sceKernelGetSystemTimeLow() - t0;
            polls++;
            if (st != last && nseq < MAXSEQ) {
                seq[nseq] = st; at[nseq] = dt; vb[nseq] = sceDisplayGetVcount() - v0; nseq++;
            }
            last = st;
            if (st == 0 || dt > 2000000 || polls >= 2000000) break;
        }
        out("  ShutdownStart = %08X\n", (w32)r);
        out("  statuses polled back to back after it:");
        for (int i = 0; i < nseq; i++) out(" %d", seq[i]);
        out("%s\n", st == 0 ? "" : " (not 0 within 2 s)");
        int four = 0;
        for (int i = 0; i < nseq; i++) if (seq[i] == 4) four = 1;
        out("  status 4 seen: %s; a vblank passed before status 0: %s\n", four ? "yes" : "no",
            st != 0 ? "n/a" : vb[nseq - 1] > 0 ? "yes" : "no");
        out("  timing (varies by run):");
        for (int i = 0; i < nseq; i++) out(" %d at +%u us (+%u vblanks)", seq[i], at[i], vb[i]);
        out(", %d polls\n", polls);
    }
    for (int frame = 0; frame < 120 && sceUtilitySavedataGetStatus() != 0; frame++)
        sceDisplayWaitVblankStart();
    out("  result %08X\n", (w32)g_p.base.result);
}

/* R8: SIZES of the save `save` would make (dataSize `size`, key A, ICON0 when
 * `icon`), with msData pointing at `msdata_save`. utility.c:850-857 returns 0
 * whatever happens and utility.c:537-554 sums utilityData over the whole
 * card; run 1 answered SIZES_NO_DATA for a missing msData save and 3
 * clusters of utilityData for a one-file request. */
static void v2_sizes(const char *save, int size, int icon, const char *msdata_save) {
    static SceUtilitySavedataMsFreeInfo fr;
    static SceUtilitySavedataMsDataInfo md;
    static SceUtilitySavedataUsedDataInfo ud;
    memset(&fr, 0, sizeof fr); memset(&md, 0, sizeof md); memset(&ud, 0, sizeof ud);
    strcpy(md.gameName, GAME); strcpy(md.saveName, msdata_save);
    param_init(SCE_UTILITY_SAVEDATA_SIZES, save, KEY_A, 0, size);
    if (size > DATA_MAX) g_p.dataBufSize = BUF_MAX;
    memcpy(g_read, g_plain, size);
    if (icon) {
        g_p.icon0FileData.buf = g_icon;
        g_p.icon0FileData.bufSize = sizeof ICON0;
        g_p.icon0FileData.size = sizeof ICON0;
    }
    g_p.msFree = &fr; g_p.msData = &md; g_p.utilityData = &ud;
    run_utility();
    log_sizes(&fr, &md, &ud);
}

/* R8: GETSIZE with a secure and a normal entry of real sizes (run 1's 1000 and
 * 5000 bytes answered 0 KB and ''). Entries sized for psprecomp as in
 * query_getsize. */
static void v2_getsize(const char *save, int secure_size, int normal_size) {
    static PspUtilitySavedataSizeEntry se[8], ne[8];
    static PspUtilitySavedataSizeInfo si;
    memset(&si, 0, sizeof si);
    memset(se, 0, sizeof se); memset(ne, 0, sizeof ne);
    si.numSecureEntries = 1; si.secureEntries = se;
    si.numNormalEntries = 1; si.normalEntries = ne;
    strcpy(se[0].name, "DATA.BIN"); se[0].size = secure_size;
    strcpy(ne[0].name, "OTHER.BIN"); ne[0].size = normal_size;
    param_init(SCE_UTILITY_SAVEDATA_GETSIZE, save, KEY_A, 0, 0);
    g_p.sizeInfo = &si;
    run_utility();
    out("  sectorSize %d freeSectors %s freeKB %s\n", si.sectorSize,
        si.freeSectors > 0 ? ">0" : "0", si.freeKB > 0 ? ">0" : "0");
    out("  neededKB %d '%s' overwriteKB %d '%s'\n",
        si.neededKB, si.neededString, si.overwriteKB, si.overwriteString);
}

/* A date's shape: the values are the clock's, so only whether the firmware
 * filled the date and the time of day, and whether it gave microseconds. The
 * struct was filled with 0xEE first, so a field it never wrote shows. */
static const char *date_shape(const ScePspDateTime *d, char *buf, int cap) {
    const unsigned char *raw = (const unsigned char *)d;
    int zero = 1, ee = 1;
    for (int i = 0; i < (int)sizeof *d; i++) {
        if (raw[i]) zero = 0;
        if (raw[i] != 0xEE) ee = 0;
    }
    if (ee) return "untouched";
    if (zero) return "0";
    const int date_ok = d->year >= 1980 && d->year <= 2107 && d->month >= 1 && d->month <= 12 &&
                        d->day >= 1 && d->day <= 31;
    snprintf(buf, cap, "%s, %s, microsecond %s", date_ok ? "date" : "no valid date",
             (d->hour || d->minute || d->second) ? "time of day" : "time 00:00:00",
             d->microsecond ? ">0" : "0");
    return buf;
}

/* Every word of a SceIoStat: mode, attr and size as they are, the dates by
 * shape, st_private as 0, set or untouched (FAT puts card-dependent values
 * there). */
static void log_stat(const SceIoStat *s) {
    const w32 *w = (const w32 *)s;
    char b[80];
    out("  st_mode %04X st_attr %04X st_size %u (high word %X)\n", w[0], w[1], w[2], w[3]);
    out("  ctime %s\n", date_shape(&s->sce_st_ctime, b, sizeof b));
    out("  atime %s\n", date_shape(&s->sce_st_atime, b, sizeof b));
    out("  mtime %s\n", date_shape(&s->sce_st_mtime, b, sizeof b));
    out("  st_private:");
    for (int i = 0; i < 6; i++)
        out(" %s", s->st_private[i] == 0xEEEEEEEEu ? "untouched" : s->st_private[i] ? "set" : "0");
    out("\n");
}

/* sceIoOpen of a directory as if it were a file, then a read from it. */
static void io_open_dir(const char *path) {
    SceUID f = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (f < 0) { out("  sceIoOpen = %08X\n", (w32)f); return; }
    out("  sceIoOpen = valid fd\n");
    char b[16];
    out("  sceIoRead of 16 bytes = %08X\n", (w32)sceIoRead(f, b, sizeof b));
    out("  sceIoClose = %08X\n", (w32)sceIoClose(f));
}

static void io_getstat(const char *path) {
    static SceIoStat st;
    memset(&st, 0xEE, sizeof st);
    int r = sceIoGetstat(path, &st);
    out("  = %08X\n", (w32)r);
    if (r >= 0) log_stat(&st);
}

static void io_dread(const char *path) {
    SceUID d = sceIoDopen(path);
    if (d < 0) { out("  sceIoDopen = %08X\n", (w32)d); return; }
    static SceIoDirent e;
    for (int i = 0; i < 16; i++) {
        memset(&e, 0xEE, sizeof e);
        e.d_private = NULL;         /* the firmware writes through it when set */
        int r = sceIoDread(d, &e);
        if (r <= 0) { out("  Dread = %08X\n", (w32)r); break; }
        e.d_name[255] = '\0';
        out("  Dread = %d, d_name '%s'\n", r, e.d_name);
        log_stat(&e.d_stat);
    }
    sceIoDclose(d);
}

static void version2(void) {
    /* R2 and R7. Loads that change one thing about a request run 1 made:
     * the secureVersion (A0 was saved at 0, A1 at 1), the key on a save no
     * secure mode made, and the key of READDATASECURE. psprecomp's load family
     * checks neither key nor version (utility.c:757-780), and its data reads
     * answer RW_DATA_BROKEN across modes (utility.c:825-827). */
    section("v2: cross-version and cross-mode loads (R2, R7)");
    STEP("load A0: AUTOLOAD with key A, secureVersion 1 (saved at 0)");
    v2_load("A0", "DATA.BIN", KEY_A, 1, 64, 0);
    STEP("load A0: AUTOLOAD with key B, secureVersion 1 (saved at 0 with key A)");
    v2_load("A0", "DATA.BIN", KEY_B, 1, 64, 0);
    STEP("load A1: AUTOLOAD with key A, secureVersion 0 (saved at 1)");
    v2_load("A1", "DATA.BIN", KEY_A, 0, 64, 0);
    STEP("load A1: AUTOLOAD with key B, secureVersion 0 (saved at 1 with key A)");
    v2_load("A1", "DATA.BIN", KEY_B, 0, 64, 0);
    STEP("load DPLAIN: AUTOLOAD with no key (a MAKEDATA save)");
    v2_load("DPLAIN", "DATA.BIN", KEY_0, 0, 100, 0);
    STEP("load DPLAIN: AUTOLOAD with key A (a MAKEDATA save)");
    v2_load("DPLAIN", "DATA.BIN", KEY_A, 0, 100, 0);
    STEP("DSEC: READDATASECURE with key B (made with key A)");
    v2_data(SCE_UTILITY_SAVEDATA_READDATASECURE, "DSEC", "DATA.BIN", KEY_B, 0);
    check_read(100);

    /* P1. Run 1's PLAIN was an AUTOSAVE with an all-zero key at
     * secureVersion 0, and the firmware refused it (80110388); loads with a
     * zero key succeed only at secureVersion 1. Does the save too? The
     * file's size says whether it was encrypted, the SFO flags how. */
    section("v2: zero key at secureVersion 1 (P1)");
    STEP("save PLAINV1: AUTOSAVE 64 bytes, key none, secureVersion 1");
    v2_save("PLAINV1", KEY_0, 1, 64, 0);
    STEP("load PLAINV1: AUTOLOAD with no key, secureVersion 1");
    v2_load("PLAINV1", "DATA.BIN", KEY_0, 1, 64, 0);

    section("v2: GetStatus after ShutdownStart, polled back to back (R1)");
    STEP("status: AUTOLOAD of A0 with key A, GetStatus polled back to back after ShutdownStart");
    param_init(PSP_UTILITY_SAVEDATA_AUTOLOAD, "A0", KEY_A, 0, 0);
    memset(g_read, 0xEE, DATA_MAX);
    run_utility_poll();
    check_read(64);
    STEP("status: FILES of A0, GetStatus polled back to back after ShutdownStart");
    {
        static SceUtilitySavedataFileListEntry sec[8], nor[8], sys[8];
        static SceUtilitySavedataFileListInfo fl;
        memset(&fl, 0, sizeof fl);
        fl.maxSecureEntries = fl.maxNormalEntries = fl.maxSystemEntries = 8;
        fl.secureEntries = sec; fl.normalEntries = nor; fl.systemEntries = sys;
        param_init(SCE_UTILITY_SAVEDATA_FILES, "A0", KEY_A, 0, 0);
        g_p.fileList = &fl;
        run_utility_poll();
        out("  secure %d normal %d system %d\n", (int)fl.resultNumSecureEntries,
            (int)fl.resultNumNormalEntries, (int)fl.resultNumSystemEntries);
    }

    /* R4's gaps in run 1's test vectors: no two saves that differ only in
     * the key, nothing over 1000 bytes, one secure file per save, no
     * sidecars. R6: how FILES classes a second secure file and ICON0. */
    section("v2: more test vectors (R4, R6)");
    STEP("save KEYPAIR: AUTOSAVE 64 bytes, key A, secureVersion 0");
    v2_save("KEYPAIR", KEY_A, 0, 64, 0);
    STEP("save KEYPAIR again: AUTOSAVE 64 bytes, key B, secureVersion 0 (only the key differs)");
    v2_save("KEYPAIR", KEY_B, 0, 64, 0);
    STEP("load KEYPAIR: AUTOLOAD with key B");
    v2_load("KEYPAIR", "DATA.BIN", KEY_B, 0, 64, 0);
    STEP("load KEYPAIR: AUTOLOAD with key A (its key before the rewrite)");
    v2_load("KEYPAIR", "DATA.BIN", KEY_A, 0, 64, 0);
    if (!STEP("save BIG: AUTOSAVE %d bytes, key A, secureVersion 0", BIG_SIZE))
        v2_save("BIG", KEY_A, 0, BIG_SIZE, 0);
    STEP("load BIG: AUTOLOAD with key A, dataBufSize %d", BUF_MAX);
    v2_load("BIG", "DATA.BIN", KEY_A, 0, BIG_SIZE, 0);
    STEP("TWO: MAKEDATASECURE DATA.BIN, 100 bytes, key A");
    v2_data(SCE_UTILITY_SAVEDATA_MAKEDATASECURE, "TWO", "DATA.BIN", KEY_A, 100);
    collect_sfo("TWO", 1);
    if (!STEP("TWO: WRITEDATASECURE a second file, DATA2.BIN, 64 bytes, key A")) {
        v2_data(SCE_UTILITY_SAVEDATA_WRITEDATASECURE, "TWO", "DATA2.BIN", KEY_A, 64);
        collect_sfo("TWO", 1);
    }
    STEP("TWO: READDATASECURE DATA.BIN with key A");
    v2_data(SCE_UTILITY_SAVEDATA_READDATASECURE, "TWO", "DATA.BIN", KEY_A, 0);
    check_read(100);
    STEP("TWO: READDATASECURE DATA2.BIN with key A");
    v2_data(SCE_UTILITY_SAVEDATA_READDATASECURE, "TWO", "DATA2.BIN", KEY_A, 0);
    check_read(64);
    query_files("TWO", KEY_A);
    if (!STEP("save ICON: AUTOSAVE 64 bytes, key A, secureVersion 0, with a %d-byte ICON0 (144x80 PNG)",
              (int)sizeof ICON0)) {
        param_init(PSP_UTILITY_SAVEDATA_AUTOSAVE, "ICON", KEY_A, 0, 64);
        memcpy(g_read, g_plain, 64);
        g_p.icon0FileData.buf = g_icon;
        g_p.icon0FileData.bufSize = sizeof ICON0;
        g_p.icon0FileData.size = sizeof ICON0;
        run_utility();
        collect_sfo("ICON", 1);
    }
    query_files("ICON", KEY_A);

    section("v2: SIZES, GETSIZE and LIST (R8, R9)");
    STEP("SIZES: save SZNEW (none), dataSize 32768, key A; msData A0");
    v2_sizes("SZNEW", 32768, 0, "A0");
    STEP("SIZES: save SZNEW (none), dataSize 32769, key A; msData A0");
    v2_sizes("SZNEW", 32769, 0, "A0");
    STEP("SIZES: save A0, dataSize 64, key A; msData NOSUCH (none)");
    v2_sizes("A0", 64, 0, "NOSUCH");
    STEP("SIZES: save SZNEW (none), dataSize 64, key A, with ICON0; msData ICON");
    v2_sizes("SZNEW", 64, 1, "ICON");
    STEP("SIZES: save BIG, dataSize %d, key A; msData BIG", BIG_SIZE);
    v2_sizes("BIG", BIG_SIZE, 0, "BIG");
    STEP("GETSIZE of SZNEW (none): secure DATA.BIN 32768 bytes, normal OTHER.BIN 100000");
    v2_getsize("SZNEW", 32768, 100000);
    STEP("GETSIZE of BIG: secure DATA.BIN 32768 bytes, normal OTHER.BIN 100000");
    v2_getsize("BIG", 32768, 100000);
    /* R9: run 1 listed nothing for '<>' and '', which utility.c:1189-1193
     * rejects and utility.c:652-674 answers with every save; is saveName a
     * pattern? */
    query_list("*", 64);
    query_list("A0*", 64);
    query_list("A?", 64);
    query_list("A0", 64);

    /* R3 and the ms0 file system. iofilemgr.c:263 opens ms0 paths with host
     * fopen, which Linux allows on a directory (run 1's psprecomp log read
     * "<dir>/" as a 0-byte file); iofilemgr.c:466-487 gives files mode
     * 0x21C0 and directories 0x11C0, attr 0x20/0x10, and zero dates, and
     * Dread's d_stat is the same (iofilemgr.c:794). FILES said 0x21FF. */
    section("v2: sceIoOpen, sceIoGetstat and sceIoDread on ms0 (R3)");
    if (!STEP("io: sceIoOpen of a save directory (DPLAIN), read-only, then a 16-byte read"))
        io_open_dir("ms0:/PSP/SAVEDATA/" GAME "DPLAIN");
    if (!STEP("io: sceIoOpen of a save directory with a trailing slash, then a 16-byte read"))
        io_open_dir("ms0:/PSP/SAVEDATA/" GAME "DPLAIN/");
    STEP("io: sceIoGetstat of DPLAIN/DATA.BIN (100 bytes, from WRITEDATA)");
    io_getstat("ms0:/PSP/SAVEDATA/" GAME "DPLAIN/DATA.BIN");
    STEP("io: sceIoGetstat of the DPLAIN directory");
    io_getstat("ms0:/PSP/SAVEDATA/" GAME "DPLAIN");
    STEP("io: sceIoDread of the DPLAIN directory, each entry's d_stat");
    io_dread("ms0:/PSP/SAVEDATA/" GAME "DPLAIN");

    /* P1. The 1480- and 1500-byte parameter blocks end before the key: what
     * does a save made without one look like? */
    section("v2: zero-key saves with the older parameter sizes (P1)");
    if (!STEP("save PLAIN1480: AUTOSAVE 64 bytes, param size 1480 (no key field)"))
        v2_save("PLAIN1480", KEY_0, 0, 64, 1480);
    if (!STEP("load PLAIN1480: AUTOLOAD, param size 1480"))
        v2_load("PLAIN1480", "DATA.BIN", KEY_0, 0, 64, 1480);
    if (!STEP("save PLAIN1500: AUTOSAVE 64 bytes, param size 1500 (no key field)"))
        v2_save("PLAIN1500", KEY_0, 0, 64, 1500);
    if (!STEP("load PLAIN1500: AUTOLOAD, param size 1500"))
        v2_load("PLAIN1500", "DATA.BIN", KEY_0, 0, 64, 1500);

    /* P1's other two explanations: a first-call effect (PLAIN was the run's
     * first utility call), and gating on the compiled SDK version, which
     * homebrew does not declare. SetCompiledSdkVersion660 changes the
     * process for good (and uofw's sysmem resets the PSP for a version it
     * does not take), so it is the last save-related step. */
    section("v2: the zero-key save again, then with SDK version 6.60 (P1)");
    STEP("save PLAINEND: AUTOSAVE 64 bytes, key none, secureVersion 0 (PLAIN's request, late in the run)");
    v2_save("PLAINEND", KEY_0, 0, 64, 0);
    STEP("load PLAINEND: AUTOLOAD with no key");
    v2_load("PLAINEND", "DATA.BIN", KEY_0, 0, 64, 0);
    int sdk_set = 0;
    if (!STEP("sdk: sceKernelGetCompiledSdkVersion, then sceKernelSetCompiledSdkVersion660(0x06060010)")) {
        out("  GetCompiledSdkVersion = %08X\n", (w32)sceKernelGetCompiledSdkVersion());
        probe_flush();
        const int r = sceKernelSetCompiledSdkVersion660(0x06060010);
        out("  SetCompiledSdkVersion660 = %08X\n", (w32)r);
        out("  GetCompiledSdkVersion = %08X\n", (w32)sceKernelGetCompiledSdkVersion());
        sdk_set = r == 0;
    }
    if (!STEP("save PLAIN660: AUTOSAVE 64 bytes, key none, secureVersion 0, after SetCompiledSdkVersion660")) {
        if (!sdk_set) out("  not run: the SDK version was not set\n");
        else v2_save("PLAIN660", KEY_0, 0, 64, 0);
    }
    if (!STEP("load PLAIN660: AUTOLOAD with no key, after SetCompiledSdkVersion660")) {
        if (!sdk_set) out("  not run: the SDK version was not set\n");
        else v2_load("PLAIN660", "DATA.BIN", KEY_0, 0, 64, 0);
    }
}

/* ---- main ---------------------------------------------------------------- */

int main(int argc, char **argv) {
    probe_init("saveprobe", PROBE_VERSION, argc, argv);
    g_plain = memalign(64, BUF_MAX);
    g_read  = memalign(64, BUF_MAX);
    g_file  = memalign(64, FILE_MAX);
    g_icon  = memalign(64, sizeof ICON0);
    if (!g_plain || !g_read || !g_file || !g_icon) { say("out of memory\n"); probe_done(); }
    for (int i = 0; i < BUF_MAX; i++) g_plain[i] = (unsigned char)((i * 7 + 3) & 0xFF);
    memcpy(g_icon, ICON0, sizeof ICON0);

    section("saves (plaintext byte i = (i*7+3) & 0xFF; key A = 00..0F, key B = FF..F0)");
    save_and_read("PLAIN",    NULL,  0, 64);
    save_and_read("A0",       KEY_A, 0, 64);
    save_and_read("A0AGAIN",  KEY_A, 0, 64);    /* same inputs: is the output the same? */
    save_and_read("B0",       KEY_B, 0, 64);
    save_and_read("A0LEN1",   KEY_A, 0, 1);
    save_and_read("A0LEN15",  KEY_A, 0, 15);
    save_and_read("A0LEN16",  KEY_A, 0, 16);
    save_and_read("A0LEN17",  KEY_A, 0, 17);
    save_and_read("A0LEN1000", KEY_A, 0, 1000);

    section("the data modes");
    data_modes();

    section("queries");
    query_sizes();
    query_list("<>", 32);
    query_list("", 32);
    query_files("A0", KEY_A);
    query_files("PLAIN", NULL);
    query_getsize("A0");

    /* Other secure versions last: the ones this firmware does not take
     * should only cost their own steps. */
    section("secure versions 1..3");
    save_and_read("A1", KEY_A, 1, 64);
    save_and_read("A2", KEY_A, 2, 64);
    save_and_read("A3", KEY_A, 3, 64);

    version2();

    probe_done();
    return 0;
}
