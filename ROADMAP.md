# Roadmap

Phased plan, last checked against the code on 2026-10-04. Phases 1, 2a and 4
are done, phase 3 lacks only a hints file, and phase 5 lacks only the PPSSPP
cross-check. Phase 2b is still open but off the critical path, because
`pspdecrypt` supplies plaintext modules today. What is left is breadth: see
*Open work* at the end.

## Phase 1 — the container stack and the decoder ✅

Getting from a disc dump to *something you can point a decoder at*, and a
decoder correct enough to trust when you do.

- [x] Two-repo split: `psprecomp` (toolkit) + `wtf-psp-recomp` (reference game,
      submodules the toolkit).
- [x] **ISO9660 walker** — full recursive directory tree, version-suffix
      stripping, sector-padding handling, extraction by substring match with
      path flattening (so `SYSDIR/EBOOT.BIN` and `SYSDIR/UPDATE/EBOOT.BIN` do
      not collide).
- [x] **PBP parser** — the 8-segment container, sizes derived from adjacent
      offsets, one-level recursion into `DATA.PSP`.
- [x] **`~PSP` and `~SCE` header parsers** — module name, segment table, entry
      point, decrypted-ELF size, decrypt mode, and the key tag.
- [x] **ELF32/PRX parser** — program headers, `PT_LOAD` extraction, `.text`
      located by `PF_X`, `e_type == 0xFFA0` recognised as a relocatable PRX.
- [x] **PARAM.SFO parser** — the disc/module identity (`DISC_ID`, `TITLE`,
      `PSP_SYSTEM_VER`).
- [x] **Complete Allegrex decoder** — the MIPS32r2 integer set, REGIMM,
      SPECIAL2/SPECIAL3, the single-precision COP1 FPU, and every Allegrex
      divergence from stock MIPS32 (`clz`/`clo` in SPECIAL 0x16/0x17,
      `max`/`min` at 0x2C/0x2D, `madd`/`msub` in SPECIAL, `bitrev`/`wsbw` in
      BSHFL, `rotr` as `srl` with `rs=1`, `mfic`/`mtic` in COP0).
- [x] **Analysis flags on every instruction** — branch vs jump vs call,
      indirect, `jr $ra` as a return specifically, likely-branch nullification,
      delay slots, block termination. This is what phase 3's discovery pass
      consumes.
- [x] **VFPU recognised as a distinct category** rather than silently invalid,
      with the load/store and vadd/vmul/vcmp families named.
- [x] **Runtime semantic helpers** (`recomp_rt.h`) — division edge cases, the
      unaligned load/store pairs in little-endian form, `ext`/`ins`, `clz`,
      `bitrev`, `wsbh`/`wsbw`, shift-amount masking.
- [x] **Memory map** — scratchpad/VRAM/RAM with cache mirrors folded, straddling
      accesses rejected, unmapped accesses counted.
- [x] **`info` / `ls` / `extract` / `dis` / `cover` CLI.**
- [x] **`ctest` suites** — decoder, runtime, containers. All synthetic.
- [x] **Validated against a retail disc** — *WTF: Work Time Fun* (ULUS10172):
      90 entries, correct SFO, the full boot chain, and all five game-sharing
      modules with their per-module key tags.
- [x] Build system (CMake, zero external dependencies), docs, MIT license.

## Phase 2a — the crypto core and KIRK CMD1 ✅

Every executable module on a retail disc is a `~PSP`-wrapped, KIRK-encrypted
PRX, and `BOOT.BIN` is zeroed. See [`docs/DECRYPT.md`](docs/DECRYPT.md).

- [x] **AES-128/192/256** (`crypto/aes.c`) — self-contained, no dependency. The
      S-box is derived from its algebraic definition rather than transcribed,
      so it is either completely right or obviously wrong.
