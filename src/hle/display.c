/* psprecomp — sceDisplay.
 *
 * The PSP's display is a framebuffer address plus a pixel format, scanned out
 * at ~59.94 Hz. There is no blitting and no compositing: a game writes pixels
 * (usually via the GE) and tells sceDisplay where they are.
 *
 * That makes this small but pivotal — it is the first point in bring-up where
 * a recompiled game produces something you can *look at*. psp_display_capture()
 * exists for exactly that: dump whatever the game currently considers the
 * front buffer, and compare it against the same frame from an emulator.
 */

#include "psprecomp/clock.h"
#include "psprecomp/sched.h"
#include "psprecomp/hle.h"
#include "psprecomp/mem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PSP_SCREEN_W 480
#define PSP_SCREEN_H 272

/* Pixel formats, as passed to sceDisplaySetFrameBuf. */
#define PSP_DISPLAY_PIXEL_FORMAT_565  0
#define PSP_DISPLAY_PIXEL_FORMAT_5551 1
#define PSP_DISPLAY_PIXEL_FORMAT_4444 2
#define PSP_DISPLAY_PIXEL_FORMAT_8888 3

static uint32_t g_fb_addr;
static uint32_t g_fb_width;      /* in pixels, the stride -- usually 512 */
static uint32_t g_fb_format;
static uint32_t g_mode, g_mode_w, g_mode_h;
static uint64_t g_vblank_count;
/* The boundary the most recent vblank was counted at, so that several threads
 * released by one vblank count it once between them. See hle_WaitVblank. */
static uint64_t g_last_vblank_us;
static uint32_t g_vcount;         /* scanline counter; see hle_GetVcount */

static uint32_t g_best[PSP_SCREEN_W * PSP_SCREEN_H];
static uint64_t g_best_score;
static uint32_t g_best_addr;
static uint64_t g_frames_scored;

/* The presentation hook.
 *
 * A windowed host wants each frame as it is shown, not a dump after the run
 * ends. The hook is called from hle_SetFrameBuf -- the moment the game hands
 * the display a finished buffer -- because that is the only cadence this game
 * actually keeps: it never calls sceDisplayWaitVblank (four hundred
 * SetFrameBufs against none in the top twelve of the call histogram), and
 * paces itself with GetAccumulatedHcount polls and delays instead. Waiting
 * for a vblank that is never requested publishes nothing, which is a black
 * window that looks exactly like a broken renderer.
 *
 * The callee runs holding the scheduler token, so it must be quick: convert
 * and hand off, no blocking. SDL stays in the host; the library only learns
 * that *someone* may want to look. */
static void (*g_present)(uint32_t addr, uint32_t stride, uint32_t fmt);

void psp_display_set_present(void (*fn)(uint32_t addr, uint32_t stride,
                                        uint32_t fmt)) {
    g_present = fn;
}

void psp_display_reset(void) {
    g_fb_addr = 0;
    g_fb_width = 512;
    g_fb_format = PSP_DISPLAY_PIXEL_FORMAT_8888;
    g_mode = 0;
    g_mode_w = PSP_SCREEN_W;
    g_mode_h = PSP_SCREEN_H;
    g_vblank_count = 0;
    g_last_vblank_us = 0;
    g_vcount = 0;
    g_best_score = 0;
    g_best_addr = 0;
    g_frames_scored = 0;
}

void psp_display_init(void) { psp_display_reset(); }

uint64_t psp_display_vblanks(void) { return g_vblank_count; }
uint32_t psp_display_framebuffer(void) { return g_fb_addr; }
uint32_t psp_display_stride(void)      { return g_fb_width; }
uint32_t psp_display_format(void)      { return g_fb_format; }

/* Expand one source pixel to RGBA8888. The 16-bit formats replicate their high
 * bits into the low ones on expansion; simply shifting left leaves the maximum
 * value slightly below full white, which shows up as a washed-out image. */
