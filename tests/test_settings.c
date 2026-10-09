/* The player settings mechanism, against a pack schema written here: the
 * player's table, types, aliases, labels, the renderer rule, a pack's resolve
 * and print hooks, play defaults and reset, precedence, and the preferences
 * file -- its sections, a version 1 file's presets, adopting an earlier app's
 * file and the failure modes. The packs' own option tables are tested in
 * their repositories. */
#include <psprecomp/host/settings.h>

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { T_INPUT = PSP_PLAYER_OPTIONS, T_MOUSE, T_DEADZONE, T_LAG, T_COUNT };
enum { T_FREE_LOOK };

#define C(k,d_,ch_,lb_,...) {.key=#k,.env="PSPRECOMP_" #k,.label=#k,.page="Page",.help="Help", \
    .type=PSP_OPTION_CHOICE,.dflt=d_,.choices=ch_,.labels=lb_, __VA_ARGS__}
#define N(k,type_,d_,min_,max_,...) {.key=#k,.env="PSPRECOMP_" #k,.label=#k,.page="Page",.help="Help", \
    .type=type_,.dflt=d_,.min=min_,.max=max_,.step=0.01, __VA_ARGS__}
static const psp_option_def options[] = {
    C(INPUT,"classic","classic|modern","Classic|Modern",.aliases="dual=modern"),
    C(MOUSE,"0","0|1","Off|On"),
    N(DEADZONE,PSP_OPTION_NUMBER,"0.10",0,0.5,.format="%.0f%%",.scale=100),
    N(LAG,PSP_OPTION_NUMBER,"game",0,0.99,.special="game",.special_label="Off",
      .zero_label="Off",.format="%.2f"),
};
#undef C
#undef N

static int resolve(psp_settings *s, char *error) {
    s->input = (int)s->number[T_INPUT];
    s->gamepad = s->input;
    s->mouse = s->input && s->number[T_MOUSE] != 0;
    s->title[T_FREE_LOOK] = s->input;
    if (s->number[T_MOUSE] && s->number[T_DEADZONE] > 0.4) {
        snprintf(error, PSP_SETTINGS_ERROR, "DEADZONE: too wide for mouse play");
        return -1;
    }
    return 0;
}
static void print_effective(const psp_settings *s, FILE *out) {
    fprintf(out, " free_look=%d", s->title[T_FREE_LOOK]);
}
static void print_notes(const psp_settings *s, FILE *out) {
    if (s->number[T_MOUSE] && !s->mouse) fputs("note: mouse needs Modern\n", out);
}
const psp_settings_schema psp_title_settings = {
    .title = "Test", .id = "test", .options = options, .count = T_COUNT - PSP_PLAYER_OPTIONS,
    .play_defaults = "INPUT=modern",
    .retired_keys = "OLD_PREVIEW|OLD_AIM",
    .gl_error = "Match window requires OpenGL",
    .resolve = resolve, .print_effective = print_effective, .print_notes = print_notes,
};
/* A second pack, for a file that serves two. */
static const psp_option_def other_options[] = {
    {.key = "SPEED", .env = "PSPRECOMP_SPEED", .label = "Speed", .page = "Page", .help = "Help",
     .type = PSP_OPTION_INTEGER, .dflt = "1", .min = 1, .max = 4, .step = 1},
};
static const psp_settings_schema other = {
    .title = "Other", .id = "other", .options = other_options, .count = 1,
};
static const psp_settings_schema player_only = { .title = "psprecomp" };

static char error[PSP_SETTINGS_ERROR];
static void putfile(const char *path, const char *text) {
    FILE *f = fopen(path, "w"); assert(f); assert(fputs(text, f) >= 0); assert(!fclose(f));
}
static int contains(const char *path, const char *text) {
    FILE *f = fopen(path, "r"); assert(f);
    static char buf[8192]; size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
    return strstr(buf, text) != NULL;
}

