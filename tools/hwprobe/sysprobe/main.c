/* sysprobe -- firmware calls the other probes do not reach.
 *
 * Written against PSPSDK (BSD) only. Nothing here needs input, but the disc
 * steps need a UMD in the drive -- any game -- and the remote steps want
 * nothing plugged into the headphone socket. Each one psprecomp answers
 * without a measurement:
 *
 *   - sceIoMkdir with a missing parent, and sceIoChstat, which
 *     src/hle/iofilemgr.c makes parents for and makes a no-op;
 *   - disc0:/sce_lbn<sector>_size<bytes>, a file named by its extent, which
 *     WipEout Pulse opens its archives by (iso_lookup's lbn_name);
 *   - sceKernelSetGPO and sceKernelGetGPI, and the headphone remote's
 *     sceHprm calls, answered as if nothing were connected (src/hle/misc.c);
 *   - SetCompiledSdkVersion's 3.70 variant, 0x342061E5 (src/hle/sysmem.c),
 *     last, since it changes the process for good.
 *
 * A log line holds what the firmware decided. Disc contents are compared,
 * never logged: the disc is whatever game is in the drive. */
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <psphprm.h>
#include <pspumd.h>

#include <stdio.h>
#include <string.h>

#include "probe.h"

PSP_MODULE_INFO("sysprobe", PSP_MODULE_USER, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(1024);

#define PROBE_VERSION 1

typedef unsigned int w32;

/* UtilsForUser, in libpspuser without a declaration in PSPSDK's headers. */
int sceKernelSetGPO(int value);
int sceKernelGetGPI(void);
/* imports.S */
int sceKernelSetCompiledSdkVersion370(unsigned int version);
int sceKernelGetCompiledSdkVersion(void);

/* sceIoChstat's bits, as the PS2's iomanX names them (FIO_CST_*). */
#define CST_MODE 0x0001
#define CST_ATTR 0x0002
#define CST_SIZE 0x0004
#define CST_CT   0x0008
#define CST_AT   0x0010
#define CST_MT   0x0020
#define CST_PRVT 0x0040

static const char *path(const char *name) {
    static char p[256];
    snprintf(p, sizeof p, "%sscratch/%s", probe_dir(), name);
    return p;
}

static void stat_of(const char *what, const char *p) {
    SceIoStat s;
    memset(&s, 0, sizeof s);
    const int r = sceIoGetstat(p, &s);
    out("  Getstat(%s) = %08X", what, (w32)r);
    if (r >= 0) out(": mode %04X, attr %04X, size %lld", (w32)s.st_mode, s.st_attr, (long long)s.st_size);
    out("\n");
}

static void times_of(const char *what, const char *p) {
    SceIoStat s;
    memset(&s, 0, sizeof s);
    if (sceIoGetstat(p, &s) < 0) { out("  %s: no stat\n", what); return; }
    const ScePspDateTime *t[3] = { &s.sce_st_ctime, &s.sce_st_atime, &s.sce_st_mtime };
    static const char *const name[3] = { "ctime", "atime", "mtime" };
    /* Exactly, when it is one of the times this probe sets (FAT keeps each
     * of its three times differently, so the rounding is the answer); else
     * only that it is not, since the clock's time differs by run. */
    out("  %s:", what);
    for (int i = 0; i < 3; i++) {
        if (t[i]->year == 2001 || t[i]->year == 2002)
            out(" %s %04d-%02d-%02d %02d:%02d:%02d.%06u", name[i], t[i]->year, t[i]->month, t[i]->day,
                t[i]->hour, t[i]->minute, t[i]->second, t[i]->microsecond);
        else
            out(" %s not set", name[i]);
    }
    out("\n");
}

/* ---- 1. directories and stat changes ----------------------------------------- */

static void io(void) {
    section("1. sceIoMkdir and sceIoChstat on the memory stick");
    char root[256];
    snprintf(root, sizeof root, "%sscratch", probe_dir());
    /* Leftovers of an earlier run, deepest first. */
    sceIoRemove(path("f.bin"));
    sceIoRmdir(path("x1/x2"));
    sceIoRmdir(path("x1"));
    sceIoRmdir(path("x3"));
    sceIoMkdir(root, 0777);

    step("Mkdir with a missing parent: x1/x2");
    out("  Mkdir(x1/x2) = %08X\n", (w32)sceIoMkdir(path("x1/x2"), 0777));
    stat_of("x1", path("x1"));
    stat_of("x1/x2", path("x1/x2"));
    step("Mkdir x1, x1 again, x1/x2, x3/ with a trailing slash; Rmdir x1 while x1/x2 is in it");
    out("  Mkdir(x1) = %08X\n", (w32)sceIoMkdir(path("x1"), 0777));
    out("  Mkdir(x1) again = %08X\n", (w32)sceIoMkdir(path("x1"), 0777));
    out("  Mkdir(x1/x2) = %08X\n", (w32)sceIoMkdir(path("x1/x2"), 0777));
    out("  Mkdir(x3/) = %08X\n", (w32)sceIoMkdir(path("x3/"), 0777));
    stat_of("x3", path("x3"));
    out("  Rmdir(x1), not empty = %08X\n", (w32)sceIoRmdir(path("x1")));

    step("Chstat: a 100-byte file, each bit on its own");
    {
        const SceUID fd = sceIoOpen(path("f.bin"), PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        static char bytes[100];
        if (fd >= 0) { sceIoWrite(fd, bytes, sizeof bytes); sceIoClose(fd); }
    }
    stat_of("f.bin", path("f.bin"));
    {
        SceIoStat s;
        memset(&s, 0, sizeof s);
        sceIoGetstat(path("f.bin"), &s);
        s.st_mode = (s.st_mode & ~0777) | 0444;
        out("  Chstat(mode r--r--r--, MODE) = %08X\n", (w32)sceIoChstat(path("f.bin"), &s, CST_MODE));
        stat_of("f.bin", path("f.bin"));
        s.st_mode = (s.st_mode & ~0777) | 0777;
        out("  Chstat(mode rwxrwxrwx, MODE) = %08X\n", (w32)sceIoChstat(path("f.bin"), &s, CST_MODE));
        stat_of("f.bin", path("f.bin"));
        s.st_attr = 0x0001;
        out("  Chstat(attr 0001, ATTR) = %08X\n", (w32)sceIoChstat(path("f.bin"), &s, CST_ATTR));
        stat_of("f.bin", path("f.bin"));
        s.st_size = 10;
        out("  Chstat(size 10, SIZE) = %08X\n", (w32)sceIoChstat(path("f.bin"), &s, CST_SIZE));
        stat_of("f.bin", path("f.bin"));

        const ScePspDateTime when = { 2001, 2, 3, 4, 5, 6, 7 };
        const ScePspDateTime other = { 2002, 3, 4, 5, 6, 8, 0 };
        s.sce_st_ctime = s.sce_st_atime = s.sce_st_mtime = when;
        out("  Chstat(2001-02-03 04:05:06.000007, MT) = %08X\n", (w32)sceIoChstat(path("f.bin"), &s, CST_MT));
        times_of("after MT", path("f.bin"));
        out("  Chstat(the same, CT) = %08X\n", (w32)sceIoChstat(path("f.bin"), &s, CST_CT));
        times_of("after CT", path("f.bin"));
        s.sce_st_atime = other;
        out("  Chstat(atime 2002-03-04 05:06:08, AT) = %08X\n", (w32)sceIoChstat(path("f.bin"), &s, CST_AT));
        times_of("after AT", path("f.bin"));
        out("  Chstat(bits 0) = %08X\n", (w32)sceIoChstat(path("f.bin"), &s, 0));
        out("  Chstat(PRVT) = %08X\n", (w32)sceIoChstat(path("f.bin"), &s, CST_PRVT));
        out("  Chstat(a file that is not there, MT) = %08X\n", (w32)sceIoChstat(path("none.bin"), &s, CST_MT));
        out("  Chstat(the directory x3, MT) = %08X\n", (w32)sceIoChstat(path("x3"), &s, CST_MT));
        times_of("x3 after MT", path("x3"));
    }

    step("clean up");
    out("  Remove(f.bin) = %08X\n", (w32)sceIoRemove(path("f.bin")));
    out("  Rmdir(x1/x2) = %08X, Rmdir(x1) = %08X, Rmdir(x3) = %08X\n",
        (w32)sceIoRmdir(path("x1/x2")), (w32)sceIoRmdir(path("x1")), (w32)sceIoRmdir(path("x3")));
}

/* ---- 2. files named by their extent -------------------------------------------- */

static unsigned char g_sfo[64], g_buf[64];

/* Open `p`, read 64 bytes from its start and the size SEEK_END reports. */
static void extent(const char *what, const char *p) {
    const SceUID fd = sceIoOpen(p, PSP_O_RDONLY, 0);
    out("  %s: Open = %s", what, fd >= 0 ? "a valid fd" : "");
    if (fd < 0) { out("%08X\n", (w32)fd); return; }
    memset(g_buf, 0, sizeof g_buf);
    const int n = sceIoRead(fd, g_buf, sizeof g_buf);
    const SceOff end = sceIoLseek(fd, 0, PSP_SEEK_END);
    sceIoClose(fd);
    out(", Read(64) = %d, the same as PARAM.SFO's start %d, SEEK_END = %lld\n",
        n, n > 0 && !memcmp(g_buf, g_sfo, n), (long long)end);
}

static void disc(void) {
    section("2. disc0:/sce_lbn<sector>_size<bytes> against the disc's own PARAM.SFO");
    step("a disc in the drive");
    const int medium = sceUmdCheckMedium();
    out("  CheckMedium = %08X\n", (w32)medium);
    if (medium <= 0) { out("  not run: no disc in the drive\n"); return; }
    out("  Activate = %08X\n", (w32)sceUmdActivate(1, "disc0:"));
    out("  WaitDriveStat(READY) = %08X\n", (w32)sceUmdWaitDriveStat(PSP_UMD_READY));

    step("PARAM.SFO by its path: Getstat's size and start sector, its first 64 bytes");
    SceIoStat s;
    memset(&s, 0, sizeof s);
    const int r = sceIoGetstat("disc0:/PSP_GAME/PARAM.SFO", &s);
    out("  Getstat = %08X\n", (w32)r);
    if (r < 0) return;
    const w32 lbn = s.st_private[0], size = (w32)s.st_size;
    out("  size %u; st_private %08X %08X %08X %08X %08X %08X (the first is taken as its sector)\n",
        size, s.st_private[0], s.st_private[1], s.st_private[2], s.st_private[3], s.st_private[4], s.st_private[5]);
    {
        const SceUID fd = sceIoOpen("disc0:/PSP_GAME/PARAM.SFO", PSP_O_RDONLY, 0);
        out("  Open(PARAM.SFO) = %s\n", fd >= 0 ? "a valid fd" : "an error");
        if (fd < 0) return;
        sceIoRead(fd, g_sfo, sizeof g_sfo);
        sceIoClose(fd);
    }

    step("the same bytes by their extent, in each spelling");
    char p[128];
    snprintf(p, sizeof p, "disc0:/sce_lbn0x%x_size0x%x", lbn, size);
    extent("sce_lbn0x<hex>_size0x<hex>", p);
    snprintf(p, sizeof p, "disc0:/sce_lbn%u_size%u", lbn, size);
    extent("sce_lbn<decimal>_size<decimal>", p);
    snprintf(p, sizeof p, "disc0:/sce_lbn0X%X_size0X%X", lbn, size);
    extent("sce_lbn0X<HEX>_size0X<HEX>", p);
    snprintf(p, sizeof p, "disc0:sce_lbn0x%x_size0x%x", lbn, size);
    extent("without the slash", p);
    snprintf(p, sizeof p, "umd0:/sce_lbn0x%x_size0x%x", lbn, size);
    extent("on umd0:", p);
    snprintf(p, sizeof p, "disc0:/sce_lbn0x%x", lbn);
    extent("no size", p);
    snprintf(p, sizeof p, "disc0:/sce_lbn0x%x_size0x%x.bin", lbn, size);
    extent("something after the size", p);
    snprintf(p, sizeof p, "disc0:/sce_lbn0x%x_size0x%x", lbn, 16);
    extent("a size of 16", p);
    snprintf(p, sizeof p, "disc0:/sce_lbn0x%x_size0x%x", lbn, size + 4096);
    extent("4096 bytes past the file", p);
    snprintf(p, sizeof p, "disc0:/SCE_LBN0x%x_SIZE0x%x", lbn, size);
    extent("in capitals", p);
    snprintf(p, sizeof p, "disc0:/sce_lbn0x%x_size0x%x", 0x7FFFFFF, 64);
    extent("a sector past the disc's end", p);

    step("Getstat and Dopen on an extent");
    snprintf(p, sizeof p, "disc0:/sce_lbn0x%x_size0x%x", lbn, size);
    stat_of("the extent", p);
    {
        const SceUID d = sceIoDopen(p);
        out("  Dopen(the extent) = %s\n", d >= 0 ? "a valid fd" : "an error");
        if (d >= 0) sceIoDclose(d);
    }
}

/* ---- 3. what nothing is connected to ------------------------------------------- */

static void connected(void) {
    section("3. general-purpose I/O and the headphone remote (nothing plugged in)");
    step("sceKernelGetGPI, sceKernelSetGPO");
    out("  GetGPI = %08X\n", (w32)sceKernelGetGPI());
    out("  SetGPO(0) = %08X\n", (w32)sceKernelSetGPO(0));
    out("  SetGPO(55) = %08X\n", (w32)sceKernelSetGPO(0x55));
    out("  GetGPI = %08X\n", (w32)sceKernelGetGPI());
    out("  SetGPO(0) = %08X\n", (w32)sceKernelSetGPO(0));
    step("sceHprm");
    out("  IsRemoteExist = %08X\n", (w32)sceHprmIsRemoteExist());
    out("  IsHeadphoneExist = %08X\n", (w32)sceHprmIsHeadphoneExist());
    out("  IsMicrophoneExist = %08X\n", (w32)sceHprmIsMicrophoneExist());
    u32 key = 0x5A5A5A5A;
    out("  PeekCurrentKey = %08X, key %08X\n", (w32)sceHprmPeekCurrentKey(&key), (w32)key);
}

/* ---- 4. last: the SDK version --------------------------------------------------- */

static void sdk(void) {
    section("4. SetCompiledSdkVersion's 3.70 variant (last: it changes the process)");
    if (step("GetCompiledSdkVersion, SetCompiledSdkVersion370(0x03070010), GetCompiledSdkVersion")) return;
    out("  GetCompiledSdkVersion = %08X\n", (w32)sceKernelGetCompiledSdkVersion());
    out("  SetCompiledSdkVersion370(03070010) = %08X\n", (w32)sceKernelSetCompiledSdkVersion370(0x03070010));
    out("  GetCompiledSdkVersion = %08X\n", (w32)sceKernelGetCompiledSdkVersion());
}

int main(int argc, char **argv) {
    probe_init("sysprobe", PROBE_VERSION, argc, argv);
    io();
    disc();
    connected();
    sdk();
    probe_done();
    return 0;
}
