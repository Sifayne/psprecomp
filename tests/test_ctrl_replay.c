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
    CHECK(psp_ctrl_pressed_buttons()==BTN_CROSS, "merged press snapshot");
    CHECK(poll_pad(NULL, NULL) == 0, "@2 up cross");
    CHECK(!psp_ctrl_pressed_buttons(), "release is not a press");
    CHECK(poll_pad(NULL, NULL) == (BTN_START | BTN_SQUARE), "@3 a button list");
    CHECK(poll_pad(NULL, NULL) == (BTN_START | BTN_SQUARE | 0x000400u),
          "@4 a raw 0x bit");
    CHECK(psp_ctrl_pressed_buttons()==0x000400u, "carrier press excludes held buttons");
    CHECK(poll_pad(&ax, &ay) == 0 && ax == 200 && ay == 60,
          "@5 state assigns buttons and stick: got %u,%u", ax, ay);
    CHECK(poll_pad(&ax, &ay) == 0 && ax == 128 && ay == 128, "@6 analog center");
    CHECK(poll_pad(NULL, NULL) == 0, "@7 neutral");
    CHECK(poll_pad(NULL, NULL) == BTN_UP, "+1 relative poll stamp");
    poll_pad(NULL, NULL);                      /* poll  9: mark, not an edge */
    poll_pad(NULL, NULL);                      /* poll 10: wait, not an edge */
    CHECK(poll_pad(NULL, NULL) == BTN_UP,
          "poll 11: @12 is not due yet -- a stamp is a floor, not a queue");
    CHECK(!psp_ctrl_pressed_buttons(), "held buttons do not repeat a press");
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

/* The look channel: a second stick and a mouse delta, carried in the same
 * lanes and written by the recorder as optional trailing fields. What has to
 * be true: a `state` or `analog` line with two more numbers claims the second
 * stick for the script; `mouse` delivers a delta at exactly one poll and never
 * again; a line without the fields leaves the channel as it was; `neutral`
 * releases it, after which the host lane shows through and mouse motion sums
 * between polls and is taken by each one. */
static void test_look_channel(void) {
    const char *p = write_scenario("t-look.pad",
        "@1 analog 128 128 200 60\n"
        "@2 mouse -40 12\n"
        "@3 state none 100 128\n"
        "@4 state none 128 128 128 128\n"
        "@5 analog 10 20 30 40\n"
        "@7 neutral\n");
    load(p);
    uint8_t ax, ay, rx, ry; int mdx, mdy;

    poll_pad(&ax, &ay); psp_ctrl_last_look(&rx, &ry, &mdx, &mdy);
    CHECK(ax == 128 && rx == 200 && ry == 60 && mdx == 0,
          "@1 analog with four values claims the second stick (rx %u ry %u)", rx, ry);
    poll_pad(NULL, NULL); psp_ctrl_last_look(&rx, &ry, &mdx, &mdy);
    CHECK(mdx == -40 && mdy == 12 && rx == 200,
          "@2 a mouse delta arrives with the stick still held (mdx %d mdy %d rx %u)",
          mdx, mdy, rx);
    poll_pad(&ax, NULL); psp_ctrl_last_look(&rx, &ry, &mdx, &mdy);
    CHECK(ax == 100 && rx == 200 && mdx == 0 && mdy == 0,
          "@3 a line without look fields leaves the stick, and the delta was spent "
          "(ax %u rx %u mdx %d)", ax, rx, mdx);
    poll_pad(NULL, NULL); psp_ctrl_last_look(&rx, &ry, &mdx, &mdy);
    CHECK(rx == 128 && ry == 128, "@4 an explicit 128 128 centres it (rx %u ry %u)", rx, ry);

    /* Live look input while the script owns the channel is ignored, and the
     * mouse sum is still taken by the poll so it cannot leak out later. */
    psp_ctrl_set_look(250, 250); psp_ctrl_add_mouse(99, 99);
    poll_pad(&ax, &ay); psp_ctrl_last_look(&rx, &ry, &mdx, &mdy);
    CHECK(ax == 10 && ay == 20 && rx == 30 && ry == 40 && mdx == 0,
          "@5 both sticks from the script, live look ignored (rx %u mdx %d)", rx, mdx);
    poll_pad(NULL, NULL); psp_ctrl_last_look(&rx, &ry, &mdx, &mdy);
    CHECK(rx == 30 && mdx == 0, "@6 nothing due: the script's stick holds (rx %u)", rx);

    psp_ctrl_add_mouse(5, 0); psp_ctrl_add_mouse(6, 0);
    poll_pad(NULL, NULL); psp_ctrl_last_look(&rx, &ry, &mdx, &mdy);
    CHECK(rx == 250 && mdx == 11,
          "@7 neutral releases the channel: the host stick shows and motion summed "
          "between polls (rx %u mdx %d)", rx, mdx);
    poll_pad(NULL, NULL); psp_ctrl_last_look(&rx, &ry, &mdx, &mdy);
    CHECK(mdx == 0, "@8 the sum was taken by the previous poll (mdx %d)", mdx);

    psp_ctrl_set_look(128, 128);
    unload();
}

/* What the recorder writes for the channel, and that it is exactly what the
 * parser above reads back. */
