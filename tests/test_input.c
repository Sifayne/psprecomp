/* The host's input (src/host/input.c, docs/PLAYER-LAYER.md §3).
 *
 * The table test first: every control, alone and in the combinations that
 * interact, through the input code as it stood before stage 6
 * (input_before.h) and through the default bindings, for every keyboard and
 * controller layout, with the title's Modern controls on and off, for a
 * title with no input of its own, for Armored Core's (named carriers, no
 * fallback) and for The 3rd Birthday's (its tables, its walk and its classic
 * fallback, with the free camera on and off). The pads they make must be the
 * same. Then the bindings: their spelling, rebinding, what is refused, host
 * actions, the wheel, and the owner.
 *
 * Includes input.c to reach its internals, with the runtime's pad calls
 * caught here. */
#define psp_ctrl_set fixture_ctrl_set
#define psp_ctrl_set_look fixture_ctrl_set_look
#define psp_ctrl_add_mouse fixture_ctrl_add_mouse
#define psp_ctrl_clear_mouse fixture_ctrl_clear_mouse
#define psp_ctrl_polls fixture_ctrl_polls
#include "../src/host/input.c"

#include <assert.h>
#include <stdatomic.h>

static pad_word published;
static int added_x, cleared, polls;
void fixture_ctrl_set(uint32_t buttons, uint8_t ax, uint8_t ay) { published.buttons = buttons; published.ax = ax; published.ay = ay; }
void fixture_ctrl_set_look(uint8_t rx, uint8_t ry) { published.lx = rx; published.ly = ry; }
void fixture_ctrl_add_mouse(int dx, int dy) { (void)dy; added_x += dx; }
void fixture_ctrl_clear_mouse(void) { cleared++; }
uint32_t fixture_ctrl_polls(void) { return (uint32_t)polls; }

static const psp_option_def options[] = {
    {.key="KEYS",.env="PSPRECOMP_KEYS",.label="Keyboard",.page="Controls",.help="",
     .type=PSP_OPTION_CHOICE,.dflt="classic",.choices="classic|wasd",.labels="Classic|WASD"},
    {.key="MOUSE",.env="PSPRECOMP_MOUSE",.label="Mouse",.page="Controls",.help="",
     .type=PSP_OPTION_CHOICE,.dflt="0",.choices="0|1",.labels="Off|On"},
};
const psp_settings_schema psp_title_settings = { .title = "Input test", .id = "input-test", .options = options, .count = 2 };
const psp_title psp_title_info = { .name = "Input test" };

/* ---- the code before, and its title hooks ------------------------------------ */

static struct {
    const psp_key_bind *keys_wasd;
    unsigned keys_wasd_count;
    const uint32_t *mouse_wasd;
    uint32_t (*classic_keys)(uint32_t buttons);
    uint32_t (*classic_mouse)(uint32_t buttons);
    uint8_t (*key_axis)(int along, int across, int walk);
    uint32_t (*pad_button)(int button);
} old_title;
static pad_word old_out;
#include "input_before.h"

/* The 3rd Birthday's input before stage 6: third-birthday-recomp
 * host/t3b_present.c and host/controls.h, with its LR_PAD_* spelled PSP_PAD_*. */
