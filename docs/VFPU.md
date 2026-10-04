# The VFPU

The PSP's vector unit is the reason people give for why the PSP has never had a
static recompiler. It is genuinely awkward. It is also **localized**: a VFPU
instruction reads and writes VFPU registers and a handful of control
registers, and nothing about it needs anything but a function call per
instruction in the emitted C. This file says how that is done and where its
numbers come from.

## What it is

COP2 on the Allegrex: a 128-register single-precision vector unit. The register
file is 128 floats, but it is *addressed* as 8 matrices of 4×4, and a single
7-bit register field can name a scalar, a 2/3/4-element row, a 2/3/4-element
column, or a whole matrix depending on bits elsewhere in the instruction.

Three things make it hard to decode:

1. **Split width encoding.** The number of lanes an instruction operates on is
   `((op >> 7) & 1) | ((op >> 14) & 2)`, giving 0–3 for 1–4 lanes. Two
   non-adjacent bits, neither adjacent to the opcode. Decode this wrong and a
   quad operation writes one lane instead of four.
2. **Prefix instructions.** `vpfxs` / `vpfxt` / `vpfxd` do not compute anything;
   they set a register that *modifies the operands of the next instruction* —
   swizzling lanes, negating, forcing constants, masking or saturating writes.
3. **A sprawling opcode space.** The VFPU occupies most of opcodes `0x18`,
   `0x19`, `0x1B`–`0x1E`, `0x32`–`0x3F`, with sub-opcodes in several different
   bit positions depending on the family.

## Decoding and emitting

The decoder (`tools/allegrexrecomp/decode.c`) names 93 VFPU mnemonics:
loads and stores including the unaligned `lvl.q`/`lvr.q`/`svl.q`/`svr.q`,
the COP2 moves and branches, arithmetic, the dot-product family, compares and
`vcmov`, the transcendentals, conversions and colour packing, `vcst`,
`viim`/`vfim`, the matrix ops (`vmmul`, `vtfm2`-`4`, `vmscl`, `vrot`,
`vmmov`, `vmidt`, ...), `vsbn`/`vwbn`, the random generator and the three
prefix instructions.

An encoding inside the VFPU space that none of those match decodes to
`A_VFPU_UNKNOWN` ("`vfpu?`"), which is deliberately *not* `A_INVALID`:

- `A_INVALID` means "this is not an instruction" — probably data misread as
  code, and the analyzer should stop following this path.
- `A_VFPU_UNKNOWN` means "this **is** a VFPU instruction, but not one we can
  name." The analyzer keeps going, and the emitter writes a run-time trap
  (`psp_unimplemented`, naming the address) instead of guessing.

The emitter (`tools/allegrexrecomp/emit.c`) lowers each named instruction to
one call into the runtime (`include/psprecomp/vfpu.h`, `src/vfpu.c`):
`psp_vmmul(vd, vs, vt, size)` and the like. A sub-encoding the runtime does
not recognise inside a family it does traps by name through
`psp_vfpu_unimplemented`.

## Prefixes

The prefix state is thread context, so it lives in `psp_cpu.vfpu_ctrl`
beside the condition codes and the random generator's registers. A prefix
instruction emits `psp_vfpu_set_prefix(n, value)`; the next VFPU instruction
reads all three prefixes and puts them back to the identity (`0xE4`, `0xE4`,
`0`), whether or not it used them, as the hardware does. "No prefix" and
"the identity prefix" are therefore the same state, and the emitted C needs
no knowledge of prefixes at all. Saturation substitutes the bound (so
`vsat0` of -0.0 is +0.0, as pspautotests' hardware capture shows).

## Arithmetic: measured, not approximated

Where the PSP's arithmetic differs from IEEE single precision or libm, the
runtime reproduces the PSP's, from `tools/hwprobe/vfpuprobe` runs on
firmware 6.60 (`fw660*.txt` beside the probe):

- **The dot-product unit.** Every reduction (`vdot`, `vhdp`, `vfad`, `vavg`,
  `vdet`, `vcrsp`, `vmmul` and the transforms) goes through one circuit:
  products to 24+2 bits with round-to-odd, alignment by truncation, an exact
  integer sum and a single rounding at the end (`psp_vfpu_dot`).
- **The transcendentals.** `vsin`, `vcos`, `vasin`, `vexp2`, `vlog2`, `vrcp`,
  `vsqrt`, `vrsq` and their negated forms are the PSP's own fixed-point
  algorithms: a 23-bit reduced argument, a piecewise core per segment and a
  result truncated to 22 significand bits. The cores' constants
  (`src/vfpu_cores.h`) are fitted by `tools/hwprobe/vfpuprobe/gencores.py` to
  the probe's dumps. Every argument of the vsin/vcos and vasin cores is fixed
  by the data; for the others, arguments the dumps skipped rest on the fit,
  which predicted 99.96% of the held-out sweeps.
- **The random generator.** `vrnds`, `vrndi`, `vrndf1` and `vrndf2` run a
  state fitted to 1,624 logged transitions, which it reproduces exactly
  (see `vrnd_next`).
- **Conversions, packing and constants.** `vf2i` rounding, `vi2f` scaling,
  the half-float and colour conversions and all 32 `vcst` constants, against
  the probe's logs.

## What is still open

- **Vector `vrnd`.** The order in which a `.p`/`.t`/`.q` `vrnd` fills its
  lanes, and what a destination prefix that masks a lane does to the state,
  have not been measured; the runtime fills lanes forward. The 3rd
  Birthday's New Game reaches `vrndf2.t`. It is on the next probe set's list
  (ROADMAP.md, *Open work*).

## Measuring a title

`allegrexrecomp cover <module>` reports, over a module's `.text`, how much
decodes, how much is VFPU, and how much is unknown. The third number is
decoder bugs or data. Validated across six real modules the VFPU came to
0.14-0.20% of what decodes, which is why VFPU completion followed the games
rather than preceding them.
