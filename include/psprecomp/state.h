/* Save states (docs/PLAYER-LAYER.md §5).
 *
 * A state is everything the guest can see -- its memory, its registers, the
 * firmware's objects, the scheduler's queues, the clock -- written at the
 * safe point (psprecomp/safepoint.h), when the thread holding the token has
 * finished a frame-boundary call and every other thread is parked in a
 * firmware call. Loading it into a fresh process continues every thread
 * natively from the return sites on its guest stack (psp_resume_chain), so
 * no host frame needs to survive.
 *
 * A state belongs to the build that wrote it: it carries a hash of the
 * executable and loads into nothing else. That is what lets most of it be
 * plain bytes: the runtime's guest-visible statics, and a title's, each
 * named once with psp_state_keep. A kept variable holds no host
 * pointer, except one into another kept variable (psp_state_delta). What
 * does -- open files, decoders -- is saved and rebuilt by a part
 * (psp_state_part) instead. */
#ifndef PSPRECOMP_STATE_H
#define PSPRECOMP_STATE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct psp_state_writer psp_state_writer;
typedef struct psp_state_reader psp_state_reader;

/* One tagged chunk (a tag of up to 8 characters), and reading it back:
 * NULL when the state has none. */
int         psp_state_put(psp_state_writer *w, const char *tag, const void *data, size_t size);
const void *psp_state_get(psp_state_reader *r, const char *tag, size_t *size);

/* A variable the state holds as it is. Name it at start-up, before any save
 * or load, and in the same order on every run of the build; a name is how a
 * load finds it again. */
void psp_state_keep(const char *name, void *data, size_t size);
#define PSP_STATE_KEEP(var) psp_state_keep(#var, &(var), sizeof (var))

/* A piece of state that is more than kept bytes: a module's, or a title's,
 * or the host's. refuse says why a save is impossible now, or NULL; save
 * writes its chunks; load runs after memory and the kept variables are back,
 * in the order the parts were registered; drop lets go of what the running
 * game holds -- decoders, files, queued audio -- before a state replaces it
 * in the same process. Register at start-up, before any save or load. */
typedef struct {
    const char *name;
    const char *(*refuse)(void);
    int (*save)(psp_state_writer *w);
    int (*load)(psp_state_reader *r, char *why, size_t size);
    void (*drop)(void);
} psp_state_part;
void psp_state_register(const psp_state_part *part);

/* A loaded state's variables sit where this process put them, not where the
 * saving one had them: a kept pointer to a kept variable moves by this. */
intptr_t psp_state_delta(void);
/* Whether a pointer is into a kept variable, so that it can be kept. */
int psp_state_is_kept(const void *p);

/* Why a save would be refused now, or NULL. The safe point's thread only. */
const char *psp_state_refusal(void);

/* Write the state, at the safe point, from the thread holding the token.
 * Returns 0, or -1 with the reason. */
int psp_state_save(const char *path, char *why, size_t size);

/* Load a state into a fresh run: after the module is loaded and registered
 * and the firmware initialised, in place of module_start. Every thread is
 * then ready to continue where the save left it, once the main context
 * drains. Returns 0, or -1 with the reason. */
int psp_state_load(const char *path, char *why, size_t size);

/* Whether this run continues a loaded state. */
int psp_state_loaded(void);

/* Load a state into the running game, from the thread at the safe point:
 * every other guest thread is ended, and every thread of the state continues,
 * this one as the thread that saved it. Does not return. When the load cannot
 * begin, returns -1 with the reason if the game is the obstacle, for now, and
 * -2 if the file is: missing, damaged, or from another build. One that fails
 * after it has begun stops the run, since the game it replaced is gone. */
int psp_state_load_here(const char *path, char *why, size_t size);

/* What a list of states shows, read without loading one: whether this build
 * can load it, when it was saved (seconds since 1970), the guest's time in it
 * -- how long the game had been played -- and its poll count. */
typedef struct {
    int      usable;
    int64_t  saved;
    uint64_t guest_us;
    uint32_t polls;
    int      has_thumb;
} psp_state_info;
enum { PSP_STATE_THUMB_W = 240, PSP_STATE_THUMB_H = 136 };
/* The header, and the thumbnail as RGBA (PSP_STATE_THUMB_W x _H) when `thumb`
 * is not NULL. Returns 0, or -1 when there is no state at `path`. */
int psp_state_peek(const char *path, psp_state_info *info, uint8_t *thumb);

/* Where states are kept: PSPRECOMP_STATE_DIR, which the launcher sets to a
 * folder of the title's own beside its saves, else "states" in the working
 * directory. The menu's slots and the state written on quitting are files
 * there. */
const char *psp_state_dir(void);
/* <dir>/<name>.state: "slot-1" to "slot-10", "quit", "undo". */
void psp_state_file(const char *name, char *out, size_t size);

/* For the host, from any thread: a save or a load at the next safe point.
 * One at a time; a new request replaces one not yet begun. A refused request
 * tries again at each safe point for a second of the game's time, then gives
 * up with the reason. A save asked for while the game is held happens there.
 * Returns the request's number. */
enum { PSP_STATE_SAVE = 1, PSP_STATE_LOAD };
unsigned psp_state_request(int kind, const char *path);
/* The last request to finish: its number (0 for none yet), whether it
 * succeeded, and a sentence for the player. */
unsigned psp_state_result(int *ok, char *message, size_t size);

/* PSPRECOMP_SAVE_STATE=<poll>:<file>[,<poll>:<file>...] saves at the first
 * safe point at or after each poll where a save is possible, for the checks
 * that a loaded state continues exactly as the run that saved it.
 * PSPRECOMP_LOAD_STATE=<poll>:<file>[:<times>] loads <file> into the running
 * game at the first safe point at or after <poll>, <times> times over (1 by
 * default): the state returns the game to its own poll, so a <poll> past it
 * makes a loop, for the checks that loading again and again leaks nothing.
 * Read when the scheduler starts. */
void psp_state_init(void);

#ifdef __cplusplus
}
#endif

#endif
