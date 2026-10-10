# Hardware probes

Small PSP homebrew programs that ask a real PSP what the firmware, the CPU
and the GE do, so psprecomp's runtime can rest on this project's own
measurements. Each is built from PSPSDK (BSD) alone; nothing here comes from
PPSSPP or pspautotests' sources.

| probe | measures | replaces |
|---|---|---|
| [mpegprobe](mpegprobe/) | sceMpeg sizes and structures, GE block transfers | values `src/hle/mpeg.c` had no source for (logged on firmware 6.60) |
| [vfpuprobe](vfpuprobe/) | VFPU and FPU results bit for bit | host libm in `src/vfpu.c`, pspautotests cpu captures |
| [geprobe](geprobe/) | reference frames for 58 GE scenes, GE callbacks | pspautotests gpu captures and rules psprecomp had guessed: dithering, skinning, patches, bbox, depth and gradient arithmetic |
| [saveprobe](saveprobe/) | savedata result codes, secure saves under known keys | pspautotests savedata captures; plaintext secure saves |
| [threadprobe](threadprobe/) | thread manager, callbacks, timers, TLS, RTC | pspautotests threads captures |
| [syncprobe](syncprobe/) | semaphores, event flags, mutexes, mailboxes, pipes, VPL/FPL | pspautotests kernel-object captures |
| [sasprobe](sasprobe/) | sceSasCore argument checks, envelopes, rendered voices | pspautotests audio captures |
| [modprobe](modprobe/) | the module manager with a game's own modules: ids, start and stop threads, free memory, linking, self-unload | uofw's modulemgr and PSPSDK's pspmodulemgr.h, which `src/hle/modulemgr.c` follows |
| [sysprobe](sysprobe/) | Mkdir with a missing parent, Chstat, `sce_lbn` extents (needs a UMD), GPO/GPI, the headphone remote, the SDK 3.70 call | answers `src/hle` gives unmeasured |

All but mpegprobe run without input: start one from the XMB, it writes
`<name>.txt` (and sometimes `.bin` or `.raw` files) beside its EBOOT and
returns to the XMB. The log is flushed before every step, so if the PSP
switches off, the last line names the step that did it. Starting the probe
again then skips that step and carries on; the log keeps every run, and the
last one is the whole result. Steps already seen to switch a 6.60 PSP off log
"not run" instead (build with `-DRUN_KNOWN_CRASHES` to try them anyway).

Every import must come from the library that exports it on the PSP. psprecomp
finds functions by NID alone, so an import from the wrong library works there
but is never linked on hardware, and the call returns garbage. threadprobe's
first build lost most of its run that way (sceKernelGetTlsAddr belongs to
Kernel_Library). uofw's `exports.exp` files (MIT) list which library exports
what on 6.60.

Logs from Sif's PSP (firmware 6.60) are kept beside each probe: `fw660.txt`
from version 1, `fw660-vN.txt` from version N. psprecomp's comments cite their
step numbers. compare.py reads the last run in a log (`--run` picks another).

## Build

With the pspdev toolchain on `PATH` (the prebuilt release from
github.com/pspdev/pspdev works), `make` in a probe's directory produces its
`EBOOT.PBP`. `common/` holds the logging they share.

## Compare with psprecomp

The same PRX runs under psprecomp, which writes the same log and files under
`./ms/PSP/GAME/<name>/`:

    allegrexrecomp interp <name>/<name>.prx --dispatch --budget 4000000000 --drain 200 --base 0x08804000
    tools/hwprobe/compare.py <folder copied from the PSP> ms/PSP/GAME/<name>

`compare.py` diffs the logs step by step and the binary files word by word or
pixel by pixel. A log line holds only what the firmware decided -- return
codes, out-parameters, orders -- never addresses, UIDs or timing, so a
difference is a difference in behaviour.
