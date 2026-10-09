/* The in-game menu (docs/PLAYER-LAYER.md §4).
 *
 * Escape, or View + Menu on the controller, opens it; the same, Resume or
 * Menu alone close it. F5 saves the game to the current slot and F9 loads
 * it, asking first unless the player chose otherwise; F6 and F7 choose the
 * slot. What a key did shows at the foot of the screen for a moment. Open, it holds the game at the next safe point
 * (psprecomp/safepoint.h) and has the controls (src/host/input.c): the mouse
 * is let go, everything held is released and the game sees a neutral pad,
 * and on closing the game gets them back once they are let go.
 *
 * Its pages come from registries, so a title writes no UI code: Save state
 * and Load state from the state folder (psprecomp/state.h), Bindings from the
 * input's table, one page per page of the settings -- the player's and the
 * pack's, merged by name, drawn by pages.c as the launcher draws them --
 * then Performance and Quit. The "Save states" page is shown under the Load
 * state list rather than as a page of its own. A change applies at once
 * where the host can apply it -- the bindings, the keyboard layout, the
 * active controller, the window mode, the volume (PSP_OPTION_LIVE) -- and
 * otherwise the next time the game starts. On closing, changes are written
 * back to the preferences file the game was started with, never what the
 * environment set.
 *
 * The C here is the menu; src/host/ui.cpp draws it with Dear ImGui. Both run
 * on the SDL thread only. */
#include "overlay.h"

#include "input.h"
#include "pages.h"
#include "ui.h"

#include "psprecomp/clock.h"
#include "psprecomp/hle.h"
#include "psprecomp/host/present.h"
#include "psprecomp/host/settings.h"
#include "psprecomp/host/title.h"
#include "psprecomp/safepoint.h"
#include "psprecomp/state.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#endif

enum { CAPTURE_MS = 5000, TOAST_MS = 2500 };
enum { PAGE_RESUME, PAGE_SAVE, PAGE_LOAD, PAGE_BINDINGS, PAGE_SCHEMA };   /* then the schema's, Performance, Quit */
/* The state files: ten slots, the one written on quitting, and the one a
 * load keeps for undoing it. */
enum { SLOTS = 10, SLOT_QUIT = SLOTS, SLOT_UNDO, SLOT_FILES };
static const char STATES_PAGE[] = "Save states";

static struct {
    int started, open;
    SDL_Window *win;
    int page;
    psp_settings edit;          /* what the menu shows and changes */
    int dirty;
    char status[PSP_SETTINGS_ERROR + 64];
    /* The settings' pages (pages_list), and how their rows are drawn. */
    const char *pages[PAGES_MAX];
    int page_count;
    pages_context rows;
    /* Press-to-bind: the row and device waiting for a control. It takes one
     * only after everything held is let go, so the press that chose the cell
     * is not the binding. */
    int capturing, capture_row, capture_device, capture_armed;
    Uint64 capture_since;
    unsigned pad_pair;          /* View and Menu held on the pad */
    int mute;
    /* Save states: the slot the keys use, what each file holds, the request
     * waited on, a load waiting for its undo save, and a choice waiting for
     * the player to confirm it (slot + 1). */
    int slot;
    int have[SLOT_FILES];
    psp_state_info info[SLOT_FILES];
    unsigned waiting;
    int load_after;
    int confirm, confirm_load;
    char toast[256];
    Uint64 toast_until;
    int shown;                  /* a frame is up: the menu's or a toast's */
} o;

static void find_pages(void) {
    o.page_count = pages_list(0, psp_settings_count(), STATES_PAGE, o.pages, PAGES_MAX);
}

static int page_performance(void) { return PAGE_SCHEMA + o.page_count; }
static int page_quit(void) { return PAGE_SCHEMA + o.page_count + 1; }

/* ---- applying -------------------------------------------------------------------- */

static void apply_live(void) {
    input_rebuild(&o.edit);
    present_set_volume(o.edit.number[PSP_OPT_VOLUME] / 100.0, o.mute);
    present_set_fullscreen(o.edit.number[PSP_OPT_WINDOW_MODE] != 0);
}

