/* Input: one owner, and bindings as data (docs/PLAYER-LAYER.md §3).
 *
 * Every control a player touches is a source: a key, a mouse button, the
 * mouse's motion or its wheel, a controller button, one half of a stick axis
 * or a trigger, or a whole stick. What it presses is a target: a PSP button,
 * a direction of the PSP stick, walk, the PSP stick or the look channel as a
 * whole, one of the title's actions -- a carrier bit its replacements decode
 * -- or a host action. A binding joins one source to one target on one
 * device (key, pad or mouse), and the pad the guest reads is worked out from
 * all of them whenever a control changes.
 *
 * The defaults are the layouts the host has always had, as tables: classic
 * and WASD for the keyboard, PSP and modern for the controller, the mouse's
 * buttons under WASD. A title adds its actions and may replace the WASD
 * table, the mouse buttons and individual controller buttons
 * (psp_title_input). A preset's bind.<device>.<target> key replaces a
 * target's sources on that device; an empty one unbinds it.
 *
 * While a device is not using the title's Modern controls, its carriers are
 * turned into each action's classic PSP buttons before the game sees them.
 *
 * Recording and replay never see any of this: the recorder takes the pad
 * this publishes, and a replay sets the pad itself.
 *
 * Everything here runs on the SDL thread. */
#include "input.h"

#include "psprecomp/host/pad.h"
#include "psprecomp/host/title.h"

#include "psprecomp/hle.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the default layouts --------------------------------------------------- */

/* SDL key -> PSP button. A button is held exactly as long as its key is.
 *
 * The classic layout is the PSP's face laid on the keyboard -- arrows for the
 * d-pad, z/x/a/s for cross/circle/square/triangle, q/e for the shoulders,
 * Return and Backspace for start and select -- and has no stick at all, so it
 * moves nothing without a pad. WASD is for a mouse player: W/A/S/D *are* the
 * left stick (digital, so a diagonal is a full push at 45 degrees), Space is
 * cross, the mouse buttons are the fire buttons -- left is square (right
 * arm), right and middle are d-pad down (left arm) -- and Q is d-pad up
 * (change weapon) on Armored Core's default assign. The letters the stick
 * took are moved: square to c, triangle to v. Which button does what in a
 * game is the game's key assign, not this table's. Keys are keycodes, found
 * where the keyboard's layout puts them; the stick is by position. */
static const psp_key_bind KEYS_CLASSIC[] = {
    { SDLK_RIGHT,      0x000020 }, { SDLK_LEFT,      0x000080 },
    { SDLK_DOWN,       0x000040 }, { SDLK_UP,        0x000010 },
    { SDLK_RETURN,     0x000008 }, { SDLK_BACKSPACE, 0x000001 },
    { SDLK_z,          0x004000 }, { SDLK_x,         0x002000 },
    { SDLK_a,          0x008000 }, { SDLK_s,         0x001000 },
    { SDLK_q,          0x000100 }, { SDLK_e,         0x000200 },
};

static const psp_key_bind KEYS_WASD[] = {
    { SDLK_RIGHT,      0x000020 }, { SDLK_LEFT,      0x000080 },
    { SDLK_DOWN,       0x000040 }, { SDLK_UP,        0x000010 },
    { SDLK_RETURN,     0x000008 }, { SDLK_BACKSPACE, 0x000001 },
    { SDLK_z,          0x004000 }, { SDLK_SPACE,     0x004000 },
    { SDLK_x,          0x002000 },
    { SDLK_c,          0x008000 }, { SDLK_v,         0x001000 },
    { SDLK_q,          0x000010 }, { SDLK_e,         0x000200 },
};

/* Mouse button -> PSP button, WASD only: SDL_BUTTON_LEFT/MIDDLE/RIGHT are
 * 1/2/3. */
static const uint32_t MOUSE_WASD[4] = { 0, 0x008000, 0x000040, 0x000040 };

/* The controller as a PSP, and the modern layout, which sends the face
 * buttons, bumpers, triggers and stick clicks as carriers (psprecomp/host/
 * pad.h) that the title's replacements decode. A title without them says so
 * in psp_title_info, and the pad speaks the PSP buttons the game already
 * understands. */
static const psp_pad_bind PAD_CLASSIC[] = {
    { SDL_CONTROLLER_BUTTON_DPAD_RIGHT,          0x000020 },
    { SDL_CONTROLLER_BUTTON_DPAD_LEFT,           0x000080 },
    { SDL_CONTROLLER_BUTTON_DPAD_DOWN,           0x000040 },
    { SDL_CONTROLLER_BUTTON_DPAD_UP,             0x000010 },
    { SDL_CONTROLLER_BUTTON_START,               0x000008 },
    { SDL_CONTROLLER_BUTTON_BACK,                0x000001 },
    { SDL_CONTROLLER_BUTTON_A,                   0x004000 },
    { SDL_CONTROLLER_BUTTON_B,                   0x002000 },
    { SDL_CONTROLLER_BUTTON_X,                   0x008000 },
    { SDL_CONTROLLER_BUTTON_Y,                   0x001000 },
    { SDL_CONTROLLER_BUTTON_LEFTSHOULDER,        0x000100 },
    { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,       0x000200 },
};
static const psp_pad_bind PAD_MODERN[] = {
    { SDL_CONTROLLER_BUTTON_DPAD_RIGHT,          0x000020 },
    { SDL_CONTROLLER_BUTTON_DPAD_LEFT,           0x000080 },
    { SDL_CONTROLLER_BUTTON_DPAD_DOWN,           0x000040 },
    { SDL_CONTROLLER_BUTTON_DPAD_UP,             0x000010 },
    { SDL_CONTROLLER_BUTTON_START,               0x000008 },
    { SDL_CONTROLLER_BUTTON_BACK,                0x000001 },
    { SDL_CONTROLLER_BUTTON_A,                   PSP_PAD_A },
    { SDL_CONTROLLER_BUTTON_B,                   PSP_PAD_B },
    { SDL_CONTROLLER_BUTTON_X,                   PSP_PAD_X },
    { SDL_CONTROLLER_BUTTON_Y,                   PSP_PAD_Y },
    { SDL_CONTROLLER_BUTTON_LEFTSHOULDER,        PSP_PAD_LB },
    { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,       PSP_PAD_RB },
    { SDL_CONTROLLER_BUTTON_LEFTSTICK,           PSP_PAD_L3 },
    { SDL_CONTROLLER_BUTTON_RIGHTSTICK,          PSP_PAD_R3 },
};

/* ---- sources, targets, bindings -------------------------------------------- */

enum { DEV_KEY, DEV_PAD, DEV_MOUSE, DEVICES };
static const char *const DEVICE_NAME[DEVICES] = { "key", "pad", "mouse" };

enum {
    SRC_KEY,            /* code: an SDL_Keycode, wherever the layout puts it */
    SRC_SCANCODE,       /* code: an SDL_Scancode, a position on the keyboard */
    SRC_BUTTON,         /* code: an SDL_GameControllerButton */
    SRC_AXIS_POS,       /* code: an SDL_GameControllerAxis, its upper half */
    SRC_AXIS_NEG,       /* ... its lower half */
    SRC_STICK,          /* code: 0 the left stick, 1 the right, both axes */
    SRC_MOUSE_BUTTON,   /* code: SDL_BUTTON_LEFT ... SDL_BUTTON_X2 */
    SRC_WHEEL,          /* code: 0 up, 1 down, 2 left, 3 right */
    SRC_MOTION,
};
enum { TGT_BUTTONS, TGT_STICK_DIR, TGT_WALK, TGT_STICK, TGT_LOOK, TGT_ACTION };
enum { DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT };

