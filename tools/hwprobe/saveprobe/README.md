# saveprobe

A PSP homebrew program, built from PSPSDK (BSD) alone, that records what the
savedata utility writes and answers. psprecomp stores secure saves as
plaintext (`src/hle/utility.c`), so a save made on a PSP cannot be read by a
port today; this probe produces the test data an implementation of the save
encryption needs, and re-measures the result codes `src/hle/utility.c` takes
from pspautotests.

It makes these saves under the made-up game id `PRCP00000`, each with known
plaintext (byte *i* is `(i*7+3) & 0xFF`):

| save | mode | key | secureVersion | bytes |
|---|---|---|---|---|
| PLAIN | AUTOSAVE | none | 0 | 64 |
| A0, A0AGAIN | AUTOSAVE | A = 00..0F | 0 | 64 (twice, to see if the output repeats) |
| B0 | AUTOSAVE | B = FF..F0 | 0 | 64 |
| A0LEN1/15/16/17/1000 | AUTOSAVE | A | 0 | 1, 15, 16, 17, 1000 |
| DSEC | MAKEDATASECURE, WRITEDATASECURE | A | 0 | 100 |
| DPLAIN, DPLAINK | MAKEDATA, WRITEDATA | none, A | 0 | 100 |
| A1, A2, A3 | AUTOSAVE | A | 1, 2, 3 | 64 |

Each save is read back with the right key, a wrong key and no key. It also
runs READDATA and AUTOLOAD on a save that does not exist, reads a secure save
through the plain mode and the other way round, and runs SIZES, LIST, FILES
and GETSIZE.

## Version 2

Version 2 keeps version 1's 67 steps and titles, so the two runs' logs line
up in `compare.py`, and adds 49 steps after them: the ones the analysis of
the first run asks for (`hwresults/fw660-run1/findings/saveprobe.md`,
section 6). Its new saves:

| save | mode | key | secureVersion | bytes |
|---|---|---|---|---|
| PLAINV1 | AUTOSAVE | none | 1 | 64 |
| KEYPAIR | AUTOSAVE, then again | A, then B | 0 | 64 |
| BIG | AUTOSAVE | A | 0 | 65537 |
| TWO | MAKEDATASECURE DATA.BIN, WRITEDATASECURE DATA2.BIN | A | 0 | 100, 64 |
| ICON | AUTOSAVE with a 144x80 ICON0.PNG | A | 0 | 64 |
| PLAIN1480, PLAIN1500 | AUTOSAVE, parameter block of 1480 / 1500 bytes (no key field) | - | - | 64 |
| PLAINEND | AUTOSAVE, PLAIN's request late in the run | none | 0 | 64 |
| PLAIN660 | AUTOSAVE after `sceKernelSetCompiledSdkVersion660` | none | 0 | 64 |

It also loads A0 at secureVersion 1 and A1 at 0, loads a MAKEDATA save with
AUTOLOAD, reads DSEC with the wrong key, polls GetStatus back to back after
ShutdownStart (logging how long status 4 lasts, on a line marked as varying
by run), runs SIZES and GETSIZE with sizes around a 32 KB cluster and with
ICON0, LIST with `*`, `A0*`, `A?` and `A0`, and logs `sceIoOpen` on a
directory and every word of `sceIoGetstat` and the `sceIoDread` d_stat for a
save file and its directory (dates by shape only). Copies of PARAM.SFO made
by these steps also log its SAVEDATA_PARAMS flags and SAVEDATA_FILE_LIST.

The SDK-version step and the PLAIN660 save after it come last, since the
call changes the process for good.

## Build

Needs the pspdev toolchain (`psp-gcc`, `psp-config` and PSPSDK) on `PATH`:

    make

This produces `EBOOT.PBP` and `saveprobe.prx`. The CMake build does not
include it. `imports.S` declares `sceKernelSetCompiledSdkVersion660`
(SysMemUserForUser, NID 0x358CA1BB), which PSPSDK has no stub for.

## Run

Copy `EBOOT.PBP` to `ms0:/PSP/GAME/saveprobe/` and start it from the XMB. It
needs no input and returns to the XMB by itself.

Beside the EBOOT it writes `saveprobe.txt` and a copy of every file the
firmware wrote, named `<save>__<file>` (for example `A0__DATA.BIN`,
`A0__PARAM.SFO`), so the probe's folder holds the whole result. A save
copied a second time in the run is copied as `<save>_s<step>__<file>`: in
version 2, `DSEC__*` is the copy after MAKEDATASECURE (step 37) and
`DSEC_s38__*` the one after WRITEDATASECURE. The saves themselves stay on the
memory stick as "psprecomp saveprobe" and can be deleted from the XMB.

If the PSP switches off or hangs, start the probe again: it skips the step
it stopped in. The steps that could do that are written to be skipped.

## Compare with psprecomp

    allegrexrecomp interp saveprobe.prx --dispatch --budget 4000000000 --drain 200 --base 0x08804000
