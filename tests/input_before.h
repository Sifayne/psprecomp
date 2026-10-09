/* The host's input as it stood before stage 6 (docs/PLAYER-LAYER.md §3),
 * for tests/test_input.c's table test: psprecomp 0a569ca src/host/present.c,
 * its input section, with every name prefixed old_, the title's hooks read
 * from old_title, the quit chord and mouse capture left out, and publish
 * writing old_out instead of the runtime's pad. Not to be edited: it is what
 * the defaults must reproduce. */

static _Atomic uint32_t old_g_key_buttons;
static _Atomic uint32_t old_g_mouse_buttons;
static _Atomic uint32_t old_g_controller_buttons;
static _Atomic uint8_t  old_g_pad_ax = 128, old_g_pad_ay = 128;

/* SDL key -> PSP button. Headless mode holds buttons for a whole run; here a
 * button is held exactly as long as its key is.
 *
 * Two layouts, PSPRECOMP_KEYS=classic (the default) or wasd. The classic one
 * is the PSP's face laid on the keyboard -- arrows for the d-pad, z/x/a/s for
 * cross/circle/square/triangle, q/e for the shoulders, Return and Backspace
 * for start and select -- and has no stick at all, so it moves nothing
 * without a pad. `wasd` is for a mouse player: W/A/S/D *are* the left stick
 * (digital, so a diagonal is a full push at 45 degrees; under
 * PSPRECOMP_INPUT=dual that is the walk and the strafe), Space is cross
 * (boost, and confirm in menus), the mouse buttons are the fire buttons --
 * left is square (right arm), right and middle are d-pad down (left arm),
 * and Q is d-pad up (change weapon) on the game's default assign. The letters
 * the stick took are moved: square to c, triangle to v. Everything else is
 * the classic layout. Which button does what in the game is the game's
 * key-assign, not this table's. */
typedef psp_key_bind old_key_bind;      /* key: an SDL_Keycode */
typedef struct { uint8_t sdl; uint32_t psp; } old_controller_bind;

static const old_key_bind old_KEYS_CLASSIC[] = {
    { SDLK_RIGHT,      0x000020 }, { SDLK_LEFT,      0x000080 },
    { SDLK_DOWN,       0x000040 }, { SDLK_UP,        0x000010 },
    { SDLK_RETURN,     0x000008 }, { SDLK_BACKSPACE, 0x000001 },
    { SDLK_z,          0x004000 }, { SDLK_x,         0x002000 },
    { SDLK_a,          0x008000 }, { SDLK_s,         0x001000 },
    { SDLK_q,          0x000100 }, { SDLK_e,         0x000200 },
};

static const old_key_bind old_KEYS_WASD[] = {
    { SDLK_RIGHT,      0x000020 }, { SDLK_LEFT,      0x000080 },
    { SDLK_DOWN,       0x000040 }, { SDLK_UP,        0x000010 },
    { SDLK_RETURN,     0x000008 }, { SDLK_BACKSPACE, 0x000001 },
    { SDLK_z,          0x004000 }, { SDLK_SPACE,     0x004000 },
    { SDLK_x,          0x002000 },
    { SDLK_c,          0x008000 }, { SDLK_v,         0x001000 },
    { SDLK_q,          0x000010 }, { SDLK_e,         0x000200 },
};

/* Mouse button -> PSP button, `wasd` only: SDL_BUTTON_LEFT/MIDDLE/RIGHT are
 * 1/2/3. */
static const uint32_t old_MOUSE_WASD[4] = { 0, 0x008000, 0x000040, 0x000040 };
static uint32_t old_g_mouse_down;               /* physical buttons, SDL thread */

/* The title's Modern controls are on: its resolved settings say input and it
 * has PSP_TITLE_MODERN_CONTROLS. Its classic_keys/classic_mouse apply when
 * they are off. */
static int old_g_keys_modern;

/* The WASD layout and the mouse buttons: the title's, or the host's. */
static const old_key_bind *old_wasd_keys(size_t *n) {
    if (old_title.keys_wasd) { *n = old_title.keys_wasd_count; return old_title.keys_wasd; }
    *n = sizeof old_KEYS_WASD / sizeof *old_KEYS_WASD;
    return old_KEYS_WASD;
}
static const uint32_t *old_mouse_wasd(void) {
    return old_title.mouse_wasd ? old_title.mouse_wasd : old_MOUSE_WASD;
}

