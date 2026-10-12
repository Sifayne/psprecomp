/* The in-game menu (src/host/overlay.c, src/host/ui.cpp; docs/PLAYER-LAYER.md
 * §4). Includes present.c, input.c and overlay.c to reach their internals,
 * with the runtime's pad caught here; Dear ImGui comes from the host layer.
 *
 *   test_overlay ui      the toolkit on a hidden window: the software path
 *                        draws the panel, the GL path's snapshot carries its
 *                        commands and the font texture, and clearing it
 *                        leaves the GL thread nothing
 *   test_overlay held    the real SDL loop with the menu: opening while
 *                        moving and firing takes the controls whole and asks
 *                        for the pause; nothing reaches the game while it is
 *                        open; closing with fire still held gives the game
 *                        nothing until it is let go; mouse capture goes and
 *                        comes back; focus loss leaves it open
 *   test_overlay bind    press-to-bind, a live volume, and the change saved
 *                        to the preferences file the game was started with
 *   test_overlay pad     controller only: into a page and back out, a slot
 *                        saved and a full one replaced, the slots scrolled
 *                        to the options under them, lists, the bindings
 *                        table, the volume a step a press, Mute; B back,
 *                        then out of the menu */
#define psp_sched_stop_all fixture_stop_all
#define psp_ctrl_set fixture_ctrl_set
#define psp_ctrl_set_look fixture_ctrl_set_look
#include "../src/host/present.c"
#include "../src/host/input.c"
#include "../src/host/overlay.c"
#include "psprecomp/safepoint.h"
#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>

static _Atomic uint32_t published;
static _Atomic int published_ax = 128;
void fixture_ctrl_set(uint32_t buttons, uint8_t ax, uint8_t ay) {
    (void)ay; atomic_store(&published, buttons); atomic_store(&published_ax, ax);
}
void fixture_ctrl_set_look(uint8_t rx, uint8_t ry) { (void)rx; (void)ry; }

enum { T_KEYS = PSP_PLAYER_OPTIONS, T_MOUSE };
static const psp_option_def options[] = {
    {.key="KEYS",.env="PSPRECOMP_KEYS",.label="Keyboard",.page="Controls",.help="",
     .type=PSP_OPTION_CHOICE,.dflt="wasd",.choices="classic|wasd",.labels="Classic|WASD",.flags=PSP_OPTION_LIVE},
    {.key="MOUSE",.env="PSPRECOMP_MOUSE",.label="Mouse",.page="Controls",.help="",
     .type=PSP_OPTION_CHOICE,.dflt="0",.choices="0|1",.labels="Off|On"},
};
static int resolve_mouse(psp_settings *s, char *error) { (void)error; s->mouse = s->number[T_MOUSE] != 0; return 0; }
const psp_settings_schema psp_title_settings = {
    .title = "Menu test", .id = "menu-test", .options = options, .count = 2, .resolve = resolve_mouse,
};
const psp_title psp_title_info = { .name = "Menu test" };

static _Atomic int stop_calls;
void fixture_stop_all(const char *reason) { (void)reason; atomic_fetch_add(&stop_calls, 1); }

/* ---- the toolkit ----------------------------------------------------------------- */

static void one_frame(void) {
    psp_ui_begin();
    psp_ui_panel_begin();
    psp_ui_heading("Paused");
    psp_ui_side_begin(200, 0);
    psp_ui_nav("Resume", 1);
    psp_ui_side_next();
    psp_ui_text("The game waits here.");
    psp_ui_side_end();
    psp_ui_panel_end();
    psp_ui_end();
}

