/* mpegprobe -- ask a real PSP the values psprecomp's runtime cannot source.
 *
 * Written against PSPSDK (BSD) only. It prints what the firmware's own
 * sceMpeg library and the GE do, so those numbers can replace the ones
 * src/hle/mpeg.c and src/hle/ge.c mark "unsourced":
 *
 *   X         sceMpeg sizes and structures: QueryMemSize,
 *             RingbufferQueryMemSize, QueryAtracEsSize, every word of
 *             SceMpegRingbuffer after Construct and Create, SceMpegAu after
 *             InitAu.
 *   SQUARE    the same setup, then a real movie (movie.pmf beside the EBOOT):
 *             which PSMF header words QueryStreamOffset/Size read, the ring
 *             after each Put, and each access unit's timestamps.
 *   TRIANGLE  GE block transfers with unaligned addresses and wide strides.
 *
 * Each test runs on its own, so one that crashes the PSP does not hide the
 * others. The log, mpegprobe.txt beside the EBOOT, is appended to and written
 * through before every call that could crash, so its last line names the call
 * that did. It holds numbers only, no movie data.
 */
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <pspge.h>
#include <pspgu.h>
#include <psputility.h>
#include <psputils.h>
#include <pspmpeg.h>
#include <pspsdk.h>

#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

PSP_MODULE_INFO("mpegprobe", PSP_MODULE_USER, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
/* A fixed heap, so the firmware modules loaded below have the rest. */
PSP_HEAP_SIZE_KB(8192);

#define PROBE_VERSION 2

#define LIBRARY_IS_NOT_LINKED 0x8002013Au  /* uofw errors.h */
#define UNCACHED(p) ((void *)((unsigned int)(p) | 0x40000000u))

/* Ring size, and the most packets per Put: what Last Raven itself uses. */
#define RING_PACKETS 640
#define PUT_MAX      32
/* How many access units of each kind to log before stopping. */
#define AU_WANTED 8

/* ---- output ----------------------------------------------------------------
 *
 * Lines collect in memory and go to the file whenever step() is called, or
 * the buffer fills. Each write opens, appends and closes, so what reached the
 * file survives the PSP switching itself off. */

static char g_dir[256] = "ms0:/";
static char g_logpath[300];
static char g_pending[8192];
static int  g_pending_len;
static int  g_step;

static void flush_log(void) {
    if (!g_pending_len) return;
    SceUID fd = sceIoOpen(g_logpath, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, g_pending, g_pending_len);
        sceIoClose(fd);
    }
    g_pending_len = 0;
}

static void vemit(int screen, const char *fmt, va_list ap) {
    char buf[512];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n < 0) return;
    if (n >= (int)sizeof buf) n = sizeof buf - 1;
    if (g_pending_len + n > (int)sizeof g_pending) flush_log();
    memcpy(g_pending + g_pending_len, buf, n);
    g_pending_len += n;
    if (screen) pspDebugScreenPrintf("%s", buf);
}

/* To the log only. */
static void out(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vemit(0, fmt, ap); va_end(ap);
}

/* To the log and the screen. */
static void say(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vemit(1, fmt, ap); va_end(ap);
}

/* Name the call about to be made, on screen and in the log, and make sure the
 * log is on the memory stick before it runs. */
static void step(const char *fmt, ...) {
    char buf[160];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    g_step++;
    pspDebugScreenPrintf("%3d %s\n", g_step, buf);
    out("[%d] %s\n", g_step, buf);
    flush_log();
}

static void dump_words(const char *label, const void *p, int nwords) {
    const volatile unsigned int *w = p;
    out("%s\n", label);
    for (int i = 0; i < nwords; i += 4) {
        out("   ");
        for (int j = i; j < nwords && j < i + 4; j++)
            out("  +%02X %08X", j * 4, w[j]);
        out("\n");
    }
}

/* ---- exit callback -------------------------------------------------------- */

