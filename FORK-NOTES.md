# This fork — what it is and what to do with it

Built locally from `last-raven`'s patch series. **Not yet pushed anywhere**:
`origin` still points at upstream, deliberately, so nothing can be pushed by
accident.

## What is here

`caca759` and everything below it is upstream `sp00nznet/psprecomp`, untouched.
The 152 commits above it are the patch series replayed as history, in series
order, one commit per patch.

The working tree is **byte-for-byte identical** to `last-raven/tools/psprecomp`,
and the tests pass here as a standalone repository: 13/13.

Each commit carries a `Last-Raven-Patch:` trailer naming the patch it came
from, so the 40-odd references to "patch `0021`" in `last-raven/docs/findings`
still resolve:

```bash
git log --format='%H %(trailers:key=Last-Raven-Patch,valueonly)' | grep 0021
```

## To publish it

1. Fork `sp00nznet/psprecomp` on GitHub (needs a browser, or `gh repo fork`
   once `gh` is installed — it is not, on this machine).
2. Point this clone at it and push:

```bash
cd /home/sif/Projects/psprecomp
git remote set-url origin git@github.com:<you>/psprecomp.git
git push -u origin main
```

3. Point last-raven's submodule at the fork:

```bash
cd /home/sif/Projects/last-raven
git config -f .gitmodules submodule.tools/psprecomp.url git@github.com:<you>/psprecomp.git
git submodule sync tools/psprecomp
```

## What still needs deciding

**The submodule is currently a dirty working tree that the patch series
reconstructs.** Once the fork exists, the natural move is to check out these
commits in `tools/psprecomp` and let git track it normally, at which point
`patches/`, `scripts/verify-patches.sh` and `mkpatch.sh` are doing a job
nothing needs any more. That is a separate change and this fork does not force
it — both can coexist while you decide.

**The core/HLE split for upstreaming.** Of the 152, roughly 38 touch the
emitter, decoder, interpreter or renderer — the part that was always
upstreamable — 99 are HLE only, and 14 touch both. Those 14 need unpicking or
accepting as mixed before any subset can be offered upstream as a clean branch.

**The Windows half of `src/os.c` is unrun.** Written against the documented
Win32 API and never compiled: there is no MSVC or mingw here. `mingw-w64-gcc`
is packaged on this machine and not installed. Compile-check it before anyone
is asked to review it.

**`src/hle/mpeg.c`'s constants** are community reverse engineering with no
published specification behind them, validated end-to-end by the movie playing
rather than by citation. Worth saying so in the file header if this goes
upstream, rather than leaving it mid-file where a reviewer meets it by surprise.