typedef struct {
    uint8_t device, src, tgt;
    uint16_t mods;      /* modifiers a key needs, for host actions */
    int32_t code;       /* the source's */
    int32_t scancode;   /* where a keycode is, as the layout last said */
    uint32_t value;     /* the target's: buttons, a direction, an action */
    uint8_t held;       /* an axis half's hysteresis, a wheel press, an action's edge */
    uint32_t until;     /* a wheel press lasts until this guest poll */
} binding;

enum { BINDINGS_MAX = 256 };
static binding g_binds[BINDINGS_MAX];
static int     g_nbinds;

/* The PSP buttons and the host's own targets, by their bind.* names. */
static const struct { const char *name; uint8_t tgt; uint32_t value; } TARGETS[] = {
    { "select", TGT_BUTTONS, 0x0001 }, { "start", TGT_BUTTONS, 0x0008 },
    { "up", TGT_BUTTONS, 0x0010 }, { "right", TGT_BUTTONS, 0x0020 },
    { "down", TGT_BUTTONS, 0x0040 }, { "left", TGT_BUTTONS, 0x0080 },
    { "l", TGT_BUTTONS, 0x0100 }, { "r", TGT_BUTTONS, 0x0200 },
    { "triangle", TGT_BUTTONS, 0x1000 }, { "circle", TGT_BUTTONS, 0x2000 },
    { "cross", TGT_BUTTONS, 0x4000 }, { "square", TGT_BUTTONS, 0x8000 },
    { "stick_up", TGT_STICK_DIR, DIR_UP }, { "stick_down", TGT_STICK_DIR, DIR_DOWN },
    { "stick_left", TGT_STICK_DIR, DIR_LEFT }, { "stick_right", TGT_STICK_DIR, DIR_RIGHT },
    { "walk", TGT_WALK, 0 }, { "stick", TGT_STICK, 0 }, { "look", TGT_LOOK, 0 },
    { "menu", TGT_ACTION, INPUT_ACTION_MENU },
    { "quick_save", TGT_ACTION, INPUT_ACTION_QUICK_SAVE },
    { "quick_load", TGT_ACTION, INPUT_ACTION_QUICK_LOAD },
    { "slot_next", TGT_ACTION, INPUT_ACTION_SLOT_NEXT },
    { "slot_prev", TGT_ACTION, INPUT_ACTION_SLOT_PREV },
    { "screenshot", TGT_ACTION, INPUT_ACTION_SCREENSHOT },
    { "release_mouse", TGT_ACTION, INPUT_ACTION_RELEASE_MOUSE },
    { "fullscreen", TGT_ACTION, INPUT_ACTION_FULLSCREEN },
    { "quit", TGT_ACTION, INPUT_ACTION_QUIT },
};
enum { TARGET_COUNT = sizeof TARGETS / sizeof *TARGETS };

const char *input_action_name(int action) {
    for (int i = 0; i < TARGET_COUNT; i++)
        if (TARGETS[i].tgt == TGT_ACTION && TARGETS[i].value == (uint32_t)action) return TARGETS[i].name;
    return "none";
}

/* ---- state ------------------------------------------------------------------ */

/* The title, its input and the layouts in force. A pointer, so the table
 * test can put fixtures in its place. */
static const psp_title *g_title = &psp_title_info;
static psp_title_input g_tin;
static int g_keys_wasd;             /* KEYS=wasd */
static int g_keys_modern;           /* the title's Modern controls, keyboard and mouse */
static int g_gamepad_modern;        /* ... and controller */

/* What the controls are doing, as SDL last said. */
static uint8_t  g_key_down[SDL_NUM_SCANCODES];
static uint32_t g_mouse_down;       /* SDL_BUTTON() bits */
static uint8_t  g_pad_down[SDL_CONTROLLER_BUTTON_MAX];
static int16_t  g_pad_axis[SDL_CONTROLLER_AXIS_MAX];

static int      g_owner = INPUT_GAME;
static int      g_returning;        /* the owner has let go; waiting for release */
static Uint64   g_return_at;
static int      g_owner_mouse_was_grabbed;

static int      g_mouse_want;       /* MOUSE=1: mouse-look */
static int      g_mouse_grabbed;

/* ---- names ------------------------------------------------------------------ */

static const char *const MOUSE_NAMES[] = { "", "left", "middle", "right", "x1", "x2" };
static const char *const WHEEL_NAMES[] = { "wheelup", "wheeldown", "wheelleft", "wheelright" };
static const struct { const char *name; uint16_t mod; } MODS[] = {
    { "Ctrl+", KMOD_CTRL }, { "Shift+", KMOD_SHIFT }, { "Alt+", KMOD_ALT }, { "Gui+", KMOD_GUI },
};

/* One source, as a bind.* value spells it on its device. */
static int parse_source(int device, const char *text, binding *b, char *why, size_t size) {
    b->mods = 0;
    if (device == DEV_KEY) {
        for (int again = 1; again;) {
            again = 0;
            for (size_t i = 0; i < sizeof MODS / sizeof *MODS; i++) {
                const size_t n = strlen(MODS[i].name);
                if (!SDL_strncasecmp(text, MODS[i].name, n) && text[n]) {
                    b->mods |= MODS[i].mod; text += n; again = 1;
                }
            }
        }
        if (!SDL_strncasecmp(text, "scancode:", 9)) {
            const SDL_Scancode sc = SDL_GetScancodeFromName(text + 9);
            if (sc == SDL_SCANCODE_UNKNOWN) { snprintf(why, size, "no key position '%s'", text + 9); return -1; }
            b->src = SRC_SCANCODE; b->code = sc; return 0;
        }
        /* SDL names the comma key ",", which a list cannot hold. */
        const SDL_Keycode k = !SDL_strcasecmp(text, "Comma") ? SDLK_COMMA : SDL_GetKeyFromName(text);
        if (k == SDLK_UNKNOWN) { snprintf(why, size, "no key '%s'", text); return -1; }
        b->src = SRC_KEY; b->code = k; return 0;
    }
    if (device == DEV_PAD) {
        if (!strcmp(text, "leftxy") || !strcmp(text, "rightxy")) {
            b->src = SRC_STICK; b->code = text[0] == 'r'; return 0;
        }
        if (text[0] == '+' || text[0] == '-') {
            const SDL_GameControllerAxis a = SDL_GameControllerGetAxisFromString(text + 1);
            if (a == SDL_CONTROLLER_AXIS_INVALID) { snprintf(why, size, "no axis '%s'", text + 1); return -1; }
            b->src = text[0] == '+' ? SRC_AXIS_POS : SRC_AXIS_NEG; b->code = a; return 0;
        }
        if (!strcmp(text, "lefttrigger") || !strcmp(text, "righttrigger")) {
            b->src = SRC_AXIS_POS; b->code = SDL_GameControllerGetAxisFromString(text); return 0;
        }
        const SDL_GameControllerButton button = SDL_GameControllerGetButtonFromString(text);
        if (button == SDL_CONTROLLER_BUTTON_INVALID) { snprintf(why, size, "no controller button '%s'", text); return -1; }
        b->src = SRC_BUTTON; b->code = button; return 0;
    }
    for (int i = 1; i < 6; i++) if (!strcmp(text, MOUSE_NAMES[i])) { b->src = SRC_MOUSE_BUTTON; b->code = i; return 0; }
    for (int i = 0; i < 4; i++) if (!strcmp(text, WHEEL_NAMES[i])) { b->src = SRC_WHEEL; b->code = i; return 0; }
    if (!strcmp(text, "motion")) { b->src = SRC_MOTION; b->code = 0; return 0; }
    snprintf(why, size, "no mouse control '%s'", text);
    return -1;
}

