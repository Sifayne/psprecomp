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
#include "psprecomp/host/settings.h"

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
    /* At a wide aspect the GL backend places the title's screen-space draws
     * by what they read: one that reads a render target composites the
     * scene and fills it, at any width, and other textured 2D -- movies and
     * menus too, full width or not -- keeps its proportions in the centred
     * area. Without it every full-width screen-space draw fills the scene
     * (Armored Core's movies in strips, bars and fades). The 3rd Birthday's. */
    PSP_TITLE_ASPECT_BY_SOURCE = 1u << 3,
};

/* The GL backend's smooth bloom filter (setting BLOOM_FILTER): the title's
 * final glow composite, which it draws from a small scratch target as a
 * full-screen sprite. The backend recognises the draw by this description,
 * not by an address, and samples it with a soft cubic instead of the PSP's
 * bilinear steps; nothing else changes. */
typedef struct {
    uint16_t w, h;          /* the scratch target, in pixels; its stride is w */
    uint8_t target_fmt;     /* the pixel format it is drawn in (GE 0..3) */
    uint8_t tex_fmt;        /* the texture format the composite reads it as */
    uint8_t tex_func;       /* the composite's texture function */
    uint8_t blend_src, blend_dst;   /* and its blend factors (equation add) */
} psp_bloom_composite;

/* key is an SDL_Keycode, button an SDL_GameControllerButton; bit a PSP
 * button or a carrier. */
typedef struct { int32_t key; uint32_t bit; } psp_key_bind;
typedef struct { int32_t button; uint32_t bit; } psp_pad_bind;

/* A title action: a carrier bit (psprecomp/host/pad.h) that the title's
 * replacements decode, with the name the bindings know it by (bind.pad.fire)
 * and the label the overlay will show. While the title's Modern controls are
 * off for a device, the device's carrier becomes the PSP buttons in classic
 * before the game sees it -- or stays, with PSP_ACTION_KEEP. */
#define PSP_ACTION_KEEP 0xFFFFFFFFu
typedef struct {
    const char *name, *label;
    uint32_t carrier, classic;
} psp_title_action;

/* What a title adds to the host's input (src/host/input.c), for its resolved
 * settings. Everything is optional. */
typedef struct {
    /* Its actions; a carrier no action names is not bound by default. */
    const psp_title_action *actions;
    unsigned action_count;
    /* The WASD keyboard layout, replacing the host's, and what SDL mouse
     * buttons 1-3 press in it ([4]). */
    const psp_key_bind *keys_wasd;
    unsigned keys_wasd_count;
    const uint32_t *mouse_wasd;
    /* Controller buttons that press these in either controller layout,
     * replacing what the layout gives them. */
    const psp_pad_bind *pad;
    unsigned pad_count;
    /* How far the keyboard's stick reaches while walk (Left Alt) is held:
     * along one axis, and on each axis of a diagonal. 0: a full push. */
    uint8_t walk, walk_diagonal;
} psp_title_input;

/* What the host does with these, for a title's own tests: a device's buttons
 * while the title's Modern controls are off for it, and the keyboard stick's
 * byte for one axis (along and across are -1, 0 or 1). */
static inline uint32_t psp_title_classic(const psp_title_input *in, uint32_t buttons) {
    uint32_t out = buttons;
    for (unsigned i = 0; i < in->action_count; i++) {
        const psp_title_action *a = &in->actions[i];
        if (a->classic == PSP_ACTION_KEEP) continue;
        out &= ~a->carrier;
        if (buttons & a->carrier) out |= a->classic;
    }
    return out;
}
static inline uint8_t psp_title_key_axis(const psp_title_input *in, int along, int across, int walk) {
    const int reach = !walk || !in->walk ? 127 : along && across ? in->walk_diagonal : in->walk;
    return (uint8_t)(128 + reach * along);
}

struct psp_audio_backend;           /* psprecomp/host/present.h */

typedef struct {
    const char *name;               /* the window title's first part */
    unsigned capabilities;          /* PSP_TITLE_* */
    /* Startup notes describing the title's own controls: the WASD keyboard
     * layout (and, if it differs, the same while the title's Modern
     * controls are off), and the modern controller layout. */
    const char *keys_wasd_help, *gamepad_modern_help, *keys_wasd_classic_help;

    /* The title's input, for its resolved settings. Its Modern controls are
     * on for the keyboard and mouse when the settings say input, and for the
     * controller when they say gamepad -- each only with
     * PSP_TITLE_MODERN_CONTROLS. */
    void (*input)(const psp_settings *s, psp_title_input *out);

    /* The adaptive aspect's scene for a drawable: the host's widens to the
     * drawable's shape at 272 rows. */
    void (*scene_extent)(int draw_w, int draw_h, int *scene_w, int *scene_h);

    /* The title's own audio output; NULL uses the host's mixer. */
    const struct psp_audio_backend *audio;

    /* The glow composite the smooth bloom filter recognises; NULL: the
     * title has none, and the filter does nothing. */
    const psp_bloom_composite *bloom;

    /* The boot host's (src/host/boot.c), each optional:
     * - settings: the title's policy over the loaded settings, before they
     *   are printed or used -- a default only it needs, or a fallback for a
     *   host linked without its replacements.
     * - start: what it sets up from them before the module loads; nonzero
     *   refuses to run (the reason on stderr).
     * - keep: names what its replacements carry from one poll to the next
     *   to a save state (psprecomp/state.h). */
    void (*settings)(psp_settings *s);
    int  (*start)(const psp_settings *s);
    void (*keep)(void);
} psp_title;

extern const psp_title psp_title_info;

static inline int psp_title_can(unsigned capability) {
    return (psp_title_info.capabilities & capability) != 0;
}
#endif
