/* render_gl — an OpenGL 3.3 core backend for the GE.
 *
 * Why this lives in the host and not in the runtime: the core has no external
 * dependencies on purpose and SDL2 is the host's (tools/psprecomp/CMakeLists.txt
 * says so in its header), so a backend needing a window and a GL context
 * belongs on this side. It reaches the interpreter through
 * psp_render_register(), after which ge.c cannot tell which side it came from.
 *
 * Why the context is claimed lazily rather than in init(): a GL context belongs
 * to exactly one thread, and init() is called by boot.c on the main thread
 * while display lists execute on a guest thread. So init() only records the
 * size, and the first entry point that arrives *on the GE thread* claims the
 * context and builds the resources. The GE is driven by exactly one host thread
 * -- measured, findings item 51 -- which is what makes that safe; if a second
 * one ever appears this refuses loudly rather than issuing calls against a
 * context that is not current.
 *
 * The backend translates points, lines, triangles, strips, fans and sprites at
 * PSP resolution or the window's physical resolution, including texture
 * decode/cache, perspective UVs, the full mip chain and measured PSP LOD/filter
 * rules, depth, scissor, blending, alpha
 * test, RGBA8888 alpha-backed stencil and fog. The software path remains the
 * differential oracle. Reduced-bit-depth stencil and blend operations without
 * a fixed GL equivalent stay explicitly counted rather than approximated.
 *
 * It is the player's, shared by every title (docs/PLAYER-LAYER.md, stage 3):
 * Last Raven's backend, then The 3rd Birthday's superset of it, merged. What
 * a title decides for itself it states in psp_title_info
 * (psprecomp/host/title.h): whether its HUD may use the wide bands, how its
 * screen-space draws are placed at a wide aspect, the scene extent, and the
 * glow composite the smooth bloom filter recognises. The settings it reads
 * (RESOLUTION, BLOOM_FILTER) are found by key, and are off where a title's
 * schema has none.
 */

#include "psprecomp/host/present.h"
#include "psprecomp/host/render_gl.h"
#include "psprecomp/host/settings.h"
#include "psprecomp/host/title.h"
#include "psprecomp/render.h"
#include "psprecomp/mem.h"
#include "psprecomp/hle.h"
#include "psprecomp/os.h"
#include "psprecomp/safepoint.h"
#include "psprecomp/sched.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* A startup setting by key: its number, or 0 where the schema has none. */
static int setting_number(const char *key) {
    const int id = psp_settings_find(key);
    return id < 0 ? 0 : (int)psp_settings_current()->number[id];
}

int render_gl_resolution_mode(void) {
    return setting_number("RESOLUTION");
}

#ifdef HAVE_SDL2

#include "psprecomp/host/save_dialog.h"
#include "overlay.h"
#include "psprecomp/savedata.h"
#include <SDL_opengl.h>
#include <pthread.h>

enum { GL_MAX_VERTS = 64 * 1024, GPU_QUERY_RING = 8 };

/* ---- the entry points we need, loaded by hand ------------------------------
 *
 * No glad, no GLEW: SDL_GL_GetProcAddress is already there and SDL's own
 * headers carry the types and enums, so a loader would be a dependency bought
 * for nothing. */
/* The GL 1.1 entry points have no PFN typedefs -- they predate the extension
 * mechanism and are exported straight from the library. Declaring them here
 * and loading them through SDL like everything else keeps -lGL out of the link,
 * which matters for the Windows port: there GL 1.1 comes from opengl32 and
 * anything newer has to come from wglGetProcAddress, so one uniform path is
 * the only one that works on both. */
typedef void (APIENTRY *PFN_glViewport)(GLint, GLint, GLsizei, GLsizei);
typedef void (APIENTRY *PFN_glDrawArrays)(GLenum, GLint, GLsizei);
typedef void (APIENTRY *PFN_glDrawElements)(GLenum, GLsizei, GLenum, const void *);
typedef void (APIENTRY *PFN_glReadPixels)(GLint, GLint, GLsizei, GLsizei,
                                          GLenum, GLenum, void *);
typedef void (APIENTRY *PFN_glGenTextures)(GLsizei, GLuint *);
typedef void (APIENTRY *PFN_glBindTexture)(GLenum, GLuint);
typedef void (APIENTRY *PFN_glTexImage2D)(GLenum, GLint, GLint, GLsizei,
                                          GLsizei, GLint, GLenum, GLenum,
                                          const void *);
typedef void (APIENTRY *PFN_glTexParameteri)(GLenum, GLenum, GLint);
typedef void (APIENTRY *PFN_glDeleteTextures)(GLsizei, const GLuint *);
typedef void (APIENTRY *PFN_glDeleteFramebuffers)(GLsizei, const GLuint *);
typedef void (APIENTRY *PFN_glDeleteRenderbuffers)(GLsizei, const GLuint *);
typedef void (APIENTRY *PFN_glGetIntegerv)(GLenum, GLint *);
typedef void (APIENTRY *PFN_glTexSubImage2D)(GLenum, GLint, GLint, GLint,
                                          GLsizei, GLsizei, GLenum, GLenum, const void *);
typedef GLenum (APIENTRY *PFN_glGetError)(void);
typedef void (APIENTRY *PFN_glEnable)(GLenum);
typedef void (APIENTRY *PFN_glDisable)(GLenum);
typedef void (APIENTRY *PFN_glDepthFunc)(GLenum);
typedef void (APIENTRY *PFN_glDepthMask)(GLboolean);
typedef void (APIENTRY *PFN_glColorMask)(GLboolean, GLboolean, GLboolean, GLboolean);
typedef void (APIENTRY *PFN_glScissor)(GLint, GLint, GLsizei, GLsizei);
typedef void (APIENTRY *PFN_glBlendFunc)(GLenum, GLenum);
typedef void (APIENTRY *PFN_glClear)(GLbitfield);
typedef void (APIENTRY *PFN_glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (APIENTRY *PFN_glClearDepth)(GLdouble);
typedef void (APIENTRY *PFN_glStencilFunc)(GLenum, GLint, GLuint);
typedef void (APIENTRY *PFN_glStencilOp)(GLenum, GLenum, GLenum);
typedef void (APIENTRY *PFN_glStencilMask)(GLuint);
typedef void (APIENTRY *PFN_glClearStencil)(GLint);
typedef void (APIENTRY *PFN_glCopyTexSubImage2D)(GLenum, GLint, GLint, GLint,
                                               GLint, GLint, GLsizei, GLsizei);

#define GL_FUNCS(X) \
    X(PFN_glViewport,                   glViewport) \
    X(PFN_glDrawArrays,                 glDrawArrays) \
    X(PFN_glDrawElements,               glDrawElements) \
    X(PFN_glReadPixels,                 glReadPixels) \
    X(PFN_glGenTextures,                glGenTextures) \
    X(PFN_glBindTexture,                glBindTexture) \
    X(PFN_glTexImage2D,                 glTexImage2D) \
    X(PFN_glTexParameteri,              glTexParameteri) \
    X(PFN_glDeleteTextures,             glDeleteTextures) \
    X(PFN_glDeleteFramebuffers,         glDeleteFramebuffers) \
    X(PFN_glDeleteRenderbuffers,        glDeleteRenderbuffers) \
    X(PFN_glGetIntegerv,                glGetIntegerv) \
    X(PFN_glTexSubImage2D,              glTexSubImage2D) \
    X(PFN_glGetError,                   glGetError) \
    X(PFN_glEnable,                     glEnable) \
    X(PFN_glDisable,                    glDisable) \
    X(PFN_glDepthFunc,                  glDepthFunc) \
    X(PFN_glDepthMask,                  glDepthMask) \
    X(PFN_glColorMask,                  glColorMask) \
    X(PFN_glScissor,                    glScissor) \
    X(PFN_glBlendFunc,                  glBlendFunc) \
    X(PFN_glClear,                      glClear) \
    X(PFN_glClearColor,                 glClearColor) \
    X(PFN_glClearDepth,                 glClearDepth) \
    X(PFN_glStencilFunc,                glStencilFunc) \
    X(PFN_glStencilOp,                  glStencilOp) \
    X(PFN_glStencilMask,                glStencilMask) \
    X(PFN_glClearStencil,               glClearStencil) \
    X(PFN_glCopyTexSubImage2D,           glCopyTexSubImage2D) \
    X(PFNGLBLENDEQUATIONPROC,           glBlendEquation) \
    X(PFNGLBLENDCOLORPROC,              glBlendColor) \
    X(PFNGLCREATESHADERPROC,            glCreateShader) \
    X(PFNGLSHADERSOURCEPROC,            glShaderSource) \
    X(PFNGLCOMPILESHADERPROC,           glCompileShader) \
    X(PFNGLGETSHADERIVPROC,             glGetShaderiv) \
    X(PFNGLGETSHADERINFOLOGPROC,        glGetShaderInfoLog) \
    X(PFNGLDELETESHADERPROC,            glDeleteShader) \
    X(PFNGLCREATEPROGRAMPROC,           glCreateProgram) \
    X(PFNGLATTACHSHADERPROC,            glAttachShader) \
    X(PFNGLLINKPROGRAMPROC,             glLinkProgram) \
    X(PFNGLGETPROGRAMIVPROC,            glGetProgramiv) \
    X(PFNGLGETPROGRAMINFOLOGPROC,       glGetProgramInfoLog) \
    X(PFNGLUSEPROGRAMPROC,              glUseProgram) \
    X(PFNGLGETUNIFORMLOCATIONPROC,      glGetUniformLocation) \
    X(PFNGLUNIFORM2FPROC,               glUniform2f) \
    X(PFNGLUNIFORM1IPROC,               glUniform1i) \
    X(PFNGLUNIFORM1FPROC,               glUniform1f) \
    X(PFNGLUNIFORM3FPROC,               glUniform3f) \
    X(PFNGLUNIFORM4FPROC,               glUniform4f) \
    X(PFNGLGENVERTEXARRAYSPROC,         glGenVertexArrays) \
    X(PFNGLBINDVERTEXARRAYPROC,         glBindVertexArray) \
    X(PFNGLGENBUFFERSPROC,              glGenBuffers) \
    X(PFNGLBINDBUFFERPROC,              glBindBuffer) \
    X(PFNGLBUFFERDATAPROC,              glBufferData) \
    X(PFNGLBUFFERSUBDATAPROC,           glBufferSubData) \
    X(PFNGLVERTEXATTRIBPOINTERPROC,     glVertexAttribPointer) \
    X(PFNGLVERTEXATTRIBIPOINTERPROC,    glVertexAttribIPointer) \
    X(PFNGLUNIFORM1FVPROC,              glUniform1fv) \
    X(PFNGLUNIFORM3FVPROC,              glUniform3fv) \
    X(PFNGLUNIFORM1IVPROC,              glUniform1iv) \
    X(PFNGLUNIFORM3IVPROC,              glUniform3iv) \
    X(PFNGLUNIFORM2IPROC,               glUniform2i) \
    X(PFNGLGETUNIFORMBLOCKINDEXPROC,    glGetUniformBlockIndex) \
    X(PFNGLUNIFORMBLOCKBINDINGPROC,     glUniformBlockBinding) \
    X(PFNGLBINDBUFFERRANGEPROC,         glBindBufferRange) \
    X(PFNGLDRAWELEMENTSBASEVERTEXPROC,  glDrawElementsBaseVertex) \
    X(PFNGLMULTIDRAWELEMENTSBASEVERTEXPROC, glMultiDrawElementsBaseVertex) \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) \
    X(PFNGLGENFRAMEBUFFERSPROC,         glGenFramebuffers) \
    X(PFNGLBINDFRAMEBUFFERPROC,         glBindFramebuffer) \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC,    glFramebufferTexture2D) \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC,  glCheckFramebufferStatus) \
    X(PFNGLBLITFRAMEBUFFERPROC,         glBlitFramebuffer) \
    X(PFNGLGENRENDERBUFFERSPROC,        glGenRenderbuffers) \
    X(PFNGLBINDRENDERBUFFERPROC,        glBindRenderbuffer) \
    X(PFNGLRENDERBUFFERSTORAGEPROC,     glRenderbufferStorage) \
    X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer) \
    X(PFNGLGENQUERIESPROC,               glGenQueries) \
    X(PFNGLBEGINQUERYPROC,               glBeginQuery) \
    X(PFNGLENDQUERYPROC,                 glEndQuery) \
    X(PFNGLGETQUERYOBJECTIVPROC,         glGetQueryObjectiv) \
    X(PFNGLGETQUERYOBJECTUI64VPROC,      glGetQueryObjectui64v)

#define X(type, name) static type p_##name;
GL_FUNCS(X)
#undef X

static int gl_load(void) {
    int missing = 0;
#define X(type, name)                                                        \
    p_##name = (type)present_gl_proc(#name);                                 \
    if (!p_##name) { fprintf(stderr, "gl: missing %s\n", #name); missing++; }
    GL_FUNCS(X)
#undef X
    return missing ? -1 : 0;
}

/* ---- the streaming rings ------------------------------------------------
 *
 * Every vertex, draw index, triangle index, transform block and CPU-path
 * batch the backend hands the GPU goes through one of five rings. With
 * ARB_buffer_storage (GL 4.4, and any Mesa or vendor driver of the last
 * decade) each ring is one persistently and coherently mapped buffer: the
 * CPU writes into memory the GPU reads from, no glBufferSubData, no driver
 * staging copy, no implicit wait on a buffer still in use. What replaces the
 * driver's protection is a fence per presented frame: an allocation that
 * would overwrite a range a pending frame wrote waits on that frame's fence,
 * and a ring that wraps fences what the current frame has written so far
 * before reusing anything. The rings hold several frames, so the wait is a
 * safety net rather than the steady state. Without buffer_storage, or with
 * PSPRECOMP_GL_PERSISTENT=0, the same allocator falls back to the previous
 * behaviour, glBufferSubData with an orphaning glBufferData on wrap. */
static PFNGLBUFFERSTORAGEPROC  p_glBufferStorage;
static PFNGLMAPBUFFERRANGEPROC p_glMapBufferRange;
static PFNGLFENCESYNCPROC      p_glFenceSync;
static PFNGLCLIENTWAITSYNCPROC p_glClientWaitSync;
static PFNGLDELETESYNCPROC     p_glDeleteSync;
enum { RING_VBO, RING_DRAW, RING_EBO, RING_UBO, RING_BATCH, RING_COUNT, RING_FRAMES = 8 };
typedef struct { GLuint buf; GLenum target; size_t size, head; uint8_t *map; int wrapped; } gl_ring;
static struct {
    int persistent, decided;
    gl_ring r[RING_COUNT];
    size_t frame_start[RING_COUNT];       /* where the current frame's writes began */
    struct { GLsync sync; size_t start[RING_COUNT], end[RING_COUNT]; } fq[RING_FRAMES];
    int fq_n;
    unsigned fences, waits;
    double wait_us;                        /* time actually spent in glClientWaitSync */
} g_rings;
static double rings_now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e6 + t.tv_nsec / 1e3; }
static void rings_decide(void) {
    if (g_rings.decided) return;
    g_rings.decided = 1;
    p_glBufferStorage  = (PFNGLBUFFERSTORAGEPROC)present_gl_proc("glBufferStorage");
    p_glMapBufferRange = (PFNGLMAPBUFFERRANGEPROC)present_gl_proc("glMapBufferRange");
    p_glFenceSync      = (PFNGLFENCESYNCPROC)present_gl_proc("glFenceSync");
    p_glClientWaitSync = (PFNGLCLIENTWAITSYNCPROC)present_gl_proc("glClientWaitSync");
    p_glDeleteSync     = (PFNGLDELETESYNCPROC)present_gl_proc("glDeleteSync");
    const char *e = getenv("PSPRECOMP_GL_PERSISTENT");
    g_rings.persistent = p_glBufferStorage && p_glMapBufferRange && p_glFenceSync && p_glClientWaitSync && p_glDeleteSync &&
                         !(e && *e == '0');
}
/* Create ring i on the currently bound target; leaves it bound. */
static void ring_create(int i, GLenum target, size_t size) {
    rings_decide();
    gl_ring *r = &g_rings.r[i];
    r->target = target; r->size = size; r->head = 0; r->map = NULL;
    p_glGenBuffers(1, &r->buf);
    p_glBindBuffer(target, r->buf);
    if (g_rings.persistent) {
        const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
        p_glBufferStorage(target, (GLsizeiptr)size, NULL, flags);
        r->map = (uint8_t *)p_glMapBufferRange(target, 0, (GLsizeiptr)size, flags);
        if (!r->map) { fprintf(stderr, "gl: persistent map of ring %d failed, falling back\n", i); g_rings.persistent = 0; }
    }
    if (!g_rings.persistent) p_glBufferData(target, (GLsizeiptr)size, NULL, GL_STREAM_DRAW);
}
/* Does [a0,a1) overlap a frame's [s,e), which wraps when e < s? */
static int span_overlaps(size_t a0, size_t a1, size_t s, size_t e, size_t size) {
    if (s == e) return 0;
    if (s < e) return a0 < e && s < a1;
    return (a0 < e) || (s < a1) || (a0 < size && a1 > s);
}
/* Fence everything written since the last fence. Called once per presented
 * frame and by a ring about to wrap. */
