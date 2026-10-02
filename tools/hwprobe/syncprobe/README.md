# syncprobe

A PSP homebrew program, built from PSPSDK (BSD) alone, that measures what the
firmware does with its synchronisation objects and memory pools. It covers
the behaviour that `src/hle/kernobj.c`, `src/hle/kernlock.c`,
`src/hle/waitq.h` and the semaphore and event-flag code in
`src/hle/threadman.c` implement:

- **Semaphores and event flags**: attribute and argument checks, counts and
  patterns after every call, the clear modes, and CancelSema/CancelEventFlag.
- **Mutexes and lwmutexes**: recursion, overflow and underflow, ownership
  across threads, a mutex left held by a thread that ended, and the lwmutex
  workarea's eight words after every operation, including forged and copied
  workareas.
- **Mailboxes**: each message's `next` link after every send and receive,
  and priority ordering (attr 0x400).
- **Message pipes**: buffered and unbuffered transfers, wait modes,
  wrap-around, and transfers that arrive in pieces.
- **VPL and FPL pools**: sizes and rounding, block headers and pool
  accounting as offsets, first fit, the order that frees are reused in, and
  frees of foreign or bad pointers.
- **Wait queues**: for every object type, the order that waiters are
  released in, both first-come and most-urgent-first (attr 0x100, and 0x1000
  for a pipe's senders). This also covers cancel and delete with waiters, and
  a release followed by a delete before the woken waiter has run.

Each step's comment names the psprecomp file:line claim that the step checks.

## Build

Needs the pspdev toolchain (`psp-gcc`, `psp-config` and PSPSDK) on `PATH`:

    make

This produces `EBOOT.PBP` and `syncprobe.prx`. The CMake build does not
include it. `imports.S` declares the handful of calls that PSPSDK has no
stubs for: the mutex family, the lwmutex status calls and
`sceKernelTryLockLwMutex_600`.

## Run

Copy `EBOOT.PBP` to `ms0:/PSP/GAME/syncprobe/` and start it from the XMB.
It needs no input and finishes in well under a minute. Then it returns to the
XMB by itself.

The log is `syncprobe.txt`, beside the EBOOT. It is saved at every step, so
if the PSP hangs or switches off, the last step in the log is the one that
did it.

To run it under psprecomp:

    allegrexrecomp interp syncprobe.prx --dispatch --budget 4000000000 --drain 200 --base 0x08804000

## How it works

Waiters are eight worker threads. Each is created once and then sleeps on a
semaphore of its own until it is handed a job. A job is a single wait with a
timeout. Before handing over the job, main sets the worker's priority for
the test. In most tests the worker is more urgent than main (priority 0x20),
so it runs straight away until it blocks, in the order main hands the jobs
out. When a waiter is released, it appends its letter to a shared list. Main
releases one waiter at a time, so the list reads back the firmware's order.

The one test that needs a thread to end while holding a mutex runs on a
thread of its own. Keeping the workers means the whole run makes about a
dozen threads. This matters under psprecomp, whose scheduler never reuses a
finished thread's slot.

The log records only what the firmware decided. UIDs appear as valid or not,
thread ids by name (main, WA, WB, and so on), addresses as offsets from a
block the probe allocated, and timeout write-backs as full, part or zero.
Refer*Status buffers are pre-filled with A5A5A5A5, so words the firmware did
not write stand out, and so do writes past the struct. Steps that pass a
NULL or small bad pointer come last in their section, or in the last
section.
