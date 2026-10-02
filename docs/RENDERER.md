# Rendering

How the GE gets to a window, and why the backend is an interface rather than
one API's file.

**The API is decided: OpenGL 3.3 core, on the SDL2 the host already links.**
Recorded 3 Sep; the reasoning is under *Choosing the API* below. This file said
SDL3 + Vulkan until then, which the roadmap had already superseded on 1 Sep
without anyone coming back here.

## The shape of the problem

The GE is not a function you call. User code builds a **display list** — 32-bit
words, each an 8-bit command and 24 bits of argument — and hands the GE a pointer
plus a stall address. The GE consumes commands up to the stall; the CPU moves
the stall forward as it writes more.

So `sceGu*`, the list-building library, is ordinary user code and gets
recompiled like anything else. Only list *execution* is ours. That execution has
two halves, and they are worth keeping apart:

| Half | What it does | Where it lives |
|---|---|---|
| **Interpretation** | Walk the list, follow JUMP/CALL/RET/END/FINISH, track state, assemble primitives | `src/hle/ge.c` — one implementation, always |
| **Presentation** | Turn assembled primitives into pixels | a *backend*, chosen at run time |

Everything upstream of a triangle is the same regardless of how the triangle is
drawn. Mixing the two produces a renderer that cannot be tested without a GPU
and cannot be diffed against a reference — which is the thing that matters most
during bring-up.

## Why a backend interface

Three reasons, in order of how much they cost to get wrong:

1. **The software path must survive.** It is the differential oracle. A
   disagreement needs investigation on both sides; either implementation can
   contain bugs. `tests/test_raster.c` asserts *where* pixels land using
   nothing but the software path, and that must keep working on a machine with
   no GPU at all — CI included.

2. **A wrong pixel is harder to debug than a missing one.** During bring-up the
   question is "did the game ask to draw this", not "does it look right". A
   backend boundary lets the answer be recorded (command counts, primitive
   counts, vertex counts) independently of whether anything was presented.

3. **Portability is the point of the project.** Any one API is one answer. No
   single one should be the only place the GE's semantics are written down —
   which is also why choosing GL below costs little if it is later replaced.

## The interface

Twelve entry points. `include/psprecomp/render.h` carries the full comments,
including what each register means and which oracle measured it; this is the
shape.

```c
typedef struct {
    const char *name;

    int  (*init)(int width, int height);
    void (*shutdown)(void);

    /* GE_FBP / GE_FBW: the framebuffer being drawn into. */
    void (*set_target)(uint32_t addr, uint32_t stride, int fmt);
    /* SCISSOR1 / SCISSOR2, both corners inclusive. Always in force on the
     * PSP, so this is the rasterizer's only bound. */
    void (*set_scissor)(int x0, int y0, int x1, int y1);
    /* The texture to sample, or addr 0 for none. Carries format, size, the
     * mip chain and its LOD mode, filters, wrap and the texture function. */
    void (*set_texture)(const psp_tex_state *t);
    /* The palette for the CLUT formats, indexed ((texel >> shift) & mask)
     * | start -- a game can page one palette through a larger CLUT. */
    void (*set_clut)(uint32_t addr, int format, int shift, int mask, int start);
    void (*set_depth)(int test_enable, int func, int write_enable);
    /* Blending, the alpha test, and the stencil -- which on the PSP is the
     * framebuffer's alpha byte. */
    void (*set_blend)(const psp_blend_state *b);
    /* GE_FOGENABLE / GE_FOGCOLOR (0xBBGGRR). The coefficient rides in the
     * vertex; this is the colour it blends toward. */
    void (*set_fog)(int enable, uint32_t colour);

    /* One assembled primitive. `count` vertices, already in screen space. */
    void (*draw)(int prim, const psp_vertex *v, int count);
    /* End of a display list -- a natural point to flush batched work. */
    void (*finish)(void);
    /* sceDisplaySetFrameBuf -- show what has accumulated. */
    void (*present)(void);
} psp_render_backend;
```

The three structs a backend has to read are where the hardware's rules
actually live: `psp_vertex` (12.4 fixed-point screen position, window depth on
the 0..65535 scale, colour, texel-unit UVs, reciprocal clip W, texture-projective
Q, and the fog byte), `psp_tex_state`
(format, the eight mip levels and `TEX_LEVEL`'s modes, both filters, wrap, the
texture function with its RGBA and doubling bits) and `psp_blend_state`
(blend, alpha test, stencil).

`draw` takes *assembled* primitives rather than raw display-list words on
purpose. Vertex format decoding — the stride arithmetic, the component
alignment, through-mode vs transformed — is fiddly and is exactly the part that
must not be duplicated per backend. Get it wrong once, centrally, and every
backend is wrong the same way, which is at least diagnosable.

## Backends

