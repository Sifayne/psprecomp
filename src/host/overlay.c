/* The in-game menu (docs/PLAYER-LAYER.md §4).
 *
 * Escape, or View + Menu on the controller, opens it; the same, Resume or
 * Menu alone close it. Open, it holds the game at the next safe point
 * (psprecomp/safepoint.h) and has the controls (src/host/input.c): the mouse
 * is let go, everything held is released and the game sees a neutral pad,
 * and on closing the game gets them back once they are let go.
 *
 * Its pages come from registries, so a title writes no UI code: Bindings
 * from the input's table, one page per page of the title's settings schema,
 * then Performance and Quit. A change applies at once where the host can
 * apply it -- the bindings, the keyboard layout, the active controller, the
 * window mode, the volume (PSP_OPTION_LIVE) -- and otherwise the next time
 * the game starts. On closing, changes are written back to the preset the
 * game was started with, never what the environment set.
 *
 * The C here is the menu; src/host/ui.cpp draws it with Dear ImGui. Both run
 * on the SDL thread only. */
#include "overlay.h"

#include "input.h"
#include "ui.h"

#include "psprecomp/clock.h"
#include "psprecomp/host/present.h"
#include "psprecomp/host/settings.h"
#include "psprecomp/host/title.h"
#include "psprecomp/safepoint.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PAGES_MAX = 12, CAPTURE_MS = 5000 };
enum { PAGE_RESUME, PAGE_BINDINGS, PAGE_SCHEMA };   /* then the schema's, Performance, Quit */

static struct {
    int started, open;
    SDL_Window *win;
    int page;
    psp_settings edit;          /* what the menu shows and changes */
    int dirty;
    char status[PSP_SETTINGS_ERROR + 64];
    /* The schema's pages, in the order their options first appear. */
    const char *pages[PAGES_MAX];
    int page_count;
    /* Press-to-bind: the row and device waiting for a control. It takes one
     * only after everything held is let go, so the press that chose the cell
     * is not the binding. */
    int capturing, capture_row, capture_device, capture_armed;
    Uint64 capture_since;
    unsigned pad_pair;          /* View and Menu held on the pad */
    int mute;
} o;

static const psp_settings_schema *schema(void) { return psp_settings_active_schema(); }

static int option(const char *key) { return psp_settings_find(key); }

static void find_pages(void) {
    o.page_count = 0;
    for (int k = 0; k < schema()->count; k++) {
        const psp_option_def *d = &schema()->options[k];
        if ((d->flags & PSP_OPTION_HIDDEN) || !d->page) continue;
        int seen = 0;
        for (int p = 0; p < o.page_count; p++) seen |= !strcmp(o.pages[p], d->page);
        if (!seen && o.page_count < PAGES_MAX) o.pages[o.page_count++] = d->page;
    }
}

static int page_performance(void) { return PAGE_SCHEMA + o.page_count; }
static int page_quit(void) { return PAGE_SCHEMA + o.page_count + 1; }

/* ---- applying -------------------------------------------------------------------- */

static void apply_live(void) {
    input_rebuild(&o.edit);
    const int volume = option("VOLUME");
    present_set_volume(volume >= 0 ? o.edit.number[volume] / 100.0 : 1.0, o.mute);
    const int mode = option("WINDOW_MODE");
    if (mode >= 0 && (schema()->options[mode].flags & PSP_OPTION_LIVE))
        present_set_fullscreen(o.edit.number[mode] != 0);
}

static void set_option(int k, const char *value) {
    char error[PSP_SETTINGS_ERROR];
    psp_settings next = o.edit;
    if (psp_settings_set(&next, k, value, PSP_SOURCE_PRESET, error) || psp_settings_resolve(&next, error)) {
        snprintf(o.status, sizeof o.status, "%s", error);
        return;
    }
    o.edit = next;
    o.dirty = 1;
    if (schema()->options[k].flags & PSP_OPTION_LIVE) apply_live();
}

static void save(void) {
    if (!o.dirty) return;
    char error[PSP_SETTINGS_ERROR];
    const char *preset = NULL;
    if (!psp_settings_origin(&preset))
        snprintf(o.status, sizeof o.status, "Started without a preferences file: changes last until the game closes.");
    else if (psp_settings_save_origin(&o.edit, error))
        snprintf(o.status, sizeof o.status, "Not saved: %s", error);
    else {
        snprintf(o.status, sizeof o.status, "Saved to the preset \"%s\".", preset);
        o.dirty = 0;
    }
    fprintf(stderr, "present: menu: %s\n", o.status);
}

/* ---- opening and closing --------------------------------------------------------- */

