#ifndef PSPRECOMP_HOST_LAUNCHER_H
#define PSPRECOMP_HOST_LAUNCHER_H
/* What the player's launcher (src/host/launcher.c) needs from a pack besides
 * its settings schema.
 *
 * The launcher draws its pages from the schema: the page names in the order
 * they first appear, each page's options in table order, leaving out those
 * marked PSP_OPTION_HIDDEN. The schema's title heads the screen. Everything
 * else that is the app's own is here. A pack defines psp_launcher_info once,
 * in a source its launcher links; like psp_title_settings, a launcher without
 * it fails to link. */

typedef struct {
    /* Heads the screen and names the window until a game is chosen, and
     * names the preferences folder a run without --config uses. */
    const char *name;
    /* The app id: the data folder under XDG_DATA_HOME that holds saves when
     * LR_DATA_ROOT does not name one. AppRun passes the same. */
    const char *id;
    const char *about;              /* the About box */
    /* The title slugs in the order their tabs appear, NULL-terminated; any
     * other slug follows them, in slug order. */
    const char *const *titles;
    /* Space-separated KEY=value assignments over the defaults: what New
     * starts a preset from, and what Reset restores. Without a movie decoder
     * MPEG_DECODE is then switched off. */
    const char *new_preset, *reset_preset;
} psp_launcher;

extern const psp_launcher psp_launcher_info;
#endif