static int exit_cb(int a, int b, void *c) {
    (void)a; (void)b; (void)c;
    flush_log();
    sceKernelExitGame();
    return 0;
}

static int cb_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    int id = sceKernelCreateCallback("exit", exit_cb, NULL);
    sceKernelRegisterExitCallback(id);
    sceKernelSleepThreadCB();
    return 0;
}

/* ---- sceMpeg setup, shared by both MPEG tests ------------------------------ */

typedef struct {
    int      ok;
    int      memsize, ringsize;
    SceInt32 es, esout;
    void    *mdata, *ringdata, *avces, *atres;
    SceMpeg  mpeg;
} mpeg_state;

static unsigned int g_ringw[16] __attribute__((aligned(64)));
static unsigned int g_avcau[8]  __attribute__((aligned(64)));
static unsigned int g_atrau[8]  __attribute__((aligned(64)));
static mpeg_state   g_m;
static SceUID       g_movie = -1;
static int          g_cb_calls;

/* Describe a ring word: a pointer into the ring data or the MPEG buffer, the
 * handle, or just a number. */
static void explain_ring_word(int i, unsigned int v) {
    const unsigned int d = (unsigned int)g_m.ringdata;
    const unsigned int m = (unsigned int)g_m.mdata;
    if (d && v >= d && v <= d + (unsigned int)g_m.ringsize)
        out("  +%02X %08X  = ring data + 0x%X\n", i * 4, v, v - d);
    else if (g_m.mpeg && v == (unsigned int)g_m.mpeg)
        out("  +%02X %08X  = the SceMpeg handle\n", i * 4, v);
    else if (m && v >= m && v < m + (unsigned int)g_m.memsize)
        out("  +%02X %08X  = sceMpegCreate's buffer + 0x%X\n", i * 4, v, v - m);
    else
        out("  +%02X %08X  = %d\n", i * 4, v, (int)v);
}

static void dump_ring(const char *label) {
    out("%s\n", label);
    for (int i = 0; i < 12; i++) explain_ring_word(i, g_ringw[i]);
}

/* The ring callback: read whole 2048-byte packets of the movie into the ring.
 * Logs only; a step() here would interleave with the Put that called it. */
static SceInt32 ring_cb(ScePVoid data, SceInt32 packets, ScePVoid param) {
    int got = 0;
    if (g_movie >= 0) {
        got = sceIoRead(g_movie, data, packets * 2048);
        if (got < 0) got = 0;
    }
    if (g_cb_calls++ < 24)
        out("  callback(ring data + 0x%X, packets %d, param %08X) -> read %d bytes\n",
            (unsigned int)data - (unsigned int)g_m.ringdata, (int)packets,
            (unsigned int)param, got);
    return got / 2048;
}

/* Load the firmware's MPEG library and build what the game builds: a
 * 640-packet ring, an MPEG context over it, and one access unit each for
 * video and audio. With verbose set, dump every structure along the way. */
