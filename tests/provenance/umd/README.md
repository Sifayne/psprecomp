# UMD activation observations

The probe uses PSPSDK API declarations and import NIDs at commit
`654ac51fc73fbf7ad9350fac90e39e66e7a0b1c6` (`src/umd/pspumd.h`,
`src/umd/sceUmdUser.S`, `src/user/ThreadManForUser.S`). These probe sources
were written for this project and use its MIT license. No emulator source
or library is incorporated. See the parent probe README for evidence limits.

From the runtime root, with the external executable installed:

```sh
python3 tests/provenance/run_probe.py /path/to/PPSSPPHeadless /tmp/umd-capture --suite umd
```

`capture.json` records the executable, compiler and source hashes.
`observed.txt` contains the 31 output records. Each record has seven words:
`step return drive_state callback_calls notify_count event common_value`.
The callback ID returned by creation is normalized to zero on success so
the capture does not depend on the executable's allocation order.

The replacement activation path and `test_hle` callback test use these
observations: registration alone queues nothing; activation defers a `0x22`
event until callback processing; two activations accumulate a count of two;
ordinary waits leave callbacks pending; callback-aware waits consume them.
Unit 0 returns `0x80010016` without notifying; units 1 and 2 notify, including
with a null drive argument. Unregistration stops later notifications.

The probe intentionally records more than this replacement implements.
Deactivation and delayed transitions back to READY (steps 11 onward) are
not reproduced by the runtime's mounted-image policy. The unregister return
value in this executable is a callback ID; the host returns zero. The tests
check removal, not identity of that return value. Physical drive timing,
invalid callback IDs, absent media and unmeasured unit values remain outside
this evidence. These limitations must not be confused with passing a full
31-record runtime comparison; no such comparison is claimed.