static const psp_key_bind TB_KEYS[18] = {
    { SDLK_RIGHT,      0x000020 }, { SDLK_LEFT,      0x000080 },
    { SDLK_DOWN,       0x000040 }, { SDLK_UP,        0x000010 },
    { SDLK_RETURN,     0x000008 }, { SDLK_BACKSPACE, 0x000001 },
    { SDLK_z,          PSP_PAD_A }, { SDLK_x,         PSP_PAD_B },
    { SDLK_SPACE,      PSP_PAD_A }, { SDLK_e,         PSP_PAD_A },
    { SDLK_r,          PSP_PAD_X }, { SDLK_f,         PSP_PAD_Y },
    { SDLK_q,          PSP_PAD_LB }, { SDLK_g,        PSP_PAD_RB },
    { SDLK_c,          PSP_PAD_L3 }, { SDLK_v,        PSP_PAD_R3 },
    { SDLK_j,          PSP_PAD_RT }, { SDLK_k,        PSP_PAD_LT },
};
static const uint32_t TB_MOUSE[4] = { 0, PSP_PAD_RT, PSP_PAD_RB, PSP_PAD_LT };
static int tb_free_look;
static uint32_t tb_shooter(uint32_t buttons) {
    uint32_t out = buttons & ~PSP_PAD_EXTRA;
    if (buttons & PSP_PAD_A) out |= 0x4000;
    if (buttons & (PSP_PAD_B | PSP_PAD_RB)) out |= 0x2000;
    if (buttons & (PSP_PAD_X | PSP_PAD_LB)) out |= 0x8000;
    if (buttons & PSP_PAD_Y) out |= 0x1000;
    if (buttons & (PSP_PAD_LT | PSP_PAD_R3)) out |= 0x0100;
    if (buttons & PSP_PAD_RT) out |= 0x0200;
    if (buttons & PSP_PAD_L3) out |= 0x3000;
    return out;
}
static uint32_t tb_camera(uint32_t buttons) {
    return tb_free_look ? tb_shooter(buttons & ~PSP_PAD_R3) | (buttons & PSP_PAD_R3) : tb_shooter(buttons);
}
static uint8_t tb_key_axis(int value, int other, int walk) {
    const int reach = !walk ? 127 : value && other ? 51 : 72;
    return (uint8_t)(128 + reach * value);
}
static uint32_t tb_pad_button(int button) {
    return tb_free_look && button == SDL_CONTROLLER_BUTTON_RIGHTSTICK ? PSP_PAD_R3 : 0;
}

/* The same title as data, which the game now registers. */
static psp_title_action tb_actions[10] = {
    { "dodge", "Dodge / interact", PSP_PAD_A, 0x4000 }, { "cancel", "Cancel", PSP_PAD_B, 0x2000 },
    { "reload", "Reload", PSP_PAD_X, 0x8000 }, { "overdive", "Overdive", PSP_PAD_Y, 0x1000 },
    { "weapon", "Weapon selection", PSP_PAD_LB, 0x8000 }, { "grenade", "Grenade", PSP_PAD_RB, 0x2000 },
    { "aim", "Aim", PSP_PAD_LT, 0x0100 }, { "fire", "Fire", PSP_PAD_RT, 0x0200 },
    { "liberation", "Liberation", PSP_PAD_L3, 0x3000 }, { "recenter", "Recenter", PSP_PAD_R3, 0x0100 },
};
static const psp_pad_bind TB_PAD[] = { { SDL_CONTROLLER_BUTTON_RIGHTSTICK, PSP_PAD_R3 } };
static void tb_input(psp_title_input *out) {
    tb_actions[9].classic = tb_free_look ? PSP_ACTION_KEEP : 0x0100;
    out->actions = tb_actions; out->action_count = 10;
    out->keys_wasd = TB_KEYS; out->keys_wasd_count = 18; out->mouse_wasd = TB_MOUSE;
    out->pad = TB_PAD; out->pad_count = tb_free_look ? 1 : 0;
    out->walk = 72; out->walk_diagonal = 51;
}

/* Armored Core's: every carrier named, none translated. */
static const psp_title_action AC_ACTIONS[10] = {
    { "a", "A", PSP_PAD_A, PSP_ACTION_KEEP }, { "b", "B", PSP_PAD_B, PSP_ACTION_KEEP },
    { "x", "X", PSP_PAD_X, PSP_ACTION_KEEP }, { "y", "Y", PSP_PAD_Y, PSP_ACTION_KEEP },
    { "lb", "LB", PSP_PAD_LB, PSP_ACTION_KEEP }, { "rb", "RB", PSP_PAD_RB, PSP_ACTION_KEEP },
    { "lt", "LT", PSP_PAD_LT, PSP_ACTION_KEEP }, { "rt", "RT", PSP_PAD_RT, PSP_ACTION_KEEP },
    { "l3", "L3", PSP_PAD_L3, PSP_ACTION_KEEP }, { "r3", "R3", PSP_PAD_R3, PSP_ACTION_KEEP },
};

/* ---- the table test ------------------------------------------------------------ */

enum { PLAIN, ARMORED_CORE, THIRD_BIRTHDAY, FIXTURES };
static const char *const FIXTURE_NAME[] = { "plain", "Armored Core", "The 3rd Birthday" };
static int failures, checks, pressed;
static char mode[96];