static void rings_fence(void) {
    if (!g_rings.persistent) return;
    int wrote = 0;
    for (int i = 0; i < RING_COUNT; i++) if (g_rings.r[i].head != g_rings.frame_start[i] || g_rings.r[i].wrapped) wrote = 1;
    if (!wrote) return;
    if (g_rings.fq_n == RING_FRAMES) {
        const double t0 = rings_now_us();
        p_glClientWaitSync(g_rings.fq[0].sync, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
        g_rings.wait_us += rings_now_us() - t0;
        p_glDeleteSync(g_rings.fq[0].sync);
        memmove(&g_rings.fq[0], &g_rings.fq[1], sizeof g_rings.fq[0] * (RING_FRAMES - 1));
        g_rings.fq_n--; g_rings.waits++;
    }
    for (int i = 0; i < RING_COUNT; i++) {
        const gl_ring *r = &g_rings.r[i];
        /* A wrap which reaches/passes the original start covers the entire
         * ring. Equal endpoints otherwise mean empty to span_overlaps(),
         * allowing in-flight vertices to be overwritten on the next wrap. */
        const int full = r->wrapped && r->head >= g_rings.frame_start[i];
        g_rings.fq[g_rings.fq_n].start[i] = full ? 0 : g_rings.frame_start[i];
        g_rings.fq[g_rings.fq_n].end[i] = full ? r->size : r->head;
        g_rings.frame_start[i] = g_rings.r[i].head;
        g_rings.r[i].wrapped = 0;
    }
    g_rings.fq[g_rings.fq_n].sync = p_glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    g_rings.fq_n++; g_rings.fences++;
}
/* Wait until no pending frame still reads [a0,a1) of ring i. Fences are in
 * order, so waiting on frame k retires every older one with it. */
static void ring_wait(int i, size_t a0, size_t a1) {
    int retire = -1;
    for (int k = 0; k < g_rings.fq_n; k++)
        if (span_overlaps(a0, a1, g_rings.fq[k].start[i], g_rings.fq[k].end[i], g_rings.r[i].size)) retire = k;
    if (retire < 0) return;
    const double t0 = rings_now_us();
    p_glClientWaitSync(g_rings.fq[retire].sync, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
    g_rings.wait_us += rings_now_us() - t0;
    for (int k = 0; k <= retire; k++) p_glDeleteSync(g_rings.fq[k].sync);
    memmove(&g_rings.fq[0], &g_rings.fq[retire + 1], sizeof g_rings.fq[0] * (size_t)(g_rings.fq_n - retire - 1));
    g_rings.fq_n -= retire + 1; g_rings.waits++;
}
/* Take `bytes` from ring i at an offset aligned to `align`, with `reserve`
 * bytes guaranteed to lie within the buffer after the offset (a uniform
 * range bound past the blocks actually written). Returns the offset. */
static size_t ring_alloc(int i, size_t bytes, size_t align, size_t reserve) {
    gl_ring *r = &g_rings.r[i];
    if (reserve < bytes) reserve = bytes;
    size_t off = (r->head + align - 1) / align * align;
    if (off + reserve > r->size) {
        if (g_rings.persistent) { rings_fence(); r->wrapped = 1; }
        else { p_glBindBuffer(r->target, r->buf); p_glBufferData(r->target, (GLsizeiptr)r->size, NULL, GL_STREAM_DRAW); }
        off = 0;
    }
    if (g_rings.persistent) ring_wait(i, off, off + reserve);
    r->head = off + bytes;
    return off;
}
static void ring_write(int i, size_t off, const void *data, size_t bytes) {
    gl_ring *r = &g_rings.r[i];
    if (r->map) memcpy(r->map + off, data, bytes);
    else { p_glBindBuffer(r->target, r->buf); p_glBufferSubData(r->target, (GLintptr)off, (GLsizeiptr)bytes, data); }
}

/* ---- the texture cache ------------------------------------------------------
 *
 * Keyed on everything that changes the decoded texels: where they live, how
 * they are laid out, and -- for the CLUT formats -- the palette and the paging
 * applied to the index. Two bindings with the same key decode identically, so
 * one upload serves both.
 *
 * The key is paired with the guest-memory generation of every byte range the
 * decoder can read. CPU stores, GE copies and render-target readbacks advance
 * those generations, so an in-place update reuses the GL texture object but
 * uploads fresh texels. A global serial makes the common no-write case O(1);
 * page ranges are scanned only after guest memory changed somewhere.
 */
enum {
    TEXCACHE_MAX = 512,
    TEXCACHE_PROBES = 32,
    TEXEL_CAP = 512 * 512,
    RT_MAX = 8
};

/* One framebuffer object per render target.
 *
 * A single shared FBO was wrong in a way that took a while to see. Last Raven
 * double-buffers and then composites: it draws the room into one display
 * buffer, and in a later frame draws a handful of full-screen passes that read
 * that buffer back. With one FBO those two live in the same pixels, so the
 * compositing frame painted over the room and the hangar came back as walls
 * with no floor. The census in findings item 50 said three targets are drawn
 * into; this gives each its own colour and depth.
 *
 * A texture bound at a target's address samples that target's colour texture
 * rather than being uploaded from guest memory, which is what makes the
 * read-back-and-composite pattern work at all. */
enum { MODEL_MAX_VERTS = 256 };   /* ge.c's GE_VERTEX_BATCH */
typedef struct {
    GLint viewport, placement, ybias, atest, aref, amask, preblend_src;
    GLint texenable, texfunc, tcc, dbl, env, tex;
    GLint minfilter, magfilter, wraps, wrapt, miptop;
    GLint fogenable, fogcolour;
    GLint texscale, bloom;
} uniforms;
/* The std140 block both model shaders read (Xform), laid out by hand: every
 * member is a vec4 / ivec4 / mat4 so the C offsets are the GLSL ones. */
typedef struct {
    float   world[16], view[16], proj[16], tgen[16];
    int32_t lmeta[16];
    float   lpos[16], ldir[16], latten[16], lexpcut[16], lamb[16], ldif[16], lspec[16];
    float   memis[4], mamb[4], mdif[4], mspec[4], gamb[4];
    float   coef_fog[4];
    int32_t flags[4];
    int32_t texmap_vp[4];
    float   texsize_off[4];
    float   vps[4], vpc[4];
    int32_t cull_strip[4];
    int32_t lodi[4];
    float   guard_slope[4];
} xform_block;
enum { XFORM_MAXB = 16, XFORM_RING_SLOTS = 256, MODEL_RING_VERTS = 65536 };
static void flush_model(void);
typedef struct {
    GLuint   ubo; GLint ubo_align; int ubo_slot, ubo_slot_bytes;
    GLuint   vbo_draw, ebo_ring;       /* per-vertex draw index; the static index patterns */
    int      vbo_head, ebo_head;       /* ring positions, in vertices and in indices */
    uint64_t state_gen, applied_gen;   /* set_* calls bump state_gen; the model path re-applies when behind */
    int      applied_rt;
    /* Consecutive model draws under one GL state, drawn as one multi-draw. */
    xform_block pend_xb[XFORM_MAXB];
    int      pend_count[XFORM_MAXB], pend_base[XFORM_MAXB], pend_prim[XFORM_MAXB];
    int      npend, pend_verts;
    uint64_t batches;
    double   t_append, t_state, t_ubo, t_ebo, t_draw, t_readback;   /* PSPRECOMP_GL_PROFILE */
    uint64_t readbacks;
    int      profile;
    /* Setters called with the state they already set are skipped, so a
     * batch survives the GE pushing the same pixel state before every
     * primitive. The texture setter may only be skipped while guest memory
     * has not been written since the bind: the cache validates against that
     * serial and would otherwise owe a re-upload the pending batch cannot see.
     * A bind of a render-target view is never skipped: the bind is what
     * flushes the draws pending into that target and blits its current
     * pixels into the view, and the primitive after it reads them. */
    uint64_t bound_serial, bound_clut_gen, clut_gen, setters_skipped;
    int      tex_valid, bound_is_view;
    /* The last draw's transform state and the block built from it. A draw
     * whose scene part (view, projection, lights, material, fog, viewport)
     * matches copies the block and rewrites only what is its own: the world
     * and texture matrices, the texture size and the per-draw flags. */
    psp_xform_state last_xs; xform_block last_xb; int last_valid;
    uint64_t scene_hits, scene_misses;
    uint64_t flush_by[16];   /* what ended each model batch, by cause */
    /* Set by every pass that borrows GL state between two flushes (the stencil
     * and alpha passes, a CPU import, a readback, a reallocation); the next
     * model flush then re-applies everything even if nothing tracked changed. */
    int      disturbed;
} model_uniforms;
typedef struct {
    int      used, dirty, configured;
    uint32_t addr, stride;
    int      fmt, w, h;
    /* Guest memory and coordinates retain their PSP extent. w/h describe GPU
     * storage, including padding. The scene fills the expanded picture; HUD
     * uses uniform ui_scale and centered offsets on either axis. */
    int      guest_w, guest_h, wide, wide_w, wide_h, display;
    int      visible_w, visible_h;
    double   sx, sy, ui_scale;
    uint8_t *cpu_dirty;
    int      cpu_pending, scene_drawn;
    GLuint   fbo, colour, depth;
    int      stencil_valid, alpha_dirty;
} rendertarget;

/* Where a batch is placed on a widened target. SCENE spans the whole target;
 * HUD and inset 3D PREVIEW geometry use the centered menu area. */
enum { CLASS_SCENE = 0, CLASS_HUD = 1, CLASS_PREVIEW = 2 };

typedef struct {
    int      used;
    uint32_t addr, stride, clut_addr;
    int      w, h, fmt, swizzled, max_level, uploaded_top;
    uint32_t lv_addr[8], lv_stride[8];
    int      lv_w[8], lv_h[8];
    int      clut_fmt, clut_shift, clut_mask, clut_start;
    uint64_t content_generation, validated_serial;
    uint64_t last_used;
    GLuint   tex;
} texcache_entry;

/* ---- state ------------------------------------------------------------------
 *
 * Everything the interpreter sets is recorded. Most of it is not acted on yet;
 * see the scope note at the top. It is kept rather than dropped so that the
 * increment that implements a rule has the value already arriving. */
static struct {
    int      w, h;
    int      ready, failed;
    int      adaptive_aspect, resolution, smooth_bloom;
    int      pixel_w, pixel_h, max_size;
    uint64_t resizes, cpu_uploads, rt_views;
    int      exporting;
    /* The host thread holding the context (0: none, since the one that held
     * it let go), its guest thread, and how often it has moved. */
    unsigned long thread;
    uint32_t uid;
    uint64_t moves;

    GLuint   prog, vao, vbo;
    GLuint   stencil_prog, stencil_copy;
    GLint    s_mode, s_bit, s_value;
    int      stencil_copy_w, stencil_copy_h;
    /* The fragment stage's uniforms, one set per program: the batched
     * screen-space program and the model program share the fragment shader
     * and its state, but not the locations. `u` is the set for `cur_prog`. */
    uniforms um, ug, *u;
    GLuint   cur_prog;
    /* The model program: the transform on the GPU. */
    GLuint   prog_model, vao_model, vbo_model, ebo_strip, ebo_fan;
    int      model_unavailable;
    uint64_t model_draws, model_verts;
    model_uniforms mu;
    float    tex_sx, tex_sy;
    GLuint   view_fbo, view_tex, copy_fbo;
    int      view_w, view_h;

    uint32_t target_addr, target_stride;
    int      target_fmt;
    /* What sceDisplaySetFrameBuf last named: the buffer the window shows. */
    uint32_t disp_addr, disp_stride;
    int      disp_fmt;
    uint64_t display_targets;   /* targets made for a displayed buffer the GE never drew into */

    /* The state the interpreter last set. Applied at flush time rather than
     * as it arrives, because a state change mid-batch would otherwise apply
     * retroactively to geometry already in the buffer -- so every setter
     * flushes what is pending first. */
    int      sc_x0, sc_y0, sc_x1, sc_y1, sc_valid;
    int      z_test, z_func, z_write;
    int      fog_enable;
    uint32_t fog_colour;
    psp_blend_state bs;
    uint64_t unsupported_blend_eq, unsupported_blend_factor;
    /* The first distinct blends counted above, for the report. */
    struct { int src, dst, eq; uint32_t fixa, fixb; uint64_t draws; } blend_miss[8];
    int blend_misses;
    uint64_t unsupported_stencil_draws;
    uint64_t partial_pixel_masks;          /* PMSK bytes other than 00 and FF */
    uint64_t rt_shrinks;                   /* targets cut back to a neighbour */
    uint64_t stencil_draws, stencil_imports, stencil_exports;

    /* Texture state as the interpreter last set it, plus what is bound. */
    psp_tex_state tex;
    int      tex_enable;
    uint32_t clut_addr;
    int      clut_fmt, clut_shift, clut_mask, clut_start;
    GLuint   bound;
    int      bound_top;

    rendertarget rts[RT_MAX];
    int      n_rts, cur_rt;
    uint64_t rt_overflow;

    /* Adaptive aspect (PSPRECOMP_ASPECT=window). Virtual PSP width/height
     * are latched per frame; g.w/g.h means identity. The staging target
     * downsamples expanded attachments to guest dimensions for readback. */
    int      wide_w, wide_h, batch_class, batch_band, batch_glyph, batch_bloom;
    float    viewport_x, viewport_y, viewport_w, viewport_h;
    uint64_t glyph_draws;
    uint64_t bloom_draws;
    GLuint   stage_fbo, stage_tex;
    int      stage_w, stage_h;
    uint64_t class_scene, class_hud, class_preview, hud_flushes, wide_allocs, wide_retired;
    uint64_t hud_depth_tests, hud_hazard_stencil, hud_hazard_dst_alpha;
    uint64_t hud_depth_func[8], hud_depth_writes;
    /* PSPRECOMP_ASPECT_LOG: what the HUD class holds, per texture. */
    struct { uint32_t addr; int w, h; uint64_t batches, tested; float z0, z1, x0, x1; } hud_by_tex[48];
    int      hud_by_tex_n;

    texcache_entry cache[TEXCACHE_MAX];
    int      cache_entries;
    uint64_t cache_clock;
    uint64_t tex_requests, tex_uploads, tex_hits, tex_fast_hits, tex_revalidated, tex_row_refreshes, tex_rows_refreshed;
    uint64_t tex_invalidations, tex_misses, tex_evictions, tex_too_big;
    uint64_t tex_vram_uploads, tex_upload_pixels;
    uint64_t tex_padded_uploads, tex_padded_pixels;
    uint64_t tex_from_rt, tex_alias_from_rt;
    uint64_t mip_chains, mip_levels, mip_incomplete;
    uint64_t tex_bind_ns, tex_generation_ns, tex_decode_ns, tex_upload_ns;

    float   *batch;          /* position, colour, UV, fog and homogeneous terms */
    size_t   batch_n;

    uint64_t draws, verts, unsupported_prims, readbacks, batch_overflows;
    uint64_t readback_ns;
    uint64_t presents, frames, frame_first_ns, frame_last_ns, frame_prev_ns;
    uint64_t frame_max_ns;
    uint64_t frame_ms[256];
    GLuint   gpu_query[GPU_QUERY_RING];
    uint8_t  gpu_query_pending[GPU_QUERY_RING];
    int      gpu_query_active, gpu_query_suppressed;
    unsigned gpu_query_next;
    uint64_t gpu_samples, gpu_total_ns, gpu_max_ns, gpu_dropped;
    uint64_t gpu_tenth_ms[256];
} g = { .gpu_query_active = -1 };

enum { FLOATS_PER_VERT = 13 };  /* x,y,z, r,g,b,a, u,v, fog, 1/w, texture q, lod16 */
static unsigned shot_counters[2]; /* window images: game presents, dialog redraws, numbered apart */

static void flush(void);
/* Why a pending model batch is about to end. The multi-draw batches only as
 * far as the state between two draws allows; these say which setter it was. */
enum { FB_TARGET, FB_SCISSOR, FB_TEXTURE, FB_CLUT, FB_DEPTH, FB_BLEND, FB_FOG, FB_GLYPH, FB_CLASS, FB_CPU, FB_FINISH, FB_PRESENT, FB_FULL, FB_COUNT };
static void flush_ends(int k) { if (g.mu.npend) g.mu.flush_by[k]++; }
static int  claim(void);
enum { RB_SLOTS = 3, RB_SLOT_BYTES = 1024 * 1024 };
static struct {
    int async; GLuint pbo; uint8_t *map; int next;
    struct { GLsync sync; int rt, rw, rh; size_t off; int valid; } pend[RB_SLOTS];
    unsigned issued, completed, demanded; double wait_us;
} g_rb;
static void readback_watch(void);
static void readback_rt(int i);
static void readback_complete(int i);
static void stencil_to_alpha(rendertarget *r);
static void rt_import(rendertarget *r);

static unsigned long this_thread(void) {
    return (unsigned long)pthread_self();
}

/* ---- asynchronous GPU frame timing --------------------------------------
 *
 * Wall cadence answers whether the game is paced; it does not answer how
 * much of the frame budget the GPU used. TIME_ELAPSED queries bracket the
 * native-resolution draws and final blit. Eight objects are rotated so a
 * result is only read after the driver says it is ready; the measurement must
 * never introduce the stall it is trying to measure. A readback later in the
 * same present normally makes the just-ended result available anyway. */
static void gpu_query_record(uint64_t ns) {
    g.gpu_samples++;
    g.gpu_total_ns += ns;
    if (ns > g.gpu_max_ns) g.gpu_max_ns = ns;
    uint64_t bin = ns / UINT64_C(100000);       /* tenths of a millisecond */
    if (bin > 255) bin = 255;
    g.gpu_tenth_ms[bin]++;
}

static void gpu_query_poll(void) {
    for (int i = 0; i < GPU_QUERY_RING; i++) {
        if (!g.gpu_query_pending[i]) continue;
        GLint available = 0;
        p_glGetQueryObjectiv(g.gpu_query[i], GL_QUERY_RESULT_AVAILABLE,
                             &available);
        if (!available) continue;
        GLuint64 ns = 0;
        p_glGetQueryObjectui64v(g.gpu_query[i], GL_QUERY_RESULT, &ns);
        g.gpu_query_pending[i] = 0;
        gpu_query_record((uint64_t)ns);
    }
}

static void gpu_query_begin_frame(void) {
    if (g.gpu_query_active >= 0 || g.gpu_query_suppressed) return;
    gpu_query_poll();
    for (int n = 0; n < GPU_QUERY_RING; n++) {
        const int i = (int)((g.gpu_query_next + (unsigned)n) % GPU_QUERY_RING);
        if (g.gpu_query_pending[i]) continue;
        p_glBeginQuery(GL_TIME_ELAPSED, g.gpu_query[i]);
        g.gpu_query_active = i;
        g.gpu_query_next = ((unsigned)i + 1u) % GPU_QUERY_RING;
        return;
    }
    g.gpu_query_suppressed = 1;
    g.gpu_dropped++;
}

static void gpu_query_end_frame(void) {
    if (g.gpu_query_active >= 0) {
        p_glEndQuery(GL_TIME_ELAPSED);
        g.gpu_query_pending[g.gpu_query_active] = 1;
        g.gpu_query_active = -1;
    }
    g.gpu_query_suppressed = 0;
}

/* ---- shaders ---------------------------------------------------------------
 *
 * Positions arrive in PSP screen pixels -- psp_vertex carries 12.4 fixed point,
 * converted on the way in -- so the vertex shader's whole job is the viewport
 * transform. Y is flipped because the PSP's origin is top-left and GL's is
 * bottom-left; getting that wrong renders a correct frame upside down, which
 * looks like a transform bug and is not one. */
static const char *VS_SRC =
    "#version 330 core\n"
    "layout(location=0) in vec3 a_pos;\n"
    "layout(location=1) in vec4 a_col;\n"
    "layout(location=2) in vec2 a_uv;\n"
    "layout(location=3) in float a_fog;\n"
    "layout(location=4) in float a_inv_w;\n"
    "layout(location=5) in float a_tex_q;\n"
    "layout(location=6) in float a_lod16;\n"
    "uniform vec2 u_viewport;\n"
    "uniform vec4 u_placement; uniform float u_ybias;\n"
    "out vec4 v_col;\n"
    /* gl_Position deliberately remains post-divide screen space with w=1 so
     * GL cannot alter the PSP coverage or depth already resolved by the GE.
     * Carry UV/W and Q/W as explicitly non-perspective varyings and perform
     * only the texture divide in the fragment shader. */
    "noperspective out vec3 v_uvq;\n"
    "out float v_fog;\n"
    "flat out int v_lod16;\n"
    "void main() {\n"
    "    vec2 p = a_pos.xy * u_placement.xy + u_placement.zw;\n"
    "    vec2 ndc = vec2( (p.x / u_viewport.x) * 2.0 - 1.0,\n"
    /* The PSP owns a pixel on its top edge; GL's lower-left half-open rule
     * owns the opposite horizontal edge after the Y flip. Move geometry by
     * one GL subpixel, sixteen times smaller than the PSP's 1/16-pixel vertex
     * grid, so exact horizontal ties land on the PSP-owned side. */
    "                     1.0 - ((p.y - u_ybias) / u_viewport.y) * 2.0 );\n"
    /* Window depth arrives on the PSP's 0..65535 scale, already divided by
     * w by the interpreter. GL wants clip space, and with w = 1 the
     * perspective divide is the identity, so mapping to -1..1 here puts it
     * on GL's depth range without a second projection. */
    "    gl_Position = vec4(ndc, a_pos.z * 2.0 - 1.0, 1.0);\n"
    "    v_col = a_col;\n"
    "    v_uvq = vec3(a_uv * a_inv_w, a_tex_q * a_inv_w);\n"
    "    v_fog = a_fog;\n"
    "    v_lod16 = int(a_lod16);\n"
    "}\n";

/* GL 3.3 core removed the fixed-function alpha test, so it is a discard. The
 * comparison codes are the GE's own, shared with the depth test. */
/* The five texture functions are the GE's own codes, and their arithmetic is
 * what gpu/texfunc pinned for the software path: MODULATE multiplies, DECAL
 * interpolates by the texture's alpha when the alpha channel takes part and
 * replaces when it does not, BLEND mixes toward the environment colour, REPLACE
 * takes the texel, ADD sums colour and keeps the vertex alpha. `u_tcc` says
 * whether the texture's alpha participates at all; `u_double` is the doubling
 * bit, applied after the function and clamped. */
/* ---- the model program: the transform on the GPU ----------------------------
 *
 * ge.c hands draw_model model-space vertices and the state its own pipeline
 * would have applied (psp_xform_state). The vertex stage is
 * draw_prim_transformed's per-vertex work -- the three products, fog,
 * light_vertex, texgen -- and the geometry stage is emit_tri: the near test or
 * clip, the guard band, the cull by the sign of the snapped-integer area, and
 * the per-triangle texture LOD the batched path computes in push_triangle.
 * Both write exactly what push() would have put in the batch, so the
 * fragment shader is the same one, compiled for 4.0. Strips and fans arrive
 * as plain triangle lists through static index tables, so the geometry stage
 * sees the PSP's vertex order and takes the strip's winding flip from the
 * primitive's parity. `precise` and the term-by-term products keep the
 * float arithmetic in C's order; the 4.0 requirement is the double the cull
 * needs for the exact 64-bit area ge.c computes in long. A context below 4.0
 * leaves the program unbuilt and model_ok() answers 0, so the CPU path stands. */
static const char *VS_MODEL_SRC =
    "#version 400 core\n"
    "layout(location=0) in vec3 a_pos;\n"
    "layout(location=1) in vec3 a_nrm;\n"
    "layout(location=2) in uint a_rgba;\n"
    "layout(location=3) in vec2 a_uv;\n"
    "layout(location=4) in uint a_draw;\n"
    "struct XformBlock {\n"
    "    mat4  world; mat4 view; mat4 proj; mat4 tgen;\n"
    "    ivec4 lmeta[4];\n"
    "    vec4  lpos[4]; vec4 ldir[4]; vec4 latten[4]; vec4 lexpcut[4]; vec4 lamb[4]; vec4 ldif[4]; vec4 lspec[4];\n"
    "    vec4  memis; vec4 mamb; vec4 mdif; vec4 mspec; vec4 gamb;\n"
    "    vec4  coef_fog;\n"
    "    ivec4 flags;\n"
    "    ivec4 texmap_vp;\n"
    "    vec4  texsize_off;\n"
    "    vec4  vps; vec4 vpc;\n"
    "    ivec4 cull_strip;\n"
    "    ivec4 lodi;\n"
    "    vec4  guard_slope;\n"
    "};\n"
    "layout(std140) uniform Xform { XformBlock xb[16]; };\n"
    "#define X xb[int(a_draw)]\n"
    "out VData { vec4 clip; vec4 col; vec2 uv; float texq; float fog; flat int draw; } o;\n"
    "/* The GE's 4x3 and 4x4 products as ge.c writes them, term by term; a 4x3 sits\n"
    " * in a mat4 as three columns of three and the translation in the fourth.\n"
    " * precise keeps the compiler from fusing or reordering, so the sums round as\n"
    " * C does. */\n"
    "vec3 mul43(mat4 m, vec3 p) {\n"
    "    precise float x = m[0].x*p.x + m[1].x*p.y + m[2].x*p.z + m[3].x;\n"
    "    precise float y = m[0].y*p.x + m[1].y*p.y + m[2].y*p.z + m[3].y;\n"
    "    precise float z = m[0].z*p.x + m[1].z*p.y + m[2].z*p.z + m[3].z;\n"
    "    return vec3(x, y, z);\n"
    "}\n"
    "vec3 mul33(mat4 m, vec3 p) {\n"
    "    precise float x = m[0].x*p.x + m[1].x*p.y + m[2].x*p.z;\n"
    "    precise float y = m[0].y*p.x + m[1].y*p.y + m[2].y*p.z;\n"
    "    precise float z = m[0].z*p.x + m[1].z*p.y + m[2].z*p.z;\n"
    "    return vec3(x, y, z);\n"
    "}\n"
    "vec4 mul44(mat4 m, vec3 p) {\n"
    "    precise float x = m[0].x*p.x + m[1].x*p.y + m[2].x*p.z + m[3].x;\n"
    "    precise float y = m[0].y*p.x + m[1].y*p.y + m[2].y*p.z + m[3].y;\n"
    "    precise float z = m[0].z*p.x + m[1].z*p.y + m[2].z*p.z + m[3].z;\n"
    "    precise float w = m[0].w*p.x + m[1].w*p.y + m[2].w*p.z + m[3].w;\n"
    "    return vec4(x, y, z, w);\n"
    "}\n"
    "float len3(vec3 a) { precise float s = a.x*a.x + a.y*a.y + a.z*a.z; return sqrt(s); }\n"
    "float dot3(vec3 a, vec3 b) { precise float s = a.x*b.x + a.y*b.y + a.z*b.z; return s; }\n"
    "/* light_vertex, ge.c. */\n"
    "uint light_colour(vec3 wp, vec3 wn, uint rgba) {\n"
    "    vec3 vc = vec3(float(rgba & 0xFFu) / 255.0, float((rgba >> 8) & 0xFFu) / 255.0, float((rgba >> 16) & 0xFFu) / 255.0);\n"
    "    int matupdate = X.flags.y;\n"
    "    vec3 m_amb = (matupdate & 1) != 0 ? vc : X.mamb.xyz;\n"
    "    vec3 m_dif = (matupdate & 2) != 0 ? vc : X.mdif.xyz;\n"
    "    vec3 m_spc = (matupdate & 4) != 0 ? vc : X.mspec.xyz;\n"
    "    precise vec3 outc = X.memis.xyz + X.gamb.xyz * m_amb;\n"
    "    vec3 n = wn;\n"
    "    bool any = X.lmeta[0].x != 0 || X.lmeta[1].x != 0 || X.lmeta[2].x != 0 || X.lmeta[3].x != 0;\n"
    "    if (any) {\n"
    "        float nlen = len3(n);\n"
    "        if (nlen > 1e-20) n = vec3(n.x / nlen, n.y / nlen, n.z / nlen);\n"
    "        for (int i = 0; i < 4; i++) {\n"
    "            if (X.lmeta[i].x == 0) continue;\n"
    "            vec3 L; float att = 1.0;\n"
    "            if (X.lmeta[i].y == 0) {\n"
    "                L = X.lpos[i].xyz;\n"
    "            } else {\n"
    "                L = vec3(X.lpos[i].x - wp.x, X.lpos[i].y - wp.y, X.lpos[i].z - wp.z);\n"
    "                float d = len3(L);\n"
    "                precise float a = X.latten[i].x + X.latten[i].y * d + X.latten[i].z * d * d;\n"
    "                att = (a != 0.0) ? 1.0 / a : 1.0;\n"
    "            }\n"
    "            float llen = len3(L);\n"
    "            if (llen > 1e-20) L = vec3(L.x / llen, L.y / llen, L.z / llen);\n"
    "            if (X.lmeta[i].y == 2) {\n"
    "                vec3 D = X.ldir[i].xyz;\n"
    "                float dlen = len3(D);\n"
    "                if (dlen > 1e-20) D = vec3(D.x / dlen, D.y / dlen, D.z / dlen);\n"
    "                float sdot = dot3(L, D);\n"
    "                if (!(sdot >= X.lexpcut[i].y)) att = 0.0;\n"
    "                else att *= pow(sdot, X.lexpcut[i].x);\n"
    "            }\n"
    "            if (att == 0.0) continue;\n"
    "            float ndl = dot3(n, L);\n"
    "            float dfac = ndl > 0.0 ? ndl : 0.0;\n"
    "            if (X.lmeta[i].z == 2 && dfac > 0.0) dfac = pow(dfac, X.coef_fog.x);\n"
    "            float sfac = 0.0;\n"
    "            if (X.lmeta[i].z == 1 && ndl >= 0.0) {\n"
    "                vec3 H = vec3(L.x, L.y, L.z + 1.0);\n"
    "                float hlen = len3(H);\n"
    "                if (hlen > 1e-20) H = vec3(H.x / hlen, H.y / hlen, H.z / hlen);\n"
    "                float ndh = dot3(n, H);\n"
    "                sfac = (ndh > 0.0) ? pow(ndh, X.coef_fog.x) : 0.0;\n"
    "            }\n"
    "            for (int k = 0; k < 3; k++)\n"
    "                outc[k] += att * (X.lamb[i][k] * m_amb[k] + X.ldif[i][k] * m_dif[k] * dfac + X.lspec[i][k] * m_spc[k] * sfac);\n"
    "        }\n"
    "    }\n"
    "    uint c = (matupdate & 1) != 0 ? (rgba & 0xFF000000u) : (uint(X.flags.z & 0xFF) << 24);\n"
    "    for (int k = 0; k < 3; k++) {\n"
    "        float f = outc[k];\n"
    "        if (f < 0.0) f = 0.0;\n"
    "        if (f > 1.0) f = 1.0;\n"
    "        int q = int(f * 255.0 + 0.5);\n"
    "        c |= uint(q) << (8 * k);\n"
    "    }\n"
    "    return c;\n"
    "}\n"
    "void main() {\n"
    "    vec3 wpos = mul43(X.world, a_pos);\n"
    "    vec3 eye = mul43(X.view, wpos);\n"
    "    vec4 clip = mul44(X.proj, eye);\n"
    "    uint rgba = a_rgba;\n"
    "    int fog = 255;\n"
    "    if (X.flags.w != 0) {\n"
    "        precise float f = (X.coef_fog.y + eye.z) * X.coef_fog.z;\n"
    "        if (isnan(f) || isinf(f) || f <= 0.0) fog = 0;\n"
    "        else if (f < 1.0) fog = int(f * 255.0 + 0.5);\n"
    "    }\n"
    "    if (X.flags.x != 0) {\n"
    "        vec3 ne = mul33(X.view, mul33(X.world, a_nrm));\n"
    "        rgba = light_colour(eye, ne, rgba);\n"
    "    }\n"
    "    float u = a_uv.x, v = a_uv.y, texq = 1.0;\n"
    "    if (X.texmap_vp.x == 1) {\n"
    "        vec3 src;\n"
    "        if (X.texmap_vp.y == 1) {\n"
    "            float tw = X.texsize_off.x != 0.0 ? X.texsize_off.x : 1.0;\n"
    "            float th = X.texsize_off.y != 0.0 ? X.texsize_off.y : 1.0;\n"
    "            src = vec3(u / tw, v / th, 0.0);\n"
    "        } else {\n"
    "            src = a_pos;\n"
    "        }\n"
    "        vec3 gen = mul43(X.tgen, src);\n"
    "        u = gen.x * X.texsize_off.x;\n"
    "        v = gen.y * X.texsize_off.y;\n"
    "        texq = gen.z;\n"
    "    }\n"
    "    o.clip = clip;\n"
    "    o.col = vec4(float(rgba & 0xFFu) / 255.0, float((rgba >> 8) & 0xFFu) / 255.0, float((rgba >> 16) & 0xFFu) / 255.0, float((rgba >> 24) & 0xFFu) / 255.0);\n"
    "    o.uv = vec2(u, v);\n"
    "    o.texq = texq;\n"
    "    o.fog = float(fog) / 255.0;\n"
    "    o.draw = int(a_draw);\n"
    "    gl_Position = clip;\n"
    "}\n";
static const char *GS_MODEL_SRC =
    "#version 400 core\n"
    "layout(triangles) in;\n"
    "layout(triangle_strip, max_vertices = 4) out;\n"
    "in VData { vec4 clip; vec4 col; vec2 uv; float texq; float fog; flat int draw; } v[];\n"
    "struct XformBlock {\n"
    "    mat4  world; mat4 view; mat4 proj; mat4 tgen;\n"
    "    ivec4 lmeta[4];\n"
    "    vec4  lpos[4]; vec4 ldir[4]; vec4 latten[4]; vec4 lexpcut[4]; vec4 lamb[4]; vec4 ldif[4]; vec4 lspec[4];\n"
    "    vec4  memis; vec4 mamb; vec4 mdif; vec4 mspec; vec4 gamb;\n"
    "    vec4  coef_fog;\n"
    "    ivec4 flags;\n"
    "    ivec4 texmap_vp;\n"
    "    vec4  texsize_off;\n"
    "    vec4  vps; vec4 vpc;\n"
    "    ivec4 cull_strip;\n"
    "    ivec4 lodi;\n"
    "    vec4  guard_slope;\n"
    "};\n"
    "layout(std140) uniform Xform { XformBlock xb[16]; };\n"
    "#define X xb[v[0].draw]\n"
    "uniform int u_texenable;\n"
    "uniform vec2 u_viewport; uniform vec4 u_placement; uniform float u_ybias;\n"
    "out vec4 v_col;\n"
    "noperspective out vec3 v_uvq;\n"
    "out float v_fog;\n"
    "flat out int v_lod16;\n"
    "struct CV { vec4 c; vec4 col; vec2 uv; float texq; float fog; };\n"
    "struct SV { int x16; int y16; float sx; float sy; float sz; float invw; vec4 col; vec2 uv; float texq; float fog; };\n"
    "CV cv_of(int i) { CV r; r.c = v[i].clip; r.col = v[i].col; r.uv = v[i].uv; r.texq = v[i].texq; r.fog = v[i].fog; return r; }\n"
    "/* lerp_clip, ge.c: the attributes come from a, only the coordinates and uv move. */\n"
    "CV lerp_clip(CV a, CV b, float t) {\n"
    "    CV o = a;\n"
    "    precise vec4 c = a.c + (b.c - a.c) * t;\n"
    "    precise vec2 uv = a.uv + (b.uv - a.uv) * t;\n"
    "    o.c = c; o.uv = uv;\n"
    "    return o;\n"
    "}\n"
    "/* to_screen + ndc_to_screen + the 12.4 snap, ge.c. */\n"
    "bool screen_of(CV a, out SV s) {\n"
    "    if (a.c.w == 0.0) return false;\n"
    "    precise float inv = 1.0 / a.c.w;\n"
    "    precise float nx = a.c.x * inv;\n"
    "    precise float ny = a.c.y * inv;\n"
    "    precise float nz = a.c.z * inv;\n"
    "    precise float sx, sy, sz;\n"
    "    if (X.texmap_vp.z != 0) {\n"
    "        sx = nx * X.vps.x + X.vpc.x - X.texsize_off.z;\n"
    "        sy = ny * X.vps.y + X.vpc.y - X.texsize_off.w;\n"
    "    } else {\n"
    "        sx = nx * 240.0 + 240.0;\n"
    "        sy = ny * -136.0 + 136.0;\n"
    "    }\n"
    "    if (X.vps.z != 0.0) sz = nz * X.vps.z + X.vpc.z;\n"
    "    else              sz = (nz * 0.5 + 0.5) * 65535.0;\n"
    "    if (X.texmap_vp.w != 0) {\n"
    "        if (sz < 0.0) sz = 0.0;\n"
    "        if (sz > 65535.0) sz = 65535.0;\n"
    "    }\n"
    "    precise float fx = (sx + 0.03125) * 16.0;\n"
    "    precise float fy = (sy + 0.03125) * 16.0;\n"
    "    s.x16 = int(floor(fx)); s.y16 = int(floor(fy));\n"
    "    s.sx = sx; s.sy = sy; s.sz = sz; s.invw = inv;\n"
    "    s.col = a.col; s.uv = a.uv; s.texq = a.texq; s.fog = a.fog;\n"
    "    return true;\n"
    "}\n"
    "/* triangle_lod16 + psp_render_lod16, render_gl.c / render.c. */\n"
    "int lod16_of(SV a, SV b, SV c) {\n"
    "    if (u_texenable == 0 || X.lodi.z != 0) return 0;\n"
    "    float e1x = float(b.x16 - a.x16) / 16.0;\n"
    "    float e1y = float(b.y16 - a.y16) / 16.0;\n"
    "    float e2x = float(c.x16 - a.x16) / 16.0;\n"
    "    float e2y = float(c.y16 - a.y16) / 16.0;\n"
    "    precise float det = e1x * e2y - e1y * e2x;\n"
    "    if (det == 0.0) return 0;\n"
    "    float du1 = b.uv.x - a.uv.x, du2 = c.uv.x - a.uv.x;\n"
    "    float dv1 = b.uv.y - a.uv.y, dv2 = c.uv.y - a.uv.y;\n"
    "    precise float dudx = (du1 * e2y - du2 * e1y) / det;\n"
    "    precise float dudy = (du2 * e1x - du1 * e2x) / det;\n"
    "    precise float dvdx = (dv1 * e2y - dv2 * e1y) / det;\n"
    "    precise float dvdy = (dv2 * e1x - dv1 * e2x) / det;\n"
    "    precise float rx2 = dudx * dudx + dvdx * dvdx;\n"
    "    precise float ry2 = dudy * dudy + dvdy * dvdy;\n"
    "    float rx = sqrt(rx2), ry = sqrt(ry2);\n"
    "    float rho = rx > ry ? rx : ry;\n"
    "    int lod;\n"
    "    if (X.lodi.x == 0)      lod = (rho > 0.0) ? int(floor(log2(rho) * 16.0)) : -4096;\n"
    "    else if (X.lodi.x == 2) {\n"
    "        float s = 2.0 * X.guard_slope.z * (1.0 / a.invw + 1.0 / b.invw + 1.0 / c.invw) / 3.0;\n"
    "        lod = s > 0.0 ? int(floor(log2(s) * 16.0)) : -4096;\n"
    "    }\n"
    "    else                  lod = 0;\n"
    "    return lod + X.lodi.y;\n"
    "}\n"
    "void emit(SV s, int lod) {\n"
    "    vec2 pos = (X.cull_strip.w != 0) ? vec2(s.sx, s.sy) : vec2(float(s.x16) / 16.0, float(s.y16) / 16.0);\n"
    "    vec2 p = pos * u_placement.xy + u_placement.zw;\n"
    "    vec2 ndc = vec2((p.x / u_viewport.x) * 2.0 - 1.0, 1.0 - ((p.y - u_ybias) / u_viewport.y) * 2.0);\n"
    "    gl_Position = vec4(ndc, (s.sz / 65535.0) * 2.0 - 1.0, 1.0);\n"
    "    v_col = s.col;\n"
    "    v_uvq = vec3(s.uv * s.invw, s.texq * s.invw);\n"
    "    v_fog = s.fog;\n"
    "    v_lod16 = lod;\n"
    "    EmitVertex();\n"
    "}\n"
    "/* emit_tri, ge.c, for one triangle of the batch. */\n"
    "void main() {\n"
    "    CV tri[3]; tri[0] = cv_of(0); tri[1] = cv_of(1); tri[2] = cv_of(2);\n"
    "    int behind = 0;\n"
    "    for (int i = 0; i < 3; i++) if (tri[i].c.w <= 0.0) behind++;\n"
    "    if (behind == 3) return;\n"
    "    CV poly[4]; int n = 3;\n"
    "    if (X.texmap_vp.w == 0) {\n"
    "        for (int i = 0; i < 3; i++) {\n"
    "            if (tri[i].c.w == 0.0) return;\n"
    "            float nz = tri[i].c.z / tri[i].c.w;\n"
    "            if (!(nz >= -1.0 && nz <= 1.0)) return;\n"
    "        }\n"
    "        poly[0] = tri[0]; poly[1] = tri[1]; poly[2] = tri[2];\n"
    "    } else {\n"
    "        n = 0;\n"
    "        for (int i = 0; i < 3; i++) {\n"
    "            CV a = tri[i]; CV b = tri[(i + 1) % 3];\n"
    "            precise float da = a.c.z + a.c.w;\n"
    "            precise float db = b.c.z + b.c.w;\n"
    "            if (da >= 0.0) poly[n++] = a;\n"
    "            if ((da >= 0.0) != (db >= 0.0)) poly[n++] = lerp_clip(a, b, da / (da - db));\n"
    "        }\n"
    "        if (n < 3) return;\n"
    "    }\n"
    "    SV sv[4]; bool anyout = false;\n"
    "    for (int i = 0; i < n; i++) {\n"
    "        if (!screen_of(poly[i], sv[i])) return;\n"
    "        if (sv[i].sx < -X.guard_slope.x || sv[i].sx >= 4096.0 - X.guard_slope.x || sv[i].sy < -X.guard_slope.y || sv[i].sy >= 4096.0 - X.guard_slope.y) anyout = true;\n"
    "    }\n"
    "    if (anyout) return;\n"
    /* The sign of ax*by - ay*bx exactly, as ge.c takes it in 64-bit
     * integers: the two products in split 32-bit form, compared as signed
     * 64-bit values. No doubles. */
    "    int ax = sv[1].x16 - sv[0].x16, ay = sv[1].y16 - sv[0].y16;\n"
    "    int bx = sv[2].x16 - sv[0].x16, by = sv[2].y16 - sv[0].y16;\n"
    "    int h1, l1, h2, l2;\n"
    "    imulExtended(ax, by, h1, l1);\n"
    "    imulExtended(ay, bx, h2, l2);\n"
    "    int sgn = (h1 != h2) ? (h1 < h2 ? -1 : 1) : (uint(l1) == uint(l2) ? 0 : (uint(l1) < uint(l2) ? -1 : 1));\n"
    "    if (X.cull_strip.z != 0 && (gl_PrimitiveIDIn & 1) != 0) sgn = -sgn;\n"
    "    if (X.cull_strip.x != 0 && sgn != 0 && ((sgn < 0) == (X.cull_strip.y != 0))) return;\n"
    /* emit_tri draws the polygon as a fan from vertex 0: (0,1,2) and, for a
     * clipped quad, (0,2,3). One strip in the order 1,2,0,3 is those same two
     * triangles -- (1,2,0) then (2,0,3) -- and the provoking vertex of each,
     * the last one emitted, carries that triangle's LOD. GL never culls here,
     * so the strip's alternating winding does not matter. */
    "    int lodA = lod16_of(sv[0], sv[1], sv[2]);\n"
    "    if (n == 3) {\n"
    "        emit(sv[0], lodA); emit(sv[1], lodA); emit(sv[2], lodA);\n"
    "    } else {\n"
    "        int lodB = lod16_of(sv[0], sv[2], sv[3]);\n"
    "        emit(sv[1], lodA); emit(sv[2], lodA); emit(sv[0], lodA); emit(sv[3], lodB);\n"
    "    }\n"
    "}\n";

static const char *FS_SRC =
    "#version 330 core\n"
    "in vec4 v_col;\n"
    "noperspective in vec3 v_uvq;\n"
    "in float v_fog;\n"
    "flat in int v_lod16;\n"
    "uniform int u_atest;\n"
    "uniform int u_aref;\n"
    "uniform int u_amask;\n"
    "uniform int u_preblend_src;\n"
    "uniform int u_texenable;\n"
    "uniform int u_texfunc;\n"
    "uniform int u_tcc;\n"
    "uniform int u_double;\n"
    "uniform vec3 u_env;\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec2 u_texscale;\n"
    "uniform int u_bloom;\n"
    "uniform int u_minfilter;\n"
    "uniform int u_magfilter;\n"
    "uniform int u_wraps;\n"
    "uniform int u_wrapt;\n"
    "uniform int u_miptop;\n"
    "uniform int u_fogenable;\n"
    "uniform vec3 u_fogcolour;\n"
    "out vec4 o_col;\n"
    /* Use texelFetch and reproduce the PSP sampler explicitly. Native GL
     * filtering has a different minification switch for one legal PSP filter
     * combination, and its bilinear precision is implementation-defined; both
     * are visible at the 1:1 UI boundary. Coordinates and mip blending here
     * follow the software oracle's measured 1/16 rules. */
    "int wrap_axis(int p, int n, int clamp_it) {\n"
    "    if (clamp_it != 0) return clamp(p, 0, n - 1);\n"
    "    int r = p % n; return r < 0 ? r + n : r;\n"
    "}\n"
    "ivec4 texel_i(ivec2 p, int level) {\n"
    "    ivec2 sz = textureSize(u_tex, level);\n"
    "    p.x = wrap_axis(p.x, sz.x, u_wraps);\n"
    "    p.y = wrap_axis(p.y, sz.y, u_wrapt);\n"
    "    return ivec4(floor(texelFetch(u_tex, p, level) * 255.0 + 0.5));\n"
    "}\n"
    "ivec4 sample_level(vec2 uv, int level, bool linear) {\n"
    "    ivec2 sz = textureSize(u_tex, level);\n"
    "    ivec2 base_sz = textureSize(u_tex, 0);\n"
    "    vec2 tc = uv * vec2(sz) / vec2(base_sz);\n"
    /* Interpolated exact texel boundaries may arrive just below the integer
     * on one triangle (observed on Radeon with a half-pixel-aligned 1:1 quad).
     * Apply the same 0.001/16 texel roundoff tolerance as the bilinear path;
     * otherwise nearest sampling shifts half the image by a whole texel. */
    "    if (!linear) return texel_i(ivec2(floor(tc + vec2(0.0000625))), level);\n"
    "    ivec2 fq = ivec2(floor((tc - vec2(0.5)) * 16.0 + vec2(0.001)));\n"
    "    ivec2 p0 = ivec2(floor(vec2(fq) / 16.0));\n"
    "    ivec2 a = fq - p0 * 16;\n"
    "    ivec4 t00 = texel_i(p0,                 level);\n"
    "    ivec4 t10 = texel_i(p0 + ivec2(1, 0), level);\n"
    "    ivec4 t01 = texel_i(p0 + ivec2(0, 1), level);\n"
    "    ivec4 t11 = texel_i(p0 + ivec2(1, 1), level);\n"
    "    int w00 = (16 - a.x) * (16 - a.y);\n"
    "    int w10 = a.x * (16 - a.y);\n"
    "    int w01 = (16 - a.x) * a.y;\n"
    "    int w11 = a.x * a.y;\n"
    "    return (t00 * w00 + t10 * w10 + t01 * w01 + t11 * w11) / 256;\n"
    "}\n"
    /* Positive cubic B-spline weights soften the bloom without the ringing
     * of a sharpening cubic. Continuous weights replace the 1/16 sampler
     * steps only for the identified final glow composite. */
    "vec4 bloom_weights(float t) {\n"
    "    float t2 = t*t, t3 = t2*t;\n"
    "    return vec4((1.0-t)*(1.0-t)*(1.0-t), 3.0*t3-6.0*t2+4.0,\n"
    "                -3.0*t3+3.0*t2+3.0*t+1.0, t3) / 6.0;\n"
    "}\n"
    "vec4 sample_bloom(vec2 uv) {\n"
    "    vec2 p = uv - vec2(0.5);\n"
    "    ivec2 base = ivec2(floor(p));\n"
    "    vec4 wx = bloom_weights(fract(p.x)), wy = bloom_weights(fract(p.y));\n"
    "    vec4 c = vec4(0.0);\n"
    "    for (int y=0; y<4; y++) for (int x=0; x<4; x++)\n"
    "        c += vec4(texel_i(base + ivec2(x-1,y-1), 0)) * wx[x] * wy[y];\n"
    "    return c / 255.0;\n"
    "}\n"
    "vec4 sample_psp(vec2 uv) {\n"
    "    uv *= u_texscale;\n"
    "    if (u_bloom != 0) return sample_bloom(uv);\n"
    "    int lod = v_lod16;\n"
    "    bool linear = (((lod > 0 ? u_minfilter : u_magfilter) & 1) != 0);\n"
    "    if (u_minfilter < 4 || u_miptop <= 0)\n"
    "        return vec4(sample_level(uv, 0, linear)) / 255.0;\n"
    "    lod = clamp(lod, 0, u_miptop * 16);\n"
    "    if ((u_minfilter & 2) == 0) {\n"
    "        int level = min((lod + 8) / 16, u_miptop);\n"
    "        return vec4(sample_level(uv, level, linear)) / 255.0;\n"
    "    }\n"
    "    int level = lod / 16, f = lod & 15;\n"
    "    ivec4 c0 = sample_level(uv, level, linear);\n"
    "    if (f == 0 || level >= u_miptop) return vec4(c0) / 255.0;\n"
    "    ivec4 c1 = sample_level(uv, level + 1, linear);\n"
    "    return vec4(c0 + (c1 - c0) * f / 16) / 255.0;\n"
    "}\n"
    /* The software oracle and the GE tests put an RGBA8 quantisation point at
     * the interpolated vertex colour, and another after the texture function.
     * Leaving either value fractional lets fog and blending see precision the
     * PSP did not carry, which becomes a two-level error after colour-double. */
    "vec4 rgba8(vec4 c) {\n"
    "    return floor(clamp(c, 0.0, 1.0) * 255.0 + 0.5) / 255.0;\n"
    "}\n"
    "vec4 texfunc(vec4 c) {\n"
    "    c = rgba8(c);\n"
    "    vec2 uv = (v_uvq.z != 0.0) ? v_uvq.xy / v_uvq.z : vec2(0.0);\n"
    "    vec4 t = sample_psp(uv);\n"
    "    vec3 tc = t.rgb; float ta = (u_tcc != 0) ? t.a : 1.0;\n"
    "    vec3 rgb; float a;\n"
    "    if (u_texfunc == 1) {\n"          /* DECAL */
    "        rgb = (u_tcc != 0) ? mix(c.rgb, tc, t.a) : tc;\n"
    "        a   = (u_tcc != 0) ? c.a : c.a;\n"
    "    } else if (u_texfunc == 2) {\n"    /* BLEND */
    "        rgb = mix(c.rgb, u_env, tc);\n"
    "        a   = c.a * ta;\n"
    "    } else if (u_texfunc == 3) {\n"    /* REPLACE */
    "        rgb = tc;\n"
    "        a   = (u_tcc != 0) ? t.a : c.a;\n"
    "    } else if (u_texfunc == 4) {\n"    /* ADD */
    "        rgb = c.rgb + tc;\n"
    "        a   = c.a * ta;\n"
    "    } else {\n"                        /* MODULATE */
    "        rgb = c.rgb * tc;\n"
    "        a   = c.a * ta;\n"
    "    }\n"
    "    vec4 q = rgba8(vec4(rgb, a));\n"
    "    if (u_double != 0) q.rgb = min(q.rgb * 2.0, vec3(1.0));\n"
    "    return q;\n"
    "}\n"
    "vec4 fog_psp(vec4 c) {\n"
    "    ivec4 ci = ivec4(floor(clamp(c, 0.0, 1.0) * 255.0 + 0.5));\n"
    "    int f = int(floor(clamp(v_fog, 0.0, 1.0) * 255.0 + 0.5));\n"
    "    if (f < 255) {\n"
    "        ivec3 fc = ivec3(floor(clamp(u_fogcolour, 0.0, 1.0) * 255.0 + 0.5));\n"
    "        ci.rgb = (ci.rgb * f + fc * (255 - f) + ivec3(255)) / 256;\n"
    "    }\n"
    "    return vec4(ci) / 255.0;\n"
    "}\n"
    "void main() {\n"
    "    vec4 c = (u_texenable != 0) ? texfunc(v_col) : rgba8(v_col);\n"
    "    if (u_fogenable != 0)\n"
    "        c = fog_psp(c);\n"
    "    if (u_atest != 1) {\n"
    "        int a = int(floor(c.a * 255.0 + 0.5)) & u_amask;\n"
    "        int ref = u_aref & u_amask;\n"
    "        bool pass = true;\n"
    "        if      (u_atest == 0) pass = false;\n"
    "        else if (u_atest == 2) pass = (a == ref);\n"
    "        else if (u_atest == 3) pass = (a != ref);\n"
    "        else if (u_atest == 4) pass = (a <  ref);\n"
    "        else if (u_atest == 5) pass = (a <= ref);\n"
    "        else if (u_atest == 6) pass = (a >  ref);\n"
    "        else if (u_atest == 7) pass = (a >= ref);\n"
    "        if (!pass) discard;\n"
    "    }\n"
    /* Fixed-function GL keeps the source-alpha multiplication fractional and
     * rounds after combining both blend terms. The GE first truncates each
     * term with ((channel + 1) * factor) >> 8. Source alpha depends only on
     * this fragment, so do that term exactly here and ask GL to add it whole;
     * the original alpha remains available to scale the destination term. */
    /* Mode 3 is additive doubled source alpha, with alpha writes
     * masked. Compute the source term before addition; GL applies the inverse
     * factor to the destination. Normalized fixed-function rounding can differ
     * from software by one RGB step, checked by the exhaustive host fixture. */
    "    if (u_preblend_src == 3) {\n"
    "        ivec4 ci = ivec4(floor(c * 255.0 + 0.5));\n"
    "        ivec3 s = min(ivec3(255), 2 * (((ci.rgb + ivec3(1)) * ci.a) / 256));\n"
    "        c.rgb = vec3(s) / 255.0;\n"
    "        c.a = float(min(255, 2 * ci.a)) / 255.0;\n"
    "    } else if (u_preblend_src != 0) {\n"
    "        ivec4 ci = ivec4(floor(c * 255.0 + 0.5));\n"
    "        ci.rgb = ((ci.rgb + ivec3(1)) * ci.a) / 256;\n"
    "        if (u_preblend_src == 2) {\n"
    /* For destination (1-src-alpha), make GL's final round reproduce
     * floor((dst+1)*(255-a)/256): use the exact f/256 scale, then add
     * f/256 - 1/2 to the already-integer source term before that round. The
     * 1/4096 bias resolves an exact half tie upward and is smaller than the
     * expression's smallest non-zero 1/256 fractional step. */
    "            float f = float(255 - ci.a) / 256.0;\n"
    "            c.rgb = (vec3(ci.rgb) + vec3(f - 0.5 + 1.0 / 4096.0)) / 255.0;\n"
    "            c.a = float(ci.a + 1) / 256.0;\n"
    "        } else {\n"
    "            c.rgb = vec3(ci.rgb) / 255.0;\n"
    "        }\n"
    "    }\n"
    "    o_col = c;\n"
    "}\n";

static GLuint compile(GLenum type, const char *src, const char *what) {
    GLuint sh = p_glCreateShader(type);
    p_glShaderSource(sh, 1, &src, NULL);
    p_glCompileShader(sh);
    GLint ok = 0;
    p_glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        p_glGetShaderInfoLog(sh, sizeof log, NULL, log);
        fprintf(stderr, "gl: %s shader failed to compile:\n%s\n", what, log);
        p_glDeleteShader(sh);
        return 0;
    }
    return sh;
}

/* GL 3.3 cannot sample the stencil attachment directly. These tiny bit-plane
 * passes keep the hardware stencil and the PSP's framebuffer alpha byte in
 * sync entirely on the GPU. Import copies the colour first to avoid texture
 * feedback; export tests each stencil bit and adds its exact byte weight.
 * Normal rendering does not pay for a copy on every draw. */
static int build_stencil_program(void) {
    const char *vs_src =
        "#version 330 core\n"
        "void main() {\n"
        " vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
        " gl_Position = vec4(p * 2.0 - 1.0, 0, 1);\n"
        "}\n";
    const char *fs_src =
        "#version 330 core\n"
        "uniform sampler2D u_copy; uniform int u_mode, u_bit;\n"
        "uniform float u_value; out vec4 o_col;\n"
        "void main() {\n"
        " if (u_mode == 1) {\n"
        "  int a = int(floor(texelFetch(u_copy, ivec2(gl_FragCoord.xy), 0).a * 255.0 + 0.5));\n"
        "  if ((a & u_bit) == 0) discard;\n"
        " }\n"
        " o_col = vec4(0, 0, 0, u_value);\n"
        "}\n";
    GLuint vs = compile(GL_VERTEX_SHADER, vs_src, "stencil vertex");
    GLuint fs = compile(GL_FRAGMENT_SHADER, fs_src, "stencil fragment");
    if (!vs || !fs) return -1;
    g.stencil_prog = p_glCreateProgram();
    p_glAttachShader(g.stencil_prog, vs); p_glAttachShader(g.stencil_prog, fs);
    p_glLinkProgram(g.stencil_prog);
    p_glDeleteShader(vs); p_glDeleteShader(fs);
    GLint ok = 0;
    p_glGetProgramiv(g.stencil_prog, GL_LINK_STATUS, &ok);
    if (!ok) { fprintf(stderr, "gl: stencil program failed to link\n"); return -1; }
    g.s_mode = p_glGetUniformLocation(g.stencil_prog, "u_mode");
    g.s_bit = p_glGetUniformLocation(g.stencil_prog, "u_bit");
    g.s_value = p_glGetUniformLocation(g.stencil_prog, "u_value");
    p_glGenTextures(1, &g.stencil_copy);
    return 0;
}

static void stencil_pass_state(rendertarget *r) {
    p_glBindFramebuffer(GL_FRAMEBUFFER, r->fbo);
    p_glViewport(0, 0, r->w, r->h);
    p_glUseProgram(g.stencil_prog);
    p_glBindVertexArray(g.vao);
    p_glDisable(GL_SCISSOR_TEST);
    p_glDisable(GL_DEPTH_TEST);
    p_glDepthMask(GL_FALSE);
    p_glDisable(GL_BLEND);
    p_glEnable(GL_STENCIL_TEST);
}

static void alpha_to_stencil(rendertarget *r) {
    if (r->stencil_valid || r->fmt != 3) return;
    g.mu.disturbed = 1;
    stencil_pass_state(r);
    p_glBindTexture(GL_TEXTURE_2D, g.stencil_copy);
    if (g.stencil_copy_w != r->w || g.stencil_copy_h != r->h) {
        p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, r->w, r->h, 0,
                       GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        g.stencil_copy_w = r->w; g.stencil_copy_h = r->h;
    }
    p_glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, r->w, r->h);
    p_glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    p_glStencilMask(255);
    p_glClearStencil(0); p_glClear(GL_STENCIL_BUFFER_BIT);
    p_glUniform1i(g.s_mode, 1);
    p_glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    for (int bit = 1; bit <= 128; bit <<= 1) {
        p_glStencilMask((GLuint)bit);
        p_glStencilFunc(GL_ALWAYS, bit, 255);
        p_glUniform1i(g.s_bit, bit);
        p_glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    r->stencil_valid = 1;
    g.stencil_imports++;
}

static void stencil_to_alpha(rendertarget *r) {
    if (!r->alpha_dirty) return;
    g.mu.disturbed = 1;
    stencil_pass_state(r);
    p_glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
    p_glClearColor(0, 0, 0, 0); p_glClear(GL_COLOR_BUFFER_BIT);
    p_glStencilMask(0);
    p_glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    p_glUniform1i(g.s_mode, 0);
    p_glEnable(GL_BLEND);
    p_glBlendFunc(GL_ONE, GL_ONE); p_glBlendEquation(GL_FUNC_ADD);
    for (int bit = 1; bit <= 128; bit <<= 1) {
        p_glStencilFunc(GL_NOTEQUAL, 0, (GLuint)bit);
        p_glUniform1f(g.s_value, (float)bit / 255.0f);
        p_glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    r->alpha_dirty = 0;
    g.stencil_exports++;
}

static void lookup_uniforms(GLuint prog, uniforms *u) {
#define U(f, n) u->f = p_glGetUniformLocation(prog, n)
    U(viewport, "u_viewport"); U(placement, "u_placement"); U(ybias, "u_ybias");
    U(atest, "u_atest"); U(aref, "u_aref"); U(amask, "u_amask"); U(preblend_src, "u_preblend_src");
    U(texenable, "u_texenable"); U(texfunc, "u_texfunc"); U(tcc, "u_tcc"); U(dbl, "u_double");
    U(env, "u_env"); U(tex, "u_tex"); U(texscale, "u_texscale"); U(bloom, "u_bloom");
    U(minfilter, "u_minfilter"); U(magfilter, "u_magfilter"); U(wraps, "u_wraps"); U(wrapt, "u_wrapt");
    U(miptop, "u_miptop"); U(fogenable, "u_fogenable"); U(fogcolour, "u_fogcolour");
#undef U
}
static void use_program(GLuint prog, uniforms *u) { p_glUseProgram(prog); g.cur_prog = prog; g.u = u; }
static int build_model_program(void) {
    GLuint vs = compile(GL_VERTEX_SHADER, VS_MODEL_SRC, "model vertex");
    GLuint gs = compile(GL_GEOMETRY_SHADER, GS_MODEL_SRC, "model geometry");
    /* The fragment shader, at the model program's GLSL version. */
    static char fs400[32768];
    const char *nl = strchr(FS_SRC, '\n');
    snprintf(fs400, sizeof fs400, "#version 400 core%s", nl ? nl : "");
    GLuint fs = compile(GL_FRAGMENT_SHADER, fs400, "model fragment");
    if (!vs || !gs || !fs) { if (vs) p_glDeleteShader(vs); if (gs) p_glDeleteShader(gs); if (fs) p_glDeleteShader(fs); return -1; }
    g.prog_model = p_glCreateProgram();
    p_glAttachShader(g.prog_model, vs);
    p_glAttachShader(g.prog_model, gs);
    p_glAttachShader(g.prog_model, fs);
    p_glLinkProgram(g.prog_model);
    GLint ok = 0;
    p_glGetProgramiv(g.prog_model, GL_LINK_STATUS, &ok);
    p_glDeleteShader(vs); p_glDeleteShader(gs); p_glDeleteShader(fs);
    if (!ok) {
        char log[1024];
        p_glGetProgramInfoLog(g.prog_model, sizeof log, NULL, log);
        fprintf(stderr, "gl: model program failed to link:\n%s\n", log);
        g.prog_model = 0;
        return -1;
    }
    lookup_uniforms(g.prog_model, &g.ug);
    const GLuint blk = p_glGetUniformBlockIndex(g.prog_model, "Xform");
    if (blk == GL_INVALID_INDEX) { fprintf(stderr, "gl: model program has no Xform block\n"); return -1; }
    p_glUniformBlockBinding(g.prog_model, blk, 1);
    return 0;
}
static int build_program(void) {
    GLuint vs = compile(GL_VERTEX_SHADER, VS_SRC, "vertex");
    GLuint fs = compile(GL_FRAGMENT_SHADER, FS_SRC, "fragment");
    if (!vs || !fs) return -1;
    g.prog = p_glCreateProgram();
    p_glAttachShader(g.prog, vs);
    p_glAttachShader(g.prog, fs);
    p_glLinkProgram(g.prog);
    GLint ok = 0;
    p_glGetProgramiv(g.prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        p_glGetProgramInfoLog(g.prog, sizeof log, NULL, log);
        fprintf(stderr, "gl: program failed to link:\n%s\n", log);
        return -1;
    }
    p_glDeleteShader(vs);
    p_glDeleteShader(fs);
    lookup_uniforms(g.prog, &g.um);
    g.u = &g.um; g.cur_prog = g.prog;
    return 0;
}

/* Find the record for an address.  Storage is deliberately deferred until the
 * first draw: FBP, FBW and FBFMT are separate GE registers, so set_target sees
 * several half-assembled combinations while a surface is being selected.  If
 * we allocated here, the first transient format would become permanent. */
static int rt_for(uint32_t addr) {
    addr &= PSP_ADDR_MASK;
    for (int i = 0; i < g.n_rts; i++)
        if (g.rts[i].addr == addr) return i;
    if (g.n_rts >= RT_MAX) { g.rt_overflow++; return g.cur_rt; }

    const int i = g.n_rts++;
    rendertarget *r = &g.rts[i];
    r->addr = addr; r->used = 1;

    return i;
}

/* ---- adaptive aspect ---------------------------------------------------------
 *
 * Guest framebuffer storage remains PSP-sized. Display attachments expand
 * to the camera's virtual width/height, or the physical drawable when enhanced
 * resolution is enabled. SCENE spreads the adjusted projection across that
 * attachment. HUD/PREVIEW use uniform scale and centered x/y offsets, with
 * corresponding scissors. Both shader paths apply the same placement.
 * Native aspect retains the original raster and presentation mappings. */
/* Only a title whose psp_title_info claims PSP_TITLE_HUD_BANDS places
 * off-screen HUD draws in the wide bands (psprecomp/host/title.h). */
static int hud_bands_available(void) {
    return psp_title_can(PSP_TITLE_HUD_BANDS);
}
/* The safe area starts on a whole physical pixel, on both axes and at every
 * scale. At ui_scale 1 (PSP resolution, or a small window) each guest pixel
 * is then exactly one target pixel, so LINEAR samples texel centres and the
 * HUD stays as sharp as at 1x; a fractional offset (645-480)/2 = 82.5 would
 * blend every texel with its neighbour and give the scissor a 481st column.
 * Scaled HUD keeps the same whole-pixel origin as its scissor. Flooring
 * leaves any odd pixel on the right/bottom, as the native mapping did; the
 * epsilon keeps the limiting axis (visible == 480 * visible/480) at 0. */
static double rt_off(const rendertarget *r) {
    if (!r->wide) return 0;
    return floor((r->visible_w - g.w * r->ui_scale) * 0.5 + 1e-6);
}
static double rt_off_y(const rendertarget *r) {
    if (!r->wide) return 0;
    return floor((r->visible_h - g.h * r->ui_scale) * 0.5 + 1e-6);
}
static int rt_scene_w(const rendertarget *r) {
    return (int)lround(r->guest_w * r->sx);
}

/* One coherent drawable snapshot drives allocation for the next frame. */
static void resolution_size(void) {
    int w, h;
    present_gl_drawable_size(&w, &h);
    if (w <= 0 || h <= 0) return;
    g.wide_w = g.w; g.wide_h = g.h;
    if (g.adaptive_aspect) {
        present_aspect_extent(w, h, &g.wide_w, &g.wide_h);
        /* The camera hooks project for exactly this frame's placement. */
        present_aspect_latch_scene_size(g.wide_w, g.wide_h);
    }
    g.pixel_w = g.wide_w; g.pixel_h = g.wide_h;
    if (!g.resolution) return;
    /* The target takes the drawable's shape when the scene does. A title's
     * own scene_extent follows the drawable on both axes (The 3rd Birthday's
     * widens or deepens); the host's only widens, so a window narrower than
     * the PSP's shape letterboxes, as Last Raven's backend did. */
    if (g.adaptive_aspect &&
        (psp_title_info.scene_extent || (long long)w * g.h >= (long long)h * g.w)) {
        g.pixel_w = w; g.pixel_h = h;
    } else {
        g.pixel_w = w;
        g.pixel_h = (int)((long long)w * g.h / g.w);
        if (g.pixel_h > h) {
            g.pixel_h = h;
            g.pixel_w = (int)((long long)h * g.w / g.h);
        }
    }
    if (g.pixel_w < 1) g.pixel_w = 1;
    if (g.pixel_h < 1) g.pixel_h = 1;
}

static int pixel_edge(double x) { return (int)ceil(x - 0.5); }

static void rt_layout(rendertarget *r) {
    r->display = r->stride >= (uint32_t)g.w && r->guest_h >= g.h;
    r->wide = r->display && g.adaptive_aspect && (g.wide_w > g.w || g.wide_h > g.h);
    r->wide_w = r->wide ? g.wide_w : g.w;
    r->wide_h = r->wide ? g.wide_h : g.h;
    r->visible_w = r->wide_w;
    r->visible_h = r->wide_h;
    if (g.resolution && r->display) {
        r->visible_w = g.pixel_w;
        r->visible_h = g.pixel_h;
    }
    r->sx = r->display ? (double)r->visible_w / g.w : 1.0;
    r->sy = r->display ? (double)r->visible_h / g.h : 1.0;
    r->ui_scale = fmin(r->sx, r->sy);
    double cap =
        fmin((double)g.max_size / (r->guest_w * r->sx), (double)g.max_size / (r->guest_h * r->sy));
    if (cap < 1.0) {
        r->visible_w = (int)fmax(1, floor(r->visible_w * cap));
        r->visible_h = (int)fmax(1, floor(r->visible_h * cap));
        r->sx = (double)r->visible_w / g.w;
        r->sy = (double)r->visible_h / g.h;
        r->ui_scale *= cap;
        fprintf(stderr, "gl: resolution capped to %dx%d by GL limit %d\n", r->visible_w,
                r->visible_h, g.max_size);
    }
    r->w = (int)ceil(r->guest_w * r->sx);
    r->h = (int)ceil(r->guest_h * r->sy);
}

/* The write observer is asked about writes into configured targets only:
 * the union of their ranges, kept as targets come and go. Without it every
 * store the recompiled code makes would pay the call. */
static void rt_watch_writes(void) {
    uint64_t lo = UINT64_MAX, hi = 0;
    for (int i = 0; i < g.n_rts; i++) {
        const rendertarget *r = &g.rts[i];
        if (!r->configured) continue;
        const uint64_t end = (uint64_t)r->addr + (uint64_t)r->stride * r->guest_h * (r->fmt == 3 ? 4 : 2);
        if (r->addr < lo) lo = r->addr;
        if (end > hi) hi = end;
    }
    if (hi > lo) psp_mem_set_write_observer_range((uint32_t)lo, (uint32_t)(hi > UINT32_MAX ? UINT32_MAX : hi));
    else psp_mem_set_write_observer_range(0, 0);
}
static void rt_release(rendertarget *r) {
    if (r->fbo) p_glDeleteFramebuffers(1, &r->fbo);
    if (r->colour) p_glDeleteTextures(1, &r->colour);
    if (r->depth) p_glDeleteRenderbuffers(1, &r->depth);
    free(r->cpu_dirty);
    r->fbo = r->colour = r->depth = 0;
    r->cpu_dirty = NULL;
    r->configured = 0;
    rt_watch_writes();
}

static uint32_t decode_pixel(uint32_t v, int fmt) {
    if (fmt == 3) return v;
    unsigned red, green, blue, alpha;
    if (fmt == 2) {
        red = (v & 15) * 17;
        green = ((v >> 4) & 15) * 17;
        blue = ((v >> 8) & 15) * 17;
        alpha = ((v >> 12) & 15) * 17;
    } else {
        red = v & 31;
        red = (red << 3) | (red >> 2);
        if (fmt == 0) {
            green = (v >> 5) & 63;
            green = (green << 2) | (green >> 4);
            blue = (v >> 11) & 31;
            alpha = 255;
        } else {
            green = (v >> 5) & 31;
            green = (green << 3) | (green >> 2);
            blue = (v >> 10) & 31;
            alpha = (v & 0x8000) ? 255 : 0;
        }
        blue = (blue << 3) | (blue >> 2);
    }
    return red | (green << 8) | (blue << 16) | (alpha << 24);
}

static uint32_t pack_pixel(uint32_t rgba, int fmt) {
    if (fmt == 3) return rgba;
    unsigned r = rgba & 255, g = (rgba >> 8) & 255, b = (rgba >> 16) & 255, a = rgba >> 24;
    if (fmt == 0) return (r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11);
    if (fmt == 1) return (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | ((a >> 7) << 15);
    return (r >> 4) | ((g >> 4) << 4) | ((b >> 4) << 8) | ((a >> 4) << 12);
}

static uint32_t guest_pixel(const rendertarget *r, int x, int y) {
    if (x >= (int)r->stride) return 0;
    const int bpp = r->fmt == 3 ? 4 : 2;
    const void *p = psp_mem_ptr(r->addr + (uint32_t)(y * r->stride + x) * bpp, bpp);
    uint32_t v = 0;
    if (p) memcpy(&v, p, bpp);
    return decode_pixel(v, r->fmt);
}

/* Track precise touched pixels, including a CPU store of the same byte value.
 * Generation/byte comparisons alone miss such a store over newer GPU pixels. */
static void rt_guest_write(uint32_t addr, uint32_t size) {
    for (int i = 0; i < g.n_rts; i++) {
        rendertarget *r = &g.rts[i];
        if (!r->configured || !r->cpu_dirty || g.exporting == i + 1) continue;
        const unsigned bpp = r->fmt == 3 ? 4 : 2;
        const uint64_t end = (uint64_t)r->addr + (uint64_t)r->stride * r->guest_h * bpp;
        if ((uint64_t)addr + size <= r->addr || addr >= end) continue;
        /* A readback of this target still in flight has already landed: the
         * access observer ran before the write, from the pointer lookup. */
        const uint64_t first = addr > r->addr ? addr - r->addr : 0;
        const uint64_t last =
            (uint64_t)addr + size < end ? (uint64_t)addr + size - r->addr : end - r->addr;
        for (uint64_t p = first / bpp; p < (last + bpp - 1) / bpp; p++) {
            unsigned mask = 0;
            for (unsigned b = 0; b < bpp; b++)
                if (p * bpp + b >= first && p * bpp + b < last) mask |= 1u << b;
            r->cpu_dirty[p] |= mask;
        }
        r->cpu_pending = 1;
    }
}

static int rt_allocate(rendertarget *r, int inherit) {
    g.mu.disturbed = 1;
    p_glGenFramebuffers(1, &r->fbo);
    p_glBindFramebuffer(GL_FRAMEBUFFER, r->fbo);
    p_glGenTextures(1, &r->colour);
    p_glBindTexture(GL_TEXTURE_2D, r->colour);
    uint32_t *initial = NULL;
    if (inherit && (r->fmt == 3 || g.resolution)) {
        initial = calloc((size_t)r->w * r->h, 4);
        if (!initial) {
            rt_release(r);
            return -1;
        }
        for (int y = 0; y < r->h; y++)
            for (int x = 0; x < r->w; x++) {
                int gx = (int)((x + 0.5) / r->sx), gy = (int)((y + 0.5) / r->sy);
                if (gx >= r->guest_w) gx = r->guest_w - 1;
                if (gy >= r->guest_h) gy = r->guest_h - 1;
                initial[(size_t)(r->h - 1 - y) * r->w + x] = guest_pixel(r, gx, gy);
            }
    }
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, r->w, r->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, initial);
    free(initial);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->colour, 0);
    p_glGenRenderbuffers(1, &r->depth);
    p_glBindRenderbuffer(GL_RENDERBUFFER, r->depth);
    p_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, r->w, r->h);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
                                r->depth);
    if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "gl: target %08X allocation %dx%d failed\n", r->addr, r->w, r->h);
        rt_release(r);
        return -1;
    }
    r->cpu_dirty = calloc((size_t)r->stride * r->guest_h, 1);
    if (!r->cpu_dirty) {
        rt_release(r);
        return -1;
    }
    r->configured = 1;
    rt_watch_writes();
    if (r->wide) g.wide_allocs++;
    return 0;
}