/* The same spelling back: what the overlay will show and write. */
static void format_source(const binding *b, char *out, size_t size) {
    char mods[32] = "";
    for (size_t i = 0; i < sizeof MODS / sizeof *MODS; i++)
        if (b->mods & MODS[i].mod) strcat(mods, MODS[i].name);
    switch (b->src) {
    case SRC_KEY:
        snprintf(out, size, "%s%s", mods, b->code == SDLK_COMMA ? "Comma" : SDL_GetKeyName(b->code)); break;
    case SRC_SCANCODE:
        snprintf(out, size, "%sscancode:%s", mods, SDL_GetScancodeName((SDL_Scancode)b->code)); break;
    case SRC_BUTTON:
        snprintf(out, size, "%s", SDL_GameControllerGetStringForButton((SDL_GameControllerButton)b->code)); break;
    case SRC_AXIS_POS: case SRC_AXIS_NEG: {
        const SDL_GameControllerAxis a = (SDL_GameControllerAxis)b->code;
        const int trigger = a == SDL_CONTROLLER_AXIS_TRIGGERLEFT || a == SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
        snprintf(out, size, "%s%s", trigger && b->src == SRC_AXIS_POS ? "" : b->src == SRC_AXIS_POS ? "+" : "-",
                 SDL_GameControllerGetStringForAxis(a));
        break;
    }
    case SRC_STICK: snprintf(out, size, "%s", b->code ? "rightxy" : "leftxy"); break;
    case SRC_MOUSE_BUTTON: snprintf(out, size, "%s", MOUSE_NAMES[b->code]); break;
    case SRC_WHEEL: snprintf(out, size, "%s", WHEEL_NAMES[b->code]); break;
    default: snprintf(out, size, "motion"); break;
    }
}

/* A target by its bind.* name: the host's, then the title's actions. */
static int parse_target(const char *name, binding *b) {
    for (int i = 0; i < TARGET_COUNT; i++)
        if (!strcmp(TARGETS[i].name, name)) { b->tgt = TARGETS[i].tgt; b->value = TARGETS[i].value; return 0; }
    for (unsigned i = 0; i < g_tin.action_count; i++)
        if (!strcmp(g_tin.actions[i].name, name)) { b->tgt = TGT_BUTTONS; b->value = g_tin.actions[i].carrier; return 0; }
    return -1;
}

/* Whether a source can press a target: the sticks and the motion only move
 * the analog targets, and only host actions take modifier keys. */
static const char *mismatch(const binding *b) {
    const int analog_src = b->src == SRC_STICK || b->src == SRC_MOTION;
    const int analog_tgt = b->tgt == TGT_STICK || b->tgt == TGT_LOOK;
    if (analog_src != analog_tgt) return analog_tgt ? "takes a whole stick (leftxy, rightxy) or the mouse's motion"
                                                    : "takes buttons, keys, axis halves or the wheel";
    if (b->src == SRC_MOTION && b->tgt != TGT_LOOK) return "the mouse's motion only looks";
    if (b->mods && b->tgt != TGT_ACTION) return "only host actions take modifier keys";
    return NULL;
}

/* ---- building the table ------------------------------------------------------ */

static void add(int device, int src, int32_t code, int tgt, uint32_t value) {
    if (g_nbinds >= BINDINGS_MAX) return;
    g_binds[g_nbinds++] = (binding){ .device = (uint8_t)device, .src = (uint8_t)src, .code = code,
                                     .tgt = (uint8_t)tgt, .value = value };
}

static int same_target(const binding *a, const binding *b) {
    return a->device == b->device && a->tgt == b->tgt && a->value == b->value;
}

/* Whether a carrier is one of the title's actions. */
static int named_carrier(uint32_t bit) {
    for (unsigned i = 0; i < g_tin.action_count; i++) if (g_tin.actions[i].carrier == bit) return 1;
    return 0;
}

static void build_defaults(void) {
    g_nbinds = 0;
    /* The keyboard. */
    const psp_key_bind *keys = KEYS_CLASSIC;
    size_t nkeys = sizeof KEYS_CLASSIC / sizeof *KEYS_CLASSIC;
    if (g_keys_wasd) {
        if (g_tin.keys_wasd) { keys = g_tin.keys_wasd; nkeys = g_tin.keys_wasd_count; }
        else { keys = KEYS_WASD; nkeys = sizeof KEYS_WASD / sizeof *KEYS_WASD; }
    }
    for (size_t i = 0; i < nkeys; i++) add(DEV_KEY, SRC_KEY, keys[i].key, TGT_BUTTONS, keys[i].bit);
    if (g_keys_wasd) {
        add(DEV_KEY, SRC_SCANCODE, SDL_SCANCODE_W, TGT_STICK_DIR, DIR_UP);
        add(DEV_KEY, SRC_SCANCODE, SDL_SCANCODE_S, TGT_STICK_DIR, DIR_DOWN);
        add(DEV_KEY, SRC_SCANCODE, SDL_SCANCODE_A, TGT_STICK_DIR, DIR_LEFT);
        add(DEV_KEY, SRC_SCANCODE, SDL_SCANCODE_D, TGT_STICK_DIR, DIR_RIGHT);
        add(DEV_KEY, SRC_SCANCODE, SDL_SCANCODE_LALT, TGT_WALK, 0);
    }
    add(DEV_KEY, SRC_KEY, SDLK_ESCAPE, TGT_ACTION, INPUT_ACTION_MENU);
    add(DEV_KEY, SRC_KEY, SDLK_q, TGT_ACTION, INPUT_ACTION_QUIT);
    g_binds[g_nbinds - 1].mods = KMOD_CTRL | KMOD_SHIFT;

    /* The mouse: its buttons under WASD, its motion whenever it is captured. */
    if (g_keys_wasd) {
        const uint32_t *mouse = g_tin.mouse_wasd ? g_tin.mouse_wasd : MOUSE_WASD;
        for (int b = 1; b < 4; b++) if (mouse[b]) add(DEV_MOUSE, SRC_MOUSE_BUTTON, b, TGT_BUTTONS, mouse[b]);
    }
    add(DEV_MOUSE, SRC_MOTION, 0, TGT_LOOK, 0);

    /* The controller: the layout, less the buttons the title takes. */
    const psp_pad_bind *map = g_gamepad_modern ? PAD_MODERN : PAD_CLASSIC;
    const size_t n = g_gamepad_modern ? sizeof PAD_MODERN / sizeof *PAD_MODERN
                                      : sizeof PAD_CLASSIC / sizeof *PAD_CLASSIC;
    for (size_t i = 0; i < n; i++) {
        int taken = 0;
        for (unsigned t = 0; t < g_tin.pad_count; t++) taken |= g_tin.pad[t].button == map[i].button;
        if (taken || ((map[i].bit & PSP_PAD_EXTRA) && !named_carrier(map[i].bit))) continue;
        add(DEV_PAD, SRC_BUTTON, map[i].button, TGT_BUTTONS, map[i].bit);
    }
    for (unsigned t = 0; t < g_tin.pad_count; t++)
        add(DEV_PAD, SRC_BUTTON, g_tin.pad[t].button, TGT_BUTTONS, g_tin.pad[t].bit);
    /* SDL's GameController layer normalises the triggers to 0..32767. */
    if (g_gamepad_modern && named_carrier(PSP_PAD_LT))
        add(DEV_PAD, SRC_AXIS_POS, SDL_CONTROLLER_AXIS_TRIGGERLEFT, TGT_BUTTONS, PSP_PAD_LT);
    if (g_gamepad_modern && named_carrier(PSP_PAD_RT))
        add(DEV_PAD, SRC_AXIS_POS, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, TGT_BUTTONS, PSP_PAD_RT);
    add(DEV_PAD, SRC_STICK, 0, TGT_STICK, 0);
    add(DEV_PAD, SRC_STICK, 1, TGT_LOOK, 0);
}

