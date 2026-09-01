/* psprecomp — ThreadManForUser.
 *
 * Threads, semaphores and event flags. This is the largest single dependency
 * any PSP game has: `module_start` typically does nothing but create a thread,
 * start it, and return — so until this works, a recompiled module runs about
 * forty instructions and stops.
 *
 * ## The execution model
 *
 * Threads are real: each guest thread gets a host thread, and a handoff token
 * keeps exactly one of them running at a time, the way a single-core PSP does.
 * The scheduler is src/hle/sched.c; the header there explains why saving the
 * register file is not enough to park a thread.
 *
 * What this module owns is what a thread *is* -- its stack, priority, exit
 * status, and the kernel objects it waits on. Waits park the caller and signals
 * release it.
 *
 * The one place the model still shows through: with no preemption and no clock,
 * a wait that nothing could ever satisfy -- because no other thread is runnable
 * -- cannot be waited out. A caller that supplied a timeout is told it elapsed;
 * one that did not is not lied to, because a fabricated timeout is something a
 * game acts on. See wait_deadlock. sceKernelDelayThread yields rather than
 * sleeping, for the same want of a clock.
 */

#include "psprecomp/hle.h"
#include "psprecomp/dispatch.h"
#include "psprecomp/clock.h"
#include "psprecomp/sched.h"
#include "psprecomp/mem.h"
#include "waitq.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_THREADS 128
#define MAX_SEMAS   128
#define MAX_FLAGS   128
#define MAX_CBS     64
#define UID_BASE    0x00040000u

enum { TH_DORMANT = 0, TH_READY, TH_RUNNING, TH_SUSPENDED };
enum { WAIT_NONE = 0, WAIT_SLEEP, WAIT_DELAY };

/* What sceKernelReferThreadStatus reports in its `status` field. These are the
 * kernel's own values, not this file's TH_* -- a created thread reports 16. */
#define PSP_THREAD_STATUS_RUNNING 1
#define PSP_THREAD_STATUS_READY   2
#define PSP_THREAD_STATUS_WAITING 4
#define PSP_THREAD_STATUS_SUSPEND 8
#define PSP_THREAD_STATUS_STOPPED 16

#define MAX_SEMA_WAITERS 32

typedef struct {
    uint32_t uid;
    char     name[32];
    uint32_t entry;
    uint32_t priority;
    /* What it was created with, which sceKernelReferThreadStatus reports
     * separately and which a priority change must not touch: a thread created
     * at 0x30 and changed to 0x21 still reports `init=30`. */
    uint32_t init_priority;
    uint32_t stack_size;
    uint32_t stack_base;   /* low address of the allocation */
    uint32_t attr;
    int      state;
    uint32_t exit_status;
    /* Banked sceKernelWakeupThread calls; see hle_SleepThread. */
    int      wakeup_count;
    /* Whether it has ever been started. A thread that has not is *dormant*,
     * and that is a different answer from one that has run and stopped. */
    int      ever_started;
    /* Which call parked it. The scheduler has one SLEEPING state for both a
     * sceKernelSleepThread and a sceKernelDelayThread, and the id list reports
     * them as different types -- so the distinction has to be kept here, where
     * the difference was made. */
    int      wait_kind;
    /* Parked in a wait whose name ends in CB, and woken by a notify rather than
     * by what it was actually waiting for. A CB wait is not "deliver callbacks
     * on the way in": a notify raised by another thread ends the wait long
     * enough to run the handler, and the wait then resumes. */
    int      cb_wait;
    int      cb_wake;
    /* Threads parked in sceKernelWaitThreadEnd on this one. */
    uint32_t enders[MAX_SEMA_WAITERS];
    int      nenders;
    int      used;
    jmp_buf  unwind;       /* where sceKernelExitThread returns to */
    int      unwind_set;
} psp_thread;

typedef struct {
    uint32_t uid;
    char     name[32];
    int32_t  count;
    int32_t  max_count;
    int32_t  init_count;      /* what it was created with; ReferSemaStatus reports it */
    uint32_t attr;
    int      used;
    /* Who is parked on it, in the order the attribute says to release them.
     * The signaller deducts the count on a waiter's behalf and wakes only the
     * ones it satisfied -- see waitq.h for why waking everyone to re-test is
     * not a conservative version of that but a different, measurable rule. */
    psp_waitq q;
    /* "sceKernelWaitSema(<name>)", built once at creation.
     *
     * The scheduler stores the string it is handed and prints it for every
     * parked thread, so naming the *object* here is the difference between
     * knowing a thread waits on a semaphore and knowing which one nobody is
     * signalling. Held in the semaphore because the slot only keeps a pointer,
     * and it has to outlive the call that blocked. */
    char     waitdesc[64];
} psp_sema;

typedef struct {
    uint32_t  uid;
    char      name[32];
    uint32_t  pattern;
    uint32_t  init_pattern;
    uint32_t  attr;
    int       used;
    psp_waitq q;
    char      waitdesc[64];
} psp_evflag;

typedef struct {
    uint32_t uid;
    char     name[32];
    uint32_t func;
    uint32_t arg;
    uint32_t thread;          /* the thread that created it */
    uint32_t notify_count;
    uint32_t notify_arg;
    int      used;
} psp_callback;

static psp_thread   g_thread[MAX_THREADS];
static psp_sema     g_sema[MAX_SEMAS];
static psp_evflag   g_flag[MAX_FLAGS];
static psp_callback g_cb[MAX_CBS];
static uint32_t     g_next_uid;
/* The thread the scheduler says is running, as a thread-manager object.
 *
 * There is no separate notion of "current" any more: the scheduler owns that,
 * and a second copy maintained here would be a second thing to keep in step.
 * Returns NULL on the main context, which is not a guest thread. */
static psp_thread *current_thread(void);
static psp_callback *find_cb(uint32_t id);
static int          g_warned_block;

void psp_threadman_reset(void) {
    memset(g_thread, 0, sizeof g_thread);
    memset(g_sema, 0, sizeof g_sema);
    memset(g_flag, 0, sizeof g_flag);
    memset(g_cb, 0, sizeof g_cb);
    g_next_uid = UID_BASE;
    g_warned_block = 0;
    /* The clock first: the scheduler stamps the main context's timeslice from
     * it, so resetting time afterwards would leave that stamp in the future. */
    psp_clock_reset();
    psp_sched_reset();
    psp_kernlock_reset();
    psp_kernobj_reset();
    psp_ktimer_reset();
}

static void on_thread_end(uint32_t uid, uint32_t status);
static void thread_ended(psp_thread *t, uint32_t status);

void psp_threadman_init(void) {
    psp_sched_set_end_hook(on_thread_end);
    psp_threadman_reset();
}

/* Typed lookups rather than one generic macro. A macro taking a parameter
 * named `uid` also rewrites every `.uid` member access it expands around,
 * which is exactly the kind of subtlety not worth inviting to save twelve
 * lines. */
static psp_thread *find_thread(uint32_t id) {
    for (int i = 0; i < MAX_THREADS; i++)
        if (g_thread[i].used && g_thread[i].uid == id) return &g_thread[i];
    return NULL;
}
static psp_thread *current_thread(void) {
    const uint32_t uid = psp_sched_current();
    return uid ? find_thread(uid) : NULL;
}

static psp_sema *find_sema(uint32_t id) {
    for (int i = 0; i < MAX_SEMAS; i++)
        if (g_sema[i].used && g_sema[i].uid == id) return &g_sema[i];
    return NULL;
}
static psp_evflag *find_flag(uint32_t id) {
    for (int i = 0; i < MAX_FLAGS; i++)
        if (g_flag[i].used && g_flag[i].uid == id) return &g_flag[i];
    return NULL;
}

/* Report a wait that nothing could satisfy, exactly once. Repeating it for
 * every frame of a game that polls a semaphore drowns out everything else. */
/* A wait that can never complete, made by a caller that asked for no timeout.
 *
 * Reporting a timeout here is a lie, and a load-bearing one: this game's
 * user_main waits on its game thread with no timeout, and on being told the
 * wait timed out it deletes that thread -- still live, mid-frame -- and quits.
 * The whole run ended in a screen clear because of it.
 *
 * So the run stops and says why. A caller that *did* pass a timeout still gets
 * one, because for that caller a timeout is a true answer. */
static void wait_deadlock(const char *what) {
    fprintf(stderr,
        "psprecomp: %s cannot be satisfied -- no thread is runnable, so nothing\n"
        "  can ever signal it, and the caller passed no timeout. Reporting a\n"
        "  timeout would make the guest act on a falsehood, so the run stops\n"
        "  here instead. Live threads:\n", what);
    psp_sched_dump_threads(stderr);
    /* The reason rides along so the boot summary can tell a stopped run from
     * one that finished -- both leave zero threads alive. */
    psp_sched_stop_all(what);
}

/* warn_block used to sit here: a wait that could not block reported a timeout
 * and said so once. Nothing reaches it now. A caller that supplies a timeout
 * gets a real one -- the wait parks with a deadline and the scheduler moves the
 * clock to it -- and a caller that supplies none is not lied to, it stops the
 * run through wait_deadlock. There is no third case left to warn about. */

/* Defined down with the waits, but needed above them: by the signal, and by
 * sceKernelWaitThreadEnd, which is a thread operation that happens to be a
 * wait and so lives with the threads. */
static int sema_release(psp_sema *s);

/* ---- threads ------------------------------------------------------------- */

/* Two attribute bits about the stack that the kernel acts on rather than merely
 * records, and one about where it comes from. All three are measured by
 * threads/start, which creates a thread with each and reports what the memory
 * looked like afterwards. */
#define PSP_THREAD_ATTR_NO_FILLSTACK 0x00100000u
#define PSP_THREAD_ATTR_CLEAR_STACK  0x00200000u
#define PSP_THREAD_ATTR_LOW_STACK    0x00400000u

/* Freeing a thread's stack, zeroing it first if it was created asking for that.
 * The guest can still read the memory afterwards -- threads/start does exactly
 * that, writing a marker into the freed stack and checking whether it survived
 * -- so "cleared" is observable and not merely tidy. */
static void release_stack(psp_thread *t) {
    if (!t->stack_base) return;
    if (t->attr & PSP_THREAD_ATTR_CLEAR_STACK) {
        void *p = psp_mem_ptr(t->stack_base, t->stack_size);
        if (p) memset(p, 0, t->stack_size);
    }
    psp_sysmem_release(t->stack_base);
}

static void hle_CreateThread(void) {
    /* (name, entry, priority, stackSize, attr, option) */
    psp_thread *t = NULL;
    for (int i = 0; i < MAX_THREADS; i++) if (!g_thread[i].used) { t = &g_thread[i]; break; }
    if (!t) { psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }

    memset(t, 0, sizeof *t);
    psp_str(psp_arg(0), t->name, sizeof t->name);
    t->entry      = psp_arg(1);
    t->priority      = psp_arg(2);
    t->init_priority = psp_arg(2);
    t->stack_size = psp_arg(3);
    t->attr       = psp_arg(4);

    /* The requested size, not a floor of our own. It used to be raised to
     * 0x1000, which is observable twice over: sceKernelReferThreadStatus
     * reports the size back (`stackSize=10000` for a create that asked for
     * 0x10000, verbatim), and threads/start locates the argument block as an
     * offset from the stack *base*, so a stack that is 0x800 too big moves
     * every one of those offsets by 0x800. */
    if (t->stack_size < 0x200) t->stack_size = 0x200;
    /* Stacks grow down, so allocate from the top of the heap: a stack that
     * overflows then runs into free space rather than into another block --
     * unless the guest asked for the other end. threads/start creates a thread
     * with PSP_THREAD_ATTR_LOW_STACK and reports `WARNING: stack allocated
     * low`, which is the test noticing that the stack came out below a block
     * allocated with PSP_SMEM_Low. */
    t->stack_base = psp_sysmem_alloc(t->stack_size,
                                     !(t->attr & PSP_THREAD_ATTR_LOW_STACK));
    if (!t->stack_base) { psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }

    t->uid = g_next_uid++;
    t->state = TH_DORMANT;
    t->used = 1;
    psp_ret(t->uid);
}

