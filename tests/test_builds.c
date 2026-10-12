/* A game's prepared builds and the one a save state needs (src/host/builds.h):
 * a build's identity, noted or hashed from its executable and then noted;
 * the build a state's executable names; and which builds are removed -- the
 * ones no state needs, never the current one, never a folder that is not a
 * build or a link to one, and nothing while the states cannot be read or
 * the current build cannot say its fingerprint. Its argument is a folder to
 * work in. */
#include "../src/host/builds.h"
#include "psprecomp/mem.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void put(const char *path, const char *text) {
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", path);
    for (char *p = dir + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(dir, 0755); *p = '/'; }
    FILE *f = fopen(path, "w");
    assert(f && fputs(text, f) >= 0 && !fclose(f));
}
static void build(const char *games, const char *name, const char *id) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s/game", games, name); put(path, name);
    snprintf(path, sizeof path, "%s/%s/build.json", games, name); put(path, "{}\n");
    if (id) { snprintf(path, sizeof path, "%s/%s/state-id", games, name); put(path, id); }
}
static int present(const char *games, const char *name) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s/game", games, name);
    return !access(path, F_OK);
}
/* A state's header, as src/hle/state.c writes it, and enough after it. */
static void state(const char *path, uint64_t exe, uint64_t fingerprint) {
    struct {
        char magic[8]; uint32_t format, header_size; uint64_t exe;
        uint32_t functions, resume_sites; uint64_t guest_us; uint32_t polls, reserved; int64_t saved; uint64_t fingerprint;
    } h = { {'P','S','P','S','T','A','T','1'}, 3, sizeof h, exe, 0, 0, 0, 0, 0, 1, fingerprint };
    put(path, "");
    FILE *f = fopen(path, "wb");
    static const unsigned char rest[64];
    assert(f && fwrite(&h, sizeof h, 1, f) == 1 && fwrite(rest, 1, sizeof rest, f) == sizeof rest && !fclose(f));
}

int main(int argc, char **argv) {
    assert(argc == 2);
    assert(!psp_mem_init());
    char root[512], games[600], states[600], cur[700], path[800], removed[256];
    snprintf(root, sizeof root, "%s/builds-test", argv[1]);
    snprintf(path, sizeof path, "rm -rf '%s'", root); assert(!system(path));
    snprintf(games, sizeof games, "%s/games/g", root);
    snprintf(states, sizeof states, "%s/states/g", root);
    snprintf(cur, sizeof cur, "%s/cur", games);

    /* An identity noted; one hashed from the executable and noted then:
     * FNV-1a over "old", the bytes of its game. */
    build(games, "cur", "exe 0000000000000c0c\nfingerprint 000000000000f00d\n");
    build(games, "old", NULL);
    uint64_t exe, fp;
    assert(!psp_build_id(cur, &exe, &fp) && exe == 0xc0c && fp == 0xf00d);
    snprintf(path, sizeof path, "%s/old", games);
    uint64_t h = 1469598103934665603ull;
    for (const char *c = "old"; *c; c++) { h ^= (unsigned char)*c; h *= 1099511628211ull; }
    assert(!psp_build_id(path, &exe, &fp) && exe == h && !fp);
    snprintf(path, sizeof path, "%s/old/state-id", games);
    assert(!access(path, F_OK));
    /* No build: a folder without the game, or without the record. */
    snprintf(path, sizeof path, "%s/bare/build.json", games); put(path, "{}\n");
    snprintf(path, sizeof path, "%s/bare", games);
    assert(psp_build_id(path, &exe, &fp));
    /* The build a state names, and none for an unknown executable. */
    char found[800];
    assert(!psp_build_find(games, h, found, sizeof found) && strstr(found, "/old"));
    assert(psp_build_find(games, 0x1234, found, sizeof found));
    snprintf(path, sizeof path, "%s/cur", games);
    psp_build_games(path, found, sizeof found);
    assert(!strcmp(found, games));

    /* What goes: "old" is needed by a state this build cannot load;
     * "spare" by none; "here" wrote a state this build can load, so is not
     * needed either; "bare" is no build; "link" is a link to "spare". */
    build(games, "spare", "exe 0000000000000b0b\n");
    build(games, "here", "exe 0000000000000d0d\nfingerprint 000000000000f00d\n");
    snprintf(path, sizeof path, "%s/link", games);
    char target[700]; snprintf(target, sizeof target, "%s/spare", games);
    assert(!symlink(target, path));
    snprintf(path, sizeof path, "%s/slot-1.state", states); state(path, h, 0);
    snprintf(path, sizeof path, "%s/quit.state", states); state(path, 0xd0d, 0xf00d);
    /* Not while the current build cannot say its fingerprint. */
    snprintf(path, sizeof path, "%s/state-id", cur); put(path, "exe 0000000000000c0c\n");
    assert(psp_builds_prune(games, cur, states, removed, sizeof removed) == 0 && present(games, "spare"));
    put(path, "exe 0000000000000c0c\nfingerprint 000000000000f00d\n");
    /* Not while the states cannot be read. */
    assert(!chmod(states, 0));
    if (access(states, R_OK)) {   /* not when run as root, who reads it anyway */
        assert(psp_builds_prune(games, cur, states, removed, sizeof removed) == 0 && present(games, "spare"));
    }
    assert(!chmod(states, 0755));
    const int n = psp_builds_prune(games, cur, states, removed, sizeof removed);
    assert(n == 2 && strstr(removed, "spare") && strstr(removed, "here"));
    assert(present(games, "cur") && present(games, "old") && !present(games, "spare") && !present(games, "here"));
    snprintf(path, sizeof path, "%s/bare/build.json", games); assert(!access(path, F_OK));
    snprintf(path, sizeof path, "%s/link", games);
    struct stat st; assert(!lstat(path, &st) && S_ISLNK(st.st_mode));
    /* A missing states folder needs nothing: everything but the current goes. */
    snprintf(path, sizeof path, "%s/states/none", root);
    assert(psp_builds_prune(games, cur, path, removed, sizeof removed) == 1 && !present(games, "old") && present(games, "cur"));

    snprintf(path, sizeof path, "rm -rf '%s'", root); assert(!system(path));
    puts("builds: identities noted and hashed, a state's build found, and only the builds no state needs removed");
    return 0;
}