static void setup(int fixture, int wasd, int keys_modern, int gamepad_modern) {
    memset(&old_title, 0, sizeof old_title);
    memset(&g_tin, 0, sizeof g_tin);
    if (fixture == ARMORED_CORE) { g_tin.actions = AC_ACTIONS; g_tin.action_count = 10; }
    if (fixture == THIRD_BIRTHDAY) {
        old_title.keys_wasd = TB_KEYS; old_title.keys_wasd_count = 18; old_title.mouse_wasd = TB_MOUSE;
        old_title.classic_keys = tb_camera; old_title.classic_mouse = tb_shooter;
        old_title.key_axis = tb_key_axis; old_title.pad_button = tb_pad_button;
        tb_input(&g_tin);
    }
    old_g_keys_wasd = g_keys_wasd = wasd;
    old_g_keys_modern = g_keys_modern = keys_modern;
    old_g_gamepad_modern = g_gamepad_modern = gamepad_modern;
    build_defaults();
    find_keys();
    old_clear_keys(); old_clear_controller(); old_g_mouse_down = 0; atomic_store(&old_g_mouse_buttons, 0);
    clear_keys(); clear_controller(); g_mouse_down = 0;
}

static void check(const char *what, int value) {
    checks++;
    old_publish_pad();
    pad_word now;
    resolve(&now);
    pressed += old_out.buttons || old_out.ax != 128 || old_out.ay != 128 || old_out.lx != 128 || old_out.ly != 128;
    if (now.buttons == old_out.buttons && now.ax == old_out.ax && now.ay == old_out.ay &&
        now.lx == old_out.lx && now.ly == old_out.ly) return;
    if (++failures <= 20)
        printf("FAIL %s: %s %d: before %06X %3u,%3u look %3u,%3u  now %06X %3u,%3u look %3u,%3u\n",
               mode, what, value, old_out.buttons, old_out.ax, old_out.ay, old_out.lx, old_out.ly,
               now.buttons, now.ax, now.ay, now.lx, now.ly);
}

static void key(SDL_Scancode sc, int is_down) { old_set_key(sc, is_down); g_key_down[sc] = (uint8_t)is_down; }
static void mouse(int b, int is_down) {
    /* The old event filter: WASD only, and only the buttons its table maps. */
    if (old_g_keys_wasd && b < 4 && old_mouse_wasd()[b]) old_set_mouse_button((uint8_t)b, is_down);
    if (is_down) g_mouse_down |= SDL_BUTTON(b); else g_mouse_down &= ~SDL_BUTTON(b);
}
static void button(int b, int is_down) { old_set_button((uint8_t)b, is_down); g_pad_down[b] = (uint8_t)is_down; }
static void axis(int a, int16_t v) { old_set_axis((uint8_t)a, v); g_pad_axis[a] = v; }