static void expand(uint32_t px, uint32_t fmt, uint8_t out[4]) {
    switch (fmt) {
    case PSP_DISPLAY_PIXEL_FORMAT_565: {
        uint32_t r = (px & 0x1F), g = (px >> 5) & 0x3F, b = (px >> 11) & 0x1F;
        out[0] = (uint8_t)((r << 3) | (r >> 2));
        out[1] = (uint8_t)((g << 2) | (g >> 4));
        out[2] = (uint8_t)((b << 3) | (b >> 2));
        out[3] = 255;
        break;
    }
    case PSP_DISPLAY_PIXEL_FORMAT_5551: {
        uint32_t r = (px & 0x1F), g = (px >> 5) & 0x1F, b = (px >> 10) & 0x1F;
        out[0] = (uint8_t)((r << 3) | (r >> 2));
        out[1] = (uint8_t)((g << 3) | (g >> 2));
        out[2] = (uint8_t)((b << 3) | (b >> 2));
        out[3] = (px & 0x8000) ? 255 : 0;
        break;
    }
    case PSP_DISPLAY_PIXEL_FORMAT_4444: {
        uint32_t r = (px & 0xF), g = (px >> 4) & 0xF, b = (px >> 8) & 0xF, a = (px >> 12) & 0xF;
        out[0] = (uint8_t)((r << 4) | r);
        out[1] = (uint8_t)((g << 4) | g);
        out[2] = (uint8_t)((b << 4) | b);
        out[3] = (uint8_t)((a << 4) | a);
        break;
    }
    default:
        out[0] = (uint8_t)(px);
        out[1] = (uint8_t)(px >> 8);
        out[2] = (uint8_t)(px >> 16);
        out[3] = (uint8_t)(px >> 24);
        break;
    }
}

/* Write the current framebuffer as a binary PPM. Returns 0 on success, -1 if
 * no framebuffer has been set yet (which is itself worth knowing during
 * bring-up: the game never got as far as showing anything). */
int psp_display_capture(const char *path) {
    if (!g_fb_addr) return -1;

    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", PSP_SCREEN_W, PSP_SCREEN_H);

    const int bpp = (g_fb_format == PSP_DISPLAY_PIXEL_FORMAT_8888) ? 4 : 2;
    for (int y = 0; y < PSP_SCREEN_H; y++) {
        for (int x = 0; x < PSP_SCREEN_W; x++) {
            uint32_t at = g_fb_addr + (uint32_t)(y * (int)g_fb_width + x) * (uint32_t)bpp;
            uint32_t px = (bpp == 4) ? psp_read32(at) : psp_read16(at);
            uint8_t rgba[4];
            expand(px, g_fb_format, rgba);
            fwrite(rgba, 1, 3, f);       /* PPM is RGB; alpha is dropped */
        }
    }
    fclose(f);
    return 0;
}

/* ---- the calls ----------------------------------------------------------- */

