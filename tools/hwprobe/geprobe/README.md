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
| 50 | (v8) the triangle setup's reciprocal at every 10-bit width: 512 through-mode right triangles 2 pixels high and 512 to 1023 sixteenths wide, depth 16384 to 49152 (a power-of-two numerator), plus 118 with depth 0 to 65535; plus the depth buffer | 8888 |
| 51 | (v8) the same for lines: 512 horizontal through-mode lines 512 to 1023 sixteenths long, each drawn left to right and right to left; plus the depth buffer | 8888 |
| 52 | (v8) spline weights along u as points: every u edge mode, 4 to 6 control columns, divisions 2, 3, 4, 6 and 8, three columns lit (red, green, blue) | 8888 |
| 53 | (v9) spline weights along u as points at 40 steps a span: a uniform span, two uniform spans and two spans with each kind of open end; control colours 255, 254 and 129, and neighbouring columns lit in pairs | 8888 |
| 54 | (v9) the same at 48 steps a span | 8888 |
| 55 | (v10) spline weights read through depth: one patch per spline and control column, 48 steps a span, orthographic, the column at another eye depth, so each point's depth is that weight to 1/32768 (and its red the same weight as a colour); plus both depth dumps | 8888 |
| 56 | (v10) the spline parameter the GE uses: the same splines with no texture coordinates over a 512-texel ramp, nearest, repeated, at texture scale 1, 4 and 16 | 8888 |
| 57 | (v10) scene 48's 144 corners as points, their depths read back and logged, then its 48 triangles in through mode at psprecomp's corners with those depths; plus both depth dumps | 8888 |
| 58 | (v10) scene 48's shape 1 at every sixteenth of a pixel across and down, 32 copies; plus both depth dumps | 8888 |
| 59 | (v11) point depths with no divide: identity projection (w = 1), 1920 eye depths from -0.99 to 0.99 at viewport z scale and centre 32768, then 1920 at sceGuDepthRange(65535, 0)'s; plus both depth dumps | 8888 |
| 60 | (v11) point depths with clip z / clip w fixed: a projection with clip w = -z and clip z = a z, 1280 eye depths from -1 to -100 for each of a = -0.3, 0.45 and -0.82, so each batch would read one depth in exact arithmetic; plus both depth dumps | 8888 |
| 61 | (v11) scene 60's first batch with the eye depth made by a model matrix translation of -37.125, -0.4375 and 0.4375; plus both depth dumps | 8888 |
| 62 | (v12) 1/w read whole: clip z a power of two c, clip w = -z in [c, 2c), viewport z scale 65536 and centre 0, so each point's depth is 1/w's top 16 bits; 6783 w in (1, 2) (every 8-bit significand, random 16- and 24-bit ones, two runs of consecutive ones) and 192 to 1024 in each of eight binades from 2^-3 to 2^13; plus the full depth dump | 8888 |
| 63 | (v12) a projection row read whole: clip w 1 and clip z = cx x + cy y + a z + b, depth clip z's top 16 bits; z as taken in, z - 1 for z just above 1, a z, z + b across alignments of 3 to 18 bits, a carry, a cancellation, and four terms of different sizes | 8888 |
| 64 | (v12) clip z times 1/w read whole: four clip z in [1.5, 2) over scene 62's 1024 w in (2, 4) | 8888 |
| 65 | (v12) world, view and projection combined, read whole: clip w 1, clip z from world and view z rows with translations of 40 and -20 against an eye z under 1, z scales in all three, an x term, and a projection translation cancelling a world one | 8888 |
| 66 | (v12) the probes' perspective (60 degrees, 1 to 100) read whole: 2048 eye depths from -1.02 to -96 | 8888 |
| 67 | (v13) patch control conversion: Bezier strips of 16- and 24-bit constants over 23 binades, carries across the 16-bit grid, exponent edges and rounding ties, 24-bit width twins; generated vertices read whole through depth (as every scene to 82); plus the full depth dump | 8888 |
| 68 | (v13) random cubics along u at every division 1-48, each read as P, -P through the projection and -P through the viewport; a second pattern at 31 divisions; a base with one control moved | 8888 |
| 69 | (v13) scene 68's cubics along v (transposed), and one control on zero read through an exponent ladder and anchor-relative (XYR) placement | 8888 |
| 70 | (v13) one set of cubics at 15 model scales, at clip scales 2^-8 to 2^8, scaled jointly and along one axis, and XYR twins | 8888 |
| 71 | (v13) mixed magnitudes and signs inside a lerp: one-hot ladders, two binades, sign-split pairs, exponent edges, large middle rows, near-zero windows, operand ties | 8888 |
| 72 | (v13) 2D Bezier cells: G, its transpose, reversal and negation at eight division pairs, multi-piece cells, a 2D one-hot ladder, contrast strips, signed patterns, rows in four binades | 8888 |
| 73 | (v13) splines in every edge mode along u and v, 2D splines and transposes, far controls that exceed w, span boundaries under additive blending (double emission) | 8888 |
| 74 | (v13) where the GE tessellates: world-translation and projection cancellation, viewport z halving and centre, shear and rotation, axis and w routing, a random row in world, view or projection | 8888 |
| 75 | (v13) perspective strips and cells, w cancellation, near-equal controls, a 16-bit z coefficient, parameter ramps, a rotated world with a view translation, patch primitives and shade models, and patch triangle colour planes against plain triangles built from the colours read back | 8888 |
| 76 | (v13, own step) extreme magnitudes: constants from 2^-125 to 2^100, random cubics at extreme scales, coarse companions, huge and tiny x, clip scale 2^+-16 | 8888 |
| 77 | (v13, own step) s16 and s8 controls against float twins, GU_INDEX_16BIT and GU_INDEX_8BIT patches, and every s16/s8 control as an anchor point | 8888 |
| 78 | (v13, own step) morphed patches (two targets cancelling, weights 1+1 and 0.5+0.5) and skinned ones (one bone translation) | 8888 |
| 79 | (v13, own step) two 48 x 48 Bezier cells, G and its transpose (2401 vertices each) | 8888 |
| 80 | (v13, own step) divisions 49 to 128: P, -P and a second pattern | 8888 |
| 81 | (v13, own step) divisions 129 to 255 at viewport x scale 1024, each strip twice so every sample is on screen once | 8888 |
| 82 | (v13, last, two steps) two 64 x 64 cells (`patchbig64`), then spline rows of more than 256 samples with each row's end on screen (`patchrows`) | 8888 |

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