| Backend | State | Notes |
|---|---|---|
| **software** | Working | The differential reference. Points, lines, line strips, triangles, triangle strips and sprites; GE-assembled fans. `test_raster.c` asserts pixel positions and sampling rules -- perspective UVs, mip/LOD, filtering, wrap, depth and clear mode among them. No GPU, no dependencies, runs in CI. |
| **null** | Working | Counts primitives, draws nothing. What the bring-up host uses when the question is "did it ask to draw". |
| **gl33-sdl2** | Working at native resolution | Points, lines, strips, triangles, fans and sprites; render targets and aliases, perspective texturing, PSP mip/LOD/filter rules, depth, scissor, blend, alpha test, RGBA8888 alpha-backed stencil and fog. Remaining gaps are listed below. |

Selection is `psp_render_select(name)` (`src/render.c`), wired to
**`PSPRECOMP_RENDER`** in `host/boot.c` (3 Sep). Before that nothing outside
`tests/test_raster.c` called it, so the software backend was the only one a
real run could have had, and "chosen at run time" above was an aspiration.

An unknown name is **fatal** — `psp_render_select` leaves the current backend
in place and returns -1, so continuing would run the software rasterizer while
the operator believed otherwise and attribute every number to the wrong
backend. The host prints the names it would have taken, which it reads from
`psp_render_backend_name(i)` rather than keeping a second copy of the list.
A set-but-empty value means unset, as `PSPRECOMP_AUDIO_DUMP` has it. Every run
now prints which backend it used.

`init()` is called once the backend is chosen. GL records the target size there
and claims the context lazily on the GE thread; `present()` is driven at each
display flip. `shutdown()` remains unwired, and that is not merely an
oversight: boot.c skips its teardown whenever a guest thread is still live,
which is the common case, so it would not run reliably even if it were called.

## Choosing the API

**OpenGL 3.3 core, on SDL2.** Decided 3 Sep, after this file and
`docs/ROADMAP.md` were found to disagree — the roadmap said GL 3.3 on 1 Sep,
this file still said SDL3 + Vulkan from 20 Jul, and the older text was the one
being read. Three reasons, all specific to this codebase rather than to the
APIs in general:

1. **The interface above is a state machine, and so is GL.** Nine of the twelve
   entry points are per-draw state setters. Vulkan wants that state baked into
   pipeline objects ahead of time, so the same interface would need a state-hash
   to pipeline cache before it could draw at all — reworking the interface to
   suit the API, having already built the interface.
2. **The first increment was already done, in SDL2.** This file's earlier plan
   opened with "upload the emulated framebuffer as a texture, blit, present"
   and called it a milestone with no ambiguity about whether it worked.
   `host/present.c` has done exactly that for some time, with a streaming
   ABGR8888 texture. Choosing SDL3 would have meant redoing working code to
   reach a milestone already met, which is why the increments below now start
   at geometry.
3. **SDL3 would land on the audio path.** SDL3 replaces SDL2's audio callback
   with `SDL_AudioStream`s bound to devices; `present.c` uses the callback, and
   M3's gate rests on it. That is unrelated risk on freshly gated work.

The sizing settles the rest: the software rasterizer is about **2.8x** over a
16.7 ms frame on `mission-1.pad` (84.6 s of raster over 1,793 GE finishes).
A GL 3.3 backend clears that by an order of magnitude, so throughput is not
the binding constraint — iterating against the software oracle is, and that is
where GL's shorter path to a first triangle is worth more than Vulkan's
ceiling.

**What would overturn it:** wanting macOS (GL caps at 4.1 and is deprecated
there; M6 names Windows and a CI matrix, not macOS), or profiling a working GL
backend and finding draw-call submission rather than rasterizing is the wall.
Both are cheap to act on later, because `ge.c` reaches the backend through
`psp_render_current()` and nowhere else. SDL2 is in maintenance and a move to
SDL3 will come — on its own schedule, not bundled into a renderer.

## Where a GL backend lives, and on which thread

Two facts settled this, both measured rather than assumed.

**It lives in the host.** The core has no external dependencies on purpose and
SDL2 is the host's, so a backend needing a window and a GL context cannot live
in the runtime. `psp_render_register()` is the seam: the host builds a backend
and hands the pointer over, after which it is selectable by name like any
other and the interpreter cannot tell which side it came from. Registration
refuses a backend missing any of the twelve entry points, because the
alternative is a crash at whichever call it forgot, arbitrarily far from the
mistake. `tests/test_raster.c` covers the seam with a probe backend and needs
no GPU to do it.

**The context belongs on the GE thread.** SDL runs on its own thread here
(`host/present.c`), which owns the window and the event loop, while display
lists execute on whichever guest thread submitted them -- and a GL context
belongs to exactly one thread. Measured: **the GE is driven by exactly one
host thread**, 864 lists in the hangar and 3,584 in the mission, all from one.
So the SDL thread creates the window and the context and then releases it with
`SDL_GL_MakeCurrent(win, NULL)`; the GE thread claims it on first use and
keeps it; `present()` swaps from there. No command queue and no cross-thread
marshalling, which is the design a second GE thread would have forced.

That assumption is load-bearing, so a GL backend must **fail loudly** if the
thread it is called on ever changes, rather than issuing GL calls against a
context that is not current. The census in `psp_ge_dump_stats` reports the
thread count in every run, so the assumption is checked continuously rather
than once.

