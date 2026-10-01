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
| 34 | (v6) lighting arithmetic: one flat quad per normal, 9 rows of scene 16's 12 fan normals under one change each (raw, normalised normals, normalised light, y-z normals, a coloured light, a coloured material, scene 16's lower-left fan exactly, specular 12 and 1) | 8888 |
| 35 | (v6) point and spot lights on grids of points, one pixel per lit vertex: a point light with attenuation, a spot (exponent 4, cutoff 0.9), a point light's specular, a spot (exponent 1.5, cutoff 0.5) | 8888 |
| 36 | (v6) 3D depth planes: four triangle shapes, each given from each of its corners and in both windings; plus the depth buffer | 8888 |
| 37 | (v6) through-mode lines whose ends fall on every sixteenth: across both ways with the end's and the start's fraction, shallow diagonals to x fractions 0.625 and 0.4375, steep lines down and up | 8888 |
| 38 | (v6) patches drawn as points, each generated vertex one pixel: Bezier at divisions 3, 5, 6, 7, 12 and splines, over a flat grid with bilinear and alternating colours and over scene 22's curved grid | 8888 |
| 39 | (v6) colour gradient precision: through-mode triangles 5 to 211 pixels wide, at whole and fractional corners; scene 17's 3D quads at several widths; scene 20's red-green-blue triangle | 8888 |
| 40 | (v6) morph weights over colour gradients whose two sets differ by 1 (does a blend keep the fraction), with the unmorphed gradients for reference | 8888 |
| 41 | (v6) bounding boxes at the camera, one list and step each, no dump: across the camera plane at four distances from the axis, behind the camera, between the camera and the near plane, across the near plane | - |
| 42 | (v6) indexed draws by hand: whether PRIM moves IADDR, VADDR or both (16- and 8-bit indices, and unindexed), and whether an indexed BBOX moves them | 8888 |
| 43 | (v7) lit colour arithmetic, one point per vertex, every channel stepped 0-255: the light's diffuse colour, the material's, the material under a 0x80C0FF light, the light over a 0x80C0FF material, and N.L from 1 to 0 under a 0x80C0FF light | 8888 |
| 44 | (v7) patch basis weights as points: one control column lit red and one row green, for each of the four, over scene 38's eight patch kinds | 8888 |
| 45 | (v7) one point per eye depth, 3840 of them from -1.05 to -99, each coloured by its index; plus the depth buffer | 8888 |
| 46 | (v7) one colour and depth gradient over 44 areas: through-mode right triangles 200 and 200.4375 pixels wide, 3 to 12.2 high; plus the depth buffer | 8888 |
| 47 | (v7) scene 46's triangles in 3D at the same screen positions | 8888 |
| 48 | (v7) 3D depth planes: eight more shapes, each from each corner and in both windings; plus the depth buffer | 8888 |
| 49 | (v7) steep through-mode lines, red to green, one per sixteenth across | 8888 |

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

Firmware 6.60 switched itself off in that repeated step in version 5, just
after `EnQueue`, and the check after it found all 16 words of such a block
replaced by the write-back; version 6 no longer runs it ("not run"). New in
version 6, after the others: `sceGeContinue` with nothing paused; a list
queued with its stall address at its start, and one with it just after a
SIGNAL, each then moved to the end with `sceGeListUpdateStallAddr` (what the
peeks say meanwhile, and when the handlers run); how long 100 sprites take at
480x272, 64x64 and 16x16, from the system clock read in the handlers; and
last, since it may not come back, a PAUSE whose own signal handler calls
`sceGeContinue`.

Version 7 adds scenes 43-49 after the callback steps, so every earlier step
keeps its version 6 number. Each one aims at something geprobe 6 left open
(`fw660-run6/findings/geprobe.md`).

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