/* bind.<device>.<target>=<source>, <source>... replaces the target's sources
 * on that device. Anything wrong is reported and skipped: a mistyped key in a
 * preset must not keep the game from starting. */
static int apply_binding(const char *key, const char *value) {
    char why[160] = "";
    int device = -1;
    for (int d = 0; d < DEVICES; d++) {
        const size_t n = strlen(DEVICE_NAME[d]);
        if (!strncmp(key, DEVICE_NAME[d], n) && key[n] == '.') device = d;
    }
    binding t = { 0 };
    if (device < 0 || parse_target(strchr(key, '.') + 1, &t)) {
        fprintf(stderr, "present: bind.%s: not a target here -- ignored\n", key);
        return -1;
    }
    /* All of it, or none. */
    binding parsed[16];
    int count = 0;
    const char *at = value;
    while (*at) {
        while (*at == ' ') at++;
        size_t n = strcspn(at, ",");
        char token[PSP_BIND_VALUE];
        snprintf(token, sizeof token, "%.*s", (int)n, at);
        for (size_t k = strlen(token); k && token[k - 1] == ' '; k--) token[k - 1] = 0;
        at += n; if (*at == ',') at++;
        if (!*token) continue;
        binding b = t;
        b.device = (uint8_t)device;
        if (parse_source(device, token, &b, why, sizeof why) == 0) {
            const char *bad = mismatch(&b);
            if (bad) snprintf(why, sizeof why, "%s: %s", token, bad);
        }
        if (*why || count >= 16) {
            fprintf(stderr, "present: bind.%s=%s: %s -- ignored\n", key, value, *why ? why : "too many sources");
            return -1;
        }
        parsed[count++] = b;
    }
    int kept = 0;
    t.device = (uint8_t)device;
    for (int i = 0; i < g_nbinds; i++) if (!same_target(&g_binds[i], &t)) g_binds[kept++] = g_binds[i];
    g_nbinds = kept;
    for (int i = 0; i < count && g_nbinds < BINDINGS_MAX; i++) g_binds[g_nbinds++] = parsed[i];
    return 0;
}

/* Where each keycode is on the keyboard: at the start, and again whenever
 * the layout changes. */
static void find_keys(void) {
    for (int i = 0; i < g_nbinds; i++)
        if (g_binds[i].src == SRC_KEY) g_binds[i].scancode = SDL_GetScancodeFromKey(g_binds[i].code);
}

/* ---- working out the pad ------------------------------------------------------ */

static uint8_t axis_byte(int16_t value) {
    /* The whole SDL range. The title's control profile applies one radial
     * deadzone later, after recording; an axial cut here as well made
     * cardinal movement need half a real stick and bent diagonals. */
    const int32_t v = value;
    if (v >= 0) return (uint8_t)(128 + v * 127 / 32767);
    return (uint8_t)(128 + v * 127 / 32768);
}

static int down(binding *b) {
    switch (b->src) {
    case SRC_KEY: return b->scancode != SDL_SCANCODE_UNKNOWN && g_key_down[b->scancode];
    case SRC_SCANCODE: return g_key_down[b->code];
    case SRC_BUTTON: return g_pad_down[b->code];
    case SRC_AXIS_POS: case SRC_AXIS_NEG: {
        /* Separate 50%/25% press and release points avoid chatter around
         * the actuation point without making a light touch sticky. */
        const int v = b->src == SRC_AXIS_POS ? g_pad_axis[b->code] : -(int)g_pad_axis[b->code];
        if (!b->held && v >= 16384) b->held = 1;
        if ( b->held && v <=  8192) b->held = 0;
        return b->held;
    }
    case SRC_MOUSE_BUTTON: return (g_mouse_down & SDL_BUTTON(b->code)) != 0;
    case SRC_WHEEL: return b->held;
    default: return 0;
    }
}

/* A device's carriers as the game sees them while the title's Modern
 * controls are off for it: each action's classic PSP buttons. */
static uint32_t classic(uint32_t buttons) { return psp_title_classic(&g_tin, buttons); }

/* The keyboard's stick byte for one axis: along and across are -1, 0 or 1. */
static uint8_t key_axis(int along, int across, int walk) { return psp_title_key_axis(&g_tin, along, across, walk); }

typedef struct { uint32_t buttons; uint8_t ax, ay, lx, ly; } pad_word;

static void stick_bytes(int stick, uint8_t *x, uint8_t *y) {
    *x = axis_byte(g_pad_axis[stick ? SDL_CONTROLLER_AXIS_RIGHTX : SDL_CONTROLLER_AXIS_LEFTX]);
    *y = axis_byte(g_pad_axis[stick ? SDL_CONTROLLER_AXIS_RIGHTY : SDL_CONTROLLER_AXIS_LEFTY]);
}

static void resolve(pad_word *out) {
    uint32_t buttons[DEVICES] = { 0 };
    int dir[4] = { 0 }, walk = 0;
    uint8_t sx = 128, sy = 128, lx = 128, ly = 128;
    for (int i = 0; i < g_nbinds; i++) {
        binding *b = &g_binds[i];
        switch (b->tgt) {
        case TGT_BUTTONS:   if (down(b)) buttons[b->device] |= b->value; break;
        case TGT_STICK_DIR: if (down(b)) dir[b->value] = 1; break;
        case TGT_WALK:      if (down(b)) walk = 1; break;
        case TGT_STICK: case TGT_LOOK: {
            if (b->src != SRC_STICK) break;
            uint8_t x, y;
            stick_bytes(b->code, &x, &y);
            /* Two sticks on one target: the one pushed further. */
            uint8_t *tx = b->tgt == TGT_STICK ? &sx : &lx, *ty = b->tgt == TGT_STICK ? &sy : &ly;
            if (abs(x - 128) + abs(y - 128) > abs(*tx - 128) + abs(*ty - 128)) { *tx = x; *ty = y; }
            break;
        }
        default: break;
        }
    }
    if (!g_keys_modern) { buttons[DEV_KEY] = classic(buttons[DEV_KEY]); buttons[DEV_MOUSE] = classic(buttons[DEV_MOUSE]); }
    if (!g_gamepad_modern) buttons[DEV_PAD] = classic(buttons[DEV_PAD]);
    out->buttons = buttons[DEV_KEY] | buttons[DEV_MOUSE] | buttons[DEV_PAD];
    /* The keys win on any axis they hold, even centred: a pad left plugged in
     * reports its resting stick whenever it drifts, and one that drifted on Y
     * alone made W and S dead while A and D walked (5 Sep). Opposite keys
     * centre the axis rather than hand it back to a drifting stick. */
    const int x = dir[DIR_RIGHT] - dir[DIR_LEFT], y = dir[DIR_DOWN] - dir[DIR_UP];
    out->ax = dir[DIR_LEFT] || dir[DIR_RIGHT] ? key_axis(x, y, walk) : sx;
    out->ay = dir[DIR_UP] || dir[DIR_DOWN] ? key_axis(y, x, walk) : sy;
    out->lx = lx; out->ly = ly;
}

/* Publish the whole pad after any change, so a guest poll between two
 * events of one press never sees it half applied. Cheap. */
static void publish(void) {
    if (g_owner != INPUT_GAME) {
        psp_ctrl_set(0, 128, 128);
        psp_ctrl_set_look(128, 128);
        return;
    }
    pad_word p;
    resolve(&p);
    psp_ctrl_set(p.buttons, p.ax, p.ay);
    psp_ctrl_set_look(p.lx, p.ly);
}