**Headless is the limit of this.** A GL backend needs a window, hidden or
otherwise, so the software-versus-GL comparison runs on a desktop rather than
in CI. That does not weaken the arrangement in *Validation* below: the
software backend is what must keep working with no GPU, and it does.

## The GL 3.3 backend

Scope it deliberately, because the GE has a large state space and most of it
does not matter until a game is already drawing:

**Prerequisites** — none of these are the backend:
- ~~`psp_render_select` wired to an environment variable in `host/boot.c`, so a
  backend can be chosen without recompiling~~ **done 3 Sep**, `PSPRECOMP_RENDER`
- ~~A seam for a backend the runtime cannot carry~~ **done 3 Sep**,
  `psp_render_register()`
- ~~A per-frame timer.~~ **Done 4 Sep.** The GL report distinguishes display
  callbacks from frames carrying new GPU work and reports average cadence plus
  median, p95 and maximum intervals. `psp_render_raster_ns()` remains the
  software backend's cumulative CPU cost.
- ~~Display-list capture and replay~~ **done 4 Sep**, through GE capture files
  and `host/gereplay.c`, including readable scene selection. The apparently
  empty saved framebuffer was a correctly copied black transition frame, not
  corruption: `PSPRECOMP_GE_CAPTURE_MINCMDS` selects substantial work and
  `PSPRECOMP_GE_CAPTURE_MINMEAN` additionally rejects a dark starting buffer.
  Hangar thresholds 5000 and 8 select a readable 14,806-command fixed list;
  software and GL reproduce the same geometry at normalized RMSE 0.00572.
- The pixel and depth counters in `ge.c` are software-backend concepts
  (`psp_render_reset_depth` has no GPU meaning) and read zero under any other
  backend; put them behind an optional query first

**First increment — done 3 Sep, and what "done" means here.** `host/render_gl.c`
is a real backend: it claims the context on the GE thread, batches vertices,
draws them into an off-screen 480x272 target, blits that to the window, and
reads it back into the guest framebuffer so the project's instruments keep
working. `PSPRECOMP_RENDER=gl` selects it.

Each stage was verified separately rather than by looking at the result: a
known triangle pushed through the batch renders 38,000 red pixels, a forced
constant in the readback reaches the frame dump, and a clear reaches the
readback. The pipeline is sound.

The first increment's game frame was uniformly white because depth, blending
and clear handling deliberately had not landed yet. That was an incremental
bring-up result, not the backend's current output.

**`finish()` had never been called** (fixed the same day). The interface has
documented it as the end of a display list and a good point to flush batched
work since it was written, and `ge.c` called only `draw()`. The software path
never noticed, because it rasterizes each primitive immediately and has
nothing to batch -- so the gap only appears the moment a backend accumulates.

**Second increment — done 3 Sep.** Depth test and depth writes, the scissor,
the colour and alpha write masks, alpha blending, and the alpha test as a
fragment discard (GL 3.3 core removed the fixed-function one). Every state
setter flushes the pending batch before recording, because a state change
would otherwise apply retroactively to geometry already in the buffer. The
hangar goes from one colour to 105 and from mean 255 to 49.7.

Two things this does **not** represent, counted in the run summary rather than
approximated -- a wrong factor renders a plausible picture, a counted one is a
number:
- The doubled blend factors (GE codes 6-9) and the absolute-difference
  equation (code 5) have no GL equivalent and need the shader.
- The stencil. On the PSP it *is* the framebuffer's alpha byte, which GL's own
  stencil buffer is not, so it needs the shader too.

**A limit worth knowing before comparing.** The software path's blend term is
`((c+1)*f) >> 8`, measured from `gpu/commands/blend` and exact. GL's is
`c*f/255`. They differ by up to one level per channel, so a GL backend cannot
be bit-identical to the oracle through a blend however correct it otherwise
is. Compare structure and brightness, and reserve exactness for the paths that
can have it.

**Third increment — done 4 Sep.** Texturing now includes all decoded formats,
CLUT paging and swizzle, the five texture functions, render-target aliasing,
and perspective-correct UV/Q interpolation. The cache key covers every mip
level's address, stride and dimensions rather than only level zero.

Mip selection does not use driver derivatives. The shared
`psp_render_lod16()` applies the measured AUTO, CONST and SLOPE rules once per
primitive, including the signed 1/16 bias. The fragment shader uses
`texelFetch` to implement PSP nearest/bilinear precision, wrap, mip-nearest and
mip-linear explicitly; this avoids OpenGL's different min/mag switchover and
also permits independently-sized PSP mip levels. A mission run uploaded 47,779
complete two-extra-level chains and no incomplete chain; its end-frame RMSE
against the software renderer fell from 0.01532 to 0.01418.

The same increment made texture identity content-aware. `mem.c` records the
latest write to each 256-byte guest-memory granule; ordinary scalar and block
writes mark themselves, while HLE code writing through a raw mapped pointer and
GL render-target readback mark the completed range explicitly. Cache entries
cover every active mip range and the reachable part of the CLUT. The global
write serial makes repeated bindings with no intervening write constant-time;
after an unrelated write, the entry revalidates only those ranges.

