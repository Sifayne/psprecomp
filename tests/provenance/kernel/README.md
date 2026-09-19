# Kernel observation probe

This original MIT probe uses the BSD PSPSDK at
`654ac51fc73fbf7ad9350fac90e39e66e7a0b1c6`: `pspsuspend.h`,
`pspthreadman.h`, `pspsysmem.h`, `pspkerror.h`, and their import declarations.
No PPSSPP implementation was read or incorporated. Historical exposure remains
recorded in the parent project's provenance audit.

The opaque ID `91de343c` was independently read from the SysMem import table of
the user's `ACLR_App.elf` (SHA256
`e5a4511acda7f06b1d94996ecc9364a0b9e55485cfbbf655305f0a3a94b0a752`).
No guessed symbol name or game bytes are included here.

From the runtime root:

```sh
python3 tests/provenance/make_imports.py /path/to/pspsdk tests/provenance/kernel/imports.json
python3 tests/provenance/run_probe.py /path/to/PPSSPPHeadless /tmp/kernel-capture --suite kernel
/path/to/build/tests/test_kernel_observed /tmp/kernel-capture/observed.txt
```

All 45 records are compared by compiling the same C stimuli against native
adapters (473 checks). `capture.json` records source/executable hashes. The
observations are from an external emulator executable, not physical hardware.

Records contain eight hex words: kind, case, and six result fields. Kind 0
records volatile-memory return value, output address, output size and worker
release flag. It exercises type rejection, duplicate acquisition, redundant
release, all four output-pointer combinations and one blocked acquisition.
The recorded successful outputs are `08400000` and `00400000`; invalid type
returns `80000107`, busy TryLock `802b0200`, and redundant Unlock `800201ae`.
The lower-priority releaser is preempted inside Unlock before updating its flag.
Multiple-waiter FIFO ordering, queue capacity and invalid mapped-output errors
remain explicitly host policies rather than measured firmware properties.

Kinds 1/2 record dormant and completed waits for six exit statuses, with normal
and callback waits, null timeouts, zero timeouts and unchanged nonzero timeouts.
Kind 3 checks zero/self/unknown IDs. Kind 4 measures timeout then successful
delayed completion for both APIs. Successful remaining time is normalized to
`0 < remaining < initial`, avoiding a false exact-timing claim. Callback
notification ordering and deletion during a wait are not measured here.

Kind 5 records six opaque-import setter results and independent getter readback.
Kind 6 is the completion marker. `abi.S` bridges Clang's o32 stack arguments to
the syscall argument registers for the six-argument CreateThread call. The
initial exploratory capture without that bridge did not complete and is not
validation evidence.