static void open_menu(void) {
    if (o.open) return;
    o.open = 1;
    o.page = PAGE_RESUME;
    o.capturing = 0;
    /* What the pad holds now: the pair that opened it, perhaps. */
    SDL_GameController *pad = input_pad();
    o.pad_pair = pad ? (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_BACK) ? 1u : 0u) |
                       (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START) ? 2u : 0u) : 0u;
    o.status[0] = 0;
    input_take(INPUT_OVERLAY);
    psp_pause_request(1);
    psp_ui_focus_next();
    fprintf(stderr, "present: menu opened\n");
}

static void close_menu(void) {
    if (!o.open) return;
    save();
    o.open = 0;
    o.capturing = 0;
    psp_ui_clear();
    input_return(INPUT_OVERLAY);
    psp_pause_request(0);
    fprintf(stderr, "present: menu closed\n");
}

static void toggle(void) { if (o.open) close_menu(); else open_menu(); }
static int is_open(void) { return o.open; }

/* ---- events ------------------------------------------------------------------------ */

static int held_anything(void) {
    int n;
    const Uint8 *keys = SDL_GetKeyboardState(&n);
    for (int i = 0; i < n; i++) if (keys[i]) return 1;
    if (SDL_GetMouseState(NULL, NULL)) return 1;
    SDL_GameController *pad = input_pad();
    if (pad)
        for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++)
            if (SDL_GameControllerGetButton(pad, (SDL_GameControllerButton)i)) return 1;
    return 0;
}

static void capture_event(const SDL_Event *e) {
    if (e->type == SDL_KEYDOWN && !e->key.repeat && o.capture_armed) {
        if (e->key.keysym.sym == SDLK_ESCAPE) { o.capturing = 0; return; }
        if (e->key.keysym.sym == SDLK_DELETE) {
            input_assign(&o.edit, o.capture_row, o.capture_device, NULL);
            input_rebuild(&o.edit);
            o.dirty = 1;
            o.capturing = 0;
            return;
        }
    }
    char source[64];
    if (!o.capture_armed || !input_spell(e, o.capture_device, source, sizeof source)) return;
    if (input_assign(&o.edit, o.capture_row, o.capture_device, source))
        snprintf(o.status, sizeof o.status, "%s cannot be bound to %s.", source, input_row_label(o.capture_row));
    else {
        input_rebuild(&o.edit);
        o.dirty = 1;
    }
    o.capturing = 0;
}

static void event(const SDL_Event *e) {
    if (!o.open) return;
    if (o.capturing) { capture_event(e); return; }
    if (e->type == SDL_KEYDOWN && !e->key.repeat && e->key.keysym.sym == SDLK_ESCAPE) { close_menu(); return; }
    if ((e->type == SDL_CONTROLLERBUTTONDOWN || e->type == SDL_CONTROLLERBUTTONUP) && e->cbutton.which == input_pad_id()) {
        const unsigned bit = e->cbutton.button == SDL_CONTROLLER_BUTTON_BACK ? 1u :
                             e->cbutton.button == SDL_CONTROLLER_BUTTON_START ? 2u : 0u;
        if (bit) {
            const unsigned before = o.pad_pair;
            if (e->cbutton.state) o.pad_pair |= bit; else o.pad_pair &= ~bit;
            /* View + Menu together, or Menu alone, closes it: on the way
             * down, so the release of the pair that opened it does not. */
            if (e->cbutton.state && ((o.pad_pair == 3 && before != 3) || (bit == 2 && !(before & 1)))) {
                close_menu();
                return;
            }
        }
    }
    psp_ui_event(e);
}

/* ---- pages -------------------------------------------------------------------------- */

/* The index-th '|'-separated field. */
static int field(const char *list, int index, char *out, size_t size) {
    if (!list) return 0;
    while (index-- > 0) { list = strchr(list, '|'); if (!list) return 0; list++; }
    const size_t n = strcspn(list, "|");
    snprintf(out, size, "%.*s", (int)n, list);
    return 1;
}

