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
  RTC tick and sceRtc's calendar arithmetic, and sysmem free sizes, granules,
  alignment and partitions.

Each step's comment names the psprecomp file:line claim that the step checks.

## Version 3

Version 3 keeps version 2's 158 steps and titles, so the logs line up in
`compare.py`, and adds 23 steps, each at the end of its section. They are the
questions the analysis of the second hardware run left open
(`fw660-run2/findings/threadprobe.md`):

| section | new step |
| --- | --- |
| basics | main's argc, and each argv string's length, device, file name and place on main's stack (the start block that explains step 85) |
| create | CreateThread check order with two or more bad arguments, and a kernel-space entry |
| refer | waitType and waitId for mbx, vpl, fpl, msgpipe, mutex, lwmutex and TLS waits |
| exit | an entry that returns -5, and one that returns 0x80020001 |
| suspend | a suspended waiter whose timeout passes; a suspended waiter that is signalled |
| sleep | ReleaseWaitThread on a dormant, a suspended ready, a delaying and a waiting+suspended thread; on waits for each object kind: its return, the timeout left, the object's waiter count before and after |
| preemption | preempt and release counters of a spinning thread main preempts twice; two equal threads, the first spinning 200ms; whether sceIo open/write/close/read/lseek/getstat/remove let an equal-priority thread run, with main's releaseCount across each |
| delay | DelayThreadCB(0) with a pending notify and with a ready thread; DelayThread(n) for n = 0..100 with a ready thread, four times each |
| dispatch | DelayThread(0) with dispatch off, ResumeDispatchThread(2) and (-1), SuspendDispatchThread with interrupts off; DelayThread and WaitSema with interrupts off |
| attribute and stack | GetThreadStackFreeSize in NO_FILLSTACK threads over 0xCC and over 0xFF, and a CLEAR_STACK one |
| TLS pools | partition 5 (risky) |
| alarms | a handler returning 1000 five times: first-hit lateness, gaps, each hit against the first |
| vtimers | a handler that falls due while main is in DelayThread |
| time | LibcGettimeofday's seconds against LibcTime and uptime, the timezone struct, two reads 20ms apart |
| rtc | CheckValid of year 9999 and 10000; GetTick of 2023-02-30 and 2023-13-01 |
| user memory | the drop for an Addr block 0x80 into a granule, and total minus max; partitions 5 and 9 (risky) |

The last step (StartThread with argp 0x10) switched the PSP off in version 2
and is now `KNOWN_CRASH`: it logs "not run" unless built with
`-DRUN_KNOWN_CRASHES`. Four new steps could hang or switch off a PSP: the
CreateThread check order (a kernel-space entry), DelayThread and WaitSema with
interrupts off, and the two partition 5 steps (psprecomp records that a vpl in
partition 5 is said to crash hardware). Starting the probe again skips the
step it stopped in.

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

    allegrexrecomp interp threadprobe.prx --dispatch --budget 4000000000 --drain 200 --base 0x08804000

The log records only what the firmware decided. UIDs appear as valid or not,
or by the name the probe gave the object. Addresses appear as offsets into a
thread's stack or a pool, or as a class (user, kernel). Times appear only as
comparisons, and a timeout written back after a wait appears as unchanged,
reduced or 0. Every subtest cleans up after itself. The summary at the end
lists any objects left over, so a cleanup that the firmware refused shows up
there.
