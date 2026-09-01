/* psprecomp — host primitives. See include/psprecomp/os.h for what and why.
 *
 * Two implementations of five things, kept side by side rather than in separate
 * files so that a change to one is visibly a change to one of a pair. Neither
 * is long enough to justify the separation, and a reader checking that they
 * agree should not have to open two files to do it.
 */

#include "psprecomp/os.h"

#include <stdlib.h>

#if defined(_WIN32)

#include <process.h>

uint64_t psp_os_mono_ns(void) {
    /* QueryPerformanceCounter is the monotonic clock on Windows. Its frequency
     * is fixed for the life of the process, so it is read once. */
    static LARGE_INTEGER freq;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    /* Split rather than `now * 1e9 / freq`, which overflows 64 bits after a few
     * seconds of uptime at a 10MHz counter. */
    const uint64_t sec  = (uint64_t)(now.QuadPart / freq.QuadPart);
    const uint64_t rest = (uint64_t)(now.QuadPart % freq.QuadPart);
    return sec * 1000000000ull + rest * 1000000000ull / (uint64_t)freq.QuadPart;
}

void psp_os_sleep_until_ns(uint64_t deadline_ns) {
    for (;;) {
        const uint64_t now = psp_os_mono_ns();
        if (now >= deadline_ns) return;
        const uint64_t left = deadline_ns - now;
        /* Sleep rounds up to the timer resolution, so the last fraction of a
         * millisecond is spent yielding rather than overshooting. */
        const DWORD ms = (DWORD)(left / 1000000ull);
        if (ms) Sleep(ms); else Sleep(0);
    }
}

void psp_os_lock(psp_os_mutex *m)   { AcquireSRWLockExclusive(m); }
void psp_os_unlock(psp_os_mutex *m) { ReleaseSRWLockExclusive(m); }

void psp_os_cond_wait(psp_os_cond *c, psp_os_mutex *m) {
    SleepConditionVariableSRW(c, m, INFINITE, 0);
}

int psp_os_cond_wait_until(psp_os_cond *c, psp_os_mutex *m, uint64_t deadline_ns) {
    const uint64_t now = psp_os_mono_ns();
    if (now >= deadline_ns) return 1;
    /* Rounded up: waking early would spin, and the caller re-checks its
     * condition anyway, but a zero here would busy-loop against the lock. */
    const uint64_t left_ms = (deadline_ns - now + 999999ull) / 1000000ull;
    const DWORD ms = left_ms > 0xFFFFFFFEull ? 0xFFFFFFFEu : (DWORD)left_ms;
    if (SleepConditionVariableSRW(c, m, ms, 0)) return 0;
    return GetLastError() == ERROR_TIMEOUT;
}

void psp_os_cond_broadcast(psp_os_cond *c) { WakeAllConditionVariable(c); }

void psp_os_cond_use_monotonic(psp_os_cond *c) { (void)c; }

struct win_thread_arg { void (*fn)(void *); void *arg; };

static unsigned __stdcall win_thread_main(void *p) {
    struct win_thread_arg *a = (struct win_thread_arg *)p;
    void (*fn)(void *) = a->fn;
    void *arg = a->arg;
    free(a);
    fn(arg);
    return 0;
}

int psp_os_thread_start(psp_os_thread *t, void (*fn)(void *), void *arg,
                        size_t stack_bytes) {
    struct win_thread_arg *a = (struct win_thread_arg *)malloc(sizeof *a);
    if (!a) return -1;
    a->fn = fn; a->arg = arg;
    /* _beginthreadex rather than CreateThread: the guest code these threads run
     * is recompiled C and uses the CRT, which wants per-thread state that
     * CreateThread does not set up. */
    const uintptr_t h = _beginthreadex(NULL, (unsigned)stack_bytes,
                                       win_thread_main, a, 0, NULL);
    if (!h) { free(a); return -1; }
    *t = (HANDLE)h;
    return 0;
}

void psp_os_thread_exit(void) { _endthreadex(0); }

#else /* POSIX */

#include <errno.h>
#include <time.h>

uint64_t psp_os_mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void psp_os_sleep_until_ns(uint64_t deadline_ns) {
    const struct timespec t = {
        (time_t)(deadline_ns / 1000000000ull),
        (long)(deadline_ns % 1000000000ull),
    };
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL) == EINTR) {}
}

void psp_os_lock(psp_os_mutex *m)   { pthread_mutex_lock(m); }
void psp_os_unlock(psp_os_mutex *m) { pthread_mutex_unlock(m); }

void psp_os_cond_wait(psp_os_cond *c, psp_os_mutex *m) {
    pthread_cond_wait(c, m);
}

int psp_os_cond_wait_until(psp_os_cond *c, psp_os_mutex *m, uint64_t deadline_ns) {
    const struct timespec t = {
        (time_t)(deadline_ns / 1000000000ull),
        (long)(deadline_ns % 1000000000ull),
    };
    return pthread_cond_timedwait(c, m, &t) == ETIMEDOUT;
}

void psp_os_cond_broadcast(psp_os_cond *c) { pthread_cond_broadcast(c); }

void psp_os_cond_use_monotonic(psp_os_cond *c) {
    /* A condition variable's timed wait uses CLOCK_REALTIME unless it is told
     * otherwise, and psp_os_cond_wait_until's deadlines come from
     * psp_os_mono_ns. Without this the two clocks are different origins and the
     * wait expires at an unrelated moment -- or not at all.
     *
     * Re-initialising a statically-initialised condition variable is how the
     * attribute gets applied, since PTHREAD_COND_INITIALIZER cannot carry one.
     * Safe before any thread waits on it, which is where the scheduler calls
     * it: nothing else exists yet at that point. */
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr) != 0) return;
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(c, &attr);
    pthread_condattr_destroy(&attr);
}

struct posix_thread_arg { void (*fn)(void *); void *arg; };

static void *posix_thread_main(void *p) {
    struct posix_thread_arg *a = (struct posix_thread_arg *)p;
    void (*fn)(void *) = a->fn;
    void *arg = a->arg;
    free(a);
    fn(arg);
    return NULL;
}

int psp_os_thread_start(psp_os_thread *t, void (*fn)(void *), void *arg,
                        size_t stack_bytes) {
    struct posix_thread_arg *a = (struct posix_thread_arg *)malloc(sizeof *a);
    if (!a) return -1;
    a->fn = fn; a->arg = arg;

    pthread_attr_t attr;
    pthread_attr_t *attrp = NULL;
    if (pthread_attr_init(&attr) == 0) {
        pthread_attr_setstacksize(&attr, stack_bytes);
        attrp = &attr;
    }
    const int rc = pthread_create(t, attrp, posix_thread_main, a);
    if (attrp) pthread_attr_destroy(attrp);
    if (rc != 0) { free(a); return -1; }
    return 0;
}

void psp_os_thread_exit(void) { pthread_exit(NULL); }

#endif
