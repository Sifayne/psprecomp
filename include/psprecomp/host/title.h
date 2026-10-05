#ifndef PSPRECOMP_HOST_TITLE_H
#define PSPRECOMP_HOST_TITLE_H
/* What the shared host needs to know about the game it runs.
 *
 * Every program built with the host defines psp_title_info once: a game next
 * to its replacements, a fixture or tool for itself. It is a strong symbol on
 * purpose. The weak capability flags it replaces read as "absent" whenever a
 * program forgot them, and a missing definition now fails the link instead.
 * The player's title packs grow this into a registration call
 * (docs/PLAYER-LAYER.md).
 *
 * Everything after the name and capabilities is optional; a zero field keeps
 * the shared host's own behaviour. */
#include <stdint.h>

enum {
    /* The title decodes the carrier bits (psprecomp/host/pad.h), so the
     * modern controller layout can send them. */
    PSP_TITLE_MODERN_CONTROLS = 1u << 0,
    /* The title's camera follows the wide target, so the adaptive aspect
     * can widen the scene. */
    PSP_TITLE_ADAPTIVE_ASPECT = 1u << 1,
    /* The title places off-screen HUD draws in the wide bands. */
    PSP_TITLE_HUD_BANDS       = 1u << 2,
};

/* key is an SDL_Keycode; bit a PSP button or a carrier. */
typedef struct { int32_t key; uint32_t bit; } psp_key_bind;

struct psp_audio_backend;           /* psprecomp/host/present.h */

typedef struct {
    const char *name;               /* the window title's first part */
    unsigned capabilities;          /* PSP_TITLE_* */
    /* Startup notes describing the title's own controls: the WASD keyboard
     * layout (and, if it differs, the same while the title's Modern
     * controls are off), and the modern controller layout. */
    const char *keys_wasd_help, *gamepad_modern_help, *keys_wasd_classic_help;

    /* Input, until stage 6 of docs/PLAYER-LAYER.md makes bindings data. The
     * title's Modern controls are on when its resolved settings say input
     * and it has PSP_TITLE_MODERN_CONTROLS. */
    const psp_key_bind *keys_wasd;  /* the WASD layout, replacing the host's */
    unsigned keys_wasd_count;
    const uint32_t *mouse_wasd;     /* [4]: what SDL mouse buttons 1-3 press */
    /* What the WASD keys and the mouse buttons press while Modern controls
     * are off, from what the tables above say. */
    uint32_t (*classic_keys)(uint32_t buttons);
    uint32_t (*classic_mouse)(uint32_t buttons);
    /* The stick byte for one axis of WASD: along and across are -1, 0 or 1,
     * walk is Left Alt. The host's is a full push. */
    uint8_t (*key_axis)(int along, int across, int walk);
    /* A carrier for a controller button the layout leaves unmapped, or 0
     * (an SDL_GameControllerButton). */
    uint32_t (*pad_button)(int button);

    /* The adaptive aspect's scene for a drawable: the host's widens to the
     * drawable's shape at 272 rows. */
    void (*scene_extent)(int draw_w, int draw_h, int *scene_w, int *scene_h);

    /* The title's own audio output; NULL uses the host's mixer. */
    const struct psp_audio_backend *audio;
} psp_title;

extern const psp_title psp_title_info;

static inline int psp_title_can(unsigned capability) {
    return (psp_title_info.capabilities & capability) != 0;
}
#endif