Direct-colour entries do not key on incidental CLUT state, and a bounded
32-entry probe chooses its least-recently-used member on collision. On
`mission-1.pad`, this reduces uploads from 232,385 to 1,073 (99.5%), of which
898 are real dirty invalidations; there are 458,311 hits, 175 cold misses and
only 16 evictions, with 159/512 slots resident at the end. Generation checks
cost 0.008 s, complete texture binding 1.157 s and render-target readback
0.832 s over a 56.6 s run. The resulting framebuffer is byte-identical to the
pre-cache mip/LOD result. `big.gcap` is likewise byte-identical before and after
the cache, and its GL output is byte-identical to the software backend.

The cadence counters also resolve the apparent scene-dependent rate: the
mission's rendered-frame interval is 33 ms median / 34 ms p95, and the hangar's
is also 33/34 ms. Two `sceDisplaySetFrameBuf` calls arrive per newly rendered
frame; reporting API calls as frames would falsely claim about 63 fps. Both
measured paths are steadily following the game's ~30 fps cadence rather than
the GL renderer slowing down only in the mission.

That cadence is now tied to one clock rather than to the speed of the caller.
In a real-time run, display vcount and accumulated hcount are derived from the
monotonic-backed guest clock; deterministic headless runs keep the synthetic
read advance they need to escape a counter busy-loop. The host also waits for
SDL window, GL-context and audio setup before it anchors real time. Previously
that asynchronous setup appeared as about 435 ms of elapsed game time before
the hangar settled; after the barrier the null and GL paths reach the same
430th update at 14.769 s and 14.787 s. The full GL mission records 56.639 s of
guest time over 56.639 s of wall time (0.018 ms drift), and every paced boot
summary now prints those two clocks directly.

GPU timer queries separate that cadence from rendering cost. Eight query
objects rotate without blocking; a result is consumed only after GL reports it
available, and the report counts any frame it could not measure rather than
stalling for one. The query covers native-resolution draws through the final
window blit, while the existing CPU timers continue to name texture binding and
readback separately. On the full mission, 1,791 GPU samples average 0.80 ms,
with 0.8 ms p50, 2.1 ms p95 and 2.67 ms maximum, and no full-ring drops. The
fixed hangar capture costs 1.27 ms and remains byte-identical before and after
instrumentation. The measured ~30 fps cadence is therefore not GPU saturation;
even a 16.7 ms budget has substantial headroom on this host.

The fixed-list oracle also pins the small arithmetic details that a plausible
GL image can hide. Interpolated colour and texture-function output are
quantized at the software path's RGBA8 boundaries; fog and masked alpha tests
operate on those byte values. For source-alpha blends whose framebuffer alpha
is the masked stencil byte, the shader computes the GE's separately truncated
source term before fixed-function blending and biases the inverse-alpha
destination term so GL's final round reproduces the GE floor. Draws that write
alpha and equations where that transformation is invalid retain the ordinary
path. A 1/256-pixel vertical bias converts GL's lower-left half-open edge rule
to the PSP's top-edge ownership after Y is flipped, removing missing rows from
half-pixel UI rectangles. Host-default dithering is explicitly disabled: the
GL backend does not reproduce the GE's dither matrix, which the software
renderer now applies (`src/render.c`, measured by `tools/hwprobe/geprobe`).

On the selected 14,806-command hangar capture these rules raise exact pixels
from 62,870 to 122,818 of 130,560 and reduce normalized RMSE from 0.005718 to
0.001168. Only seven pixels differ by more than two channel levels. The full
mission still completes at zero bad accesses and normal pacing; its measured
draw-plus-blit cost is 0.81 ms mean, 2.1 ms p95 and 2.85 ms maximum.

**Fifth increment — done 4 Sep.** Points and lines now use explicit pixel
coverage shared between backends (`psp_render_walk_line`). GL expands samples
to pixel quads and uses the normal fragment pipeline; software shades them
directly. Line-strip decode batches retain their shared endpoint. Transformed
points/lines follow the existing near-plane/guard-band policy instead of
projecting behind-eye vertices onto the origin. Integer endpoint cases are
checked against public primitive fixtures; fractional coverage still merits
wider hardware checks.

RGBA8888 stencil uses GL's 8-bit stencil attachment, with lazy GPU bit-plane
imports/exports to keep the PSP framebuffer alpha byte coherent. Reconcile
before destination-alpha blending, target sampling, readback and partial alpha
clears. Blended primitives which also change stencil synchronize individually
when their destination-alpha dependency requires it. The host's synthetic
`render_tests.c` checks all 256 byte values and fail/pass operations in RGBA,
not only RGB; it also found software dropping stencil-only clears when RGB
writes were disabled, fixed in `shade_pixel()`.

**Remaining renderer work:**
- Stencil for 5650/5551/4444 targets, doubled blend factors and absolute-difference
  blend equation; these remain explicitly counted when encountered.
- History-dependent render-target size/format changes, dithering, broader
  scene/hardware coverage and fractional point/line edge cases.

## When the GE runs

