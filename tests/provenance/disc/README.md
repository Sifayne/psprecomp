# Disc observation probe: the ISO 9660 filesystem and the raw UMD device

This probe was authored for this project. `make_iso.py` builds a 51-sector
bootable ISO 9660 image from ECMA-119 whose every data word names its own
sector, with the probe itself as its `PSP_GAME/SYSDIR/EBOOT.BIN`; the
external `PPSSPPHeadless` executable boots that image the way it boots a
game, and the probe opens, stats, reads, seeks, lists, queues asynchronous
operations and activates the drive, printing what came back. The function
names and NIDs come from BSD PSPSDK commit
`654ac51fc73fbf7ad9350fac90e39e66e7a0b1c6` (`pspiofilemgr.h`, `pspumd.h`,
their stub files, and `pspiofilemgr_stat.h` for the stat layout). No PPSSPP
source is an input; running the executable and comparing its output is what
the parent project's provenance policy permits. The probe holds no game
data.

The boot matters: the same probe loaded from the host with the image
mounted by `-m` (runs 157-160) saw no `umd0:` at all, while booted from the
image (run 161) it sees `umd0:` exactly as `umd1:`. A game is booted from
its disc, so the recording is taken that way and the runtime follows it.

Reproduce from the runtime directory, with a checkout of that SDK commit:

```sh
python3 tests/provenance/make_imports.py /path/to/pspsdk tests/provenance/disc/imports.json
python3 tests/provenance/run_probe.py /usr/bin/PPSSPPHeadless /new/output --suite disc
cp /new/output/observed.txt tests/provenance/disc/observed.txt
cp /new/output/metadata.json tests/provenance/disc/capture.json
cp /new/output/probe.iso tests/provenance/disc/probe.iso
python3 tests/provenance/disc/derive.py
/path/to/build/tests/test_disc_observed tests/provenance/disc/observed.txt tests/provenance/disc/probe.iso
```

The runner builds the probe, has `make_iso.py` wrap it into the image, and
boots the image. `capture.json` records executable, compiler, source, image
and output hashes. These are emulator observations, not physical PSP
evidence. The native test includes the same `probe.c`, runs it against the
runtime's I/O adapter with the recorded image mounted, and compares all 154
records field by field.

## The image

Root directory at sector 20; `SUB` at 21; `ALPHA.BIN` at 22, eight sectors;
`BETA.TXT` at 30, 100 bytes; `SUB/GAMMA.BIN` at 31, 6,151 bytes; `PSP_GAME`
at 35 with `PARAM.SFO` at 36, `SYSDIR` at 37 and `EBOOT.BIN`, the probe,
from 38. Word `j` of sector `s` is `(s << 16) | j` in the three data files,
so a read identifies the sector and offset it came from. Every directory
record is dated 2026-09-19 12:00:00.

## Records

Each line is `kind case words…`. Kind 0 is an open (1 or the error); kind 1
a stat (result and the 22 words of the block); kind 2 a read (count and the
first words); kind 3 a seek; kind 4 the raw device; kind 5 the asynchronous
sequence; kind 6 a directory open or close; kind 7 a directory entry (result,
mode, attribute, size, `st_private[0]`, the name's first 12 bytes and the
first time word); kind 8 the drive activation section; kind 9 ends the run.

## What the recording establishes

- **Names** (kind 0): `disc0:`, `umd0:` and `umd1:` with a path are the
  filesystem; `umd0:` and `umd1:` alone are the device. Names are compared
  exactly (`alpha.bin` fails) and without the version suffix (`ALPHA.BIN;1`
  fails). The root and a directory open as files, with or without a trailing
  slash, and `disc0:ALPHA.BIN` opens.
- **Stat** (kind 1): files report mode 0x216d and attribute 0x20,
  directories 0x116d and 0x10, the size, all three times as year 1900 month
  1 whatever the record says, the start sector in `st_private[0]` and
  0xfefefefe in the other five private words. The root reports size 0 and
  sector 0; `umd0:` and `umd1:` are files whose size is the image's sector
  count. A failed stat writes nothing.
- **Reads and seeks** (kinds 2-3): byte-granular positions, `SEEK_END` at
  the file's size, short reads at the end, nothing past it, and the bad
  descriptor error 0x80020323. A directory opened as a file reads its own
  records and sizes as its extent.
- **The raw device** (kind 4, under both names): positions and counts are
  2,048-byte sectors, `SEEK_END` is the image's sector count, a seek past the
  image is not clamped, and a read there answers the count asked and writes
  nothing.
- **Asynchronous operations** (kind 5): a poll found the read running (1)
  before the result came; poll, wait and `sceIoGetAsyncStat` with nothing
  outstanding return 0x8002032a and leave the result alone; a second
  operation before the first is collected returns 0x80020329 and never runs;
  an async seek's result is the position and it moves the descriptor;
  collecting an async close releases the descriptor; a failed async open
  still returns a descriptor whose result is the error, sign-extended, and
  collecting that releases it.
- **Listings** (kinds 6-7): entries come in stored order with no `.` or
  `..`; each carries the stat block above and the name without its version;
  the end of a listing writes only the name's first byte and returns 0; a
  missing directory fails with 0x80010002; a file opens as an empty listing.
- **Activation** (kind 8): the drive reports 0x32 before and after
  `sceUmdActivate`, 0x12 after `sceUmdDeactivate`; neither changes any
  name's answer, and the device reads the same under `umd0:` once activated.

`derive.py` checks every rule above against the records and generates
`src/hle/io_observed.h` (sector size, modes, attributes, the time word, the
private fill and the four error codes).

## Limits

Writes, `ms0:` and host paths, `sceIoDevctl`, `sceIoIoctl`, seeks past the
end of a file, reads that straddle the end of the device, how long an
operation stays running, and the order of directories on a real disc's
larger extents are not observed here. The image is small and its boot tree
is the probe's own; a real disc's `PARAM.SFO` and encrypted `EBOOT.BIN` are
not part of the experiment. Hardware timing is not addressed.