/* Guest reconfiguration reinterprets bytes; resizing preserves GPU history. */
static int rt_prepare_shape(int i, int stride, int fmt, int w, int h, int inherit) {
    rendertarget *r = &g.rts[i];
    if (r->configured) {
        if (r->stride == (uint32_t)stride && r->fmt == fmt && r->guest_w >= w &&
            r->guest_h >= h)
            return 0;
        if (r->dirty || r->cpu_pending) readback_rt(i);
        if (r->stride == (uint32_t)stride && r->fmt == fmt) {
            if (w < r->guest_w) w = r->guest_w;
            if (h < r->guest_h) h = r->guest_h;
        }
        rt_release(r);
    }
    r->stride = stride;
    r->fmt = fmt;
    r->guest_w = w;
    r->guest_h = h;
    r->dirty = r->alpha_dirty = r->stencil_valid = r->cpu_pending = 0;
    rt_layout(r);
    return rt_allocate(r, inherit);
}

/* The GE packs surfaces end to end, so a target ends where the next one
 * that has been drawn into begins, however tall the scissor says it is.
 * WipEout Pulse keeps two 256x136 half-resolution surfaces and its 128x128
 * countdown screen back to back at 0x04110000, 0x04132000 and 0x04154000,
 * and draws the first two under a full-screen scissor. Sized 480x272 from
 * it, the second reached over the countdown screen, its readbacks wrote over
 * it, and the screen over the start line drew black; every write into the
 * overlap also marked both targets for a CPU import. */
