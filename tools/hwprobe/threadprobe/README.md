# threadprobe

A PSP homebrew program, built from PSPSDK (BSD) alone, that measures what the
firmware's thread manager and clocks do. It covers the behaviour that
`src/hle/threadman.c`, `src/hle/sched.c`, `include/psprecomp/sched.h`,
`src/hle/waitq.h`, the TLS pools in `src/hle/kernobj.c`, `src/hle/ktimer.c`,
`src/hle/clock.c` and the time and memory calls in `src/hle/misc.c` and
`src/hle/sysmem.c` implement:

- **Threads**: create argument sweeps (priority, stack size, names,
  attribute bits), every word of ReferThreadStatus in each thread state, the
  start argument block and the register file a thread starts with, the stack
  fill and the words the kernel writes at the top, and a matrix of every
  thread call against every thread state.
- **Scheduling**: who runs first after start, wakeup, SignalSema, priority
  changes, rotate, suspend and resume, delays of different lengths, and
  equal-priority threads that spin (is there a timeslice?). Threads append a
  tag to a shared in-memory list, so the log shows the order they ran in.
- **Sleep and wakeup counts, exit status, WaitThreadEnd, dispatch disable,
  ChangeCurrentThreadAttr, stack free size.**
- **Callbacks**: counts and arguments, which CB waits deliver them, handler
  return values, a handler that notifies itself, the power callback.
- **Id lists**: GetThreadmanIdList for every type number, with one object of
  each kind and threads in each wait state.
- **TLS pools, alarms and vtimers**: status structs, block offsets and
  reuse, waiter order, limits, handler arguments and return values.
- **Time and memory**: system time, clock conversions, the libc clocks, the
  RTC tick and sceRtc's calendar arithmetic (none of which psprecomp has),
  and sysmem free sizes, granules, alignment and partitions.

Each step's comment names the psprecomp file:line claim that the step checks.

## Build

Needs the pspdev toolchain (`psp-gcc`, `psp-config` and PSPSDK) on `PATH`:

    make

This produces `EBOOT.PBP` and `threadprobe.prx`. The CMake build does not
include it. `stubs.S` declares the TLS pool and mutex calls that PSPSDK has
no stubs for, and holds `regs_entry`, a thread entry point that saves the
whole register file before any compiled code touches it.

## Run

Copy `EBOOT.PBP` to `ms0:/PSP/GAME/threadprobe/` and start it from the XMB.
It needs no input and finishes in well under a minute. Then it returns to the
XMB by itself.

The log is `threadprobe.txt`, beside the EBOOT. It is saved at every step, so
if the PSP hangs or switches off, the last step in the log is the one that
did it. The steps that pass NULL or small bad pointers are all in the last
section.

To run it under psprecomp:

    allegrexrecomp interp threadprobe.prx --dispatch --budget 4000000000 --drain 200

The log records only what the firmware decided. UIDs appear as valid or not,
or by the name the probe gave the object. Addresses appear as offsets into a
thread's stack or a pool, or as a class (user, kernel). Times appear only as
comparisons, and a timeout written back after a wait appears as unchanged,
reduced or 0. Every subtest cleans up after itself. The summary at the end
lists any objects left over, so a cleanup that the firmware refused shows up
there.