static void set_option(int k, const char *value) {
    char error[PSP_SETTINGS_ERROR];
    psp_settings next = o.edit;
    if (psp_settings_set(&next, k, value, PSP_SOURCE_FILE, error) || psp_settings_resolve(&next, error)) {
        snprintf(o.status, sizeof o.status, "%s", error);
        return;
    }
    o.edit = next;
    o.dirty = 1;
    if (psp_settings_option(k)->flags & PSP_OPTION_LIVE) apply_live();
}

static void save(void) {
    if (!o.dirty) return;
    char error[PSP_SETTINGS_ERROR];
    if (!psp_settings_origin())
        snprintf(o.status, sizeof o.status, "Started without a preferences file: changes last until the game closes.");
    else if (psp_settings_save_origin(&o.edit, error))
        snprintf(o.status, sizeof o.status, "Not saved: %s", error);
    else {
        snprintf(o.status, sizeof o.status, "Settings saved.");
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
    o.confirm = 0;
    psp_ui_clear();
    input_return(INPUT_OVERLAY);
    psp_pause_request(0);
    fprintf(stderr, "present: menu closed\n");
}

static void toggle(void) { if (o.open) close_menu(); else open_menu(); }

static void read_slots(void);

static void open_page(const char *page) {
    open_menu();
    if (!page || !*page) return;
    if (!strcmp(page, "Save state")) { o.page = PAGE_SAVE; read_slots(); }
    else if (!strcmp(page, "Load state")) { o.page = PAGE_LOAD; read_slots(); }
    else if (!strcmp(page, "Bindings")) o.page = PAGE_BINDINGS;
    else if (!strcmp(page, "Performance")) o.page = page_performance();
    else if (!strcmp(page, "Quit")) o.page = page_quit();
    for (int p = 0; p < o.page_count; p++) if (!strcmp(o.pages[p], page)) o.page = PAGE_SCHEMA + p;
}
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
    /* Escape closes an open list or answers a question no; otherwise it
     * closes the menu. */
    if (e->type == SDL_KEYDOWN && !e->key.repeat && e->key.keysym.sym == SDLK_ESCAPE &&
        !psp_ui_popup_open() && !o.confirm) { close_menu(); return; }
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

/* ---- save states ------------------------------------------------------------------ */

static void option_row(int k);

/* The player's "Save states" options. */
static int load_mode(void) {             /* 0 ask first, 1 load at once, 2 keep an undo */
    return (int)o.edit.number[PSP_OPT_STATE_LOAD];
}
static int continue_at_start(void) { return o.edit.number[PSP_OPT_STATE_START] != 0; }

static void slot_path(int slot, char *out, size_t size) {
    char name[24];
    if (slot == SLOT_QUIT) snprintf(name, sizeof name, "quit");
    else if (slot == SLOT_UNDO) snprintf(name, sizeof name, "undo");
    else snprintf(name, sizeof name, "slot-%d", (slot + 1) % 100);
    psp_state_file(name, out, size);
}

static const char *slot_name(int slot, char *out, size_t size) {
    if (slot == SLOT_QUIT) return "When you quit";
    if (slot == SLOT_UNDO) return "Before the last load";
    snprintf(out, size, "Slot %d", slot + 1);
    return out;
}

static void slot_detail(int slot, char *out, size_t size) {
    if (!o.have[slot]) { snprintf(out, size, "Empty"); return; }
    if (!o.info[slot].usable) { snprintf(out, size, "From another build of the game, so it cannot be loaded"); return; }
    const time_t t = (time_t)o.info[slot].saved;
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char when[64];
    strftime(when, sizeof when, "%d %b %H:%M", &tm);
    const uint64_t secs = o.info[slot].guest_us / 1000000u;
    snprintf(out, size, "%s, played %u:%02u:%02u", when, (unsigned)(secs / 3600),
             (unsigned)(secs / 60 % 60), (unsigned)(secs % 60));
}

/* What every state file holds, and its thumbnail for the lists. */
static void read_slots(void) {
    static unsigned char thumb[PSP_STATE_THUMB_W * PSP_STATE_THUMB_H * 4];
    for (int i = 0; i < SLOT_FILES; i++) {
        char path[1100];
        slot_path(i, path, sizeof path);
        o.have[i] = !psp_state_peek(path, &o.info[i], thumb);
        psp_ui_image_set(i, o.have[i] && o.info[i].has_thumb ? thumb : NULL, PSP_STATE_THUMB_W, PSP_STATE_THUMB_H);
    }
}

static int make_dir(const char *path) {
    char work[1024];
    const size_t len = strlen(path);
    if (!len || len >= sizeof work) return -1;
    memcpy(work, path, len + 1);
    for (char *p = work + 1; ; p++) {
        if (*p && *p != '/' && *p != '\\') continue;
        const char was = *p;
        *p = 0;
#ifdef _WIN32
        const int rc = _mkdir(work);
#else
        const int rc = mkdir(work, 0755);
#endif
        if (rc && errno != EEXIST) return -1;
        if (!was) return 0;
        *p = was;
    }
}

/* A line at the foot of the screen for a moment, and in the menu's status. */
static void toast(const char *text) {
    snprintf(o.toast, sizeof o.toast, "%.255s", text);
    o.toast_until = SDL_GetTicks64() + TOAST_MS;
    if (o.open) snprintf(o.status, sizeof o.status, "%.255s", text);
    fprintf(stderr, "present: %s\n", text);
}

static int g_waiting_slot, g_waiting_load;

static void request(int kind, int slot) {
    char path[1100], name[32], line[128];
    slot_path(slot, path, sizeof path);
    if (kind == PSP_STATE_SAVE && make_dir(psp_state_dir())) {
        snprintf(line, sizeof line, "Not saved: the folder %.60s cannot be made.", psp_state_dir());
        toast(line);
        return;
    }
    o.waiting = psp_state_request(kind, path);
    g_waiting_slot = slot;
    g_waiting_load = kind == PSP_STATE_LOAD;
    if (kind == PSP_STATE_SAVE && slot == SLOT_UNDO)
        snprintf(line, sizeof line, "Keeping the game as it is, to undo the load...");
    else
        snprintf(line, sizeof line, "%s %s...", kind == PSP_STATE_SAVE ? "Saving to" : "Loading",
                 slot_name(slot, name, sizeof name));
    toast(line);
}

/* A load, once the player has said yes or was not to be asked: with the
 * game as it is kept first when undo is wanted. The game goes on to its
 * next safe point to load, so the menu closes. */
static void load_from(int slot) {
    if (load_mode() == 2 && slot != SLOT_UNDO) {
        o.load_after = slot + 1;
        request(PSP_STATE_SAVE, SLOT_UNDO);
    } else {
        request(PSP_STATE_LOAD, slot);
    }
    close_menu();
}

static void ask_load(int slot) {
    char name[32], line[160];
    slot_name(slot, name, sizeof name);
    if (!o.have[slot] || !o.info[slot].usable) {
        snprintf(line, sizeof line, "%s %s.", name, !o.have[slot] ? "is empty" :
                 "is from another build of the game, so it cannot be loaded");
        toast(line);
        return;
    }
    if (load_mode() != 0) { load_from(slot); return; }
    if (!o.open) open_menu();
    o.page = PAGE_LOAD;
    o.confirm = slot + 1;
    o.confirm_load = 1;
}

static void ask_save(int slot) {
    if (!o.have[slot]) { request(PSP_STATE_SAVE, slot); return; }
    o.confirm = slot + 1;
    o.confirm_load = 0;
}

/* What the keys do: save to the current slot, load it, choose it. */
static void action(int a) {
    if (!o.started) return;
    char name[32], detail[160], line[220];
    switch (a) {
    case INPUT_ACTION_QUICK_SAVE:
        request(PSP_STATE_SAVE, o.slot);
        break;
    case INPUT_ACTION_QUICK_LOAD:
        read_slots();
        ask_load(o.slot);
        break;
    case INPUT_ACTION_SLOT_NEXT:
    case INPUT_ACTION_SLOT_PREV:
        o.slot = (o.slot + (a == INPUT_ACTION_SLOT_NEXT ? 1 : SLOTS - 1)) % SLOTS;
        read_slots();
        slot_detail(o.slot, detail, sizeof detail);
        snprintf(line, sizeof line, "%s: %s", slot_name(o.slot, name, sizeof name), detail);
        toast(line);
        break;
    default:
        break;
    }
}

/* The runtime's answer to the request waited on, once it comes. */
static void poll_states(void) {
    if (!o.waiting) return;
    int ok = 0;
    char message[384], name[32], line[460];
    if (psp_state_result(&ok, message, sizeof message) < o.waiting) return;
    o.waiting = 0;
    if (o.load_after) {
        const int slot = o.load_after - 1;
        o.load_after = 0;
        if (!ok) {
            snprintf(line, sizeof line, "No undo for this load -- %s", message);
            toast(line);
        }
        request(PSP_STATE_LOAD, slot);
        return;
    }
    slot_name(g_waiting_slot, name, sizeof name);
    snprintf(line, sizeof line, "%s: %s", name, message);
    toast(line);
    if (!g_waiting_load) read_slots();
}

/* Quitting, with "continue where I quit" chosen: the game as it is, written
 * at its next safe point -- within a frame when it is running, at once when
 * the menu holds it -- before the window goes. */
static void before_quit(void) {
    if (!o.started || !continue_at_start()) return;
    char path[1100], message[384];
    if (make_dir(psp_state_dir())) {
        fprintf(stderr, "present: on quitting: the folder %s cannot be made\n", psp_state_dir());
        return;
    }
    slot_path(SLOT_QUIT, path, sizeof path);
    const unsigned seq = psp_state_request(PSP_STATE_SAVE, path);
    const Uint64 until = SDL_GetTicks64() + 3000;
    int ok = 0;
    while (psp_state_result(&ok, message, sizeof message) < seq && SDL_GetTicks64() < until) SDL_Delay(5);
    fprintf(stderr, "present: on quitting: %s\n", psp_state_result(&ok, message, sizeof message) >= seq ? message :
            "the game did not reach a point to save at in time; nothing was written");
}

static void save_page(void) {
    char line[200], name[32], detail[160], title[64], id[16];
    psp_ui_heading("Save state");
    snprintf(line, sizeof line, "Quick save writes to the current slot, Slot %d.", o.slot + 1);
    psp_ui_note(line);
    for (int i = 0; i < SLOTS; i++) {
        slot_detail(i, detail, sizeof detail);
        snprintf(title, sizeof title, "%s%s", slot_name(i, name, sizeof name), i == o.slot ? "  (current)" : "");
        snprintf(id, sizeof id, "save%d", i);
        if (psp_ui_picture_row(id, o.have[i] ? i : -1, title, detail, i == o.slot)) {
            o.slot = i;
            ask_save(i);
        }
    }
}

static void load_page(void) {
    char name[32], detail[160], title[64], id[16];
    psp_ui_heading("Load state");
    for (int i = 0; i < SLOT_FILES; i++) {
        if (i >= SLOTS && !o.have[i]) continue;
        slot_detail(i, detail, sizeof detail);
        snprintf(title, sizeof title, "%s%s", slot_name(i, name, sizeof name), i == o.slot ? "  (current)" : "");
        snprintf(id, sizeof id, "load%d", i);
        if (psp_ui_picture_row(id, o.have[i] ? i : -1, title, detail, i == o.slot)) {
            if (i < SLOTS) o.slot = i;
            ask_load(i);
        }
    }
    psp_ui_separator();
    for (int k = 0; k < psp_settings_count(); k++)
        if (pages_on(k, STATES_PAGE)) option_row(k);
}

static void confirm_prompt(void) {
    if (!o.confirm) { psp_ui_confirm_close(); return; }
    const int slot = o.confirm - 1;
    char name[32], text[256];
    slot_name(slot, name, sizeof name);
    if (o.confirm_load)
        snprintf(text, sizeof text, "Load %s? What has happened since it was saved is lost%s.", name,
                 load_mode() == 2 ? ", unless you load \"Before the last load\"" : "");
    else
        snprintf(text, sizeof text, "Replace %s with the game as it is now?", name);
    const int answer = psp_ui_confirm(o.confirm_load ? "Load state" : "Save state", text,
                                      o.confirm_load ? "Load" : "Replace", "Cancel");
    if (answer < 0) return;
    o.confirm = 0;
    if (answer && o.confirm_load) load_from(slot);
    else if (answer) request(PSP_STATE_SAVE, slot);
}

/* ---- pages -------------------------------------------------------------------------- */

static void option_row(int k) {
    char value[PSP_SETTINGS_VALUE];
    if (pages_option(&o.edit, k, &o.rows, value, sizeof value)) set_option(k, value);
}

static void schema_page(const char *page) {
    psp_ui_heading(page);
    int later = 0;
    for (int k = 0; k < psp_settings_count(); k++) {
        if (!pages_on(k, page)) continue;
        option_row(k);
        later |= !(psp_settings_option(k)->flags & PSP_OPTION_LIVE);
        if (k == PSP_OPT_VOLUME) {
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
    if (!o.started) return;
    poll_states();
    const int toast_on = o.toast[0] && SDL_GetTicks64() < o.toast_until;
    if (!o.open) {
        /* Closed, the menu draws only what a key just did, for a moment. */
        if (toast_on) {
            psp_ui_begin();
            psp_ui_toast(o.toast);
            psp_ui_end();
            o.shown = 1;
        } else if (o.shown) {
            psp_ui_clear();
            o.shown = 0;
        }
        return;
    }
    o.shown = 1;
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
    psp_ui_side_begin(220, 0);
    const int resume = psp_ui_nav("Resume", o.page == PAGE_RESUME);
    if (psp_ui_nav("Save state", o.page == PAGE_SAVE) && o.page != PAGE_SAVE) { o.page = PAGE_SAVE; read_slots(); }
    if (psp_ui_nav("Load state", o.page == PAGE_LOAD) && o.page != PAGE_LOAD) { o.page = PAGE_LOAD; read_slots(); }
    if (psp_ui_nav("Bindings", o.page == PAGE_BINDINGS)) o.page = PAGE_BINDINGS;
    for (int p = 0; p < o.page_count; p++)
        if (psp_ui_nav(o.pages[p], o.page == PAGE_SCHEMA + p)) o.page = PAGE_SCHEMA + p;
    if (psp_ui_nav("Performance", o.page == page_performance())) o.page = page_performance();
    if (psp_ui_nav("Quit", o.page == page_quit())) o.page = page_quit();
    psp_ui_side_next();
    if (o.page == PAGE_RESUME) {
        psp_ui_heading("Paused");
        psp_ui_text("The game waits here. Escape, Menu, or View + Menu on the controller resumes it.");
    } else if (o.page == PAGE_SAVE) save_page();
    else if (o.page == PAGE_LOAD) load_page();
    else if (o.page == PAGE_BINDINGS) bindings_page();
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
    confirm_prompt();
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
    o.rows = (pages_context){ .menu = 1, .movie_available = psp_mpeg_decoding_available() };
    find_pages();
    read_slots();
}

static void stop(void) {
    if (!o.started) return;
    if (o.open) close_menu();
    psp_ui_stop();
    o.started = 0;
}

static void draw(SDL_Renderer *ren) { if (o.open || o.shown) psp_ui_draw(ren); }

static const present_overlay OVERLAY = {
    .start = start, .stop = stop, .toggle = toggle, .open_page = open_page, .is_open = is_open, .event = event,
    .frame = frame, .draw = draw, .action = action, .before_quit = before_quit,
    .gl_lock = psp_ui_gl_lock, .gl_unlock = psp_ui_gl_unlock,
};

void present_use_overlay(void) { present_set_overlay(&OVERLAY); }
