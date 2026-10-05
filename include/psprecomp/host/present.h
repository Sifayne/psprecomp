/* present — the windowed presentation layer every game's host shares.
 *
 * SDL2 window, real gamepad, audio out. Part of the host layer, not the
 * runtime: the runtime learns only that *someone* may want frames and
 * samples, through the hooks in hle.h, and stays dependency-free. The game
 * it presents is psp_title_info (psprecomp/host/title.h); the options it
 * reads come from the title's settings schema, by key.
 *
 * All of SDL runs on one dedicated thread -- the thread that initialises
 * video owns the event queue -- so the guest threads never touch SDL. They
 * publish converted frames and PCM through small critical sections and get
 * back to the guest immediately. */

#ifndef PSPRECOMP_HOST_PRESENT_H
#define PSPRECOMP_HOST_PRESENT_H

/* Turn the presentation layer on. Enables the real-time clock, registers the
 * display and audio hooks, and spawns the SDL thread. Returns 0 on success,
 * -1 if SDL could not start (the run continues headless either way). */
#ifdef HAVE_SDL2
int present_start(void);
/* One rendered frame reached the window: the backend that drew it says so
 * (render_gl.c on a dirty target; the SDL thread on a published software
 * frame), and the window title shows the rate once a second. */
void present_note_frame(void);
#else
/* No SDL2 when this was built, so present.c was never compiled and there is
 * nothing to link against. The declaration becomes a stub rather than the call
 * site becoming conditional: a host without SDL2 has to build, and asking for a
 * window on one should say why it did not get one instead of failing to link. */
#include <stdio.h>
static inline int present_start(void) {
    fprintf(stderr, "present: built without SDL2 -- no window. Install the "
                    "SDL2 development package and rebuild the host\n");
    return -1;
}
#endif

/* ---- the GL handoff --------------------------------------------------------
 *
 * A GL context belongs to one thread and SDL's is not it. present_want_gl,
 * called before present_start, asks for a GL-capable window and a 3.3 core
 * context; the SDL thread creates both and releases the context so the GE
 * thread can claim it with present_gl_make_current, which blocks until the
 * window exists. See docs/RENDERER.md. */
#ifdef HAVE_SDL2
void  present_want_gl(void);
int   present_gl_make_current(void);
void  present_gl_drawable_size(int *w, int *h);
/* Queue a window resize on the SDL thread; useful for settings and checks. */
void  present_request_window_size(int w, int h);
/* The resolved aspect choice from shared startup settings. The game-camera
 * replacement and final GL blit use this same value. */
int   present_adaptive_aspect(void);
/* The virtual PSP width the window's shape asks for: 480 unless
 * PSPRECOMP_ASPECT=window and the drawable is known, else
 * max(480, round(272 * draw_w / draw_h)). One helper because the camera
 * replacement (guest thread) and the GL backend (GE thread) must agree to the
 * pixel; never below 480, so a narrower window letterboxes instead of
 * shrinking the HUD. */
int   present_aspect_wide_width(void);
void  present_gl_swap(void);
void *present_gl_proc(const char *name);
#else
static inline void  present_want_gl(void) { }
static inline int   present_gl_make_current(void) { return -1; }
static inline void  present_gl_drawable_size(int *w, int *h) {
    if (w) *w = 0;
    if (h) *h = 0;
}
static inline int   present_adaptive_aspect(void) { return 0; }
static inline void  present_request_window_size(int w, int h) { (void)w; (void)h; }
static inline int   present_aspect_wide_width(void) { return 480; }
static inline void  present_gl_swap(void) { }
static inline void  present_note_frame(void) { }
static inline void *present_gl_proc(const char *name) { (void)name; return 0; }
#endif

#endif
