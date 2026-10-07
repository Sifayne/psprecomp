/* present — the windowed presentation layer. See psprecomp/host/present.h.
 * Last Raven's host/present.c until stage 1 of docs/PLAYER-LAYER.md moved it
 * here; the game it presents is psp_title_info, and the options it reads
 * are found by key in the title's settings schema.
 *
 * Three streams cross between guest and host here, each in the cheapest
 * direction for its rate:
 *
 *   video   guest frame thread  ->  converted RGBA, one slot, mutex   (~60/s)
 *   input   SDL event thread    ->  atomics read by sceCtrl            (on change)
 *   audio   guest audio thread  ->  SDL_QueueAudio, real-time drain    (~100/s)
 *
 * Audio is no longer queued: the device callback mixes a ring per channel
 * (see the audio section). */

#include "psprecomp/host/present.h"
#include "psprecomp/host/pad.h"
#include "psprecomp/host/save_dialog.h"
#include "psprecomp/host/settings.h"
#include "psprecomp/host/title.h"

#include "psprecomp/clock.h"
#include "psprecomp/hle.h"
#include "psprecomp/mem.h"
#include "psprecomp/safepoint.h"
#include "psprecomp/sched.h"

#include <SDL2/SDL.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { SCREEN_W = 480, SCREEN_H = 272 };

/* The window title's first part: the title's own name. */
static const char *title_name(void) {
    return psp_title_info.name ? psp_title_info.name : "PSP";
}

/* An option the title's schema may or may not have, by key. The shared
 * resolve already turned the player options into settings fields; these are
 * the rest the host reads. */
static double setting(const char *key, double absent) {
    const int id = psp_settings_find(key);
    return id < 0 ? absent : psp_settings_current()->number[id];
}

/* ---- frames -------------------------------------------------------------- */

static pthread_t      g_thread;
static pthread_mutex_t g_frame_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_frame_cv;
static uint32_t        g_frame_rgba[SCREEN_W * SCREEN_H];
static int             g_frame_fresh;
/* The SDL thread's own copy, so the upload happens outside g_frame_lock: the
 * publishing thread holds the scheduler token, and must never wait on a GPU. */
static uint32_t        g_frame_present[SCREEN_W * SCREEN_H];
static _Atomic int     g_quit;

/* The display hook. Runs on a guest thread holding the scheduler token, so
 * convert and publish, and get out -- the ~130k reads take well under a
 * millisecond, and the lock is held only across that. */
static void present_frame(uint32_t addr, uint32_t stride, uint32_t fmt) {
    /* The window is gone and the run is being torn down; there is nobody to
     * hand a frame to, and the guest thread should not pay for converting one. */
    if (atomic_load(&g_quit)) return;

    /* Bring-up diagnostics: the difference between "the window is black"
     * because nothing was ever published and because SDL failed to paint it
     * is the first thing to know, and neither leaves another trace. */
    static int published;
    if (!published++)
        fprintf(stderr, "present: first frame addr=0x%08X stride=%u fmt=%u\n",
                addr, stride, fmt);
    else if (published % 60 == 0)
        fprintf(stderr, "present: %d frames (last addr=0x%08X)\n", published, addr);

    pthread_mutex_lock(&g_frame_lock);

    /* `stride` is sceDisplaySetFrameBuf's bufferwidth, which is in *pixels*
     * (512 for a 480-wide panel) -- so a row step is stride * bytes-per-pixel.
     * Using it as a byte pitch advanced 512 bytes instead of 2048 and sheared
     * the frame: the unwritten 480..511 stride padding walked across the
     * picture as three black bands, and only the top 68 scanlines were ever
     * read. Nothing faults when you do this, which is why it read as a
     * rasterizer fault. The bpp split matches the switch below, where anything
     * that is not 565/5551/4444 is 8888. */
    const uint32_t bpp = (fmt <= 2) ? 2u : 4u;

    for (int y = 0; y < SCREEN_H; y++) {
        uint32_t *out = g_frame_rgba + (size_t)y * SCREEN_W;
        const uint32_t row = addr + (uint32_t)y * stride * bpp;
        for (int x = 0; x < SCREEN_W; x++) {
            uint32_t r, g, b, a = 255;
            switch (fmt) {
            case 0:                                     /* 565 */
                r = psp_read16(row + x * 2u);
                g = (r >> 5) & 0x3F;  b = (r >> 11) & 0x1F;  r &= 0x1F;
                out[x] = 0xFF000000u | (b << 3 | b >> 2) << 16 |
                         (g << 2 | g >> 4) << 8 | (r << 3 | r >> 2);
                continue;
            case 1:                                     /* 5551 */
                r = psp_read16(row + x * 2u);
                g = (r >> 5) & 0x1F;  b = (r >> 10) & 0x1F;
                a = (r & 0x8000) ? 255 : 0;  r &= 0x1F;
                out[x] = a << 24 | (b << 3 | b >> 2) << 16 |
                         (g << 3 | g >> 2) << 8 | (r << 3 | r >> 2);
                continue;
            case 2:                                     /* 4444 */
                r = psp_read16(row + x * 2u);
                g = (r >> 4) & 0xF;  b = (r >> 8) & 0xF;  a = (r >> 12) & 0xF;
                r &= 0xF;
                out[x] = (a << 4 | a) << 24 | (b << 4 | b) << 16 |
                         (g << 4 | g) << 8 | (r << 4 | r);
                continue;
            default:                                    /* 8888 */
                /* Guest RGBA8888 and SDL ABGR8888 are the same bytes in the
                 * same order on little-endian: R G B A in memory, alpha in
                 * the high byte of the word. */
                out[x] = psp_read32(row + x * 4u);
                continue;
            }
        }
    }

    g_frame_fresh = 1;
    pthread_cond_signal(&g_frame_cv);
    pthread_mutex_unlock(&g_frame_lock);
}

/* ---- input ---------------------------------------------------------------- */

static _Atomic uint32_t g_key_buttons;
static _Atomic uint32_t g_mouse_buttons;
static _Atomic uint32_t g_controller_buttons;
static _Atomic uint8_t  g_pad_ax = 128, g_pad_ay = 128;

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
typedef psp_key_bind key_bind;      /* key: an SDL_Keycode */
typedef struct { uint8_t sdl; uint32_t psp; } controller_bind;

static const key_bind KEYS_CLASSIC[] = {
    { SDLK_RIGHT,      0x000020 }, { SDLK_LEFT,      0x000080 },
    { SDLK_DOWN,       0x000040 }, { SDLK_UP,        0x000010 },
    { SDLK_RETURN,     0x000008 }, { SDLK_BACKSPACE, 0x000001 },
    { SDLK_z,          0x004000 }, { SDLK_x,         0x002000 },
    { SDLK_a,          0x008000 }, { SDLK_s,         0x001000 },
    { SDLK_q,          0x000100 }, { SDLK_e,         0x000200 },
};

