# A game's own modules

The plan for games that load modules (PRX files) of their own at run time,
through `sceKernelLoadModule`. Written 9 Oct 2026 against psprecomp e73adc1.
Sif chose the approach the same day: the importer recompiles each module on
the disc, as it does the executable. M1 to M3 are built; the rest is not.

## Where things stand

The runtime knows one module, the executable. The boot host loads it at its
link address (0 for a relocatable executable) into the one module window
(`src/mem.c`), and every `ModuleMgrForUser` call answers as if nothing else
could exist. `sceKernelLoadModule` has no entry at all. `sceKernelStartModule`,
`StopModule` and `UnloadModule` answer 0, and `GetModuleId` and
`GetModuleIdByAddress` answer the executable's id.

WipEout Pulse (UCUS98712) is the first game seen to load modules. It loads
three from `disc0:/PSP_GAME/USRDIR/PRX/`, all of them Sony libraries shipped
on the disc:

| file | module | code | exports | imports |
|---|---|---|---|---|
| `libfont.prx` | `sceFont_Library` | 34,664 bytes | 28 | `sceReg` 6, `IoFileMgrForUser` 5 |
| `libmp3.prx` | `sceMp3_Library` | 4,832 | 17, of which the executable imports 12 | `sceAudiocodec` 7, `Kernel_Library` 2 |
| `pspnet_ap_dialog_dummy.prx` | `sceNetApDialogDummy_Library` | 2,032 | 4 | 9 across `sceNet*` and `sceUtility_netparam_internal` |

So recompiling a library moves the work down a level rather than removing
it. libmp3 is a thin layer over `sceAudiocodec`, the firmware's decoder
interface, which the runtime does not answer yet.

What it took to read them:
- Each file is a 64-byte `~SCE` wrapper around a `~PSP` module. The bundled
  `pspdecrypt` accepts the module once the wrapper is stripped (tag
  0x457B06F0).
- The decrypted module is gzip-compressed (attribute `comp=0x0001`). Inflated,
  it is an ordinary PRX that `allegrexrecomp` reads.
- None has section headers. Their relocations are in a program header: the
  older Elf32_Rel entries (`0x700000A0`) in libmp3, and the **packed format**
  (`0x700000A1`) in the other two. psprecomp read only relocation sections,
  so these modules gave no relocation pointers and could not be moved from
  their link address (M1 fixes that).

None of them has a `module_start` (entry 0xFFFFFFFF). Games' own code modules
generally do.

## The shape

At import, every module on the disc is recompiled at an address of its own
and linked into the one game executable, beside the main module's code. At
run time, `sceKernelLoadModule` finds which of those the file it was given
is, maps that module's image in at its address and connects it to the
others.

- **Where a module lives.** Each module gets a fixed base, chosen by the
  importer: after the executable, aligned, below the PSP's RAM, in an
  extended module window. This is the same departure the executable already
  makes (it lives at 0, not at 0x08804000). The recompiled code has its
  addresses baked in, so the base cannot depend on run-time allocation.
- **Which module a load means.** A module is known by the SHA-256 of the file
  on the disc. The importer keeps each module's inflated, decrypted image
  beside `module.elf`, with a manifest of hashes. The runtime cannot decrypt;
  the importer's `pspdecrypt` is a separate GPL program. A load whose file
  matches nothing fails, as the firmware does for a file it cannot load.
- **Linking.** A module's exports go into a table the runtime keeps. An
  import stub that the HLE does not answer is looked up there, including
  stubs of modules loaded before the exporter. This is how WipEout's
  `sceMp3` imports reach `libmp3.prx`. A module's own imports resolve
  against the HLE and the other modules the same way.

## Stages

**M1. Packed relocations.** Read `0x700000A1` in the recompiler (relocation
pointers become seeds) and the loader (applying them at any base). This is
worth having before anything else: newer SDKs link executables this way too,
and discovery currently misses their pointer seeds.

*Built (d7af565).* `reloc.c` reads all three forms: sections, Elf32_Rel in a
`0x700000A0` segment (libmp3 uses that one), and the packed `0x700000A1`.
- The seed harvest and the loader share it. Sections win when present, so
  the five main modules relocate and emit byte-identically.
- The packed layout follows prxtool and uofw, which differ only on kinds 6
  and 7. WipEout's modules settle it: every kind 6 is a `j` and every kind 7
  a `jal`.
- Moved to 0x00400000, all 529 relocated jumps in libfont and 50 in libmp3
  land inside the moved module.
- libfont now gives 304 pointer seeds. `test_reloc` covers both segment
  forms.