/* What a thread is started with, and the one part of it that is held back.
 *
 * threads/semaphores/semaphores measures three rules and this implements two.
 *
 * The two: a NULL pointer with a non-zero length arrives as length **0**, and a
 * zero length with a real pointer arrives as a **NULL pointer**. Each cancels
 * the other out, in both directions, and neither is what passing the arguments
 * straight through gives.
 *
 * The third is that the block is *copied* onto the thread's own stack, and the
 * evidence for it is not in doubt. A one-byte start of the global 0x4567 reads
 * back on hardware as **0xFFFFFF67** -- one byte of data with this stack's 0xFF
 * fill above it -- which no reading of the original address can produce. A
 * variable holding 7, handed to a thread that writes 3 through the pointer,
 * still reads 7 afterwards.
 *
 * **It is not implemented, because it takes Armored Core from 633 GE lists to
 * 3.** Measured directly, and narrowed: performing the copy is harmless, and
 * handing the thread the copy's *address* is what breaks it. Copying 256 bytes
 * instead of four does not help, so the game is not merely reading past the
 * length it declared. What it does do is start three workers in a row from one
 * shared slot, rewriting the word between each -- so with the original pointer
 * all three read the last value, and with copies each reads its own, which is
 * the correct behaviour and the one the game does not survive.
 *
 * That points at something else being wrong upstream rather than at the rule,
 * and shipping a rule that is right in principle and breaks the only real
 * program available is the wrong trade. See docs/findings/autotests.md. */
static uint32_t start_arg_block(uint32_t *arglen, uint32_t argp) {
    if (!argp || !*arglen) { *arglen = 0; return 0; }
    return argp;
}

/* Whether the block can be delivered at all. Two refusals, both `800200d3`,
 * both leaving the thread unstarted -- threads/start proves the second half by
 * clearing the thread's out-parameters first and finding them still clear:
 *
 *     -1 arg length:    800200d3 (-1, NULL)
 *     arg ptr #1:       800200d3 (-1, NULL)      argp = 0xDEADBEEF
 *     arg ptr #2:       800200d3 (-1, NULL)      argp = 0x80000000
 *
 * A negative length is checked before the NULL-pointer rule, which is why
 * `With NULL ptr` succeeds with a length of 8 but `-1 arg length` does not
 * succeed with a real pointer. */
static int start_args_ok(uint32_t arglen, uint32_t argp) {
    if ((int32_t)arglen < 0) return 0;
    if (!argp || !arglen) return 1;
    /* Checked before the address is masked, because masking is what loses the
     * distinction: 0x80000000 is kernel space, and PSP_ADDR_MASK folds it onto
     * address zero, where a module linked at 0 makes it look mapped. A user
     * thread cannot be handed a kernel pointer, and 0xDEADBEEF (which does not
     * fold onto anything) is refused by the mapping check below. */
    if (argp >= 0x80000000u) return 0;
    return psp_mem_ptr(argp, arglen) != NULL;
}