static int old_g_keys_wasd;                     /* PSPRECOMP_KEYS=wasd */
static int old_g_gamepad_modern;                /* semantic buttons in modern/dual */


static void old_set_bit(_Atomic uint32_t *buttons, uint32_t bit, int down) {
    if (down) atomic_fetch_or(buttons, bit);
    else      atomic_fetch_and(buttons, ~bit);
}

static void old_set_mouse_button(uint8_t button, int down) {
    if (down) old_g_mouse_down |= SDL_BUTTON(button);
    else      old_g_mouse_down &= ~SDL_BUTTON(button);
    /* Right and middle share left-arm fire. Releasing either must not cancel
     * the other while it is still held. */
    uint32_t buttons = 0;
    for (int b = 1; b < 4; b++)
        if (old_g_mouse_down & SDL_BUTTON(b)) buttons |= old_mouse_wasd()[b];
    if (!old_g_keys_modern && old_title.classic_mouse) buttons = old_title.classic_mouse(buttons);
    atomic_store(&old_g_mouse_buttons, buttons);
}

/* The stick from the keyboard: which of W/A/S/D are down, as bits, and the
 * bytes they make. Kept apart from the pad's stick and merged per axis at
 * publish time, the keyboard winning on any axis it holds off centre: a
 * pad left plugged in reports its resting stick as a stream of centre
 * values whenever it drifts, and one that drifts on Y alone made W and S
 * dead while A and D walked (5 Sep) -- the pad's centre kept overwriting
 * the keyboard's byte between key events. */
static _Atomic uint8_t old_g_key_ax = 128, old_g_key_ay = 128;
static _Atomic int     old_g_key_x_owned, old_g_key_y_owned;
static uint8_t         old_g_key_down[SDL_NUM_SCANCODES];

static void old_rebuild_key_stick(void) {
    const int w = old_g_key_down[SDL_SCANCODE_W] != 0;
    const int s = old_g_key_down[SDL_SCANCODE_S] != 0;
    const int a = old_g_key_down[SDL_SCANCODE_A] != 0;
    const int d = old_g_key_down[SDL_SCANCODE_D] != 0;
    const int x = d - a, y = s - w;
    if (old_title.key_axis) {
        const int walk = old_g_key_down[SDL_SCANCODE_LALT] != 0;
        atomic_store(&old_g_key_ax, old_title.key_axis(x, y, walk));
        atomic_store(&old_g_key_ay, old_title.key_axis(y, x, walk));
    } else {
        atomic_store(&old_g_key_ax, (uint8_t)(128 + 127 * x));
        atomic_store(&old_g_key_ay, (uint8_t)(128 + 127 * y));
    }
    /* Opposite keys explicitly own a centred axis.  Without these flags,
     * W+S handed Y back to a drifting controller instead of cancelling it. */
    atomic_store(&old_g_key_x_owned, a || d);
    atomic_store(&old_g_key_y_owned, w || s);
}

static void old_rebuild_key_buttons(void) {
    size_t n = sizeof old_KEYS_CLASSIC / sizeof *old_KEYS_CLASSIC;
    const old_key_bind *keys = old_g_keys_wasd ? old_wasd_keys(&n) : old_KEYS_CLASSIC;
    uint32_t buttons = 0;
    for (size_t i = 0; i < n; i++) {
        const SDL_Scancode sc = SDL_GetScancodeFromKey(keys[i].key);
        if (sc != SDL_SCANCODE_UNKNOWN && old_g_key_down[sc]) buttons |= keys[i].bit;
    }
    if (old_g_keys_wasd && !old_g_keys_modern && old_title.classic_keys)
        buttons = old_title.classic_keys(buttons);
    atomic_store(&old_g_key_buttons, buttons);
}

static void old_set_key(SDL_Scancode sc, int down) {
    if (sc <= SDL_SCANCODE_UNKNOWN || sc >= SDL_NUM_SCANCODES) return;
    old_g_key_down[sc] = down != 0;
    if (old_g_keys_wasd) old_rebuild_key_stick();
    old_rebuild_key_buttons();
}

static void old_clear_keys(void) {
    memset(old_g_key_down, 0, sizeof old_g_key_down);
    atomic_store(&old_g_key_buttons, 0);
    atomic_store(&old_g_key_ax, 128);
    atomic_store(&old_g_key_ay, 128);
    atomic_store(&old_g_key_x_owned, 0);
    atomic_store(&old_g_key_y_owned, 0);
}