/* A display-sized target keeps every row the screen shows: a target that
 * starts inside those is a window onto the same surface, not the next one.
 * WipEout Pulse draws its music ticker into one 249 rows down each display
 * buffer; cut back to it, the display buffers were re-grown and cut again
 * every frame. */
static int rt_min_rows(int stride, int h) {
    return stride >= g.w ? (h < g.h ? h : g.h) : 1;
}

static int rt_rows_before_next(int i, uint32_t addr, int stride, int fmt, int h) {
    const uint64_t row = (uint64_t)stride * (fmt == 3 ? 4u : 2u);
    if (!row) return h;
    const int least = rt_min_rows(stride, h);
    for (int k = 0; k < g.n_rts; k++) {
        const rendertarget *o = &g.rts[k];
        if (k == i || !o->configured || o->addr <= addr) continue;
        const uint64_t gap = o->addr - addr;
        if (gap >= row * (uint64_t)least && gap < row * (uint64_t)h) h = (int)(gap / row);
    }
    return h;
}

/* Re-shape a configured target smaller, keeping what it holds. */
static int rt_shrink(int i, int stride, int fmt, int w, int h) {
    rendertarget *r = &g.rts[i];
    if (g.rt_shrinks < 8)
        fprintf(stderr, "gl: target %08X cut back from %dx%d to %dx%d (stride %d, format %d)\n",
                r->addr, r->guest_w, r->guest_h, w, h, stride, fmt);
    if (r->dirty || r->cpu_pending) readback_rt(i);
    rt_release(r);
    g.rt_shrinks++;
    return rt_prepare_shape(i, stride, fmt, w, h, 1);
}

/* An older target that covers a newer one's start gives it the rows. */
static void rt_yield_to(int i) {
    const uint32_t start = g.rts[i].addr;
    for (int k = 0; k < g.n_rts; k++) {
        const rendertarget *o = &g.rts[k];
        if (k == i || !o->configured || o->addr >= start) continue;
        const uint64_t row = (uint64_t)o->stride * (o->fmt == 3 ? 4u : 2u);
        if (!row || (uint64_t)o->addr + row * (uint64_t)o->guest_h <= start) continue;
        const int rows = (int)((start - o->addr) / row);
        if (rows >= rt_min_rows((int)o->stride, o->guest_h))
            rt_shrink(k, (int)o->stride, o->fmt, o->guest_w, rows);
    }
}

/* The GE's target, shaped by its stride, format and scissor. A row is no
 * wider than its stride -- past it the GE is writing the next row -- and the
 * target stops at the next one (rt_rows_before_next). */
static int rt_prepare(int i) {
    const int stride = g.target_stride ? (int)g.target_stride : g.w;
    int w = stride, h = g.sc_valid ? g.sc_y1 + 1 : g.h;
    if (g.sc_valid && g.sc_x1 + 1 > w) w = g.sc_x1 + 1;
    if (w < 1) w = g.w;
    if (h < 1) h = g.h;
    if (w > stride) w = stride;
    h = rt_rows_before_next(i, g.rts[i].addr, stride, g.target_fmt, h);
    const rendertarget *r = &g.rts[i];
    int rc = 1;
    /* A smaller scissor is no reason to shrink; a neighbour or the stride is. */
    if (r->configured && r->stride == (uint32_t)stride && r->fmt == g.target_fmt) {
        const int keep_w = r->guest_w > stride ? stride : r->guest_w;
        const int keep_h = rt_rows_before_next(i, r->addr, stride, g.target_fmt, r->guest_h);
        if (keep_w < r->guest_w || keep_h < r->guest_h)
            rc = rt_shrink(i, stride, g.target_fmt, w > keep_w ? w : keep_w, h > keep_h ? h : keep_h);
    }
    if (rc == 1) rc = rt_prepare_shape(i, stride, g.target_fmt, w, h, 1);
    if (rc == 0) rt_yield_to(i);
    return rc;
}

/* The target the window shows: the buffer sceDisplaySetFrameBuf named, not
 * the last one the GE drew into. The 3rd Birthday plays its movies by writing
 * each decoded picture straight into the display buffer and drawing nothing there
 * with the GE, so composing the GE's last target showed every movie as black
 * (run 157: 598 frames presented, the shown buffer never a GE target). A
 * displayed buffer the GE never configured gets a target shaped by the
 * display's own stride and format, every pixel imported from guest memory;
 * from then on the write observer keeps it current like any other target,
 * and a GE draw into it later reconfigures it the usual way. */
static int rt_shown(void) {
    if (!g.disp_addr) return g.cur_rt;
    const int i = rt_for(g.disp_addr);
    rendertarget *r = &g.rts[i];
    if (r->addr != (g.disp_addr & PSP_ADDR_MASK)) return g.cur_rt;   /* the table is full */
    if (!r->configured) {
        const int stride = g.disp_stride ? (int)g.disp_stride : g.w;
        if (rt_prepare_shape(i, stride, g.disp_fmt, stride, g.h, 0) != 0) return g.cur_rt;
        memset(r->cpu_dirty, (r->fmt == 3 ? 0xf : 0x3), (size_t)r->stride * r->guest_h);
        r->cpu_pending = 1;
        g.display_targets++;
    }
    return i;
}

static void rt_import(rendertarget *r) {
    if (!r->cpu_pending || !r->configured) return;
    g.mu.disturbed = 1;
    stencil_to_alpha(r);
    uint32_t *row = malloc((size_t)r->w * 4), *values = malloc((size_t)r->stride * 4);
    if (!row || !values) {
        free(row);
        free(values);
        g.failed = 1;
        return;
    }
    p_glBindTexture(GL_TEXTURE_2D, r->colour);
    for (int y = 0; y < r->guest_h; y++) {
        int gy0 = pixel_edge(y * r->sy), gy1 = pixel_edge((y + 1) * r->sy);
        for (int x = 0; x < (int)r->stride;) {
            if (!r->cpu_dirty[(size_t)y * r->stride + x]) {
                x++;
                continue;
            }
            const int start = x;
            while (x < (int)r->stride && r->cpu_dirty[(size_t)y * r->stride + x]) x++;
            int left = pixel_edge(start * r->sx), right = pixel_edge(x * r->sx);
            if (right > r->w) right = r->w;
            if (gy1 > r->h) gy1 = r->h;
            if (right <= left || gy1 <= gy0) continue;
            const int bpp = r->fmt == 3 ? 4 : 2;
            const unsigned full = (1u << bpp) - 1;
            int partial = 0;
            for (int gx = start; gx < x; gx++) {
                values[gx] = guest_pixel(r, gx, y);
                if (r->cpu_dirty[(size_t)y * r->stride + gx] != full) partial = 1;
            }
            /* A byte write owns only those channels. Preserve every physical
             * sample of the others, not just one resolved PSP pixel -- an
             * alpha-only CPU write must not erase a fine RGB edge. Full-pixel
             * uploads keep the fast path with no GPU read. */
            uint32_t *previous = NULL;
            if (partial) {
                previous = malloc((size_t)(right - left) * (gy1 - gy0) * 4);
                if (!previous) {
                    free(row);
                    free(values);
                    g.failed = 1;
                    return;
                }
                p_glBindFramebuffer(GL_READ_FRAMEBUFFER, r->fbo);
                p_glReadPixels(left, r->h - gy1, right - left, gy1 - gy0, GL_RGBA, GL_UNSIGNED_BYTE,
                               previous);
            }
            for (int py = gy0; py < gy1; py++) {
                for (int k = left; k < right; k++) {
                    const int gx = (int)((k + 0.5) / r->sx);
                    const unsigned mask = r->cpu_dirty[(size_t)y * r->stride + gx];
                    uint32_t value = values[gx];
                    if (mask != full) {
                        uint32_t old = pack_pixel(
                            previous[(size_t)(gy1 - 1 - py) * (right - left) + k - left], r->fmt);
                        uint32_t cpu = pack_pixel(value, r->fmt), bits = 0;
                        for (int b = 0; b < bpp; b++)
                            if (mask & (1u << b)) bits |= 255u << (b * 8);
                        value = decode_pixel((old & ~bits) | (cpu & bits), r->fmt);
                    }
                    row[k - left] = value;
                }
                p_glTexSubImage2D(GL_TEXTURE_2D, 0, left, r->h - 1 - py, right - left, 1, GL_RGBA,
                                  GL_UNSIGNED_BYTE, row);
            }
            free(previous);
        }
    }
    free(row);
    free(values);
    memset(r->cpu_dirty, 0, (size_t)r->stride * r->guest_h);
    r->cpu_pending = 0;
    r->stencil_valid = 0;
    r->dirty = 1;
    g.cpu_uploads++;
}

static void rt_resize_all(void) {
    for (int i = 0; i < g.n_rts; i++) {
        rendertarget *r = &g.rts[i];
        if (!r->configured || !r->display) continue;
        rendertarget next = *r;
        rt_layout(&next);
        if (r->w == next.w && r->h == next.h && r->wide_w == next.wide_w && r->wide_h == next.wide_h &&
            r->visible_w == next.visible_w && r->visible_h == next.visible_h)
            continue;
        rt_import(r);
        stencil_to_alpha(r);
        next.fbo = next.colour = next.depth = 0;
        next.cpu_dirty = NULL;
        if (rt_allocate(&next, 0) != 0) continue; /* keep the working allocation */
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, r->fbo);
        p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, next.fbo);
        p_glDisable(GL_SCISSOR_TEST);
        p_glBlitFramebuffer(0, 0, r->w, r->h, 0, 0, next.w, next.h,
                            GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT,
                            GL_NEAREST);
        next.stencil_valid = r->stencil_valid;
        next.alpha_dirty = 0;
        next.cpu_pending = 0;
        next.dirty = r->dirty;
        rt_release(r);
        *r = next;
        g.resizes++;
        g.wide_retired++;
    }
}

/* Let go of the context on the host thread holding it: when another thread
 * needs it (claim), and when this one's guest thread ends (the scheduler's
 * host exit hook). A context left current on a thread that has gone cannot
 * be made current again under X11 (BadAccess), nor reliably under EGL. */
static void let_go(void) {
    if (g.ready && g.thread && g.thread == this_thread()) {
        present_gl_release();
        g.thread = 0;
    }
}

/* Claim the context on whichever thread the GE is on. Every entry point goes
 * through here, so a call arriving on another thread is caught at the
 * boundary rather than as corruption inside the driver. The context follows
 * the GE when it moves: WipEout Pulse draws its first lists on the thread
 * that boots it and every later one on a render thread of its own. The
 * thread holding it is parked, waiting for its turn, while another runs, so
 * it is asked to let go there (psp_sched_run_on); one whose guest thread has
 * ended let go as it went. */
static int claim(void) {
    const unsigned long me = this_thread();
    if (g.ready) {
        if (g.thread == me) return 0;
        if (g.thread) psp_sched_run_on(g.uid, let_go);
        if (g.thread || present_gl_make_current() != 0) {
            static int said;
            if (!said++)
                fprintf(stderr, "gl: the GE reached this backend on host thread %lu, and the "
                                "context could not be moved from %lu; refusing rather than "
                                "drawing through a context that is not current.\n", me, g.thread);
            return -1;
        }
        g.thread = me;
        g.uid = psp_sched_current();
        if (++g.moves <= 4)
            fprintf(stderr, "gl: the context followed the GE to host thread %lu\n", me);
        return 0;
    }
    if (g.failed) return -1;

    if (present_gl_make_current() != 0 || gl_load() != 0) { g.failed = 1; return -1; }
    g.thread = me;
    g.uid = psp_sched_current();
    GLint tex_limit, rb_limit, viewport_limit[2];
    p_glGetIntegerv(GL_MAX_TEXTURE_SIZE, &tex_limit);
    p_glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &rb_limit);
    p_glGetIntegerv(GL_MAX_VIEWPORT_DIMS, viewport_limit);
    g.max_size = tex_limit < rb_limit ? tex_limit : rb_limit;
    if (viewport_limit[0] < g.max_size) g.max_size = viewport_limit[0];
    if (viewport_limit[1] < g.max_size) g.max_size = viewport_limit[1];

    if (build_program() != 0) { g.failed = 1; return -1; }
    if (build_stencil_program() != 0) { g.failed = 1; return -1; }
    /* The model program needs GLSL 4.0; without it the CPU path stands. */
    {
        const char *e = getenv("PSPRECOMP_GL_TRANSFORM");
        if (e && strcmp(e, "cpu") == 0) g.model_unavailable = 1;
        else if (build_model_program() != 0) { g.model_unavailable = 1; fprintf(stderr, "gl: model program unavailable, transforming on the CPU\n"); }
        else {
            p_glGenVertexArrays(1, &g.vao_model);
            p_glBindVertexArray(g.vao_model);
            ring_create(RING_VBO, GL_ARRAY_BUFFER, (size_t)4 * MODEL_RING_VERTS * sizeof(psp_model_vertex));
            g.vbo_model = g_rings.r[RING_VBO].buf;
            g.mu.vbo_head = 0;
            p_glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &g.mu.ubo_align);
            if (g.mu.ubo_align < 16) g.mu.ubo_align = 16;
            g.mu.ubo_slot_bytes = (int)((XFORM_MAXB * sizeof(xform_block) + g.mu.ubo_align - 1) / g.mu.ubo_align * g.mu.ubo_align);
            ring_create(RING_UBO, GL_UNIFORM_BUFFER, (size_t)4 * 1024 * 1024);
            g.mu.ubo = g_rings.r[RING_UBO].buf;
            g.mu.ubo_slot = 0;
            p_glBindBuffer(GL_ARRAY_BUFFER, g.vbo_model);
            p_glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(psp_model_vertex), (void *)offsetof(psp_model_vertex, pos));
            p_glEnableVertexAttribArray(0);
            p_glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(psp_model_vertex), (void *)offsetof(psp_model_vertex, nrm));
            p_glEnableVertexAttribArray(1);
            p_glVertexAttribIPointer(2, 1, GL_UNSIGNED_INT, sizeof(psp_model_vertex), (void *)offsetof(psp_model_vertex, rgba));
            p_glEnableVertexAttribArray(2);
            p_glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(psp_model_vertex), (void *)offsetof(psp_model_vertex, u));
            p_glEnableVertexAttribArray(3);
            ring_create(RING_DRAW, GL_ARRAY_BUFFER, (size_t)4 * MODEL_RING_VERTS * sizeof(GLuint));
            g.mu.vbo_draw = g_rings.r[RING_DRAW].buf;
            p_glVertexAttribIPointer(4, 1, GL_UNSIGNED_INT, sizeof(GLuint), (void *)0);
            p_glEnableVertexAttribArray(4);
            /* The triangle indices of every draw are a prefix of one of three
             * patterns -- a list counts up, a strip is (t, t+1, t+2), a fan
             * (0, t+1, t+2) -- so they live in one static buffer and a draw
             * names its pattern and length. Nothing is written per batch. */
            {
                enum { NPAT = MODEL_MAX_VERTS + 2 * 3 * (MODEL_MAX_VERTS - 2) };
                static GLushort pat[NPAT]; int k = 0;
                for (int t = 0; t < MODEL_MAX_VERTS; t++) pat[k++] = (GLushort)t;
                for (int t = 0; t + 2 < MODEL_MAX_VERTS; t++) { pat[k++] = (GLushort)t; pat[k++] = (GLushort)(t + 1); pat[k++] = (GLushort)(t + 2); }
                for (int t = 0; t + 2 < MODEL_MAX_VERTS; t++) { pat[k++] = 0; pat[k++] = (GLushort)(t + 1); pat[k++] = (GLushort)(t + 2); }
                p_glGenBuffers(1, &g.mu.ebo_ring);
                p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.mu.ebo_ring);
                p_glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)((size_t)k * sizeof(GLushort)), pat, GL_STATIC_DRAW);
            }
            g.mu.profile = getenv("PSPRECOMP_GL_PROFILE") != NULL;
            p_glBindVertexArray(0);
            {
                /* Asynchronous unless asked otherwise: the pixels land in guest
                 * memory at the next present, or earlier the moment anything
                 * reaches for them (rt_guest_access). PSPRECOMP_GL_READBACK=sync
                 * keeps the glReadPixels in the present. */
                const char *rb = getenv("PSPRECOMP_GL_READBACK");
                if (!(rb && strcmp(rb, "sync") == 0) && g_rings.persistent) {
                    const GLbitfield flags = GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
                    p_glGenBuffers(1, &g_rb.pbo);
                    p_glBindBuffer(GL_PIXEL_PACK_BUFFER, g_rb.pbo);
                    p_glBufferStorage(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)(RB_SLOTS * RB_SLOT_BYTES), NULL, flags);
                    g_rb.map = (uint8_t *)p_glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)(RB_SLOTS * RB_SLOT_BYTES), flags);
                    p_glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
                    g_rb.async = g_rb.map != NULL;
                }
            }
        }
    }

    /* Desktop GL starts with dithering enabled. The backend contract is the
     * currently undithered software GE, so an implicit host dither pattern is
     * never valid state and turns exact RGBA8 arithmetic back into LSB noise. */
    p_glDisable(GL_DITHER);

    p_glGenVertexArrays(1, &g.vao);
    p_glBindVertexArray(g.vao);
    ring_create(RING_BATCH, GL_ARRAY_BUFFER, (size_t)4 * GL_MAX_VERTS * FLOATS_PER_VERT * sizeof(float));
    g.vbo = g_rings.r[RING_BATCH].buf;
    p_glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                            FLOATS_PER_VERT * sizeof(float), (void *)0);
    p_glEnableVertexAttribArray(0);
    p_glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE,
                            FLOATS_PER_VERT * sizeof(float),
                            (void *)(3 * sizeof(float)));
    p_glEnableVertexAttribArray(1);
    p_glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE,
                            FLOATS_PER_VERT * sizeof(float),
                            (void *)(7 * sizeof(float)));
    p_glEnableVertexAttribArray(2);
    p_glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE,
                            FLOATS_PER_VERT * sizeof(float),
                            (void *)(9 * sizeof(float)));
    p_glEnableVertexAttribArray(3);
    p_glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE,
                            FLOATS_PER_VERT * sizeof(float),
                            (void *)(10 * sizeof(float)));
    p_glEnableVertexAttribArray(4);
    p_glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE,
                            FLOATS_PER_VERT * sizeof(float),
                            (void *)(11 * sizeof(float)));
    p_glEnableVertexAttribArray(5);
    p_glVertexAttribPointer(6, 1, GL_FLOAT, GL_FALSE,
                            FLOATS_PER_VERT * sizeof(float),
                            (void *)(12 * sizeof(float)));
    p_glEnableVertexAttribArray(6);

    g.batch = calloc(GL_MAX_VERTS * FLOATS_PER_VERT, sizeof(float));
    if (!g.batch) { g.failed = 1; return -1; }
    p_glGenQueries(GPU_QUERY_RING, g.gpu_query);

    fprintf(stderr, "gl: context claimed on thread %lu, %dx%d target\n",
            me, g.w, g.h);
    const GLubyte *(APIENTRY *get_string)(GLenum) = present_gl_proc("glGetString");
    const GLubyte *renderer = get_string ? get_string(GL_RENDERER) : NULL;
    fprintf(stderr, "gl: renderer %s\n", renderer ? (const char *)renderer : "unknown");
    const GLubyte *vendor = get_string ? get_string(GL_VENDOR) : NULL;
    const GLubyte *version = get_string ? get_string(GL_VERSION) : NULL;
    fprintf(stderr, "gl: vendor %s\ngl: version %s\n",
            vendor ? (const char *)vendor : "unknown",
            version ? (const char *)version : "unknown");
    g.ready = 1;
    resolution_size();
    psp_mem_set_write_observer(rt_guest_write);
    rt_watch_writes();
    /* Whatever the GE last named, or the primary display buffer if it has not
     * named one yet -- a target has to exist before the first draw. */
    g.cur_rt = rt_for(g.target_addr ? g.target_addr : 0x04000000u);
    return 0;
}

/* ---- the interface ---------------------------------------------------------- */
static int gl_dialog_redraw(void);
static void gl_pause_redraw(void);

static int gl_init(int w, int h) {
    /* No GL here on purpose: this runs on boot.c's thread, not the GE's. */
    g.w = w; g.h = h;
    g.wide_w = w; g.wide_h = h;
    g.pixel_w = w; g.pixel_h = h;
    g.resolution = render_gl_resolution_mode();
    if (g.resolution < 0) return -1;
    g.adaptive_aspect = present_adaptive_aspect();
    g.smooth_bloom = psp_title_info.bloom && setting_number("BLOOM_FILTER") != 0;
    psp_savedata_set_redraw(gl_dialog_redraw);
    psp_pause_set_redraw(gl_pause_redraw);
    psp_sched_set_host_exit_hook(let_go);
    return 0;
}

/* Before a save state: every target drawn into, back in guest memory. A
 * frame the guest presented is there already; a target only sampled later,
 * or not at all, may not be. */
static void gl_to_memory(void) {
    if (!g.ready) return;
    flush();
    for (int i = 0; i < g.n_rts; i++) readback_rt(i);
}

static void gl_shutdown(void) {
    psp_savedata_set_redraw(NULL);
    psp_pause_set_redraw(NULL);
    if (g_rb.async) readback_complete(-1);
    psp_mem_set_vram_access_observer(NULL, 0, 0);
    psp_mem_set_write_observer(NULL);
}

static void gl_target(uint32_t addr, uint32_t stride, int fmt) {
    g.mu.state_gen++;
    /* No claim() here: set_target is the one setter ge.c calls while the
     * register is still being assembled, long before any drawing, and on a
     * run that never draws it would otherwise create a context for nothing. */
    if (g.ready) { flush_ends(FB_TARGET); flush(); }
    g.target_addr = addr; g.target_stride = stride; g.target_fmt = fmt;
    if (g.ready && addr) {
        g.cur_rt = rt_for(addr);
    }
}

/* sceDisplaySetFrameBuf, just before present(): only recorded here, since
 * the present that follows runs on the GL thread and does the work. */
static void gl_display(uint32_t addr, uint32_t stride, int fmt) {
    g.disp_addr = addr; g.disp_stride = stride; g.disp_fmt = fmt;
}

/* Every setter flushes what is pending before recording. A state change
 * applies from the next primitive onward; without the flush it would apply
 * retroactively to geometry already sitting in the batch. */
static void gl_scissor(int x0, int y0, int x1, int y1) {
    if (claim() != 0) return;
    if (g.sc_valid && g.sc_x0 == x0 && g.sc_y0 == y0 && g.sc_x1 == x1 && g.sc_y1 == y1) { g.mu.setters_skipped++; return; }
    g.mu.state_gen++;
    flush_ends(FB_SCISSOR); flush();
    g.sc_x0 = x0; g.sc_y0 = y0; g.sc_x1 = x1; g.sc_y1 = y1; g.sc_valid = 1;
}

static void gl_viewport(float x, float y, float w, float h) {
    /* Metadata only. Classification at the draw flushes if placement changes;
     * a pending batch never consults this rectangle during its later flush. */
    g.viewport_x = x; g.viewport_y = y; g.viewport_w = w; g.viewport_h = h;
}

static int menu_preview(const rendertarget *r) {
    /* Assembly's part/AC cameras occupy inset viewports on the display, with
     * scissors matching their rectangles (half-pixel differences for odd
     * dimensions). They project at the panel's aspect, so scene expansion
     * would stretch and displace them. Require the matching scissor as well
     * as an inset viewport; a scissored full-screen camera remains SCENE.
     * Scratch targets never participate in the wide-display mapping. */
    const float x = g.viewport_x, y = g.viewport_y;
    const float w = g.viewport_w, h = g.viewport_h;
    return r->wide && g.sc_valid && x >= 0 && y >= 0 &&
           w > 0 && w < g.w && h > 0 && h < g.h &&
           x + w <= g.w && y + h <= g.h &&
           fabsf(g.sc_x0 - x) <= 1 && fabsf(g.sc_y0 - y) <= 1 &&
           fabsf(g.sc_x1 + 1 - (x + w)) <= 1 &&
           fabsf(g.sc_y1 + 1 - (y + h)) <= 1;
}
/* Only a mipmap minification filter consumes the extra levels. TEXMODE may
 * retain a non-zero top while a draw deliberately selects ordinary nearest or
 * linear filtering; uploading those unreachable levels would make a sampling
 * state change invalidate an otherwise identical cache entry for no result. */
static int texture_top(const psp_tex_state *t) {
    if (t->min_filter < 4 || t->max_level <= 0) return 0;
    return t->max_level > 7 ? 7 : t->max_level;
}

static uint32_t level_addr(const psp_tex_state *t, int level) {
    return level ? t->lv_addr[level] : t->addr;
}
static uint32_t level_stride(const psp_tex_state *t, int level) {
    return level ? t->lv_stride[level] : t->stride;
}
static int level_w(const psp_tex_state *t, int level) {
    return level ? t->lv_w[level] : t->w;
}
static int level_h(const psp_tex_state *t, int level) {
    return level ? t->lv_h[level] : t->h;
}

/* Bytes the shared decoder can touch for one level. A swizzled texture is laid
 * out in eight-row blocks, so a short final block occupies its padded height;
 * tracking only visible rows would miss a write to a texel the swizzle maps
 * beyond stride*height. */
static uint32_t level_bytes(const psp_tex_state *t, int level) {
    static const int halfbytes[8] = { 4, 4, 4, 8, 1, 2, 4, 8 };
    const int fmt = t->fmt;
    const uint32_t stride = level_stride(t, level);
    const int h = level_h(t, level);
    if (fmt < 0 || fmt >= 8 || !stride || h <= 0) return 0;
    const uint64_t row = ((uint64_t)stride * (uint64_t)halfbytes[fmt]) / 2u;
    const uint64_t rows = t->swizzled && row >= 16u
                        ? (uint64_t)(h + 7) & ~UINT64_C(7)
                        : (uint64_t)h;
    const uint64_t bytes = row * rows;
    return bytes > UINT32_MAX ? UINT32_MAX : (uint32_t)bytes;
}

/* The newest write generation under the texture's bytes. `supplied` is a
 * bit per render target whose part of the texture texcache_get is going to
 * blit in from the GPU: the decoded copy of those bytes is never seen, so
 * their changes -- every frame, for a display buffer sampled as a texture
 * -- must not invalidate the entry. Every other byte counts, including a
 * target the texture overlaps that the view cannot supply (a 16-bit
 * off-screen target under a 512-row display texture, say): that one is
 * read back into guest memory and must reach the decoded copy. */
static uint64_t range_generation_excluding_rts(uint32_t addr, uint32_t size, uint32_t supplied) {
    /* GPU allocations retain the declared texture extent, including unbacked
     * padding; the runtime answers for the mapped part of such a range and
     * skips the rest. Metadata only, so generation checks cannot cause
     * framebuffer readback. */
    addr &= PSP_ADDR_MASK;
    if (!size) return 0;
    if (!supplied) return psp_mem_range_generation(addr, size);
    uint64_t pieces[2 * RT_MAX + 2][2]; int n = 1;
    pieces[0][0] = addr; pieces[0][1] = (uint64_t)addr + size;
    for (int i = 0; i < g.n_rts; i++) {
        rendertarget *r = &g.rts[i];
        if (!r->configured || !(supplied & (1u << i))) continue;
        const uint64_t ra = r->addr, rb = (uint64_t)r->addr + (uint64_t)r->stride * r->guest_h * (r->fmt == 3 ? 4 : 2);
        for (int k = 0; k < n; k++) {
            const uint64_t a = pieces[k][0], b = pieces[k][1];
            if (rb <= a || ra >= b) continue;
            /* cut [ra,rb) out of [a,b): keep the left and right remainders */
            pieces[k][1] = ra > a ? ra : a;
            if (rb < b && n < 2 * RT_MAX + 2) { pieces[n][0] = rb; pieces[n][1] = b; n++; }
        }
    }
    uint64_t newest = 0;
    for (int k = 0; k < n; k++) {
        if (pieces[k][1] <= pieces[k][0]) continue;
        const uint64_t gen = psp_mem_range_generation((uint32_t)pieces[k][0], (uint32_t)(pieces[k][1] - pieces[k][0]));
        if (gen > newest) newest = gen;
    }
    return newest;
}
static uint64_t texture_generation(const psp_tex_state *t, int top, uint32_t supplied) {
    uint64_t newest = 0;
    for (int level = 0; level <= top; level++) {
        const uint64_t gen = range_generation_excluding_rts(level_addr(t, level),
                                                             level_bytes(t, level), supplied);
        if (gen > newest) newest = gen;
    }
    if (t->fmt >= 4 && t->fmt <= 7 && g.clut_addr) {
        /* CLUT start is already in entries (multiples of sixteen), and mask is
         * applied before ORing it. Tracking through their greatest possible
         * index covers every palette read without knowing the texel values. */
        const uint32_t entries = (uint32_t)(g.clut_start | g.clut_mask) + 1u;
        const uint32_t bytes = entries * (g.clut_fmt == 3 ? 4u : 2u);
        const uint64_t gen = range_generation_excluding_rts(g.clut_addr, bytes, 0);
        if (gen > newest) newest = gen;
    }
    return newest;
}

static uint32_t hash_word(uint32_t hash, uint32_t word) {
    return (hash ^ word) * UINT32_C(16777619);
}

static size_t texture_slot(const psp_tex_state *t, int top) {
    uint32_t hash = UINT32_C(2166136261);
    hash = hash_word(hash, (uint32_t)t->fmt);
    hash = hash_word(hash, (uint32_t)t->swizzled);
    hash = hash_word(hash, (uint32_t)top);
    for (int level = 0; level <= top; level++) {
        hash = hash_word(hash, level_addr(t, level));
        hash = hash_word(hash, level_stride(t, level));
        hash = hash_word(hash, (uint32_t)level_w(t, level));
        hash = hash_word(hash, (uint32_t)level_h(t, level));
    }
    if (t->fmt >= 4 && t->fmt <= 7) {
        hash = hash_word(hash, g.clut_addr);
        hash = hash_word(hash, (uint32_t)g.clut_fmt);
        hash = hash_word(hash, (uint32_t)g.clut_shift);
        hash = hash_word(hash, (uint32_t)g.clut_mask);
        hash = hash_word(hash, (uint32_t)g.clut_start);
    }
    return (size_t)hash % TEXCACHE_MAX;
}

