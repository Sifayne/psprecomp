# modprobe

A PSP homebrew program, built from PSPSDK (BSD) alone, that records what the
module manager does with modules a game loads itself. psprecomp's module
manager (`src/hle/modulemgr.c`, `docs/MODULES.md`) follows uofw's modulemgr
and PSPSDK's `pspmodulemgr.h`; this probe is the measurement its stage M6
asks for.

It loads six small modules, built here from `mods/` and copied beside the
EBOOT. Each reports what it saw through the argument block StartModule and
StopModule pass it (`mods/report.h`), never through a library, so the report
does not depend on the linking it measures:

| module | what it is for |
|---|---|
| mod_a | resident; exports `ProbeLibA` (add, which module am I); its module_start records its thread and `$gp` |
| mod_b | module_start answers 1 (not resident); names `module_start_thread_parameter` {3, 0x31, 0x4000, 0} |
| mod_c | loaded after mod_a starts; calls `ProbeLibA` from module_start |
| mod_d | imports `ProbeLibNone`, which nothing exports, and calls it from module_start |
| mod_e | a thread of its own calls `sceKernelSelfStopUnloadModule` |
| mod_f | loaded and started before mod_a, importing `ProbeLibA`; calls it from module_stop after mod_a starts |

The log holds, step by step:
- the error codes for a missing file, a file that is not a module, and an
  id nothing has;
- what GetModuleId and GetModuleIdByAddress answer for the executable's
  code, stack and heap and for 0, and what QueryModuleInfo says about the
  executable;
- for mod_a: the free memory a load takes, where the module goes against the
  lowest free address, every QueryModuleInfo field, and StartModule's answer
  and status;
- module_start's thread: its name, priority, stack, attributes and `$gp`;
- the errors for starting twice, unloading while started and stopping
  before starting;
- loading the same file twice, LoadModuleByID, and stopping and unloading,
  with the memory given back;
- whether a library is linked to stubs loaded before it (mod_f);
- what a module that is not resident leaves behind (mod_b);
- a self-unload (mod_e);
- last, since it may not come back, a call to an import nothing exports
  (mod_d).

Running it needs a PSP that runs unsigned homebrew, as every probe here
does; the modules are unsigned PRXs, loaded from the memory stick.

## Build

Needs the pspdev toolchain (`psp-gcc`, `psp-config` and PSPSDK) on `PATH`:

    make

builds `EBOOT.PBP` and `mods/*/mod_*.prx`. All six PRXs go in the probe's
folder on the memory stick, beside the EBOOT.

Under psprecomp the interpreter loads the PRXs as the console does, into a
block of their size from the lowest free address, and runs them interpreted:

    mkdir -p ms/PSP/GAME/modprobe && cp mods/*/mod_*.prx ms/PSP/GAME/modprobe/
    allegrexrecomp interp modprobe.prx --dispatch --base 0x08804000

Its `modprobe.txt` is byte-identical to the PSP's (set 25, fw 6.60).