static void hle_StartThread(void) {
    /* (thid, arglen, argp)
     *
     * Three ids, three answers, and threads/start.expected keeps them apart:
     * `NULL: 80020197` for zero, `Deleted`/`Invalid: 80020198` for an id that
     * names nothing, and `Twice`/`Current: 800201a4` for a thread that is
     * already running. Only the first was implemented, for all three. */
    if (psp_arg(0) == 0) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_THID); return; }
    psp_thread *t = find_thread(psp_arg(0));
    if (!t) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
    if (t->ever_started && t->state != TH_DORMANT) {
        psp_ret(SCE_KERNEL_ERROR_NOT_DORMANT);
        return;
    }

    if (!start_args_ok(psp_arg(1), psp_arg(2))) {
        psp_ret(SCE_KERNEL_ERROR_ILLEGAL_SIZE);
        return;
    }

    /* Starting forgets whatever sceKernelChangeThreadPriority did: the thread
     * comes back at the priority it was created with. threads/change restarts
     * one after every successful change and reads it back --
     * `0x08 priority: 00000000` / `After restart: Current=30, init=30` -- so
     * the 0x08 is gone by the time the thread runs. */
    t->priority = t->init_priority;

    /* A fresh thread's stack is filled with 0xFF, not left as it was found.
     *
     * That is the pattern sceKernelGetThreadStackFreeSize walks looking for --
     * this file already noted that we do not paint one -- and it is directly
     * observable: a short argument block on hardware reads back with 0xFF above
     * it. Verified against the game before shipping, which is not idle: filling
     * a stack changes what every uninitialised local reads.
     *
     * PSP_THREAD_ATTR_NO_FILLSTACK turns it off, and threads/start proves the
     * attribute is honoured rather than ignored: it scribbles 0xCC over the
     * stack area first and then reports `stack not set to FF, instead:
     * cccccccc` -- a line that only appears because the fill did *not* happen.
     *
     * Then the kernel's own two words at the very top and one at the very
     * bottom, which the same test reads back through sceKernelReferThreadStatus
     * and checks by hand:
     *
     *     stack[0]        == thread id
     *     stackEnd[-16]   == thread id
     *     stackEnd[-14]   == stack base
     *     stackEnd[-2..-1] == 0xFFFFFFFF
     *
     * The last pair look like the fill and are not: they are still there when
     * PSP_THREAD_ATTR_NO_FILLSTACK suppressed it, which is how that test
     * distinguishes them -- so they are written here rather than left to the
     * memset.
     *
     * That is the k0 area a PSP keeps at the top of every thread stack, and the
     * top 0x100 bytes of the stack are reserved for it -- which is also where
     * the argument block stops, so the two facts check each other. */
    if (t->stack_base) {
        void *p = psp_mem_ptr(t->stack_base, t->stack_size);
        if (p && !(t->attr & PSP_THREAD_ATTR_NO_FILLSTACK))
            memset(p, 0xFF, t->stack_size);
        if (p) {
            const uint32_t top = t->stack_base + t->stack_size;
            psp_write32(t->stack_base, t->uid);
            psp_write32(top - 16 * 4, t->uid);
            psp_write32(top - 14 * 4, t->stack_base);
            psp_write32(top -  2 * 4, 0xFFFFFFFFu);
            psp_write32(top -  1 * 4, 0xFFFFFFFFu);
        }
    }

    uint32_t arglen = psp_arg(1);
    uint32_t argp   = start_arg_block(&arglen, psp_arg(2));

    /* The block is *copied* onto the thread's own stack, and the thread is
     * handed the copy. threads/start measures where it lands, for a 0x800
     * stack:
     *
     *     1..8 bytes  -> stack+0x6f0      80 bytes -> stack+0x6b0
     *     90 bytes    -> stack+0x6a0      0x600    -> stack+0x100
     *
     * which is `top - 0x100 - roundup(len, 16)` in every case. The 0x100 is the
     * k0 area reserved above, so the two measurements agree with each other.
     *
     * $sp then starts below the copy rather than at a fixed offset from the top
     * -- the argument block is on the stack, so it has to be out of reach of
     * the frames. */
    const uint32_t stack_top = t->stack_base + t->stack_size - 0x100u;
    uint32_t sp = stack_top;
    if (arglen) {
        sp = stack_top - ((arglen + 15u) & ~15u);
        void *dst = psp_mem_ptr(sp, arglen);
        void *src = psp_mem_ptr(argp, arglen);
        if (dst && src) { memcpy(dst, src, arglen); argp = sp; }
    }

    /* The thread becomes runnable; it does not run here.
     *
     * It used to run to completion inside this call, on the caller's register
     * file, which worked for the module_start -> create -> start -> return
     * shape and for nothing else. A thread that blocks part-way has to be able
     * to stop and let another run, and its position in the host call stack is
     * its state -- so it needs a stack of its own. See sched.c.
     *
     * Starting a thread does not hand it the token, but psp_sched_spawn may:
     * a thread that outranks its starter is switched to inside that call, and
     * a short one can be finished before it returns.
     *
     * So the bookkeeping happens *first*. Marking the thread READY afterwards
     * overwrote the DORMANT that its own end hook had just written, and left a
     * finished thread looking runnable for the rest of the run --
     * threads/terminate asks four calls later and hardware answers
     * `Finished: 800201a2` where we answered `00000000`. */
    const int was_started = t->ever_started;
    const int was_state   = t->state;
    t->state = TH_READY;
    t->ever_started = 1;
    if (psp_sched_spawn(t->uid, t->entry, sp, arglen, argp,
                        (int)t->priority) != 0) {
        t->state = was_state;
        t->ever_started = was_started;
        psp_ret(SCE_KERNEL_ERROR_NO_MEMORY);
        return;
    }
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_ExitThread(void) {
    const uint32_t status = psp_arg(0);

    /* A thread ending is the last chance to see how it got there, and a thread
     * that ends with a nonzero status is usually reporting a failure its caller
     * will act on -- by which point the code that decided is long gone. Costs
     * nothing unless the generated code was built with PSPRECOMP_TRACE. */
    if (psp_hle_logging()) {
        fprintf(stderr, "hle: thread 0x%08X exiting with status %u\n",
                psp_sched_current(), status);
        psp_trace_dump();
    }
    psp_thread *t = find_thread(psp_sched_current());
    if (t) thread_ended(t, status);
    /* Does not return when called from a guest thread: the scheduler ends the
     * host thread underneath it. From the main context there is nothing to
     * unwind, so it falls through. */
    psp_sched_exit(psp_sched_current());
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_TerminateThread(void);

/* Delete frees a thread; it does not stop one, and it refuses anything it would
 * have to stop. threads/terminate runs the same ten cases through terminate,
 * terminate-delete and delete, and delete is the only one of the three that
 * answers `800201a4` to a thread that is ready, waiting, suspended or running
 * -- including the caller itself, which is what an id of 0 names here. */
static void hle_DeleteThread(void) {
    const uint32_t id = psp_arg(0) ? psp_arg(0) : psp_sched_current();
    psp_thread *t = find_thread(id);
    if (!t) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
    if (t->ever_started && t->state != TH_DORMANT) {
        psp_ret(SCE_KERNEL_ERROR_NOT_DORMANT);
        return;
    }
    /* Deleting a thread the guest never let start has to un-start it too --
     * sched.h says this happens here and it did not. Harmless when the thread
     * is already gone, which is the common case. */
    psp_sched_cancel_spawn(t->uid);
    release_stack(t);
    t->used = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* With one thread running to completion there is nothing to schedule during a
 * delay, so it returns immediately. Time still advances for anything reading
 * the clock. */
/* There is no clock, so a delay cannot be timed -- but it is nearly always a
 * thread being polite, and returning immediately starves everything else in a
 * game whose main loop delays. Yielding gives the other threads the token,
 * which is the useful half of the semantics. */
static void hle_DelayThread(void) {
    if (!psp_sched_can_wait()) { psp_ret(SCE_KERNEL_ERROR_CAN_NOT_WAIT); return; }
    psp_thread *me = current_thread();
    if (me) me->wait_kind = WAIT_DELAY;
    psp_sched_delay(psp_arg(0));
    me = current_thread();
    if (me) me->wait_kind = WAIT_NONE;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* A thread died by returning from its entry point. Records what it returned and
 * releases anyone waiting for it. */
/* One end for all the ways a thread reaches one: falling off its entry point,
 * sceKernelExitThread, and being terminated. They differ only in the status
 * left behind, and every one of them releases whatever was waiting for this
 * thread to end -- which terminate did not do, so a waiter parked on a thread
 * that was killed under it stayed parked for the rest of the run. */
static void thread_ended(psp_thread *t, uint32_t status) {
    t->exit_status = status;
    t->state       = TH_DORMANT;
    for (int i = 0; i < t->nenders && i < MAX_SEMA_WAITERS; i++)
        psp_sched_wake(t->enders[i]);
    t->nenders = 0;
}

static void on_thread_end(uint32_t uid, uint32_t status) {
    psp_thread *t = find_thread(uid);
    if (!t) return;
    thread_ended(t, status);
}

/* sceKernelWaitThreadEnd(SceUID thid, SceUInt *timeout)
 *
 * The second argument is a *timeout pointer*, not somewhere to put the exit
 * status -- writing the status there corrupted whatever the guest kept at that
 * address. The status is the return value, as PPSSPP's implementation shows. */
static void hle_WaitThreadEnd(void) {
    if (!psp_sched_can_wait()) { psp_ret(SCE_KERNEL_ERROR_CAN_NOT_WAIT); return; }
    const uint32_t thid    = psp_arg(0);
    const uint32_t timeout = psp_arg(1);
    /* You cannot wait for yourself to end, and zero does not mean "me" here the
     * way it does for a priority change -- both are ILLEGAL_THID.
     * threads/threadend prints them next to an id that names nothing, which is
     * the different code: `Zero: 80020197`, `Self: 80020197`,
     * `Invalid: 80020198`. */
    if (thid == 0 || thid == psp_sched_current()) {
        psp_ret(SCE_KERNEL_ERROR_ILLEGAL_THID);
        return;
    }
    psp_thread *t = find_thread(thid);
    if (!t) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
    /* A thread that was never started will never end, so there is nothing to
     * wait for -- DORMANT, immediately, rather than the timeout. One that has
     * *finished* is also dormant and returns its exit status, so the test is
     * "was it ever started", not "is it stopped now". */
    if (!t->ever_started) { psp_ret(SCE_KERNEL_ERROR_DORMANT); return; }

    /* The same deadline the other waits use. This one was left on the untimed
     * path when they were converted, and it is not a wait that can be left
     * there: a caller waiting on a thread that sleeps forever has a timeout
     * precisely so that it can give up, and without one the whole run stops.
     * threads/threads/threadend is that test, and it went silent. */
    const uint64_t deadline = psp_wait_deadline(timeout);

    /* Park until it ends. This is what drives a freshly started thread: nothing
     * runs it until the thread holding the token gives it up, and a caller
     * waiting for its result is the usual moment that happens. */
    while (t->state != TH_DORMANT) {
        const uint32_t me = psp_sched_current();
        t->enders[t->nenders++ % MAX_SEMA_WAITERS] = me;
        const int rc = psp_sched_block_until(me, PSP_SCHED_BLOCKED,
                                             "sceKernelWaitThreadEnd", deadline);
        if (rc == PSP_SCHED_EXPIRED) {
            psp_wait_writeback(timeout, deadline);
            psp_ret(SCE_KERNEL_ERROR_WAIT_TIMEOUT);
            return;
        }
        if (rc != PSP_SCHED_WOKEN) {
            wait_deadlock("sceKernelWaitThreadEnd");
            psp_ret(SCE_KERNEL_ERROR_OK);
            return;
        }
        /* Woken, and the thread is gone: terminate-and-delete freed it out from
         * under this wait. Vanishing *during* the wait is not the same as never
         * having been there -- threads/threadend answers `800201ac` to the
         * first and `80020198` to the second, on consecutive lines. */
        t = find_thread(thid);
        if (!t) { psp_ret(SCE_KERNEL_ERROR_THREAD_TERMINATED); return; }
    }

    psp_wait_writeback(timeout, deadline);
    psp_ret(t->exit_status);
}

static void hle_GetThreadId(void) { psp_ret(psp_sched_current()); }

/* ---- the clock ------------------------------------------------------------
 *
 * Microseconds since the module started. See include/psprecomp/clock.h for why
 * it is virtual rather than the host's. A 64-bit result comes back in $v0:$v1,
 * low word first, which is the o32 convention the compiler emitted the caller
 * against. */
static void hle_GetSystemTimeWide(void) {
    const uint64_t us = psp_clock_read();
    psp_cpu.r[PSP_REG_V0] = (uint32_t)us;
    psp_cpu.r[PSP_REG_V1] = (uint32_t)(us >> 32);
}

static void hle_GetSystemTimeLow(void) {
    psp_ret((uint32_t)psp_clock_read());
}

/* Takes a SceKernelSysClock * to fill in rather than returning the value. */
static void hle_GetSystemTime(void) {
    const uint32_t out = psp_arg(0);
    const uint64_t us  = psp_clock_read();
    if (out) {
        psp_write32(out,     (uint32_t)us);
        psp_write32(out + 4, (uint32_t)(us >> 32));
    }
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* Suspend, resume and priority all have to reach the scheduler.
 *
 * Each of these used to write a thread-manager field and stop. That was
 * invisible while a thread ran to completion at its start point and nothing
 * was ever scheduled against anything: a suspended thread still ran, and a
 * reprioritised one was still ordered by the priority it was created with,
 * forever. With real threads they are the difference between a test passing
 * and the wrong thread holding the CPU. */
/* **A thread cannot suspend itself**, and neither call takes 0 to mean the
 * current one -- both answer ILLEGAL_THID. threads/threads/suspend measures all
 * eight situations for each:
 *
 *     Zero 80020197   Invalid 80020198   Created 800201a2   Ready 00000000
 *     Finished 800201a2   Deleted 80020198   Suspended 800201a3
 *     Current 80020197
 *
 * The self case is not a detail. Implementing suspend as "park until resumed"
 * and letting a thread apply it to itself deadlocked that test outright, and it
 * printed nothing at all -- the checkpoint buffer is only flushed at the end. */
static void hle_SuspendThread(void) {
    const uint32_t id = psp_arg(0);
    if (id == 0 || id == psp_sched_current()) {
        psp_ret(SCE_KERNEL_ERROR_ILLEGAL_THID);
        return;
    }
    psp_thread *t = find_thread(id);
    if (!t) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
    /* Never started, or already finished. */
    if (t->state == TH_DORMANT)   { psp_ret(SCE_KERNEL_ERROR_DORMANT); return; }
    if (t->state == TH_SUSPENDED) { psp_ret(SCE_KERNEL_ERROR_SUSPEND); return; }
    t->state = TH_SUSPENDED;
    psp_sched_suspend(t->uid);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_ResumeThread(void) {
    const uint32_t id = psp_arg(0);
    if (id == 0 || id == psp_sched_current()) {
        psp_ret(SCE_KERNEL_ERROR_ILLEGAL_THID);
        return;
    }
    psp_thread *t = find_thread(id);
    if (!t) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
    /* Resuming anything that is not suspended is an error, including a thread
     * that is merely ready -- the call is not idempotent. */
    if (t->state != TH_SUSPENDED) { psp_ret(SCE_KERNEL_ERROR_NOT_SUSPEND); return; }
    t->state = TH_READY;
    psp_sched_resume(t->uid);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_ChangeThreadPriority(void) {
    /* A zero thread id means the running thread, which is how a thread lowers
     * its own priority without asking what it is. */
    const uint32_t id = psp_arg(0) ? psp_arg(0) : psp_sched_current();
    psp_thread *t = find_thread(id);
    if (!t) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
    /* A thread that has never run, or has finished, has no priority to change:
     * threads/change.expected answers DORMANT for `Created` and `Finished`
     * where it answers 0 for `Ready`, `Suspended` and `Waiting`. */
    if (!t->ever_started || t->state == TH_DORMANT) {
        psp_ret(SCE_KERNEL_ERROR_DORMANT);
        return;
    }

    /* Priority zero is not a priority, it is "the one I am running at".
     * threads/change.expected shows a thread created at 0x30 being set to 0
     * and reading back **0x18**, which is what the caller had set itself to
     * two lines earlier -- so it is the *caller's* priority, not the target's
     * initial one, and neither is guessable from the other.
     *
     * Everything outside 0x08..0x77 is refused. The same file sweeps it: -2,
     * -1, 0x01 through 0x07, 0x78 and 0x79 all answer ILLEGAL_PRIORITY, and
     * 0x08 through 0x77 all succeed. The negative cases need no separate test
     * because the comparison is unsigned. */
    uint32_t prio = psp_arg(1);
    if (prio == 0) prio = psp_threadman_current_priority();
    else if (prio < 0x08u || prio > 0x77u) {
        psp_ret(SCE_KERNEL_ERROR_ILLEGAL_PRIORITY);
        return;
    }
    t->priority = prio;
    psp_sched_set_priority(t->uid, (int)t->priority);
    psp_ret(SCE_KERNEL_ERROR_OK);
    /* Lowering your own priority is a reschedule point: something that was
     * behind you may now be ahead. threads/change.expected tags that line `[r]`,
     * so hardware does switch there and it is observable.
     *
     * Raising *another* thread above the caller is the same reschedule, and it
     * was missing. sceKernelStartThread already performs it -- see the comment
     * in psp_sched_spawn -- and a priority change reaches the same state by a
     * different route. threads/change sweeps a ready thread through every legal
     * priority, and the four values higher than the caller's are exactly the
     * four where ` - testThread` appears *before* the line reporting the change
     * that caused it: the thread ran to completion inside the call. */
    if (t->uid == psp_sched_current()) psp_sched_yield();
    else if ((int)prio < (int)psp_threadman_current_priority()) psp_sched_preempt();
}

/* ---- sleep and wakeup ------------------------------------------------------
 *
 * Not a timed wait: a sleeping thread stays asleep until somebody wakes it by
 * name. The counter is what makes the pair race-free, and it is the whole
 * content of the call -- a wakeup that arrives *before* the sleep is remembered,
 * so the sleep returns at once rather than missing it and hanging forever. */
static void hle_SleepThread(void) {
    if (!psp_sched_can_wait()) { psp_ret(SCE_KERNEL_ERROR_CAN_NOT_WAIT); return; }
    psp_thread *t = current_thread();
    if (!t) { psp_ret(SCE_KERNEL_ERROR_OK); return; }
    if (t->wakeup_count > 0) { t->wakeup_count--; psp_ret(SCE_KERNEL_ERROR_OK); return; }

    t->wait_kind = WAIT_SLEEP;
    if (psp_sched_block(t->uid, PSP_SCHED_SLEEPING, "sceKernelSleepThread") != 0) {
        t->wait_kind = WAIT_NONE;
        wait_deadlock("sceKernelSleepThread");
        psp_ret(SCE_KERNEL_ERROR_OK);
        return;
    }
    /* Woken by name, so the wakeup this consumed is spent. */
    t = current_thread();
    if (t) t->wait_kind = WAIT_NONE;
    if (t && t->wakeup_count > 0) t->wakeup_count--;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_WakeupThread(void) {
    psp_thread *t = find_thread(psp_arg(0));
    if (!t) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
    t->wakeup_count++;
    const int urgent = psp_sched_wake(t->uid);
    psp_ret(SCE_KERNEL_ERROR_OK);
    if (urgent) psp_sched_yield();
}

/* Throw away wakeups that have been banked but not slept on, and say how many
 * there were -- which is the only way a thread can find out. */
static void hle_CancelWakeupThread(void) {
    const uint32_t id = psp_arg(0) ? psp_arg(0) : psp_sched_current();
    psp_thread *t = find_thread(id);
    if (!t) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
    const uint32_t n = (uint32_t)t->wakeup_count;
    t->wakeup_count = 0;
    psp_ret(n);
}

/* Stop switching threads for the duration of a critical section.
 *
 * The previous setting comes back so the guest can restore it rather than
 * assuming it was on -- these nest, and a resume that unconditionally enabled
 * dispatch would break the outer one. pspautotests' scheduling/dispatch is an
 * entire test of this, and without it that test deadlocks before it prints
 * anything at all. */
/* These do *not* nest. Suspending dispatch that is already suspended is an
 * error, and so is resuming with anything that is not a state a suspend
 * returned -- which is how the second failure in dispatch.expected arises, the
 * test handing the failed suspend's error code straight to resume. */
static void hle_SuspendDispatchThread(void) {
    if (!psp_sched_can_wait()) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_CONTEXT); return; }
    psp_ret((uint32_t)psp_sched_set_dispatch(0));
}

static void hle_ResumeDispatchThread(void) {
    const uint32_t state = psp_arg(0);
    if (state > 1) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_CONTEXT); return; }
    psp_sched_set_dispatch((int)state);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* A module's entry point is not running on "no thread".
 *
 * On hardware the loader creates a thread to call module_start and that thread
 * has a priority like any other -- 0x20 for a user module. Here module_start
 * is called directly, so psp_sched_current() is zero until the guest creates
 * threads of its own, and answering 0 says "priority zero", which is a real
 * and very high priority rather than an absence.
 *
 * pspautotests threads/mutex/unlock2 opens by checking exactly this and
 * refuses to run at all when it is wrong. */
#define PSP_MAIN_THREAD_PRIORITY 0x20

/* The next uid, shared so that every kernel object type draws from one space --
 * which is what makes a handle of the wrong type resolve to nothing. */
uint32_t psp_threadman_next_uid(void) { return g_next_uid++; }

uint32_t psp_threadman_current_priority(void) {
    const psp_thread *c = current_thread();
    return c ? c->priority : PSP_MAIN_THREAD_PRIORITY;
}

static void hle_GetThreadCurrentPriority(void) {
    psp_ret(psp_threadman_current_priority());
}

/* Terminate stops a thread without freeing it. Unimplemented until now, which
 * mattered more than it looks: pspautotests' checkpoint helper terminates its
 * rescheduler thread between every pair of checks, and a terminate that does
 * nothing leaves that thread to run later. */
/* **A thread cannot terminate itself**, exactly as it cannot suspend itself,
 * and with the same code: threads/terminate.expected answers
 * `Current: 80020197`.
 *
 * That is not a detail either. The test terminates its own thread on one line
 * of every block, and once terminate actually worked, obeying it killed the
 * thread that was going to flush the checkpoint buffer -- so the whole file
 * emitted nothing at all. A rule that reads like a nicety, silencing a test. */
static void hle_TerminateThread(void) {
    const uint32_t id = psp_arg(0);
    if (id == 0 || id == psp_sched_current()) {
        psp_ret(SCE_KERNEL_ERROR_ILLEGAL_THID);
        return;
    }
    psp_thread *t = find_thread(id);
    if (!t) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
    /* Never started, or already finished: there is nothing to terminate. */
    if (!t->ever_started || t->state == TH_DORMANT) {
        psp_ret(SCE_KERNEL_ERROR_DORMANT);
        return;
    }
    /* Both models: cancel a spawn the interpreter is holding, *and* stop a real
     * host thread if there is one. Only the first existed, and it is a no-op
     * whenever the second is what is needed. */
    psp_sched_cancel_spawn(t->uid);
    psp_sched_terminate(t->uid);
    thread_ended(t, SCE_KERNEL_ERROR_THREAD_TERMINATED);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* Terminate-and-delete is not terminate followed by delete: it accepts a thread
 * terminate would refuse. threads/terminate.expected runs the same ten cases
 * through both and the two dormant ones are where they part --
 *
 *     sceKernelTerminateThread        Created: 800201a2   Finished: 800201a2
 *     sceKernelTerminateDeleteThread  Created: 00000000   Finished: 00000000
 *
 * -- because there is nothing to stop but there is still something to free. The
 * id checks are terminate's, though, including the one that forbids a thread
 * from ending itself. */
static void hle_TerminateDeleteThread(void) {
    const uint32_t id = psp_arg(0);
    if (id == 0 || id == psp_sched_current()) {
        psp_ret(SCE_KERNEL_ERROR_ILLEGAL_THID);
        return;
    }
    psp_thread *t = find_thread(id);
    if (!t) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
    if (t->ever_started && t->state != TH_DORMANT) {
        psp_sched_terminate(t->uid);
        thread_ended(t, SCE_KERNEL_ERROR_THREAD_TERMINATED);
    }
    psp_sched_cancel_spawn(t->uid);
    release_stack(t);
    t->used = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* sceKernelChangeCurrentThreadAttr(clear, set)
 *
 * One bit of the attribute word is the thread's own business, and it is
 * PSP_THREAD_ATTR_VFPU. Every other bit belongs to the kernel and naming one is
 * an error, not a no-op -- which is what this was, applying whatever it was
 * handed.
 *
 * threads/change sweeps all thirty-two bits through both arguments and gets
 * `80020191` for thirty-one of them in each direction, twice:
 *
 *     add:     4000: 00000000, attr=800040ff     (already set, so unchanged)
 *     remove:  4000: 00000000, attr=800000ff
 *
 * -- and the attribute readback on the next line proves the refused calls
 * changed nothing, because it holds still across the whole sweep. */
#define PSP_THREAD_ATTR_VFPU 0x00004000u

static void hle_ChangeCurrentThreadAttr(void) {
    const uint32_t clear = psp_arg(0), set = psp_arg(1);
    if ((clear | set) & ~PSP_THREAD_ATTR_VFPU) {
        psp_ret(SCE_KERNEL_ERROR_ILLEGAL_ATTR);
        return;
    }
    psp_thread *c = current_thread();
    if (c) c->attr = (c->attr & ~clear) | set;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_GetThreadStackFreeSize(void) {
    /* Real firmware walks the stack looking for the fill pattern. We do not
     * paint one, so report the whole stack: it is used for "am I close to
     * overflowing", and claiming plenty of room is the safe direction. */
    psp_thread *t = find_thread(psp_arg(0));
    const psp_thread *c = current_thread();
    psp_ret(t ? t->stack_size : (c ? c->stack_size : 0));
}

/* ---- semaphores ---------------------------------------------------------- */

/* PSPRECOMP_SEMA=<substring> narrates the traffic on the semaphores whose name
 * contains it -- who waits, who signals, and what the count was each time.
 *
 * "Nobody signals this semaphore" is the shape of several bugs here, and the
 * thread dump can only say a thread is parked on one. It cannot say whether
 * the signal never came or came too early, and those need opposite fixes. A
 * signal also dumps the guest function trace, which names the code that sent
 * it; a wait does not, because waits are the common case and the trace is long.
 *
 * Off unless the variable is set, and matched by substring so PSPRECOMP_SEMA=Movie
 * covers a whole subsystem at once. */
static const char *sema_watch(void) {
    static int done;
    static const char *v;
    if (!done) { v = getenv("PSPRECOMP_SEMA"); done = 1; }
    return v;
}

static int sema_watched(const psp_sema *s) {
    const char *w = sema_watch();
    return w && *w && strstr(s->name, w) != NULL;
}

static void sema_log(const psp_sema *s, const char *op, int32_t arg, int trace) {
    if (!sema_watched(s)) return;
    fprintf(stderr, "sema: thread 0x%08X  %-7s uid 0x%08X %-20s count=%d arg=%d\n",
            psp_sched_current(), op, s->uid, s->name, s->count, arg);
    if (trace) psp_trace_dump();
}

/* Every distinct semaphore uid ever handed to sceKernelSignalSema.
 *
 * "Nobody signals this one" is a claim about code that was never executed, and
 * the watch above can only report signals that happened. This records the whole
 * set instead, so the claim can be checked against it rather than inferred from
 * silence -- including the case where the name the watch matches on is not the
 * object the waiter is actually parked on. */
#define MAX_SIGNALLED_UIDS 64
static uint32_t g_sig_uid[MAX_SIGNALLED_UIDS];
static int      g_sig_uids;

static void note_signalled(uint32_t uid) {
    for (int i = 0; i < g_sig_uids; i++) if (g_sig_uid[i] == uid) return;
    if (g_sig_uids < MAX_SIGNALLED_UIDS) g_sig_uid[g_sig_uids++] = uid;
}

/* Every thread the game ever created, alive or not.
 *
 * The scheduler's list is of threads that still exist, which cannot answer what
 * became of one that is missing -- created and never started looks identical to
 * never created at all, and both look identical to started and long since
 * finished. This keeps the record instead. */
void psp_threadman_dump_threads(FILE *out) {
    static const char *const ST[] = { "dormant", "ready", "running", "suspended" };
    fprintf(out, "  threads created:\n");
    for (int i = 0; i < MAX_THREADS; i++) {
        const psp_thread *t = &g_thread[i];
        if (!t->uid) continue;          /* never allocated */
        fprintf(out, "    uid 0x%08X  entry 0x%08X  prio %-3u  %-9s  exit %u  %s%s\n",
                t->uid, t->entry, t->priority,
                (unsigned)t->state < 4 ? ST[t->state] : "?",
                t->exit_status, t->name,
                t->used ? "" : "  (deleted)");
    }
}

void psp_threadman_dump_signalled(FILE *out) {
    fprintf(out, "  semaphore uids ever signalled (%d):", g_sig_uids);
    for (int i = 0; i < g_sig_uids; i++) fprintf(out, " 0x%08X", g_sig_uid[i]);
    fprintf(out, "\n");
    for (int i = 0; i < MAX_SEMAS; i++) {
        if (!g_sema[i].used) continue;
        int seen = 0;
        for (int k = 0; k < g_sig_uids; k++) if (g_sig_uid[k] == g_sema[i].uid) seen = 1;
        fprintf(out, "    uid 0x%08X  %-24s %s\n", g_sema[i].uid, g_sema[i].name,
                seen ? "signalled" : "NEVER SIGNALLED");
    }
}

/* A NULL name is rejected, and the attribute word is range-checked.
 *
 * Both are observable and neither was done. The pspautotests create tests
 * pass a null pointer as the name and expect SCE_KERNEL_ERROR_ERROR back;
 * we accepted it, read a string from guest address 0, and reported success.
 * The two objects that answer NO_MEMORY instead of ERROR (fpl, msgpipe) are
 * not implemented here, so the single code is right for everything that is.
 *
 * **The attribute rule is per object type, and sharing one check broke the
 * game.** A semaphore takes the low nine bits and refuses anything above them;
 * that was measured from semaphores/create.expected, and then applied to event
 * flags too, where it is simply false. Armored Core creates an event flag with
 * attribute 0x200 -- PSP_EVENT_WAITMULTIPLE, an ordinary and documented flag
 * attribute -- was told ILLEGAL_ATTR, used the error code as a uid, and the run
 * went from 633 GE lists to 0 with 123,606 bad memory accesses.
 *
 * The lesson is the one state.md keeps recording in other forms: a measurement
 * about one object is not a fact about its neighbours. Both rules below come
 * from a hardware capture, and the two capture files disagree with each other. */
static int name_ok(uint32_t name_ptr) { return name_ptr != 0; }

/* semaphores/create.expected: 1, 0x100 and 0x1ff are accepted; 0x200, 0x400,
 * 0x800, 0x900, 0x1000, 0x2000, 0x4000, 0x8000 and 0x10000 are all refused. */
static int sema_attr_ok(uint32_t attr) { return attr < 0x200u; }

/* events/create/create.expected, all ten of its cases:
 *
 *     0x000 ok   0x001 ok   0x010 ok   0x100 FAIL  0x122 FAIL
 *     0x200 ok   0x222 ok   0x300 FAIL 0x900 FAIL  0x1200 FAIL
 *
 * So bit 0x100 is illegal here where it is legal for a semaphore, bit 0x200 is
 * legal here where it is illegal for a semaphore, and nothing above 0x2FF is
 * allowed at all. Every one of the ten fits this and nothing narrower does. */
static int flag_attr_ok(uint32_t attr) {
    return (attr & ~0x2FFu) == 0 && (attr & 0x100u) == 0;
}

/* Copy a name into a guest SceKernel*Info block: 32 bytes, truncated to 31
 * characters and NUL-terminated, which is what hardware reports back for the
 * 31-character name the create tests hand it. */
void psp_threadman_write_name(uint32_t dst, const char *name) {
    char buf[32];
    memset(buf, 0, sizeof buf);
    for (int i = 0; i < 31 && name[i]; i++) buf[i] = name[i];
    for (int i = 0; i < 32; i++) psp_write8(dst + (uint32_t)i, (uint8_t)buf[i]);
}

static void hle_CreateSema(void) {
    /* (name, attr, initVal, maxVal, option) */
    if (!name_ok(psp_arg(0))) { psp_ret(SCE_KERNEL_ERROR_ERROR); return; }
    if (!sema_attr_ok(psp_arg(1))) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_ATTR); return; }
    psp_sema *s = NULL;
    for (int i = 0; i < MAX_SEMAS; i++) if (!g_sema[i].used) { s = &g_sema[i]; break; }
    if (!s) { psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }

    memset(s, 0, sizeof *s);
    psp_str(psp_arg(0), s->name, sizeof s->name);
    s->attr       = psp_arg(1);
    s->count      = (int32_t)psp_arg(2);
    s->init_count = (int32_t)psp_arg(2);
    s->max_count  = (int32_t)psp_arg(3);
    s->uid = g_next_uid++;
    s->used = 1;
    /* Through a local: source and destination are both inside g_sema, and the
     * compiler cannot see that two distinct members never overlap. */
    char nm[sizeof s->name];
    memcpy(nm, s->name, sizeof nm);
    snprintf(s->waitdesc, sizeof s->waitdesc, "sceKernelWaitSema(%s)", nm);
    sema_log(s, "create", s->count, 0);
    psp_ret(s->uid);
}

static void hle_DeleteSema(void) {
    psp_sema *s = find_sema(psp_arg(0));
    if (!s) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_SEMID); return; }
    /* Everyone parked on it has to be let go, not left parked on an object that
     * no longer exists -- each discovers for itself that its lookup now fails. */
    const int urgent = psp_waitq_release_all(&s->q);
    s->used = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
    if (urgent) psp_sched_yield();
}

static void hle_SignalSema(void) {
    psp_sema *s = find_sema(psp_arg(0));
    if (!s) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_SEMID); return; }
    s->count += (int32_t)psp_arg(1);
    if (s->max_count > 0 && s->count > s->max_count) s->count = s->max_count;
    note_signalled(s->uid);
    sema_log(s, "signal", (int32_t)psp_arg(1), 1);

    /* Release in the object's own order, deducting on each waiter's behalf.
     *
     * This used to wake everyone and let them re-test, which is not a
     * conservative version of the same thing: the winner was then whichever
     * thread the *scheduler* picked, always the most urgent, so a FIFO
     * semaphore behaved like a priority one. See waitq.h. */
    const int urgent = sema_release(s);

    psp_ret(SCE_KERNEL_ERROR_OK);
    /* Released a thread that outranks us, so it runs now. */
    if (urgent) psp_sched_yield();
}

/* Take the semaphore if it can be taken, and never block. The distinction from
 * WaitSema is the whole point of the call: a caller uses it precisely because
 * it has something else to do when the answer is no. */
static void hle_PollSema(void) {
    psp_sema *s = find_sema(psp_arg(0));
    if (!s) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_SEMID); return; }
    const int32_t need = (int32_t)psp_arg(1);
    if (s->count < need) { psp_ret(SCE_KERNEL_ERROR_SEMA_ZERO); return; }
    s->count -= need;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* Give the rest of this priority level a turn. With round-robin handoff a
 * plain yield already does exactly that, and the priority argument only
 * selects a level we would reach anyway. */