static void test_look_is_recorded(void) {
    unsetenv("PSPRECOMP_REPLAY");
    setenv("PSPRECOMP_REPLAY_REC", "t-look-rec.pad", 1);
    psp_misc_init();
    psp_ctrl_set_look(200, 60);  poll_pad(NULL, NULL);      /* 1: the stick */
    psp_ctrl_add_mouse(-40, 12); poll_pad(NULL, NULL);      /* 2: and a delta */
    poll_pad(NULL, NULL);                                    /* 3: delta spent */
    psp_ctrl_set_look(128, 128); poll_pad(NULL, NULL);      /* 4: released */
    poll_pad(NULL, NULL);                                    /* 5: nothing new */
    psp_ctrl_replay_finish(NULL);
    unsetenv("PSPRECOMP_REPLAY_REC");

    char text[2048] = {0};
    FILE *f = fopen("t-look-rec.pad", "r");
    if (f) { (void)fread(text, 1, sizeof text - 1, f); fclose(f); }
    CHECK(strstr(text, "@1      state none 128 128 200 60   #") != NULL,
          "the second stick is written as two trailing fields");
    CHECK(strstr(text, "@2      state none 128 128 200 60 -40 12   #") != NULL,
          "a mouse delta as two more");
    CHECK(strstr(text, "@3      state none 128 128 200 60   #") != NULL,
          "the delta is not carried into the next poll");
    CHECK(strstr(text, "@4      state none 128 128 128 128   #") != NULL,
          "the return to neutral is written, so a replay releases the channel");
    CHECK(strstr(text, "@5 ") == NULL, "and nothing is written when nothing changed");
    psp_misc_init();
}

/* A title can supply room for ten samples and read the latched look more
 * than once. Neither operation changes its poll identity or spends mouse
 * motion. Native gameplay owns that consumption, separately from transport. */
static void test_look_snapshot_and_clear(void) {
    unload();
    psp_ctrl_set(0x03fc0c00u, 170, 90);
    psp_ctrl_set_look(201, 61);
    psp_ctrl_add_mouse(17, -9);
    psp_cpu.r[PSP_REG_A0] = BUF;
    psp_cpu.r[PSP_REG_A1] = 10;
    psp_hle_call(CTRL_PEEK);
    CHECK(psp_ctrl_polls() == 1 && psp_cpu.r[PSP_REG_V0]==10,
          "ten samples of room are one poll");
    CHECK(psp_ctrl_pressed_buttons()==0x03fc0c00u, "snapshot retains the press edge");
    CHECK(psp_read32(BUF + 4) == 0x03fc0c00u, "physical carrier bits survive");
    psp_ctrl_set_look(30, 40); /* Live changes cannot alter a recorded snapshot. */
    psp_ctrl_add_mouse(100, 100);
    for (int i = 0; i < 3; i++) {
        uint8_t rx, ry; int dx, dy;
        psp_ctrl_last_look(&rx, &ry, &dx, &dy);
        CHECK(rx == 201 && ry == 61 && dx == 17 && dy == -9,
              "look snapshot remains identical on reread %d", i);
    }
    psp_ctrl_clear_mouse(); /* Focus/modal reset drops only unpolled motion. */
    poll_pad(NULL, NULL);
    uint8_t rx, ry; int dx, dy;
    psp_ctrl_last_look(&rx, &ry, &dx, &dy);
    CHECK(rx == 30 && ry == 40 && dx == 0 && dy == 0,
          "next poll sees new stick and no cleared mouse backlog");
    unload();
}

static void test_complete_input_round_trip(void) {
    uint32_t buttons[5] = {0x00400400u, 0x00800800u, 0x03000000u, 0x03fc0c00u, 0};
    uint8_t axes[5][4] = {{160,128,128,192}, {128,64,255,0}, {0,255,64,128},
                         {192,192,128,128}, {128,128,128,128}};
    int mouse[5][2] = {{7,-3}, {-12,9}, {0,0}, {100,-50}, {0,0}};
    unload();
    setenv("PSPRECOMP_REPLAY_REC", "t-complete-rec.pad", 1);
    psp_misc_init();
    for (int i = 0; i < 5; i++) {
        psp_ctrl_set(buttons[i], axes[i][0], axes[i][1]);
        psp_ctrl_set_look(axes[i][2], axes[i][3]);
        psp_ctrl_add_mouse(mouse[i][0], mouse[i][1]);
        poll_pad(NULL, NULL);
        CHECK(psp_ctrl_pressed_buttons()==(buttons[i] & ~(i ? buttons[i-1] : 0)),
              "live merged button edges at poll %d", i+1);
    }
    psp_ctrl_replay_finish(NULL);
    unsetenv("PSPRECOMP_REPLAY_REC");
    load("t-complete-rec.pad");
    for (int i = 0; i < 5; i++) {
        uint8_t ax, ay, rx, ry; int dx, dy;
        uint32_t b = poll_pad(&ax, &ay);
        CHECK(psp_ctrl_pressed_buttons()==(buttons[i] & ~(i ? buttons[i-1] : 0)),
              "replayed button edges at poll %d", i+1);
        psp_ctrl_last_look(&rx, &ry, &dx, &dy);
        CHECK(b == buttons[i] && ax == axes[i][0] && ay == axes[i][1] &&
              rx == axes[i][2] && ry == axes[i][3] && dx == mouse[i][0] && dy == mouse[i][1],
              "complete physical input record/replay at poll %d", i + 1);
    }
    unload();
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
    test_look_channel();
    test_look_is_recorded();
    test_look_snapshot_and_clear();
    test_complete_input_round_trip();

    remove("t-directives.pad"); remove("t-bad.pad"); remove("t-tap.pad");
    remove("t-catchup.pad"); remove("t-lanes.pad"); remove("t-host.pad");
    remove("t-rt-src.pad"); remove("t-rt-rec.pad");
    remove("t-look.pad"); remove("t-look-rec.pad");
    remove("t-complete-rec.pad");
    psp_mem_free();

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("all scripted-input checks passed\n");
    return 0;
}
