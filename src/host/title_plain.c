/* A game with no pack (docs/PLAYER-LAYER.md, stage 11): the plain
 * recompiled game, with the player's settings alone and nothing a pack adds
 * -- no replacements, no controls of its own, no hooks. The app compiles this
 * with the game, beside a source it writes naming the game as its disc does
 * (psp_plain_title) and listing the modules it loads at run time
 * (psp_plain_modules, docs/MODULES.md), PSP_PLAIN_MODULE_COUNT of them. */
#include "psprecomp/host/settings.h"
#include "psprecomp/host/title.h"

#ifndef PSP_PLAIN_MODULE_COUNT
#define PSP_PLAIN_MODULE_COUNT 0
#endif

extern const char psp_plain_title[];
extern const psp_title_module psp_plain_modules[];

const psp_title psp_title_info = {
    .name = psp_plain_title,
    .modules = psp_plain_modules,
    .module_count = PSP_PLAIN_MODULE_COUNT,
};

/* The player's options only: its section of the preferences file is
 * [player]. */
const psp_settings_schema psp_title_settings = { .title = "psprecomp" };
