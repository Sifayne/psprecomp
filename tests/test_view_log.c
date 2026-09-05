/* PSPRECOMP_VIEW_LOG — the camera observable, using synthetic GE lists only.
 *
 * The log is an instrument for measuring what the guest's control code does
 * with a given input, so the properties that matter are not "it prints
 * something" but: it stamps each line with the pad-poll count a scenario is
 * keyed on, it emits a line when a matrix *changes*, and it emits nothing
 * when the camera holds still -- including the case a real frame produces,
 * where the scene's matrix and the HUD's 2D matrix alternate every frame
 * while the camera itself has not moved. And because this game carries its
 * camera in the world matrices rather than the view matrix, it records the
 * k-th world upload of each frame too, and only that one.
 */

#include "psprecomp/hle.h"
#include "psprecomp/mem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIST 0x08810000u
#define PAD  0x08820000u

#define GE_WORLDMATRIXNUMBER 0x3Au
#define GE_WORLDMATRIXDATA   0x3Bu
#define GE_VIEWMATRIXNUMBER  0x3Cu
#define GE_VIEWMATRIXDATA    0x3Du

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

/* A GE command word: command in the top byte, 24-bit argument below. Matrix
 * data carries the top 24 bits of an IEEE float, which is what ge_float
 * reconstructs by shifting back up. */
static uint32_t cmd(uint32_t op, uint32_t arg) {
    return (op << 24) | (arg & 0x00FFFFFFu);
}

static uint32_t as_matrix_word(float f) {
    union { uint32_t u; float f; } c;
    c.f = f;
    return c.u >> 8;
}

static unsigned g_w;   /* list cursor, in words */

static void put_matrix(uint32_t number_op, uint32_t data_op, const float m[12]) {
    psp_write32(LIST + 4 * g_w++, cmd(number_op, 0));
    for (int i = 0; i < 12; i++)
        psp_write32(LIST + 4 * g_w++, cmd(data_op, as_matrix_word(m[i])));
}

/* Finish the list under construction and run it: one frame. */
static void run_list(void) {
    psp_write32(LIST + 4 * g_w++, 0x0F000000);   /* FINISH */
    psp_write32(LIST + 4 * g_w++, 0x0C000000);   /* END */
    g_w = 0;

    psp_cpu.r[PSP_REG_A0] = LIST;
    psp_cpu.r[PSP_REG_A1] = 0;
    psp_cpu.r[PSP_REG_A2] = 0;
    psp_cpu.r[PSP_REG_A3] = 0;
    psp_hle_call(0xAB49E76A);                  /* sceGeListEnQueue */
    psp_ge_drain_all();
}

/* A matrix whose third basis column points somewhere distinctive, so a change
 * is unambiguous. */
static void basis(float m[12], float third_x, float third_z) {
    const float t[12] = {
        1.0f, 0.0f, 0.0f,          /* basis column 0 */
        0.0f, 1.0f, 0.0f,          /* basis column 1 */
        third_x, 0.0f, third_z,    /* basis column 2 -- the one yaw derives from */
        0.0f, 0.0f, 0.0f,          /* translation */
    };
    memcpy(m, t, sizeof t);
}

static void submit_view(float third_x, float third_z) {
    float m[12];
    basis(m, third_x, third_z);
    put_matrix(GE_VIEWMATRIXNUMBER, GE_VIEWMATRIXDATA, m);
    run_list();
}

/* The 2D flip this game's HUD sets every frame, after the scene. */
static void submit_hud(void) {
    const float m[12] = {
        1.0f, 0.0f, 0.0f,
        0.0f, -1.0f, 0.0f,
        0.0f, 0.0f, -1.0f,
        0.0f, 0.0f, 0.0f,
    };
    put_matrix(GE_VIEWMATRIXNUMBER, GE_VIEWMATRIXDATA, m);
    run_list();
}

/* Two world uploads in one frame, the way a scene with two objects does it.
 * Only the first is the k-th (k = 1); the second must never be logged. */
static void submit_world_pair(float first_x, float first_z) {
    float a[12], b[12];
    basis(a, first_x, first_z);
    basis(b, 0.5f, 0.5f);                      /* a second object, never logged */
    put_matrix(GE_WORLDMATRIXNUMBER, GE_WORLDMATRIXDATA, a);
    put_matrix(GE_WORLDMATRIXNUMBER, GE_WORLDMATRIXDATA, b);
    run_list();
}