static void every_control(void) {
    for (int sc = 1; sc < SDL_NUM_SCANCODES; sc++) {
        key((SDL_Scancode)sc, 1); check("key", sc);
        key((SDL_Scancode)sc, 0); check("key up", sc);
    }
    /* The keyboard's stick, every combination, walking and not. */
    static const SDL_Scancode STICK[5] = { SDL_SCANCODE_W, SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_D, SDL_SCANCODE_LALT };
    for (int set = 0; set < 32; set++) {
        for (int k = 0; k < 5; k++) key(STICK[k], (set >> k) & 1);
        check("stick keys", set);
    }
    for (int k = 0; k < 5; k++) key(STICK[k], 0);
    for (int b = 1; b <= 5; b++) { mouse(b, 1); check("mouse", b); mouse(b, 0); check("mouse up", b); }
    for (int set = 0; set < 8; set++) {
        for (int b = 1; b <= 3; b++) mouse(b, (set >> (b - 1)) & 1);
        check("mouse buttons", set);
    }
    for (int b = 1; b <= 3; b++) mouse(b, 0);
    for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++) {
        button(b, 1); check("button", b);
        button(b, 0); check("button up", b);
    }
    static const int16_t TRIGGER[] = { 0, 16383, 16384, 12000, 8193, 8192, 0, 32767, 20000, 8192, 0 };
    for (int a = SDL_CONTROLLER_AXIS_TRIGGERLEFT; a <= SDL_CONTROLLER_AXIS_TRIGGERRIGHT; a++)
        for (size_t i = 0; i < sizeof TRIGGER / sizeof *TRIGGER; i++) { axis(a, TRIGGER[i]); check("trigger", TRIGGER[i]); }
    static const int16_t STICK_AT[] = { -32768, -16384, -1, 0, 1, 16384, 32767, 0 };
    for (int a = SDL_CONTROLLER_AXIS_LEFTX; a <= SDL_CONTROLLER_AXIS_RIGHTY; a++)
        for (size_t i = 0; i < sizeof STICK_AT / sizeof *STICK_AT; i++) { axis(a, STICK_AT[i]); check("stick", STICK_AT[i]); }
    /* A drifting pad under the keyboard's stick, axis by axis. */
    axis(SDL_CONTROLLER_AXIS_LEFTX, -20000); axis(SDL_CONTROLLER_AXIS_LEFTY, 20000);
    for (int set = 0; set < 16; set++) {
        for (int k = 0; k < 4; k++) key(STICK[k], (set >> k) & 1);
        check("stick keys over a pad", set);
    }
    for (int k = 0; k < 4; k++) key(STICK[k], 0);
    axis(SDL_CONTROLLER_AXIS_LEFTX, 0); axis(SDL_CONTROLLER_AXIS_LEFTY, 0);
    /* Everything at once: a key, a mouse button, a face button, a trigger. */
    for (int sc = 1; sc < SDL_NUM_SCANCODES; sc += 7) {
        key((SDL_Scancode)sc, 1); mouse(1, 1); button(SDL_CONTROLLER_BUTTON_A, 1);
        button(SDL_CONTROLLER_BUTTON_RIGHTSTICK, 1); axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 30000);
        check("together", sc);
        key((SDL_Scancode)sc, 0); mouse(1, 0); button(SDL_CONTROLLER_BUTTON_A, 0);
        button(SDL_CONTROLLER_BUTTON_RIGHTSTICK, 0); axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 0);
        check("together, released", sc);
    }
}

static void table_test(void) {
    int modes = 0;
    for (int fixture = 0; fixture < FIXTURES; fixture++)
        for (int free_look = 0; free_look < (fixture == THIRD_BIRTHDAY ? 2 : 1); free_look++)
            for (int wasd = 0; wasd < 2; wasd++)
                for (int keys_modern = 0; keys_modern < (fixture == PLAIN ? 1 : 2); keys_modern++)
                    for (int gamepad_modern = 0; gamepad_modern < (fixture == PLAIN ? 1 : 2); gamepad_modern++) {
                        tb_free_look = free_look;
                        snprintf(mode, sizeof mode, "%s%s, keys %s%s, gamepad %s", FIXTURE_NAME[fixture],
                                 free_look ? " (free camera)" : "", wasd ? "wasd" : "classic",
                                 keys_modern ? " modern" : "", gamepad_modern ? "modern" : "PSP");
                        setup(fixture, wasd, keys_modern, gamepad_modern);
                        every_control();
                        modes++;
                    }
    printf("input: table test, %d modes, %d pads compared (%d of them pressed), %d different\n",
           modes, checks, pressed, failures);
    assert(!failures && pressed > checks / 10);
}

/* ---- bindings ----------------------------------------------------------------------- */

static int bound(int device, const char *target_name, const char *source_text) {
    binding t = { 0 }, s = { 0 };
    char why[160];
    assert(!parse_target(target_name, &t));
    assert(!parse_source(device, source_text, &s, why, sizeof why));
    for (int i = 0; i < g_nbinds; i++) {
        const binding *b = &g_binds[i];
        if (b->device == device && b->tgt == t.tgt && b->value == t.value && b->src == s.src &&
            b->code == s.code && b->mods == s.mods) return 1;
    }
    return 0;
}

