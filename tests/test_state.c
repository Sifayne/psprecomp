/* Save states (psprecomp/state.h): what is refused, and what a load will not
 * read. Saving and loading a running game are the games' gates
 * (docs/PLAYER-LAYER.md §5); this is the part a unit test can reach: a state
 * is only written from the safe point, by a build with resume entries, and a
 * load reads only a state its own build wrote. A slot list reads what is
 * there without loading it, and the host's requests wait for the safe point. */
#include "psprecomp/dispatch.h"
#include "psprecomp/hle.h"
#include "psprecomp/mem.h"
#include "psprecomp/state.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);   \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
            failures++;                                   \
        }                                                 \
    } while (0)

static void site(uint32_t at) { (void)at; }
static const psp_resume_site SITES[] = { { 0x08804000u, site } };

static struct { uint32_t a, b[4]; } g_kept;
/* A state's header, as state.c lays it out. */
typedef struct {
    char magic[8];
    uint32_t format, header_size;
    uint64_t exe_hash;
    uint32_t functions, resume_sites;
    uint64_t guest_us;
    uint32_t polls, reserved;
    int64_t  saved;
    uint64_t fingerprint;
} state_header;
static uint32_t g_other;

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    char path[1024], why[256];
    if (psp_mem_init() != 0) { printf("FAIL no guest memory\n"); return 1; }
    psp_hle_init();

    /* A build without resume entries cannot continue a thread anywhere. */
    const char *refused = psp_state_refusal();
    CHECK(refused && strstr(refused, "resume"), "no resume table: %s", refused ? refused : "(none)");
    CHECK(psp_state_save("unused", why, sizeof why) == -1, "a refused save writes nothing");

    /* With them, a save still waits for the safe point. */
    psp_resume_register(SITES, 1);
    refused = psp_state_refusal();
    CHECK(refused && strstr(refused, "safe point"), "outside the safe point: %s", refused ? refused : "(none)");

    /* Kept variables: named once, found by any address inside them. */
    PSP_STATE_KEEP(g_kept);
    PSP_STATE_KEEP(g_kept);
    CHECK(psp_state_is_kept(&g_kept.b[3]), "an address inside a kept variable is kept");
    CHECK(!psp_state_is_kept(&g_other), "another variable is not");

    /* A load reads nothing it cannot vouch for. */
    snprintf(path, sizeof path, "%s/state-missing.bin", dir);
    remove(path);
    CHECK(psp_state_load(path, why, sizeof why) == -1 && strstr(why, "state-missing"),
          "a missing file: %s", why);
    snprintf(path, sizeof path, "%s/state-garbage.bin", dir);
    FILE *f = fopen(path, "wb");
    if (f) {
        static unsigned char junk[4096];
        for (size_t i = 0; i < sizeof junk; i++) junk[i] = (unsigned char)(i * 131u + 7u);
        fwrite(junk, 1, sizeof junk, f);
        fclose(f);
    }
    why[0] = '\0';
    CHECK(psp_state_load(path, why, sizeof why) == -1 && strstr(why, "not a state"),
          "a file that is not a state: %s", why);
    CHECK(!psp_state_loaded(), "a failed load leaves the run fresh");
    remove(path);

    /* Slot lists: no file, no state; a file that is not one, no state either. */
    psp_state_info info;
    CHECK(psp_state_peek(path, &info, NULL) == -1, "a missing file has no header");
    f = fopen(path, "wb");
    if (f) { fputs("not a state at all, but long enough to have a header's worth of bytes in it......", f); fclose(f); }
    CHECK(psp_state_peek(path, &info, NULL) == -1, "a file that is not a state has no header");
    remove(path);

    /* Where states live, and their names there. */
    char file[1024];
#ifdef _WIN32
    _putenv_s("PSPRECOMP_STATE_DIR", "");
#else
    unsetenv("PSPRECOMP_STATE_DIR");
#endif
    CHECK(!strcmp(psp_state_dir(), "states"), "the default folder: %s", psp_state_dir());
#ifdef _WIN32
    _putenv_s("PSPRECOMP_STATE_DIR", "somewhere");
