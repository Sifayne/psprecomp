# The player

One application per pack: a launcher, an ISO importer, an on-device compiler
built from the bundled Zig, this runtime and the shared host. It prepares
each supported game on the player's own machine from their own disc; no game
code or assets are ever in the package. The design and its stages are
[`docs/PLAYER-LAYER.md`](../docs/PLAYER-LAYER.md); this directory was Last
Raven's packaging until stage 2 moved it here.

## Building a package

```sh
python3 player/package-linux.py build --pack PATH [--output DIR] [--work DIR]
```

`PATH` is a directory holding a `pack.json`. The build needs an x86-64 Linux
host with Python 3.9+, curl, tar, Bubblewrap and unprivileged user
namespaces. Pinned downloads (`linux/dependencies.json`,
`../third_party/ffmpeg/source.json`) and an unprivileged Ubuntu 22.04 builder
are cached in `--work`, by default the pack's `build/package`; everything
after the bootstrap runs without a network. The container sees this
toolkit's runtime, recompiler and player sources and the files the pack
names, nothing else, and the release carries a matching source archive of
exactly those.

## Packs

A pack is everything about one game, or one family of games, as source and
data. Its manifest is documented at the top of [`pack.py`](pack.py):
- the app's names and folders;
- the titles, with each one's accepted executable hash;
- the files compiled on the player's machine, and its code generators;
- a CMake file for the pack's own programs, which
  [`linux/CMakeLists.txt`](linux/CMakeLists.txt) includes;
- the checks the builder runs;
- the sources the matching archive must carry.

The launcher is the toolkit's (`src/host/launcher.c`). Its pages come from
the pack's settings schema, and the rest of what it shows from the pack's
`psp_launcher_info` (`include/psprecomp/host/launcher.h`). A pack's CMake
file builds it from `PLAYER_LAUNCHER` with those two.

Last Raven's `pack.json` is the first. Its CMake file also carries `boot.c`
and the GL backend until those move into the shared host.

## Files

| File | Role |
|---|---|
| `package-linux.py` | the host side: fetch, bootstrap, stage inputs, run the build |
| `linux/build.py`, `linux/preparation.py` | the build inside the container: libraries, tools, checks, staging, the AppImage, the source archive |
| `linux/CMakeLists.txt` | the runtime, recompiler and shared host checks; includes the pack's file |
| `linux/AppRun.in`, `linux/import-game.in`, `linux/run-game.in` | the app's entry points, filled in with the pack's names |
| `linux/game_fingerprints.py` | which inputs invalidate each title's prepared game |
| `import_game.py`, `compile_game.py`, `emit-split.py` | staged in the app: importing a disc, compiling a title |
| `ffmpeg.py` | the pinned LGPL FFmpeg, also used by the games' development builds (`--deps`) |
| `pack.py` | loading and checking a pack's manifest |
| `tests/` | package, importer, fingerprint and FFmpeg checks |

`tests/test_import_game.py` and `tests/test_game_fingerprints.py` run from
a checkout with synthetic titles; `tests/test_package.py` needs a built
AppDir; `tests/test_ffmpeg.py` needs an FFmpeg build (`FFMPEG_DEPS`).
