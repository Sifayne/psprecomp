/* The pack list of the player's app, which was linked with none: its packs
 * are installed, and the launcher loads them (psprecomp/host/launcher.h).
 * The settings mechanism still names a schema to fall back to; this one is
 * the player's options alone. */
#include "psprecomp/host/launcher.h"

#include <stddef.h>

const psp_settings_schema psp_title_settings = { .title = "psprecomp" };
const psp_pack psp_packs[1] = { { NULL, NULL } };
const int psp_pack_count = 0;