static void hle_RotateReadyQueue(void) {
    psp_sched_yield();
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* It answered OK and deleted nothing, so a notify on a deleted callback
 * succeeded where callbacks/notify expects `Deleted: Failed (800201a1)`. */
static void hle_DeleteCallback(void) {
    psp_callback *c = find_cb(psp_arg(0));
    if (!c) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_CBID); return; }
    c->used = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* ---- the shape every kernel wait has ---------------------------------------
 *
 * The timeout argument is a *pointer*, and it is in and out. Hardware reads how
 * long to wait and, when the wait ends, writes back how much of it was left --
 * which the tests print, so it is observable rather than a nicety.
 * semaphores/wait.expected pins all three cases:
 *
 *     Signaled: OK (500ms left)                 immediate, nothing spent
 *     Wait timeout: ... remaining=4             blocked, woken with 4ms of 5000 left
 *     Never signaled: Failed (800201A8, 0ms)    ran out
 *
 * and a fourth, which is why the write-back is conditional: a call that fails
 * on its *arguments* -- `Greater than max: Failed (800201BD, 500ms left)` --
 * leaves the word alone, because it never waited.
 *
 * This was previously read only for its non-NULL-ness, to decide whether a
 * fabricated timeout was allowed. The duration itself was discarded. */

/* Hand the count to whoever is next in line, as far as it will go.
 *
 * The head blocks the queue -- a waiter the count cannot satisfy is not skipped
 * over, and a *later* caller does not take what it is waiting for.
 * threads/semaphores/fifo measures both halves in eight lines: with a count of
 * 1, a thread asking for 5 parks; a second thread asking for 1 parks behind it
 * rather than taking what is there; and only when the first times out and
 * leaves the queue does the second get it.
 *
 * That is also why this runs whenever the queue *changes*, not only on a
 * signal. A waiter leaving on its own timeout can unblock the one behind it. */
