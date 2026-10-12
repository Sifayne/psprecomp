/* A game's prepared builds, and the one a save state needs. See builds.h. */
#if !defined(_GNU_SOURCE) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700
#endif
#include "builds.h"

#include "psprecomp/state.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <dirent.h>
#include <ftw.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

/* FNV-1a over a file's bytes: what a state records of the executable that
 * wrote it (src/hle/state.c). */
static int hash_file(const char *path, uint64_t *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    static unsigned char buf[1 << 16];
    uint64_t h = 1469598103934665603ull;
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        for (size_t i = 0; i < n; i++) { h ^= buf[i]; h *= 1099511628211ull; }
    const int bad = ferror(f);
    fclose(f);
    if (bad) return -1;
    *out = h ? h : 1;
    return 0;
}

static int is_file(const char *path) {
    struct stat st;
    return !stat(path, &st) && S_ISREG(st.st_mode);
}

/* A folder the importer made for a build: a real directory, not a link,
 * with the game and its record. */
static int is_build(const char *dir) {
    struct stat st;
    if (lstat(dir, &st) || !S_ISDIR(st.st_mode)) return 0;
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/game", dir);
    if (!is_file(path)) return 0;
    snprintf(path, sizeof path, "%s/build.json", dir);
    return is_file(path);
}

int psp_build_id(const char *build, uint64_t *exe, uint64_t *fingerprint) {
    *exe = 0;
    *fingerprint = 0;
    if (!is_build(build)) return -1;
    char note[PATH_MAX];
    snprintf(note, sizeof note, "%s/state-id", build);
    FILE *f = fopen(note, "r");
    if (f) {
        char key[32];
        unsigned long long value;
        while (fscanf(f, "%31s %llx", key, &value) == 2) {
            if (!strcmp(key, "exe")) *exe = value;
            else if (!strcmp(key, "fingerprint")) *fingerprint = value;
        }
        fclose(f);
        if (*exe) return 0;
    }
    /* A build from before the note: its executable hashed once, the note
     * kept for next time. */
    char game[PATH_MAX], tmp[PATH_MAX + 8];
    snprintf(game, sizeof game, "%s/game", build);
    if (hash_file(game, exe)) return -1;
    snprintf(tmp, sizeof tmp, "%s.tmp", note);
    if ((f = fopen(tmp, "w"))) {
        fprintf(f, "exe %016llx\n", (unsigned long long)*exe);
        if (fclose(f) || rename(tmp, note)) remove(tmp);
    }
    return 0;
}

void psp_build_games(const char *build, char *out, size_t cap) {
    snprintf(out, cap, "%s", build);
    size_t n = strlen(out);
    while (n > 1 && out[n - 1] == '/') out[--n] = 0;
    char *slash = strrchr(out, '/');
    if (slash && slash != out) *slash = 0;
    else if (slash) out[1] = 0;
    else snprintf(out, cap, ".");
}

int psp_build_find(const char *games, uint64_t exe, char *out, size_t cap) {
    if (!exe) return -1;
    DIR *d = opendir(games);
    if (!d) return -1;
    int found = -1;
    struct dirent *e;
    while (found && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char dir[PATH_MAX];
        snprintf(dir, sizeof dir, "%s/%s", games, e->d_name);
        uint64_t id, fp;
        if (!psp_build_id(dir, &id, &fp) && id == exe) {
            snprintf(out, cap, "%s", dir);
            found = 0;
        }
    }
    closedir(d);
    return found;
}

static int remove_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw) {
    (void)st; (void)flag; (void)ftw;
    return remove(path);
}

int psp_builds_prune(const char *games, const char *current, const char *states, char *removed, size_t cap) {
    if (removed && cap) removed[0] = 0;
    uint64_t cur_exe, cur_fp;
    if (psp_build_id(current, &cur_exe, &cur_fp) || !cur_fp) return 0;
    char cur_real[PATH_MAX];
    if (!realpath(current, cur_real)) return 0;
    /* What the states need: the build that wrote each one this build
     * cannot load. */
    enum { NEED_MAX = 512 };
    static uint64_t need[NEED_MAX];
    int nneed = 0;
    DIR *d = opendir(states);
    if (!d && errno != ENOENT) return 0;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            const size_t n = strlen(e->d_name);
            if (n < 7 || strcmp(e->d_name + n - 6, ".state")) continue;
            char path[PATH_MAX];
            snprintf(path, sizeof path, "%s/%s", states, e->d_name);
            psp_state_info info;
            if (psp_state_peek(path, &info, NULL) || !info.exe) continue;
            if (info.fingerprint && info.fingerprint == cur_fp) continue;
            if (nneed == NEED_MAX) { closedir(d); return 0; }
            need[nneed++] = info.exe;
        }
        closedir(d);
    }
    DIR *g = opendir(games);
    if (!g) return 0;
    int count = 0;
    struct dirent *e;
    while ((e = readdir(g))) {
        if (e->d_name[0] == '.') continue;
        char dir[PATH_MAX], real[PATH_MAX];
        snprintf(dir, sizeof dir, "%s/%s", games, e->d_name);
        if (!is_build(dir) || !realpath(dir, real) || !strcmp(real, cur_real)) continue;
        uint64_t exe, fp;
        if (psp_build_id(dir, &exe, &fp)) continue;
        int needed = 0;
        for (int i = 0; i < nneed && !needed; i++) needed = need[i] == exe;
        if (needed) continue;
        if (nftw(dir, remove_entry, 16, FTW_DEPTH | FTW_PHYS)) continue;
        count++;
        if (removed && cap) {
            const size_t at = strlen(removed);
            snprintf(removed + at, cap - at, "%s%s", at ? ", " : "", e->d_name);
        }
    }
    closedir(g);
    return count;
}

#else   /* The Windows half is not built in this fork (ROADMAP: Windows). */

int psp_build_id(const char *build, uint64_t *exe, uint64_t *fingerprint) {
    (void)build; *exe = 0; *fingerprint = 0; return -1;
}
int psp_build_find(const char *games, uint64_t exe, char *out, size_t cap) {
    (void)games; (void)exe; (void)out; (void)cap; return -1;
}
void psp_build_games(const char *build, char *out, size_t cap) { snprintf(out, cap, "%s", build); }
int psp_builds_prune(const char *games, const char *current, const char *states, char *removed, size_t cap) {
    (void)games; (void)current; (void)states; if (removed && cap) removed[0] = 0; return 0;
}

#endif
