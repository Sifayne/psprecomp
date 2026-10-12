/* A game's prepared builds, and the one a save state needs
 * (docs/PLAYER-LAYER.md §5, "States across updates").
 *
 * Every app update prepares a game again, into a folder of its own beside the
 * last: <data>/games/<slug>/<build>-<time>, holding `game`, `module.elf` and
 * `build.json`. A state carries the hash of the executable that wrote it and
 * its layout's fingerprint (psprecomp/state.h). One the current build cannot
 * load is played in the build whose `game` has that hash, as long as that
 * build is kept; a build no state needs is removed.
 *
 * Each build's identity is noted in its `state-id`: "exe <hex>" and, from a
 * build that can say it (boot --state-id, which the importer runs when it
 * prepares one), "fingerprint <hex>". A build from before that has its
 * executable hashed here the first time it is asked, and the note written. */
#ifndef PSPRECOMP_HOST_BUILDS_H
#define PSPRECOMP_HOST_BUILDS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A build folder's identity: 0, or -1 when it is no build. *fingerprint is
 * 0 when the build cannot say. */
int psp_build_id(const char *build, uint64_t *exe, uint64_t *fingerprint);

/* The folder of the build in `games` (a game's folder of builds) whose
 * executable hashes to `exe`, into `out`: 0, or -1 when none is kept. */
int psp_build_find(const char *games, uint64_t exe, char *out, size_t cap);

/* The folder of builds a build sits in: its parent. */
void psp_build_games(const char *build, char *out, size_t cap);

/* Remove every build in `games` but `current` that no state in `states`
 * needs: one the current build cannot load (another fingerprint, or none)
 * needs the build that wrote it. Only folders that hold a `game` and a
 * `build.json` are touched, and nothing at all when `states` cannot be read
 * or the current build's fingerprint is unknown. Returns how many were
 * removed, and their names in `removed`. */
int psp_builds_prune(const char *games, const char *current, const char *states, char *removed, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