- [x] **AES-CMAC** (`crypto/cmac.c`), RFC 4493.
- [x] **Validated against the primary standards** before touching a game
      module: FIPS-197 Appendix C (all three key sizes, plus the inverse
      cipher), NIST SP 800-38A F.2 (CBC, including in-place operation and a
      zero IV), RFC 4493 (all four CMAC examples, covering both padding
      branches). `ctest` target `crypto`.
- [x] **KIRK CMD1** (`crypto/kirk.c`) — key unwrapping, both CMAC checks, body
      decryption. Handles nonzero header/body padding and unaligned data
      lengths, and bounds-checks every size field so a corrupt module cannot
      read out of bounds.
- [x] **Verified end to end with synthetic blobs** (`ctest` target `kirk`) —
      round trip, padding, unaligned lengths, wrong key caught at the *header*
      CMAC, tampered body caught at the *data* CMAC, and every malformed-input
      path. No PSP key material is needed for any of it, because CMD1's
      structure does not depend on which key it is given.
- [x] **External key loading** (`keys.c`) — `--keys`, `$PSPRECOMP_KEYS`, or
      `./keys/psp_keys.txt`. No key material is bundled; a missing or
      wrong-length key produces a named diagnostic.
- [x] `allegrexrecomp kirk1` (decrypt a raw CMD1 blob) and
      `allegrexrecomp decrypt` (identify a module and probe it).

## Phase 2b — the `~PSP` tag layer

Above CMD1 sits a per-tag transform that *builds* the CMD1 header.

Established empirically rather than assumed: `decrypt` scans for an embedded
CMD1 metadata block using a ~100-bit structural signature, and finds none in
any module on the WTF disc. So the CMD1 header is constructed by the tag layer,
not sitting in the `~PSP` header at a fixed offset. Guessing that offset would
have produced silently-wrong output.

Also established: WTF's main executable is **decrypt mode 9** while all five
game-sharing modules are **mode 10**, so at least two variants are in play.

- [ ] The tag → key-set table, from the external key file.
- [ ] The mode 9 and mode 10 transforms that build a CMD1 header from a `~PSP`
      header. Needs the reference algorithm — approximating it would produce
      plausible garbage that the CMACs would reject without saying which step
      was wrong.
- [ ] **`~PSP` → ELF** — reassemble decrypted segments into a valid ELF32 using
      the header's segment table.
- [ ] **Cross-check**: output must be exactly `elf_size` bytes, parse with the
      phase-1 ELF parser, have its entry point land in a `PF_X` segment, and
      show a code-shaped opcode histogram under `cover` rather than a flat one.
- [ ] `info`/`dis`/`cover` decrypting transparently, so the rest of the tool
      stops caring about the wrapper.

