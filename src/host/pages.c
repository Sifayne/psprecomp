/* The settings pages the in-game menu and the launcher share. See pages.h. */
#include "pages.h"

#include "ui.h"

#include <SDL2/SDL.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { CHOICES_MAX = 24 };

/* "Other...": a value typed in, for the option waiting on it. */
static struct {
    int id;                 /* -1: none */
    char text[PSP_SETTINGS_VALUE];
} other = { -1, "" };

static const char ADVANCED[] = "Advanced";

static int shown(const psp_option_def *d) { return d->page && !(d->flags & PSP_OPTION_HIDDEN); }

int pages_on(int id, const char *page) {
    const psp_option_def *d = psp_settings_option(id);
    return d && shown(d) && !strcmp(d->page, page);
}

int pages_list(int from, int to, const char *skip, const char **names, int max) {
    int n = 0, advanced = 0;
    for (int k = from; k < to; k++) {
        const psp_option_def *d = psp_settings_option(k);
        if (!d || !shown(d) || (skip && !strcmp(d->page, skip))) continue;
        if (!strcmp(d->page, ADVANCED)) { advanced = 1; continue; }
        int seen = 0;
        for (int p = 0; p < n; p++) seen |= !strcmp(names[p], d->page);
        if (!seen && n < max) names[n++] = d->page;
    }
    if (advanced && n < max) names[n++] = ADVANCED;
    return n;
}

/* The index-th '|'-separated field. */
static int field(const char *list, int index, char *out, size_t size) {
    if (!list) return 0;
    while (index-- > 0) { list = strchr(list, '|'); if (!list) return 0; list++; }
    const size_t n = strcspn(list, "|");
    snprintf(out, size, "%.*s", (int)n, list);
    return 1;
}

/* A list of what the player can pick: labels and the values they stand for;
 * an empty value is "Other...". */
typedef struct {
    int count, at;
    char label[CHOICES_MAX][PSP_SETTINGS_VALUE];
    char value[CHOICES_MAX][PSP_SETTINGS_VALUE];
} picks;

static void pick(picks *p, const char *label, const char *value, int current) {
    if (p->count == CHOICES_MAX) return;
    snprintf(p->label[p->count], sizeof p->label[0], "%s", label);
    snprintf(p->value[p->count], sizeof p->value[0], "%s", value);
    if (current) p->at = p->count;
    p->count++;
}

/* How a value of option id is shown. */
static void label_of(const psp_settings *s, int id, const char *value, char *out, size_t size) {
    psp_settings t = *s;
    char error[PSP_SETTINGS_ERROR];
    if (psp_settings_set(&t, id, value, PSP_SOURCE_FILE, error)) snprintf(out, size, "%s", value);
    else psp_option_label(&t, id, out, size);
}

static void display_picks(const psp_settings *s, picks *p) {
    const int screen = (int)s->number[PSP_OPT_DISPLAY];
    int count = SDL_GetNumVideoDisplays();
    if (count < 0) count = 0;
    pick(p, "Primary display", "primary", screen < 0);
    for (int i = 1; i <= count && i < CHOICES_MAX - 1; i++) {
        char label[PSP_SETTINGS_VALUE], value[16];
        const char *name = SDL_GetDisplayName(i - 1);
        if (name && *name) snprintf(label, sizeof label, "%d: %.80s", i, name);
        else snprintf(label, sizeof label, "Display %d", i);
        snprintf(value, sizeof value, "%d", i);
        pick(p, label, value, screen == i);
    }
    if (screen > count) {
        char label[PSP_SETTINGS_VALUE];
        snprintf(label, sizeof label, "Display %d (unavailable)", screen);
        pick(p, label, s->value[PSP_OPT_DISPLAY], 1);
    }
}

static void stop_picks(const psp_settings *s, int id, picks *p) {
    const psp_option_def *d = psp_settings_option(id);
    char stop[PSP_SETTINGS_VALUE], label[PSP_SETTINGS_VALUE];
    int found = 0;
    for (int i = 0; field(d->stops, i, stop, sizeof stop); i++) {
        label_of(s, id, stop, label, sizeof label);
        psp_settings t = *s;
        char error[PSP_SETTINGS_ERROR];
        const int same = !psp_settings_set(&t, id, stop, PSP_SOURCE_FILE, error) && !strcmp(t.value[id], s->value[id]);
        found |= same;
        pick(p, label, stop, same);
    }
    /* A value of the player's own stays on the list, after the stops. */
    if (!found) {
        psp_option_label(s, id, label, sizeof label);
        pick(p, label, s->value[id], 1);
    }
    pick(p, "Other...", "", 0);
}

/* Text: what it is now, and a way to type another. */
static void text_picks(const psp_settings *s, int id, picks *p) {
    pick(p, s->value[id], s->value[id], 1);
    pick(p, "Change...", "", 0);
}