/* ---- the mouse ------------------------------------------------------------------ */

/* Capture the pointer for mouse-look, or let it go. Only ever when asked for
 * with MOUSE=1; and even then focus loss and Escape release it, because a
 * window that traps the pointer and cannot be left is the one thing worse
 * than no mouse-look at all. A click takes it back. */
static void mouse_grab(int on) {
    if (!g_mouse_want || on == g_mouse_grabbed) return;
    SDL_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE);
    g_mouse_grabbed = on;
    if (!on) {
        psp_ctrl_clear_mouse();
        /* A button-up outside capture is intentionally ignored. Clear the
         * old held set now so Escape cannot leave fire/aim stuck on. */
        g_mouse_down = 0;
        publish();
    }
    fprintf(stderr, on ? "present: mouse captured -- Escape releases it\n"
                       : "present: mouse released -- click the window to capture it\n");
}

/* ---- the menu chord ---------------------------------------------------------------- */

/* Physical View/Back + Menu/Start on the active pad, pressed together, opens
 * the menu (docs/PLAYER-LAYER.md, decided 4 Oct): the press that completes the
 * pair. It was a two-second hold that quit; Quit is in the menu now. Kept
 * apart from the bindings, the guest and replays. Both buttons still reach
 * the game until the menu takes the controls. */
static struct { unsigned buttons; int fired; } g_chord;

static int chord_button(uint8_t button, int is_down) {
    const unsigned bit = button == SDL_CONTROLLER_BUTTON_BACK ? 1u :
                         button == SDL_CONTROLLER_BUTTON_START ? 2u : 0u;
    if (!bit) return 0;
    if (is_down) g_chord.buttons |= bit;
    else g_chord.buttons &= ~bit;
    if (g_chord.buttons != 3) { g_chord.fired = 0; return 0; }
    if (g_chord.fired) return 0;
    g_chord.fired = 1;
    return 1;
}

/* ---- controllers -------------------------------------------------------------------- */

/* One controller owns the pad lane. Opening every connected pad used to let
 * an idle second one overwrite the one in the hand, so by default only the
 * first is opened. ACTIVE_PAD=last opens them all and hands the lane to
 * whichever was pressed last. */
enum { PADS_MAX = 8 };
static SDL_GameController *g_pads[PADS_MAX];
static SDL_JoystickID      g_pad_ids[PADS_MAX];
static int                 g_npads;
static SDL_GameController *g_controller;
static SDL_JoystickID      g_controller_id = -1;
static int                 g_pad_last;

SDL_GameController *input_pad(void) { return g_controller; }
SDL_JoystickID input_pad_id(void) { return g_controller_id; }

static void clear_controller(void) {
    memset(&g_chord, 0, sizeof g_chord);
    memset(g_pad_down, 0, sizeof g_pad_down);
    memset(g_pad_axis, 0, sizeof g_pad_axis);
    for (int i = 0; i < g_nbinds; i++)
        if (g_binds[i].src == SRC_AXIS_POS || g_binds[i].src == SRC_AXIS_NEG) g_binds[i].held = 0;
}

static void clear_keys(void) { memset(g_key_down, 0, sizeof g_key_down); }

static void sample_controller(void) {
    if (!g_controller) return;
    for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++) {
        g_pad_down[b] = SDL_GameControllerGetButton(g_controller, (SDL_GameControllerButton)b) != 0;
        chord_button((uint8_t)b, g_pad_down[b]);
    }
    /* A pair already held when the pad is sampled is not a press. */
    g_chord.fired = g_chord.buttons == 3;
    for (int a = 0; a < SDL_CONTROLLER_AXIS_MAX; a++)
        g_pad_axis[a] = SDL_GameControllerGetAxis(g_controller, (SDL_GameControllerAxis)a);
}

static void activate(int i, const char *how) {
    g_controller = g_pads[i];
    g_controller_id = g_pad_ids[i];
    clear_controller();
    sample_controller();
    fprintf(stderr, "present: controller \"%s\" %s -- %s buttons, radial stick filtering, keyboard overrides per axis\n",
            SDL_GameControllerName(g_controller) ? SDL_GameControllerName(g_controller) : "?", how,
            g_gamepad_modern ? "modern" : "PSP");
    publish();
}

static int open_controller(int device_index) {
    if ((g_controller && !g_pad_last) || g_npads >= PADS_MAX || !SDL_IsGameController(device_index)) return 0;
    SDL_GameController *c = SDL_GameControllerOpen(device_index);
    if (!c) {
        fprintf(stderr, "present: cannot open controller %d: %s\n", device_index, SDL_GetError());
        return 0;
    }
    SDL_Joystick *joy = SDL_GameControllerGetJoystick(c);
    const SDL_JoystickID id = joy ? SDL_JoystickInstanceID(joy) : -1;
    if (id < 0) { SDL_GameControllerClose(c); return 0; }
    for (int i = 0; i < g_npads; i++)
        if (g_pad_ids[i] == id) { SDL_GameControllerClose(c); return 0; }   /* already open */
    g_pads[g_npads] = c;
    g_pad_ids[g_npads] = id;
    g_npads++;
    if (!g_controller) activate(g_npads - 1, "opened as the active pad");
    return 1;
}

static void open_controllers(void) {
    const int n = SDL_NumJoysticks();
    for (int i = 0; i < n && (!g_controller || g_pad_last); i++) open_controller(i);
}

static int pad_slot(SDL_JoystickID id) {
    for (int i = 0; i < g_npads; i++) if (g_pad_ids[i] == id) return i;
    return -1;
}

static void controller_removed(SDL_JoystickID id) {
    const int i = pad_slot(id);
    if (i < 0) return;
    SDL_GameControllerClose(g_pads[i]);
    memmove(&g_pads[i], &g_pads[i + 1], (size_t)(g_npads - i - 1) * sizeof *g_pads);
    memmove(&g_pad_ids[i], &g_pad_ids[i + 1], (size_t)(g_npads - i - 1) * sizeof *g_pad_ids);
    g_npads--;
    if (id != g_controller_id) return;
    fprintf(stderr, "present: active controller removed; input cleared\n");
    g_controller = NULL;
    g_controller_id = -1;
    clear_controller();
    if (g_npads) activate(0, "is now the active pad");
    else open_controllers();
    publish();
}

/* ACTIVE_PAD=last: a press on another open pad hands it the lane. A stick
 * or trigger counts from halfway, so a resting stick's drift never does. */
static int pressed_elsewhere(const SDL_Event *e) {
    if (!g_pad_last) return 0;
    SDL_JoystickID id;
    if (e->type == SDL_CONTROLLERBUTTONDOWN) id = e->cbutton.which;
    else if (e->type == SDL_CONTROLLERAXISMOTION && abs(e->caxis.value) >= 16384) id = e->caxis.which;
    else return 0;
    if (id == g_controller_id) return 0;
    const int i = pad_slot(id);
    if (i < 0) return 0;
    activate(i, "is now the active pad");
    return 1;
}

/* ---- ownership ------------------------------------------------------------------------ */

int input_owner(void) { return g_owner; }

static void let_go(void) {
    clear_keys();
    clear_controller();
    g_mouse_down = 0;
    for (int i = 0; i < g_nbinds; i++) if (g_binds[i].src == SRC_WHEEL || g_binds[i].tgt == TGT_ACTION) g_binds[i].held = 0;
}

void input_take(int owner) {
    if (owner == g_owner && !g_returning) return;
    if (g_owner == INPUT_GAME) g_owner_mouse_was_grabbed = g_mouse_grabbed;
    g_owner = owner;
    g_returning = 0;
    g_return_at = 0;
    mouse_grab(0);
    let_go();
    psp_ctrl_clear_mouse();
    publish();
}

