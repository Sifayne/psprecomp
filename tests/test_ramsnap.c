/* PSPRECOMP_RAMSNAP — whole-RAM snapshots at chosen polls. No game data.
 *
 * What matters about a snapshot is *when* it was taken, because the offline
 * differ reads a constant step across consecutive snapshots as an integrator
 * at work, and a snapshot that is one poll late attributes the step to the
 * wrong input. So: a word written between poll 2 and poll 3 must be absent
 * from the poll-2 snapshot and present in the poll-3 one, and no snapshot may
 * exist for a poll that was not asked for.
 */

#include "psprecomp/hle.h"
#include "psprecomp/mem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAD    0x08820000u
#define MARKER 0x08900000u          /* somewhere quiet in RAM */

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

static void env(const char *key, const char *value) {
#ifdef _WIN32
    _putenv_s(key, value);
#else
    setenv(key, value, 1);
#endif
}

static void poll_pad(void) {
    psp_cpu.r[PSP_REG_A0] = PAD;
    psp_cpu.r[PSP_REG_A1] = 1;
    psp_hle_call(0x3A622550);                  /* sceCtrlPeekBufferPositive */
}

/* -1 if missing, else the word at the marker's offset. */
static long word_in(const char *path, long *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, MARKER - PSP_RAM_BASE, SEEK_SET);
    uint32_t w = 0;
    if (fread(&w, sizeof w, 1, f) != 1) w = 0xFFFFFFFFu;
    fclose(f);
    return (long)w;
}

int main(int argc, char **argv) {
    const char *prefix = argc > 1 ? argv[1] : "ramsnap-test";
    char p1[512], p2[512], p3[512], p4[512];
    snprintf(p1, sizeof p1, "%s-1.ram", prefix);
    snprintf(p2, sizeof p2, "%s-2.ram", prefix);
    snprintf(p3, sizeof p3, "%s-3.ram", prefix);
    snprintf(p4, sizeof p4, "%s-4.ram", prefix);
    remove(p1); remove(p2); remove(p3); remove(p4);

    env("PSPRECOMP_RAMSNAP", prefix);
    env("PSPRECOMP_RAMSNAP_POLLS", "2,3");

    if (psp_mem_init()) return 1;
    psp_hle_init();                             /* reads the environment */

    psp_write32(MARKER, 0x11111111u);
    poll_pad();                                 /* poll 1: nothing */
    poll_pad();                                 /* poll 2: snapshot, marker 0x1111.. */
    psp_write32(MARKER, 0x22222222u);
    poll_pad();                                 /* poll 3: snapshot, marker 0x2222.. */
    poll_pad();                                 /* poll 4: nothing */

    long size = 0;
    CHECK(word_in(p1, &size) == -1, "no snapshot for poll 1, which was not asked for");
    CHECK(word_in(p4, &size) == -1, "no snapshot for poll 4, which was not asked for");

    const long w2 = word_in(p2, &size);
    CHECK(w2 != -1, "poll 2 snapshot exists");
    CHECK(size == (long)PSP_RAM_SIZE, "poll 2 snapshot is all of RAM (%ld bytes)", size);
    CHECK(w2 == 0x11111111L, "poll 2 snapshot holds the value from before the "
                             "write (got %08lX)", w2);

    const long w3 = word_in(p3, &size);
    CHECK(w3 != -1, "poll 3 snapshot exists");
    CHECK(w3 == 0x22222222L, "poll 3 snapshot holds the write made between the "
                             "polls (got %08lX)", w3);

    remove(p1); remove(p2); remove(p3); remove(p4);   /* synthetic output only */
    /* A module image is written beside each snapshot when one is mapped at
     * guest 0; nothing is loaded there in this test, so these usually do not
     * exist, but a runtime that maps one anyway must not leave 64 MB behind. */
    for (int i = 1; i <= 4; i++) {
        char m[512];
        snprintf(m, sizeof m, "%s-%d.mod", prefix, i);
        remove(m);
    }
    psp_mem_free();

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("ramsnap checks passed (no game data)\n");
    return 0;
}
