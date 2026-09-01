/* allegrexrecomp — the Allegrex interpreter that acts as the bring-up oracle.
 *
 * This exists to answer one question during bring-up: when the recompiled game
 * does something wrong, *where did it first diverge*?
 *
 * The property that makes it useful is not fidelity — it is shared machinery.
 * The interpreter runs on the same decoder (a_decode), the same CPU state
 * (psp_cpu), the same memory model (the psp_read and psp_write family), and
 * the same semantic helpers (recomp_rt.h, vfpu.h) as the recompiled C. The
 * only thing it does differently is sequence instructions itself instead of
 * relying on emitted control flow.
 *
 * So when the interpreter and the recompiled path disagree, the bug is in
 * exactly one of two places: the emitter, or this file's sequencing. Decode,
 * memory, arithmetic and HLE are shared, and therefore cannot be the
 * difference. That narrowing is worth more than accuracy.
 *
 * See docs/ORACLE.md. This is the tier-1 oracle; PPSSPP and pspautotests are
 * tiers 2 and 3, and both are separate processes — never linked.
 */
#ifndef ALLEGREX_INTERP_H
#define ALLEGREX_INTERP_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Why a run stopped. Anything other than I_OK_RETURN means the trace ends
 * early, and the caller should say so rather than treating it as a clean run. */
typedef enum {
    I_RUNNING = 0,
    I_OK_RETURN,      /* reached the sentinel return address — a clean finish */
    I_BUDGET,         /* hit the instruction budget; probably an infinite loop */
    I_TRAP_INVALID,   /* an encoding the decoder does not recognise */
    I_TRAP_VFPU,      /* a VFPU op with no implementation behind it */
    I_TRAP_SYSCALL,   /* syscall — needs HLE that is not wired up here */
    I_TRAP_BREAK,
    I_TRAP_BRANCH_IN_SLOT, /* a control transfer inside a delay slot */
    I_TRAP_BADPC,     /* pc left mapped memory */
    I_EXIT,           /* the guest called sceKernelExitGame -- a clean finish */
} psp_interp_status;

/* A run in progress.
 *
 * `ra_sentinel` is how a run terminates: the caller seeds $ra with an address
 * that is deliberately not mapped to code, and the run stops when pc reaches
 * it. This is what makes single functions testable in isolation, which is the
 * whole point during bring-up — the game cannot boot yet, so whole-program
 * execution is not available to compare against. */
typedef struct {
    uint32_t          pc;
    uint32_t          ra_sentinel;
    uint64_t          budget;      /* max instructions; 0 = unlimited */
    uint64_t          executed;
    psp_interp_status status;
    uint32_t          fault_pc;    /* where a trap happened */

    /* Trace output. NULL disables tracing entirely, which is the fast path. */
    FILE             *trace;
    int               trace_regs;  /* also log every register the step changed */
} psp_interp;

/* Set up a run: pc = entry, $ra = sentinel, counters cleared. Does not touch
 * memory or any register other than $ra, so the caller controls the arguments. */
/* Run every thread that was started but parked because it did not outrank its
 * starter. Call after a top-level run ends; see the note at spawn_hook. */
void psp_interp_drain_pending(uint64_t budget);

void psp_interp_init(psp_interp *it, uint32_t entry, uint32_t ra_sentinel,
                     uint64_t budget);

/* ---- the firmware boundary ------------------------------------------------
 *
 * A module reaches the firmware through one small thunk per imported function,
 * in .sceStub.text. On a *linked* module the loader has patched each thunk to
 * jump at the real routine; on the raw module we run here, it is still the
 * unlinked `jr $ra` the linker left, so executing it returns immediately and
 * does nothing.
 *
 * The recompiled C does not have that problem: the emitter turns each thunk
 * into a psp_hle_call() on the function's NID. So without this the two paths
 * disagree at every firmware call by construction, and every function that
 * reaches one is uncomparable — which was most of them.
 *
 * Registering the thunk-to-NID map makes the interpreter cross the boundary
 * the same way: a pc landing on a thunk calls psp_hle_call() and returns to
 * $ra, exactly as the generated stub does.
 *
 * The table is borrowed, not copied — keep it alive for the run. Passing NULL
 * (or n == 0) clears it and restores the raw-execution behaviour. */

typedef struct {
    uint32_t addr;   /* thunk address in .sceStub.text */
    uint32_t nid;    /* firmware function it stands for */
} psp_interp_import;

int psp_interp_set_imports(const psp_interp_import *tbl, int n);

/* Frees the lookup index built by psp_interp_set_imports(). */
void psp_interp_free_imports(void);

/* Where a run spent its instructions.
 *
 * "The budget ran out" says a run looped; it does not say where, and the
 * address is what turns a count into something actionable. The busiest address
 * is the innermost loop body, and the branch just past it is the exit test that
 * never became true.
 *
 * Opt-in: it costs one increment per instruction, which is not worth paying on
 * runs that terminate. The buffer is owned by the caller's arena, sized from
 * the module extent, and cleared by psp_interp_profile_reset().
 *
 * Pass NULL to disable. */
void     psp_interp_profile(uint32_t base, uint32_t words);
void     psp_interp_profile_reset(void);
uint32_t psp_interp_hot_pc(uint32_t *count);

/* The firmware call a run repeated most, and how many times in a row. A run
 * that exhausts its instruction budget is usually spinning on one unimplemented
 * function, and this names it. Reset between runs. */
void     psp_interp_hle_reset(void);
uint32_t psp_interp_hot_nid(uint32_t *count);

/* Execute one instruction, plus its delay slot if it has one. A branch and its
 * delay slot are one step: splitting them would let a trace interleave at a
 * point the hardware never exposes. Returns the status after the step. */
psp_interp_status psp_interp_step(psp_interp *it);

/* Step until the run stops. Returns the terminating status. */
psp_interp_status psp_interp_run(psp_interp *it);

/* Service guest re-entry from HLE handlers inside the interpreter.
 *
 * With this enabled, a psp_dispatch() call made from a firmware handler while
 * an interpreter run is in progress executes the target *interpreted*, nested
 * under the current run and charged against its budget, instead of jumping into
 * recompiled code. Outside an interpreter run the hook does nothing, so the
 * recompiled path is unaffected and this can be left on.
 *
 * Without it a differential run has to discard every function that reaches such
 * a callback -- and recompiled code has no instruction budget, so one that does
 * not return hangs the run rather than failing it. */
void psp_interp_service_dispatch(int enable);

/* How many re-entries were refused for exceeding the nesting limit. Nonzero
 * means some guest callback did not run; the result is still bounded, but it is
 * not a faithful execution. */
uint64_t psp_interp_nest_refused(void);

/* How many nested runs ended on anything other than a clean return. Each one
 * is reported to stderr as it happens; this is for the end-of-run summary, so
 * that "returned" at the top level cannot hide a thread that died. */
unsigned long long psp_interp_nest_failed(void);

/* Human-readable status, for error messages. Never NULL. */
const char *psp_interp_status_str(psp_interp_status s);

/* Execute a single already-decoded instruction, no control flow. Exposed for
 * the test suite's coverage guard, which walks a list of real encodings and
 * asserts the interpreter implements everything the emitter does — the two
 * drifting apart would quietly destroy the oracle's only useful property.
 *
 * Not for general use: it does not touch pc and does not handle delay slots.
 * Callers that want to run code want psp_interp_step(). */
psp_interp_status psp_interp_exec_one(const void *decoded_insn);

#ifdef __cplusplus
}
#endif

#endif /* ALLEGREX_INTERP_H */