static void spelling(void) {
    static const struct { int device; const char *text; } SAME[] = {
        { DEV_KEY, "Z" }, { DEV_KEY, "Space" }, { DEV_KEY, "Left Alt" }, { DEV_KEY, "Comma" },
        { DEV_KEY, "Ctrl+Shift+Q" }, { DEV_KEY, "scancode:W" }, { DEV_KEY, "Keypad +" }, { DEV_KEY, "Alt+Return" },
        { DEV_PAD, "a" }, { DEV_PAD, "leftshoulder" }, { DEV_PAD, "rightstick" }, { DEV_PAD, "+leftx" },
        { DEV_PAD, "-righty" }, { DEV_PAD, "lefttrigger" }, { DEV_PAD, "leftxy" }, { DEV_PAD, "rightxy" },
        { DEV_MOUSE, "left" }, { DEV_MOUSE, "x2" }, { DEV_MOUSE, "wheelup" }, { DEV_MOUSE, "motion" },
    };
    for (size_t i = 0; i < sizeof SAME / sizeof *SAME; i++) {
        binding b = { 0 };
        char why[160], back[64];
        if (parse_source(SAME[i].device, SAME[i].text, &b, why, sizeof why)) { printf("FAIL %s: %s\n", SAME[i].text, why); assert(0); }
        format_source(&b, back, sizeof back);
        if (strcmp(back, SAME[i].text)) { printf("FAIL %s reads back as %s\n", SAME[i].text, back); assert(0); }
    }
    static const struct { int device; const char *text; } WRONG[] = {
        { DEV_KEY, "Nonsense" }, { DEV_KEY, "scancode:Nonsense" }, { DEV_PAD, "z" }, { DEV_PAD, "+nonsense" },
        { DEV_MOUSE, "Z" }, { DEV_MOUSE, "wheel" },
    };
    for (size_t i = 0; i < sizeof WRONG / sizeof *WRONG; i++) {
        binding b = { 0 };
        char why[160];
        assert(parse_source(WRONG[i].device, WRONG[i].text, &b, why, sizeof why) && *why);
    }
    puts("input: sources read and write back the same, and nonsense is refused");
}

static void rebinding(void) {
    tb_free_look = 0;
    setup(THIRD_BIRTHDAY, 1, 1, 1);
    assert(bound(DEV_KEY, "fire", "J") && bound(DEV_PAD, "fire", "righttrigger") && bound(DEV_MOUSE, "fire", "left"));
    /* A target's sources on one device, replaced; the other devices keep theirs. */
    assert(!apply_binding("key.fire", "F, Comma"));
    find_keys();
    assert(!bound(DEV_KEY, "fire", "J") && bound(DEV_KEY, "fire", "F") && bound(DEV_KEY, "fire", "Comma"));
    assert(bound(DEV_PAD, "fire", "righttrigger") && bound(DEV_MOUSE, "fire", "left"));
    pad_word p;
    key(SDL_SCANCODE_J, 1); resolve(&p); assert(!(p.buttons & PSP_PAD_RT)); key(SDL_SCANCODE_J, 0);
    key(SDL_SCANCODE_COMMA, 1); resolve(&p); assert(p.buttons & PSP_PAD_RT); key(SDL_SCANCODE_COMMA, 0);
    /* Empty: unbound on that device. */
    assert(!apply_binding("mouse.fire", ""));
    assert(!bound(DEV_MOUSE, "fire", "left"));
    g_mouse_down = SDL_BUTTON(1); resolve(&p); assert(!(p.buttons & PSP_PAD_RT)); g_mouse_down = 0;
    /* A PSP button, an axis half, the analog targets. */
    assert(!apply_binding("pad.cross", "+lefty"));
    g_pad_axis[SDL_CONTROLLER_AXIS_LEFTY] = 20000; resolve(&p); assert(p.buttons & 0x4000);
    g_pad_axis[SDL_CONTROLLER_AXIS_LEFTY] = 10000; resolve(&p); assert(p.buttons & 0x4000);
    g_pad_axis[SDL_CONTROLLER_AXIS_LEFTY] = 8000; resolve(&p); assert(!(p.buttons & 0x4000));
    g_pad_axis[SDL_CONTROLLER_AXIS_LEFTY] = 0;
    assert(!apply_binding("pad.stick", "rightxy") && !apply_binding("pad.look", "leftxy"));
    g_pad_axis[SDL_CONTROLLER_AXIS_RIGHTX] = 32767; g_pad_axis[SDL_CONTROLLER_AXIS_LEFTY] = -32768;
    resolve(&p); assert(p.ax == 255 && p.ay == 128 && p.lx == 128 && p.ly == 1);
    g_pad_axis[SDL_CONTROLLER_AXIS_RIGHTX] = 0; g_pad_axis[SDL_CONTROLLER_AXIS_LEFTY] = 0;
    /* Refused whole, each leaving the table as it was. */
    const int before = g_nbinds;
    static const char *const REFUSED[][2] = {
        { "key.nonsense", "Z" }, { "pad.fire", "a, nonsense" }, { "pad.stick", "a" }, { "key.cross", "Ctrl+Z" },
        { "mouse.look", "left" }, { "mouse.stick", "motion" }, { "joystick.cross", "a" },
    };
    for (size_t i = 0; i < sizeof REFUSED / sizeof *REFUSED; i++) assert(apply_binding(REFUSED[i][0], REFUSED[i][1]));
    assert(g_nbinds == before && bound(DEV_PAD, "fire", "righttrigger"));
    /* Another title's action is not a target here. */
    setup(ARMORED_CORE, 1, 1, 1);
    assert(apply_binding("pad.fire", "a") && !apply_binding("pad.rt", "a"));
    puts("input: rebinding replaces one device's sources, empty unbinds, mistakes are refused whole");
}

