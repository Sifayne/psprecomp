# geprobe

A PSP homebrew program, built from PSPSDK (BSD) alone, that draws small test
scenes with the GE and saves the framebuffer after each one. It gives the
software renderer and `src/hle/ge.c` reference frames from real hardware for
the features they lack (dithering, skinning, Bezier and spline patches,
bounding-box jumps, GE callbacks) and for the ones they implement from
pspautotests captures (texture functions, filtering, CLUTs, fog, lighting,
blending, depth, clipping, the per-pixel tests).

| # | scene | format |
|---|---|---|
| 01-04 | triangles, slivers, sub-pixel and half-pixel vertices, flat shading, lines, points, sprites, a long gradient | 8888, 5650, 5551, 4444 |
| 05-10 | gradients and flat values with dithering off, with an ordinary matrix, and with an extreme one | 5650, 5551, 4444, 8888 |
| 11 | twelve blend equation and factor pairs over a gradient | 8888 |
| 12 | a 16x16 texture magnified and minified, nearest and linear, clamp and repeat, half-texel offsets | 8888 |
| 13 | the five texture functions, RGB and RGBA, with and without colour doubling | 8888 |
| 14 | a T8 texture through 8888 and 5650 palettes with shift, mask and start offset | 8888 |
| 15 | fog over stepped depths and a floor | 8888 |
| 16 | directional, point and spot lights; ambient, diffuse, specular; material colours | 8888 |
| 17 | depth functions and interpenetration, plus the depth buffer itself | 8888 |
| 18 | triangles crossing the near plane and far outside the screen, clip planes on and off | 8888 |
| 19 | alpha test, colour test, stencil, logic ops, pixel mask | 8888 |
| 20 | skinning with 2 and 3 weights, float and 8-bit | 8888 |
| 21 | morphing between two vertex sets | 8888 |
| 22 | Bezier patches at several subdivisions, as triangles, lines and points, and textured with generated UVs | 8888 |
| 23 | spline patches with each open/closed edge combination | 8888 |
| 24 | bounding-box jumps: one marker per box, drawn only if the GE finds the box visible. Each box is its own list and step, and a list still running after a second is broken off with `sceGeBreak(1)` and logged. Versions 2 and 3 called `sceGuGetMemory` inside `sceGuBeginObject`/`sceGuEndObject`, which lets the GE reach libgu's placeholder BJUMP (target 0) before it is patched; with matrices set, an invisible box took it and firmware 6.60 switched itself off. Version 4 puts both vertex blocks in the list first | 8888 |
| 25 | (v5) specular and diffuse curves: one flat quad per normal (N.L = N.H from 1 to 0.45, plus two unnormalised normals), specular coefficients 1-32, powered diffuse 4 and 12, plain diffuse; two wide quads for a fixed or local eye | 8888 |
| 26 | (v5) Bezier patches over a flat, evenly spaced grid at divisions 1-4 (exact bilinear surface) and as a spline; curved patches at divisions 1 and 2; a colour bump; ten morph weight pairs on flat quads; one triangle unskinned and skinned through identity bones (float 1, float 0.5+0.5, 8-bit 0x80); two triangles morphed in position | 8888 |
| 27 | (v5) depth interpolation: through-mode triangles with given z (full range, nearly flat, constant, steep in y), lines with z, 3D quads at one depth and sloping in y and in x, a 3D line; plus the depth buffer | 8888 |
| 28 | (v5) a through-mode vertex at x = -5000 and one at 5000, float and 16-bit; 16-bit sprites at x -10, 4100, -4086 and float ones at 4100, -10; 2 texels onto 7 pixels (plain, flipped in u, v, both, corners reversed; nearest and linear) and onto 70; lines with texture coordinates, through mode and 3D | 8888 |
| 29 | (v5) the eight logic ops scene 19 left out; scene 19's stencil band with EQUAL rectangles wholly inside y 112-142; stencil writes under PMSK2 (REPLACE, INCR, INVERT); clears with the pixel mask, logic op, dither, colour and alpha tests, PMSK2 on a stencil clear, and blending | 8888 |
| 30-32 | (v5) on 5650, 5551 and 4444: four pixel masks writing white and black, the colour test, four logic ops, the clears of scene 29, stencil writes under PMSK2 | 5650, 5551, 4444 |
| 33 | (v5) more bounding boxes, one list and step each, no dump: behind the camera and far to the side, across the camera plane, against a small scissor, outside a half-size viewport's clip volume but on screen, a PRIM straight after BBOX with no VADDR (does BBOX advance it), a hidden box drawn without BJUMP | - |

After the scenes it records which GE callbacks run, with which arguments and
when, through libgu (signal and finish) and through a raw `sceGe` list.

Firmware 6.60 switched itself off in version 4's raw-list step, which had
run in version 1, and the step's log lines went with it. Version 5 no longer
runs that step ("not run") and splits it into one step per call, with the
list in a static 64-byte-aligned buffer written back from the cache
(version 4's sat in a `memalign` block sharing a cache line with its malloc
header, written through the uncached alias). Then, new in version 5:
`ListSync(peek)` between `ListSync(wait)` and `DrawSync`; a PAUSE signal
(which handler, and whether the list waits for `sceGeContinue`); a long list
without a stall (200 full-screen sprites between SIGNALs, to see how much
runs inside `EnQueue`); how many callbacks `sceGeSetCallback` takes and the
error after; version 4's raw-list step again, call for call with the log
flushed after each call, and last a check, without the GE, of whether such a
`memalign` block's words survive a cache write-back.

## Build

Needs the pspdev toolchain (`psp-gcc`, `psp-config` and PSPSDK) on `PATH`:

    make

This produces `EBOOT.PBP`. The CMake build does not include it.

## Run

Copy `EBOOT.PBP` to `ms0:/PSP/GAME/geprobe/` and start it from the XMB. It
needs no input; the screen flickers through the scenes, then shows the log
and returns to the XMB by itself.

It writes, beside the EBOOT, `geprobe.txt` and one `ge_NN_<name>.raw` per
scene (about 14 MB in all): 480 x 272 pixels, rows packed, in the scene's
framebuffer format, exactly as the GE wrote VRAM. `raw2png.py` turns them into
PNGs for looking at; comparisons should use the raw files.

## Compare with psprecomp

    allegrexrecomp interp geprobe.prx --dispatch --budget 4000000000 --drain 200

psprecomp's software renderer draws into the same guest VRAM, so its run
leaves the same files under `./ms/PSP/GAME/geprobe/`.