static int cache_matches(const texcache_entry *e, const psp_tex_state *t,
                         int top) {
    if (!e->used || e->fmt != t->fmt || e->swizzled != t->swizzled ||
        e->max_level != top)
        return 0;
    /* Palette state has no bearing on direct-colour formats. Including the
     * GE's incidental last CLUT there multiplied identical cache entries and
     * was responsible for most of the mission's post-generation evictions. */
    if (t->fmt >= 4 && t->fmt <= 7 &&
        (e->clut_addr != g.clut_addr || e->clut_fmt != g.clut_fmt ||
         e->clut_shift != g.clut_shift || e->clut_mask != g.clut_mask ||
         e->clut_start != g.clut_start))
        return 0;
    for (int level = 0; level <= top; level++)
        if (e->lv_addr[level] != level_addr(t, level) ||
            e->lv_stride[level] != level_stride(t, level) ||
            e->lv_w[level] != level_w(t, level) ||
            e->lv_h[level] != level_h(t, level))
            return 0;
    return 1;
}

static void cache_record(texcache_entry *e, const psp_tex_state *t, int top,
                         int uploaded_top, uint64_t generation,
                         uint64_t serial) {
    if (!e->used) g.cache_entries++;
    e->used = 1;
    e->addr = t->addr; e->stride = t->stride; e->w = t->w; e->h = t->h;
    e->fmt = t->fmt; e->swizzled = t->swizzled;
    e->max_level = top; e->uploaded_top = uploaded_top;
    for (int level = 0; level <= top; level++) {
        e->lv_addr[level] = level_addr(t, level);
        e->lv_stride[level] = level_stride(t, level);
        e->lv_w[level] = level_w(t, level);
        e->lv_h[level] = level_h(t, level);
    }
    e->clut_addr = g.clut_addr; e->clut_fmt = g.clut_fmt;
    e->clut_shift = g.clut_shift; e->clut_mask = g.clut_mask;
    e->clut_start = g.clut_start;
    e->content_generation = generation;
    e->validated_serial = serial;
    e->last_used = g.cache_clock;
}

/* A cached texture whose bytes changed under it is re-decoded only where
 * they changed: the rows whose write generation moved past the entry's,
 * merged across short gaps and sent with glTexSubImage2D. The display
 * buffer sampled as a 512-row texture is the case: an off-screen target
 * read back under its lower rows used to cost the whole megabyte again.
 * Direct-colour, unswizzled, single-level textures only -- a palette
 * change or a swizzle is not a row. Returns 0 to ask for the full upload,
 * which then overwrites whatever rows this may already have refreshed. */
static int texcache_refresh_rows(texcache_entry *e, const psp_tex_state *t, uint32_t supplied,
                                 uint64_t generation) {
    if (texture_top(t) != 0 || e->uploaded_top != 0 || t->swizzled || t->fmt > 3 || !e->tex) return 0;
    if (t->w <= 0 || t->h <= 0) return 0;
    const int bpp = t->fmt == 3 ? 4 : 2;
    const uint32_t row_bytes = (uint32_t)t->stride * (uint32_t)bpp;
    if (!row_bytes || (size_t)t->w * (size_t)t->h > TEXEL_CAP) return 0;
    const uint32_t base = level_addr(t, 0);
    static uint32_t *texels;
    if (!texels) texels = malloc(TEXEL_CAP * sizeof(uint32_t));
    if (!texels) return 0;
    const psp_clut_state clut = { g.clut_addr, g.clut_fmt, g.clut_shift,
                                  g.clut_mask, g.clut_start };
    enum { GAP = 16, RUNS_MAX = 16 };
    int runs = 0, bound = 0;
    for (int y = 0; y < t->h;) {
        if (range_generation_excluding_rts(base + (uint32_t)y * row_bytes, row_bytes, supplied) <= e->content_generation) { y++; continue; }
        int y1 = y + 1, last = y;
        while (y1 < t->h && y1 - last <= GAP) {
            if (range_generation_excluding_rts(base + (uint32_t)y1 * row_bytes, row_bytes, supplied) > e->content_generation) last = y1;
            y1++;
        }
        y1 = last + 1;
        if (++runs > RUNS_MAX) return 0;
        psp_tex_state sub = *t;
        sub.addr = t->addr + (uint32_t)y * row_bytes;
        sub.h = y1 - y;
        sub.lv_addr[0] = sub.addr; sub.lv_h[0] = sub.h;
        int dw = 0, dh = 0;
        const uint64_t decode_t0 = psp_os_mono_ns();
        size_t padded = 0;
        const size_t decoded = psp_render_decode_level_padded(&sub, 0, &clut, texels,
                                                               TEXEL_CAP, &dw, &dh, &padded);
        g.tex_padded_pixels += padded;
        g.tex_padded_uploads += padded != 0;
        g.tex_decode_ns += psp_os_mono_ns() - decode_t0;
        if (!decoded || dw != t->w || dh != y1 - y) return 0;
        if (!bound) { p_glBindTexture(GL_TEXTURE_2D, e->tex); bound = 1; }
        const uint64_t upload_t0 = psp_os_mono_ns();
        p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, dw, dh, GL_RGBA, GL_UNSIGNED_BYTE, texels);
        g.tex_upload_ns += psp_os_mono_ns() - upload_t0;
        g.tex_upload_pixels += decoded;
        g.tex_rows_refreshed += (uint64_t)dh;
        y = y1;
    }
    e->content_generation = generation;
    e->validated_serial = psp_mem_write_serial();
    g.tex_row_refreshes++;
    return 1;
}

static GLuint texcache_native(const psp_tex_state *t, uint32_t supplied) {
    if (!t->addr || t->w <= 0 || t->h <= 0) return 0;
    g.tex_requests++;
    const int top = texture_top(t);
    /* A target can only be sampled directly when the texture describes the
     * same pixel layout.  The AC scratch surface is the important opposite
     * case: it is rendered as 5551, then deliberately read as CLUT8 through a
     * palette.  Returning its RGBA attachment used to skip that byte-level
     * reinterpretation and produced the bright menu shards and mirrored
     * mission foreground.  Synchronise such aliases to guest memory and send
     * them through the common decoder instead. */
    for (int i = 0; i < g.n_rts; i++) {
        if (g.rts[i].used && g.rts[i].addr == t->addr) {
            g.tex_from_rt++;
            rendertarget *r = &g.rts[i];
            if (!g.resolution && r->configured && r->fmt == 3 && t->fmt == 3 &&
                !t->swizzled && t->stride == r->stride &&
                t->w == r->w && t->h == r->h && top == 0) {
                g.bound_top = 0;
                stencil_to_alpha(r);
                return r->colour;
            }
            if (r->dirty && !(supplied & (1u << i))) {
                readback_rt(i);
                r->dirty = 0;
            }
            g.tex_alias_from_rt++;
            break;
        }
    }
    /* A lower mip can independently alias a render target. Synchronise every
     * level the sampler can reach before decoding; checking only level zero
     * leaves a perfectly keyed cache holding yesterday's generated mip. */
    for (int level = 1; level <= top; level++) {
        const uint32_t addr = level_addr(t, level);
        for (int i = 0; addr && i < g.n_rts; i++) {
            rendertarget *r = &g.rts[i];
            if (!r->used || r->addr != addr) continue;
            g.tex_from_rt++;
            if (r->dirty) { readback_rt(i); r->dirty = 0; }
            g.tex_alias_from_rt++;
            break;
        }
    }
    for (int level = 0; level <= top; level++)
        if (level_w(t, level) <= 0 || level_h(t, level) <= 0 ||
            (size_t)level_w(t, level) * (size_t)level_h(t, level) > TEXEL_CAP) {
            g.tex_too_big++;
            return 0;
        }

    size_t slot = texture_slot(t, top);

    /* Textures in VRAM used to skip this lookup altogether. That avoided a
     * stale render-to-texture result, but decoded and uploaded every binding --
     * 232,297 times in the first full mission measurement. Write generations
     * let VRAM use the same cache while preserving the in-place update. */
    int in_vram = 0;
    for (int level = 0; level <= top; level++)
        if ((level_addr(t, level) & 0xFF000000u) == 0x04000000u)
            in_vram = 1;

    const uint64_t memory_serial = psp_mem_write_serial();
    g.cache_clock++;
    size_t victim = slot;
    uint64_t oldest = UINT64_MAX;
    for (size_t probe = 0; probe < TEXCACHE_PROBES; probe++) {
        const size_t at = (slot + probe) % TEXCACHE_MAX;
        texcache_entry *e = &g.cache[at];
        if (cache_matches(e, t, top)) {
            if (e->validated_serial == memory_serial) {
                g.tex_fast_hits++;
            } else {
                const uint64_t gen_t0 = psp_os_mono_ns();
                const uint64_t generation = texture_generation(t, top, supplied);
                g.tex_generation_ns += psp_os_mono_ns() - gen_t0;
                if (generation != e->content_generation) {
                    g.tex_invalidations++;
                    if (!texcache_refresh_rows(e, t, supplied, generation)) {
                        slot = at;
                        goto upload;
                    }
                } else {
                    e->validated_serial = memory_serial;
                    g.tex_revalidated++;
                }
            }
            g.tex_hits++;
            e->last_used = g.cache_clock;
            g.bound_top = e->uploaded_top;
            return e->tex;
        }
        if (!e->used) { slot = (slot + probe) % TEXCACHE_MAX; goto upload; }
        if (e->last_used < oldest) { oldest = e->last_used; victim = at; }
    }
    /* The bounded probe window keeps lookup cost predictable. If it fills,
     * retain the hot entries instead of repeatedly replacing the hash's first
     * slot; the eviction counter says whether 512 entries/32 probes suffices. */
    slot = victim;
    g.tex_evictions++;
upload: {
    texcache_entry *e = &g.cache[slot];
    if (!cache_matches(e, t, top)) g.tex_misses++;
    static uint32_t *texels;
    if (!texels) texels = malloc(TEXEL_CAP * sizeof(uint32_t));
    if (!texels) return 0;

    /* One decoder for every backend -- the formats, the CLUT paging and the
     * swizzle are the runtime's, not repeated here. */
    const psp_clut_state clut = { g.clut_addr, g.clut_fmt, g.clut_shift,
                                  g.clut_mask, g.clut_start };
    if (!e->tex) p_glGenTextures(1, &e->tex);
    p_glBindTexture(GL_TEXTURE_2D, e->tex);
    int uploaded_top = -1;
    for (int level = 0; level <= top; level++) {
        int dw = 0, dh = 0;
        const uint64_t decode_t0 = psp_os_mono_ns();
        size_t padded = 0;
        const size_t decoded = psp_render_decode_level_padded(t, level, &clut,
                                                       texels, TEXEL_CAP,
                                                       &dw, &dh, &padded);
        g.tex_padded_pixels += padded;
        g.tex_padded_uploads += padded != 0;
        g.tex_decode_ns += psp_os_mono_ns() - decode_t0;
        if (decoded == 0)
            break;
        const uint64_t upload_t0 = psp_os_mono_ns();
        p_glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA8, dw, dh, 0,
                       GL_RGBA, GL_UNSIGNED_BYTE, texels);
        g.tex_upload_ns += psp_os_mono_ns() - upload_t0;
        g.tex_upload_pixels += decoded;
        uploaded_top = level;
        if (level) g.mip_levels++;
    }
    if (uploaded_top < 0) return 0;
    if (uploaded_top < top) g.mip_incomplete++;
    if (uploaded_top > 0) g.mip_chains++;
    /* texelFetch performs the PSP's filtering in the shader. Keeping the GL
     * sampler itself non-mipmapped also permits the PSP's independently-sized
     * levels without making the texture incomplete under GL's stricter
     * halving rule. */
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, uploaded_top);
    const uint64_t gen_t0 = psp_os_mono_ns();
    const uint64_t generation = texture_generation(t, top, supplied);
    g.tex_generation_ns += psp_os_mono_ns() - gen_t0;
    cache_record(e, t, top, uploaded_top, generation,
                 psp_mem_write_serial());
    g.bound_top = uploaded_top;
    g.tex_uploads++;
    if (in_vram) g.tex_vram_uploads++;
    return e->tex;
}
}

/* A view has the texture's declared extent, populated from guest bytes and
 * overlaid with all compatible GPU-owned rows. Its top row is texture row 0,
 * unlike a render attachment. Snapshotting also makes self-composites safe. */
static GLuint texcache_get(const psp_tex_state *t) {
    g.tex_sx = g.tex_sy = 1;
    int hit = 0;
    uint32_t supplied = 0;
    double sx = 1, sy = 1;
    const uint32_t base = t->addr & PSP_ADDR_MASK;
    const uint64_t end = (uint64_t)base + (uint64_t)t->stride * t->h * (t->fmt == 3 ? 4 : 2);
    /* In either mode: at native the scale is one and the view is a copy of
     * the target's rows under the texture, which is what a texture over a
     * 272-row target and 240 rows of whatever follows it needs. The one case
     * texcache_native serves better -- a texture that is exactly the target,
     * at native -- is left to it, and samples the attachment itself. */
    for (int i = 0; i < g.n_rts; i++) {
            rendertarget *r = &g.rts[i];
            const uint64_t re =
                (uint64_t)r->addr + (uint64_t)r->stride * r->guest_h * (r->fmt == 3 ? 4 : 2);
            if (!r->configured || base >= re || end <= r->addr) continue;
            const int exact_alias = !g.resolution && r->addr == base && r->fmt == 3 && t->fmt == 3 &&
                                    !t->swizzled && t->stride == r->stride && t->w == r->w &&
                                    t->h == r->h && texture_top(t) == 0;
            if (exact_alias) continue;
            const int view_ok = t->fmt == 3 && r->fmt == 3 && !t->swizzled && texture_top(t) == 0 &&
                                t->stride == r->stride &&
                                ((int64_t)r->addr - base) % ((int64_t)t->stride * 4) == 0;
            if (view_ok) {
                /* The target's part comes from the GPU below; only the CPU's
                 * pending writes into it need to reach it, and no readback. */
                if (r->cpu_pending) rt_import(r);
                hit = 1;
                supplied |= 1u << i;
                if (r->sx > sx) sx = r->sx;
                if (r->sy > sy) sy = r->sy;
            } else if (r->dirty || r->cpu_pending) {
                readback_rt(i);
                r->dirty = 0;
            }
        }
    GLuint native = texcache_native(t, supplied);
    if (!hit || !native) return native;
    const int w = (int)ceil(t->w * sx), h = (int)ceil(t->h * sy);
    if (w > g.max_size || h > g.max_size || w < 1 || h < 1) return native;
    if (!g.view_fbo) {
        p_glGenFramebuffers(1, &g.view_fbo);
        p_glGenFramebuffers(1, &g.copy_fbo);
        p_glGenTextures(1, &g.view_tex);
    }
    p_glBindTexture(GL_TEXTURE_2D, g.view_tex);
    if (g.view_w != w || g.view_h != h) {
        p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        g.view_w = w;
        g.view_h = h;
    }
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g.view_fbo);
    p_glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g.view_tex,
                             0);
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, g.copy_fbo);
    p_glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, native, 0);
    p_glDisable(GL_SCISSOR_TEST);
    p_glBlitFramebuffer(0, 0, t->w, t->h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    for (int i = 0; i < g.n_rts; i++) {
        rendertarget *r = &g.rts[i];
        if (!r->configured || r->fmt != 3 || t->stride != r->stride) continue;
        const int64_t delta = (int64_t)r->addr - base, pitch = (int64_t)t->stride * 4;
        if (delta % pitch) continue;
        const int row = (int)(delta / pitch);
        const int y0 = row < 0 ? -row : 0;
        const int y1 = row + r->guest_h > t->h ? t->h - row : r->guest_h;
        if (y0 >= y1) continue;
        const int cols = t->w < r->guest_w ? t->w : r->guest_w;
        rt_import(r);
        stencil_to_alpha(r);
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, r->fbo);
        p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g.view_fbo);
        p_glDisable(GL_SCISSOR_TEST);
        p_glBlitFramebuffer(0, r->h - pixel_edge(y0 * r->sy), pixel_edge(cols * r->sx),
                            r->h - pixel_edge(y1 * r->sy), 0, pixel_edge((row + y0) * sy),
                            pixel_edge(cols * sx), pixel_edge((row + y1) * sy), GL_COLOR_BUFFER_BIT,
                            GL_NEAREST);
    }
    g.tex_sx = (float)sx;
    g.tex_sy = (float)sy;
    g.bound_top = 0;
    g.rt_views++;
    return g.view_tex;
}

static void gl_texture(const psp_tex_state *t) {
    if (claim() != 0) return;
    if (g.mu.tex_valid && memcmp(t, &g.tex, sizeof *t) == 0 &&
        g.mu.bound_clut_gen == g.mu.clut_gen &&
        psp_mem_write_serial() == g.mu.bound_serial && !g.mu.bound_is_view) { g.mu.setters_skipped++; return; }
    g.mu.state_gen++;
    flush_ends(FB_TEXTURE); flush();
    g.mu.tex_valid = 1; g.mu.bound_clut_gen = g.mu.clut_gen; g.mu.bound_serial = psp_mem_write_serial();
    g.tex = *t;
    g.tex_enable = t->addr != 0;
    g.bound_top = 0;
    g.tex_sx = g.tex_sy = 1;
    const uint64_t bind_t0 = psp_os_mono_ns();
    g.bound = g.tex_enable ? texcache_get(t) : 0;
    g.tex_bind_ns += psp_os_mono_ns() - bind_t0;
    g.mu.bound_is_view = g.bound && g.view_tex && g.bound == g.view_tex;
    if (g.bound) {
        p_glBindTexture(GL_TEXTURE_2D, g.bound);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, g.bound_top);
    } else {
        g.tex_enable = 0;
    }
}
static void gl_clut(uint32_t a, int f, int sh, int m, int st) {
    if (claim() != 0) return;
    if (g.clut_addr == a && g.clut_fmt == f && g.clut_shift == sh && g.clut_mask == m && g.clut_start == st) { g.mu.setters_skipped++; return; }
    g.mu.state_gen++; g.mu.clut_gen++;
    flush_ends(FB_CLUT); flush();
    /* Part of the cache key rather than state of its own: the palette is what
     * a CLUT texture's texels decode through, so a new palette is a new
     * texture even at the same address. */
    g.clut_addr = a; g.clut_fmt = f;
    g.clut_shift = sh; g.clut_mask = m; g.clut_start = st;
}
static void gl_depth(int test, int func, int write) {
    if (claim() != 0) return;
    if (g.z_test == test && g.z_func == func && g.z_write == write) { g.mu.setters_skipped++; return; }
    g.mu.state_gen++;
    flush_ends(FB_DEPTH); flush();
    g.z_test = test; g.z_func = func; g.z_write = write;
}

static void gl_blend(const psp_blend_state *b) {
    if (claim() != 0) return;
    if (memcmp(b, &g.bs, sizeof *b) == 0) { g.mu.setters_skipped++; return; }
    g.mu.state_gen++;
    flush_ends(FB_BLEND); flush();
    g.bs = *b;
}
static void gl_fog(int enable, uint32_t colour) {
    if (claim() != 0) return;
    if (g.fog_enable == enable && g.fog_colour == colour) { g.mu.setters_skipped++; return; }
    g.mu.state_gen++;
    flush_ends(FB_FOG); flush();
    g.fog_enable = enable;
    g.fog_colour = colour;
}

static void reserve_vertices(size_t n) {
    /* Flush only between complete triangles. GL_MAX_VERTS is not divisible by
     * three, so checking one vertex at a time can strand one endpoint at the
     * end of one draw and two at the start of the next. */
    if (g.batch_n + n > GL_MAX_VERTS) { flush(); g.batch_overflows++; }
}

static void push(const psp_vertex *v, int lod16) {
    float *o = g.batch + g.batch_n * FLOATS_PER_VERT;
    /* The reference path keeps PSP quantization. Enhanced display targets can
     * use the float position retained by the GE; byte-interpreted scratch
     * surfaces continue using the exact 1x coordinate contract. */
    const int precise = g.resolution && g.rts[g.cur_rt].display && v->precise;
    o[0] = precise ? v->precise_x : (float)v->x / PSP_SUBPX;
    o[1] = precise ? v->precise_y : (float)v->y / PSP_SUBPX;
    o[2] = v->z / 65535.0f;                /* the PSP's window depth scale */
    o[3] = (float)( v->rgba        & 0xFF) / 255.0f;
    o[4] = (float)((v->rgba >>  8) & 0xFF) / 255.0f;
    o[5] = (float)((v->rgba >> 16) & 0xFF) / 255.0f;
    o[6] = (float)((v->rgba >> 24) & 0xFF) / 255.0f;
    /* Keep UVs in the texel units the backend contract supplies. The shader
     * scales those units for each mip level; normalising here and multiplying
     * back there moves exact 1/16 boundaries through an avoidable round trip. */
    o[7] = v->u;
    o[8] = v->v;
    o[9] = (float)v->fog / 255.0f;
    o[10] = v->inv_w;
    o[11] = v->tex_q;
    o[12] = (float)lod16;
    g.batch_n++;
}

/* Match sw_tri's per-primitive scale calculation. In particular this is based
 * on the submitted texel coordinates, not on GL's per-fragment derivatives:
 * CONST and SLOPE have no derivative at all, while AUTO is measured by the PSP
 * once for the primitive and quantised to a sixteenth before adding its bias. */
static int triangle_lod16(const psp_vertex *a, const psp_vertex *b,
                          const psp_vertex *c) {
    if (!g.tex_enable) return 0;
    const float e1x = (float)(b->x - a->x) / 16.0f;
    const float e1y = (float)(b->y - a->y) / 16.0f;
    const float e2x = (float)(c->x - a->x) / 16.0f;
    const float e2y = (float)(c->y - a->y) / 16.0f;
    const float det = e1x * e2y - e1y * e2x;
    if (det == 0.0f) return 0;
    const float du1 = b->u - a->u, du2 = c->u - a->u;
    const float dv1 = b->v - a->v, dv2 = c->v - a->v;
    const float dudx = (du1 * e2y - du2 * e1y) / det;
    const float dudy = (du2 * e1x - du1 * e2x) / det;
    const float dvdx = (dv1 * e2y - dv2 * e1y) / det;
    const float dvdy = (dv2 * e1x - dv1 * e2x) / det;
    const float rx = sqrtf(dudx * dudx + dvdx * dvdx);
    const float ry = sqrtf(dudy * dudy + dvdy * dvdy);
    const float w = (psp_render_vertex_w(a) + psp_render_vertex_w(b) + psp_render_vertex_w(c)) / 3.0f;
    return psp_render_lod16(&g.tex, rx > ry ? rx : ry, w);
}

static void push_triangle(const psp_vertex *a, const psp_vertex *b,
                          const psp_vertex *c) {
    const int lod16 = triangle_lod16(a, b, c);
    reserve_vertices(3);
    push(a, lod16); push(b, lod16); push(c, lod16);
}

/* A sprite is two triangles from opposite corners, axis-aligned, taking its
 * colour and fog from the second vertex and depth from the first the way the
 * software path does. The other two UVs have to be made here: copying the
 * second vertex to all four corners collapses every textured sprite to one
 * texel.
 *
 * With exactly one screen axis reversed, the PSP transposes the mapping: u
 * follows y and v follows x. This is the same rule measured and implemented
 * by sw_sprite(), rather than a GL-specific approximation. */
static void push_sprite(const psp_vertex *v) {
    psp_vertex a = v[1], b = v[1], c = v[1], d = v[1];
    a.x = v[0].x; a.y = v[0].y;
    b.x = v[1].x; b.y = v[0].y;
    c.x = v[1].x; c.y = v[1].y;
    d.x = v[0].x; d.y = v[1].y;
    a.precise_x = d.precise_x = v[0].precise_x;
    a.precise_y = b.precise_y = v[0].precise_y;
    b.precise_x = c.precise_x = v[1].precise_x;
    c.precise_y = d.precise_y = v[1].precise_y;
    a.precise = b.precise = c.precise = d.precise = v[0].precise && v[1].precise;
    a.z = b.z = c.z = d.z = v[0].z;
    /* A sprite's two-corner mapping is affine even when its endpoints came
     * through the transform pipeline.  The software rectangle path has the
     * same rule; forcing homogeneous ones keeps the GL expansion equivalent. */
    a.inv_w = b.inv_w = c.inv_w = d.inv_w = 1.0f;
    a.tex_q = b.tex_q = c.tex_q = d.tex_q = 1.0f;
    a.u = v[0].u; a.v = v[0].v;
    c.u = v[1].u; c.v = v[1].v;
    const int transposed = (v[1].x < v[0].x) != (v[1].y < v[0].y);
    if (transposed) {
        b.u = v[0].u; b.v = v[1].v;
        d.u = v[1].u; d.v = v[0].v;
    } else {
        b.u = v[1].u; b.v = v[0].v;
        d.u = v[0].u; d.v = v[1].v;
    }
    int lod16 = 0;
    const int dx = v[1].x - v[0].x, dy = v[1].y - v[0].y;
    const int uden = transposed ? dy : dx;
    const int vden = transposed ? dx : dy;
    if (g.tex_enable && uden && vden) {
        const float du = (v[1].u - v[0].u) / (float)uden;
        const float dv = (v[1].v - v[0].v) / (float)vden;
        const float rx = fabsf(du) * 16.0f, ry = fabsf(dv) * 16.0f;
        lod16 = psp_render_lod16(&g.tex, rx > ry ? rx : ry,
                                 (psp_render_vertex_w(&v[0]) + psp_render_vertex_w(&v[1])) * 0.5f);
    }
    reserve_vertices(6);
    push(&a, lod16); push(&b, lod16); push(&c, lod16);
    push(&a, lod16); push(&c, lod16); push(&d, lod16);
}

/* Explicit pixel quads avoid GL's implementation-dependent native line/point
 * coverage. The shared walker defines coverage only; these fragments still
 * pass through the normal texture, alpha, depth, stencil and blend pipeline. */
/* How far, in guest pixels, a band batch may reach past the screen on
 * either side: the wide target's extra columns, halved. */
static int band_reach(const rendertarget *r) {
    if (!r->wide || !g.batch_band) return 0;
    const double guest_visible = g.resolution ? r->visible_w / r->ui_scale : (double)r->wide_w;
    return (int)floor((guest_visible - g.w) / 2);
}
static int band_reach_y(const rendertarget *r) {
    if (!r->wide || !g.batch_band) return 0;
    return (int)floor((r->visible_h / r->ui_scale - g.h) / 2);
}
static void push_point_sample(const psp_vertex *v, void *opaque) {
    psp_vertex a = *v, b = *v, c = *v, d = *v;
    a.precise = b.precise = c.precise = d.precise = 0;
    const int x = (int)floorf((float)v->x / PSP_SUBPX);
    const int y = (int)floorf((float)v->y / PSP_SUBPX);
    const int reach = band_reach(&g.rts[g.cur_rt]);
    const int reach_y = band_reach_y(&g.rts[g.cur_rt]);
    if (x < -reach || y < -reach_y || x >= g.rts[g.cur_rt].guest_w + reach || y >= g.rts[g.cur_rt].guest_h + reach_y) return;
    a.x = d.x = x * PSP_SUBPX; b.x = c.x = a.x + PSP_SUBPX;
    a.y = b.y = y * PSP_SUBPX; c.y = d.y = a.y + PSP_SUBPX;
    const int lod16 = *(const int *)opaque;
    reserve_vertices(6);
    push(&a, lod16); push(&b, lod16); push(&c, lod16);
    push(&a, lod16); push(&c, lod16); push(&d, lod16);
}

/* Last Raven's body font: 512x512 CLUT4 atlas, axis-aligned 13-pixel-high
 * glyph triangles with a 1:1 texel mapping. At 1x LINEAR samples texel centres
 * exactly. At higher resolution it interpolates the bitmap's already shaded
 * edges again, reducing stroke contrast. Preserve those texels with NEAREST.
 * Recognize the draw rather than an allocation address (which changes across
 * menus). This policy belongs to the game host, not the PSP sampler; it is
 * applied to every title, as both games' backends did before they merged. */
static int bitmap_glyph_draw(int prim, const psp_vertex *v, int count) {
    const rendertarget *r = &g.rts[g.cur_rt];
    if (!g.resolution || !r->display || r->sy <= 1.0 || !g.tex_enable || g.tex.fmt != 4 ||
        g.tex.w != 512 || g.tex.h != 512 || g.bound_top || prim != PSP_PRIM_TRIANGLES ||
        count <= 0 || count % 3 || (g.z_test && g.z_func != 1))
        return 0;
    for (int i = 0; i < count; i += 3) {
        int x0 = v[i].x, x1 = x0, y0 = v[i].y, y1 = y0;
        const float du = v[i].u - (float)v[i].x / PSP_SUBPX;
        const float dv = v[i].v - (float)v[i].y / PSP_SUBPX;
        for (int j = i; j < i + 3; j++) {
            if (!v[j].screen_space || fabsf(v[j].tex_q - 1.0f) > 0.0001f ||
                fabsf(v[j].u - (float)v[j].x / PSP_SUBPX - du) > 0.01f ||
                fabsf(v[j].v - (float)v[j].y / PSP_SUBPX - dv) > 0.01f)
                return 0;
            if (v[j].x < x0) x0 = v[j].x;
            if (v[j].x > x1) x1 = v[j].x;
            if (v[j].y < y0) y0 = v[j].y;
            if (v[j].y > y1) y1 = v[j].y;
        }
        if (y1 - y0 != 13 * PSP_SUBPX || x1 - x0 <= 0 || x1 - x0 > 13 * PSP_SUBPX) return 0;
        for (int j = i; j < i + 3; j++)
            if ((v[j].x != x0 && v[j].x != x1) || (v[j].y != y0 && v[j].y != y1)) return 0;
    }
    return 1;
}

/* The title's final glow composite (psp_title_info.bloom): a full-display
 * sprite reading the whole of a small scratch target. Match the producer and
 * the draw, not the VRAM address, which other work reuses (The 3rd
 * Birthday's movies do). Its blur and downsample passes, authored glows, UI,
 * nearest sampling and perspective draws keep the PSP's sampling. */
static int bloom_composite(int prim, const psp_vertex *v, int count) {
    const psp_bloom_composite *b = psp_title_info.bloom;
    if (!g.smooth_bloom || !b || !g.rts[g.cur_rt].display || prim != PSP_PRIM_SPRITES || count != 2 ||
        !g.tex_enable || g.tex.fmt != b->tex_fmt || g.tex.swizzled || g.bound_top ||
        g.tex.w != b->w || g.tex.h != b->h || g.tex.stride != b->w ||
        !(g.tex.min_filter & 1) || !(g.tex.mag_filter & 1) || g.tex.func != b->tex_func ||
        !g.bs.enable || g.bs.eq != 0 || g.bs.src != b->blend_src || g.bs.dst != b->blend_dst || g.z_test)
        return 0;
    if (!v[0].screen_space || !v[1].screen_space ||
        v[0].x != 0 || v[0].y != 0 || v[1].x != g.w*PSP_SUBPX || v[1].y != g.h*PSP_SUBPX ||
        v[0].u != 0 || v[0].v != 0 || v[1].u != b->w || v[1].v != b->h)
        return 0;
    for (int i=0; i<g.n_rts; i++) {
        const rendertarget *r = &g.rts[i];
        if (r->configured && !r->display && r->addr == (g.tex.addr & PSP_ADDR_MASK) &&
            r->fmt == b->target_fmt && r->stride == b->w && r->guest_w == b->w && r->guest_h == b->h)
            return 1;
    }
    return 0;
}

