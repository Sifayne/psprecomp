/* sasprobe -- measure sceSasCore, the PSP's voice mixer, on real hardware.
 *
 * Written against PSPSDK (BSD) only. Each section re-measures rules that
 * psprecomp's src/hle/sascore.c states as hardware facts; the comment above a
 * step names the line and the claim it checks. It runs once from the XMB with
 * no input. The log is sasprobe.txt beside the EBOOT. The audio of the decode
 * tests is saved beside it as raw little-endian s16 (*.bin, stereo frames
 * unless the name says mode 1), so psprecomp's output from the same PRX can
 * be compared sample by sample.
 *
 * In the log: return codes and envelope heights are hex, samples are signed
 * decimal, and "L[i]" is output frame i counted from the first core after the
 * key-on. A SasCore word that points into memory is shown as an offset from
 * one of the probe's own buffers ("vag+0", "u:core+40" for an uncached
 * alias), or as "ptr" when it points anywhere else.
 *
 * fresh() starts most tests: __sceSasInit, then every voice unpaused, silenced
 * and keyed off with an instant release, then one core. On a firmware whose
 * Init resets the voices this changes nothing; on one that keeps them (as
 * psprecomp's does) it stops one test's voices sounding in the next. Whether
 * Init resets them is measured on its own in the init section.
 */
#include <pspkernel.h>
#include <psputility.h>
#include <psputils.h>
#include <pspsascore.h>

#include <stdio.h>
#include <string.h>

#include "probe.h"

