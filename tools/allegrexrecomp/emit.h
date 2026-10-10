/* The C emitter — turning discovered functions into readable native C.
 *
 * One `psp_func_<addr>` per discovered function, every line carrying its
 * address and original disassembly as a comment, lowered to the helpers in
 * <psprecomp/recomp_rt.h>. The output is meant to be *read*, not merely
 * compiled: a recomp project is only useful to other people if they can open
 * the generated file and see what the original was doing.
 *
 * The hard part is delay slots. Every MIPS branch and jump executes the
 * instruction *after* it before control transfers, and "likely" branches
 * nullify theirs when not taken. See the notes in emit.c — that single
 * detail is where most of this file's care goes.
 */
#ifndef ALLEGREX_EMIT_H
#define ALLEGREX_EMIT_H

#include "analyze.h"
#include "container.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *outdir;     /* directory to write into */
    const char *prefix;     /* file/base name, e.g. "recomp" */
    const char *module;     /* module name, for the file header comment */

    /* The module's import table, so each generated thunk can dispatch to the
     * HLE layer by NID and carry the firmware function's library and NID in a
     * comment. Without it the thunks can only trap. */
    const psp_import_entry *imports;
    int                     nimports;

    /* Addresses the host means to implement itself.
     *
     * For each one the translated body is still emitted, but under
     * `psp_func_<addr>__orig`, and the public `psp_func_<addr>` is left
     * undefined. A hand-written native C function of that name, compiled into
     * the host, then satisfies every call site the emitter wrote -- direct
     * `jal`s included, which no run-time hook can reach because they lower to
     * plain C calls and never touch the dispatch table.
     *
     * This is how a recompilation gets to *change* the game rather than only
     * run it: the original stays callable as `__orig`, so a replacement can
     * defer to it, wrap it, or ignore it. Keeping the body is what makes the
     * mechanism cheap to back out of and what lets one build carry both the
     * stock behaviour and the new one. */
    const uint32_t *replace;
    int             nreplace;

    /* Resume entries (docs/PLAYER-LAYER.md §5, "Resuming natively").
     *
     * Every call's return site becomes an entry of the function containing
     * it: a case in that body's entry switch, without a dispatch thunk of its
     * own. Each function holding one gets a public psp_resume_<addr>(site),
     * and the module a sorted table of {site, resume function} that
     * psp_recomp_register hands to psp_resume_register. A thread whose
     * registers and memory are restored can then continue at its innermost
     * return site and climb its guest call chain from there
     * (psp_resume_chain). Off by default until a save state needs it. */
    int resume;

    /* The registration function's name. NULL is psp_recomp_register, the
     * program's. A module the game loads at run time has one of its own, called
     * when it loads (docs/MODULES.md). */
    const char *register_name;
} emit_opts;

/* Emit <outdir>/<prefix>_funcs.c, <prefix>_funcs.h and <prefix>_imports.c.
 * Returns 0 on success. */
int a_emit(const a_analysis *an, const emit_opts *o);

#ifdef __cplusplus
}
#endif

#endif /* ALLEGREX_EMIT_H */