static int sema_release(psp_sema *s) {
    int urgent = 0;
    for (;;) {
        const int i = psp_waitq_pick(&s->q, s->attr);
        if (i < 0 || s->count < (int32_t)s->q.w[i].need) break;
        const psp_waiter w = psp_waitq_take(&s->q, i);
        s->count -= (int32_t)w.need;
        sema_log(s, "taken", (int32_t)w.need, 0);
        urgent |= psp_sched_wake(w.uid);
    }
    return urgent;
}

static void hle_WaitSema(void) {
    if (!psp_sched_can_wait()) { psp_ret(SCE_KERNEL_ERROR_CAN_NOT_WAIT); return; }
    const uint32_t id      = psp_arg(0);
    const int32_t  need    = (int32_t)psp_arg(1);
    const uint32_t tmo_ptr = psp_arg(2);
    /* Zero, negative, or more than the semaphore could ever hold. Answered
     * before the count is consulted and without touching the timeout word --
     * semaphores/wait.expected reports `Greater than max: Failed (800201BD,
     * 500ms left)`, so the call never waited. */
    psp_sema *s = find_sema(id);
    if (!s) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_SEMID); return; }
    if (need <= 0 || (s->max_count > 0 && need > s->max_count)) {
        psp_ret(SCE_KERNEL_ERROR_ILLEGAL_COUNT);
        return;
    }

    const uint64_t deadline = psp_wait_deadline(tmo_ptr);

    /* Available, and nobody ahead of us. Both conditions: see sema_release. */
    if (psp_waitq_count(&s->q) == 0 && s->count >= need) {
        s->count -= need;
        sema_log(s, "taken", need, 0);
        psp_wait_writeback(tmo_ptr, deadline);
        psp_ret(SCE_KERNEL_ERROR_OK);
        return;
    }

    sema_log(s, "park", need, 0);
    const uint32_t me = psp_sched_current();
    if (psp_waitq_add(&s->q, me, (uint32_t)need, 0, 0) != 0) {
        psp_ret(SCE_KERNEL_ERROR_NO_MEMORY);
        return;
    }

    const int rc = psp_sched_block_until(me, PSP_SCHED_BLOCKED, s->waitdesc,
                                         deadline);

    /* Gone while we were parked, which is what sceKernelDeleteSema releasing
     * its waiters looks like from in here -- and a different answer from asking
     * about a semaphore that was already gone before the call. */
    s = find_sema(id);
    if (!s) { psp_ret(SCE_KERNEL_ERROR_WAIT_DELETE); return; }

    if (rc == PSP_SCHED_WOKEN) {
        /* The signaller already took the count on our behalf, so there is
         * nothing to re-test: being woken *is* the semaphore. */
        psp_wait_writeback(tmo_ptr, deadline);
        psp_ret(SCE_KERNEL_ERROR_OK);
        return;
    }

    /* Not released, so we are still queued. Leaving may unblock the thread
     * behind us -- that is the second half of what fifo.expected measures. */
    psp_waitq_drop(&s->q, me);
    const int urgent = sema_release(s);

    if (rc == PSP_SCHED_EXPIRED) {
        psp_wait_writeback(tmo_ptr, deadline);
        psp_ret(SCE_KERNEL_ERROR_WAIT_TIMEOUT);
        if (urgent) psp_sched_yield();
        return;
    }

    /* Stranded: nothing is runnable and no deadline can release us, so nobody
     * could ever signal this. A caller that asked for no timeout is not told
     * one elapsed -- a fabricated timeout is something a game acts on. */
    wait_deadlock("sceKernelWaitSema");
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* ---- event flags --------------------------------------------------------- */

#define PSP_EVENT_WAITAND   0x00
#define PSP_EVENT_WAITOR    0x01
#define PSP_EVENT_WAITCLEAR 0x20
/* The creation attribute that lets more than one thread wait at once. It is
 * also the bit an event flag accepts where a semaphore refuses it -- see the
 * two attribute rules above. */
#define PSP_EVENT_WAITMULTIPLE 0x200

