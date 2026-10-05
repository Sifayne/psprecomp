/* The player settings mechanism, against a schema written here: types,
 * aliases, labels, the renderer rule, a title's resolve and print hooks,
 * starter presets, precedence and the preferences file's failure modes. The
 * titles' own option tables are tested in their repositories. */
#include <psprecomp/host/settings.h>

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { T_RESOLUTION, T_WINDOW_SIZE, T_WINDOW_MODE, T_DISPLAY, T_INPUT, T_MOUSE,
       T_DEADZONE, T_LAG, T_RENDER, T_WINDOW, T_REALTIME, T_COUNT };
enum { T_FREE_LOOK };

#define C(k,d_,ch_,lb_,...) {.key=#k,.env="PSPRECOMP_" #k,.label=#k,.page="Page",.help="Help", \
    .type=PSP_OPTION_CHOICE,.dflt=d_,.choices=ch_,.labels=lb_, __VA_ARGS__}
#define N(k,type_,d_,min_,max_,...) {.key=#k,.env="PSPRECOMP_" #k,.label=#k,.page="Page",.help="Help", \
    .type=type_,.dflt=d_,.min=min_,.max=max_,.step=0.01, __VA_ARGS__}
static const psp_option_def options[T_COUNT] = {
    C(RESOLUTION,"psp","psp|window","Original|Match window",.flags=PSP_OPTION_NEEDS_GL),
    N(WINDOW_SIZE,PSP_OPTION_SIZE,"960x544",1,16384),
    C(WINDOW_MODE,"windowed","windowed|borderless","Windowed|Borderless"),
    N(DISPLAY,PSP_OPTION_INTEGER,"primary",1,65535,.special="primary",
      .special_label="Primary display",.format="Display %.0f"),
    C(INPUT,"classic","classic|modern","Classic|Modern",.aliases="dual=modern"),
    C(MOUSE,"0","0|1","Off|On"),
    N(DEADZONE,PSP_OPTION_NUMBER,"0.10",0,0.5,.format="%.0f%%",.scale=100),
    N(LAG,PSP_OPTION_NUMBER,"game",0,0.99,.special="game",.special_label="Off",
      .zero_label="Off",.format="%.2f"),
    C(RENDER,"auto","auto|software|gl|null","Automatic|Software|OpenGL|Null"),
    C(WINDOW,"0","0|1","Off|On"),
    C(REALTIME,"0","0|1","Off|On"),
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
static const psp_preset_def presets[] = {
    {"Classic", "WINDOW=1 RENDER=gl"},
    {"Modern", "WINDOW=1 RENDER=gl INPUT=modern RESOLUTION=window"},
};
const psp_settings_schema psp_title_settings = {
    .title = "Test", .options = options, .count = T_COUNT,
    .presets = presets, .preset_count = 2, .preset_selected = 1,
    .retired_keys = "OLD_PREVIEW|OLD_AIM",
    .gl_error = "Match window requires OpenGL",
    .resolve = resolve, .print_effective = print_effective, .print_notes = print_notes,
};

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
    /* Nothing installed: the title's own schema. */
    assert(psp_settings_active_schema() == &psp_title_settings);
    psp_settings_schema_use(&psp_title_settings);
    assert(psp_settings_find("LAG") == T_LAG && psp_settings_find("NOPE") == -1);
    for (int i = 0; i < T_COUNT; i++) unsetenv(options[i].env);

    /* Defaults and the shared derived values. */
    psp_settings s; assert(!psp_settings_load(&s, NULL, NULL, error));
    assert(s.render == 1 && !s.window && !s.realtime && !s.gamepad);
    assert(s.width == 960 && s.height == 544 && s.number[T_LAG] == -1);
    assert(s.number[T_DISPLAY] == -1 && !strcmp(s.value[T_DISPLAY], "primary"));

    /* Types: choices with booleans and aliases, sentinels, integers, sizes. */
    assert(!psp_settings_set(&s, T_MOUSE, "on", PSP_SOURCE_PRESET, error) && s.number[T_MOUSE] == 1);
    assert(!psp_settings_set(&s, T_MOUSE, "false", PSP_SOURCE_PRESET, error) && s.number[T_MOUSE] == 0);
    assert(!psp_settings_set(&s, T_INPUT, "dual", PSP_SOURCE_PRESET, error));
    assert(s.number[T_INPUT] == 1 && !strcmp(s.value[T_INPUT], "modern"));
    assert(psp_settings_set(&s, T_INPUT, "dualx", PSP_SOURCE_PRESET, error));
    assert(psp_settings_set(&s, T_INPUT, "", PSP_SOURCE_PRESET, error));
    assert(psp_settings_set(&s, T_DISPLAY, "1.5", PSP_SOURCE_PRESET, error));
    assert(psp_settings_set(&s, T_DISPLAY, "0", PSP_SOURCE_PRESET, error));
    assert(!psp_settings_set(&s, T_DISPLAY, "2", PSP_SOURCE_PRESET, error));
    const char *bad[] = {"nan", "inf", "-inf", "1junk", "0.1 ", "-0.01", "0.51", "1e999"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        assert(psp_settings_set(&s, T_DEADZONE, bad[i], PSP_SOURCE_PRESET, error));
    assert(psp_settings_set(&s, T_WINDOW_SIZE, "1920x1080oops", PSP_SOURCE_PRESET, error));
    assert(psp_settings_set(&s, T_WINDOW_SIZE, "0x1080", PSP_SOURCE_PRESET, error));
    assert(!psp_settings_set(&s, T_WINDOW_SIZE, "1280x720", PSP_SOURCE_PRESET, error));
    assert(s.width == 1280 && s.height == 720);

    /* Labels. */
    char label[64];
    psp_option_label(&s, T_DISPLAY, label, sizeof label); assert(!strcmp(label, "Display 2"));
    assert(!psp_settings_set(&s, T_DISPLAY, "primary", PSP_SOURCE_PRESET, error));
    psp_option_label(&s, T_DISPLAY, label, sizeof label); assert(!strcmp(label, "Primary display"));
    psp_option_label(&s, T_DEADZONE, label, sizeof label); assert(!strcmp(label, "10%"));
    psp_option_label(&s, T_LAG, label, sizeof label); assert(!strcmp(label, "Off"));
    assert(!psp_settings_set(&s, T_LAG, "0", PSP_SOURCE_PRESET, error));
    psp_option_label(&s, T_LAG, label, sizeof label); assert(!strcmp(label, "Off"));
    assert(!psp_settings_set(&s, T_LAG, "0.5", PSP_SOURCE_PRESET, error));
    psp_option_label(&s, T_LAG, label, sizeof label); assert(!strcmp(label, "0.50"));
    psp_option_label(&s, T_INPUT, label, sizeof label); assert(!strcmp(label, "Modern"));
    psp_option_label(&s, T_WINDOW_SIZE, label, sizeof label); assert(!strcmp(label, "1280x720"));

    /* The renderer rule, then the title's resolve. */
    assert(!psp_settings_set(&s, T_RESOLUTION, "window", PSP_SOURCE_PRESET, error));
    assert(!psp_settings_resolve(&s, error));
    assert(s.render == 2 && s.window && s.realtime && s.gamepad && s.title[T_FREE_LOOK]);
    assert(!psp_settings_set(&s, T_RENDER, "software", PSP_SOURCE_PRESET, error));
    assert(psp_settings_resolve(&s, error) && strstr(error, "Match window requires OpenGL"));
    assert(!psp_settings_set(&s, T_RENDER, "auto", PSP_SOURCE_PRESET, error));
    assert(!psp_settings_set(&s, T_MOUSE, "1", PSP_SOURCE_PRESET, error));
    assert(!psp_settings_set(&s, T_DEADZONE, "0.45", PSP_SOURCE_PRESET, error));
    assert(psp_settings_resolve(&s, error) && strstr(error, "too wide"));
    assert(!psp_settings_set(&s, T_DEADZONE, "0.1", PSP_SOURCE_PRESET, error));
    assert(!psp_settings_set(&s, T_WINDOW_MODE, "borderless", PSP_SOURCE_PRESET, error));
    assert(!psp_settings_set(&s, T_RESOLUTION, "psp", PSP_SOURCE_PRESET, error));
    assert(!psp_settings_set(&s, T_RENDER, "software", PSP_SOURCE_PRESET, error));
    assert(!psp_settings_resolve(&s, error) && s.render == 1 && s.window && s.realtime);

    /* Assignments are all or nothing. */
    psp_settings before = s;
    assert(psp_settings_assign(&s, "WINDOW=1 NOPE=2", PSP_SOURCE_PRESET, error));
    assert(psp_settings_assign(&s, "WINDOW=1 INPUT=typo", PSP_SOURCE_PRESET, error));
    assert(psp_settings_assign(&s, "WINDOW", PSP_SOURCE_PRESET, error));
    assert(!memcmp(&before, &s, sizeof s));
    assert(!psp_settings_assign(&s, "  WINDOW=0   INPUT=classic ", PSP_SOURCE_PRESET, error));
    assert(!s.number[T_WINDOW] && !s.number[T_INPUT]);

    /* print: options, the effective line with the title's part, its notes. */
    char dir[] = "/tmp/psp-settings-test-XXXXXX"; assert(mkdtemp(dir));
    char path[256], out[256];
    snprintf(path, sizeof path, "%s/settings.ini", dir);
    snprintf(out, sizeof out, "%s/print.txt", dir);
    assert(!psp_settings_set(&s, T_MOUSE, "1", PSP_SOURCE_PRESET, error));
    assert(!psp_settings_resolve(&s, error));
    FILE *pf = fopen(out, "w"); assert(pf); psp_settings_print(&s, pf); fclose(pf);
    assert(contains(out, "effective: renderer=software gamepad=classic window=1 realtime=1 free_look=0\n"));
    assert(contains(out, "note: mouse needs Modern\n"));
    assert(contains(out, "LAG                      = 0.5          [preset]\n"));

    /* Starter presets, a round trip and the file's header. */
    psp_presets p, loaded; psp_presets_defaults(&p);
    assert(p.count == 2 && p.selected == 1);
    assert(p.presets[1].settings.number[T_INPUT] == 1 && p.presets[1].settings.render == 2);
    assert(p.presets[0].settings.source[T_WINDOW] == PSP_SOURCE_PRESET);
    assert(p.presets[0].settings.source[T_INPUT] == PSP_SOURCE_DEFAULT);
    strcpy(p.game, "slug");
    assert(!psp_settings_set(&p.presets[1].settings, T_DISPLAY, "2", PSP_SOURCE_PRESET, error));
    assert(!psp_presets_save(&p, path, error)); assert(!psp_presets_load(&loaded, path, error));
    assert(loaded.count == 2 && loaded.selected == 1 && !strcmp(loaded.game, "slug"));
    assert(contains(path, "# Test player settings. Environment overrides are never saved.\n"));
    for (int i = 0; i < 2; i++) for (int k = 0; k < T_COUNT; k++)
        assert(!strcmp(p.presets[i].settings.value[k], loaded.presets[i].settings.value[k]));

    /* Precedence: environment over preset, never saved back. */
    setenv("PSPRECOMP_DISPLAY", "primary", 1);
    assert(!psp_settings_load(&s, path, "Modern", error));
    assert(s.number[T_DISPLAY] == -1 && s.source[T_DISPLAY] == PSP_SOURCE_ENV);
    unsetenv("PSPRECOMP_DISPLAY");
    assert(!psp_settings_load(&s, path, "Modern", error));
    assert(s.number[T_DISPLAY] == 2 && s.source[T_DISPLAY] == PSP_SOURCE_PRESET);
    assert(psp_settings_load(&s, path, "Missing", error));
    assert(psp_settings_load(&s, NULL, "Modern", error));
    setenv("PSPRECOMP_INPUT", "", 1);
    assert(!psp_settings_load(&s, NULL, NULL, error) && s.source[T_INPUT] == PSP_SOURCE_DEFAULT);
    setenv("PSPRECOMP_INPUT", "invalid", 1);
    before = s; assert(psp_settings_env(&s, error)); assert(!memcmp(&before, &s, sizeof s));
    unsetenv("PSPRECOMP_INPUT");

    /* Retired keys are skipped; an unknown key or a bad value refuses the file
     * and leaves what was loaded before untouched. */
    putfile(path, "version=1\nselected=Old\n[preset Old]\nOLD_PREVIEW=1\nINPUT=dual\n");
    assert(!psp_settings_load(&s, path, "Old", error) && s.number[T_INPUT] == 1);
    assert(!psp_presets_load(&loaded, path, error));
    const char *invalid[] = {
        "version=2\nselected=A\n[preset A]\n",
        "version=1\nselected=Missing\n[preset A]\n",
        "version=1\nselected=A\n[preset A]\nINPUT=modern\nINPUT=classic\n",
        "version=1\nselected=A\n[preset A]\n[preset A]\n",
        "version=1\nselected=A\n[preset A]\nCAPTURE=oops\n",
        "version=1\nselected=A\n[preset A]\nDEADZONE=NaN\n",
    };
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        putfile(path, invalid[i]); psp_presets prior = loaded;
        assert(psp_presets_load(&loaded, path, error)); assert(!memcmp(&prior, &loaded, sizeof loaded));
    }
    /* A failed save leaves the previous file in place. */
    psp_presets_defaults(&p);
    assert(!psp_presets_save(&p, path, error));
    strcpy(p.presets[0].settings.value[T_WINDOW_SIZE], "broken");
    assert(psp_presets_save(&p, path, error));
    assert(!psp_presets_load(&loaded, path, error) && loaded.count == 2);
    assert(psp_presets_save(&p, "/no-such-directory/settings.ini", error));

    /* The installed snapshot. */
    assert(!psp_settings_load(&s, NULL, NULL, error));
    assert(!psp_settings_set(&s, T_INPUT, "modern", PSP_SOURCE_COMMAND_LINE, error));
    psp_settings_use(&s);
    assert(psp_settings_current()->number[T_INPUT] == 1);

    unlink(out); unlink(path); rmdir(dir);
    puts("settings: types, aliases, labels, renderer rule, title hooks, presets, precedence and file failures passed");
    return 0;
}