static void option_row(int k) {
    const psp_option_def *d = &schema()->options[k];
    const int locked = o.edit.source[k] == PSP_SOURCE_ENV;
    const int live = (d->flags & PSP_OPTION_LIVE) != 0;
    char label[160], help[1024];
    snprintf(label, sizeof label, "%s%s", d->label, live ? "" : " *");
    snprintf(help, sizeof help, "%s%s%s", d->help ? d->help : "",
             live ? "" : " Applies the next time the game starts.",
             locked ? " Set by the environment for this run." : "");
    psp_ui_disabled_begin(locked);
    if (d->type == PSP_OPTION_CHOICE) {
        char names[16][PSP_SETTINGS_VALUE];
        const char *labels[16];
        int count = 0;
        while (count < 16 && field(d->labels ? d->labels : d->choices, count, names[count], sizeof names[count])) {
            labels[count] = names[count];
            count++;
        }
        int index = (int)o.edit.number[k];
        if (psp_ui_choice(label, &index, labels, count)) {
            char value[PSP_SETTINGS_VALUE];
            if (field(d->choices, index, value, sizeof value)) set_option(k, value);
        }
    } else if (d->type == PSP_OPTION_SIZE || (d->type == PSP_OPTION_INTEGER && d->max - d->min > 10000)) {
        char value[PSP_SETTINGS_VALUE], line[sizeof label + sizeof value + 2];
        psp_option_label(&o.edit, k, value, sizeof value);
        snprintf(line, sizeof line, "%s: %s", label, value);
        psp_ui_text(line);
    } else {
        /* A number; one with a special word (Auto, Off, the game's own) is
         * first a choice between that and a value of the player's. */
        const double scale = d->scale ? d->scale : 1;
        int custom = !(d->special && o.edit.number[k] < 0);
        if (d->special) {
            char special[64];
            snprintf(special, sizeof special, "%s", d->special_label ? d->special_label : d->special);
            if (!d->special_label && special[0] >= 'a' && special[0] <= 'z') special[0] -= 'a' - 'A';
            const char *labels[2] = { special, "Custom" };
            if (psp_ui_choice(label, &custom, labels, 2)) {
                char value[PSP_SETTINGS_VALUE];
                snprintf(value, sizeof value, "%.9g", d->min > 0 ? d->min : 0);
                set_option(k, custom ? value : d->special);
            }
            psp_ui_help(help);
        }
        if (custom) {
            double v = o.edit.number[k] * scale;
            char bare[PSP_SETTINGS_NAME + 4];
            snprintf(bare, sizeof bare, "##%s", d->key);
            if (psp_ui_number(d->special ? bare : label, &v, d->min * scale, d->max * scale,
                              d->format ? d->format : "%.2f")) {
                double n = v / scale;
                if (d->step > 0) n = d->min + floor((n - d->min) / d->step + 0.5) * d->step;
                if (d->type == PSP_OPTION_INTEGER) n = floor(n + 0.5);
                char value[PSP_SETTINGS_VALUE];
                snprintf(value, sizeof value, "%.9g", n < d->min ? d->min : n > d->max ? d->max : n);
                set_option(k, value);
            }
        }
    }
    psp_ui_help(help);
    psp_ui_disabled_end();
}

static void schema_page(const char *page) {
    psp_ui_heading(page);
    int later = 0;
    for (int k = 0; k < schema()->count; k++) {
        const psp_option_def *d = &schema()->options[k];
        if ((d->flags & PSP_OPTION_HIDDEN) || !d->page || strcmp(d->page, page)) continue;
        option_row(k);
        later |= !(d->flags & PSP_OPTION_LIVE);
        if (!strcmp(d->key, "VOLUME")) {
            if (psp_ui_toggle("Mute", &o.mute)) apply_live();
            psp_ui_help("Silence the game until it is unmuted. Not saved.");
        }
    }
    if (later) psp_ui_note("* applies the next time the game starts");
}

static void bindings_page(void) {
    psp_ui_heading("Bindings");
    static const char *const headers[] = { "Action", "Keyboard", "Controller", "Mouse" };
    if (psp_ui_table_begin("##bindings", 4, headers)) {
        for (int r = 0; r < input_rows(); r++) {
            psp_ui_table_row();
            psp_ui_table_text(input_row_label(r));
            for (int dev = 0; dev < INPUT_DEVICES; dev++) {
                char sources[PSP_BIND_VALUE], id[16];
                input_row_sources(r, dev, sources, sizeof sources);
                snprintf(id, sizeof id, "%d.%d", r, dev);
                const int waiting = o.capturing && o.capture_row == r && o.capture_device == dev;
                if (psp_ui_table_button(id, waiting ? "..." : sources, waiting)) {
                    o.capturing = 1; o.capture_row = r; o.capture_device = dev;
                    o.capture_armed = 0; o.capture_since = SDL_GetTicks64();
                }
                psp_ui_help("Choose to bind: press the control. Escape cancels, Delete unbinds.");
            }
        }
        psp_ui_table_end();
    }
    if (psp_ui_button("Restore the defaults")) {
        char error[PSP_SETTINGS_ERROR];
        while (o.edit.bind_count) psp_settings_bind(&o.edit, o.edit.bind[0].key, NULL, error);
        input_rebuild(&o.edit);
        o.dirty = 1;
    }
    psp_ui_help("Every binding back to the layout's own.");
}