The GE works alongside the CPU, and a game can see how far behind it is: in
when its signal and finish handlers run, and in what `sceGeListSync` and
`sceGeDrawSync` report when asked without waiting. geprobe 5 and 6 (fw 6.60)
measured it:

- A list of a few commands, enqueued with no stall, has run with all its
  handlers before `sceGeListEnQueue` returns (geprobe 5 steps 54-56).
- 200 full-screen sprites, enqueued the same way, are still being drawn when
  EnQueue returns. `ListSync(peek)` reads 2 (drawing), and only the handler of
  the first SIGNAL, which comes before the sprites, has run (step 58).
- Words released by `sceGeListUpdateStallAddr` run inside that call when they
  can. A SIGNAL and a FINISH released from a stall have both called their
  handlers by its return (geprobe 6 steps 79 and 80). libgu's finish
  handler runs after `sceGuFinish` has returned, in `sceGuSync` (geprobe 5
  step 50), because a FINISH waits for the drawing before it to end.
- A list waiting at its stall peeks 2 from both `ListSync` and `DrawSync`,
  as EnQueue returns and 10 ms later (steps 79 and 80).
- A SIGNAL with the PAUSE behaviour stops the list until `sceGeContinue`.
  Twenty milliseconds later only its own handler has run, and the peeks read
  4 (paused) and 2 (drawing). The FINISH that libgu writes after the pause
  calls no handler. The rest of the list runs inside `sceGeContinue` (step 57).
  A `sceGeContinue` from the PAUSE's own handler lets the list straight
  through (geprobe 6 step 82). With nothing paused it returns 0 (step 78).
- Step 81 times 100 sprites between two SIGNALs, then a FINISH, at three
  sizes, in microseconds after the call before EnQueue:

  | sprites | SIGNAL 1 | SIGNAL 2 | FINISH | EnQueue returns | DrawSync returns |
  |---|---|---|---|---|---|
  | 480x272 | 51 | 35609 | 62409 | 68 | 62474 |
  | 64x64 | 46 | 1130 | 1952 | 57 | 1993 |
  | 16x16 | 41 | 126 | 138 | 52 | 176 |

  Drawing takes 623.6 and 19.06 µs a sprite at the larger sizes: 209.2 pixels
  a microsecond. SIGNAL 2 comes 43 sprites' drawing before the FINISH: the
  command processor runs up to 43 drawing commands ahead of the drawing,
  and calls a SIGNAL's handler when it reaches it. It takes 0.85 µs a sprite
  (a VADDR and a PRIM), which is what limits the 16x16 sprites.

psprecomp's model (`src/hle/ge.c`, "When the GE runs"):

- **Time.** The GE keeps its own time on the guest clock (`clock.h`:
  virtual microseconds, a tick per firmware call, a frame per vblank), which
  is also what `sceKernelGetSystemTimeLow` reads. The command processor
  costs 0.1 µs a command word, 0.6 a PRIM, BEZIER or SPLINE and 0.025 a
  vertex. Up to 43 drawing commands wait for the drawing, which costs
  1/209.2 µs a pixel. A FINISH waits for the drawing to end. The split of
  the 0.85 µs between word, PRIM and vertex is not measured. 16x16 sprites
  draw a quarter faster on the PSP than this charges.
- **Where it runs.** Every firmware call first lets the GE catch up to the
  present; a SIGNAL or FINISH it reaches then waits, with the GE, until the
  next firmware call made with interrupts enabled. EnQueue,
  UpdateStallAddr and Continue let the GE run 11 µs ahead, calling handlers
  as it reaches them. EnQueue itself takes 51 µs: the GE starts 40 µs in.
  When every thread waits, the GE goes on until the next deadline, and a
  handler it reaches runs at its moment. A Sync that waits runs everything
  and takes as long as the GE does: the guest clock follows the GE to each
  handler and to its end. Under psprecomp step 81 reads 43, 35615 and
  62449 for the full-screen sprites, with DrawSync returning at 62451.
- **Peeks.** The truth: 2 running or at a stall, 4 paused, 1 queued behind
  another list, 0 done.
- **Pause.** A PAUSE holds the list at its FINISH until `sceGeContinue`, and
  a `sceGeContinue` from the PAUSE's own handler lets it through.
- **Frame boundaries** (`psp_ge_drain_all`, at `sceDisplaySetFrameBuf`)
  still run everything at once and charge no time, since they are no wait.
- **Backends.** Pixel costs come from the software renderer's counter. Under
  any other backend the GE charges commands and vertices only, so it runs
  faster there.

What this can change for a game, compared with the models before it (every
list ran to its stall inside the call that released it, then a GE a
thousand times too fast that cost no time in a Sync):

- `sceGuSync` and the other waits now take the GE's time, about what they
  take on the PSP, so a frame's guest time includes its drawing. A thread
  sleeping on a deadline that falls inside such a wait wakes when the wait
  ends, not during it.
- Handlers for words released by a stall update run inside the update when
  the GE gets to them in 11 µs, otherwise at a later firmware call. For the
  usual `sceGuFinish` then `sceGuSync`, the finish handler runs inside the
  Sync.