static void gl_draw(int prim, const psp_vertex *v, int count) {
    if (claim() != 0) return;
    if (g.mu.npend) flush_model();
    g.draws++;
    g.verts += (uint64_t)count;
    if (g.bs.stencil_test) {
        const rendertarget *r = &g.rts[g.cur_rt];
        if (g.target_fmt == 3 && (!r->configured || r->fmt == 3)) g.stencil_draws++;
        else g.unsupported_stencil_draws++;
    }
    if ((g.resolution || g.smooth_bloom || prim <= PSP_PRIM_LINE_STRIP) && rt_prepare(g.cur_rt) != 0) return;
    const int glyph = bitmap_glyph_draw(prim, v, count);
    if (glyph != g.batch_glyph) { flush_ends(FB_GLYPH); flush(); g.batch_glyph = glyph; }
    if (glyph) g.glyph_draws++;
    const int bloom = bloom_composite(prim, v, count);
    if (bloom != g.batch_bloom) {
        flush_ends(FB_CLASS); flush(); g.batch_bloom = bloom; g.mu.state_gen++;
    }
    if (bloom) g.bloom_draws++;

    /* Insets follow the centred safe area. Perspective geometry fills the
     * scene, and so do the screen-space draws the title's rule below says;
     * the rest, fixed HUD, keeps its proportions. A class change ends the
     * pending batch. */
    if (g.adaptive_aspect) {
        if (rt_prepare(g.cur_rt) != 0) return;
        int cls = CLASS_SCENE, band = 0;
        if (menu_preview(&g.rts[g.cur_rt])) {
            cls = CLASS_PREVIEW;
        } else if (g.rts[g.cur_rt].wide && count > 0) {
            int ss = 1, min_x = v[0].x, max_x = v[0].x;
            for (int i = 0; i < count; i++) {
                if (!v[i].screen_space) { ss = 0; break; }
                if (v[i].x < min_x) min_x = v[i].x;
                if (v[i].x > max_x) max_x = v[i].x;
            }
            const int full = min_x <= PSP_SUBPX && max_x >= (g.w - 1) * PSP_SUBPX;
            /* Every other screen-space draw is HUD, including the ones that
             * test depth (the lock-on reticle: two 64x64 sprites, GREATER).
             * The game projects those on the CPU with the display camera's
             * own matrices, which the camera replacement keeps at the native
             * 480x272 focal length; a target d pixels from the centre gets
             * its reticle d guest pixels from x=240, and the HUD mapping
             * puts that at wide/2 + d -- the same target pixel the scene
             * mapping gave the target (vertical FOV, so pixel scale, is
             * unchanged). Its depth test therefore meets the target's own
             * depth. The scene mapping would multiply d by wide/480 again. */
            /* Which other screen-space draws fill the scene is the title's
             * choice (psp_title_info, PSP_TITLE_ASPECT_BY_SOURCE). */
            int target_texture = 0;
            const int by_source = psp_title_can(PSP_TITLE_ASPECT_BY_SOURCE);
            const uint32_t tex_addr = g.tex.addr & PSP_ADDR_MASK;
            for (int i = 0; by_source && g.tex_enable && i < g.n_rts; i++) {
                const rendertarget *source = &g.rts[i];
                const uint64_t end = (uint64_t)source->addr + (uint64_t)source->stride * source->guest_h *
                                     (source->fmt == 3 ? 4 : 2);
                /* A palette may reinterpret each rendered pixel without
                 * changing its location: The 3rd Birthday's 128x64 bloom is
                 * drawn as 4444, then composited through CLUT16. Match those
                 * equal-width texels too. Byte/nibble indices and different
                 * strides do not preserve that layout (nor does movie memory
                 * reuse). */
                const int same_pixels = g.tex.fmt == source->fmt ||
                    (g.tex.fmt == 6 && source->fmt >= 0 && source->fmt <= 2) ||
                    (g.tex.fmt == 7 && source->fmt == 3);
                if (source->configured && !g.tex.swizzled && same_pixels &&
                    g.tex.stride == source->stride && tex_addr >= source->addr && tex_addr < end)
                    target_texture = 1;
            }
            if (by_source) {
                /* The 3rd Birthday's movie and menu strips keep their
                 * authored composition; a movie's separate strips stay
                 * together. A draw reading a render target composites pixels
                 * the scene mapping placed, so it takes that mapping whatever
                 * its width: a post effect drawn as several partial strips
                 * must not be squeezed into the safe area (the hub-lighting
                 * failure). Movies and HUD read authored textures, never a
                 * target (hub, street, aim, free-look, status-menu and movie
                 * captures: that repository's docs/ASPECT-VALIDATION.md).
                 * Only full-width untextured draws -- clears, fades -- join
                 * them. */
                if (ss && !target_texture && (!full || g.tex_enable)) cls = CLASS_HUD;
            } else {
                /* Armored Core's: any screen-space draw spanning the full
                 * guest width -- clears, fades, a movie in strips, a bar --
                 * must cover the wide target, so it fills the scene. */
                if (ss && !full) cls = CLASS_HUD;
            }
            if (!ss) g.rts[g.cur_rt].scene_drawn = 1;
            if (ss && target_texture && getenv("PSPRECOMP_ASPECT_LOG")) {
                static int said3;
                if (said3++ < 16 || atoi(getenv("PSPRECOMP_ASPECT_LOG")) >= 2)
                    fprintf(stderr, "aspect: %s draw reads target %08X %dx%d fmt %d -> scene:"
                                    " prim %d, %d verts, x %.1f..%.1f, at present %llu\n",
                            full ? "full-width" : "partial-width", g.tex.addr, g.tex.w, g.tex.h,
                            g.tex.fmt, prim, count, (float)min_x / PSP_SUBPX, (float)max_x / PSP_SUBPX,
                            (unsigned long long)g.presents);
            }
            int min_y = v[0].y, max_y = v[0].y;
            for (int i = 0; i < count; i++) { if (v[i].y < min_y) min_y = v[i].y; if (v[i].y > max_y) max_y = v[i].y; }
            const int beyond = min_x < 0 || max_x > g.w * PSP_SUBPX || min_y < 0 || max_y > g.h * PSP_SUBPX;
            /* World-tracking HUD -- a lock marker, lock-on rings -- is drawn
             * wherever its target projects, off the native screen included
             * (the PSP's scissor removed those; a Last Raven run logs ring x
             * from -31580 to 1380). On a wide target such a draw is placed in
             * the bands beside the centred area: the HUD mapping, with a
             * viewport and scissor spanning the whole target. Only a title
             * whose psp_title_info claims PSP_TITLE_HUD_BANDS does this, once
             * its menus and missions are audited for 2D parked off the
             * screen (AC3 Portable's menus draw thousands of untextured
             * pieces there).
             *
             * The PSP's scissor clips 2D at 480x272, and HUD that merely
             * overhangs the edge must stay clipped there: The 3rd Birthday's
             * street HUD frame pieces run one pixel past it (x -1, 481,
             * y -1), its status menu's pieces reach x -17 and 490, and panels
             * slide in across it; Armored Core's menus tile a backdrop whose
             * last column spans x 448..512. So a draw that misses the native
             * screen entirely goes to the bands -- plus line strokes (Last
             * Raven's lock box slides past the edge with its target) and
             * depth-tested reticles, which no 2D backdrop uses. A marker
             * crossing the edge otherwise is clipped like fixed HUD: The 3rd
             * Birthday draws both with the same matrix, vertex type and
             * state. Only after world geometry this frame, since a pure 2D
             * menu parks pieces off screen (x -37..-5), and only for a draw
             * smaller than the screen. The screen's right edge is x = 480
             * exclusive: a quad ending there is on screen, not beyond it. */
            const int outside = max_x <= 0 || min_x >= g.w * PSP_SUBPX ||
                                max_y <= 0 || min_y >= g.h * PSP_SUBPX;
            const int lines = prim == PSP_PRIM_LINES || prim == PSP_PRIM_LINE_STRIP;
            band = hud_bands_available() && cls == CLASS_HUD && g.rts[g.cur_rt].scene_drawn &&
                   (outside || (beyond && (lines || (g.z_test && g.z_func != 1)))) &&
                   max_y - min_y < g.h * PSP_SUBPX && max_x - min_x < g.w * PSP_SUBPX;
            if (cls == CLASS_HUD && beyond && getenv("PSPRECOMP_ASPECT_LOG")) {
                static int said2;
                if (said2++ < 64 || atoi(getenv("PSPRECOMP_ASPECT_LOG")) >= 2)
                    fprintf(stderr, "aspect: 2D draw beyond the screen%s: prim %d, %d verts, x %.1f..%.1f,"
                                    " y %.1f..%.1f, rgba %08X, tex %08X %dx%d, at present %llu, window image %u\n",
                            band ? " (band)" : "", prim, count, (float)min_x / PSP_SUBPX, (float)max_x / PSP_SUBPX,
                            (float)min_y / PSP_SUBPX, (float)max_y / PSP_SUBPX, v[0].rgba,
                            g.tex_enable ? g.tex.addr : 0u, g.tex.w, g.tex.h,
                            (unsigned long long)g.presents, shot_counters[0]);
            }
            if (ss && !full && g.z_test && g.z_func != 1 && getenv("PSPRECOMP_ASPECT_LOG")) {
                /* PSPRECOMP_ASPECT_LOG=2: every such draw, not the first 16. */
                static int said;
                if (said++ < 16 || atoi(getenv("PSPRECOMP_ASPECT_LOG")) >= 2)
                    fprintf(stderr, "aspect: screen-space draw tests depth (func %d)"
                                    " -> HUD%s: %d verts, x %.1f..%.1f, tex %08X %dx%d,"
                                    " at present %llu, window image %u\n",
                            g.z_func, band ? " (band)" : "", count, (float)min_x / PSP_SUBPX,
                            (float)max_x / PSP_SUBPX,
                            g.tex_enable ? g.tex.addr : 0u, g.tex.w, g.tex.h,
                            (unsigned long long)g.presents, shot_counters[0]);
            }
        }
        if (cls != g.batch_class || band != g.batch_band) {
            flush_ends(FB_CLASS); flush();
            g.batch_class = cls; g.batch_band = band; g.mu.state_gen++;
        }
        if (cls == CLASS_PREVIEW) g.class_preview++;
        else if (cls == CLASS_HUD) g.class_hud++; else g.class_scene++;
    }

    switch (prim) {
    case PSP_PRIM_POINTS: {
        float w = 0.0f;
        for (int i = 0; i < count; i++) w += psp_render_vertex_w(&v[i]);
        int lod16 = psp_render_lod16(&g.tex, 1.0f, count ? w / (float)count : 1.0f);
        for (int i = 0; i < count; i++) push_point_sample(&v[i], &lod16);
        break;
    }
    case PSP_PRIM_LINES:
    case PSP_PRIM_LINE_STRIP: {
        const rendertarget *r = &g.rts[g.cur_rt];
        /* A band batch under the game's whole-screen scissor (0..479) walks
         * into the bands; the scissor stage lets those pixels through too. */
        const int reach = g.sc_valid && g.sc_x0 <= 0 && g.sc_x1 >= g.w - 1 ? band_reach(r) : 0;
        const int x0 = reach ? -reach : g.sc_valid && g.sc_x0 > 0 ? g.sc_x0 : 0;
        const int reach_y = g.sc_valid && g.sc_y0 <= 0 && g.sc_y1 >= g.h - 1 ? band_reach_y(r) : 0;
        const int y0 = reach_y ? -reach_y : g.sc_valid && g.sc_y0 > 0 ? g.sc_y0 : 0;
        const int x1 = reach ? g.w - 1 + reach : g.sc_valid && g.sc_x1 < r->guest_w - 1 ? g.sc_x1 : r->guest_w - 1;
        const int y1 = reach_y ? g.h - 1 + reach_y : g.sc_valid && g.sc_y1 < r->guest_h - 1 ? g.sc_y1 : r->guest_h - 1;
        for (int i = 0; i + 1 < count; i += prim == PSP_PRIM_LINES ? 2 : 1) {
            int lod16 = psp_render_line_lod16(&g.tex, &v[i], &v[i + 1]);
            psp_render_walk_line(&v[i], &v[i + 1], x0, y0, x1, y1, push_point_sample, &lod16);
        }
        break;
    }
    case 3:                                        /* triangles */
        for (int i = 0; i + 2 < count; i += 3)
            push_triangle(&v[i], &v[i + 1], &v[i + 2]);
        break;
    case 4:                                        /* triangle strip */
        for (int i = 0; i + 2 < count; i++) {
            /* Winding alternates along a strip; preserve it so a later
             * increment can turn face culling on without the strip flipping. */
            if (i & 1) push_triangle(&v[i + 1], &v[i], &v[i + 2]);
            else       push_triangle(&v[i], &v[i + 1], &v[i + 2]);
        }
        break;
    case 5:                                        /* triangle fan */
        for (int i = 1; i + 1 < count; i++)
            push_triangle(&v[0], &v[i], &v[i + 1]);
        break;
    case 6:                                        /* sprites, in pairs */
        for (int i = 0; i + 1 < count; i += 2) push_sprite(&v[i]);
        break;
    default:
        g.unsupported_prims++;
        break;
    }
}

/* The GE's comparison codes, shared by the depth test, the alpha test and the
 * stencil test. Same order as software's depth_pass(). */
static GLenum gl_compare(int func) {
    switch (func) {
    case 0:  return GL_NEVER;
    case 2:  return GL_EQUAL;
    case 3:  return GL_NOTEQUAL;
    case 4:  return GL_LESS;
    case 5:  return GL_LEQUAL;
    case 6:  return GL_GREATER;
    case 7:  return GL_GEQUAL;
    default: return GL_ALWAYS;
    }
}

static GLenum gl_stencil_op(int op) {
    switch (op) {
    case 1: return GL_ZERO;
    case 2: return GL_REPLACE;
    case 3: return GL_INVERT;
    case 4: return GL_INCR;
    case 5: return GL_DECR;
    default: return GL_KEEP;
    }
}

static int uses_dest_alpha(void) {
    return g.bs.enable && ((g.bs.src >= 4 && g.bs.src <= 5) ||
                           (g.bs.src >= 8 && g.bs.src <= 9) ||
                           (g.bs.dst >= 4 && g.bs.dst <= 5) ||
                           (g.bs.dst >= 8 && g.bs.dst <= 9));
}

/* Codes 6..9 have no direct GL factor. apply_state handles the additive 6/7
 * pair in the fragment shader; remaining combinations retain the unsupported
 * counter so they are visible in reports. */
static GLenum gl_factor(int f, int is_src, int *ok) {
    switch (f) {
    case 0:  return is_src ? GL_DST_COLOR : GL_SRC_COLOR;
    case 1:  return is_src ? GL_ONE_MINUS_DST_COLOR : GL_ONE_MINUS_SRC_COLOR;
    case 2:  return GL_SRC_ALPHA;
    case 3:  return GL_ONE_MINUS_SRC_ALPHA;
    case 4:  return GL_DST_ALPHA;
    case 5:  return GL_ONE_MINUS_DST_ALPHA;
    default: *ok = 0; return GL_ONE;
    }
}

/* PSP fixed blend factors are separate RGB constants for the source and
 * destination terms. GL has only one glBlendColor, but the two important
 * endpoints need no constant at all: fixed black is GL_ZERO and fixed white
 * is GL_ONE. That makes the hangar compositor's FIXA=808080/FIXB=000000 pair
 * exactly representable. `uses_constant` tells apply_state whether the one GL
 * constant is still needed for this side. */
static GLenum gl_fixed_factor(uint32_t colour, int *uses_constant) {
    colour &= 0xFFFFFFu;
    if (colour == 0x000000u) return GL_ZERO;
    if (colour == 0xFFFFFFu) return GL_ONE;
    *uses_constant = 1;
    return GL_CONSTANT_COLOR;
}

static void note_blend_miss(void) {
    int i = 0;
    while (i < g.blend_misses &&
           (g.blend_miss[i].src != g.bs.src || g.blend_miss[i].dst != g.bs.dst || g.blend_miss[i].eq != g.bs.eq ||
            ((g.bs.src == 10 || g.bs.dst == 10) &&
             (g.blend_miss[i].fixa != (g.bs.fixa & 0xFFFFFFu) || g.blend_miss[i].fixb != (g.bs.fixb & 0xFFFFFFu)))))
        i++;
    if (i == g.blend_misses) {
        if (i == (int)(sizeof g.blend_miss / sizeof g.blend_miss[0])) return;
        g.blend_miss[i].src = g.bs.src; g.blend_miss[i].dst = g.bs.dst; g.blend_miss[i].eq = g.bs.eq;
        g.blend_miss[i].fixa = g.bs.fixa & 0xFFFFFFu; g.blend_miss[i].fixb = g.bs.fixb & 0xFFFFFFu;
        g.blend_misses++;
    }
    g.blend_miss[i].draws++;
}

static GLenum gl_equation(int eq, int *ok) {
    switch (eq) {
    case 0:  return GL_FUNC_ADD;
    case 1:  return GL_FUNC_SUBTRACT;
    case 2:  return GL_FUNC_REVERSE_SUBTRACT;
    case 3:  return GL_MIN;
    case 4:  return GL_MAX;
    default: *ok = 0; return GL_FUNC_ADD;   /* 5 is |src-dst|, shader work */
    }
}

/* Apply what the interpreter last set. Called once per flush rather than per
 * primitive: the batch is by construction all one state. */
static void apply_state(void) {
    if (g.bs.stencil_test && g.rts[g.cur_rt].fmt == 3) {
        p_glEnable(GL_STENCIL_TEST);
        /* PMSK2 keeps the stencil bits it sets. */
        p_glStencilMask(~(g.bs.pixel_mask >> 24) & 255u);
        /* GL compares reference against stored stencil; the GE backend
         * contract compares stored stencil against reference. */
        const int func = g.bs.stencil_func;
        p_glStencilFunc(gl_compare(func >= 4 && func <= 7 ? func ^ 2 : func),
                         g.bs.stencil_ref & 255, (GLuint)g.bs.stencil_mask & 255);
        p_glStencilOp(gl_stencil_op(g.bs.op_sfail), gl_stencil_op(g.bs.op_zfail),
                       gl_stencil_op(g.bs.op_zpass));
    } else {
        p_glDisable(GL_STENCIL_TEST);
        p_glStencilMask(0);
    }
    /* In OpenGL, disabling GL_DEPTH_TEST also disables depth-buffer writes,
     * regardless of glDepthMask, and the hardware agrees: ZMSK on its own does
     * not write depth through a disabled test. An earlier revision kept the GL
     * test alive with ALWAYS whenever a write was asked for, so that clear-mode
     * draws -- which the GE layer used to encode as a *disabled* test -- would
     * still establish the depth later geometry tests against. That also let
     * every ordinary draw with the test off stamp the depth buffer: in AC3
     * Portable's garage the panel's translucent tint is drawn that way, at the
     * near plane, and it occluded the AC and part previews entirely (only the
     * foot below its bottom edge survived). Clear mode now arrives as an
     * enabled ALWAYS test instead, so both cases are right. */
    if (g.z_test) {
        p_glEnable(GL_DEPTH_TEST);
        p_glDepthFunc(gl_compare(g.z_func));
    } else {
        p_glDisable(GL_DEPTH_TEST);
    }
    p_glDepthMask(g.z_write ? GL_TRUE : GL_FALSE);

    /* write_colour is clear mode's colour mask. Alpha is the stencil byte and
     * an ordinary draw does not write it, which is why the alpha channel is
     * masked off unless the state says otherwise. */
    /* The pixel mask, PMSK1 | PMSK2 << 24 in 0xAABBGGRR, keeps the
     * framebuffer's bits where it is set (render.h). A byte of FF keeps its
     * channel whole, which glColorMask says exactly; a byte of anything but
     * 00 or FF keeps some bits of it, which it cannot, and is counted.
     * WipEout Pulse draws the shadow volumes under its ships with PMSK1
     * FFFFFF, for the stencil alone; drawn in colour they were solid blue
     * blocks under every ship. */
    const uint32_t pm = g.bs.pixel_mask;
    for (int ch = 0; ch < 4; ch++) {
        const uint32_t byte = (pm >> (8 * ch)) & 0xFFu;
        if (byte && byte != 0xFFu && (ch < 3 ? g.bs.write_colour : g.bs.write_alpha)) { g.partial_pixel_masks++; break; }
    }
    p_glColorMask(g.bs.write_colour && (pm & 0x0000FFu) != 0x0000FFu ? GL_TRUE : GL_FALSE,
                  g.bs.write_colour && (pm & 0x00FF00u) != 0x00FF00u ? GL_TRUE : GL_FALSE,
                  g.bs.write_colour && (pm & 0xFF0000u) != 0xFF0000u ? GL_TRUE : GL_FALSE,
                  g.bs.write_alpha  && (pm >> 24) != 0xFFu ? GL_TRUE : GL_FALSE);

    if (g.sc_valid) {
        p_glEnable(GL_SCISSOR_TEST);
        const rendertarget *r = &g.rts[g.cur_rt];
        const int hud = r->wide && g.batch_class != CLASS_SCENE;
        const double sx = hud ? r->ui_scale : r->sx;
        const double sy = hud ? r->ui_scale : r->sy;
        const double off = hud ? rt_off(r) : 0;
        const double off_y = hud ? rt_off_y(r) : 0;
        int x0, x1, y0, y1;
        if (g.resolution) {
            x0 = pixel_edge(g.sc_x0*sx+off);
            x1 = pixel_edge((g.sc_x1+1)*sx+off);
            y0 = pixel_edge(g.sc_y0*sy+off_y);
            y1 = pixel_edge((g.sc_y1+1)*sy+off_y);
        } else {
            x0 = (int)floor(g.sc_x0*sx+off);
            x1 = (int)ceil((g.sc_x1+1)*sx+off);
            y0 = (int)floor(g.sc_y0*sy+off_y); y1 = (int)ceil((g.sc_y1+1)*sy+off_y);
        }
        /* A band batch under the game's whole-screen scissor may use the
         * whole target; a narrower scissor still applies where it maps. */
        if (hud && g.batch_band && g.sc_x0 <= 0 && g.sc_x1 >= g.w - 1) { x0 = 0; x1 = r->visible_w; }
        if (hud && g.batch_band && g.sc_y0 <= 0 && g.sc_y1 >= g.h - 1) { y0 = 0; y1 = r->visible_h; }
        p_glScissor(x0, r->h-y1, x1>x0?x1-x0:0, y1>y0?y1-y0:0);
    } else {
        p_glDisable(GL_SCISSOR_TEST);
    }

    if (g.bs.enable) {
        int ok = 1;
        int src_constant = 0, dst_constant = 0;
        const int double_alpha = g.bs.src == 6 && g.bs.eq == 0 && !g.bs.write_alpha &&
                                 (g.bs.dst == 7 || (g.bs.dst == 10 &&
                                  ((g.bs.fixb & 0xffffffu) == 0 ||
                                   (g.bs.fixb & 0xffffffu) == 0xffffffu)));
        GLenum src = double_alpha ? GL_ONE : g.bs.src == 10
                   ? gl_fixed_factor(g.bs.fixa, &src_constant)
                   : gl_factor(g.bs.src, 1, &ok);
        GLenum dst = double_alpha && g.bs.dst == 7 ? GL_ONE_MINUS_SRC_ALPHA : g.bs.dst == 10
                   ? gl_fixed_factor(g.bs.fixb, &dst_constant)
                   : gl_factor(g.bs.dst, 0, &ok);
        /* A crossfade: FIXB the complement of FIXA in every channel, which
         * the one GL constant says as ONE_MINUS_CONSTANT_COLOR on the
         * destination. WipEout Pulse blends its race picture this way, FIXA
         * 606060 and FIXB 9F9F9F; given FIXA on both sides it came out a
         * quarter too dark. */
        const int crossfade = src_constant && dst_constant &&
                              (g.bs.fixb & 0xFFFFFFu) == (~g.bs.fixa & 0xFFFFFFu);
        if (crossfade) dst = GL_ONE_MINUS_CONSTANT_COLOR;
        if (!ok) g.unsupported_blend_factor++;
        if (src_constant && dst_constant && !crossfade &&
            (g.bs.fixa & 0xFFFFFFu) != (g.bs.fixb & 0xFFFFFFu)) {
            g.unsupported_blend_factor++;
            ok = 0;
        }
        int eq_ok = 1;
        const GLenum eq = gl_equation(g.bs.eq, &eq_ok);
        if (!eq_ok) g.unsupported_blend_eq++;
        if (!ok || !eq_ok) note_blend_miss();
        /* This rewrite changes fragment alpha to carry an exact destination
         * factor, so only use it when alpha is the framebuffer's masked-off
         * stencil byte. MIN/MAX ignore factors; abs-difference is not
         * represented by this fixed-function path. */
        const int preblend_src = g.bs.src == 2 && g.bs.eq <= 2 &&
                                 !g.bs.write_alpha;
        if (preblend_src) src = GL_ONE;
        p_glEnable(GL_BLEND);
        p_glBlendFunc(src, dst);
        p_glBlendEquation(eq);
        /* If both sides need a non-trivial, unequal fixed colour the counter
         * above records the case GL's fixed pipeline cannot represent. Equal
         * constants, complementary ones, or one non-trivial constant paired
         * with zero/one, are exact. */
        const uint32_t fx = src_constant ? g.bs.fixa : g.bs.fixb;
        p_glBlendColor((float)( fx        & 0xFF) / 255.0f,
                       (float)((fx >>  8) & 0xFF) / 255.0f,
                       (float)((fx >> 16) & 0xFF) / 255.0f,
                       (float)((fx >> 24) & 0xFF) / 255.0f);
        p_glUniform1i(g.u->preblend_src,
                      double_alpha ? 3 : preblend_src ? (g.bs.dst == 3 ? 2 : 1) : 0);
    } else {
        p_glDisable(GL_BLEND);
        p_glUniform1i(g.u->preblend_src, 0);
    }

    p_glUniform1i(g.u->texenable, g.tex_enable ? 1 : 0);
    p_glUniform1i(g.u->texfunc, g.tex.func);
    p_glUniform1i(g.u->tcc, g.tex.tcc_rgba ? 1 : 0);
    p_glUniform1i(g.u->dbl, g.tex.color_double ? 1 : 0);
    p_glUniform3f(g.u->env, (float)( g.tex.env        & 0xFF) / 255.0f,
                           (float)((g.tex.env >>  8) & 0xFF) / 255.0f,
                           (float)((g.tex.env >> 16) & 0xFF) / 255.0f);
    p_glUniform1i(g.u->tex, 0);
    p_glUniform2f(g.u->texscale, g.tex_sx, g.tex_sy);
    p_glUniform1i(g.u->bloom, g.batch_bloom);
    p_glUniform1i(g.u->minfilter, g.batch_glyph ? g.tex.min_filter & ~1 : g.tex.min_filter);
    p_glUniform1i(g.u->magfilter, g.batch_glyph ? g.tex.mag_filter & ~1 : g.tex.mag_filter);
    p_glUniform1i(g.u->wraps, g.tex.wrap_s ? 1 : 0);
    p_glUniform1i(g.u->wrapt, g.tex.wrap_t ? 1 : 0);
    p_glUniform1i(g.u->miptop, g.bound_top);
    if (g.bound) p_glBindTexture(GL_TEXTURE_2D, g.bound);

    p_glUniform1i(g.u->fogenable, g.fog_enable ? 1 : 0);
    p_glUniform3f(g.u->fogcolour,
                  (float)( g.fog_colour        & 0xFF) / 255.0f,
                  (float)((g.fog_colour >>  8) & 0xFF) / 255.0f,
                  (float)((g.fog_colour >> 16) & 0xFF) / 255.0f);

    p_glUniform1i(g.u->atest, g.bs.alpha_test ? g.bs.alpha_func : 1);
    p_glUniform1i(g.u->aref,  g.bs.alpha_ref);
    p_glUniform1i(g.u->amask, g.bs.alpha_mask);
}

static void apply_placement(const rendertarget *r) {
    const int hud = r->wide && g.batch_class != CLASS_SCENE;
    p_glUseProgram(g.cur_prog);
    p_glViewport(0, 0, r->w, r->h);
    p_glUniform2f(g.u->viewport, (float)r->w, (float)r->h);
    p_glUniform4f(g.u->placement, hud ? r->ui_scale : r->sx,
                  hud ? r->ui_scale : r->sy, hud ? rt_off(r) : 0,
                  hud ? rt_off_y(r) : 0);
    p_glUniform1f(g.u->ybias, 1.0f/256.0f);
}

