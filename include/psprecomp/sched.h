/* psprecomp — thread scheduling.
 *
 * Why host threads and not saved registers
 * ----------------------------------------
 * Recompiled guest code is ordinary C. A guest thread that blocks is not
 * sitting at a tidy boundary with its state in `psp_cpu` -- it is part-way down
 * a host call stack, inside psp_func_X called from psp_func_Y called from the
 * dispatcher. Its control flow *is* that stack. Saving the guest register file
 * would preserve none of it.
 *
 * So each guest thread gets a host thread, whose stack holds that context for
 * free. What the host thread does not get is the right to run whenever it
 * likes: the PSP is single-core and never runs two threads at once, so a
 * handoff token enforces exactly one runnable thread at a time. Everything else
 * blocks on a condition variable.
 *
 * That also means `psp_cpu` can stay a plain global. Only the thread holding
 * the token touches it, and a switch saves it into the outgoing thread's slot
 * and restores the incoming one's -- which is cheaper than making it
 * thread-local, since generated code reads and writes it on nearly every line.
 *
 * What this is not
 * ----------------
 * There is no clock, and preemption is approximate: a thread gives way to its
 * equals every so many firmware calls (see psp_sched_tick) rather than on a
 * timer, because recompiled C is an ordinary host call stack and cannot be
 * interrupted part-way. A thread that spins without ever calling into the
 * kernel therefore still hangs -- reported, rather than silently.
 */
#ifndef PSPRECOMP_SCHED_H
#define PSPRECOMP_SCHED_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Why a thread is not running. Reported in diagnostics, so a stuck game names
 * what it is stuck on rather than merely being stuck. */
typedef enum {
    PSP_SCHED_READY = 0,
    PSP_SCHED_RUNNING,
    PSP_SCHED_BLOCKED,     /* waiting on a kernel object */
    PSP_SCHED_SLEEPING,    /* a deadline, or an explicit wakeup */
    PSP_SCHED_DEAD,
    /* sceKernelSuspendThread. Distinct from blocked: nothing it is waiting for
     * can release it, only sceKernelResumeThread, so a signal must not. */
    PSP_SCHED_SUSPENDED,
} psp_sched_state;

void psp_sched_init(void);
void psp_sched_reset(void);

/* Turn real threading off, for a caller that must stay single-context and
 * deterministic.
 *
 * The differential oracle is the case this exists for. It runs one guest
 * function at a time and compares register files, so a function that happens
 * to call sceKernelStartThread would have it run concurrently -- which makes
 * the comparison depend on scheduling, and hangs the harness outright when the
 * spawned thread never gives the token back. Neither is something an oracle
 * can tolerate: it exists to attribute a disagreement to a codegen bug, and a
 * disagreement it cannot reproduce is worse than no oracle.
 *
 * With threading off, spawning records the thread and reports success but runs
 * nothing, waits fail rather than parking, and yields do nothing -- the
 * behaviour batch callers had before the scheduler existed. */
void psp_sched_set_threading(int on);

/* Create a host thread for a guest thread and leave it READY. `entry` is the
 * guest address to dispatch, `sp` its stack top, `a0`/`a1` the two arguments
 * the PSP passes (arglen, argp). Returns 0 on success. */
int psp_sched_spawn(uint32_t uid, uint32_t entry, uint32_t sp,
                    uint32_t a0, uint32_t a1, int priority);

/* Give up the token. `block` parks the caller until something readies it;
 * `yield` puts it back on the ready queue first.
 *
 * block returns 0 once the caller is running again, or -1 if parking would have
 * left nothing runnable -- meaning no thread exists that could ever satisfy the
 * wait. Callers must fail the wait in that case rather than retry, or they spin
 * against a scheduler handing the token straight back. */
int  psp_sched_block(uint32_t uid, psp_sched_state why, const char *what);

/* What ended a timed wait. Distinct answers because they need opposite
 * handling: a signal means the wait succeeded, a deadline means it did not. */
#define PSP_SCHED_WOKEN     0
#define PSP_SCHED_STRANDED (-1)   /* nothing runnable, and no deadline set */
#define PSP_SCHED_EXPIRED  (-2)

/* Park until woken *or* until `deadline_us` of guest time arrives.
 *
 * A deadline is not a convenience over psp_sched_block -- it is what lets a
 * timed wait expire at all. The machinery is already here: when nothing is
 * runnable, handoff_locked moves the clock to the earliest sleeping deadline
 * and releases whoever it belongs to, because guest time only advances when the
 * guest advances it and a stalled scheduler would otherwise wait forever for a
 * moment that cannot arrive. So a caller that parks with a deadline is released
 * at the right guest instant even when it is the last thread alive -- exactly
 * the case an undated block has to refuse.
 *
 * `deadline_us` is absolute guest microseconds, from psp_clock_peek(); 0 means
 * no deadline, which makes this identical to psp_sched_block. A caller asking
 * for a zero-length timeout wants the shortest deadline that exists, not the
 * absence of one, so it should pass `psp_clock_peek() + 1`.
 *
 * Returns PSP_SCHED_WOKEN, PSP_SCHED_EXPIRED, or PSP_SCHED_STRANDED. */
int  psp_sched_block_until(uint32_t uid, psp_sched_state why, const char *what,
                           uint64_t deadline_us);

void psp_sched_yield(void);

/* Sleep for `usec` of guest time.
 *
 * A yield cannot express this: it leaves the caller READY, and the handoff picks
 * the most urgent READY thread, which is the caller again whenever it outranks
 * everything else. Nor is one round enough -- two threads at the same priority
 * simply hand the CPU back and forth and anything below them still starves.
 *
 * Sleeping until a deadline makes the caller ineligible for as long as it asked
 * for, which is the whole reason a game's main loop delays. */