static void choice_picks(const psp_settings *s, int id, picks *p) {
    const psp_option_def *d = psp_settings_option(id);
    char value[PSP_SETTINGS_VALUE], label[PSP_SETTINGS_VALUE];
    for (int i = 0; field(d->choices, i, value, sizeof value); i++) {
        /* Null is for files and the command line, not for play. */
        if (id == PSP_OPT_RENDER && !strcmp(value, "null") && s->number[id] != i) continue;
        if (!field(d->labels, i, label, sizeof label)) snprintf(label, sizeof label, "%s", value);
        /* What Automatic gives, for the console's language and its confirm
         * button, which follows the language. */
        if ((id == PSP_OPT_LANGUAGE || id == PSP_OPT_CONFIRM) && !strcmp(value, "auto")) {
            const int lang = id == PSP_OPT_LANGUAGE || !s->number[PSP_OPT_LANGUAGE]
                           ? psp_settings_host_language() : (int)s->number[PSP_OPT_LANGUAGE] - 1;
            char named[PSP_SETTINGS_VALUE];
            if (id == PSP_OPT_CONFIRM) snprintf(named, sizeof named, "%s", lang ? "Cross" : "Circle");
            else if (!field(psp_settings_option(PSP_OPT_LANGUAGE)->labels, lang + 1, named, sizeof named))
                snprintf(named, sizeof named, "English");
            snprintf(label, sizeof label, "Automatic (%.60s)", named);
        }
        pick(p, label, value, s->number[id] == i);
    }
}

/* A number with no stops: a slider, after a choice between the special word
 * (Auto, Off, the game's own) and a value of the player's, where it has one. */
static int number_row(const psp_settings *s, int id, const char *label, const char *help,
                      char *value, size_t size) {
    const psp_option_def *d = psp_settings_option(id);
    const double scale = d->scale ? d->scale : 1;
    int custom = !(d->special && s->number[id] < 0);
    if (d->special) {
        char special[64];
        snprintf(special, sizeof special, "%s", d->special_label ? d->special_label : d->special);
        if (!d->special_label && special[0] >= 'a' && special[0] <= 'z') special[0] -= 'a' - 'A';
        const char *labels[2] = { special, "Custom" };
        const int was = custom;
        if (psp_ui_choice(label, &custom, labels, 2) && custom != was) {
            if (custom) snprintf(value, size, "%.9g", d->min > 0 ? d->min : 0);
            else snprintf(value, size, "%s", d->special);
            psp_ui_help(help);
            return 1;
        }
        psp_ui_help(help);
    }
    if (!custom) return 0;
    double v = s->number[id] * scale;
    char bare[PSP_SETTINGS_NAME + 4];
    snprintf(bare, sizeof bare, "##%s", d->key);
    if (!psp_ui_number(d->special ? bare : label, &v, d->min * scale, d->max * scale,
                       d->format ? d->format : "%.2f")) return 0;
    double n = v / scale;
    if (d->step > 0) n = d->min + floor((n - d->min) / d->step + 0.5) * d->step;
    if (d->type == PSP_OPTION_INTEGER) n = floor(n + 0.5);
    snprintf(value, size, "%.9g", n < d->min ? d->min : n > d->max ? d->max : n);
    return 1;
}

int pages_option(const psp_settings *s, int id, const pages_context *c, char *value, size_t size) {
    const psp_option_def *d = psp_settings_option(id);
    const int locked = s->source[id] == PSP_SOURCE_ENV;
    const int later = c->menu && !(d->flags & PSP_OPTION_LIVE);
    const int no_movie = id == PSP_OPT_MPEG_DECODE && !c->movie_available;
    const int desktop = id == PSP_OPT_WINDOW_SIZE && s->number[PSP_OPT_WINDOW_MODE] != 0;
    char label[160], help[1024];
    snprintf(label, sizeof label, "%s%s##%s", d->label, later ? " *" : "", d->key);
    snprintf(help, sizeof help, "%s%s%s%s", d->help ? d->help : "",
             later ? " Applies the next time the game starts." : "",
             no_movie ? " This build has no movie decoder." : "",
             locked ? " Set by the environment for this run." : "");
    int chosen = 0;
    psp_ui_disabled_begin(locked || desktop || (no_movie && !s->number[id]));
    if (desktop) {
        int at = 0;
        const char *labels[1] = { "Desktop size" };
        psp_ui_choice(label, &at, labels, 1);
    } else if (d->type == PSP_OPTION_CHOICE || d->type == PSP_OPTION_TEXT || d->stops || id == PSP_OPT_DISPLAY) {
        static picks p;
        memset(&p, 0, sizeof p);
        if (id == PSP_OPT_DISPLAY) display_picks(s, &p);
        else if (d->type == PSP_OPTION_TEXT) text_picks(s, id, &p);
        else if (d->stops) stop_picks(s, id, &p);
        else choice_picks(s, id, &p);
        const char *labels[CHOICES_MAX];
        for (int i = 0; i < p.count; i++) labels[i] = p.label[i];
        int at = p.at;
        if (psp_ui_choice(label, &at, labels, p.count)) {
            if (p.value[at][0]) { snprintf(value, size, "%s", p.value[at]); chosen = 1; }
            else { other.id = id; snprintf(other.text, sizeof other.text, "%s", s->value[id]); }
        }
    } else {
        chosen = number_row(s, id, label, help, value, size);
    }
    psp_ui_help(help);
    psp_ui_disabled_end();
    if (other.id == id) {
        char hint[160];
        if (d->type == PSP_OPTION_SIZE) snprintf(hint, sizeof hint, "WIDTHxHEIGHT, such as 1920x1080");
        else if (d->type == PSP_OPTION_TEXT)
            snprintf(hint, sizeof hint, "Up to %d bytes, with no spaces at either end", PSP_SETTINGS_VALUE - 1);
        else snprintf(hint, sizeof hint, "A number from %g to %g%s%s", d->min, d->max,
                      d->special ? ", or " : "", d->special ? d->special : "");
        const int answer = psp_ui_ask_text(d->label, hint, other.text, sizeof other.text, "Use", "Cancel");
        if (answer >= 0) other.id = -1;
        if (answer > 0 && other.text[0]) { snprintf(value, size, "%s", other.text); chosen = 1; }
    }
    return chosen;
}