static void flush(void) {
    if (g.ready && g.mu.npend) flush_model();
    if (!g.ready || g.batch_n == 0) return;
    if (rt_prepare(g.cur_rt) != 0) { g.batch_n = 0; return; }
    rendertarget *r = &g.rts[g.cur_rt];
    gpu_query_begin_frame();
    rt_import(r);
    /* Clear-mode alpha writes invalidate the hardware stencil. Synchronise
     * pending stencil first so a scissored alpha clear preserves the rest. */
    if (g.bs.write_alpha || uses_dest_alpha()) stencil_to_alpha(r);
    if (g.bs.stencil_test) alpha_to_stencil(r);
    p_glBindFramebuffer(GL_FRAMEBUFFER, r->fbo);
    r->dirty = 1;
    /* Retain the aspect diagnostics; apply_placement sets the final viewport
     * and, in window-resolution mode, the physical coordinate transform. */
    if (r->wide && g.batch_class == CLASS_HUD) {
        p_glViewport(rt_off(r), 0, r->guest_w, r->h);
        g.hud_flushes++;
        /* A HUD batch that tests depth is the lock-on reticle, and it meets
         * its target's depth: both sit at the native pixel offset from the
         * centre (see the classification). Counted for the report. A HUD
         * batch reading stencil or destination alpha at an arbitrary place
         * would meet scene pixels placed by the other mapping; counted, not
         * handled: neither game's HUD is expected to do it. */
        if (g.z_test) {
            g.hud_depth_func[g.z_func & 7]++;
            if (g.z_func != 1) g.hud_depth_tests++;   /* ALWAYS reads nothing */
        }
        if (getenv("PSPRECOMP_ASPECT_LOG")) {
            const uint32_t key = g.tex_enable ? g.tex.addr : 0u;
            int k = 0;
            while (k < g.hud_by_tex_n && g.hud_by_tex[k].addr != key) k++;
            if (k == g.hud_by_tex_n && k < 48) {
                g.hud_by_tex_n++;
                g.hud_by_tex[k].addr = key; g.hud_by_tex[k].w = g.tex.w; g.hud_by_tex[k].h = g.tex.h;
                g.hud_by_tex[k].z0 = 1e9f; g.hud_by_tex[k].z1 = -1e9f;
                g.hud_by_tex[k].x0 = 1e9f; g.hud_by_tex[k].x1 = -1e9f;
            }
            if (k < 48) {
                g.hud_by_tex[k].batches++;
                if (g.z_test && g.z_func != 1) g.hud_by_tex[k].tested++;
                for (size_t i = 0; i < g.batch_n; i++) {
                    const float z = g.batch[i * FLOATS_PER_VERT + 2];
                    const float x = g.batch[i * FLOATS_PER_VERT + 0];
                    if (z < g.hud_by_tex[k].z0) g.hud_by_tex[k].z0 = z;
                    if (z > g.hud_by_tex[k].z1) g.hud_by_tex[k].z1 = z;
                    if (x < g.hud_by_tex[k].x0) g.hud_by_tex[k].x0 = x;
                    if (x > g.hud_by_tex[k].x1) g.hud_by_tex[k].x1 = x;
                }
            }
        }
        if (g.z_write) g.hud_depth_writes++;
        if (g.bs.stencil_test) g.hud_hazard_stencil++;
        if (uses_dest_alpha()) g.hud_hazard_dst_alpha++;
    } else {
        p_glViewport(0, 0, rt_scene_w(r), r->h);
    }
    use_program(g.prog, &g.um);
    apply_placement(r);
    apply_state();
    g.mu.disturbed = 0;
    p_glBindVertexArray(g.vao);
    const size_t bbytes = g.batch_n * FLOATS_PER_VERT * sizeof(float);
    const size_t boff = ring_alloc(RING_BATCH, bbytes, FLOATS_PER_VERT * sizeof(float), 0);
    ring_write(RING_BATCH, boff, g.batch, bbytes);
    const GLint bfirst = (GLint)(boff / (FLOATS_PER_VERT * sizeof(float)));
    const int stencil_writes = g.bs.stencil_test && r->fmt == 3 &&
                              (g.bs.op_sfail || g.bs.op_zfail || g.bs.op_zpass);
    if (stencil_writes && uses_dest_alpha()) {
        /* Later overlapping primitives must blend against the updated alpha,
         * not the value at the start of the batch. Only this dependency needs
         * a synchronisation per triangle; ordinary stencil batches stay batched. */
        for (size_t i = 0; i < g.batch_n; i += 3) {
            if (i) {
                stencil_to_alpha(r);
                apply_placement(r);
                apply_state();
            }
            p_glDrawArrays(GL_TRIANGLES, bfirst + (GLint)i, 3);
            r->alpha_dirty = 1;
        }
    } else {
        p_glDrawArrays(GL_TRIANGLES, bfirst, (GLsizei)g.batch_n);
        if (stencil_writes) r->alpha_dirty = 1;
    }
    if (g.bs.write_alpha) { r->stencil_valid = 0; r->alpha_dirty = 0; }
    g.batch_n = 0;
}

static void gl_finish(void) {
    if (claim() != 0) return;
    flush_ends(FB_FINISH); flush();
}

/* The guest-sized staging target a wide attachment is resolved into before a
 * readback. One, resized when the guest extent changes. */
static int stage_prepare(int w, int h) {
    if (g.stage_fbo && g.stage_w == w && g.stage_h == h) return 0;
    if (!g.stage_fbo) {
        p_glGenFramebuffers(1, &g.stage_fbo);
        p_glGenTextures(1, &g.stage_tex);
    }
    p_glBindTexture(GL_TEXTURE_2D, g.stage_tex);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                   GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    p_glBindFramebuffer(GL_FRAMEBUFFER, g.stage_fbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, g.stage_tex, 0);
    /* The sampler binding was borrowed; the next flush rebinds g.bound too. */
    if (g.bound) p_glBindTexture(GL_TEXTURE_2D, g.bound);
    if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "gl: the aspect staging target is incomplete\n");
        return -1;
    }
    g.stage_w = w; g.stage_h = h;
    return 0;
}

/* Read one colour attachment back into the guest's framebuffer.
 *
 * This is what keeps the rest of the project working. score_frame,
 * dump_frame_seq, dump_framebuffer, survey_vram and the frame comparison that
 * passed the M2 gate all read guest memory, and render-target aliases need the
 * actual PSP bytes before the shared texture decoder can reinterpret them.
 *
 * One glReadPixels for the whole surface matters: the old row-at-a-time path
 * forced 272 GPU/CPU synchronisation points at every flip, which made complex
 * mission frames disproportionately slow.  Rows are flipped and packed on the
 * CPU after that single transfer. */
/* ---- the readback ----------------------------------------------------------
 *
 * Every presented frame is read back into the guest framebuffer, because the
 * guest and the runtime's instruments read that memory. Synchronously that is
 * a glReadPixels per frame, which waits for the frame's GPU work and copies
 * it: 0.26 ms a frame at native, 0.69 ms at 1080p, measured. With
 * PSPRECOMP_GL_READBACK=async the pixels go into a persistently mapped
 * pixel-pack ring behind a fence and are copied into guest memory at the
 * next present, when the fence has long signalled; guest memory then holds
 * the previous frame between presents, which the instruments in display.c
 * see as a one-frame lag. A synchronous readback of a target -- a render
 * target about to be sampled as a texture, or shutdown -- completes what is
 * pending for it first, so an older frame never lands after a newer one. */
static void readback_copy(rendertarget *r, const uint8_t *pixels, int rw, int rh, int i) {
    const int bpp = r->fmt == 3 ? 4 : 2;
    const size_t bytes = (size_t)r->stride * (size_t)r->guest_h * (size_t)bpp;
    void *dst = psp_mem_ptr(r->addr, bytes);
    if (!dst) return;
    uint8_t *out = (uint8_t *)dst;
    const int copy_w = rw < (int)r->stride ? rw : (int)r->stride;
    for (int y = 0; y < rh; y++) {
        const uint8_t *row = pixels + (size_t)(rh - 1 - y) * (size_t)rw * 4u;
        if (r->fmt == 3) {
            memcpy(out + (size_t)y * r->stride * 4u, row, (size_t)copy_w * 4u);
            continue;
        }
        uint16_t *row16 = (uint16_t *)(out + (size_t)y * r->stride * 2u);
        for (int x = 0; x < copy_w; x++) {
            const uint32_t red   = row[x * 4 + 0];
            const uint32_t green = row[x * 4 + 1];
            const uint32_t blue  = row[x * 4 + 2];
            const uint32_t alpha = row[x * 4 + 3];
            if (r->fmt == 0)
                row16[x] = (uint16_t)((red >> 3) | ((green >> 2) << 5) |
                                      ((blue >> 3) << 11));
            else if (r->fmt == 1)
                row16[x] = (uint16_t)((red >> 3) | ((green >> 3) << 5) |
                                      ((blue >> 3) << 10) | ((alpha >> 7) << 15));
            else
                row16[x] = (uint16_t)((red >> 4) | ((green >> 4) << 4) |
                                      ((blue >> 4) << 8) | ((alpha >> 4) << 12));
        }
    }
    g.exporting = i+1;
    psp_mem_mark_write(r->addr, (uint32_t)bytes);
    g.exporting = 0;
    g.readbacks++;
}
/* Bind the target's pixels for reading at guest size: the wide or scaled
 * target resolved through the stage first. Returns 0 with rw/rh set. */
static int readback_bind(rendertarget *r, int *rw, int *rh) {
    g.mu.disturbed = 1;
    rt_import(r);
    *rw = r->guest_w; *rh = r->guest_h;
    stencil_to_alpha(r);
    if (r->w != *rw || r->h != *rh) {
        if (stage_prepare(*rw, *rh) != 0) return -1;
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, r->fbo);
        p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g.stage_fbo);
        p_glDisable(GL_SCISSOR_TEST);
        p_glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        p_glBlitFramebuffer(0, 0, rt_scene_w(r), r->h, 0, 0, *rw, *rh,
                            GL_COLOR_BUFFER_BIT, GL_NEAREST);
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, g.stage_fbo);
    } else {
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, r->fbo);
    }
    return 0;
}
/* Complete every pending asynchronous readback of target i (or all, i < 0). */
static void readback_complete(int i) {
    for (int k = 0; k < RB_SLOTS; k++) {
        if (!g_rb.pend[k].valid || (i >= 0 && g_rb.pend[k].rt != i)) continue;
        const double t0 = rings_now_us();
        p_glClientWaitSync(g_rb.pend[k].sync, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
        g_rb.wait_us += rings_now_us() - t0;
        p_glDeleteSync(g_rb.pend[k].sync);
        /* Retired before the copy: the copy's own pointer lookup into the
         * target reaches the access observer, which must find nothing left. */
        const int rt = g_rb.pend[k].rt, rw = g_rb.pend[k].rw, rh = g_rb.pend[k].rh;
        const size_t off = g_rb.pend[k].off;
        g_rb.pend[k].valid = 0; g_rb.completed++;
        readback_watch();
        readback_copy(&g.rts[rt], g_rb.map + off, rw, rh, rt);
    }
}
/* Land pixels in flight before the guest touches their bytes. Reached from
 * psp_mem_ptr for any access into the watched range: a load or store by the
 * recompiled code, the movie decoder's writes, a block copy, an instrument
 * dumping the frame, the texture decoder. What the guest then sees is what
 * a synchronous readback would have left there, and the readback's wait is
 * paid only by the frames somebody actually looks at. */
static void rt_guest_access(uint32_t addr, uint32_t size) {
    for (int k = 0; k < RB_SLOTS; k++) {
        if (!g_rb.pend[k].valid) continue;
        const rendertarget *r = &g.rts[g_rb.pend[k].rt];
        const uint64_t end = (uint64_t)r->addr + (uint64_t)r->stride * r->guest_h * (r->fmt == 3 ? 4 : 2);
        if ((uint64_t)addr + size <= r->addr || addr >= end) continue;
        g_rb.demanded++;
        readback_complete(g_rb.pend[k].rt);
    }
}
/* The observer is armed over the union of the targets in flight, and not at
 * all when nothing is: the common path in psp_mem_ptr then costs a null test. */
static void readback_watch(void) {
    uint64_t lo = UINT64_MAX, hi = 0;
    for (int k = 0; k < RB_SLOTS; k++) {
        if (!g_rb.pend[k].valid) continue;
        const rendertarget *r = &g.rts[g_rb.pend[k].rt];
        const uint64_t end = (uint64_t)r->addr + (uint64_t)r->stride * r->guest_h * (r->fmt == 3 ? 4 : 2);
        if (r->addr < lo) lo = r->addr;
        if (end > hi) hi = end;
    }
    if (hi > lo) psp_mem_set_vram_access_observer(rt_guest_access, (uint32_t)lo, (uint32_t)(hi > UINT32_MAX ? UINT32_MAX : hi));
    else psp_mem_set_vram_access_observer(NULL, 0, 0);
}
/* Start an asynchronous readback of target i into the next ring slot. */
static void readback_issue(int i) {
    rendertarget *r = &g.rts[i];
    if (!r->configured || !r->addr || !r->stride) return;
    int rw, rh;
    if (readback_bind(r, &rw, &rh) != 0) return;
    if ((size_t)rw * (size_t)rh * 4u > RB_SLOT_BYTES) { readback_rt(i); return; }
    const int k = g_rb.next++ % RB_SLOTS;
    if (g_rb.pend[k].valid) readback_complete(g_rb.pend[k].rt);
    p_glBindBuffer(GL_PIXEL_PACK_BUFFER, g_rb.pbo);
    p_glReadPixels(0, 0, rw, rh, GL_RGBA, GL_UNSIGNED_BYTE, (void *)(uintptr_t)((size_t)k * RB_SLOT_BYTES));
    p_glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    g_rb.pend[k].sync = p_glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    g_rb.pend[k].rt = i; g_rb.pend[k].rw = rw; g_rb.pend[k].rh = rh;
    g_rb.pend[k].off = (size_t)k * RB_SLOT_BYTES; g_rb.pend[k].valid = 1;
    g_rb.issued++;
    readback_watch();
}
static void readback_rt(int i) {
    rendertarget *r = &g.rts[i];
    if (!r->configured || !r->addr || !r->stride) return;
    if (g_rb.async) readback_complete(i);
    static uint8_t *pixels;
    static size_t capacity;
    int rw, rh;
    const uint64_t readback_t0 = psp_os_mono_ns();
    if (readback_bind(r, &rw, &rh) != 0) return;
    const size_t need = (size_t)rw * (size_t)rh * 4u;
    if (need > capacity) {
        uint8_t *larger = realloc(pixels, need);
        if (!larger) return;
        pixels = larger;
        capacity = need;
    }
    p_glReadPixels(0, 0, rw, rh, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    readback_copy(r, pixels, rw, rh, i);
    g.readback_ns += psp_os_mono_ns() - readback_t0;
}

static void note_frame_time(void) {
    const uint64_t now = psp_os_mono_ns();
    if (!g.frames) {
        g.frame_first_ns = now;
    } else {
        const uint64_t gap = now - g.frame_prev_ns;
        uint64_t ms = gap / UINT64_C(1000000);
        if (ms > 255) ms = 255;
        g.frame_ms[ms]++;
        if (gap > g.frame_max_ns) g.frame_max_ns = gap;
    }
    g.frame_prev_ns = g.frame_last_ns = now;
    g.frames++;
    present_note_frame();
}

/* PSPRECOMP_GL_SHOT=<prefix> writes every Nth presented *window* image as
 * <prefix>-NNNN.ppm, N from PSPRECOMP_GL_SHOT_EVERY (default 30), starting
 * at window image PSPRECOMP_GL_SHOT_FROM (default 0). The frame dumps in
 * boot.c and display.c read guest memory, which in this backend is the
 * readback -- never the drawable. This is the only view of what the window
 * actually shows, and it is what a text-sharpness claim rests on. The
 * aspect log names the window image a draw lands in by this count, which is
 * not the present count: a present need not render a new image. */
static void gl_shot(int draw_w, int draw_h, int dialog) {
    static const char *prefix;
    static int every = -1;
    static unsigned from;
    if (every < 0) {
        prefix = getenv("PSPRECOMP_GL_SHOT");
        if (prefix && !*prefix) prefix = NULL;
        const char *e = getenv("PSPRECOMP_GL_SHOT_EVERY");
        every = e && *e ? atoi(e) : 30;
        if (every < 1) every = 1;
        const char *f = getenv("PSPRECOMP_GL_SHOT_FROM");
        from = f && *f ? (unsigned)strtoul(f, NULL, 0) : 0;
    }
    if (!prefix || draw_w <= 0 || draw_h <= 0) return;
    const unsigned idx = shot_counters[dialog ? 1 : 0]++;
    if (idx < from || idx % (unsigned)every) return;
    uint8_t *px = malloc((size_t)draw_w * (size_t)draw_h * 4u);
    uint8_t *row = malloc((size_t)draw_w * 3u);
    if (px && row) {
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        p_glReadPixels(0, 0, draw_w, draw_h, GL_RGBA, GL_UNSIGNED_BYTE, px);
        char path[512];
        snprintf(path, sizeof path, "%s%s-%04u.ppm", prefix, dialog ? "-dialog" : "", idx / (unsigned)every);
        FILE *f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", draw_w, draw_h);
            for (int y = draw_h - 1; y >= 0; y--) {
                const uint8_t *src = px + (size_t)y * (size_t)draw_w * 4u;
                for (int x = 0; x < draw_w; x++) {
                    row[x * 3 + 0] = src[x * 4 + 0];
                    row[x * 3 + 1] = src[x * 4 + 1];
                    row[x * 3 + 2] = src[x * 4 + 2];
                }
                fwrite(row, 1, (size_t)draw_w * 3u, f);
            }
            const int failed = ferror(f);
            const int closed = fclose(f);
            if (!failed && !closed)
                fprintf(stderr, "gl-shot: image %u poll %u %dx%d %s\n",
                        idx, psp_ctrl_polls(), draw_w, draw_h, path);
            else
                fprintf(stderr, "gl-shot: failed to write %s\n", path);
        } else fprintf(stderr, "gl-shot: failed to open %s\n", path);
    } else fprintf(stderr, "gl-shot: failed to allocate capture pixels\n");
    free(px);
    free(row);
}

/* Overlay GL resources and calls belong exclusively to the GE owner thread. */
static GLuint dialog_program, dialog_texture, dialog_vao;
static uint64_t dialog_revision;
static uint32_t dialog_pixels[SAVE_DIALOG_W * SAVE_DIALOG_H];
static int dialog_failed;
/* The utility replaces the picture: the overlay is fitted into the game's
 * picture box, and the box is blacked out first so a wider game image does
 * not show beside the 480:272 dialog. */
static void gl_dialog_overlay(int x, int y, int width, int height) {
    uint64_t previous=dialog_revision;
    if (!save_dialog_copy_pixels(dialog_pixels,&dialog_revision) || dialog_failed) return;
    GLint old_program,old_texture,old_vao,viewport[4];
    p_glGetIntegerv(GL_CURRENT_PROGRAM,&old_program);
    p_glGetIntegerv(GL_TEXTURE_BINDING_2D,&old_texture);
    p_glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&old_vao);
    p_glGetIntegerv(GL_VIEWPORT,viewport);
    if (!dialog_program) {
        GLuint vs=compile(GL_VERTEX_SHADER,
            "#version 330 core\n"
            "out vec2 uv; void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);"
            "gl_Position=vec4(p*2.-1.,0,1);uv=vec2(p.x,1.-p.y);}","savedata vertex");
        GLuint fs=compile(GL_FRAGMENT_SHADER,
            "#version 330 core\n"
            "in vec2 uv; uniform sampler2D tex; out vec4 color;"
            "void main(){color=texture(tex,uv);}","savedata fragment");
        if (!vs || !fs) { dialog_failed=1; return; }
        dialog_program=p_glCreateProgram();
        p_glAttachShader(dialog_program,vs); p_glAttachShader(dialog_program,fs);
        p_glLinkProgram(dialog_program); p_glDeleteShader(vs); p_glDeleteShader(fs);
        GLint ok=0; p_glGetProgramiv(dialog_program,GL_LINK_STATUS,&ok);
        if (!ok) { dialog_failed=1; return; }
        p_glGenVertexArrays(1,&dialog_vao); p_glGenTextures(1,&dialog_texture);
        p_glBindTexture(GL_TEXTURE_2D,dialog_texture);
        p_glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
        p_glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        p_glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        p_glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        p_glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,SAVE_DIALOG_W,SAVE_DIALOG_H,0,GL_RGBA,GL_UNSIGNED_BYTE,dialog_pixels);
    } else {
        p_glBindTexture(GL_TEXTURE_2D,dialog_texture);
        if (previous!=dialog_revision)
            p_glTexSubImage2D(GL_TEXTURE_2D,0,0,0,SAVE_DIALOG_W,SAVE_DIALOG_H,GL_RGBA,GL_UNSIGNED_BYTE,dialog_pixels);
    }
    int w=width,h=(int)((int64_t)width*SAVE_DIALOG_H/SAVE_DIALOG_W);
    if (h>height) { h=height; w=(int)((int64_t)h*SAVE_DIALOG_W/SAVE_DIALOG_H); }
    p_glEnable(GL_SCISSOR_TEST); p_glScissor(x,y,width,height);
    p_glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    p_glClearColor(0.0f,0.0f,0.0f,1.0f); p_glClear(GL_COLOR_BUFFER_BIT);
    p_glViewport(x+(width-w)/2,y+(height-h)/2,w,h);
    p_glDisable(GL_DEPTH_TEST); p_glDisable(GL_STENCIL_TEST); p_glDisable(GL_SCISSOR_TEST);
    p_glDisable(GL_CULL_FACE); p_glEnable(GL_BLEND);
    p_glBlendEquation(GL_FUNC_ADD); p_glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    p_glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    p_glUseProgram(dialog_program); p_glBindVertexArray(dialog_vao);
    p_glDrawArrays(GL_TRIANGLES,0,3);
    p_glUseProgram((GLuint)old_program); p_glBindVertexArray((GLuint)old_vao);
    p_glBindTexture(GL_TEXTURE_2D,(GLuint)old_texture);
    p_glViewport(viewport[0],viewport[1],viewport[2],viewport[3]);
    /* Both draw paths reapply their PSP state; model draws normally cache it. */
    g.mu.disturbed=1;
}
/* The in-game menu (src/host/overlay.c), over everything: a frame the SDL
 * thread laid out with Dear ImGui (src/host/ui.h), drawn here because the
 * context is this thread's. Its textures -- the font atlas -- arrive as
 * create, update and destroy requests, taken in order before the frame that
 * uses them. One shader and a scissor per command, on this file's own GL
 * loader. Nothing of it reaches a render target or guest memory. */
enum { UI_TEXTURES_MAX = 32 };
static GLuint ui_program, ui_vao, ui_vbo, ui_ibo;
static GLint ui_display;
static int ui_failed;
static struct { uint32_t id; GLuint tex; } ui_textures[UI_TEXTURES_MAX];

static GLuint ui_texture(uint32_t id) {
    for (int i = 0; i < UI_TEXTURES_MAX; i++) if (ui_textures[i].id == id) return ui_textures[i].tex;
    return 0;
}

static void ui_take(const psp_ui_texture_op *ops, int count) {
    for (int k = 0; k < count; k++) {
        const psp_ui_texture_op *op = &ops[k];
        if (op->op == PSP_UI_TEXTURE_CREATE) {
            int slot = -1;
            for (int i = 0; i < UI_TEXTURES_MAX && slot < 0; i++) if (!ui_textures[i].id) slot = i;
            if (slot < 0 || !op->pixels) { fprintf(stderr, "gl: menu texture %u not created\n", op->texture); continue; }
            GLuint tex = 0;
            p_glGenTextures(1, &tex);
            p_glBindTexture(GL_TEXTURE_2D, tex);
            p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, op->w, op->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, op->pixels);
            ui_textures[slot].id = op->texture;
            ui_textures[slot].tex = tex;
        } else if (op->op == PSP_UI_TEXTURE_UPDATE) {
            const GLuint tex = ui_texture(op->texture);
            if (!tex || !op->pixels) continue;
            p_glBindTexture(GL_TEXTURE_2D, tex);
            p_glTexSubImage2D(GL_TEXTURE_2D, 0, op->x, op->y, op->w, op->h, GL_RGBA, GL_UNSIGNED_BYTE, op->pixels);
        } else {
            for (int i = 0; i < UI_TEXTURES_MAX; i++)
                if (ui_textures[i].id == op->texture) {
                    p_glDeleteTextures(1, &ui_textures[i].tex);
                    ui_textures[i].id = 0; ui_textures[i].tex = 0;
                }
        }
    }
}

static int ui_setup(void) {
    if (ui_program) return 0;
    if (ui_failed) return -1;
    GLuint vs = compile(GL_VERTEX_SHADER,
        "#version 330 core\n"
        "layout(location=0) in vec2 pos; layout(location=1) in vec2 uv; layout(location=2) in vec4 col;"
        "uniform vec2 display; out vec2 v_uv; out vec4 v_col;"
        "void main(){v_uv=uv;v_col=col;"
        "gl_Position=vec4(pos.x/display.x*2.-1.,1.-pos.y/display.y*2.,0,1);}", "menu vertex");
    GLuint fs = compile(GL_FRAGMENT_SHADER,
        "#version 330 core\n"
        "in vec2 v_uv; in vec4 v_col; uniform sampler2D tex; out vec4 color;"
        "void main(){color=v_col*texture(tex,v_uv);}", "menu fragment");
    if (!vs || !fs) { ui_failed = 1; return -1; }
    ui_program = p_glCreateProgram();
    p_glAttachShader(ui_program, vs); p_glAttachShader(ui_program, fs);
    p_glLinkProgram(ui_program); p_glDeleteShader(vs); p_glDeleteShader(fs);
    GLint ok = 0; p_glGetProgramiv(ui_program, GL_LINK_STATUS, &ok);
    if (!ok) { ui_failed = 1; ui_program = 0; return -1; }
    ui_display = p_glGetUniformLocation(ui_program, "display");
    GLint old_vao, old_buffer;
    p_glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &old_vao);
    p_glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &old_buffer);
    p_glGenVertexArrays(1, &ui_vao); p_glGenBuffers(1, &ui_vbo); p_glGenBuffers(1, &ui_ibo);
    p_glBindVertexArray(ui_vao);
    p_glBindBuffer(GL_ARRAY_BUFFER, ui_vbo);
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ui_ibo);
    p_glEnableVertexAttribArray(0); p_glEnableVertexAttribArray(1); p_glEnableVertexAttribArray(2);
    p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(psp_ui_vertex), (void *)0);
    p_glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(psp_ui_vertex), (void *)8);
    p_glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(psp_ui_vertex), (void *)16);
    p_glBindVertexArray((GLuint)old_vao);
    p_glBindBuffer(GL_ARRAY_BUFFER, (GLuint)old_buffer);
    return 0;
}

static void gl_ui_overlay(int draw_w, int draw_h) {
    const psp_ui_texture_op *ops = NULL;
    int count = 0;
    const psp_ui_frame *f = present_ui_lock(&ops, &count);
    if ((!f && !count) || ui_setup()) { present_ui_unlock(); return; }
    GLint old_program, old_texture, old_vao, old_buffer, viewport[4];
    p_glGetIntegerv(GL_CURRENT_PROGRAM, &old_program);
    p_glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture);
    p_glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &old_vao);
    p_glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &old_buffer);
    p_glGetIntegerv(GL_VIEWPORT, viewport);
    ui_take(ops, count);
    if (f && f->command_count && f->width > 0 && f->height > 0) {
        /* Laid out for the drawable as the SDL thread last saw it; a resize
         * since stretches it for a frame. */
        const float sx = (float)draw_w / (float)f->width, sy = (float)draw_h / (float)f->height;
        p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        p_glViewport(0, 0, draw_w, draw_h);
        p_glDisable(GL_DEPTH_TEST); p_glDisable(GL_STENCIL_TEST); p_glDisable(GL_CULL_FACE);
        p_glEnable(GL_BLEND); p_glBlendEquation(GL_FUNC_ADD);
        p_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        p_glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        p_glEnable(GL_SCISSOR_TEST);
        p_glUseProgram(ui_program);
        p_glUniform2f(ui_display, (float)f->width / f->scale_x, (float)f->height / f->scale_y);
        p_glBindVertexArray(ui_vao);
        p_glBindBuffer(GL_ARRAY_BUFFER, ui_vbo);
        p_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)f->vertex_count * sizeof(psp_ui_vertex), f->vertices, GL_STREAM_DRAW);
        p_glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)f->index_count * sizeof(uint16_t), f->indices, GL_STREAM_DRAW);
        for (uint32_t i = 0; i < f->command_count; i++) {
            const psp_ui_command *c = &f->commands[i];
            const int x0 = (int)(c->clip[0] * sx), y0 = (int)(c->clip[1] * sy);
            const int x1 = (int)(c->clip[2] * sx), y1 = (int)(c->clip[3] * sy);
            if (x1 <= x0 || y1 <= y0 || !c->count) continue;
            p_glScissor(x0, draw_h - y1, x1 - x0, y1 - y0);
            p_glBindTexture(GL_TEXTURE_2D, ui_texture(c->texture));
            p_glDrawElementsBaseVertex(GL_TRIANGLES, (GLsizei)c->count, GL_UNSIGNED_SHORT,
                                       (void *)(uintptr_t)(c->first_index * sizeof(uint16_t)), (GLint)c->first_vertex);
        }
        p_glDisable(GL_SCISSOR_TEST);
    }
    present_ui_unlock();
    p_glUseProgram((GLuint)old_program); p_glBindVertexArray((GLuint)old_vao);
    p_glBindBuffer(GL_ARRAY_BUFFER, (GLuint)old_buffer);
    p_glBindTexture(GL_TEXTURE_2D, (GLuint)old_texture);
    p_glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    /* Both draw paths reapply their PSP state; model draws normally cache it. */
    g.mu.disturbed = 1;
}

static void gl_compose(int dialog_redraw) {
    /* The current target, scaled into the window's physical GL drawable. SDL
     * window sizes are logical pixels on a high-DPI desktop; blitting to the
     * fixed 960x544 logical size therefore occupied only the lower-left
     * quarter of a 1920x1088 drawable. Preserve aspect ratio for arbitrary
     * resizes and clear the letterbox before the blit. */
    int draw_w = 0, draw_h = 0;
    present_gl_drawable_size(&draw_w, &draw_h);
    if (draw_w <= 0) draw_w = g.w * 2;
    if (draw_h <= 0) draw_h = g.h * 2;
    const int shown_i = rt_shown();
    if (shown_i == g.cur_rt && rt_prepare(g.cur_rt) != 0) {
        return;
    }
    rendertarget *shown = &g.rts[shown_i];
    if (!shown->configured) return;
    rt_import(shown);
    /* The picture is 480 guest pixels wide, or the virtual width they were
     * spread over. The fit is uniform either way: a wide target has the
     * window's shape by construction, so its letterbox is only the rounding. */
    const int pic_w = shown->wide ? shown->wide_w : g.w;
    const int pic_h = shown->wide ? shown->wide_h : g.h;
    const int src_w = shown->display ? shown->visible_w : (shown->w < g.w ? shown->w : g.w);
    const int src_h = shown->display ? shown->visible_h : (shown->h < g.h ? shown->h : g.h);
    const int fit_w = g.resolution ? src_w : pic_w;
    const int fit_h = g.resolution ? src_h : pic_h;
    int out_w = draw_w;
    int out_h = (int)((long long)draw_w * fit_h / fit_w);
    if (out_h > draw_h) {
        out_h = draw_h;
        out_w = (int)((long long)draw_h * fit_w / fit_h);
    }
    const int out_x = (draw_w - out_w) / 2;
    const int out_y = (draw_h - out_h) / 2;
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, shown->fbo);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    p_glDisable(GL_SCISSOR_TEST);
    p_glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    p_glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    p_glClear(GL_COLOR_BUFFER_BIT);
    p_glBlitFramebuffer(0, shown->h-src_h, src_w, shown->h,
                        out_x, out_y, out_x + out_w, out_y + out_h,
                        GL_COLOR_BUFFER_BIT, GL_LINEAR);
    gl_dialog_overlay(out_x,out_y,out_w,out_h);
    gl_ui_overlay(draw_w,draw_h);
    gl_shot(draw_w,draw_h,dialog_redraw);
}
/* Called from the guest's SavedataUpdate while the dialog is open, so the
 * window keeps showing it even when the game presents no new frames. Only
 * the GL owner thread may draw: another thread skips (0) rather than fails
 * (-1), so a title that polls the utility from a second thread keeps its
 * dialog through the game's own presents. Composes are paced to the display
 * so a busy poll loop does not spend the scheduler token on swaps. */
static int gl_dialog_redraw(void) {
    if (dialog_failed || g.failed) return -1;
    if (!g.ready) return 0;
    if (g.thread!=this_thread()) return 0;
    static double last_us;
    const double now_us = rings_now_us();
    if (last_us && now_us - last_us < 15000.0) return 0;
    last_us = now_us;
    GLint viewport[4]; p_glGetIntegerv(GL_VIEWPORT,viewport);
    gl_compose(1); present_gl_swap();
    p_glViewport(viewport[0],viewport[1],viewport[2],viewport[3]);
    if (g.rts[g.cur_rt].configured)
        p_glBindFramebuffer(GL_FRAMEBUFFER,g.rts[g.cur_rt].fbo);
    /* The compose rebound framebuffers, scissor, masks and the clear colour
     * whether or not the overlay drew: the model path must reapply. */
    g.mu.disturbed=1;
    return dialog_failed ? -1 : 0;
}
/* The same while the guest is held (psprecomp/safepoint.h): the safe point
 * is on the thread that last ran a display list, which owns the context, and
 * calls this about once a display refresh. */
