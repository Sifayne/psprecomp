/* An installed pack's launcher part, for tests/test_launcher.c: built with
 * src/host/pack_api.c into packs/gamma/built/launcher.so, as the importer builds a
 * pack's settings.c and launcher_info.c. Its resolve hook calls back into
 * the launcher's settings mechanism, as a pack's helpers do. */
#include "psprecomp/host/launcher.h"

#include <stddef.h>
#include <string.h>

static const psp_option_def options[] = {
    {.key = "GLOW", .env = "PSPRECOMP_GLOW", .label = "Glow", .page = "Picture", .help = "Help",
     .type = PSP_OPTION_CHOICE, .dflt = "0", .choices = "0|1", .labels = "Off|On"},
};
static int resolve(psp_settings *s, char *error) {
    (void)error;
    s->title[0] = psp_settings_find("GLOW") == PSP_PLAYER_OPTIONS && s->number[PSP_PLAYER_OPTIONS] != 0;
    return 0;
}
const psp_settings_schema psp_title_settings = {
    .title = "Gamma", .id = "gamma", .options = options, .count = 1, .play_defaults = "GLOW=1", .resolve = resolve,
};
static const char *const titles[] = {"gam", NULL};
static const char *const names[] = {"Gamma Game", NULL};
const psp_launcher psp_launcher_info = { .name = "Gamma", .about = "About Gamma.", .titles = titles, .names = names };
