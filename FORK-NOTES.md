# Last Raven's psprecomp fork

This is [Sifayne/psprecomp](https://github.com/Sifayne/psprecomp), the runtime
and recompilation toolkit used by
[last-raven-recomp](https://github.com/Sifayne/last-raven-recomp).
The original project is [sp00nznet/psprecomp](https://github.com/sp00nznet/psprecomp).

## History and development

`caca759` and its ancestors are unchanged upstream history. The initial
152-patch Last Raven series was replayed as commits above it. Each carries a
`Last-Raven-Patch:` trailer so references to patch numbers in the game's
findings still resolve:

```bash
git log --format='%H %(trailers:key=Last-Raven-Patch,valueonly)' | grep 0021
```

Subsequent commits develop the runtime directly: rendering, input/replay,
audio/video, scheduling and interactive savedata support. The game's
`tools/psprecomp` submodule pins the exact runtime revision it uses; building
the game does not use a separate standalone clone or apply a patch series.

For runtime work in the game checkout, commit inside `tools/psprecomp`, push
that commit to this fork, then commit the parent repository's updated pointer.
Keep the original project as an optional `upstream` remote:

```bash
git remote add upstream https://github.com/sp00nznet/psprecomp.git
git log upstream/main..main
```

The fork contains source and synthetic tests. Game dumps, extracted assets,
generated game C, real saves and PSP key files stay local. The key template
contains placeholders, and the crypto tests use public standard vectors and
arbitrary synthetic keys.

## Remaining limits

- The Windows half of `src/os.c` has not been validated with a Windows build.
- Some MPEG constants and PSP ABI facts come from community reverse
  engineering; source comments identify the evidence. PPSSPP references in
  the networking shim identify names, numeric values and observed behavior.
- Upstream contributions need their own review and scope. The original patch
  series mixes core toolchain changes and game-driven HLE work; publishing
  this fork does not submit those changes upstream.
