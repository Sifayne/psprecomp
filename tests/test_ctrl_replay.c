/* Scripted pad input — the parser, the player, and the record/replay round trip.
 *
 * Everything here drives the *real* guest read, sceCtrlPeekBufferPositive, and
 * reads the SceCtrlData back out of guest memory. Testing the player against
 * an accessor instead would leave the part that actually matters untested:
 * the merge in misc.c, the poll counter it is keyed on, and the recorder that
 * has to observe the same value the guest did.
 *
 * No game data: every scenario here is written by the test into a temp file.
 */

#include "psprecomp/hle.h"
#include "psprecomp/dispatch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);        \
            printf(__VA_ARGS__);                               \
            printf("\n");                                      \
            failures++;                                        \
        }                                                      \
    } while (0)

#define CTRL_PEEK 0x3A622550u
#define BUF       0x08820000u

#define BTN_START  0x000008u
#define BTN_UP     0x000010u
#define BTN_DOWN   0x000040u
#define BTN_CROSS  0x004000u
#define BTN_SQUARE 0x008000u

/* One guest poll. Returns the buttons the guest was handed; the analog pair
 * comes back through `ax`/`ay` when asked for. */
static uint32_t poll_pad(uint8_t *ax, uint8_t *ay) {
    psp_cpu.r[PSP_REG_A0] = BUF;
    psp_cpu.r[PSP_REG_A1] = 1;
    psp_hle_call(CTRL_PEEK);
    if (ax) *ax = psp_read8(BUF + 8);
    if (ay) *ay = psp_read8(BUF + 9);
    return psp_read32(BUF + 4);
}

static const char *write_scenario(const char *name, const char *body) {
    static char path[256];
    snprintf(path, sizeof path, "%s", name);
    FILE *f = fopen(path, "w");
    if (!f) { printf("FAIL: cannot write %s\n", path); failures++; return NULL; }
    fputs(body, f);
    fclose(f);
    return path;
}

/* Load a scenario and reset the pad, as a fresh run would. */
static void load(const char *path) {
    setenv("PSPRECOMP_REPLAY", path, 1);
    psp_misc_init();
}

static void unload(void) {
    unsetenv("PSPRECOMP_REPLAY");
    unsetenv("PSPRECOMP_REPLAY_REC");
    psp_misc_init();
}

/* ---- the parser ---------------------------------------------------------- */

static void test_directives(void) {
    /* Every directive and every timestamp form, in one file. If any line
     * fails to parse the loader rejects the whole file, and every poll below
     * would then read neutral -- so the first CHECK covers the parse too. */
    const char *p = write_scenario("t-directives.pad",
        "# a comment, and a blank line follow\n"
        "\n"
        "drain   90\n"
        "minhold 2\n"
        "  @1    down cross\n"
        "  @2    up   cross\n"
        "  @3    down start,square       # a list\n"
        "  @4    down 0x000400           # a bit the table does not name\n"
        "  @5    state none 200 60\n"
        "  @6    analog center\n"
        "  @7    neutral\n"
        "  +1    down up                 # relative poll\n"
        "  @9    mark \"a mark # with a hash\"\n"
        "  @10   wait 1\n"
        "  @12   down down\n");
    if (!p) return;
    load(p);

    CHECK(psp_ctrl_replay_active(), "scenario loaded and parsed");
    CHECK(psp_ctrl_replay_drain() == 90, "drain header read: got %d",
          psp_ctrl_replay_drain());

    uint8_t ax = 0, ay = 0;
    CHECK(poll_pad(NULL, NULL) == BTN_CROSS, "@1 down cross");
    CHECK(poll_pad(NULL, NULL) == 0, "@2 up cross");
    CHECK(poll_pad(NULL, NULL) == (BTN_START | BTN_SQUARE), "@3 a button list");
    CHECK(poll_pad(NULL, NULL) == (BTN_START | BTN_SQUARE | 0x000400u),
          "@4 a raw 0x bit");
    CHECK(poll_pad(&ax, &ay) == 0 && ax == 200 && ay == 60,
          "@5 state assigns buttons and stick: got %u,%u", ax, ay);
    CHECK(poll_pad(&ax, &ay) == 0 && ax == 128 && ay == 128, "@6 analog center");
    CHECK(poll_pad(NULL, NULL) == 0, "@7 neutral");
    CHECK(poll_pad(NULL, NULL) == BTN_UP, "+1 relative poll stamp");
    poll_pad(NULL, NULL);                      /* poll  9: mark, not an edge */
    poll_pad(NULL, NULL);                      /* poll 10: wait, not an edge */
    CHECK(poll_pad(NULL, NULL) == BTN_UP,
          "poll 11: @12 is not due yet -- a stamp is a floor, not a queue");
    CHECK(poll_pad(NULL, NULL) == (BTN_UP | BTN_DOWN), "@12 after a wait");

    unload();
}

static void test_malformed_is_rejected(void) {
    /* A scenario with a bad line must not load *at all*. Half-loading it
     * would deliver a truncated input sequence while looking like it worked,
     * which is the worst of the three options. */
    const char *p = write_scenario("t-bad.pad",
        "@1 down cross\n"
        "@2 down nosuchbutton\n"
        "@3 frobnicate\n");
    if (!p) return;
    load(p);
    CHECK(!psp_ctrl_replay_active(), "a file with bad lines does not load");
    CHECK(poll_pad(NULL, NULL) == 0, "and drives nothing");
    unload();
}

