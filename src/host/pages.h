/* The settings pages the in-game menu (overlay.c) and the launcher
 * (launcher.c) share: which pages the options make, and one option's row,
 * drawn with ui.h. Internal to the host layer; SDL thread only.
 *
 * A row offers what a player can choose rather than a free number where it
 * can: a choice's labels, an option's stops (psp_option_def.stops) with
 * "Other..." for a value of the player's own, the connected displays for
 * DISPLAY, and a slider otherwise. The caller sets what was chosen -- the
 * menu at once where it can apply it, the launcher into its unsaved
 * settings -- so a row never changes settings itself. */
#ifndef PSPRECOMP_HOST_PAGES_H
#define PSPRECOMP_HOST_PAGES_H

#include "psprecomp/host/settings.h"

#include <stddef.h>

typedef struct {
    int menu;               /* in the game: mark what applies only at the next start */
    int movie_available;    /* MPEG_DECODE can be turned on */
} pages_context;

enum { PAGES_MAX = 16 };

/* The pages of options from..to-1, in the order their first shown option
 * appears, with Advanced last and skip (if any) left out. */
int pages_list(int from, int to, const char *skip, const char **names, int max);
/* Whether option id is shown on page. */
int pages_on(int id, const char *page);
/* One option's row. 1 when the player chose a value, its text in value. */
int pages_option(const psp_settings *s, int id, const pages_context *c, char *value, size_t size);

#endif