static SDL_Event key_event(Uint32 type, SDL_Keycode sym, SDL_Scancode sc, Uint16 mod) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = type; e.key.state = type == SDL_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
    e.key.keysym.sym = sym; e.key.keysym.scancode = sc; e.key.keysym.mod = mod;
    return e;
}

static void host_actions(void) {
    setup(PLAIN, 0, 0, 0);
    /* Ctrl+Shift+Q quits, from the keyboard, whoever owns it; Q alone is L. */
    SDL_Event e = key_event(SDL_KEYDOWN, SDLK_q, SDL_SCANCODE_Q, KMOD_LCTRL | KMOD_RSHIFT);
    assert(input_quit_event(&e));
    e.key.repeat = 1; assert(!input_quit_event(&e)); e.key.repeat = 0;
    e.key.keysym.mod = KMOD_CTRL; assert(!input_quit_event(&e));
    e = key_event(SDL_KEYDOWN, SDLK_q, SDL_SCANCODE_Q, KMOD_LCTRL | KMOD_LSHIFT);
    assert(input_event(&e) == INPUT_ACTION_QUIT && !(published.buttons & 0x100));
    e = key_event(SDL_KEYDOWN, SDLK_q, SDL_SCANCODE_Q, 0);
    assert(input_event(&e) == INPUT_ACTION_NONE && (published.buttons & 0x100));
    e = key_event(SDL_KEYUP, SDLK_q, SDL_SCANCODE_Q, 0); input_event(&e);
    /* A controller button bound to a host action presses it once per press. */
    assert(!apply_binding("pad.fullscreen", "guide"));
    g_controller_id = 3;
    SDL_Event b;
    memset(&b, 0, sizeof b);
    b.type = SDL_CONTROLLERBUTTONDOWN; b.cbutton.which = 3; b.cbutton.button = SDL_CONTROLLER_BUTTON_GUIDE; b.cbutton.state = 1;
    assert(input_event(&b) == INPUT_ACTION_FULLSCREEN);
    assert(input_event(&b) == INPUT_ACTION_NONE);
    b.type = SDL_CONTROLLERBUTTONUP; b.cbutton.state = 0; assert(input_event(&b) == INPUT_ACTION_NONE);
    b.type = SDL_CONTROLLERBUTTONDOWN; b.cbutton.state = 1; assert(input_event(&b) == INPUT_ACTION_FULLSCREEN);
    b.type = SDL_CONTROLLERBUTTONUP; b.cbutton.state = 0; input_event(&b);
    /* Another pad's press does nothing while the first owns the lane. */
    b.type = SDL_CONTROLLERBUTTONDOWN; b.cbutton.which = 9; b.cbutton.state = 1; b.cbutton.button = SDL_CONTROLLER_BUTTON_A;
    input_event(&b); assert(!(published.buttons & 0x4000));
    g_controller_id = -1;
    puts("input: host actions press once, Ctrl+Shift+Q is not also Q, other pads are ignored");
}