/* Advance the pad-poll counter, which is what the log stamps with. */
static void poll_pad(void) {
    psp_cpu.r[PSP_REG_A0] = PAD;
    psp_cpu.r[PSP_REG_A1] = 1;
    psp_hle_call(0x3A622550);                  /* sceCtrlPeekBufferPositive */
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "view-log.txt";
    env("PSPRECOMP_VIEW_LOG", path);

    if (psp_mem_init()) return 1;
    psp_hle_init();
    psp_ge_init();

    /* Three distinct orientations, each held for two frames. The repeats are
     * the point: they must not produce a second line. */
    poll_pad();  submit_view(0.0f, 1.0f);      /* poll 1: logged */
    poll_pad();  submit_view(0.0f, 1.0f);
    poll_pad();  submit_view(1.0f, 0.0f);      /* poll 3: logged */
    poll_pad();  submit_view(1.0f, 0.0f);
    poll_pad();  submit_view(0.0f, -1.0f);     /* poll 5: logged */

    /* Then the pattern a real frame produces: scene, HUD, scene, HUD. The
     * camera has not moved, so only the HUD's first appearance is new. This
     * is what a last-matrix-only dedupe got wrong, logging twice a frame. */
    poll_pad();  submit_hud();                 /* poll 6: logged, first sight */
    poll_pad();  submit_view(0.0f, -1.0f);     /* two uploads ago: silent */
    poll_pad();  submit_hud();                 /* silent */
    poll_pad();  submit_view(-1.0f, 0.0f);     /* poll 9: the camera moved */

    /* World matrices: two objects a frame, and only the first is recorded.
     * A still camera is silent; a turn is one line. */
    poll_pad();  submit_world_pair(0.0f, 1.0f);   /* poll 10: W logged */
    poll_pad();  submit_world_pair(1.0f, 0.0f);   /* poll 11: W logged, it turned */
    poll_pad();  submit_world_pair(1.0f, 0.0f);   /* silent */

    FILE *f = fopen(path, "r");
    CHECK(f != NULL, "the log was created at %s", path);
    if (!f) { psp_mem_free(); return 1; }

    char line[1024];
    char tags[16];
    unsigned polls[16];
    double yaws[16];
    int n = 0;
    while (fgets(line, sizeof line, f) && n < 16) {
        if (line[0] == '#') continue;           /* the header comment */
        char tag;
        unsigned poll;
        float v[12];
        double yaw, pitch;
        const int got = sscanf(line,
            " %c %u %f %f %f %f %f %f %f %f %f %f %f %f %lf %lf",
            &tag, &poll, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5],
            &v[6], &v[7], &v[8], &v[9], &v[10], &v[11], &yaw, &pitch);
        CHECK(got == 16, "line %d has a tag, a poll, twelve floats and both "
              "angles, got %d", n, got);
        tags[n] = tag;
        polls[n] = poll;
        yaws[n] = yaw;
        n++;
    }
    fclose(f);
    remove(path);                               /* synthetic test output only */

    /* Seven new matrices out of twelve frames. */
    CHECK(n == 7, "one line per new matrix, expected 7 got %d", n);

    if (n == 7) {
        /* Stamps are the poll count at the moment of the upload, and they
         * must advance -- that is what lines a measurement up with the input
         * that caused it. */
        const unsigned want[7] = { 1, 3, 5, 6, 9, 10, 11 };
        const char     tag[7]  = { 'V', 'V', 'V', 'V', 'V', 'W', 'W' };
        for (int i = 0; i < 7; i++) {
            CHECK(polls[i] == want[i], "line %d stamped at poll %u, got %u",
                  i, want[i], polls[i]);
            CHECK(tags[i] == tag[i], "line %d tagged %c, got %c",
                  i, tag[i], tags[i]);
        }

        /* Forward +Z, +X, -Z, then -X: 0, 90, 180 and -90 degrees under the
         * convention the log documents. Loose tolerance -- the matrix words
         * carry only the top 24 bits of each float. */
        CHECK(yaws[0] > -1.0 && yaws[0] < 1.0, "yaw for +Z is ~0, got %.3f", yaws[0]);
        CHECK(yaws[1] > 89.0 && yaws[1] < 91.0, "yaw for +X is ~90, got %.3f", yaws[1]);
        CHECK(yaws[2] > 179.0 || yaws[2] < -179.0,
              "yaw for -Z is ~180, got %.3f", yaws[2]);
        CHECK(yaws[4] > -91.0 && yaws[4] < -89.0,
              "yaw for -X is ~-90, got %.3f", yaws[4]);
        CHECK(yaws[5] > -1.0 && yaws[5] < 1.0, "world +Z is ~0, got %.3f", yaws[5]);
        CHECK(yaws[6] > 89.0 && yaws[6] < 91.0, "world +X is ~90, got %.3f", yaws[6]);
    }

    psp_mem_free();

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("view log checks passed (synthetic GE lists, no game data)\n");
    return 0;
}
