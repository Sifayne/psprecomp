# MPEG bookkeeping observation probe

This original MIT probe uses the declarations and structures in BSD PSPSDK
`src/mpeg/pspmpeg.h` at `654ac51fc73fbf7ad9350fac90e39e66e7a0b1c6`, plus its
import stubs. It incorporates no PPSSPP implementation source; the reference
is the observable output of the PPSSPPHeadless executable, which the parent
project's provenance policy permits (see its `docs/PROVENANCE.md`). The
parent project's audit retains the earlier source exposure.

From the runtime root, with a checkout of that SDK commit
(`git clone https://github.com/pspdev/pspsdk && git -C pspsdk checkout
654ac51fc73fbf7ad9350fac90e39e66e7a0b1c6`):

```sh
python3 tests/provenance/make_imports.py /path/to/pspsdk tests/provenance/mpeg/imports.json
python3 tests/provenance/run_probe.py /usr/bin/PPSSPPHeadless /tmp/mpeg-capture --suite mpeg
cp /tmp/mpeg-capture/observed.txt tests/provenance/mpeg/observed.txt
cp /tmp/mpeg-capture/metadata.json tests/provenance/mpeg/capture.json
python3 tests/provenance/mpeg/derive.py
/path/to/build/tests/test_mpeg_observed tests/provenance/mpeg/observed.txt
```

`capture.json` records source/executable hashes. These are external emulator
observations, not hardware results. The native test compiles the same C stimuli
and compares all 148 records (1,564 checks). Firmware AV-module loading is
external setup; the native harness already links the HLE modules and does not
test their loader. The parent `scripts/test-sdk-mpeg-layout.py` separately
compiles 20 layout assertions against the actual SDK for MIPS o32.

The experiments cover memory queries at five SDK versions; ring construction
and context creation before any initialization, and a repeated initialization;
eight ring packet counts including wrapped multiplication; undersized, exact
and oversized ring storage; poisoned ring fields and guards; undersized
contexts; four context alignments; a copied opaque handle; ES allocation,
exhaustion and reuse; AU initialization before and after ES allocation; a
zeroed stream header; synthetic stream headers with distinct big-endian words,
the same words little-endian, unaligned and zero offsets, a wrong magic, and
version tags swept from `0000` to `1015`; stream registration of both types
with two forms of unregistration; an empty video fetch; and three callback
packet transfers. `abi.S` bridges the compiler's stack arguments to syscall
registers for six/seven-argument APIs.

Each stdout record is `kind case values...`. Kinds 0/1/2 are query/init
results (kind 1 case 1 is the repeated init); 3/4 are ring construction result
and sixteen words; 5/6 are context result, handle, availability and ring
words; 7 is ATRAC size query; 8/9 and 13/14 are AU initialization and eight
words; 10 is ES allocation; 11 is ring state after destruction; 12 completes
the probe; 15 records zeroed-header/empty-fetch results; 16/17 record callback
results, arguments, availability and ring words; 18 is construction, creation,
handle and availability before initialization; 19 records the size query's
result and output and the offset query's result and output for each synthetic
header; 20 records stream handles, their differences and re-registration after
each unregistration; 21 is the offset query's result and output for each
swept tag.

Pointers into context/data/ring are normalized to `a0/a1/a2` prefixes plus
offset. Callback and handle-variable addresses are `f0000000`/`f1000000`.
Pointers outside our storage are recorded as `ffffffff`, with their differences
kept as plain values. All other values are compared unchanged, including poison
and guard words. The ring's SDK-reserved word at +36 remains untouched; +44 is
zero even with caller GP set to `24681357`. The callback transfers move both
counters and occupied count together in these stimuli; their behavior under
real decoding is not inferred from that equality, and the runtime names those
two words by position.

`derive.py` obtains capacities, error values, ES sizes, the handle offset, the
two header field positions, the accepted tag range, the header result code and
the stream-handle step only from the recorded rows. It verifies packet-query
multiplication, all four context alignments, big-endian field storage, the
refusal order (magic, then tag, then offset), and that the tag range's
neighbours on both sides refuse, before generating `src/hle/mpeg_observed.h`.
The synthetic headers hold no game data; their eight-byte opening follows the
header this game itself supplies, captured once in the parent project's
`reports/146-header-capture.log`.

The host keeps InitAu's supplied buffer association privately because the
observed initial SDK `iEsBuffer` field is zero. For filled ATRAC AUs the host
publishes its payload address in that SDK field so guest copies can be decoded.
The observations above do not validate that filled-AU policy or media decoding.
The separate original synthetic-PES test checks host timestamp layout and
payload delivery. Video/audio timing, short-read EOF inference, packet release
under active decoding, the base value of stream handles, maximum context/AU
capacity, arbitrary malformed pointers and full firmware memory contents remain
host policies or unverified behavior. No full decoder equivalence is claimed.
