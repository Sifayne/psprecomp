# psprecomp

### *Because the Allegrex deserves a native second life*

> Static recompilation toolkit for PlayStation Portable titles.
> Turn PSP binaries into native executables. No emulator required.

This is [Sifayne/psprecomp](https://github.com/Sifayne/psprecomp), the fork of
[sp00nznet/psprecomp](https://github.com/sp00nznet/psprecomp) that serves as
the runtime for [last-raven-recomp](https://github.com/Sifayne/last-raven-recomp).
[FORK-NOTES.md](FORK-NOTES.md) covers what the fork adds and how the game repo
consumes it.

---

## What Is This?

**psprecomp** is an open-source toolkit providing the analysis tools, code
generator, and runtime libraries needed to **statically recompile PSP games
into native executables**.

Instead of interpreting or JIT-ing Allegrex MIPS at runtime (what PPSSPP does
brilliantly), we take the opposite approach: **translate everything ahead of
time** into C that compiles with any modern compiler on any platform.

This is the same philosophy behind:
- [N64Recomp](https://github.com/N64Recomp/N64Recomp) (N64 → native)
- [UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp) (Xbox 360 → native)
- [PS2Recomp](https://github.com/ran-j/PS2Recomp) (PS2 → native)
- [ps3recomp](https://github.com/sp00nznet/ps3recomp) (PS3 → native)

...and for the PSP, where
[sal063/PSP-recompilation-project](https://github.com/sal063/PSP-recompilation-project)
got there first — same idea, offline MIPS-to-C plus a native runtime, with an
SDL3/Vulkan backend. Worth reading if you are interested in this problem.

The two differ mainly in licensing posture: that project ports GPL code from
PPSSPP for parts of its HLE, while psprecomp keeps a hard MIT boundary and uses
emulators only as an oracle to diff against (see
[docs/ORACLE.md](docs/ORACLE.md)). If you want a permissively-licensed base to
build on, that is the distinction that matters.

## Why PSP?

The PSP has excellent emulation and, until recently, essentially no static
recompilation work. It is a genuinely *good* recompilation target hiding behind
a reputation for being a hard one:

- **One documented CPU.** The Allegrex is MIPS32r2. No second processor, no SPU
  swarm, no exotic addressing. MIPS is the best-understood RISC there is, and
  it is what the original static-recomp work was built on.
- **No TLB.** The memory map is fixed and small — scratchpad, VRAM, main RAM,
  each mirrored at three cache-behaviour bases. Address translation is a mask
  and an index, not a page walk.
- **A documented OS boundary.** Games call `sceKernel*` / `sceGu*` / `sceCtrl*`.
  That is a *library* surface, not a hardware surface — it can be implemented as
  HLE C rather than emulated, and a complete MIT-licensed reverse engineering of
  that firmware already exists.

Plus the usual static-recomp payoffs: native performance, no runtime translation
overhead, moddable C, and ports that survive long after emulators stop being
maintained.

## The Challenge

| Component | What It Is | Why It's Hard |
|-----------|-----------|---------------|
| **Allegrex** | MIPS32r2 main CPU | Sony extensions (`bitrev`, `wsbw`, `max`/`min`), branch delay slots, likely-branch nullification |
| **VFPU** | 128-register vector unit on COP2 | Prefix instructions that rewrite the *next* instruction's operands; matrix-shaped register aliasing |
| **GE** | Display-list GPU | Not called — *fed*. A producer/consumer command stream with its own control flow |
| **Container stack** | ISO9660 → PBP → `~PSP` → PRX | Four nested formats before you reach an ELF, the innermost KIRK-encrypted |
| **Firmware** | `sceKernel*`, `sceGu*`, `sceCtrl*` | Hundreds of NID-addressed entry points, several with non-standard calling conventions |

We don't shy away from hard problems. We just break them into smaller ones.

## Architecture

```
psprecomp/
├── tools/allegrexrecomp/     # Analysis & recompilation pipeline
│   ├── container.c           # ISO9660 / PBP / ~PSP / ~SCE / ELF-PRX / SFO peeling
│   ├── crypto/               # AES, AES-CMAC, SHA-1, KIRK CMD1
│   ├── keys.c                # External key loading (--keys, $PSPRECOMP_KEYS)
│   ├── decode.c              # Allegrex decoder: MIPS32r2 + COP1 + VFPU
│   ├── analyze.c             # Function discovery (exports, relocations, jump tables)
│   ├── emit.c                # Allegrex → readable C, one psp_func_<addr> each
│   ├── loader.c              # Module loading and relocation
│   ├── interp.c              # Interpreter oracle on the same runtime
│   └── main.c                # The allegrexrecomp CLI
│
├── src/                      # Runtime the generated C links against
│   ├── cpu.c                 # Register file, HI/LO, reset state
│   ├── mem.c                 # Memory map, cache mirrors, write generations, observers
│   ├── dispatch.c            # Address → function table, tracing, stack auditing
│   ├── vfpu.c                # Vector unit: arithmetic, matrix ops, prefixes
│   ├── ctors.c               # Static-constructor discovery and execution
│   ├── render.c              # Render backend interface; software and null backends
│   ├── os.c                  # Host threads, locks and clock (POSIX and Windows)
│   └── hle/                  # Firmware, by module
│       ├── hle.c             # NID table, call path, call log and histogram
│       ├── sched.c           # One host thread per guest thread, one run token
│       ├── threadman.c       # Threads, semaphores, event flags, callbacks
│       ├── kernlock.c        # Mutexes and LwMutexes
│       ├── kernobj.c         # VPL, FPL, mailboxes, message pipes, TLS pools
│       ├── ktimer.c          # Alarms and VTimers
│       ├── waitq.c           # The wait queue the kernel objects share
│       ├── clock.c           # Guest time
│       ├── sysmem.c          # Partitions and memory blocks
│       ├── ge.c              # Display-list execution, vertex pipeline, capture
│       ├── display.c         # Framebuffer, vblank, frame dumps
│       ├── sascore.c         # sceSasCore voice mixer
│       ├── atrac.c           # sceAtrac3plus (decodes with optional libavcodec)
│       ├── mpeg.c            # sceMpeg (decodes with optional openh264, opt-in)
│       ├── iofilemgr.c       # sceIo over a host directory or inside an ISO
│       ├── umd.c             # sceUmd: a disc that is always ready
│       ├── utility.c         # sceUtility savedata
│       ├── ctrl_replay.c     # Scripted pad input: player and recorder
│       ├── net.c             # sceNet / adhoc, refused as hardware does with no radio
│       └── misc.c            # sceCtrl, sceAudio, scePower, ModuleMgr, LoadExec, …
│
├── include/psprecomp/        # Public API
│   ├── cpu.h  mem.h          # Execution context and memory
│   ├── dispatch.h            # Dispatch table + the bring-up instruments
│   ├── hle.h                 # NID registration, argument/return convention
│   ├── sched.h  clock.h      # Scheduler and guest time
│   ├── render.h              # The backend interface a host implements
│   ├── savedata.h            # Savedata host bridge
│   ├── recomp_rt.h           # Semantic helpers the generated C calls
│   ├── vfpu.h  ctors.h  os.h # Vector unit, static constructors, host primitives
│
├── tests/                    # 18 self-contained suites (25 ctest cases), no game data
└── docs/                     # See Documentation below
```

## How It Works

```
  game.iso ──► allegrexrecomp ──► EBOOT.BIN / DATA.PSP     peel the container
              (ISO9660 · PBP)      (a ~PSP-wrapped PRX)     stack down to a module
                  │
                  ▼
          ┌────────────────────┐   ~PSP  ──►  a plain ELF32 MIPS PRX.
          │  decrypt           │   External for now (pspdecrypt): KIRK CMD1 is
          └────────────────────┘   done here, the ~PSP tag layer is not
                  │
                  ▼
          ┌────────────────────┐   decode Allegrex, discover functions
          │  allegrexrecomp    │   (jump tables + the module's export table),
          │  (tools/)          │   translate each to readable C
          └────────────────────┘
                  │  <prefix>_funcs.c
                  ▼
          ┌────────────────────┐   CPU state · memory map · semantic helpers ·
          │  psprecomp (lib)   │   HLE firmware · scheduler · dispatch ·
          │  (src/, include/)  │   GE + render backends
          └────────────────────┘
                  │
                  ▼   (+ a per-game host: load module, register, run, present)
          native executable
```

Every generated function is named for its address and carries the original
disassembly as comments, so the output is readable and diffable against the
binary it came from:

```c
/* ---------------------------------------------------------------
 * psp_func_0000E06C  --  16 instructions, 64 bytes
 * ------------------------------------------------------------- */
static void psp_body_0000E06C(uint32_t _entry) {
    /* 0000E06C  addiu      $sp, $sp, -16 */
    r_sp = r_sp + -16;
    /* 0000E070  sw         $ra, 8($sp) */
    psp_write32(r_sp + 8, r_ra);
    ...
```

## Status

**A retail PSP game recompiles, runs and renders on this runtime.**
Armored Core: Last Raven Portable, brought up in
[last-raven-recomp](https://github.com/Sifayne/last-raven-recomp), reaches its
garage, missions and combat and draws them. Being specific about which parts
are real:

| Area | State | Detail |
|------|-------|--------|
| **Container stack** | ✅ Complete | ISO9660, PBP, `~PSP`/`~SCE`, ELF/PRX, `PARAM.SFO`. `info` recurses the whole stack |
| **Decryption** | 🔨 Partial | AES, AES-CMAC and KIRK CMD1 are complete and pinned to published test vectors. The `~PSP` tag layer that builds a CMD1 header from a module is not written, so retail modules go through [pspdecrypt](https://github.com/John-K/pspdecrypt) first. See [DECRYPT.md](docs/DECRYPT.md) |
| **Allegrex decoder** | ✅ Complete | MIPS32r2 + Sony extensions + COP1 + VFPU, including the prefix instructions |
| **Function discovery** | ✅ Working | Exports, `jal` harvesting, relocation mining, a pointer-shape heuristic for static modules, and `switch` jump-table recovery. Upstream measured 5,849 functions and 92% of `.text` on WTF's Lumberjack module |
| **C emitter** | ✅ Working | Every discovered function, every label individually dispatchable, so computed jumps resolve by construction. `emit --replace` leaves named functions for the host to implement natively |
| **Interpreter oracle** | ✅ Working | `allegrexrecomp interp` runs a decrypted module on the same decoder, memory, helpers, HLE and scheduler as the recompiled C, so a disagreement points at the emitter or the sequencing |
| **Runtime core** | ✅ Working | Register file, memory map with cache mirrors, dispatch, static constructors, guest clock |
| **VFPU** | ✅ Working | The emitter translates every VFPU instruction the decoder names, prefixes included. A sub-encoding the runtime does not recognise traps by name rather than computing a guess |
| **Threading** | ✅ Working | Each guest thread runs on its own host thread, and a handoff token lets exactly one run at a time. As on a PSP (measured by `tools/hwprobe/threadprobe`), each priority has one FIFO ready queue and there is no timeslice. Timers and more urgent threads take over at the next firmware call, so a thread that spins without calling the kernel still hangs, and is reported |
| **HLE firmware** | 🔨 Partial | 340 functions across 25 libraries. The kernel objects (threads, semaphores, event flags, mutexes, LwMutexes, VPL/FPL, mailboxes, message pipes, TLS pools, alarms, VTimers, callbacks) are written against measurements from a PSP on firmware 6.60 (`tools/hwprobe/`) and, where those do not reach, pspautotests' hardware captures; the source cites which. `test_hle` checks every named NID against SHA-1 of its name. An unregistered NID is logged and returns 0 |
| **GE and rendering** | 🔨 Partial | Through-mode and transformed geometry with lighting, fog and texgen; textures with CLUTs and mips, depth, blending, alpha test and stencil. This repo has the software and null backends and the interface a host backend registers through ([RENDERER.md](docs/RENDERER.md)). Colour interpolation, dithering, blend and texture-function rounding, colour test, logic op and pixel mask follow a PSP pixel for pixel on the `tools/hwprobe/geprobe` scenes, and `sceGe` signal/finish callbacks are delivered. Not handled: skinned (weighted) vertices, Bezier/spline patches (counted, not drawn), bounding-box jumps |
| **Audio and video** | 🔨 Partial | `sceAudio` output to a host hook and the `sceSasCore` voice mixer. `sceAtrac3plus` decodes through optional libavcodec; without it every track ends at its first frame. `sceMpeg` does the bookkeeping, and its decode path (openh264 plus libavcodec) is opt-in with `PSPRECOMP_MPEG_DECODE=1` |
| **Files, input, saves** | ✅ Working | `sceIo` over a host directory or inside an ISO; `sceCtrl` with scripted replay and recording; `sceUtility` savedata under `ms0:/PSP/SAVEDATA` with an interactive session. Secure-mode saves are stored as plaintext |
| **Networking** | ➖ Refused | `sceNet` and adhoc answer the way hardware does with no radio, so a multiplayer menu fails instead of hanging |
| **A game on screen** | ✅ Yes | Six Last Raven captures (garage, mission, combat) match between the software backend here and the game host's GL 3.3 backend at normalized RMSE 0.0025 to 0.0031 ([RENDERER.md](docs/RENDERER.md#validation)). Upstream's [BRINGUP.md](docs/BRINGUP.md) ends before WTF rendered, and WTF has not been revisited in this fork |

### Bring-up instruments

Static recompilation fails quietly: a wrong value is usually *plausible*, not
obviously broken. The toolkit therefore ships the instruments that catch that,
because each one found a bug that reasoning had missed:

| Instrument | Catches |
|---|---|
| Function-entry tracing | Where execution actually went, vs where you assumed |
| Loop back-edge recording | Spins inside a single function, invisible to any call-boundary counter |
| Label-level reachability | Whether a specific block ran, for any labelled address |
| Memory write watch | *Which code* writes a given word; `PSPRECOMP_WATCHMEM_FROM` holds the report budget until a chosen pad poll |
| Stack-balance checking | A callee that consumes stack and never returns it — corrupts every callee-saved restore afterwards |
| Firmware call log and histogram | What a run did, in order (`PSPRECOMP_HLE_LOG`) or in total, and which call last handed the game a zero |
| Scripted pad input | A bug three menus deep, made reproducible: `PSPRECOMP_REPLAY` plays a scenario, `PSPRECOMP_REPLAY_REC` records one |
| GE capture | Frames of display-list work plus the guest memory they read (`PSPRECOMP_GE_CAPTURE`), for a host to replay against any backend |
| RAM snapshots | Whole-RAM dumps at chosen pad polls (`PSPRECOMP_RAMSNAP`), for diffing what an input changed |

Two real codegen bugs were found this way, both affecting *every* recompiled
program, and both producing plausible values rather than visible failures:

- **`$ra` was never assigned by `jal`/`jalr`** — 2 assignments existed across
  137,748 instructions where 9,814 belong. Every non-leaf function returned
  through a stale register.
- **`psp_arg` read arguments 5–8 from the stack.** PSP firmware passes them in
  `$t0`–`$t3`. The wrong read turned a valid 4096-byte alignment into garbage,
  so a correct 15.9 MB allocation was rejected — and every downstream symptom
  followed from that.

## Documentation

| Document | What It Covers |
|----------|---------------|
| **[Architecture](docs/ARCHITECTURE.md)** | Allegrex overview, pipeline stages, memory model |
| **[Containers](docs/CONTAINERS.md)** | ISO9660 → PBP → `~PSP` → PRX, and how to identify each |
| **[Decryption](docs/DECRYPT.md)** | KIRK, key tags, and supplying your own key material |
| **[Recompiler](docs/RECOMPILER.md)** | Discovery, emission, delay slots, jump tables |
| **[VFPU](docs/VFPU.md)** | Register aliasing, prefix instructions, matrix layout |
| **[Oracle](docs/ORACLE.md)** | How emulators are used as references without being linked |
| **[Renderer](docs/RENDERER.md)** | The backend interface, the software reference, and how a GPU backend is validated against it |
| **[Bring-up log](docs/BRINGUP.md)** | Upstream's WTF investigation record — every finding, **and every retraction** |
| **[Fork notes](FORK-NOTES.md)** | What this fork adds over upstream, and the submodule workflow |

`BRINGUP.md` is worth singling out. It records the wrong turns as prominently
as the results, because in this domain the wrong turns are the transferable
part: seven conclusions in one session were withdrawn after measurement
contradicted them, and every one came from an instrument adopted before it was
verified.

## Getting Started

Requires CMake 3.16+ and a C11 compiler. The core has **no external
dependencies** beyond the platform's threads — a fresh clone builds with
nothing installed. This fork is built and tested with gcc/clang; upstream
builds with MSVC, but the Windows half of `src/os.c` has not been through a
Windows build here yet (see [FORK-NOTES.md](FORK-NOTES.md)).

```bash
git clone https://github.com/Sifayne/psprecomp
cd psprecomp
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```

Two optional libraries are picked up if CMake finds them, and the build says
which it found:

| Library | License | Enables |
|---|---|---|
| openh264 | BSD | H.264 video for `sceMpeg`'s opt-in decode path |
| FFmpeg libavcodec + libavutil | LGPL-2.1 (GPL if your build was configured `--enable-gpl`) | ATRAC3 / ATRAC3+ audio for `sceAtrac3plus` and movie sound |

```
allegrexrecomp info    <file>                    identify any layer of the stack
allegrexrecomp ls      <file.iso>                list a disc image
allegrexrecomp extract <file.iso> <match> <dir>  pull files out of a disc image
allegrexrecomp dis     <file> [addr] [count]     disassemble Allegrex code
allegrexrecomp cover   <file>                    decode-coverage report
allegrexrecomp funcs   <file> [--list]           function discovery report
allegrexrecomp emit    <file> <outdir> [prefix] [--replace <addrs>|@<file>]
                                                 generate C
allegrexrecomp interp  <file> [--from <addr>] [--budget <n>] [--trace] [--regs]
                       [--dispatch] [--drain <s>]
                                                 run a module under the interpreter oracle
allegrexrecomp decrypt <file> [--keys <path>]    identify a ~PSP module and probe it
allegrexrecomp kirk1   <file> [out] [--keys <path>]
                                                 decrypt a raw KIRK CMD1 blob
```

`decrypt` reports a module's tag and decrypt mode but cannot yet produce the
ELF, so `emit` and `interp` take a module already decrypted with
[pspdecrypt](https://github.com/John-K/pspdecrypt).

`info` accepts anything in the stack — a disc image, a PBP, a `~PSP` or `~SCE`
module, a plain ELF/PRX — and recurses one level, so pointing it at a
game-sharing PBP reports the inner module's name, size and key tag in one go.

> **No game data here.** Disc images, `EBOOT.BIN`, PRX modules and PSP
> decryption keys are all `.gitignore`d. This repo is the recompiler, the
> runtime, and the docs — bring your own UMD or PSN dump.

## Game Ports Using psprecomp

The toolkit is separate from the games brought up on it; each game lives in its
own repo consuming this one as a submodule. That split is deliberate — the
toolkit is the thing you fork to recompile *your* PSP game.

- [**last-raven-recomp**](https://github.com/Sifayne/last-raven-recomp) —
  *Armored Core: Last Raven Portable*, and the reason this fork exists. Its
  `tools/psprecomp` submodule pins a revision of this repo, and its host
  carries the SDL2 window, audio and the GL 3.3 render backend.
- [**wtf-psp-recomp**](https://github.com/sp00nznet/wtf-psp-recomp) —
  *WTF: Work Time Fun*, upstream's reference game. An anthology of ~30
  microgames, five of which ship as separately-bootable modules, which makes it
  an unusually good bring-up target: each module is a small, self-contained
  program.

## Relationship to Other Projects

There is no MIT-licensed PSP emulator. PPSSPP is GPLv2+, JPCSP is GPL-3.0.
Neither can be linked into an MIT toolkit, and neither needs to be:

| Project | License | How it is used |
|---|---|---|
| [uofw](https://github.com/uofw/uofw) | **MIT** | A complete reverse engineering of the PSP firmware in readable C. The reference for the HLE layer. |
| [pspsdk](https://github.com/pspdev/pspsdk) | **BSD** | Headers and the published ABI for `sceGu` / `sceKernel` / `sceAudio`. (`tools/PrxEncrypter` is GPL-3.0 and excluded.) |
| **PPSSPP** | GPLv2+ | **Oracle only** — run as a separate process and compared against. No code copied, linked, or vendored. |
| [pspautotests](https://github.com/hrydgard/pspautotests) | — | A hardware-validated behavioural corpus to check the runtime against. |

Same arrangement `ps3recomp` has with RPCS3 and `lynxrecomp` has with Handy: the
emulator is a thing you *diff against*, never a thing you link. See
[docs/ORACLE.md](docs/ORACLE.md).

## Legal

This project contains no Sony code, no firmware images, and no decryption keys.
It is a clean-room toolkit built from published documentation and MIT/BSD
reverse-engineering work. Recompiling a game requires a dump you made from
media you own.

## License

MIT. See [LICENSE](LICENSE).
