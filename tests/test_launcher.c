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
/* The pad, pressed and let go over a few frames each. */
static void frames(launcher *a, int n) {
    for (int i = 0; i < n; i++) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) event(a, &e);
        draw(a);
    }
}
static void press(launcher *a, SDL_Joystick *joy, int button) {
    assert(!SDL_JoystickSetVirtualButton(joy, button, 1)); frames(a, 3);
    assert(!SDL_JoystickSetVirtualButton(joy, button, 0)); frames(a, 3);
}
/* The cursor on the widget with this label, or this label then "##". */
static int cursor_on(const char *label) {
    const char *f = psp_ui_focused();
    const size_t n = strlen(label);
    return !strncmp(f, label, n) && (!f[n] || !strncmp(f + n, "##", 2));
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
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
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
    /* A preparation keeps the settings open, its progress under them. A
     * stand-in importer reports the compile and waits: play, Add game and
     * Add pack wait for it, closing asks first, and Stop ends it. */
    snprintf(path, sizeof path, "%s/importer.sh", root);
    putfile(path, "#!/bin/sh\necho 'Compiling game: 12/32'\nexec sleep 30\n");
    assert(!chmod(path, 0755));
    a.importer = path;
    import_start(&a, WORK_IMPORT, "/no/such.iso");
    assert(working(&a) && a.view == VIEW_SETTINGS);
    for (int i = 0; i < 300 && strcmp(a.import_status, "Compiling game: 12/32"); i++) { SDL_Delay(10); import_poll(&a); }
    assert(!strcmp(a.import_status, "Compiling game: 12/32"));
    show_page(&a, GROUP_PACKS, "Packs"); draw(&a);
    show_page(&a, GROUP_PLAYER, "Display"); draw(&a);
    a.status[0] = 0;
    launch_game(&a);
    assert(strstr(a.status, "Wait for the game") && !a.child);
    browser_open(&a, BROWSE_ISO);
    assert(a.view == VIEW_SETTINGS);
    activate_back(&a);
    assert(a.confirm == CONFIRM_STOP && a.running);
    draw(&a); draw(&a);
    a.confirm = CONFIRM_NONE; draw(&a);
    import_cancel(&a);
    for (int i = 0; i < 300 && working(&a); i++) { SDL_Delay(10); import_poll(&a); }
    assert(!working(&a) && strstr(a.status, "Preparation canceled") && a.running);
    a.importer = "/bin/false";
    select_game(&a, 2); draw(&a);
    /* The plain game: the player's pages alone. */
    select_game(&a, 3); draw(&a);
    assert(a.pack == -1 && !group_pages(&a, GROUP_PACK, names) && group_pages(&a, GROUP_PLAYER, names) == 6);

    /* States across updates (psprecomp/builds.h). Zeta's builds: the one it
     * plays, the earlier one its place on quitting was saved in, and a spare
     * no state needs. Play asks where to play; each answer starts its build,
     * the earlier one by its own module; and once the game is over the spare
     * is removed, the other two kept. */
    {
        char games[600], cur[700], old[700], spare[700], states[600], file[800], boot[600], disc[600], launched[600];
        snprintf(games, sizeof games, "%s/data/psprecomp/games/zeta", root);
        snprintf(cur, sizeof cur, "%s/cur", games);
        snprintf(old, sizeof old, "%s/old", games);
        snprintf(spare, sizeof spare, "%s/spare", games);
        const char *const ids[3] = { "exe 0000000000000c0c\nfingerprint 000000000000f00d\n",
                                     "exe 0000000000000a0a\nfingerprint 000000000000beef\n",
                                     "exe 0000000000000b0b\n" };
        const char *const dirs[3] = { cur, old, spare };
        for (int k = 0; k < 3; k++) {
            snprintf(file, sizeof file, "%s/game", dirs[k]); putfile(file, "#!/bin/sh\n");
            snprintf(file, sizeof file, "%s/build.json", dirs[k]); putfile(file, "{}\n");
            snprintf(file, sizeof file, "%s/module.elf", dirs[k]); putfile(file, "");
            snprintf(file, sizeof file, "%s/state-id", dirs[k]); putfile(file, ids[k]);
        }
        /* The place on quitting: format 3, the earlier build's. */
        snprintf(states, sizeof states, "%s/data/psprecomp/states/zeta", root);
        snprintf(file, sizeof file, "%s/quit.state", states);
        putfile(file, "");
        struct {
            char magic[8]; uint32_t format, header_size; uint64_t exe;
            uint32_t functions, resume_sites; uint64_t guest_us; uint32_t polls, reserved; int64_t saved; uint64_t fingerprint;
        } h = { {'P','S','P','S','T','A','T','1'}, 3, sizeof h, 0xa0a, 0, 0, 0, 0, 0, 1760000000, 0xbeef };
        FILE *f = fopen(file, "wb"); assert(f);
        static const unsigned char rest[64];
        fwrite(&h, sizeof h, 1, f); fwrite(rest, 1, sizeof rest, f); fclose(f);
        snprintf(launched, sizeof launched, "%s/launched.txt", root);
        snprintf(boot, sizeof boot, "%s/boot.sh", root);
        char script[800]; snprintf(script, sizeof script, "#!/bin/sh\necho \"$1\" > '%s'\n", launched);
        putfile(boot, script); assert(!chmod(boot, 0755));
        snprintf(disc, sizeof disc, "%s/disc.iso", root); putfile(disc, "");
        static char module[800];
        snprintf(module, sizeof module, "%s/module.elf", cur);
        a.games[3].boot = boot; a.games[3].module = module; a.games[3].iso = disc;
        select_game(&a, 3);
        char error[PSP_SETTINGS_ERROR];
        assert(!psp_settings_set(&a.edit, PSP_OPT_STATE_START, "continue", PSP_SOURCE_FILE, error));
        a.status[0] = 0;
        launch_game(&a);
        assert(a.confirm == CONFIRM_EARLIER && !a.child && !strcmp(a.earlier, old));
        draw(&a); draw(&a);
        for (int answer = 1; answer <= 2; answer++) {
            remove(launched);
            a.confirm = CONFIRM_NONE; a.earlier_choice = answer;
            launch_game(&a);
            assert(a.child);
            for (int i = 0; i < 500 && a.child; i++) { SDL_Delay(10); poll_child(&a); }
            assert(!a.child);
            a.running = 1;
            assert(contains(launched, answer == 1 ? old : cur));
        }
        /* After the game: the spare is gone, the others stay. */
        snprintf(file, sizeof file, "%s/game", spare); assert(access(file, F_OK));
        snprintf(file, sizeof file, "%s/game", old); assert(!access(file, F_OK));
        snprintf(file, sizeof file, "%s/game", cur); assert(!access(file, F_OK));
        /* A place this build can load is not asked about. */
        h.fingerprint = 0xf00d; h.exe = 0xc0c;
        snprintf(file, sizeof file, "%s/quit.state", states);
        f = fopen(file, "wb"); assert(f); fwrite(&h, sizeof h, 1, f); fwrite(rest, 1, sizeof rest, f); fclose(f);
        remove(launched);
        launch_game(&a);
        assert(!a.confirm && a.child);
        for (int i = 0; i < 500 && a.child; i++) { SDL_Delay(10); poll_child(&a); }
        a.running = 1;
        assert(contains(launched, cur));
        SDL_ShowWindow(a.window);
    }

    /* Controller only: down reaches the list of pages; A goes into a page,
     * B back to its entry, right in again to the row it left, left out; a
     * bumper takes the cursor along the list; down past the list's last
     * entry reaches the buttons under it, and up from them a pane; B from the
     * list leaves. */
    const int pad_index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, SDL_CONTROLLER_AXIS_MAX,
                                                    SDL_CONTROLLER_BUTTON_MAX, 0);
    assert(pad_index >= 0);
    SDL_Joystick *joy = SDL_JoystickOpen(pad_index);
    assert(joy);
    SDL_JoystickSetVirtualAxis(joy, SDL_CONTROLLER_AXIS_TRIGGERLEFT, -32768);
    SDL_JoystickSetVirtualAxis(joy, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, -32768);
    frames(&a, 3);
    assert(a.pad);
    a.group = GROUP_PLAYER; a.page = "Display"; a.dirty = 0;
    for (int i = 0; i < 8 && !cursor_on("Display"); i++) press(&a, joy, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    assert(cursor_on("Display"));
    const char *first = psp_settings_option(PSP_OPT_RESOLUTION)->label;
    const char *second = psp_settings_option(PSP_OPT_WINDOW_MODE)->label;
    press(&a, joy, SDL_CONTROLLER_BUTTON_A);
    assert(cursor_on(first));
    press(&a, joy, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    assert(cursor_on(second));
    press(&a, joy, SDL_CONTROLLER_BUTTON_B);
    assert(cursor_on("Display") && a.running && !a.confirm);
    press(&a, joy, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    assert(cursor_on(second));
    press(&a, joy, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    assert(cursor_on("Display"));
    press(&a, joy, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    assert(!strcmp(a.page, "Audio") && cursor_on("Audio"));
    for (int i = 0; i < 12 && !cursor_on("About"); i++) press(&a, joy, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    assert(cursor_on("About"));
    press(&a, joy, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    assert(cursor_on("Reset to defaults") || cursor_on("Cancel"));
    press(&a, joy, SDL_CONTROLLER_BUTTON_DPAD_UP);
    assert(cursor_on("About"));
    press(&a, joy, SDL_CONTROLLER_BUTTON_B);
    assert(!a.running);
    a.running = 1;
    SDL_JoystickClose(joy);
    SDL_JoystickDetachVirtual(pad_index);
    close_window(&a);

    psp_settings_file_free(a.file);
    free(a.library_buffer);
    SDL_Quit();
    char rm[700]; snprintf(rm, sizeof rm, "rm -r -- '%s'", root);
    assert(!system(rm));
    puts("launcher: packs linked and installed, pages, pack switches, resets, earlier settings, library, ring, screens and the pad passed");
    return 0;
}