int main(void) {
    /* Nothing installed: the title's own schema, after the player's table. */
    assert(psp_settings_active_schema() == &psp_title_settings);
    assert(psp_settings_count() == T_COUNT);
    assert(psp_settings_find("LAG") == T_LAG && psp_settings_find("NOPE") == -1);
    assert(psp_settings_find("RENDER") == PSP_OPT_RENDER && psp_settings_option(PSP_OPT_VOLUME) == &psp_player_options[PSP_OPT_VOLUME]);
    assert(psp_settings_option(T_INPUT) == &options[0] && !psp_settings_option(T_COUNT));
    for (int i = 0; i < T_COUNT; i++) unsetenv(psp_settings_option(i)->env);

    /* Defaults and the shared derived values. */
    psp_settings s; assert(!psp_settings_load(&s, NULL, error));
    assert(s.render == 1 && !s.window && !s.realtime && !s.gamepad);
    assert(s.width == 960 && s.height == 544 && s.number[T_LAG] == -1);
    assert(s.number[PSP_OPT_DISPLAY] == -1 && !strcmp(s.value[PSP_OPT_DISPLAY], "primary"));
    assert(!psp_settings_origin());

    /* Types: choices with booleans and aliases, sentinels, integers, sizes. */
    assert(!psp_settings_set(&s, T_MOUSE, "on", PSP_SOURCE_FILE, error) && s.number[T_MOUSE] == 1);
    assert(!psp_settings_set(&s, T_MOUSE, "false", PSP_SOURCE_FILE, error) && s.number[T_MOUSE] == 0);
    assert(!psp_settings_set(&s, T_INPUT, "dual", PSP_SOURCE_FILE, error));
    assert(s.number[T_INPUT] == 1 && !strcmp(s.value[T_INPUT], "modern"));
    assert(psp_settings_set(&s, T_INPUT, "dualx", PSP_SOURCE_FILE, error));
    assert(psp_settings_set(&s, T_INPUT, "", PSP_SOURCE_FILE, error));
    assert(psp_settings_set(&s, PSP_OPT_DISPLAY, "1.5", PSP_SOURCE_FILE, error));
    assert(psp_settings_set(&s, PSP_OPT_DISPLAY, "0", PSP_SOURCE_FILE, error));
    assert(!psp_settings_set(&s, PSP_OPT_DISPLAY, "2", PSP_SOURCE_FILE, error));
    const char *bad[] = {"nan", "inf", "-inf", "1junk", "0.1 ", "-0.01", "0.51", "1e999"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        assert(psp_settings_set(&s, T_DEADZONE, bad[i], PSP_SOURCE_FILE, error));
    assert(psp_settings_set(&s, PSP_OPT_WINDOW_SIZE, "1920x1080oops", PSP_SOURCE_FILE, error));
    assert(psp_settings_set(&s, PSP_OPT_WINDOW_SIZE, "0x1080", PSP_SOURCE_FILE, error));
    assert(!psp_settings_set(&s, PSP_OPT_WINDOW_SIZE, "1280x720", PSP_SOURCE_FILE, error));
    assert(s.width == 1280 && s.height == 720);
    assert(psp_settings_set(&s, T_COUNT, "1", PSP_SOURCE_FILE, error));

    /* Labels. */
    char label[64];
    psp_option_label(&s, PSP_OPT_DISPLAY, label, sizeof label); assert(!strcmp(label, "Display 2"));
    assert(!psp_settings_set(&s, PSP_OPT_DISPLAY, "primary", PSP_SOURCE_FILE, error));
    psp_option_label(&s, PSP_OPT_DISPLAY, label, sizeof label); assert(!strcmp(label, "Primary display"));
    psp_option_label(&s, T_DEADZONE, label, sizeof label); assert(!strcmp(label, "10%"));
    psp_option_label(&s, T_LAG, label, sizeof label); assert(!strcmp(label, "Off"));
    assert(!psp_settings_set(&s, T_LAG, "0", PSP_SOURCE_FILE, error));
    psp_option_label(&s, T_LAG, label, sizeof label); assert(!strcmp(label, "Off"));
    assert(!psp_settings_set(&s, T_LAG, "0.5", PSP_SOURCE_FILE, error));
    psp_option_label(&s, T_LAG, label, sizeof label); assert(!strcmp(label, "0.50"));
    psp_option_label(&s, T_INPUT, label, sizeof label); assert(!strcmp(label, "Modern"));
    psp_option_label(&s, PSP_OPT_WINDOW_SIZE, label, sizeof label); assert(!strcmp(label, "1280x720"));
    psp_option_label(&s, PSP_OPT_AUDIO_LEAD_MS, label, sizeof label); assert(!strcmp(label, "Auto"));

    /* The renderer rule, then the title's resolve. */
    assert(!psp_settings_set(&s, PSP_OPT_RESOLUTION, "window", PSP_SOURCE_FILE, error));
    assert(!psp_settings_resolve(&s, error));
    assert(s.render == 2 && s.window && s.realtime && s.gamepad && s.title[T_FREE_LOOK]);
    assert(!psp_settings_set(&s, PSP_OPT_RENDER, "software", PSP_SOURCE_FILE, error));
    assert(psp_settings_resolve(&s, error) && strstr(error, "Match window requires OpenGL"));
    assert(!psp_settings_set(&s, PSP_OPT_RENDER, "auto", PSP_SOURCE_FILE, error));
    assert(!psp_settings_set(&s, T_MOUSE, "1", PSP_SOURCE_FILE, error));
    assert(!psp_settings_set(&s, T_DEADZONE, "0.45", PSP_SOURCE_FILE, error));
    assert(psp_settings_resolve(&s, error) && strstr(error, "too wide"));
    assert(!psp_settings_set(&s, T_DEADZONE, "0.1", PSP_SOURCE_FILE, error));
    assert(!psp_settings_set(&s, PSP_OPT_WINDOW_MODE, "borderless", PSP_SOURCE_FILE, error));
    assert(!psp_settings_set(&s, PSP_OPT_RESOLUTION, "psp", PSP_SOURCE_FILE, error));
    assert(!psp_settings_set(&s, PSP_OPT_RENDER, "software", PSP_SOURCE_FILE, error));
    assert(!psp_settings_resolve(&s, error) && s.render == 1 && s.window && s.realtime);

    /* Assignments are all or nothing. */
    psp_settings before = s;
    assert(psp_settings_assign(&s, "WINDOW=1 NOPE=2", PSP_SOURCE_FILE, error));
    assert(psp_settings_assign(&s, "WINDOW=1 INPUT=typo", PSP_SOURCE_FILE, error));
    assert(psp_settings_assign(&s, "WINDOW", PSP_SOURCE_FILE, error));
    assert(!memcmp(&before, &s, sizeof s));
    assert(!psp_settings_assign(&s, "  WINDOW=0   INPUT=classic ", PSP_SOURCE_FILE, error));
    assert(!s.number[PSP_OPT_WINDOW] && !s.number[T_INPUT]);

    /* print: options, the effective line with the title's part, its notes. */
    char dir[] = "/tmp/psp-settings-test-XXXXXX"; assert(mkdtemp(dir));
    char path[256], out[256], older[256];
    snprintf(path, sizeof path, "%s/settings.ini", dir);
    snprintf(out, sizeof out, "%s/print.txt", dir);
    snprintf(older, sizeof older, "%s/older.ini", dir);
    assert(!psp_settings_set(&s, T_MOUSE, "1", PSP_SOURCE_FILE, error));
    assert(!psp_settings_resolve(&s, error));
    FILE *pf = fopen(out, "w"); assert(pf); psp_settings_print(&s, pf); fclose(pf);
    assert(contains(out, "effective: renderer=software gamepad=classic window=1 realtime=1 free_look=0\n"));
    assert(contains(out, "note: mouse needs Modern\n"));
    assert(contains(out, "LAG                      = 0.5          [file]\n"));

    /* Play defaults over both tables, and a reset of one table. */
    psp_settings play; psp_settings_play_defaults(&play);
    assert(play.number[PSP_OPT_WINDOW] == 1 && play.number[PSP_OPT_MPEG_DECODE] == 1 && play.render == 2);
    assert(play.number[T_INPUT] == 1 && play.source[T_INPUT] == PSP_SOURCE_FILE);
    assert(play.source[T_MOUSE] == PSP_SOURCE_DEFAULT);
    psp_settings mixed = s;
    assert(!psp_settings_bind(&mixed, "key.cross", "Z", error));
    psp_settings_reset(&mixed, T_LAG);                  /* the pack's table */
    assert(mixed.number[T_INPUT] == 1 && mixed.number[T_MOUSE] == 0 && mixed.number[T_LAG] == -1);
    assert(mixed.number[PSP_OPT_WINDOW_MODE] == 1 && mixed.bind_count == 1);
    psp_settings_reset(&mixed, PSP_OPT_VOLUME);         /* the player's */
    assert(mixed.number[PSP_OPT_WINDOW_MODE] == 0 && mixed.number[PSP_OPT_WINDOW] == 1);
    assert(mixed.width == 960 && mixed.render == 2 && mixed.number[T_INPUT] == 1 && mixed.bind_count == 1);

    /* A file: [player] and [pack test], a round trip, its header. */
    psp_settings_file *f = psp_settings_file_new(); assert(f);
    assert(!psp_settings_file_has(f, 0) && !psp_settings_file_has(f, 1));
    assert(!psp_settings_set(&play, PSP_OPT_DISPLAY, "2", PSP_SOURCE_FILE, error));
    assert(!psp_settings_file_put(f, &play, error));
    assert(psp_settings_file_has(f, 0) && psp_settings_file_has(f, 1));
    assert(!psp_settings_file_set_game(f, "slug", error) && psp_settings_file_set_game(f, "bad]", error));
    assert(!psp_settings_file_write(f, path, error));
    psp_settings_file_free(f);
    assert(contains(path, "# psprecomp player settings. Environment overrides are never saved.\n"));
    assert(contains(path, "version=2\ngame=slug\n\n[player]\nRESOLUTION=window\nWINDOW_MODE=windowed\n"));
    assert(contains(path, "REALTIME=0\n\n[pack test]\nINPUT=modern\nMOUSE=0\nDEADZONE=0.1\nLAG=game\n"));
    f = psp_settings_file_read(path, error); assert(f);
    assert(!strcmp(psp_settings_file_game(f), "slug"));
    psp_settings got; assert(!psp_settings_file_get(f, &got, error));
    for (int k = 0; k < T_COUNT; k++) assert(!strcmp(got.value[k], play.value[k]));
    assert(got.source[T_MOUSE] == PSP_SOURCE_FILE && got.number[PSP_OPT_DISPLAY] == 2);

    /* Bindings ride in the pack's section as written, whatever the host makes
     * of them, and an empty one survives as an unbinding. Only their shape is
     * checked. */
    psp_settings *b = &got;
    assert(!psp_settings_bind(b, "key.cross", "Z, Space", error));
    assert(!psp_settings_bind(b, "pad.fire", "righttrigger", error));
    assert(!psp_settings_bind(b, "mouse.square", "", error));
    assert(!psp_settings_bind(b, "key.cross", "F", error) && b->bind_count == 3);   /* replaced */
    assert(psp_settings_bind(b, "joystick.cross", "a", error));
    assert(psp_settings_bind(b, "key.", "a", error));
    assert(psp_settings_bind(b, "key.cross", "Z\t", error));
    assert(psp_settings_bind(b, "key.cross", "Z ", error));
    assert(!psp_settings_bind(b, "pad.fire", NULL, error) && b->bind_count == 2);
    assert(!psp_settings_bind(b, "pad.fire", "righttrigger", error));
    assert(!psp_settings_file_put(f, b, error) && !psp_settings_file_write(f, path, error));
    psp_settings_file_free(f);
    assert(contains(path, "LAG=game\nbind.key.cross=F\nbind.mouse.square=\nbind.pad.fire=righttrigger\n"));
    assert(!psp_settings_load(&s, path, error) && s.bind_count == 3);
    assert(!strcmp(psp_settings_origin(), path));
    assert(!strcmp(psp_settings_binding(&s, "key.cross"), "F"));
    assert(!strcmp(psp_settings_binding(&s, "mouse.square"), ""));
    assert(!psp_settings_binding(&s, "pad.cross"));
    pf = fopen(out, "w"); assert(pf); psp_settings_print(&s, pf); fclose(pf);
    assert(contains(out, "bind.key.cross           = F            [file]\n"));

    /* Precedence: environment over the file, never saved back; a value the
     * command line set keeps the file's own too. */
    setenv("PSPRECOMP_DISPLAY", "primary", 1);
    assert(!psp_settings_load(&s, path, error));
    assert(s.number[PSP_OPT_DISPLAY] == -1 && s.source[PSP_OPT_DISPLAY] == PSP_SOURCE_ENV);
    assert(!psp_settings_set(&s, PSP_OPT_WINDOW, "0", PSP_SOURCE_COMMAND_LINE, error));
    assert(!psp_settings_set(&s, PSP_OPT_VOLUME, "40", PSP_SOURCE_FILE, error));
    assert(!psp_settings_bind(&s, "pad.fire", NULL, error));
    assert(!psp_settings_save_origin(&s, error));
    assert(contains(path, "DISPLAY=2\nVOLUME=40\n") && contains(path, "MPEG_DECODE=1\nWINDOW=1\n"));
    assert(!contains(path, "bind.pad.fire"));
    unsetenv("PSPRECOMP_DISPLAY");
    assert(!psp_settings_load(&s, path, error));
    assert(s.number[PSP_OPT_DISPLAY] == 2 && s.source[PSP_OPT_DISPLAY] == PSP_SOURCE_FILE);
    setenv("PSPRECOMP_INPUT", "", 1);
    assert(!psp_settings_load(&s, NULL, error) && s.source[T_INPUT] == PSP_SOURCE_DEFAULT);
    setenv("PSPRECOMP_INPUT", "invalid", 1);
    before = s; assert(psp_settings_env(&s, error)); assert(!memcmp(&before, &s, sizeof s));
    unsetenv("PSPRECOMP_INPUT");

    /* Another pack's section, and the player's alone: each reads [player]
     * and its own, and writing leaves the rest as it was. */
    psp_settings_schema_use(&other);
    assert(psp_settings_count() == PSP_PLAYER_OPTIONS + 1);
    assert(!psp_settings_load(&s, path, error) && s.number[PSP_OPT_VOLUME] == 40 && !s.bind_count);
    assert(!psp_settings_set(&s, PSP_PLAYER_OPTIONS, "3", PSP_SOURCE_FILE, error));
    assert(!psp_settings_save_origin(&s, error));
    assert(contains(path, "[pack test]\nINPUT=modern\n") && contains(path, "bind.key.cross=F\n"));
    assert(contains(path, "[pack other]\nSPEED=3\n"));
    psp_settings_schema_use(&player_only);
    assert(psp_settings_count() == PSP_PLAYER_OPTIONS);
    assert(!psp_settings_load(&s, path, error) && s.number[PSP_OPT_VOLUME] == 40);
    psp_settings_schema_use(&psp_title_settings);

    /* A version 1 file: the selected preset's options split into [player]
     * and an unnamed [pack] the first pack claims; retired keys skipped; the
     * other presets kept as they were, written after. */
    putfile(path, "version=1\nselected=Mine\ngame=aclr\n"
                  "[preset Classic]\nINPUT=classic\nWINDOW=1\n"
                  "[preset Mine]\nOLD_PREVIEW=1\nINPUT=dual\nRESOLUTION=window\nbind.key.cross=Q\n");
    assert(!psp_settings_load(&s, path, error));
    assert(s.number[T_INPUT] == 1 && s.number[PSP_OPT_RESOLUTION] == 1 && s.bind_count == 1);
    assert(!psp_settings_save_origin(&s, error));
    assert(contains(path, "version=2\ngame=aclr\n"));
    assert(contains(path, "[pack test]\nINPUT=modern\nMOUSE=0\nDEADZONE=0.1\nLAG=game\nbind.key.cross=Q\n"));
    assert(contains(path, "\n[preset Classic]\nINPUT=classic\nWINDOW=1\n") && !contains(path, "[preset Mine]"));
    assert(!contains(path, "selected="));

    /* Adopting an earlier app's file: its [player] where there is none, its
     * pack where there is none, its presets under the pack's id. */
    putfile(older, "version=1\nselected=B\n[preset A]\nSPEED=2\n[preset B]\nSPEED=4\nVOLUME=10\n");
    psp_settings_file *old = psp_settings_file_read(older, error); assert(old);
    f = psp_settings_file_new(); assert(f);
    assert(!psp_settings_file_adopt(f, old, "other", error) && !psp_settings_file_adopt(f, old, "other", error));
    assert(psp_settings_file_adopt(f, old, "bad id", error));
    psp_settings_file *mine = psp_settings_file_read(path, error); assert(mine);
    assert(!psp_settings_file_adopt(f, mine, "test", error));
    assert(!psp_settings_file_write(f, older, error));
    psp_settings_file_free(old); psp_settings_file_free(mine); psp_settings_file_free(f);
    assert(contains(older, "game=aclr\n\n[player]\nVOLUME=10\n\n[pack other]\nSPEED=4\n\n[pack test]\n"));
    assert(contains(older, "[preset other/A]\nSPEED=2\n\n[preset test/Classic]\n"));
    f = psp_settings_file_read(older, error); assert(f);
    psp_settings_schema_use(&other);
    assert(!psp_settings_file_get(f, &s, error) && s.number[PSP_PLAYER_OPTIONS] == 4 && s.number[PSP_OPT_VOLUME] == 10);
    psp_settings_schema_use(&psp_title_settings);
    assert(!psp_settings_file_get(f, &s, error) && s.number[T_INPUT] == 1 && s.number[PSP_OPT_VOLUME] == 10);
    psp_settings_file_free(f);

    /* An unknown key, a key in the wrong section or a bad value refuses the
     * file and leaves what was loaded before untouched. */
    const char *invalid[] = {
        "version=3\n",
        "[player]\n",
        "version=2\nselected=A\n",
        "version=2\n[presets A]\n",
        "version=2\n[pack has space]\n",
        "version=1\nselected=Missing\n[preset A]\n",
        "version=1\n[preset A]\n",
        "version=2\n[player]\nVOLUME=1\nVOLUME=2\n",
        "version=2\n[player]\n[player]\n",
        "version=2\n[player]\nINPUT=modern\n",
        "version=2\n[pack test]\nRENDER=gl\n",
        "version=2\n[pack test]\nCAPTURE=oops\n",
        "version=2\n[pack test]\nDEADZONE=NaN\n",
        "version=2\n[pack test]\nbind.key.cross=Z\nbind.key.cross=X\n",
        "version=2\n[pack test]\nbind.keyboard.cross=Z\n",
        "version=2\n[player]\nnot a pair\n",
    };
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        putfile(path, invalid[i]);
        before = s;
        assert(psp_settings_load(&s, path, error));
        assert(!memcmp(&before, &s, sizeof s));
    }
    /* Another pack's keys are its own business, and a preset is never read. */
    putfile(path, "version=2\n[pack else]\nWHATEVER=1\n[preset X]\nNOPE=2\n");
    assert(!psp_settings_load(&s, path, error));
    f = psp_settings_file_new(); assert(f);
    assert(psp_settings_file_write(f, "/no-such-directory/settings.ini", error));
    psp_settings_file_free(f);

    /* The installed snapshot. */
    assert(!psp_settings_load(&s, NULL, error));
    assert(!psp_settings_set(&s, T_INPUT, "modern", PSP_SOURCE_COMMAND_LINE, error));
    psp_settings_use(&s);
    assert(psp_settings_current()->number[T_INPUT] == 1);

    unlink(out); unlink(path); unlink(older); rmdir(dir);
    puts("settings: player table, types, labels, renderer rule, pack hooks, play defaults, precedence, sections, presets and adoption passed");
    return 0;
}