void psp_sched_delay(uint64_t usec);

/* Charge the running thread one tick of its timeslice, and rotate to an equal
 * if it has used the slice up. Called from the firmware-call path: a PSP
 * preempts on a timer, and nothing here can interrupt recompiled C part-way, so
 * kernel calls are where a thread that never blocks gives way to its equals. */
void psp_sched_tick(void);

/* Make a parked thread runnable. No effect on one that is already ready.
 *
 * Returns 1 if the thread it readied is more urgent than the caller. A kernel
 * switches to it at that point, so a caller that has finished releasing
 * waiters should yield; otherwise a game that signals a high-priority worker
 * and carries on gets no work done until it happens to block. */
int psp_sched_wake(uint32_t uid);

/* The calling guest thread has finished. Does not return. */
void psp_sched_exit(uint32_t uid);

/* Consulted before a thread is spawned, before any other spawn logic.
 * Returning nonzero means the hook took the thread: the scheduler records
 * nothing and spawns no host thread. The interpreter uses this to run a
 * started thread synchronously inside the run that started it; a host that
 * wants real threads installs nothing. */
/* Undo a spawn the hook took but has not run yet.
 *
 * A hook that models "runnable, not running" has to be told when the guest
 * kills a thread it never let start -- otherwise the thread runs later, which
 * is worse than running early. Called by sceKernelTerminateThread and
 * sceKernelDeleteThread; a no-op when no hook is installed. */
void psp_sched_cancel_spawn(uint32_t uid);
void psp_sched_set_cancel_hook(void (*fn)(uint32_t uid));

void psp_sched_set_spawn_hook(int (*fn)(uint32_t uid, uint32_t entry, uint32_t sp,
                                        uint32_t a0, uint32_t a1, int priority));

/* Called at the top of every guest thread's host thread, before it runs any
 * guest code. The boot host uses it to give each thread its own alternate
 * signal stack: sigaltstack is per-thread, and a thread that faults by
 * exhausting its stack has nowhere to run a handler without one -- the fault
 * recurses and the process dies with no diagnosis at all. */
void psp_sched_set_thread_hook(void (*fn)(void));

/* Called when a thread's entry point *returns* rather than calling
 * sceKernelExitThread, with $v0 -- the status a PSP thread yields that way.
 * The scheduler knows a thread has died but not what a thread means, so the
 * thread manager supplies the meaning. */
void psp_sched_set_end_hook(void (*fn)(uint32_t uid, uint32_t status));

/* Stop the whole guest: every thread is marked dead and the main context is
 * given the token back.
 *
 * This exists because a guest thread cannot unwind the host. The boot host
 * guards a run with sigsetjmp in the main thread, but siglongjmp from a *guest*
 * thread would jump across threads and into a frame that may already have
 * returned -- undefined, and it does happen: sceKernelExitGame is normally
 * called from a game's main thread, not from module_start. So a guest thread
 * ending the run does it by stopping the scheduler and letting the main context
 * discover it, rather than by jumping.
 *
 * Does not return when called from a guest thread. */
void psp_sched_stop_all(const char *why);

/* Why the run was force-stopped, or NULL if every thread ended on its own.
 *
 * drain answers "how many threads are still alive", and a stopped run answers
 * zero -- indistinguishable from a run that finished. The boot summary used to
 * print "all finished" for a run the host had just killed, which read as
 * success. This is the difference. */
const char *psp_sched_stop_reason(void);

/* Run until every guest thread is dead, nothing can make progress, or
 * `timeout_s` elapses. Called from the main context once module_start has
 * returned. Returns the number of threads still alive; nonzero means a deadlock
 * or a runaway thread, and either is reported. */
int psp_sched_drain(int timeout_s);

/* Every live thread and what it waits on. */
void psp_sched_dump_threads(FILE *out);

/* Introspection, for the boot report. */
int      psp_sched_live(void);
uint32_t psp_sched_current(void);

/* A thread's priority, or the least urgent value there is when it has no slot.
 *
 * Asked by the waiter queue, which releases most-urgent-first for objects
 * created with that attribute. Read from the scheduler rather than kept
 * alongside the waiter, because a thread's priority can change while it waits
 * and the order has to follow it. An unknown uid sorts last, so a stale entry
 * can never win a release it should not. */
int      psp_sched_priority(uint32_t uid);

/* Change a thread's priority. The thread manager owns what a priority *means*;
 * the scheduler owns which thread runs, so it has to be told -- writing the new
 * value only into the thread-manager record left the scheduler ordering threads
 * by the priority they were created with forever. */
void psp_sched_set_priority(uint32_t uid, int priority);

/* Suspend and resume, which are not block and wake.
 *
 * A suspended thread is waiting for nothing, so nothing it was parked on may
 * release it -- only a resume. Suspending the *running* thread gives up the
 * token, so this does not return until something resumes it.
 *
 * Returns 1 if the thread existed. */
int  psp_sched_suspend(uint32_t uid);
int  psp_sched_resume(uint32_t uid);

/* sceKernelSuspendDispatchThread: stop switching between threads entirely.
 *
 * A guest asks for this around a critical section it needs to finish
 * uninterrupted, and pspautotests' scheduling/dispatch is an entire test of it.
 * While off, the timeslice does not fire and a yield does nothing; the current
 * thread keeps the CPU until it turns dispatch back on. Returns the previous
 * setting, which is what the guest passes back to restore it. */
int  psp_sched_set_dispatch(int on);

#ifdef __cplusplus
}
#endif

#endif /* PSPRECOMP_SCHED_H */
