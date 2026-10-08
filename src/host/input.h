/* The host's input: one owner, and bindings as data (src/host/input.c,
 * docs/PLAYER-LAYER.md §3). Internal to the host layer; SDL thread only. */
#ifndef PSPRECOMP_HOST_INPUT_INTERNAL_H
#define PSPRECOMP_HOST_INPUT_INTERNAL_H

#include "psprecomp/host/settings.h"

#include <SDL2/SDL.h>

/* Who the controls belong to. The game reads them as the PSP pad; anyone
 * else takes them whole, and the game sees a neutral pad meanwhile. */
enum { INPUT_GAME, INPUT_DIALOG, INPUT_OVERLAY };

/* Host actions a binding can press (bind.key.quit=...). The input layer
 * carries out releasing the mouse itself; input_event returns the rest. */
enum {
    INPUT_ACTION_NONE, INPUT_ACTION_MENU, INPUT_ACTION_QUICK_SAVE, INPUT_ACTION_QUICK_LOAD,
    INPUT_ACTION_SLOT_NEXT, INPUT_ACTION_SLOT_PREV, INPUT_ACTION_SCREENSHOT,
    INPUT_ACTION_RELEASE_MOUSE, INPUT_ACTION_FULLSCREEN, INPUT_ACTION_QUIT,
};
const char *input_action_name(int action);

/* From the settings: the layouts, the title's input and the preset's bind.*
 * keys. Prints the controls, captures the mouse if asked, opens a pad. */
void input_start(const psp_settings *s);
/* Whether this event asks to quit (Ctrl+Shift+Q by default), whoever owns
 * the controls. */
int input_quit_event(const SDL_Event *e);
/* A controller event that an owner other than the game consumed: only the
 * quit chord sees it. */
void input_chord_event(const SDL_Event *e);
/* Every other event: devices come and go, focus, and the controls while the
 * game owns them. Returns a host action the caller carries out, or NONE. */
int input_event(const SDL_Event *e);
/* Once a loop: ownership coming back, wheel presses ending. */
void input_tick(void);

/* Take the controls from the game: the mouse is released, everything held is
 * let go and the game sees a neutral pad. */
void input_take(int owner);
/* Give them back. The game has them again once everything is released, or a
 * second later regardless, so the press that closed a dialog is not a shot. */
void input_return(int owner);
int input_owner(void);

/* The menu's bindings page. Devices are 0 keyboard, 1 controller, 2 mouse. */
enum { INPUT_KEYBOARD, INPUT_CONTROLLER, INPUT_MOUSE, INPUT_DEVICES };
int  input_rows(void);
const char *input_row_label(int row);
/* A row's sources on a device, as the menu shows them: "Z, Space",
 * "D-pad up, LB", or "". */
void input_row_sources(int row, int device, char *out, size_t size);
/* Bind one source to a row on a device in s's bind.* keys, taking it from
 * whatever else it pressed there; NULL unbinds the row on that device. The
 * table changes with input_rebuild. */
int  input_assign(psp_settings *s, int row, int device, const char *source);
/* The control an event presses on a device, spelled as a source: 1, or 0
 * when the event is not one. */
int  input_spell(const SDL_Event *e, int device, char *out, size_t size);
/* What the host can apply at once: the keyboard layout, the active pad and
 * the bindings. */
void input_rebuild(const psp_settings *s);
void input_release_mouse(void);

/* The active controller, or NULL. */
SDL_GameController *input_pad(void);
SDL_JoystickID input_pad_id(void);

#endif