- A guest that spins on a flag set by a GE handler without making any
  firmware call never sees it set. psprecomp cannot interrupt recompiled
  code, which is already true of alarms and vtimers (`src/hle/ktimer.c`).
  With any firmware call in the loop, the GE progresses and the handler runs.
- Peeks that read done may now read 2 or 4, including on a list waiting at
  its stall, such as a libgu list still being built. A heavy list enqueued
  without a stall can still be drawing when EnQueue returns.
- A game that pauses a list and never calls `sceGeContinue` leaves it
  paused, as the PSP does. A Sync that waits on a paused list gives up after
  one yield rather than blocking for ever.
- PRIM and BBOX move VADDR past the vertices they read (geprobe 5 scene 33,
  and libgu's `sceGuDrawArrayN`); indexed draws move IADDR instead (geprobe
  6 scene 42).

## Depth values

The software backend writes the depth geprobes 5 to 7 (fw 6.60) measured, not
a float blend (`src/render.c` sw_tri, `src/hle/ge.c` ge_proj_row,
ge_screen_z, clip_to_fx16):

- **The projection runs in the GE's own float** (16 significant bits, cut
  toward zero; ge24). Each eye coordinate is cut to it, each product with a
  projection entry is cut, and the products and the translation are summed
  without a cut. That gives clip x, y, z and w.
- **A transformed vertex's depth** is clip z times 1/w, with w's reciprocal
  cut to 24 bits and the product to 18. The scale and centre follow, the
  sum rounded to 16 bits, and the rasterizer takes the integer part.
  geprobe 7 scene 45 reads back 3720 points, one per eye depth from -1.05
  to -99. This fits 2883 of them, against 1703 for the rule it replaced.
  It is the best of 460,800 variants searched, and it keeps the eleven
  depths that geprobes 5 and 6 pinned. The misses are one step either way,
  so the GE's arithmetic is within half a step of this but is not it.
  Scene 48 adds one: eye z -5.3 reads 11829, where this gives 11828.75.
- **A transformed vertex's x and y** come from the same clip x, y and w.
  1/w is cut to 24 bits, as for depth, and each quotient to 16. The
  sixteenth is then taken toward the centre (screen_axis_fx16). geprobe 7
  scene 48 found six corners a sixteenth further out than the earlier rule
  put them (a 14-bit reciprocal on float coordinates). Their depth planes
  were 100 to 500 pixels off and match once each corner moves. Every rule
  searched that places scene 48's 198 depth-confirmed coordinates and
  scenes 15 and 18's measured corners cuts the eye coordinates to 16 bits.
  Patch vertices keep the 14-bit rule: psprecomp's tessellation is not the
  GE's, and the new rule costs scene 22 342 pixels.
- **Across a triangle** depth is a plane, like colour: anchored at the same
  vertex, with gradients from the numerator times 1/area, then floored to
  1/1024 a pixel. Colour uses the same 1/area (geprobe 6 scene 39). Scene
  27's four through-mode triangles match on every pixel, and so do 22 of
  scene 36's 24 3D triangles from one of their corners.
- **1/area comes from a table** with a linear step (area_rcp), not a
  division. The area's significand is cut to 17 bits. Its leading nine
  bits, h, pick one of 256 entries holding 2^27/h and the slope 2^24/h²,
  each cut to an integer. The last eight bits, l, take l times the slope
  over 32, rounded up, off the first, and the result is cut to 16 bits.
  geprobe 8 scenes 50 and 51 read it at every 10-bit length, as a
  triangle's area and as a line's length drawn either way; all three agree
  on all 512. The reciprocal cut to 16 bits, which this replaces, was a
  unit out on 50 of them: every one a length whose last bit is set, where
  the one-bit step lands up to 0.39 of a unit above the exact value or 1.19
  below it. The table matches all 512, all 44 of scene 46's areas of 18 to
  20 bits (depth and every colour channel), and every pixel of scenes 37,
  39, 46, 47, 50 and 51.
- **Along a line** (psp_render_walk_line) one pixel is drawn per
  major-axis column whose centre lies on the segment. Its row, colour and
  depth are those of the centre's projection onto the line. The gradients
  are the difference times the same reciprocal, of the major length,
  floored to 1/16384 of a step per sixteenth of a pixel. For whole-pixel endpoints that is the
  step centre geprobe step 1 measured. Scene 27's 3D line, whose ends fall
  between centres, matches on every pixel only this way. All 5862 pixels of
  geprobe 7 scene 49's steep lines match.
- **Line ends** follow each end pixel's diamond (geprobe 6 scene 37): a
  pixel's diamond is the points within half a pixel of its centre, counting
  x and y distance together. Its upper edges and top corner are inside, and
  its lower edges and side corners are outside. The last pixel is dropped
  when the line ends inside its diamond. The pixel before the first is
  drawn when the line starts inside that pixel's diamond. Scene 49 finds an
  exception at the start of a steep line going up. A start on the upper
  right edge (5/16 right and 3/16 up) or on the top corner is outside, so
  the hardware draws no pixel there. That is 4 pixels, and it is not
  modelled.