Version 8 adds scenes 50-52 after those, so steps 1-89 keep their version 7
numbers. They aim at the two rules geprobe 7 could not settle
(`fw660-run7/findings/geprobe.md`): the gradient reciprocal, read once per
10-bit length for triangles and for lines, and the spline weights.

Version 9 adds scenes 53 and 54 after those, so steps 1-92 keep their
version 8 numbers. Version 8 settled the reciprocal (src/render.c
area_rcp). Scene 52 left the spline weights' inner pair a step off at some
samples with nothing to tell a weight error from colour arithmetic, so
these read the same weights at 40 and 48 steps a span, at three control
colours, and in neighbouring pairs whose sum shows whether the errors
cancel.

Version 10 adds scenes 55-58, so steps 1-94 keep their version 9 numbers.
Run 10 found each spline weight one value at every colour level and a
function of the span and t alone; scene 55 reads the weights through depth,
about 128 times finer than a colour, and scene 56 reads the parameter the
GE uses through a texture. Scene 48's 3D depth planes are off by a step in
one direction only; scene 57 draws its corners as points, reads back the
depth the PSP gives each, and redraws the triangles in through mode at
psprecomp's corners with those depths (`twin_table.inc`, from psprecomp's
projection), so the two frames say whether the 3D path differs from
through mode; scene 58 moves one shape by every sixteenth of a pixel. The
`_depthfull.bin` dumps are the whole 512-pixel stride, which the 480-wide
`_depth.raw` cannot read all of.

Version 11 adds scenes 59-61, so steps 1-98 keep their version 10 numbers.
Version 10 settled the depth plane's corner (src/render.c sw_tri); every 3D
plane still off is a corner's own depth a step off, and scene 45's points
fit psprecomp's depth arithmetic on 2883 of 3720. These take that
arithmetic apart: scene 59 without the divide, scene 60 with the ratio
clip z / clip w fixed so only the reciprocal and the product vary, and
scene 61 with the eye depth made by a model translation.

