/* render_gl — the OpenGL 3.3 core backend, host-side (src/host/render_gl.c).
 *
 * The runtime cannot carry this: it needs a window and a GL context, and the
 * core has no external dependencies on purpose. A game's boot host registers
 * it with psp_render_register() and it becomes selectable as "gl" like any
 * other backend. Returns NULL when the host was built without SDL2, in which
 * case there is nowhere to put a context and asking for "gl" should say so
 * rather than fail to link. What it asks of a title is in psp_title_info
 * (psprecomp/host/title.h).
 *
 * See docs/RENDERER.md for why it lives here and which thread owns the
 * context. */

#ifndef PSPRECOMP_HOST_RENDER_GL_H
#define PSPRECOMP_HOST_RENDER_GL_H

#include "psprecomp/render.h"
#include <stdio.h>

const psp_render_backend *render_gl_backend(void);

/* Read the validated startup settings snapshot: 0 is PSP, 1 is window. */
int render_gl_resolution_mode(void);

/* Read a target's physical RGBA pixels, top-left first, for renderer checks
 * and captures. Call on the GE/context thread after finish(). Caller frees. */
unsigned char *render_gl_capture(uint32_t addr, int *w, int *h);

/* One line for the end-of-run summary: whether it ran, and what it drew. */
void render_gl_report(FILE *out);

#endif