#else
    setenv("PSPRECOMP_STATE_DIR", "somewhere", 1);
#endif
    psp_state_file("slot-3", file, sizeof file);
    CHECK(!strcmp(file, "somewhere/slot-3.state"), "a slot's file: %s", file);

    /* A request waits for the safe point: nothing has answered it yet. */
    int ok = -1;
    const unsigned asked = psp_state_request(PSP_STATE_SAVE, "unused");
    CHECK(asked > 0 && psp_state_result(&ok, NULL, 0) < asked, "a request is not answered before a safe point");
    CHECK(psp_state_request(PSP_STATE_LOAD, "unused") == asked + 1, "requests are numbered in order");

    /* The build as states know it. The fingerprint is what a load is decided
     * by: it follows the guest code and the kept variables, not the order
     * functions were registered in. */
    uint64_t exe = 0, fp = 0, fp2 = 0;
    psp_state_id(&exe, &fp);
    CHECK(exe && fp, "a build has an id: exe %llx fingerprint %llx", (unsigned long long)exe, (unsigned long long)fp);
    const uint64_t code = psp_dispatch_fingerprint();
    psp_register(0x08804100u, NULL);
    psp_register(0x08804200u, NULL);
    const uint64_t code2 = psp_dispatch_fingerprint();
    CHECK(code2 != code, "a function more is other code");
    psp_state_id(NULL, &fp2);
    CHECK(fp2 != fp, "and another fingerprint");
    psp_dispatch_reset();
    psp_hle_init();
    psp_resume_register(SITES, 1);
    psp_register(0x08804200u, NULL);
    psp_register(0x08804100u, NULL);
    CHECK(psp_dispatch_fingerprint() == code2, "registered in another order, the same code");
    psp_state_id(NULL, &fp);
    CHECK(fp == fp2, "and the same fingerprint");
    PSP_STATE_KEEP(g_other);
    psp_state_id(NULL, &fp2);
    CHECK(fp2 != fp, "a variable more is another layout");
    CHECK(psp_state_moved(&g_kept) == NULL, "before a load nothing has moved");

    /* Headers, as a slot list reads them. */
    state_header h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, "PSPSTAT1", 8);
    h.guest_us = 61000000;
    h.saved = 1760000000;
    /* Format 3, from another executable with this layout: it loads here. */
    h.format = 3;
    h.header_size = sizeof h;
    h.exe_hash = exe ^ 1;
    h.fingerprint = fp2;
    snprintf(path, sizeof path, "%s/state-header.bin", dir);
    f = fopen(path, "wb");
    if (f) { fwrite(&h, sizeof h, 1, f); fclose(f); }
    CHECK(psp_state_peek(path, &info, NULL) == 0 && info.usable && info.exe == (exe ^ 1) && info.fingerprint == fp2 &&
          info.guest_us == 61000000, "another executable, this layout: usable %d", info.usable);
    /* Another layout: not here, but it names its executable. */
    h.fingerprint = fp2 ^ 1;
    f = fopen(path, "wb");
    if (f) { fwrite(&h, sizeof h, 1, f); fclose(f); }
    CHECK(psp_state_peek(path, &info, NULL) == 0 && !info.usable && info.exe == (exe ^ 1),
          "another layout: usable %d", info.usable);
    /* Format 2 had no fingerprint: never loadable now, still named. */
    h.format = 2;
    h.header_size = (uint32_t)offsetof(state_header, fingerprint);
    f = fopen(path, "wb");
    if (f) {   /* and the chunks a real one has after it */
        static const unsigned char rest[64];
        fwrite(&h, h.header_size, 1, f);
        fwrite(rest, 1, sizeof rest, f);
        fclose(f);
    }
    CHECK(psp_state_peek(path, &info, NULL) == 0 && !info.usable && info.exe == (exe ^ 1) && !info.fingerprint &&
          info.saved == 1760000000, "format 2: usable %d exe %llx", info.usable, (unsigned long long)info.exe);
    why[0] = '\0';
    CHECK(psp_state_load(path, why, sizeof why) == -1 && strstr(why, "another build"), "format 2 loads: %s", why);
    remove(path);

    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("state: ok\n");
    return 0;
}