static void hle_CreateEventFlag(void) {
    /* (name, attr, bits, option) */
    if (!name_ok(psp_arg(0))) { psp_ret(SCE_KERNEL_ERROR_ERROR); return; }
    if (!flag_attr_ok(psp_arg(1))) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_ATTR); return; }
    psp_evflag *f = NULL;
    for (int i = 0; i < MAX_FLAGS; i++) if (!g_flag[i].used) { f = &g_flag[i]; break; }
    if (!f) { psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }

    memset(f, 0, sizeof *f);
    psp_str(psp_arg(0), f->name, sizeof f->name);
    f->attr         = psp_arg(1);
    f->pattern      = psp_arg(2);
    f->init_pattern = psp_arg(2);
    f->uid = g_next_uid++;
    f->used = 1;
    char nm[sizeof f->name];
    memcpy(nm, f->name, sizeof nm);
    snprintf(f->waitdesc, sizeof f->waitdesc, "sceKernelWaitEventFlag(%s)", nm);
    psp_ret(f->uid);
}

static void hle_DeleteEventFlag(void) {
    psp_evflag *f = find_flag(psp_arg(0));
    if (!f) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_EVFID); return; }
    const int urgent = psp_waitq_release_all(&f->q);
    f->used = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
    if (urgent) psp_sched_yield();
}

static int flag_satisfied(const psp_evflag *f, uint32_t bits, uint32_t mode) {
    return (mode & PSP_EVENT_WAITOR) ? (f->pattern & bits) != 0
                                     : (f->pattern & bits) == bits;
}

/* Release every waiter the pattern now satisfies, in the object's order.
 *
 * Unlike a semaphore there is no head-of-line blocking here: waiters ask for
 * different bit patterns, so one that cannot be satisfied says nothing about
 * the next. What does have to stay ordered is WAITCLEAR -- a released waiter
 * consumes the bits it woke on, so who is considered first decides who gets
 * them, and that is the whole content of the attribute.
 *
 * The pattern each waiter woke on goes to the address it supplied, which is why
 * the queue carries that address: by the time the release happens the waiter's
 * own frame is not reachable from here. */
static int flag_release(psp_evflag *f) {
    int urgent = 0;
    for (;;) {
        /* The satisfiable waiters, in the queue's own arrival order, so that
         * psp_waitq_pick stays the single place that knows what the attribute
         * means. Copying them out is what lets an unsatisfiable waiter be
         * passed over without being removed. */
        psp_waitq eligible = { 0 };
        for (int i = 0; i < psp_waitq_count(&f->q); i++) {
            const psp_waiter *w = &f->q.w[i];
            if (flag_satisfied(f, w->need, w->mode))
                psp_waitq_add(&eligible, w->uid, w->need, w->mode, w->out);
        }
        const int i = psp_waitq_pick(&eligible, f->attr);
        if (i < 0) break;

        const psp_waiter w = eligible.w[i];
        psp_waitq_drop(&f->q, w.uid);
        if (w.out) psp_write32(w.out, f->pattern);
        if (w.mode & PSP_EVENT_WAITCLEAR) f->pattern &= ~w.need;
        urgent |= psp_sched_wake(w.uid);
    }
    return urgent;
}

static void hle_SetEventFlag(void) {
    psp_evflag *f = find_flag(psp_arg(0));
    if (!f) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_EVFID); return; }
    f->pattern |= psp_arg(1);
    const int urgent = flag_release(f);
    psp_ret(SCE_KERNEL_ERROR_OK);
    if (urgent) psp_sched_yield();
}

static void hle_ClearEventFlag(void) {
    psp_evflag *f = find_flag(psp_arg(0));
    if (!f) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_EVFID); return; }
    /* The argument is a mask of bits to KEEP, not bits to clear. Getting this
     * backwards leaves a game waiting on a flag that never clears. */
    f->pattern &= psp_arg(1);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* sceKernelWaitEventFlag(evfid, bits, mode, outBits, timeout)
 *
 * The fifth argument is psp_arg(4), and that question is settled rather than
 * open. A previous version of this function returned a fabricated timeout
 * instead of blocking, on the grounds that o32 spills argument five to sp+16
 * while psp_arg(4) reads $t0 -- so which one the guest used was unknown, and
 * guessing had already been wrong once.
 *
 * hle.h answers it, with a disassembly and a bug that turned on it: PSP
 * firmware stubs do *not* spill. They load $t0-$t3 and branch, visible in the
 * delay slot of every such call, and reading sp+16 instead returned whatever
 * the stack happened to hold -- which is what once made the allocator refuse a
 * 15.9 MB request. So the fifth argument is $t0, the ordinary rule applies, and
 * this can block like every other wait. */
static void hle_WaitEventFlag(void) {
    if (!psp_sched_can_wait()) { psp_ret(SCE_KERNEL_ERROR_CAN_NOT_WAIT); return; }
    const uint32_t id      = psp_arg(0);
    const uint32_t bits    = psp_arg(1);
    const uint32_t mode    = psp_arg(2);
    const uint32_t out     = psp_arg(3);
    const uint32_t tmo_ptr = psp_arg(4);

    /* Arguments before the handle, which is the order hardware uses and is
     * observable. events/wait/wait.expected answers a wait for *no bits* on a
     * NULL, invalid or deleted flag with EVF_ILPAT rather than UNKNOWN_EVFID,
     * and a wrong mode on a NULL flag with ILLEGAL_MODE -- so both checks see
     * the arguments before anything has looked the object up. */
    if (bits == 0) { psp_ret(SCE_KERNEL_ERROR_EVF_ILPAT); return; }
    /* WAITOR and WAITCLEAR and nothing else: 0x02, 0x04, 0x08, 0x40, 0x80 and
     * 0xFF are each refused in that same file. */
    if (mode & ~(uint32_t)(PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR)) {
        psp_ret(SCE_KERNEL_ERROR_ILLEGAL_MODE);
        return;
    }

    psp_evflag *f = find_flag(id);
    if (!f) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_EVFID); return; }

    const uint64_t deadline = psp_wait_deadline(tmo_ptr);

    if (flag_satisfied(f, bits, mode)) {
        if (out) psp_write32(out, f->pattern);
        if (mode & PSP_EVENT_WAITCLEAR) f->pattern &= ~bits;
        psp_wait_writeback(tmo_ptr, deadline);
        psp_ret(SCE_KERNEL_ERROR_OK);
        return;
    }

    /* One waiter, unless the flag was created to allow more. That is what
     * attribute 0x200 is -- PSP_EVENT_WAITMULTIPLE -- and it is the same bit
     * the create test showed an event flag accepting where a semaphore refuses
     * it. A second waiter without it is refused rather than queued. */
    if (psp_waitq_count(&f->q) > 0 && !(f->attr & PSP_EVENT_WAITMULTIPLE)) {
        psp_ret(SCE_KERNEL_ERROR_EVF_MULTI);
        return;
    }

    const uint32_t me = psp_sched_current();
    if (psp_waitq_add(&f->q, me, bits, mode, out) != 0) {
        psp_ret(SCE_KERNEL_ERROR_NO_MEMORY);
        return;
    }

    const int rc = psp_sched_block_until(me, PSP_SCHED_BLOCKED, f->waitdesc,
                                         deadline);

    /* Gone while we were parked, which is what sceKernelDeleteEventFlag
     * releasing its waiters looks like from in here -- and a different answer
     * from asking about a flag that was already gone before the call. */
    f = find_flag(id);
    if (!f) { psp_ret(SCE_KERNEL_ERROR_WAIT_DELETE); return; }

    if (rc == PSP_SCHED_WOKEN) {
        /* flag_release already wrote the pattern we woke on and applied our
         * clear, on our behalf and in the object's order. */
        psp_wait_writeback(tmo_ptr, deadline);
        psp_ret(SCE_KERNEL_ERROR_OK);
        return;
    }

    psp_waitq_drop(&f->q, me);
    if (out) psp_write32(out, f->pattern);

    if (rc == PSP_SCHED_EXPIRED) {
        psp_wait_writeback(tmo_ptr, deadline);
        psp_ret(SCE_KERNEL_ERROR_WAIT_TIMEOUT);
        return;
    }

    wait_deadlock("sceKernelWaitEventFlag");
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* ---- callbacks ----------------------------------------------------------- */

/* The Refer*Status calls: what an object currently is, written into a struct
 * the caller supplies.
 *
 * None of the three existed. An unregistered firmware call returns without
 * touching its out-parameter, so the caller prints its own uninitialised
 * stack -- the create tests reported `attr=167767488, init=1308, cur=43`
 * where hardware writes zeros. Same failure as the missing
 * sceDisplayGetFrameBuf: success reported, nothing written.
 *
 * `size` is written rather than left alone. A caller sets it before the call
 * and hardware fills what fits, so echoing the real size is right for the
 * common case and better than leaving a field the caller may not have set. */
static void hle_ReferSemaStatus(void) {
    const psp_sema *sm = find_sema(psp_arg(0));
    const uint32_t info = psp_arg(1);
    if (!sm)   { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_SEMID); return; }
    if (!info) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
    psp_write32(info +  0, 56);
    psp_threadman_write_name(info + 4, sm->name);
    psp_write32(info + 36, sm->attr);
    psp_write32(info + 40, (uint32_t)sm->init_count);
    psp_write32(info + 44, (uint32_t)sm->count);
    psp_write32(info + 48, (uint32_t)sm->max_count);
    psp_write32(info + 52, (uint32_t)psp_waitq_count(&sm->q));
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_ReferEventFlagStatus(void) {
    const psp_evflag *f = find_flag(psp_arg(0));
    const uint32_t info = psp_arg(1);
    if (!f)    { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_EVFID); return; }
    if (!info) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
    psp_write32(info +  0, 52);
    psp_threadman_write_name(info + 4, f->name);
    psp_write32(info + 36, f->attr);
    psp_write32(info + 40, f->init_pattern);
    psp_write32(info + 44, f->pattern);
    psp_write32(info + 48, (uint32_t)psp_waitq_count(&f->q));
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* No lookup existed: callbacks are only ever created here, never resolved. */
static psp_callback *find_cb(uint32_t id) {
    for (int i = 0; i < MAX_CBS; i++)
        if (g_cb[i].used && g_cb[i].uid == id) return &g_cb[i];
    return NULL;
}

static void hle_ReferCallbackStatus(void) {
    const psp_callback *c = find_cb(psp_arg(0));
    const uint32_t info = psp_arg(1);
    if (!c)    { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_UID); return; }
    if (!info) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
    psp_write32(info +  0, 56);
    psp_threadman_write_name(info + 4, c->name);
    psp_write32(info + 36, c->thread);
    psp_write32(info + 40, c->func);
    psp_write32(info + 44, c->arg);
    psp_write32(info + 48, c->notify_count);
    psp_write32(info + 52, c->notify_arg);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* sceKernelReferThreadStatus: the whole of what a thread is, in 104 bytes.
 *
 * The size is not a guess -- threads/refer.expected reports `=> 104` for a
 * caller that asks for more than the structure holds. The layout is the SDK's
 * SceKernelThreadInfo, and the fields past exitStatus (run clocks, preemption
 * counts) are written as zero rather than invented: the test itself has them
 * commented out with the note that getting them right would be slow. */