PSP_MODULE_INFO("sasprobe", PSP_MODULE_USER, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(512);

#define PROBE_VERSION 1

typedef unsigned int w32;   /* PSPSDK's u32 is long; this prints with %X */

#define NOT_LINKED 0x8002013Au   /* an import nothing has linked yet */

/* ---- buffers ---------------------------------------------------------------
 *
 * All static and 64-byte aligned. The SasCore struct gets 16 KB although
 * PSPSDK sizes it at 3616 bytes (psprecomp's mirror reaches 1812), so a
 * bigger struct, or an unaligned core that is accepted, still lands inside
 * memory the probe owns. Every sample source has 64 bytes before it that the
 * probe owns too. */

#define CORE_BYTES 16384
static unsigned char g_core_mem[CORE_BYTES] __attribute__((aligned(64)));
static unsigned char g_snap[CORE_BYTES] __attribute__((aligned(64)));
static SceSasCore *core;

#define OUT_SHORTS 20480   /* grain 2048 in output mode 1 is 8192 */
static short g_out[OUT_SHORTS] __attribute__((aligned(64)));
#define CAP_SHORTS 65536
static short g_cap[CAP_SHORTS] __attribute__((aligned(64)));
static int g_capn;         /* shorts captured by the last render */

#define VAG_BYTES 2048
static unsigned char g_vagmem[VAG_BYTES + 128] __attribute__((aligned(64)));
static unsigned char g_vag2mem[VAG_BYTES + 128] __attribute__((aligned(64)));
#define VAG  (g_vagmem + 64)
#define VAG2 (g_vag2mem + 64)

#define RAMP_N 8000        /* RAMP[i] = 4 * (i + 1) */
static short g_rampmem[RAMP_N + 64] __attribute__((aligned(64)));
#define RAMP (g_rampmem + 32)
#define R16_N 224          /* R16[i] = 16 * (i + 1) */
static short g_r16mem[R16_N + 64] __attribute__((aligned(64)));
#define R16 (g_r16mem + 32)
#define PCM_N 1024
static short g_pcmmem[4][PCM_N + 64] __attribute__((aligned(64)));
#define PCM(k) (g_pcmmem[k] + 32)   /* PCM(0) is always 0x1000 */

static int g_hts[48] __attribute__((aligned(64)));

static int g_grain = 256, g_mode = 0;

/* Where hardware keeps voice 0's ADSR fields, found in the layout section;
 * psprecomp's claim (sascore.c:72-76) until then. */
static int off_ar = 20 + 24, off_dr = 20 + 28, off_sr = 20 + 32, off_rr = 20 + 36;
static int off_sl = 20 + 40;
static int off_mode[4] = { 20 + 44, -1, -1, -1 };
static int n_mode = 1;

/* ---- plumbing --------------------------------------------------------------*/

static void sync(void) { sceKernelDcacheWritebackInvalidateAll(); }
static w32 cw(int off) { return *(volatile w32 *)(g_core_mem + off); }
static w32 rd(int off) { sync(); return off >= 0 ? cw(off) : 0xFFFFFFFFu; }

static const struct { const char *name; const void *base; unsigned size; } g_ranges[] = {
    { "core", g_core_mem, sizeof g_core_mem },
    { "out",  g_out,      sizeof g_out },
    { "cap",  g_cap,      sizeof g_cap },
    { "vag",  g_vagmem + 64,  VAG_BYTES + 64 },
    { "vag2", g_vag2mem + 64, VAG_BYTES + 64 },
    { "ramp", g_rampmem + 32, RAMP_N * 2 + 64 },
    { "r16",  g_r16mem + 32,  R16_N * 2 + 64 },
    { "pcm0", g_pcmmem[0] + 32, PCM_N * 2 + 64 },
    { "pcm1", g_pcmmem[1] + 32, PCM_N * 2 + 64 },
    { "pcm2", g_pcmmem[2] + 32, PCM_N * 2 + 64 },
    { "pcm3", g_pcmmem[3] + 32, PCM_N * 2 + 64 },
    { "hts",  g_hts, sizeof g_hts },
};

/* A struct word as it may be logged: an address becomes an offset. */
static char *norm(w32 w, char *b) {
    const w32 top = w >> 29;
    if (top == 0 || top == 2 || top == 4 || top == 5) {
        const w32 phys = w & 0x1FFFFFFFu;
        if (phys >= 0x08000000u && phys < 0x0A000000u) {
            for (unsigned i = 0; i < sizeof g_ranges / sizeof g_ranges[0]; i++) {
                const w32 base = (w32)g_ranges[i].base & 0x1FFFFFFFu;
                if (phys >= base && phys < base + g_ranges[i].size) {
                    sprintf(b, "%s%s+%X", (top == 2 || top == 5) ? "u:" : top == 4 ? "k:" : "",
                            g_ranges[i].name, phys - base);
                    return b;
                }
            }
            strcpy(b, "ptr");
            return b;
        }
    }
    sprintf(b, "%08X", w);
    return b;
}

static char *offs(int o, char *b) {
    if (o < 0) strcpy(b, "none"); else sprintf(b, "+%X", o);
    return b;
}

/* ---- struct views ---------------------------------------------------------*/

static int g_chg[8], g_nchg;

/* Bytes of the struct no longer 0xCC, as an extent, and words changed. */
static int core_extent_cc(int *changed) {
    int last = -1, ch = 0;
    for (int i = 0; i < CORE_BYTES; i++) if (g_core_mem[i] != 0xCC) last = i;
    for (int i = 0; i < CORE_BYTES / 4; i++)
        if (((const w32 *)g_core_mem)[i] != 0xCCCCCCCCu) ch++;
    *changed = ch;
    return last + 1;
}

/* The struct from `from` to `to`, runs of three or more equal words folded. */
static void dump_struct(int from, int to) {
    char cur[32], nxt[32];
    int i = from / 4, end = to / 4, col = 0;
    while (i < end) {
        norm(cw(i * 4), cur);
        int j = i + 1;
        while (j < end && strcmp(norm(cw(j * 4), nxt), cur) == 0) j++;
        if (col == 0) out("   ");
        if (j - i >= 3) { out(" +%03X..%03X=%s", i * 4, (j - 1) * 4, cur); i = j; }
        else            { out(" +%03X=%s", i * 4, cur); i++; }
        if (++col == 4) { out("\n"); col = 0; }
    }
    if (col) out("\n");
}

static void snap(void) { sync(); memcpy(g_snap, g_core_mem, CORE_BYTES); }

/* Log the words that changed since the last snap(), then snap. */
static void diff(void) {
    char s1[32], s2[32];
    const w32 *a = (const w32 *)g_snap, *b = (const w32 *)g_core_mem;
    int n = 0, shown = 0;
    sync();
    g_nchg = 0;
    for (int i = 0; i < CORE_BYTES / 4; i++)
        if (a[i] != b[i]) { if (g_nchg < 8) g_chg[g_nchg++] = i * 4; n++; }
    out("  struct: %d words changed\n", n);
    for (int i = 0; i < CORE_BYTES / 4 && shown < 24; i++) {
        if (a[i] == b[i]) continue;
        if (shown % 3 == 0) out("   ");
        out(" +%03X %s>%s", i * 4, norm(a[i], s1), norm(b[i], s2));
        if (++shown % 3 == 0) out("\n");
    }
    if (shown % 3) out("\n");
    if (n > shown) out("    ... %d more\n", n - shown);
    memcpy(g_snap, g_core_mem, CORE_BYTES);
}

static int find_word(w32 val, int from) {
    sync();
    for (int o = from < 0 ? 0 : from; o + 4 <= CORE_BYTES; o += 4) if (cw(o) == val) return o;
    return -1;
}

/* ---- rendering ------------------------------------------------------------*/

static int fill_n(void) {
    int n = g_grain * 4 + 64;
    if (n > OUT_SHORTS || n < 64) n = OUT_SHORTS;
    return n;
}

/* One __sceSasCore into g_out, prefilled with 0xA5A5 so the extent shows. */
static int core_raw(void) {
    const int n = fill_n();
    for (int i = 0; i < n; i++) g_out[i] = (short)0xA5A5;
    sync();
    int r = __sceSasCore(core, g_out);
    sync();
    return r;
}

static int out_extent(void) {
    int n = fill_n();
    while (n > 0 && (unsigned short)g_out[n - 1] == 0xA5A5) n--;
    return n;
}

static int grain_words(void) { return g_mode == 1 ? g_grain * 4 : g_grain * 2; }

#define MAXG 128
static w32 g_ef[MAXG];
static w32 g_h[MAXG];

/* `n` cores into g_cap, with the end flags and voice `hv`'s height after
 * each; voice `kv` is keyed off just before core `koff`. */
static int render_ko(int n, int hv, int koff, int kv) {
    const int w = grain_words();
    if (n > MAXG) n = MAXG;
    if (n * w > CAP_SHORTS) n = CAP_SHORTS / w;
    for (int i = 0; i < n; i++) {
        if (i == koff) {
            int r = __sceSasSetKeyOff(core, kv);
            if (r) out("  KeyOff before core %d = %08X\n", i, (w32)r);
        }
        int r = core_raw();
        if (r) out("  core %d = %08X\n", i, (w32)r);
        memcpy(g_cap + i * w, g_out, w * 2);
        g_ef[i] = (w32)__sceSasGetEndFlag(core);
        g_h[i] = hv >= 0 ? (w32)__sceSasGetEnvelopeHeight(core, hv) : 0;
    }
    g_capn = n * w;
    return n;
}
static int render(int n, int hv) { return render_ko(n, hv, -1, 0); }

static w32 fnv(const void *p, int len) {
    const unsigned char *b = p;
    w32 h = 2166136261u;
    for (int i = 0; i < len; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

#define CL(i) (g_cap[2 * (i)])
#define CR(i) (g_cap[2 * (i) + 1])
static int frames(void) { return g_capn / 2; }

static void log_sum(void) { out("  sum %08X over %d bytes\n", fnv(g_cap, g_capn * 2), g_capn * 2); }

/* Channel `ch` (0 L, 1 R) of frames from..from+count-1, eight to a line. */
static void log_ch(int ch, int from, int count) {
    for (int i = 0; i < count; i++) {
        const int f = from + i;
        if (i % 8 == 0) out("  %c[%d]:", ch ? 'R' : 'L', f);
        out(" %d", f < frames() ? g_cap[2 * f + ch] : 0);
        if (i % 8 == 7 || i == count - 1) out("\n");
    }
}

static int first_nz(void) {
    for (int i = 0; i < frames(); i++) if (CL(i) || CR(i)) return i;
    return -1;
}

static int lr_diff(void) {
    int n = 0;
    for (int i = 0; i < frames(); i++) if (CL(i) != CR(i)) n++;
    return n;
}

static void log_end(int v, int n) {
    char s[MAXG + 1];
    if (n > MAXG) n = MAXG;
    for (int i = 0; i < n; i++) s[i] = ((g_ef[i] >> v) & 1) ? '1' : '0';
    s[n] = 0;
    out("  end flag v%d after each core: %s\n", v, s);
}

static void log_heights(int n) {
    for (int i = 0; i < n; i++) {
        if (i % 8 == 0) out("  h[%d]:", i);
        out(" %08X", g_h[i]);
        if (i % 8 == 7 || i == n - 1) out("\n");
    }
}

static void save(const char *name) {
    int r = probe_write_file(name, g_cap, g_capn * 2);
    out("  saved %s: %d bytes\n", name, r);
}

/* Runs of equal L samples, "value*count". */
static void log_rle(int nframes) {
    int i = 0, tok = 0;
    out("  L runs:");
    while (i < nframes) {
        const int v = CL(i);
        int j = i + 1;
        while (j < nframes && CL(j) == v) j++;
        if (tok == 60) { out(" ..."); break; }
        if (tok && tok % 10 == 0) out("\n         ");
        out(" %d*%d", v, j - i);
        tok++;
        i = j;
    }
    out("\n");
}

/* L read as an index into R16: z<n> is n silent frames, a-b a rising run,
 * a*n a held value, a-b/d a run stepping d, ?v a value that is no index. */
static int idx16(int v) {
    if (v == 0) return -1;
    if (v < 0 || (v & 15)) return -2;
    return v / 16 - 1;
}
static void log_ranges(int nframes) {
    int i = 0, tok = 0;
    out("  L as R16 index:");
    while (i < nframes) {
        if (tok == 40) { out(" ..."); break; }
        if (tok && tok % 10 == 0) out("\n                 ");
        const int k = idx16(CL(i));
        if (k == -1) {
            int j = i;
            while (j < nframes && CL(j) == 0) j++;
            out(" z%d", j - i);
            i = j;
        } else if (k == -2) {
            const int v = CL(i);
            int j = i + 1;
            while (j < nframes && CL(j) == v) j++;
            if (j - i > 1) out(" ?%d*%d", v, j - i); else out(" ?%d", v);
            i = j;
        } else {
            int j = i + 1, d = 0;
            if (j < nframes && idx16(CL(j)) >= 0) {
                d = idx16(CL(j)) - k;
                while (j < nframes && idx16(CL(j)) >= 0 && idx16(CL(j)) - idx16(CL(j - 1)) == d) j++;
            }
            const int last = idx16(CL(j - 1)), n = j - i;
            if (n == 1)      out(" %d", k);
            else if (d == 0) out(" %d*%d", k, n);
            else if (d == 1) out(" %d-%d", k, last);
            else             out(" %d-%d/%d", k, last, d);
            i = j;
        }
        tok++;
    }
    out("\n");
}

/* ---- voice setup ----------------------------------------------------------*/

#define NOSL 0x7EEEEEEE   /* env(): leave the sustain level alone */

static const char *const CURVE[6] = { "lin-inc", "lin-dec", "bent", "exp-rev", "exp", "direct" };

static void env(int v, int am, int ar, int dm, int dr, int sm, int sr, int rm, int rr, int sl) {
    int r1 = __sceSasSetADSRmode(core, v, 0xF, am, dm, sm, rm);
    int r2 = __sceSasSetADSR(core, v, 0xF, ar, dr, sr, rr);
    int r3 = sl == NOSL ? 0 : __sceSasSetSL(core, v, sl);
    if (r1 || r2 || r3)
        out("  (env v%d: ADSRmode %08X ADSR %08X SL %08X)\n", v, (w32)r1, (w32)r2, (w32)r3);
}

/* Full height from the second sample on: an attack that gets there in one
 * step, then a decay and sustain that do not move. */
static void env_flat(int v) { env(v, 0, 0x7FFFFFFF, 1, 0, 1, 0, 1, 0, 0x40000000); }

static void pitch_vol(int v) {
    int r1 = __sceSasSetPitch(core, v, 0x1000);
    int r2 = __sceSasSetVolume(core, v, 0x1000, 0x1000, 0, 0);
    if (r1 || r2) out("  (v%d: SetPitch %08X SetVolume %08X)\n", v, (w32)r1, (w32)r2);
}

static void pcm_voice(int v, const short *p, int n, int loop) {
    sync();   /* the samples reach memory before the firmware can look */
    int r = __sceSasSetVoicePCM(core, v, (void *)p, n, loop);
    if (r) out("  (SetVoicePCM v%d = %08X)\n", v, (w32)r);
    pitch_vol(v);
}

static void vag_voice(int v, unsigned char *b, int size, int loop) {
    sync();
    int r = __sceSasSetVoice(core, v, b, size, loop);
    if (r) out("  (SetVoice v%d = %08X)\n", v, (w32)r);
    pitch_vol(v);
}

static int keyon(int v) {
    int r = __sceSasSetKeyOn(core, v);
    if (r) out("  (KeyOn v%d = %08X)\n", v, (w32)r);
    return r;
}

static void fresh(void) {
    int r = __sceSasInit(core, 256, 32, 0, 44100);
    if (r) out("  (fresh: Init = %08X)\n", (w32)r);
    /* Said again in case a repeated Init is refused. */
    __sceSasSetGrain(core, 256);
    __sceSasSetOutputmode(core, 0);
    g_grain = 256;
    g_mode = 0;
    __sceSasSetPause(core, 0xFFFFFFFFu, 0);
    for (int v = 0; v < 32; v++) {
        __sceSasSetVolume(core, v, 0, 0, 0, 0);
        __sceSasSetADSRmode(core, v, 8, 0, 1, 1, 1);
        __sceSasSetADSR(core, v, 8, 0, 0, 0, 0x7FFFFFFF);
        __sceSasSetKeyOff(core, v);
    }
    core_raw();
}

static void fill_const(short *p, int n, int v) { for (int i = 0; i < n; i++) p[i] = (short)v; }

/* ---- VAG building ---------------------------------------------------------*/

static int g_vn;   /* blocks written */

static void vag_clear(unsigned char *b) { memset(b - 64, 0, VAG_BYTES + 128); g_vn = 0; }

/* One 16-byte block: filter in the header's top nibble, shift in the low;
 * nibble i of 28 in byte 2 + i/2, low nibble first. */
static void vag_blk(unsigned char *b, int filter, int shift, int flags, const signed char *nib) {
    unsigned char *p = b + 16 * g_vn++;
    p[0] = (unsigned char)((filter << 4) | (shift & 15));
    p[1] = (unsigned char)flags;
    for (int i = 0; i < 14; i++)
        p[2 + i] = (unsigned char)((nib[2 * i] & 15) | ((nib[2 * i + 1] & 15) << 4));
}

static void vag_blk_c(unsigned char *b, int filter, int shift, int flags, int nibble) {
    signed char n[28];
    memset(n, nibble, sizeof n);
    vag_blk(b, filter, shift, flags, n);
}

/* ============================================================================
 * module
 * ==========================================================================*/

static void sec_module(void) {
    section("module");
    step("module: sceUtilityLoadModule(PSP_MODULE_AV_AVCODEC)");
    ret(sceUtilityLoadModule(PSP_MODULE_AV_AVCODEC));
    step("module: sceUtilityLoadModule(PSP_MODULE_AV_SASCORE)");
    ret(sceUtilityLoadModule(PSP_MODULE_AV_SASCORE));

    memset(g_core_mem, 0xCC, CORE_BYTES);
    sync();
    step("module: __sceSasInit(core, 256, 32, 0, 44100) on a struct filled with 0xCC");
    int r = __sceSasInit(core, 256, 32, 0, 44100);
    ret(r);
    if ((w32)r == NOT_LINKED) {
        step("module: not linked; sceUtilityLoadAvModule(AVCODEC) and (SASCORE)");
        ret(sceUtilityLoadAvModule(PSP_AV_MODULE_AVCODEC));
        ret(sceUtilityLoadAvModule(PSP_AV_MODULE_SASCORE));
        memset(g_core_mem, 0xCC, CORE_BYTES);
        sync();
        step("module: __sceSasInit again");
        r = __sceSasInit(core, 256, 32, 0, 44100);
        ret(r);
        if ((w32)r == NOT_LINKED) {
            say("sceSasCore is not linked; nothing more to measure.\n");
            probe_done();
        }
    }
}

/* ============================================================================
 * init
 * ==========================================================================*/

static int init_try(unsigned char *c, int g, int nv, int m, int rate) {
    char cs[16];
    int r = __sceSasInit((SceSasCore *)c, g, nv, m, rate);
    if (!c) strcpy(cs, "NULL"); else sprintf(cs, "core+%d", (int)(c - g_core_mem));
    out("  Init(%s, %d, %d, %d, %d) = %08X", cs, g, nv, m, rate, (w32)r);
    return r;
}

static void sec_init(void) {
    unsigned char *c0 = g_core_mem;
    int ch, ext;
    section("init");

    /* sascore.c:63-76: the caller's struct is live state, a 20-byte header
     * and 56-byte voices. How much of a 0xCC-filled buffer the first Init
     * wrote (PSPSDK sizes SceSasCore at 3616 bytes), and what. */
    step("init: bytes the first Init wrote into the 0xCC-filled struct");
    sync();
    ext = core_extent_cc(&ch);
    out("  written extent 0x%X bytes, %d words changed\n", ext, ch);
    dump_struct(0, (ext + 15) & ~15);

    step("init: Init again on a 0xCC-filled struct; words that differ from the first Init");
    memcpy(g_snap, g_core_mem, CORE_BYTES);
    memset(g_core_mem, 0xCC, CORE_BYTES);
    sync();
    ret(__sceSasInit(core, 256, 32, 0, 44100));
    sync();
    ext = core_extent_cc(&ch);
    out("  written extent 0x%X bytes, %d words changed\n", ext, ch);
    diff();

    step("init: state after Init, before any core");
    out("  GetGrain %d GetOutputmode %d GetEndFlag %08X GetPauseFlag %08X height v0 %08X v31 %08X\n",
        __sceSasGetGrain(core), __sceSasGetOutputmode(core), (w32)__sceSasGetEndFlag(core),
        (w32)__sceSasGetPauseFlag(core), (w32)__sceSasGetEnvelopeHeight(core, 0),
        (w32)__sceSasGetEnvelopeHeight(core, 31));

    /* sascore.c:22-23,61,491: grain outside 64..2048 or off a multiple of 32
     * refused with 80420001 (PSPSDK's header says a multiple of 64). */
    step("init: grain values (claim 80420001 outside 64..2048 or off a multiple of 32; sascore.c:22-23,491)");
    {
        static const int gs[] = { 0, 32, 63, 64, 65, 96, 100, 128, 160, 192, 1024, 2016, 2048,
                                  2049, 2080, 4096, -1, -64 };
        for (unsigned i = 0; i < sizeof gs / sizeof gs[0]; i++) {
            init_try(c0, gs[i], 32, 0, 44100);
            out(", GetGrain %d\n", __sceSasGetGrain(core));
        }
    }
    /* sascore.c:23-24,492: voices outside 1..32 refused with 80420002. */
    step("init: maxVoices values (claim 80420002 outside 1..32; sascore.c:23-24,492)");
    {
        static const int vs[] = { 0, 1, 2, 16, 31, 32, 33, 64, -1 };
        for (unsigned i = 0; i < sizeof vs / sizeof vs[0]; i++) { init_try(c0, 256, vs[i], 0, 44100); out("\n"); }
    }
    /* sascore.c:24-25,493: an output mode other than 0 or 1 refused with 80420003. */
    step("init: outputMode values (claim 80420003 for other than 0 and 1; sascore.c:24-25,493)");
    {
        static const int ms[] = { 0, 1, 2, 3, -1 };
        for (unsigned i = 0; i < sizeof ms / sizeof ms[0]; i++) {
            init_try(c0, 256, 32, ms[i], 44100);
            out(", GetOutputmode %d\n", __sceSasGetOutputmode(core));
        }
    }
    /* sascore.c:25,494-497: 44100 only; 48000 refused with 80420004. */
    step("init: sampleRate values (claim only 44100, 48000 refused, 80420004; sascore.c:25,494-497)");
    {
        static const int rs[] = { 44100, 48000, 44099, 44101, 22050, 11025, 32000, 24000, 88200, 0, -1 };
        for (unsigned i = 0; i < sizeof rs / sizeof rs[0]; i++) { init_try(c0, 256, 32, 0, rs[i]); out("\n"); }
    }
    /* sascore.c:25-26,490: an unaligned core refused with 80420005. The
     * offsets stay inside the probe's 16 KB. */
    step("init: core alignment (claim 80420005 unless 64-byte aligned; sascore.c:25-26,490)");
    {
        static const int os[] = { 64, 128, 32, 16, 8, 4, 2, 1 };
        for (unsigned i = 0; i < sizeof os / sizeof os[0]; i++) { init_try(c0 + os[i], 256, 32, 0, 44100); out("\n"); }
    }
    /* sascore.c:484-493: "checked in the order sascore.expected reports them". */
    step("init: which bad argument wins (claim core, grain, voices, mode, rate; sascore.c:484-497)");
    init_try(c0 + 4, 0, 0, 2, 0);   out("\n");
    init_try(c0, 0, 0, 2, 0);       out("\n");
    init_try(c0, 256, 0, 2, 0);     out("\n");
    init_try(c0, 256, 32, 2, 0);    out("\n");
    init_try(c0, 0, 32, 0, 0);      out("\n");
    init_try(c0, 0, 32, 2, 44100);  out("\n");
    init_try(c0, 256, 0, 0, 48000); out("\n");
    init_try(c0 + 4, 256, 32, 0, 48000); out("\n");
    init_try(c0 + 4, 0, 32, 0, 44100);   out("\n");
    step("init: Init(core, 256, 32, 0, 44100) again");
    ret(__sceSasInit(core, 256, 32, 0, 44100));
    g_grain = 256; g_mode = 0;

    /* The defaults a voice has after Init: nothing set but its sample. No
     * voice has been touched since the first Init, so this is the same on a
     * firmware whose Init keeps voice state. */
    step("init: defaults: SetVoicePCM(v0, R16, 100, loop 0) and KeyOn only; 6 cores, key off before core 3");
    sync();
    out("  SetVoicePCM %08X", (w32)__sceSasSetVoicePCM(core, 0, R16, 100, 0));
    out(" KeyOn %08X\n", (w32)__sceSasSetKeyOn(core, 0));
    render_ko(6, 0, 3, 0);
    log_heights(6);
    log_end(0, 6);
    log_sum();
    log_ch(0, 28, 16);
    log_ch(1, 28, 16);
    log_ranges(6 * 256);

    /* psprecomp's Init (sascore.c:483-503) sets the globals and leaves every
     * voice as it was. What hardware's does to a keyed, paused voice. */
    step("init: key on v0 (PCM loop), pause v1, SetGrain 128, 2 cores, then Init again");
    fresh();
    pcm_voice(0, PCM(0), 256, 0);
    env_flat(0);
    pcm_voice(1, PCM(0), 256, 0);
    env_flat(1);
    out("  KeyOn v0 %08X", (w32)__sceSasSetKeyOn(core, 0));
    out(" SetPause(2,1) %08X", (w32)__sceSasSetPause(core, 2, 1));
    out(" SetGrain(128) %08X\n", (w32)__sceSasSetGrain(core, 128));
    g_grain = 128;
    render(2, 0);
    out("  before Init: height v0 %08X, end flag %08X, pause flag %08X\n",
        g_h[1], g_ef[1], (w32)__sceSasGetPauseFlag(core));
    out("  Init %08X\n", (w32)__sceSasInit(core, 256, 32, 0, 44100));
    g_grain = 256;
    out("  after Init: GetGrain %d, height v0 %08X, end flag %08X, pause flag %08X\n",
        __sceSasGetGrain(core), (w32)__sceSasGetEnvelopeHeight(core, 0),
        (w32)__sceSasGetEndFlag(core), (w32)__sceSasGetPauseFlag(core));
    out("  KeyOff v0 %08X (0: the key survived Init)\n", (w32)__sceSasSetKeyOff(core, 0));
    {
        int r = core_raw(), nz = 0;
        for (int i = 0; i < 512; i++) if (g_out[i]) nz++;
        out("  one core %08X: %d nonzero samples\n", (w32)r, nz);
    }

    /* sascore.c:398,492: voices at or past maxVoices are not rendered. */
    step("init: maxVoices 8: v7 (constant 100) and v8 (constant 1000) set up and keyed on");
    fresh();
    ret(__sceSasInit(core, 256, 8, 0, 44100));
    fill_const(PCM(1), PCM_N, 100);
    fill_const(PCM(2), PCM_N, 1000);
    sync();
    for (int v = 7; v <= 8; v++) {
        int r1 = __sceSasSetVoicePCM(core, v, v == 7 ? PCM(1) : PCM(2), 256, 0);
        int r2 = __sceSasSetVolume(core, v, 0x1000, 0x1000, 0, 0);
        int r3 = __sceSasSetADSRmode(core, v, 0xF, 0, 1, 1, 1);
        int r4 = __sceSasSetADSR(core, v, 0xF, 0x7FFFFFFF, 0, 0, 0);
        int r5 = __sceSasSetSL(core, v, 0x40000000);
        int r6 = __sceSasSetKeyOn(core, v);
        out("  v%d: SetVoicePCM %08X SetVolume %08X SetADSRmode %08X SetADSR %08X SetSL %08X KeyOn %08X\n",
            v, (w32)r1, (w32)r2, (w32)r3, (w32)r4, (w32)r5, (w32)r6);
    }
    render(2, -1);
    out("  L[100] %d L[300] %d, end flags %08X %08X, heights v7 %08X v8 %08X\n",
        CL(100), CL(300), g_ef[0], g_ef[1],
        (w32)__sceSasGetEnvelopeHeight(core, 7), (w32)__sceSasGetEnvelopeHeight(core, 8));
}

/* ============================================================================
 * layout: which struct words each setter writes
 * ==========================================================================*/

#define TA 0x01A2A3A4
#define TD 0x02D2D3D4
#define TS 0x03C2C3C4
#define TR 0x04E2E3E4
#define TL 0x05B2B3B4

static void sec_layout(void) {
    char b1[16], b2[16], b3[16], b4[16], b5[16];
    section("layout");
    fresh();
    vag_clear(VAG);
    for (int i = 0; i < 5; i++) vag_blk_c(VAG, 0, 4, i == 4 ? 1 : 0, 1);
    snap();

    /* sascore.c:63-76: psprecomp mirrors only the ADSR block of a 20-byte
     * header + 56-byte voice layout. Every setter here logs every word it
     * changed, so the real layout can be read off. */
    step("layout: SetVoice(0, vag, 0x50, 0)");
    ret(__sceSasSetVoice(core, 0, VAG, 0x50, 0)); diff();
    step("layout: SetVoice(1, vag, 0x50, 1)");
    ret(__sceSasSetVoice(core, 1, VAG, 0x50, 1)); diff();
    step("layout: SetVoicePCM(2, pcm0, 100, 50)");
    ret(__sceSasSetVoicePCM(core, 2, PCM(0), 100, 50)); diff();
    step("layout: SetPitch(0, 0x1234)");
    ret(__sceSasSetPitch(core, 0, 0x1234)); diff();
    step("layout: SetVolume(0, 0x111, 0x222, 0x333, 0x444)");
    ret(__sceSasSetVolume(core, 0, 0x111, 0x222, 0x333, 0x444)); diff();
    step("layout: SetADSR(0, 0xF, %08X, %08X, %08X, %08X)", TA, TD, TS, TR);
    ret(__sceSasSetADSR(core, 0, 0xF, TA, TD, TS, TR)); diff();
    step("layout: SetADSR(1, 0xF, same)");
    ret(__sceSasSetADSR(core, 1, 0xF, TA, TD, TS, TR)); diff();
    step("layout: SetADSR(31, 0xF, same)");
    ret(__sceSasSetADSR(core, 31, 0xF, TA, TD, TS, TR)); diff();
    step("layout: SetADSRmode(0, 0xF, 2, 3, 4, 5)");
    ret(__sceSasSetADSRmode(core, 0, 0xF, 2, 3, 4, 5)); diff();
    {
        int n = g_nchg < 4 ? g_nchg : 4;
        if (n > 0) { n_mode = n; for (int i = 0; i < n; i++) off_mode[i] = g_chg[i]; }
    }
    step("layout: SetSL(0, %08X)", TL);
    ret(__sceSasSetSL(core, 0, TL)); diff();
    step("layout: SetSimpleADSR(3, 0x8F3A, 0x4ABC)");
    ret(__sceSasSetSimpleADSR(core, 3, 0x8F3A, 0x4ABC)); diff();
    step("layout: SetNoise(4, 17)");
    ret(__sceSasSetNoise(core, 4, 17)); diff();
    step("layout: SetKeyOn(2)");
    ret(__sceSasSetKeyOn(core, 2)); diff();
    step("layout: __sceSasCore once");
    ret(core_raw()); diff();
    step("layout: SetKeyOff(2)");
    ret(__sceSasSetKeyOff(core, 2)); diff();
    step("layout: SetPause(4, 1)");
    ret(__sceSasSetPause(core, 4, 1)); diff();
    step("layout: SetPause(4, 0)");
    ret(__sceSasSetPause(core, 4, 0)); diff();
    step("layout: SetOutputmode(1)");
    ret(__sceSasSetOutputmode(core, 1)); diff();
    step("layout: SetOutputmode(0)");
    ret(__sceSasSetOutputmode(core, 0)); diff();
    step("layout: SetGrain(512)");
    ret(__sceSasSetGrain(core, 512)); diff();
    step("layout: SetGrain(256)");
    ret(__sceSasSetGrain(core, 256)); diff();

    step("layout: where the ADSR values landed (psprecomp: v0 attack +2C, stride 0x38)");
    {
        int a0 = find_word(TA, 0), a1 = find_word(TA, a0 + 4), a31 = find_word(TA, (a1 < 0 ? a0 : a1) + 4);
        int d0 = find_word(TD, 0), s0 = find_word(TS, 0), r0 = find_word(TR, 0), l0 = find_word(TL, 0);
        out("  attack rate v0 %s v1 %s v31 %s\n", offs(a0, b1), offs(a1, b2), offs(a31, b3));
        out("  v0 decay %s sustain %s release %s SL %s\n", offs(d0, b1), offs(s0, b2), offs(r0, b3), offs(l0, b4));
        out("  curve words: %d from %s\n", n_mode, offs(off_mode[0], b5));
        if (a0 >= 0 && d0 >= 0 && s0 >= 0 && r0 >= 0 && l0 >= 0) {
            off_ar = a0; off_dr = d0; off_sr = s0; off_rr = r0; off_sl = l0;
            out("  later sections read these offsets\n");
        } else {
            out("  not all found; later sections read psprecomp's offsets\n");
        }
    }
}

/* ============================================================================
 * args: argument checks on each setter
 * ==========================================================================*/

static int call_idx(int which, int v) {
    switch (which) {
    case 0:  return __sceSasSetVoice(core, v, VAG, 64, 0);
    case 1:  return __sceSasSetPitch(core, v, 0x1000);
    case 2:  return __sceSasSetVolume(core, v, 0, 0, 0, 0);
    case 3:  return __sceSasSetADSR(core, v, 0xF, 0x1000, 0x1000, 0x1000, 0x1000);
    case 4:  return __sceSasSetADSRmode(core, v, 0xF, 0, 1, 1, 1);
    case 5:  return __sceSasSetSimpleADSR(core, v, 0x000F, 0x0000);
    case 6:  return __sceSasSetSL(core, v, 0);
    case 7:  return __sceSasSetNoise(core, v, 0);
    case 8:  return __sceSasGetEnvelopeHeight(core, v);
    case 9:  return __sceSasSetVoicePCM(core, v, PCM(0), 64, -1);
    case 10: return __sceSasSetKeyOn(core, v);
    default: return __sceSasSetKeyOff(core, v);
    }
}

static void log_rates(const char *what) {
    out("  %s: A %08X D %08X S %08X R %08X\n", what, rd(off_ar), rd(off_dr), rd(off_sr), rd(off_rr));
}

static void sec_args(void) {
    static const char *const names[12] = {
        "SetVoice", "SetPitch", "SetVolume", "SetADSR", "SetADSRmode", "SetSimpleADSR",
        "SetSL", "SetNoise", "GetEnvelopeHeight", "SetVoicePCM", "SetKeyOn", "SetKeyOff" };
    static const int vi[4] = { -1, 0, 31, 32 };
    section("args");
    fresh();
    vag_clear(VAG);
    for (int i = 0; i < 4; i++) vag_blk_c(VAG, 0, 0, 0, 0);

    /* sascore.c:26-27,478-481: a voice index outside 0..31 refused with 80420010. */
    step("args: voice -1, 0, 31, 32 on each per-voice call (claim 80420010 outside 0..31; sascore.c:26-27,478-481)");
    for (int w = 0; w < 12; w++) {
        out("  %-17s", names[w]);
        for (int k = 0; k < 4; k++) out(" %d:%08X", vi[k], (w32)call_idx(w, vi[k]));
        out("\n");
    }

    /* sascore.c:27-29,531: size 0 or off a multiple of 16 refused with
     * 80420014; negative multiples of 16 accepted. */
    step("args: SetVoice size, loop 0 (claim 80420014 for 0 or not a multiple of 16, -16 accepted; sascore.c:27-29,531)");
    {
        static const int ss[] = { 0, 1, 15, 16, 17, 32, 0x10000, 0x7FFFFFF0, -16, -1, -32, (int)0x80000000 };
        for (unsigned i = 0; i < sizeof ss / sizeof ss[0]; i++)
            out("  size 0x%X: %08X\n", (w32)ss[i], (w32)__sceSasSetVoice(core, 0, VAG, ss[i], 0));
    }
    /* sascore.c:532-536: loop mode 0 and 1 only, everything else 80420015. */
    step("args: SetVoice loop mode, size 64 (claim 0 and 1 only, else 80420015; sascore.c:532-536)");
    {
        static const int ls[] = { 0, 1, 2, -1, (int)0x80000000, 0x100 };
        for (unsigned i = 0; i < sizeof ls / sizeof ls[0]; i++)
            out("  loop %d: %08X\n", ls[i], (w32)__sceSasSetVoice(core, 0, VAG, 64, ls[i]));
    }
    step("args: SetVoice which bad argument wins");
    out("  size 0 loop 2: %08X\n", (w32)__sceSasSetVoice(core, 0, VAG, 0, 2));
    out("  voice 32 size 0 loop 2: %08X\n", (w32)__sceSasSetVoice(core, 32, VAG, 0, 2));
    step("args: SetVoice address vag+1, +2, +4, +8 (size 32; no key-on follows)");
    for (int o = 1; o <= 8; o *= 2)
        out("  vag+%d: %08X\n", o, (w32)__sceSasSetVoice(core, 0, VAG + o, 32, 0));
    __sceSasSetVoice(core, 0, VAG, 64, 0);

    /* sascore.c:37-39,564: PCM size outside 1..0x10000 refused with
     * 8042001A, compared signed. */
    step("args: SetVoicePCM size, loop -1 (claim 8042001A outside 1..0x10000, signed; sascore.c:37-39,564)");
    {
        static const int ss[] = { 0, 1, 2, 0x10000, 0x10001, -1, 0x7FFFFFFF, (int)0x80000000 };
        for (unsigned i = 0; i < sizeof ss / sizeof ss[0]; i++)
            out("  size 0x%X: %08X\n", (w32)ss[i], (w32)__sceSasSetVoicePCM(core, 0, PCM(0), ss[i], -1));
    }
    /* sascore.c:37-40,554-557,565: a loop position at or past the size is
     * refused with 80420015, signed: -1 and 0x80000001 pass, 0x40000001 not. */
    step("args: SetVoicePCM loop, size 100 (claim 80420015 at or past size, signed; sascore.c:37-40,565)");
    {
        static const int ls[] = { -1, -2, 0, 1, 98, 99, 100, 101, 0xFFFF, 0x7FFFFFFF,
                                  (int)0x80000000, (int)0x80000001, 0x40000001 };
        for (unsigned i = 0; i < sizeof ls / sizeof ls[0]; i++)
            out("  loop %d (0x%X): %08X\n", ls[i], (w32)ls[i], (w32)__sceSasSetVoicePCM(core, 0, PCM(0), 100, ls[i]));
    }
    step("args: SetVoicePCM which bad argument wins");
    out("  size 0 loop 5: %08X\n", (w32)__sceSasSetVoicePCM(core, 0, PCM(0), 0, 5));
    out("  size 0x10001 loop 0x20000: %08X\n", (w32)__sceSasSetVoicePCM(core, 0, PCM(0), 0x10001, 0x20000));
    __sceSasSetVoicePCM(core, 0, PCM(0), 64, -1);

    /* sascore.c:45-46,579: pitch above 0x4000 refused with 80420012, unsigned. */
    step("args: SetPitch (claim 80420012 above 0x4000, unsigned; sascore.c:45-46,579)");
    {
        static const w32 ps[] = { 0, 1, 0xFFF, 0x1000, 0x3FFF, 0x4000, 0x4001, 0x8000, 0x7FFFFFFF,
                                  0x80000001, 0xFFFFFFFF };
        for (unsigned i = 0; i < sizeof ps / sizeof ps[0]; i++)
            out("  pitch 0x%X: %08X\n", ps[i], (w32)__sceSasSetPitch(core, 0, (int)ps[i]));
    }
    /* sascore.c:47-48,584-591: noise frequency outside 0..63 refused with
     * 80420011, unsigned. */
    step("args: SetNoise (claim 80420011 outside 0..63, unsigned; sascore.c:47-48,591)");
    {
        static const w32 ns[] = { 0, 1, 62, 63, 64, 0xFF, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF };
        for (unsigned i = 0; i < sizeof ns / sizeof ns[0]; i++)
            out("  freq 0x%X: %08X\n", ns[i], (w32)__sceSasSetNoise(core, 0, (int)ns[i]));
    }
    /* sascore.c:51-53,595-603: all four volumes checked, -0x1000..0x1000,
     * else 80420018. */
    step("args: SetVolume, one argument at a time (claim all four checked, 80420018 outside -0x1000..0x1000; sascore.c:51-53,603)");
    {
        static const char *const pn[4] = { "left", "right", "send-l", "send-r" };
        static const int vs[] = { 0x1000, 0x1001, -0x1000, -0x1001, 0x7FFFFFFF, (int)0x80000000 };
        for (int p = 0; p < 4; p++) {
            out("  %-6s", pn[p]);
            for (unsigned i = 0; i < sizeof vs / sizeof vs[0]; i++) {
                int a[4] = { 0, 0, 0, 0 };
                a[p] = vs[i];
                out(" %X:%08X", (w32)vs[i], (w32)__sceSasSetVolume(core, 0, a[0], a[1], a[2], a[3]));
            }
            out("\n");
        }
        out("  voice 32 with left 0x1001: %08X\n", (w32)__sceSasSetVolume(core, 32, 0x1001, 0, 0, 0));
    }
    /* sascore.c:58-59,635-642: a rate with the top bit set refused with
     * 80420019, only for the fields the flags select, before anything is
     * stored. */
    step("args: SetADSR rates (claim 80420019 for a set top bit, only flagged fields, nothing stored; sascore.c:58-59,635-642)");
    {
        out("  flags F all 7FFFFFFF: %08X\n",
            (w32)__sceSasSetADSR(core, 0, 0xF, 0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF));
        for (int p = 0; p < 4; p++) {
            int a[4] = { 0x1000, 0x1000, 0x1000, 0x1000 };
            a[p] = -1;
            int r1 = __sceSasSetADSR(core, 0, 0xF, a[0], a[1], a[2], a[3]);
            a[p] = (int)0x80000000;
            int r2 = __sceSasSetADSR(core, 0, 0xF, a[0], a[1], a[2], a[3]);
            out("  flags F, field %d = -1: %08X, = 80000000: %08X\n", p, (w32)r1, (w32)r2);
        }
        out("  set 111,222,333,444: %08X\n", (w32)__sceSasSetADSR(core, 0, 0xF, 0x111, 0x222, 0x333, 0x444));
        log_rates("read back");
        out("  flags 0 all -1: %08X\n", (w32)__sceSasSetADSR(core, 0, 0, -1, -1, -1, -1));
        log_rates("read back");
        out("  flags 1 attack 555, others -1: %08X\n", (w32)__sceSasSetADSR(core, 0, 1, 0x555, -1, -1, -1));
        log_rates("read back");
        out("  flags F attack 666, decay -1: %08X\n", (w32)__sceSasSetADSR(core, 0, 0xF, 0x666, -1, 0x333, 0x444));
        log_rates("read back");
        out("  flags 2 attack -1, decay 777: %08X\n", (w32)__sceSasSetADSR(core, 0, 2, -1, 0x777, -1, -1));
        log_rates("read back");
        out("  flags 10 all 888: %08X\n", (w32)__sceSasSetADSR(core, 0, 0x10, 0x888, 0x888, 0x888, 0x888));
        log_rates("read back");
        out("  flags 10 all -1: %08X\n", (w32)__sceSasSetADSR(core, 0, 0x10, -1, -1, -1, -1));
        out("  flags FFFFFFFF all 999: %08X\n", (w32)__sceSasSetADSR(core, 0, 0xFFFFFFFFu, 0x999, 0x999, 0x999, 0x999));
        log_rates("read back");
    }
    /* sascore.c:743-751: psprecomp stores any level. */
    step("args: SetSL level, read back (psprecomp takes anything; sascore.c:743-751)");
    {
        static const w32 ls[] = { 0, 1, 0x40000000, 0x40000001, 0x7FFFFFFF, 0xFFFFFFFF, 0x80000000 };
        for (unsigned i = 0; i < sizeof ls / sizeof ls[0]; i++) {
            int r = __sceSasSetSL(core, 0, (int)ls[i]);
            out("  SL 0x%X: %08X, read back %08X\n", ls[i], (w32)r, rd(off_sl));
        }
    }
    /* sascore.c:514-519: 0 and 1 accepted, else 80420003; Get answers the mode. */
    step("args: SetOutputmode then GetOutputmode (claim 0 and 1 only, else 80420003; sascore.c:514-519)");
    {
        static const w32 ms[] = { 0, 1, 2, 3, 0xFFFFFFFF, 0x80000000, 0x80000001 };
        for (unsigned i = 0; i < sizeof ms / sizeof ms[0]; i++) {
            int r = __sceSasSetOutputmode(core, (int)ms[i]);
            out("  mode 0x%X: %08X, GetOutputmode %d\n", ms[i], (w32)r, __sceSasGetOutputmode(core));
        }
        __sceSasSetOutputmode(core, 0);
    }
    /* sascore.c:812-825: SetPause takes a bitmask; GetPauseFlag reads it back. */
    step("args: SetPause masks and pause values, GetPauseFlag after each (sascore.c:812-825)");
    {
        static const struct { w32 m; int p; } ps[] = {
            { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0xFFFFFFFF, 1 }, { 0xFFFFFFFF, 0 },
            { 0x80000000, 2 }, { 0x80000000, -1 }, { 0x80000000, 0 }, { 0x10, 0x100 }, { 0xFFFFFFFF, 0 } };
        for (unsigned i = 0; i < sizeof ps / sizeof ps[0]; i++) {
            int r = __sceSasSetPause(core, ps[i].m, ps[i].p);
            out("  SetPause(0x%X, %d): %08X, flag %08X\n", ps[i].m, ps[i].p, (w32)r, (w32)__sceSasGetPauseFlag(core));
        }
    }
}

/* ============================================================================
 * adsrmode: the curve matrix
 * ==========================================================================*/

static void log_modes(const char *what) {
    char b[16];
    out("  %s:", what);
    for (int i = 0; i < n_mode; i++) out(" %s=%08X", offs(off_mode[i], b), rd(off_mode[i]));
    out("\n");
}

static void sec_adsrmode(void) {
    static const w32 mv[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 0x10, 0x100, 0xFFFFFFFF, 0x80000000,
                              0x80000001, 0x80000002, 0x80000004, 0x80000005, 0x80000006,
                              0x40000000, 0x40000001 };
    static const char *const ph[4] = { "attack", "decay", "sustain", "release" };
    section("adsrmode");
    fresh();

    /* sascore.c:108-118,124-130,655-663: attack takes 0/2/4, decay and
     * release 1/3/5, sustain 0..5; above 5 or any bit outside 0x80000007 is
     * refused with 80420013; 0x80000001 is mode 1, 0x40000001 refused. */
    step("adsrmode: each phase alone x mode value (claim attack even, decay/release odd, sustain 0..5, else 80420013; sascore.c:108-118,124-130)");
    for (int p = 0; p < 4; p++) {
        out("  %s:", ph[p]);
        for (unsigned i = 0; i < sizeof mv / sizeof mv[0]; i++) {
            int a[4] = { 0, 1, 1, 1 };
            a[p] = (int)mv[i];
            int r = __sceSasSetADSRmode(core, 0, 1u << p, a[0], a[1], a[2], a[3]);
            if (i && i % 5 == 0) out("\n   ");
            out(" %X=%08X", mv[i], (w32)r);
        }
        out("\n");
    }
    step("adsrmode: unflagged fields are not checked (sascore.c:660-663)");
    out("  flags 0, modes 7,7,7,7: %08X\n", (w32)__sceSasSetADSRmode(core, 0, 0, 7, 7, 7, 7));
    out("  flags 1, attack 0 decay 0: %08X\n", (w32)__sceSasSetADSRmode(core, 0, 1, 0, 0, 7, 7));
    out("  flags 10, modes 7,7,7,7: %08X\n", (w32)__sceSasSetADSRmode(core, 0, 0x10, 7, 7, 7, 7));
    /* sascore.c:651-654: every named curve is checked before anything is stored. */
    step("adsrmode: a refused call stores nothing (claim sascore.c:651-654)");
    out("  set 0,1,1,1: %08X\n", (w32)__sceSasSetADSRmode(core, 0, 0xF, 0, 1, 1, 1));
    log_modes("curve words");
    out("  set 2,0,3,3 (decay 0 refused): %08X\n", (w32)__sceSasSetADSRmode(core, 0, 0xF, 2, 0, 3, 3));
    log_modes("curve words");
    out("  set 4,3,3,6 (release 6 refused): %08X\n", (w32)__sceSasSetADSRmode(core, 0, 0xF, 4, 3, 3, 6));
    log_modes("curve words");
    /* sascore.c:117-118,664-667: 0x80000001 is accepted as mode 1. */
    step("adsrmode: how accepted values with the sign bit are stored (claim as the low three bits; sascore.c:664-667)");
    out("  set 80000004,80000001,80000005,80000003: %08X\n",
        (w32)__sceSasSetADSRmode(core, 0, 0xF, (int)0x80000004, (int)0x80000001, (int)0x80000005, (int)0x80000003));
    log_modes("curve words");
    out("  set 2,5,0,5: %08X\n", (w32)__sceSasSetADSRmode(core, 0, 0xF, 2, 5, 0, 5));
    log_modes("curve words");
}

/* ============================================================================
 * simpleadsr: the packed envelope
 * ==========================================================================*/

static void simple_sweep(const char *label, int n, int shift_a1, int shift_a2, int base_a2, int off) {
    for (int k = 0; k < n; k++) {
        const w32 a1 = shift_a1 >= 0 ? (w32)k << shift_a1 : 0;
        const w32 a2 = shift_a2 >= 0 ? ((w32)k << shift_a2) | (w32)base_a2 : (w32)base_a2;
        int r = __sceSasSetSimpleADSR(core, 0, a1, a2);
        if (k % 8 == 0) out("  %s %3d:", label, k);
        if (r) out(" !%08X", (w32)r); else out(" %08X", rd(off));
        if (k % 8 == 7 || k == n - 1) out("\n");
    }
}

static void sec_simple(void) {
    section("simpleadsr");
    fresh();

    /* sascore.c:716: bit 13 of the second word is reserved and refused. */
    step("simpleadsr: reserved bit 13 of adsr2 and bits above 15 (claim 80420013 for bit 13; sascore.c:716)");
    {
        static const w32 ps[][2] = { { 0, 0 }, { 0, 0x2000 }, { 0, 0xDFFF }, { 0xFFFF, 0xDFFF }, { 0, 0xFFFF },
                                     { 0x10000, 0 }, { 0, 0x10000 }, { 0, 0x12000 }, { 0xFFFFFFFF, 0 },
                                     { 0, 0xFFFF0000 } };
        for (unsigned i = 0; i < sizeof ps / sizeof ps[0]; i++)
            out("  (0x%X, 0x%X): %08X\n", ps[i][0], ps[i][1], (w32)__sceSasSetSimpleADSR(core, 0, ps[i][0], ps[i][1]));
    }
    /* sascore.c:672-681,695: the seven-bit rate, all ones reads zero. */
    step("simpleadsr: attack rate 0..127 -> attack word (claim sascore.c:672-681,695)");
    simple_sweep("ar", 128, 8, -1, 0, off_ar);
    /* sascore.c:697-698,735: 0x80000000 >> rate, saturated. */
    step("simpleadsr: decay rate 0..15 -> decay word (claim sascore.c:697-698,735)");
    simple_sweep("dr", 16, 4, -1, 0, off_dr);
    /* sascore.c:738: (level + 1) << 26. */
    step("simpleadsr: sustain level 0..15 -> SL word (claim sascore.c:738)");
    simple_sweep("sl", 16, 0, -1, 0, off_sl);
    step("simpleadsr: sustain rate 0..127, sustain type 0 -> sustain word (claim sascore.c:695)");
    simple_sweep("sr0", 128, -1, 6, 0x0000, off_sr);
    /* sascore.c:695-696,723: type 3 takes a further quarter. */
    step("simpleadsr: sustain rate 0..127, sustain type 3 -> sustain word (claim sascore.c:695-696,723)");
    simple_sweep("sr3", 128, -1, 6, 0xC000, off_sr);
    /* sascore.c:699-704: linear 0x40000000 >> (rate + 2) with a MIPS shift,
     * exponential 0x80000000 >> rate; all ones zero. */
    step("simpleadsr: release rate 0..31, linear type then exponential -> release word (claim sascore.c:699-704)");
    simple_sweep("rr0", 32, -1, 0, 0x0000, off_rr);
    simple_sweep("rr1", 32, -1, 0, 0x0020, off_rr);
    /* sascore.c:706-709,730-733: attack -> lin-inc/bent, decay exp-rev,
     * sustain its two bits, release lin-dec/exp-rev. */
    step("simpleadsr: curve words for attack type x sustain type x release type (claim sascore.c:706-709,730-733)");
    for (int at = 0; at < 2; at++)
        for (int st = 0; st < 4; st++)
            for (int rt = 0; rt < 2; rt++) {
                char lab[40];
                int r = __sceSasSetSimpleADSR(core, 0, (w32)at << 15, ((w32)st << 14) | ((w32)rt << 5));
                sprintf(lab, "a%d s%d r%d = %08X", at, st, rt, (w32)r);
                log_modes(lab);
            }
}

/* ============================================================================
 * keys: key on and off
 * ==========================================================================*/

static void key_setup(int ar, int rm, int rr) {
    pcm_voice(0, PCM(0), 256, 0);
    env(0, 0, ar, 1, 0, 1, 0, rm, rr, 0x40000000);
}

static void sec_keys(void) {
    section("keys");
    fresh();
    key_setup(0x1000, 1, 0);

    /* sascore.c:40-41,756-761: keying on a voice that is on is refused, 80420016. */
    step("keys: KeyOn twice (claim second 80420016; sascore.c:40-41,761)");
    out("  %08X %08X\n", (w32)__sceSasSetKeyOn(core, 0), (w32)__sceSasSetKeyOn(core, 0));
    /* sascore.c:794-806: keying off a voice that is not on is refused, 80420016. */
    step("keys: KeyOff twice (claim second 80420016; sascore.c:794-806)");
    out("  %08X %08X\n", (w32)__sceSasSetKeyOff(core, 0), (w32)__sceSasSetKeyOff(core, 0));
    step("keys: KeyOff v9, never keyed on (claim 80420016; sascore.c:794-806)");
    ret(__sceSasSetKeyOff(core, 9));

    /* sascore.c:191-201,787: the voice is held 32 samples after a key-on;
     * at grain 128 and attack 0x1000 the height reads 0 before a core,
     * 0x60000 after one and 0x1E0000 after four. */
    step("keys: grain 128, attack lin-inc 0x1000: height at key-on and after 4 cores (claim 0, 60000..1E0000; sascore.c:191-201,787)");
    fresh();
    out("  SetGrain(128) %08X\n", (w32)__sceSasSetGrain(core, 128));
    g_grain = 128;
    key_setup(0x1000, 1, 0);
    {
        w32 h0 = (w32)__sceSasGetEnvelopeHeight(core, 0);
        keyon(0);
        w32 h1 = (w32)__sceSasGetEnvelopeHeight(core, 0);
        render(4, 0);
        out("  before KeyOn %08X, after KeyOn %08X\n", h0, h1);
        log_heights(4);
    }
    step("keys: grain 256, attack lin-inc 0x1000: height after 4 cores");
    fresh();
    key_setup(0x1000, 1, 0);
    keyon(0);
    render(4, 0);
    log_heights(4);

    /* sascore.c:176-178,798-802: key-off lifts the key at once, so a key-on
     * straight after it, with no core between, restarts the voice. */
    step("keys: KeyOff then KeyOn with no core between (claim restarts; sascore.c:176-178,798-802)");
    fresh();
    key_setup(0x1000, 1, 0x1000);
    keyon(0);
    render(2, 0);
    {
        w32 ha = g_h[1];
        int r1 = __sceSasSetKeyOff(core, 0), r2 = __sceSasSetKeyOn(core, 0);
        w32 hb = (w32)__sceSasGetEnvelopeHeight(core, 0);
        render(1, 0);
        out("  height after 2 cores %08X; KeyOff %08X KeyOn %08X; height %08X; after 1 core %08X\n",
            ha, (w32)r1, (w32)r2, hb, g_h[0]);
    }
    /* sascore.c:178-179,798: the release waits for the next core. */
    step("keys: release lin-dec 0x100000 from full: height at KeyOff and after 2 cores (claim release starts at the next core; sascore.c:178-179)");
    fresh();
    key_setup(0x7FFFFFFF, 1, 0x100000);
    keyon(0);
    render(2, 0);
    {
        w32 ha = g_h[1];
        int r = __sceSasSetKeyOff(core, 0);
        w32 hb = (w32)__sceSasGetEnvelopeHeight(core, 0), ef = (w32)__sceSasGetEndFlag(core);
        render(2, 0);
        out("  height before KeyOff %08X; KeyOff %08X; height %08X end flag %08X right after\n", ha, (w32)r, hb, ef);
        log_heights(2);
        log_end(0, 2);
    }
    /* sascore.c:179-185: a key-on is taken while the release of the last
     * key-off is still sounding, and restarts the envelope from 0. */
    step("keys: KeyOn during an unfinished release (attack lin-inc 0x200000, 3 cores; release lin-dec 0x100000, 1 core) (claim accepted, restarts; sascore.c:179-185)");
    fresh();
    key_setup(0x200000, 1, 0x100000);
    keyon(0);
    render(3, 0);
    out("  KeyOff %08X", (w32)__sceSasSetKeyOff(core, 0));
    render(1, 0);
    out(", height after 1 core of release %08X end %08X", g_h[0], g_ef[0]);
    out(", KeyOn %08X", (w32)__sceSasSetKeyOn(core, 0));
    out(", height %08X\n", (w32)__sceSasGetEnvelopeHeight(core, 0));
    render(2, 0);
    log_heights(2);
    log_end(0, 2);
    step("keys: KeyOn then KeyOff before any core (attack lin-inc 0x1000, release lin-dec 0x1000), 3 cores");
    fresh();
    key_setup(0x1000, 1, 0x1000);
    out("  KeyOn %08X", (w32)__sceSasSetKeyOn(core, 0));
    out(" KeyOff %08X\n", (w32)__sceSasSetKeyOff(core, 0));
    render(3, 0);
    log_heights(3);
    log_end(0, 3);

    /* sascore.c:429,461-467: a voice that runs out of data ends, and its key
     * with it; the PSPSDK header says end flags change only in a core. */
    step("keys: VAG v0 of 2 blocks (second flagged 1) played out; KeyOff, KeyOn, end flags");
    fresh();
    vag_clear(VAG);
    vag_blk_c(VAG, 0, 4, 0, 1);
    vag_blk_c(VAG, 0, 4, 1, 2);
    vag_voice(0, VAG, 32, 0);
    env_flat(0);
    {
        w32 e0 = (w32)__sceSasGetEndFlag(core);
        keyon(0);
        w32 e1 = (w32)__sceSasGetEndFlag(core);
        render(2, 0);
        out("  end flag before KeyOn %08X, right after %08X\n", e0, e1);
        log_end(0, 2);
        int r1 = __sceSasSetKeyOff(core, 0);
        int r2 = __sceSasSetKeyOn(core, 0);
        w32 e2 = (w32)__sceSasGetEndFlag(core);
        render(1, 0);
        out("  after the end: KeyOff %08X KeyOn %08X, end flag right after %08X, after a core %08X\n",
            (w32)r1, (w32)r2, e2, g_ef[0]);
        log_sum();
        log_rle(256);
    }
}

/* ============================================================================
 * envscale: how height and volume scale a sample
 * ==========================================================================*/

static void sec_envscale(void) {
    static const int vals[] = { 0x7FFF, -0x8000, 12345, -12345, 1, -1, 3, -3 };
    static const int vols[] = { 0x1000, 0xFFF, 0xC00, 0x800, 0x400, 1, 0, -0x800, -0x1000 };
    section("envscale");

    /* sascore.c:434-443: height is read before it steps, so the first sample
     * is multiplied by 0; the gain is height >> 18 as a 12-bit factor. */
    for (int k = 0; k < 2; k++) {
        const int v = k ? -0x8000 : 0x7FFF;
        step("envscale: attack lin-inc 0x400000 on a constant %d: L by frame (claim first sample silent, gain height>>18; sascore.c:434-443)", v);
        fresh();
        fill_const(PCM(1), PCM_N, v);
        pcm_voice(0, PCM(1), PCM_N, 0);
        env(0, 0, 0x400000, 1, 0, 1, 0, 1, 0, 0x40000000);
        keyon(0);
        render(2, 0);
        log_sum();
        log_ch(0, 28, 16);
        out("  L every 16th from 32:");
        for (int f = 32; f <= 320; f += 16) out(" %d", CL(f));
        out("\n");
        log_heights(2);
        save(k ? "env_ramp_m8000.bin" : "env_ramp_7fff.bin");
    }
    /* sascore.c:445,869-872: volume multiplies and shifts down 12, truncating. */
    step("envscale: sample x left volume at full height, L[100] (R at 0x1000) (claim (s*vol)>>12; sascore.c:445,869-872)");
    for (unsigned k = 0; k < sizeof vals / sizeof vals[0]; k++) {
        fresh();
        fill_const(PCM(1), PCM_N, vals[k]);
        pcm_voice(0, PCM(1), PCM_N, 0);
        env_flat(0);
        keyon(0);
        core_raw();
        out("  %6d:", vals[k]);
        for (unsigned j = 0; j < sizeof vols / sizeof vols[0]; j++) {
            __sceSasSetVolume(core, 0, vols[j], 0x1000, 0, 0);
            core_raw();
            out(" %X:%d", (w32)vols[j] & 0xFFFF, g_out[200]);
        }
        out(" R:%d\n", g_out[201]);
    }
}

/* ============================================================================
 * adsr: envelope heights, core by core
 * ==========================================================================*/

typedef struct {
    const char *what;
    int am, ar, dm, dr, sm, sr, rm, rr, sl, grains, koff, grain;
} sweep_t;

#define FULL 0x7FFFFFFF
#define TOP  0x40000000
/* attack under test; decay/sustain/release hold */
#define SA(m, r, n)        { "attack", m, r, 1, 0, 1, 0, 1, 0, TOP, n, -1, 256 }
#define SAG(m, r, n, g)    { "attack", m, r, 1, 0, 1, 0, 1, 0, TOP, n, -1, g }
/* decay under test from full height to `sl` */
#define SD(m, r, sl, n)    { "decay", 0, FULL, m, r, 1, 0, 1, 0, sl, n, -1, 256 }
/* sustain under test from 0x20000000 */
#define SS(m, r, n)        { "sustain", 0, FULL, 1, FULL, m, r, 1, 0, 0x20000000, n, -1, 256 }
/* release under test from full height, key off before core 2 */
#define SR(m, r, n)        { "release", 0, FULL, 1, 0, 1, 0, m, r, TOP, n, 2, 256 }

static const sweep_t g_sweeps[] = {
    SA(0, 0x1000, 6), SA(0, 0x40000, 20), SA(0, 0x100000, 6), SA(0, 0x2000000, 4),
    SA(0, FULL, 3), SA(0, 0, 3), SA(0, 1, 3),
    SA(2, 0x40000, 32), SA(2, 0xA0000, 16), SA(2, 0x100000, 8), SAG(2, 0x100000, 32, 64), SA(2, FULL, 3),
    SA(4, 0, 6), SA(4, 1, 4), SA(4, 0x1000, 4), SA(4, 0x10000, 6), SA(4, 0x100000, 12),
    SA(4, 0x1000000, 12), SA(4, 0x10000000, 8), SA(4, FULL, 3),
    SD(1, 0x40000, 0x10000000, 16), SD(1, 0x200000, 0x10000000, 6), SD(1, FULL, 0x10000000, 3),
    SD(3, 1, 0x10000000, 3), SD(3, 9, 0x10000000, 3), SD(3, 0x10000, 0x10000000, 6),
    SD(3, 0x100000, 0x10000000, 16), SD(3, 0x1000000, 0x10000000, 16), SD(3, 0x8000000, 0x10000000, 8),
    SD(3, FULL, 0x10000000, 4),
    SD(5, 0, 0x10000000, 4), SD(5, 0x20000000, 0x10000000, 4),
    SD(3, 0x1000000, 0, 16), SD(1, 0x100000, TOP, 4),
    SS(0, 0x40000, 24), SS(1, 0x40000, 24), SS(2, 0x40000, 24), SS(3, 0x1000000, 16),
    SS(4, 0x1000000, 16), SS(5, 0x08000000, 4), SS(5, 0, 4),
    SR(1, 0x40000, 20), SR(1, 0x100000, 8), SR(1, FULL, 4), SR(1, 0, 5),
    SR(3, 0x100000, 16), SR(3, 0x1000000, 12), SR(3, FULL, 4), SR(5, 0, 4), SR(5, 0x10000000, 4),
    { "release during attack", 0, 0x100000, 1, 0, 1, 0, 1, 0x20000, TOP, 10, 1, 256 },
    { "release during decay", 0, FULL, 3, 0x1000000, 1, 0, 1, 0x40000, 0x10000000, 12, 2, 256 },
    /* SetSimpleADSR in place of the two setters: ar = adsr1, dr = adsr2 */
    { "simple", -1, 0x28A8, 0, 0x5030, 0, 0, 0, 0, NOSL, 24, 12, 256 },
    { "simple", -1, 0x9F13, 0, 0xC7C4, 0, 0, 0, 0, NOSL, 28, 16, 256 },
};

static void sec_adsr(void) {
    section("adsr");
    /* sascore.c:308-357 (curve shapes), 360-388 (phases; a direct decay
     * holds at the top, sustain holds), 164-169 (the sustain rate drives
     * nothing). The voice is a looping constant; the height is read after
     * every core. */
    for (unsigned i = 0; i < sizeof g_sweeps / sizeof g_sweeps[0]; i++) {
        const sweep_t *s = &g_sweeps[i];
        char ko[32];
        if (s->koff < 0) strcpy(ko, "no key off");
        else sprintf(ko, "key off before core %d", s->koff);
        if (s->am < 0)
            step("adsr: %s: SetSimpleADSR(0x%04X, 0x%04X), %d cores, %s",
                 s->what, s->ar, s->dr, s->grains, ko);
        else
            step("adsr: %s: A %s 0x%X, D %s 0x%X, S %s 0x%X, R %s 0x%X, SL 0x%X, %d cores, %s, grain %d",
                 s->what, CURVE[s->am], s->ar, CURVE[s->dm], s->dr, CURVE[s->sm], s->sr,
                 CURVE[s->rm], s->rr, s->sl, s->grains, ko, s->grain);
        fresh();
        if (s->grain != 256) {
            int r = __sceSasSetGrain(core, s->grain);
            if (r) out("  (SetGrain %08X)\n", (w32)r); else g_grain = s->grain;
        }
        pcm_voice(0, PCM(0), 256, 0);
        if (s->am < 0) {
            int r = __sceSasSetSimpleADSR(core, 0, s->ar, s->dr);
            if (r) out("  (SetSimpleADSR %08X)\n", (w32)r);
        } else {
            env(0, s->am, s->ar, s->dm, s->dr, s->sm, s->sr, s->rm, s->rr, s->sl);
        }
        w32 h0 = (w32)__sceSasGetEnvelopeHeight(core, 0);
        keyon(0);
        w32 h1 = (w32)__sceSasGetEnvelopeHeight(core, 0);
        render_ko(s->grains, 0, s->koff, 0);
        out("  before KeyOn %08X, after %08X\n", h0, h1);
        log_heights(s->grains);
        log_end(0, s->grains);
    }
}

/* ============================================================================
 * vag: ADPCM decode
 * ==========================================================================*/

static void sec_vag(void) {
    char name[32];
    section("vag");

    /* sascore.c:78-104,274-298: sixteen filters, the documented five and
     * eleven that read past the table (W[i], W[i+5], /64, second weight
     * subtracted), a sign-extended nibble shifted down, clamped; history
     * carried across blocks. Each render: one voice, flat envelope, volume
     * 0x1000, pitch 0x1000. The data starts at L[33] if sascore.c:773-788
     * is right about the VAG delay. */
    for (int f = 0; f < 16; f++) {
        signed char imp[28], ramp[28];
        memset(imp, 0, sizeof imp);
        imp[1] = 7;   /* second sample, so it is heard even if the first is silenced */
        for (int i = 0; i < 28; i++) ramp[i] = (signed char)(i & 15);
        step("vag: filter %d: impulse 7 (2nd nibble) at shift 4, zeros, nibbles 0..15,0..11 at shift 2, all 7 at shift 0, silent end block flag 1 (sascore.c:78-104,274-298)", f);
        fresh();
        vag_clear(VAG);
        vag_blk(VAG, f, 4, 0, imp);
        vag_blk_c(VAG, f, 0, 0, 0);
        vag_blk(VAG, f, 2, 0, ramp);
        vag_blk_c(VAG, f, 0, 0, 7);
        vag_blk_c(VAG, 0, 0, 1, 0);
        vag_blk_c(VAG, 0, 0, 0, 3);   /* past the size: heard only if read */
        vag_blk_c(VAG, 0, 0, 0, 3);
        vag_voice(0, VAG, 5 * 16, 0);
        env_flat(0);
        keyon(0);
        render(2, 0);
        log_sum();
        out("  first nonzero frame %d, frames with L != R %d\n", first_nz(), lr_diff());
        log_ch(0, 32, 120);
        log_end(0, 2);
        sprintf(name, "vag_filter%02d.bin", f);
        save(name);
    }

    /* sascore.c:290-291: the nibble goes to the top of 16 bits and shifts
     * down; shifts 13..15 are not special there. */
    step("vag: shifts 0..15, filter 0, nibbles 1..15,0,1..12 per block, one block per shift (sascore.c:290-291)");
    fresh();
    vag_clear(VAG);
    {
        signed char n[28];
        for (int i = 0; i < 28; i++) n[i] = (signed char)((i + 1) & 15);
        for (int s = 0; s < 16; s++) vag_blk(VAG, 0, s, 0, n);
        vag_blk_c(VAG, 0, 0, 1, 0);
    }
    vag_voice(0, VAG, 17 * 16, 0);
    env_flat(0);
    keyon(0);
    render(3, 0);
    log_sum();
    {
        const int base = first_nz();
        out("  first nonzero frame %d; per shift, the samples for nibbles 1..15,0:\n", base);
        for (int s = 0; s < 16 && base >= 0; s++) {
            out("  shift %2d:", s);
            for (int k = 0; k < 16; k++) {
                const int fr = base + 28 * s + k;
                out(" %d", fr < frames() ? CL(fr) : 0);
            }
            out("\n");
        }
    }
    log_end(0, 3);
    save("vag_shifts.bin");

    /* sascore.c:451-454: pitch steps through decoded samples as for PCM. */
    for (int k = 0; k < 2; k++) {
        const int p = k ? 0x2000 : 0x800;
        step("vag: triangle wave VAG (filter 0, shift 8) at pitch %X", p);
        fresh();
        vag_clear(VAG);
        {
            signed char n[28];
            for (int b = 0; b < 8; b++) {
                for (int i = 0; i < 28; i++) {
                    const int t = (b * 28 + i) % 32;
                    n[i] = (signed char)(t < 16 ? t - 8 : 23 - t);
                }
                vag_blk(VAG, 0, 8, b == 7 ? 1 : 0, n);
            }
        }
        vag_voice(0, VAG, 8 * 16, 0);
        env_flat(0);
        out("  SetPitch %08X\n", (w32)__sceSasSetPitch(core, 0, p));
        keyon(0);
        render(3, 0);
        log_sum();
        log_ch(0, 30, 24);
        log_end(0, 3);
        save(k ? "vag_pitch2000.bin" : "vag_pitch0800.bin");
    }
}

/* ============================================================================
 * vagflags: end and loop flags
 * ==========================================================================*/

typedef struct { const char *what; unsigned char f[4]; int loop; } flagcase_t;

static const flagcase_t g_flags[] = {
    { "0,0,0,0 loop 0",    { 0, 0, 0, 0 }, 0 },
    { "0,0,0,0 loop 1",    { 0, 0, 0, 0 }, 1 },
    { "0,1,0,0 loop 0",    { 0, 1, 0, 0 }, 0 },
    { "0,1,0,0 loop 1",    { 0, 1, 0, 0 }, 1 },
    { "0,3,0,0 loop 0",    { 0, 3, 0, 0 }, 0 },
    { "0,3,0,0 loop 1",    { 0, 3, 0, 0 }, 1 },
    { "0,6,0,3 loop 1",    { 0, 6, 0, 3 }, 1 },
    { "0,6,0,3 loop 0",    { 0, 6, 0, 3 }, 0 },
    { "6,0,0,3 loop 1",    { 6, 0, 0, 3 }, 1 },
    { "0,6,3,0 loop 1",    { 0, 6, 3, 0 }, 1 },
    { "0,7,0,0 loop 0",    { 0, 7, 0, 0 }, 0 },
    { "0,7,0,0 loop 1",    { 0, 7, 0, 0 }, 1 },
    { "0,6,0,7 loop 1",    { 0, 6, 0, 7 }, 1 },
    { "0,2,0,0 loop 1",    { 0, 2, 0, 0 }, 1 },
    { "0,4,0,0 loop 0",    { 0, 4, 0, 0 }, 0 },
    { "0,5,0,0 loop 0",    { 0, 5, 0, 0 }, 0 },
    { "41,0,0,0 loop 1",   { 0x41, 0, 0, 0 }, 1 },
    { "0,41,0,0 loop 0",   { 0, 0x41, 0, 0 }, 0 },
    { "0,87,0,0 loop 1",   { 0, 0x87, 0, 0 }, 1 },
};

static void sec_vagflags(void) {
    char name[32];
    section("vagflags");
    /* sascore.c:252-273,300-303: exactly 3 loops back (to the block flagged
     * 6, else the start) in loop mode; 1, 3 and 7 end after their block;
     * 0x41 does not end; the end of the buffer ends the voice in either loop
     * mode. Blocks are DC levels: block k plays 256*(k+1). Blocks 4 and 5
     * (1280, 1536) lie past the size and are heard only if read. */
    for (unsigned c = 0; c < sizeof g_flags / sizeof g_flags[0]; c++) {
        const flagcase_t *fc = &g_flags[c];
        step("vagflags: block flags %s, 4 cores (sascore.c:252-273,300-303)", fc->what);
        fresh();
        vag_clear(VAG);
        for (int b = 0; b < 6; b++) vag_blk_c(VAG, 0, 4, b < 4 ? fc->f[b] : 0, b + 1);
        vag_voice(0, VAG, 64, fc->loop);
        env_flat(0);
        keyon(0);
        render(4, 0);
        log_rle(4 * 256);
        log_end(0, 4);
        sprintf(name, "vag_flags%02u.bin", c);
        save(name);
    }

    /* sascore.c:526-547: SetVoice resets the decode position. */
    step("vagflags: SetVoice to other data while playing (loop 0,0,0,3 looping), after core 1");
    fresh();
    vag_clear(VAG);
    for (int b = 0; b < 4; b++) vag_blk_c(VAG, 0, 4, b == 3 ? 3 : 0, b + 1);
    vag_clear(VAG2);
    for (int b = 0; b < 4; b++) vag_blk_c(VAG2, 0, 4, b == 3 ? 3 : 0, (b + 9) & 15);
    vag_voice(0, VAG, 64, 1);
    env_flat(0);
    keyon(0);
    {
        const int w = grain_words();
        core_raw();
        memcpy(g_cap, g_out, w * 2);
        out("  SetVoice %08X\n", (w32)__sceSasSetVoice(core, 0, VAG2, 64, 1));
        for (int i = 1; i < 3; i++) { core_raw(); memcpy(g_cap + i * w, g_out, w * 2); }
        g_capn = 3 * w;
    }
    log_rle(3 * 256);
    save("vag_setvoice_playing.bin");
}

/* ============================================================================
 * pcm: PCM voices and loop positions
 * ==========================================================================*/

static void sec_pcm(void) {
    static const struct { const char *file; int size, loop; } cs[] = {
        { "pcm_loop_none.bin", 100, -1 }, { "pcm_loop0.bin", 100, 0 }, { "pcm_loop50.bin", 100, 50 },
        { "pcm_loop99.bin", 100, 99 }, { "pcm_size1_loop0.bin", 1, 0 } };
    section("pcm");

    /* sascore.c:191-201,420-425,454-462: a PCM voice starts at L[32] with
     * L[32] itself silent (height 0), steps one sample per output at pitch
     * 0x1000, and at the size jumps to the loop position or ends. R16[i] is
     * 16*(i+1), so L reads as the index played. R16 goes on past 100 and has
     * -1000, -2000 just before it, so a read outside shows. */
    for (unsigned c = 0; c < sizeof cs / sizeof cs[0]; c++) {
        step("pcm: R16 size %d loop %d, 3 cores (claim start at L[32], loop 32 late; sascore.c:191-201,454-462)", cs[c].size, cs[c].loop);
        fresh();
        pcm_voice(0, R16, cs[c].size, cs[c].loop);
        env_flat(0);
        keyon(0);
        render(3, 0);
        log_ranges(3 * 256);
        log_ch(0, 28, 12);
        log_end(0, 3);
        log_sum();
        save(cs[c].file);
    }
    step("pcm: KeyOn again after the one-shot ended, 1 core");
    fresh();
    pcm_voice(0, R16, 100, -1);
    env_flat(0);
    keyon(0);
    render(2, 0);
    out("  KeyOff %08X", (w32)__sceSasSetKeyOff(core, 0));
    out(" KeyOn %08X\n", (w32)__sceSasSetKeyOn(core, 0));
    render(1, 0);
    log_ranges(256);

    step("pcm: SetVoicePCM to a constant 5000 while R16 loop 0 plays, after core 1");
    fresh();
    fill_const(PCM(1), PCM_N, 5000);
    pcm_voice(0, R16, 100, 0);
    env_flat(0);
    keyon(0);
    {
        const int w = grain_words();
        core_raw();
        memcpy(g_cap, g_out, w * 2);
        out("  SetVoicePCM %08X\n", (w32)__sceSasSetVoicePCM(core, 0, PCM(1), 100, 0));
        for (int i = 1; i < 3; i++) { core_raw(); memcpy(g_cap + i * w, g_out, w * 2); }
        g_capn = 3 * w;
    }
    log_ranges(3 * 256);
    log_ch(0, 250, 16);

    /* Accepted by the signed check (sascore.c:565); psprecomp plays it as no
     * loop (sascore.c:460). Last: a firmware that jumps there reads R16[-2]. */
    step("pcm: R16 size 100 loop -2, 3 cores");
    fresh();
    {
        int r = __sceSasSetVoicePCM(core, 0, R16, 100, -2);
        out("  SetVoicePCM %08X\n", (w32)r);
        if (r == 0) {
            pitch_vol(0);
            env_flat(0);
            keyon(0);
            render(3, 0);
            log_ranges(3 * 256);
            log_end(0, 3);
            save("pcm_loop_m2.bin");
        }
    }
}

/* ============================================================================
 * pitch
 * ==========================================================================*/

static void sec_pitch(void) {
    static const int ps[] = { 0x1000, 0x800, 0x2000, 0x4000, 0xC00, 0x1800, 0x1001, 0xFFF, 0x100, 0x1, 0 };
    char name[32];
    section("pitch");
    /* sascore.c:451-458: 0x1000 is the source rate; each output adds the
     * pitch to a 12-bit fraction and steps a sample per 0x1000 (no
     * interpolation). RAMP[i] = 4*(i+1), so L/4 - 1 is the position reached. */
    for (unsigned k = 0; k < sizeof ps / sizeof ps[0]; k++) {
        step("pitch: 0x%X on RAMP (4*(i+1)), 3 cores: first samples and the last of each core (sascore.c:451-458)", ps[k]);
        fresh();
        pcm_voice(0, RAMP, RAMP_N, -1);
        env_flat(0);
        int r = __sceSasSetPitch(core, 0, ps[k]);
        if (r) out("  SetPitch %08X\n", (w32)r);
        keyon(0);
        render(3, 0);
        log_ch(0, 32, 16);
        out("  last L of each core: %d %d %d\n", CL(255), CL(511), CL(767));
        log_end(0, 3);
        log_sum();
        sprintf(name, "pitch_%04X.bin", ps[k]);
        save(name);
    }
    step("pitch: 0x1000 for core 1, 0x2000 for core 2, 0x800 for core 3");
    fresh();
    pcm_voice(0, RAMP, RAMP_N, -1);
    env_flat(0);
    keyon(0);
    {
        const int w = grain_words();
        for (int i = 0; i < 3; i++) {
            if (i) __sceSasSetPitch(core, 0, i == 1 ? 0x2000 : 0x800);
            core_raw();
            memcpy(g_cap + i * w, g_out, w * 2);
        }
        g_capn = 3 * w;
    }
    out("  last L of each core: %d %d %d\n", CL(255), CL(511), CL(767));
    log_ch(0, 252, 8);
    log_ch(0, 508, 8);
    save("pitch_change.bin");
}

/* ============================================================================
 * noise
 * ==========================================================================*/

static void noise_stats(void) {
    int mn = 32767, mx = -32768, changes = 0, signs = 0;
    for (int i = 33; i < frames(); i++) {
        const int v = CL(i), p = CL(i - 1);
        if (v < mn) mn = v;
        if (v > mx) mx = v;
        if (v != p) changes++;
        if ((v < 0) != (p < 0)) signs++;
    }
    out("  min %d max %d, value changes %d, sign changes %d, L != R %d\n", mn, mx, changes, signs, lr_diff());
}

static void sec_noise(void) {
    static const int fs[] = { 0, 1, 8, 16, 32, 48, 63 };
    char name[32];
    w32 first32 = 0;
    section("noise");
    /* sascore.c:584-591: psprecomp checks the frequency and plays no noise. */
    for (unsigned k = 0; k < sizeof fs / sizeof fs[0]; k++) {
        step("noise: SetNoise freq %d, flat envelope, 4 cores (psprecomp has no generator; sascore.c:584-591)", fs[k]);
        fresh();
        pcm_voice(0, PCM(0), 256, 0);
        out("  SetNoise %08X\n", (w32)__sceSasSetNoise(core, 0, fs[k]));
        env_flat(0);
        keyon(0);
        render(4, 0);
        log_sum();
        log_ch(0, 30, 16);
        noise_stats();
        log_end(0, 4);
        if (fs[k] == 32) first32 = fnv(g_cap, g_capn * 2);
        sprintf(name, "noise_f%02d.bin", fs[k]);
        save(name);
    }
    /* Whether the generator restarts with an Init or a key-on, or runs on. */
    step("noise: freq 32 again after Init, then after KeyOff+KeyOn with no core between: same 4 cores as the first?");
    fresh();
    pcm_voice(0, PCM(0), 256, 0);
    __sceSasSetNoise(core, 0, 32);
    env_flat(0);
    keyon(0);
    render(4, 0);
    out("  after Init: %s", fnv(g_cap, g_capn * 2) == first32 ? "same" : "different");
    out("; KeyOff %08X", (w32)__sceSasSetKeyOff(core, 0));
    out(" KeyOn %08X", (w32)__sceSasSetKeyOn(core, 0));
    render(4, 0);
    out("; after re-key: %s\n", fnv(g_cap, g_capn * 2) == first32 ? "same" : "different");
    log_ch(0, 30, 8);
    step("noise: SetNoise 32 then SetVoicePCM(constant 0x1000): which plays?");
    fresh();
    __sceSasSetNoise(core, 0, 32);
    pcm_voice(0, PCM(0), 256, 0);
    env_flat(0);
    keyon(0);
    render(1, 0);
    log_ch(0, 30, 16);
}

/* ============================================================================
 * outmode: output modes
 * ==========================================================================*/

static void sec_outmode(void) {
    section("outmode");
    /* 64 samples of -33, then 64 of 1001, looping. */
    for (int i = 0; i < 128; i++) PCM(1)[i] = (short)(i < 64 ? -33 : 1001);

    /* sascore.c:865-884: mode 1 writes four mono blocks of `grain` samples
     * (dry L, dry R, send L, send R); -33 at 0x1000/0xC00/0x800/0x400 reads
     * -33, -25, -17, -9. */
    step("outmode: SetOutputmode(1), volumes 1000/C00/800/400, 2 cores: blocks at 40, 100, 200 (claim four mono blocks; sascore.c:865-884)");
    fresh();
    {
        int r = __sceSasSetOutputmode(core, 1);
        out("  SetOutputmode %08X, GetOutputmode %d\n", (w32)r, __sceSasGetOutputmode(core));
        if (r == 0) g_mode = 1;
    }
    pcm_voice(0, PCM(1), 128, 0);
    out("  SetVolume %08X\n", (w32)__sceSasSetVolume(core, 0, 0x1000, 0xC00, 0x800, 0x400));
    env_flat(0);
    keyon(0);
    render(1, 0);
    out("  shorts written by the first core: %d\n", out_extent());
    render(2, 0);
    if (g_mode == 1) {
        for (int b = 0; b < 4; b++)
            out("  block %d: [40] %d [100] %d [200] %d\n", b, g_cap[b * 256 + 40], g_cap[b * 256 + 100], g_cap[b * 256 + 200]);
    }
    log_sum();
    save("outmode1.bin");

    step("outmode: Init with outputMode 1, GetOutputmode");
    out("  Init %08X", (w32)__sceSasInit(core, 256, 32, 1, 44100));
    out(" GetOutputmode %d\n", __sceSasGetOutputmode(core));

    /* The PSPSDK header says mode 0 has "dry + send already mixed together". */
    step("outmode: mode 0, same voice, volumes 1000/C00/800/400: L and R at 40, 100, 200");
    fresh();
    pcm_voice(0, PCM(1), 128, 0);
    __sceSasSetVolume(core, 0, 0x1000, 0xC00, 0x800, 0x400);
    env_flat(0);
    keyon(0);
    render(2, 0);
    out("  shorts written by the second core: %d\n", out_extent());
    out("  [40] %d/%d [100] %d/%d [200] %d/%d\n", CL(40), CR(40), CL(100), CR(100), CL(200), CR(200));
    log_sum();
    save("outmode0.bin");

    step("outmode: mode 0 with sends 0 and with sends 1000/1000 (do the sends reach L and R?)");
    fresh();
    pcm_voice(0, PCM(1), 128, 0);
    env_flat(0);
    keyon(0);
    core_raw();
    __sceSasSetVolume(core, 0, 0x800, 0x800, 0, 0);
    core_raw();
    out("  sends 0: [100] %d/%d", g_out[200], g_out[201]);
    __sceSasSetVolume(core, 0, 0x800, 0x800, 0x1000, 0x1000);
    core_raw();
    out("; sends 1000: [100] %d/%d\n", g_out[200], g_out[201]);

    /* sascore.c:246-250,887-888: voices sum, then clamp to 16 bits. */
    step("outmode: two voices summing: 1000+2000, and 30000+30000 (claim clamp at 32767; sascore.c:887-888)");
    for (int k = 0; k < 2; k++) {
        fresh();
        fill_const(PCM(2), PCM_N, k ? 30000 : 1000);
        fill_const(PCM(3), PCM_N, k ? 30000 : 2000);
        pcm_voice(0, PCM(2), 256, 0);
        pcm_voice(1, PCM(3), 256, 0);
        env_flat(0);
        env_flat(1);
        keyon(0);
        keyon(1);
        render(2, -1);
        out("  L[100] %d R[100] %d L[300] %d\n", CL(100), CR(100), CL(300));
    }
    step("outmode: negative voice volume -0x1000 left, 0x800 right on constant 0x1000");
    fresh();
    pcm_voice(0, PCM(0), 256, 0);
    out("  SetVolume %08X\n", (w32)__sceSasSetVolume(core, 0, -0x1000, 0x800, 0, 0));
    env_flat(0);
    keyon(0);
    render(2, -1);
    out("  L[100] %d R[100] %d\n", CL(100), CR(100));
}

/* ============================================================================
 * mix: __sceSasCoreWithMix
 * ==========================================================================*/

static int mix_call(int lv, int rv, int pl, int pr) {
    const int n = fill_n();
    for (int i = 0; i < n; i++) g_out[i] = (short)0xA5A5;
    for (int i = 0; i < g_grain; i++) { g_out[2 * i] = (short)pl; g_out[2 * i + 1] = (short)pr; }
    sync();
    int r = __sceSasCoreWithMix(core, g_out, lv, rv);
    sync();
    return r;
}

static void mix_line(int lv, int rv, int pl, int pr) {
    int r = mix_call(lv, rv, pl, pr);
    out("  mix(0x%X, 0x%X) over %d/%d = %08X: [10] %d/%d [100] %d/%d, extent %d\n", (w32)lv, (w32)rv, pl, pr,
        (w32)r, g_out[20], g_out[21], g_out[200], g_out[201], out_extent());
}

static void sec_mix(void) {
    section("mix");
    /* sascore.c:854-859,889-891: leftMix/rightMix scale what is already in the
     * buffer, 12-bit, and the rendered voices are added; 0 leaves the
     * rendered samples alone. */
    step("mix: nothing playing, buffer 1000/-2000 (claim buffer*mix>>12 added; sascore.c:854-859,889-891)");
    fresh();
    mix_line(0, 0, 1000, -2000);
    mix_line(0x1000, 0x1000, 1000, -2000);
    mix_line(0x800, 0x400, 1000, -2000);
    mix_line(0x1000, 0, 1000, -2000);
    mix_line(0x1001, 0x1000, 1000, -2000);
    mix_line(-1, 0x1000, 1000, -2000);
    mix_line(0x2000, 0x2000, 1000, -2000);
    mix_line(0x7FFFFFFF, 0, 1000, -2000);
    step("mix: voice playing a constant 2000 at full volume");
    fill_const(PCM(1), PCM_N, 2000);
    pcm_voice(0, PCM(1), 256, 0);
    env_flat(0);
    keyon(0);
    core_raw();
    mix_line(0, 0, 1000, -2000);
    mix_line(0x1000, 0x1000, 1000, -2000);
    mix_line(0x800, 0x800, 1000, -2000);
    mix_line(0x1000, 0x1000, 31000, -32000);
    /* sascore.c:907-912: refused in output mode 1 with 0x80000004, buffer untouched. */
    step("mix: in output mode 1 (claim 80000004 and nothing written; sascore.c:907-912)");
    {
        int r1 = __sceSasSetOutputmode(core, 1);
        int r = mix_call(0x1000, 0x1000, 1000, -2000);
        int changed = 0;
        for (int i = 0; i < fill_n(); i++) {
            short want = i < 2 * g_grain ? (short)(i & 1 ? -2000 : 1000) : (short)0xA5A5;
            if (g_out[i] != want) changed++;
        }
        out("  SetOutputmode(1) %08X, CoreWithMix %08X, %d shorts changed\n", (w32)r1, (w32)r, changed);
        __sceSasSetOutputmode(core, 0);
    }
}

/* ============================================================================
 * grain
 * ==========================================================================*/

static void sec_grain(void) {
    static const int gs[] = { 0, 32, 63, 64, 65, 96, 100, 128, 160, 192, 256, 1024, 2016, 2048, 2049,
                              2080, 4096, -1, -64 };
    section("grain");
    fresh();
    /* sascore.c:22-23,61,505-512: 64..2048 and a multiple of 32. */
    step("grain: SetGrain then GetGrain (claim 80420001 outside 64..2048 or off a multiple of 32; sascore.c:505-512)");
    for (unsigned i = 0; i < sizeof gs / sizeof gs[0]; i++) {
        int r = __sceSasSetGrain(core, gs[i]);
        out("  %d: %08X, GetGrain %d\n", gs[i], (w32)r, __sceSasGetGrain(core));
    }
    __sceSasSetGrain(core, 256);
    step("grain: shorts one core writes at grain 64, 96, 128, 2048, 256 (pattern-filled buffer)");
    fresh();
    pcm_voice(0, PCM(0), 256, 0);
    env_flat(0);
    keyon(0);
    {
        static const int ts[] = { 64, 96, 128, 2048, 256 };
        for (unsigned i = 0; i < sizeof ts / sizeof ts[0]; i++) {
            int r = __sceSasSetGrain(core, ts[i]);
            int gg = __sceSasGetGrain(core);
            if (gg >= 64 && gg <= 2048) g_grain = gg;
            int c = core_raw();
            out("  grain %d: SetGrain %08X GetGrain %d core %08X, %d shorts written, L[40] %d\n",
                ts[i], (w32)r, gg, (w32)c, out_extent(), g_out[80]);
        }
    }
}

/* ============================================================================
 * endflag
 * ==========================================================================*/

static void sec_endflag(void) {
    section("endflag");
    fresh();
    /* sascore.c:827-834: a bit per voice, set when the voice is not playing. */
    step("endflag: after Init and one core with nothing playing (sascore.c:827-834)");
    out("  %08X\n", (w32)__sceSasGetEndFlag(core));

    step("endflag: v0 PCM 100 one-shot, v5 PCM 600 one-shot, v17 VAG 4 blocks ending flag 1, v31 PCM loop (key off before core 2, instant release)");
    vag_clear(VAG);
    for (int b = 0; b < 4; b++) vag_blk_c(VAG, 0, 4, b == 3 ? 1 : 0, b + 1);
    pcm_voice(0, R16, 100, -1);
    pcm_voice(5, RAMP, 600, -1);
    vag_voice(17, VAG, 64, 0);
    pcm_voice(31, PCM(0), 256, 0);
    env_flat(0); env_flat(5); env_flat(17);
    env(31, 0, FULL, 1, 0, 1, 0, 1, FULL, TOP);
    {
        w32 e0 = (w32)__sceSasGetEndFlag(core);
        keyon(0); keyon(5); keyon(17); keyon(31);
        w32 e1 = (w32)__sceSasGetEndFlag(core);
        render_ko(4, -1, 2, 31);
        out("  before KeyOn %08X, after KeyOn %08X, after cores %08X %08X %08X %08X\n",
            e0, e1, g_ef[0], g_ef[1], g_ef[2], g_ef[3]);
    }
    step("endflag: Init with maxVoices 4, one core");
    out("  Init %08X", (w32)__sceSasInit(core, 256, 4, 0, 44100));
    core_raw();
    out(", end flag %08X\n", (w32)__sceSasGetEndFlag(core));
}

/* ============================================================================
 * pause
 * ==========================================================================*/

static void sec_pause(void) {
    section("pause");
    fresh();
    /* sascore.c:812-825: psprecomp records the bits whether or not the voice plays. */
    step("pause: SetPause(0x80000021, 1) with nothing playing, then GetPauseFlag (sascore.c:812-825)");
    out("  %08X, flag %08X\n", (w32)__sceSasSetPause(core, 0x80000021u, 1), (w32)__sceSasGetPauseFlag(core));
    step("pause: SetPause(1, 0), GetPauseFlag");
    out("  %08X, flag %08X\n", (w32)__sceSasSetPause(core, 1, 0), (w32)__sceSasGetPauseFlag(core));
    step("pause: SetPause(0xFFFFFFFF, 2), GetPauseFlag, then SetPause(0xFFFFFFFF, 0)");
    out("  %08X, flag %08X", (w32)__sceSasSetPause(core, 0xFFFFFFFFu, 2), (w32)__sceSasGetPauseFlag(core));
    out("; %08X, flag %08X\n", (w32)__sceSasSetPause(core, 0xFFFFFFFFu, 0), (w32)__sceSasGetPauseFlag(core));

    step("pause: RAMP voice (attack lin-inc 0x100000) paused before core 1, resumed before core 3, 5 cores");
    fresh();
    pcm_voice(0, RAMP, RAMP_N, -1);
    env(0, 0, 0x100000, 1, 0, 1, 0, 1, 0, TOP);
    keyon(0);
    {
        const int w = grain_words();
        w32 pf[5];
        for (int i = 0; i < 5; i++) {
            if (i == 1) out("  SetPause(1,1) %08X\n", (w32)__sceSasSetPause(core, 1, 1));
            if (i == 3) out("  SetPause(1,0) %08X\n", (w32)__sceSasSetPause(core, 1, 0));
            core_raw();
            memcpy(g_cap + i * w, g_out, w * 2);
            g_ef[i] = (w32)__sceSasGetEndFlag(core);
            g_h[i] = (w32)__sceSasGetEnvelopeHeight(core, 0);
            pf[i] = (w32)__sceSasGetPauseFlag(core);
        }
        g_capn = 5 * w;
        log_heights(5);
        log_end(0, 5);
        out("  pause flags %08X %08X %08X %08X %08X\n", pf[0], pf[1], pf[2], pf[3], pf[4]);
        for (int i = 0; i < 5; i++) out("  core %d: first L %d, last L %d\n", i, CL(i * 256), CL(i * 256 + 255));
        save("pause_ramp.bin");
    }

    step("pause: KeyOff on a paused voice that is on (release lin-dec 0x100000)");
    fresh();
    pcm_voice(0, PCM(0), 256, 0);
    env(0, 0, FULL, 1, 0, 1, 0, 1, 0x100000, TOP);
    keyon(0);
    core_raw();
    out("  SetPause %08X", (w32)__sceSasSetPause(core, 1, 1));
    out(" KeyOff %08X", (w32)__sceSasSetKeyOff(core, 0));
    out(" flag %08X\n", (w32)__sceSasGetPauseFlag(core));
    core_raw();
    out("  paused core: height %08X end %08X\n", (w32)__sceSasGetEnvelopeHeight(core, 0), (w32)__sceSasGetEndFlag(core));
    out("  SetPause(1,0) %08X\n", (w32)__sceSasSetPause(core, 1, 0));
    core_raw();
    out("  resumed core: height %08X end %08X\n", (w32)__sceSasGetEnvelopeHeight(core, 0), (w32)__sceSasGetEndFlag(core));

    /* sascore.c:756-761: a paused voice takes no key-on, even one that is off. */
    step("pause: KeyOn on a paused voice that is off (claim 80420016; sascore.c:756-761)");
    fresh();
    pcm_voice(1, PCM(0), 256, 0);
    env_flat(1);
    out("  SetPause %08X", (w32)__sceSasSetPause(core, 2, 1));
    out(" KeyOn %08X\n", (w32)__sceSasSetKeyOn(core, 1));
    /* sascore.c:794-796: nor a key-off. */
    step("pause: KeyOff on a paused voice that is off (claim 80420016; sascore.c:794-796)");
    ret(__sceSasSetKeyOff(core, 1));
    __sceSasSetPause(core, 2, 0);
    step("pause: SetPause(3, 1) then Init: pause flags");
    out("  SetPause %08X", (w32)__sceSasSetPause(core, 3, 1));
    out(" Init %08X", (w32)__sceSasInit(core, 256, 32, 0, 44100));
    out(" flag %08X\n", (w32)__sceSasGetPauseFlag(core));
    __sceSasSetPause(core, 0xFFFFFFFFu, 0);
}

/* ============================================================================
 * heights: __sceSasGetAllEnvelopeHeights
 * ==========================================================================*/

static void sec_heights(void) {
    section("heights");
    fresh();
    pcm_voice(0, PCM(0), 256, 0); env_flat(0);
    pcm_voice(1, PCM(0), 256, 0); env(1, 0, 0x10000, 1, 0, 1, 0, 1, 0, TOP);
    pcm_voice(2, PCM(0), 256, 0); env_flat(2);
    pcm_voice(3, PCM(0), 256, 0); env(3, 0, FULL, 1, 0, 1, 0, 1, 0x100000, TOP);
    pcm_voice(31, PCM(0), 256, 0); env_flat(31);
    keyon(0); keyon(1); keyon(3); keyon(31);
    render_ko(3, -1, 2, 3);
    /* sascore.c:842-852: exactly 32 ints written. */
    step("heights: GetAllEnvelopeHeights into 40 words of 0xCCCCCCCC (claim exactly 32 written; sascore.c:842-852)");
    for (int i = 0; i < 40; i++) g_hts[i] = (int)0xCCCCCCCC;
    sync();
    {
        int r = __sceSasGetAllEnvelopeHeights(core, g_hts);
        int n = 0, last = -1;
        sync();
        for (int i = 0; i < 40; i++) if ((w32)g_hts[i] != 0xCCCCCCCCu) { n++; last = i; }
        out("  = %08X, %d words written, last %d\n", (w32)r, n, last);
        out("  [0..3] %08X %08X %08X %08X [31] %08X [32] %08X [33] %08X\n", (w32)g_hts[0], (w32)g_hts[1],
            (w32)g_hts[2], (w32)g_hts[3], (w32)g_hts[31], (w32)g_hts[32], (w32)g_hts[33]);
        out("  GetEnvelopeHeight 0..3: %08X %08X %08X %08X\n", (w32)__sceSasGetEnvelopeHeight(core, 0),
            (w32)__sceSasGetEnvelopeHeight(core, 1), (w32)__sceSasGetEnvelopeHeight(core, 2),
            (w32)__sceSasGetEnvelopeHeight(core, 3));
    }
}

/* ============================================================================
 * reverb (sascore.c:917-919 accepts every call)
 * ==========================================================================*/

static void sec_reverb(void) {
    section("reverb");
    fresh();
    step("reverb: RevType -2..9 (psprecomp accepts all; sascore.c:917-919)");
    out(" ");
    for (int t = -2; t <= 9; t++) out(" %d:%08X", t, (w32)__sceSasRevType(core, t));
    out("\n");
    step("reverb: RevParam(delay, feedback)");
    {
        static const int ps[][2] = { { 0, 0 }, { 64, 64 }, { 128, 128 }, { 129, 0 }, { 0, 129 }, { -1, 0 }, { 0, -1 } };
        for (unsigned i = 0; i < sizeof ps / sizeof ps[0]; i++)
            out("  (%d, %d): %08X\n", ps[i][0], ps[i][1], (w32)__sceSasRevParam(core, ps[i][0], ps[i][1]));
    }
    step("reverb: RevEVOL(left, right)");
    {
        static const int ps[][2] = { { 0, 0 }, { 0x1000, 0x1000 }, { 0x1001, 0 }, { 0, 0x1001 }, { -1, 0 },
                                     { -0x1000, -0x1000 }, { -0x1001, 0 } };
        for (unsigned i = 0; i < sizeof ps / sizeof ps[0]; i++)
            out("  (0x%X, 0x%X): %08X\n", (w32)ps[i][0], (w32)ps[i][1], (w32)__sceSasRevEVOL(core, ps[i][0], ps[i][1]));
    }
    step("reverb: RevVON(dry, wet)");
    {
        static const int ps[][2] = { { 0, 0 }, { 1, 1 }, { 1, 0 }, { 0, 1 }, { 2, 0 }, { 0, 2 }, { -1, 0 } };
        for (unsigned i = 0; i < sizeof ps / sizeof ps[0]; i++)
            out("  (%d, %d): %08X\n", ps[i][0], ps[i][1], (w32)__sceSasRevVON(core, ps[i][0], ps[i][1]));
    }
    step("reverb: hall, a 100-sample burst of 8000 with sends 1000, 16 cores");
    fresh();
    out("  RevType %08X", (w32)__sceSasRevType(core, 4));
    out(" RevParam %08X", (w32)__sceSasRevParam(core, 64, 64));
    out(" RevEVOL %08X", (w32)__sceSasRevEVOL(core, 0x1000, 0x1000));
    out(" RevVON %08X\n", (w32)__sceSasRevVON(core, 1, 1));
    fill_const(PCM(1), PCM_N, 8000);
    pcm_voice(0, PCM(1), 100, -1);
    __sceSasSetVolume(core, 0, 0x1000, 0x1000, 0x1000, 0x1000);
    env_flat(0);
    keyon(0);
    render(16, -1);
    {
        int nz = 0;
        for (int i = 200; i < frames(); i++) if (CL(i) || CR(i)) nz++;
        out("  frames after 200 that are not silent: %d; L[300] %d L[1000] %d L[2000] %d\n", nz, CL(300), CL(1000), CL(2000));
    }
    log_sum();
    save("rev_hall.bin");
    __sceSasRevType(core, -1);
    __sceSasRevEVOL(core, 0, 0);
    __sceSasRevVON(core, 0, 0);
}

/* ============================================================================
 * waves: steep and triangular (not in psprecomp)
 * ==========================================================================*/

static void sec_waves(void) {
    static const int ds[] = { -1, 0, 50, 100, 101 };
    section("waves");
    fresh();
    step("waves: SetSteepWave and SetTriangularWave with -1, 0, 50, 100, 101 (not implemented in psprecomp)");
    out("  steep:");
    for (unsigned i = 0; i < 5; i++) out(" %d:%08X", ds[i], (w32)__sceSasSetSteepWave(core, 0, ds[i]));
    out("\n  triangular:");
    for (unsigned i = 0; i < 5; i++) out(" %d:%08X", ds[i], (w32)__sceSasSetTriangularWave(core, 0, ds[i]));
    out("\n");
    for (int k = 0; k < 2; k++) {
        step("waves: %s wave 50, pitch 441, flat envelope, 2 cores", k ? "triangular" : "steep");
        fresh();
        pcm_voice(0, PCM(0), 256, 0);
        out("  Set %08X", (w32)(k ? __sceSasSetTriangularWave(core, 0, 50) : __sceSasSetSteepWave(core, 0, 50)));
        out(" SetPitch %08X\n", (w32)__sceSasSetPitch(core, 0, 441));
        env_flat(0);
        keyon(0);
        render(2, 0);
        log_sum();
        log_ch(0, 30, 24);
        save(k ? "wave_tri50.bin" : "wave_steep50.bin");
    }
}

/* ============================================================================
 * badptr: null pointers, last of all
 * ==========================================================================*/

static void sec_badptr(void) {
    section("badptr");
    fresh();
    /* sascore.c:25-26,490: a null core refused with 80420005. */
    step("badptr: __sceSasInit(NULL, 256, 32, 0, 44100) (claim 80420005; sascore.c:25-26,490)");
    ret(__sceSasInit(NULL, 256, 32, 0, 44100));
    /* sascore.c:846-848: psprecomp refuses a null buffer with 80420005. */
    /* sascore.c:420-422,557-558: a null PCM address is accepted. No key-on
     * follows either of these; the voice is pointed back at real data. */
    step("badptr: SetVoicePCM(0, NULL, 16, -1) (claim accepted; sascore.c:420-422,557-558)");
    ret(__sceSasSetVoicePCM(core, 0, NULL, 16, -1));
    __sceSasSetVoicePCM(core, 0, PCM(0), 64, -1);
    step("badptr: SetVoice(0, NULL, 32, 0)");
    ret(__sceSasSetVoice(core, 0, NULL, 32, 0));
    __sceSasSetVoicePCM(core, 0, PCM(0), 64, -1);
    step("badptr: __sceSasGetAllEnvelopeHeights(core, NULL) (psprecomp 80420005; sascore.c:846-848)");
    ret(__sceSasGetAllEnvelopeHeights(core, NULL));
    step("badptr: __sceSasCore(NULL, out)");
    ret(__sceSasCore(NULL, g_out));
}

/* ============================================================================ */

int main(int argc, char **argv) {
    probe_init("sasprobe", PROBE_VERSION, argc, argv);
    core = (SceSasCore *)g_core_mem;

    for (int i = 0; i < RAMP_N; i++) RAMP[i] = (short)(4 * (i + 1));
    for (int i = 0; i < R16_N; i++) R16[i] = (short)(16 * (i + 1));
    R16[-1] = -1000;
    R16[-2] = -2000;
    fill_const(PCM(0), PCM_N, 0x1000);

    sec_module();
    sec_init();
    sec_layout();
    sec_args();
    sec_adsrmode();
    sec_simple();
    sec_keys();
    sec_envscale();
    sec_adsr();
    sec_vag();
    sec_vagflags();
    sec_pcm();
    sec_pitch();
    sec_noise();
    sec_outmode();
    sec_mix();
    sec_grain();
    sec_endflag();
    sec_pause();
    sec_heights();
    sec_reverb();
    sec_waves();
    sec_badptr();
    probe_done();
    return 0;
}