static void performance_page(void) {
    char line[160];
    psp_ui_heading("Performance");
    const double fps = present_fps();
    snprintf(line, sizeof line, "Frames per second: %.0f", fps);
    psp_ui_text(line);
    snprintf(line, sizeof line, "Frame time: %.1f ms", fps > 0 ? 1000.0 / fps : 0.0);
    psp_ui_text(line);
    snprintf(line, sizeof line, "Renderer: %s", present_renderer_name());
    psp_ui_text(line);
    int w = 0, h = 0;
    if (o.win) SDL_GetWindowSize(o.win, &w, &h);
    snprintf(line, sizeof line, "Window: %dx%d", w, h);
    psp_ui_text(line);
    uint64_t guest = 0, wall = 0;
    if (psp_clock_realtime_stats(&guest, &wall)) {
        snprintf(line, sizeof line, "Played: %.0f s, paused: %.0f s", guest / 1e6, psp_clock_held_us() / 1e6);
        psp_ui_text(line);
    }
    if (option("VOLUME") < 0) {
        if (psp_ui_toggle("Mute", &o.mute)) apply_live();
        psp_ui_help("Silence the game until it is unmuted. Not saved.");
    }
}

static void quit_page(void) {
    psp_ui_heading("Quit");
    psp_ui_text("Quit the game? Progress since the game last saved is lost.");
    if (psp_ui_button("Quit the game")) {
        close_menu();
        present_quit();
    }
}

static void frame(void) {
    if (!o.open) return;
    psp_ui_set_pad(input_pad());
    if (o.capturing) {
        if (!o.capture_armed && !held_anything()) o.capture_armed = 1;
        if (SDL_GetTicks64() - o.capture_since >= CAPTURE_MS) o.capturing = 0;
    }
    psp_ui_begin();
    psp_ui_panel_begin();
    char title[160];
    snprintf(title, sizeof title, "%s -- paused", psp_title_info.name ? psp_title_info.name : "Game");
    psp_ui_accent(title);
    psp_ui_side_begin(220);
    const int resume = psp_ui_nav("Resume", o.page == PAGE_RESUME);
    if (psp_ui_nav("Bindings", o.page == PAGE_BINDINGS)) o.page = PAGE_BINDINGS;
    for (int p = 0; p < o.page_count; p++)
        if (psp_ui_nav(o.pages[p], o.page == PAGE_SCHEMA + p)) o.page = PAGE_SCHEMA + p;
    if (psp_ui_nav("Performance", o.page == page_performance())) o.page = page_performance();
    if (psp_ui_nav("Quit", o.page == page_quit())) o.page = page_quit();
    psp_ui_side_next();
    if (o.page == PAGE_RESUME) {
        psp_ui_heading("Paused");
        psp_ui_text("The game waits here. Escape, Menu, or View + Menu on the controller resumes it.");
    } else if (o.page == PAGE_BINDINGS) bindings_page();
    else if (o.page == page_performance()) performance_page();
    else if (o.page == page_quit()) quit_page();
    else schema_page(o.pages[o.page - PAGE_SCHEMA]);
    psp_ui_help_area();
    if (o.status[0]) psp_ui_note(o.status);
    psp_ui_side_end();
    psp_ui_panel_end();
    if (o.capturing) {
        static const char *const how[] = {
            "Press a key. Escape cancels, Delete unbinds.",
            "Press a button, or push a stick or trigger. Escape cancels, Delete unbinds.",
            "Press a mouse button or turn the wheel. Escape cancels, Delete unbinds.",
        };
        char text[256];
        snprintf(text, sizeof text, "%s%s", o.capture_armed ? "" : "Let go of everything first. ",
                 how[o.capture_device]);
        psp_ui_prompt(input_row_label(o.capture_row), text);
    }
    psp_ui_end();
    if (resume) close_menu();
}

/* ---- the presentation layer's side ------------------------------------------------- */

static void start(SDL_Window *win, SDL_Renderer *ren) {
    if (o.started) return;
    if (psp_ui_start(win, ren)) {
        fprintf(stderr, "present: the menu could not start\n");
        return;
    }
    o.started = 1;
    o.win = win;
    o.edit = *psp_settings_current();
    find_pages();
}

static void stop(void) {
    if (!o.started) return;
    if (o.open) close_menu();
    psp_ui_stop();
    o.started = 0;
}

static void draw(SDL_Renderer *ren) { if (o.open) psp_ui_draw(ren); }

static const present_overlay OVERLAY = {
    .start = start, .stop = stop, .toggle = toggle, .is_open = is_open, .event = event,
    .frame = frame, .draw = draw, .gl_lock = psp_ui_gl_lock, .gl_unlock = psp_ui_gl_unlock,
};

void present_use_overlay(void) { present_set_overlay(&OVERLAY); }