static void ui(void) {
    assert(SDL_Init(SDL_INIT_VIDEO) == 0);
    SDL_Window *win = SDL_CreateWindow("menu", 0, 0, 960, 544, SDL_WINDOW_HIDDEN);
    assert(win);
    /* Software: the panel's colour where the panel is, the game's black
     * around it under the dimming. */
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    assert(ren);
    SDL_RenderSetLogicalSize(ren, 480, 272);
    assert(!psp_ui_start(win, ren));
    for (int i = 0; i < 3; i++) {
        one_frame();
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        psp_ui_draw(ren);
    }
    int lw = 0, lh = 0;
    SDL_RenderGetLogicalSize(ren, &lw, &lh);
    assert(lw == 480 && lh == 272);     /* the game's scaling is put back */
    SDL_RenderSetLogicalSize(ren, 0, 0);
    uint8_t centre[4], corner[4];
    SDL_Rect at = { 480, 400, 1, 1 }, edge = { 4, 4, 1, 1 };
    assert(!SDL_RenderReadPixels(ren, &at, SDL_PIXELFORMAT_ABGR8888, centre, 4));
    assert(!SDL_RenderReadPixels(ren, &edge, SDL_PIXELFORMAT_ABGR8888, corner, 4));
    assert(centre[0] > 10 && centre[2] > 20);           /* a navy panel, not black */
    assert(corner[0] < 10 && corner[1] < 10 && corner[2] < 10);
    psp_ui_stop();
    SDL_DestroyRenderer(ren);
    /* GL: nothing drawn here, a snapshot for the GL thread instead, its font
     * texture created before the frame that uses it, and nothing after a
     * clear. */
    assert(!psp_ui_start(win, NULL));
    one_frame();
    const psp_ui_texture_op *ops;
    int count;
    const psp_ui_frame *f = psp_ui_gl_lock(&ops, &count);
    assert(f && f->command_count && f->vertex_count && f->index_count && f->width == 960 && f->height == 544);
    int created = 0;
    for (int i = 0; i < count; i++)
        if (ops[i].op == PSP_UI_TEXTURE_CREATE && ops[i].pixels && ops[i].w > 0) created = (int)ops[i].texture;
    assert(created);
    for (uint32_t i = 0; i < f->command_count; i++) {
        assert(f->commands[i].first_index + f->commands[i].count <= f->index_count);
        assert(f->commands[i].first_vertex < f->vertex_count);
    }
    psp_ui_gl_unlock();
    one_frame();
    f = psp_ui_gl_lock(&ops, &count);
    assert(f && f->command_count);
    for (int i = 0; i < count; i++) assert(ops[i].op != PSP_UI_TEXTURE_CREATE || (int)ops[i].texture != created);
    psp_ui_gl_unlock();
    psp_ui_clear();
    assert(!psp_ui_gl_lock(&ops, &count));
    psp_ui_gl_unlock();
    psp_ui_stop();
    SDL_DestroyWindow(win);
    SDL_Quit();
    puts("menu: the toolkit draws in software, snapshots for GL with its textures, and clears");
}

/* ---- the real loop ----------------------------------------------------------------- */

static void virtual_button(SDL_Joystick *joystick, int button, int down) {
    assert(SDL_JoystickSetVirtualButton(joystick, button, (Uint8)down) == 0);
}
static int becomes(_Atomic uint32_t *what, uint32_t want) {
    Uint64 deadline = SDL_GetTicks64() + 1000;
    while (atomic_load(what) != want && SDL_GetTicks64() < deadline) SDL_Delay(2);
    return atomic_load(what) == want;
}
static int waits_for(int (*check)(void)) {
    Uint64 deadline = SDL_GetTicks64() + 1500;
    while (!check() && SDL_GetTicks64() < deadline) SDL_Delay(2);
    return check();
}
static int game_owns(void) { return input_owner() == INPUT_GAME; }
static int menu_owns(void) { return input_owner() == INPUT_OVERLAY && o.open; }
static int menu_shut(void) { return !o.open; }
static int grabbed(void) { return g_mouse_grabbed; }
static void key(SDL_Keycode sym, SDL_Scancode sc, int down) {
    SDL_Event e; memset(&e, 0, sizeof e);
    e.type = down ? SDL_KEYDOWN : SDL_KEYUP; e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.key.keysym.sym = sym; e.key.keysym.scancode = sc;
    assert(SDL_PushEvent(&e) == 1);
}