static const key_bind KEYS_WASD[] = {
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
static const uint32_t MOUSE_WASD[4] = { 0, 0x008000, 0x000040, 0x000040 };
static uint32_t g_mouse_down;               /* physical buttons, SDL thread */

/* The title's Modern controls are on: its resolved settings say input and it
 * has PSP_TITLE_MODERN_CONTROLS. Its classic_keys/classic_mouse apply when
 * they are off. */
static int g_keys_modern;

/* The WASD layout and the mouse buttons: the title's, or the host's. */
static const key_bind *wasd_keys(size_t *n) {
    if (psp_title_info.keys_wasd) { *n = psp_title_info.keys_wasd_count; return psp_title_info.keys_wasd; }
    *n = sizeof KEYS_WASD / sizeof *KEYS_WASD;
    return KEYS_WASD;
}
static const uint32_t *mouse_wasd(void) {
    return psp_title_info.mouse_wasd ? psp_title_info.mouse_wasd : MOUSE_WASD;
}

static int g_keys_wasd;                     /* PSPRECOMP_KEYS=wasd */
static int g_gamepad_modern;                /* semantic buttons in modern/dual */
static SDL_GameController *g_controller;    /* one controller owns the pad lane */
static SDL_JoystickID      g_controller_id = -1;

/* Host-only quit chord: physical View/Back + Menu/Start, held continuously.
 * Keep it separate from guest/replay buttons and from controller mappings. */
static struct { unsigned buttons; uint64_t since; int timing; } g_quit_chord;
enum { QUIT_HOLD_MS = 2000 };

static void quit_chord_button(uint8_t button, int down, uint64_t now) {
    const unsigned bit = button == SDL_CONTROLLER_BUTTON_BACK ? 1u :
                         button == SDL_CONTROLLER_BUTTON_START ? 2u : 0u;
    if (!bit) return;
    if (down) g_quit_chord.buttons |= bit;
    else g_quit_chord.buttons &= ~bit;
    if (g_quit_chord.buttons == 3) {
        if (!g_quit_chord.timing) {
            g_quit_chord.since = now; g_quit_chord.timing = 1;
            /* The window title says so at once (below); this is for the log. */
            fprintf(stderr, "present: quit chord held -- keep holding Select + Start for %d s to close, release to cancel\n",
                    QUIT_HOLD_MS / 1000);
        }
    } else g_quit_chord.timing = 0;
}

static int quit_chord_due(uint64_t now) {
    return g_quit_chord.timing && now - g_quit_chord.since >= QUIT_HOLD_MS;
}

static int quit_key_event(const SDL_Event *event) {
    return event->type == SDL_KEYDOWN && !event->key.repeat &&
           event->key.keysym.sym == SDLK_q &&
           (event->key.keysym.mod & KMOD_CTRL) && (event->key.keysym.mod & KMOD_SHIFT);
}

static void set_bit(_Atomic uint32_t *buttons, uint32_t bit, int down) {
    if (down) atomic_fetch_or(buttons, bit);
    else      atomic_fetch_and(buttons, ~bit);
}

static void set_mouse_button(uint8_t button, int down) {
    if (down) g_mouse_down |= SDL_BUTTON(button);
    else      g_mouse_down &= ~SDL_BUTTON(button);
    /* Right and middle share left-arm fire. Releasing either must not cancel
     * the other while it is still held. */
    uint32_t buttons = 0;
    for (int b = 1; b < 4; b++)
        if (g_mouse_down & SDL_BUTTON(b)) buttons |= mouse_wasd()[b];
    if (!g_keys_modern && psp_title_info.classic_mouse) buttons = psp_title_info.classic_mouse(buttons);
    atomic_store(&g_mouse_buttons, buttons);
}

/* The stick from the keyboard: which of W/A/S/D are down, as bits, and the
 * bytes they make. Kept apart from the pad's stick and merged per axis at
 * publish time, the keyboard winning on any axis it holds off centre: a
 * pad left plugged in reports its resting stick as a stream of centre
 * values whenever it drifts, and one that drifts on Y alone made W and S
 * dead while A and D walked (5 Sep) -- the pad's centre kept overwriting
 * the keyboard's byte between key events. */
static _Atomic uint8_t g_key_ax = 128, g_key_ay = 128;
static _Atomic int     g_key_x_owned, g_key_y_owned;
static uint8_t         g_key_down[SDL_NUM_SCANCODES];

static void rebuild_key_stick(void) {
    const int w = g_key_down[SDL_SCANCODE_W] != 0;
    const int s = g_key_down[SDL_SCANCODE_S] != 0;
    const int a = g_key_down[SDL_SCANCODE_A] != 0;
    const int d = g_key_down[SDL_SCANCODE_D] != 0;
    const int x = d - a, y = s - w;
    if (psp_title_info.key_axis) {
        const int walk = g_key_down[SDL_SCANCODE_LALT] != 0;
        atomic_store(&g_key_ax, psp_title_info.key_axis(x, y, walk));
        atomic_store(&g_key_ay, psp_title_info.key_axis(y, x, walk));
    } else {
        atomic_store(&g_key_ax, (uint8_t)(128 + 127 * x));
        atomic_store(&g_key_ay, (uint8_t)(128 + 127 * y));
    }
    /* Opposite keys explicitly own a centred axis.  Without these flags,
     * W+S handed Y back to a drifting controller instead of cancelling it. */
    atomic_store(&g_key_x_owned, a || d);
    atomic_store(&g_key_y_owned, w || s);
}

static void rebuild_key_buttons(void) {
    size_t n = sizeof KEYS_CLASSIC / sizeof *KEYS_CLASSIC;
    const key_bind *keys = g_keys_wasd ? wasd_keys(&n) : KEYS_CLASSIC;
    uint32_t buttons = 0;
    for (size_t i = 0; i < n; i++) {
        const SDL_Scancode sc = SDL_GetScancodeFromKey(keys[i].key);
        if (sc != SDL_SCANCODE_UNKNOWN && g_key_down[sc]) buttons |= keys[i].bit;
    }
    if (g_keys_wasd && !g_keys_modern && psp_title_info.classic_keys)
        buttons = psp_title_info.classic_keys(buttons);
    atomic_store(&g_key_buttons, buttons);
}

static void set_key(SDL_Scancode sc, int down) {
    if (sc <= SDL_SCANCODE_UNKNOWN || sc >= SDL_NUM_SCANCODES) return;
    g_key_down[sc] = down != 0;
    if (g_keys_wasd) rebuild_key_stick();
    rebuild_key_buttons();
}

static void clear_keys(void) {
    memset(g_key_down, 0, sizeof g_key_down);
    atomic_store(&g_key_buttons, 0);
    atomic_store(&g_key_ax, 128);
    atomic_store(&g_key_ay, 128);
    atomic_store(&g_key_x_owned, 0);
    atomic_store(&g_key_y_owned, 0);
}

static void set_button(uint8_t b, int down) {
    quit_chord_button(b, down, SDL_GetTicks64());
    if (psp_title_info.pad_button) {
        const uint32_t carrier = psp_title_info.pad_button(b);
        if (carrier) { set_bit(&g_controller_buttons, carrier, down); return; }
    }
    static const controller_bind CLASSIC[] = {
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
    static const controller_bind MODERN[] = {
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
    const controller_bind *map = g_gamepad_modern ? MODERN : CLASSIC;
    const size_t n = g_gamepad_modern ? sizeof MODERN / sizeof *MODERN
                                      : sizeof CLASSIC / sizeof *CLASSIC;
    for (size_t i = 0; i < n; i++) {
        if (map[i].sdl != b) continue;
        set_bit(&g_controller_buttons, map[i].psp, down);
        return;
    }
}

/* The second stick and the mouse: the look channel. The PSP has neither, and
 * the game as shipped reads neither -- they travel the sceCtrl lanes beside
 * the pad, where the recorder sees them, and only host/replacements.c reads
 * them. Mouse travel is summed here per motion event and taken by the poll,
 * so nothing is lost between a 1 kHz mouse and a 36 Hz game. */
static _Atomic uint8_t  g_pad_rx = 128, g_pad_ry = 128;
static int              g_mouse_want;       /* PSPRECOMP_MOUSE=1 */
static int              g_mouse_grabbed;
static int              g_dialog_block_input;
static Uint64           g_dialog_closed_at;
static int              g_dialog_mouse_was_grabbed;

static uint8_t axis_byte(int16_t value) {
    /* Preserve the complete SDL range here.  The selected control profile
     * applies one radial deadzone later, after recording; doing an axial 25%
     * cut here as well made cardinal movement need roughly half a real stick
     * and bent diagonals toward the axes. */
    const int32_t v = value;
    if (v >= 0) return (uint8_t)(128 + v * 127 / 32767);
    return (uint8_t)(128 + v * 127 / 32768);
}

static void set_axis(uint8_t axis, int16_t value) {
    if (axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ||
        axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
        if (!g_gamepad_modern) return;
        const uint32_t bit = axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? PSP_PAD_LT : PSP_PAD_RT;
        const int held = (atomic_load(&g_controller_buttons) & bit) != 0;
        /* SDL's GameController layer normalises triggers to 0..32767.
         * Separate 50%/25% press/release thresholds avoid chatter around the
         * actuation point without making a light touch sticky. */
        if (!held && value >= 16384) set_bit(&g_controller_buttons, bit, 1);
        if ( held && value <=  8192) set_bit(&g_controller_buttons, bit, 0);
        return;
    }
    if (axis != SDL_CONTROLLER_AXIS_LEFTX  && axis != SDL_CONTROLLER_AXIS_LEFTY &&
        axis != SDL_CONTROLLER_AXIS_RIGHTX && axis != SDL_CONTROLLER_AXIS_RIGHTY)
        return;
    const uint8_t v = axis_byte(value);
    switch (axis) {
    case SDL_CONTROLLER_AXIS_LEFTX:  atomic_store(&g_pad_ax, v); break;
    case SDL_CONTROLLER_AXIS_LEFTY:  atomic_store(&g_pad_ay, v); break;
    case SDL_CONTROLLER_AXIS_RIGHTX: atomic_store(&g_pad_rx, v); break;
    default:                         atomic_store(&g_pad_ry, v); break;
    }
}

/* Capture the pointer for mouse-look, or let it go. Only ever when asked for
 * with PSPRECOMP_MOUSE=1; and even then focus loss and Escape release it,
 * because a window that traps the pointer and cannot be left is the one
 * thing worse than no mouse-look at all. A click takes it back. */
static void publish_pad(void);
static void mouse_grab(int on) {
    if (!g_mouse_want || on == g_mouse_grabbed) return;
    SDL_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE);
    g_mouse_grabbed = on;
    if (!on) {
        psp_ctrl_clear_mouse();
        /* A button-up outside capture is intentionally ignored. Clear the
         * old held set now so Escape cannot leave fire/aim stuck on. */
        g_mouse_down = 0;
        atomic_store(&g_mouse_buttons, 0);
        publish_pad();
    }
    fprintf(stderr, on ? "present: mouse captured -- Escape releases it\n"
                       : "present: mouse released -- click the window to capture it\n");
}

/* Publish the whole pad state after any change, so a guest poll between two
 * events of one press never sees the press half-applied. Cheap. */
static void publish_pad(void) {
    if (g_dialog_block_input || save_dialog_active()) {
        psp_ctrl_set(0, 128, 128);
        psp_ctrl_set_look(128, 128);
        return;
    }
    const uint8_t kax = atomic_load(&g_key_ax), kay = atomic_load(&g_key_ay);
    const uint32_t buttons = atomic_load(&g_key_buttons) |
                             atomic_load(&g_mouse_buttons) |
                             atomic_load(&g_controller_buttons);
    psp_ctrl_set(buttons,
                 atomic_load(&g_key_x_owned) ? kax : atomic_load(&g_pad_ax),
                 atomic_load(&g_key_y_owned) ? kay : atomic_load(&g_pad_ay));
    psp_ctrl_set_look(atomic_load(&g_pad_rx), atomic_load(&g_pad_ry));
}

/* The modern layout sends the face buttons, bumpers, triggers and stick
 * clicks as carrier bits (psprecomp/host/pad.h) that the game never sees: a
 * native replacement of its button converter reads them. A title without one
 * says so in psp_title_info, and the pad speaks the PSP buttons the game
 * already understands -- otherwise A, B, X, Y, LB, RB and both triggers
 * would go nowhere. */
static int gamepad_modern(void) {
    if (!psp_settings_current()->gamepad) return 0;
    if (psp_title_can(PSP_TITLE_MODERN_CONTROLS)) return 1;
    fprintf(stderr, "present: this title has no native replacements for the modern "
                    "controller layout -- using the classic PSP buttons instead\n");
    return 0;
}

static void clear_controller(void) {
    memset(&g_quit_chord, 0, sizeof g_quit_chord);
    atomic_store(&g_controller_buttons, 0);
    atomic_store(&g_pad_ax, 128);
    atomic_store(&g_pad_ay, 128);
    atomic_store(&g_pad_rx, 128);
    atomic_store(&g_pad_ry, 128);
}

static void sample_controller(void) {
    if (!g_controller) return;
    for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++)
        set_button((uint8_t)b, SDL_GameControllerGetButton(
            g_controller, (SDL_GameControllerButton)b));
    for (int a = 0; a < SDL_CONTROLLER_AXIS_MAX; a++)
        set_axis((uint8_t)a, SDL_GameControllerGetAxis(
            g_controller, (SDL_GameControllerAxis)a));
}

static int open_controller(int device_index) {
    if (g_controller || !SDL_IsGameController(device_index)) return 0;
    SDL_GameController *c = SDL_GameControllerOpen(device_index);
    if (!c) {
        fprintf(stderr, "present: cannot open controller %d: %s\n",
                device_index, SDL_GetError());
        return 0;
    }
    SDL_Joystick *joy = SDL_GameControllerGetJoystick(c);
    const SDL_JoystickID id = joy ? SDL_JoystickInstanceID(joy) : -1;
    if (id < 0) {
        SDL_GameControllerClose(c);
        return 0;
    }
    g_controller = c;
    g_controller_id = id;
    clear_controller();
    sample_controller();
    fprintf(stderr, "present: controller \"%s\" opened as the active pad -- "
                    "%s buttons, radial stick filtering, keyboard overrides per axis\n",
            SDL_GameControllerName(c) ? SDL_GameControllerName(c) : "?",
            g_gamepad_modern ? "modern" : "PSP");
    publish_pad();
    return 1;
}

static void open_first_controller(void) {
    if (g_controller) return;
    const int n = SDL_NumJoysticks();
    for (int i = 0; i < n; i++)
        if (open_controller(i)) return;
}

/* ---- audio ---------------------------------------------------------------- */

/* A mixer, not a queue.
 *
 * This used to hand every channel's buffer to SDL_QueueAudio, which is one
 * stream: two channels playing at once -- the music on one, the SAS effects
 * on another -- came out interleaved block by block, each at the wrong
 * moment. The PSP has eight hardware channels and sums them, so this keeps a
 * ring of stereo frames per channel and the device callback sums whatever
 * each ring holds, silence for an empty one. The volumes are applied on the
 * way in, on the 0..0x8000 scale the output call was given.
 *
 * Pacing: each blocking output call is told how far its own channel is ahead
 * of the speaker, and the scheduler delays it by that. The target depth is
 * how far a channel may run ahead before it is made to wait -- short enough
 * that an effect is heard when it happens, long enough to ride out a frame
 * that rasterizes slowly. */
/* A channel does not play until it holds a pre-roll, and after running dry it
 * waits for one again. A stream the game paces itself -- the movie's sound
 * thread waits on the movie's own clock, not on our backlog -- arrives one
 * block at a time at exactly the rate it is consumed, so the ring hovers near
 * empty and every push that lands a few milliseconds late is a gap the
 * callback fills with silence: the popping Sif heard through the intro. A
 * pre-roll of 4096 frames is 93 ms of slack against that jitter, paid once as
 * latency at the start of each stream. */
enum { MIX_CHANNELS = 8, MIX_RING_FRAMES = 44100 * 8 };
typedef struct {
    int16_t *pcm;
    uint32_t head, tail, count;
    int      playing;          /* holds a pre-roll, or has not run dry since */
    uint32_t pushed, dropped, underruns, silence;
    /* Push timing, for the report: when the first and last pushes came, the
     * longest wait between two, and how many waits were longer than the
     * pre-roll -- each of those a gap the pre-roll could not cover. */
    uint64_t first_ms, last_ms, max_gap_ms;
    uint32_t long_gaps;
} mix_ring;

static SDL_AudioDeviceID g_audio_dev;
static mix_ring g_mix[MIX_CHANNELS];
/* How far a channel may run ahead of the speaker before its blocking output
 * is made to wait. Hardware's answer is two of the channel's own buffers:
 * sceAudioOutputBlocking returns when the previous buffer has finished
 * playing, so at most the one playing and the one queued are ever in
 * flight. That is also the sound-to-picture latency, and this used to be
 * half a second -- the game's movie player takes its sound thread as the
 * clock, that thread ran half a second ahead of the speaker, and the picture
 * followed the decode while the sound arrived later: Sif saw the intro out
 * of sync. Two buffers it is, but never less than the pre-roll, since a
 * channel cannot start until it holds that much. PSPRECOMP_AUDIO_LEAD_MS
 * overrides it, and PSPRECOMP_AUDIO_PREROLL_MS the pre-roll, for listening
 * without a rebuild. */
static uint32_t g_audio_target_frames  = 0;      /* 0: two of the channel's buffers */
static uint32_t g_audio_preroll_frames = 4096;
static int      g_audio_warned_fmt;

static uint32_t settings_ms_frames(const char *key, uint32_t dflt_frames) {
    const double ms = setting(key, -1);
    if (ms < 0) return dflt_frames;
    uint64_t f = (uint64_t)(ms * 44100.0 / 1000.0);
    if (f >= MIX_RING_FRAMES / 2) f = MIX_RING_FRAMES / 2;
    return (uint32_t)f;
}

/* The device callback: runs on SDL's audio thread with the device lock held,
 * which is the lock present_audio takes to push. */
static void audio_callback(void *ud, Uint8 *stream, int len) {
    (void)ud;
    int16_t *out = (int16_t *)stream;
    const int frames = len / 4;
    for (int i = 0; i < frames; i++) {
        int32_t l = 0, r = 0;
        for (int ch = 0; ch < MIX_CHANNELS; ch++) {
            mix_ring *m = &g_mix[ch];
            if (!m->playing) {
                if (m->count < g_audio_preroll_frames) { if (m->pushed) m->silence++; continue; }
                m->playing = 1;
            } else if (!m->count) {
                m->playing = 0;
                m->underruns++;
                m->silence++;
                continue;
            }
            l += m->pcm[m->tail * 2];
            r += m->pcm[m->tail * 2 + 1];
            m->tail = (m->tail + 1) % MIX_RING_FRAMES;
            m->count--;
        }
        if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
        if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
        out[i * 2] = (int16_t)l;
        out[i * 2 + 1] = (int16_t)r;
    }
}

static void mix_prepare(uint32_t lead_frames, uint32_t preroll_frames) {
    g_audio_target_frames = lead_frames;
    g_audio_preroll_frames = preroll_frames;
}
/* The mixer locks the device through g_audio_dev, which is this file's. */
static void mix_opened(uint32_t device) { (void)device; }

/* The audio hook. Runs on the guest audio thread, inside the output call:
 * the buffer is complete -- the game filled it before calling -- so read it
 * out, scale it, and push it. Returns the backlog in microseconds for the
 * blocking calls to pay. */
static int64_t present_audio(int ch, uint32_t samples, uint32_t fmt,
                             uint32_t buf, uint32_t lvol, uint32_t rvol) {
    if (!g_audio_dev || !samples || ch < 0 || ch >= MIX_CHANNELS) return 0;
    mix_ring *m = &g_mix[ch];
    if (!m->pcm) {
        m->pcm = malloc(sizeof(int16_t) * 2 * MIX_RING_FRAMES);
        if (!m->pcm) return 0;
    }

    /* PSP_AUDIO_FORMAT_STEREO is 0 and MONO is 0x10; the mono form carries
     * one sample per frame and plays on both sides. Anything else in the
     * format word is reported once and treated as stereo. */
    const int mono = (fmt & 0x10) != 0;
    if ((fmt & ~0x10u) && !g_audio_warned_fmt++)
        fprintf(stderr, "present: channel format 0x%X is unfamiliar; treating it as %s\n",
                fmt, mono ? "mono" : "stereo");
    if (lvol > 0x8000) lvol = 0x8000;
    if (rvol > 0x8000) rvol = 0x8000;

    /* Run time, so a host pause between two pushes is not a gap. */
    const uint64_t now = psp_clock_run_ns() / 1000000u;
    SDL_LockAudioDevice(g_audio_dev);
    if (!m->first_ms) m->first_ms = now;
    else {
        const uint64_t gap = now - m->last_ms;
        if (gap > m->max_gap_ms) m->max_gap_ms = gap;
        if (gap * 44100u / 1000u > g_audio_preroll_frames) m->long_gaps++;
    }
    m->last_ms = now;
    for (uint32_t i = 0; i < samples; i++) {
        int32_t sl, sr;
        if (mono) { sl = sr = (int16_t)psp_read16(buf + i * 2u); }
        else      { sl = (int16_t)psp_read16(buf + i * 4u); sr = (int16_t)psp_read16(buf + i * 4u + 2u); }
        sl = (sl * (int32_t)lvol) >> 15;
        sr = (sr * (int32_t)rvol) >> 15;
        if (m->count >= MIX_RING_FRAMES) { m->dropped++; continue; }
        m->pcm[m->head * 2] = (int16_t)sl;
        m->pcm[m->head * 2 + 1] = (int16_t)sr;
        m->head = (m->head + 1) % MIX_RING_FRAMES;
        m->count++;
        m->pushed++;
    }
    const int64_t queued = m->count;
    SDL_UnlockAudioDevice(g_audio_dev);

    int64_t allow = g_audio_target_frames ? g_audio_target_frames : 2 * (int64_t)samples;
    if (allow < (int64_t)g_audio_preroll_frames) allow = g_audio_preroll_frames;
    const int64_t over = queued - allow;
    if (over <= 0) return 0;
    const int64_t us = over * 1000000 / 44100;
    return us > 100000 ? 100000 : us;          /* never claim more than 100ms */
}

/* What the mixer saw, per channel: frames pushed, dropped for a full ring,
 * and the times the ring ran dry -- each of those a gap the listener heard. */
static void mix_report(FILE *out) {
    if (!g_audio_dev) return;
    SDL_LockAudioDevice(g_audio_dev);
    for (int ch = 0; ch < MIX_CHANNELS; ch++) {
        const mix_ring *m = &g_mix[ch];
        if (m->pushed)
            fprintf(out, "present: audio ch %d  %.1f s pushed over %.1f s  %u dropped  "
                         "%u underruns (%.2f s of silence)  longest wait between pushes %llu ms, "
                         "%u waits longer than the pre-roll\n",
                    ch, m->pushed / 44100.0,
                    (m->last_ms - m->first_ms) / 1000.0, m->dropped,
                    m->underruns, m->silence / 44100.0,
                    (unsigned long long)m->max_gap_ms, m->long_gaps);
    }
    SDL_UnlockAudioDevice(g_audio_dev);
}

static const psp_audio_backend host_mixer = {
    .prepare = mix_prepare, .callback = audio_callback, .opened = mix_opened,
    .output = present_audio, .report = mix_report,
};

/* The title's audio output, or the host's mixer. */
static const psp_audio_backend *audio(void) {
    return psp_title_info.audio ? psp_title_info.audio : &host_mixer;
}

/* While the guest is held the device plays silence and the mixer is not
 * asked: its rings keep what was queued, and resume where the pause cut
 * them, without counting the pause as an underrun. */
static void audio_device_callback(void *ud, Uint8 *stream, int len) {
    if (psp_paused()) { memset(stream, 0, (size_t)len); return; }
    audio()->callback(ud, stream, len);
}

void present_audio_report(FILE *out) { audio()->report(out); }

/* ---- the GL handoff -------------------------------------------------------
 *
 * A GL context belongs to one thread, and it is not this one. SDL owns the
 * window and the event queue here, but display lists execute on the guest
 * thread that submitted them -- measured as exactly one thread, which is what
 * makes this arrangement possible at all (findings item 51).
 *
 * So the SDL thread creates the window and the context and then *releases*
 * the context, and the GE thread claims it once and keeps it. The handshake
 * below also keeps presentation startup out of guest time: present_start does
 * not enable the real-time clock until SDL has a usable window/context. */
static int             g_gl_want;        /* set before present_start */
static SDL_Window     *g_gl_win;
static SDL_GLContext   g_gl_ctx;
static int             g_gl_state;       /* 0 pending, 1 ready, -1 failed */
static _Atomic uint64_t g_gl_draw_size;
static _Atomic uint64_t g_requested_size;
static pthread_mutex_t g_gl_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_gl_cv   = PTHREAD_COND_INITIALIZER;

/* GL has its own handoff because its context is claimed later by the GE
 * thread. This second handshake covers both presentation paths and is waited
 * by present_start itself. Without it, SDL window/context/audio setup happened
 * after the wall-clock origin was captured; the first game frame therefore
 * inherited several hundred milliseconds of host startup as elapsed guest
 * time. Light and heavy scenes then began from different apparent times. */
static int             g_start_state;    /* 0 pending, 1 ready, -1 failed */
static pthread_mutex_t g_start_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_start_cv   = PTHREAD_COND_INITIALIZER;

void present_want_gl(void) { g_gl_want = 1; }

static void gl_publish(SDL_Window *win, SDL_GLContext ctx, int ok) {
    pthread_mutex_lock(&g_gl_lock);
    g_gl_win = win; g_gl_ctx = ctx; g_gl_state = ok ? 1 : -1;
    pthread_cond_broadcast(&g_gl_cv);
    pthread_mutex_unlock(&g_gl_lock);
}

static void start_publish(int ok) {
    pthread_mutex_lock(&g_start_lock);
    g_start_state = ok ? 1 : -1;
    pthread_cond_broadcast(&g_start_cv);
    pthread_mutex_unlock(&g_start_lock);
}

int present_gl_make_current(void) {
    pthread_mutex_lock(&g_gl_lock);
    while (g_gl_state == 0) pthread_cond_wait(&g_gl_cv, &g_gl_lock);
    const int st = g_gl_state;
    SDL_Window *win = g_gl_win;
    SDL_GLContext ctx = g_gl_ctx;
    pthread_mutex_unlock(&g_gl_lock);
    if (st < 0) return -1;
    if (SDL_GL_MakeCurrent(win, ctx) != 0) {
        fprintf(stderr, "present: cannot make the GL context current: %s\n",
                SDL_GetError());
        return -1;
    }
    /* Swap on the GE thread's own schedule; the game paces itself and the
     * frame limiter is the clock's job, not the driver's -- except when the
     * Higher FPS cap is "unlimited", which otherwise has no limiter at all on
     * this path (the game's pacer is bypassed inside the enhanced loop and
     * the FPS clock only sleeps toward a positive cap). Unlimited then means
     * the display's own rate: vertical sync. */
    {
        const int unlimited = setting("HIGH_FPS", 0) && setting("FPS_CAP", 60) < 0;
        SDL_GL_SetSwapInterval(unlimited ? 1 : 0);
        if (unlimited) fprintf(stderr, "present: FPS cap unlimited -- pacing by vertical sync\n");
    }
    return 0;
}

void present_gl_swap(void) { if (g_gl_win) SDL_GL_SwapWindow(g_gl_win); }
static _Atomic uint64_t g_frames_rendered;
void present_note_frame(void) { atomic_fetch_add(&g_frames_rendered, 1); }

/* SDL owns window queries on its presentation thread.  Publish the physical
 * drawable size through atomics so the GE thread can scale its final blit
 * correctly on high-DPI displays without reaching back into SDL. */
void present_gl_drawable_size(int *w, int *h) {
    const uint64_t size = atomic_load(&g_gl_draw_size);
    if (w) *w = (int)(size >> 32);
    if (h) *h = (int)(size & UINT32_MAX);
}

void present_request_window_size(int w, int h) {
    if (w > 0 && h > 0)
        atomic_store(&g_requested_size, ((uint64_t)(uint32_t)w << 32) | (uint32_t)h);
}

/* As the modern pad above: the wide mapping in the GL backend spreads a scene
 * the game's camera compressed into 480 columns, so a title whose camera has
 * no aspect replacement must not be put into it -- the 3D would stretch and
 * the assembly's inset previews would leave their panels. The settings file is
 * shared by every title, so a preset saved on one reaches the others. */
int present_adaptive_aspect(void) {
    if (!setting("ASPECT", 0)) return 0;
    if (psp_title_can(PSP_TITLE_ADAPTIVE_ASPECT)) return 1;
    static int said;
    if (!said) {
        said = 1;
        fprintf(stderr, "present: this title has no native camera replacement for "
                        "the adaptive aspect -- keeping the original PSP view\n");
    }
    return 0;
}

/* The GL renderer latches the drawable once per present and sizes its
 * targets from that snapshot. The camera hooks read the same snapshot, so a
 * window drag cannot give one frame's projection and its placement different
 * sizes. Until the renderer's first latch the live drawable is used. (The
 * 3rd Birthday's present.c had this before it moved here.) */
static _Atomic uint64_t g_scene_latch;
void present_aspect_latch_scene_size(int w, int h) {
    if (w > 0 && h > 0)
        atomic_store(&g_scene_latch, ((uint64_t)(uint32_t)w << 32) | (uint32_t)h);
}

/* The drawable's shape at 272 rows, never narrower than 480. */
static void wide_scene(int draw_w, int draw_h, int *scene_w, int *scene_h) {
    long long ww = ((long long)SCREEN_H * draw_w + draw_h / 2) / draw_h;
    if (ww < SCREEN_W) ww = SCREEN_W;
    if (ww > 8192) ww = 8192;       /* a texture limit, and past any monitor */
    *scene_w = (int)ww; *scene_h = SCREEN_H;
}

void present_aspect_extent(int draw_w, int draw_h, int *scene_w, int *scene_h) {
    *scene_w = SCREEN_W; *scene_h = SCREEN_H;
    if (psp_title_info.scene_extent) psp_title_info.scene_extent(draw_w, draw_h, scene_w, scene_h);
    else if (draw_w > 0 && draw_h > 0) wide_scene(draw_w, draw_h, scene_w, scene_h);
}

void present_aspect_scene_size(int *scene_w, int *scene_h) {
    *scene_w = SCREEN_W; *scene_h = SCREEN_H;
    if (!present_adaptive_aspect()) return;
    const uint64_t latched = atomic_load(&g_scene_latch);
    if (latched) {
        *scene_w = (int)(latched >> 32);
        *scene_h = (int)(latched & UINT32_MAX);
        return;
    }
    int w = 0, h = 0;
    present_gl_drawable_size(&w, &h);
    present_aspect_extent(w, h, scene_w, scene_h);
}

int present_aspect_wide_width(void) {
    int w, h;
    present_aspect_scene_size(&w, &h);
    return w;
}

void *present_gl_proc(const char *name) { return SDL_GL_GetProcAddress(name); }

/* ---- the SDL thread -------------------------------------------------------- */

/* Window close and both shortcuts share the scheduler's normal shutdown. */
static void close_game_window(void) {
    save_dialog_shutdown();
    present_audio_report(stderr);
    /* Stop the run the way the host already stops one, rather than
     * _exit(0): that killed the process mid-drain and took the
     * whole end-of-run report with it. This is not a guest thread,
     * so it marks the guest threads dead and wakes the main
     * context in psp_sched_drain, which then reports "stopped by
     * the host (window closed)" and prints the summary. */
    atomic_store(&g_quit, 1);
    psp_sched_stop_all("window closed");
}

static void *sdl_thread(void *arg) {
    (void)arg;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "present: SDL_Init: %s -- running headless\n",
                SDL_GetError());
        if (g_gl_want) gl_publish(NULL, NULL, 0);
        start_publish(0);
        return NULL;
    }

    /* 4.0 core first: the GL backend's model program (the transform on the
     * GPU) needs GLSL 4.0 for its doubles. A driver that cannot give 4.0
     * gets the 3.3 core context the rest of the backend needs, and the model
     * program stays unbuilt. */
    int gl_major = 4, gl_minor = 0;
    if (g_gl_want) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, gl_major);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, gl_minor);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                            SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    }
    /* Windowed mode uses the saved size; borderless fullscreen uses the
     * current desktop resolution without switching the display mode. */
    const psp_settings *settings = psp_settings_current();
    int win_w = settings->width, win_h = settings->height;
    const double display_setting = setting("DISPLAY", -1);
    int display = display_setting < 0 ? 0 : (int)display_setting - 1;
    if (display >= SDL_GetNumVideoDisplays()) {
        fprintf(stderr, "present: display %d unavailable -- using primary display\n", display + 1);
        display = 0;
    }
    char window_title[128];
    snprintf(window_title, sizeof window_title, "%s -- recompiled", title_name());
    SDL_Window *win = SDL_CreateWindow(
        window_title,
        SDL_WINDOWPOS_CENTERED_DISPLAY(display), SDL_WINDOWPOS_CENTERED_DISPLAY(display),
        win_w, win_h,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
        (setting("WINDOW_MODE", 0) ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) |
        (g_gl_want ? SDL_WINDOW_OPENGL : 0));
    if (!win) {
        fprintf(stderr, "present: cannot create window: %s -- running headless\n",
                SDL_GetError());
        if (g_gl_want) gl_publish(NULL, NULL, 0);
        start_publish(0);
        SDL_Quit();
        return NULL;
    }

    /* With GL the renderer, the streaming texture and the blit below are all
     * skipped: the backend draws into the window itself. Creating an
     * SDL_Renderer on the same window would fight it for the context. */
    if (g_gl_want) {
        SDL_GLContext ctx = win ? SDL_GL_CreateContext(win) : NULL;
        if (!ctx && win) {
            gl_major = 3; gl_minor = 3;
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, gl_major);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, gl_minor);
            ctx = SDL_GL_CreateContext(win);
        }
        if (ctx) {
            /* Created here because SDL wants it on the thread that made the
             * window, released here because it has to be current on the GE
             * thread instead. */
            SDL_GL_MakeCurrent(win, NULL);
            int dw = 0, dh = 0;
            SDL_GL_GetDrawableSize(win, &dw, &dh);
            atomic_store(&g_gl_draw_size, ((uint64_t)(uint32_t)dw << 32) | (uint32_t)dh);
            fprintf(stderr, "present: GL %d.%d core context created, "
                            "handed to the GE thread\n", gl_major, gl_minor);
        } else {
            fprintf(stderr, "present: no GL 3.3 core context: %s\n",
                    SDL_GetError());
        }
        gl_publish(win, ctx, ctx != NULL);
        if (!ctx) {
            start_publish(0);
            SDL_DestroyWindow(win);
            SDL_Quit();
            return NULL;
        }
    }

    SDL_Renderer *ren = (win && !g_gl_want) ?
        SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED |
                                    SDL_RENDERER_PRESENTVSYNC) : NULL;
    if (ren) SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    /* The window is resizable, so pin the aspect: SDL letterboxes 480x272
     * inside whatever the user drags it to rather than stretching it. */
    if (ren) SDL_RenderSetLogicalSize(ren, SCREEN_W, SCREEN_H);
    SDL_Texture *tex = ren ?
        SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888,
                          SDL_TEXTUREACCESS_STREAMING, SCREEN_W, SCREEN_H) : NULL;
    if (!tex && !g_gl_want) {
        fprintf(stderr, "present: no window/renderer -- frames go nowhere\n");
        start_publish(0);
        if (ren) SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        SDL_Quit();
        return NULL;
    }

    SDL_AudioSpec want = { 0 };
    want.freq     = 44100;
    want.format   = AUDIO_S16SYS;
    want.channels = 2;
    want.samples  = 1024;
    want.callback = audio_device_callback;
    SDL_AudioSpec have;
    /* Lead 0 is two of the channel's own buffers; the pre-roll is 93 ms. */
    audio()->prepare(settings_ms_frames("AUDIO_LEAD_MS", 0),
                     settings_ms_frames("AUDIO_PREROLL_MS", 4096));
    g_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (g_audio_dev) {
        audio()->opened(g_audio_dev);
        SDL_PauseAudioDevice(g_audio_dev, 0);
    }
    else fprintf(stderr, "present: no audio device: %s\n", SDL_GetError());

    g_keys_wasd = setting("KEYS", 0) != 0;
    g_gamepad_modern = gamepad_modern();
    g_keys_modern = settings->input && psp_title_can(PSP_TITLE_MODERN_CONTROLS);
    if (g_keys_wasd && !g_keys_modern && psp_title_info.keys_wasd_classic_help)
        fprintf(stderr, "present: %s\n", psp_title_info.keys_wasd_classic_help);
    else if (g_keys_wasd && psp_title_info.keys_wasd_help)
        fprintf(stderr, "present: %s\n", psp_title_info.keys_wasd_help);
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
    if (g_gamepad_modern && psp_title_info.gamepad_modern_help)
        fprintf(stderr, "present: %s\n", psp_title_info.gamepad_modern_help);
    /* Mouse-look, only when asked for. Captured from the start so a run
     * launched for it is playable at once; Escape lets go, a click retakes.
     * After the GL handoff above on purpose -- that block owns the context
     * juggling and does not need company. */
    {
        g_mouse_want = settings->mouse;
        if (g_mouse_want) mouse_grab(1);
    }

    fprintf(stderr, "present: quit with Ctrl+Shift+Q or hold View + Menu (Select + Start) for 2 seconds\n");
    open_first_controller();

    if (save_dialog_init() != 0)
        fprintf(stderr, "present: in-game save dialog unavailable (%s); interactive save/load requests will be cancelled\n",
                save_dialog_error() ? save_dialog_error() : "unknown reason");

    start_publish(1);

    for (;;) {
        /* PSPRECOMP_WINDOW_RESIZE=<seconds>:WxH resizes the window once, that
         * many seconds after it opened, so the renderer's resize path is a
         * command rather than a drag. */
        {
            static int done, at_s = -1, rw, rh;
            static uint32_t t0;
            if (at_s < 0) {
                const char *rs = getenv("PSPRECOMP_WINDOW_RESIZE");
                int a = 0, w = 0, h = 0;
                at_s = rs && sscanf(rs, "%d:%dx%d", &a, &w, &h) == 3 &&
                       a > 0 && w > 0 && h > 0 ? a : 0;
                if (at_s <= 0) done = 1;
                rw = w; rh = h; t0 = SDL_GetTicks();
            }
            if (!done && win && SDL_GetTicks() - t0 >= (uint32_t)at_s * 1000u) {
                SDL_SetWindowSize(win, rw, rh);
                fprintf(stderr, "present: resized the window to %dx%d\n", rw, rh);
                done = 1;
            }
        }
        const uint64_t requested = atomic_exchange(&g_requested_size, 0);
        if (requested && win)
            SDL_SetWindowSize(win, (int)(requested >> 32), (int)(requested & UINT32_MAX));
        /* Logical window pixels and GL drawable pixels differ under desktop
         * scaling.  Refresh this on the SDL thread so resize and display-scale
         * changes are reflected by the next GL presentation. */
        if (g_gl_want && win) {
            int dw = 0, dh = 0;
            SDL_GL_GetDrawableSize(win, &dw, &dh);
            atomic_store(&g_gl_draw_size, ((uint64_t)(uint32_t)dw << 32) | (uint32_t)dh);
        }

        /* Publish the latest frame, if the guest has produced one. Waiting
         * bounded rather than forever keeps the window alive -- and showing
         * its last frame -- when the guest stalls, which it does. */
        struct timespec due;
        clock_gettime(CLOCK_REALTIME, &due);
        due.tv_nsec += 17 * 1000000;
        if (due.tv_nsec >= 1000000000) {
            due.tv_nsec -= 1000000000;
            due.tv_sec  += 1;
        }

        pthread_mutex_lock(&g_frame_lock);
        if (!g_frame_fresh)
            pthread_cond_timedwait(&g_frame_cv, &g_frame_lock, &due);
        const int fresh = g_frame_fresh;
        if (fresh) {
            memcpy(g_frame_present, g_frame_rgba, sizeof g_frame_present);
            g_frame_fresh = 0;
        }
        pthread_mutex_unlock(&g_frame_lock);

        /* Outside the lock. On a timeout there is no new frame, so the texture
         * keeps the last one and the window still repaints -- which is the
         * point of the bounded wait. */
        if (tex && fresh)
            SDL_UpdateTexture(tex, NULL, g_frame_present, SCREEN_W * 4);
        if (fresh && !g_gl_want) present_note_frame();
        /* The frame rate in the title, once a second: rendered frames over
         * wall time, whichever backend rendered them. Set from this thread,
         * which owns the window. While the guest is held the title says so,
         * and the count starts again when it is let go. */
        {
            static uint32_t title_t0; static uint64_t title_frames; static char last[128];
            static int was_quitting, was_paused; static double shown_fps;
            const uint32_t now = SDL_GetTicks();
            /* The quit chord shows in the title the moment it is held, so a
             * player sees the countdown rather than an unexplained close. */
            const int quitting = g_quit_chord.timing;
            const int paused = psp_paused();
            if (!title_t0 || paused != was_paused) { title_t0 = now; title_frames = atomic_load(&g_frames_rendered); }
            if ((now - title_t0 >= 1000 || quitting != was_quitting || paused != was_paused) && win) {
                if (now - title_t0 >= 1000) {
                    const uint64_t frames = atomic_load(&g_frames_rendered);
                    shown_fps = (double)(frames - title_frames) * 1000.0 / (double)(now - title_t0);
                    title_t0 = now; title_frames = frames;
                }
                char title[128];
                if (paused)
                    snprintf(title, sizeof title, "%s -- recompiled  |  paused%s", title_name(),
                             quitting ? "  |  QUITTING: keep holding Select + Start, release to cancel" : "");
                else
                    snprintf(title, sizeof title, "%s -- recompiled  |  %.0f fps%s", title_name(), shown_fps,
                             quitting ? "  |  QUITTING: keep holding Select + Start, release to cancel" : "");
                if (strcmp(title, last) != 0) { SDL_SetWindowTitle(win, title); snprintf(last, sizeof last, "%s", title); }
                was_quitting = quitting;
                was_paused = paused;
            }
        }

        const int was_dialog = save_dialog_active();
        save_dialog_update(g_controller);
        if (save_dialog_active() && !was_dialog) {
            g_dialog_block_input = 1;
            g_dialog_closed_at = 0;
            g_dialog_mouse_was_grabbed = g_mouse_grabbed;
            mouse_grab(0);
            clear_keys();
            clear_controller();
            g_mouse_down = 0;
            atomic_store(&g_mouse_buttons, 0);
            psp_ctrl_clear_mouse();
            publish_pad();
        } else if (!save_dialog_active() && g_dialog_block_input) {
            /* Game input resumes once the dialog's own controls are released,
             * or after a second regardless: a key or button whose release
             * SDL never reports must not leave the game deaf. */
            const Uint64 now = SDL_GetTicks64();
            if (!g_dialog_closed_at) g_dialog_closed_at = now;
            const int neutral = save_dialog_input_neutral(g_controller);
            if (neutral || now - g_dialog_closed_at >= 1000) {
                if (!neutral)
                    fprintf(stderr, "present: input still held a second after the save dialog closed; resuming game input\n");
                g_dialog_block_input = 0;
                g_dialog_closed_at = 0;
                clear_keys();
                clear_controller();
                sample_controller();
                publish_pad();
                if (g_dialog_mouse_was_grabbed) mouse_grab(1);
                g_dialog_mouse_was_grabbed = 0;
            }
        }
        if (tex) {
            SDL_RenderCopy(ren, tex, NULL, NULL);
            save_dialog_draw_software(ren);
            SDL_RenderPresent(ren);
        }

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (quit_key_event(&e)) {
                fprintf(stderr, "present: keyboard quit shortcut\n");
                e.type = SDL_QUIT;
            }
            if (save_dialog_event(&e, g_controller_id) ||
                (g_dialog_block_input && (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP ||
                 e.type == SDL_MOUSEMOTION || e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP ||
                 e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_CONTROLLERBUTTONUP ||
                 e.type == SDL_CONTROLLERAXISMOTION))) {
                if ((e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_CONTROLLERBUTTONUP) &&
                    e.cbutton.which == g_controller_id)
                    quit_chord_button(e.cbutton.button, e.cbutton.state, SDL_GetTicks64());
                continue;
            }
            switch (e.type) {
            case SDL_QUIT:
                close_game_window();
                return NULL;
            case SDL_CONTROLLERDEVICEADDED: {
                /* One controller owns this lane.  Opening every connected pad
                 * let an idle second device overwrite the one in the hand. */
                open_controller(e.cdevice.which);
                break;
            }
            case SDL_CONTROLLERDEVICEREMOVED: {
                if (e.cdevice.which != g_controller_id) break;
                fprintf(stderr, "present: active controller removed; input cleared\n");
                SDL_GameControllerClose(g_controller);
                g_controller = NULL;
                g_controller_id = -1;
                clear_controller();
                open_first_controller();
                publish_pad();
                break;
            }
            case SDL_CONTROLLERBUTTONDOWN:
            case SDL_CONTROLLERBUTTONUP:
                if (e.cbutton.which != g_controller_id) break;
                set_button(e.cbutton.button, e.cbutton.state);
                publish_pad();
                break;
            case SDL_CONTROLLERAXISMOTION:
                if (e.caxis.which != g_controller_id) break;
                set_axis(e.caxis.axis, e.caxis.value);
                publish_pad();
                break;
            case SDL_MOUSEMOTION:
                /* Relative motion only while captured: a pointer crossing an
                 * uncaptured window is not a look. */
                if (g_mouse_grabbed)
                    psp_ctrl_add_mouse(e.motion.xrel, e.motion.yrel);
                break;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
                /* The click that captures the pointer is not also a shot. */
                if (!g_mouse_grabbed) { if (e.type == SDL_MOUSEBUTTONDOWN) mouse_grab(1); break; }
                if (g_keys_wasd && e.button.button < 4 && mouse_wasd()[e.button.button]) {
                    set_mouse_button(e.button.button, e.type == SDL_MOUSEBUTTONDOWN);
                    publish_pad();
                }
                break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                    mouse_grab(0);
                    clear_keys();
                    g_mouse_down = 0;
                    atomic_store(&g_mouse_buttons, 0);
                    clear_controller();
                    publish_pad();
                } else if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                    /* SDL need not emit motion/button events for controls that
                     * stayed held while another window owned focus. */
                    sample_controller();
                    publish_pad();
                }
                break;
            case SDL_KEYDOWN:
            case SDL_KEYUP:
                if (e.key.keysym.sym == SDLK_ESCAPE) {
                    if (e.type == SDL_KEYDOWN) mouse_grab(0);
                    break;
                }
                set_key(e.key.keysym.scancode, e.key.state == SDL_PRESSED);
                publish_pad();
                break;
            }
        }
        if (quit_chord_due(SDL_GetTicks64())) {
            fprintf(stderr, "present: controller quit shortcut (held 2 seconds)\n");
            close_game_window();
            return NULL;
        }
    }
}

