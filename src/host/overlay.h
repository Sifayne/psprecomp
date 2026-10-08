/* The in-game menu (src/host/overlay.c), as the presentation layer sees it.
 * Internal to the host layer. A program has the menu when it calls
 * present_use_overlay (psprecomp/host/present.h); one that only presents --
 * a render check, a test -- leaves it out, and Dear ImGui with it. */
#ifndef PSPRECOMP_HOST_OVERLAY_H
#define PSPRECOMP_HOST_OVERLAY_H

#include "ui.h"

#include <SDL2/SDL.h>

typedef struct {
    /* SDL thread. ren is the software renderer, NULL under GL. */
    void (*start)(SDL_Window *win, SDL_Renderer *ren);
    void (*stop)(void);
    void (*toggle)(void);                   /* the menu action */
    void (*open_page)(const char *page);    /* opens it on a page, by its name */
    int  (*is_open)(void);
    void (*event)(const SDL_Event *e);      /* every event while it is open */
    void (*frame)(void);                    /* once a loop while it is open */
    void (*draw)(SDL_Renderer *ren);        /* software, before presenting */
    /* GL thread, through present_ui_lock below. */
    const psp_ui_frame *(*gl_lock)(const psp_ui_texture_op **ops, int *count);
    void (*gl_unlock)(void);
} present_overlay;

/* present.c */
void present_set_overlay(const present_overlay *overlay);
/* GL thread: the menu's frame to draw over the game and the texture changes
 * it asked for, or NULL while it is shut or the program has none. Unlock
 * after drawing. */
const psp_ui_frame *present_ui_lock(const psp_ui_texture_op **ops, int *op_count);
void present_ui_unlock(void);
/* What the menu asks of the presentation layer, on the SDL thread. */
double present_fps(void);
void present_quit(void);                    /* the run stops at the loop's end */
void present_set_volume(double fraction, int mute);
void present_set_fullscreen(int on);
const char *present_renderer_name(void);

#endif
