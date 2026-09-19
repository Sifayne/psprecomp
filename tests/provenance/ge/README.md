# GE callback observation probe

This project-authored MIT probe constructs small command lists using the
packet layouts independently documented by BSD PSPSDK's `sceGuSignal.c`,
`sceGuFinishId.c` and `sceGuCallList.c` at commit
`654ac51fc73fbf7ad9350fac90e39e66e7a0b1c6`. Callback structure/API declarations
come from `pspge.h`; SDK version set/get declarations come from `pspsysmem.h`.
No emulator implementation source or library is incorporated. Earlier source
exposure is retained in the parent project's provenance audit.

From the runtime root:

```sh
python3 tests/provenance/make_imports.py /path/to/pspsdk tests/provenance/ge/imports.json
python3 tests/provenance/run_probe.py /path/to/PPSSPPHeadless /tmp/ge-capture --suite ge
/path/to/build/tests/test_ge_callback /tmp/ge-capture/observed.txt
```

`capture.json` records input and executable hashes. This is external-executable
output, not physical PSP evidence. The native test compiles the same probe C
file with adapters for imports and guest addresses, comparing all 112 records.

The five list scenarios are FINISH/END; SIGNAL wait plus FINISH; SIGNAL nowait
plus FINISH; a SIGNAL inside a CALL/RET sublist; and FINISH queued with CPU
interrupt delivery masked. The probe records both callback arguments,
the otherwise undocumented third argument, module GP, S/T/D/RCX0 controls,
and whether callbacks ran before enqueue returned. Callback mutations of GP
and controls are checked against the caller's restored values.

The first four SDK versions sample unset, `0x01ffffff`, `0x02000000` and the
game's `0x06030010`. Binary search then locates the callback third-argument
boundary without embedding a threshold. It observes zero through
`0x02000010` and the address after END starting at `0x02000011`; 32 separately
varied versions check that inferred split. The address is normalized to a
main-list byte offset or `0x10000` plus a sublist byte offset.

The game import identifier `0x1b4217bc` is an opaque input from ULUS10567's
import table. Its call at guest address `0x002f57a0` supplies `0x06030010`.
The probe independently observes that this import updates the SDK getter and
callback third-argument behavior. It is registered unnamed; no speculative
firmware symbol name is assigned.

Record kinds: 0 is the scenario result and restored caller state; 1 is a
callback event; 2 is one SDK-version experiment (`kind version third_argument
setter_result getter_result`); 3 records the last zero/first nonzero boundary.
Scenarios are encoded as `16 * version_index + list_index`.

The runtime's callback-table capacity and errors for invalid pointers/IDs or
table exhaustion use explicit host policy and SDK error categories. They are
not measured exact firmware errors. Callback removal, SIGNAL pause/resume,
arbitrary long-list CPU/GE overlap and physical GPU timing are outside this
fixture. The short-list observations do not certify general hardware timing.