void input_return(int owner) {
    if (owner != g_owner || g_returning) return;
    g_returning = 1;
    g_return_at = 0;
}

/* Nothing held anywhere: every key, mouse button and button of the pad. */
static int neutral(void) {
    int n;
    const Uint8 *keys = SDL_GetKeyboardState(&n);
    for (int i = 0; i < n; i++) if (keys[i]) return 0;
    if (SDL_GetMouseState(NULL, NULL)) return 0;
    if (g_controller)
        for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++)
            if (SDL_GameControllerGetButton(g_controller, (SDL_GameControllerButton)i)) return 0;
    return 1;
}

/* The controls come back once released, or after a second regardless: a key
 * or button whose release SDL never reports must not leave the game deaf. */
static void returning(void) {
    if (!g_returning) return;
    const Uint64 now = SDL_GetTicks64();
    if (!g_return_at) g_return_at = now;
    const int calm = neutral();
    if (!calm && now - g_return_at < 1000) return;
    if (!calm)
        fprintf(stderr, "present: input still held a second after the %s closed; resuming game input\n",
                g_owner == INPUT_DIALOG ? "save dialog" : "menu");
    g_returning = 0;
    g_return_at = 0;
    let_go();
    sample_controller();
    if (g_owner_mouse_was_grabbed) mouse_grab(1);
    g_owner_mouse_was_grabbed = 0;
    /* The game's again only now, with everything in place. */
    g_owner = INPUT_GAME;
    publish();
}

/* ---- events -------------------------------------------------------------------------- */

static int key_matches(const binding *b, const SDL_Keysym *k) {
    if (b->src == SRC_KEY ? b->code != k->sym : b->src != SRC_SCANCODE || b->code != (int32_t)k->scancode)
        return 0;
    for (size_t i = 0; i < sizeof MODS / sizeof *MODS; i++)
        if ((b->mods & MODS[i].mod) && !(k->mod & MODS[i].mod)) return 0;
    return 1;
}

int input_quit_event(const SDL_Event *e) {
    if (e->type != SDL_KEYDOWN || e->key.repeat) return 0;
    for (int i = 0; i < g_nbinds; i++)
        if (g_binds[i].tgt == TGT_ACTION && g_binds[i].value == INPUT_ACTION_QUIT && key_matches(&g_binds[i], &e->key.keysym))
            return 1;
    return 0;
}

void input_chord_event(const SDL_Event *e) {
    if ((e->type == SDL_CONTROLLERBUTTONDOWN || e->type == SDL_CONTROLLERBUTTONUP) && e->cbutton.which == g_controller_id)
        chord_button(e->cbutton.button, e->cbutton.state);
}

/* Host actions press on the way down, once. One bound to a key with
 * modifiers takes the key event whole, so Ctrl+Shift+Q is not also Q. */
static int actions(void) {
    int fired = INPUT_ACTION_NONE;
    for (int i = 0; i < g_nbinds; i++) {
        binding *b = &g_binds[i];
        if (b->tgt != TGT_ACTION || b->mods) continue;
        const int now = b->src == SRC_WHEEL ? b->held : down(b);
        if (b->src == SRC_WHEEL) { if (now) { b->held = 0; fired = (int)b->value; } continue; }
        if (now && !b->held && !fired) fired = (int)b->value;
        b->held = (uint8_t)now;
    }
    return fired;
}

static int carry_out(int action) {
    if (action == INPUT_ACTION_RELEASE_MOUSE) { mouse_grab(0); return INPUT_ACTION_NONE; }
    return action;
}

static int input_types(Uint32 type) {
    return type == SDL_KEYDOWN || type == SDL_KEYUP || type == SDL_MOUSEMOTION ||
           type == SDL_MOUSEBUTTONDOWN || type == SDL_MOUSEBUTTONUP || type == SDL_MOUSEWHEEL ||
           type == SDL_CONTROLLERBUTTONDOWN || type == SDL_CONTROLLERBUTTONUP ||
           type == SDL_CONTROLLERAXISMOTION;
}

int input_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_CONTROLLERDEVICEADDED:
        open_controller(e->cdevice.which);
        return INPUT_ACTION_NONE;
    case SDL_CONTROLLERDEVICEREMOVED:
        controller_removed(e->cdevice.which);
        return INPUT_ACTION_NONE;
    case SDL_KEYMAPCHANGED:
        find_keys();
        publish();
        return INPUT_ACTION_NONE;
    case SDL_WINDOWEVENT:
        if (e->window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            mouse_grab(0);
            let_go();
            publish();
        } else if (e->window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
            /* SDL need not report the controls that stayed held while
             * another window had focus. */
            sample_controller();
            publish();
        }
        return INPUT_ACTION_NONE;
    default: break;
    }
    if (!input_types(e->type)) return INPUT_ACTION_NONE;
    if (g_owner != INPUT_GAME) { input_chord_event(e); return INPUT_ACTION_NONE; }
    int menu = 0;

    switch (e->type) {
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP:
        if (e->cbutton.which != g_controller_id && !pressed_elsewhere(e)) return INPUT_ACTION_NONE;
        menu = chord_button(e->cbutton.button, e->cbutton.state);
        if (e->cbutton.button < SDL_CONTROLLER_BUTTON_MAX) g_pad_down[e->cbutton.button] = e->cbutton.state != 0;
        break;
    case SDL_CONTROLLERAXISMOTION:
        if (e->caxis.which != g_controller_id && !pressed_elsewhere(e)) return INPUT_ACTION_NONE;
        if (e->caxis.axis < SDL_CONTROLLER_AXIS_MAX) g_pad_axis[e->caxis.axis] = e->caxis.value;
        break;
    case SDL_MOUSEMOTION:
        /* Relative motion only while captured: a pointer crossing an
         * uncaptured window is not a look. */
        if (g_mouse_grabbed)
            for (int i = 0; i < g_nbinds; i++)
                if (g_binds[i].src == SRC_MOTION) { psp_ctrl_add_mouse(e->motion.xrel, e->motion.yrel); break; }
        return INPUT_ACTION_NONE;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        /* The click that captures the pointer is not also a shot. */
        if (!g_mouse_grabbed) { if (e->type == SDL_MOUSEBUTTONDOWN) mouse_grab(1); return INPUT_ACTION_NONE; }
        if (e->button.button >= 1 && e->button.button <= 5) {
            if (e->type == SDL_MOUSEBUTTONDOWN) g_mouse_down |= SDL_BUTTON(e->button.button);
            else g_mouse_down &= ~SDL_BUTTON(e->button.button);
        }
        break;
    case SDL_MOUSEWHEEL: {
        if (!g_mouse_grabbed) return INPUT_ACTION_NONE;
        int y = e->wheel.y, x = e->wheel.x;
        if (e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED) { y = -y; x = -x; }
        const int which = y > 0 ? 0 : y < 0 ? 1 : x < 0 ? 2 : x > 0 ? 3 : -1;
        /* A wheel notch is a press the game sees for two of its polls. */
        for (int i = 0; i < g_nbinds; i++)
            if (g_binds[i].src == SRC_WHEEL && g_binds[i].code == which) {
                g_binds[i].held = 1;
                g_binds[i].until = psp_ctrl_polls() + 2;
            }
        break;
    }
    case SDL_KEYDOWN:
    case SDL_KEYUP:
        if (e->type == SDL_KEYDOWN && !e->key.repeat)
            for (int i = 0; i < g_nbinds; i++)
                if (g_binds[i].tgt == TGT_ACTION && g_binds[i].mods && key_matches(&g_binds[i], &e->key.keysym))
                    return carry_out((int)g_binds[i].value);
        if (e->key.keysym.scancode > SDL_SCANCODE_UNKNOWN && e->key.keysym.scancode < SDL_NUM_SCANCODES)
            g_key_down[e->key.keysym.scancode] = e->key.state == SDL_PRESSED;
        break;
    }
    const int action = carry_out(actions());
    publish();
    return menu ? INPUT_ACTION_MENU : action;
}

