# The player

One application, psprecomp, for every game. It has:
- a launcher;
- an importer for packs and ISOs;
- an on-device compiler built from the bundled Zig;
- this runtime and the shared host.

It holds no game of its own. A player adds a **pack**, the source of what
makes one game or one family of games run, and the app builds it on their
machine. Then it prepares each game from their own disc. No game code or
assets are ever in the package.

The design and its stages are in
[`docs/PLAYER-LAYER.md`](../docs/PLAYER-LAYER.md), §1 and §6. This directory
was Last Raven's packaging until stage 2 moved it here.

## Building the app and the packs' files

```sh
python3 player/package-linux.py build [--pack PATH]... [--output DIR] [--work DIR]
```

The release holds `psprecomp-x86_64.AppImage`, its matching source archive
and, for each `--pack`, a `<file>-pack.zip` that a player adds from the
launcher's Packs page or with `--add-pack`. Each pack's checks run in the
builder, and its file is added to a copy of the staged app as a player would
add it, which compiles the pack with the app's own Zig.

The build needs an x86-64 Linux host with:
- Python 3.9+, curl, tar and Bubblewrap;
- unprivileged user namespaces.

Pinned downloads (`linux/dependencies.json`,
`../third_party/ffmpeg/source.json`) and an unprivileged Ubuntu 22.04
builder are cached in `--work`, by default `build/package` here. Everything
after the bootstrap runs without a network.

The container sees this toolkit's runtime, recompiler and player sources and
the files each pack names, nothing else. The release carries a matching
source archive of exactly those.

## Packs

A pack is everything about one game, or one family of games, as source and
data. Its manifest, version 2, is documented at the top of
[`pack.py`](pack.py):
- the pack's id, name, file name and license;
- the titles, with each one's accepted executable hash;
- its host sources: compiled once when it is added, and linked into each of
  its games;
- its launcher part: its settings schema and `psp_launcher_info`, built into
  a `launcher.so` the launcher loads;
- the files compiled with each title's game, and its code generators;
- optionally, a CMake file for its checks, which
  [`linux/CMakeLists.txt`](linux/CMakeLists.txt) includes;
- the other files its pack file carries.

**The launcher** is the toolkit's (`src/host/launcher.c`):
- Its pages are the player's settings, then the selected game's pack's.
- The rest of what it shows of a pack comes from that pack's
  `psp_launcher_info` (`include/psprecomp/host/launcher.h`).
- A game's own development launcher links its pack in with
  `src/host/launcher_one.c`.

Last Raven's and The 3rd Birthday's `pack.json` are the two so far.

## Files

| File | Role |
|---|---|
| `package-linux.py` | the host side: fetch, bootstrap, stage inputs, run the build |
| `linux/build.py`, `linux/preparation.py` | the build inside the container: libraries, tools, checks, staging, the packs' files, the AppImage, the source archive |
| `linux/CMakeLists.txt` | the runtime, the player archive, the recompiler, the launcher and the shared host's checks; each pack's checks |
| `linux/AppRun.in`, `linux/import-game.in`, `linux/run-game.in` | the app's entry points |
| `linux/psprecomp.desktop`, `linux/README.txt` | the app's desktop entry and its release notes |
| `linux/game_fingerprints.py` | which inputs invalidate each title's prepared game; staged in the app |
| `import_game.py`, `compile_game.py`, `emit-split.py`, `pack.py` | staged in the app: adding and removing packs, importing a disc, building a pack and a title |
| `ffmpeg.py` | the pinned LGPL FFmpeg, also used by the games' development builds (`--deps`) |
| `tests/` | package, importer, fingerprint and FFmpeg checks |

These checks run from a checkout, with a synthetic pack:
- `tests/test_import_game.py`;
- `tests/test_game_fingerprints.py`.

`tests/test_package.py` needs a built AppDir, and `tests/test_ffmpeg.py`
needs an FFmpeg build (`FFMPEG_DEPS`). The importer insists on 2 GB free
where it prepares; set `TMPDIR` to a disk folder when `/tmp` is small.
