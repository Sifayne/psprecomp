#ifndef PSPRECOMP_HOST_TITLE_H
#define PSPRECOMP_HOST_TITLE_H
/* What the shared host needs to know about the game it runs.
 *
 * Every program built with the host defines psp_title_info once: a game next
 * to its replacements, a fixture or tool for itself. It is a strong symbol on
 * purpose. The weak capability flags it replaces read as "absent" whenever a
 * program forgot them, and a missing definition now fails the link instead.
 * The player's title packs grow this into a registration call
 * (docs/PLAYER-LAYER.md). */

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

typedef struct {
    const char *name;               /* the window title's first part */
    unsigned capabilities;          /* PSP_TITLE_* */
    /* Optional startup notes describing the title's own controls: the WASD
     * keyboard layout and the modern controller layout. */
    const char *keys_wasd_help, *gamepad_modern_help;
} psp_title;

extern const psp_title psp_title_info;

static inline int psp_title_can(unsigned capability) {
    return (psp_title_info.capabilities & capability) != 0;
}
#endif