static char preferences[512];

static void start_loop(const char *mode) {
    psp_settings settings; char error[PSP_SETTINGS_ERROR];
    if (!strcmp(mode, "bind")) {
        /* A preferences file, as the launcher hands the game. */
        snprintf(preferences, sizeof preferences, "/tmp/psprecomp-menu-%d.ini", (int)getpid());
        psp_settings_file *f = psp_settings_file_new();
        psp_settings s; psp_settings_defaults(&s);
        assert(f && !psp_settings_file_put(f, &s, error) && !psp_settings_file_write(f, preferences, error));
        psp_settings_file_free(f);
        assert(!psp_settings_load(&settings, preferences, error));
    } else {
        setenv("PSPRECOMP_MOUSE", "1", 1);
        assert(!psp_settings_load(&settings, NULL, error));
    }
    psp_settings_use(&settings);
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_HIDAPI, "0", SDL_HINT_OVERRIDE);
    SDL_SetHintWithPriority(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, "0x0000/0x0000", SDL_HINT_OVERRIDE);
    present_use_overlay();
    assert(present_start() == 0);
}

static void finish_loop(const char *mode) {
    SDL_Event e; memset(&e, 0, sizeof e); e.type = SDL_KEYDOWN; e.key.state = SDL_PRESSED;
    e.key.keysym.sym = SDLK_q; e.key.keysym.scancode = SDL_SCANCODE_Q; e.key.keysym.mod = KMOD_LCTRL | KMOD_LSHIFT;
    assert(SDL_PushEvent(&e) == 1);
    Uint64 deadline = SDL_GetTicks64() + 3000;
    while (!atomic_load(&stop_calls) && SDL_GetTicks64() < deadline) SDL_Delay(5);
    assert(atomic_load(&stop_calls) == 1);
    assert(!pthread_join(g_thread, NULL));
    printf("menu: %s passed\n", mode);
}

