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

/* A piece of state that is more than kept bytes: a module's, or a title's.
 * refuse says why a save is impossible now, or NULL; save writes its chunks;
 * load runs after memory and the kept variables are back, in the order the
 * parts were registered. Register at start-up, before any save or load. */
typedef struct {
    const char *name;
    const char *(*refuse)(void);
    int (*save)(psp_state_writer *w);
    int (*load)(psp_state_reader *r, char *why, size_t size);
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

/* PSPRECOMP_SAVE_STATE=<poll>:<file>[,<poll>:<file>...] saves at the first
 * safe point at or after each poll where a save is possible, for the checks
 * that a loaded state continues exactly as the run that saved it. Read when
 * the scheduler starts. */
void psp_state_init(void);

#ifdef __cplusplus
}
#endif

#endif