**Not on the critical path.** Phase 3 needs *plaintext*, not necessarily *our*
decryptor — [`pspdecrypt`](https://github.com/John-K/pspdecrypt) (GPL-3.0) run
as an external process produces it today, exactly like PPSSPP-as-oracle. 2b is
about being self-contained, which is a goal but not a gate.

## Phase 3 — function discovery + the C emitter

- [x] **ELF section parsing** — `.text` gives the true code extent. A PSP PRX
      has one rwx `PT_LOAD` covering code *and* data, so decoding the segment
      measures 3.3 MB of data as instructions. Using `.text` moved decode
      coverage on Lumberjack from 70.8% to **99.81%, with 0% unknown**.
- [x] **Module info + export table** — layout corrected against a real module
      (`attribute` is a u16 and `name` starts at 0x04; the widely-circulated
      struct shifts every field by two). Verified by cross-checking the parsed
      export/import ranges against the `.lib.ent` / `.lib.stub` section
      addresses.
- [x] **Recursive-descent discovery** — seeds from the entry point and exports,
      follows calls and branches, carves at `jr $ra`, classifies calls into
      `.sceStub.text` as HLE imports rather than functions.
- [x] **`jal` harvesting by linear scan** — required, not optional: PSP
      `module_start` passes the real entry point to a thread as a *pointer*, so
      recursive descent alone finds 47 instructions. 1 function -> 2206.
- [x] **Tail-call disambiguation** — a full entry map built before walking, so
      a forward `j` is recognised as a tail call and a walk stops when it runs
      into the next function's entry. Without it one trace swallowed 121 KB.
- [x] **Honest metrics** — code size (instructions actually visited) reported
      separately from extent (entry to furthest address), with divergence
      counted. This is what made the tail-call bug visible.
- [x] **Validated across six real modules** — 2206-3410 functions each, ~75% of
      `.text` reached, **zero invalid instructions**, VFPU measured at 0.14-0.20%.
- [x] **Relocation mining** — R_MIPS_32 relocations name every word holding an
      address, which is exactly the set of stored function pointers (thread
      entries, callbacks, vtables) that no control-flow scan can see. This is
      enumeration, not guesswork: the linker recorded them because they *are*
      addresses. **75% -> 89% of `.text`** on the microgames, 2633 pointer
      seeds on Lumberjack alone.
      A PSP PRX tags these sections `SHT_PRXRELOC` (`0x700000A0`) rather than
      `SHT_REL` (9); checking only for the generic type finds nothing at all.
- [x] **Data-pointer scanning for static modules** — a statically linked
      module (`ET_EXEC`, which is what `EBOOT.BIN` is) has *empty* relocation
      sections, because absolute addresses need no patching. The fallback is to
      recognise pointers by shape: in the code extent, instruction-aligned, and
      pointing at something that decodes. Deliberately kept separate and named
      as a **heuristic** rather than enumeration. **70% -> 88%** on `hell2k`.
- [x] **Jump-table resolution** — the `lui`/`addiu`/`sll`/`addu`/`lw`/`jr`
      idiom that MIPS compilers emit for `switch` (`resolve_jump_table` in
      `analyze.c`). A table is rejected wholesale if any entry fails to land on
      valid code in `.text`. A table it cannot resolve still works at run time,
      because every label is dispatchable.
- [x] **The delay-slot problem.** Ordinary branches capture their condition
      into a temporary before the slot runs (so a slot that writes a compared
      register cannot flip the branch); likely branches duplicate the slot into
      the taken path; jumps, calls and returns hoist it. Asserted by
      `tests/test_emit.c`.
- [x] **The C emitter** — one `psp_func_<addr>` per routine, every line carrying
      its address and disassembly, lowered to `recomp_rt.h` helpers, flow as
      labels + `goto`, `jal` as a direct call, imports as named stubs, indirect
      jumps through the dispatch table. Untranslated instructions emit named
      run-time traps rather than silence.
- [x] **Dispatch table** (`src/dispatch.c`) — open-addressed `addr -> psp_fn_t`,
      with a miss handler that names the address instead of crashing.
- [x] **Proven end to end on a real game.** Lumberjack emits 2206 functions /
      256,566 lines, which compile with MSVC, link against the runtime, and run:
      2212 functions registered and `module_start` resolves through the table.
      Two edge cases the real module forced, each producing exactly one compile
      error in a quarter-million lines: a delay slot that is also a branch
      target (must be emitted twice), and a function owning instructions
      *below* its entry point (so functions carry a `start` as well as an `end`).
- [ ] **Hints file** — per-title force-code/force-data, function names, and
      HLE overrides, so a title's hard-won analysis is data rather than a patch.
      The override part exists as `emit --replace`: listed functions are left
      for the host to implement, with the translated body still callable as
      `psp_func_<addr>__orig`. Force-code/force-data and names do not.

## Phase 4 — the HLE library ✅

Games do not touch hardware directly; they call the firmware. That surface is a
library, which means it is implemented, not emulated. 407 functions across 29
libraries are registered; `test_hle` checks every named NID against SHA-1 of
its name.

- [x] **`sceKernel`** — threads, semaphores, event flags, mutexes and
      LwMutexes, callbacks, the memory partitions, VPL/FPL, mailboxes, message
      pipes, TLS pools, alarms and VTimers (`threadman.c`, `kernlock.c`,
      `kernobj.c`, `ktimer.c`, `sysmem.c`). Error codes and pool layouts are
      transcribed from pspautotests' hardware captures.
- [x] **`sceIo`** — the file API over a host directory standing in for the UMD,
      or over files inside an ISO; sync and async calls, directories.
- [x] **`sceCtrl`** — the pad, merged from host input, `PSPRECOMP_PAD` and
      scripted scenarios, with a recorder (`ctrl_replay.c`).
- [x] **`sceDisplay` + `sceGe`** — the display list processor. The GE is a
      fixed-function GPU with a documented command stream; it is emulated as a
      peripheral, the same split as the Lynx's Suzy/Mikey. See phase 5 for
      rendering.
- [x] **`sceAudio` / `sceSas`** — PCM out to a host hook, and the hardware
      voice mixer with its ADSR curves and argument checks.
- [x] **`sceAtrac3plus` and `sceMpeg`** — the streaming contracts, with
      optional libavcodec and openh264 behind them for the actual decoding;
      `sceAtracReinit`'s six-slot ID layout as uofw has it.
- [x] **`sceUtility` savedata** — `ms0:/PSP/SAVEDATA` on a host directory, an
      interactive session a host can present, and a savedata host bridge
      (`include/psprecomp/savedata.h`).
- [x] **`sceUmd`, `scePower`, `sceRtc`, `sceDmac`, ModuleMgr, LoadExec,
      `sceImpose`, `sceOpenPSID`** and `Kernel_Library`'s memset and memcpy —
      the small libraries a game's startup needs.
- [x] **`sceNet` / adhoc** — refused the way hardware refuses with no radio.
- [x] **Module import resolution** — the emitter turns each import stub into
      a traced call through the NID table, and the interpreter binds
      `.sceStub.text` thunks to the same table. An unregistered NID is logged
      by number and returns 0 rather than crashing.

## Phase 5 — running the recompiled C

- [x] **Dispatch table** — `addr -> psp_fn_t`, for indirect calls and jump
      tables (`src/dispatch.c`). Static `jal` stays a direct C call.
- [x] **The interpreter oracle** — an Allegrex interpreter sharing this repo's
      decoder and runtime, so the recompiled path can be diffed against it
      instruction-for-instruction (`tools/allegrexrecomp/interp.c`,
      `allegrexrecomp interp`). When they disagree, the bug is in exactly one of
      them. This is the bring-up tool that everything else leans on.
- [x] **Threads** — each guest thread on its own host thread, with a handoff
      token so exactly one runs at a time (`src/hle/sched.c`), and a guest
      clock that advances at every firmware call (`src/hle/clock.c`).
- [ ] **PPSSPP cross-check** — trace comparison against the external oracle at
      the syscall and frame level. See [`docs/ORACLE.md`](docs/ORACLE.md).
      Nothing here compares a game run against PPSSPP yet. What does compare:
      `tools/oracle/oracle_diff.c` runs each function of a game both
      recompiled and interpreted and diffs the results, and
      `tools/hwprobe/compare.py` diffs a probe's PSP log against the same probe
      under `allegrexrecomp interp`.
- [x] **First pixels** — a recompiled module reaching a rendered frame.
      Armored Core: Last Raven Portable renders its garage, missions and
      combat.
- [x] **A render backend interface** — twelve required calls in
      `include/psprecomp/render.h` plus optional hooks for a backend that runs
      its own transform, the software backend as the reference, a null backend
      for counting, and `psp_render_register` for a backend the host supplies.
      See [`docs/RENDERER.md`](docs/RENDERER.md).
- [x] **GE capture** — frames of display-list work plus the memory they read,
      selected by pad poll, for replay against any backend.

## Phase 6 — corpus and player-facing layer

- [ ] `scripts/sweep` over a large PSP corpus as a correctness harness:
      decrypt-all, decode-all, report coverage. Every module that fails is a
      concrete decoder or container bug, and the aggregate VFPU percentage
      tells us which titles are cheap targets. There is no `scripts/`
      directory yet.
- [x] VFPU completion. The emitter translates every VFPU instruction the
      decoder names; a sub-encoding the runtime does not recognise traps by
      name.
- [ ] Save states, an SDL2 + Dear ImGui frontend, controller remapping.
      The host layer has started: `PSPRECOMP_HOST` builds `src/host/` as
      `psprecomp_host`, with the savedata dialog and the presentation layer
      (SDL2 window, input, audio); player settings are `psprecomp_settings`.
      Last Raven's titles use them; The 3rd Birthday's copies and both
      games' GL backends remain (see *Open work*). `player/` builds the
      AppImage for a title pack (`player/README.md`); Last Raven is the
      first. RAM snapshots (`PSPRECOMP_RAMSNAP`) are an instrument, not save
      states. The plan is [`docs/PLAYER-LAYER.md`](docs/PLAYER-LAYER.md): one
      player in this repository, every game a title pack compiled on the
      player's machine, and the three features staged from a shared safe
      point and host pause.

## Open work

Gaps found in the code while checking this file, none of them on a phase above:

- [ ] **The GE's queue calls** — DeQueue and GetCmd/GetMtx/GetStack are
      unregistered, and a full list pool answers NO_MEMORY rather than the
      hardware's 0x80000022 (`src/hle/ge.c`). What the GE draws is done:
      skinning, morphing, patches, bounding boxes and the rest match the PSP
      bit for bit through geprobe 23. The GL backend's own gaps are listed in
      [`docs/RENDERER.md`](docs/RENDERER.md).
- [ ] **Preemption** — a thread yields only at firmware calls, so one that
      spins without calling the kernel hangs (reported, not silent). The 3rd
      Birthday's host adds a 1 ms yield after GE calls to keep its sound
      threads fed.
- [ ] **Static-constructor discovery** — `src/ctors.c` picks the single
      longest null-terminated table; its own notes say candidates should be
      scored on locality before it is trusted on another title.
- [ ] **MPEG decode** — the opt-in path plays The 3rd Birthday's movies, in
      up to four contexts. `sceMpegDelete` only frees the slot, and
      `sceMpegCreate` clears a reused one without freeing its decoder or
      picture, so each movie leaks them.
- [ ] **Secure savedata** — secure modes store plaintext. saveprobe already
      holds the hardware's secure saves under known keys.
- [ ] **Windows** — the Windows half of `src/os.c` has not been through a
      Windows build in this fork.
- [ ] **The rest of the host** — `present.c` and the settings mechanism are
      here (`src/host/`), and Last Raven's titles use them. The 3rd Birthday
      still carries its own copies, which differ in audio handoff, input
      carriers and aspect; both games carry `render_gl.c` (the GL backend) and
      `boot.c`, which have drifted apart. The order is stages 1–3 of
      [`docs/PLAYER-LAYER.md`](docs/PLAYER-LAYER.md). `boot.c` is where both
      register `sceKernelStopUnloadSelfModuleWithStatus` (0x8F2DF740, whose
      name does not hash to its NID; uofw `start-stopModule.c`), because its
      exit path needs the host.
- [ ] **Hardware probe set 25** — what the games rely on and no PSP has
      answered: vector `vrnd` lane order and D prefix (`src/vfpu.c`), a GE
      signal handler rewriting the list words behind its SIGNAL now that lists
      are drawn as they are released (`src/hle/ge.c`), and `sceIoMkdir` with a
      missing parent and `sceIoChstat` (`src/hle/iofilemgr.c`). Then the vrnd,
      savedata and mpeg provenance checkers, built but unregistered, can be
      checked against hardware logs instead of an emulator's.