static void held(void) {
    start_loop("held");
    int index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, SDL_CONTROLLER_AXIS_MAX,
                                          SDL_CONTROLLER_BUTTON_MAX, 0);
    assert(index >= 0);
    SDL_Joystick *joy = SDL_JoystickOpen(index);
    assert(joy);
    SDL_JoystickSetVirtualAxis(joy, SDL_CONTROLLER_AXIS_TRIGGERLEFT, -32768);
    SDL_JoystickSetVirtualAxis(joy, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, -32768);
    assert(becomes(&published, 0));
    /* MOUSE=1 captures at the start; the dummy video driver then reports
     * the window losing focus, which lets go. A click takes it back. */
    SDL_Delay(200);
    SDL_Event click; memset(&click, 0, sizeof click);
    click.type = SDL_MOUSEBUTTONDOWN; click.button.button = SDL_BUTTON_LEFT; click.button.state = SDL_PRESSED;
    assert(SDL_PushEvent(&click) == 1);
    click.type = SDL_MOUSEBUTTONUP; click.button.state = SDL_RELEASED;
    assert(SDL_PushEvent(&click) == 1);
    assert(waits_for(grabbed));
    /* Moving (D, WASD) and firing (cross on the pad). */
    key(SDLK_d, SDL_SCANCODE_D, 1);
    virtual_button(joy, SDL_CONTROLLER_BUTTON_A, 1);
    assert(becomes(&published, 0x4000));
    assert(atomic_load(&published_ax) == 255);
    /* View + Menu opens the menu: a neutral pad, the pause asked for, the
     * mouse let go. */
    virtual_button(joy, SDL_CONTROLLER_BUTTON_BACK, 1);
    virtual_button(joy, SDL_CONTROLLER_BUTTON_START, 1);
    assert(waits_for(menu_owns));
    assert(becomes(&published, 0) && atomic_load(&published_ax) == 128);
    assert(psp_pause_requested() && !g_mouse_grabbed);
    virtual_button(joy, SDL_CONTROLLER_BUTTON_BACK, 0);
    virtual_button(joy, SDL_CONTROLLER_BUTTON_START, 0);
    /* Nothing reaches the game while it is open. */
    key(SDLK_z, SDL_SCANCODE_Z, 1);
    virtual_button(joy, SDL_CONTROLLER_BUTTON_X, 1);
    SDL_Delay(150);
    assert(atomic_load(&published) == 0 && !g_key_down[SDL_SCANCODE_Z]);
    key(SDLK_z, SDL_SCANCODE_Z, 0);
    virtual_button(joy, SDL_CONTROLLER_BUTTON_X, 0);
    /* Focus loss leaves it open and owning the controls. */
    SDL_Event focus; memset(&focus, 0, sizeof focus);
    focus.type = SDL_WINDOWEVENT; focus.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
    assert(SDL_PushEvent(&focus) == 1);
    SDL_Delay(100);
    assert(menu_owns());
    /* Menu alone closes it, with cross still held: the pause is let go,
     * and the game has nothing until cross is released. */
    virtual_button(joy, SDL_CONTROLLER_BUTTON_START, 1);
    assert(waits_for(menu_shut));
    assert(!psp_pause_requested());
    virtual_button(joy, SDL_CONTROLLER_BUTTON_START, 0);
    SDL_Delay(300);
    assert(input_owner() == INPUT_OVERLAY && atomic_load(&published) == 0);
    virtual_button(joy, SDL_CONTROLLER_BUTTON_A, 0);
    assert(waits_for(game_owns));
    assert(atomic_load(&published) == 0 && g_mouse_grabbed);   /* capture comes back */
    /* The game's controls work again. */
    virtual_button(joy, SDL_CONTROLLER_BUTTON_A, 1);
    assert(becomes(&published, 0x4000));
    virtual_button(joy, SDL_CONTROLLER_BUTTON_A, 0);
    assert(becomes(&published, 0));
    /* Escape opens and closes it too. A button still held a second after
     * closing is the game's: the menu does not keep it deaf. */
    key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE, 1);
    key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE, 0);
    assert(waits_for(menu_owns));
    virtual_button(joy, SDL_CONTROLLER_BUTTON_B, 1);
    key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE, 1);
    key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE, 0);
    assert(waits_for(menu_shut));
    assert(waits_for(game_owns));                /* a second, B still held */
    assert(becomes(&published, 0x2000));
    virtual_button(joy, SDL_CONTROLLER_BUTTON_B, 0);
    assert(becomes(&published, 0));
    SDL_JoystickClose(joy);
    SDL_JoystickDetachVirtual(index);
    finish_loop("held");
}

/* Controller only: the pad opens the menu and walks its list of pages with
 * the D-pad. A or right on an entry goes into its page -- to the row it was on
 * there, else the current slot, else the first -- and left at the page's
 * edge or B back to the entry; B from the list resumes, and right on Resume
 * does not. On the pages: a slot saved to, a full one replaced after the
 * question (B says no), the slots scrolled through to the options under
 * them and not past the last, a list opened and chosen from and another
 * closed with B, the bindings table crossed and left at its foot, the volume
 * taken with A and moved a step a press, Mute. */
