# Interrupt observation probe

This project-authored MIT probe uses declarations from the BSD PSPSDK at
`654ac51fc73fbf7ad9350fac90e39e66e7a0b1c6`: `pspintrman.h`, `pspdisplay.h`,
their import stubs, and the `pspkerror.h` interrupt error categories. The
`samples/gu/vsync30FPS` semaphore pattern is also exercised by the native test.
No emulator implementation source or library is incorporated. Historical
source exposure remains in the parent project's provenance audit.

From the runtime root:

```sh
python3 tests/provenance/make_imports.py /path/to/pspsdk tests/provenance/intr/imports.json
python3 tests/provenance/run_probe.py /path/to/PPSSPPHeadless /tmp/intr-capture --suite intr
/path/to/build/tests/test_interrupt /tmp/intr-capture/observed.txt
```

`capture.json` records executable and input hashes. The reference is the
installed external executable's output, not a physical PSP. The same probe C
file is compiled into the native test with adapters for imports and registers;
every word in all 68 records is compared. The native test also verifies full
CPU/control restoration and scheduler ownership during an idle Vblank wakeup.

Each record contains `step result hits subindex common handler_gp reserved
handler_cpu_enabled caller_cpu_enabled caller_gp handler_controls[4]
caller_controls[4]`. Controls are S/T/D prefixes and RCX0. The reserved field
is zero: the SDK context query is a kernel-only import, so it cannot establish
context state in this user-module experiment. An earlier exploratory run's
link error is not treated as a context observation.

The observations establish nested suspend/resume tokens; initially disabled
registrations; callback subindex/common arguments; retention of incoming VFPU
controls and restoration after handler mutation; and dropping Vblanks during
the measured CPU-masked interval. They cover duplicate/absent registrations,
release/re-registration, null handlers, interrupt IDs -1/0/1/25/30/66/67 and
subindices -1/0/31/32/255/256. These samples do not prove every possible input.

Handler GP follows module metadata. Exploratory captures with module GP zero
and `0x31415926`, while registration/caller GP differ, isolate this dependency.
The checked-in fixture uses the latter. It is regenerated from the manifest's
`gp` field; the host now registers its loaded module extent and GP explicitly.

The bounded delivery budget, module-table capacity, prevention of recursive
native dispatch and execution at completed HLE boundaries are host policies.
Exact physical interrupt latency, arbitrary device interrupt sources and
multiple pending-handler ordering under long host stalls remain unverified.
