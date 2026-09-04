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

1. **The software path must survive.** It is the oracle. A GPU backend that
   disagrees with it is wrong, and you cannot establish that if the GPU backend
   is the only one. `tests/test_raster.c` asserts *where* pixels land using
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
the 0..65535 scale, colour, texel-unit UVs, the fog byte), `psp_tex_state`
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
| **software** | Working | The reference. Triangles, strips and sprites; points, lines and fans are not drawn. 17 tests in `test_raster.c` assert pixel positions -- texturing, filtering, wrap, depth and clear mode among them. No GPU, no dependencies, runs in CI. |
| **null** | Working | Counts primitives, draws nothing. What the bring-up host uses when the question is "did it ask to draw". |
| **gl33-sdl2** | Not started | The intended presentation path. See below. |

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

`init()` is called once the backend is chosen; both current backends return 0
without doing anything. `shutdown()` and `present()` remain unwired, and
`shutdown()` is not merely an oversight: boot.c skips its teardown whenever a
guest thread is still live, which is the common case, so it would not run
reliably even if it were called.

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

## The GL 3.3 backend, when it happens

Scope it deliberately, because the GE has a large state space and most of it
does not matter until a game is already drawing:

**Prerequisites** — none of these are the backend:
- ~~`psp_render_select` wired to an environment variable in `host/boot.c`, so a
  backend can be chosen without recompiling~~ **done 3 Sep**, `PSPRECOMP_RENDER`
- ~~A seam for a backend the runtime cannot carry~~ **done 3 Sep**,
  `psp_render_register()`
- A per-frame timer. `psp_render_raster_ns()` is cumulative and printed once at
  the end of a run, which cannot demonstrate the 60 fps the M5 gate asks for
- Display-list capture and replay. The gate wants "pixel-comparable on a fixed
  set of display lists"; `09-replay.sh` replays *controller input*, and
  `PSPRECOMP_FRAMES` dumps final images. Recording GE lists to a file and
  replaying them into an arbitrary backend does not exist
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

**The game's frame is nevertheless uniformly white, and that is the scope
rather than a fault.** There is no depth test, no blending and no clear
handling, so a frame is every primitive of that frame painted in submission
order with the last one winning, and the accumulation is white. Fixing it is
the second increment; approximating it now would produce a picture that looks
plausible and is not the software path's, which is the one thing this
arrangement exists to prevent.

**`finish()` had never been called** (fixed the same day). The interface has
documented it as the end of a display list and a good point to flush batched
work since it was written, and `ge.c` called only `draw()`. The software path
never noticed, because it rasterizes each primitive immediately and has
nothing to batch -- so the gap only appears the moment a backend accumulates.

**Second increment** — the state that makes the frame right:
- Depth test and depth writes, which is what stops submission order deciding
  the picture
- Alpha blending and the alpha test
- The scissor, and clear-mode draws
- Diff against the software path on the same list: same pixels, or the backend
  is wrong

**Third increment** — the parts that need real work:
- Texture cache keyed on the GE's texture state (address, format, size, CLUT)
- The transform pipeline, which needs the VFPU matrices to be correct first
- Blending, depth, scissor — each is a small addition once the above holds
- The awkward ones, which no API makes easy: the stencil living in the
  framebuffer's alpha byte, and the texture function's colour doubling. Both
  are shader work either way and are not an argument for or against GL.

## Validation

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