static int on_save(void) { return o.open && o.page == PAGE_SAVE; }
static int have_pad(void) { return input_pad() != NULL; }
static int popup_up(void) { return psp_ui_popup_open(); }
static int popup_down(void) { return !psp_ui_popup_open(); }
static int saving(void) { return o.waiting != 0; }
static int muted(void) { return o.mute; }
static void press(SDL_Joystick *joy, int button) {
    virtual_button(joy, button, 1);
    SDL_Delay(80);
    virtual_button(joy, button, 0);
    SDL_Delay(80);
}
/* The cursor on the widget with this label, or this label then "##". */
static const char *wanted;
static int cursor_matches(void) {
    const char *f = psp_ui_focused();
    const size_t n = strlen(wanted);
    return !strncmp(f, wanted, n) && (!f[n] || !strncmp(f + n, "##", 2));
}
static int cursor_on(const char *label) { wanted = label; return waits_for(cursor_matches); }
static int still_on(const char *label) { SDL_Delay(150); wanted = label; return cursor_matches(); }
static void pad_menu(SDL_Joystick *joy) {
    virtual_button(joy, SDL_CONTROLLER_BUTTON_BACK, 1);
    virtual_button(joy, SDL_CONTROLLER_BUTTON_START, 1);
    assert(waits_for(menu_owns));
    virtual_button(joy, SDL_CONTROLLER_BUTTON_BACK, 0);
    virtual_button(joy, SDL_CONTROLLER_BUTTON_START, 0);
}
static void pad(void) {
    start_loop("pad");
    int index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, SDL_CONTROLLER_AXIS_MAX,
                                          SDL_CONTROLLER_BUTTON_MAX, 0);
    assert(index >= 0);
    SDL_Joystick *joy = SDL_JoystickOpen(index);
    assert(joy);
    SDL_JoystickSetVirtualAxis(joy, SDL_CONTROLLER_AXIS_TRIGGERLEFT, -32768);
    SDL_JoystickSetVirtualAxis(joy, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, -32768);
    assert(waits_for(have_pad));
    pad_menu(joy);
    assert(cursor_on("Resume"));
    press(joy, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    assert(still_on("Resume") && o.open);
    /* Save state: A goes in on the current slot; A on an empty one saves. */
    press(joy, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    assert(cursor_on("Save state"));
    press(joy, SDL_CONTROLLER_BUTTON_A);
    assert(waits_for(on_save) && cursor_on("Slot 1  (current)"));
    press(joy, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    assert(cursor_on("Slot 2"));
    press(joy, SDL_CONTROLLER_BUTTON_A);
    assert(waits_for(saving) && g_waiting_slot == 1 && !g_waiting_load);
    o.waiting = 0;
    /* A full slot asks first: B says no, A on Replace saves. */
    o.have[2] = 1; o.info[2].usable = 1;
    press(joy, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    assert(cursor_on("Slot 3"));
    press(joy, SDL_CONTROLLER_BUTTON_A);
    assert(cursor_on("Replace") && o.confirm == 3);
    press(joy, SDL_CONTROLLER_BUTTON_B);
    assert(cursor_on("Slot 3  (current)") && !o.confirm && !o.waiting && o.open);
    press(joy, SDL_CONTROLLER_BUTTON_A);
    assert(cursor_on("Replace"));
    press(joy, SDL_CONTROLLER_BUTTON_A);
    assert(waits_for(saving) && g_waiting_slot == 2 && !o.confirm);
    o.waiting = 0;
    /* B goes back to the entry, right in again to the row it left, left
     * back out. */
    press(joy, SDL_CONTROLLER_BUTTON_B);
    assert(cursor_on("Save state") && o.open);
    press(joy, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    assert(cursor_on("Slot 3  (current)"));
    press(joy, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    assert(cursor_on("Save state"));
    /* Load state: in on the current slot, down through the slots as the
     * page scrolls, to the options under them, and the last keeps it. */
    press(joy, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    assert(cursor_on("Load state"));
    press(joy, SDL_CONTROLLER_BUTTON_A);
    assert(cursor_on("Slot 3  (current)"));
    for (int i = 0; i < 7; i++) press(joy, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    assert(cursor_on("Slot 10"));
    press(joy, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    assert(cursor_on(psp_settings_option(PSP_OPT_STATE_LOAD)->label));
    press(joy, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    const char *start = psp_settings_option(PSP_OPT_STATE_START)->label;
    assert(cursor_on(start));
    press(joy, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    assert(still_on(start));
    /* A opens its list, and down and A choose the other; B closes it
     * again, leaving the menu open. */
    press(joy, SDL_CONTROLLER_BUTTON_A);
    assert(waits_for(popup_up));
    press(joy, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    press(joy, SDL_CONTROLLER_BUTTON_A);
    assert(waits_for(popup_down) && o.edit.number[PSP_OPT_STATE_START] == 1 && cursor_on(start));
    press(joy, SDL_CONTROLLER_BUTTON_A);
    assert(waits_for(popup_up));
    press(joy, SDL_CONTROLLER_BUTTON_B);
    assert(waits_for(popup_down) && o.open && cursor_on(start));
    press(joy, SDL_CONTROLLER_BUTTON_A);
    press(joy, SDL_CONTROLLER_BUTTON_DPAD_UP);
    press(joy, SDL_CONTROLLER_BUTTON_A);
    assert(waits_for(popup_down) && o.edit.number[PSP_OPT_STATE_START] == 0);
    /* Bindings: in on the table's first cell, across a row, down past its
     * last row -- the table scrolls -- to the button under it. */
    press(joy, SDL_CONTROLLER_BUTTON_B);
    assert(cursor_on("Load state"));
    press(joy, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    assert(cursor_on("Bindings"));
    press(joy, SDL_CONTROLLER_BUTTON_A);
    assert(cursor_on("0.0"));
    press(joy, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    assert(cursor_on("0.1"));
    for (int i = 0; i < input_rows() + 2 && !cursor_on("Restore the defaults"); i++)
        press(joy, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    assert(cursor_on("Restore the defaults"));
    press(joy, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    assert(cursor_on("Bindings"));
    /* Audio: A takes the volume and the D-pad moves it, B lets it go and
     * the menu stays; Mute toggles with A. */
    for (int i = 0; i < 6 && !cursor_on("Audio"); i++) press(joy, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    assert(cursor_on("Audio"));
    press(joy, SDL_CONTROLLER_BUTTON_A);
    assert(cursor_on(psp_settings_option(PSP_OPT_VOLUME)->label));
    press(joy, SDL_CONTROLLER_BUTTON_A);
    press(joy, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    press(joy, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    press(joy, SDL_CONTROLLER_BUTTON_B);
    assert(still_on(psp_settings_option(PSP_OPT_VOLUME)->label) && o.open);
    assert(o.edit.number[PSP_OPT_VOLUME] == 90);
    press(joy, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    assert(cursor_on("Mute"));
    press(joy, SDL_CONTROLLER_BUTTON_A);
    assert(waits_for(muted));
    /* B to the entry, B again resumes. */
    press(joy, SDL_CONTROLLER_BUTTON_B);
    assert(cursor_on("Audio") && o.open);
    press(joy, SDL_CONTROLLER_BUTTON_B);
    assert(waits_for(menu_shut));
    assert(waits_for(game_owns));
    /* Opened again, it is on Resume, and A resumes. */
    pad_menu(joy);
    assert(cursor_on("Resume"));
    press(joy, SDL_CONTROLLER_BUTTON_A);
    assert(waits_for(menu_shut));
    assert(waits_for(game_owns));
    SDL_JoystickClose(joy);
    SDL_JoystickDetachVirtual(index);
    finish_loop("pad");
}

static int capture_done(void) { return !o.capturing; }

static void bind(void) {
    start_loop("bind");
    assert(waits_for(game_owns));
    key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE, 1);
    key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE, 0);
    assert(waits_for(menu_owns));
    /* Press-to-bind on the keyboard: cross is the fifth row. */
    int cross = -1;
    for (int r = 0; r < input_rows(); r++) if (!strcmp(input_row_label(r), "Cross")) cross = r;
    assert(cross >= 0);
    char sources[PSP_BIND_VALUE];
    input_row_sources(cross, INPUT_KEYBOARD, sources, sizeof sources);
    assert(!strcmp(sources, "Z, Space"));
    o.capture_row = cross; o.capture_device = INPUT_KEYBOARD;
    o.capture_since = SDL_GetTicks64(); o.capture_armed = 0; o.capturing = 1;
    SDL_Delay(100);
    key(SDLK_f, SDL_SCANCODE_F, 1);
    assert(waits_for(capture_done));
    key(SDLK_f, SDL_SCANCODE_F, 0);
    input_row_sources(cross, INPUT_KEYBOARD, sources, sizeof sources);
    assert(!strcmp(sources, "F"));
    /* A live option: the volume. */
    set_option(PSP_OPT_VOLUME, "40");
    assert(atomic_load(&g_volume) == (int)(0.4 * 65536 + 0.5));
    /* Closing saves both to the file it was started with. */
    key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE, 1);
    key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE, 0);
    assert(waits_for(menu_shut));
    assert(waits_for(game_owns));
    /* And the game has F as cross now. */
    key(SDLK_f, SDL_SCANCODE_F, 1);
    assert(becomes(&published, 0x4000));
    key(SDLK_f, SDL_SCANCODE_F, 0);
    assert(becomes(&published, 0));
    char error[PSP_SETTINGS_ERROR];
    psp_settings_file *f = psp_settings_file_read(preferences, error);
    psp_settings saved;
    assert(f && !psp_settings_file_get(f, &saved, error));
    psp_settings_file_free(f);
    assert(!strcmp(psp_settings_binding(&saved, "key.cross"), "F"));
    assert(!strcmp(saved.value[PSP_OPT_VOLUME], "40"));
    unlink(preferences);
    finish_loop("bind");
}

/* States from an earlier version (src/host/builds.h): what the menu says of
 * one whose build is kept and of one whose build is gone; that loading the
 * kept one asks to restart in that version, and the gone one is refused; and
 * what the restart starts with -- this run's arguments, with the build's
 * executable and module, and the state to load in place of any other. */
static void earlier(void) {
    char detail[256];
    o.open = 1;
    o.have[SLOT_EARLIER] = 1; o.info[SLOT_EARLIER].usable = 0; o.kept[SLOT_EARLIER] = 0;
    slot_detail(SLOT_EARLIER, detail, sizeof detail);
    assert(strstr(detail, "no longer kept"));
    ask_load(SLOT_EARLIER);
    assert(!o.confirm);
    o.kept[SLOT_EARLIER] = 1; o.info[SLOT_EARLIER].saved = 1760000000; o.info[SLOT_EARLIER].guest_us = 3723000000ull;
    slot_detail(SLOT_EARLIER, detail, sizeof detail);
    assert(strstr(detail, "played 1:02:03") && strstr(detail, "earlier version"));
    ask_load(SLOT_EARLIER);
    assert(o.confirm == SLOT_EARLIER + 1 && o.confirm_load == 2);
    o.confirm = 0;
    char cmdline[] = "/games/g/new/game\0/games/g/new/module.elf\0/disc.iso\0--config\0/c.ini\0"
                     "--load-state\0/s/x.state\0--window";
    char game[] = "/games/g/old/game", module[] = "/games/g/old/module.elf", state[] = "/s/earlier.state";
    char *argv[16];
    const int n = restart_args(cmdline, sizeof cmdline, game, module, state, argv, 16);
    const char *want[] = { game, module, "/disc.iso", "--config", "/c.ini", "--window", "--load-state", state };
    assert(n == 8 && !argv[8]);
    for (int i = 0; i < 8; i++) assert(!strcmp(argv[i], want[i]));
    puts("menu: an earlier version's state, kept or not, and the restart that loads it");
}

int main(int argc, char **argv) {
    assert(argc == 2);
    if (!strcmp(argv[1], "ui")) ui();
    else if (!strcmp(argv[1], "earlier")) earlier();
    else if (!strcmp(argv[1], "held")) held();
    else if (!strcmp(argv[1], "bind")) bind();
    else if (!strcmp(argv[1], "pad")) pad();
    else assert(0);
    return 0;
}
