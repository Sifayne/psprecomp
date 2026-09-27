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

#define PROBE_VERSION 1

typedef unsigned int w32;   /* PSPSDK's u32 is uint32_t, a long here, which %X does not take */

#define GAME "PRCP00000"

static w32 crc32(const void *p, int n) {
    const unsigned char *b = p;
    w32 c = 0xFFFFFFFFu;
    for (int i = 0; i < n; i++) {
        c ^= b[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static const unsigned char KEY_A[16] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F };
static const unsigned char KEY_B[16] = {
    0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xFA, 0xF9, 0xF8,
    0xF7, 0xF6, 0xF5, 0xF4, 0xF3, 0xF2, 0xF1, 0xF0 };
static const unsigned char KEY_0[16];

/* The plaintext every save carries: byte i is (i * 7 + 3) & 0xFF, so any
 * prefix of it is known from its length alone. */
static unsigned char *g_plain;
static unsigned char *g_read;
static unsigned char *g_file;
#define DATA_MAX 4096
#define FILE_MAX (64 * 1024)

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

/* List the save's directory, log each file, and copy it beside the EBOOT as
 * <save>__<file>. */
static void collect(const char *save) {
    char dir[96];
    snprintf(dir, sizeof dir, "ms0:/PSP/SAVEDATA/" GAME "%s", save);
    SceUID d = sceIoDopen(dir);
    if (d < 0) { out("  dir %s: open %08X\n", save, (w32)d); return; }
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
            snprintf(name, sizeof name, "%s__%s", save, e.d_name);
            out("  -> %s", name);
            probe_write_file(name, g_file, n);
        }
        out("\n");
    }
    sceIoDclose(d);
}

static void check_read(int size) {
    const int n = g_p.dataSize;
    out("  read back: dataSize %d, matches the plaintext: %s\n", n,
        (n == size && memcmp(g_read, g_plain, size) == 0) ? "yes" :
        (n > 0 && n <= DATA_MAX) ? "no" : "n/a");
    if (n > 0 && n <= DATA_MAX && memcmp(g_read, g_plain, n < size ? n : size) != 0) {
        out("  first 16 read:");
        for (int i = 0; i < 16 && i < n; i++) out(" %02X", g_read[i]);
        out("\n");
    }
}

/* One save: write it with AUTOSAVE, collect its files, read it back with the
 * right key, a wrong key and no key. */
static void save_and_read(const char *save, const unsigned char *key, int ver, int size) {
    step("save %s: AUTOSAVE %d bytes, key %s, secureVersion %d", save, size,
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
        step("load %s: AUTOLOAD with %s", save, i == 1 && key == KEY_B ? "key A" : R[i].name);
        param_init(PSP_UTILITY_SAVEDATA_AUTOLOAD, save, k, ver, 0);
        memset(g_read, 0xEE, DATA_MAX);
        run_utility();
        check_read(size);
    }
}

/* ---- queries ------------------------------------------------------------- */

static void query_sizes(void) {
    step("SIZES for PLAIN (needed space for a save of this shape)");
    static SceUtilitySavedataMsFreeInfo fr;
    static SceUtilitySavedataMsDataInfo md;
    static SceUtilitySavedataUsedDataInfo ud;
    memset(&fr, 0, sizeof fr); memset(&md, 0, sizeof md); memset(&ud, 0, sizeof ud);
    strcpy(md.gameName, GAME); strcpy(md.saveName, "PLAIN");
    param_init(SCE_UTILITY_SAVEDATA_SIZES, "PLAIN", NULL, 0, 256);
    memcpy(g_read, g_plain, 256);
    g_p.msFree = &fr; g_p.msData = &md; g_p.utilityData = &ud;
    run_utility();
    /* Free space depends on the card, so only its shape is logged. */
    out("  msFree: clusterSize %d, freeClusters %s, freeSpaceKB %s\n",
        fr.clusterSize, fr.freeClusters > 0 ? ">0" : "0", fr.freeSpaceKB > 0 ? ">0" : "0");
    out("  msData: usedClusters %d usedSpaceKB %d '%s' used32KB %d '%s'\n",
        md.info.usedClusters, md.info.usedSpaceKB, md.info.usedSpaceStr,
        md.info.usedSpace32KB, md.info.usedSpace32Str);
    out("  utilityData: usedClusters %d usedSpaceKB %d '%s' used32KB %d '%s'\n",
        ud.usedClusters, ud.usedSpaceKB, ud.usedSpaceStr, ud.usedSpace32KB, ud.usedSpace32Str);
}

