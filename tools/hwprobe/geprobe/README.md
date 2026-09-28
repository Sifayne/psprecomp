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
| 24 | bounding-box jumps: one marker per box, drawn only if the GE finds the box visible. Each box is its own list and step, and a list still running after a second is broken off with `sceGeBreak(1)` and logged: version 2 drew all eight in one list and firmware 6.60 never finished it | 8888 |

After the scenes it records which GE callbacks run, with which arguments and
when, through libgu (signal and finish) and through a raw `sceGe` list.

## Build

Needs the pspdev toolchain (`psp-gcc`, `psp-config` and PSPSDK) on `PATH`:

    make

This produces `EBOOT.PBP`. The CMake build does not include it.

## Run

Copy `EBOOT.PBP` to `ms0:/PSP/GAME/geprobe/` and start it from the XMB. It
needs no input; the screen flickers through the scenes, then shows the log
and returns to the XMB by itself.

It writes, beside the EBOOT, `geprobe.txt` and one `ge_NN_<name>.raw` per
scene (about 9 MB in all): 480 x 272 pixels, rows packed, in the scene's
framebuffer format, exactly as the GE wrote VRAM. `raw2png.py` turns them into
PNGs for looking at; comparisons should use the raw files.

## Compare with psprecomp

    allegrexrecomp interp geprobe.prx --dispatch --budget 4000000000 --drain 200

psprecomp's software renderer draws into the same guest VRAM, so its run
leaves the same files under `./ms/PSP/GAME/geprobe/`.
