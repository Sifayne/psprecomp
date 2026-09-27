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

## Build

Needs the pspdev toolchain (`psp-gcc`, `psp-config` and PSPSDK) on `PATH`:

    make

This produces `EBOOT.PBP`. The CMake build does not include it.

## Run

Copy `EBOOT.PBP` to `ms0:/PSP/GAME/saveprobe/` and start it from the XMB. It
needs no input and returns to the XMB by itself.

Beside the EBOOT it writes `saveprobe.txt` and a copy of every file the
firmware wrote, named `<save>__<file>` (for example `A0__DATA.BIN`,
`A0__PARAM.SFO`), so the probe's folder holds the whole result. The saves
themselves stay on the memory stick as "psprecomp saveprobe" and can be
deleted from the XMB.

## Compare with psprecomp

    allegrexrecomp interp saveprobe.prx --dispatch --budget 4000000000 --drain 200
