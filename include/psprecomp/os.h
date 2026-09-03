/* psprecomp — the host primitives the runtime needs and C does not provide.
 *
 * Threads, a mutex, a condition variable, a monotonic clock and an absolute
 * sleep. Five things, and every one of them is spelled differently on Windows
 * than on everything else.
 *
 * This exists because the runtime claims to build with MSVC and the scheduler
 * was written against pthreads. README.md says "MSVC on Windows; gcc/clang
 * elsewhere", and `find_package(Threads)` does not make that true: it links the
 * right library, it does not make `pthread_mutex_t` a type. Anything the
 * scheduler needs goes through here, so that the platform question is answered
 * in one file rather than at eighty call sites.
 *
 * Deliberately small. It is not a threading library and should not grow into
 * one -- the test is whether src/hle/sched.c needs it, since that is the only
 * part of the runtime that has threads at all.
 */
#ifndef PSPRECOMP_OS_H
#define PSPRECOMP_OS_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#else
#  include <pthread.h>
#endif

/* ---- thread-local storage --------------------------------------------------
 *
 * C11's `_Thread_local` is what this wants. MSVC has supported the C11 spelling
 * only since it grew a C11 mode, and `__declspec(thread)` predates it by two
 * decades and is what every MSVC build has, so that is what MSVC gets. */
#if defined(_MSC_VER)
#  define PSP_THREAD_LOCAL __declspec(thread)
#else
#  define PSP_THREAD_LOCAL _Thread_local
#endif

/* ---- the clock -------------------------------------------------------------
 *
 * Monotonic nanoseconds from an arbitrary origin: the only property anything
 * here relies on is that it does not go backwards. Wall-clock time is not
 * wanted anywhere in the runtime -- guest time is virtual (see clock.h) and
 * this is only ever used to measure or to pace. */
uint64_t psp_os_mono_ns(void);

/* Sleep until an absolute instant on that clock, returning immediately if it
 * has already passed. Absolute rather than a duration on purpose: a pacer that
 * sleeps for "the remaining 4ms" drifts by whatever it spends computing that,
 * and the drift accumulates once per frame. */
void psp_os_sleep_until_ns(uint64_t deadline_ns);

/* ---- mutex -----------------------------------------------------------------
 *
 * Not recursive, on either platform -- a POSIX mutex is not by default and an
 * SRWLOCK cannot be. Locking one twice from the same thread deadlocks, which is
 * the behaviour the scheduler was written against.
 *
 * Named `psp_os_mutex` rather than `psp_mutex` because `psp_mutex` is already
 * the *guest's* mutex, in src/hle/kernlock.c. They are unrelated: one is a
 * host primitive, the other is a PSP kernel object with a uid and an owner. */
#if defined(_WIN32)
typedef SRWLOCK psp_os_mutex;
#  define PSP_OS_MUTEX_INIT SRWLOCK_INIT
#else
typedef pthread_mutex_t psp_os_mutex;
#  define PSP_OS_MUTEX_INIT PTHREAD_MUTEX_INITIALIZER
#endif

void psp_os_lock(psp_os_mutex *m);
void psp_os_unlock(psp_os_mutex *m);

/* ---- condition variable ---------------------------------------------------- */
#if defined(_WIN32)
typedef CONDITION_VARIABLE psp_os_cond;
#  define PSP_OS_COND_INIT CONDITION_VARIABLE_INIT
#else
typedef pthread_cond_t psp_os_cond;
#  define PSP_OS_COND_INIT PTHREAD_COND_INITIALIZER
#endif

void psp_os_cond_wait(psp_os_cond *c, psp_os_mutex *m);

/* Wait until signalled or until an absolute instant on psp_os_mono_ns's clock.
 * Returns non-zero if the deadline arrived first.
 *
 * The deadline is monotonic, which POSIX's pthread_cond_timedwait is not by
 * default -- it takes CLOCK_REALTIME, so a wait armed for "two seconds from
 * now" can be cut short or extended by the system clock being set. The
 * condition attribute fixes that, and setting it is why the POSIX side has an
 * initialiser function at all. */
int psp_os_cond_wait_until(psp_os_cond *c, psp_os_mutex *m, uint64_t deadline_ns);

void psp_os_cond_broadcast(psp_os_cond *c);

/* Give a statically-initialised condition variable a monotonic clock. A no-op
 * on Windows, where SleepConditionVariableSRW takes a relative timeout and the
 * question does not arise. Safe to call more than once. */
void psp_os_cond_use_monotonic(psp_os_cond *c);

/* ---- threads ---------------------------------------------------------------
 *
 * Start, and join only at the end. The scheduler's threads end by running out
 * of guest code or by being told to stop; they are never reaped mid-run -- see
 * the comment on slot reuse in src/hle/sched.c -- but a host that is about to
 * free the memory they execute from must wait for them, which is what
 * psp_os_thread_join and psp_sched_join_all are for.
 *
 * `stack_bytes` is a request, not a guarantee, and the reason it is a parameter
 * is in psp_sched_spawn: a recompiled frame is much larger than the MIPS one it
 * came from, so a guest thread that fits a PSP's 16K can still need megabytes
 * of host stack. */
#if defined(_WIN32)
typedef HANDLE psp_os_thread;
#else
typedef pthread_t psp_os_thread;
#endif

/* Returns 0 on success. The entry point takes ownership of nothing: `arg` must
 * outlive the thread. */
int  psp_os_thread_start(psp_os_thread *t, void (*fn)(void *), void *arg,
                         size_t stack_bytes);

/* End the calling thread. Does not return. Only ever called on a thread this
 * module started. */
void psp_os_thread_exit(void);

/* Wait for a thread this module started to end, and release its handle. Once
 * per thread. */
void psp_os_thread_join(psp_os_thread *t);

#endif /* PSPRECOMP_OS_H */
