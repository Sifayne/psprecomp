/* PSPRECOMP_WATCHMEM_FROM — the memory watch's report budget, held to a poll.
 *
 * The watch reports the first 32 writes to an address and then stops. For a
 * word that is cleared at mission start and rewritten every frame, all 32 go
 * to the clears, and the write that was being asked about -- during play,
 * thousands of polls later -- is never reported. WATCHMEM_FROM holds the
 * budget until a named poll. What has to be true: writes before that poll
 * count for nothing, the write at that poll and after count, and with the
 * variable unset nothing changes.
 */

#include "psprecomp/hle.h"
#include "psprecomp/mem.h"
#include <stdio.h>
#include <stdlib.h>

#define PAD  0x08820000u
#define WORD 0x08900000u

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

int main(void) {
    env("PSPRECOMP_WATCHMEM_FROM", "3");

    if (psp_mem_init()) return 1;
    psp_hle_init();                             /* reads the environment: disarms */
    psp_mem_watch_write(WORD);

    psp_write32(WORD, 1);
    CHECK(psp_mem_watch_hits() == 0, "before any poll the watch is held (hits %d)",
          psp_mem_watch_hits());
    poll_pad();                                 /* poll 1 */
    psp_write32(WORD, 2);
    poll_pad();                                 /* poll 2 */
    psp_write32(WORD, 3);
    CHECK(psp_mem_watch_hits() == 0, "writes before poll 3 are not reported (hits %d)",
          psp_mem_watch_hits());

    poll_pad();                                 /* poll 3: armed */
    psp_write32(WORD, 4);
    CHECK(psp_mem_watch_hits() == 1, "the first write at poll 3 is reported (hits %d)",
          psp_mem_watch_hits());
    poll_pad();                                 /* poll 4 */
    psp_write32(WORD, 5);
    CHECK(psp_mem_watch_hits() == 2, "and it stays armed afterwards (hits %d)",
          psp_mem_watch_hits());

    psp_mem_free();

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("watch-arm checks passed (no game data)\n");
    return 0;
}
