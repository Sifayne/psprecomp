/* psprecomp — sceGe_user.
 *
 * The GE is the PSP's GPU. It is not driven by function calls: user code builds
 * a **display list** — an array of 32-bit words, each an 8-bit command and 24
 * bits of argument — and hands the GE a pointer plus a *stall address*. The GE
 * consumes commands up to the stall, and the CPU moves the stall forward as it
 * writes more. That producer/consumer arrangement is the whole API.
 *
 * So `sceGu*` (the list-building library) is ordinary user code and gets
 * recompiled like anything else. Only list *execution* is emulated, and that is
 * this file.
 *
 * ## What this does and does not do
 *
 * It walks the list, follows control flow (JUMP/CALL/RET/END/FINISH), and
 * tracks the state commands that matter — framebuffer, vertex format,
 * primitive counts. It does **not rasterize**. No triangles are drawn.
 *
 * That is deliberately the useful half to build first. During bring-up the
 * question is not "does it look right" but "is the game drawing anything at
 * all, and what?" — and a command-stream summary answers that, while a
 * half-working rasterizer answers it misleadingly. psp_ge_dump_stats() reports
 * what the game asked for; making those triangles appear is a separate phase
 * with its own correctness problem.
 */

#include <limits.h>
#include <math.h>
#ifndef _WIN32
#include <pthread.h>          /* the GE thread census; see ge_note_thread */
#else
#include <windows.h>
#endif
#include "psprecomp/hle.h"
#include "psprecomp/clock.h"
#include "psprecomp/interrupt.h"
#include "psprecomp/dispatch.h"
#include "psprecomp/render.h"
#include "psprecomp/sched.h"
#include "census.h"
#include "psprecomp/state.h"

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* ---- PSPRECOMP_GE_PROFILE=1 -- where the transform pipeline spends its time
 *
 * Six cycle-counter marks per vertex and one per batch, so the share of the
 * per-vertex work that a GPU-side transform could take can be read off a run
 * rather than guessed: position decode, the three matrix products, colour +
 * fog + lighting, texture coordinates + texgen, the screen-space step, and
 * the batch's clipping and backend draw. Reported by psp_ge_dump_stats with
 * the TSC calibrated against CLOCK_MONOTONIC over the run. Off, the marks
 * are one predictable branch each. The marks themselves cost about 40 ns a
 * vertex when on, so read the shares, not the absolute total. */
#include <time.h>
#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#define GE_PROF_TSC() __rdtsc()
#else
#define GE_PROF_TSC() ((uint64_t)0)
#endif
static int      g_prof_on = -1;
static uint64_t g_model_verts;      /* vertices handed to the backend's own transform */
static uint64_t g_prof[6], g_prof_verts, g_prof_batches, g_prof_tsc0, g_prof_ns0;
/* The interpreter on a GPU-transform backend: decode, append, state pushes, the rest of the walk. */
static uint64_t g_prof_m[8], g_prof_lists, g_prof_op[256], g_prof_opn[256];
static inline uint64_t ge_prof_now(void) { return g_prof_on > 0 ? GE_PROF_TSC() : 0; }
static uint64_t ge_prof_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static void ge_prof_init(void) {
    const char *e = getenv("PSPRECOMP_GE_PROFILE");
    g_prof_on = (e && *e && *e != '0') ? 1 : 0;
    if (g_prof_on) { g_prof_tsc0 = GE_PROF_TSC(); g_prof_ns0 = ge_prof_ns(); }
}

/* A screen coordinate onto the rasterizer's 1/16-pixel grid, floored. Written
 * out rather than calling floorf: this file has no <math.h>, and a cast rounds
 * toward zero, which puts -0.0625 on the wrong side of pixel 0. */
static int fx16_floor(float f) {
    const float s = f * (float)PSP_SUBPX;
    const int i = (int)s;
    return i - (s < (float)i);
}

/* One axis of a transformed vertex onto the 1/16 grid: the viewport centre
 * (less the screen offset) plus ndc * scale, taken to sixteenths toward zero,
 * that is toward the centre -- left of it and above it a position rounds up,
 * right of it and below it down. ndc is the clip coordinate over w, as
 * clip_to_fx16 forms it. geprobe 2 (fw 6.60) scene 20's Gouraud triangles
 * pin every corner to one sixteenth through their colours; scenes 15 and 21
 * pin 18 more. A short reciprocal with rounding (the earlier fit to edges
 * alone) moved a third of them by one sixteenth and 2432 pixels of scene 20
 * by one step of colour. */
static int screen_axis_fx16(float ndc, float scale, float centre) {
    float t = ndc * scale * (float)PSP_SUBPX;
    if (!(t > -1073741824.0f)) t = -1073741824.0f;   /* NaN too */
    if (t > 1073741824.0f) t = 1073741824.0f;
    return fx16_floor(centre + 0.5f / (float)PSP_SUBPX) + (int)t;
}

/* A float through-mode coordinate, saturated to signed 12.4 (-2048 ..
 * 2047.9375 pixels). geprobe step 18 (fw 6.60) draws a triangle with a
 * vertex at x = 5000 exactly as if it were at x = 2048: every pixel's
 * coverage and colour match. The lower bound is assumed by symmetry, not
 * measured. Clamping the float first also keeps an out-of-range value from
 * reaching the int conversion; NaN goes to the lower bound. */
static int fx16_sat(float f) {
    if (f >= 2048.0f) return 2048 * PSP_SUBPX - 1;
    if (!(f > -2048.0f)) return -2048 * PSP_SUBPX;
    return fx16_floor(f);
}

/* Display-list opcodes. Only the ones the walk needs to be correct about are
 * named; everything else is counted rather than guessed at, because a
 * misidentified state command silently changes rendering. */
#define GE_NOP          0x00
#define GE_VADDR        0x01
#define GE_IADDR        0x02
#define GE_PRIM         0x04
#define GE_BEZIER       0x05
#define GE_SPLINE       0x06
#define GE_BBOX         0x07
#define GE_JUMP         0x08
#define GE_BJUMP        0x09
#define GE_CALL         0x0A
#define GE_RET          0x0B
#define GE_END          0x0C
#define GE_SIGNAL       0x0E
#define GE_FINISH       0x0F
#define GE_BASE         0x10
#define GE_VTYPE        0x12
#define GE_OFFSET_ADDR  0x13
#define GE_ORIGIN_ADDR  0x14
/* Texture state. The command numbers here and above are PSPSDK's, from
 * src/gu/guInternal.h -- BSD, and the SDK that emits them, so it is the
 * definition rather than a reading of one. Names differ (TEX_ADDR0,
 * TEX_BUF_WIDTH0, CLUT_BUF_PTR); the values do not. Every GE_* command number
 * in this file is also a command of the same number in uofw's
 * include/ge_user.h (MIT). */
#define GE_TEXADDR0     0xA0
#define GE_TEXBUFWIDTH0 0xA8
#define GE_CLUTADDR     0xB0
#define GE_CLUTADDRUPPER 0xB1
#define GE_TEXSIZE0     0xB8
#define GE_TEXMODE      0xC2
#define GE_TEXLEVEL     0xC8
#define GE_TEXLODSLOPE  0xD0
#define GE_TEXFORMAT    0xC3
#define GE_LOADCLUT     0xC4
#define GE_CLUTFORMAT   0xC5
#define GE_TEXFILTER    0xC6
#define GE_TEXWRAP      0xC7
#define GE_TGENMATRIXNUMBER 0x40
#define GE_TGENMATRIXDATA   0x41
#define GE_LIGHTINGENABLE   0x17
#define GE_FOGENABLE        0x1F
#define GE_FOG1             0xCD
#define GE_FOG2             0xCE
#define GE_FOGCOLOR         0xCF
/* Immediate-mode vertices: one register per component and 0xF7 to commit. */
#define GE_IMM_VSCX         0xF0
#define GE_IMM_VSCY         0xF1
#define GE_IMM_VSCZ         0xF2
#define GE_IMM_VTCS         0xF3
#define GE_IMM_VTCT         0xF4
#define GE_IMM_VTCQ         0xF5
#define GE_IMM_CV           0xF6
#define GE_IMM_AP           0xF7
#define GE_IMM_FC           0xF8
#define GE_IMM_SCV          0xF9
#define GE_LIGHTENABLE0     0x18
#define GE_MATERIALUPDATE   0x53
#define GE_MATERIALEMISSIVE 0x54
#define GE_AMBIENTCOLOR     0x55
#define GE_MATERIALDIFFUSE  0x56
#define GE_MATERIALSPECULAR 0x57
#define GE_AMBIENTALPHA     0x58
#define GE_MATERIALSPECCOEF 0x5B
#define GE_AMBIENTLIGHT     0x5C
#define GE_AMBIENTLIGHTALPHA 0x5D
#define GE_LIGHTMODE        0x5E
#define GE_LIGHTTYPE0       0x5F
#define GE_LIGHT0X          0x63
#define GE_LIGHT0DIRX       0x6F
#define GE_LIGHT0ATTEN0     0x7B
#define GE_LIGHT0EXPONENT   0x87
#define GE_LIGHT0CUTOFF     0x8B
#define GE_LIGHT0AMBIENT    0x8F
#define GE_DEPTHCLIPENABLE 0x1C
#define GE_FRAMEBUFPIXFORMAT 0xD2
#define GE_SCISSOR1     0xD4
#define GE_SCISSOR2     0xD5
#define GE_TEXMAPMODE   0xC0
#define GE_TEXSCALEU    0x48
#define GE_TEXSCALEV    0x49
#define GE_TEXOFFSETU   0x4A
#define GE_TEXOFFSETV   0x4B
#define GE_TEXFUNC      0xC9
#define GE_TEXENVCOLOR  0xCA

/* Block transfer -- how a game gets image data into VRAM. */
/* Whether texturing applies at all. Distinct from whether a texture is bound:
 * the GE keeps its texture state across draws, so geometry drawn with
 * texturing off must not be painted with whatever was last set up. */
#define GE_TEXTUREMAPENABLE 0x1E

#define GE_TRANSFERSRC     0xB2
#define GE_TRANSFERSRCW    0xB3
#define GE_TRANSFERDST     0xB4
#define GE_TRANSFERDSTW    0xB5
#define GE_TRANSFERSTART   0xEA
#define GE_TRANSFERSRCPOS  0xEB
#define GE_TRANSFERDSTPOS  0xEC
#define GE_TRANSFERSIZE    0xEE

#define GE_FBP          0x9C
#define GE_FBW          0x9D
#define GE_ZBP          0x9E
#define GE_ZBW          0x9F

/* Transform and lighting state.
 *
 * Matrix uploads are two commands: a NUMBER that sets the write index, then a
 * run of DATA words each carrying one element. World, view and texgen are 4
 * columns of 3 rows -- the bottom row is implied (0,0,0,1) and never sent --
 * so twelve elements; projection is a full 4x4, so sixteen. That asymmetry is
 * the SDK's, not a guess: sceGuSetMatrix sends 12 for GU_MODEL/GU_VIEW and 16
 * for GU_PROJECTION.
 *
 * DATA carries 24 bits where a float needs 32. The hardware takes the low
 * eight off the mantissa, so the word reassembles as arg << 8. */
#define GE_WORLDMATRIXNUMBER 0x3A
#define GE_WORLDMATRIXDATA   0x3B
#define GE_VIEWMATRIXNUMBER  0x3C
#define GE_VIEWMATRIXDATA    0x3D
#define GE_PROJMATRIXNUMBER  0x3E
#define GE_PROJMATRIXDATA    0x3F
#define GE_VIEWPORTXSCALE    0x42
#define GE_VIEWPORTYSCALE    0x43
#define GE_VIEWPORTZSCALE    0x44
#define GE_VIEWPORTXCENTER   0x45
#define GE_VIEWPORTYCENTER   0x46
#define GE_VIEWPORTZCENTER   0x47
/* Skinning, morphing and patches: the numbers libpspgu's sceGuBoneMatrix,
 * sceGuMorphWeight, sceGuPatchDivide, sceGuPatchPrim and sceGuPatchFrontFace
 * write (PSPSDK, BSD). A bone is uploaded like the world matrix, twelve DATA
 * words after a NUMBER of bone * 12. */
#define GE_BONEMATRIXNUMBER  0x2A
#define GE_BONEMATRIXDATA    0x2B
#define GE_MORPHWEIGHT0      0x2C
#define GE_PATCHDIVISION     0x36
#define GE_PATCHPRIMITIVE    0x37
#define GE_PATCHFACING       0x38
#define GE_OFFSETX           0x4C
#define GE_OFFSETY           0x4D
#define GE_CULLFACEENABLE    0x1D
#define GE_CULL              0x9B
/* SHADE: sceGuShadeModel sends GU_SMOOTH (1) or GU_FLAT (0), PSPSDK pspgu.h. */
#define GE_SHADE             0x50
/* Dither: DTE and the four matrix rows DITH1..4 (uofw include/ge_user.h). */
#define GE_DITHERENABLE      0x20
#define GE_DITHER0           0xE2
/* Colour test, logic op and pixel mask (uofw include/ge_user.h). 0xD8/0xD9
 * were named as RGB/alpha masks here; they are the colour-test function and
 * reference, and geprobe step 19 (fw 6.60) shows sceGuPixelMask sending
 * 0xE8/0xE9 instead. */
#define GE_COLORTESTENABLE   0x27
#define GE_LOGICOPENABLE     0x28
#define GE_COLORTEST         0xD8
#define GE_COLORREF          0xD9
#define GE_COLORTESTMASK     0xDA
#define GE_LOGICOP           0xE6
#define GE_PIXELMASKRGB      0xE8
#define GE_PIXELMASKALPHA    0xE9
#define GE_ZTESTENABLE       0x23
#define GE_ZTEST             0xDE
#define GE_ZWRITEDISABLE     0xE7
#define GE_CLEARMODE         0xD3
#define GE_ALPHABLENDENABLE  0x21
#define GE_ALPHATESTENABLE   0x22
#define GE_STENCILTESTENABLE 0x24
#define GE_STENCILTEST       0xDC
#define GE_STENCILOP         0xDD
#define GE_BLENDMODE         0xDF
#define GE_BLENDFIXEDA       0xE0
#define GE_BLENDFIXEDB       0xE1
#define GE_ALPHATEST         0xDB

/* VTYPE field extraction. */
#define VT_TEX(v)     ((v) & 3)
#define VT_COLOR(v)   (((v) >> 2) & 7)
#define VT_NORMAL(v)  (((v) >> 5) & 3)
#define VT_POS(v)     (((v) >> 7) & 3)
#define VT_WEIGHT(v)  (((v) >> 9) & 3)
#define VT_INDEX(v)   (((v) >> 11) & 3)
#define VT_WCOUNT(v)  ((((v) >> 14) & 7) + 1)   /* weights per vertex, when VT_WEIGHT */
#define VT_MORPH(v)   ((((v) >> 18) & 7) + 1)   /* vertex sets blended by morphing */
#define VT_THROUGH(v) (((v) >> 23) & 1)

#define MAX_QUEUES 8
#define GE_STACK   8

/* SIGNAL behaviour emitted by sceGuSignal(GU_SIGNAL_PAUSE); PSPSDK's pspgu.h
 * defines GU_SIGNAL_PAUSE as 3. */
#define GE_SIGNAL_HANDLER_PAUSE 0x03
/* PSPSDK pspge.h's PSP_GE_SIGNAL_SYNC: a SIGNAL 08 / END / FINISH / END
 * sequence is a sync point inside the list, not its end. The 3rd
 * Birthday's list writer (003FC590) emits it and goes on writing the HUD
 * after it. Not measured on a PSP. */
#define GE_SIGNAL_SYNC 0x08

/* Primitive types, from the PRIM argument's type field. */
static const char *const PRIM_NAME[8] = {
    "points", "lines", "line-strip", "triangles",
    "triangle-strip", "triangle-fan", "sprites", "?"
};

typedef struct {
    uint32_t id;
    uint32_t list;      /* current read pointer */
    uint32_t stall;     /* stop before this address; 0 means "no stall" */
    uint32_t base;      /* GE_BASE: high bits for addresses */
    uint32_t origin;
    int      signal;    /* pending PAUSE or SYNC through its FINISH/END pair */
    int      used;
    /* What the guest sees, which follows the GE's time (see "Drawing now,
     * reporting on the clock"). */
    int      done;
    int      paused;    /* stopped at a PAUSE until sceGeContinue */
    int      psig;      /* between a PAUSE's SIGNAL and its FINISH */
    /* Where the walk itself is, which may be ahead. */
    int      xdone;     /* walked to its end */
    int      xpaused;   /* the walk stopped at a PAUSE's FINISH */
    uint64_t t_x;       /* the GE's time where the walk last stopped */
    int      cbid;      /* sceGeSetCallback id given at EnQueue; -1 none */
    int      cont_early;/* sceGeContinue arrived before the pause took hold */
    int      hung;      /* stopped for good at a patch it cannot draw, until sceGeBreak */
    int      replay;    /* a capture's list (psp_ge_replay_list): no guest to Continue it */
    /* CALL's return addresses live with the list, not the walk: a walk that
     * stops at the stall or at the end of its time slice inside a CALL has
     * to find them when it resumes. */
    uint32_t stack[GE_STACK];
    int      sp;
} ge_queue;

static ge_queue g_queue[MAX_QUEUES];
static int g_ge_hang;   /* draw_patch met a division that hangs the GE (run_list_body) */

/* When the GE runs.
 *
 * The GE works alongside the CPU, and geprobe 5 and 6 (fw 6.60) show how far
 * behind it: a list of a few commands has run, callbacks and all, before
 * sceGeListEnQueue returns (geprobe 5 steps 54-56), but 200 full-screen
 * sprites have not -- ListSync(peek) reads 2, DRAWING, as EnQueue returns,
 * and only the first SIGNAL's handler has run (step 58). Words released by
 * sceGeListUpdateStallAddr run inside that call when they can: a SIGNAL and
 * a FINISH released from a stall have both called their handlers by the time
 * it returns, and ListSync(peek) then reads 0 (geprobe 6 steps 79 and 80).
 * libgu's finish handler still runs after sceGuFinish, in sceGuSync (geprobe
 * 5 step 50), because a FINISH waits for the drawing before it to end, and
 * the clear and sprites before it are still being drawn.
 *
 * Two parts, as geprobe 6 step 81 shows them. It times 100 sprites between
 * SIGNAL 1 and SIGNAL 2, then FINISH, at three sizes, from handlers that read
 * the system clock (microseconds after the call before EnQueue):
 *
 *     480x272   SIGNAL 1 at 51, SIGNAL 2 at 35609, FINISH at 62409
 *      64x64    SIGNAL 1 at 46, SIGNAL 2 at  1130, FINISH at  1952
 *      16x16    SIGNAL 1 at 41, SIGNAL 2 at   126, FINISH at   138
 *
 * The drawing (the back end) takes 623.6 and 19.06 microseconds a sprite at
 * the two larger sizes: 209.2 pixels a microsecond, with nothing to spare for
 * a cost per primitive (GE_PIXEL_UNITS). The command processor (the front
 * end) runs ahead of it and calls a SIGNAL's handler when it reaches it: at
 * both larger sizes SIGNAL 2 comes 43.0 sprites' drawing before FINISH, so up
 * to 43 drawing commands wait between the two (GE_FIFO_PRIMS). At 16x16 the
 * front end is the slower part: 85 microseconds for 100 VADDR and PRIM pairs
 * between the SIGNALs, 0.85 a sprite. How that splits between a command word,
 * a primitive's set-up and a vertex is not measured; GE_COMMAND_UNITS,
 * GE_PRIM_UNITS and GE_VERTEX_UNITS divide it as 0.1 a word, 0.6 a PRIM and
 * 0.025 a vertex, so a long strip is not charged as if each vertex were a
 * sprite. A FINISH waits for the back end to finish; then it calls its
 * handler and the list is done. The 16x16 sprites draw faster than 209.2 a
 * microsecond (0.97 a sprite, not 1.22), which this leaves out: small
 * primitives are charged up to a quarter too much.
 *
 * The clock is the guest's (clock.h): virtual microseconds, a tick per
 * firmware call or clock read, a frame per vblank, and a jump to the next
 * deadline when every thread waits. sceKernelGetSystemTimeLow reads it, and
 * so do the stamps of step 81. The GE keeps its own time on that clock,
 * g_ge_t for the front end and g_ge_back for the back end, in
 * GE_UNITS_PER_US units a microsecond. The guest sees the GE get only as far
 * as the moment it may reach: the present at a firmware call (psp_ge_tick),
 * GE_KICK_WINDOW_US past it inside EnQueue, UpdateStallAddr and Continue,
 * the next deadline when every thread waits (psp_ge_idle_run), and no limit
 * in a Sync that waits or at a frame boundary. The kick window is the 11-17
 * microseconds by which step 81's EnQueue returns after its first SIGNAL's
 * handler; the GE starts on a list GE_ENQUEUE_US into the EnQueue call, which
 * returns at the window's end.
 *
 * When a handler runs, the guest clock is moved up to the GE's time for it,
 * where the caller is waiting on the GE anyway: in a Sync that waits, while
 * every thread waits, and inside the kick window. A Sync that waits so takes
 * as long as the GE does: step 81's DrawSync returns at 62474 on the PSP.
 * Only the drain at a frame boundary (psp_ge_drain_all), which is no wait,
 * still lets the GE's work cost no time. */
#define GE_UNITS_PER_US   (1ull << 20)
#define GE_COMMAND_UNITS  (GE_UNITS_PER_US / 10)
#define GE_PRIM_UNITS     (GE_UNITS_PER_US * 6 / 10)
#define GE_VERTEX_UNITS   (GE_UNITS_PER_US / 40)
#define GE_PIXEL_UNITS    5012ull      /* 2^20 / 209.2 */
#define GE_FIFO_PRIMS     43
#define GE_KICK_WINDOW_US 11ull
#define GE_ENQUEUE_US     40ull
static uint64_t g_ge_t;        /* the front end's time, in units */
static uint64_t g_ge_back;     /* when the back end has drawn all it was given */
static uint64_t g_ge_fifo[GE_FIFO_PRIMS];   /* when each of the last drawing commands is drawn */
static unsigned g_ge_fifo_i;

/* Drawing now, reporting on the clock.
 *
 * The clock above moves only at firmware calls: guest code between them
 * costs no time. A GE that drew no faster than that clock fell behind the
 * game, where on a PSP the CPU's own work gives the GE time to keep up. The
 * game went on to rewrite vertices and matrices the GE had not read yet, and
 * parts of Last Raven's scenes flickered from frame to frame. So the walk
 * (ge_execute) runs a list as soon as its words are released, as far as its
 * stall, its end or a PAUSE, and works out the GE's times as it goes. What
 * the guest can see of it -- a handler running, a list done or paused -- is
 * not applied there. It goes into g_ev, stamped with the GE's time, and the
 * timeline (ge_advance) applies it when the guest clock gets there, by the
 * rules above.
 *
 * What the guest cannot see first is the order. A handler runs after the
 * words behind its SIGNAL or FINISH have been drawn, and a list queued behind
 * another can be drawn before the first one's finish handler runs. A handler
 * that rewrites words the GE has already passed is too late here. On a PSP
 * it may be in time.
 *
 * A held handler. Inside EnQueue, UpdateStallAddr and Continue, and in a
 * Sync that waits, handlers run as the timeline reaches them. When it
 * catches up at a firmware call, a SIGNAL or FINISH it reaches is held at
 * the head of g_ev instead (g_ev_held), and the timeline waits on it. The
 * handler runs at the next firmware call made with interrupts enabled, which
 * is when the interrupt would have been taken. geprobe 5 step 50 (fw 6.60)
 * is the measurement: libgu's sceGuFinish releases a sprite and the FINISH,
 * then suspends and resumes interrupts, and the finish handler runs after it
 * has returned. One reached while every thread waits is held until the
 * clock gets to its time. */
enum { GE_EV_HANDLER, GE_EV_DONE, GE_EV_PAUSE, GE_EV_PSIG };
typedef struct {
    int       kind, finish, cbid;
    ge_queue *q;
    uint32_t  qid;             /* q's id, in case its slot has been reused */
    uint32_t  code, pc;        /* a handler's command and the address after its END */
    uint64_t  start, at;       /* the GE's time at the command's start and end */
} ge_event;
#define GE_EVENTS 256
static ge_event g_ev[GE_EVENTS];
static unsigned g_ev_head, g_ev_n;
static int      g_ev_held;     /* the head is a handler held for a firmware call */
static int      g_ge_xfull;    /* the walk stopped for want of room in g_ev */

static uint64_t ge_now(void) { return psp_clock_peek() * GE_UNITS_PER_US; }

/* sceGeSetCallback's registrations: PSPSDK pspge.h's PspGeCallbackData,
 * copied at registration, indexed by the id it returns. */
#define GE_MAX_CALLBACKS 16
static struct {
    int      used;
    uint32_t signal_func, signal_arg, finish_func, finish_arg;
} g_ge_cb[GE_MAX_CALLBACKS];

/* Display-list capture, defined at the end of the file with the rest of it;
 * declared here because both the list runner and the enqueue path call into
 * it and they come first. */
static void cap_note_list(const ge_queue *q);
static void view_log_note(void);          /* PSPRECOMP_VIEW_LOG; see below */
static void view_log_note_world(void);
static void cap_snapshot_memory(void);
static uint32_t g_next_id;

/* Vertices the draw path declined, split by why.
 *
 * These were one counter, reported as "transformed, or no position". Three
 * causes with three unrelated fixes -- a missing transform pipeline, a vertex
 * pointer the stream never set, and a layout this does not decode -- summed
 * into a number that could not tell you which you were looking at. The first
 * needs T&L, the second is a state-tracking bug, the third is a decoder gap.
 * Guessing between them is the same mistake as reading one stop reason for
 * another, so they are counted apart. */
static uint64_t g_skip_noaddr;     /* no vertex address in the stream */
static uint64_t g_skip_layout;     /* no position -- vertex_layout declined */
static uint64_t g_skip_nearplane;  /* lines and points behind the eye (triangles go to the clipper) */
static uint64_t g_clip_eye, g_clip_z, g_clip_guard, g_clip_split;
static uint64_t g_draw_mip;
static uint64_t g_lit_verts;  /* transformed with LIGHTING_ENABLE set */
static uint64_t g_fog_verts;  /* transformed with FOG_ENABLE set */
static uint64_t g_imm_draws;  /* primitives assembled from immediate-mode vertices */  /* textured draws with a mip chain (TEX_MODE top level > 0) */  /* the clipper's decisions, in vertices */
static uint64_t g_culled;          /* backfacing, by the game's own winding rule */
static uint64_t g_xformed;         /* vertices that went through the pipeline */
/* Draws by path and by whether a texture was bound. "Most pixels are flat" has
 * two very different readings depending on which path they came from. */
static uint64_t g_draw_2d_tex, g_draw_2d_flat, g_draw_3d_tex, g_draw_3d_flat;
/* The distinct vertex colours the transform path reads. "Everything is white"
 * needs to distinguish a white model from a colour that is not being read. */
static uint32_t g_col_seen[8]; static int g_col_n;
static uint32_t g_col_last; static int g_col_last_valid;   /* note_colour's short cut */
/* What the eye-space lights were last computed from. The view matrix and the
 * lights change a few times a frame; the draws that read them come by the
 * hundred. Padding is cleared before the compare, so bytes may be compared. */
typedef struct { float view[12]; struct { int enable, type; float pos[3], dir[3]; } light[4]; int valid; } light_inputs;
static light_inputs g_light_eye_from;
static uint64_t g_clear_draws, g_clear_z_draws;
static void note_colour(uint32_t c) {
    if (g_col_last_valid && c == g_col_last) return;   /* a model's vertices mostly share one */
    for (int i = 0; i < g_col_n; i++) if (g_col_seen[i] == c) { g_col_last = c; g_col_last_valid = 1; return; }
    if (g_col_n < 8) g_col_seen[g_col_n++] = c;
    g_col_last = c; g_col_last_valid = 1;
}

/* The transform pipeline's state.
 *
 * Kept apart from g_ge because it resets differently: matrices persist across
 * lists, and a NUMBER command sets a cursor that the following DATA words walk
 * forward. Out-of-range writes are dropped rather than wrapped -- a list still
 * being built by the CPU can be read mid-write, and wrapping would corrupt the
 * matrix rather than skip a word of it. */
static struct {
    float world[12], view[12], proj[16];
    /* Eight bone matrices, 4 columns of 3 like the world matrix, and the
     * eight morph weights. */
    float bone[8 * 12];
    int   bone_n;
    float morph_w[8];
    /* Texture coordinate generation. TEX_MAP_MODE's low two bits choose where
     * texture coordinates come from -- 0 the vertex's own, 1 the generation
     * matrix, 2 the environment map -- and bits 8..9 choose what that matrix is
     * applied to: the model position, the vertex UV, or the normal. The matrix
     * is uploaded like the others, 12 elements, 4 columns of 3.
     *
     * Unimplemented, mode 1 read the vertex's texture-coordinate field anyway,
     * and a game using generation leaves that field uninitialised: Armored
     * Core's menu background draws a full-screen quad this way and the field
     * held -512 and NaN, which is where the GE summary's "u -inf..inf" and the
     * hatched sheet over the settings panel came from. Numbers from PSPSDK's
     * guInternal.h (BSD): TEX_MAP_MODE 0xC0, TGEN_MATRIX_NUMBER 0x40,
     * TGEN_MATRIX_DATA 0x41. */
    float tgen[12];
    int   tgen_n;
    int   tex_map_mode, tex_proj_mode;
    /* DEPTH_CLIP_ENABLE (0x1C). On the PSP this flag means *clamp*, not
     * clip: with it set, geometry beyond the near or far plane is drawn with
     * its depth clamped; with it clear, it is clipped away. gpu/clipping
     * measures both -- guardband's "Flat out negative Z" is DRAW=0 clipped
     * and DRAW=1 unclipped, and homogeneous's "Z outside near" is 171 lit
     * pixels with clamp and 0 without. */
    int   depth_clamp;
    /* Lighting. LIGHTING_ENABLE (0x17); the material ambient colour and alpha
     * (0x55, 0x58). Not a lighting model yet: an experiment to find out
     * whether the settings screen's black hangar is lit geometry drawn with
     * its raw vertex alpha. */
    /* Lighting. Register numbers from PSPSDK's guInternal.h: LIGHTING_ENABLE
     * 0x17, LIGHT_ENABLE0..3 0x18..0x1B, MATERIAL_COLOR 0x53, the four
     * material colours 0x54..0x57, AMBIENT_ALPHA 0x58, the specular
     * coefficient 0x5B, the global ambient 0x5C/0x5D, LIGHT_MODE 0x5E, the
     * per-light type 0x5F..0x62, positions 0x63..0x6E, spot directions
     * 0x6F..0x7A, attenuation 0x7B..0x86, spot exponent 0x87..0x8A, spot
     * cutoff 0x8B..0x8E, and each light's ambient/diffuse/specular
     * 0x8F..0x9A. */
    struct {
        int   enable;
        int   type;              /* 0 directional, 1 point, 2 spot */
        int   kind;              /* 0 diffuse, 1 diffuse+specular, 2 powered */
        float pos[3], dir[3], atten[3], exponent, cutoff;
        float amb[3], dif[3], spec[3];
    } light[4];
    int   lighting, light_mode, mat_update, mat_alpha;
    /* Fog: FOG_ENABLE 0x1F, FOG1 0xCD (end), FOG2 0xCE (1/range), FOG_COLOR
     * 0xCF. Decoded for the census before it is applied: gpu/commands/fog is
     * 264 of 272 values off, and every 3D scene the game has reached is dark
     * against the reference, so the first question is whether fog is on. */
    int   fog_enable;
    float fog_end, fog_range;
    float fog_colour[3];
    uint32_t fog_colour_raw;   /* the register as written, 0xBBGGRR, for the backend */
    float mat_emissive[3], mat_ambient[3], mat_diffuse[3], mat_specular[3];
    float mat_spec_coef;
    float global_amb[3];
    int   world_n, view_n, proj_n;
    float vp_xs, vp_ys, vp_zs, vp_xc, vp_yc, vp_zc;
    float off_x, off_y;
    int   vp_set;
    int   cull_enable, cull_ccw;
    int   ztest_enable, ztest_func, zwrite_off, clear_mode, clear_colour, clear_z, clear_stencil;
    psp_blend_state blend;
    /* Which matrices the stream actually uploaded, and where the result lands.
     * "Geometry is being transformed" and "transformed by the matrices the game
     * meant" are different claims, and a screen-space bounding box separates
     * them: a plausible scene sits inside the frame, an identity-by-omission
     * pipeline piles everything at one point, and a wrong matrix throws it to
     * coordinates with no relation to a 480x272 screen. */
    uint32_t world_words, view_words, proj_words;
    float bb_x0, bb_y0, bb_x1, bb_y1;
    int   bb_seen;
} g_tl;

/* Each bone matrix entry cut to 16 bits, as skinning takes it (bones_cut).
 * Valid until a bone is written: GE_BONEMATRIXDATA, a reset, a state load. */
static double g_bone16[8 * 12];
static int    g_bone16_ok;

/* A GE float argument: 24 bits of mantissa-truncated float, in the low bits. */
static float ge_float(uint32_t arg) {
    union { uint32_t u; float f; } c;
    c.u = arg << 8;
    return c.f;
}

/* Tracked state, and the counters that make the report worth reading. */
/* A census of render targets, for one question a GPU backend has to answer
 * before it is designed: does this game ever draw anywhere other than the two
 * buffers it displays? If it does not, a GL backend can own its own colour
 * buffer and read back lazily; if it renders to texture, targets have to be
 * tracked and reconciled, which is the expensive machinery. The end-of-run
 * summary only ever reported the *last* FBP, which cannot answer it. */
enum { GE_MAX_TARGETS = 16 };
typedef struct {
    uint32_t addr, stride, fmt;
    uint64_t sets, prims;
} ge_target;

static struct {
    uint32_t fbp, fbw, fbfmt, vtype, vaddr, iaddr;
    uint32_t zbp, zbw;
    /* PATCHDIVISION, PATCHPRIMITIVE (0 triangles, 1 lines, 2 points) and
     * PATCHFACING. */
    int      patch_du, patch_dv, patch_prim, patch_face;
    ge_target targets[GE_MAX_TARGETS];
    int       n_targets, cur_target;
    uint64_t  target_overflow;
    int      sc_x0, sc_y0, sc_x1, sc_y1, sc_set;
    /* The last BBOX's verdict: set when every corner of the box fell beyond
     * one edge of the scissor, which is when BJUMP jumps. */
    int      bbox_hidden;
    /* Texture state, recorded so the sampler can be built against what this
     * game uses rather than against the whole hardware surface. */
    uint32_t tex_addr, tex_stride, tex_w, tex_h, tex_enable;
    uint32_t tex_lv_addr[8], tex_lv_stride[8], tex_lv_w[8], tex_lv_h[8];
    int      tex_max_level, tex_lod_mode, tex_lod_bias16;
    float    tex_lod_slope;
    uint32_t tex_format, tex_func, tex_tcc, tex_double, tex_env, tex_filter, tex_wrap, tex_swizzled;
    /* GE_TEXSCALE / GE_TEXOFFSET: applied to transformed geometry's texture
     * coordinates before they are scaled by the texture size. Through-mode
     * coordinates are texels already and are not touched. */
    float    tex_scale_u, tex_scale_v, tex_offset_u, tex_offset_v;
    uint32_t clut_addr, clut_format, clut_raw;
    uint32_t tex_formats_seen, tex_funcs_seen;
    uint32_t xfer_src, xfer_srcw, xfer_dst, xfer_dstw;
    uint32_t xfer_srcpos, xfer_dstpos, xfer_size, xfer_start;
    uint64_t xfers, xfer_bytes, xfer_starts, xfer_rejected;
    /* Measured over geometry that actually draws, as opposed to the state
     * last *set* -- which is a different thing, and confusing the two has
     * already sent this investigation down one blind alley. */
    uint32_t drawn_vtype;
    uint64_t drawn_prims;
    float    u_lo, u_hi, v_lo, v_hi;
    int      uv_seen;
    uint64_t clut_loads;
    uint64_t commands;
    uint64_t prims[8];
    uint64_t vertices;
    uint64_t unknown;
    uint64_t lists;
    uint64_t finishes;
} g_ge;

/* The texel range the draws actually sample.
 *
 * u_lo/u_hi were declared and printed from the first version of this file and
 * never once assigned, so the summary reported "u 0.0..0.0" for every run ever
 * made -- a reading that looks like a measurement and is a fixed constant. It
 * is what tells "the texture is sampled at one corner" apart from "the texture
 * is sampled across its whole surface", which is the question that comes up
 * every time the frame is one flat colour. */
static void note_uv(float u, float v) {
    /* A draw with texturing off never reads its coordinates, so they do not
     * belong in the summary's range. The game's fade overlay is such a draw,
     * and its field held NaN; letting it in reported "u -inf..inf" for a run
     * whose textured draws were all sane -- an instrument pointing at the
     * wrong thing. */
    if (!g_ge.tex_enable) return;
    if (!g_ge.uv_seen) {
        g_ge.u_lo = g_ge.u_hi = u;
        g_ge.v_lo = g_ge.v_hi = v;
        g_ge.uv_seen = 1;
        return;
    }
    if (u < g_ge.u_lo) g_ge.u_lo = u;
    if (u > g_ge.u_hi) g_ge.u_hi = u;
    if (v < g_ge.v_lo) g_ge.v_lo = v;
    if (v > g_ge.v_hi) g_ge.v_hi = v;
}


/* Which host threads execute display lists. A GL context belongs to exactly
 * one thread, and the GE runs on whichever guest thread submitted the list --
 * so before a GL backend can be designed this has to be a number, not an
 * assumption. Census only; nothing depends on it yet. */
enum { GE_MAX_THREADS = 8 };
static struct { unsigned long id; uint64_t lists; } g_ge_threads[GE_MAX_THREADS];
static int g_ge_nthreads;
static uint64_t g_ge_thread_overflow;

/* The guest thread that last ran a list: the one whose host thread owns the
 * GL context, and so the one a save is taken on (census.h). */
static uint32_t g_ge_owner;
uint32_t psp_ge_owner(void) { return g_ge_owner; }

static void ge_note_thread(void) {
    g_ge_owner = psp_sched_current();
#ifndef _WIN32
    const unsigned long id = (unsigned long)pthread_self();
#else
    const unsigned long id = (unsigned long)GetCurrentThreadId();
#endif
    for (int i = 0; i < g_ge_nthreads; i++)
        if (g_ge_threads[i].id == id) { g_ge_threads[i].lists++; return; }
    if (g_ge_nthreads >= GE_MAX_THREADS) { g_ge_thread_overflow++; return; }
    g_ge_threads[g_ge_nthreads].id = id;
    g_ge_threads[g_ge_nthreads].lists = 1;
    g_ge_nthreads++;
}

void psp_ge_reset(void) {
    memset(g_queue, 0, sizeof g_queue);
    g_ge_t = g_ge_back = 0;
    memset(g_ge_fifo, 0, sizeof g_ge_fifo);
    g_ge_fifo_i = 0;
    g_ev_head = g_ev_n = 0;
    g_ev_held = g_ge_xfull = 0;
    memset(g_ge_cb, 0, sizeof g_ge_cb);
    memset(&g_ge, 0, sizeof g_ge);
    g_ge.fbfmt = 3;
    g_ge.tex_scale_u = g_ge.tex_scale_v = 1.0f;
    psp_render_reset_pixels();
    psp_render_reset_depth();
    g_skip_noaddr = g_skip_layout = g_skip_nearplane = 0;
    g_clip_eye = g_clip_z = g_clip_guard = g_clip_split = 0;
    g_draw_mip = 0;
    g_lit_verts = 0;
    g_fog_verts = 0;
    g_imm_draws = 0;
    g_culled = g_xformed = 0;
    g_draw_2d_tex = g_draw_2d_flat = g_draw_3d_tex = g_draw_3d_flat = 0;
    g_col_n = 0; g_col_last_valid = 0;
    g_clear_draws = g_clear_z_draws = 0;
    memset(&g_tl, 0, sizeof g_tl);
    g_bone16_ok = 0;
    memset(&g_light_eye_from, 0, sizeof g_light_eye_from);
    g_next_id = 0x00080000u;
}

void psp_ge_init(void) { psp_ge_reset(); ge_prof_init(); }

void psp_ge_dump_stats(FILE *out) {
    fprintf(out, "GE: %llu lists, %llu commands, %llu finishes\n",
            (unsigned long long)g_ge.lists,
            (unsigned long long)g_ge.commands,
            (unsigned long long)g_ge.finishes);
    if (g_model_verts)
        fprintf(out, "    transformed on the backend: %llu vertices\n", (unsigned long long)g_model_verts);
    if (g_prof_on > 0 && g_prof_verts) {
        const double cyc_per_ns = (double)(GE_PROF_TSC() - g_prof_tsc0) / (double)(ge_prof_ns() - g_prof_ns0 + 1);
        static const char *const NAMES[6] = { "position decode", "matrix products", "colour+fog+lighting", "uv+texgen", "screen+bbox", "clip+draw (per batch)" };
        double tot = 0; for (int i = 0; i < 6; i++) tot += (double)g_prof[i];
        fprintf(out, "GE profile: %llu vertices in %llu batches, %.1f ms in the transform pipeline (%.0f ns/vertex)\n",
                (unsigned long long)g_prof_verts, (unsigned long long)g_prof_batches, tot / cyc_per_ns / 1e6, tot / cyc_per_ns / (double)g_prof_verts);
        for (int i = 0; i < 6; i++)
            fprintf(out, "    %-24s %7.1f ms  %4.1f%%\n", NAMES[i], (double)g_prof[i] / cyc_per_ns / 1e6, 100.0 * (double)g_prof[i] / (tot > 0 ? tot : 1));
    }
    if (g_prof_on > 0 && g_prof_lists) {
        const double cyc_per_ns = (double)(GE_PROF_TSC() - g_prof_tsc0) / (double)(ge_prof_ns() - g_prof_ns0 + 1);
        const double total = (double)g_prof_m[3] / cyc_per_ns / 1e6, dec = (double)g_prof_m[0] / cyc_per_ns / 1e6,
                     app = (double)g_prof_m[1] / cyc_per_ns / 1e6, st = (double)g_prof_m[2] / cyc_per_ns / 1e6;
        fprintf(out, "GE interpreter: %.1f ms in %llu list run(s): model decode %.1f ms (%.0f%%), backend append %.1f ms (%.0f%%), pixel-state pushes %.1f ms (%.0f%%), the rest of the walk %.1f ms (%.0f%%)\n",
                total, (unsigned long long)g_prof_lists, dec, 100*dec/total, app, 100*app/total, st, 100*st/total, total-dec-app-st, 100*(total-dec-app-st)/total);
        {
            uint64_t opsum = 0; for (int i = 0; i < 256; i++) opsum += g_prof_op[i];
            fprintf(out, "    inside PRIM as well: texture push %.1f ms, transform-state fill %.1f ms, draw prelude %.1f ms; outside every command (fetch, stall test, dispatch): %.1f ms\n",
                    (double)g_prof_m[4] / cyc_per_ns / 1e6, (double)g_prof_m[5] / cyc_per_ns / 1e6, (double)g_prof_m[6] / cyc_per_ns / 1e6,
                    total - (double)opsum / cyc_per_ns / 1e6);
        }
        int order[256]; for (int i = 0; i < 256; i++) order[i] = i;
        for (int a = 0; a < 256; a++) for (int b = a + 1; b < 256; b++) if (g_prof_op[order[b]] > g_prof_op[order[a]]) { int t = order[a]; order[a] = order[b]; order[b] = t; }
        fprintf(out, "    by command, top 12:");
        for (int i = 0; i < 12 && g_prof_opn[order[i]]; i++)
            fprintf(out, " %02X:%.0fms/%lluk(%.0fns)", order[i], (double)g_prof_op[order[i]] / cyc_per_ns / 1e6,
                    (unsigned long long)(g_prof_opn[order[i]] / 1000), (double)g_prof_op[order[i]] / cyc_per_ns / (double)g_prof_opn[order[i]]);
        fprintf(out, "\n");
    }
    if (g_ge.tex_addr || g_ge.tex_formats_seen) {
        static const char *const TF[16] = {
            "5650","5551","4444","8888","clut4","clut8","clut16","clut32",
            "dxt1","dxt3","dxt5","?","?","?","?","?" };
        static const char *const FN[8] = {
            "modulate","decal","blend","replace","add","?","?","?" };
        fprintf(out, "    texture    0x%08X %ux%u stride %u, %s, %s%s\n",
                g_ge.tex_addr, g_ge.tex_w, g_ge.tex_h, g_ge.tex_stride,
                TF[g_ge.tex_format & 15], FN[g_ge.tex_func & 7],
                g_ge.tex_swizzled ? ", swizzled" : "");
        fprintf(out, "    formats used:");
        for (int i = 0; i < 16; i++)
            if (g_ge.tex_formats_seen & (1u << i)) fprintf(out, " %s", TF[i]);
        for (int i = 0; i < 8; i++)
            if (g_ge.tex_funcs_seen & (1u << i)) fprintf(out, " %s", FN[i]);
        if (g_ge.clut_loads)
            fprintf(out, "  (%llu clut loads, fmt %u)",
                    (unsigned long long)g_ge.clut_loads, g_ge.clut_format);
        fprintf(out, "\n");
        /* Sampling state, which the report has never carried. It decides
         * whether a glyph edge is a hard texel boundary or a ramp, so "the
         * letters are ragged" cannot be reasoned about without it. Both are
         * tracked and neither reaches the backend yet; printing them says which
         * of the two is worth wiring up for this game rather than for the
         * hardware in general. */
        {
            static const char *const FI[8] = {
                "nearest","linear","?","?",
                "near/mip-near","lin/mip-near","near/mip-lin","lin/mip-lin" };
            fprintf(out, "    sampling   filter min %s mag %s, wrap s %s t %s",
                    FI[g_ge.tex_filter & 7], FI[(g_ge.tex_filter >> 8) & 7],
                    (g_ge.tex_wrap & 1) ? "clamp" : "repeat",
                    ((g_ge.tex_wrap >> 8) & 1) ? "clamp" : "repeat");
            /* The backend samples with the magnification filter, having no
             * scale factor to choose with. This is how many primitives that
             * choice was actually visible on. */
            if (psp_render_filter_split())
                fprintf(out, "  (%llu prims where min/mag differ)",
                        (unsigned long long)psp_render_filter_split());
            fprintf(out, "\n");
        }
    }
    if (g_ge.drawn_prims)
        fprintf(out, "    drawn      %llu prims, vertex type 0x%06X"
                     " (tex %u colour %u pos %u through %u), u %.1f..%.1f v %.1f..%.1f, texturing %s\n",
                (unsigned long long)g_ge.drawn_prims, g_ge.drawn_vtype,
                VT_TEX(g_ge.drawn_vtype), VT_COLOR(g_ge.drawn_vtype),
                VT_POS(g_ge.drawn_vtype), VT_THROUGH(g_ge.drawn_vtype),
                (double)g_ge.u_lo, (double)g_ge.u_hi,
                (double)g_ge.v_lo, (double)g_ge.v_hi,
                g_ge.tex_enable ? "on" : "off");
    if (g_ge.xfer_starts)
        fprintf(out, "    transfers  %llu asked, %llu done (%llu bytes), %llu rejected\n",
                (unsigned long long)g_ge.xfer_starts, (unsigned long long)g_ge.xfers,
                (unsigned long long)g_ge.xfer_bytes,
                (unsigned long long)g_ge.xfer_rejected);
    fprintf(out, "    framebuffer 0x%08X stride %u, vertex type 0x%06X\n",
            g_ge.fbp, g_ge.fbw, g_ge.vtype);
    /* Distinct render targets. Two of these (the display pair) means a GPU
     * backend can own its colour buffer; more means render-to-texture, and
     * targets have to be tracked. Primitives are attributed to the target
     * current when they were drawn, which is the number that matters -- a
     * target set once and never drawn into is noise. */
    fprintf(out, "    GE driven by %d host thread(s)%s\n", g_ge_nthreads,
            g_ge_thread_overflow ? " (more than the census holds)" : "");
    for (int i = 0; i < g_ge_nthreads; i++)
        fprintf(out, "      thread %lu: %llu list(s)\n", g_ge_threads[i].id,
                (unsigned long long)g_ge_threads[i].lists);
    int drawn_into = 0;
    for (int i = 0; i < g_ge.n_targets; i++)
        if (g_ge.targets[i].prims) drawn_into++;
    /* Two counts, because the first one alone misleads: FBP, FBW and the pixel
     * format arrive as three separate commands, so every combination seen
     * while they are half-updated registers as its own target and draws
     * nothing. The number that matters is how many were actually drawn into. */
    fprintf(out, "    render targets: %d seen, %d drawn into%s\n",
            g_ge.n_targets, drawn_into,
            g_ge.target_overflow ? " (more than the census holds)" : "");
    for (int i = 0; i < g_ge.n_targets; i++)
        fprintf(out, "      0x%08X stride %-4u fmt %u  %llu set(s), %llu primitive(s)%s\n",
                g_ge.targets[i].addr, g_ge.targets[i].stride, g_ge.targets[i].fmt,
                (unsigned long long)g_ge.targets[i].sets,
                (unsigned long long)g_ge.targets[i].prims,
                (g_ge.targets[i].addr & 0xFF000000u) == PSP_VRAM_BASE ? "" : "  <- not VRAM");
    fprintf(out, "    vertices submitted: %llu\n", (unsigned long long)g_ge.vertices);
    for (int i = 0; i < 8; i++)
        if (g_ge.prims[i])
            fprintf(out, "    %-15s %llu\n", PRIM_NAME[i], (unsigned long long)g_ge.prims[i]);
    if (g_ge.unknown)
        fprintf(out, "    %llu commands not individually decoded\n",
                (unsigned long long)g_ge.unknown);
    fprintf(out, "    pixels written: %llu (%llu textured, %llu flat)\n",
            (unsigned long long)psp_render_pixels(),
            (unsigned long long)psp_render_textured_pixels(),
            (unsigned long long)psp_render_flat_pixels());
    {   /* What the rasterizer cost, and the only comparison that matters:
         * a PSP frame is 16.7ms, so a per-frame figure near that says the
         * software path cannot carry the scene in real time. */
        const uint64_t ns = psp_render_raster_ns();
        const uint64_t px = psp_render_pixels();
        fprintf(out, "    raster time: %.3f s (%.1f ns/pixel)\n",
                (double)ns / 1e9, px ? (double)ns / (double)px : 0.0);
    }
    fprintf(out, "    depth: test %s func %d, write %s, %llu pixels rejected\n",
            g_tl.ztest_enable ? "on" : "off", g_tl.ztest_func,
            g_tl.zwrite_off ? "off" : "on",
            (unsigned long long)psp_render_zfail_pixels());
    fprintf(out, "    blend %s (src %d dst %d eq %d), alpha test %s func %d ref %d: "
                 "%llu blended, %llu alpha-killed\n",
            g_tl.blend.enable ? "on" : "off", g_tl.blend.src, g_tl.blend.dst,
            g_tl.blend.eq, g_tl.blend.alpha_test ? "on" : "off",
            g_tl.blend.alpha_func, g_tl.blend.alpha_ref,
            (unsigned long long)psp_render_blended_pixels(),
            (unsigned long long)psp_render_alphakill_pixels());
    fprintf(out, "    clear-mode draws: %llu (%llu clearing depth)\n",
            (unsigned long long)g_clear_draws, (unsigned long long)g_clear_z_draws);
    if (g_imm_draws)
        fprintf(out, "    immediate-mode draws: %llu (untextured)\n", (unsigned long long)g_imm_draws);
    if (g_xformed) {
        fprintf(out, "    transformed %llu vertices; viewport %s",
                (unsigned long long)g_xformed,
                g_tl.vp_set ? "set" : "defaulted");
        if (g_tl.vp_set)
            fprintf(out, " (scale %.1f,%.1f centre %.1f,%.1f offset %.1f,%.1f)",
                    g_tl.vp_xs, g_tl.vp_ys, g_tl.vp_xc, g_tl.vp_yc,
                    g_tl.off_x, g_tl.off_y);
        fprintf(out, ", cull %s\n", g_tl.cull_enable ? (g_tl.cull_ccw ? "ccw" : "cw") : "off");
        fprintf(out, "    draws: 2d %llu textured / %llu flat, 3d %llu textured / %llu flat\n",
                (unsigned long long)g_draw_2d_tex, (unsigned long long)g_draw_2d_flat,
                (unsigned long long)g_draw_3d_tex, (unsigned long long)g_draw_3d_flat);
        fprintf(out, "    vertex colours seen (first %d):", g_col_n);
        for (int i = 0; i < g_col_n; i++) fprintf(out, " %08X", g_col_seen[i]);
        fprintf(out, "\n");
        fprintf(out, "    matrix words: world %u, view %u, proj %u\n",
                g_tl.world_words, g_tl.view_words, g_tl.proj_words);
        if (g_tl.bb_seen)
            fprintf(out, "    screen bounds: x %.1f..%.1f  y %.1f..%.1f\n",
                    g_tl.bb_x0, g_tl.bb_x1, g_tl.bb_y0, g_tl.bb_y1);
        if (g_fog_verts)
            fprintf(out, "    fogged     %llu vertices transformed with fog on; colour %02X%02X%02X end %.1f range %.4f\n",
                    (unsigned long long)g_fog_verts,
                    (unsigned)(g_tl.fog_colour[0] * 255.0f + 0.5f), (unsigned)(g_tl.fog_colour[1] * 255.0f + 0.5f),
                    (unsigned)(g_tl.fog_colour[2] * 255.0f + 0.5f), (double)g_tl.fog_end, (double)g_tl.fog_range);
        if (g_lit_verts)
        {
            int nlights = 0;
            for (int i = 0; i < 4; i++) if (g_tl.light[i].enable) nlights++;
            fprintf(out, "    lit        %llu vertices, %d light(s) on, %s specular, material update %d\n",
                    (unsigned long long)g_lit_verts, nlights,
                    g_tl.light_mode ? "separate" : "single", g_tl.mat_update);
        }
        if (g_draw_mip)
            fprintf(out, "    mipmapped  %llu textured draws carried a mip chain\n", (unsigned long long)g_draw_mip);
        if (g_clip_eye || g_clip_z || g_clip_guard || g_clip_split)
            fprintf(out, "    clipped    %llu verts all behind the eye, %llu beyond near/far, "
                         "%llu outside the guard band, %llu added by splits; depth %s\n",
                    (unsigned long long)g_clip_eye, (unsigned long long)g_clip_z,
                    (unsigned long long)g_clip_guard, (unsigned long long)g_clip_split,
                    g_tl.depth_clamp ? "clamp" : "clip");
        if (g_culled)
            fprintf(out, "    %llu vertices culled as backfacing\n",
                    (unsigned long long)g_culled);
    }
    if (g_skip_noaddr)
        fprintf(out, "    %llu vertices dropped: no vertex address set\n",
                (unsigned long long)g_skip_noaddr);
    if (g_skip_layout)
        fprintf(out, "    %llu vertices dropped: layout has no position\n",
                (unsigned long long)g_skip_layout);
}

uint64_t psp_ge_command_count(void) { return g_ge.commands; }
uint64_t psp_ge_vertex_count(void)  { return g_ge.vertices; }

/* ---- rasterizer ----------------------------------------------------------
 *
 * Enough of the GE to put pixels in the framebuffer. Deliberately narrow:
 *
 *  - "through" mode only (VTYPE bit 23), where vertex coordinates are already
 *    in screen space. That is what 2D games and every UI layer use. Transformed
 *    geometry needs the matrix pipeline and is not attempted here -- drawing it
 *    with the wrong transform would look like a rendering bug rather than a
 *    missing feature.
 *  - Position as 16-bit or float; colour as 8888 or none.
 *  - Flat/interpolated colour, no texturing, no depth, no blending.
 *
 * The point is to close the loop from display list to visible pixels so the
 * rest can be measured against something. Everything omitted is omitted
 * loudly: unsupported vertex formats are counted, not guessed at.
 */



/* Where the GE actually writes.
 *
 * FBP is an offset into VRAM, not an address: the base is implicit in the
 * hardware and never appears in the display list. This game sets 0x00088000
 * and expects 0x04088000 -- which it confirms itself, by flushing the cache
 * over exactly that address before it presents.
 *
 * Taking the offset literally does not merely draw in the wrong place. The
 * module image is mapped at zero, so 0x00088000 lands inside the game's own
 * code, and every rasterized pixel overwrites an instruction. */
/* ---- block transfer --------------------------------------------------------
 *
 * The GE's 2D copy, and the only way a texture reaches VRAM: a game decodes or
 * loads an image into main memory and blits it across. Without this the
 * texture sampler reads whatever VRAM happened to contain, which is zeros --
 * a correctly sampled empty texture.
 *
 * The commands are PSPSDK's TRANSFER_* group (guInternal.h): SRC 0xb2, SRC_W
 * 0xb3, DST 0xb4, DST_W 0xb5, START 0xea, SRC_OFFSET 0xeb, DST_OFFSET 0xec,
 * SIZE 0xee. How the fields sit inside them is what PSPSDK's sceGuCopyImage
 * writes, and the parts worth naming are the ways to be quietly wrong. The
 * masks were checked on a PSP (firmware 6.60) with tools/hwprobe/mpegprobe:
 *   - address bits 0-23 come from SRC/DST, and bits 24-31 from the *stride*
 *     register's high byte -- the same split as FBP/FBW. PSPSDK's pspgu.h
 *     says the data must be 16-byte aligned, and the GE ignores the low four
 *     bits: a source or destination 2, 4, 8 or 12 bytes in copied from or
 *     to the aligned address below it, in both pixel sizes.
 *   - width and height are stored as n-1; offsets are y << 10 | x.
 *   - the stride is bits 3-10 of its register (0x7F8). On the PSP 0x44 acted
 *     as 0x40, 0x404 as 0x400 and 0x808 as 0x008, while 0x408 and 0x7F8 were
 *     used as given, for source and destination alike. Last Raven's only
 *     transfer builder (0x2B678C) is called with strides 0x200 and 0x80.
 *   - TRANSFERSTART bit 0 selects 32-bit pixels (PSM 8888); everything else
 *     is 16-bit.
 */
static uint32_t xfer_addr(uint32_t base, uint32_t widthreg) {
    return (base & 0xFFFFF0u) | ((widthreg & 0xFF0000u) << 8);
}

static uint32_t xfer_stride(uint32_t widthreg) {
    return widthreg & 0x7F8u;
}

static void do_block_transfer(void) {
    const uint32_t src  = xfer_addr(g_ge.xfer_src, g_ge.xfer_srcw);
    const uint32_t dst  = xfer_addr(g_ge.xfer_dst, g_ge.xfer_dstw);
    const uint32_t ss   = xfer_stride(g_ge.xfer_srcw);
    const uint32_t ds   = xfer_stride(g_ge.xfer_dstw);
    const uint32_t sx   = g_ge.xfer_srcpos & 0x3FFu;
    const uint32_t sy   = (g_ge.xfer_srcpos >> 10) & 0x3FFu;
    const uint32_t dx   = g_ge.xfer_dstpos & 0x3FFu;
    const uint32_t dy   = (g_ge.xfer_dstpos >> 10) & 0x3FFu;
    const uint32_t w    = (g_ge.xfer_size & 0x3FFu) + 1u;
    const uint32_t h    = ((g_ge.xfer_size >> 10) & 0x3FFu) + 1u;
    const uint32_t bpp  = (g_ge.xfer_start & 1u) ? 4u : 2u;

    g_ge.xfer_starts++;
    if (!src || !dst || !ss || !ds) {
        /* Counted separately: a transfer the game asked for but that names no
         * usable source, destination or stride is a different problem from one
         * that never arrived. */
        g_ge.xfer_rejected++;
        return;
    }

    /* Row at a time, because source and destination strides differ in general
     * and a single memcpy would only be right when they happen to match. */
    for (uint32_t row = 0; row < h; row++) {
        const uint32_t s = src + ((sy + row) * ss + sx) * bpp;
        const uint32_t d = dst + ((dy + row) * ds + dx) * bpp;
        for (uint32_t i = 0; i < w * bpp; i += 4)
            psp_write32(d + i, psp_read32(s + i));
    }

    g_ge.xfers++;
    g_ge.xfer_bytes += (uint64_t)w * h * bpp;
}

/* Record the target being switched to, and make it current. Linear scan: the
 * expectation this measures is that there are two or three of these, and if
 * that expectation is wrong the overflow counter says so rather than the
 * scan getting slow. */
static void ge_note_target(uint32_t addr, uint32_t stride, uint32_t fmt) {
    for (int i = 0; i < g_ge.n_targets; i++) {
        if (g_ge.targets[i].addr == addr && g_ge.targets[i].stride == stride &&
            g_ge.targets[i].fmt == fmt) {
            g_ge.targets[i].sets++;
            g_ge.cur_target = i;
            return;
        }
    }
    if (g_ge.n_targets >= GE_MAX_TARGETS) { g_ge.target_overflow++; return; }
    const int i = g_ge.n_targets++;
    g_ge.targets[i].addr = addr;
    g_ge.targets[i].stride = stride;
    g_ge.targets[i].fmt = fmt;
    g_ge.targets[i].sets = 1;
    g_ge.cur_target = i;
}

static uint32_t ge_fb_address(uint32_t fbp) {
    return PSP_VRAM_BASE | (fbp & 0x001FFFF0u);
}

uint64_t psp_ge_pixels(void) { return psp_render_pixels(); }

/* Where the GE last drew, as a real address. */
uint32_t psp_ge_target(void) { return g_ge.fbp ? ge_fb_address(g_ge.fbp) : 0; }

/* Size of one vertex in bytes, and the offsets within it. Components appear in
 * a fixed order (weights, texture, colour, normal, position) and each is
 * aligned to its own size, which is what makes the stride awkward enough to be
 * worth computing rather than assuming.
 *
 * With morphing (VT_MORPH > 1) the whole record repeats once per vertex set,
 * weights included, and the stride returned is the full vertex's: geprobe 2
 * scene 21 (fw 6.60) draws weights (1, 0) from records 0, 2 and 4 of a
 * two-set array and (0, 1) from 1, 3 and 5. g_vl keeps the rest. */
static struct {
    int w_fmt, w_n;          /* weight format (VT_WEIGHT) and count; w_fmt 0: none */
    int set_stride, morph_n; /* one vertex set's size, and how many sets */
} g_vl;

static int vertex_layout(uint32_t vtype, int *col_off, int *pos_off, int *tex_off, int *norm_off) {
    static const int tex_sz[4]   = { 0, 1, 2, 4 };
    static const int col_sz[8]   = { 0, 0, 0, 0, 2, 2, 2, 4 };
    static const int norm_sz[4]  = { 0, 1, 2, 4 };
    static const int pos_sz[4]   = { 0, 1, 2, 4 };
    static const int w_sz[4]     = { 0, 1, 2, 4 };

    int off = 0, align = 1;
    int t = tex_sz[VT_TEX(vtype)] * 2;
    int c = col_sz[VT_COLOR(vtype)];
    int n = norm_sz[VT_NORMAL(vtype)] * 3;
    int p = pos_sz[VT_POS(vtype)] * 3;

    g_vl.w_fmt = VT_WEIGHT(vtype);
    g_vl.w_n = g_vl.w_fmt ? VT_WCOUNT(vtype) : 0;
    g_vl.morph_n = VT_MORPH(vtype);
    if (g_vl.w_fmt) { off = w_sz[g_vl.w_fmt] * g_vl.w_n; align = w_sz[g_vl.w_fmt]; }

    int ts = tex_sz[VT_TEX(vtype)];
    if (ts) { off = (off + ts - 1) & ~(ts - 1); *tex_off = off; off += t;
              if (ts > align) align = ts; }
    else *tex_off = -1;
    int cs = col_sz[VT_COLOR(vtype)];
    if (cs) { off = (off + cs - 1) & ~(cs - 1); *col_off = off; off += c; if (cs > align) align = cs; }
    else *col_off = -1;
    int ns = norm_sz[VT_NORMAL(vtype)];
    if (ns) { off = (off + ns - 1) & ~(ns - 1); *norm_off = off; off += n;
              if (ns > align) align = ns; }
    else *norm_off = -1;
    int ps = pos_sz[VT_POS(vtype)];
    if (!ps) return 0;                        /* no position: nothing to draw */
    off = (off + ps - 1) & ~(ps - 1); *pos_off = off; off += p;
    if (ps > align) align = ps;

    g_vl.set_stride = (off + align - 1) & ~(align - 1);
    return g_vl.set_stride * g_vl.morph_n;    /* stride */
}

/* The address of element i of the current draw. Bits 11..12 of the vertex
 * type say whether PRIM's count is vertices or indices: 0 reads the vertex
 * array in order, 1 and 2 read an 8- or 16-bit index list at IADDR and fetch
 * each vertex by it.
 *
 * Until this was written, IADDR was decoded and dropped and every indexed
 * draw read the vertex array in order, as if the index list were 0,1,2,3...
 * For a quad drawn as 0,1,2,0,2,3 that is right for the first three indices
 * and wrong for everything after, and the error grows with each quad until
 * the read runs off the end of the vertex array into whatever follows. The
 * game's text is drawn this way -- 25 glyphs, 150 indices, 100 vertices --
 * and that is where the fragmentary glyphs came from: the first glyph whole,
 * the rest progressively misassembled, and the tail a single huge triangle
 * with coordinates read from unrelated memory, painted as diagonal stripes
 * across the lower half of the screen. */
static uint32_t vertex_addr(uint32_t i, int stride) {
    switch (VT_INDEX(g_ge.vtype)) {
    case 1:  return g_ge.vaddr + (uint32_t)psp_read8(g_ge.iaddr + i) * (uint32_t)stride;
    case 2:  return g_ge.vaddr + (uint32_t)psp_read16(g_ge.iaddr + 2u * i) * (uint32_t)stride;
    default: return g_ge.vaddr + i * (uint32_t)stride;
    }
}

/* The colour of a vertex that carries none.
 *
 * It is not white. It is the material ambient colour and alpha, registers
 * 0x55 and 0x58 -- what PSPSDK's sceGuColor writes (it is sceGuMaterial with
 * every component selected) -- and that is how a game animates a fade
 * without touching a vertex: one register write per frame, then a quad with
 * no colour field. Armored Core's missions open on exactly such a quad, a
 * full-screen white sprite of vertex type 0x800102, and with the old
 * 0xFFFFFFFF default its alpha read as opaque in all 1,103 frames it was
 * drawn. The scene underneath was rendered correctly the entire time. */
static uint32_t current_colour(void) {
    uint32_t c = (uint32_t)(g_tl.mat_alpha & 0xFF) << 24;
    for (int k = 0; k < 3; k++) {
        float f = g_tl.mat_ambient[k];
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        c |= (uint32_t)(int)(f * 255.0f + 0.5f) << (8 * k);
    }
    return c;
}

/* A vertex colour in any of its four formats, as 8888: each 16-bit format's
 * fields widened by repeating their top bits below them (5 bits to 8 as
 * v << 3 | v >> 2), 5650 opaque and 5551's alpha bit 0 or 255. geprobe 22
 * (fw 6.60) scene 139 draws 200 points of each 16-bit format and reads every
 * one of their 1800 channels so; this took 8888 alone and gave the rest the
 * material colour. The alpha is not read (a frame's alpha byte is the
 * stencil). */
static uint32_t widen_colour(uint32_t fmt, uint32_t c) {
    uint32_t r, g, b, a;
    switch (fmt) {
    case 4: r = c & 31; g = (c >> 5) & 63; b = (c >> 11) & 31;
            return 0xFF000000u | (b << 3 | b >> 2) << 16 | (g << 2 | g >> 4) << 8 | (r << 3 | r >> 2);
    case 5: r = c & 31; g = (c >> 5) & 31; b = (c >> 10) & 31; a = (c >> 15) & 1;
            return (a ? 0xFF000000u : 0u) | (b << 3 | b >> 2) << 16 | (g << 3 | g >> 2) << 8 | (r << 3 | r >> 2);
    case 6: r = c & 15; g = (c >> 4) & 15; b = (c >> 8) & 15; a = (c >> 12) & 15;
            return (a * 17) << 24 | (b * 17) << 16 | (g * 17) << 8 | r * 17;
    default: return c;
    }
}
static uint32_t read_colour_at(const uint8_t *vp, uint32_t addr, uint32_t vtype, int col_off) {
    const uint32_t fmt = VT_COLOR(vtype);
    uint32_t c;
    if (fmt == 7) {
        if (vp) memcpy(&c, vp + col_off, 4);
        else    c = psp_read32(addr + (uint32_t)col_off);
        return c;
    }
    if (vp) { uint16_t h; memcpy(&h, vp + col_off, 2); c = h; }
    else    c = psp_read16(addr + (uint32_t)col_off);
    return widen_colour(fmt, c);
}

static int read_vertex(uint32_t addr, uint32_t vtype, int col_off, int pos_off,
                       int tex_off, psp_vertex *out) {
    out->spec = 0; out->spec_set = 0;
    out->rgba = current_colour();
    out->u = out->v = 0.0f;
    out->inv_w = out->tex_q = 1.0f; /* through mode is affine in screen space */
    out->fog = 255;              /* through-mode geometry is never fogged */
    out->screen_space = 1;
    out->precise = 0;
    out->precise_x = out->precise_y = 0;

    /* Through-mode texture coordinates are in texels, whatever their width, so
     * every form is taken as it comes. Leaving the narrow ones at zero -- which
     * is what this did at first -- makes every sprite sample texel (0,0) and
     * paints the screen in whatever colour happens to be in that corner. */
    switch (tex_off >= 0 ? VT_TEX(vtype) : 0) {
    case 1:   /* 8-bit */
        out->u = (float)psp_read8(addr + (uint32_t)tex_off);
        out->v = (float)psp_read8(addr + (uint32_t)tex_off + 1);
        break;
    case 2:   /* 16-bit */
        out->u = (float)psp_read16(addr + (uint32_t)tex_off);
        out->v = (float)psp_read16(addr + (uint32_t)tex_off + 2);
        break;
    case 3:   /* float */
        out->u = psp_read_f32(addr + (uint32_t)tex_off);
        out->v = psp_read_f32(addr + (uint32_t)tex_off + 4);
        break;
    default:
        break;
    }
    if (tex_off >= 0) note_uv(out->u, out->v);
    if (col_off >= 0 && VT_COLOR(vtype) >= 4)
        out->rgba = read_colour_at(NULL, addr, vtype, col_off);

    switch (VT_POS(vtype)) {
    case 2:   /* 16-bit: whole pixels, onto the 1/16 grid */
        /* Saturated to 12.4 like a float's (fx16_sat): geprobe 5 (fw 6.60)
         * scene 28 draws its 16-bit triangles with vertices at x -5000 and
         * 5000 pixel for pixel as it draws the same ones given as floats. */
        out->x = fx16_sat((float)(int16_t)psp_read16(addr + (uint32_t)pos_off));
        out->y = fx16_sat((float)(int16_t)psp_read16(addr + (uint32_t)pos_off + 2));
        /* Through-mode depth is already a window value, and unsigned: the
         * screen z range is 0..65535, not -32768..32767. */
        out->z = (float)(uint16_t)psp_read16(addr + (uint32_t)pos_off + 4);
        return 1;
    case 3: { /* float: floored onto the 1/16 grid, fraction kept, saturated */
        out->x = fx16_sat(psp_read_f32(addr + (uint32_t)pos_off));
        out->y = fx16_sat(psp_read_f32(addr + (uint32_t)pos_off + 4));
        out->z = psp_read_f32(addr + (uint32_t)pos_off + 8);
        return 1;
    }
    default:
        return 0;
    }
}


/* Model-space position, for transformed geometry.
 *
 * Narrow components are normalised here and raw in through-mode: an s16 is a
 * signed fraction of 32768, an s8 of 128. Reading them raw instead puts a unit
 * cube 32768 units across, which projects to nothing recognisable and looks
 * like a broken matrix rather than a scaling mistake. */
/* The vertex normal, in model space. A vertex type with no normal still gets
 * lit -- the game draws such batches -- so it falls back to facing the eye. */
static void read_normal_model(uint32_t addr, uint32_t vtype, int norm_off, float n[3]) {
    n[0] = n[1] = 0.0f; n[2] = 1.0f;
    if (norm_off < 0) return;
    const uint32_t a = addr + (uint32_t)norm_off;
    switch (VT_NORMAL(vtype)) {
    case 1:
        n[0] = (float)(int8_t)psp_read8(a)       / 128.0f;
        n[1] = (float)(int8_t)psp_read8(a + 1)   / 128.0f;
        n[2] = (float)(int8_t)psp_read8(a + 2)   / 128.0f;
        break;
    case 2:
        n[0] = (float)(int16_t)psp_read16(a)     / 32768.0f;
        n[1] = (float)(int16_t)psp_read16(a + 2) / 32768.0f;
        n[2] = (float)(int16_t)psp_read16(a + 4) / 32768.0f;
        break;
    case 3:
        n[0] = psp_read_f32(a);
        n[1] = psp_read_f32(a + 4);
        n[2] = psp_read_f32(a + 8);
        break;
    default: break;
    }
}

static int read_pos_model(uint32_t addr, uint32_t vtype, int pos_off, float p[3]) {
    const uint32_t a = addr + (uint32_t)pos_off;
    switch (VT_POS(vtype)) {
    case 1:
        p[0] = (float)(int8_t)psp_read8(a)       / 128.0f;
        p[1] = (float)(int8_t)psp_read8(a + 1)   / 128.0f;
        p[2] = (float)(int8_t)psp_read8(a + 2)   / 128.0f;
        return 1;
    case 2:
        p[0] = (float)(int16_t)psp_read16(a)     / 32768.0f;
        p[1] = (float)(int16_t)psp_read16(a + 2) / 32768.0f;
        p[2] = (float)(int16_t)psp_read16(a + 4) / 32768.0f;
        return 1;
    case 3:
        p[0] = psp_read_f32(a);
        p[1] = psp_read_f32(a + 4);
        p[2] = psp_read_f32(a + 8);
        return 1;
    default:
        return 0;
    }
}

/* Texture coordinates for transformed geometry are normalised, not texels: the
 * texture scale and offset registers apply first, then the texture size.
 * Through-mode gives texels directly, which is why the two paths read them
 * differently.
 *
 * The narrow formats are unsigned. gpu/textures/size draws a 3D sprite with
 * 16-bit coordinates from 0 to 32768 and hardware reads texel 0 at the left
 * and the last texel at the right; read as a signed 16-bit value, 32768 is
 * -1.0 and the right edge came out texel 0. Neither TEXSCALE nor TEXOFFSET was
 * decoded before: gpu/filtering/precisionnearest3d scales by 0.5, and its texel
 * boundary landed at a quarter of the sprite instead of the middle. */
static int read_uv_raw(uint32_t addr, uint32_t vtype, int tex_off, float *u, float *v) {
    *u = *v = 0.0f;
    if (tex_off < 0) return 0;
    const uint32_t a = addr + (uint32_t)tex_off;
    switch (VT_TEX(vtype)) {
    case 1: *u = (float)psp_read8(a)       / 128.0f;
            *v = (float)psp_read8(a + 1)   / 128.0f;   return 1;
    case 2: *u = (float)psp_read16(a)      / 32768.0f;
            *v = (float)psp_read16(a + 2)  / 32768.0f; return 1;
    case 3: *u = psp_read_f32(a); *v = psp_read_f32(a + 4); return 1;
    default: return 0;
    }
}
static void uv_to_texels(float u, float v, psp_vertex *out);

/* The same three decoders over one host span for the whole record.
 *
 * draw_prim_transformed translates each vertex's address once with
 * psp_mem_ptr and reads the components from that pointer; the hooked
 * per-component reads above cost a translation and a range check each, eight
 * to eleven times a vertex, and were the largest single item in the
 * PSPRECOMP_GE_PROFILE breakdown outside lighting. The bytes and the
 * conversions are the same, so the values are the same. A record the span
 * cannot cover (vp == NULL) takes the hooked path, which is what reports the
 * bad access. */
static inline int16_t rd_s16(const uint8_t *p) { int16_t v; memcpy(&v, p, 2); return v; }
static inline float   rd_f32(const uint8_t *p) { float v; memcpy(&v, p, 4); return v; }
static int read_pos_model_at(const uint8_t *vp, uint32_t addr, uint32_t vtype, int pos_off, float p[3]) {
    if (!vp) return read_pos_model(addr, vtype, pos_off, p);
    const uint8_t *a = vp + pos_off;
    switch (VT_POS(vtype)) {
    case 1: p[0] = (float)(int8_t)a[0] / 128.0f; p[1] = (float)(int8_t)a[1] / 128.0f; p[2] = (float)(int8_t)a[2] / 128.0f; return 1;
    case 2: p[0] = (float)rd_s16(a) / 32768.0f; p[1] = (float)rd_s16(a + 2) / 32768.0f; p[2] = (float)rd_s16(a + 4) / 32768.0f; return 1;
    case 3: p[0] = rd_f32(a); p[1] = rd_f32(a + 4); p[2] = rd_f32(a + 8); return 1;
    default: return 0;
    }
}
static void read_normal_model_at(const uint8_t *vp, uint32_t addr, uint32_t vtype, int norm_off, float n[3]) {
    if (!vp) { read_normal_model(addr, vtype, norm_off, n); return; }
    n[0] = n[1] = 0.0f; n[2] = 1.0f;
    if (norm_off < 0) return;
    const uint8_t *a = vp + norm_off;
    switch (VT_NORMAL(vtype)) {
    case 1: n[0] = (float)(int8_t)a[0] / 128.0f; n[1] = (float)(int8_t)a[1] / 128.0f; n[2] = (float)(int8_t)a[2] / 128.0f; break;
    case 2: n[0] = (float)rd_s16(a) / 32768.0f; n[1] = (float)rd_s16(a + 2) / 32768.0f; n[2] = (float)rd_s16(a + 4) / 32768.0f; break;
    case 3: n[0] = rd_f32(a); n[1] = rd_f32(a + 4); n[2] = rd_f32(a + 8); break;
    default: break;
    }
}
static int read_uv_raw_at(const uint8_t *vp, uint32_t addr, uint32_t vtype, int tex_off, float *u, float *v) {
    if (!vp) return read_uv_raw(addr, vtype, tex_off, u, v);
    *u = *v = 0.0f;
    if (tex_off < 0) return 0;
    const uint8_t *a = vp + tex_off;
    switch (VT_TEX(vtype)) {
    case 1: *u = (float)a[0] / 128.0f;                  *v = (float)a[1] / 128.0f;                    return 1;
    case 2: *u = (float)(uint16_t)rd_s16(a) / 32768.0f; *v = (float)(uint16_t)rd_s16(a + 2) / 32768.0f; return 1;
    case 3: *u = rd_f32(a); *v = rd_f32(a + 4); return 1;
    default: return 0;
    }
}
static void read_uv_model_at(const uint8_t *vp, uint32_t addr, uint32_t vtype, int tex_off, psp_vertex *out) {
    float u, v;
    out->u = out->v = 0.0f;
    if (read_uv_raw_at(vp, addr, vtype, tex_off, &u, &v)) uv_to_texels(u, v, out);
}

/* World and view are 4 columns of 3 rows; the implied bottom row makes the
 * fourth column a translation. */
static void mul_4x3(const float m[12], const float in[3], float out[3]) {
    out[0] = m[0]*in[0] + m[3]*in[1] + m[6]*in[2] + m[9];
    out[1] = m[1]*in[0] + m[4]*in[1] + m[7]*in[2] + m[10];
    out[2] = m[2]*in[0] + m[5]*in[1] + m[8]*in[2] + m[11];
}

/* Projection is a full 4x4, column-major, and produces the w that the divide
 * needs -- which is the whole reason it is not folded into the 4x3 above. */
/* The rotation part alone, for normals. The translation lives in m[9..11]. */
/* A GE colour register is 0xBBGGRR; our pixels are 0xAABBGGRR with red low,
 * so the three bytes land in the same order. */
static void ge_colour3(uint32_t arg, float c[3]) {
    c[0] = (float)(arg & 0xFFu) / 255.0f;
    c[1] = (float)((arg >> 8) & 0xFFu) / 255.0f;
    c[2] = (float)((arg >> 16) & 0xFFu) / 255.0f;
}

static void mul_3x3(const float m[12], const float in[3], float out[3]) {
    out[0] = m[0]*in[0] + m[3]*in[1] + m[6]*in[2];
    out[1] = m[1]*in[0] + m[4]*in[1] + m[7]*in[2];
    out[2] = m[2]*in[0] + m[5]*in[1] + m[8]*in[2];
}

/* ---- Exact arithmetic without libm -----------------------------------------
 *
 * The vertex path below cuts, splits and scales every value it touches, and
 * frexp, ldexp, trunc and floor are library calls on x86-64 without SSE4.1:
 * they were most of a lit vertex's cost. These give the same doubles bit for
 * bit wherever they take the fast path (a normal finite input, a power of two
 * a double can hold) and fall back to the library call everywhere else. */
static inline uint64_t ge_bits(double v) { uint64_t b; memcpy(&b, &v, sizeof b); return b; }
static inline double ge_from_bits(uint64_t b) { double v; memcpy(&v, &b, sizeof v); return v; }

/* 2^n, exactly. */
static inline double ge_pow2(int n) {
    return n >= -1022 && n <= 1023 ? ge_from_bits((uint64_t)(n + 1023) << 52) : ldexp(1.0, n);
}

/* ldexp(x, n): x times an exact 2^n rounds once, as ldexp does. */
static inline double ge_ldexp(double x, int n) {
    return n >= -1022 && n <= 1023 ? x * ge_pow2(n) : ldexp(x, n);
}

/* frexp's exponent of a normal, non-zero x (x = m 2^e, m in [0.5, 1));
 * INT_MIN for anything else, whose callers keep frexp. */
static inline int ge_frexp_e(double x) {
    const int ex = (int)((ge_bits(x) >> 52) & 0x7FF);
    return ex && ex != 0x7FF ? ex - 1022 : INT_MIN;
}

/* trunc and floor of a finite x below 2^52, through an integer; the sign of a
 * zero result is the one the library gives. */
static inline double ge_trunc(double x) {
    if (!(fabs(x) < 4503599627370496.0)) return trunc(x);
    const double r = (double)(int64_t)x;
    return r == 0.0 ? copysign(0.0, x) : r;
}
static inline double ge_floor(double x) {
    if (!(fabs(x) < 4503599627370496.0) || x == 0.0) return floor(x);
    const int64_t i = (int64_t)x;
    return (double)(i - ((double)i > x));
}

/* v with its significand cut to `bits` bits, toward zero or (round) to
 * nearest. Toward zero on a normal v is clearing the low bits. */
static double ge_cut(double v, int bits, int round) {
    if (v == 0.0 || !isfinite(v)) return v;
    if (!round && bits >= 1 && bits <= 53 && ((ge_bits(v) >> 52) & 0x7FF))
        return ge_from_bits(ge_bits(v) & ~((UINT64_C(1) << (53 - bits)) - 1));
    int e;
    const double m = ldexp(frexp(v, &e), bits);
    return ldexp(round ? floor(m + 0.5) : trunc(m), e - bits);
}


/* ---- The GE's vertex arithmetic ----------------------------------------
 *
 * geprobe 12 (fw 6.60) reads each stage of a vertex's depth whole: with the
 * viewport z centre 0 and its scale a power of two, a point's depth is
 * clip z / w's top 16 bits (scenes 62-66). What it reads, and every point of
 * scenes 45 and 59-61 besides (41,543 points), fit this to the last bit:
 *  - A number has 16 significant bits. A vertex coordinate is cut to them
 *    toward zero on the way in (ge_split); a matrix word holds no more.
 *  - A product a b keeps its bits down to 2^(ea + eb - 15), toward zero, ea
 *    and eb the exponents of a and b: when the significands' product reaches
 *    2 it keeps one bit more than 16 (ge_mul).
 *  - A sum puts every term on the grid 2^(E - 15), each cut toward zero, E
 *    the largest term's exponent -- a product's ea + eb, whatever its
 *    significand -- adds them, and cuts the total to 16 bits (ge_sum). A
 *    matrix row's three products and its translation are one such sum.
 *  - The GE multiplies projection by view, that by world, in this same
 *    arithmetic, and transforms a vertex by the product (ge_wvp). Forming
 *    eye z first instead cancels scene 61's errors in clip z / w: it fits 939
 *    of scene 65's 3072 points where this fits all of them.
 *  - 1/w is a table (ge_rcp16); clip z / w is clip z times it, cut to 16 bits;
 *    the viewport is zc plus zs times that, a sum, floored (ge_screen_z).
 * Scene 63 sums two to four terms across alignments of 3 to 18 bits, with a
 * carry and a cancellation; scene 66 is the probes' own perspective, where
 * every point a product carries in was a step off until the sum lined it up
 * by ea + eb. */
typedef struct {
    int64_t q;       /* the value is q 2^(e - 15); 0 for zero */
    int     e;
    int     bad;     /* an input not finite: sums fall back to v */
    double  v;       /* the plain product */
} ge_term;

/* v cut to 16 significant bits toward zero, as a signed significand in
 * [2^15, 2^16) and an exponent; 0 for zero, -1 for a value not finite. */
static inline int ge_split(double v, int64_t *s, int *e) {
    uint64_t b;
    memcpy(&b, &v, sizeof b);
    const int ex = (int)((b >> 52) & 0x7FF);
    if (ex == 0x7FF) return -1;
    if (ex == 0) {
        if (v == 0.0) return 0;
        int fe;
        *s = (int64_t)(frexp(fabs(v), &fe) * 65536.0);
        *e = fe - 1;
    } else {
        *s = (int64_t)(0x8000u | ((b >> 37) & 0x7FFFu));
        *e = ex - 1023;
    }
    if (b >> 63) *s = -*s;
    return 1;
}

static inline ge_term ge_mul(double a, double b) {
    ge_term t = { 0, INT_MIN, 0, a * b };
    int64_t sa, sb;
    int ea, eb;
    const int ka = ge_split(a, &sa, &ea), kb = ge_split(b, &sb, &eb);
    if (ka < 0 || kb < 0) { t.bad = 1; return t; }
    if (!ka || !kb) return t;
    const int64_t p = sa * sb;                     /* under 2^32 */
    t.q = p < 0 ? -((-p) >> 15) : p >> 15;
    t.e = ea + eb;
    return t;
}

/* A number split once for ge_mul -- ge_split's significand, exponent and
 * verdict, and the number -- so that a matrix entry or a coordinate used in
 * many products is not split again for each. */
typedef struct { int64_t s; int e, bad; double v; } ge_sp;
/* A zero splits to a zero significand, whose products are zero terms, as
 * ge_mul makes them; a number not finite sets bad, as ge_split's -1 does. */
static inline ge_sp ge_sp_of(double v) {
    ge_sp r = { 0, 0, 0, v };
    const int k = ge_split(v, &r.s, &r.e);
    if (k <= 0) { r.s = 0; r.e = 0; r.bad = k < 0; }
    return r;
}

/* ge_mul(a->v, b->v), from the split halves. A term whose q is zero is no
 * term to ge_sum, whatever its e. The shift is toward zero, as ge_mul's. */
static inline ge_term ge_mul_sp(const ge_sp *a, const ge_sp *b) {
    const int64_t p = a->s * b->s;                 /* under 2^32 */
    ge_term t;
    t.q = (p + ((p >> 63) & 0x7FFF)) >> 15;
    t.e = a->e + b->e;
    t.bad = a->bad | b->bad;
    t.v = a->v * b->v;
    return t;
}

/* ge_sp_of for the value last asked of this memo, kept: the viewport and
 * texture constants every vertex of a draw multiplies by. Keyed on the bits,
 * so a changed constant is split afresh. */
typedef struct { uint64_t key; int ok; ge_sp sp; } ge_sp_memo;
static inline const ge_sp *ge_sp_memo_of(ge_sp_memo *m, double x) {
    if (!m->ok || m->key != ge_bits(x)) { m->sp = ge_sp_of(x); m->key = ge_bits(x); m->ok = 1; }
    return &m->sp;
}
static const ge_sp GE_SP_ONE = { 32768, 0, 0, 1.0 };      /* ge_sp_of(1.0) */

static inline double ge_sum(const ge_term *t, int n) {
    int e = INT_MIN, bad = 0;
    for (int i = 0; i < n; i++) {
        bad |= t[i].bad;
        if (t[i].q && t[i].e > e) e = t[i].e;
    }
    if (bad) {                                     /* the plain products' sum, in order */
        double plain = 0.0;
        for (int i = 0; i < n; i++) plain += t[i].v;
        return plain;
    }
    if (e == INT_MIN) return 0.0;
    int64_t s = 0;
    for (int i = 0; i < n; i++) {
        if (!t[i].q) continue;
        const int d = e - t[i].e;
        const int64_t m = t[i].q < 0 ? -t[i].q : t[i].q;
        const int64_t a = m >> (d < 63 ? d : 63);     /* m is under 2^34: 63 leaves 0 */
        s += t[i].q < 0 ? -a : a;
    }
    if (!s) return 0.0;
    int64_t m = s < 0 ? -s : s;
    const int len = 64 - __builtin_clzll((uint64_t)m);
    const int sh = len > 16 ? len - 16 : 0;        /* the total to 16 bits */
    m >>= sh;
    return ge_ldexp(s < 0 ? -(double)m : (double)m, e - 15 + sh);
}

/* A vertex's texture coordinates, in units, through TEXSCALE and TEXOFFSET to
 * texels: u s + o as one GE sum of two GE products (ge_mul, ge_sum), then the
 * texture's size. geprobe 22 (fw 6.60) scene 140 reads 600 plain float
 * coordinates at scales 1, 2^8 and 2^12 through a texture that names its own
 * texels, and this fits all 1800 readings; the float sum, as before, missed
 * 119 at 2^12. */
static void uv_to_texels(float u, float v, psp_vertex *out) {
    static ge_sp_memo su, ou, sv, ov;
    const ge_sp us = ge_sp_of(u), vs = ge_sp_of(v);
    const ge_term tu[2] = { ge_mul_sp(&us, ge_sp_memo_of(&su, g_ge.tex_scale_u)),
                            ge_mul_sp(ge_sp_memo_of(&ou, g_ge.tex_offset_u), &GE_SP_ONE) };
    const ge_term tv[2] = { ge_mul_sp(&vs, ge_sp_memo_of(&sv, g_ge.tex_scale_v)),
                            ge_mul_sp(ge_sp_memo_of(&ov, g_ge.tex_offset_v), &GE_SP_ONE) };
    out->u = (float)(ge_sum(tu, 2) * (double)g_ge.tex_w);
    out->v = (float)(ge_sum(tv, 2) * (double)g_ge.tex_h);
    note_uv(out->u, out->v);
}

static double ge_dot4(const double row[4], const double in[4]) {
    ge_term t[4];
    for (int k = 0; k < 4; k++) t[k] = ge_mul(row[k], in[k]);
    return ge_sum(t, 4);
}

static void ge_mat4_mul(const double a[4][4], const double b[4][4], double out[4][4]) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            const double col[4] = { b[0][j], b[1][j], b[2][j], b[3][j] };
            out[i][j] = ge_dot4(a[i], col);
        }
}

/* Projection times view times world, row by row, as the GE combines them
 * (projection and view first: scene 65 cannot tell that from view and world
 * first). World and view are 4 x 3, their last row 0 0 0 1. */
static void ge_wvp(double m[4][4]) {
    double p[4][4], v[4][4], w[4][4], pv[4][4];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            p[i][j] = g_tl.proj[4 * j + i];
            v[i][j] = i < 3 ? g_tl.view[3 * j + i]  : (j == 3);
            w[i][j] = i < 3 ? g_tl.world[3 * j + i] : (j == 3);
        }
    ge_mat4_mul(p, v, pv);
    ge_mat4_mul(pv, w, m);
}

/* A model position to clip space through ge_wvp's matrix. */
static void ge_clip(const double m[4][4], const float model[3], float clip[4]) {
    const double in[4] = { model[0], model[1], model[2], 1.0 };   /* ge_mul cuts each to 16 bits */
    for (int i = 0; i < 4; i++) clip[i] = (float)ge_dot4(m[i], in);
}

/* ge_clip through the matrix split once (ge_sp_of of each entry). */
static void ge_clip_sp(const ge_sp m[4][4], const float model[3], float clip[4]) {
    const ge_sp in[4] = { ge_sp_of(model[0]), ge_sp_of(model[1]), ge_sp_of(model[2]), ge_sp_of(1.0) };
    for (int i = 0; i < 4; i++) {
        const ge_term t[4] = { ge_mul_sp(&m[i][0], &in[0]), ge_mul_sp(&m[i][1], &in[1]),
                               ge_mul_sp(&m[i][2], &in[2]), ge_mul_sp(&m[i][3], &in[3]) };
        clip[i] = (float)ge_sum(t, 4);
    }
}

/* 1/w as the GE has it. w is cut to 16 bits; the 7 significand bits below
 * the leading one pick one of 128 straight segments, the 8 after step along
 * it: twice its start in units of 2^-16, less its drop times the step over
 * 256, that product rounded up, then floored, for 1/w's significand in
 * units of 2^-16. Each segment's ends are within a unit of 2^23/h and
 * 2^23/(h+1), h its index plus 128, so it lies a little above 1/w between
 * them (a chord, where geprobe 8's gradient reciprocal is a tangent). The
 * entries are geprobe 12's: scene 62 reads 1/w whole for 9151 w over nine
 * binades (2^-3 to 2^13), and these reproduce every one; the drops are all
 * even. Not a rounding of 2^23/h or of the chord at any width tried, and
 * not consecutive starts' differences either (they differ from the drop by
 * up to a unit), so they are kept as measured. Every run of 256 w differing
 * only in the bits a float has past 16 reads one value. */
static const uint32_t ge_rcp_tab[128][2] = {
{ 131073, 508 }, { 130056, 500 }, { 129055, 492 }, { 128072, 486 },
    { 127100, 478 }, { 126144, 470 }, { 125204, 464 }, { 124275, 456 },
    { 123362, 450 }, { 122462, 444 }, { 121575, 438 }, { 120701, 432 },
    { 119837, 424 }, { 118987, 418 }, { 118151, 414 }, { 117324, 408 },
    { 116509, 402 }, { 115705, 396 }, { 114914, 392 }, { 114131, 386 },
    { 113359, 380 }, { 112600, 376 }, { 111848, 370 }, { 111108, 366 },
    { 110376, 360 }, { 109655, 356 }, { 108944, 352 }, { 108240, 346 },
    { 107546, 342 }, { 106862, 338 }, { 106185, 334 }, { 105518, 330 },
    { 104859, 326 }, { 104207, 322 }, { 103564, 318 }, { 102928, 314 },
    { 102301, 310 }, { 101681, 306 }, { 101068, 302 }, { 100464, 300 },
    {  99866, 296 }, {  99274, 292 }, {  98690, 288 }, {  98114, 286 },
    {  97543, 282 }, {  96978, 278 }, {  96422, 276 }, {  95870, 272 },
    {  95327, 270 }, {  94787, 266 }, {  94256, 264 }, {  93728, 260 },
    {  93208, 258 }, {  92692, 254 }, {  92183, 252 }, {  91681, 250 },
    {  91181, 246 }, {  90689, 244 }, {  90202, 242 }, {  89718, 238 },
    {  89241, 236 }, {  88770, 234 }, {  88303, 232 }, {  87839, 228 },
    {  87382, 226 }, {  86929, 224 }, {  86482, 222 }, {  86038, 220 },
    {  85600, 218 }, {  85165, 216 }, {  84733, 212 }, {  84308, 210 },
    {  83886, 208 }, {  83469, 206 }, {  83056, 204 }, {  82647, 202 },
    {  82242, 200 }, {  81840, 198 }, {  81443, 196 }, {  81050, 194 },
    {  80660, 192 }, {  80276, 192 }, {  79893, 190 }, {  79514, 188 },
    {  79139, 186 }, {  78767, 184 }, {  78399, 182 }, {  78034, 180 },
    {  77674, 180 }, {  77316, 178 }, {  76961, 176 }, {  76609, 174 },
    {  76261, 172 }, {  75915, 170 }, {  75575, 170 }, {  75235, 168 },
    {  74899, 166 }, {  74565, 164 }, {  74237, 164 }, {  73909, 162 },
    {  73585, 160 }, {  73265, 160 }, {  72945, 158 }, {  72629, 156 },
    {  72317, 156 }, {  72006, 154 }, {  71698, 152 }, {  71394, 152 },
    {  71091, 150 }, {  70790, 148 }, {  70494, 148 }, {  70198, 146 },
    {  69907, 146 }, {  69616, 144 }, {  69328, 142 }, {  69043, 142 },
    {  68760, 140 }, {  68480, 140 }, {  68201, 138 }, {  67926, 138 },
    {  67651, 136 }, {  67379, 134 }, {  67110, 134 }, {  66842, 132 },
    {  66578, 132 }, {  66314, 130 }, {  66053, 130 }, {  65794, 128 },
};

static double ge_rcp16(double w) {
    if (w == 0.0 || !isfinite(w)) return 1.0 / w;
    int e = ge_frexp_e(w);
    uint32_t sig;
    if (e != INT_MIN) sig = (uint32_t)(ge_bits(w) >> 37) & 0x7FFFu;    /* 15 bits below the leading one */
    else {
        const double m = frexp(fabs(w), &e);                 /* [0.5, 1) */
        sig = (uint32_t)(m * 65536.0) & 0x7FFFu;
    }
    if (sig == 0) return copysign(ge_ldexp(1.0, 1 - e), w);  /* a power of two: exact */
    const uint32_t t = sig >> 8, l = sig & 0xFFu;
    const uint32_t q = (128u * ge_rcp_tab[t][0] - ge_rcp_tab[t][1] * l - 1u) >> 8;
    return copysign(ge_ldexp((double)q, -15 - e), w);        /* (q / 2^16) * 2^(1-e) */
}

/* clip c over w as the GE forms it: c times 1/w, cut to 16 bits. */
static double ge_over_w(double c, double w) {
    return ge_cut(c * ge_rcp16(w), 16, 0);
}

/* One vertex as the transform stage takes it, after skinning and morphing:
 * model-space position and normal, colour, and texture coordinates in texels
 * (uv_to_texels' units). Patches are tessellated into these too. */
typedef struct { float pos[3], nrm[3]; uint32_t rgba; float u, v; } ge_mvert;

/* A vertex's skinning weights. Normalised like the other narrow fields: an
 * 8-bit weight of 0x80 is 1.0 -- geprobe 2 scene 20 (fw 6.60) draws weights
 * 0x80, 0x40+0x40 and 0xFF where 1.0, 0.5+0.5 and 1.99 put them -- and a
 * 16-bit one of 0x8000 is assumed to be by the same rule. */
static void read_weights(uint32_t a, float *w) {
    for (int i = 0; i < g_vl.w_n; i++) {
        switch (g_vl.w_fmt) {
        case 1:  w[i] = (float)psp_read8(a + (uint32_t)i) / 128.0f; break;
        case 2:  w[i] = (float)psp_read16(a + 2u * (uint32_t)i) / 32768.0f; break;
        default: w[i] = psp_read_f32(a + 4u * (uint32_t)i); break;
        }
    }
}

/* ---- Skinning and morphing arithmetic ------------------------------------
 *
 * geprobe 16 (fw 6.60) reads skinned and morphed positions whole, the way
 * geprobe 12 read the vertex path (scenes 110-114, 4171 points drawn), and
 * every one fits this to the last bit:
 *  - Both are one accumulator. Each step adds a term to it, putting the two
 *    on the grid 2^(E - 15), each cut toward zero, E the larger one's own
 *    exponent (floor log2 |x|, not a product's ea + eb as ge_sum has it), and
 *    cuts the total to 16 bits (ge_acc). Summing all the terms at once on one
 *    grid fits 113 of scene 114's 160 four-set points; this fits all 480 of
 *    its morph-only ones.
 *  - Morphing adds each vertex set's coordinate times its morph weight (the
 *    24-bit float the command holds), set 0 first. The weights of a skinned
 *    vertex are morphed the same way, and the morphed vertex is then skinned
 *    once: skinning each set and morphing the results fits 4 of 160.
 *  - Skinning adds, bone by bone, the translation, x, y and z terms of that
 *    bone's row: (w_i B_i[k]) cut to 16 bits, times the coordinate, cut. Of
 *    the 8! orders of two bones' terms only this one (and swapping the first
 *    two) fits all 260 points that pose it; blending the matrices first and
 *    then one row sum fits 135 of 160 single-bone points.
 *  - Inputs are cut to 16 bits toward zero, as on the vertex path: a float
 *    weight too. 8- and 16-bit weights are unsigned, 0x80 and 0x8000 one:
 *    0xFF and 0xFFFF weigh just under two (scene 111's extremes).
 * The skinned position then takes the world, view and projection matrices
 * as an unskinned one would (scene 113).
 *
 * geprobe 22 (fw 6.60) reads the rest through the same accumulator:
 *  - Normals (scene 138, 2400 points lit through bones and morph sets whose
 *    terms cancel): skinned bone by bone over the x, y and z terms, no
 *    translation, and morphed set by set, as positions are. Of the six
 *    orders of a bone's terms only x, y, z fits all 2400; float32, as this
 *    did, fits 1315; adding the translation, 1 of the 300 that pose it.
 *  - Colours (scene 139, 4400 points): each channel, widened to 8 bits,
 *    times the 16-bit morph weight into the accumulator, then floored, the
 *    sign dropped and clamped to 255 -- a sum of -34.004 reads 34. Every one
 *    of the 13,200 channels fits.
 *  - Texture coordinates (scene 140, 2500 points at three scales): morphed
 *    in units before TEXSCALE and TEXOFFSET (uv_to_texels), so the offset is
 *    not weighted; all 15,000 readings fit, the float blend of the scaled
 *    coordinates, as this did, 7,025. */
static double ge_c16(double v) { return ge_cut(v, 16, 0); }

static double ge_acc(double acc, double t) {
    if (t == 0.0 || !isfinite(t) || !isfinite(acc)) return t == 0.0 ? acc : acc + t;
    if (acc == 0.0) return ge_c16(t);
    int ea = ge_frexp_e(acc), et = ge_frexp_e(t);
    if (ea == INT_MIN) frexp(acc, &ea);
    if (et == INT_MIN) frexp(t, &et);
    const int n = (ea > et ? ea : et) - 1 - 15;
    const double u = ge_pow2(n);
    if (n > -1022 && n < 1022) {                   /* 1/u exact too: the same quotients */
        const double iu = ge_pow2(-n);
        return ge_c16(ge_trunc(acc * iu) * u + ge_trunc(t * iu) * u);
    }
    return ge_c16(ge_trunc(acc / u) * u + ge_trunc(t / u) * u);
}

static void bones_cut(void) {
    for (int i = 0; i < 8 * 12; i++) g_bone16[i] = ge_c16(g_tl.bone[i]);
    g_bone16_ok = 1;
}

/* One axis of a skinned position (B[c][k] = bone[3k + c]), from the weights
 * and the position already cut (v[3] = 1). */
static float ge_skin_axis(const double *wi, const double v[4], int c) {
    static const int order[4] = { 3, 0, 1, 2 };
    double acc = 0.0;
    for (int i = 0; i < g_vl.w_n; i++) {
        for (int j = 0; j < 4; j++) {
            const int k = order[j];
            const double b = ge_c16(wi[i] * g_bone16[12 * i + 3 * k + c]);
            acc = ge_acc(acc, ge_c16(b * v[k]));
        }
    }
    return (float)acc;
}

/* One axis of a skinned normal: the x, y and z terms of each bone in turn,
 * as a position's without its translation. */
static float ge_skin_normal_axis(const double *wi, const double v[3], int c) {
    double acc = 0.0;
    for (int i = 0; i < g_vl.w_n; i++) {
        for (int k = 0; k < 3; k++) {
            const double b = ge_c16(wi[i] * g_bone16[12 * i + 3 * k + c]);
            acc = ge_acc(acc, ge_c16(b * v[k]));
        }
    }
    return (float)acc;
}

/* Skin a vertex in place, position and normal, by the GE's arithmetic. */
static void skin_mvert(ge_mvert *o, const float *w, int want_normal) {
    if (!g_bone16_ok) bones_cut();
    double wi[8];
    for (int i = 0; i < g_vl.w_n; i++) wi[i] = ge_c16(w[i]);
    const double v[4] = { ge_c16(o->pos[0]), ge_c16(o->pos[1]), ge_c16(o->pos[2]), 1.0 };
    float p[3], sn[3];
    for (int c = 0; c < 3; c++) p[c] = ge_skin_axis(wi, v, c);
    if (want_normal) {
        const double nv[3] = { ge_c16(o->nrm[0]), ge_c16(o->nrm[1]), ge_c16(o->nrm[2]) };
        for (int c = 0; c < 3; c++) sn[c] = ge_skin_normal_axis(wi, nv, c);
        memcpy(o->nrm, sn, sizeof sn);
    }
    memcpy(o->pos, p, sizeof p);
}

/* One vertex set of a record: its fields and, when it carries them, its
 * weights (w; left alone otherwise). Nothing is skinned yet. */
static int read_vertex_set(uint32_t a, uint32_t vtype, int col_off, int pos_off, int tex_off,
                           int norm_off, int want_normal, ge_mvert *o, float *w) {
    const uint8_t *vp = (const uint8_t *)psp_mem_ptr(a, (uint32_t)g_vl.set_stride);
    if (!read_pos_model_at(vp, a, vtype, pos_off, o->pos)) return 0;
    if (want_normal) read_normal_model_at(vp, a, vtype, norm_off, o->nrm);
    else { o->nrm[0] = o->nrm[1] = 0.0f; o->nrm[2] = 1.0f; }
    o->rgba = current_colour();
    if (col_off >= 0 && VT_COLOR(vtype) >= 4) o->rgba = read_colour_at(vp, a, vtype, col_off);
    read_uv_raw_at(vp, a, vtype, tex_off, &o->u, &o->v);       /* in units: read_mvert scales */
    if (g_vl.w_fmt) read_weights(a, w);
    return 1;
}

/* A record's texture coordinates, read or morphed in units, to texels. */
static void mvert_texels(ge_mvert *o, uint32_t vtype, int tex_off) {
    psp_vertex t;
    t.u = t.v = 0.0f;
    if (tex_off >= 0 && VT_TEX(vtype)) uv_to_texels(o->u, o->v, &t);
    o->u = t.u; o->v = t.v;
}

/* A whole record: its vertex sets blended by the morph weights, when there
 * is more than one, then skinned. Every field is blended, each through the
 * accumulator above (see there for colour's sign and clamp). */
static int read_mvert(uint32_t a, uint32_t vtype, int col_off, int pos_off, int tex_off,
                      int norm_off, int want_normal, ge_mvert *o) {
    float w[8] = { 0 };
    if (g_vl.morph_n <= 1) {
        if (!read_vertex_set(a, vtype, col_off, pos_off, tex_off, norm_off, want_normal, o, w))
            return 0;
        mvert_texels(o, vtype, tex_off);
        if (g_vl.w_fmt) skin_mvert(o, w, want_normal);
        return 1;
    }
    double pos[3] = { 0, 0, 0 }, nrm[3] = { 0, 0, 0 }, col[4] = { 0, 0, 0, 0 }, uv[2] = { 0, 0 }, mw8[8] = { 0 };
    for (int k = 0; k < g_vl.morph_n; k++) {
        ge_mvert s1;
        float w1[8] = { 0 };
        if (!read_vertex_set(a + (uint32_t)(k * g_vl.set_stride), vtype, col_off, pos_off, tex_off,
                             norm_off, want_normal, &s1, w1))
            return 0;
        const double m16 = ge_c16(g_tl.morph_w[k]);
        for (int i = 0; i < 3; i++) {
            pos[i] = ge_acc(pos[i], m16 * ge_c16(s1.pos[i]));
            nrm[i] = ge_acc(nrm[i], m16 * ge_c16(s1.nrm[i]));
        }
        for (int i = 0; i < g_vl.w_n && g_vl.w_fmt; i++) mw8[i] = ge_acc(mw8[i], m16 * ge_c16(w1[i]));
        for (int i = 0; i < 4; i++) col[i] = ge_acc(col[i], m16 * (double)((s1.rgba >> (8 * i)) & 0xFFu));
        uv[0] = ge_acc(uv[0], m16 * ge_c16(s1.u));
        uv[1] = ge_acc(uv[1], m16 * ge_c16(s1.v));
    }
    for (int i = 0; i < 3; i++) { o->pos[i] = (float)pos[i]; o->nrm[i] = (float)nrm[i]; }
    for (int i = 0; i < 8; i++) w[i] = (float)mw8[i];
    o->rgba = 0;
    for (int i = 0; i < 4; i++) {
        const double c = floor(fabs(col[i]));
        o->rgba |= (uint32_t)(c > 255.0 ? 255.0 : c) << (8 * i);
    }
    o->u = (float)uv[0]; o->v = (float)uv[1];
    mvert_texels(o, vtype, tex_off);
    if (g_vl.w_fmt) skin_mvert(o, w, want_normal);
    return 1;
}

/* Set while a patch draws: draw_prim_transformed then takes vertex i from
 * here rather than from the vertex array. */
static const ge_mvert *g_mv_src;

/* Clip space to screen. The viewport is the game's if it set one; the fallback
 * is the standard 480x272 arrangement, with y scaled negative because screen y
 * grows downward and clip y grows up. */
static void ndc_to_screen(float nx, float ny, float nz, float *sx, float *sy, float *sz) {
    if (g_tl.vp_set) {
        *sx = nx * g_tl.vp_xs + g_tl.vp_xc - g_tl.off_x;
        *sy = ny * g_tl.vp_ys + g_tl.vp_yc - g_tl.off_y;
    } else {
        *sx = nx * 240.0f + 240.0f;
        *sy = ny * -136.0f + 136.0f;
    }
    /* The z terms are set independently of the x/y ones, so a game can leave
     * them at zero; that would collapse every depth to one value and make the
     * test meaningless, so fall back to the full 0..65535 window range. */
    *sz = (g_tl.vp_zs != 0.0f) ? nz * g_tl.vp_zs + g_tl.vp_zc
                               : (nz * 0.5f + 0.5f) * 65535.0f;
}

/* Screen depth from clip z and w, as ge_sum says: zc plus zs times clip z
 * over w, floored. sceGuDepthRange(65535, 0) sends zs -32768 and zc 32767
 * (it halves 65535 as an integer). geprobe 11 (fw 6.60) scene 59's 3840
 * points at w = 1 pinned the sum by itself; geprobe 12 the rest. */
static float ge_screen_z(float cz, float w) {
    if (g_tl.vp_zs == 0.0f) return ((cz / w) * 0.5f + 0.5f) * 65535.0f;
    const double nz = ge_over_w(cz, w);
    const ge_term t[2] = { ge_mul(g_tl.vp_zs, nz), ge_mul(g_tl.vp_zc, 1.0) };
    return (float)ge_floor(ge_sum(t, 2));
}

static void to_screen(const float clip[4], float *sx, float *sy, float *sz) {
    const float inv = 1.0f / clip[3];
    ndc_to_screen(clip[0] * inv, clip[1] * inv, clip[2] * inv, sx, sy, sz);
    *sz = ge_screen_z(clip[2], clip[3]);
}

/* One axis of a vertex the GE read onto the 1/16 grid: the viewport centre
 * plus scale times clip over w as one of ge_sum's sums, less the screen
 * offset. With the centre at 2048 the sum's grid is a sixteenth, so this is
 * screen_axis_fx16's rule -- the product taken to sixteenths toward the
 * centre -- without its float product. */
static int ge_screen_axis(double ndc, float scale, float centre, float off) {
    const ge_term t[2] = { ge_mul(scale, ndc), ge_mul(centre, 1.0) };
    double v = (ge_sum(t, 2) - (double)off) * PSP_SUBPX;
    if (!(v > -1073741824.0)) v = -1073741824.0;   /* NaN too */
    if (v > 1073741824.0) v = 1073741824.0;
    return (int)ge_floor(v);
}

/* The same projection onto the rasterizer's grid, as screen_axis_fx16 says.
 *
 * A vertex the GE read from memory (plain, skinned or morphed) comes here
 * with clip x, y and w from ge_clip, and goes through ge_over_w and
 * ge_screen_axis: geprobe 12's arithmetic. Until then 1/w was cut to 24
 * bits and each quotient to 16, the fit below. geprobe 7 (fw 6.60) scene
 * 48 shows why: six of its corners sit a sixteenth further from the centre
 * than ge_recip's rule put them -- each one's triangle has a depth plane
 * 100 to 500 pixels off that matches once the corner moves, and the colour
 * dump's six differing pixels are at those corners. Of 49,920 variants
 * searched (the eye coordinates, the products, w, the quotient and the
 * scaled position each cut to 16 to 24 bits or left alone, a reciprocal of
 * 13 to 24 bits truncated or rounded, exact, or a divide), every one that
 * puts the 198 coordinates of scene 48's depth-confirmed corners where the
 * hardware has them, scene 15's corner at -1808 and scene 18's at -28944
 * cuts the eye coordinates to 16 bits, and with this reciprocal the
 * quotient too. Against run 7: scene 16 497 -> 369 pixels off, scene 36's
 * depth 696 -> 56, scene 39 217 -> 7, scene 48 6 -> 0 and its depth
 * 1898 -> 958; skinned and morphed scenes 20 and 21 stay at 101 and 400.
 * Tessellated vertices come this way too, since geprobe 13 (draw_patch). */
static void clip_to_fx16(const float clip[4], int *x, int *y) {
    const double gx = ge_over_w(clip[0], clip[3]), gy = ge_over_w(clip[1], clip[3]);
    if (g_tl.vp_set) {
        *x = ge_screen_axis(gx, g_tl.vp_xs, g_tl.vp_xc, g_tl.off_x);
        *y = ge_screen_axis(gy, g_tl.vp_ys, g_tl.vp_yc, g_tl.off_y);
        return;
    }
    const float nx = (float)gx, ny = (float)gy;
    if (g_tl.vp_set) {
        *x = screen_axis_fx16(nx, g_tl.vp_xs, g_tl.vp_xc - g_tl.off_x);
        *y = screen_axis_fx16(ny, g_tl.vp_ys, g_tl.vp_yc - g_tl.off_y);
    } else {
        *x = screen_axis_fx16(nx, 240.0f, 240.0f);
        *y = screen_axis_fx16(ny, -136.0f, 136.0f);
    }
}

/* ge_screen_axis with the scale and centre split through memos, one pair
 * per axis. */
static int ge_screen_axis_m(double ndc, float scale, float centre, float off, ge_sp_memo m[2]) {
    const ge_sp n = ge_sp_of(ndc);
    const ge_term t[2] = { ge_mul_sp(ge_sp_memo_of(&m[0], scale), &n),
                           ge_mul_sp(ge_sp_memo_of(&m[1], centre), &GE_SP_ONE) };
    double v = (ge_sum(t, 2) - (double)off) * PSP_SUBPX;
    if (!(v > -1073741824.0)) v = -1073741824.0;   /* NaN too */
    if (v > 1073741824.0) v = 1073741824.0;
    return (int)ge_floor(v);
}

/* to_screen and clip_to_fx16 for one vertex at once: their three ge_over_w
 * take 1/w from the same table entry, so it is looked up once. clip[3] must
 * be past the near limit, as for those two. */
static void clip_to_screen(const float clip[4], float *sx, float *sy, float *sz, int *x, int *y) {
    const float inv = 1.0f / clip[3];
    float unused;
    ndc_to_screen(clip[0] * inv, clip[1] * inv, clip[2] * inv, sx, sy, &unused);
    static ge_sp_memo mz[2], mx[2], my[2];
    const double r = ge_rcp16(clip[3]);
    if (g_tl.vp_zs == 0.0f) *sz = ((clip[2] / clip[3]) * 0.5f + 0.5f) * 65535.0f;
    else {
        const ge_sp nz = ge_sp_of(ge_cut(clip[2] * r, 16, 0));
        const ge_term t[2] = { ge_mul_sp(ge_sp_memo_of(&mz[0], g_tl.vp_zs), &nz),
                               ge_mul_sp(ge_sp_memo_of(&mz[1], g_tl.vp_zc), &GE_SP_ONE) };
        *sz = (float)ge_floor(ge_sum(t, 2));
    }
    const double gx = ge_cut(clip[0] * r, 16, 0), gy = ge_cut(clip[1] * r, 16, 0);
    if (g_tl.vp_set) {
        *x = ge_screen_axis_m(gx, g_tl.vp_xs, g_tl.vp_xc, g_tl.off_x, mx);
        *y = ge_screen_axis_m(gy, g_tl.vp_ys, g_tl.vp_yc, g_tl.off_y, my);
    } else {
        *x = screen_axis_fx16((float)gx, 240.0f, 240.0f);
        *y = screen_axis_fx16((float)gy, -136.0f, 136.0f);
    }
}

/* Transformed geometry, one primitive at a time.
 *
 * Triangles go through emit_tri below -- the near-plane clip, the guard band
 * and the cull are per-triangle decisions. Points and lines use the same
 * near-plane/guard-band policy below. Sprites retain the two-corner path. */
/* PSPRECOMP_GE_DRAWLOG=<n> narrates the first n primitives: where they landed,
 * what colour, and whether a texture was bound.
 *
 * The summary reports aggregates -- a bounding box over every transformed
 * vertex, one vertex type, one texture. When one element on screen looks wrong
 * and the rest looks right, aggregates cannot say which draw is the bad one.
 * This can. */
/* PSPRECOMP_GE_TEXDRAW=<hex> logs every draw that binds one particular texture,
 * with each vertex's position and texture coordinates.
 *
 * The plain draw log counts down from the first draws of the run, which is the
 * wrong end for a question about a menu three screens in -- by then its budget
 * is long spent. Naming the texture asks the question the other way round:
 * "show me the draws that use this", which is how a glyph atlas's quads are
 * found among a million primitives. */
static int texdraw_hit(void) {
    static uint32_t want = 1;
    if (want == 1) {
        const char *v = getenv("PSPRECOMP_GE_TEXDRAW");
        want = (v && *v) ? (uint32_t)strtoul(v, NULL, 0) : 0;
    }
    return want && g_ge.tex_addr == want;
}

/* PSPRECOMP_GE_WILDUV=1 logs textured draws whose coordinates land far outside
 * the texture, or are not finite. A texture that repeats hundreds of times
 * across one triangle paints stripes, and a draw like that is found by its
 * coordinates, not by which texture it bound. */
static int wilduv_hit(const psp_vertex *v, uint32_t n) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("PSPRECOMP_GE_WILDUV"); on = (e && *e) ? 1 : 0; }
    if (!on || !g_ge.tex_enable || !g_ge.tex_addr) return 0;
    const float lu = 8.0f * (float)(g_ge.tex_w ? g_ge.tex_w : 1);
    const float lv = 8.0f * (float)(g_ge.tex_h ? g_ge.tex_h : 1);
    for (uint32_t i = 0; i < n; i++) {
        if (!(v[i].u == v[i].u) || !(v[i].v == v[i].v)) return 1;
        if (v[i].u < -lu || v[i].u > lu || v[i].v < -lv || v[i].v > lv) return 1;
    }
    return 0;
}

static void texdraw_dump(const char *tag, const psp_vertex *v, uint32_t n) {
    static int left = 24;
    if (left <= 0) return;
    left--;
    if (g_tl.lighting) {
        fprintf(stderr, "texdraw-light: upd %d mode %d emis %.2f,%.2f,%.2f amb %.2f,%.2f,%.2f "
                        "dif %.2f,%.2f,%.2f spec %.2f,%.2f,%.2f coef %.2f global %.2f,%.2f,%.2f\n",
                g_tl.mat_update, g_tl.light_mode,
                (double)g_tl.mat_emissive[0], (double)g_tl.mat_emissive[1], (double)g_tl.mat_emissive[2],
                (double)g_tl.mat_ambient[0], (double)g_tl.mat_ambient[1], (double)g_tl.mat_ambient[2],
                (double)g_tl.mat_diffuse[0], (double)g_tl.mat_diffuse[1], (double)g_tl.mat_diffuse[2],
                (double)g_tl.mat_specular[0], (double)g_tl.mat_specular[1], (double)g_tl.mat_specular[2],
                (double)g_tl.mat_spec_coef,
                (double)g_tl.global_amb[0], (double)g_tl.global_amb[1], (double)g_tl.global_amb[2]);
        for (int i = 0; i < 4; i++) {
            if (!g_tl.light[i].enable) continue;
            fprintf(stderr, "texdraw-light:  L%d type %d kind %d pos %.1f,%.1f,%.1f dir %.2f,%.2f,%.2f "
                            "att %.3f,%.3f,%.3f exp %.2f cut %.2f  amb %.2f,%.2f,%.2f dif %.2f,%.2f,%.2f spec %.2f,%.2f,%.2f\n",
                    i, g_tl.light[i].type, g_tl.light[i].kind,
                    (double)g_tl.light[i].pos[0], (double)g_tl.light[i].pos[1], (double)g_tl.light[i].pos[2],
                    (double)g_tl.light[i].dir[0], (double)g_tl.light[i].dir[1], (double)g_tl.light[i].dir[2],
                    (double)g_tl.light[i].atten[0], (double)g_tl.light[i].atten[1], (double)g_tl.light[i].atten[2],
                    (double)g_tl.light[i].exponent, (double)g_tl.light[i].cutoff,
                    (double)g_tl.light[i].amb[0], (double)g_tl.light[i].amb[1], (double)g_tl.light[i].amb[2],
                    (double)g_tl.light[i].dif[0], (double)g_tl.light[i].dif[1], (double)g_tl.light[i].dif[2],
                    (double)g_tl.light[i].spec[0], (double)g_tl.light[i].spec[1], (double)g_tl.light[i].spec[2]);
        }
    }
    fprintf(stderr, "texdraw: %s %u verts  tex %08X %ux%u fmt %u  texen %u map %u/%u vtype %06X rgba %08X",
            tag, n, g_ge.tex_addr, g_ge.tex_w, g_ge.tex_h, g_ge.tex_format, g_ge.tex_enable,
            g_tl.tex_map_mode, g_tl.tex_proj_mode, g_ge.vtype, v[0].rgba);
    for (uint32_t i = 0; i < n && i < 4; i++)
        fprintf(stderr, "  | x %.3f y %.3f u %.3f v %.3f",
                (double)v[i].x / 16.0, (double)v[i].y / 16.0,
                (double)v[i].u, (double)v[i].v);
    fprintf(stderr, "\n");
}

/* PSPRECOMP_GE_DRAWLOG=<n> logs the first n draws; PSPRECOMP_GE_DRAWLOG_SKIP=<k>
 * skips k draws first, so the log can be aimed at the end of a run -- the
 * summary's "drawn N prims" is the count to aim by. Only draws count against
 * the skip; the matrix-upload lines share the budget but not the skip, or
 * they would eat it. */
static int s_dl_n = -1, s_dl_skip = 0;
static void drawlog_init(void) {
    if (s_dl_n >= 0) return;
    const char *v = getenv("PSPRECOMP_GE_DRAWLOG");      s_dl_n    = (v && *v) ? atoi(v) : 0;
    const char *k = getenv("PSPRECOMP_GE_DRAWLOG_SKIP"); s_dl_skip = (k && *k) ? atoi(k) : 0;
}
static int drawlog_left(void) {
    drawlog_init();
    if (s_dl_skip > 0) { s_dl_skip--; return 0; }
    return s_dl_n > 0 ? s_dl_n-- : 0;
}
static int drawlog_aux(void) {
    drawlog_init();
    if (s_dl_skip > 0) return 0;
    return s_dl_n > 0 ? s_dl_n-- : 0;
}

/* Triangles between the transform and the rasterizer.
 *
 * Until this was written, a triangle was dropped if any vertex had w <= 0 and
 * drawn as-is otherwise. A vertex between the eye and the near plane has a
 * small positive w and projects to a screen position in the hundreds of
 * thousands; the settings screen's 3D backdrop is full of them, and the
 * slivers those triangles left across the frame were the "white pixels in
 * lines" Sif saw in the background. Hardware never rasterizes such a vertex.
 *
 * The rules are read off gpu/clipping, forty data points, plus the one case
 * those tests cannot pose and the game does: a vertex behind the eye.
 *  - A triangle with every vertex at w <= 0 draws nothing ("Flat W=0: 0",
 *    "Flat W=-1: 0", "Linear W -1->-1->-1: 0").
 *  - With DEPTH_CLIP_ENABLE clear, near and far *reject*: any vertex with
 *    z/w outside -1..1 drops the triangle whole. guardband's
 *    TRIANGLE_OUT_NEG_Z has one vertex at -1.2 and two inside and is DRAW=0;
 *    "Z outside near (noclamp)" lights 0 pixels; "Flat W=0.001 (noclamp)" is
 *    0 because its other two vertices sit at z/w = 499.
 *  - With the flag set the hardware clamps rather than rejects, clips the
 *    near plane geometrically and the far plane not at all: "Z outside near"
 *    (one vertex at z = -2) lights 171 of the 255 pixels on the wide edge,
 *    the cut at t = 1/3; "Z outside both" (the others at 2) 192, the cut at
 *    t = 1/4; "Z outside far" alone keeps all 255. Depth is pinned to the
 *    range after projection.
 *  - The near plane is z + w = 0 in *clip* space and the cut is made there,
 *    before the divide. This used to divide first and clip z/w >= -1 in NDC,
 *    and the two tests cannot tell the difference: every vertex they pose
 *    sits at z = -w exactly, so "Linear W 1->-1->-1" lights the same 16,384
 *    pixels either way -- (-w,-w,-w,w)/w is one point whatever the sign of
 *    w. The hangar can tell. Its wall pieces are drawn with the camera
 *    inside them, so vertices behind the eye reach the GE at w < 0 with no
 *    clipping by the game. Divided, such a vertex lands mirrored through the
 *    screen centre with z/w inside -1..1 (the game's projection puts it at
 *    1.4), the NDC clip keeps it, and the triangle is either a shard across
 *    the frame or, for most of them, dropped by the guard band: 117,000
 *    vertices a run "behind the eye", 73,000 "outside the guard band", and
 *    a floor that was black. In clip space z + w is, for any standard
 *    projection, an affine function of eye z that is positive in front of
 *    the near plane and negative behind the eye, so one cut handles both and
 *    the divide only sees what survives it. A vertex that passes the test
 *    with w < 0 is still divided, as hardware does -- that is what the
 *    "1->-1->-1" row measures.
 *  - The guard band. Screen positions are 12 bits, a 4096-square box placed
 *    by OFFSET_X/Y, and a triangle with **any** vertex outside it is not
 *    drawn, whatever the depth flag. guardband is precise about this: its
 *    TRIANGLE_OUT_NEG_X puts one vertex at x = -1809 against a -1808 edge,
 *    leaves the other two well inside, and reads DRAW=0. This was briefly
 *    relaxed to "all three beyond the same edge" on a theory about the
 *    hangar's missing walls; the theory was wrong, the sweep caught it as a
 *    regression on this test, and the walls turned out to be dark for
 *    reasons in the compositing passes instead.
 *
 * Attributes interpolate linearly along the cut edge in clip space, which is
 * exact for the cut vertex; colour rounds to the nearest channel value. The
 * clip preserves orientation, so the cull test runs on the first clipped
 * triangle. */
typedef struct { float c[4]; psp_vertex v; } clipvert;

static void lerp_clip(const clipvert *a, const clipvert *b, float t, clipvert *o) {
    for (int k = 0; k < 4; k++) o->c[k] = a->c[k] + (b->c[k] - a->c[k]) * t;
    o->v = a->v;
    o->v.u = a->v.u + (b->v.u - a->v.u) * t;
    o->v.v = a->v.v + (b->v.v - a->v.v) * t;
    o->v.tex_q = a->v.tex_q + (b->v.tex_q - a->v.tex_q) * t;
    uint32_t r = 0;
    for (int k = 0; k < 4; k++) {
        const float ca = (float)((a->v.rgba >> (8 * k)) & 0xFFu);
        const float cb = (float)((b->v.rgba >> (8 * k)) & 0xFFu);
        int q = (int)(ca + (cb - ca) * t + 0.5f);
        if (q < 0) q = 0;
        if (q > 255) q = 255;
        r |= (uint32_t)q << (8 * k);
    }
    o->v.rgba = r;
    if (a->v.spec_set || b->v.spec_set) {
        const uint32_t sa = a->v.spec_set ? a->v.spec : 0, sb = b->v.spec_set ? b->v.spec : 0;
        uint32_t sr = 0;
        for (int k = 0; k < 3; k++) {
            const float ca = (float)((sa >> (8 * k)) & 0xFFu), cb = (float)((sb >> (8 * k)) & 0xFFu);
            sr |= (uint32_t)(int)(ca + (cb - ca) * t + 0.5f) << (8 * k);
        }
        o->v.spec = sr;
        o->v.spec_set = 1;
    }
    int fg = (int)((float)a->v.fog + ((float)b->v.fog - (float)a->v.fog) * t + 0.5f);
    if (fg < 0) fg = 0;
    if (fg > 255) fg = 255;
    o->v.fog = fg;
}

/* The near plane in clip space, Sutherland-Hodgman: keeps z + w >= 0. */
static int clip_near(const clipvert *in, int n, clipvert *out) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        const clipvert *a = &in[i], *b = &in[(i + 1) % n];
        const float da = a->c[2] + a->c[3], db = b->c[2] + b->c[3];
        if (da >= 0.0f) out[m++] = *a;
        if ((da >= 0.0f) != (db >= 0.0f)) lerp_clip(a, b, da / (da - db), &out[m++]);
    }
    return m;
}

/* A line is clipped as a segment, not as a two-vertex polygon (which would
 * generate the same intersection twice). Points have no intersection to add.
 * This follows the triangle path's existing clip/clamp and guard-band policy. */
static void emit_point_line(const psp_render_backend *be, clipvert p[2], int n) {
    /* Past the x and y planes, with depth clamping or without: a point
     * outside either is not drawn, and a line whose two ends lie beyond the
     * same one is not either; a line with an end inside, or one across the
     * volume, is drawn whole. geprobe 22 (fw 6.60) scene 141: points at
     * 0.95 to 1.05 w on every side draw only to 1 w with clamping on too (it
     * drew all nine), and lines beyond x = w at both ends draw nothing either
     * way (13 and 7 pixels before); its 74 other lines match as they were.
     * Ends beyond different planes are drawn whole even where the line
     * misses the volume, and an end on a plane is not beyond it (geprobe
     * 23 scene 142: 20 such lines, as they were). With clamping on, a line
     * with both ends past the far plane is not drawn either (11 pixels
     * before); past the near one the clip below drops it. */
    if (n == 1 && !(fabsf(p[0].c[0]) <= p[0].c[3] && fabsf(p[0].c[1]) <= p[0].c[3])) {
        g_clip_guard += 1; return;
    }
    if (n == 2) {
        for (int k = 0; k < 2; k++)
            if ((p[0].c[k] > p[0].c[3] && p[1].c[k] > p[1].c[3]) ||
                (p[0].c[k] < -p[0].c[3] && p[1].c[k] < -p[1].c[3])) { g_clip_guard += 2; return; }
        if (g_tl.depth_clamp && p[0].c[2] > p[0].c[3] && p[1].c[2] > p[1].c[3]) { g_clip_z += 2; return; }
    }
    /* With clamping on a point past the far plane is not drawn either (scene
     * 141: z 1.0001 w on); the near one is the clip below's. */
    if (n == 1 && g_tl.depth_clamp && p[0].c[2] > p[0].c[3]) { g_clip_z += 1; return; }
    if (!g_tl.depth_clamp) {
        for (int i = 0; i < n; i++) {
            if (!(p[i].c[3] > 0)) { g_skip_nearplane += (uint64_t)n; return; }
            const float z = p[i].c[2] / p[i].c[3];
            if (!(z >= -1 && z <= 1)) { g_clip_z += (uint64_t)n; return; }
            /* The x and y planes are above: geprobe 16 (fw 6.60) first saw
             * them, of scenes 110-114's points every one drawn within 0.989
             * w, every one missing (379) at least 1.005 w. */
        }
    } else {
        const float a = p[0].c[2] + p[0].c[3];
        const float b = p[n-1].c[2] + p[n-1].c[3];
        if (a < 0 && b < 0) { g_clip_z += (uint64_t)n; return; }
        if ((a < 0) != (b < 0)) {
            clipvert cut;
            lerp_clip(&p[0], &p[1], a / (a-b), &cut);
            p[a < 0 ? 0 : 1] = cut;
            g_clip_split++;
        }
    }
    psp_vertex v[2];
    const float ox = g_tl.vp_set ? g_tl.off_x : 1808.0f;
    const float oy = g_tl.vp_set ? g_tl.off_y : 1912.0f;
    for (int i = 0; i < n; i++) {
        if (!(p[i].c[3] > 0)) { g_skip_nearplane += (uint64_t)n; return; }
        float x, y, z;
        to_screen(p[i].c, &x, &y, &z);
        if (!(x >= -ox && x < 4096-ox && y >= -oy && y < 4096-oy) || !isfinite(z) ||
            (!g_tl.depth_clamp && (z < 0.0f || z > 65535.0f))) {
            /* A point whose depth falls outside 0..65535 is not drawn
             * (geprobe 13, fw 6.60: every calibration point with clip z in
             * (-w, 0) at a positive scale, or past 65535, is absent), and
             * nor is a line with an end there: geprobe 23 scene 142's line
             * with both ends on z = w, its depth just under 0 at a depth
             * range of 65535 to 0, draws nothing (11 pixels before), the
             * one on z = -w all its pixels. */
            g_clip_guard += (uint64_t)n; return;
        }
        v[i] = p[i].v;
        clip_to_fx16(p[i].c, &v[i].x, &v[i].y);
        v[i].precise_x = x; v[i].precise_y = y; v[i].precise = 1;
        v[i].z = g_tl.depth_clamp ? fmaxf(0, fminf(65535, z)) : z;
        v[i].inv_w = 1.0f / p[i].c[3];
    }
    be->draw(n == 1 ? PSP_PRIM_POINTS : PSP_PRIM_LINES, v, n);
}

/* A triangle whose three corners all lie beyond one plane of the clip volume
 * -- x or y past w either way, or with depth clamping on z past the far plane
 * -- is not drawn: geprobe 23 (fw 6.60) scene 142, every side, clamping off
 * and on (22 to 126 pixels each before). Corners on a plane are not beyond
 * it, and corners beyond different planes draw whole, as they did (its 14
 * such triangles). Returns 1 when culled. */
static int tri_beyond_one_plane(const float *a, const float *b, const float *c) {
    const float *t[3] = { a, b, c };
    for (int k = 0; k < 2; k++) {
        int hi = 0, lo = 0;
        for (int i = 0; i < 3; i++) { hi += t[i][k] > t[i][3]; lo += t[i][k] < -t[i][3]; }
        if (hi == 3 || lo == 3) { g_clip_guard += 3; return 1; }
    }
    if (g_tl.depth_clamp && a[2] > a[3] && b[2] > b[3] && c[2] > c[3]) { g_clip_z += 3; return 1; }
    return 0;
}

static void emit_tri(const psp_render_backend *be, const clipvert tri[3], int flip) {
    int behind = 0;
    for (int i = 0; i < 3; i++) if (tri[i].c[3] <= 0.0f) behind++;
    if (behind == 3) { g_clip_eye += 3; return; }
    if (tri_beyond_one_plane(tri[0].c, tri[1].c, tri[2].c)) return;

    clipvert b[9];
    const clipvert *poly = tri; int n = 3;
    if (!g_tl.depth_clamp) {
        for (int i = 0; i < 3; i++) {
            if (tri[i].c[3] == 0.0f) { g_clip_eye += 3; return; }
            const float nz = tri[i].c[2] / tri[i].c[3];
            if (!(nz >= -1.0f && nz <= 1.0f)) { g_clip_z += 3; return; }
        }
    } else {
        n = clip_near(tri, 3, b);
        if (n < 3) { g_clip_z += 3; return; }
        poly = b;
        if (n > 3) g_clip_split += (uint64_t)(n - 3);
    }

    /* Divide, project, then the guard band. A vertex at w = 0 exactly has no
     * projection, and hardware draws nothing for it ("Flat W=0"). */
    const float ox = g_tl.vp_set ? g_tl.off_x : 1808.0f;
    const float oy = g_tl.vp_set ? g_tl.off_y : 1912.0f;
    psp_vertex p[9];
    unsigned any_out = 0;
    for (int i = 0; i < n; i++) {
        if (poly[i].c[3] == 0.0f) { g_clip_eye += 3; return; }
        const float inv = 1.0f / poly[i].c[3];
        float sx, sy, sz;
        ndc_to_screen(poly[i].c[0] * inv, poly[i].c[1] * inv, poly[i].c[2] * inv, &sx, &sy, &sz);
        sz = ge_screen_z(poly[i].c[2], poly[i].c[3]);
        if (g_tl.depth_clamp) {
            if (sz < 0.0f) sz = 0.0f;
            if (sz > 65535.0f) sz = 65535.0f;
        }
        if (sx < -ox || sx >= 4096.0f - ox || sy < -oy || sy >= 4096.0f - oy) any_out = 1;
        p[i] = poly[i].v;
        clip_to_fx16(poly[i].c, &p[i].x, &p[i].y);
        p[i].precise_x = sx; p[i].precise_y = sy; p[i].precise = 1;
        p[i].z = sz;
        /* Keep the divide's missing term with the screen-space vertex.  UVs
         * are the one interpolant the PSP corrects for perspective; colour
         * and fog remain affine in the rasterizer. */
        p[i].inv_w = inv;
        if (!g_tl.bb_seen) { g_tl.bb_x0 = g_tl.bb_x1 = sx; g_tl.bb_y0 = g_tl.bb_y1 = sy; g_tl.bb_seen = 1; }
        if (sx < g_tl.bb_x0) g_tl.bb_x0 = sx;
        if (sx > g_tl.bb_x1) g_tl.bb_x1 = sx;
        if (sy < g_tl.bb_y0) g_tl.bb_y0 = sy;
        if (sy > g_tl.bb_y1) g_tl.bb_y1 = sy;
    }
    if (any_out) { g_clip_guard += 3; return; }

    const long ax = p[1].x - p[0].x, ay = p[1].y - p[0].y;
    const long bx = p[2].x - p[0].x, by = p[2].y - p[0].y;
    long area = ax * by - ay * bx;
    if (flip) area = -area;
    if (g_tl.cull_enable && area != 0 && ((area < 0) == (g_tl.cull_ccw != 0))) { g_culled += 3; return; }

    /* A flat triangle is its last vertex's colour; the clip's cut vertices
     * would otherwise hand the rasterizer an interpolated one as the last. */
    if (g_tl.blend.shade_flat)
        for (int i = 0; i < n; i++) p[i].rgba = tri[2].v.rgba;
    for (int i = 1; i + 1 < n; i++) {
        const psp_vertex t[3] = { p[0], p[i], p[i + 1] };
        be->draw(PSP_PRIM_TRIANGLES, t, 3);
    }
}

/* Per-vertex lighting.
 *
 * Until this was written, LIGHTING_ENABLE was ignored and the vertex's own
 * colour field was used as the shaded colour. Hardware does the opposite:
 * with lighting on, the vertex colour is not a colour at all -- it feeds
 * whichever material components MATERIAL_COLOR selects -- and the colour that
 * reaches the rasterizer is computed here. Armored Core's hangar carries a
 * vertex colour around 0x10 grey and gets its brightness entirely from a
 * light, so the settings screen rendered a near-black room with only its
 * brightest texture seams showing: the "white pixels in lines".
 *
 * Every rule below is a reading from gpu/commands/light, which lights a
 * two-pixel box of known normal with a red ambient, green diffuse and blue
 * specular light and prints the pixel:
 *
 *  - The light's ambient always contributes; attenuation and the spot factor
 *    scale it along with everything else ("Diffuse 0.5 - Spot A + D: 7f3f00"
 *    halves the ambient and quarters the diffuse).
 *  - Diffuse is max(N.L, 0), or pow(N.L, specular coefficient) when the
 *    light's kind is powered diffuse.
 *  - Specular is pow(N.H, coefficient) with H = normalize(L + (0,0,1)) -- a
 *    fixed eye direction, not the view position: with N.L = 0 the test reads
 *    0xb5, and 1/sqrt(2) is what a fixed +Z eye gives. It contributes only
 *    when N.L >= 0 ("Diffuse 0.0 ... 0xb5" but "Diffuse -0.5 ... 0x00").
 *  - The spot factor compares the *vertex-to-light* direction against the
 *    spot direction, dot(L, D), not the usual dot(-L, D): with the light
 *    overhead and a direction of +Z the test reads a full-strength spot.
 *    Below the cutoff the light contributes nothing.
 *  - Directional lights ignore attenuation and take their position field as
 *    a direction.
 *
 * Two more from geprobe 5 scene 25 (fw 6.60), which lights one flat quad per
 * normal with white lights and materials, 198 values, all matched:
 *  - The powers (specular, powered diffuse) are ge_pow's, a piecewise-linear
 *    log2 and exp2, not pow(): N.H = 0.9 with coefficient 4 reads 153
 *    (0.6), where 0.9^4 = 0.656 would give 167. The two wide quads read one
 *    value from edge to edge, so the eye direction is the fixed +Z above,
 *    and normals are normalised (the 0.9 normal scaled by 2 and by 0.5
 *    reads as the unit one).
 *  - A lit channel goes to 8 bits as floor(256 * value), 1.0 being a full
 *    255 light on a 255 material: N.L = 0.99 reads 253, where rounding
 *    255 * 0.99 gives 252 and 0.6 reads 153, not 154. That is the byte
 *    arithmetic at lit_mul, below, seen through a white light.
 *
 * And the sum, from geprobe 7 (fw 6.60), scenes 16 and 43:
 *  - Each light adds lit_mul(spot, lit_mul(attenuation, term)), the term
 *    being its ambient plus its diffuse, and its specular separately; the
 *    factors are bytes too (lit_byte). Geprobe 6 scene 35's attenuated and
 *    spot-lit points fit this on 790 of 800 (run 7). The 10 others are a
 *    step low, each where ge_pow's spot or specular power lands just below
 *    a byte boundary (within 0.04 of one): the GE's power comes out a hair
 *    higher there.
 *  - Single-colour mode (LIGHTMODE 0) adds the specular in and clamps the
 *    total to 255 per vertex: scene 16's bottom right fan, whose rims sum
 *    past 255 in blue, is 630 pixels off with the excess carried to the
 *    pixel and 92 with it clamped here, the 92 being slips its unclamped
 *    neighbour shows too.
 *  - Separate-specular mode keeps the specular apart, clamped on its own,
 *    as the vertex's secondary colour (psp_vertex.spec), which the
 *    rasterizer interpolates and adds per pixel.
 */
/* The lights in eye space, for a backend that lights in eye space itself
 * (fill_xform_state): the same light as the GE's world-space lighting below
 * (light_vertex), with the eye's direction (0, 0, 1) where the GE takes the
 * view's third row. A directional light's position field is a direction and
 * only rotates, a point or spot light's is a point and translates too. */
static struct { float pos[3], dir[3]; } g_light_eye[4];

static void lights_to_eye(void) {
    light_inputs now;
    memset(&now, 0, sizeof now);
    memcpy(now.view, g_tl.view, sizeof now.view);
    for (int i = 0; i < 4; i++) {
        now.light[i].enable = g_tl.light[i].enable; now.light[i].type = g_tl.light[i].type;
        memcpy(now.light[i].pos, g_tl.light[i].pos, sizeof now.light[i].pos);
        memcpy(now.light[i].dir, g_tl.light[i].dir, sizeof now.light[i].dir);
    }
    now.valid = 1;
    if (memcmp(&now, &g_light_eye_from, sizeof now) == 0) return;
    g_light_eye_from = now;
    for (int i = 0; i < 4; i++) {
        if (!g_tl.light[i].enable) continue;
        if (g_tl.light[i].type == 0) mul_3x3(g_tl.view, g_tl.light[i].pos, g_light_eye[i].pos);
        else                         mul_4x3(g_tl.view, g_tl.light[i].pos, g_light_eye[i].pos);
        mul_3x3(g_tl.view, g_tl.light[i].dir, g_light_eye[i].dir);
    }
}

/* x^k as the GE's lighting computes it (geprobe 5 scene 25, fw 6.60): a log2
 * that reads a float's exponent and mantissa as e + (m - 1), m in [1, 2), and
 * the matching exp2, 2^n * (1 + f) for the integer and fractional parts of the
 * product. With k = 1 it is x itself; N.H = 0.9 gives 0.8, 0.6, 0.35, 0.2 and
 * 0.1125 for k = 2, 4, 8, 12 and 16, which is what the hardware drew. The
 * spot exponent goes through it too (geprobe 6 scene 35, fw 6.60): of 200
 * points lit by a spot of exponent 4, 174 read ge_pow's value and 122
 * powf's, and with exponent 1.5, 144 and 88. With the byte arithmetic of
 * geprobe 7 (lit_mul) 197 and all 200 read it, and the three left are a
 * step off next to a byte boundary: ge_pow's own precision, not known. */
static float ge_pow(float x, float k) {
    if (!(x > 0.0f)) return 0.0f;
    int e;
    float m;                                                /* [0.5, 1) */
    uint32_t b;
    memcpy(&b, &x, sizeof b);
    if (((b >> 23) & 0xFF) && ((b >> 23) & 0xFF) != 0xFF) { /* normal: frexpf by hand */
        e = (int)((b >> 23) & 0xFF) - 126;
        b = (b & 0x807FFFFFu) | (126u << 23);
        memcpy(&m, &b, sizeof m);
    } else m = frexpf(x, &e);
    const float y = k * ((float)(e - 1) + (2.0f * m - 1.0f));
    if (!(y > -126.0f)) return 0.0f;
    if (!(y < 127.0f)) {                                    /* the library's overflow */
        const float n = floorf(y);
        return ldexpf(1.0f + (y - n), (int)n);
    }
    /* floorf and ldexpf: y is in (-126, 127), so n is an int and 2^n a
     * normal float, and the product rounds once as ldexpf's does. */
    const int ni = (int)y - ((float)(int)y > y);
    const float n = (float)ni;
    const uint32_t pb = (uint32_t)(ni + 127) << 23;
    float p;
    memcpy(&p, &pb, sizeof p);
    return (1.0f + (y - n)) * p;
}

/* ---- The lighting's vectors (geprobe 17, fw 6.60) ----------------------
 *
 * Scene 115 reads L.D whole: the spot cutoff is a threshold on it, so the
 * probe searches the cutoff a pass at a time on the PSP for 3800 geometries.
 * 116-119 read the spot, diffuse, specular and attenuation factors as bytes,
 * 17122 more. All 20922 fit this, in the vertex path's arithmetic (ge_mul,
 * ge_sum) wherever a product or sum is taken:
 *  - 1/sqrt is a table like 1/w's: 128 entries for each parity of the
 *    exponent, a value and a slope each, indexed by the significand's top 7
 *    bits after the leading one and interpolated by the next 8 (ge_rsqrt16).
 *    The light at (a/128, b/128, 1) 2^j over a vertex at the origin, with
 *    D = +z, reads it directly: its 128-segment chords fit every point to
 *    the last bit where 64 segments leave kinks. geprobe 18 (scene 120)
 *    reads it at every one of its 65536 16-bit inputs in [1, 4) the same
 *    way, so each entry's 256 outputs are known and exactly one value and
 *    slope give them. Every entry is then a formula: the value
 *    floor(2^17/sqrt(knot)) + 1, the slope 2 floor(c/4), c the exact chord
 *    2^17/sqrt(knot) - 2^17/sqrt(next knot) (the last knot's next being 2
 *    or 4). The table below is that, as measured.
 *  - L, the light minus the vertex (or a directional light's vector), is
 *    normalised component by component: L times 1/sqrt(L.L), each cut to 16
 *    bits.
 *  - The spot direction and the normal are not: the GE takes the dot product
 *    with them as given and scales it by their 1/sqrt after. Normalising D
 *    component by component misses 145 of 1200 general geometries (every one
 *    where 1/sqrt(D.D) is not 1); scaling after misses none. N.L the same
 *    way fits all 5000 diffuse bytes.
 *  - H is L + (0, 0, 1) normalised component by component, and N.H is taken
 *    like N.L.
 *  - Spot exponents and specular coefficients keep 5 significant bits, cut
 *    toward zero, before ge_pow: every exponent with no more (quarters,
 *    halves, whole numbers) already fit, and of 942 with more, all do so
 *    and 613 at best otherwise.
 *  - The distance is L.L times its 1/sqrt, the attenuation's sum k0 + k1 d +
 *    (k2 d) d one GE sum, and its reciprocal the 1/w table's (ge_rcp16).
 * Scene 35's last ten points, each a step low, come out right. */
static const uint32_t ge_rsq_tab[2][128][2] = {
    {  /* even exponent: s in [1, 2) */
        { 131073, 254 }, { 130563, 250 }, { 130060, 248 }, { 129563, 244 }, { 129071, 242 }, { 128585, 240 }, { 128104, 236 }, { 127629, 234 },
        { 127159, 232 }, { 126694, 228 }, { 126234, 226 }, { 125779, 224 }, { 125329, 222 }, { 124884, 220 }, { 124444, 216 }, { 124008, 214 },
        { 123576, 212 }, { 123150, 210 }, { 122727, 208 }, { 122309, 206 }, { 121895, 204 }, { 121485, 202 }, { 121080, 200 }, { 120678, 198 },
        { 120280, 196 }, { 119887, 194 }, { 119497, 192 }, { 119111, 190 }, { 118728, 188 }, { 118350, 186 }, { 117975, 184 }, { 117603, 184 },
        { 117235, 182 }, { 116870, 180 }, { 116509, 178 }, { 116151, 176 }, { 115796, 174 }, { 115445, 174 }, { 115097, 172 }, { 114752, 170 },
        { 114410, 168 }, { 114071, 166 }, { 113735, 166 }, { 113401, 164 }, { 113071, 162 }, { 112744, 162 }, { 112420, 160 }, { 112098, 158 },
        { 111779, 158 }, { 111463, 156 }, { 111149, 154 }, { 110838, 154 }, { 110530, 152 }, { 110224, 150 }, { 109921, 150 }, { 109620, 148 },
        { 109322, 146 }, { 109026, 146 }, { 108733, 144 }, { 108442, 144 }, { 108153, 142 }, { 107866, 142 }, { 107582, 140 }, { 107300, 138 },
        { 107020, 138 }, { 106743, 136 }, { 106467, 136 }, { 106194, 134 }, { 105923, 134 }, { 105653, 132 }, { 105386, 132 }, { 105121, 130 },
        { 104858, 130 }, { 104597, 128 }, { 104338, 128 }, { 104080, 126 }, { 103825, 126 }, { 103571, 124 }, { 103320, 124 }, { 103070, 124 },
        { 102822, 122 }, { 102576, 122 }, { 102331, 120 }, { 102088, 120 }, { 101847, 118 }, { 101608, 118 }, { 101370, 118 }, { 101134, 116 },
        { 100900, 116 }, { 100667, 114 }, { 100436, 114 }, { 100206, 112 }, { 99978, 112 }, { 99752, 112 }, { 99527, 110 }, { 99304, 110 },
        { 99082, 110 }, { 98861, 108 }, { 98642, 108 }, { 98425, 108 }, { 98209, 106 }, { 97994, 106 }, { 97781, 104 }, { 97569, 104 },
        { 97358, 104 }, { 97149, 102 }, { 96941, 102 }, { 96735, 102 }, { 96530, 100 }, { 96326, 100 }, { 96123, 100 }, { 95922, 100 },
        { 95722, 98 }, { 95523, 98 }, { 95326, 98 }, { 95129, 96 }, { 94934, 96 }, { 94740, 96 }, { 94547, 94 }, { 94356, 94 },
        { 94165, 94 }, { 93976, 94 }, { 93788, 92 }, { 93601, 92 }, { 93415, 92 }, { 93230, 90 }, { 93047, 90 }, { 92864, 90 },
    },
    {  /* odd exponent: s in [2, 4) */
        { 92682, 178 }, { 92322, 176 }, { 91967, 174 }, { 91615, 172 }, { 91267, 170 }, { 90924, 168 }, { 90584, 168 }, { 90248, 166 },
        { 89915, 164 }, { 89586, 162 }, { 89261, 160 }, { 88940, 158 }, { 88621, 156 }, { 88307, 154 }, { 87995, 154 }, { 87687, 152 },
        { 87382, 150 }, { 87080, 148 }, { 86781, 146 }, { 86486, 146 }, { 86193, 144 }, { 85903, 142 }, { 85616, 140 }, { 85332, 140 },
        { 85051, 138 }, { 84773, 136 }, { 84497, 136 }, { 84224, 134 }, { 83954, 132 }, { 83686, 132 }, { 83421, 130 }, { 83158, 130 },
        { 82898, 128 }, { 82640, 126 }, { 82384, 126 }, { 82131, 124 }, { 81881, 124 }, { 81632, 122 }, { 81386, 122 }, { 81142, 120 },
        { 80900, 118 }, { 80660, 118 }, { 80423, 116 }, { 80187, 116 }, { 79954, 114 }, { 79722, 114 }, { 79493, 112 }, { 79265, 112 },
        { 79040, 110 }, { 78816, 110 }, { 78595, 108 }, { 78375, 108 }, { 78157, 108 }, { 77941, 106 }, { 77726, 106 }, { 77513, 104 },
        { 77303, 104 }, { 77093, 102 }, { 76886, 102 }, { 76680, 102 }, { 76476, 100 }, { 76273, 100 }, { 76072, 98 }, { 75873, 98 },
        { 75675, 98 }, { 75479, 96 }, { 75284, 96 }, { 75091, 94 }, { 74899, 94 }, { 74708, 94 }, { 74520, 92 }, { 74332, 92 },
        { 74146, 92 }, { 73961, 90 }, { 73778, 90 }, { 73596, 90 }, { 73416, 88 }, { 73236, 88 }, { 73058, 88 }, { 72882, 86 },
        { 72706, 86 }, { 72532, 86 }, { 72359, 84 }, { 72187, 84 }, { 72017, 84 }, { 71848, 84 }, { 71680, 82 }, { 71513, 82 },
        { 71347, 82 }, { 71182, 80 }, { 71019, 80 }, { 70857, 80 }, { 70695, 80 }, { 70535, 78 }, { 70376, 78 }, { 70218, 78 },
        { 70061, 76 }, { 69906, 76 }, { 69751, 76 }, { 69597, 76 }, { 69444, 74 }, { 69292, 74 }, { 69142, 74 }, { 68992, 74 },
        { 68843, 72 }, { 68695, 72 }, { 68548, 72 }, { 68402, 72 }, { 68257, 72 }, { 68113, 70 }, { 67970, 70 }, { 67827, 70 },
        { 67686, 70 }, { 67545, 68 }, { 67406, 68 }, { 67267, 68 }, { 67129, 68 }, { 66992, 68 }, { 66855, 66 }, { 66720, 66 },
        { 66585, 66 }, { 66451, 66 }, { 66318, 66 }, { 66186, 64 }, { 66055, 64 }, { 65924, 64 }, { 65794, 64 }, { 65665, 64 },
    },
};

/* 1/sqrt(s) from the table, s finite and positive: s = sig 2^(e-15). */
static double ge_rsqrt16(double s) {
    int64_t sig;
    int e;
    if (!(s > 0.0) || ge_split(s, &sig, &e) <= 0) return INFINITY;
    const int p = e & 1, t = (int)((sig & 0x7FFF) >> 8), l = (int)(sig & 0xFF);
    const int64_t q = (128 * (int64_t)ge_rsq_tab[p][t][0] - (int64_t)ge_rsq_tab[p][t][1] * l - 1) >> 8;
    return ge_ldexp((double)q, -16 - (e - p) / 2);
}

static double gl_mul(double a, double b) { const ge_term t = ge_mul(a, b); return ge_sum(&t, 1); }

static double gl_dot3(const double a[3], const double b[3]) {
    const ge_term t[3] = { ge_mul(a[0], b[0]), ge_mul(a[1], b[1]), ge_mul(a[2], b[2]) };
    return ge_sum(t, 3);
}

/* v normalised component by component. */
static void gl_unit(const double v[3], double o[3]) {
    const double r = ge_rsqrt16(gl_dot3(v, v));
    for (int k = 0; k < 3; k++) o[k] = isfinite(r) ? ge_cut(v[k] * r, 16, 0) : 0.0;
}

static inline int any_light_enabled(void) {
    return g_tl.light[0].enable || g_tl.light[1].enable || g_tl.light[2].enable || g_tl.light[3].enable;
}

/* The lit colour's arithmetic is in bytes (geprobe 7 scene 43, fw 6.60). A
 * byte x stands for (x + 1/2)/256, and the product of two is
 *
 *     lit_mul(a, b) = floor((a + 1/2)(b + 1/2) / 256),
 *
 * so 255 times x is x, and 192 times 1 is 1 where the exact 192/255 * 1/255
 * would floor to 0. Scene 43 steps every byte through a 255, 192 and 128
 * partner, as light or as material, and all 1536 values fit; light and
 * material give the same table either way round. N.L and the other factors
 * become bytes first, floor(256 x) up to 255 (lit_byte): row E steps N.L
 * under a 0x80C0FF light and its 768 values fit only so, where an exact
 * N.L times the colour misses 75. A light's diffuse term is
 * lit_mul(N.L, lit_mul(light, material)), and every value of geprobe 6
 * scene 34 fits it, coloured rows included (12737 pixels were off). */
static inline int lit_mul(int a, int b) { return ((2 * a + 1) * (2 * b + 1)) >> 10; }
static inline int lit_byte(float x) {
    if (!(x > 0.0f)) return 0;
    const float q = x * 256.0f;
    return q >= 255.0f ? 255 : (int)q;
}
static inline int lit_colour_byte(float c) { return (int)lrintf(c * 255.0f); }

/* The GE lights in world space (geprobe 19, fw 6.60). Scene 121 runs scene
 * 115's cutoff search with real world and view matrices (2504 geometries
 * under 313 pairs, translations up to 2^10, cancelling 3x3s), and 122-124
 * read diffuse, specular and attenuation bytes the same way (4800). All of
 * them fit this, in the vertex path's arithmetic (ge_mul, ge_sum):
 *  - The view matrix does not touch the vertex, normal, light or spot
 *    direction: with W identity and V rotated and translated by up to 2^10,
 *    lighting in world space fits all 904 search points and V (W v) 167.
 *  - L for a point or spot light is -(W3 m + (t - p)), one row sum, t the
 *    world translation and t - p cut to 16 bits first, m the model-space
 *    vertex. The world position is never cut on its own: forming W m first
 *    and subtracting fits 114 of 600 world-matrix points, this all of them,
 *    translations of 2^10 included. A directional light's L is its position
 *    field as given.
 *  - N is W3 n, each row one sum; D is the spot direction as given.
 *  - The eye's direction is the view matrix's third row, normalised
 *    component by component; H is L + that. (0, 0, 1) there fits 1 of 1600
 *    specular bytes under rotated views; this fits them all. For a view that
 *    is identity it is (0, 0, 1), as sets 18 and 19 had it.
 *  - The attenuation's quadratic term is k2 (L.L), not (k2 d) d: one of
 *    3000 attenuation bytes tells them apart, and it reads k2 (L.L). */
/* What light_vertex needs that does not change within a draw, worked out once
 * per draw by light_setup: the colour bytes, the eye's direction, and for a
 * directional light its L and H, which do not depend on the vertex. Every
 * value is the one light_vertex used to work out for each vertex, by the same
 * arithmetic. */
static struct {
    int emissive[3], global_amb[3], mat_amb[3], mat_dif[3], mat_spc[3];
    double E[3];
    ge_sp W[9];                         /* the world matrix's 3x3, split for ge_mul */
    float spec_k;                       /* the specular coefficient, cut to 5 bits */
    struct {
        int amb[3], dif[3], spec[3];
        ge_sp L[3], H[3];               /* directional only, split */
        ge_sp tp[3];                    /* point and spot: t - p, one GE sum, split */
        ge_sp D[3];                     /* spot: its direction, split */
        double d_rs;                    /* and 1/sqrt(D.D) */
        int d_ok;                       /* D.D > 0 */
        float spot_k;                   /* the spot exponent, cut to 5 bits */
    } light[4];
} g_ls;

static void light_setup(void) {
    for (int k = 0; k < 3; k++) {
        g_ls.emissive[k]   = lit_colour_byte(g_tl.mat_emissive[k]);
        g_ls.global_amb[k] = lit_colour_byte(g_tl.global_amb[k]);
        g_ls.mat_amb[k]    = lit_colour_byte(g_tl.mat_ambient[k]);
        g_ls.mat_dif[k]    = lit_colour_byte(g_tl.mat_diffuse[k]);
        g_ls.mat_spc[k]    = lit_colour_byte(g_tl.mat_specular[k]);
    }
    if (!any_light_enabled()) return;
    const double Ev[3] = { g_tl.view[2], g_tl.view[5], g_tl.view[8] };
    gl_unit(Ev, g_ls.E);
    g_ls.spec_k = (float)ge_cut(g_tl.mat_spec_coef, 5, 0);
    const float *W = g_tl.world;
    for (int k = 0; k < 9; k++) g_ls.W[k] = ge_sp_of(W[k]);
    for (int i = 0; i < 4; i++) {
        if (!g_tl.light[i].enable) continue;
        for (int k = 0; k < 3; k++) {
            g_ls.light[i].amb[k]  = lit_colour_byte(g_tl.light[i].amb[k]);
            g_ls.light[i].dif[k]  = lit_colour_byte(g_tl.light[i].dif[k]);
            g_ls.light[i].spec[k] = lit_colour_byte(g_tl.light[i].spec[k]);
        }
        if (g_tl.light[i].type == 0) {
            const double Lv[3] = { g_tl.light[i].pos[0], g_tl.light[i].pos[1], g_tl.light[i].pos[2] };
            double L[3], Hv[3], H[3];
            gl_unit(Lv, L);
            for (int k = 0; k < 3; k++) {
                const ge_term h[2] = { ge_mul(L[k], 1.0), ge_mul(g_ls.E[k], 1.0) };
                Hv[k] = ge_sum(h, 2);
            }
            gl_unit(Hv, H);
            for (int k = 0; k < 3; k++) {
                g_ls.light[i].L[k] = ge_sp_of(L[k]);
                g_ls.light[i].H[k] = ge_sp_of(H[k]);
            }
        } else {
            for (int k = 0; k < 3; k++) {
                const ge_term tp[2] = { ge_mul(W[9 + k], 1.0), ge_mul(g_tl.light[i].pos[k], -1.0) };
                g_ls.light[i].tp[k] = ge_sp_of(ge_sum(tp, 2));
            }
        }
        if (g_tl.light[i].type == 2) {
            const double D[3] = { g_tl.light[i].dir[0], g_tl.light[i].dir[1], g_tl.light[i].dir[2] };
            for (int k = 0; k < 3; k++) g_ls.light[i].D[k] = ge_sp_of(D[k]);
            const double dd = gl_dot3(D, D);
            g_ls.light[i].d_ok = dd > 0.0;
            g_ls.light[i].d_rs = g_ls.light[i].d_ok ? ge_rsqrt16(dd) : 0.0;
            g_ls.light[i].spot_k = (float)ge_cut(g_tl.light[i].exponent, 5, 0);
        }
    }
}

/* gl_dot3 of split vectors. */
static inline double gl_dot3_sp(const ge_sp a[3], const ge_sp b[3]) {
    const ge_term t[3] = { ge_mul_sp(&a[0], &b[0]), ge_mul_sp(&a[1], &b[1]), ge_mul_sp(&a[2], &b[2]) };
    return ge_sum(t, 3);
}

/* a . u, a taken as given and its 1/sqrt applied to the dot product, with
 * a's half worked out already: whether a.a > 0 (ok), and its 1/sqrt (rs). */
static double gl_dot_pre(int ok, double rs, const ge_sp a[3], const ge_sp u[3]) {
    return ok ? gl_mul(gl_dot3_sp(a, u), rs) : 0.0;
}

/* x^k for the lighting, k already cut to 5 significant bits (ge_cut(k, 5, 0)). */
static float gl_pow_k(double x, float k) { return x > 0.0 ? ge_pow((float)x, k) : 0.0f; }

static void light_vertex(const float model[3], const float nm[3], uint32_t *rgba, psp_vertex *lit) {
    const int vc[3] = { (int)(*rgba & 0xFFu), (int)((*rgba >> 8) & 0xFFu), (int)((*rgba >> 16) & 0xFFu) };
    /* MATERIAL_COLOR picks which material components the vertex colour
     * supplies: bit 0 ambient, bit 1 diffuse, bit 2 specular. */
    int m_amb[3], m_dif[3], m_spc[3];
    for (int k = 0; k < 3; k++) {
        m_amb[k] = (g_tl.mat_update & 1) ? vc[k] : g_ls.mat_amb[k];
        m_dif[k] = (g_tl.mat_update & 2) ? vc[k] : g_ls.mat_dif[k];
        m_spc[k] = (g_tl.mat_update & 4) ? vc[k] : g_ls.mat_spc[k];
    }

    int out[3], sec[3] = { 0, 0, 0 };
    for (int k = 0; k < 3; k++)
        out[k] = g_ls.emissive[k] + lit_mul(g_ls.global_amb[k], m_amb[k]);

    /* With every light disabled the colour is emissive plus ambient and the
     * normal never enters: skip the loop. Same result. */
    if (any_light_enabled()) {
    const ge_sp *W = g_ls.W;                     /* row i, column j at W[3j + i] */
    const ge_sp N[3] = { ge_sp_of(nm[0]), ge_sp_of(nm[1]), ge_sp_of(nm[2]) };
    const ge_sp M[3] = { ge_sp_of(model[0]), ge_sp_of(model[1]), ge_sp_of(model[2]) };
    const ge_sp one = ge_sp_of(1.0);
    double n[3];
    for (int i = 0; i < 3; i++) {
        const ge_term t[3] = { ge_mul_sp(&W[i], &N[0]), ge_mul_sp(&W[3 + i], &N[1]), ge_mul_sp(&W[6 + i], &N[2]) };
        n[i] = ge_sum(t, 3);
    }
    /* N's half of every N.L and N.H below (gl_dot_pre). */
    const ge_sp Ns[3] = { ge_sp_of(n[0]), ge_sp_of(n[1]), ge_sp_of(n[2]) };
    const double nn = gl_dot3_sp(Ns, Ns);
    const int n_ok = nn > 0.0;
    const double n_rs = n_ok ? ge_rsqrt16(nn) : 0.0;
    for (int i = 0; i < 4; i++) {
        if (!g_tl.light[i].enable) continue;
        double Lv[3], L[3];
        ge_sp Ls[3];
        const ge_sp *Lp = Ls;
        float att = 1.0f, spot = 1.0f;
        if (g_tl.light[i].type == 0) {
            Lp = g_ls.light[i].L;
        } else {
            for (int k = 0; k < 3; k++) {
                const ge_term t[4] = { ge_mul_sp(&W[k], &M[0]), ge_mul_sp(&W[3 + k], &M[1]),
                                       ge_mul_sp(&W[6 + k], &M[2]), ge_mul_sp(&g_ls.light[i].tp[k], &one) };
                Lv[k] = -ge_sum(t, 4);
            }
            const double ll = gl_dot3(Lv, Lv);
            const double d = gl_mul(ll, ge_rsqrt16(ll));
            const ge_term at[3] = { ge_mul(g_tl.light[i].atten[0], 1.0), ge_mul(g_tl.light[i].atten[1], d),
                                    ge_mul(g_tl.light[i].atten[2], ll) };
            const double a = ge_sum(at, 3);
            att = a != 0.0 ? (float)ge_rcp16(a) : 1.0f;
            gl_unit(Lv, L);
            for (int k = 0; k < 3; k++) Ls[k] = ge_sp_of(L[k]);
        }

        if (g_tl.light[i].type == 2) {
            const double sdot = gl_dot_pre(g_ls.light[i].d_ok, g_ls.light[i].d_rs, g_ls.light[i].D, Lp);
            if (!(sdot >= g_tl.light[i].cutoff)) continue;
            spot = gl_pow_k(sdot, g_ls.light[i].spot_k);
        }

        const double ndl = gl_dot_pre(n_ok, n_rs, Ns, Lp);
        float dfac = ndl > 0.0 ? (float)ndl : 0.0f;
        if (g_tl.light[i].kind == 2 && dfac > 0.0f) dfac = gl_pow_k(dfac, g_ls.spec_k);

        float sfac = 0.0f;
        if (g_tl.light[i].kind == 1 && ndl >= 0.0) {
            ge_sp Hs[3];
            const ge_sp *H = g_ls.light[i].H;
            if (g_tl.light[i].type != 0) {
                double Hv[3], Hb[3];
                for (int k = 0; k < 3; k++) {
                    const ge_term h[2] = { ge_mul(L[k], 1.0), ge_mul(g_ls.E[k], 1.0) };
                    Hv[k] = ge_sum(h, 2);
                }
                gl_unit(Hv, Hb);
                for (int k = 0; k < 3; k++) Hs[k] = ge_sp_of(Hb[k]);
                H = Hs;
            }
            sfac = gl_pow_k(gl_dot_pre(n_ok, n_rs, Ns, H), g_ls.spec_k);
        }

        const int vd = lit_byte(dfac), vs = lit_byte(sfac);
        const int va = att >= 1.0f ? 255 : lit_byte(att), vsp = spot >= 1.0f ? 255 : lit_byte(spot);
        for (int k = 0; k < 3; k++) {
            const int t = lit_mul(g_ls.light[i].amb[k], m_amb[k])
                        + lit_mul(vd, lit_mul(g_ls.light[i].dif[k], m_dif[k]));
            const int ts = lit_mul(vs, lit_mul(g_ls.light[i].spec[k], m_spc[k]));
            out[k] += lit_mul(vsp, lit_mul(va, t));
            sec[k] += lit_mul(vsp, lit_mul(va, ts));
        }
    }

    }
    /* The alpha comes from the material, or from the vertex when the vertex
     * is supplying the ambient. */
    uint32_t c = (g_tl.mat_update & 1) ? (*rgba & 0xFF000000u)
                                       : ((uint32_t)(g_tl.mat_alpha & 0xFF) << 24);
    uint32_t s = 0;
    for (int k = 0; k < 3; k++) {
        if (!g_tl.light_mode) { out[k] += sec[k]; sec[k] = 0; }
        c |= (uint32_t)(out[k] > 255 ? 255 : out[k]) << (8 * k);
        s |= (uint32_t)(sec[k] > 255 ? 255 : sec[k]) << (8 * k);
    }
    *rgba = c;
    lit->spec = s;
    lit->spec_set = s != 0;
}

enum { GE_VERTEX_BATCH = 256 };

/* Keep independent primitives whole when a draw is larger than the stack
 * buffer.  A triangle list cannot be cut at 256: the backend consumes triples,
 * so it draws 255 vertices, drops vertex 255, and starts the next batch at
 * vertex 256.  The game's long indexed text draws exposed this exactly after
 * 42 glyph quads (252 vertices).  The final batch may contain an incomplete
 * primitive because the GE count itself may; only intermediate boundaries
 * must be aligned so a later complete primitive is not shifted. */
static uint32_t primitive_batch_count(uint32_t type, uint32_t remaining) {
    if (remaining <= GE_VERTEX_BATCH) return remaining;

    uint32_t multiple = 1;
    if (type == PSP_PRIM_TRIANGLES) multiple = 3;
    else if (type == PSP_PRIM_LINES || type == PSP_PRIM_SPRITES) multiple = 2;
    return GE_VERTEX_BATCH - GE_VERTEX_BATCH % multiple;
}

/* What draw_model receives alongside the vertices: g_tl and the lights in
 * eye space, in the backend's own struct. */
static void fill_xform_state(psp_xform_state *xs) {
    /* Every member is assigned below; only padding would have been cleared. */
    memcpy(xs->world, g_tl.world, sizeof xs->world);
    memcpy(xs->view,  g_tl.view,  sizeof xs->view);
    memcpy(xs->proj,  g_tl.proj,  sizeof xs->proj);
    memcpy(xs->tgen,  g_tl.tgen,  sizeof xs->tgen);
    xs->lighting = g_tl.lighting; xs->mat_update = g_tl.mat_update; xs->mat_alpha = g_tl.mat_alpha;
    for (int i = 0; i < 4; i++) {
        xs->light[i].enable = g_tl.light[i].enable;
        xs->light[i].type = g_tl.light[i].type;
        xs->light[i].kind = g_tl.light[i].kind;
        memcpy(xs->light[i].pos, g_light_eye[i].pos, sizeof xs->light[i].pos);
        memcpy(xs->light[i].dir, g_light_eye[i].dir, sizeof xs->light[i].dir);
        memcpy(xs->light[i].atten, g_tl.light[i].atten, sizeof xs->light[i].atten);
        xs->light[i].exponent = g_tl.light[i].exponent; xs->light[i].cutoff = g_tl.light[i].cutoff;
        memcpy(xs->light[i].amb, g_tl.light[i].amb, sizeof xs->light[i].amb);
        memcpy(xs->light[i].dif, g_tl.light[i].dif, sizeof xs->light[i].dif);
        memcpy(xs->light[i].spec, g_tl.light[i].spec, sizeof xs->light[i].spec);
    }
    memcpy(xs->mat_emissive, g_tl.mat_emissive, sizeof xs->mat_emissive);
    memcpy(xs->mat_ambient,  g_tl.mat_ambient,  sizeof xs->mat_ambient);
    memcpy(xs->mat_diffuse,  g_tl.mat_diffuse,  sizeof xs->mat_diffuse);
    memcpy(xs->mat_specular, g_tl.mat_specular, sizeof xs->mat_specular);
    xs->mat_spec_coef = g_tl.mat_spec_coef;
    memcpy(xs->global_amb, g_tl.global_amb, sizeof xs->global_amb);
    xs->fog_enable = g_tl.fog_enable; xs->fog_end = g_tl.fog_end; xs->fog_range = g_tl.fog_range;
    xs->vp_set = g_tl.vp_set;
    xs->vp_xs = g_tl.vp_xs; xs->vp_ys = g_tl.vp_ys; xs->vp_zs = g_tl.vp_zs;
    xs->vp_xc = g_tl.vp_xc; xs->vp_yc = g_tl.vp_yc; xs->vp_zc = g_tl.vp_zc;
    xs->off_x = g_tl.off_x; xs->off_y = g_tl.off_y;
    xs->depth_clamp = g_tl.depth_clamp; xs->cull_enable = g_tl.cull_enable; xs->cull_ccw = g_tl.cull_ccw;
    xs->tex_map_mode = g_tl.tex_map_mode; xs->tex_proj_mode = g_tl.tex_proj_mode;
    xs->tex_w = (int)g_ge.tex_w; xs->tex_h = (int)g_ge.tex_h;
}

/* A triangle straight from the vertex batch. When emit_tri would pass it
 * through unclipped -- every w above the vertex loop's own threshold, and
 * inside the near plane the way the clipper tests it (z + w >= 0 under depth
 * clamp, normalised z within range without) -- the screen-space values the
 * vertex loop already computed from the same clip coordinates are the ones
 * emit_tri would recompute, so it is drawn from them directly: no clip-vertex
 * copies, no second projection, the same guard-band and cull decisions on the
 * same numbers. Anything else takes emit_tri unchanged. */
static void emit_tri_indexed(const psp_render_backend *be, const psp_vertex *v,
                             float (*cl)[4], uint32_t i0, uint32_t i1, uint32_t i2, int flip) {
    const uint32_t idx[3] = { i0, i1, i2 };
    if (tri_beyond_one_plane(cl[i0], cl[i1], cl[i2])) return;
    int fast = 1;
    for (int k = 0; k < 3 && fast; k++) {
        const float *c = cl[idx[k]];
        if (!(c[3] > 1e-6f)) { fast = 0; break; }
        if (g_tl.depth_clamp) { if (!(c[2] + c[3] >= 0.0f)) fast = 0; }
        else { const float nz = c[2] / c[3]; if (!(nz >= -1.0f && nz <= 1.0f)) fast = 0; }
    }
    if (!fast) {
        clipvert tri[3];
        for (int k = 0; k < 3; k++) { tri[k].v = v[idx[k]]; memcpy(tri[k].c, cl[idx[k]], sizeof tri[k].c); }
        emit_tri(be, tri, flip);
        return;
    }
    const float ox = g_tl.vp_set ? g_tl.off_x : 1808.0f;
    const float oy = g_tl.vp_set ? g_tl.off_y : 1912.0f;
    psp_vertex t[3];
    for (int k = 0; k < 3; k++) {
        const psp_vertex *sv = &v[idx[k]];
        const float sx = sv->precise_x, sy = sv->precise_y;
        if (sx < -ox || sx >= 4096.0f - ox || sy < -oy || sy >= 4096.0f - oy) { g_clip_guard += 3; return; }
        t[k] = *sv;
        if (g_tl.depth_clamp) {
            if (t[k].z < 0.0f) t[k].z = 0.0f;
            if (t[k].z > 65535.0f) t[k].z = 65535.0f;
        }
    }
    const long ax = t[1].x - t[0].x, ay = t[1].y - t[0].y;
    const long bx = t[2].x - t[0].x, by = t[2].y - t[0].y;
    long area = ax * by - ay * bx;
    if (flip) area = -area;
    if (g_tl.cull_enable && area != 0 && ((area < 0) == (g_tl.cull_ccw != 0))) { g_culled += 3; return; }
    be->draw(PSP_PRIM_TRIANGLES, t, 3);
}

static void draw_prim_transformed(uint32_t type, uint32_t count,
                                  int col_off, int pos_off, int tex_off,
                                  int norm_off, int stride) {
    psp_vertex v[GE_VERTEX_BATCH];
    float      cl[GE_VERTEX_BATCH][4];
    const psp_render_backend *be = psp_render_current();
    /* Perspective projection has a model-dependent clip W (the game's usual
     * matrix has m[11] = -1, m[15] = 0). Menus and HUD use an orthographic
     * matrix whose clip W is constant. Carry that distinction past the shared
     * transform so an aspect-aware host backend can keep the latter in a
     * native-aspect safe area without double-correcting the wider camera. */
    const int screen_space = fabsf(g_tl.proj[3])  < 1e-6f &&
                             fabsf(g_tl.proj[7])  < 1e-6f &&
                             fabsf(g_tl.proj[11]) < 1e-6f &&
                             fabsf(g_tl.proj[15]) > 1e-6f;
    const uint64_t _x0 = ge_prof_now();
    if (g_tl.lighting) { lights_to_eye(); light_setup(); }
    const int any_light = g_tl.lighting && any_light_enabled();

    /* The backend's transform, when it offers one. Triangles only (points,
     * lines and sprites stay here), and not screen-space projections, whose
     * flag the GL backend reads to tell HUD from scene. */
    const int blended = g_mv_src || g_vl.w_fmt || g_vl.morph_n > 1;
    if (be->draw_model && be->model_ok && !screen_space && !blended &&
        (type == PSP_PRIM_TRIANGLES || type == PSP_PRIM_TRIANGLE_STRIP ||
         type == PSP_PRIM_TRIANGLE_FAN) && be->model_ok()) {
        psp_xform_state xs;
        fill_xform_state(&xs);
        if (g_prof_on > 0) g_prof_m[5] += ge_prof_now() - _x0;
        psp_model_vertex mv[GE_VERTEX_BATCH], centre;
        uint32_t mdone = 0;
        while (mdone < count) {
            /* A fan's later batches start again from its first vertex (see
             * draw_prim): one fewer read, then the centre in front. */
            const uint32_t lead = type == PSP_PRIM_TRIANGLE_FAN && mdone > 0;
            const uint32_t n = primitive_batch_count(type, count - mdone + lead) - lead;
            uint32_t decoded = 0;
            const uint64_t _m0 = ge_prof_now();
            /* One translation for the batch's vertices and one for its
             * indices, in place of two per vertex: the bytes are the same,
             * and a batch the flat map cannot hand over whole (an index off
             * the end of memory, a range straddling a region) falls back to
             * the per-vertex lookups, which say what they always said. */
            const uint32_t vtype = g_ge.vtype;
            const int isz = VT_INDEX(vtype) == 1 ? 1 : VT_INDEX(vtype) == 2 ? 2 : 0;
            const uint8_t *vspan = NULL, *ispan = NULL;
            uint32_t vspan_addr = 0;
            if (!isz) {
                vspan_addr = g_ge.vaddr + mdone * (uint32_t)stride;
                vspan = (const uint8_t *)psp_mem_ptr(vspan_addr, n * (uint32_t)stride);
            } else {
                ispan = (const uint8_t *)psp_mem_ptr(g_ge.iaddr + mdone * (uint32_t)isz, n * (uint32_t)isz);
                if (ispan) {
                    uint32_t hi = 0;
                    for (uint32_t k = 0; k < n; k++) {
                        const uint32_t ix = isz == 1 ? ispan[k] : (uint32_t)ispan[2 * k] | ((uint32_t)ispan[2 * k + 1] << 8);
                        if (ix > hi) hi = ix;
                    }
                    vspan_addr = g_ge.vaddr;
                    vspan = (const uint8_t *)psp_mem_ptr(g_ge.vaddr, (hi + 1) * (uint32_t)stride);
                }
            }
            const uint32_t base_colour = current_colour();
            const int vertex_colour = col_off >= 0 && VT_COLOR(vtype) >= 4;
            for (; decoded < n; decoded++) {
                uint32_t a;
                const uint8_t *vp;
                if (!isz) {
                    a = vspan_addr + decoded * (uint32_t)stride;
                    vp = vspan ? vspan + decoded * (uint32_t)stride : (const uint8_t *)psp_mem_ptr(a, (uint32_t)stride);
                } else if (ispan && vspan) {
                    const uint32_t ix = isz == 1 ? ispan[decoded] : (uint32_t)ispan[2 * decoded] | ((uint32_t)ispan[2 * decoded + 1] << 8);
                    a = vspan_addr + ix * (uint32_t)stride;
                    vp = vspan + ix * (uint32_t)stride;
                } else {
                    a = vertex_addr(mdone + decoded, stride);
                    vp = (const uint8_t *)psp_mem_ptr(a, (uint32_t)stride);
                }
                psp_model_vertex *m = &mv[decoded];
                if (!read_pos_model_at(vp, a, vtype, pos_off, m->pos)) break;
                m->rgba = base_colour;
                if (vertex_colour) m->rgba = read_colour_at(vp, a, vtype, col_off);
                if (any_light) read_normal_model_at(vp, a, vtype, norm_off, m->nrm);
                else { m->nrm[0] = m->nrm[1] = 0.0f; m->nrm[2] = 1.0f; }
                psp_vertex uv;
                read_uv_model_at(vp, a, vtype, tex_off, &uv);
                m->u = uv.u; m->v = uv.v;
                note_colour(m->rgba);
            }
            if (g_tl.fog_enable) g_fog_verts += decoded;
            if (g_tl.lighting)   g_lit_verts += decoded;
            if (lead && decoded) {
                memmove(mv + 1, mv, decoded * sizeof *mv);
                mv[0] = centre;
                decoded++;
            } else if (type == PSP_PRIM_TRIANGLE_FAN && decoded) centre = mv[0];
            const uint64_t _m1 = ge_prof_now();
            be->draw_model((int)type, mv, (int)decoded, &xs);
            if (g_prof_on > 0) { const uint64_t _m2 = ge_prof_now(); g_prof_m[0] += _m1 - _m0; g_prof_m[1] += _m2 - _m1; }
            g_model_verts += decoded;
            if (type == PSP_PRIM_TRIANGLE_STRIP && decoded == GE_VERTEX_BATCH && mdone + decoded < count)
                mdone += decoded - 2;
            else if (type == PSP_PRIM_TRIANGLE_FAN && decoded == GE_VERTEX_BATCH && mdone + decoded - lead < count)
                mdone += decoded - lead - 1;
            else if (decoded <= lead)
                break;
            else
                mdone += decoded - lead;
        }
        return;
    }

    double wvp[4][4];
    ge_wvp(wvp);
    ge_sp wsp[4][4];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) wsp[i][j] = ge_sp_of(wvp[i][j]);
    uint32_t done = 0;
    psp_vertex centre_v;
    float centre_cl[4];
    while (done < count) {
        /* A fan's later batches start again from its first vertex (see
         * draw_prim): one fewer read, then the centre in front. */
        const uint32_t lead = type == PSP_PRIM_TRIANGLE_FAN && done > 0;
        const uint32_t n = primitive_batch_count(type, count - done + lead) - lead;

        uint32_t decoded = 0;
        for (; decoded < n; decoded++) {
            float model[3], world[3], eye[3], clip[4];
            const uint64_t _p0 = ge_prof_now();
            /* A skinned, morphed or tessellated vertex arrives whole in mv;
             * a plain one is read field by field below, as before. */
            ge_mvert mv;
            uint32_t a = 0;
            const uint8_t *vp = NULL;
            if (g_mv_src) {
                mv = g_mv_src[done + decoded];
                memcpy(model, mv.pos, sizeof model);
            } else {
                a = vertex_addr(done + decoded, stride);
                if (blended) {
                    if (!read_mvert(a, g_ge.vtype, col_off, pos_off, tex_off, norm_off, any_light, &mv)) break;
                    memcpy(model, mv.pos, sizeof model);
                } else {
                    vp = (const uint8_t *)psp_mem_ptr(a, (uint32_t)stride);
                    if (!read_pos_model_at(vp, a, g_ge.vtype, pos_off, model)) break;
                }
            }
            const uint64_t _p1 = ge_prof_now();

            /* The eye position only feeds the fog. */
            if (g_tl.fog_enable) {
                mul_4x3(g_tl.world, model, world);
                mul_4x3(g_tl.view,  world, eye);
            }
            /* Clip space as the GE forms it (ge_clip), for depth
             * (ge_screen_z) and position (clip_to_fx16). */
            ge_clip_sp(wsp, model, clip);
            const uint64_t _p2 = ge_prof_now();

            psp_vertex *o = &v[decoded];
            o->screen_space = screen_space;
            o->spec = 0; o->spec_set = 0;
            o->rgba = current_colour();
            o->tex_q = 1.0f;
            if (blended) o->rgba = mv.rgba;
            else if (col_off >= 0 && VT_COLOR(g_ge.vtype) >= 4) o->rgba = read_colour_at(vp, a, g_ge.vtype, col_off);
            /* Fog. The coefficient is (end - depth) * range with depth the
             * eye-space distance -- w of the clip position for a standard
             * projection, -z of the eye position here -- clamped to 0..1 and
             * quantised to the hardware's byte, 255 unfogged: floor(f * 256),
             * clamped. geprobe 2 scene 15 (fw 6.60) fogs six quads at f of
             * 0.861 ... 0.028 and reads 220, 177, 135, 92, 49 and 7, where
             * rounding f * 255 gives 50 for the fifth. gpu/commands/fog
             * fixes the rest: near == far makes sceGuFog's range 1/0, and the
             * hardware reads the infinite product as fully fogged, whichever
             * sign ("Basic" and "Both neg" both read the fog colour); "Near
             * neg" at depth 0 with end 1 and range 0.5 reads half. */
            o->fog = 255;
            if (g_tl.fog_enable) {
                const float f = (g_tl.fog_end + eye[2]) * g_tl.fog_range;
                if (!isfinite(f) || f <= 0.0f) o->fog = 0;
                else if (f < 1.0f)             o->fog = (int)(f * 256.0f) > 255 ? 255 : (int)(f * 256.0f);
                g_fog_verts++;
            }
            if (g_tl.lighting) {
                float nm[3] = { 0.0f, 0.0f, 1.0f };
                if (any_light) {
                    if (blended) memcpy(nm, mv.nrm, sizeof nm);
                    else read_normal_model_at(vp, a, g_ge.vtype, norm_off, nm);
                }
                light_vertex(model, nm, &o->rgba, o);
                g_lit_verts++;
            }
            const uint64_t _p3 = ge_prof_now();
            note_colour(o->rgba);
            if (blended) { o->u = mv.u; o->v = mv.v; }
            else read_uv_model_at(vp, a, g_ge.vtype, tex_off, o);
            if (g_tl.tex_map_mode == 1) {
                /* The source row the matrix is applied to. GU_POSITION is the
                 * model-space position, GU_UV the vertex's own coordinates,
                 * and the two normal modes the normal -- which this decoder
                 * does not read, so they fall back to the position rather than
                 * to nothing. */
                float src[3];
                if (g_tl.tex_proj_mode == 1) {
                    src[0] = o->u / (float)(g_ge.tex_w ? g_ge.tex_w : 1);
                    src[1] = o->v / (float)(g_ge.tex_h ? g_ge.tex_h : 1);
                    src[2] = 0.0f;
                } else {
                    src[0] = model[0]; src[1] = model[1]; src[2] = model[2];
                }
                float gen[3];
                mul_4x3(g_tl.tgen, src, gen);
                o->u = gen[0] * (float)g_ge.tex_w;
                o->v = gen[1] * (float)g_ge.tex_h;
                o->tex_q = gen[2];
                note_uv(o->u, o->v);
            }

            const uint64_t _p4 = ge_prof_now();
            memcpy(cl[decoded], clip, sizeof clip);
            float sx, sy, sz;
            /* Onto the 1/16 grid, as screen_axis_fx16 explains. */
            if (clip[3] > 1e-6f) clip_to_screen(clip, &sx, &sy, &sz, &o->x, &o->y);
            else { sx = sy = sz = 0.0f; o->x = o->y = 0; }
            o->precise_x = sx; o->precise_y = sy; o->precise = 1;
            o->z = sz;
            o->inv_w = clip[3] > 1e-6f ? 1.0f / clip[3] : 1.0f;
            if (clip[3] > 1e-6f) {
                if (!g_tl.bb_seen) { g_tl.bb_x0 = g_tl.bb_x1 = sx;
                                     g_tl.bb_y0 = g_tl.bb_y1 = sy; g_tl.bb_seen = 1; }
                if (sx < g_tl.bb_x0) g_tl.bb_x0 = sx;
                if (sx > g_tl.bb_x1) g_tl.bb_x1 = sx;
                if (sy < g_tl.bb_y0) g_tl.bb_y0 = sy;
                if (sy > g_tl.bb_y1) g_tl.bb_y1 = sy;
            }
                    if (g_prof_on > 0) {
                const uint64_t _p5 = ge_prof_now();
                g_prof[0] += _p1 - _p0; g_prof[1] += _p2 - _p1; g_prof[2] += _p3 - _p2;
                g_prof[3] += _p4 - _p3; g_prof[4] += _p5 - _p4; g_prof_verts++;
            }
        }
        if (!decoded) break;
        g_xformed += decoded;
        if (lead) {
            memmove(v + 1, v, decoded * sizeof *v);
            memmove(cl + 1, cl, decoded * sizeof *cl);
            v[0] = centre_v;
            memcpy(cl[0], centre_cl, sizeof centre_cl);
            decoded++;
        } else if (type == PSP_PRIM_TRIANGLE_FAN) {
            centre_v = v[0];
            memcpy(centre_cl, cl[0], sizeof centre_cl);
        }

        if (texdraw_hit() || wilduv_hit(v, decoded)) texdraw_dump(PRIM_NAME[type & 7], v, decoded);

        if (drawlog_left()) {
            int x0 = v[0].x, x1 = v[0].x, y0 = v[0].y, y1 = v[0].y;
            for (uint32_t i = 1; i < decoded; i++) {
                if (v[i].x < x0) x0 = v[i].x;   if (v[i].x > x1) x1 = v[i].x;
                if (v[i].y < y0) y0 = v[i].y;   if (v[i].y > y1) y1 = v[i].y;
            }
            fprintf(stderr, "draw: %-14s %2u verts  x %4d..%-4d y %4d..%-4d  "
                            "fbp %08X  vaddr %08X  rgba %08X  vtype %06X  z %d/%d/%d clr %d  tex %s %08X %ux%u fmt %u filt %u/%u uv %.1f,%.1f..%.1f,%.1f\n",
                    PRIM_NAME[type & 7], decoded, x0 >> 4, x1 >> 4, y0 >> 4, y1 >> 4,
                    ge_fb_address(g_ge.fbp), g_ge.vaddr, v[0].rgba,
                    g_ge.vtype, g_tl.ztest_enable, g_tl.ztest_func, !g_tl.zwrite_off, g_tl.clear_mode, (g_ge.tex_enable && tex_off >= 0 && g_ge.tex_addr)
                                ? "yes" : "no", g_ge.tex_addr, g_ge.tex_w, g_ge.tex_h, g_ge.tex_format, (unsigned)(g_ge.tex_filter & 7), (unsigned)((g_ge.tex_filter >> 8) & 1), (double)v[0].u, (double)v[0].v, (double)v[decoded-1].u, (double)v[decoded-1].v);
            fprintf(stderr, "      cols");
            for (uint32_t i = 0; i < decoded && i < 4; i++)
                fprintf(stderr, " %08X", v[i].rgba);
            fprintf(stderr, "   blend %s src %d dst %d eq %d  atest %s func %d ref %d"
                            "  clear %s\n",
                    g_tl.blend.enable ? "on" : "off", g_tl.blend.src, g_tl.blend.dst,
                    g_tl.blend.eq, g_tl.blend.alpha_test ? "on" : "off",
                    g_tl.blend.alpha_func, g_tl.blend.alpha_ref,
                    g_tl.clear_mode ? "ON" : "off");
            fprintf(stderr, "      fbp %08X fbw %u  vp scale %.1f,%.1f centre %.1f,%.1f"
                            "  offset %.1f,%.1f%s\n      world",
                    ge_fb_address(g_ge.fbp), g_ge.fbw,
                    g_tl.vp_xs, g_tl.vp_ys, g_tl.vp_xc, g_tl.vp_yc,
                    g_tl.off_x, g_tl.off_y, g_tl.vp_set ? "" : " (defaulted)");
            for (int i = 0; i < 12; i++) fprintf(stderr, " %.2f", g_tl.world[i]);
            fprintf(stderr, "\n      view ");
            for (int i = 0; i < 12; i++) fprintf(stderr, " %.2f", g_tl.view[i]);
            fprintf(stderr, "\n      proj ");
            for (int i = 0; i < 16; i++) fprintf(stderr, " %.3f", g_tl.proj[i]);
            fprintf(stderr, "\n      raw v0@%08X:", g_ge.vaddr + done * (uint32_t)stride);
            for (int b = 0; b < stride && b < 32; b++)
                fprintf(stderr, "%02X", psp_read8(g_ge.vaddr + done * (uint32_t)stride + (uint32_t)b));
            fprintf(stderr, "  stride %d tex_off %d col_off %d pos_off %d",
                    stride, tex_off, col_off, pos_off);
            fprintf(stderr, "\n      model v0");
            {
                float m[3];
                for (uint32_t i = 0; i < decoded && i < 4; i++) {
                    read_pos_model(g_ge.vaddr + (done + i) * (uint32_t)stride,
                                   g_ge.vtype, pos_off, m);
                    fprintf(stderr, "  (%.1f,%.1f,%.1f)", m[0], m[1], m[2]);
                }
            }
            fprintf(stderr, "\n");
        }

        /* Emit primitive by primitive rather than handing the backend the
         * batch: near-plane rejection and culling are per-primitive decisions,
         * and a batch cannot express "all but this one". */
        const uint64_t _pe = ge_prof_now();
        const int step = (type == PSP_PRIM_TRIANGLE_STRIP ||
                          type == PSP_PRIM_TRIANGLE_FAN) ? 1 : 3;
        if (type == PSP_PRIM_TRIANGLES || type == PSP_PRIM_TRIANGLE_STRIP ||
            type == PSP_PRIM_TRIANGLE_FAN) {
            for (uint32_t i = 2; i < decoded; i += (uint32_t)step) {
                uint32_t i0 = (type == PSP_PRIM_TRIANGLE_FAN) ? 0 : i - 2;
                uint32_t i1 = i - 1, i2 = i;
                if (type == PSP_PRIM_TRIANGLES) { i0 = i - 2; i1 = i - 1; i2 = i; }

                /* A strip alternates winding, so every second triangle flips
                 * -- ignoring that culls exactly half of every strip. */
                emit_tri_indexed(be, v, cl, i0, i1, i2, type == PSP_PRIM_TRIANGLE_STRIP && ((i - 2) & 1));
            }
        } else if (type <= PSP_PRIM_LINE_STRIP) {
            const uint32_t size = type == PSP_PRIM_POINTS ? 1 : 2;
            const uint32_t advance = type == PSP_PRIM_LINES ? 2 : 1;
            for (uint32_t i = 0; i + size <= decoded; i += advance) {
                clipvert p[2];
                for (uint32_t j = 0; j < size; j++) {
                    p[j].v = v[i+j]; memcpy(p[j].c, cl[i+j], sizeof p[j].c);
                }
                emit_point_line(be, p, (int)size);
            }
        } else {
            be->draw((int)type, v, (int)decoded);
        }

        if (g_prof_on > 0) { g_prof[5] += ge_prof_now() - _pe; g_prof_batches++; }
        if (type == PSP_PRIM_TRIANGLE_STRIP && decoded == GE_VERTEX_BATCH && done + decoded < count)
            done += decoded - 2;
        else if (type == PSP_PRIM_TRIANGLE_FAN && decoded == GE_VERTEX_BATCH && done + decoded - lead < count)
            done += decoded - lead - 1;
        else if (type == PSP_PRIM_LINE_STRIP && decoded == GE_VERTEX_BATCH && done + decoded < count)
            done += decoded - 1;
        else
            done += decoded - lead;
    }
}

/* The backend state a draw depends on, pushed at draw time rather than on
 * every register write: the GE sets these in any order and only their value
 * at the draw matters. Shared by the vertex-array draws and the immediate
 * ones. */
static void push_texture_state(int has_uv) {
    psp_render_current()->set_clut(g_ge.clut_addr, (int)(g_ge.clut_raw & 3),
                                   (int)((g_ge.clut_raw >> 2) & 0x1F),
                                   (int)((g_ge.clut_raw >> 8) & 0xFF),
                                   (int)(((g_ge.clut_raw >> 16) & 0x1F) << 4));
    {
        /* The two filter fields are handed over raw. Which one applies depends
         * on the pixel-to-texel scale, which only the rasterizer can work out,
         * so picking one here would be the interpreter guessing at a decision
         * that is not its to make. */
        psp_tex_state t = {
            .addr       = has_uv ? g_ge.tex_addr : 0,
            .stride     = g_ge.tex_stride,
            .w          = (int)g_ge.tex_w,
            .h          = (int)g_ge.tex_h,
            .fmt        = (int)g_ge.tex_format,
            .func       = (int)g_ge.tex_func,
            .swizzled   = (int)g_ge.tex_swizzled,
            .min_filter = (int)(g_ge.tex_filter & 7),
            .mag_filter = (int)((g_ge.tex_filter >> 8) & 7),
            .wrap_s     = (int)(g_ge.tex_wrap & 1),
            .wrap_t     = (int)((g_ge.tex_wrap >> 8) & 1),
            .tcc_rgba   = (int)g_ge.tex_tcc,
            .color_double = (int)g_ge.tex_double,
            .env        = g_ge.tex_env,
            .max_level  = g_ge.tex_max_level,
            .lod_mode   = g_ge.tex_lod_mode,
            .lod_bias16 = g_ge.tex_lod_bias16,
            .lod_slope  = g_ge.tex_lod_slope,
        };
        t.lv_addr[0] = g_ge.tex_addr; t.lv_stride[0] = g_ge.tex_stride;
        t.lv_w[0] = (int)g_ge.tex_w;  t.lv_h[0] = (int)g_ge.tex_h;
        for (int L = 1; L < 8; L++) {
            t.lv_addr[L] = g_ge.tex_lv_addr[L]; t.lv_stride[L] = g_ge.tex_lv_stride[L];
            t.lv_w[L] = (int)g_ge.tex_lv_w[L];  t.lv_h[L] = (int)g_ge.tex_lv_h[L];
        }
        if (has_uv && g_ge.tex_max_level > 0) g_draw_mip++;
        psp_render_current()->set_texture(&t);
    }
}

static void push_pixel_state_body(void);
static void push_pixel_state(void) {
    const uint64_t _s0 = ge_prof_now();
    push_pixel_state_body();
    if (g_prof_on > 0) g_prof_m[2] += ge_prof_now() - _s0;
}
static void push_pixel_state_body(void) {
    const psp_render_backend *be = psp_render_current();
    if (be->set_viewport) {
        const float hx = g_tl.vp_set ? fabsf(g_tl.vp_xs) : 240.0f;
        const float hy = g_tl.vp_set ? fabsf(g_tl.vp_ys) : 136.0f;
        const float cx = g_tl.vp_set ? g_tl.vp_xc - g_tl.off_x : 240.0f;
        const float cy = g_tl.vp_set ? g_tl.vp_yc - g_tl.off_y : 136.0f;
        be->set_viewport(cx - hx, cy - hy, 2.0f * hx, 2.0f * hy);
    }
    {
        /* Clear mode writes the clear values straight through: no blend, no
         * alpha test, or the clear would be filtered by the state it is
         * supposed to be resetting. */
        psp_blend_state b = g_tl.blend;
        b.write_colour = 1;
        b.write_alpha  = g_tl.clear_mode ? g_tl.clear_stencil : 0;
        if (g_tl.clear_mode) {
            b.enable = 0; b.alpha_test = 0; b.stencil_test = 0;
            b.write_colour = g_tl.clear_colour;
            /* What a clear keeps and drops, from geprobe 5 scenes 29-32
             * (fw 6.60), which clear one band each with a state on, on
             * 8888, 5650, 5551 and 4444: blending, the logic op (XOR) and
             * the colour and alpha tests (which the clear colour fails)
             * change nothing, but dither and the pixel mask apply as to a
             * draw. The dither matrix offsets the clear colour per pixel
             * (0x80 under the extreme matrix reads 7D 7C 7F 7F ... on the
             * first row of 8888), and PMSK1 keeps the colour bits it
             * covers and PMSK2 the stencil bits (0xAB under 0xF0 on a
             * stencil of 0 reads 0x0B, and on 5551 and 4444 the alpha
             * field keeps its old value). */
            b.colour_test = 0; b.logic_enable = 0;
        }
        psp_render_current()->set_blend(&b);
    }
    /* Clear mode bypasses the depth test as well as texturing, blending and the
     * alpha test. It is a blit of the clear values, so the comparison is forced
     * to ALWAYS -- an *enabled* test that always passes, not a disabled one --
     * and depth write comes from the clear-mode depth bit rather than ZMSK.
     * That bit is PSPSDK's: sceGuClear sends its GU_*_BUFFER_BIT flags
     * shifted into CLEAR_MODE bits 8-10, depth at bit 10. The distinction
     * matters because a disabled test writes no depth at all, so
     * encoding the clear as "test off, write on" would stop it clearing.
     *
     * An earlier revision ran the game's own test here instead, on the reasoning
     * that a clear should not overwrite geometry that rejected it. That gets the
     * dependency backwards: a clear is what *establishes* the value everything
     * else is tested against, so testing it against the values it is replacing
     * makes it a no-op exactly when it matters. This game runs GEQUAL and clears
     * to the near end, so every clear failed its own test and the depth buffer
     * was never cleared at all. */
    psp_render_current()->set_depth(
        g_tl.clear_mode ? 1 : g_tl.ztest_enable,
        g_tl.clear_mode ? 1 : g_tl.ztest_func,
        g_tl.clear_mode ? g_tl.clear_z : !g_tl.zwrite_off);
    psp_render_current()->set_fog(g_tl.fog_enable, g_tl.fog_colour_raw);
}

/* Immediate-mode vertices.
 *
 * Ten registers, one per component, and 0xF7 commits a vertex: its low byte
 * is the alpha, bits 8..10 the primitive type -- 7 meaning "the one already
 * in progress" -- and bit 22 says the fog coefficient in 0xF8 applies.
 * Positions are screen space in 12.4 fixed point on the 4096 grid, like
 * OFFSET_X/Y, so the offset is subtracted; depth is the 16-bit window value.
 * gpu/commands/fog uses this path for its 256-row rounding table and nothing
 * in the game has, so texture coordinates (0xF3..0xF5) and the specular
 * colour (0xF9) are accepted and not applied: immediate draws are untextured
 * here. Strips and fans are emitted a triangle or line at a time. */
static struct {
    uint32_t   x, y, z, rgb, fog;
    int        type, count;
    psp_vertex v[3];
} g_imm = { .type = -1 };

static void imm_vertex(uint32_t arg) {
    const int type = (int)((arg >> 8) & 7);
    if (type != 7) { g_imm.type = type; g_imm.count = 0; }
    if (g_imm.type < 0 || g_imm.type == 7) return;

    psp_vertex o;
    memset(&o, 0, sizeof o);
    o.x    = (int)g_imm.x - (int)(g_tl.off_x * 16.0f);
    o.y    = (int)g_imm.y - (int)(g_tl.off_y * 16.0f);
    o.z    = (float)(g_imm.z & 0xFFFFu);
    o.rgba = (g_imm.rgb & 0xFFFFFFu) | ((arg & 0xFFu) << 24);
    o.inv_w = o.tex_q = 1.0f;
    o.fog  = (arg & 0x400000u) ? (int)(g_imm.fog & 0xFFu) : 255;
    o.screen_space = 1;

    int need, out_type = g_imm.type;
    switch (g_imm.type) {
    case PSP_PRIM_POINTS:         need = 1; break;
    case PSP_PRIM_LINES:          need = 2; break;
    case PSP_PRIM_LINE_STRIP:     need = 2; out_type = PSP_PRIM_LINES;     break;
    case PSP_PRIM_TRIANGLES:      need = 3; break;
    case PSP_PRIM_TRIANGLE_STRIP:
    case PSP_PRIM_TRIANGLE_FAN:   need = 3; out_type = PSP_PRIM_TRIANGLES; break;
    default:                      need = 2; break;   /* sprites */
    }
    if (g_imm.count < need) g_imm.v[g_imm.count++] = o;
    else {
        /* A strip or fan already full: slide the window. */
        if (g_imm.type == PSP_PRIM_TRIANGLE_FAN) { g_imm.v[1] = g_imm.v[2]; g_imm.v[2] = o; }
        else { for (int i = 1; i < need; i++) g_imm.v[i - 1] = g_imm.v[i]; g_imm.v[need - 1] = o; }
    }
    if (g_imm.count < need) return;

    push_texture_state(0);
    push_pixel_state();
    g_imm_draws++;
    psp_render_current()->draw(out_type, g_imm.v, need);
    /* Lists start over; strips and fans keep their window for the next one. */
    if (g_imm.type == PSP_PRIM_POINTS || g_imm.type == PSP_PRIM_LINES ||
        g_imm.type == PSP_PRIM_TRIANGLES || g_imm.type == PSP_PRIM_SPRITES)
        g_imm.count = 0;
}

static void draw_prim(uint32_t type, uint32_t count) {
    /* The sampler is told the current texture at draw time rather than on every
     * state command: the GE sets these fields in any order, and only their
     * value at the draw matters. */

    if (!g_ge.vaddr) { g_skip_noaddr += count; return; }
    const uint64_t _d0 = ge_prof_now();

    int col_off = -1, pos_off = 0, tex_off = -1;
    int norm_off;
    int stride = vertex_layout(g_ge.vtype, &col_off, &pos_off, &tex_off, &norm_off);
    if (!stride) { g_skip_layout += count; return; }

    /* Bound whenever texture mapping is enabled and a texture is set --
     * whether or not these vertices carry coordinates. A previous version also
     * required texcoords in the vertex type, reasoning that geometry without
     * them would otherwise be painted with a stale texture sampled at texel
     * zero. That is exactly what hardware does: gpu/texfunc draws sprites with
     * GU_COLOR_8888 | GU_VERTEX_32BITF, no texcoords, over a solid 4x4
     * texture, and reads the texture's colour back. read_uv_model_at already
     * answers (0,0) for a vertex without them. A game that wants flat geometry
     * disables texturing, and this one does. */
    /* The texture address is complete as decoded -- unlike FBP, which is a
     * VRAM offset with the base implied. A texture may legitimately live in
     * main RAM, and forcing it into the VRAM window would send those reads
     * somewhere unrelated. */
    g_ge.drawn_vtype = g_ge.vtype;
    g_ge.drawn_prims++;

    const int has_uv = g_ge.tex_enable && g_ge.tex_addr;
    const uint64_t _d1 = ge_prof_now();
    push_texture_state(has_uv);
    if (g_prof_on > 0) { const uint64_t _d2 = ge_prof_now(); g_prof_m[4] += _d2 - _d1; g_prof_m[6] += _d1 - _d0; }
    if (g_tl.clear_mode) { g_clear_draws++; if (g_tl.clear_z) g_clear_z_draws++; }
    if (VT_THROUGH(g_ge.vtype)) { if (has_uv) g_draw_2d_tex++; else g_draw_2d_flat++; }
    else                        { if (has_uv) g_draw_3d_tex++; else g_draw_3d_flat++; }
    push_pixel_state();

    /* Decode the whole batch, then hand it to the backend in one call.
     *
     * Format decoding stays here rather than in each backend: the stride
     * arithmetic and component alignment are fiddly, and duplicating them per
     * backend means every backend is wrong in its own way. Wrong once,
     * centrally, is at least diagnosable. */
    if (!VT_THROUGH(g_ge.vtype)) {
        draw_prim_transformed(type, count, col_off, pos_off, tex_off, norm_off, stride);
        return;
    }

    psp_vertex v[GE_VERTEX_BATCH], centre;
    const psp_render_backend *be = psp_render_current();

    uint32_t done = 0;
    while (done < count) {
        /* Strips overlap at a batch boundary: two vertices for triangles,
         * one for lines, or the primitive spanning the boundary is lost. A
         * fan's later batches start again from its first vertex, then the
         * last batch's last: each triangle is (first, previous, this). */
        const uint32_t lead = type == PSP_PRIM_TRIANGLE_FAN && done > 0;
        const uint32_t n = primitive_batch_count(type, count - done + lead) - lead;

        uint32_t decoded = 0;
        for (; decoded < n; decoded++) {
            if (!read_vertex(vertex_addr(done + decoded, stride),
                             g_ge.vtype, col_off, pos_off, tex_off, &v[decoded]))
                break;
        }
        if (!decoded) break;
        if (lead) {
            memmove(v + 1, v, decoded * sizeof *v);
            v[0] = centre;
            decoded++;
        } else if (type == PSP_PRIM_TRIANGLE_FAN) centre = v[0];

        if (texdraw_hit() || wilduv_hit(v, decoded)) texdraw_dump(PRIM_NAME[type & 7], v, decoded);

        if (drawlog_left()) {
            int x0=v[0].x,x1=v[0].x,y0=v[0].y,y1=v[0].y;
            for (uint32_t i=1;i<decoded;i++){
                if(v[i].x<x0)x0=v[i].x; if(v[i].x>x1)x1=v[i].x;
                if(v[i].y<y0)y0=v[i].y; if(v[i].y>y1)y1=v[i].y;
            }
            fprintf(stderr, "2d:   %-14s %2u verts  x %4d..%-4d y %4d..%-4d  "
                            "fbp %08X  vaddr %08X  rgba %08X %08X  vtype %06X  z %d/%d/%d clr %d  tex %s\n",
                    PRIM_NAME[type & 7], decoded, x0 >> 4, x1 >> 4, y0 >> 4, y1 >> 4,
                    ge_fb_address(g_ge.fbp), g_ge.vaddr,
                    v[0].rgba, v[decoded>1?1:0].rgba, g_ge.vtype, g_tl.ztest_enable, g_tl.ztest_func, !g_tl.zwrite_off, g_tl.clear_mode,
                    has_uv ? "yes" : "no");
            fprintf(stderr, "      raw");
            for (uint32_t i = 0; i < decoded && i < 2; i++) {
                const uint32_t a = g_ge.vaddr + (done + i) * (uint32_t)stride;
                fprintf(stderr, "  v%u@%08X:", i, a);
                for (int b = 0; b < stride && b < 16; b++)
                    fprintf(stderr, "%02X", psp_read8(a + (uint32_t)b));
            }
            fprintf(stderr, "  stride %d col_off %d pos_off %d\n", stride, col_off, pos_off);
            fprintf(stderr, "      clear %s (z %d)  blend %s src %d dst %d  "
                            "atest %s func %d ref %d\n",
                    g_tl.clear_mode ? "ON" : "off", g_tl.clear_z,
                    g_tl.blend.enable ? "on" : "off", g_tl.blend.src, g_tl.blend.dst,
                    g_tl.blend.alpha_test ? "on" : "off",
                    g_tl.blend.alpha_func, g_tl.blend.alpha_ref);
        }
        be->draw((int)type, v, (int)decoded);

        if (type == PSP_PRIM_TRIANGLE_STRIP && decoded == GE_VERTEX_BATCH && done + decoded < count)
            done += decoded - 2;     /* strip overlap */
        else if (type == PSP_PRIM_TRIANGLE_FAN && decoded == GE_VERTEX_BATCH && done + decoded - lead < count)
            done += decoded - lead - 1;
        else if (type == PSP_PRIM_LINE_STRIP && decoded == GE_VERTEX_BATCH && done + decoded < count)
            done += decoded - 1;
        else
            done += decoded - lead;
    }
}

/* Bezier and spline patches.
 *
 * BEZIER (0x05) and SPLINE (0x06) name a grid of control points at VADDR,
 * ucount across and vcount down (PSPSDK's sceGuDrawBezier/sceGuDrawSpline:
 * ucount in bits 0-7, vcount in 8-15, and for a spline the two edge modes in
 * 16-17 and 18-19). The GE evaluates the surface on a grid set by
 * PATCHDIVISION and draws it as PATCHPRIMITIVE says: triangles, lines along
 * the grid, or its points. Every attribute the control points carry is
 * evaluated with the same weights; a vertex type without texture
 * coordinates gets the surface parameters as coordinates (geprobe 2 scene 22,
 * fw 6.60, maps the probe texture once across a Bezier patch that has none).
 *
 * The control points go through read_mvert first, so a skinned or morphed
 * control grid is skinned or morphed before it is evaluated. */

/* Where along one direction the grid samples, and how: each sample blends
 * four consecutive control points starting at `first`, by de Boor's
 * algorithm with the six 8-bit parameters `a` (deboor_params). Colours run
 * that algorithm in fixed point (deboor_fix); positions, normals and texture
 * coordinates take the weights the same parameters give (deboor_weights),
 * in double: geprobe 10 scene 55 (fw 6.60) reads 2332 weights through
 * depth, and every one sits within -2 to 0 depth steps of those, the
 * readout's own noise on Bezier pieces, where exact Cox-de Boor weights
 * range from -86 to +63 steps. */
typedef struct { int first; double w[4]; float param; uint16_t a[6]; } patch_sample;

/* The six lerp parameters of de Boor's algorithm at local step T (1/256)
 * of the span starting at knot index k, from integer span-local knots, in
 * the order the levels take them: level 1's three (control 3, 2, 1 of the
 * span), level 2's two, level 3's one. Each is an 8-bit fraction taken from
 * the nearer knot: the smaller of t - u_lo and u_hi - t over the knot gap,
 * cut to 1/256 (geprobe 8-10, fw 6.60: see deboor_eval). */
static void deboor_params(const int *kn, int k, int T, uint16_t a[6]) {
    int n = 0;
    for (int j = 1; j <= 3; j++)
        for (int idx = 3; idx >= j; idx--) {
            const int i = k - 3 + idx, ul = kn[i] - kn[k], ur = kn[i + 4 - j] - kn[k];
            const int den = ur - ul, dl = T - 256 * ul, dr = 256 * ur - T;
            a[n++] = (uint16_t)(dl <= dr ? dl / den : 256 - dr / den);
        }
}

/* de Boor's algorithm on four values with those parameters: integers in
 * 1/128 of a colour step, each lerp floored back to 1/128. */
static int64_t deboor_fix(const int64_t in[4], const uint16_t a[6]) {
    int64_t P[4] = { in[0], in[1], in[2], in[3] };
    int n = 0;
    for (int j = 1; j <= 3; j++) {
        int64_t Q[4] = { P[0], P[1], P[2], P[3] };
        for (int idx = 3; idx >= j; idx--, n++)
            Q[idx] = ((256 - (int64_t)a[n]) * P[idx - 1] + (int64_t)a[n] * P[idx]) >> 8;
        memcpy(P, Q, sizeof P);
    }
    return P[3];
}

/* One lerp of a patch position, as the GE runs it (geprobe 13, fw 6.60):
 * a weight of 0 or 256 hands its operand on as it is; otherwise both
 * operands go onto the 16-bit grid of the larger, each cut toward zero,
 * and ((256 - a) P + a Q) / 256 is floored on that grid. */
static double la_lerp(double p, double q, int a) {
    if (a == 0) return p;
    if (a == 256) return q;
    int ep = INT_MIN, eq = INT_MIN, e;
    if (p != 0.0) frexp(p, &ep);
    if (q != 0.0) frexp(q, &eq);
    e = ep > eq ? ep : eq;
    if (e == INT_MIN) return 0.0;
    const double P = trunc(ldexp(p, 16 - e)), Q = trunc(ldexp(q, 16 - e));
    return ldexp(floor(((256.0 - a) * P + a * Q) / 256.0), e - 16);
}
static double deboor_la(const double in[4], const uint16_t a[6]) {
    double P[4] = { in[0], in[1], in[2], in[3] };
    int n = 0;
    for (int j = 1; j <= 3; j++) {
        double Q[4] = { P[0], P[1], P[2], P[3] };
        for (int idx = 3; idx >= j; idx--, n++) Q[idx] = la_lerp(P[idx - 1], P[idx], a[n]);
        memcpy(P, Q, sizeof P);
    }
    return P[3];
}

/* The same in real arithmetic, on unit vectors: the four weights those
 * parameters give, for positions, normals and texture coordinates. */
static void deboor_weights(const uint16_t a[6], double w[4]) {
    for (int q = 0; q < 4; q++) {
        double P[4] = { 0, 0, 0, 0 };
        P[q] = 1.0;
        int n = 0;
        for (int j = 1; j <= 3; j++) {
            double Q[4] = { P[0], P[1], P[2], P[3] };
            for (int idx = 3; idx >= j; idx--, n++)
                Q[idx] = (1.0 - a[n] / 256.0) * P[idx - 1] + (a[n] / 256.0) * P[idx];
            memcpy(P, Q, sizeof P);
        }
        w[q] = P[3];
    }
}

/* Step i of div along a piece, as the GE places it: on a 1/256 grid, a
 * step of 256/div in 8.6 fixed point rounded up, i steps of it floored to
 * 1/256 up to the middle and 1 less div - i steps past it, so a piece reads
 * the same from either end. geprobe 13 (fw 6.60) reads the parameter of
 * 1128 (div, i) pairs whole through depth (scenes 67-82), and this fits
 * every one; floor(256 i/div), the rule geprobe 7 scene 44 suggested, is
 * a step off at 68. geprobe 7 scene 44 (fw 6.60) found the grid and the
 * mirroring. Scene 44 lights one control column and row
 * of a patch drawn as points, so each point's colour is one weight, 255
 * times: of its 42 Bezier and open spline samples (168 weights) all fit
 * this, 28 the exact i/div, and 29 the grid floored throughout. The points
 * move with it: Bezier 7's fifth column sits a pixel right of i/div's. */
static double patch_param(int i, int div) {
    const int q = (16384 + div - 1) / div;          /* 256/div in 8.6, rounded up */
    return (2 * i <= div ? (i * q) >> 6 : 256 - (((div - i) * q) >> 6)) / 256.0;
}

/* A Bezier direction: (count - 1) / 3 cubic pieces sharing end points, each
 * cut into `div` steps. Returns the number of samples. */
/* Set while a patch is drawn as points: each piece or span then emits its
 * own end samples, so a sample two of them share is drawn twice (geprobe 13
 * scene 73, fw 6.60: with additive blending those points read doubled).
 * Scene 73 shares samples along u only; v is taken to do the same.
 * Triangles and lines share it as one vertex. */
static int g_patch_dup;

static int bezier_samples(int count, int div, patch_sample *out, int max) {
    const int pieces = (count - 1) / 3;
    int n = 0;
    for (int pc = 0; pc < pieces; pc++) {
        for (int i = pc && !g_patch_dup ? 1 : 0; i <= div && n < max; i++) {
            const double t = patch_param(i, div);
            patch_sample *o = &out[n++];
            o->first = 3 * pc;
            /* A Bezier piece is de Casteljau's algorithm: every parameter t. */
            for (int q = 0; q < 6; q++) o->a[q] = (uint16_t)lround(t * 256.0);
            deboor_weights(o->a, o->w);
            o->param = (float)pc + (float)t;
        }
    }
    return n;
}

/* A spline direction: a cubic B-spline over `count` control points, count - 3
 * spans each cut into `div` steps. Bit 0 of the edge mode opens the start and
 * bit 1 the end (GU_OPEN_FILL is 1, GU_FILL_OPEN 2): an open end repeats its
 * knot so the surface runs out to the edge control point, a filled one keeps
 * the uniform knots and stops short of it. Scene 23's OPEN_OPEN patch spans
 * its whole control grid and its FILL_FILL patch the middle third. */
static int spline_samples(int count, int div, int edge, patch_sample *out, int max) {
    int kn[64];
    const int spans = count - 3;
    if (spans < 1 || count + 4 > 64) return 0;
    for (int k = 0; k < count + 4; k++) {
        int t = k - 3;
        if ((edge & 1) && t < 0) t = 0;
        if ((edge & 2) && t > spans) t = spans;
        kn[k] = t;
    }
    int n = 0;
    for (int sp = 0; sp < spans; sp++) {
        for (int i = sp && !g_patch_dup ? 1 : 0; i <= div && n < max; i++) {
            /* The same grid as a Bezier's (geprobe 10 scene 56 reads the
             * parameter itself through a texture: every step on it), and
             * de Boor's algorithm with 8-bit parameters from it
             * (deboor_params; deboor_fix says what it reproduces). */
            const double tl = patch_param(i, div);
            const int k = sp + 3;                     /* kn[k] <= t <= kn[k+1] */
            patch_sample *o = &out[n++];
            o->first = k - 3;
            deboor_params(kn, k, (int)lround(tl * 256.0), o->a);
            deboor_weights(o->a, o->w);
            o->param = (float)(sp + tl);
        }
    }
    return n;
}

enum { PATCH_MAX_SAMPLES = 4096 };   /* geprobe 13 scene 82: rows past 256 draw whole */

static void draw_patch(int spline, uint32_t arg) {
    const int ucount = (int)(arg & 0xFF), vcount = (int)((arg >> 8) & 0xFF);
    const int uedge = (int)((arg >> 16) & 3), vedge = (int)((arg >> 18) & 3);
    if (!g_ge.vaddr || VT_THROUGH(g_ge.vtype) || ucount < 4 || vcount < 4) return;
    int col_off = -1, pos_off = 0, tex_off = -1, norm_off = -1;
    const int stride = vertex_layout(g_ge.vtype, &col_off, &pos_off, &tex_off, &norm_off);
    if (!stride) return;
    /* The GE takes 7 bits of each division and draws up to 64. geprobe 13
     * (fw 6.60) scenes 80 and 81: 49 to 64 draw, and 65 (d 193 too) to 127
     * (d 255) hang the GE: nothing of the patch, nothing more of the list,
     * and the queue stays busy (sceGeDrawSync peeks 2) until sceGeBreak.
     * The run_list walk stops here and the queue is marked hung; set 21's
     * scenes 80 and 81 match the PSP so, where going on drew 120 pixels the
     * PSP never did. 0 draws as 1 (geprobe 23 scene 145: twelve patches at
     * 0 in u, v or both, none hangs and every pixel is 1's). */
    const int du = g_ge.patch_du ? g_ge.patch_du : 1, dv = g_ge.patch_dv ? g_ge.patch_dv : 1;
    if (du > 64 || dv > 64) { g_ge_hang = 1; return; }

    static patch_sample su[PATCH_MAX_SAMPLES], sv[PATCH_MAX_SAMPLES];
    g_patch_dup = g_ge.patch_prim == 2;
    const int nu = spline ? spline_samples(ucount, du, uedge, su, PATCH_MAX_SAMPLES)
                          : bezier_samples(ucount, du, su, PATCH_MAX_SAMPLES);
    const int nv = spline ? spline_samples(vcount, dv, vedge, sv, PATCH_MAX_SAMPLES)
                          : bezier_samples(vcount, dv, sv, PATCH_MAX_SAMPLES);
    if (nu < 2 || nv < 2) return;

    ge_mvert *cp = malloc(sizeof *cp * (size_t)(ucount * vcount));
    ge_mvert *grid = malloc(sizeof *grid * (size_t)(nu * nv));
    ge_mvert *list = malloc(sizeof *list * (size_t)(nu * nv * 6));
    if (!cp || !grid || !list) { free(cp); free(grid); free(list); return; }
    const int lit = g_tl.lighting && any_light_enabled();
    int ok = 1;
    for (int k = 0; k < ucount * vcount && ok; k++)
        ok = read_mvert(vertex_addr((uint32_t)k, stride), g_ge.vtype, col_off, pos_off, tex_off,
                        norm_off, lit, &cp[k]);
    if (!ok) { free(cp); free(grid); free(list); return; }

    for (int j = 0; j < nv; j++)
        for (int i = 0; i < nu; i++) {
            float pos[3] = { 0, 0, 0 }, nrm[3] = { 0, 0, 0 }, u = 0, v = 0;
            for (int b = 0; b < 4; b++)
                for (int a = 0; a < 4; a++) {
                    const double wd = sv[j].w[b] * su[i].w[a];
                    const float w = (float)wd;
                    if (wd == 0.0) continue;
                    const ge_mvert *c = &cp[(sv[j].first + b) * ucount + su[i].first + a];
                    for (int k = 0; k < 3; k++) nrm[k] += w * c->nrm[k];
                    u += w * c->u; v += w * c->v;
                }
            /* A position is de Boor's algorithm too, each coordinate on its
             * own, every lerp la_lerp's: along v for each of the four
             * control columns, then along u over the four results, from
             * controls cut to 16 bits (s16 and s8 controls are exact
             * fractions). geprobe 13 (fw 6.60) reads 134,198 generated
             * points whole through depth (scenes 67-82), over every
             * division to 64, edge mode, sign, binade, format, morph and
             * skin, and this places every one on its pixel with its depth;
             * u first misses 8767 of them. The weights above stay for
             * normals and texture coordinates. */
            for (int k = 0; k < 3; k++) {
                double C[4];
                for (int q = 0; q < 4; q++) {
                    double in[4];
                    for (int r = 0; r < 4; r++)
                        in[r] = ge_cut(cp[(sv[j].first + r) * ucount + su[i].first + q].pos[k], 16, 0);
                    C[q] = deboor_la(in, sv[j].a);
                }
                pos[k] = (float)deboor_la(C, su[i].a);
            }
            ge_mvert *o = &grid[j * nu + i];
            memcpy(o->pos, pos, sizeof pos);
            memcpy(o->nrm, nrm, sizeof nrm);
            /* A generated vertex's colour is not a blend by weights: the GE
             * runs de Boor's algorithm on each channel of the control
             * colours, with deboor_params' 8-bit parameters, along v for
             * each of the four control columns and then along u, every lerp
             * floored to 1/128 of a step and the end rounded up to a whole
             * one (deboor_fix). geprobe 9 and 10 (fw 6.60) scenes 52-54 read
             * 19,546 single and paired control columns at three levels over
             * every edge mode, and this reproduces every one; weights by
             * Cox-de Boor, through the cut-to-1/256 rule this replaces, fit
             * 13,452 of scenes 53 and 54's 16,302. The tiny weights near a
             * span's end that read low are those 1/128 cuts. Bezier pieces
             * are the same with every parameter t, which is why exact
             * weights fit them. v goes first, as for positions: geprobe 13
             * scene 72's colour-order cells read 747 pixels a step off u
             * first and none this way. */
            o->rgba = 0;
            for (int k = 0; k < 4; k++) {
                int64_t Q[4];
                for (int q = 0; q < 4; q++) {
                    int64_t in[4];
                    for (int r = 0; r < 4; r++)
                        in[r] = (int64_t)((cp[(sv[j].first + r) * ucount + su[i].first + q].rgba >> (8 * k)) & 0xFFu) * 128;
                    Q[q] = deboor_fix(in, sv[j].a);
                }
                int c = (int)((deboor_fix(Q, su[i].a) + 127) >> 7);
                if (c < 0) c = 0;
                if (c > 255) c = 255;
                o->rgba |= (uint32_t)c << (8 * k);
            }
            if (tex_off < 0) {
                u = su[i].param * g_ge.tex_scale_u + g_ge.tex_offset_u;
                v = sv[j].param * g_ge.tex_scale_v + g_ge.tex_offset_v;
                u *= (float)g_ge.tex_w;
                v *= (float)g_ge.tex_h;
            }
            o->u = u; o->v = v;
        }

    /* The grid goes out as the GE's own strips would: one per pair of rows,
     * alternating between them, (0,j) (0,j+1) (1,j) (1,j+1) ... As lines that
     * sequence is a zigzag -- each column's rung and a diagonal to the next
     * -- with no line along the rows at all: geprobe 2 scene 22 (fw 6.60)
     * draws its line patch exactly so (627 differing pixels against 1407 for
     * a plain grid of rows and columns). Points are the grid itself.
     *
     * Each span goes out on its own, over its own du x dv samples, the spans
     * a row of them at a time, so a column or row two spans share is drawn
     * by both: geprobe 21 (fw 6.60) scenes 131-133 draw nine patches as
     * lines smooth, flat and adding, and this order fits every pixel of
     * them, the whole grid's 204 off where segments cross or rungs are
     * drawn twice, spans a column at a time's 1 off. Triangles take the
     * same order, which no probe has told apart. */
    uint32_t n = 0, type;
    if (g_ge.patch_prim == 2) {
        type = PSP_PRIM_POINTS;
        for (int k = 0; k < nu * nv; k++) list[n++] = grid[k];
    } else {
        type = g_ge.patch_prim == 1 ? PSP_PRIM_LINES : PSP_PRIM_TRIANGLES;
        for (int ja = 0; ja + 1 < nv; ja += dv) {
            const int jb = ja + dv < nv ? ja + dv : nv - 1;
            for (int ia = 0; ia + 1 < nu; ia += du) {
                const int ib = ia + du < nu ? ia + du : nu - 1;
                for (int j = ja; j < jb; j++)
                    for (int i = ia; i < ib; i++) {
                        const ge_mvert *s0 = &grid[j * nu + i], *s1 = &grid[(j + 1) * nu + i];
                        const ge_mvert *s2 = &grid[j * nu + i + 1], *s3 = &grid[(j + 1) * nu + i + 1];
                        if (type == PSP_PRIM_LINES) {
                            list[n++] = *s0; list[n++] = *s1;
                            list[n++] = *s1; list[n++] = *s2;
                            if (i + 1 == ib) { list[n++] = *s2; list[n++] = *s3; }
                        } else {
                            list[n++] = *s0; list[n++] = *s1; list[n++] = *s2;
                            list[n++] = *s1; list[n++] = *s2; list[n++] = *s3;
                        }
                    }
            }
        }
    }
    g_mv_src = list;
    draw_prim(type, n);
    g_mv_src = NULL;
    free(cp); free(grid); free(list);
}

/* Walk a list until END/FINISH, the stall address, or a step budget.
 *
 * The budget is not paranoia: a list whose JUMP forms a cycle is a normal
 * intermediate state while the CPU is still writing, and without a bound a
 * malformed or partially-written list hangs the host with no diagnostic. */
static void run_list_body(ge_queue *q);

/* Set while a list is being walked. No guest code runs inside a walk, since
 * handlers run from the timeline, but a walk is never started inside one. */
static int g_ge_walking;
/* Set while the timeline applies events, calling handlers as it reaches
 * them. A handler is guest code and may call back into sceGe. A list it
 * enqueues or releases is walked at once. The timeline it would start is left
 * to the one already running, which goes on to that list's events. */
static int g_tl_in;
/* Set while a handler runs outside the timeline (one held for a firmware
 * call): a firmware call it makes does not start the timeline again inside it. */
static int g_ge_in_cb;

/* A SIGNAL or FINISH interrupt handler, called the way the firmware calls it:
 * (id, arg), id being the command's low 16 bits and arg the one registered
 * with sceGeSetCallback. geprobe step 26 (fw 6.60): SIGNAL 0x0E010044
 * and 0x0E020055 then FINISH 0x0F000066 under signal_arg 0x5A, finish_arg
 * 0xA5 reach the handlers as (0x44, 0x5A), (0x55, 0x5A), (0x66, 0xA5).
 * On hardware they run in interrupt context; here on the thread driving the
 * list, as an interrupt (src/hle/interrupt.c), with dispatch off so a
 * handler cannot block or be switched away.
 *
 * From the game's callback probe (tests/provenance/ge, recorded under an
 * emulator, not a PSP): the handler runs with its module's gp and the
 * incoming VFPU controls, and a module built against SDK 0x02000011 or
 * later finds in a2 the address after the END that follows the command
 * (`pc`). A host that registers no module keeps the interrupted thread's
 * gp, as this did before. */
static void ge_callback(int cbid, int finish, uint32_t id, uint32_t pc) {
    if (cbid < 0 || cbid >= GE_MAX_CALLBACKS || !g_ge_cb[cbid].used) return;
    const uint32_t fn  = finish ? g_ge_cb[cbid].finish_func : g_ge_cb[cbid].signal_func;
    const uint32_t arg = finish ? g_ge_cb[cbid].finish_arg  : g_ge_cb[cbid].signal_arg;
    if (!fn) return;
    /* Guest code is about to read what the GE drew. */
    psp_render_current()->finish();
    const uint32_t a2 = psp_sysmem_compiled_sdk() >= 0x02000011u ? pc : 0;
    const int was = psp_sched_set_dispatch(0);
    psp_interrupt_call_now(fn, psp_interrupt_handler_gp(fn), id & 0xFFFFu, arg, a2);
    psp_sched_set_dispatch(was);
}

/* The guest clock moved up to GE time `at`, for a handler that runs there
 * (see "When the GE runs"). Never back. */
static void ge_clock_to(uint64_t at) {
    const uint64_t us = at / GE_UNITS_PER_US;
    if (us > psp_clock_peek()) psp_clock_advance_to(us);
}

/* ---- the events (see "Drawing now, reporting on the clock") ---- */

static ge_event *ev_at(unsigned i) { return &g_ev[(g_ev_head + i) % GE_EVENTS]; }

/* The walk has run a command the guest will see, `start` being the GE's
 * time when the command started. The walk stops before a command while fewer
 * than two slots are free, so there is room. */
static void ev_push(int kind, ge_queue *q, int finish, uint32_t code, uint32_t pc, uint64_t start) {
    ge_event *e = ev_at(g_ev_n++);
    e->kind = kind; e->finish = finish; e->cbid = q->cbid;
    e->q = q; e->qid = q->id;
    e->code = code; e->pc = pc;
    e->start = start; e->at = g_ge_t;
}

static void ev_pop(void) {
    g_ev_head = (g_ev_head + 1) % GE_EVENTS;
    g_ev_n--;
    g_ev_held = 0;
}

static void ev_remove(unsigned i) {
    if (i == 0) { ev_pop(); return; }
    for (; i + 1 < g_ev_n; i++) *ev_at(i) = *ev_at(i + 1);
    g_ev_n--;
}

/* Whether an event's list is still in its slot: a list retired by DrawSync
 * may have given the slot to a new one. */
static int ev_live(const ge_event *e) { return e->q->used && e->q->id == e->qid; }

/* A SIGNAL or FINISH that has a handler to call. Asked when the timeline
 * reaches it, as the GE asks when it does. */
static int ev_has_handler(const ge_event *e) {
    return e->cbid >= 0 && e->cbid < GE_MAX_CALLBACKS && g_ge_cb[e->cbid].used &&
           (e->finish ? g_ge_cb[e->cbid].finish_func : g_ge_cb[e->cbid].signal_func);
}

/* The walk reaching a SIGNAL or FINISH whose list names a callback. */
static void ge_x_raise(ge_queue *q, int finish, uint32_t code, uint32_t pc, uint64_t start) {
    if (q->cbid < 0 || q->cbid >= GE_MAX_CALLBACKS) return;
    ev_push(GE_EV_HANDLER, q, finish, code, pc, start);
}

/* The walk reaching a list's end. A capture's replay has no guest to tell. */
static void ge_x_done(ge_queue *q, uint64_t start) {
    q->xdone = 1;
    if (q->replay) q->done = 1;
    else ev_push(GE_EV_DONE, q, 0, 0, 0, start);
}

/* Run a held handler. `force` runs it whatever the interrupt state (a wait on
 * the GE, which has to see handlers in order), moving the clock up to when
 * the GE reached it; otherwise only with interrupts enabled and once the
 * clock has got there. Returns nonzero if one is still held. */
static int ge_deliver(int force) {
    if (!g_ev_held) return 0;
    const ge_event e = *ev_at(0);
    if (!force && (!psp_intr_enabled() || e.at > ge_now())) return 1;
    ev_pop();
    if (force) ge_clock_to(e.at);
    g_ge_in_cb = 1;
    ge_callback(e.cbid, e.finish, e.code, e.pc);
    g_ge_in_cb = 0;
    return 0;
}

/* For the interrupt layer (src/hle/interrupt.c): a handler held for an
 * interrupt-enabled moment, and delivering it at a completed firmware call. */
int psp_ge_callbacks_pending(void) { return g_ev_held; }

void psp_ge_run_pending_callbacks(void) {
    if (g_tl_in || g_ge_in_cb || psp_interrupt_in_handler()) return;
    ge_deliver(0);
}

/* BBOX (0x07): the next `count` vertices at VADDR, of the current vertex
 * type, are the corners of a box the following BJUMP skips when it cannot be
 * seen. geprobe 4 scene 24 (fw 6.60) runs eight boxes of eight corners under
 * a 60-degree perspective: only the one far right of the screen and the one
 * far above it are skipped. One wholly behind the camera, one beyond the far
 * plane, one through the near plane and two across or near an edge are all
 * drawn -- so nothing about depth or the camera counts, only where x and y
 * land after the divide. The box is hidden when all its corners are beyond
 * the same edge.
 *
 * geprobe 5 scene 33 (fw 6.60) settles more:
 *  - The divide is by |w|: a corner behind the camera is not mirrored. The
 *    box across the camera plane far right (corners at w = 0.5 and -0.5) is
 *    skipped, which a mirrored divide would spread from far left to far
 *    right and draw; the two wholly behind the camera and far to one side
 *    are skipped as well. Scene 24's box straight behind the camera lands on
 *    screen either way. That the hardware takes |w| rather than, say, the
 *    side each corner's x points to is the simplest reading, not a
 *    measurement.
 *  - The edges are the scissor's: a box in view is skipped with the scissor
 *    cut to a corner away from it, and drawn with the scissor over part of
 *    it; one outside a half-size viewport's clip volume but on screen is
 *    drawn.
 *  - BBOX moves VADDR past its corners as PRIM does (see advance_vertices):
 *    a PRIM straight after it with no VADDR draws the vertices that follow
 *    the box.
 *  - BBOX alone does not stop a draw; only BJUMP acts on its result.
 * Not measured: skinned or morphed corners (the first vertex set, unskinned,
 * is used), indexed boxes, and through-mode boxes (taken as visible). */
static int bbox_hidden(uint32_t count) {
    int col_off, pos_off, tex_off, norm_off;
    const int stride = vertex_layout(g_ge.vtype, &col_off, &pos_off, &tex_off, &norm_off);
    if (!stride || !count || VT_THROUGH(g_ge.vtype)) return 0;
    const int x0 = g_ge.sc_set ? g_ge.sc_x0 : 0,   y0 = g_ge.sc_set ? g_ge.sc_y0 : 0;
    const int x1 = g_ge.sc_set ? g_ge.sc_x1 : 479, y1 = g_ge.sc_set ? g_ge.sc_y1 : 271;
    int left = 1, right = 1, above = 1, below = 1;
    double wvp[4][4];
    ge_wvp(wvp);
    for (uint32_t i = 0; i < count; i++) {
        float m[3], c[4];
        if (!read_pos_model(vertex_addr(i, stride), g_ge.vtype, pos_off, m)) return 0;
        ge_clip(wvp, m, c);
        c[2] = 0.0f;
        c[3] = fabsf(c[3]);
        if (c[3] == 0.0f || !isfinite(c[0] / c[3]) || !isfinite(c[1] / c[3])) return 0;
        int x, y;
        clip_to_fx16(c, &x, &y);
        left  &= x < x0 * PSP_SUBPX;
        right &= x >= (x1 + 1) * PSP_SUBPX;
        above &= y < y0 * PSP_SUBPX;
        below &= y >= (y1 + 1) * PSP_SUBPX;
    }
    return left || right || above || below;
}

/* PRIM and BBOX leave the vertex pointer past what they read: VADDR moves on
 * by count vertices, or IADDR by count indices when the type is indexed.
 * geprobe 5 scene 33 (fw 6.60) measures it for BBOX, whose box is followed
 * by a PRIM with no VADDR of its own that draws the vertices after the box.
 * libgu's sceGuDrawArrayN depends on it for PRIM: it sends VADDR once and
 * then one PRIM per primitive. geprobe 6 scene 42 (fw 6.60) measures the
 * indexed case, with 8- and 16-bit indices: PRIM and BBOX move IADDR on by
 * count indices and leave VADDR where it was. */
static void advance_vertices(uint32_t count) {
    int col_off, pos_off, tex_off, norm_off;
    const int stride = vertex_layout(g_ge.vtype, &col_off, &pos_off, &tex_off, &norm_off);
    switch (VT_INDEX(g_ge.vtype)) {
    case 1:  g_ge.iaddr += count;      break;
    case 2:  g_ge.iaddr += 2u * count; break;
    default: g_ge.vaddr += count * (uint32_t)stride; break;
    }
}

/* A drawing command handed from the front end to the back end, `pixels`
 * pixels of drawing. With GE_FIFO_PRIMS commands already waiting, the front
 * end waits for the oldest to be drawn. */
static void ge_back_push(uint64_t pixels) {
    uint64_t *slot = &g_ge_fifo[g_ge_fifo_i];
    if (*slot > g_ge_t) g_ge_t = *slot;
    g_ge_back = (g_ge_back > g_ge_t ? g_ge_back : g_ge_t) + pixels * GE_PIXEL_UNITS;
    *slot = g_ge_back;
    g_ge_fifo_i = (g_ge_fifo_i + 1) % GE_FIFO_PRIMS;
}

static void run_list(ge_queue *q) {
    if (g_ge_walking || q->xpaused || q->hung) return;
    g_ge_walking = 1;
    const uint64_t _r0 = ge_prof_now();
    run_list_body(q);
    q->t_x = g_ge_t;
    if (g_prof_on > 0) { g_prof_m[3] += ge_prof_now() - _r0; g_prof_lists++; }
    g_ge_walking = 0;
}
/* Set by a save state's load: the backend has not been told the registers.
 * Told from the first list's walk rather than from the load, because the GL
 * backend belongs to the thread that first calls it, and that has to be the
 * GE's, not the boot's. */
static int g_backend_stale;

static void run_list_body(ge_queue *q) {
    ge_note_thread();
    if (g_backend_stale) { g_backend_stale = 0; psp_ge_sync_backend(); }
    uint64_t budget = 1u << 22;

    /* NB: lists are counted at enqueue (submitted), not here: a
     * stall-streamed list resumes here several times but was submitted
     * once. Counting runs made the total depend on how many stall updates
     * the CPU interleaved. */

    /* Words are fetched through one host pointer over a span of the list,
     * looked up again only when a command redirects the list or the span
     * runs out: a translated load per word was a sixth of the walk. The span
     * lookup is the same translation, so an unmapped list still counts a bad
     * access, and a list in VRAM still reaches the access observer. */
    const uint8_t *lp = NULL;
    uint32_t lp_addr = 0, lp_end = 0;
    while (budget--) {
        if (q->stall && q->list == q->stall) break;   /* caught up to the CPU */
        /* A command adds at most two events; the walk goes on once the
         * timeline has made room (ge_advance). */
        if (!q->replay && g_ev_n + 2 > GE_EVENTS) { g_ge_xfull = 1; break; }

        uint32_t word;
        if (q->list != lp_addr || q->list + 4 > lp_end || !lp) {
            uint32_t span = 4096;
            lp = (const uint8_t *)psp_mem_ptr(q->list, span);
            if (!lp) { span = 4; lp = (const uint8_t *)psp_mem_ptr(q->list, span); }
            lp_addr = q->list; lp_end = lp ? q->list + span : 0;
        }
        if (lp) { memcpy(&word, lp, 4); lp += 4; lp_addr += 4; }
        else word = psp_read32(q->list);
        uint32_t cmd  = word >> 24;
        uint32_t arg  = word & 0x00FFFFFF;
        /* A FINISH waits for the drawing before it (see "When the GE
         * runs"). */
        if (cmd == GE_FINISH && g_ge_back > g_ge_t) g_ge_t = g_ge_back;
        const uint64_t _c0 = ge_prof_now();
        const uint64_t t0 = g_ge_t;
        q->list += 4;
        g_ge.commands++;
        g_ge_t += GE_COMMAND_UNITS;

        switch (cmd) {
        case GE_NOP:
            break;

        case GE_PRIM: {
            uint32_t type  = (arg >> 16) & 7;
            uint32_t count = arg & 0xFFFF;
            g_ge.prims[type]++;
            /* A game that never writes FBP still draws somewhere -- register
             * the default target lazily so the census is never empty. */
            if (g_ge.n_targets == 0)
                ge_note_target(ge_fb_address(g_ge.fbp), g_ge.fbw, g_ge.fbfmt);
            g_ge.targets[g_ge.cur_target].prims++;
            g_ge.vertices += count;
            const uint64_t px0 = psp_render_pixels();
            draw_prim(type, count);
            advance_vertices(count);
            g_ge_t += GE_PRIM_UNITS + GE_VERTEX_UNITS * count;
            ge_back_push(psp_render_pixels() - px0);
            break;
        }
        case GE_BEZIER:
        case GE_SPLINE: {
            g_ge.prims[3]++;
            const uint64_t px0 = psp_render_pixels();
            draw_patch(cmd == GE_SPLINE, arg);
            if (g_ge_hang) { g_ge_hang = 0; q->hung = 1; return; }
            g_ge_t += GE_PRIM_UNITS + GE_VERTEX_UNITS * (arg & 0xFF) * ((arg >> 8) & 0xFF);
            ge_back_push(psp_render_pixels() - px0);
            break;
        }

        case GE_JUMP:
            q->list = (q->base | (arg & 0xFFFFFC));
            break;
        case GE_CALL:
            if (q->sp < GE_STACK) q->stack[q->sp++] = q->list;
            q->list = (q->base | (arg & 0xFFFFFC));
            break;
        case GE_RET:
            if (q->sp > 0) q->list = q->stack[--q->sp];
            break;
        case GE_BBOX:
            g_ge.bbox_hidden = bbox_hidden(arg & 0xFFFF);
            advance_vertices(arg & 0xFFFF);
            break;
        case GE_BJUMP:
            /* Jumps when the last BBOX found its box hidden (bbox_hidden). It
             * is read only once the stall address has passed it, so the
             * placeholder libgu's sceGuBeginObject writes is never taken:
             * sceGuEndObject has patched it by then. */
            if (g_ge.bbox_hidden) q->list = (q->base | (arg & 0xFFFFFC));
            break;

        case GE_FINISH:
            if (q->signal == GE_SIGNAL_SYNC) {
                /* A sync point's FINISH flushes what came before it and the
                 * list goes on: no finish handler, no capture snapshot, and
                 * the list is not done. The END after it clears the mark. */
                psp_render_current()->finish();
                break;
            }
            if (q->signal == GE_SIGNAL_HANDLER_PAUSE) {
                /* sceGuSignal(GU_SIGNAL_PAUSE) writes SIGNAL, END, FINISH,
                 * END. geprobe 5 step 57 (fw 6.60): the signal handler runs
                 * at the SIGNAL, inside EnQueue; then the list stops until
                 * sceGeContinue -- 20 ms on, one handler has run,
                 * ListSync(peek) reads 4 and DrawSync(peek) 2 -- and this
                 * FINISH calls no finish handler. After sceGeContinue the
                 * rest runs, inside the call as a new list would. It stops
                 * here, at the FINISH, and resumes at the END after it. A
                 * sceGeContinue that came first (from the signal handler)
                 * lets it through; not measured. */
                psp_render_current()->finish();
                if (q->cont_early) { q->cont_early = 0; break; }
                /* A capture's replay has no guest to call sceGeContinue, so
                 * it goes on through to the list's real FINISH. */
                if (q->replay) break;
                q->xpaused = 1;
                ev_push(GE_EV_PAUSE, q, 0, 0, 0, t0);
                return;
            }
            g_ge.finishes++;
            cap_snapshot_memory();
            /* The end of a list is what finish() means, and until now nothing
             * called it -- the interface has documented it as "a good point to
             * flush batched work" since it was written, and the software path
             * never noticed because it draws each primitive immediately and
             * has nothing to batch. A backend that accumulates geometry has no
             * flush point without this, so its batch spans a whole frame. */
            psp_render_current()->finish();
            /* The list is done before its handler runs, so a handler that
             * asks after it is told so. Not measured. */
            ge_x_done(q, t0);
            ge_x_raise(q, 1, arg, q->list + 4, t0);
            return;

        case GE_END: {
            /* END is also the second word of every SIGNAL encoding. Treating
             * it as list completion drops everything after a signal -- in
             * this game, the rest of the scene and the entire HUD. */
            uint32_t signal = psp_read32(q->list - 8);
            if ((signal >> 24) != GE_SIGNAL) {
                if ((signal >> 24) == GE_FINISH &&
                    (q->signal == GE_SIGNAL_HANDLER_PAUSE || q->signal == GE_SIGNAL_SYNC)) {
                    q->signal = 0;
                    break;
                }
                /* Preserve the old bare-END fallback. Normal completed lists
                 * stop at FINISH above and never reach their trailing END. */
                cap_snapshot_memory();
                psp_render_current()->finish();
                ge_x_done(q, t0);
                return;
            }

            uint32_t behaviour = (signal >> 16) & 0xFF;
            if (behaviour == GE_SIGNAL_HANDLER_PAUSE || behaviour == GE_SIGNAL_SYNC) {
                q->signal = (int)behaviour;
            }
            break;
        }

        case GE_SIGNAL:
            /* Behaviours 1-3 (PSPSDK pspgu.h GU_SIGNAL_WAIT, NOWAIT, PAUSE)
             * call the signal handler; the list then carries on, the PAUSE
             * variant through its FINISH/END pair below. The jump/call/ret
             * and other behaviours are not modelled and call nobody. A
             * PAUSE is marked here, before its handler runs, so that a
             * sceGeContinue from that handler finds it (cont_early);
             * without the mark the handler's call did nothing and the list
             * paused anyway. Which of the two the PSP does is not measured
             * (geprobe 6 asks). The guest's Continue looks for the mark once
             * the timeline has reached it (psig). */
            if (((arg >> 16) & 0xFF) == GE_SIGNAL_HANDLER_PAUSE) {
                q->signal = GE_SIGNAL_HANDLER_PAUSE;
                if (!q->replay) ev_push(GE_EV_PSIG, q, 0, 0, 0, t0);
            }
            if (((arg >> 16) & 0xFF) >= 1 && ((arg >> 16) & 0xFF) <= 3)
                ge_x_raise(q, 0, arg, q->list + 4, t0);
            break;

        case GE_BASE:        q->base = (arg & 0xFF0000) << 8; break;
        case GE_ORIGIN_ADDR: q->origin = q->list - 4; break;
        case GE_OFFSET_ADDR: q->base = arg << 8; break;

        case GE_VTYPE: g_ge.vtype = arg; break;

        /* ---- transform state ----------------------------------------------
         *
         * NUMBER sets the write cursor, DATA advances it. Writes past the end
         * are dropped: a list can be read while the CPU is still writing it,
         * and wrapping the cursor would scribble over elements already set. */
        case GE_WORLDMATRIXNUMBER:
            if (drawlog_aux()) fprintf(stderr, "mtx: WORLD NUMBER arg=%06X -> %d\n",
                                        arg, (int)(arg & 0xF));
            g_tl.world_n = (int)(arg & 0xF); break;
        case GE_VIEWMATRIXNUMBER:  g_tl.view_n  = (int)(arg & 0xF); break;
        case GE_PROJMATRIXNUMBER:  g_tl.proj_n  = (int)(arg & 0x1F); break;
        case GE_WORLDMATRIXDATA:
            if (drawlog_aux()) fprintf(stderr, "mtx: WORLD DATA  arg=%06X -> [%d] = %.2f\n",
                                        arg, g_tl.world_n, ge_float(arg));
            if (g_tl.world_n < 12) {
                g_tl.world[g_tl.world_n++] = ge_float(arg);
                if (g_tl.world_n == 12) view_log_note_world();
            }
            g_tl.world_words++;
            break;
        case GE_VIEWMATRIXDATA:
            if (g_tl.view_n < 12) {
                g_tl.view[g_tl.view_n++] = ge_float(arg);
                /* The twelfth word completes an upload. Noted here and not at
                 * the frame boundary: the HUD draws last and sets a view of
                 * its own, so by drain time the scene's matrix is gone. */
                if (g_tl.view_n == 12) view_log_note();
            }
            g_tl.view_words++;
            break;
        case GE_PROJMATRIXDATA:
            if (g_tl.proj_n < 16) g_tl.proj[g_tl.proj_n++] = ge_float(arg);
            g_tl.proj_words++;
            break;
        case GE_BONEMATRIXNUMBER: g_tl.bone_n = (int)(arg & 0x7F); break;
        case GE_BONEMATRIXDATA:
            if (g_tl.bone_n < 8 * 12) { g_tl.bone[g_tl.bone_n++] = ge_float(arg); g_bone16_ok = 0; }
            break;
        case GE_MORPHWEIGHT0:     case GE_MORPHWEIGHT0 + 1: case GE_MORPHWEIGHT0 + 2:
        case GE_MORPHWEIGHT0 + 3: case GE_MORPHWEIGHT0 + 4: case GE_MORPHWEIGHT0 + 5:
        case GE_MORPHWEIGHT0 + 6: case GE_MORPHWEIGHT0 + 7:
            g_tl.morph_w[cmd - GE_MORPHWEIGHT0] = ge_float(arg);
            break;
        case GE_PATCHDIVISION:
            g_ge.patch_du = (int)(arg & 0x7F);            /* 7 bits: geprobe 13 */
            g_ge.patch_dv = (int)((arg >> 8) & 0x7F);
            break;
        case GE_PATCHPRIMITIVE: g_ge.patch_prim = (int)(arg & 3); break;
        case GE_PATCHFACING:    g_ge.patch_face = (int)(arg & 1); break;

        case GE_VIEWPORTXSCALE:  g_tl.vp_xs = ge_float(arg); g_tl.vp_set = 1; break;
        case GE_VIEWPORTYSCALE:  g_tl.vp_ys = ge_float(arg); g_tl.vp_set = 1; break;
        case GE_VIEWPORTZSCALE:  g_tl.vp_zs = ge_float(arg); break;
        case GE_VIEWPORTXCENTER: g_tl.vp_xc = ge_float(arg); break;
        case GE_VIEWPORTYCENTER: g_tl.vp_yc = ge_float(arg); break;
        case GE_VIEWPORTZCENTER: g_tl.vp_zc = ge_float(arg); break;

        /* Offsets are in sixteenths of a pixel: sceGuOffset sends x << 4. */
        case GE_OFFSETX: g_tl.off_x = (float)(arg & 0xFFFFFu) / 16.0f; break;
        case GE_OFFSETY: g_tl.off_y = (float)(arg & 0xFFFFFu) / 16.0f; break;

        case GE_COLORTESTENABLE: g_tl.blend.colour_test  = (int)(arg & 1); break;
        case GE_COLORTEST:       g_tl.blend.colour_func  = (int)(arg & 3); break;
        case GE_COLORREF:        g_tl.blend.colour_ref   = arg & 0xFFFFFFu; break;
        case GE_COLORTESTMASK:   g_tl.blend.colour_mask  = arg & 0xFFFFFFu; break;
        case GE_LOGICOPENABLE:   g_tl.blend.logic_enable = (int)(arg & 1); break;
        case GE_LOGICOP:         g_tl.blend.logic_op     = (int)(arg & 15); break;
        case GE_PIXELMASKRGB:
            g_tl.blend.pixel_mask = (g_tl.blend.pixel_mask & 0xFF000000u) | (arg & 0xFFFFFFu);
            break;
        case GE_PIXELMASKALPHA:
            g_tl.blend.pixel_mask = (g_tl.blend.pixel_mask & 0x00FFFFFFu) | ((arg & 0xFFu) << 24);
            break;
        case GE_CULLFACEENABLE: g_tl.cull_enable = (int)(arg & 1); break;
        case GE_CULL:           g_tl.cull_ccw    = (int)(arg & 1); break;
        /* Stored inverted, so the reset state (all zero) stays Gouraud, as it
         * was before this register was decoded. */
        case GE_SHADE:          g_tl.blend.shade_flat = !(arg & 1); break;
        case GE_DITHERENABLE:   g_tl.blend.dither = (int)(arg & 1); break;
        /* One matrix row each, four signed nibbles, element 0 lowest:
         * sceGuSetDither's row {-4, 0, -3, 1} arrives as 0x001D0C. */
        case GE_DITHER0: case GE_DITHER0 + 1: case GE_DITHER0 + 2: case GE_DITHER0 + 3:
            for (int j = 0; j < 4; j++)
                g_tl.blend.dither_m[cmd - GE_DITHER0][j] =
                    (int8_t)((int)((arg >> (4 * j)) & 0xF) - (int)(((arg >> (4 * j)) & 0x8) << 1));
            break;
        /* ---- texture state -------------------------------------------------
         *
         * Recorded here and handed to the backend, which samples it: the
         * software one in render.c, against geprobe's texel readings
         * (docs/RENDERER.md). */
        /* Texture and palette addresses arrive in two registers, and the
         * second one carries the high *nibble* -- bits 24..27 -- in its own
         * bits 16..19, not in its low byte. Both addresses are 16-byte
         * aligned, so the low four bits of the base are not part of it
         * either. Taking the low byte instead reads the palette from a
         * completely different place, which leaves texel *indices* right and
         * every colour wrong: legible shapes, speckled everywhere. */
        case GE_TEXADDR0 + 1:
            g_ge.tex_lv_addr[1] = (g_ge.tex_lv_addr[1] & 0x0F000000u) | (arg & 0x00FFFFF0u);
            break;
        case GE_TEXBUFWIDTH0 + 1:
            g_ge.tex_lv_stride[1] = arg & 0x7FF;
            g_ge.tex_lv_addr[1]   = (g_ge.tex_lv_addr[1] & 0x00FFFFF0u) | ((arg << 8) & 0x0F000000u);
            break;
        case GE_TEXSIZE0 + 1:
            g_ge.tex_lv_w[1] = 1u << (arg & 0xF);
            g_ge.tex_lv_h[1] = 1u << ((arg >> 8) & 0xF);
            if (g_ge.tex_lv_w[1] > 512) g_ge.tex_lv_w[1] = 512;
            if (g_ge.tex_lv_h[1] > 512) g_ge.tex_lv_h[1] = 512;
            break;
        case GE_TEXADDR0 + 2:
            g_ge.tex_lv_addr[2] = (g_ge.tex_lv_addr[2] & 0x0F000000u) | (arg & 0x00FFFFF0u);
            break;
        case GE_TEXBUFWIDTH0 + 2:
            g_ge.tex_lv_stride[2] = arg & 0x7FF;
            g_ge.tex_lv_addr[2]   = (g_ge.tex_lv_addr[2] & 0x00FFFFF0u) | ((arg << 8) & 0x0F000000u);
            break;
        case GE_TEXSIZE0 + 2:
            g_ge.tex_lv_w[2] = 1u << (arg & 0xF);
            g_ge.tex_lv_h[2] = 1u << ((arg >> 8) & 0xF);
            if (g_ge.tex_lv_w[2] > 512) g_ge.tex_lv_w[2] = 512;
            if (g_ge.tex_lv_h[2] > 512) g_ge.tex_lv_h[2] = 512;
            break;
        case GE_TEXADDR0 + 3:
            g_ge.tex_lv_addr[3] = (g_ge.tex_lv_addr[3] & 0x0F000000u) | (arg & 0x00FFFFF0u);
            break;
        case GE_TEXBUFWIDTH0 + 3:
            g_ge.tex_lv_stride[3] = arg & 0x7FF;
            g_ge.tex_lv_addr[3]   = (g_ge.tex_lv_addr[3] & 0x00FFFFF0u) | ((arg << 8) & 0x0F000000u);
            break;
        case GE_TEXSIZE0 + 3:
            g_ge.tex_lv_w[3] = 1u << (arg & 0xF);
            g_ge.tex_lv_h[3] = 1u << ((arg >> 8) & 0xF);
            if (g_ge.tex_lv_w[3] > 512) g_ge.tex_lv_w[3] = 512;
            if (g_ge.tex_lv_h[3] > 512) g_ge.tex_lv_h[3] = 512;
            break;
        case GE_TEXADDR0 + 4:
            g_ge.tex_lv_addr[4] = (g_ge.tex_lv_addr[4] & 0x0F000000u) | (arg & 0x00FFFFF0u);
            break;
        case GE_TEXBUFWIDTH0 + 4:
            g_ge.tex_lv_stride[4] = arg & 0x7FF;
            g_ge.tex_lv_addr[4]   = (g_ge.tex_lv_addr[4] & 0x00FFFFF0u) | ((arg << 8) & 0x0F000000u);
            break;
        case GE_TEXSIZE0 + 4:
            g_ge.tex_lv_w[4] = 1u << (arg & 0xF);
            g_ge.tex_lv_h[4] = 1u << ((arg >> 8) & 0xF);
            if (g_ge.tex_lv_w[4] > 512) g_ge.tex_lv_w[4] = 512;
            if (g_ge.tex_lv_h[4] > 512) g_ge.tex_lv_h[4] = 512;
            break;
        case GE_TEXADDR0 + 5:
            g_ge.tex_lv_addr[5] = (g_ge.tex_lv_addr[5] & 0x0F000000u) | (arg & 0x00FFFFF0u);
            break;
        case GE_TEXBUFWIDTH0 + 5:
            g_ge.tex_lv_stride[5] = arg & 0x7FF;
            g_ge.tex_lv_addr[5]   = (g_ge.tex_lv_addr[5] & 0x00FFFFF0u) | ((arg << 8) & 0x0F000000u);
            break;
        case GE_TEXSIZE0 + 5:
            g_ge.tex_lv_w[5] = 1u << (arg & 0xF);
            g_ge.tex_lv_h[5] = 1u << ((arg >> 8) & 0xF);
            if (g_ge.tex_lv_w[5] > 512) g_ge.tex_lv_w[5] = 512;
            if (g_ge.tex_lv_h[5] > 512) g_ge.tex_lv_h[5] = 512;
            break;
        case GE_TEXADDR0 + 6:
            g_ge.tex_lv_addr[6] = (g_ge.tex_lv_addr[6] & 0x0F000000u) | (arg & 0x00FFFFF0u);
            break;
        case GE_TEXBUFWIDTH0 + 6:
            g_ge.tex_lv_stride[6] = arg & 0x7FF;
            g_ge.tex_lv_addr[6]   = (g_ge.tex_lv_addr[6] & 0x00FFFFF0u) | ((arg << 8) & 0x0F000000u);
            break;
        case GE_TEXSIZE0 + 6:
            g_ge.tex_lv_w[6] = 1u << (arg & 0xF);
            g_ge.tex_lv_h[6] = 1u << ((arg >> 8) & 0xF);
            if (g_ge.tex_lv_w[6] > 512) g_ge.tex_lv_w[6] = 512;
            if (g_ge.tex_lv_h[6] > 512) g_ge.tex_lv_h[6] = 512;
            break;
        case GE_TEXADDR0 + 7:
            g_ge.tex_lv_addr[7] = (g_ge.tex_lv_addr[7] & 0x0F000000u) | (arg & 0x00FFFFF0u);
            break;
        case GE_TEXBUFWIDTH0 + 7:
            g_ge.tex_lv_stride[7] = arg & 0x7FF;
            g_ge.tex_lv_addr[7]   = (g_ge.tex_lv_addr[7] & 0x00FFFFF0u) | ((arg << 8) & 0x0F000000u);
            break;
        case GE_TEXSIZE0 + 7:
            g_ge.tex_lv_w[7] = 1u << (arg & 0xF);
            g_ge.tex_lv_h[7] = 1u << ((arg >> 8) & 0xF);
            if (g_ge.tex_lv_w[7] > 512) g_ge.tex_lv_w[7] = 512;
            if (g_ge.tex_lv_h[7] > 512) g_ge.tex_lv_h[7] = 512;
            break;
        case GE_TEXADDR0:
            g_ge.tex_addr = (g_ge.tex_addr & 0x0F000000u) | (arg & 0x00FFFFF0u);
            break;
        case GE_TEXBUFWIDTH0:
            g_ge.tex_stride = arg & 0x7FF;
            g_ge.tex_addr   = (g_ge.tex_addr & 0x00FFFFF0u) | ((arg << 8) & 0x0F000000u);
            break;
        case GE_TEXSIZE0:
            /* log2 of each dimension, four bits each. Masking a whole byte
             * lets a stray high bit ask for a 1 << 200 texture. */
            /* 512 is the largest texture the hardware samples, whatever the
             * size field says. gpu/textures/size asks for 1024 up to 8192 and
             * every one of them reads texel 511 at its far edge -- so the
             * dimension saturates rather than wrapping or scaling. */
            g_ge.tex_w = 1u << (arg & 0xF);
            g_ge.tex_h = 1u << ((arg >> 8) & 0xF);
            if (g_ge.tex_w > 512) g_ge.tex_w = 512;
            if (g_ge.tex_h > 512) g_ge.tex_h = 512;
            break;
        case GE_TEXFORMAT:
            g_ge.tex_format = arg & 0xF;
            g_ge.tex_formats_seen |= 1u << (arg & 0xF);
            break;
        case GE_TEXLEVEL:
            g_ge.tex_lod_mode   = (int)(arg & 3);
            g_ge.tex_lod_bias16 = (int)(int8_t)((arg >> 16) & 0xFF);
            break;
        case GE_TEXLODSLOPE: g_ge.tex_lod_slope = ge_float(arg); break;
        case GE_TEXMODE:
            g_ge.tex_max_level = (int)((arg >> 16) & 7);
            g_ge.tex_swizzled = arg & 1;        /* bit 0 selects swizzled */
            break;
        case GE_CLUTFORMAT:
            /* The low two bits are the palette format; the rest is how a texel
             * indexes it -- shift, mask and a start offset. Keeping only the
             * format, which is what this did, samples entry (texel & 0xFF) of
             * whichever palette page happens to be first. */
            g_ge.clut_raw    = arg;
            g_ge.clut_format = arg & 3;
            break;
        case GE_CLUTADDR:
            g_ge.clut_addr = (g_ge.clut_addr & 0x0F000000u) | (arg & 0x00FFFFF0u);
            break;
        case GE_CLUTADDRUPPER:
            g_ge.clut_addr = (g_ge.clut_addr & 0x00FFFFF0u) | ((arg << 8) & 0x0F000000u);
            break;
        case GE_TEXFUNC:
            g_ge.tex_func   = arg & 7;
            g_ge.tex_tcc    = (arg >> 8) & 1;     /* RGBA: the texel's alpha takes part */
            g_ge.tex_double = (arg >> 16) & 1;    /* colour doubling */
            g_ge.tex_funcs_seen |= 1u << (arg & 7);
            break;
        case GE_TEXENVCOLOR:
            g_ge.tex_env = arg & 0xFFFFFFu;
            break;
        case GE_TEXFILTER:
            g_ge.tex_filter = arg & 0xFFFF;
            break;
        case GE_TEXMAPMODE:
            g_tl.tex_map_mode  = (int)(arg & 3);
            g_tl.tex_proj_mode = (int)((arg >> 8) & 3);
            break;
        case GE_DEPTHCLIPENABLE: g_tl.depth_clamp = (int)(arg & 1); break;
        case GE_LIGHTINGENABLE: g_tl.lighting = (int)(arg & 1); break;
        case GE_FOGENABLE: g_tl.fog_enable = (int)(arg & 1); break;
        case GE_FOG1:      g_tl.fog_end   = ge_float(arg); break;
        case GE_FOG2:      g_tl.fog_range = ge_float(arg); break;
        case GE_FOGCOLOR:  ge_colour3(arg, g_tl.fog_colour); g_tl.fog_colour_raw = arg & 0xFFFFFFu; break;
        case GE_IMM_VSCX: g_imm.x   = arg & 0xFFFFFFu; break;
        case GE_IMM_VSCY: g_imm.y   = arg & 0xFFFFFFu; break;
        case GE_IMM_VSCZ: g_imm.z   = arg & 0xFFFFu;   break;
        case GE_IMM_VTCS: case GE_IMM_VTCT: case GE_IMM_VTCQ: case GE_IMM_SCV: break;
        case GE_IMM_CV:   g_imm.rgb = arg & 0xFFFFFFu; break;
        case GE_IMM_FC:   g_imm.fog = arg & 0xFFu;     break;
        case GE_IMM_AP:   imm_vertex(arg);             break;
        case GE_LIGHTMODE:      g_tl.light_mode = (int)(arg & 1); break;
        case GE_MATERIALUPDATE: g_tl.mat_update = (int)(arg & 7); break;
        case GE_MATERIALEMISSIVE: ge_colour3(arg, g_tl.mat_emissive); break;
        case GE_AMBIENTCOLOR:     ge_colour3(arg, g_tl.mat_ambient);  break;
        case GE_MATERIALDIFFUSE:  ge_colour3(arg, g_tl.mat_diffuse);  break;
        case GE_MATERIALSPECULAR: ge_colour3(arg, g_tl.mat_specular); break;
        case GE_AMBIENTALPHA:     g_tl.mat_alpha = (int)(arg & 0xFF); break;
        case GE_MATERIALSPECCOEF: g_tl.mat_spec_coef = ge_float(arg); break;
        case GE_AMBIENTLIGHT:     ge_colour3(arg, g_tl.global_amb); break;
        case GE_AMBIENTLIGHTALPHA: break;   /* the global ambient's alpha does not reach a vertex */
        case GE_LIGHTENABLE0 + 0: g_tl.light[0].enable = (int)(arg & 1); break;
        case GE_LIGHTTYPE0 + 0:
            g_tl.light[0].kind = (int)(arg & 3);
            g_tl.light[0].type = (int)((arg >> 8) & 3);
            break;
        case GE_LIGHT0X + 0 * 3:     g_tl.light[0].pos[0]   = ge_float(arg); break;
        case GE_LIGHT0X + 0 * 3 + 1: g_tl.light[0].pos[1]   = ge_float(arg); break;
        case GE_LIGHT0X + 0 * 3 + 2: g_tl.light[0].pos[2]   = ge_float(arg); break;
        case GE_LIGHT0DIRX + 0 * 3:     g_tl.light[0].dir[0] = ge_float(arg); break;
        case GE_LIGHT0DIRX + 0 * 3 + 1: g_tl.light[0].dir[1] = ge_float(arg); break;
        case GE_LIGHT0DIRX + 0 * 3 + 2: g_tl.light[0].dir[2] = ge_float(arg); break;
        case GE_LIGHT0ATTEN0 + 0 * 3:     g_tl.light[0].atten[0] = ge_float(arg); break;
        case GE_LIGHT0ATTEN0 + 0 * 3 + 1: g_tl.light[0].atten[1] = ge_float(arg); break;
        case GE_LIGHT0ATTEN0 + 0 * 3 + 2: g_tl.light[0].atten[2] = ge_float(arg); break;
        case GE_LIGHT0EXPONENT + 0: g_tl.light[0].exponent = ge_float(arg); break;
        case GE_LIGHT0CUTOFF + 0:   g_tl.light[0].cutoff   = ge_float(arg); break;
        case GE_LIGHT0AMBIENT + 0 * 3:     ge_colour3(arg, g_tl.light[0].amb);  break;
        case GE_LIGHT0AMBIENT + 0 * 3 + 1: ge_colour3(arg, g_tl.light[0].dif);  break;
        case GE_LIGHT0AMBIENT + 0 * 3 + 2: ge_colour3(arg, g_tl.light[0].spec); break;
        case GE_LIGHTENABLE0 + 1: g_tl.light[1].enable = (int)(arg & 1); break;
        case GE_LIGHTTYPE0 + 1:
            g_tl.light[1].kind = (int)(arg & 3);
            g_tl.light[1].type = (int)((arg >> 8) & 3);
            break;
        case GE_LIGHT0X + 1 * 3:     g_tl.light[1].pos[0]   = ge_float(arg); break;
        case GE_LIGHT0X + 1 * 3 + 1: g_tl.light[1].pos[1]   = ge_float(arg); break;
        case GE_LIGHT0X + 1 * 3 + 2: g_tl.light[1].pos[2]   = ge_float(arg); break;
        case GE_LIGHT0DIRX + 1 * 3:     g_tl.light[1].dir[0] = ge_float(arg); break;
        case GE_LIGHT0DIRX + 1 * 3 + 1: g_tl.light[1].dir[1] = ge_float(arg); break;
        case GE_LIGHT0DIRX + 1 * 3 + 2: g_tl.light[1].dir[2] = ge_float(arg); break;
        case GE_LIGHT0ATTEN0 + 1 * 3:     g_tl.light[1].atten[0] = ge_float(arg); break;
        case GE_LIGHT0ATTEN0 + 1 * 3 + 1: g_tl.light[1].atten[1] = ge_float(arg); break;
        case GE_LIGHT0ATTEN0 + 1 * 3 + 2: g_tl.light[1].atten[2] = ge_float(arg); break;
        case GE_LIGHT0EXPONENT + 1: g_tl.light[1].exponent = ge_float(arg); break;
        case GE_LIGHT0CUTOFF + 1:   g_tl.light[1].cutoff   = ge_float(arg); break;
        case GE_LIGHT0AMBIENT + 1 * 3:     ge_colour3(arg, g_tl.light[1].amb);  break;
        case GE_LIGHT0AMBIENT + 1 * 3 + 1: ge_colour3(arg, g_tl.light[1].dif);  break;
        case GE_LIGHT0AMBIENT + 1 * 3 + 2: ge_colour3(arg, g_tl.light[1].spec); break;
        case GE_LIGHTENABLE0 + 2: g_tl.light[2].enable = (int)(arg & 1); break;
        case GE_LIGHTTYPE0 + 2:
            g_tl.light[2].kind = (int)(arg & 3);
            g_tl.light[2].type = (int)((arg >> 8) & 3);
            break;
        case GE_LIGHT0X + 2 * 3:     g_tl.light[2].pos[0]   = ge_float(arg); break;
        case GE_LIGHT0X + 2 * 3 + 1: g_tl.light[2].pos[1]   = ge_float(arg); break;
        case GE_LIGHT0X + 2 * 3 + 2: g_tl.light[2].pos[2]   = ge_float(arg); break;
        case GE_LIGHT0DIRX + 2 * 3:     g_tl.light[2].dir[0] = ge_float(arg); break;
        case GE_LIGHT0DIRX + 2 * 3 + 1: g_tl.light[2].dir[1] = ge_float(arg); break;
        case GE_LIGHT0DIRX + 2 * 3 + 2: g_tl.light[2].dir[2] = ge_float(arg); break;
        case GE_LIGHT0ATTEN0 + 2 * 3:     g_tl.light[2].atten[0] = ge_float(arg); break;
        case GE_LIGHT0ATTEN0 + 2 * 3 + 1: g_tl.light[2].atten[1] = ge_float(arg); break;
        case GE_LIGHT0ATTEN0 + 2 * 3 + 2: g_tl.light[2].atten[2] = ge_float(arg); break;
        case GE_LIGHT0EXPONENT + 2: g_tl.light[2].exponent = ge_float(arg); break;
        case GE_LIGHT0CUTOFF + 2:   g_tl.light[2].cutoff   = ge_float(arg); break;
        case GE_LIGHT0AMBIENT + 2 * 3:     ge_colour3(arg, g_tl.light[2].amb);  break;
        case GE_LIGHT0AMBIENT + 2 * 3 + 1: ge_colour3(arg, g_tl.light[2].dif);  break;
        case GE_LIGHT0AMBIENT + 2 * 3 + 2: ge_colour3(arg, g_tl.light[2].spec); break;
        case GE_LIGHTENABLE0 + 3: g_tl.light[3].enable = (int)(arg & 1); break;
        case GE_LIGHTTYPE0 + 3:
            g_tl.light[3].kind = (int)(arg & 3);
            g_tl.light[3].type = (int)((arg >> 8) & 3);
            break;
        case GE_LIGHT0X + 3 * 3:     g_tl.light[3].pos[0]   = ge_float(arg); break;
        case GE_LIGHT0X + 3 * 3 + 1: g_tl.light[3].pos[1]   = ge_float(arg); break;
        case GE_LIGHT0X + 3 * 3 + 2: g_tl.light[3].pos[2]   = ge_float(arg); break;
        case GE_LIGHT0DIRX + 3 * 3:     g_tl.light[3].dir[0] = ge_float(arg); break;
        case GE_LIGHT0DIRX + 3 * 3 + 1: g_tl.light[3].dir[1] = ge_float(arg); break;
        case GE_LIGHT0DIRX + 3 * 3 + 2: g_tl.light[3].dir[2] = ge_float(arg); break;
        case GE_LIGHT0ATTEN0 + 3 * 3:     g_tl.light[3].atten[0] = ge_float(arg); break;
        case GE_LIGHT0ATTEN0 + 3 * 3 + 1: g_tl.light[3].atten[1] = ge_float(arg); break;
        case GE_LIGHT0ATTEN0 + 3 * 3 + 2: g_tl.light[3].atten[2] = ge_float(arg); break;
        case GE_LIGHT0EXPONENT + 3: g_tl.light[3].exponent = ge_float(arg); break;
        case GE_LIGHT0CUTOFF + 3:   g_tl.light[3].cutoff   = ge_float(arg); break;
        case GE_LIGHT0AMBIENT + 3 * 3:     ge_colour3(arg, g_tl.light[3].amb);  break;
        case GE_LIGHT0AMBIENT + 3 * 3 + 1: ge_colour3(arg, g_tl.light[3].dif);  break;
        case GE_LIGHT0AMBIENT + 3 * 3 + 2: ge_colour3(arg, g_tl.light[3].spec); break;

        case GE_FRAMEBUFPIXFORMAT:
            g_ge.fbfmt = arg & 3;
            ge_note_target(ge_fb_address(g_ge.fbp), g_ge.fbw, g_ge.fbfmt);
            psp_render_current()->set_target(ge_fb_address(g_ge.fbp), g_ge.fbw, (int)g_ge.fbfmt);
            break;
        case GE_SCISSOR1: g_ge.sc_x0 = (int)(arg & 0x3FF); g_ge.sc_y0 = (int)((arg >> 10) & 0x3FF); break;
        case GE_SCISSOR2:
            g_ge.sc_x1 = (int)(arg & 0x3FF); g_ge.sc_y1 = (int)((arg >> 10) & 0x3FF);
            g_ge.sc_set = 1;
            psp_render_current()->set_scissor(g_ge.sc_x0, g_ge.sc_y0, g_ge.sc_x1, g_ge.sc_y1);
            break;
        case GE_TGENMATRIXNUMBER: g_tl.tgen_n = (int)(arg & 0xF); break;
        case GE_TGENMATRIXDATA:
            if (g_tl.tgen_n < 12) g_tl.tgen[g_tl.tgen_n++] = ge_float(arg);
            break;
        case GE_TEXSCALEU:  g_ge.tex_scale_u  = ge_float(arg); break;
        case GE_TEXSCALEV:  g_ge.tex_scale_v  = ge_float(arg); break;
        case GE_TEXOFFSETU: g_ge.tex_offset_u = ge_float(arg); break;
        case GE_TEXOFFSETV: g_ge.tex_offset_v = ge_float(arg); break;
        case GE_TEXWRAP:
            g_ge.tex_wrap = arg & 0xFFFF;
            break;
        case GE_LOADCLUT:
            g_ge.clut_loads++;
            break;

        case GE_TEXTUREMAPENABLE: g_ge.tex_enable = arg & 1; break;

        case GE_ALPHABLENDENABLE: g_tl.blend.enable     = (int)(arg & 1); break;
        case GE_ALPHATESTENABLE:  g_tl.blend.alpha_test = (int)(arg & 1); break;
        case GE_STENCILTESTENABLE: g_tl.blend.stencil_test = (int)(arg & 1); break;
        case GE_STENCILTEST:
            g_tl.blend.stencil_func = (int)(arg & 7);
            g_tl.blend.stencil_ref  = (int)((arg >> 8) & 0xFF);
            g_tl.blend.stencil_mask = (int)((arg >> 16) & 0xFF);
            break;
        case GE_STENCILOP:
            g_tl.blend.op_sfail = (int)(arg & 7);
            g_tl.blend.op_zfail = (int)((arg >> 8) & 7);
            g_tl.blend.op_zpass = (int)((arg >> 16) & 7);
            break;
        case GE_BLENDMODE:
            g_tl.blend.src = (int)(arg & 0xF);
            g_tl.blend.dst = (int)((arg >> 4) & 0xF);
            g_tl.blend.eq  = (int)((arg >> 8) & 7);
            break;
        case GE_BLENDFIXEDA: g_tl.blend.fixa = arg & 0xFFFFFFu; break;
        case GE_BLENDFIXEDB: g_tl.blend.fixb = arg & 0xFFFFFFu; break;
        case GE_ALPHATEST:
            g_tl.blend.alpha_func = (int)(arg & 7);
            g_tl.blend.alpha_ref  = (int)((arg >> 8) & 0xFF);
            g_tl.blend.alpha_mask = (int)((arg >> 16) & 0xFF);
            break;
        case GE_ZTESTENABLE:   g_tl.ztest_enable = (int)(arg & 1); break;
        case GE_ZTEST:         g_tl.ztest_func   = (int)(arg & 7); break;
        case GE_ZWRITEDISABLE: g_tl.zwrite_off   = (int)(arg & 1); break;
        /* Clear mode turns the draw into a blit of the clear values: the depth
         * test is bypassed and depth is written only when the Z bit is set. */
        case GE_CLEARMODE:
            if (drawlog_aux())
                fprintf(stderr, "clr: CLEARMODE arg=%06X  enable %d  colour %d "
                                "alpha %d  depth %d\n",
                        arg, (int)(arg & 1), (int)((arg >> 8) & 1),
                        (int)((arg >> 9) & 1), (int)((arg >> 10) & 1));
            g_tl.clear_mode   = (int)(arg & 1);
            g_tl.clear_stencil = (int)((arg >> 9) & 1);
            g_tl.clear_colour = (int)((arg >> 8) & 1);
            g_tl.clear_z      = (int)((arg >> 10) & 1);
            break;

        case GE_TRANSFERSRC:    g_ge.xfer_src    = arg; break;
        case GE_TRANSFERSRCW:   g_ge.xfer_srcw   = arg; break;
        case GE_TRANSFERDST:    g_ge.xfer_dst    = arg; break;
        case GE_TRANSFERDSTW:   g_ge.xfer_dstw   = arg; break;
        case GE_TRANSFERSRCPOS: g_ge.xfer_srcpos = arg; break;
        case GE_TRANSFERDSTPOS: g_ge.xfer_dstpos = arg; break;
        case GE_TRANSFERSIZE:   g_ge.xfer_size   = arg; break;
        case GE_TRANSFERSTART:
            g_ge.xfer_start = arg;
            do_block_transfer();
            break;

        case GE_FBP:
            g_ge.fbp = (g_ge.fbp & 0xFF000000u) | arg;
            ge_note_target(ge_fb_address(g_ge.fbp), g_ge.fbw, g_ge.fbfmt);
            psp_render_current()->set_target(ge_fb_address(g_ge.fbp), g_ge.fbw, (int)g_ge.fbfmt);
            break;
        case GE_FBW:
            g_ge.fbw = arg & 0xFFFF;
            g_ge.fbp = (g_ge.fbp & 0x00FFFFFFu) | ((arg & 0xFF0000) << 8);
            ge_note_target(ge_fb_address(g_ge.fbp), g_ge.fbw, g_ge.fbfmt);
            psp_render_current()->set_target(ge_fb_address(g_ge.fbp), g_ge.fbw, (int)g_ge.fbfmt);
            break;

        /* The depth buffer: low 24 bits of the address in ZBP, the high byte
         * and the stride in ZBW, the same split as FBP/FBW. */
        case GE_ZBP:
            g_ge.zbp = (g_ge.zbp & 0xFF000000u) | arg;
            psp_render_set_depth_buffer(g_ge.zbp, g_ge.zbw);
            break;
        case GE_ZBW:
            g_ge.zbw = arg & 0xFFFF;
            g_ge.zbp = (g_ge.zbp & 0x00FFFFFFu) | ((arg & 0xFF0000) << 8);
            psp_render_set_depth_buffer(g_ge.zbp, g_ge.zbw);
            break;

        case GE_VADDR: g_ge.vaddr = (q->base | (arg & 0xFFFFFF)); break;
        case GE_IADDR: g_ge.iaddr = (q->base | (arg & 0xFFFFFF)); break;

        default:
            /* A real state command we do not decode individually. Counted --
             * and, for the first few, named. "240 commands not individually
             * decoded" hides whether the game is configuring a draw or just
             * poking state, which is the difference between a rendering bug
             * and a game that has not asked to render yet. */
            if (g_ge.unknown < 64)
                fprintf(stderr, "  GE cmd 0x%02X arg 0x%06X\n", cmd, arg);
            g_ge.unknown++;
            break;
        }
        if (g_prof_on > 0) { g_prof_op[cmd] += ge_prof_now() - _c0; g_prof_opn[cmd]++; }
    }
}

/* ---- the calls ----------------------------------------------------------- */

static ge_queue *find_queue(uint32_t id) {
    for (int i = 0; i < MAX_QUEUES; i++)
        if (g_queue[i].used && g_queue[i].id == id) return &g_queue[i];
    return NULL;
}


static ge_queue *oldest_pending(void);
static int  ge_busy(void);
static void ge_release(int was_busy);
static void ge_execute(void);
static void ge_kick(void);

static void enqueue(int head) {
    /* (list, stall, cbid, arg) */
    ge_queue *q = NULL;
    for (int i = 0; i < MAX_QUEUES; i++) if (!g_queue[i].used) { q = &g_queue[i]; break; }

    /* A slot is only worth keeping while its list can still be referred to. A
     * finished list is kept until DrawSync(WAIT) retires it, so that a late
     * sceGeListUpdateStallAddr or ListSync can still resolve its id, but it
     * is holding a slot it no longer needs -- so when
     * the pool is full, the oldest finished list is what to give up. Ids come
     * from a monotonic counter and are never reused, so a stale id resolves to
     * "unknown uid" rather than to somebody else's list.
     *
     * Without this nothing ever clears `used` and the pool is consumed once,
     * for the life of the process. The game gets MAX_QUEUES display lists in
     * total and every enqueue after that is refused with NO_MEMORY: measured
     * here at 8 accepted and 204 refused during the intro. That is invisible
     * from the outside -- the GE summary counts lists that *ran*, the call
     * histogram is a top-N, and a non-zero error escapes the zero-return ring
     * -- so it reads as "the game submits no geometry" when the truth is that
     * the geometry was submitted and turned away. */
    if (!q)
        for (int i = 0; i < MAX_QUEUES; i++)
            if (g_queue[i].done && (!q || g_queue[i].id < q->id)) q = &g_queue[i];

    if (!q) { psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }

    memset(q, 0, sizeof *q);
    q->id    = g_next_id++;
    /* Addresses are cut to 28 bits, as the GE sees them. libgu passes the
     * list and stall with the uncached bit (0x40000000) set, but JUMP and CALL
     * targets built from BASE carry no such bit, so a list that had jumped
     * never met its stall and ran on into stale words: each geprobe frame
     * showed the previous scene until this (hardware run, 2026-09-28). */
    q->list  = psp_arg(0) & 0x0FFFFFFCu;
    q->stall = psp_arg(1) & 0x0FFFFFFCu;
    /* Recorded here, at submission, and not after the queue has been serviced:
     * the deferred GE runs a list as words are released, so by the end of
     * enqueue q->list has advanced and pointing a replay at it runs off into
     * whatever follows -- which is what produced a capture that executed
     * 2^23 commands and never found an END. */
    cap_note_list(q);
    q->used  = 1;
    q->cbid  = (int)psp_arg(2);
    (void)head;

    /* The GE starts on the queue here and runs it for GE_KICK_WINDOW_US
     * of its own time before EnQueue returns (see g_ge_t): geprobe step 26
     * and geprobe 5 steps 54-56 (fw 6.60) enqueue a seven-word list with no
     * stall, and both its SIGNALs and its FINISH have called their handlers
     * before sceGeListEnQueue returns; geprobe 5 step 58's 200 full-screen
     * sprites are still running when it returns (ListSync(peek) 2), with
     * only the first SIGNAL's handler run. The rest runs at later firmware
     * calls, or in the Sync that waits for it. A list queued behind another
     * waits its turn. A libgu DIRECT list is enqueued with its stall at its
     * start and runs nothing yet.
     *
     * UpdateStallAddr releases words that run from the next firmware call
     * on. That the tail of one list has run before the next firmware call
     * is load-bearing: pspgu reuses one list buffer across tests, so a tail
     * left for Sync would run the new list's words under the old id
     * (simple.prx's 8 grey pixels), and the new list's EnQueue is such a
     * call.
     *
     * Sync(WAIT) on a still-pending list yields once -- an equal-priority
     * thread runs there on hardware, which is pspautotests' checkpoint [r]
     * -- then completes; an already-done list returns immediately ([x]).
     * In practice pspgu's Finish releases the stall before Sync, so tiny
     * DIRECT lists are already done at Sync and read [x] here. Hardware
     * reads [r] for blend and [x] for fog on the identical GE pattern; the
     * split correlates with the checkpoint window's DMA/cache traffic
     * (557KB vs 128B per test), not with GE state, so no GE-Sync model
     * reproduces both and none is attempted -- that prefix is timing noise
     * (strip it for gpu verdicts) while WAIT/NOWAIT blocking is honoured.
     * NOWAIT reports the list's status as it is: geprobe 6 steps 79 and 80
     * (fw 6.60) peek 2 on a list held at its stall, for both ListSync and
     * DrawSync, even 10 ms later. Tiny lists that pspgu's helpers poll
     * have finished by then because the kick window ran them.
     *
     * Not yet modelled: DeQueue and GetCmd/GetMtx/GetStack (unregistered,
     * so they answer 0), and the queue-full code (hardware 0x80000022; a
     * full pool here answers NO_MEMORY). EnQueueHead and sceGeBreak are. */
    g_ge.lists++;
    /* The call itself takes time: step 81's first SIGNAL, the list's first
     * word, calls its handler 41-51 microseconds after the call is made,
     * and the call returns 11-17 after that (GE_ENQUEUE_US, then the kick
     * window). */
    const uint64_t start = psp_clock_peek() + GE_ENQUEUE_US;
    psp_clock_advance_to(start);
    const int first = q == oldest_pending();
    if (first) ge_release(0);
    /* Drawn now, if the lists ahead of it have been; seen on the clock. */
    ge_execute();
    if (first) ge_kick();
    psp_clock_advance_to(start + GE_KICK_WINDOW_US);
    psp_ret(q->id);
}

static void hle_ListEnQueue(void)     { enqueue(0); }
static void hle_ListEnQueueHead(void) { enqueue(1); }

static void hle_ListUpdateStallAddr(void) {
    ge_queue *q = find_queue(psp_arg(0));
    if (!q) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_UID); return; }
    const int busy = ge_busy();
    q->stall = psp_arg(1) & 0x0FFFFFFCu;
    /* The words run inside the call, for the kick window, as EnQueue's do:
     * geprobe 6 steps 79 and 80 (fw 6.60) release a SIGNAL and a FINISH
     * from a stall, and both handlers have run by the return. libgu's
     * sceGuFinish still sees its finish handler run later, in sceGuSync
     * (geprobe 5 step 50), because the FINISH waits for the drawing before
     * it. */
    if (!q->done) {
        ge_release(busy);
        ge_execute();
        if (q == oldest_pending()) ge_kick();
    }
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* List/Draw status, as pspautotests' status_str reads it: 0 DONE, 1 QUEUED,
 * 2 DRAWING, 4 PAUSED (geprobe 5 step 57, fw 6.60). A list held at its
 * stall reads 2, not 3 (geprobe 6 steps 79 and 80). BREAK is not
 * modelled. */
#define GE_SYNC_DONE 0
#define GE_SYNC_WAIT 0
static ge_queue *oldest_pending(void) {
    ge_queue *best = NULL;
    for (int i = 0; i < MAX_QUEUES; i++)
        if (g_queue[i].used && !g_queue[i].done &&
            (!best || g_queue[i].id < best->id)) best = &g_queue[i];
    return best;
}

static int list_status(ge_queue *q) {
    if (!q || !q->used) return 0x80000100;
    if (q->done) return GE_SYNC_DONE;
    if (q->paused) return 4;                                   /* PAUSED */
    if (q != oldest_pending()) return 1;                       /* QUEUED */
    /* DRAWING, waiting at its stall too: geprobe 6 steps 79 and 80 (fw
     * 6.60) peek 2 on a list stalled at its start and after a SIGNAL, as
     * EnQueue returns and 10 ms later. */
    return 2;
}

/* The walk's own queue: lists it has not run to their end, oldest first. */
static ge_queue *oldest_xpending(void) {
    ge_queue *best = NULL;
    for (int i = 0; i < MAX_QUEUES; i++)
        if (g_queue[i].used && !g_queue[i].xdone &&
            (!best || g_queue[i].id < best->id)) best = &g_queue[i];
    return best;
}

/* Walk the lists, in id order -- the order hardware runs them -- as far as
 * their words are released: until one stalls, pauses or hangs, the queue is
 * empty, or g_ev is full. Nothing here waits on the clock. */
static void ge_execute(void) {
    if (g_ge_walking) return;
    g_ge_xfull = 0;
    for (;;) {
        ge_queue *q = oldest_xpending();
        if (!q) break;
        run_list(q);
        if (!q->xdone) break;     /* stalled, paused or hung: later lists wait */
    }
}

/* Whether the GE is still busy, as the guest's clock sees it: there are
 * events the timeline has not reached, or the walk has words it could run. */
static int ge_busy(void) {
    if (g_ev_n) return 1;
    const ge_queue *q = oldest_xpending();
    return q && !q->xpaused && !q->hung && !(q->stall && q->list == q->stall);
}

/* Words were released (EnQueue, a stall update, Continue). An idle GE starts
 * on them now; a busy one gets to them after what it has. `was_busy` is
 * ge_busy() from before the release. */
static void ge_release(int was_busy) {
    const uint64_t now = ge_now();
    if (!was_busy && g_ge_t < now) g_ge_t = now;
}

static uint64_t g_tl_done_at;  /* the GE's time at the last list the timeline saw end */

/* The timeline: what the walk did, applied in order as far as GE time
 * `limit` -- the guest sees each command that started before it -- and for
 * lists up to id `through`. A handler runs as it is reached (the clock moved
 * up to it when `follow`), or is held when `defer`, with interrupts off, or
 * inside another handler, and the timeline then waits on it. */
static void ge_advance(uint64_t limit, int defer, int follow, uint32_t through) {
    if (g_tl_in) return;
    if (ge_deliver(!defer)) return;   /* still waiting on a handler */
    g_tl_in = 1;
    for (;;) {
        if (g_ge_xfull && g_ev_n + 2 <= GE_EVENTS) ge_execute();
        if (!g_ev_n) break;
        const ge_event e = *ev_at(0);
        if (e.qid > through || e.start >= limit) break;
        if (e.kind == GE_EV_HANDLER && ev_has_handler(&e) &&
            (defer || !psp_intr_enabled() || psp_interrupt_in_handler())) {
            /* With the CPU's interrupts suspended, or inside another
             * interrupt's handler, the GE's interrupt waits: the handler is
             * held and runs at the first completed firmware call after the
             * resume (the game's callback probe, scenario 4, recorded under
             * an emulator; not measured on a PSP). */
            g_ev_held = 1;
            break;
        }
        ev_pop();
        switch (e.kind) {
        case GE_EV_DONE:
            if (ev_live(&e)) e.q->done = 1;
            g_tl_done_at = e.at;
            break;
        case GE_EV_PAUSE:
            if (ev_live(&e)) { e.q->paused = 1; e.q->psig = 0; }
            break;
        case GE_EV_PSIG:
            if (ev_live(&e)) e.q->psig = 1;
            break;
        case GE_EV_HANDLER:
            if (!ev_has_handler(&e)) break;
            if (follow) ge_clock_to(e.at);
            ge_callback(e.cbid, e.finish, e.code, e.pc);
            break;
        }
    }
    g_tl_in = 0;
}

/* Inside EnQueue, UpdateStallAddr and Continue: the GE runs for the kick
 * window, handlers and all, the clock following it to each handler. */
static void ge_kick(void) {
    if (g_ge_in_cb) return;
    ge_advance(ge_now() + GE_KICK_WINDOW_US * GE_UNITS_PER_US, 0, 1, UINT32_MAX);
}

/* A Sync that waits: the caller is blocked until the GE is through with the
 * lists up to id `through` (or stalled), so the clock follows it to each
 * handler and to where it stops. Its front end is then idle at the present
 * moment, waiting for words; the back end may still be drawing what came
 * before a stall. Lists behind `through` that the walk has already run keep
 * their times. */
static void ge_wait(uint32_t through) {
    if (g_tl_in) return;
    ge_execute();
    g_tl_done_at = 0;
    ge_advance(UINT64_MAX, 0, 1, through);
    uint64_t stop = g_tl_done_at;
    const ge_queue *q = oldest_pending();
    if (g_ev_held) stop = ev_at(0)->at;
    else if (q && q->id <= through) stop = q->t_x;
    ge_clock_to(stop);
    if (g_ge_t <= stop) g_ge_t = ge_now();
}

/* At every firmware call (hle.c): the guest catches up with the GE, to the
 * present. */
void psp_ge_tick(void) {
    if (!g_ev_n || g_tl_in || g_ge_in_cb) return;
    ge_advance(ge_now(), 1, 0, UINT32_MAX);
}

/* From the scheduler when no thread can run, up to `until_us`, the next
 * deadline (0: none): the GE goes on as it would while the CPU idles, and
 * the first handler it reaches by then runs, the clock moved up to it, so a
 * thread waiting for something a GE handler provides is not left stranded.
 * Returns whether a handler ran; the scheduler then looks again. */
int psp_ge_idle_run(uint64_t until_us) {
    if (!g_ev_n || g_tl_in || g_ge_in_cb) return 0;
    const uint64_t until = until_us ? until_us * GE_UNITS_PER_US : UINT64_MAX;
    if (!g_ev_held) ge_advance(until, 1, 0, UINT32_MAX);
    if (g_ev_held && ev_at(0)->at <= until) { ge_deliver(1); return 1; }
    return 0;
}

/* Flush for paths that present or inspect pixels without going through Sync
 * -- SetFrameBuf's present hook and the unit tests. No yield: this is not a
 * wait, and it must be safe where no thread holds the token. */
/* ---- display-list capture ---------------------------------------------------
 *
 * One frame of GE work, written to a file so it can be replayed into any
 * backend. It exists because comparing backends on a running game does not
 * work: a windowed GL run is paced in real time and a headless software run is
 * not, so their frame numbering drifts and "the same frame" stops meaning
 * anything (findings item 56). A capture removes time from the comparison --
 * the same lists, the same memory, the same starting state, twice.
 *
 * What has to be in it: the GE's register state as it stood at the *start* of
 * the frame, because commands are differential and a frame inherits what came
 * before; guest memory, because the lists are pointers into it and so are the
 * vertices, textures and palettes they name; and the lists themselves, which
 * are only addresses since their words live in that memory.
 *
 * Memory is snapshotted at the END of the frame rather than the start. Vertex
 * data a frame builds is still there when it ends, whereas at the start it is
 * the previous frame's. A game that overwrote its own vertex buffer within one
 * frame would defeat this, which is a real thing to look for if a replay ever
 * disagrees with the run it came from. */

#define GE_CAP_MAGIC  0x50414347u    /* "GCAP" */
#define GE_CAP_VER    2

typedef struct {
    uint32_t magic, version;
    uint32_t state_bytes, n_lists;
    uint32_t ram_base, ram_bytes;
    uint32_t vram_base, vram_bytes;
    /* The module image is a third region and not an optional one: this game's
     * display lists live *inside* it. psp_mem_ptr checks the module before RAM
     * or VRAM, and a capture without it replays a list address that resolves
     * to nothing -- which read as a list that never reached its END and ran
     * to the interpreter's command budget. */
    uint32_t mod_base, mod_bytes;
} ge_cap_header;

typedef struct { uint32_t list, stall, base; } ge_cap_list;

enum { GE_CAP_MAX_LISTS = 4096 };
static ge_cap_list g_cap_lists[GE_CAP_MAX_LISTS];
static int      g_cap_n;
static int      g_cap_frame = -1;    /* frame to capture, -1 = off */
static int      g_cap_seen;          /* frames elapsed */
static int      g_cap_arming;        /* recording this frame */
static int      g_cap_snapped;       /* memory image taken */
static uint64_t g_cap_cmd0;          /* commands executed when arming */
static uint64_t g_cap_min_cmds;      /* skip frames smaller than this */
static uint64_t g_cap_min_mean;      /* skip dark display buffers, 0..255 RGB */
static uint64_t g_cap_fb_sum, g_cap_fb_samples; /* accepted candidate */
static uint8_t *g_cap_state;         /* state as of the frame's start */
static uint8_t *g_cap_ram, *g_cap_vram, *g_cap_mod;
static uint32_t g_cap_mod_base, g_cap_mod_size;
static const char *g_cap_path;
/* A scene suite uses the same pad-poll timebase as its input recording. Each
 * requested poll selects the first complete, qualifying frame starting at or
 * after it. The original one-shot FRAME selector remains available. */
enum { GE_CAP_MAX_POLLS = 64 };
static uint32_t g_cap_polls[GE_CAP_MAX_POLLS], g_cap_start_poll;
static int g_cap_poll_count, g_cap_poll_index;

size_t psp_ge_state_size(void) { return sizeof g_tl + sizeof g_ge; }

void psp_ge_state_save(void *buf) {
    memcpy(buf, &g_tl, sizeof g_tl);
    memcpy((uint8_t *)buf + sizeof g_tl, &g_ge, sizeof g_ge);
}

/* Push the current registers at the backend.
 *
 * Loading state restores what the GE believes; it does not tell the backend,
 * which learns the target and the scissor only when a register is *written*.
 * A replay that skips this draws its pixels into whatever the backend still
 * had -- for a fresh one, address zero -- and reports a million pixels written
 * while every buffer reads black, which is precisely what it did. */
void psp_ge_sync_backend(void) {
    const psp_render_backend *be = psp_render_current();
    be->set_target(ge_fb_address(g_ge.fbp), g_ge.fbw, (int)g_ge.fbfmt);
    be->set_scissor(g_ge.sc_x0, g_ge.sc_y0, g_ge.sc_x1, g_ge.sc_y1);
    /* The depth buffer lives in VRAM where ZBP puts it; a replay that left
     * it at the reset address wrote its depth over the frame being drawn. */
    psp_render_set_depth_buffer(g_ge.zbp, g_ge.zbw);
}

void psp_ge_state_load(const void *buf) {
    memcpy(&g_tl, buf, sizeof g_tl);
    g_bone16_ok = 0;
    memcpy(&g_ge, (const uint8_t *)buf + sizeof g_tl, sizeof g_ge);
    /* The counters travel with the registers because they share a struct;
     * a replay should report what *it* drew, not what the capture did. */
    g_ge.prims[0] = g_ge.prims[1] = g_ge.prims[2] = g_ge.prims[3] = 0;
    g_ge.prims[4] = g_ge.prims[5] = g_ge.prims[6] = g_ge.prims[7] = 0;
    g_ge.vertices = g_ge.lists = g_ge.finishes = g_ge.commands = 0;
}

/* What a save state keeps of the GE (psprecomp/state.h): the lists, the
 * timeline and its events, the callbacks and every register. A state is
 * saved between firmware calls, so no walk is in progress. */
static int ge_load(psp_state_reader *r, char *why, size_t size) {
    (void)r; (void)why; (void)size;
    /* The events' lists are kept with them: their pointers move as one. */
    for (unsigned i = 0; i < GE_EVENTS; i++)
        if (g_ev[i].q) g_ev[i].q = (ge_queue *)((char *)g_ev[i].q + psp_state_delta());
    /* Caches of the registers, rebuilt from them as needed. */
    g_bone16_ok = 0;
    memset(&g_light_eye_from, 0, sizeof g_light_eye_from);
    g_backend_stale = 1;
    return 0;
}

static void ge_keep(void) {
    static const psp_state_part part = { .name = "ge", .load = ge_load };
    PSP_STATE_KEEP(g_queue);
    PSP_STATE_KEEP(g_ge_hang);
    PSP_STATE_KEEP(g_ge_t);
    PSP_STATE_KEEP(g_ge_back);
    PSP_STATE_KEEP(g_ge_fifo);
    PSP_STATE_KEEP(g_ge_fifo_i);
    PSP_STATE_KEEP(g_ev);
    PSP_STATE_KEEP(g_ev_head);
    PSP_STATE_KEEP(g_ev_n);
    PSP_STATE_KEEP(g_ev_held);
    PSP_STATE_KEEP(g_ge_xfull);
    PSP_STATE_KEEP(g_ge_cb);
    PSP_STATE_KEEP(g_next_id);
    PSP_STATE_KEEP(g_tl);
    PSP_STATE_KEEP(g_ge);
    PSP_STATE_KEEP(g_imm);
    PSP_STATE_KEEP(g_ge_owner);
    psp_state_register(&part);
}

static void cap_init(void) {
    static int looked;
    if (looked) return;
    looked = 1;
    g_cap_path = getenv("PSPRECOMP_GE_CAPTURE");
    if (g_cap_path && !*g_cap_path) g_cap_path = NULL;
    const char *f = getenv("PSPRECOMP_GE_CAPTURE_FRAME");
    g_cap_frame = (g_cap_path && f && *f) ? atoi(f) : (g_cap_path ? 1 : -1);
    const char *polls = getenv("PSPRECOMP_GE_CAPTURE_POLLS");
    if (g_cap_path && polls && *polls) {
        const char *p = polls;
        for (;;) {
            char *end;
            errno = 0;
            const unsigned long value = strtoul(p, &end, 10);
            if (*p < '0' || *p > '9' || end == p || errno == ERANGE ||
                value > UINT32_MAX || g_cap_poll_count == GE_CAP_MAX_POLLS ||
                (g_cap_poll_count && value <= g_cap_polls[g_cap_poll_count - 1]) ||
                (*end && *end != ',')) {
                fprintf(stderr, "ge: invalid PSPRECOMP_GE_CAPTURE_POLLS; "
                                "expected up to 64 increasing poll numbers\n");
                g_cap_poll_count = 0;
                g_cap_frame = -1;
                return;
            }
            g_cap_polls[g_cap_poll_count++] = (uint32_t)value;
            if (!*end) break;
            p = end + 1;
        }
        g_cap_frame = 1;       /* POLLS takes precedence over FRAME */
    }
    /* Frames are not equal: this game's compositing frames run a few hundred
     * commands and the ones that draw the room run thousands, and picking by
     * number lands on whichever happens to be there. This says "the first
     * frame at or after the number that is actually big", which is how you ask
     * for a frame with a scene in it. */
    const char *m = getenv("PSPRECOMP_GE_CAPTURE_MINCMDS");
    g_cap_min_cmds = (m && *m) ? strtoull(m, NULL, 0) : 0;
    /* A large frame at a scene transition can still be the wrong specimen.
     * This game's first 10k-command hangar frame starts from two deliberately
     * black display buffers and ends in a destination-colour doubling pass;
     * replaying it from black is therefore correctly black. Let a capture ask
     * for an already-populated display buffer as well as a command budget. */
    const char *p = getenv("PSPRECOMP_GE_CAPTURE_MINMEAN");
    g_cap_min_mean = (p && *p) ? strtoull(p, NULL, 0) : 0;
    if (g_cap_min_mean > 255) g_cap_min_mean = 255;
}

static void cap_note_list(const ge_queue *q) {
    if (!g_cap_arming || g_cap_n >= GE_CAP_MAX_LISTS) return;
    g_cap_lists[g_cap_n].list  = q->list;
    /* Deliberately no stall. At enqueue the game has usually released nothing
     * yet, so the stall equals the start and a replay of it executes zero
     * words -- which is exactly what the first capture produced. By the time
     * the memory snapshot is taken, at the end of the frame, the list is
     * complete in that memory, so the replay should run it to its own END. */
    g_cap_lists[g_cap_n].stall = 0;
    g_cap_lists[g_cap_n].base  = q->base;

    g_cap_n++;
}

/* The moment a list is worth snapshotting memory at: when it has just run to
 * its own END.
 *
 * Neither obvious moment works. At submission the list is usually incomplete,
 * because the GE is stall-streamed -- the CPU releases words as it writes them,
 * so a snapshot then holds a few hundred commands of a frame that will run ten
 * thousand, which is exactly what the first attempt captured. At the end of the
 * frame the list is complete but this game has reused the buffer for the next
 * one. Completion is the only point where the words are all present and none
 * has been overwritten, and it is where the vertices and textures the list
 * names are guaranteed to be live too, because the GE has just read them. */
static void cap_snapshot_memory(void) {
    if (!g_cap_arming || g_cap_snapped || g_cap_n == 0) return;
    g_cap_snapped = 1;
    if (!g_cap_ram)  g_cap_ram  = malloc(PSP_RAM_SIZE);
    if (!g_cap_vram) g_cap_vram = malloc(PSP_VRAM_SIZE);
    const void *r = psp_mem_ptr(PSP_RAM_BASE,  PSP_RAM_SIZE);
    const void *v = psp_mem_ptr(PSP_VRAM_BASE, PSP_VRAM_SIZE);
    if (g_cap_ram  && r) memcpy(g_cap_ram,  r, PSP_RAM_SIZE);
    if (g_cap_vram && v) memcpy(g_cap_vram, v, PSP_VRAM_SIZE);
    psp_mem_module_region(&g_cap_mod_base, &g_cap_mod_size);
    if (g_cap_mod_size) {
        if (!g_cap_mod) g_cap_mod = malloc(g_cap_mod_size);
        const void *m = psp_mem_ptr(g_cap_mod_base, g_cap_mod_size);
        if (g_cap_mod && m) memcpy(g_cap_mod, m, g_cap_mod_size);
    }
}

/* Measure mean visible RGB in the display-sized targets already seen by the
 * GE. Texture data elsewhere in VRAM must not satisfy this filter: it was
 * exactly why a capture with a black framebuffer looked populated. Counting
 * merely non-zero bytes is not enough either -- a nearly-black fade can have
 * every pixel populated. Target addresses are de-duplicated because the
 * census keeps distinct stride/format combinations for the same storage. */
static uint64_t cap_framebuffer_rgb_sum(uint64_t *samples) {
    if (samples) *samples = 0;
    if (!g_cap_vram) return 0;
    uint32_t seen[GE_MAX_TARGETS];
    int n_seen = 0;
    uint64_t sum = 0, n = 0;
    for (int i = 0; i < g_ge.n_targets; i++) {
        const ge_target *t = &g_ge.targets[i];
        if (!t->prims || t->stride < 480 || t->fmt > 3 ||
            t->addr < PSP_VRAM_BASE)
            continue;
        int duplicate = 0;
        for (int j = 0; j < n_seen; j++)
            if (seen[j] == t->addr) duplicate = 1;
        if (duplicate) continue;
        const uint32_t bpp = t->fmt == 3 ? 4u : 2u;
        const uint64_t off = (uint64_t)t->addr - PSP_VRAM_BASE;
        const uint64_t last = off + ((uint64_t)271 * t->stride + 480u) * bpp;
        if (last > PSP_VRAM_SIZE) continue;
        seen[n_seen++] = t->addr;
        for (uint32_t y = 0; y < 272; y++) {
            const uint8_t *row =
                g_cap_vram + off + (uint64_t)y * t->stride * bpp;
            for (uint32_t x = 0; x < 480; x++) {
                if (t->fmt == 3) {
                    sum += row[x * 4u] + row[x * 4u + 1u] +
                           row[x * 4u + 2u];
                } else {
                    const uint16_t px = (uint16_t)row[x * 2u] |
                        (uint16_t)((uint16_t)row[x * 2u + 1u] << 8);
                    if (t->fmt == 0) {
                        const uint32_t r = px & 31u;
                        const uint32_t g = (px >> 5) & 63u;
                        const uint32_t b = (px >> 11) & 31u;
                        sum += (r << 3 | r >> 2) + (g << 2 | g >> 4) +
                               (b << 3 | b >> 2);
                    } else if (t->fmt == 1) {
                        const uint32_t r = px & 31u;
                        const uint32_t g = (px >> 5) & 31u;
                        const uint32_t b = (px >> 10) & 31u;
                        sum += (r << 3 | r >> 2) + (g << 3 | g >> 2) +
                               (b << 3 | b >> 2);
                    } else {
                        const uint32_t r = px & 15u;
                        const uint32_t g = (px >> 4) & 15u;
                        const uint32_t b = (px >> 8) & 15u;
                        sum += (r << 4 | r) + (g << 4 | g) + (b << 4 | b);
                    }
                }
                n += 3;
            }
        }
    }
    if (samples) *samples = n;
    return sum;
}

static void cap_write(void) {
    char numbered[4096];
    const char *path = g_cap_path;
    if (g_cap_poll_count) {
        const int n = snprintf(numbered, sizeof numbered, "%s-%u.gcap",
                               g_cap_path, g_cap_polls[g_cap_poll_index]);
        if (n < 0 || (size_t)n >= sizeof numbered) {
            fprintf(stderr, "ge: capture path is too long\n");
            return;
        }
        path = numbered;
    }
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "ge: cannot write capture %s\n", path); return; }
    ge_cap_header h = { GE_CAP_MAGIC, GE_CAP_VER, (uint32_t)psp_ge_state_size(),
                        (uint32_t)g_cap_n, PSP_RAM_BASE, PSP_RAM_SIZE,
                        PSP_VRAM_BASE, PSP_VRAM_SIZE,
                        g_cap_mod_base, g_cap_mod_size };
    fwrite(&h, sizeof h, 1, f);
    fwrite(g_cap_state, h.state_bytes, 1, f);
    fwrite(g_cap_lists, sizeof g_cap_lists[0], (size_t)g_cap_n, f);
    if (g_cap_ram)  fwrite(g_cap_ram,  PSP_RAM_SIZE,  1, f);
    if (g_cap_vram) fwrite(g_cap_vram, PSP_VRAM_SIZE, 1, f);
    if (g_cap_mod && g_cap_mod_size) fwrite(g_cap_mod, g_cap_mod_size, 1, f);
    fclose(f);
    fprintf(stderr, "ge: captured %d list(s), %llu command(s)",
            g_cap_n, (unsigned long long)(g_ge.commands - g_cap_cmd0));
    if (g_cap_min_mean && g_cap_fb_samples)
        fprintf(stderr, ", framebuffer RGB mean %.2f",
                (double)g_cap_fb_sum / (double)g_cap_fb_samples);
    fprintf(stderr, ", polls %u..%u -> %s\n", g_cap_start_poll,
            psp_ctrl_polls(), path);
}

/* Called at every frame boundary, which is what drain_all marks. */
static void cap_frame_boundary(void) {
    cap_init();
    if (!g_cap_path || g_cap_frame < 0) return;

    if (g_cap_arming) {
        /* Half of this game's presents carry no lists at all -- it sets the
         * frame buffer twice a frame (findings item 56) -- and a capture of
         * one of those is a file with nothing in it. Keep waiting until a
         * frame actually submits work, and re-snapshot the state each time so
         * it still belongs to the frame that gets captured. */
        const uint64_t ran = g_ge.commands - g_cap_cmd0;
        uint64_t fb_sum = 0, fb_samples = 0;
        int reject_fb = 0;
        if (g_cap_n != 0 && ran >= g_cap_min_cmds && g_cap_min_mean) {
            fb_sum = cap_framebuffer_rgb_sum(&fb_samples);
            g_cap_fb_sum = fb_sum;
            g_cap_fb_samples = fb_samples;
            reject_fb = !fb_samples || fb_sum < g_cap_min_mean * fb_samples;
            if (reject_fb)
                fprintf(stderr, "ge: capture candidate has %llu command(s) but "
                                "framebuffer RGB mean %.2f; continuing\n",
                        (unsigned long long)ran,
                        fb_samples ? (double)fb_sum / (double)fb_samples : 0.0);
        }
        if (g_cap_n == 0 || ran < g_cap_min_cmds || reject_fb) {
            psp_ge_state_save(g_cap_state);
            g_cap_start_poll = psp_ctrl_polls();
            g_cap_cmd0 = g_ge.commands;
            g_cap_n = 0;
            g_cap_snapped = 0;
            return;
        }
        cap_write();
        g_cap_arming = 0;
        if (!g_cap_poll_count || ++g_cap_poll_index == g_cap_poll_count) {
            g_cap_frame = -1;
            return;
        }
    }
    g_cap_seen++;
    if (g_cap_poll_count ? psp_ctrl_polls() >= g_cap_polls[g_cap_poll_index]
                         : g_cap_seen == g_cap_frame) {
        if (!g_cap_state) g_cap_state = malloc(psp_ge_state_size());
        if (!g_cap_state) return;
        psp_ge_state_save(g_cap_state);   /* before the frame's commands run */
        g_cap_start_poll = psp_ctrl_polls();
        g_cap_cmd0 = g_ge.commands;
        g_cap_n = 0;
        g_cap_snapped = 0;
        g_cap_arming = 1;
    }
}

/* ---- PSPRECOMP_VIEW_LOG=<file> -- the camera, per frame ------------------
 *
 * The GE's matrices are the one piece of the guest's own world state this
 * runtime already decodes. Everything else about where the player is and which
 * way they face lives in guest memory under no name, but the transforms arrive
 * here as floats every frame, for free.
 *
 * Which matrix carries the camera is a fact about the game, not the GE, and
 * for this game it is not the view matrix. A full mission uploads the view
 * ~155,000 times and it is the same axis flip, diag(1,-1,-1), every time; the
 * camera is composed on the CPU -- PS2-era scalar code -- into each object's
 * world matrix, ~240 uploads a frame. So the log records both: every distinct
 * view upload (tag V, which for this game is two lines a run), and the k-th
 * world upload of each frame (tag W). For a static object drawn at a stable
 * point in the frame, that world matrix *is* the camera, up to a constant.
 *
 * That makes it the instrument for questions the host otherwise cannot ask:
 * what turn rate does a given stick deflection actually produce, is there a
 * deadzone, is there a maximum, how long does the camera take to settle. Those
 * are measurements, and without this they are guesses.
 *
 * Read-only and off unless asked for. Noted when an upload *completes* -- the
 * twelfth VIEWMATRIXDATA word -- and not at the frame boundary. The first
 * version sampled at drain time and saw one matrix for an entire mission: the
 * HUD draws last, sets a 2D view of its own, and had overwritten the scene's
 * by the time the frame ended. Every distinct upload gets a line, so the 2D
 * one still appears; it is the constant diag(1,-1,-1) and trivially filtered.
 *
 * Stamped with the pad-poll count rather than a frame number or a wall clock,
 * because that is the unit scenario files are keyed on: a line here lines up
 * with the input that produced it, and stays lined up when the host runs at a
 * different speed. */
static FILE *g_view_log;
static int   g_view_log_init;
/* The last few uploads, not the last one. A frame uploads the scene's matrix
 * and then the HUD's, so "differs from the previous upload" is true twice a
 * frame even when the camera has not moved; remembering a handful collapses a
 * still camera to nothing. The cost is that a camera snapping *back* to a
 * matrix seen within the last four uploads is not logged either -- rare with
 * float trig, but real for a menu camera stepping between fixed positions. */
enum { VIEW_RING = 4 };
/* One ring per tag: the view flip is constant, and a moving camera's world
 * uploads would evict it from a shared ring every few frames, re-logging the
 * same matrix each time -- 952 lines of it in one probe run. */
static float g_view_ring[2][VIEW_RING][12];
static int   g_view_ring_n[2], g_view_ring_next[2];

static void view_log_emit(char tag, const float *m, int dedupe) {
    if (!g_view_log_init) {
        g_view_log_init = 1;
        const char *p = getenv("PSPRECOMP_VIEW_LOG");
        if (p && *p) {
            g_view_log = fopen(p, "w");
            if (!g_view_log) {
                fprintf(stderr, "ge: cannot write PSPRECOMP_VIEW_LOG %s\n", p);
            } else {
                fprintf(g_view_log,
                    "# tag poll  m[0..11] (3x4, column-major: 3 basis columns "
                    "then translation)  yaw_deg pitch_deg\n"
                    "# tag V = a GE view matrix upload; W = the k-th world matrix "
                    "upload of a frame (PSPRECOMP_VIEW_LOG_WORLD, default 1). This "
                    "game holds its view matrix constant and composes the camera "
                    "into each object's world matrix, so a static object's W line "
                    "is the camera up to a constant. WORLD=all logs every upload, "
                    "undeduplicated, with an F <poll> line at each frame boundary; "
                    "PSPRECOMP_VIEW_LOG_POLLS=lo-hi[,lo-hi] confines W lines to "
                    "those polls.\n"
                    "# yaw and pitch are derived from the third basis column on "
                    "the assumption that it is the forward axis. That convention "
                    "is UNVERIFIED -- confirm it against a known rotation before "
                    "trusting the two derived columns; the twelve raw floats are "
                    "the measurement.\n");
            }
        }
    }
    if (!g_view_log) return;

    /* One line per *new* matrix. A held stick still redraws every frame, and
     * a log with a line per frame regardless is mostly duplicates. */
    if (dedupe) {
        const int r = tag == 'W';
        for (int i = 0; i < g_view_ring_n[r]; i++)
            if (!memcmp(g_view_ring[r][i], m, sizeof g_view_ring[r][i])) return;
        memcpy(g_view_ring[r][g_view_ring_next[r]], m, sizeof g_view_ring[r][0]);
        g_view_ring_next[r] = (g_view_ring_next[r] + 1) % VIEW_RING;
        if (g_view_ring_n[r] < VIEW_RING) g_view_ring_n[r]++;
    }

    const float fx = m[6], fy = m[7], fz = m[8];
    const double yaw   = atan2((double)fx, (double)fz) * 180.0 / 3.14159265358979323846;
    const double pitch = atan2((double)fy,
                               sqrt((double)fx * fx + (double)fz * fz))
                         * 180.0 / 3.14159265358979323846;

    fprintf(g_view_log, "%c %u", tag, psp_ctrl_polls());
    for (int i = 0; i < 12; i++) fprintf(g_view_log, " %.6f", m[i]);
    fprintf(g_view_log, " %.3f %.3f\n", yaw, pitch);
    fflush(g_view_log);   /* a run that ends in a crash still leaves its trace */
}

/* View uploads always; world uploads only the k-th of each frame, counted from
 * the frame boundary psp_ge_drain_all marks. k = 0 turns world logging off.
 *
 * `all` logs every world upload, undeduplicated, with an `F <poll>` line at
 * each frame boundary so a reader can match objects across frames by their
 * position in the frame. That exists because the k-th upload turned out not to
 * be one object: draw order shifts with what is on screen, so a single index
 * mixes terrain, HUD pieces and enemies from frame to frame. With every upload
 * in hand the camera is recoverable anyway -- every static object's matrix
 * rotates by exactly the camera's rotation, so the dominant per-object yaw
 * delta between two frames is the camera's. See scripts/view-analyze.py.
 *
 * PSPRECOMP_VIEW_LOG_POLLS=lo-hi[,lo-hi...] confines world logging to those
 * polls, which is what makes `all` affordable at ~240 uploads a frame. */
enum { VIEW_WORLD_ALL = -2, VIEW_POLL_RANGES = 16 };
static int      g_view_world_seen, g_view_world_k = -1;
static uint32_t g_view_poll_lo[VIEW_POLL_RANGES], g_view_poll_hi[VIEW_POLL_RANGES];
static int      g_view_poll_n;

static void view_log_world_init(void) {
    const char *e = getenv("PSPRECOMP_VIEW_LOG_WORLD");
    g_view_world_k = (!e || !*e) ? 1 : !strcmp(e, "all") ? VIEW_WORLD_ALL : atoi(e);
    const char *p = getenv("PSPRECOMP_VIEW_LOG_POLLS");
    while (p && *p && g_view_poll_n < VIEW_POLL_RANGES) {
        char *end;
        const uint32_t lo = (uint32_t)strtoul(p, &end, 10);
        if (end == p) break;
        uint32_t hi = lo;
        if (*end == '-') hi = (uint32_t)strtoul(end + 1, &end, 10);
        g_view_poll_lo[g_view_poll_n] = lo;
        g_view_poll_hi[g_view_poll_n] = hi;
        g_view_poll_n++;
        p = (*end == ',') ? end + 1 : end;
    }
}

static int view_log_world_wanted(void) {
    if (!g_view_poll_n) return 1;
    const uint32_t poll = psp_ctrl_polls();
    for (int i = 0; i < g_view_poll_n; i++)
        if (poll >= g_view_poll_lo[i] && poll <= g_view_poll_hi[i]) return 1;
    return 0;
}

static void view_log_note(void) { view_log_emit('V', g_tl.view, 1); }

static void view_log_note_world(void) {
    if (g_view_world_k == -1) view_log_world_init();
    if (g_view_world_k == 0 || !view_log_world_wanted()) return;
    g_view_world_seen++;
    if (g_view_world_k == VIEW_WORLD_ALL)           view_log_emit('W', g_tl.world, 0);
    else if (g_view_world_seen == g_view_world_k)   view_log_emit('W', g_tl.world, 1);
}

/* The frame boundary: restart the per-frame count, and in `all` mode say so in
 * the log, so the uploads before this line are one frame's. */
static void view_log_frame_mark(void) {
    g_view_world_seen = 0;
    if (g_view_world_k == VIEW_WORLD_ALL && g_view_log && view_log_world_wanted())
        fprintf(g_view_log, "F %u\n", psp_ctrl_polls());
}

void psp_ge_drain_all(void) {
    if (!g_tl_in) {
        ge_execute();
        ge_advance(UINT64_MAX, 0, 0, UINT32_MAX);
        /* Not a wait: what the GE did here cost no time, as before the GE
         * was timed. */
        const uint64_t now = ge_now();
        g_ge_t = now;
        if (g_ge_back > now) g_ge_back = now;
        for (int i = 0; i < GE_FIFO_PRIMS; i++) if (g_ge_fifo[i] > now) g_ge_fifo[i] = now;
    }
    cap_frame_boundary();
    view_log_frame_mark();
}

/* Replay one captured list. Deliberately not sceGeListEnQueue: that reads its
 * arguments from guest registers and hands back an id nobody here has any use
 * for. This is the same machinery underneath -- a queue slot, then run it --
 * without the firmware call wrapped round it. */
void psp_ge_replay_list(uint32_t list, uint32_t stall, uint32_t base) {
    ge_queue *q = NULL;
    for (int i = 0; i < MAX_QUEUES; i++) if (!g_queue[i].used) { q = &g_queue[i]; break; }
    if (!q) { for (int i = 0; i < MAX_QUEUES; i++) if (g_queue[i].done) { q = &g_queue[i]; break; } }
    if (!q) return;
    memset(q, 0, sizeof *q);
    q->used = 1; q->id = 0x10000u + (uint32_t)(q - g_queue); q->cbid = -1;
    q->replay = 1;
    q->list = list & 0x0FFFFFFCu;
    q->stall = stall & 0x0FFFFFFCu;
    q->base = base;
    q->origin = list & ~3u;
    g_ge.lists++;
    run_list(q);
}

/* What the GE is drawing into, for a caller that has to read the result back
 * out of guest memory afterwards. */
void psp_ge_current_target(uint32_t *addr, uint32_t *stride, int *fmt) {
    if (addr)   *addr   = ge_fb_address(g_ge.fbp);
    if (stride) *stride = g_ge.fbw;
    if (fmt)    *fmt    = (int)g_ge.fbfmt;
}

static void hle_ListSync(void) {
    ge_queue *q = find_queue(psp_arg(0));
    if (!q) { psp_ret(0x80000100); return; }
    if (psp_arg(1) == GE_SYNC_WAIT && !g_tl_in) ge_deliver(1);
    if (q->done) { psp_ret(GE_SYNC_DONE); return; }
    /* NOWAIT tells the truth: 4 while paused, 1 queued behind another list,
     * 2 otherwise -- running, or waiting at its stall: geprobe 5 steps 57
     * and 58 and geprobe 6 steps 79 and 80 (fw 6.60). It used to read DONE
     * at a stall, for pspgu's helpers, which poll it on tiny internal lists
     * that on hardware have finished by the time anyone asks; they now
     * finish inside the stall update that releases them, as on the PSP. */
    if (psp_arg(1) != GE_SYNC_WAIT) {
        psp_ret((uint32_t)list_status(q));
        return;
    }
    /* WAIT on a pending list: give up the CPU once -- an equal-priority
     * thread runs here on hardware, which is the checkpoint [r] -- then
     * complete, taking as long as the GE does. A lone thread yields to
     * itself and carries on, so the game is unaffected. */
    psp_sched_yield();
    ge_wait(q->id);
    psp_ret(q->done ? GE_SYNC_DONE : list_status(q));
}

/* A finished list's id stops resolving once DrawSync(WAIT) has returned:
 * geprobe step 26 (fw 6.60) reads 0 from ListSync(wait) on its completed list,
 * 0 from DrawSync(wait), then 0x80000100 from ListSync(peek). Whether
 * ListSync(wait) alone already retires it is not measured. */
static void retire_done(void) {
    for (int i = 0; i < MAX_QUEUES; i++)
        if (g_queue[i].used && g_queue[i].done) g_queue[i].used = 0;
}

static void hle_DrawSync(void) {
    /* NOWAIT: 2 while any list is pending -- running, paused (geprobe 5
     * step 57, fw 6.60) or waiting at its stall (geprobe 6 steps 79 and
     * 80). */
    if (psp_arg(0) != GE_SYNC_WAIT) {
        psp_ret(oldest_pending() ? 2u : GE_SYNC_DONE);
        return;
    }
    if (!g_tl_in) ge_deliver(1);          /* a wait sees the handlers it owes run */
    if (!oldest_pending()) { retire_done(); psp_ret(GE_SYNC_DONE); return; }
    psp_sched_yield();
    ge_wait(UINT32_MAX);
    retire_done();
    /* DONE even if a stalled or hung list remains. A list hung at a patch
     * division (draw_patch) would block this wait for ever on the PSP; here
     * it returns, so the host cannot freeze, and the list stays pending for
     * sceGeDrawSync's peek and sceGeBreak. Not measured. */
    psp_ret(GE_SYNC_DONE);
}

/* sceGeBreak: mode 1 drops every list, mode 0 the one at the head. Only
 * mode 1 on a hung queue is measured: geprobe 13's scenes 80 and 81 (fw
 * 6.60) break a list hung at a patch division of 65-127 with sceGeBreak(1),
 * which returns 0 or more, after which new lists run as on a fresh queue.
 * Return values and the rest are not measured. */
static void break_queue(ge_queue *q) {
    q->done = q->xdone = 1;
    q->hung = q->paused = q->xpaused = q->psig = 0;
    /* What the walk did past the point the guest has seen never happens,
     * but a handler the timeline has already reached and holds still runs. */
    for (unsigned i = g_ev_held ? 1u : 0u; i < g_ev_n;)
        if (ev_at(i)->q == q && ev_at(i)->qid == q->id) ev_remove(i); else i++;
}

static void hle_Break(void) {
    if (psp_arg(0) == 1) {
        for (int i = 0; i < MAX_QUEUES; i++)
            if (g_queue[i].used && !g_queue[i].done) break_queue(&g_queue[i]);
    } else {
        ge_queue *q = oldest_pending();
        if (q) break_queue(q);
    }
    /* The lists behind a broken one go on. */
    ge_execute();
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* sceGeContinue: a paused list goes on, run inside the call as EnQueue runs a
 * new one -- geprobe 5 step 57 (fw 6.60): the SIGNAL and FINISH after the
 * pause call their handlers before sceGeContinue returns, which reads 0.
 * Called before the pause has taken hold (from the PAUSE's own signal
 * handler), it lets the list through the pause instead; with nothing paused
 * or about to be, it does nothing. Neither case, nor any error code, is
 * measured. */
static void hle_Continue(void) {
    ge_queue *q = oldest_pending();
    if (q && q->paused) {
        q->paused = q->xpaused = 0;
        ge_release(0);
        ge_execute();
        ge_kick();
    } else if (q && q->psig) {
        q->psig = 0;
        if (q->xpaused) {
            /* The walk has stopped at the FINISH already, the guest not yet:
             * the pause it has yet to see never takes hold. */
            for (unsigned i = 0; i < g_ev_n; i++)
                if (ev_at(i)->kind == GE_EV_PAUSE && ev_at(i)->q == q && ev_at(i)->qid == q->id) {
                    ev_remove(i);
                    break;
                }
            q->xpaused = 0;
            ge_execute();
        } else {
            q->cont_early = 1;
        }
    }
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* sceGeSetCallback(PspGeCallbackData *): signal_func, signal_arg,
 * finish_func, finish_arg, copied now; the id goes to sceGeListEnQueue. The
 * table holds 16 and a full one answers 0x80000022: geprobe 5 step 59 (fw
 * 6.60) registers 15 beside libgu's own and the 16th is refused with it. */
static void hle_SetCallback(void) {
    const uint32_t p = psp_arg(0);
    /* Four words the call reads; an unreadable table is refused rather than
     * read as zeros (PSPSDK's ILLEGAL_ADDR; not measured). */
    if (!psp_mem_ptr(p, 16)) { psp_ret(0x800200D3u); return; }
    for (int i = 0; i < GE_MAX_CALLBACKS; i++) {
        if (g_ge_cb[i].used) continue;
        g_ge_cb[i].used        = 1;
        g_ge_cb[i].signal_func = psp_read32(p);
        g_ge_cb[i].signal_arg  = psp_read32(p + 4);
        g_ge_cb[i].finish_func = psp_read32(p + 8);
        g_ge_cb[i].finish_arg  = psp_read32(p + 12);
        psp_ret((uint32_t)i);
        return;
    }
    psp_ret(0x80000022u);
}

static void hle_UnsetCallback(void) {
    const uint32_t id = psp_arg(0);
    if (id < GE_MAX_CALLBACKS) {
        g_ge_cb[id].used = 0;
        /* Lists queued under it call nothing now, rather than whatever a later
         * SetCallback puts in the same slot. */
        for (int i = 0; i < MAX_QUEUES; i++)
            if (g_queue[i].used && g_queue[i].cbid == (int)id) g_queue[i].cbid = -1;
        for (unsigned i = 0; i < g_ev_n; i++)
            if (ev_at(i)->cbid == (int)id) ev_at(i)->cbid = -1;
    }
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* eDRAM is the GPU-visible VRAM window: 2 MB at 0x04000000. */
static void hle_EdramGetAddr(void) { psp_ret(PSP_VRAM_BASE); }
static void hle_EdramGetSize(void) { psp_ret(PSP_VRAM_SIZE); }

void psp_ge_register(void) {
    ge_keep();
    psp_hle_register(0xAB49E76A, "sceGe_user", "sceGeListEnQueue",         hle_ListEnQueue);
    psp_hle_register(0x1C0D95A6, "sceGe_user", "sceGeListEnQueueHead",     hle_ListEnQueueHead);
    psp_hle_register(0xE0D68148, "sceGe_user", "sceGeListUpdateStallAddr", hle_ListUpdateStallAddr);
    psp_hle_register(0x03444EB4, "sceGe_user", "sceGeListSync",            hle_ListSync);
    psp_hle_register(0xB287BD61, "sceGe_user", "sceGeDrawSync",            hle_DrawSync);
    psp_hle_register(0xB448EC0D, "sceGe_user", "sceGeBreak",               hle_Break);
    psp_hle_register(0x4C06E472, "sceGe_user", "sceGeContinue",            hle_Continue);
    psp_hle_register(0xA4FC06A4, "sceGe_user", "sceGeSetCallback",         hle_SetCallback);
    psp_hle_register(0x05DB22CE, "sceGe_user", "sceGeUnsetCallback",       hle_UnsetCallback);
    psp_hle_register(0xE47E40E4, "sceGe_user", "sceGeEdramGetAddr",        hle_EdramGetAddr);
    psp_hle_register(0x1F6752AD, "sceGe_user", "sceGeEdramGetSize",        hle_EdramGetSize);
}