static void old_set_button(uint8_t b, int down) {
    if (old_title.pad_button) {
        const uint32_t carrier = old_title.pad_button(b);
        if (carrier) { old_set_bit(&old_g_controller_buttons, carrier, down); return; }
    }
    static const old_controller_bind CLASSIC[] = {
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
    static const old_controller_bind MODERN[] = {
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
    const old_controller_bind *map = old_g_gamepad_modern ? MODERN : CLASSIC;
    const size_t n = old_g_gamepad_modern ? sizeof MODERN / sizeof *MODERN
                                      : sizeof CLASSIC / sizeof *CLASSIC;
    for (size_t i = 0; i < n; i++) {
        if (map[i].sdl != b) continue;
        old_set_bit(&old_g_controller_buttons, map[i].psp, down);
        return;
    }
}

/* The second stick and the mouse: the look channel. The PSP has neither, and
 * the game as shipped reads neither -- they travel the sceCtrl lanes beside
 * the pad, where the recorder sees them, and only host/replacements.c reads
 * them. Mouse travel is summed here per motion event and taken by the poll,
 * so nothing is lost between a 1 kHz mouse and a 36 Hz game. */
static _Atomic uint8_t  old_g_pad_rx = 128, old_g_pad_ry = 128;

static uint8_t old_axis_byte(int16_t value) {
    /* Preserve the complete SDL range here.  The selected control profile
     * applies one radial deadzone later, after recording; doing an axial 25%
     * cut here as well made cardinal movement need roughly half a real stick
     * and bent diagonals toward the axes. */
    const int32_t v = value;
    if (v >= 0) return (uint8_t)(128 + v * 127 / 32767);
    return (uint8_t)(128 + v * 127 / 32768);
}

static void old_set_axis(uint8_t axis, int16_t value) {
    if (axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ||
        axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
        if (!old_g_gamepad_modern) return;
        const uint32_t bit = axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? PSP_PAD_LT : PSP_PAD_RT;
        const int held = (atomic_load(&old_g_controller_buttons) & bit) != 0;
        /* SDL's GameController layer normalises triggers to 0..32767.
         * Separate 50%/25% press/release thresholds avoid chatter around the
         * actuation point without making a light touch sticky. */
        if (!held && value >= 16384) old_set_bit(&old_g_controller_buttons, bit, 1);
        if ( held && value <=  8192) old_set_bit(&old_g_controller_buttons, bit, 0);
        return;
    }
    if (axis != SDL_CONTROLLER_AXIS_LEFTX  && axis != SDL_CONTROLLER_AXIS_LEFTY &&
        axis != SDL_CONTROLLER_AXIS_RIGHTX && axis != SDL_CONTROLLER_AXIS_RIGHTY)
        return;
    const uint8_t v = old_axis_byte(value);
    switch (axis) {
    case SDL_CONTROLLER_AXIS_LEFTX:  atomic_store(&old_g_pad_ax, v); break;
    case SDL_CONTROLLER_AXIS_LEFTY:  atomic_store(&old_g_pad_ay, v); break;
    case SDL_CONTROLLER_AXIS_RIGHTX: atomic_store(&old_g_pad_rx, v); break;
    default:                         atomic_store(&old_g_pad_ry, v); break;
    }
}

/* Publish the whole pad state after any change, so a guest poll between two
 * events of one press never sees the press half-applied. Cheap. */
static void old_publish_pad(void) {
    const uint8_t kax = atomic_load(&old_g_key_ax), kay = atomic_load(&old_g_key_ay);
    const uint32_t buttons = atomic_load(&old_g_key_buttons) |
                             atomic_load(&old_g_mouse_buttons) |
                             atomic_load(&old_g_controller_buttons);
    old_out.buttons = buttons;
    old_out.ax = atomic_load(&old_g_key_x_owned) ? kax : atomic_load(&old_g_pad_ax);
    old_out.ay = atomic_load(&old_g_key_y_owned) ? kay : atomic_load(&old_g_pad_ay);
    old_out.lx = atomic_load(&old_g_pad_rx);
    old_out.ly = atomic_load(&old_g_pad_ry);
}

static void old_clear_controller(void) {
    atomic_store(&old_g_controller_buttons, 0);
    atomic_store(&old_g_pad_ax, 128);
    atomic_store(&old_g_pad_ay, 128);
    atomic_store(&old_g_pad_rx, 128);
    atomic_store(&old_g_pad_ry, 128);
}