static int mpeg_setup(int verbose) {
    int r;
    memset(&g_m, 0, sizeof g_m);

    /* Three ways to get the library linked, newest first; the log records
     * which one worked, since the answers belong to that library. An import
     * that is still unlinked returns LIBRARY_IS_NOT_LINKED. */
    step("sceUtilityLoadModule(AV_AVCODEC)");
    r = sceUtilityLoadModule(PSP_MODULE_AV_AVCODEC);
    out("  = %08X\n", r);
    step("sceUtilityLoadModule(AV_MPEGBASE)");
    r = sceUtilityLoadModule(PSP_MODULE_AV_MPEGBASE);
    out("  = %08X\n", r);
    step("sceMpegInit");
    r = sceMpegInit();
    out("  = %08X\n", r);
    if ((unsigned int)r == LIBRARY_IS_NOT_LINKED) {
        step("sceUtilityLoadAvModule(AVCODEC)");
        r = sceUtilityLoadAvModule(PSP_AV_MODULE_AVCODEC);
        out("  = %08X\n", r);
        step("sceUtilityLoadAvModule(MPEGBASE)");
        r = sceUtilityLoadAvModule(PSP_AV_MODULE_MPEGBASE);
        out("  = %08X\n", r);
        step("sceMpegInit");
        r = sceMpegInit();
        out("  = %08X\n", r);
    }
    if ((unsigned int)r == LIBRARY_IS_NOT_LINKED) {
        step("pspSdkLoadStartModule(flash0:/kd/mpeg_vsh.prx)");
        r = pspSdkLoadStartModule("flash0:/kd/mpeg_vsh.prx", PSP_MEMORY_PARTITION_USER);
        out("  = %08X\n", r);
        step("sceMpegInit");
        r = sceMpegInit();
        out("  = %08X\n", r);
    }
    if (r < 0) {
        say("sceMpeg is not available (%08X)\n", r);
        return -1;
    }

    step("sceMpegQueryMemSize(0)");
    g_m.memsize = sceMpegQueryMemSize(0);
    out("  = %d (0x%X)\n", g_m.memsize, g_m.memsize);
    if (verbose) {
        static const int pk[] = { 1, 2, 3, 4, 16, 32, 64, 256, 512, RING_PACKETS };
        for (unsigned i = 0; i < sizeof pk / sizeof pk[0]; i++) {
            step("sceMpegRingbufferQueryMemSize(%d)", pk[i]);
            r = sceMpegRingbufferQueryMemSize(pk[i]);
            out("  = %d (0x%X)\n", r, r);
        }
    }
    step("sceMpegRingbufferQueryMemSize(%d)", RING_PACKETS);
    g_m.ringsize = sceMpegRingbufferQueryMemSize(RING_PACKETS);
    out("  = %d (0x%X)\n", g_m.ringsize, g_m.ringsize);
    if (g_m.memsize <= 0 || g_m.ringsize <= 0) {
        say("a size query failed; stopped\n");
        return -1;
    }

    g_m.ringdata = memalign(64, g_m.ringsize);
    g_m.mdata = memalign(64, g_m.memsize);
    if (!g_m.ringdata || !g_m.mdata) { say("out of memory; stopped\n"); return -1; }
    memset(g_m.ringdata, 0xCC, g_m.ringsize);
    memset(g_m.mdata, 0, g_m.memsize);
    for (int i = 0; i < 16; i++) g_ringw[i] = 0xCCCCCCCCu;
    out("ring struct at %08X, ring data at %08X, MPEG buffer at %08X, callback %08X\n",
        (unsigned int)g_ringw, (unsigned int)g_m.ringdata,
        (unsigned int)g_m.mdata, (unsigned int)ring_cb);

    step("sceMpegRingbufferConstruct(ring, %d, data, %d, cb, 0x12345678)",
         RING_PACKETS, g_m.ringsize);
    r = sceMpegRingbufferConstruct((SceMpegRingbuffer *)g_ringw, RING_PACKETS,
                                   g_m.ringdata, g_m.ringsize, ring_cb,
                                   (ScePVoid)0x12345678);
    out("  = %08X\n", r);
    if (verbose)
        dump_ring("ring after Construct (the struct is 44 bytes; +2C is the word after it):");
    if (r < 0) return -1;

    step("sceMpegCreate(&mpeg, buffer, %d, ring, 512, 0, 0)", g_m.memsize);
    r = sceMpegCreate(&g_m.mpeg, g_m.mdata, g_m.memsize,
                      (SceMpegRingbuffer *)g_ringw, 512, 0, 0);
    out("  = %08X; handle %08X (handle - buffer = 0x%X)\n", r,
        (unsigned int)g_m.mpeg, (unsigned int)g_m.mpeg - (unsigned int)g_m.mdata);
    if (r < 0) return -1;
    if (verbose) dump_ring("ring after Create:");

    step("sceMpegRingbufferAvailableSize");
    r = sceMpegRingbufferAvailableSize((SceMpegRingbuffer *)g_ringw);
    out("  = %08X (%d)\n", r, r);

    step("sceMpegQueryAtracEsSize");
    g_m.es = g_m.esout = -1;
    r = sceMpegQueryAtracEsSize(&g_m.mpeg, &g_m.es, &g_m.esout);
    out("  = %08X: es size %d (0x%X), output size %d (0x%X)\n", r, (int)g_m.es,
        (unsigned int)g_m.es, (int)g_m.esout, (unsigned int)g_m.esout);

    for (int i = 0; i < 8; i++) g_avcau[i] = g_atrau[i] = 0xCCCCCCCCu;
    step("sceMpegMallocAvcEsBuf");
    g_m.avces = sceMpegMallocAvcEsBuf(&g_m.mpeg);
    out("  = %08X\n", (unsigned int)g_m.avces);
    step("sceMpegInitAu(video)");
    r = sceMpegInitAu(&g_m.mpeg, g_m.avces, (SceMpegAu *)g_avcau);
    out("  = %08X\n", r);
    if (verbose)
        dump_words("video AU after InitAu (PSPSDK: pts msb, pts, dts msb, dts, es, size; +18 is past it):",
                   g_avcau, 8);

    g_m.atres = memalign(64, g_m.es > 0 ? g_m.es : 4096);
    step("sceMpegInitAu(audio, es buffer %08X)", (unsigned int)g_m.atres);
    r = sceMpegInitAu(&g_m.mpeg, g_m.atres, (SceMpegAu *)g_atrau);
    out("  = %08X\n", r);
    if (verbose) dump_words("audio AU after InitAu:", g_atrau, 8);

    g_m.ok = 1;
    return 0;
}