int present_start(void) {
    /* Under GL the backend owns the window and swaps for itself, so the
     * guest-thread conversion this hook does -- 130k pixels into RGBA, every
     * frame -- would be work whose result nothing reads. */
    if (!g_gl_want) psp_display_set_present(present_frame);
    psp_audio_set_output(audio()->output);
    if (audio()->pending) psp_audio_set_pending(audio()->pending);

    /* Queue depth target: two buffers -- deep enough that jitter never
     * underruns, shallow enough that the backlog tracks real playback. */

    if (pthread_cond_init(&g_frame_cv, NULL) != 0) {
        psp_display_set_present(NULL);
        psp_audio_set_output(NULL);
        psp_audio_set_pending(NULL);
        return -1;
    }
    if (pthread_create(&g_thread, NULL, sdl_thread, NULL) != 0) {
        fprintf(stderr, "present: cannot start the SDL thread\n");
        psp_display_set_present(NULL);
        psp_audio_set_output(NULL);
        psp_audio_set_pending(NULL);
        pthread_cond_destroy(&g_frame_cv);
        return -1;
    }

    /* Presentation is host setup, not game execution. Wait until it is usable
     * before anchoring the real-time guest clock, so driver and audio startup
     * cannot become an initial animation jump. All SDL failure exits publish
     * a negative state, so this also makes present_start's documented return
     * value truthful instead of reporting success before SDL has run. */
    pthread_mutex_lock(&g_start_lock);
    while (g_start_state == 0)
        pthread_cond_wait(&g_start_cv, &g_start_lock);
    const int ready = g_start_state > 0;
    pthread_mutex_unlock(&g_start_lock);
    if (!ready) {
        psp_display_set_present(NULL);
        psp_audio_set_output(NULL);
        psp_audio_set_pending(NULL);
        pthread_join(g_thread, NULL);
        pthread_cond_destroy(&g_frame_cv);
        return -1;
    }

    if (audio()->started) audio()->started(g_audio_dev != 0, g_gl_want);
    psp_clock_realtime(1);
    return 0;
}