/* ---- the semantics ------------------------------------------------------- */

static void test_one_edge_per_poll(void) {
    /* Both edges of a tap come due at the same poll. Applying both would show
     * the guest an unchanged pad -- it would never see the button down, and a
     * press the guest cannot observe is a press that did not happen. */
    const char *p = write_scenario("t-tap.pad",
        "minhold 1\n"
        "@0 tap cross\n");
    if (!p) return;
    load(p);

    int saw_down = 0, saw_up_after = 0;
    for (int i = 0; i < 6; i++) {
        uint32_t b = poll_pad(NULL, NULL);
        if (b & BTN_CROSS) saw_down = 1;
        else if (saw_down) saw_up_after = 1;
    }
    CHECK(saw_down, "a tap whose edges are both due is still observable");
    CHECK(saw_up_after, "and it is released afterwards");
    unload();
}

static void test_catch_up(void) {
    /* Every stamp is already in the past at the first poll. All of them must
     * still be delivered, in written order, one per poll. */
    const char *p = write_scenario("t-catchup.pad",
        "@0 down cross\n"
        "@0 down start\n"
        "@0 up   cross\n");
    if (!p) return;
    load(p);

    CHECK(poll_pad(NULL, NULL) == BTN_CROSS, "catch-up: first edge");
    CHECK(poll_pad(NULL, NULL) == (BTN_CROSS | BTN_START), "catch-up: second");
    CHECK(poll_pad(NULL, NULL) == BTN_START, "catch-up: third, in order");
    unload();
}

static void test_hold_lane_survives_the_script(void) {
    /* PSPRECOMP_PAD is a separate lane, so a scenario releasing the same
     * button cannot cancel the hold. The old single-word arrangement could:
     * the press instrument's fetch_and cleared bits it never set. */
    const char *p = write_scenario("t-lanes.pad", "@1 up cross\n");
    if (!p) return;
    setenv("PSPRECOMP_PAD", "cross", 1);
    load(p);

    CHECK(poll_pad(NULL, NULL) == BTN_CROSS, "hold lane is delivered");
    CHECK(poll_pad(NULL, NULL) == BTN_CROSS, "and a script `up` cannot clear it");

    unsetenv("PSPRECOMP_PAD");
    unload();
}

static void test_host_lane_composes(void) {
    /* Live input ORs with the script rather than replacing it, and taints. */
    const char *p = write_scenario("t-host.pad", "@1 down cross\n");
    if (!p) return;
    load(p);

    CHECK(poll_pad(NULL, NULL) == BTN_CROSS, "script lane");
    psp_ctrl_set(BTN_START, 128, 128);
    CHECK(poll_pad(NULL, NULL) == (BTN_CROSS | BTN_START),
          "host lane ORs with the script instead of erasing it");

    psp_ctrl_set(0, 128, 128);
    unload();
}

/* ---- the round trip ------------------------------------------------------ */

static void drive(uint32_t *out, int n) {
    for (int i = 0; i < n; i++) out[i] = poll_pad(NULL, NULL);
}

static void test_round_trip(void) {
    /* Record a run, replay the recording, and require the guest to see the
     * same thing both times. This is the check that pins the whole contract:
     * the recorder writes in the timebase the player consumes, so a file it
     * produces must reproduce the session it came from. */
    const char *p = write_scenario("t-rt-src.pad",
        "minhold 2\n"
        "@2  down cross\n"
        "@5  down start\n"
        "@6  up   cross,start\n"
        "@8  analog 30 220\n"
        "@11 neutral\n");
    if (!p) return;

    enum { N = 16 };
    uint32_t first[N], second[N];

    setenv("PSPRECOMP_REPLAY_REC", "t-rt-rec.pad", 1);
    load(p);
    drive(first, N);
    psp_ctrl_replay_finish(NULL);       /* closes the recording */
    unload();

    load("t-rt-rec.pad");
    CHECK(psp_ctrl_replay_active(), "the recording parses as a scenario");
    drive(second, N);
    unload();

    int same = 1;
    for (int i = 0; i < N; i++) if (first[i] != second[i]) same = 0;
    CHECK(same, "replaying a recording reproduces it");
    if (!same)
        for (int i = 0; i < N; i++)
            printf("      poll %2d: recorded 0x%06X  replayed 0x%06X%s\n",
                   i, first[i], second[i], first[i] != second[i] ? "  <--" : "");
}

int main(void) {
    CHECK(psp_mem_init() == 0, "memory init");
    psp_cpu_reset();
    psp_cpu.r[PSP_REG_SP] = 0x08810000u;
    psp_hle_init();

    test_directives();
    test_malformed_is_rejected();
    test_one_edge_per_poll();
    test_catch_up();
    test_hold_lane_survives_the_script();
    test_host_lane_composes();
    test_round_trip();

    remove("t-directives.pad"); remove("t-bad.pad"); remove("t-tap.pad");
    remove("t-catchup.pad"); remove("t-lanes.pad"); remove("t-host.pad");
    remove("t-rt-src.pad"); remove("t-rt-rec.pad");
    psp_mem_free();

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("all scripted-input checks passed\n");
    return 0;
}
