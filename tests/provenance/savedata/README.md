# Original savedata result experiment

`probe.c` submits our own savedata requests and records API returns and the
parameter block's result word. Layout, modes and imports come from BSD PSPSDK
commit `654ac51fc73fbf7ad9350fac90e39e66e7a0b1c6`, `psputility.h`,
`psputility_savedata.h` and `sceUtility.S`. No PPSSPP implementation source is
an input. Earlier exposure remains documented in the parent provenance audit.

Run from the runtime directory:

```sh
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy \
  python3 tests/provenance/run_probe.py /path/to/PPSSPPHeadless /new/output --suite savedata
python3 tests/provenance/savedata/derive.py
```

This suite requires Linux bubblewrap. The entire real home is hidden behind
a temporary filesystem. Only `/new/output/memorystick` is mounted at the
external executable's observed Memory Stick location; the rest of the host
filesystem is read-only. The probe ELF is mounted read-only under `/tmp/probe`.
HOME is not changed. `--root` alone is insufficient: it mounts `host0`, not
`ms0`. Every fixture is project-authored and named `PRV260919*`; no user saves
or game assets are supplied to the experiment.

`run_probe.py` creates deliberately malformed metadata, unreadable files,
a directory where a data file should be, and a directory without write
permission. The probe creates a valid save via the public API, then deletes
selected files, tries absent saves, restricts output capacity, and submits
invalid-size and overlapping requests. `capture.json` records hashes of the
executable, ELF, sources, runner/fixture generator and all 29 output records.

`derive.py` selects nine observed results by named stimulus and generates
`src/hle/savedata_results.h`. There is no retained table of source-derived
utility access-error numbers. Host I/O failures use the BSD SDK generic error
as an explicit host policy. Load error selection has also been reconstructed:
missing named data takes precedence over malformed metadata; an insufficient
output buffer or malformed metadata produces the observed invalid-data result.
The existing UI tests no longer carry their old fixed result-code oracle.

The native checker includes the same probe C file. **23 records compare
exactly**, and **six cases exercise documented host policies separately**.
Those six cannot honestly be labeled a full emulator match: our transactional
save can rename a read-only slot when its parent is writable, whereas the
external executable attempts in-place writes/deletes and sometimes reports
success despite filesystem failures. The checker verifies the committed file
or removed directory for successful native transactions and negative results
for failed native operations. Permission checks are conditional on POSIX.

This does not establish exact firmware permission-error codes, encryption,
all dialog modes, physical Memory Stick behavior, or firmware timing. Native
secure modes still use the previously documented plaintext storage capability.
These are external executable observations, not physical PSP captures.
