#ifndef PSPRECOMP_HOST_LAUNCHER_H
#define PSPRECOMP_HOST_LAUNCHER_H
/* What the player's launcher (src/host/launcher.c) needs from each pack it
 * holds besides its settings schema.
 *
 * The launcher draws its pages from the settings: the player's pages, for
 * every game, then the selected game's pack's, each page's options in table
 * order, leaving out those marked PSP_OPTION_HIDDEN. Everything else that is
 * the pack's own is here. A pack defines psp_launcher_info once, in a source
 * its launcher links. */
#include "psprecomp/host/settings.h"

typedef struct {
    const char *name;               /* the pack, where the launcher names it */
    /* Optional: the name its game's own launcher kept settings under, in
     * SDL's preference folder of that name. The launcher brings them in
     * once, as it does those of the pack's own app, in the XDG config folder
     * named by its schema's id. */
    const char *earlier;
    const char *about;              /* its part of the About page */
    /* Its title slugs in the order their tabs appear, NULL-terminated, and
     * their names, in the same order. */
    const char *const *titles;
    const char *const *names;
} psp_launcher;

extern const psp_launcher psp_launcher_info;

/* The packs a launcher was linked with. A game's own launcher links
 * src/host/launcher_one.c, which names its pack's psp_launcher_info and
 * psp_title_settings; the player's app links src/host/launcher_none.c and
 * finds its packs installed instead. */
typedef struct {
    const psp_launcher *info;
    const psp_settings_schema *settings;
} psp_pack;

extern const psp_pack psp_packs[];
extern const int psp_pack_count;

/* An installed pack's launcher part: its settings.c and launcher_info.c
 * with src/host/pack_api.c, built by the importer into launcher.so in the
 * pack's folder and loaded by the launcher. It exports psp_title_settings,
 * psp_launcher_info and psp_pack_api, which must equal this number: the
 * layout of the structures above, raised whenever one changes. */
#define PSP_PACK_API 1
extern const int psp_pack_api;
#endif