static void gl_pause_redraw(void) { (void)gl_dialog_redraw(); }

static void gl_present(void) {
    if (claim() != 0) return;
    g.presents++;
    flush_ends(FB_PRESENT); flush();
    /* PSPRECOMP_GL_FRAME_LOG=1: what each presented frame drew, as deltas of
     * the run counters -- the way an inserted high-FPS frame is compared with
     * the simulated one before it. */
    if (getenv("PSPRECOMP_GL_FRAME_LOG")) {
        static uint64_t d, sc, hu, md, mb, gl_, gc, gv;
        const uint64_t cmds = psp_ge_command_count(), vtx = psp_ge_vertex_count();
        fprintf(stderr, "frame-log present %llu: ge cmds %llu verts %llu; draws %llu scene %llu hud %llu model %llu/%llu glyph %llu\n",
                (unsigned long long)g.presents, (unsigned long long)(cmds - gc), (unsigned long long)(vtx - gv),
                (unsigned long long)(g.draws - d),
                (unsigned long long)(g.class_scene - sc), (unsigned long long)(g.class_hud - hu),
                (unsigned long long)(g.model_draws - md), (unsigned long long)(g.mu.batches - mb),
                (unsigned long long)(g.glyph_draws - gl_));
        d = g.draws; sc = g.class_scene; hu = g.class_hud; md = g.model_draws; mb = g.mu.batches; gl_ = g.glyph_draws; gc = cmds; gv = vtx;
    }
    /* Count deferred alpha/stencil transfers inside the GPU frame timer. */
    for (int i = 0; i < g.n_rts; i++) stencil_to_alpha(&g.rts[i]);

    gl_compose(0);
    gpu_query_end_frame();
    present_gl_swap();
    rings_fence();

    /* Read every target that has been drawn into since the last flip. The
     * instruments in display.c and boot.c all read guest memory, and which
     * buffer they read is not this backend's to know -- so all of them are
     * made true rather than guessing at one. */
    int rendered = 0;
    const double rb_t0 = g.mu.profile ? rings_now_us() : 0;
    for (int i = 0; i < g.n_rts; i++) {
        if (!g.rts[i].dirty && !g.rts[i].cpu_pending) continue;
        if (g_rb.async) { readback_complete(i); readback_issue(i); }
        else readback_rt(i);
        g.rts[i].dirty = 0;
        rendered = 1; g.mu.readbacks++;
    }
    if (g.mu.profile) g.mu.t_readback += rings_now_us() - rb_t0;
    gpu_query_poll();

    for (int i=0; i<g.n_rts; i++) g.rts[i].scene_drawn = 0;
    resolution_size();
    rt_resize_all();
    if (g.rts[g.cur_rt].configured)
        p_glBindFramebuffer(GL_FRAMEBUFFER, g.rts[g.cur_rt].fbo);
    /* Last Raven calls sceDisplaySetFrameBuf twice per rendered frame. Counting
     * both callbacks produced a fictitious 63 fps with alternating 12/20 ms
     * intervals; a dirty target is the evidence that new work was presented. */
    if (rendered) note_frame_time();
}

unsigned char *render_gl_capture(uint32_t addr, int *w, int *h) {
    if (claim() != 0) return NULL;
    flush_ends(FB_PRESENT); flush();
    for (int i = 0; i < g.n_rts; i++) {
        rendertarget *r = &g.rts[i];
        if (!r->configured || r->addr != (addr & PSP_ADDR_MASK)) continue;
        rt_import(r);
        stencil_to_alpha(r);
        uint8_t *out = malloc((size_t)r->w * r->h * 4), *row = malloc((size_t)r->w * 4);
        if (!out || !row) {
            free(out);
            free(row);
            return NULL;
        }
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, r->fbo);
        p_glReadPixels(0, 0, r->w, r->h, GL_RGBA, GL_UNSIGNED_BYTE, out);
        for (int y = 0; y < r->h / 2; y++) {
            uint8_t *a = out + (size_t)y * r->w * 4, *b = out + (size_t)(r->h - 1 - y) * r->w * 4;
            memcpy(row, a, (size_t)r->w * 4);
            memcpy(a, b, (size_t)r->w * 4);
            memcpy(b, row, (size_t)r->w * 4);
        }
        free(row);
        *w = r->w;
        *h = r->h;
        return out;
    }
    return NULL;
}

/* Whether the next transformed draw may take the model path: the program
 * built, and not the one case flush() handles triangle by triangle (stencil
 * writes with a destination-alpha blend), which the CPU path keeps. */
static int gl_model_ok(void) {
    if (!g.ready || g.failed || g.model_unavailable || !g.prog_model) return 0;
    const rendertarget *r = &g.rts[g.cur_rt];
    const int stencil_writes = g.bs.stencil_test && r->fmt == 3 &&
                              (g.bs.op_sfail || g.bs.op_zfail || g.bs.op_zpass);
    if (stencil_writes && uses_dest_alpha()) return 0;
    return 1;
}
static double gl_now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e6 + t.tv_nsec / 1e3; }
/* Draw the pending model draws as one multi-draw: their blocks go into one
 * UBO slot, their triangles into the index ring in the PSP's vertex order,
 * and each draw's base vertex points at its records in the vertex ring. GL
 * state is the one they were appended under, applied once. */
static void flush_model(void) {
    if (g.mu.npend == 0) return;
    const int nb = g.mu.npend; g.mu.npend = 0;
    rendertarget *r = &g.rts[g.cur_rt];
    const double t0 = g.mu.profile ? gl_now_us() : 0;
    gpu_query_begin_frame();
    rt_import(r);
    if (g.bs.write_alpha || uses_dest_alpha()) stencil_to_alpha(r);
    if (g.bs.stencil_test) alpha_to_stencil(r);
    p_glBindFramebuffer(GL_FRAMEBUFFER, r->fbo);
    r->dirty = 1;
    use_program(g.prog_model, &g.ug);
    /* Placement owns the viewport too. Resetting it to SCENE here would
     * undo a cached PREVIEW placement after a model batch fills at PSP
     * resolution, where centering is implemented by the GL viewport. */
    if (g.mu.disturbed || g.mu.applied_gen != g.mu.state_gen || g.mu.applied_rt != g.cur_rt) {
        apply_placement(r);
        apply_state();
        g.mu.applied_gen = g.mu.state_gen; g.mu.applied_rt = g.cur_rt;
        g.mu.disturbed = 0;
    }
    const double t1 = g.mu.profile ? gl_now_us() : 0;
    /* The blocks, packed: the bound range covers XFORM_MAXB blocks past the
     * offset, so that much is reserved even when fewer were written. */
    const size_t uoff = ring_alloc(RING_UBO, (size_t)nb * sizeof(xform_block), (size_t)g.mu.ubo_align, (size_t)XFORM_MAXB * sizeof(xform_block));
    ring_write(RING_UBO, uoff, g.mu.pend_xb, (size_t)nb * sizeof(xform_block));
    p_glBindBufferRange(GL_UNIFORM_BUFFER, 1, g.mu.ubo, (GLintptr)uoff, (GLsizeiptr)(XFORM_MAXB * sizeof(xform_block)));
    const double t2 = g.mu.profile ? gl_now_us() : 0;
    /* The triangles, in the PSP's order: strips as (t, t+1, t+2), fans as
     * (0, t+1, t+2), lists as they are -- each a prefix of its pattern in
     * the static index buffer. Indices are relative to each draw's base
     * vertex, so they stay under 256. */
    GLsizei counts[XFORM_MAXB]; const void *offs[XFORM_MAXB]; GLint bases[XFORM_MAXB];
    for (int d = 0; d < nb; d++) {
        const int n = g.mu.pend_count[d], prim = g.mu.pend_prim[d];
        size_t pat; int k;
        if (prim == PSP_PRIM_TRIANGLES) { pat = 0; k = 3 * (n / 3); }
        else if (prim == PSP_PRIM_TRIANGLE_STRIP) { pat = MODEL_MAX_VERTS; k = n >= 3 ? 3 * (n - 2) : 0; }
        else { pat = MODEL_MAX_VERTS + 3 * (MODEL_MAX_VERTS - 2); k = n >= 3 ? 3 * (n - 2) : 0; }
        counts[d] = k; bases[d] = g.mu.pend_base[d];
        offs[d] = (const void *)(uintptr_t)(pat * sizeof(GLushort));
    }
    p_glBindVertexArray(g.vao_model);
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.mu.ebo_ring);
    const double t3 = g.mu.profile ? gl_now_us() : 0;
    p_glMultiDrawElementsBaseVertex(GL_TRIANGLES, counts, GL_UNSIGNED_SHORT, offs, nb, bases);
    p_glBindVertexArray(0);
    const int stencil_writes = g.bs.stencil_test && r->fmt == 3 &&
                              (g.bs.op_sfail || g.bs.op_zfail || g.bs.op_zpass);
    if (stencil_writes) r->alpha_dirty = 1;
    if (g.bs.write_alpha) { r->stencil_valid = 0; r->alpha_dirty = 0; }
    g.mu.batches++;
    if (g.mu.profile) { const double t4 = gl_now_us(); g.mu.t_state += t1 - t0; g.mu.t_ubo += t2 - t1; g.mu.t_ebo += t3 - t2; g.mu.t_draw += t4 - t3; }
}
/* One display-list primitive of transformed triangles: appended to the
 * pending batch under the current GL state. The batch is drawn by
 * flush_model, which every state change, CPU-path draw, finish and present
 * reaches through flush(), so the order of drawing is the display list's. */
static void gl_draw_model(int prim, const psp_model_vertex *v, int count, const psp_xform_state *xs) {
    if (claim() != 0) return;
    if (count < 3 || count > MODEL_MAX_VERTS) return;
    if (rt_prepare(g.cur_rt) != 0) return;
    if (g.batch_glyph) { flush_ends(FB_GLYPH); flush(); g.batch_glyph = 0; }
    if (g.batch_bloom) { flush_ends(FB_CLASS); flush(); g.batch_bloom = 0; g.mu.state_gen++; }
    if (g.adaptive_aspect) {
        /* ge.c sends only perspective geometry through draw_model. Affine
         * UI is already classified through the CPU-transform draw path. */
        const int cls = menu_preview(&g.rts[g.cur_rt]) ? CLASS_PREVIEW : CLASS_SCENE;
        if (cls == CLASS_SCENE && g.rts[g.cur_rt].display) g.rts[g.cur_rt].scene_drawn = 1;
        if (cls != g.batch_class) {
            flush_ends(FB_CLASS); flush(); g.batch_class = cls; g.mu.state_gen++;
        }
        if (cls == CLASS_PREVIEW) g.class_preview++;
        else if (cls == CLASS_HUD) g.class_hud++; else g.class_scene++;
    }
    if (g.batch_n) { flush_ends(FB_CPU); flush(); }
    /* A batch ends when its block slots are used up. It may span a wrap of
     * the vertex ring: each draw addresses its own base vertex, the wrap
     * fences and waits like any allocation, and the orphaning fallback below
     * has its own guard. Ending it whenever the ring's head had passed the
     * old single buffer's size ended a third of all batches at one draw. */
    if (g.mu.npend == XFORM_MAXB) { flush_ends(FB_FULL); flush_model(); }
    const double t0 = g.mu.profile ? gl_now_us() : 0;
    rendertarget *r = &g.rts[g.cur_rt];
    xform_block *xb = &g.mu.pend_xb[g.mu.npend];
    /* The scene part of the state is everything but the world and texture
     * matrices and the texture size: bytes [view, tgen) and [lighting, tex_w). */
    const size_t s0 = offsetof(psp_xform_state, view), s1 = offsetof(psp_xform_state, tgen);
    const size_t s2 = offsetof(psp_xform_state, lighting), s3 = offsetof(psp_xform_state, tex_w);
    const int same_scene = g.mu.last_valid &&
        memcmp((const char *)xs + s0, (const char *)&g.mu.last_xs + s0, s1 - s0) == 0 &&
        memcmp((const char *)xs + s2, (const char *)&g.mu.last_xs + s2, s3 - s2) == 0;
    if (same_scene) {
        memcpy(xb, &g.mu.last_xb, sizeof *xb);
        g.mu.scene_hits++;
    } else {
        memset(xb, 0, sizeof *xb);
        for (int c = 0; c < 3; c++) for (int r3 = 0; r3 < 3; r3++) xb->view[c*4+r3] = xs->view[c*3+r3];
        for (int r3 = 0; r3 < 3; r3++) xb->view[12+r3] = xs->view[9+r3];
        xb->view[15] = 1.0f;
        memcpy(xb->proj, xs->proj, sizeof xb->proj);
        for (int i = 0; i < 4; i++) {
            xb->lmeta[i*4] = xs->light[i].enable; xb->lmeta[i*4+1] = xs->light[i].type; xb->lmeta[i*4+2] = xs->light[i].kind;
            memcpy(xb->lpos + i*4, xs->light[i].pos, 12); memcpy(xb->ldir + i*4, xs->light[i].dir, 12);
            memcpy(xb->latten + i*4, xs->light[i].atten, 12);
            xb->lexpcut[i*4] = xs->light[i].exponent; xb->lexpcut[i*4+1] = xs->light[i].cutoff;
            memcpy(xb->lamb + i*4, xs->light[i].amb, 12); memcpy(xb->ldif + i*4, xs->light[i].dif, 12); memcpy(xb->lspec + i*4, xs->light[i].spec, 12);
        }
        memcpy(xb->memis, xs->mat_emissive, 12); memcpy(xb->mamb, xs->mat_ambient, 12);
        memcpy(xb->mdif, xs->mat_diffuse, 12); memcpy(xb->mspec, xs->mat_specular, 12); memcpy(xb->gamb, xs->global_amb, 12);
        xb->coef_fog[0] = xs->mat_spec_coef; xb->coef_fog[1] = xs->fog_end; xb->coef_fog[2] = xs->fog_range;
        xb->flags[0] = xs->lighting; xb->flags[1] = xs->mat_update; xb->flags[2] = xs->mat_alpha; xb->flags[3] = xs->fog_enable;
        xb->texmap_vp[0] = xs->tex_map_mode; xb->texmap_vp[1] = xs->tex_proj_mode; xb->texmap_vp[2] = xs->vp_set; xb->texmap_vp[3] = xs->depth_clamp;
        xb->texsize_off[2] = xs->off_x; xb->texsize_off[3] = xs->off_y;
        xb->vps[0] = xs->vp_xs; xb->vps[1] = xs->vp_ys; xb->vps[2] = xs->vp_zs;
        xb->vpc[0] = xs->vp_xc; xb->vpc[1] = xs->vp_yc; xb->vpc[2] = xs->vp_zc;
        xb->cull_strip[0] = xs->cull_enable; xb->cull_strip[1] = xs->cull_ccw;
        memcpy(&g.mu.last_xs, xs, sizeof g.mu.last_xs);
        memcpy(&g.mu.last_xb, xb, sizeof g.mu.last_xb);
        g.mu.last_valid = 1;
        g.mu.scene_misses++;
    }
    for (int c = 0; c < 3; c++) for (int r3 = 0; r3 < 3; r3++) { xb->world[c*4+r3] = xs->world[c*3+r3]; xb->tgen[c*4+r3] = xs->tgen[c*3+r3]; }
    for (int r3 = 0; r3 < 3; r3++) { xb->world[12+r3] = xs->world[9+r3]; xb->tgen[12+r3] = xs->tgen[9+r3]; }
    xb->world[15] = xb->tgen[15] = 1.0f;
    xb->texsize_off[0] = (float)xs->tex_w; xb->texsize_off[1] = (float)xs->tex_h;
    xb->cull_strip[2] = prim == PSP_PRIM_TRIANGLE_STRIP; xb->cull_strip[3] = g.resolution && r->display;
    xb->lodi[0] = g.tex.lod_mode; xb->lodi[1] = g.tex.lod_bias16;
    /* sample_psp reads the LOD only to pick min over mag filtering and, past
     * that, a mip level; with one level (or no mip filter) and filters of
     * the same kind the value cannot matter, and the geometry stage skips
     * the derivative work. */
    xb->lodi[2] = !g.tex_enable || (((g.tex.min_filter & 1) == (g.tex.mag_filter & 1)) && (g.tex.min_filter < 4 || g.bound_top <= 0));
    xb->guard_slope[0] = xs->vp_set ? xs->off_x : 1808.0f; xb->guard_slope[1] = xs->vp_set ? xs->off_y : 1912.0f; xb->guard_slope[2] = g.tex.lod_slope;
    /* The vertices and their draw index, into the vertex and draw-index
     * rings in lockstep: one base vertex addresses both. */
    if (!g_rings.persistent && g.mu.vbo_head + count > MODEL_RING_VERTS * 4) flush_model();   /* an orphaning wrap must not split a batch */
    const size_t voff = ring_alloc(RING_VBO, (size_t)count * sizeof(psp_model_vertex), sizeof(psp_model_vertex), 0);
    const int base = (int)(voff / sizeof(psp_model_vertex));
    ring_write(RING_VBO, voff, v, (size_t)count * sizeof(psp_model_vertex));
    static GLuint dix[MODEL_MAX_VERTS];
    for (int i = 0; i < count; i++) dix[i] = (GLuint)g.mu.npend;
    g_rings.r[RING_DRAW].head = (size_t)(base + count) * sizeof(GLuint);
    ring_write(RING_DRAW, (size_t)base * sizeof(GLuint), dix, (size_t)count * sizeof(GLuint));
    g.mu.vbo_head = base + count;
    g.mu.pend_count[g.mu.npend] = count; g.mu.pend_base[g.mu.npend] = base; g.mu.pend_prim[g.mu.npend] = prim;
    g.mu.npend++;
    g.draws++; g.verts += (uint64_t)count;
    g.model_draws++; g.model_verts += (uint64_t)count;
    if (g.mu.profile) g.mu.t_append += gl_now_us() - t0;
}

static const psp_render_backend gl_backend = {
    .name = "gl",
    .init = gl_init,
    .shutdown = gl_shutdown,
    .to_memory = gl_to_memory,
    .set_target = gl_target,
    .set_scissor = gl_scissor,
    .set_texture = gl_texture,
    .set_clut = gl_clut,
    .set_depth = gl_depth,
    .set_blend = gl_blend,
    .set_fog = gl_fog,
    .draw = gl_draw,
    .finish = gl_finish,
    .present = gl_present,
    .model_ok = gl_model_ok,
    .draw_model = gl_draw_model,
    .set_viewport = gl_viewport,
    .set_display = gl_display,
};

const psp_render_backend *render_gl_backend(void) { return &gl_backend; }

void render_gl_report(FILE *out) {
    if (!g.ready && !g.failed) return;
    fprintf(out, "gl:       %s", g.failed ? "failed to start" : "ran");
    if (g.ready)
        fprintf(out, " -- %llu draw(s), %llu vertices, %llu readback(s), the context moved %llu time(s)",
                (unsigned long long)g.draws, (unsigned long long)g.verts,
                (unsigned long long)g.readbacks, (unsigned long long)g.moves);
    fprintf(out, "\n          transform: %s, %llu model draw(s) in %llu batch(es), %llu vertices; %llu repeated setter(s) skipped, scene state reused %llu of %llu time(s)",
            g.model_unavailable ? "CPU" : "GPU", (unsigned long long)g.model_draws, (unsigned long long)g.mu.batches, (unsigned long long)g.model_verts, (unsigned long long)g.mu.setters_skipped,
            (unsigned long long)g.mu.scene_hits, (unsigned long long)(g.mu.scene_hits + g.mu.scene_misses));
    {
        static const char *const cause[FB_COUNT] = { "target", "scissor", "texture", "clut", "depth", "blend", "fog", "glyph", "class", "cpu draw", "finish", "present", "full" };
        fprintf(out, "\n          batches ended by:");
        for (int k = 0; k < FB_COUNT; k++) if (g.mu.flush_by[k]) fprintf(out, " %s %llu", cause[k], (unsigned long long)g.mu.flush_by[k]);
    }
    fprintf(out, "\n          rings: %s, %u frame fence(s), %u wait(s) totalling %.1f ms",
            g_rings.persistent ? "persistently mapped" : "glBufferSubData with orphaning", g_rings.fences, g_rings.waits, g_rings.wait_us / 1e3);
    if (g.mu.profile)
        fprintf(out, "\n          model path time: append %.1f ms, state %.1f ms, blocks %.1f ms, indices %.1f ms, draw calls %.1f ms",
                g.mu.t_append / 1e3, g.mu.t_state / 1e3, g.mu.t_ubo / 1e3, g.mu.t_ebo / 1e3, g.mu.t_draw / 1e3);
    fprintf(out, "\n          readback: %s", g_rb.async ? "asynchronous, landed at the next present or on demand" : "synchronous");
    if (g_rb.async) fprintf(out, ", %u issued, %u completed (%u on a guest access), fence waits %.1f ms", g_rb.issued, g_rb.completed, g_rb.demanded, g_rb.wait_us / 1e3);
    if (g.mu.profile)
        fprintf(out, "\n          present readback: %llu target(s) in %.1f ms (%.2f ms each)",
                (unsigned long long)g.mu.readbacks, g.mu.t_readback / 1e3, g.mu.readbacks ? g.mu.t_readback / 1e3 / (double)g.mu.readbacks : 0.0);
    fprintf(out, "\n          targets: %d%s", g.n_rts,
            g.rt_overflow ? " (more than the table holds)" : "");
    if (g.display_targets)
        fprintf(out, ", %llu made for a displayed buffer the GE never drew into",
                (unsigned long long)g.display_targets);
    if (g.rt_shrinks)
        fprintf(out, ", %llu cut back to the next target", (unsigned long long)g.rt_shrinks);
    fprintf(out, ", %llu CPU import(s)", (unsigned long long)g.cpu_uploads);
    for (int i = 0; i < g.n_rts; i++)
        if (g.rts[i].configured)
            fprintf(out, " %08X(%dx%d,s%u,f%d)", g.rts[i].addr,
                    g.rts[i].w, g.rts[i].h, g.rts[i].stride, g.rts[i].fmt);
        else
            fprintf(out, " %08X(unused)", g.rts[i].addr);
    if (g.resolution)
        fprintf(out, "\n          resolution: window %dx%d, %llu resize(s), %llu CPU upload(s), %llu GPU texture view(s), %llu bitmap font draw(s)",
                g.pixel_w, g.pixel_h, (unsigned long long)g.resizes,
                (unsigned long long)g.cpu_uploads, (unsigned long long)g.rt_views,
                (unsigned long long)g.glyph_draws);
    if (g.smooth_bloom)
        fprintf(out, "\n          bloom: smooth, %llu composite draw(s)", (unsigned long long)g.bloom_draws);
    if (g.adaptive_aspect)
        fprintf(out, "\n          aspect: virtual size %dx%d (x%.4f), %llu scene draw(s),"
                     " %llu HUD draw(s) in %llu batch(es), %llu HUD batch(es) depth-tested,"
                     " HUD hazards stencil %llu dst-alpha %llu, %llu wide allocation(s), %llu retired,"
                     " %llu preview draw(s)",
                g.wide_w, g.wide_h, (double)g.wide_w / (double)g.w,
                (unsigned long long)g.class_scene, (unsigned long long)g.class_hud,
                (unsigned long long)g.hud_flushes,
                (unsigned long long)g.hud_depth_tests,
                (unsigned long long)g.hud_hazard_stencil,
                (unsigned long long)g.hud_hazard_dst_alpha,
                (unsigned long long)g.wide_allocs, (unsigned long long)g.wide_retired,
                (unsigned long long)g.class_preview);
    for (int k = 0; k < g.hud_by_tex_n; k++)
        fprintf(out, "\n          HUD tex %08X %dx%d: %llu batch(es), %llu depth-tested, z %.4f..%.4f, x %.1f..%.1f",
                g.hud_by_tex[k].addr, g.hud_by_tex[k].w, g.hud_by_tex[k].h,
                (unsigned long long)g.hud_by_tex[k].batches,
                (unsigned long long)g.hud_by_tex[k].tested,
                g.hud_by_tex[k].z0, g.hud_by_tex[k].z1,
                g.hud_by_tex[k].x0, g.hud_by_tex[k].x1);
    if (g.adaptive_aspect && (g.hud_depth_tests || g.hud_depth_writes)) {
        fprintf(out, "\n          HUD depth: %llu write(s); tests by func",
                (unsigned long long)g.hud_depth_writes);
        for (int f = 0; f < 8; f++)
            if (g.hud_depth_func[f])
                fprintf(out, " %d:%llu", f, (unsigned long long)g.hud_depth_func[f]);
    }
    fprintf(out, "\n          textures: %llu request(s), %llu upload(s), %llu hit(s)"
                 " (%llu immediate, %llu revalidated), %llu dirty invalidation(s),"
                 " %llu miss(es), %llu eviction(s), %d/%d resident, %llu too big, %llu from VRAM,"
                 " %llu sampled from a target, %llu row refresh(es) of %llu row(s)",
            (unsigned long long)g.tex_requests,
            (unsigned long long)g.tex_uploads, (unsigned long long)g.tex_hits,
            (unsigned long long)g.tex_fast_hits,
            (unsigned long long)g.tex_revalidated,
            (unsigned long long)g.tex_invalidations,
            (unsigned long long)g.tex_misses,
            (unsigned long long)g.tex_evictions, g.cache_entries, TEXCACHE_MAX,
            (unsigned long long)g.tex_too_big,
            (unsigned long long)g.tex_vram_uploads,
            (unsigned long long)g.tex_from_rt,
            (unsigned long long)g.tex_row_refreshes, (unsigned long long)g.tex_rows_refreshed);
    if (g.tex_alias_from_rt)
        fprintf(out, ", %llu target alias decode(s)",
                (unsigned long long)g.tex_alias_from_rt);
    if (g.tex_padded_uploads)
        fprintf(out, ", %llu upload(s) with %llu unbacked padding texel(s)",
                (unsigned long long)g.tex_padded_uploads,
                (unsigned long long)g.tex_padded_pixels);
    if (g.mip_chains || g.mip_incomplete) {
        fprintf(out, ", mipmaps: %llu chain upload(s), %llu extra level(s)",
                (unsigned long long)g.mip_chains,
                (unsigned long long)g.mip_levels);
        if (g.mip_incomplete)
            fprintf(out, ", %llu incomplete",
                    (unsigned long long)g.mip_incomplete);
    }
    fprintf(out, "\n          timing: texture bind %.3f s (generation %.3f, decode %.3f,"
                 " upload %.3f; %.1f MiB RGBA), readback %.3f s",
            g.tex_bind_ns / 1.0e9, g.tex_generation_ns / 1.0e9,
            g.tex_decode_ns / 1.0e9, g.tex_upload_ns / 1.0e9,
            g.tex_upload_pixels * 4.0 / (1024.0 * 1024.0),
            g.readback_ns / 1.0e9);
    if (g.gpu_samples) {
        const uint64_t p50_at = (g.gpu_samples + 1) / 2;
        const uint64_t p95_at = (g.gpu_samples * 95 + 99) / 100;
        uint64_t seen = 0;
        int p50 = -1, p95 = -1;
        for (int tenth = 0; tenth < 256; tenth++) {
            seen += g.gpu_tenth_ms[tenth];
            if (p50 < 0 && seen >= p50_at) p50 = tenth;
            if (seen >= p95_at) { p95 = tenth; break; }
        }
        fprintf(out, "\n          gpu: %llu draw+blit sample(s), mean %.2f ms, "
                     "p50 %.1f ms, p95 %.1f ms, max %.2f ms",
                (unsigned long long)g.gpu_samples,
                g.gpu_total_ns / (double)g.gpu_samples / 1.0e6,
                p50 / 10.0, p95 / 10.0, g.gpu_max_ns / 1.0e6);
        if (g.gpu_dropped)
            fprintf(out, ", %llu frame(s) unmeasured because the query ring "
                         "was full",
                    (unsigned long long)g.gpu_dropped);
    }
    if (g.frames > 1 && g.frame_last_ns > g.frame_first_ns) {
        const uint64_t intervals = g.frames - 1;
        const uint64_t p50_at = (intervals + 1) / 2;
        const uint64_t p95_at = (intervals * 95 + 99) / 100;
        uint64_t seen = 0;
        int p50 = -1, p95 = -1;
        for (int ms = 0; ms < 256; ms++) {
            seen += g.frame_ms[ms];
            if (p50 < 0 && seen >= p50_at) p50 = ms;
            if (seen >= p95_at) { p95 = ms; break; }
        }
        const double seconds = (g.frame_last_ns - g.frame_first_ns) / 1.0e9;
        fprintf(out, "\n          frames: %llu rendered / %llu present call(s) over %.3f s"
                     " (%.2f/s), interval p50 %d ms, p95 %d ms, max %.1f ms",
                (unsigned long long)g.frames, (unsigned long long)g.presents, seconds,
                intervals / seconds, p50, p95, g.frame_max_ns / 1.0e6);
    }
    if (g.batch_overflows)
        fprintf(out, ", %llu batch flush(es) from overflow",
                (unsigned long long)g.batch_overflows);
    if (g.unsupported_prims)
        fprintf(out, ", %llu point/line draw(s) skipped",
                (unsigned long long)g.unsupported_prims);
    /* Counted rather than approximated: the doubled blend factors and the
     * absolute-difference equation have no GL equivalent and need the shader.
     * A wrong factor renders a plausible picture; a counted one is a number. */
    if (g.partial_pixel_masks)
        fprintf(out, ", %llu draw(s) with a partial pixel mask written whole",
                (unsigned long long)g.partial_pixel_masks);
    if (g.unsupported_blend_factor || g.unsupported_blend_eq)
        fprintf(out, ", blend not represented: %llu factor, %llu equation",
                (unsigned long long)g.unsupported_blend_factor,
                (unsigned long long)g.unsupported_blend_eq);
    for (int i = 0; i < g.blend_misses; i++)
        fprintf(out, "%s src %d dst %d eq %d fixa %06X fixb %06X: %llu",
                i ? ";" : " (", g.blend_miss[i].src, g.blend_miss[i].dst, g.blend_miss[i].eq,
                (unsigned)g.blend_miss[i].fixa, (unsigned)g.blend_miss[i].fixb,
                (unsigned long long)g.blend_miss[i].draws);
    if (g.blend_misses) fputc(')', out);
    if (g.unsupported_stencil_draws)
        fprintf(out, ", %llu draw(s) need alpha-backed stencil",
                (unsigned long long)g.unsupported_stencil_draws);
    if (g.stencil_draws)
        fprintf(out, "\n          stencil: %llu RGBA8888 draw(s), %llu alpha import(s), %llu export(s)",
                (unsigned long long)g.stencil_draws,
                (unsigned long long)g.stencil_imports, (unsigned long long)g.stencil_exports);
    fprintf(out, "\n");
}

#else   /* no SDL2: there is no window to put a context on */

const psp_render_backend *render_gl_backend(void) { return NULL; }
void render_gl_report(FILE *out) { (void)out; }
unsigned char *render_gl_capture(uint32_t addr, int *w, int *h) {
    (void)addr; (void)w; (void)h; return NULL;
}

#endif
