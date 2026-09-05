/* Capture selection and memory lifetime, using tiny synthetic GE lists only. */
#include "psprecomp/hle.h"
#include "psprecomp/mem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIST 0x08810000u
#define PAD  0x08820000u

static void env(const char *key, const char *value) {
#ifdef _WIN32
    _putenv_s(key, value);
#else
    setenv(key, value, 1);
#endif
}

/* The first word identifies which complete list survived the selection. */
static void draw(unsigned poll) {
    unsigned words = 0;
    psp_write32(LIST + 4 * words++, poll);             /* NOP marker */
    if (poll != 2) psp_write32(LIST + 4 * words++, 0); /* poll 2 is too small */
    psp_write32(LIST + 4 * words++, 0x0F000000);       /* FINISH */
    psp_write32(LIST + 4 * words++, 0x0C000000);       /* END */
}

static int check_file(const char *path, unsigned marker) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "missing %s\n", path); return 1; }
    uint32_t h[10], list[3], got = 0;
    int failed = fread(h, sizeof h, 1, f) != 1;
    if (!failed) {
        failed = h[0] != 0x50414347 || h[1] != 2 ||
                 h[2] != psp_ge_state_size() || h[3] != 1;
        if (!failed) {
            fseek(f, (long)(sizeof h + h[2]), SEEK_SET);
            failed = fread(list, sizeof list, 1, f) != 1 || list[0] != LIST;
            fseek(f, (long)(sizeof h + h[2] + 12 + LIST - h[4]), SEEK_SET);
            failed |= fread(&got, sizeof got, 1, f) != 1 || got != marker;
        }
    }
    fclose(f);
    remove(path);  /* synthetic test output only */
    if (failed) fprintf(stderr, "bad capture %s: marker %u, expected %u\n",
                        path, got, marker);
    return failed;
}

int main(int argc, char **argv) {
    if (argc < 3 || (strcmp(argv[1], "invalid") == 0 && argc < 4)) return 2;
    const int legacy = strcmp(argv[1], "legacy") == 0;
    const int invalid = strcmp(argv[1], "invalid") == 0;
    /* CTest supplies a unique prefix even when these tests run in parallel. */
    const char *prefix = argv[2];
    char first[512], second[512];
    snprintf(first, sizeof first, legacy ? "%s" : "%s-2.gcap", prefix);
    snprintf(second, sizeof second, "%s-5.gcap", prefix);
    env("PSPRECOMP_GE_CAPTURE", prefix);
    env("PSPRECOMP_GE_CAPTURE_FRAME", "1");
    env("PSPRECOMP_GE_CAPTURE_MINCMDS", "3");
    env("PSPRECOMP_GE_CAPTURE_MINMEAN", "0");
    env("PSPRECOMP_GE_CAPTURE_POLLS", legacy ? "" : invalid ? argv[3] : "2,5");
    if (psp_mem_init()) return 1;
    psp_hle_init();
    psp_ge_init();
    psp_ge_drain_all();
    for (unsigned poll = 1; poll <= 8; poll++) {
        psp_cpu.r[PSP_REG_A0] = PAD;
        psp_cpu.r[PSP_REG_A1] = 1;
        psp_hle_call(0x3A622550);                    /* CtrlPeek */
        psp_ge_drain_all();
        /* Enqueue through the HLE: replay_list intentionally bypasses the
         * capture enqueue hook. */
        draw(poll);
        psp_cpu.r[PSP_REG_A0] = LIST;
        psp_cpu.r[PSP_REG_A1] = 0;
        psp_cpu.r[PSP_REG_A2] = 0;
        psp_cpu.r[PSP_REG_A3] = 0;
        psp_hle_call(0xAB49E76A);
        psp_ge_drain_all();
    }
    int failed = 0;
    if (invalid) {
        FILE *f = fopen(first, "rb");
        if (f) { fclose(f); remove(first); failed = 1; }
    } else {
        failed = check_file(first, legacy ? 1 : 3);
        if (!legacy) failed |= check_file(second, 5);
    }
    if (failed) {
        psp_ge_dump_stats(stderr);
        fprintf(stderr, "polls %u, capture %s\n", psp_ctrl_polls(),
                getenv("PSPRECOMP_GE_CAPTURE"));
    }
    psp_mem_free();
    return failed;
}
