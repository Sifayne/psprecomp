/* The player's launcher (src/host/launcher.c), with two packs of its own and
 * one installed: their pages, a pack switch that keeps every edit, resets of
 * one group, the earlier apps' settings brought in, the game library, the
 * page ring on the bumpers, the screens drawn, and a pack's launcher.so
 * loaded from the packs folder. Includes launcher.c for its internals; its
 * first argument is the folder holding packs/gamma/built/launcher.so
 * (tests/test_pack_plugin.c). */
#include <SDL2/SDL.h>
#define main launcher_entry
#include "../src/host/launcher.c"
#undef main
#include <assert.h>

enum { A_INPUT = PSP_PLAYER_OPTIONS, A_LAG, A_COUNT };
static const psp_option_def alpha_options[] = {
    {.key="INPUT",.env="PSPRECOMP_TEST_INPUT",.label="Control scheme",.page="Controls",.help="",
     .type=PSP_OPTION_CHOICE,.dflt="classic",.choices="classic|modern",.labels="Classic|Modern"},
    {.key="LAG",.env="PSPRECOMP_TEST_LAG",.label="Smoothing",.page="Controls",.help="",
     .type=PSP_OPTION_NUMBER,.dflt="game",.min=0,.max=0.99,.step=0.05,.special="game",.special_label="Off"},
};
static const psp_settings_schema alpha = {
    .title = "Alpha", .id = "alpha", .options = alpha_options, .count = 2, .play_defaults = "INPUT=modern",
};
enum { B_CAP = PSP_PLAYER_OPTIONS };
static const psp_option_def beta_options[] = {
    {.key="CAP",.env="PSPRECOMP_TEST_CAP",.label="Cap",.page="Picture",.help="",
     .type=PSP_OPTION_INTEGER,.dflt="60",.min=30,.max=1000,.step=1,.special="unlimited",
     .stops="30|60|120|unlimited"},
};
static const psp_settings_schema beta = {
    .title = "Beta", .id = "beta", .options = beta_options, .count = 1,
};
static const char *const alpha_titles[] = {"second", "first", NULL};
static const char *const beta_titles[] = {"third", NULL};
static const psp_launcher alpha_info = { .name = "Alpha", .about = "About Alpha.", .titles = alpha_titles };
static const psp_launcher beta_info = { .name = "Beta", .earlier = "Beta App", .about = "About Beta.", .titles = beta_titles };
const psp_pack psp_packs[] = { { &alpha_info, &alpha }, { &beta_info, &beta } };
const int psp_pack_count = 2;
const psp_settings_schema psp_title_settings = { .title = "psprecomp" };

