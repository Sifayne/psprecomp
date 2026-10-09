/* The player's UI toolkit: Dear ImGui (third_party/imgui) behind a C API,
 * implemented in src/host/ui.cpp, the host's one C++ file of its own
 * (docs/PLAYER-LAYER.md §4). The in-game menu (overlay.c) draws its pages
 * with it.
 *
 * One ImGui context, used only on the SDL thread: events, frames, widgets
 * and, under the software renderer, drawing. Under GL the frame is copied
 * into a snapshot that the GL thread draws (render_gl.c), together with the
 * texture changes ImGui asked for since it last looked. */
#ifndef PSPRECOMP_HOST_UI_H
#define PSPRECOMP_HOST_UI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the SDL thread -------------------------------------------------------------- */

struct SDL_Window;
struct SDL_Renderer;
union SDL_Event;
struct _SDL_GameController;

/* ren draws the UI under the software renderer; NULL lays it out for the GL
 * thread to draw. A font is looked for as the save dialog looks for one
 * (PSPRECOMP_UI_FONT first); without one ImGui's own is used. */
int  psp_ui_start(struct SDL_Window *win, struct SDL_Renderer *ren);
void psp_ui_stop(void);
/* An event, while the UI has the controls. */
void psp_ui_event(const union SDL_Event *e);
/* The one controller the UI may read: the input owner's active pad. */
void psp_ui_set_pad(struct _SDL_GameController *pad);
/* A frame: begin, widgets, end. End lays it out for whichever renderer. */
void psp_ui_begin(void);
void psp_ui_end(void);
/* Software: draw the frame just ended, over the game, before presenting. */
void psp_ui_draw(struct SDL_Renderer *ren);
/* The UI is shut: the GL thread draws nothing more of it. */
void psp_ui_clear(void);

/* Widgets. Each returns 1 when the player changed or chose it. A label's
 * text ends at "##", which begins what tells it from another. */
void psp_ui_panel_begin(void);          /* centred over the dimmed game */
void psp_ui_panel_end(void);
void psp_ui_side_begin(float width);    /* the list of pages, then the page */
void psp_ui_side_next(void);
void psp_ui_side_end(void);
int  psp_ui_nav(const char *label, int selected);
void psp_ui_heading(const char *text);
void psp_ui_text(const char *text);
void psp_ui_note(const char *text);     /* muted */
void psp_ui_accent(const char *text);   /* in the accent colour */
void psp_ui_separator(void);
void psp_ui_same_line(void);
int  psp_ui_button(const char *label);
int  psp_ui_choice(const char *label, int *index, const char *const *labels, int count);
int  psp_ui_number(const char *label, double *value, double min, double max, const char *format);
int  psp_ui_toggle(const char *label, int *value);
void psp_ui_disabled_begin(int disabled);
void psp_ui_disabled_end(void);
/* Shown in the help area while the last widget has focus or the pointer. */
void psp_ui_help(const char *text);
void psp_ui_help_area(void);
/* The first widget after this has focus when the panel appears. */
void psp_ui_focus_next(void);
/* A table: headers, then rows of cells left to right. A button cell
 * returns 1 when chosen. */
int  psp_ui_table_begin(const char *id, int columns, const char *const *headers);
void psp_ui_table_row(void);
void psp_ui_table_text(const char *text);
int  psp_ui_table_button(const char *id, const char *text, int highlight);
void psp_ui_table_end(void);
/* A box over everything else, for a question that waits on the player. */
void psp_ui_prompt(const char *title, const char *text);
/* The same with an answer: 1 for `yes`, 0 for `no`, -1 while it waits. */
int  psp_ui_confirm(const char *title, const char *text, const char *yes, const char *no);
/* The question withdrawn without an answer, when its asker no longer waits. */
void psp_ui_confirm_close(void);

/* Pictures the menu shows -- a save state's thumbnail -- by slot,
 * 0..PSP_UI_IMAGES-1: RGBA pixels, w x h, given again whenever they change;
 * NULL forgets one. */
enum { PSP_UI_IMAGES = 16 };
void psp_ui_image_set(int slot, const unsigned char *rgba, int w, int h);
/* A row that can be chosen: a picture (-1 for a blank one), a title and a
 * line under it. Returns 1 when chosen. */
int  psp_ui_picture_row(const char *id, int image, const char *title, const char *detail, int highlight);
/* A line at the foot of the screen, menu or not: what a key just did. */
void psp_ui_toast(const char *text);

/* ---- the GL snapshot ---------------------------------------------------------- */

/* ImDrawVert's layout: position, texture coordinates, RGBA8 colour. */
typedef struct { float x, y, u, v; uint32_t rgba; } psp_ui_vertex;
/* One draw: a texture, a clip rectangle in framebuffer pixels (x0, y0, x1,
 * y1 from the top left), and indices into the frame's buffers. */
typedef struct {
    uint32_t texture;
    float clip[4];
    uint32_t first_index, count, first_vertex;
} psp_ui_command;
typedef struct {
    int width, height;              /* the framebuffer it was laid out for */
    float scale_x, scale_y;         /* framebuffer pixels per UI unit */
    const psp_ui_vertex *vertices;
    const uint16_t *indices;
    const psp_ui_command *commands;
    uint32_t vertex_count, index_count, command_count;
} psp_ui_frame;

/* A change ImGui asked for: create a texture (its whole pixels), write a
 * rectangle of one, or destroy one. Pixels are RGBA8, row by row. */
enum { PSP_UI_TEXTURE_CREATE, PSP_UI_TEXTURE_UPDATE, PSP_UI_TEXTURE_DESTROY };
typedef struct {
    int op;
    uint32_t texture;
    int x, y, w, h;                 /* the rectangle; the whole texture on create */
    const unsigned char *pixels;    /* w*h*4 bytes */
} psp_ui_texture_op;

/* GL thread: the frame to draw over the game, or NULL when the UI is shut,
 * and every texture change not yet taken, in order. Between lock and unlock
 * the snapshot stays put; the SDL thread lays out the next one beside it. */
const psp_ui_frame *psp_ui_gl_lock(const psp_ui_texture_op **ops, int *op_count);
void psp_ui_gl_unlock(void);

#ifdef __cplusplus
}
#endif

#endif