- **Open:** the 3D depth anchor. geprobe 7 scene 48 draws eight more shapes
  from each corner in both windings. With the corners and vertex depths
  above, and its one off depth moved, 78 of the 90 3D triangles in scenes
  17, 36 and 48 match on every pixel from some corner. The leftmost corner
  matches 57 of them and the top 58. No rule tried picks the matching
  corner (bottom, right, submission order, depth, the corner opposite the
  longest or shortest edge, coarser comparisons). Nor does starting the
  plane at a point that is not a corner, or rounding the step differently.
  Leftmost stays. A GPU backend shares the vertex depths and interpolates them
  itself.

## Texture coordinates on sprites and lines

Where a pixel centre lands exactly on a texel boundary, the step decides
which texel is read (geprobe step 12 and geprobe 5 scene 28, fw 6.60):

- **A sprite** ramps u from its left edge and v from its top edge (swapped
  when the sprite is transposed), whichever vertex gave that edge, by a step
  of texels a sixteenth truncated toward zero to 2^-16. Two texels onto
  seven pixels with the corners given bottom-right first read the far texel
  at the boundary; ramping from the first vertex read the near one.
- **A through-mode line** takes u and v at the pixel centre's projection,
  like colour and depth, by a step truncated to 2^-24 a sixteenth: at an
  exact boundary it reads the texel before. A 3D line divides by w at the
  same point.

## Known differences

- **The data cache is not modelled.** geprobe 4's step 35, repeated as
  geprobe 5's step 60, builds its list in a `memalign(16, 64)` block through
  the uncached alias. The block's cache line also holds the heap header, which
  was written through the cache. The cache therefore holds a dirty line with
  the list's old contents, and when the driver writes the cache back the list
  is overwritten before the GE reads it. geprobe 5 step 61 shows it directly:
  such a block reads back changed in 16 of 16 words after
  `sceKernelDcacheWritebackAll`. On the PSP the step switched fw 6.60 off.
  psprecomp has no cache and runs it, and step 61 reads 0 words changed.
  Modelling the cache is out of scope: the step is `KNOWN_CRASH` from geprobe 6
  on, and step 61 stays a difference.

## Lighting

The lit colour is computed in bytes (`src/hle/ge.c` light_vertex, lit_mul;
geprobe 7, fw 6.60):

- A byte x stands for (x + 1/2)/256, so the product of two bytes is
  floor((2a + 1)(2b + 1) / 1024). Scene 43 steps every byte against 255,
  192 and 128, as light and as material, and N.L through a coloured light.
  All 1280 of its points fit.
- N.L, the specular power, attenuation and the spot factor become bytes
  first, as floor(256 x) up to 255.
- Each light adds spot × (attenuation × (ambient + diffuse)), product by
  product. Its specular is added separately, with the same factors. Every
  value of geprobe 6 scene 34 fits, coloured rows included, and 790 of
  scene 35's 800 points fit. The 10 others are one step low, all in the two
  grids that go through the GE's power function (the specular with
  coefficient 8 and the spot with exponent 4), each where the power lands
  just below a byte boundary.
- In single-colour mode the specular is added in, and the total is clamped
  to 255 per vertex.
- In separate-specular mode the specular is the vertex's secondary colour,
  clamped on its own. The rasterizer interpolates it as three more planes
  and adds it to each pixel after the texture function (psp_vertex.spec).
  In scene 16, 46 of 72 triangle channels match exactly this way, against
  9 with the specular folded into one colour.

## Patches

Bezier and spline patches are tessellated by psprecomp (geprobe 7 scene 44,
fw 6.60):

- **Steps.** Step i of a division sits on a 1/256 grid, cut toward the
  nearer end of the piece: floor(256 i/div)/256 up to the middle, and
  1 - floor(256 (div - i)/div)/256 past it. All 168 Bezier and open-spline
  weights of scene 44 fit.
- **Weights.** The weights at those steps are exact.
- **Colour.** The blend is cut to 1/256, then rounded up to a whole step,
  so 13.0008 reads 13 and 63.75 reads 64.
- **Splines.** Splines use the same grid. The weights of fill/fill splines
  do not fit yet.

## Still open after geprobe 8

These are what geprobe 8 (fw 6.60) still shows psprecomp getting wrong,
with pixels off on run 8. Run 8 drew every scene it shares with run 7 to
the byte the same. Details on the geprobe 7 items, and what further probing
could settle, are in `fw660-run7/findings/geprobe.md`.