static char root[256];
static void putfile(const char *path, const char *text) {
    char dir[512]; snprintf(dir, sizeof dir, "%s", path);
    *strrchr(dir, '/') = 0; assert(!make_directories(dir));
    FILE *f = fopen(path, "w"); assert(f); assert(fputs(text, f) >= 0); assert(!fclose(f));
}
static int contains(const char *path, const char *text) {
    FILE *f = fopen(path, "r"); if (!f) return 0;
    static char buf[16384]; size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
    return strstr(buf, text) != NULL;
}
static void key(launcher *a, SDL_Keycode sym) {
    SDL_Event e; memset(&e, 0, sizeof e);       /* {0} would set the union's first member alone */
    e.type = SDL_KEYDOWN; e.key.keysym.sym = sym; event(a, &e);
}
static int has_page(const launcher *a, int group, const char *name) {
    const char *names[PAGES_MAX];
    const int n = group_pages(a, group, names);
    for (int i = 0; i < n; i++) if (!strcmp(names[i], name)) return 1;
    return 0;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    for (int k = 0; k < PSP_PLAYER_OPTIONS; k++) unsetenv(psp_player_options[k].env);
    unsetenv("PSPRECOMP_TEST_INPUT"); unsetenv("PSPRECOMP_TEST_CAP"); unsetenv("PSPRECOMP_DATA_ROOT");
    snprintf(root, sizeof root, "/tmp/psp-launcher-test-XXXXXX"); assert(mkdtemp(root));
    char config[512], data[512], path[600], packs_dir[600];
    snprintf(config, sizeof config, "%s/config", root); snprintf(data, sizeof data, "%s/data", root);
    setenv("XDG_CONFIG_HOME", config, 1); setenv("XDG_DATA_HOME", data, 1);
    /* No host controller may press anything. SDL_Quit clears hints, so they
     * come before SDL_Init. */
    assert(SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_HIDAPI, "0", SDL_HINT_OVERRIDE));
    assert(SDL_SetHintWithPriority(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, "0x0000/0x0000", SDL_HINT_OVERRIDE));
    assert(!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER));

    /* The earlier apps' settings: Alpha's own app's (presets), Beta's game's
     * development launcher's, under its name. */
    snprintf(path, sizeof path, "%s/alpha/settings.ini", config);
    putfile(path, "version=1\nselected=Mine\n[preset Old]\nINPUT=classic\n[preset Mine]\nINPUT=modern\nVOLUME=30\n");
    snprintf(path, sizeof path, "%s/Beta App/settings.ini", data);
    putfile(path, "version=1\nselected=B\n[preset B]\nCAP=120\nVOLUME=90\n");
    /* The game library: Zeta has no pack here, and plays plain; Second is
     * not prepared yet. */
    char library[600];
    snprintf(library, sizeof library, "%s/library.bin", root);
    {
        const char fields[] = "LRLIB1\0\0third\0Third\0/bin/true\0/m3\0/i3\0zeta\0Zeta\0/bin/true\0/mz\0/iz\0"
                              "first\0First\0/bin/true\0/m1\0/i1\0second\0Second\0\0/m2\0/i2\0";
        FILE *f = fopen(library, "wb"); assert(f); fwrite(fields, 1, sizeof fields - 1, f); fclose(f);
    }
    snprintf(packs_dir, sizeof packs_dir, "%s/packs", argv[1]);
    char *args[] = { "launcher", "--library", library, "--packs", packs_dir, NULL };
    launcher a = {0}; a.running = 1;
    int check = 0; const char *wanted = NULL;
    assert(!start(&a, 5, args, &check, &wanted));

    /* Three packs: the two linked, then gamma's launcher.so; its resolve
     * reached the launcher's settings mechanism. */
    assert(pack_count == 3 && packs[2].handle && !strcmp(packs[2].settings->id, "gamma"));
    assert(!strcmp(packs[2].info->names[0], "Gamma Game"));
    use_pack(&a, 2);
    assert(a.edit.number[PSP_PLAYER_OPTIONS] == 1 && a.edit.title[0] == 1);
    assert(group_pages(&a, GROUP_PACK, (const char *[PAGES_MAX]){0}) == 1);
    use_pack(&a, 0);
    /* Alpha's earlier settings came in as it was first shown; nothing is
     * written until Save. */
    snprintf(path, sizeof path, "%s/psprecomp/settings.ini", config);
    assert(strstr(a.status, "Alpha") && !a.dirty && access(path, F_OK));

    /* Tabs: the packs in order, each pack's titles in its order, then the
     * game with no pack. */
    assert(a.game_count == 4 && !strcmp(a.games[0].slug, "second") && !strcmp(a.games[1].slug, "first") &&
           !strcmp(a.games[2].slug, "third") && a.games[2].pack == 1 &&
           !strcmp(a.games[3].slug, "zeta") && a.games[3].pack == -1);
    assert(a.pack == 0 && a.edit.number[PSP_OPT_VOLUME] == 30 && a.edit.number[A_INPUT] == 1);
    assert(a.game == 0 && !a.boot && !strcmp(a.iso, "/i2"));

    /* Pages: the player's, the pack's, then Packs and About. */
    assert(has_page(&a, GROUP_PLAYER, "Display") && has_page(&a, GROUP_PLAYER, "Save states"));
    const char *names[PAGES_MAX];
    assert(group_pages(&a, GROUP_PLAYER, names) == 6 && !strcmp(names[4], "System") && !strcmp(names[5], "Advanced"));
    assert(group_pages(&a, GROUP_PACK, names) == 1 && !strcmp(names[0], "Controls"));

    /* An edit to each table, then the other pack: the player's carries, the
     * first pack's waits in the file, the second's comes from it. */
    char error[PSP_SETTINGS_ERROR];
    assert(!psp_settings_set(&a.edit, PSP_OPT_WINDOW_MODE, "borderless", PSP_SOURCE_FILE, error)); changed(&a);
    assert(!psp_settings_set(&a.edit, A_LAG, "0.5", PSP_SOURCE_FILE, error)); changed(&a);
    show_page(&a, GROUP_PACK, "Controls");
    select_game(&a, 2);                                 /* Beta's come in now */
    assert(strstr(a.status, "Beta") && a.edit.number[PSP_OPT_VOLUME] == 30);
    assert(a.pack == 1 && a.edit.number[PSP_OPT_WINDOW_MODE] == 1 && a.edit.number[B_CAP] == 120);
    assert(a.group == GROUP_PLAYER && !a.page);         /* Beta has no Controls page */
    assert(!psp_settings_set(&a.edit, B_CAP, "unlimited", PSP_SOURCE_FILE, error)); changed(&a);
    select_game(&a, 0);
    assert(a.pack == 0 && a.edit.number[A_LAG] == 0.5 && a.edit.number[PSP_OPT_WINDOW_MODE] == 1);
    assert(!save(&a) && !a.dirty);
    assert(contains(path, "WINDOW_MODE=borderless") && contains(path, "LAG=0.5") && contains(path, "CAP=unlimited"));
    assert(contains(path, "[player]\nVOLUME=30\nRESOLUTION=") && contains(path, "[pack alpha]\nINPUT=modern\n"));
    assert(contains(path, "[preset alpha/Old]\nINPUT=classic\n"));
    assert(!contains(path, "[preset beta/B]"));          /* the selected one is the settings now */
    assert(contains(path, "game=second\n"));

    /* Reset one group: the pack's, then the player's; the movie stays off
     * without a decoder. */
    a.movie_available = 0;
    a.group = GROUP_PACK; a.confirm = CONFIRM_RESET;
    psp_settings_reset(&a.edit, PSP_PLAYER_OPTIONS); changed(&a);
    assert(a.edit.number[A_LAG] == -1 && a.edit.number[A_INPUT] == 1 && a.edit.number[PSP_OPT_WINDOW_MODE] == 1);
    psp_settings_reset(&a.edit, 0); movie_off(&a); changed(&a);
    assert(!a.edit.number[PSP_OPT_WINDOW_MODE] && a.edit.number[PSP_OPT_WINDOW] == 1 && !a.edit.number[PSP_OPT_MPEG_DECODE]);
    assert(a.valid);
    a.confirm = CONFIRM_NONE;

    /* Escape with unsaved changes asks; without, it closes. */
    key(&a, SDLK_ESCAPE); assert(a.confirm == CONFIRM_DISCARD && a.running);
    a.confirm = CONFIRM_NONE; a.dirty = 0;
    key(&a, SDLK_ESCAPE); assert(!a.running);
    a.running = 1;

    /* The screens draw: every page of the ring on the bumpers, the browser,
     * the preparation and the confirmations. */
    assert(!open_window(&a));
    SDL_HideWindow(a.window);
    a.group = GROUP_PLAYER; a.page = NULL;
    draw(&a);
    assert(a.page && !strcmp(a.page, "Display"));
    int groups[4 * PAGES_MAX], at; const char *ring[4 * PAGES_MAX];
    const int n = page_ring(&a, groups, ring, &at);
    assert(n == 6 + 1 + 2 && at == 0 && groups[n - 1] == GROUP_ABOUT && groups[n - 2] == GROUP_PACKS);
    for (int i = 0; i < n; i++) { step_page(&a, 1); draw(&a); }
    assert(a.group == GROUP_PLAYER && !strcmp(a.page, "Display"));
    step_page(&a, -1); assert(a.group == GROUP_ABOUT);
    draw(&a);
    a.confirm = CONFIRM_RESET; draw(&a); draw(&a); a.confirm = CONFIRM_NONE; draw(&a);
    a.importer = "/bin/false";
    browser_open(&a, BROWSE_ISO); assert(a.view == VIEW_BROWSE); draw(&a);
    key(&a, SDLK_ESCAPE); assert(a.view == VIEW_SETTINGS);
    a.view = VIEW_PREPARING; a.work = WORK_INSTALL; draw(&a); a.view = VIEW_SETTINGS;
    select_game(&a, 2); draw(&a);
    /* The plain game: the player's pages alone. */
    select_game(&a, 3); draw(&a);
    assert(a.pack == -1 && !group_pages(&a, GROUP_PACK, names) && group_pages(&a, GROUP_PLAYER, names) == 6);
    close_window(&a);

    psp_settings_file_free(a.file);
    free(a.library_buffer);
    SDL_Quit();
    char rm[700]; snprintf(rm, sizeof rm, "rm -r -- '%s'", root);
    assert(!system(rm));
    puts("launcher: packs linked and installed, pages, pack switches, resets, earlier settings, library, ring and screens passed");
    return 0;
}