static void query_list(const char *pattern) {
    step("LIST of " GAME " with saveName '%s'", pattern);
    static SceUtilitySavedataIdListEntry ents[32];
    static SceUtilitySavedataIdListInfo il;
    memset(ents, 0, sizeof ents);
    il.maxCount = 32; il.resultCount = -1; il.entries = ents;
    param_init(SCE_UTILITY_SAVEDATA_LIST, pattern, NULL, 0, 0);
    g_p.idList = &il;
    run_utility();
    out("  resultCount %d\n", il.resultCount);
    for (int i = 0; i < il.resultCount && i < 32; i++)
        out("  entry st_mode %04X name '%s'\n", (w32)ents[i].st_mode, ents[i].name);
}

static void query_files(const char *save, const unsigned char *key) {
    step("FILES of %s", save);
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
    step("GETSIZE of %s", save);
    static PspUtilitySavedataSizeEntry se[2], ne[2];
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
        step("%s: make (mode %d)", D[i].save, D[i].make);
        param_init(D[i].make, D[i].save, D[i].key, 0, 100);
        memcpy(g_read, g_plain, 100);
        run_utility();
        collect(D[i].save);
        step("%s: write 100 bytes (mode %d)", D[i].save, D[i].write);
        param_init(D[i].write, D[i].save, D[i].key, 0, 100);
        memcpy(g_read, g_plain, 100);
        run_utility();
        collect(D[i].save);
        step("%s: read (mode %d)", D[i].save, D[i].read);
        param_init(D[i].read, D[i].save, D[i].key, 0, 0);
        memset(g_read, 0xEE, DATA_MAX);
        run_utility();
        check_read(100);
    }
    step("READDATA of a save that does not exist");
    param_init(SCE_UTILITY_SAVEDATA_READDATA, "NOSUCH", KEY_0, 0, 0);
    run_utility();
    step("AUTOLOAD of a save that does not exist");
    param_init(PSP_UTILITY_SAVEDATA_AUTOLOAD, "NOSUCH", KEY_0, 0, 0);
    run_utility();
    step("READDATASECURE of a non-secure save (DPLAIN)");
    param_init(SCE_UTILITY_SAVEDATA_READDATASECURE, "DPLAIN", KEY_A, 0, 0);
    memset(g_read, 0xEE, DATA_MAX);
    run_utility();
    check_read(100);
    step("READDATA of a secure save (DSEC)");
    param_init(SCE_UTILITY_SAVEDATA_READDATA, "DSEC", KEY_A, 0, 0);
    memset(g_read, 0xEE, DATA_MAX);
    run_utility();
    check_read(100);
}

/* ---- main ---------------------------------------------------------------- */

int main(int argc, char **argv) {
    probe_init("saveprobe", PROBE_VERSION, argc, argv);
    g_plain = memalign(64, DATA_MAX);
    g_read  = memalign(64, DATA_MAX);
    g_file  = memalign(64, FILE_MAX);
    if (!g_plain || !g_read || !g_file) { say("out of memory\n"); probe_done(); }
    for (int i = 0; i < DATA_MAX; i++) g_plain[i] = (unsigned char)((i * 7 + 3) & 0xFF);

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
    query_list("<>");
    query_list("");
    query_files("A0", KEY_A);
    query_files("PLAIN", NULL);
    query_getsize("A0");

    /* Other secure versions last: the ones this firmware does not take
     * should only cost their own steps. */
    section("secure versions 1..3");
    save_and_read("A1", KEY_A, 1, 64);
    save_and_read("A2", KEY_A, 2, 64);
    save_and_read("A3", KEY_A, 3, 64);

    probe_done();
    return 0;
}