static void mpeg_teardown(void) {
    if (!g_m.ok) return;
    step("sceMpegDelete");
    sceMpegDelete(&g_m.mpeg);
    step("sceMpegRingbufferDestruct");
    sceMpegRingbufferDestruct((SceMpegRingbuffer *)g_ringw);
    step("sceMpegFinish");
    sceMpegFinish();
    g_m.ok = 0;
}

/* ---- test X: sizes and structures ------------------------------------------ */

static void test_mpeg(void) {
    if (mpeg_setup(1) == 0) mpeg_teardown();
}

/* ---- test SQUARE: a real movie --------------------------------------------- */

static unsigned int be32(const unsigned char *p) {
    return (unsigned int)p[0] << 24 | (unsigned int)p[1] << 16 |
           (unsigned int)p[2] << 8 | p[3];
}

static void put_be32(unsigned char *p, unsigned int v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

/* Which header words QueryStreamOffset/Size read, and what they reject:
 * change one word of a copy of the real header at a time. */
static void psmf_header_tests(const unsigned char *hdr) {
    static unsigned char h[2048] __attribute__((aligned(64)));
    static const struct { const char *what; int off; unsigned int val; } edits[] = {
        { "unchanged",               -1, 0 },
        { "word 0x08 := 0x00001000", 0x08, 0x00001000u },
        { "word 0x08 := 0x00000800", 0x08, 0x00000800u },
        { "word 0x08 := 0x00000801", 0x08, 0x00000801u },
        { "word 0x08 := 0",          0x08, 0 },
        { "word 0x0C := 0x00123456", 0x0C, 0x00123456u },
        { "word 0x0C := 0",          0x0C, 0 },
        { "word 0x04 := 0",          0x04, 0 },
        { "word 0x00 := 0",          0x00, 0 },
    };
    SceInt32 v;
    int r;

    out("header words (big-endian): 00 %08X  04 %08X  08 %08X  0C %08X\n",
        be32(hdr), be32(hdr + 4), be32(hdr + 8), be32(hdr + 12));
    out("                           10 %08X  14 %08X  18 %08X  1C %08X\n",
        be32(hdr + 16), be32(hdr + 20), be32(hdr + 24), be32(hdr + 28));

    for (unsigned i = 0; i < sizeof edits / sizeof edits[0]; i++) {
        memcpy(h, hdr, sizeof h);
        if (edits[i].off >= 0) put_be32(h + edits[i].off, edits[i].val);

        step("QueryStreamOffset/Size, header %s", edits[i].what);
        v = -1;
        r = sceMpegQueryStreamOffset(&g_m.mpeg, h, &v);
        out("  QueryStreamOffset = %08X, offset %08X", (unsigned int)r, (unsigned int)v);
        v = -1;
        r = sceMpegQueryStreamSize(h, &v);
        out("; QueryStreamSize = %08X, size %08X\n", (unsigned int)r, (unsigned int)v);
    }
}

static void test_movie(void) {
    char path[300];
    snprintf(path, sizeof path, "%smovie.pmf", g_dir);
    g_movie = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (g_movie < 0) {
        say("no movie.pmf beside the EBOOT (%08X)\n", g_movie);
        return;
    }
    static unsigned char hdr[2048] __attribute__((aligned(64)));
    const int flen = sceIoLseek32(g_movie, 0, SEEK_END);
    sceIoLseek32(g_movie, 0, SEEK_SET);
    int r = sceIoRead(g_movie, hdr, sizeof hdr);
    out("movie.pmf: %d bytes; read %d header bytes\n", flen, r);

    if (mpeg_setup(0) < 0) goto done;

    psmf_header_tests(hdr);

    SceInt32 soff = 0;
    step("sceMpegQueryStreamOffset(real header)");
    r = sceMpegQueryStreamOffset(&g_m.mpeg, hdr, &soff);
    out("  = %08X, offset %08X\n", r, (unsigned int)soff);
    if (r < 0 || soff <= 0) goto teardown;

    SceMpegStream *s[2];
    for (int t = 0; t < 2; t++) {
        step("sceMpegRegistStream(type %d, 0)", t);
        s[t] = sceMpegRegistStream(&g_m.mpeg, t, 0);
        out("  = %08X\n", (unsigned int)s[t]);
    }
    /* A null or negative handle is an error code, not a stream. */
    const int have_v = s[0] && (int)s[0] > 0;
    const int have_a = s[1] && (int)s[1] > 0;

    sceIoLseek32(g_movie, soff, SEEK_SET);
    int navc = have_v ? 0 : AU_WANTED, natr = have_a ? 0 : AU_WANTED, puts = 0;
    unsigned int last_v = 1, last_a = 1;
    for (int it = 0; it < 2000 && (navc < AU_WANTED || natr < AU_WANTED); it++) {
        const int avail = sceMpegRingbufferAvailableSize((SceMpegRingbuffer *)g_ringw);
        if (avail > 0) {
            const int want = avail < PUT_MAX ? avail : PUT_MAX;
            if (puts < 12) step("sceMpegRingbufferPut(ring, %d, %d)", want, avail);
            r = sceMpegRingbufferPut((SceMpegRingbuffer *)g_ringw, want, avail);
            if (puts < 12) {
                out("  = %08X\n", r);
                dump_ring("ring after Put:");
            }
            puts++;
            if (r <= 0 && navc == 0 && natr == 0 && puts > 64) break;
        }

        if (navc < AU_WANTED) {
            SceInt32 attr = (SceInt32)0xCCCCCCCC;
            if (navc == 0 && last_v == 1) step("sceMpegGetAvcAu (first call)");
            r = sceMpegGetAvcAu(&g_m.mpeg, s[0], (SceMpegAu *)g_avcau, &attr);
            if (r == 0) {
                char l[96];
                snprintf(l, sizeof l, "GetAvcAu #%d = 0, 4th arg now %08X; AU:",
                         navc, (unsigned int)attr);
                dump_words(l, g_avcau, 6);
                navc++;
            } else if ((unsigned int)r != last_v) {
                out("GetAvcAu = %08X (after %d AUs, %d puts)\n", r, navc, puts);
            }
            last_v = (unsigned int)r;
        }
        if (natr < AU_WANTED) {
            static unsigned int attr2[4];
            for (int i = 0; i < 4; i++) attr2[i] = 0xCCCCCCCCu;
            if (natr == 0 && last_a == 1) step("sceMpegGetAtracAu (first call)");
            r = sceMpegGetAtracAu(&g_m.mpeg, s[1], (SceMpegAu *)g_atrau, attr2);
            if (r == 0) {
                char l[96];
                snprintf(l, sizeof l, "GetAtracAu #%d = 0, 4th arg now %08X; AU:",
                         natr, attr2[0]);
                dump_words(l, g_atrau, 6);
                natr++;
            } else if ((unsigned int)r != last_a) {
                out("GetAtracAu = %08X (after %d AUs, %d puts)\n", r, natr, puts);
            }
            last_a = (unsigned int)r;
        }
    }
    out("stopped after %d puts\n", puts);
    say("%d video and %d audio access units logged\n",
        have_v ? navc : 0, have_a ? natr : 0);

teardown:
    mpeg_teardown();
done:
    sceIoClose(g_movie);
    g_movie = -1;
}

/* ---- test TRIANGLE: GE block transfer --------------------------------------- */

static unsigned int g_list[1024] __attribute__((aligned(64)));

#define XSRC_BYTES (64 * 1024)
#define XDST_BYTES (512 * 1024)

typedef struct {
    const char *name;
    int psm;
    unsigned srcoff, dstoff;
    int sx, sy, w, h, srcw, dx, dy, dstw;
} xcase;

/* Ordinary cases first, so a case that upsets the GE is reached last. */
static const xcase xcases[] = {
    /* name                   psm           src dst  sx sy  w  h  srcw   dx dy dstw  */
    { "baseline, 32-bit",     GU_PSM_8888,  0,  0,   0, 0,  4, 2, 64,    0, 0, 64 },
    { "baseline, 16-bit",     GU_PSM_5650,  0,  0,   0, 0,  8, 2, 64,    0, 0, 64 },
    { "src stride 0x3F8",     GU_PSM_8888,  0,  0,   0, 1,  4, 1, 0x3F8, 0, 0, 64 },
    { "src stride 0x400",     GU_PSM_8888,  0,  0,   0, 1,  4, 1, 0x400, 0, 0, 64 },
    { "src address +4",       GU_PSM_8888,  4,  0,   0, 0,  4, 1, 64,    0, 0, 64 },
    { "src address +8",       GU_PSM_8888,  8,  0,   0, 0,  4, 1, 64,    0, 0, 64 },
    { "src address +12",      GU_PSM_8888,  12, 0,   0, 0,  4, 1, 64,    0, 0, 64 },
    { "16-bit src +2",        GU_PSM_5650,  2,  0,   0, 0,  8, 1, 64,    0, 0, 64 },
    { "16-bit src +4",        GU_PSM_5650,  4,  0,   0, 0,  8, 1, 64,    0, 0, 64 },
    { "src stride 0x44",      GU_PSM_8888,  0,  0,   0, 1,  4, 1, 0x44,  0, 0, 64 },
    { "src stride 0x404",     GU_PSM_8888,  0,  0,   0, 1,  4, 1, 0x404, 0, 0, 64 },
    { "src stride 0x408",     GU_PSM_8888,  0,  0,   0, 1,  4, 1, 0x408, 0, 0, 64 },
    { "src stride 0x7F8",     GU_PSM_8888,  0,  0,   0, 1,  4, 1, 0x7F8, 0, 0, 64 },
    { "src stride 0x808",     GU_PSM_8888,  0,  0,   0, 1,  4, 1, 0x808, 0, 0, 64 },
    { "dst address +4",       GU_PSM_8888,  0,  4,   0, 0,  4, 1, 64,    0, 0, 64 },
    { "dst address +8",       GU_PSM_8888,  0,  8,   0, 0,  4, 1, 64,    0, 0, 64 },
    { "16-bit dst +2",        GU_PSM_5650,  0,  2,   0, 0,  8, 1, 64,    0, 0, 64 },
    { "dst stride 0x408",     GU_PSM_8888,  0,  0,   0, 0,  4, 1, 64,    0, 1, 0x408 },
    { "dst stride 0x808",     GU_PSM_8888,  0,  0,   0, 0,  4, 1, 64,    0, 1, 0x808 },
};

static int ge_wait(void) {
    for (int i = 0; i < 500; i++) {
        if (sceGuSync(GU_SYNC_FINISH, GU_SYNC_NOWAIT) == 0) return 0;
        sceKernelDelayThread(1000);
    }
    return -1;
}

static void test_ge(void) {
    out("source pixels hold their own index: 32-bit 0x5A000000 | i, 16-bit 0x8000 | i.\n");
    out("each line: where in the destination a pixel landed <- which source pixel it holds\n");

    static unsigned char *src, *dst;
    if (!src) src = memalign(64, XSRC_BYTES);
    if (!dst) dst = memalign(64, XDST_BYTES);
    if (!src || !dst) { say("out of memory\n"); return; }
    /* From here on both are touched only through uncached addresses. */
    sceKernelDcacheWritebackInvalidateAll();

    static int gu_ready;
    if (!gu_ready) {
        step("sceGuInit");
        sceGuInit();
        sceGuStart(GU_DIRECT, g_list);
        sceGuFinish();
        if (ge_wait() < 0) { say("the GE did not start; stopped\n"); return; }
        gu_ready = 1;
    }

    for (unsigned c = 0; c < sizeof xcases / sizeof xcases[0]; c++) {
        const xcase *x = &xcases[c];
        const int wide = x->psm == GU_PSM_8888;
        volatile unsigned int   *s32 = UNCACHED(src), *d32 = UNCACHED(dst);
        volatile unsigned short *s16 = UNCACHED(src), *d16 = UNCACHED(dst);

        if (wide) for (int i = 0; i < XSRC_BYTES / 4; i++) s32[i] = 0x5A000000u | i;
        else      for (int i = 0; i < XSRC_BYTES / 2; i++) s16[i] = 0x8000u | (i & 0x7FFF);
        for (int i = 0; i < XDST_BYTES / 4; i++) d32[i] = 0;

        step("GE copy: %s", x->name);
        sceGuStart(GU_DIRECT, g_list);
        sceGuCopyImage(x->psm, x->sx, x->sy, x->w, x->h, x->srcw, src + x->srcoff,
                       x->dx, x->dy, x->dstw, dst + x->dstoff);
        sceGuTexSync();
        sceGuFinish();
        const int hung = ge_wait() < 0;

        out("  src base+%u stride 0x%X at (%d,%d), dst base+%u stride 0x%X at (%d,%d), %dx%d%s\n",
            x->srcoff, x->srcw, x->sx, x->sy, x->dstoff, x->dstw,
            x->dx, x->dy, x->w, x->h, hung ? "  ** GE DID NOT FINISH **" : "");
        int shown = 0, total = 0;
        if (wide) {
            for (int j = 0; j < XDST_BYTES / 4; j++) {
                if (!d32[j]) continue;
                if (shown++ < 12) {
                    if ((d32[j] & 0xFF000000u) == 0x5A000000u)
                        out("   dst byte +0x%05X <- src byte +0x%05X\n", j * 4,
                            (d32[j] & 0xFFFFFFu) * 4);
                    else
                        out("   dst byte +0x%05X <- %08X (not a source pixel)\n", j * 4, d32[j]);
                }
                total++;
            }
        } else {
            for (int j = 0; j < XDST_BYTES / 2; j++) {
                if (!d16[j]) continue;
                if (shown++ < 12) {
                    if (d16[j] & 0x8000u)
                        out("   dst byte +0x%05X <- src byte +0x%05X\n", j * 2,
                            (d16[j] & 0x7FFFu) * 2);
                    else
                        out("   dst byte +0x%05X <- %04X (not a source pixel)\n", j * 2, d16[j]);
                }
                total++;
            }
        }
        out("   %d pixels written in total\n", total);
        if (hung) { say("the GE hung on \"%s\"; stopped\n", x->name); return; }
    }
}

/* ---- main ----------------------------------------------------------------- */

static unsigned int wait_buttons(void) {
    SceCtrlData pad;
    /* Let go of whatever started the last test first. */
    do { sceCtrlReadBufferPositive(&pad, 1); } while (pad.Buttons & 0xF000u);
    for (;;) {
        sceCtrlReadBufferPositive(&pad, 1);
        const unsigned int b = pad.Buttons & (PSP_CTRL_CROSS | PSP_CTRL_SQUARE |
                                              PSP_CTRL_TRIANGLE | PSP_CTRL_CIRCLE);
        if (b) return b;
        sceDisplayWaitVblankStart();
    }
}

static void menu_text(void) {
    pspDebugScreenClear();
    pspDebugScreenPrintf("mpegprobe %d   firmware %08X\n", PROBE_VERSION,
                         sceKernelDevkitVersion());
    pspDebugScreenPrintf("log: %s\n\n", g_logpath);
    pspDebugScreenPrintf("Run one test at a time. If the PSP switches off,\n");
    pspDebugScreenPrintf("start the probe again and run the next test.\n\n");
    pspDebugScreenPrintf("  X         MPEG sizes and structures\n");
    pspDebugScreenPrintf("  SQUARE    movie test (needs movie.pmf)\n");
    pspDebugScreenPrintf("  TRIANGLE  GE image copies\n");
    pspDebugScreenPrintf("  CIRCLE    quit\n");
}

int main(int argc, char *argv[]) {
    int th = sceKernelCreateThread("exit_cb", cb_thread, 0x11, 0xFA0, 0, 0);
    if (th >= 0) sceKernelStartThread(th, 0, 0);

    pspDebugScreenInit();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);

    /* argv[0] is the EBOOT's own path; the log and movie.pmf live beside it. */
    if (argc > 0 && argv[0]) {
        const char *slash = strrchr(argv[0], '/');
        if (slash && (size_t)(slash - argv[0] + 1) < sizeof g_dir) {
            memcpy(g_dir, argv[0], slash - argv[0] + 1);
            g_dir[slash - argv[0] + 1] = 0;
        }
    }
    snprintf(g_logpath, sizeof g_logpath, "%smpegprobe.txt", g_dir);

    out("\n==== mpegprobe %d, firmware (sceKernelDevkitVersion) %08X ====\n",
        PROBE_VERSION, sceKernelDevkitVersion());
    flush_log();

    for (;;) {
        menu_text();
        const unsigned int b = wait_buttons();
        if (b & PSP_CTRL_CIRCLE) break;

        pspDebugScreenClear();
        g_step = 0;
        if (b & PSP_CTRL_CROSS) {
            out("\n---- test X: MPEG sizes and structures ----\n");
            test_mpeg();
        } else if (b & PSP_CTRL_SQUARE) {
            out("\n---- test SQUARE: movie ----\n");
            test_movie();
        } else {
            out("\n---- test TRIANGLE: GE image copies ----\n");
            test_ge();
        }
        out("---- end of test ----\n");
        flush_log();
        pspDebugScreenPrintf("\nTest finished. Press any button for the menu.\n");
        wait_buttons();
    }

    flush_log();
    sceKernelExitGame();
    return 0;
}
