/* Save states (psprecomp/state.h): what is refused, and what a load will not
 * read. Saving and loading a running game are the games' gates
 * (docs/PLAYER-LAYER.md §5); this is the part a unit test can reach: a state
 * is only written from the safe point, by a build with resume entries, and a
 * load reads only a state its own build wrote. */
#include "psprecomp/dispatch.h"
#include "psprecomp/hle.h"
#include "psprecomp/mem.h"
#include "psprecomp/state.h"

#include <stdio.h>
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

    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("state: ok\n");
    return 0;
}