**M2. Several modules in one game.** `allegrexrecomp emit` takes a base and a
module index, and relocates the image to that base before discovery and
emission. Generated symbols are named by guest address (`psp_func_<addr>`),
so modules at distinct bases cannot collide. The one program-wide name,
`psp_recomp_register`, becomes a registration function per module, called
when the module loads. Tests use synthetic PRXs in both relocation formats.

*Emitter built.* `emit --base <addr> --module <n>`:
- WipEout's libfont at 0x00400000 gives 150 functions that compile, needing
  only runtime symbols.
- `test_emit` checks the name, and `test_reloc` checks the move.

What remains of M2 belongs with M3: the runtime calls a module's
registration function when the module loads. `psp_resume_register` keeps
one table, which a second module would replace.

**M3. The runtime's module table.**
- `sceKernelLoadModule` and `sceKernelLoadModuleByID` (an open file).
- `sceKernelStartModule`: `module_start` on a thread of its own, waited for,
  with its status returned.
- `sceKernelStopModule`, `sceKernelUnloadModule`, and the self-stop forms.
- `sceKernelGetModuleIdByAddress` and `sceKernelQueryModuleInfo`.
- The export table and the stub fallback.
- Each module's `$gp` for its interrupt handlers and callbacks.

*Built (bcd4cb1).*
- The runtime half is `src/hle/modulemgr.c`, which also took over the
  executable's ModuleMgrForUser answers unchanged.
- The boot host's half is `src/host/module_loader.c`. A title lists its
  modules in `psp_title.modules` {sha1, image, base, size, registration}.
- Semantics follow uofw's modulemgr and PSPSDK's pspmodulemgr.h:
  - LoadModule answers the id. StartModule answers the id (resident) or 0,
    and runs `module_start` on `SceModmgrStart` at priority 32 with 256 KB
    under the module's `$gp`.
  - Errors are `0x8002012E` for an unknown module and `0x8002012F` for a
    file the game was not prepared with.
- A started module's exports answer the imports the runtime does not; the
  runtime's own answer wins.
- `test_modules` drives a synthetic module through all of it.
- The game repos build `boot.c` themselves (`scripts/06-boot.sh`, TB's
  `dev/CMakeLists.txt`). At their next psprecomp pin they also need:
  - `reloc.c`, `crypto/sha1.c` and `src/host/module_loader.c`;
  - in Last Raven's `scripts/14-aspect-tests.sh`, `reloc.c`.

**M4. The importer.**
- Find every module on the disc: each file that is a `~SCE`, `~PSP` or ELF
  PRX, with duplicates by hash collapsed.
- Decrypt and inflate each one, and choose its base.
- Recompile it and link it into the game.
- Store the images and the manifest, and add their hashes to the game's
  fingerprint.
- Plain games first. A pack's profile names the modules its title expects.

**M5. Save states.** The module table and the extended window go into the
state, and a state refuses to load into a game whose modules differ.

**M6. Evidence and gates.**
- **A hardware probe** that loads small PRXs from the Memory Stick. It
  measures what the plan assumes, on the PSP:
  - the ids and error codes;
  - `module_start`'s thread and status;
  - `GetModuleIdByAddress`;
  - where the firmware puts a module;
  - how much free memory a load takes;
  - what an unresolved import does.
- **WipEout** loads its three modules, and its font calls reach libfont.
- **The 3rd Birthday and the Armored Core titles**, which load no modules,
  replay byte-identical to stage 11.

## Decided (9 Oct, with Sif)

All three proposals below, as written: a load allocates the module's size
from the user partition, the runtime's own answer wins over a module's export
(a pack may reverse that for its title), and a module the importer did not
see fails as an unknown one.

## Settled before M3

1. **Free memory.** A module's code lives outside RAM, so a load would take
   nothing from the user partition. Games print and check free memory (WipEout
   logs it at every step). The proposal is to allocate a block of the module's
   size from the partition at load, so the reports match the PSP's, once M6's
   probe says how much a load takes.
2. **HLE or the module's export.** A disc may ship a library the runtime
   already answers natively, for example Sony's ATRAC library, which the
   runtime's sceAtrac3plus answers. On the PSP the module's code runs. Running
   it here means its own firmware calls must be answered too. The proposal is
   that the runtime's answer wins where one exists, and the module's export
   fills the rest. A pack can reverse that for its title.
3. **Modules not on the disc.** Modules loaded from the Memory Stick
   (downloadable content) or from a memory buffer are not known at import.
   They fail as an unknown module would, until a game needs them.
