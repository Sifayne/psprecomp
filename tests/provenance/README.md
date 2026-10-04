# Random-unit observation probe

These probe sources were authored for this project on 2026-09-19. They use
two imports documented by the BSD-licensed PSPSDK (`sceIoWrite` and
`sceKernelExitGame`) and raw Allegrex instruction encodings. They contain no
game data and link no emulator library. Project-authored files use the
project's MIT license.

The reference is **observable output from an external executable**. No
PPSSPP implementation source is read by this workflow. The recorded executable
is the installed Arch package `ppsspp 1.20.4-4.1`; its SHA-256, probe and source
hashes, compiler versions and command are in `capture.json`. This is not a
physical PSP observation or proof of hardware accuracy. Earlier source
exposure is recorded separately in the parent project's `docs/PROVENANCE.md`;
this experiment does not erase it or certify the entire runtime.

## Reproduce

With Clang's MIPS target and LLD installed, from the runtime root:

```sh
python3 tests/provenance/run_probe.py /path/to/PPSSPPHeadless /tmp/vrnd-capture
python3 tests/provenance/fit.py /tmp/vrnd-capture/observed.txt > /tmp/vrnd-capture/fit.json
/path/to/build/tests/test_vrnd_observed /tmp/vrnd-capture/observed.txt
```

The output directory must be new. The wrapper does not download an executable
or emulator source. It saves the full diagnostic log separately and extracts
only the probe's stdout records. The external executable may attempt its
normal user configuration setup; the recorded run used the restricted task
sandbox, where writes outside the project and `/tmp` were denied.

The 54,809 observations cover seeded streams, all 256 individual RCX input
bits, 4,096 mixed RCX inputs, 128 varied seeds, 1,000 carry-boundary states,
all 4,096 D-prefix patterns for each of 12 type/size combinations, 116
S-prefix seed cases, and the initial state/revision. `vrnd-observed.txt` is
a deterministic 1,420-record subset selected by `run_probe.py` for CTest;
full validation runs on `observed.txt`. No reference executable is needed
to run the checked-in test fixture.

## Derivation

`fit.py` accepts only recorded words. One-bit interventions show four
32-bit quantities stored in paired low halfwords, plus a fifth quantity
distributed across eight nibbles. They identify an affine first component,
the 32 columns of a bit-linear second component, and affine coefficients
for a fourth component. Carry-boundary interventions distinguish candidate
carry equations that ordinary varied inputs cannot separate. The fitter
searches small integer coefficient/shift models, requiring a unique match;
it checks a further 3,968 mixed states excluded from fitting. Seed experiments
identify constant and seed-controlled bits independently.

Vector records observe lane values, all eight resulting RCX registers and
all three resulting prefix controls. They establish reverse generation order,
float mantissa formatting, and which D-prefix controls apply to the first
generated value. The native comparison checks every recorded word, including
state advancement during masking and consumption of S/T/D prefixes. These
are empirical results over the stated inputs, not a complete specification
of untested aliases, floating-point corner cases or hardware revisions.

Each line consists of hexadecimal 32-bit words. Scalar lines contain
`kind id before[8] result after[8]`; vector lines (kind 9) contain
`9 id before[8] lanes[4] after[8] prefixes[3]`. For vectors, `id >> 12` selects
integer, float-1 or float-2 in groups of four vector sizes, and `id & 4095`
is D's input prefix. Kind 10 identifies the S prefix applied before seeding
with `0xbf400001`; kind 11 identifies the observed revision and startup RCX.
