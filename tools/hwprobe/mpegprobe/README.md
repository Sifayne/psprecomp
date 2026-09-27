# mpegprobe

A PSP homebrew program, built from PSPSDK (BSD) alone, that measures what the
firmware's sceMpeg library and the GE actually do. It covers the values that
`src/hle/mpeg.c` and `src/hle/ge.c` could not source anywhere else.

`fw660.txt` is its log from a PSP running firmware 6.60
(`sceKernelDevkitVersion` 0x06060010), taken on 2026-09-27 with Last Raven's
`mail_001.pmf` as the movie. Its first three lines come from an earlier build
that stopped right after start-up; everything after them is this version.

## Build

Needs the pspdev toolchain (`psp-gcc`, `psp-config` and PSPSDK) on `PATH`:

    make

This produces `EBOOT.PBP`. The CMake build does not include it.

## Run

Copy `EBOOT.PBP` to `ms0:/PSP/GAME/mpegprobe/`. For the movie test, put a PMF
file beside it named `movie.pmf`. Start it from the XMB and pick one test at a
time from its menu:

- **X**: sceMpeg sizes, and the ring buffer and access-unit structures after
  Construct, Create and InitAu.
- **SQUARE**: the same setup on a real movie. It shows which PSMF header words
  the stream queries read and what they reject, the ring after each Put, and
  each access unit's timestamps.
- **TRIANGLE**: GE block transfers with unaligned addresses and wide strides.

The log, `mpegprobe.txt` beside the EBOOT, is only appended to, and it is
saved before every call that could crash. If the PSP switches off, the log's
last line names the call that did it. The log holds numbers only, no movie
data.
