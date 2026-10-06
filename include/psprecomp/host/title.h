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

    /* The glow composite the smooth bloom filter recognises; NULL: the
     * title has none, and the filter does nothing. */
    const psp_bloom_composite *bloom;
} psp_title;

extern const psp_title psp_title_info;

static inline int psp_title_can(unsigned capability) {
    return (psp_title_info.capabilities & capability) != 0;
}
#endif