void input_tick(void) {
    returning();
    int ended = 0;
    for (int i = 0; i < g_nbinds; i++)
        if (g_binds[i].src == SRC_WHEEL && g_binds[i].held && g_binds[i].tgt != TGT_ACTION &&
            (int32_t)(psp_ctrl_polls() - g_binds[i].until) >= 0) {
            g_binds[i].held = 0;
            ended = 1;
        }
    if (ended) publish();
}

/* ---- the menu's bindings page ------------------------------------------------------------ */

/* What the page lists, in order: the PSP's buttons, the stick and walk, the
 * title's actions, and the host actions that do something today. */
static const struct { const char *name, *label; } HOST_ROWS[] = {
    { "up", "Up" }, { "down", "Down" }, { "left", "Left" }, { "right", "Right" },
    { "cross", "Cross" }, { "circle", "Circle" }, { "square", "Square" }, { "triangle", "Triangle" },
    { "l", "L" }, { "r", "R" }, { "start", "Start" }, { "select", "Select" },
    { "stick_up", "Stick up" }, { "stick_down", "Stick down" }, { "stick_left", "Stick left" },
    { "stick_right", "Stick right" }, { "walk", "Walk" },
};
static const struct { const char *name, *label; } HOST_ACTION_ROWS[] = {
    { "menu", "Menu" }, { "fullscreen", "Fullscreen" }, { "release_mouse", "Release the mouse" }, { "quit", "Quit" },
};
enum { HOST_ROW_COUNT = sizeof HOST_ROWS / sizeof *HOST_ROWS,
       HOST_ACTION_ROW_COUNT = sizeof HOST_ACTION_ROWS / sizeof *HOST_ACTION_ROWS };

int input_rows(void) { return HOST_ROW_COUNT + (int)g_tin.action_count + HOST_ACTION_ROW_COUNT; }

static const char *row_name(int row) {
    if (row < HOST_ROW_COUNT) return HOST_ROWS[row].name;
    row -= HOST_ROW_COUNT;
    if (row < (int)g_tin.action_count) return g_tin.actions[row].name;
    return HOST_ACTION_ROWS[row - (int)g_tin.action_count].name;
}

const char *input_row_label(int row) {
    if (row < HOST_ROW_COUNT) return HOST_ROWS[row].label;
    row -= HOST_ROW_COUNT;
    if (row < (int)g_tin.action_count) return g_tin.actions[row].label;
    return HOST_ACTION_ROWS[row - (int)g_tin.action_count].label;
}

/* The key on the keyboard a source is, for telling two spellings apart. */
static int32_t physical(const binding *b) { return b->src == SRC_KEY ? b->scancode : b->code; }
static int same_source(const binding *a, const binding *b) {
    const int key_a = a->src == SRC_KEY || a->src == SRC_SCANCODE, key_b = b->src == SRC_KEY || b->src == SRC_SCANCODE;
    if (key_a || key_b) return key_a && key_b && physical(a) == physical(b) && a->mods == b->mods;
    return a->src == b->src && a->code == b->code;
}

/* A source as the menu shows it: SDL's names are for the settings file. */
static void describe_source(const binding *b, char *out, size_t size) {
    static const struct { const char *sdl, *shown; } PAD[] = {
        { "a", "A" }, { "b", "B" }, { "x", "X" }, { "y", "Y" }, { "back", "View" }, { "guide", "Guide" },
        { "start", "Menu" }, { "leftstick", "L3" }, { "rightstick", "R3" }, { "leftshoulder", "LB" },
        { "rightshoulder", "RB" }, { "dpup", "D-pad up" }, { "dpdown", "D-pad down" },
        { "dpleft", "D-pad left" }, { "dpright", "D-pad right" }, { "misc1", "Share" },
        { "paddle1", "P1" }, { "paddle2", "P2" }, { "paddle3", "P3" }, { "paddle4", "P4" },
        { "touchpad", "Touchpad" }, { "lefttrigger", "LT" }, { "righttrigger", "RT" },
        { "+leftx", "Left stick right" }, { "-leftx", "Left stick left" },
        { "+lefty", "Left stick down" }, { "-lefty", "Left stick up" },
        { "+rightx", "Right stick right" }, { "-rightx", "Right stick left" },
        { "+righty", "Right stick down" }, { "-righty", "Right stick up" },
        { "leftxy", "Left stick" }, { "rightxy", "Right stick" },
    };
    static const char *const MOUSE_SHOWN[] = { "", "Left button", "Middle button", "Right button",
                                               "Back button", "Forward button" };
    static const char *const WHEEL_SHOWN[] = { "Wheel up", "Wheel down", "Wheel left", "Wheel right" };
    char spelled[64];
    format_source(b, spelled, sizeof spelled);
    if (b->src == SRC_SCANCODE) {
        snprintf(out, size, "%s", spelled);
        char *at = strstr(out, "scancode:");
        if (at) memmove(at, at + 9, strlen(at + 9) + 1);
        return;
    }
    if (b->src == SRC_MOUSE_BUTTON) { snprintf(out, size, "%s", MOUSE_SHOWN[b->code]); return; }
    if (b->src == SRC_WHEEL) { snprintf(out, size, "%s", WHEEL_SHOWN[b->code]); return; }
    if (b->src == SRC_MOTION) { snprintf(out, size, "Motion"); return; }
    if (b->device == DEV_PAD)
        for (size_t i = 0; i < sizeof PAD / sizeof *PAD; i++)
            if (!strcmp(PAD[i].sdl, spelled)) { snprintf(out, size, "%s", PAD[i].shown); return; }
    snprintf(out, size, "%s", spelled);
}

/* A target's sources on a device, joined as a bind.* value spells them;
 * skip one equal to `without`. */
static void sources_of(const binding *t, int device, const binding *without, char *out, size_t size) {
    *out = 0;
    for (int i = 0; i < g_nbinds; i++) {
        const binding *b = &g_binds[i];
        if (b->device != device || b->tgt != t->tgt || b->value != t->value) continue;
        if (without && same_source(b, without)) continue;
        char one[64];
        format_source(b, one, sizeof one);
        if (strlen(out) + strlen(one) + 3 < size) { if (*out) strcat(out, ", "); strcat(out, one); }
    }
}

void input_row_sources(int row, int device, char *out, size_t size) {
    binding t = { 0 };
    *out = 0;
    if (row < 0 || row >= input_rows() || parse_target(row_name(row), &t)) return;
    for (int i = 0; i < g_nbinds; i++) {
        const binding *b = &g_binds[i];
        if (b->device != device || b->tgt != t.tgt || b->value != t.value) continue;
        char one[64];
        describe_source(b, one, sizeof one);
        if (strlen(out) + strlen(one) + 3 < size) { if (*out) strcat(out, ", "); strcat(out, one); }
    }
}

