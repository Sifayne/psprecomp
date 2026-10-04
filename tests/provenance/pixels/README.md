# Pixel observation probe: textures, transfers, masks, clears and depth

This probe was authored for this project. It builds display lists and pixel
data of its own, renders into a 64×64 8888 target under the external
`PPSSPPHeadless` executable's software renderer, and prints what came back.
Command numbers, field packing and vertex declarations come from BSD PSPSDK
commit `654ac51fc73fbf7ad9350fac90e39e66e7a0b1c6` (`guInternal.h`,
`pspgu.h`, and the `sceGuTexImage`, `sceGuClutMode`, `sceGuClutLoad`,
`sceGuCopyImage`, `sceGuTexMode`, `sceGuTexFunc`, `sceGuClear` and
`sceGuDepthBuffer` producers). No PPSSPP source is an input; running the
executable and comparing its output is what the parent project's provenance
policy permits. The probe holds no game data: every texel names its own
coordinates, every palette entry its index, every transfer word its position.

Reproduce from the runtime directory, with a checkout of that SDK commit:

```sh
python3 tests/provenance/make_imports.py /path/to/pspsdk tests/provenance/pixels/imports.json
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy \
  python3 tests/provenance/run_probe.py /usr/bin/PPSSPPHeadless /new/output --suite pixels
cp /new/output/observed.txt tests/provenance/pixels/observed.txt
cp /new/output/metadata.json tests/provenance/pixels/capture.json
python3 tests/provenance/pixels/derive.py
/path/to/build/tests/test_pixels_observed tests/provenance/pixels/observed.txt
```

`capture.json` records executable, compiler, source and output hashes. These
are emulator observations, not physical PSP evidence. The native test includes
the same `probe.c`, runs it through the runtime's software renderer, and
compares all 5,374 records field by field (43,082 checks).

## Records

Each line is `kind case a b c`. Kind 0 is a list's status, nonzero pixel
count and a hash of the whole target; kind 1 is a target pixel by index
`y*64+x`; kind 2 a transfer destination word by the same index; kind 4 a
destination word at an explicit offset; kind 3 hashes of the destination and
source buffers; kind 9 ends the run.

## Experiments and what they establish

- **Texture registers** (100-114): the SDK encoding samples texel (x, y);
  bytes above the address nibble in the width register are ignored, as are
  the base's low four bits; +16 bytes moves four 8888 texels; the stride
  honours 40 and 48 and ignores bit 11; size exponent bit 4 is ignored; a
  declared 1024-texel row repeats with a 512 mask and clamps at texel 511;
  TEX_MODE bit 0 swizzles and bits 1 and 8 do not; the swizzle block shape
  is the one `derive.py` fits to case 112.
- **Formats** (115-117, 126-128): 5650, 5551 and 4444 texels and palette
  entries expand with their high bits replicated downward.
- **Palettes** (118-125): 4-, 8-, 16- and 32-bit indices select entry
  `((raw >> shift) & mask) | (start << 4)`.
- **Texture functions** (129-134, 141-142): modulation is the texel times the
  vertex channel widened by its own top bit, doubled before the shift when
  doubling is on, truncated; decal and replace with an opaque texel return
  the texel. Blend and add are recorded and compared, not modelled here.
- **Wrap and filter** (135-138): clamp and repeat past the width; nearest
  magnification by two; bilinear magnification is recorded and compared.
- **Colour masks** (139-140, 143): 0xE8 keeps every framebuffer bit set in it,
  bit for bit, and 0xD8 (the SDK's colour test) changes nothing.
- **Block transfers** (200-221): 32-bit pixels when START bit 0 is set and
  16-bit otherwise, bit 1 ignored; positions and sizes in pixels, sizes as
  n-1; the stride field masked with 0x7f8, honoured at 0x400 and collapsing
  to zero at 0x7f8, which lands every row on one line; a zero source stride
  rereads one line; the low four address bits ignored; the address high
  byte's cache-alias values 0x48 and 0x88 reach the same memory and 0x18
  copies nothing; transfers into the framebuffer read back as pixels.
- **Clears and depth** (300-321): a clear's own z is what later draws are
  tested against; the colour, stencil and depth bits each write only their
  buffer; a clear under a NEVER test and a clear with depth writes masked off
  still write; ordinary draws leave the alpha byte; all eight comparisons at
  z below, equal to and above the cleared value; the disabled test; the depth
  mask.

`derive.py` checks every rule above against the records and generates
`src/hle/ge_observed.h` (maximum texture size, swizzle block shape, transfer
stride mask and limit, mask register numbers).

## Limits

Mipmapped sampling, texture matrices, 16-bit framebuffers, DXT formats, the
colour test, the alpha and stencil tests, blending under masks, and the exact
rounding of bilinear, blend and add are not derived here; the native
comparison still checks their recorded pixels where the probe draws them.
Depth is observed only through later draws, never read from the depth
buffer. Hardware timing is not addressed.