static void hle_SetMode(void) {
    g_mode   = psp_arg(0);
    g_mode_w = psp_arg(1);
    g_mode_h = psp_arg(2);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* The most-drawn frame of the run, kept as it goes past.
 *
 * Dumping at the end samples whatever the run happened to stop on, and a run
 * that stops just after a frame-start clear finds an empty buffer -- which
 * reads as "the renderer drew nothing" when it drew a whole frame and then
 * cleared for the next. Scoring every presented frame and keeping the fullest
 * one answers "did anything ever appear" rather than "was anything there at
 * the instant we looked". */
uint64_t        psp_display_best_score(void) { return g_best_score; }
uint32_t        psp_display_best_addr(void)  { return g_best_addr; }
const uint32_t *psp_display_best(void)       { return g_best; }

/* PSPRECOMP_FRAMES=<prefix> writes every Nth presented frame as
 * <prefix>-NNNN.ppm, N from PSPRECOMP_FRAMES_EVERY (default 30).
 *
 * The best-frame capture answers "did anything ever appear". It cannot answer
 * "is this what the game looks like", because one frame cannot tell a colour
 * bug from a bright moment or a camera move -- and a washed-out still is
 * exactly the case where those are indistinguishable. A spread across the run
 * can. */
static void dump_frame_seq(uint32_t base) {
    static const char *prefix; static int looked; static int every, n, written;
    if (!looked) {
        looked = 1;
        prefix = getenv("PSPRECOMP_FRAMES");
        if (prefix && !*prefix) prefix = NULL;
        const char *e = getenv("PSPRECOMP_FRAMES_EVERY");
        every = (e && *e) ? atoi(e) : 30;
        if (every < 1) every = 1;
    }
    if (!prefix || !base || g_fb_format != PSP_DISPLAY_PIXEL_FORMAT_8888) return;
    if (n++ % every) return;
    if (written >= 400) return;              /* ~150MB ceiling, stated not silent */

    char path[1024];
    snprintf(path, sizeof path, "%s-%04d.ppm", prefix, written);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", PSP_SCREEN_W, PSP_SCREEN_H);
    for (int y = 0; y < PSP_SCREEN_H; y++)
        for (int x = 0; x < PSP_SCREEN_W; x++) {
            const uint32_t p =
                psp_read32(base + (uint32_t)(y * (int)g_fb_width + x) * 4u);
            const uint8_t rgb[3] = { (uint8_t)(p & 0xFF), (uint8_t)((p >> 8) & 0xFF),
                                     (uint8_t)((p >> 16) & 0xFF) };
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
    written++;
}

static void score_frame(uint32_t base) {
    if (!base || g_fb_format != PSP_DISPLAY_PIXEL_FORMAT_8888) return;
    g_frames_scored++;
    /* Scored by how much the frame *varies*, not by how much of it is lit. A
     * solid fill -- which a fade is -- scores zero however bright it is, so
     * this answers "was there ever a picture" rather than "was the screen ever
     * on". Counting non-black instead ranked a white flash above real
     * imagery. */
    const uint32_t first = psp_read32(base) & 0x00FFFFFFu;
    uint64_t varied = 0;
    for (int y = 0; y < PSP_SCREEN_H; y++)
        for (int x = 0; x < PSP_SCREEN_W; x++)
            if ((psp_read32(base + (uint32_t)(y * (int)g_fb_width + x) * 4u)
                 & 0x00FFFFFFu) != first)
                varied++;
    if (varied <= g_best_score) return;
    g_best_score = varied;
    g_best_addr  = base;
    for (int y = 0; y < PSP_SCREEN_H; y++)
        for (int x = 0; x < PSP_SCREEN_W; x++)
            g_best[y * PSP_SCREEN_W + x] =
                psp_read32(base + (uint32_t)(y * (int)g_fb_width + x) * 4u);
}

static void hle_SetFrameBuf(void) {
    /* (topaddr, bufferwidth, pixelformat, sync) */
    g_fb_addr   = psp_arg(0);
    g_fb_width  = psp_arg(1);
    g_fb_format = psp_arg(2);
    if (!g_fb_width) g_fb_width = 512;
    score_frame(g_fb_addr);
    dump_frame_seq(g_fb_addr);
    /* The frame flip. What the game hands the display is what a window shows;
     * with sync==NEXTFRAME this is one vblank early, which for bring-up is
     * indistinguishable and does not drop anything. */
    if (g_present && g_fb_addr) g_present(g_fb_addr, g_fb_width, g_fb_format);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* The other half of SetFrameBuf, and it was missing entirely.
 *
 * An unregistered firmware call returns without touching the caller's
 * out-parameters, so the caller reads back whatever its stack happened to
 * hold. That is the failure state.md names as the shape of most bugs here --
 * a call that looks like it succeeded while writing nothing -- and it is what
 * nineteen of the gpu autotests were tripping over: pspautotests' screenshot
 * helper asks for the pixel format, gets stack garbage, and prints
 * "ERROR: Invalid format 2928" once per scanline where hardware prints
 * nothing at all.
 *
 * `sync` is ignored for the same reason WaitVblank returns immediately:
 * there is no scanout, so there is no moment to be early or late for. */
static void hle_GetFrameBuf(void) {
    /* (topaddr*, bufferwidth*, pixelformat*, sync) */
    const uint32_t p_addr = psp_arg(0), p_width = psp_arg(1), p_fmt = psp_arg(2);
    if (p_addr)  psp_write32(p_addr,  g_fb_addr);
    if (p_width) psp_write32(p_width, g_fb_width);
    if (p_fmt)   psp_write32(p_fmt,   g_fb_format);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* A vblank wait is a wait, and one vblank releases everybody waiting on it.
 *
 * There is no scanout, so the frame boundary is a moment on the clock's grid
 * rather than an event -- but it is still a *shared* moment, and both halves of
 * that matter. Yielding instead of waiting does not deschedule the caller at
 * all: the handoff picks the most urgent READY thread, which is the caller
 * again whenever it outranks everything else, so a priority-24 thread looping
 * `checkpoint(); WaitVblank();` ran its whole loop before its two equals
 * started theirs. threads/scheduling expects them interleaved one line per
 * frame, and psp_sched_delay is the operation that expresses it -- the same
 * reasoning already written out above that function.
 *
 * Advancing per caller was the other half. Three threads waiting on one vblank
 * each added a frame, so the guest saw three frames of time pass for one frame
 * of scanout. Parking them all on the same absolute moment costs one.
 *
 * A game's main loop is usually `render(); WaitVblank();`, so the counter is
 * still the frame number -- the most useful single number during bring-up,
 * because it says whether the game is looping or stuck. It is counted per
 * vblank rather than per waiter, which is what it was always meant to mean. */
static void hle_WaitVblank(void) {
    const uint64_t target = psp_clock_next_frame();

    psp_sched_delay(target - psp_clock_peek());
    /* The frame passes whether or not there was anyone to schedule around it:
     * with threading off -- the oracle's configuration -- the delay above parks
     * nobody, and the clock has to reach the boundary regardless. Monotonic, so
     * it does nothing when the wait already arrived there. */
    psp_clock_advance_to(target);

    /* First one through this boundary counts it; the rest woke on the same
     * vblank and must not count it again. */
    if (target > g_last_vblank_us) {
        g_last_vblank_us = target;
        g_vblank_count++;
    }
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* Scanline counters.
 *
 * On hardware these advance with the beam whether or not anyone is looking.
 * Here nothing advances on its own -- there is no scanout and no clock -- so a
 * counter that only moved when something else moved it would sit still, and
 * code of the form
 *
 *     start = sceDisplayGetVcount();
 *     while (sceDisplayGetVcount() == start) { }
 *
 * would never leave the loop. Reading the counter therefore advances it. That
 * is not faithful -- a caller timing itself against vcount sees time run fast
 * -- but the alternative is a hang, and the same trade is already made by
 * hle_WaitVblank above.
 *
 * The two counters are kept consistent with each other rather than invented
 * separately, because a caller that reads both and compares them would
 * otherwise see nonsense. PSP_HLINES_PER_FRAME is the total scanline count
 * including blanking; the absolute figure matters far less here than the
 * ratio between the two counters staying fixed. */
#define PSP_HLINES_PER_FRAME 286u

static void hle_GetVcount(void) {
    psp_ret(g_vcount++);
}

static void hle_GetAccumulatedHcount(void) {
    /* Derived, not independent: hcount is vcount's worth of scanlines plus
     * however far into the current frame we pretend to be. */
    psp_ret(g_vcount * PSP_HLINES_PER_FRAME);
    g_vcount++;
}

/* sceDisplayGetFramePerSec(void) -> float
 *
 * The refresh rate itself, as a float in $f0 -- not a status code in $v0, which
 * is the only reason its absence was survivable at all. Unimplemented, the call
 * returned zero and left $f0 holding whatever the guest had there.
 *
 * This game asks 28,470 times in a minute, so it is squarely on the frame path,
 * and a rate of zero poisons everything derived from it: a frame interval of
 * 1/fps is a division by zero, and a budget of `fps * seconds` is nothing to do.
 *
 * The clock rounds the frame *interval* to whole microseconds for its own
 * purposes (see PSP_FRAME_US in clock.c); this is the rate, which is what a
 * caller asking for frames per second wants, and the two should not be derived
 * from each other. */
#define PSP_REFRESH_HZ 59.940059f

static void hle_GetFramePerSec(void) {
    psp_cpu.f[0] = PSP_REFRESH_HZ;
}

void psp_display_register(void) {
    psp_hle_register(0x0E20F177, "sceDisplay", "sceDisplaySetMode",           hle_SetMode);
    psp_hle_register(0x289D82FE, "sceDisplay", "sceDisplaySetFrameBuf",       hle_SetFrameBuf);
    psp_hle_register(0xEEDA2E54, "sceDisplay", "sceDisplayGetFrameBuf",       hle_GetFrameBuf);
    psp_hle_register(0x36CDFADE, "sceDisplay", "sceDisplayWaitVblank",        hle_WaitVblank);
    psp_hle_register(0x984C27E7, "sceDisplay", "sceDisplayWaitVblankStart",   hle_WaitVblank);
    psp_hle_register(0x46F186C3, "sceDisplay", "sceDisplayWaitVblankStartCB", hle_WaitVblank);
    psp_hle_register(0x9C6EAAD7, "sceDisplay", "sceDisplayGetVcount",         hle_GetVcount);
    psp_hle_register(0x210EAB3A, "sceDisplay", "sceDisplayGetAccumulatedHcount",
                     hle_GetAccumulatedHcount);
    psp_hle_register(0xDBA6C4C4, "sceDisplay", "sceDisplayGetFramePerSec",
                     hle_GetFramePerSec);
}