static void hle_ReferThreadStatus(void) {
    const uint32_t id   = psp_arg(0) ? psp_arg(0) : psp_sched_current();
    const uint32_t info = psp_arg(1);
    const psp_thread *t = find_thread(id);
    if (!t)    { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
    if (!info) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }

    /* The reported attribute is not the one the caller passed: hardware ORs in
     * 0x800000FF. create.expected reports `attr=800000ff` for a thread created
     * with 0 and `attr=807000ff` for one created with 0x700000 -- twenty-eight
     * rows, one rule. */
    const uint32_t attr = 0x800000FFu | t->attr;

    /* `status` is the kernel's own enumeration, not this file's TH_*, and a
     * thread that has never been started reports STOPPED (16).
     *
     * For a live thread the scheduler is the authority, not the TH_* field:
     * this file records that a thread was started and never that it parked, so
     * a thread sitting in sceKernelSleepThread read back as READY. It is
     * WAITING, and threads/threadend puts the two next to each other --
     * `before start status=00000010`, `after start status=00000004` -- with
     * nothing between them but the start of a thread whose whole body is a
     * sleep. */
    const int dormant = !t->ever_started || t->state == TH_DORMANT;
    uint32_t status = PSP_THREAD_STATUS_STOPPED;
    if (!dormant) {
        switch (psp_sched_state_of(t->uid)) {
            case PSP_SCHED_RUNNING:   status = PSP_THREAD_STATUS_RUNNING; break;
            case PSP_SCHED_READY:     status = PSP_THREAD_STATUS_READY;   break;
            case PSP_SCHED_BLOCKED:
            case PSP_SCHED_SLEEPING:  status = PSP_THREAD_STATUS_WAITING; break;
            case PSP_SCHED_SUSPENDED: status = PSP_THREAD_STATUS_SUSPEND; break;
            /* No live slot: threading is off, or it was never spawned. Fall
             * back to what this file knows. */
            case PSP_SCHED_DEAD:
                status = t->state == TH_RUNNING   ? PSP_THREAD_STATUS_RUNNING
                       : t->state == TH_SUSPENDED ? PSP_THREAD_STATUS_SUSPEND
                                                  : PSP_THREAD_STATUS_READY;
                break;
        }
    }

    /* Three different answers for `exitStatus`, and none of them is zero:
     * DORMANT for a thread never started, NOT_DORMANT for one still running --
     * it has not exited, so there is nothing to report -- and what it returned
     * for one that has finished. create.expected reads 800201a2 throughout,
     * refer.expected 800201a4. */
    const uint32_t exit_status = !t->ever_started ? SCE_KERNEL_ERROR_DORMANT
                               : dormant          ? t->exit_status
                                                  : SCE_KERNEL_ERROR_NOT_DORMANT;

    /* Its current priority is its initial one until the scheduler has a slot
     * for it; psp_sched_priority answers with a sort-last sentinel otherwise,
     * and that is not a priority. */
    const int slot_pri = psp_sched_priority(t->uid);

    const uint32_t words[26] = {
        104, 0,0,0,0,0,0,0,0,                        /* size, then name[32] */
        attr, status, t->entry, t->stack_base, t->stack_size,
        psp_cpu.r[PSP_REG_GP], t->init_priority,
        slot_pri > 0x7F ? t->priority : (uint32_t)slot_pri,
        0 /*waitType*/, 0 /*waitId*/, (uint32_t)t->wakeup_count, exit_status,
        0,0,0,0,0,          /* run clocks and preemption counts, left at zero */
    };
    uint8_t buf[104];
    for (int w = 0; w < 26; w++)
        for (int b = 0; b < 4; b++) buf[w * 4 + b] = (uint8_t)(words[w] >> (b * 8));
    for (int i = 0; i < 31 && t->name[i]; i++) buf[4 + i] = (uint8_t)t->name[i];

    /* Only as many bytes as the caller says it has room for. refer.expected
     * sweeps the size field and watches the exit word, which starts at offset
     * 80: untouched at 80, half written at 82 (`ffff01a4`), whole at 108. A
     * refer that wrote all 104 regardless differs on every row of that sweep,
     * and one asked for zero bytes writes none at all. */
    uint32_t room = psp_read32(info);
    if (room > sizeof buf) room = sizeof buf;
    for (uint32_t i = 0; i < room; i++) psp_write8(info + i, buf[i]);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* ---- sceKernelGetThreadmanIdList ------------------------------------------ */

#define MAX_LISTERS 8
static psp_uid_lister g_lister[MAX_LISTERS];
static int            g_listers;

void psp_threadman_add_lister(psp_uid_lister fn) {
    if (g_listers < MAX_LISTERS) g_lister[g_listers++] = fn;
}

/* Append one uid, counting it whether or not there was room for it. */
static void list_add(uint32_t uid, uint32_t out, int max, int *count) {
    if (out && *count < max) psp_write32(out + (uint32_t)*count * 4, uid);
    (*count)++;
}

static int thread_matches(const psp_thread *t, int type) {
    const int dormant = !t->ever_started || t->state == TH_DORMANT;
    switch (type) {
        case PSP_TMID_THREAD:    return 1;
        case PSP_TMID_DORMANT:   return dormant;
        case PSP_TMID_SUSPENDED: return !dormant && t->state == TH_SUSPENDED;
        case PSP_TMID_SLEEPING:  return !dormant && t->wait_kind == WAIT_SLEEP;
        case PSP_TMID_DELAYING:  return !dormant && t->wait_kind == WAIT_DELAY;
        default:                 return 0;
    }
}

static void threadman_list(int type, uint32_t out, int max, int *count) {
    if (type == PSP_TMID_THREAD || (type >= PSP_TMID_SLEEPING && type <= PSP_TMID_DORMANT)) {
        for (int i = 0; i < MAX_THREADS; i++)
            if (g_thread[i].used && thread_matches(&g_thread[i], type))
                list_add(g_thread[i].uid, out, max, count);
        return;
    }
    if (type == PSP_TMID_SEMA)
        for (int i = 0; i < MAX_SEMAS; i++)
            if (g_sema[i].used) list_add(g_sema[i].uid, out, max, count);
    if (type == PSP_TMID_EVENTFLAG)
        for (int i = 0; i < MAX_FLAGS; i++)
            if (g_flag[i].used) list_add(g_flag[i].uid, out, max, count);
    if (type == PSP_TMID_CALLBACK)
        for (int i = 0; i < MAX_CBS; i++)
            if (g_cb[i].used) list_add(g_cb[i].uid, out, max, count);
}

/* (type, buffer, entries, countOut)
 *
 * The count it writes is how many objects *exist*, not how many fitted -- the
 * two differ whenever the buffer is short, and the return value is the number
 * actually written. threads/threadmanidlist checks all three against each
 * other, and separately that a bad type or a negative size leaves the caller's
 * count word alone entirely. */
static void hle_GetThreadmanIdList(void) {
    const int      type = (int)psp_arg(0);
    const uint32_t buf  = psp_arg(1);
    const int32_t  max  = (int32_t)psp_arg(2);
    const uint32_t nout = psp_arg(3);

    const int valid = (type >= PSP_TMID_THREAD && type <= PSP_TMID_TLSPL) ||
                      (type >= PSP_TMID_SLEEPING && type <= PSP_TMID_DORMANT);
    if (!valid) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_TYPE); return; }
    if (max < 0) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_SIZE); return; }

    int count = 0;
    threadman_list(type, buf, max, &count);
    for (int i = 0; i < g_listers; i++) g_lister[i](type, buf, max, &count);

    if (nout) psp_write32(nout, (uint32_t)count);
    psp_ret((uint32_t)(count < max ? count : max));
}

static void hle_CreateCallback(void) {
    if (!name_ok(psp_arg(0))) { psp_ret(SCE_KERNEL_ERROR_ERROR); return; }
    psp_callback *c = NULL;
    for (int i = 0; i < MAX_CBS; i++) if (!g_cb[i].used) { c = &g_cb[i]; break; }
    if (!c) { psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }

    memset(c, 0, sizeof *c);
    psp_str(psp_arg(0), c->name, sizeof c->name);
    c->func = psp_arg(1);
    c->arg  = psp_arg(2);
    c->thread = psp_sched_current();
    c->uid  = g_next_uid++;
    c->used = 1;
    /* Registered but never fired: callbacks are delivered from the scheduler,
     * which does not exist yet. A game that only registers them (the common
     * case -- exit and power callbacks) is unaffected. */
    psp_ret(c->uid);
}

/* sceKernelNotifyCallback(cb, arg)
 *
 * Raising a callback does not run it: it accumulates, and the thread that owns
 * it collects the whole accumulation the next time it asks. callbacks/notify
 * fires it 10002 times and the handler is entered *once*, with 0x2712 in its
 * first argument. */
static void hle_NotifyCallback(void) {
    psp_callback *c = find_cb(psp_arg(0));
    if (!c) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_CBID); return; }
    c->notify_count++;
    c->notify_arg = psp_arg(1);
    /* If the owner is parked in a CB wait, this ends it. callbacks/notify has
     * two threads sitting in sceKernelSleepThreadCB that are never woken by
     * name at all -- the handler lines they print are the only evidence they
     * ran, and without this they print nothing. */
    psp_thread *owner = find_thread(c->thread);
    if (owner && owner->cb_wait) {
        owner->cb_wake = 1;
        psp_sched_wake(owner->uid);
    }
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* sceKernelGetThreadExitStatus(thid)
 *
 * The same value sceKernelReferThreadStatus reports in its exitStatus field,
 * on its own. Unregistered until now, so it answered zero -- and zero is a
 * meaningful answer here: the callbacks tests use it to decide whether a
 * worker is still alive, and a thread that reads as "exited cleanly" is torn
 * down silently instead of being terminated and announced. Whole lines went
 * missing from four tests because of it. */
static void hle_GetThreadExitStatus(void) {
    const psp_thread *t = find_thread(psp_arg(0));
    if (!t) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
    if (!t->ever_started)              { psp_ret(SCE_KERNEL_ERROR_DORMANT); return; }
    if (t->state != TH_DORMANT)        { psp_ret(SCE_KERNEL_ERROR_NOT_DORMANT); return; }
    psp_ret(t->exit_status);
}

/* Deliver every callback pending for the current thread.
 *
 * The handler's three arguments are pinned by callbacks/notify, which prints
 * what arrived -- `cbFunc hit: 00002712, 00000001, 00000000` -- against 10002
 * notifies whose last argument was 1 and a common pointer of NULL. So they are
 * the accumulated count, the *last* notify argument, and the common pointer
 * given at create; the intermediate arguments are not kept.
 *
 * Ownership matters: a callback is delivered to the thread that created it and
 * to no other. callbacks/check has two sleeping threads each waiting on its own
 * and gets one hit apiece.
 *
 * Returns whether anything ran, not how many: callbacks/check answers
 * `With 2 pending: 00000001` after entering two handlers.
 *
 * Re-entrant on purpose. A handler may notify itself and then call a CB wait,
 * which delivers the notify it just raised -- callbacks/notify does exactly
 * that and prints both hits. Guarding against re-entry silences the second.
 * What stops it running away is that the count is cleared before the dispatch,
 * so a handler that does this once is entered twice and no more. */