- **Vertex depth** (scene 45: 837 pixels; scene 48's depth: 860). The
  depth arithmetic above is within half a step of the GE's on every point,
  but not equal to it.
- **The 3D depth anchor** (scene 17's depth: 303; scene 27's: 1867; scene
  36's: 45). No vertex rule fits; see Depth values.
- **Patch vertices** (scenes 22, 23 and 26: 610, 1876 and 1087 pixels;
  scene 38: 28; scene 44: 74; scene 52: 600). Spline weights are
  unsettled. geprobe 8 scene 52 reads 3244 of them, every edge mode at
  five divisions. Exact Cox-de Boor weights on the 1/256 grid, through the
  colour rule above, fit 2951. All 112 from open/open 4-column splines (a
  Bezier piece) fit, but only 670 of 811 fill/fill ones. Those read as if
  weighted by 256/255: ceil(256 w) fits 763. On a uniform span three of
  the four weights do so at every step, and the third from the nearer end
  reads one lower at 1/4 and 1/3. Ruled out: t² and t³ cut to 6 to 16 bits
  (at best 3058), Cox-de Boor in fixed point (3046), de Boor's algorithm on
  the colours with each level rounded (3016), and a Bezier conversion of
  each span whose control values are rounded (no values fit fill/fill).
  geprobe 9 scenes 53 and 54 read 16302 more at 40 and 48 steps a span,
  at control levels 255, 254 and 129 and in neighbouring pairs. Every one
  of their 3920 weights reads as one value at all three levels, so the GE
  computes a weight off by up to about 0.55/256 and then colours by it as
  above; the colour arithmetic is not the cause. Exact weights fit 2984 of
  the 3920 (any colour cut from 1/32 to 1/65536 does about as well). The
  weights are not any cubic evaluated exactly, even w0 = (1-t)^3/6 on a
  uniform span: no four Bernstein control values reproduce its 90 steps.
  t^3/6 as a chain of rounded products fits 82 of 90, and power-form
  weights with rounded t^2 and t^3, evaluated from the nearer end or not,
  3002 of 3920. The tiny weights near a span's end read below exact
  (t^3/6 at t = 19/256 is under half of it). The points' positions differ
  from psprecomp's at 10-20% of samples, but patch positions go through
  psprecomp's own projection, so that does not separate the two.
  psprecomp's tessellated positions are its own, which is why patches
  keep the older projection rule.
- **Lit triangles** (scene 16: 325 pixels). Every pixel off is one step
  in a channel, and all fall in the same four triangles of each fan (the
  two on either side of the vertical, on the left), whatever the light:
  directional, point and single-colour alike, and none in the spot fan. So
  it is the triangles' shape, not the lighting arithmetic. Anchoring every
  3D plane at the top, bottom, rightmost, first or last vertex instead
  makes scene 16 worse (395 to 1079), so it joins the 3D anchor question
  above.
- **Morph blends** (scene 21: 400) and **skinned corners** (scene 20: 54)
  are unchanged by the projection change.
- **Point and spot lights** (scene 35: 10 pixels), each one step low, all
  where the GE's power function is used.
- **Steep line starts** (scene 49: 4 pixels): a start on a diamond's upper
  right edge or top corner, going up.

## Validation

The host's expanded scene suite uses `PSPRECOMP_GE_CAPTURE_POLLS=740,915,...`
with `PSPRECOMP_GE_CAPTURE=<prefix>` to record multiple frames in one run.
Each increasing poll number selects the first qualifying frame beginning at
or after that controller poll; files are `<prefix>-<poll>.gcap`. The existing
MINCMDS/MINMEAN filters still apply, and the log records actual start/end
polls. POLLS takes precedence over the legacy one-shot FRAME selector.
Malformed schedules disable capture with a diagnostic. Synthetic
`test_ge_capture` cases cover selection, memory snapshot contents, invalid
schedules, and the legacy selector.

Six Last Raven scene captures now include garage, mission ground/smoke and
combat. Each is replayed twice per backend with identical RGB output on the
repeats; normalized software-versus-GL RMSE ranges from 0.002474 to 0.003105.
Both combat frames now execute their 801 stencil-enabled draws and 8/9 line
draws, with no unsupported counts for either feature. The host's synthetic
suite passes 430 exact RGBA assertions per backend, including alpha clears,
stencil outcomes, depth rejection, texture alpha and destination-alpha blending.
RGB agreement alone still cannot establish completeness. Reduced-bit-depth
stencil and render-target allocation history need dedicated tests; the capture
runner rejects multi-list frames until the
recorder can preserve per-list memory lifetimes. The host's
`docs/RENDER-CHECKS.md` carries the commands, regions and limitations.

The software backend is checked against hand-built display lists in
`tests/test_raster.c` — synthetic, no game data, asserting pixel positions rather
than pixel counts. A backend that fills the whole screen and one that fills the
right rectangle both report a nonzero count; only one is correct.

Any GPU backend is checked against the software backend on the same list. That
is the same oracle arrangement the rest of the project uses (see
[ORACLE.md](ORACLE.md)): a reference you diff against, not a dependency you
link.

## Prior art

[sal063/PSP-recompilation-project](https://github.com/sal063/PSP-recompilation-project)
independently built an SDL3 + Vulkan GE backend, validated against PPSSPP's
software renderer. It demonstrates the approach is sound. That it chose a
different API is not an argument against the choice above: what it evidences
is that display-list translation validated against a software reference works,
which is the part both plans share.

This design was written from the GE's documented behaviour and the shape of our
existing interpreter, not from reading theirs — deliberately, because that
project incorporates GPL code from PPSSPP in parts of its HLE and psprecomp
keeps a hard MIT boundary. Their *result* is evidence the problem is tractable;
their *implementation* is not something this project can borrow from.