int input_assign(psp_settings *s, int row, int device, const char *source) {
    char key[PSP_BIND_KEY], error[PSP_SETTINGS_ERROR];
    binding t = { 0 }, src = { 0 };
    if (row < 0 || row >= input_rows() || device < 0 || device >= DEVICES || parse_target(row_name(row), &t)) return -1;
    snprintf(key, sizeof key, "%s.%s", DEVICE_NAME[device], row_name(row));
    if (!source) return psp_settings_bind(s, key, "", error);
    char why[160];
    src.tgt = t.tgt; src.value = t.value;
    if (parse_source(device, source, &src, why, sizeof why) || mismatch(&src)) return -1;
    if (src.src == SRC_KEY) src.scancode = SDL_GetScancodeFromKey(src.code);
    if (psp_settings_bind(s, key, source, error)) return -1;
    /* A control does one thing: it leaves whatever else it pressed here. */
    for (int r = 0; r < input_rows(); r++) {
        binding other = { 0 };
        if (r == row || parse_target(row_name(r), &other)) continue;
        int has = 0;
        for (int i = 0; i < g_nbinds; i++)
            has |= g_binds[i].device == device && g_binds[i].tgt == other.tgt && g_binds[i].value == other.value &&
                   same_source(&g_binds[i], &src);
        if (!has) continue;
        char rest[PSP_BIND_VALUE];
        sources_of(&other, device, &src, rest, sizeof rest);
        snprintf(key, sizeof key, "%s.%s", DEVICE_NAME[device], row_name(r));
        if (psp_settings_bind(s, key, rest, error)) return -1;
    }
    return 0;
}

int input_spell(const SDL_Event *e, int device, char *out, size_t size) {
    switch (e->type) {
    case SDL_KEYDOWN:
        if (device != DEV_KEY || e->key.repeat) return 0;
        snprintf(out, size, "%s", e->key.keysym.sym == SDLK_COMMA ? "Comma" : SDL_GetKeyName(e->key.keysym.sym));
        return *out != 0;
    case SDL_CONTROLLERBUTTONDOWN:
        if (device != DEV_PAD) return 0;
        snprintf(out, size, "%s", SDL_GameControllerGetStringForButton((SDL_GameControllerButton)e->cbutton.button));
        return 1;
    case SDL_CONTROLLERAXISMOTION: {
        if (device != DEV_PAD || abs(e->caxis.value) < 16384) return 0;
        const SDL_GameControllerAxis a = (SDL_GameControllerAxis)e->caxis.axis;
        const int trigger = a == SDL_CONTROLLER_AXIS_TRIGGERLEFT || a == SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
        if (trigger && e->caxis.value < 0) return 0;
        snprintf(out, size, "%s%s", trigger ? "" : e->caxis.value > 0 ? "+" : "-", SDL_GameControllerGetStringForAxis(a));
        return 1;
    }
    case SDL_MOUSEBUTTONDOWN:
        if (device != DEV_MOUSE || e->button.button < 1 || e->button.button > 5) return 0;
        snprintf(out, size, "%s", MOUSE_NAMES[e->button.button]);
        return 1;
    case SDL_MOUSEWHEEL: {
        if (device != DEV_MOUSE) return 0;
        int y = e->wheel.y, x = e->wheel.x;
        if (e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED) { y = -y; x = -x; }
        const int which = y > 0 ? 0 : y < 0 ? 1 : x < 0 ? 2 : x > 0 ? 3 : -1;
        if (which < 0) return 0;
        snprintf(out, size, "%s", WHEEL_NAMES[which]);
        return 1;
    }
    default: return 0;
    }
}

void input_release_mouse(void) { mouse_grab(0); }

/* ---- starting -------------------------------------------------------------------------- */

static double option_value(const psp_settings *s, const char *key, double absent) {
    const int id = psp_settings_find(key);
    return id >= 0 ? s->number[id] : absent;
}

/* The modern layout's carriers only mean something to a title whose
 * replacements read them. */
static int gamepad_modern(const psp_settings *s) {
    if (!s->gamepad) return 0;
    if (g_title->capabilities & PSP_TITLE_MODERN_CONTROLS) return 1;
    fprintf(stderr, "present: this title has no native replacements for the modern "
                    "controller layout -- using the classic PSP buttons instead\n");
    return 0;
}

/* The keyboard layout, the active pad and the bindings, from settings the
 * menu changed: what the host can apply while the game runs. The title's
 * input and its Modern controls stay as they started. */
void input_rebuild(const psp_settings *s) {
    g_keys_wasd = option_value(s, "KEYS", 0) != 0;
    const int was_last = g_pad_last;
    g_pad_last = option_value(s, "ACTIVE_PAD", 0) != 0;
    build_defaults();
    for (int i = 0; i < s->bind_count; i++) apply_binding(s->bind[i].key, s->bind[i].value);
    find_keys();
    if (g_pad_last && !was_last) open_controllers();
    publish();
}

/* The layouts, the title's input and the preset's bindings, as a table. */
static void build(const psp_settings *s) {
    memset(&g_tin, 0, sizeof g_tin);
    if (g_title->input) g_title->input(s, &g_tin);
    g_keys_wasd = option_value(s, "KEYS", 0) != 0;
    g_gamepad_modern = gamepad_modern(s);
    g_keys_modern = s->input && (g_title->capabilities & PSP_TITLE_MODERN_CONTROLS);
    build_defaults();
    for (int i = 0; i < s->bind_count; i++) apply_binding(s->bind[i].key, s->bind[i].value);
    find_keys();
}

static void print_controls(const psp_settings *s) {
    if (g_keys_wasd && !g_keys_modern && g_title->keys_wasd_classic_help)
        fprintf(stderr, "present: %s\n", g_title->keys_wasd_classic_help);
    else if (g_keys_wasd && g_title->keys_wasd_help)
        fprintf(stderr, "present: %s\n", g_title->keys_wasd_help);
    else if (g_keys_wasd)
        fprintf(stderr, "present: keys wasd stick | space/z cross, x circle, "
                        "c square, v triangle | q d-pad up, e R shoulder | enter start | "
                        "backspace select | mouse: left = square, right/middle = d-pad down | "
                        "close window to stop\n");
    else
        fprintf(stderr, "present: keys arrows dpad | z cross, x circle, "
                        "a square, s triangle | q/e shoulders | enter start | "
                        "backspace select | PSPRECOMP_KEYS=wasd for a mouse layout | "
                        "close window to stop\n");
    if (g_gamepad_modern && g_title->gamepad_modern_help)
        fprintf(stderr, "present: %s\n", g_title->gamepad_modern_help);
    /* The preset's bindings, as the table holds them. */
    for (int i = 0; i < s->bind_count; i++) {
        const char *key = s->bind[i].key, *dot = strchr(key, '.');
        binding t = { 0 };
        int device = -1;
        for (int d = 0; d < DEVICES; d++)
            if ((size_t)(dot - key) == strlen(DEVICE_NAME[d]) && !strncmp(key, DEVICE_NAME[d], (size_t)(dot - key))) device = d;
        if (device < 0 || parse_target(dot + 1, &t)) continue;
        t.device = (uint8_t)device;
        char list[256] = "", one[64];
        for (int k = 0; k < g_nbinds; k++) {
            if (!same_target(&g_binds[k], &t)) continue;
            format_source(&g_binds[k], one, sizeof one);
            if (strlen(list) + strlen(one) + 3 < sizeof list) { if (*list) strcat(list, ", "); strcat(list, one); }
        }
        fprintf(stderr, "present: bind.%s = %s\n", key, *list ? list : "(nothing)");
    }
}

void input_start(const psp_settings *s) {
    build(s);
    print_controls(s);
    g_pad_last = option_value(s, "ACTIVE_PAD", 0) != 0;
    /* Mouse-look, only when asked for. Captured from the start so a run
     * launched for it is playable at once; Escape lets go, a click retakes. */
    g_mouse_want = s->mouse;
    if (g_mouse_want) mouse_grab(1);
    fprintf(stderr, "present: Escape or View + Menu (Select + Start) opens the menu; Ctrl+Shift+Q quits\n");
    open_controllers();
}