int psp_threadman_run_callbacks(void) {
    const uint32_t me = psp_sched_current();
    int ran = 0;
    for (int i = 0; i < MAX_CBS; i++) {
        psp_callback *c = &g_cb[i];
        if (!c->used || c->thread != me || !c->notify_count) continue;

        const uint32_t func = c->func, common = c->arg;
        const uint32_t count = c->notify_count, arg = c->notify_arg;
        /* Cleared before the handler runs, so a handler that notifies itself --
         * which callbacks/notify does deliberately -- is pending again when it
         * returns rather than being swallowed or looping here. */
        c->notify_count = 0;

        /* Guest code, run between two instructions of whichever thread asked.
         * That thread must not be able to tell, so its registers are put back;
         * same reasoning as the alarm handler in ktimer.c. */
        const uint32_t uid = c->uid;
        const psp_cpu_state saved = psp_cpu;
        psp_cpu.r[PSP_REG_A0] = count;
        psp_cpu.r[PSP_REG_A1] = arg;
        psp_cpu.r[PSP_REG_A2] = common;
        psp_cpu.r[PSP_REG_RA] = 0;
        psp_dispatch(func);
        const uint32_t handler_result = psp_cpu.r[PSP_REG_V0];
        psp_cpu = saved;
        ran = 1;

        /* A handler that returns non-zero is saying it is finished, and the
         * callback is deleted. callbacks/notify measures it from the outside:
         * a sleeper whose handler returns 0x1337 answers `Notify #1: OK` and
         * then `Notify #2: Failed (800201a1)`, with nothing between the two but
         * the handler running. */
        if (handler_result) {
            psp_callback *again = find_cb(uid);
            if (again) again->used = 0;
        }
    }
    return ran;
}

static void hle_CheckCallback(void) { psp_ret((uint32_t)psp_threadman_run_callbacks()); }

/* Discard what has accumulated without running the handler. The argument is
 * left alone -- callbacks/cancel reads the object back afterwards and finds
 * `notifyCount=00000000,notifyArg=0`, so the count going to zero takes the
 * argument with it. */
static void hle_CancelCallback(void) {
    psp_callback *c = find_cb(psp_arg(0));
    if (!c) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_CBID); return; }
    c->notify_count = 0;
    c->notify_arg   = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* How much has accumulated, without disturbing it. callbacks/count notifies
 * three times and reads `OK (3)` alongside `notifyCount=00000003`. */
static void hle_GetCallbackCount(void) {
    const psp_callback *c = find_cb(psp_arg(0));
    psp_ret(c ? c->notify_count : SCE_KERNEL_ERROR_UNKNOWN_CBID);
}

/* The CB suffix on a wait means "and deliver my callbacks while you are at it".
 * Delivering them first is what the tests measure: threads/threadend prints
 * ` * cbFunc` *before* the line reporting the wait's result, on a wait that
 * returns immediately. */
static void hle_WaitThreadEndCB(void) {
    psp_threadman_run_callbacks();
    hle_WaitThreadEnd();
}

static void hle_DelayThreadCB(void) {
    psp_threadman_run_callbacks();
    hle_DelayThread();
}

static void hle_WaitSemaCB(void) {
    psp_threadman_run_callbacks();
    hle_WaitSema();
}

/* A sleep that a notify can interrupt, and that goes back to sleep afterwards.
 * The other CB waits above deliver on the way in and are done; this one is the
 * shape the callback tests are built around, because a thread parked here is
 * reachable only by a callback. */
static void hle_SleepThreadCB(void) {
    if (!psp_sched_can_wait()) { psp_ret(SCE_KERNEL_ERROR_CAN_NOT_WAIT); return; }
    for (;;) {
        psp_threadman_run_callbacks();
        psp_thread *t = current_thread();
        if (!t) { psp_ret(SCE_KERNEL_ERROR_OK); return; }
        if (t->wakeup_count > 0) { t->wakeup_count--; psp_ret(SCE_KERNEL_ERROR_OK); return; }

        t->wait_kind = WAIT_SLEEP;
        t->cb_wait   = 1;
        const int rc = psp_sched_block(t->uid, PSP_SCHED_SLEEPING,
                                       "sceKernelSleepThreadCB");
        t = current_thread();
        if (t) { t->wait_kind = WAIT_NONE; t->cb_wait = 0; }
        if (rc != PSP_SCHED_WOKEN) {
            wait_deadlock("sceKernelSleepThreadCB");
            psp_ret(SCE_KERNEL_ERROR_OK);
            return;
        }
        /* Woken to deliver, not woken by name: run the handler and park again. */
        if (t && t->cb_wake) { t->cb_wake = 0; continue; }
        if (t && t->wakeup_count > 0) t->wakeup_count--;
        psp_ret(SCE_KERNEL_ERROR_OK);
        return;
    }
}

void psp_threadman_register(void) {
    /* NIDs are SHA-1(name)[0:4] little-endian; tests/test_hle.c verifies every
     * pair below. */
    psp_hle_register(0x349D6D6C, "ThreadManForUser", "sceKernelCheckCallback",           hle_CheckCallback);
    psp_hle_register(0xC11BA8C4, "ThreadManForUser", "sceKernelNotifyCallback",          hle_NotifyCallback);
    psp_hle_register(0x3B183E26, "ThreadManForUser", "sceKernelGetThreadExitStatus",     hle_GetThreadExitStatus);
    psp_hle_register(0xBA4051D6, "ThreadManForUser", "sceKernelCancelCallback",          hle_CancelCallback);
    psp_hle_register(0x2A3D44FF, "ThreadManForUser", "sceKernelGetCallbackCount",        hle_GetCallbackCount);
    psp_hle_register(0x840E8133, "ThreadManForUser", "sceKernelWaitThreadEndCB",         hle_WaitThreadEndCB);
    psp_hle_register(0x446D8DE6, "ThreadManForUser", "sceKernelCreateThread",            hle_CreateThread);
    psp_hle_register(0xF475845D, "ThreadManForUser", "sceKernelStartThread",             hle_StartThread);
    psp_hle_register(0xAA73C935, "ThreadManForUser", "sceKernelExitThread",              hle_ExitThread);
    psp_hle_register(0x9FA03CD3, "ThreadManForUser", "sceKernelDeleteThread",            hle_DeleteThread);
    psp_hle_register(0x616403BA, "ThreadManForUser", "sceKernelTerminateThread",         hle_TerminateThread);
    psp_hle_register(0x383F7BCC, "ThreadManForUser", "sceKernelTerminateDeleteThread",   hle_TerminateDeleteThread);
    psp_hle_register(0xCEADEB47, "ThreadManForUser", "sceKernelDelayThread",             hle_DelayThread);
    psp_hle_register(0x68DA9E36, "ThreadManForUser", "sceKernelDelayThreadCB",           hle_DelayThreadCB);
    psp_hle_register(0x278C0DF5, "ThreadManForUser", "sceKernelWaitThreadEnd",           hle_WaitThreadEnd);
    psp_hle_register(0x82BC5777, "ThreadManForUser", "sceKernelGetSystemTimeWide",        hle_GetSystemTimeWide);
    psp_hle_register(0x369ED59D, "ThreadManForUser", "sceKernelGetSystemTimeLow",         hle_GetSystemTimeLow);
    psp_hle_register(0xDB738F35, "ThreadManForUser", "sceKernelGetSystemTime",            hle_GetSystemTime);
    psp_hle_register(0x293B45B8, "ThreadManForUser", "sceKernelGetThreadId",             hle_GetThreadId);
    psp_hle_register(0x9944F31F, "ThreadManForUser", "sceKernelSuspendThread",           hle_SuspendThread);
    psp_hle_register(0x75156E8F, "ThreadManForUser", "sceKernelResumeThread",            hle_ResumeThread);
    psp_hle_register(0x71BC9871, "ThreadManForUser", "sceKernelChangeThreadPriority",    hle_ChangeThreadPriority);
    psp_hle_register(0x94AA61EE, "ThreadManForUser", "sceKernelGetThreadCurrentPriority",hle_GetThreadCurrentPriority);
    psp_hle_register(0xEA748E31, "ThreadManForUser", "sceKernelChangeCurrentThreadAttr", hle_ChangeCurrentThreadAttr);
    psp_hle_register(0x52089CA1, "ThreadManForUser", "sceKernelGetThreadStackFreeSize",  hle_GetThreadStackFreeSize);
    psp_hle_register(0x9ACE131E, "ThreadManForUser", "sceKernelSleepThread",             hle_SleepThread);
    psp_hle_register(0x82826F70, "ThreadManForUser", "sceKernelSleepThreadCB",           hle_SleepThreadCB);
    psp_hle_register(0xD59EAD2F, "ThreadManForUser", "sceKernelWakeupThread",            hle_WakeupThread);
    psp_hle_register(0xFCCFAD26, "ThreadManForUser", "sceKernelCancelWakeupThread",      hle_CancelWakeupThread);
    psp_hle_register(0x3AD58B8C, "ThreadManForUser", "sceKernelSuspendDispatchThread",   hle_SuspendDispatchThread);
    psp_hle_register(0x27E22EC2, "ThreadManForUser", "sceKernelResumeDispatchThread",    hle_ResumeDispatchThread);
    psp_hle_register(0x17C1684E, "ThreadManForUser", "sceKernelReferThreadStatus",       hle_ReferThreadStatus);
    psp_hle_register(0x94416130, "ThreadManForUser", "sceKernelGetThreadmanIdList",      hle_GetThreadmanIdList);

    psp_hle_register(0xD6DA4BA1, "ThreadManForUser", "sceKernelCreateSema",              hle_CreateSema);
    psp_hle_register(0x28B6489C, "ThreadManForUser", "sceKernelDeleteSema",              hle_DeleteSema);
    psp_hle_register(0x3F53E640, "ThreadManForUser", "sceKernelSignalSema",              hle_SignalSema);
    psp_hle_register(0x4E3A1105, "ThreadManForUser", "sceKernelWaitSema",                hle_WaitSema);
    /* The CB form additionally runs the thread's pending callbacks while it
     * waits. Callbacks are delivered by sceKernelCheckCallback here, so the
     * two differ only in that -- and unimplemented was much worse than
     * imperfect: it returned zero, and zero means "you have the semaphore",
     * so a thread carried on holding a lock it had never taken. */
    psp_hle_register(0x6D212BAC, "ThreadManForUser", "sceKernelWaitSemaCB",              hle_WaitSemaCB);
    psp_hle_register(0x58B1F937, "ThreadManForUser", "sceKernelPollSema",                hle_PollSema);
    psp_hle_register(0x912354A7, "ThreadManForUser", "sceKernelRotateThreadReadyQueue",  hle_RotateReadyQueue);
    psp_hle_register(0xEDBA5844, "ThreadManForUser", "sceKernelDeleteCallback",          hle_DeleteCallback);

    psp_hle_register(0x55C20A00, "ThreadManForUser", "sceKernelCreateEventFlag",         hle_CreateEventFlag);
    psp_hle_register(0xEF9E4C70, "ThreadManForUser", "sceKernelDeleteEventFlag",         hle_DeleteEventFlag);
    psp_hle_register(0x1FB15A32, "ThreadManForUser", "sceKernelSetEventFlag",            hle_SetEventFlag);
    psp_hle_register(0x812346E4, "ThreadManForUser", "sceKernelClearEventFlag",          hle_ClearEventFlag);
    psp_hle_register(0x402FCF22, "ThreadManForUser", "sceKernelWaitEventFlag",           hle_WaitEventFlag);

    psp_hle_register(0xE81CAF8F, "ThreadManForUser", "sceKernelCreateCallback",          hle_CreateCallback);
    psp_hle_register(0xBC6FEBC5, "ThreadManForUser", "sceKernelReferSemaStatus",         hle_ReferSemaStatus);
    psp_hle_register(0xA66B0120, "ThreadManForUser", "sceKernelReferEventFlagStatus",    hle_ReferEventFlagStatus);
    psp_hle_register(0x730ED8BC, "ThreadManForUser", "sceKernelReferCallbackStatus",     hle_ReferCallbackStatus);
}