Version 12 adds scenes 62-66, so steps 1-101 keep their version 11 numbers.
Set 12 showed a point's depth is floor(zc + zs ndc) with zs ndc cut to the
larger term's 16 significant bits first (scene 59, every point), so with zc
0 and zs a power of two that puts zs ndc in [32768, 65536) the depth *is*
ndc's top 16 bits. Scenes 62-66 use that to read each stage whole instead
of a step either side of a rounding. Every input is an integer bit pattern
(xorshift32 from a fixed seed per scene; matrix entries 16-bit, so the
GE's words hold them exactly), and each scene logs a CRC of its points'
eye z for the analysis to check its copy against. Points sit two pixels
apart, 240 a row from row 10, coloured by slot (red low byte, green high)
and batch (blue from 0x40), at viewport scale 256 across and -128 down so
a slot's ndc x and y are exact. Clip z stays inside -w..w: outside it a
point is dropped.

Version 13 adds scenes 67-82, so steps 1-106 keep their version 12 numbers.
Set 13 put a vertex's depth at ge_screen_z exactly; these read patch
positions through it, a generated vertex drawn as a point whose depth is
its z's top 16 bits (or a ladder of lower-precision reads, by design), to
settle how the GE evaluates de Casteljau and de Boor (the lead: each lerp
cuts both operands to the larger one's 16-bit grid, columns first). The
scenes are data: `patch13.py` builds every item (generator order, integer
half-pixel placement, first-fit-decreasing shelves) and writes
`patch13_data.inc`, one command stream per step, which `main.c`'s
`p13_run()` replays; each step logs its item and sample counts and the
CRCs of its control vertices, item records and whole stream. Every step
opens with a 480-point cal band (rows 10 and 12) in that scene's read
modes, whose slots 432-479 read what the GE does with clip z beyond w,
exactly at w, and with negative screen depths. Scenes 76-82 are the risky
ones (extreme ranges, s16/s8/indexed/morphed/skinned controls, 2401- and
4225-vertex patches, divisions over 48, rows of more than 256 samples),
each its own step at the end, logging every batch's GE time and the
list's headroom.

Regenerate the streams after changing `patch13.py` with
`python3 patch13.py emit` (needs numpy; it rewrites `patch13_data.inc`
only when the streams change, and `main.o` depends on it, so `make`
then rebuilds); `python3 patch13.py check <dir>`
checks a run's log CRCs and, per batch, that every expected point is on
its predicted pixel; `python3 patch13.py points <dir> <scene>` lists every
point's inputs, prediction and reading.

Version 14 adds scenes 83-98 after them, so steps 1-123 keep their version 13
numbers. Geprobe 13 left one step of colour off inside some triangles
(scenes 16, 21, 22, 23 and 26), all of them triangles whose middle corner lies
left of the long edge. Across all the earlier dumps, starting colour, fog and
secondary planes from the depth plane's corner instead of the leftmost explains
every one of those pixels and changes no other. These scenes confirm or refute
that over every orientation, vertex order, tie, sub-pixel offset, scissor
case, gradient range, the alpha, fog and secondary planes, and 3D and
perspective twins. Like 67-82 they are replayed from command streams:
`colour14.py emit` writes `c14_data.inc`; `colour14.py sums <dir>` checks each
scene's logged CRCs; `compare`, `decode` and `selfcheck` read the answers.

## Build

Needs the pspdev toolchain (`psp-gcc`, `psp-config` and PSPSDK) on `PATH`:

    make

This produces `EBOOT.PBP`. The CMake build does not include it.

## Run

Copy `EBOOT.PBP` to `ms0:/PSP/GAME/geprobe/` and start it from the XMB. It
needs no input; the screen flickers through the scenes, then shows the log
and returns to the XMB by itself.

It writes, beside the EBOOT, `geprobe.txt` and one `ge_NN_<name>.raw` per
scene (about 25 MB in all): 480 x 272 pixels, rows packed, in the scene's
framebuffer format, exactly as the GE wrote VRAM. `raw2png.py` turns them into
PNGs for looking at; comparisons should use the raw files.
`readout.py <geprobe dir> [scene]` regenerates scenes 62-66's inputs, checks
them against the log's CRCs, and lists each point's inputs and depth;
for scenes 67-82 it hands over to `patch13.py`, and for 83-98 to `colour14.py`.

## Compare with psprecomp

    allegrexrecomp interp geprobe.prx --dispatch --budget 4000000000 --drain 200 --base 0x08804000

psprecomp's software renderer draws into the same guest VRAM, so its run
leaves the same files under `./ms/PSP/GAME/geprobe/`.