static void wheel(void) {
    setup(PLAIN, 1, 0, 0);
    g_mouse_grabbed = 1;
    assert(!apply_binding("mouse.up", "wheelup") && !apply_binding("mouse.quick_save", "wheeldown"));
    SDL_Event w;
    memset(&w, 0, sizeof w);
    w.type = SDL_MOUSEWHEEL; w.wheel.y = 1;
    polls = 10;
    assert(input_event(&w) == INPUT_ACTION_NONE && (published.buttons & 0x10));
    polls = 11; input_tick(); assert(published.buttons & 0x10);
    polls = 12; input_tick(); assert(!(published.buttons & 0x10));
    w.wheel.y = -1; assert(input_event(&w) == INPUT_ACTION_QUICK_SAVE);
    w.wheel.y = 1; w.wheel.direction = SDL_MOUSEWHEEL_FLIPPED; assert(input_event(&w) == INPUT_ACTION_QUICK_SAVE);
    added_x = 0;
    SDL_Event m;
    memset(&m, 0, sizeof m);
    m.type = SDL_MOUSEMOTION; m.motion.xrel = 5;
    input_event(&m); assert(added_x == 5);
    assert(!apply_binding("mouse.look", "")); input_event(&m); assert(added_x == 5);
    g_mouse_grabbed = 0;
    puts("input: a wheel notch is a two-poll press or one host action; motion looks while bound");
}

static void ownership(void) {
    setup(PLAIN, 0, 0, 0);
    SDL_Event e = key_event(SDL_KEYDOWN, SDLK_z, SDL_SCANCODE_Z, 0);
    input_event(&e); assert(published.buttons == 0x4000);
    const int was_cleared = cleared;
    input_take(INPUT_DIALOG);
    assert(input_owner() == INPUT_DIALOG && published.buttons == 0 && published.ax == 128 && cleared == was_cleared + 1);
    assert(!g_key_down[SDL_SCANCODE_Z]);
    /* Its controls are not the game's. */
    input_event(&e); assert(published.buttons == 0 && !g_key_down[SDL_SCANCODE_Z]);
    input_return(INPUT_DIALOG);
    input_tick();   /* nothing is held on SDL's side: back at once */
    assert(input_owner() == INPUT_GAME);
    input_event(&e); assert(published.buttons == 0x4000);
    e = key_event(SDL_KEYUP, SDLK_z, SDL_SCANCODE_Z, 0); input_event(&e);
    /* Returning what one does not own does nothing. */
    input_return(INPUT_OVERLAY); input_tick(); assert(input_owner() == INPUT_GAME);
    puts("input: an owner takes the controls whole, and the game gets them back released");
}

static void from_settings(void) {
    psp_settings s;
    char error[PSP_SETTINGS_ERROR];
    psp_settings_defaults(&s);
    assert(!psp_settings_assign(&s, "KEYS=wasd", PSP_SOURCE_FILE, error));
    assert(!psp_settings_bind(&s, "key.cross", "F", error));
    assert(!psp_settings_bind(&s, "key.nonsense", "F", error));   /* the host skips it */
    assert(psp_settings_bind(&s, "keyboard.cross", "F", error));
    assert(psp_settings_bind(&s, "key.Cross", "F", error));
    assert(psp_settings_bind(&s, "key.cross", " F", error));
    assert(!psp_settings_resolve(&s, error));
    g_title = &psp_title_info;
    build(&s);
    assert(g_keys_wasd && bound(DEV_KEY, "cross", "F") && !bound(DEV_KEY, "cross", "Z") && bound(DEV_KEY, "cross", "Space") == 0);
    assert(bound(DEV_KEY, "stick_up", "scancode:W"));
    assert(!psp_settings_bind(&s, "key.cross", NULL, error) && !psp_settings_binding(&s, "key.cross"));
    build(&s);
    assert(bound(DEV_KEY, "cross", "Z") && bound(DEV_KEY, "cross", "Space"));
    puts("input: the layouts and a pack's bind.* keys build the table");
}

int main(void) {
    assert(SDL_Init(SDL_INIT_VIDEO) == 0);
    /* Keycodes are found through the keyboard's layout: there has to be one. */
    assert(SDL_GetScancodeFromKey(SDLK_z) == SDL_SCANCODE_Z);
    table_test();
    spelling();
    rebinding();
    host_actions();
    wheel();
    ownership();
    from_settings();
    SDL_Quit();
    return 0;
}
