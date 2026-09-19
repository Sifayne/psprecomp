# Original vertex-stream and palette experiment

This probe was authored for this project. It submits our own command lists and
vertex bytes, then prints nonzero pixels from a 64×64 framebuffer. Command
encodings and declared formats come from BSD PSPSDK commit
`654ac51fc73fbf7ad9350fac90e39e66e7a0b1c6`, `src/gu/pspgu.h`,
`guInternal.h`, and the `sceGu*` command producers. No PPSSPP source is an input.

`make_cases.py` generates stimuli only. `observed.txt` is stdout from running
our compiled ELF in an external `PPSSPPHeadless` executable; `capture.json`
records executable, compiler, source and output hashes. This is an external
behavior comparison, not physical PSP evidence or a claim of unexposed authorship.
Earlier source exposure is retained in the parent project's provenance audit.

Reproduce from the runtime directory:

```sh
python3 tests/provenance/vertices/make_cases.py
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy \
  python3 tests/provenance/run_probe.py /path/to/PPSSPPHeadless /new/output --suite vertices
python3 tests/provenance/vertices/derive.py
```

The SDK import generator and manifest are retained beside the probe. The probe
writes only stdout and its framebuffer; it does not access savedata. The external
executable's `--root` selects `host0`, not the Memory Stick root.

## Observations and reconstruction

- Six initial cases distinguish successive vertex consumption from byte and
  halfword index consumption. Reissuing VADDR resets the stream; zero counts
  consume nothing. Reserved index format 3 is not inferred.
- Powers, adjacent values, and translated/zoomed weight sweeps distinguish
  the unsigned integer scales. `derive.py` tests every positive integer divisor
  in each format's range; exactly 128 and 32768 fit. Float weights include
  negative and greater-than-one values.
- One-hot weights select every palette entry, for one through eight weights
  and all three formats. Two full weights distinguish summing transformed
  inputs from normalizing their sum.
- Three consecutive vertices in poison-padded records exercise SDK field
  order, scalar alignment, end padding, optional fields, and through mode.
  The replacement computes a field descriptor rather than retaining the old
  hand-expanded layout. Through mode consumes weights but bypasses skinning.
- A +Z directional light distinguishes normal rotation, mixed direction and
  translation exclusion across all three weight and normal formats. The
  replacement sums transformed positions and normals per bone, then applies
  the existing world/view pipeline.
- Packed RGB walking-bit and pattern cases expose previously ignored
  5650/5551/4444 colors. Their channel expansion is checked by `derive.py`.
  Alpha semantics are SDK-derived; these ordinary draws do not write alpha.
- Fine float point sweeps bound the effective pixel bias to
  `[0.0234375, 0.0244140625)` pixels. The runtime chooses the lower endpoint
  for points only; no universal subpixel rule is asserted.
- Nine perspective triangles exercise the host's separate model-transform
  branch with identity, rotation, and two-bone sums. Fractional positions avoid
  exact edge ties, isolating the palette transform from raster edge ownership.

The native test includes the same `probe.c` and compares **every pixel**, including
zero pixels, for **517 rasters**. Coverage and unlit RGB are exact. Nine lit
pixels differ by one RGB step in the inherited lighting path; the test permits
and reports that one-step shading tolerance explicitly. No tolerance applies to
coverage. The old consecutive-PRIM and skinning test oracles have been removed.

The parent's `scripts/test-vertex-gl.sh` builds the same native adapter with its
actual SDL/OpenGL backend and runs the nine triangle cases. It asserts nine
`draw_model` calls, so a silent CPU fallback cannot pass as a GPU test. In this
environment the optimized backend passes all nine exactly with
`PSPRECOMP_GL_PERSISTENT=0`. The default persistent-buffer path currently gives
blank triangles at `-O2`, while an `-O0` build passes; that separate host upload
issue remains open. This is an offscreen desktop check, not a hardware/device
acceptance claim.

## Limits and retained discrepancies

The earlier integer-aligned triangle experiments in the parent's ignored
`reports/101-vertices/` and `reports/102-vertices/` expose a one-pixel edge
ownership difference. Adjusting subsequent stimuli avoids that tie for the
palette test; it does **not** fix or erase the raster discrepancy. Those reports
are diagnostic evidence, not passing validation. The current fixture still
checks all its triangle pixels exactly.

Malformed palette cursors, wrapping, reserved formats, morph targets, NaNs,
infinities, every floating-point accumulation order, exact light quantization,
and hardware timing are not established. The SDK producer writes twelve
successive entries at `bone_index * 12`; unsupported out-of-range writes are
ignored without reproducing an unverified wrap rule.
