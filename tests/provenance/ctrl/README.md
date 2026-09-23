# Controller sampling observation probe

This project-authored MIT probe asks how many `SceCtrlData` entries
`sceCtrlReadBufferPositive` and `sceCtrlPeekBufferPositive` hand back, and with
which timestamps, when the caller gives room for more than one sample and a
varying number of vblanks pass between calls. It contains no game data and
links no emulator library. No emulator implementation source was read.

The NIDs in `imports.json` are given explicitly (`opaque_imports`). They are
the values already in this runtime's HLE registration table and in earlier
suites' stubs generated from the BSD PSPSDK, so `make_imports.py` needs no SDK
tree for this suite (any empty directory will do).

From the runtime root:

```sh
python3 tests/provenance/make_imports.py /empty/dir tests/provenance/ctrl/imports.json
python3 tests/provenance/run_probe.py /usr/bin/PPSSPPHeadless /tmp/ctrl-capture --suite ctrl
/path/to/build/tests/test_ctrl_observed /tmp/ctrl-capture/observed.txt
```

`capture.json` records executable and input hashes. The reference is the
installed external executable's output (PPSSPPHeadless 1.20.4), not a physical
PSP. The hardware-recorded pspautotests `ctrl/sampling` and `ctrl/vblank`
expectations agree where they overlap: Peek with room 64 returns 64, Read with
room 96 returns `80000104`, and a Read every vblank returns 1.

Each record is `id result waited during highest written t0 t1 t2 t3 tlast
buttons0 sticks0`:
- `waited` is the number of sceDisplayWaitVblankStart calls before the call.
- `during` is the vcount change across the call, so 1 means the call blocked.
- `highest` and `written` describe which entries lost the buffer's sentinel.
- `t*` are timestamps relative to the newest one any earlier call returned.

Observed:

- Read returns the samples taken since the previous Read, one per vblank,
  oldest first, at most 63, and at most the room given; with fewer slots it
  keeps the newest. With nothing unread it waits one vblank and returns 1.
  Room 0 behaves as room 1.
- Read every other vblank with room for ten (The 3rd Birthday's pattern)
  returns 2 every time; every vblank returns 1.
- Peek returns `room` samples of history, including ones already read, and
  consumes nothing. Room 0 returns 0 and writes nothing.
- Room above 64 returns `80000104` from both, and nothing is written.

`tests/test_ctrl_observed.c` compiles this probe against the runtime with
native adapters (a vblank wait advances the guest clock to the next frame).
It compares every field except the timestamps: 36 records, 409 checks. A
runtime that always returns one sample fails 59 of them.

Two records (ids `16` and `3a`) depend on sampling phase. The observed
executable takes each sample a little after vblank start. A Read issued
immediately after `sceDisplayWaitVblankStart` therefore sees samples only up
to the previous vblank, and the next Read picks up the one it arrived at.
This runtime samples on the boundary. Runs of reads agree (K vblanks between
reads return K), but the first read after a change of phase returns one
fewer in the observed output. Those two records are compared for id and
blocking only.

Limits: this provider keeps one merged input state per call, not a sample
history. Every entry therefore carries the current state, and timestamps
count samples delivered rather than microseconds. Peek's history timestamps
are not reproduced. Live play can deliver more than two samples after a
late frame, as the hardware would; scenario replays run in virtual time,
where the count is deterministic.
