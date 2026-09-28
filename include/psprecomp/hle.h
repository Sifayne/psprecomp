/* psprecomp — high-level emulation of the PSP firmware.
 *
 * A PSP game does not touch hardware. It calls firmware entry points through
 * an import table, and every one of those calls is identified by a **NID** —
 * the first four bytes of SHA-1(function name), little-endian. That is a
 * verifiable fact rather than a convention, and `tests/test_hle.c` checks it
 * for every function registered here: a mistyped NID or a wrong name cannot
 * survive the test.
 *
 * Because the surface is a library rather than hardware, it is *implemented*,
 * not emulated. What a game needs is exactly its import table and nothing
 * else, which `allegrexrecomp funcs` reports — so the work is bounded and
 * knowable in advance.
 *
 * Calling convention is MIPS o32: arguments in $a0-$a3 then the stack, return
 * value in $v0. Handlers take no C arguments and use psp_arg()/psp_ret().
 */
#ifndef PSPRECOMP_HLE_H
#define PSPRECOMP_HLE_H

#include "cpu.h"
#include "mem.h"
#include "savedata.h"

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*psp_hle_fn)(void);

/* Register one firmware function. `name` is kept for diagnostics and is what
 * the NID is verified against. */
void psp_hle_register(uint32_t nid, const char *lib, const char *name, psp_hle_fn fn);

/* Every semaphore, and whether anything ever signalled it. Answers "nobody
 * signals this" with the whole set rather than with the absence of a log line. */
void psp_threadman_dump_signalled(FILE *out);

/* Every thread ever created, with what became of it. The scheduler's list only
 * covers threads that still exist. */
void psp_threadman_dump_threads(FILE *out);
void psp_hle_register_unnamed(uint32_t nid, const char *lib, psp_hle_fn fn);
int psp_hle_is_named(int index);

/* Call a firmware function by NID. An unregistered NID reports itself by name
 * where possible and by number otherwise, rather than failing silently. */
void psp_hle_call(uint32_t nid);

/* Silence the per-call "unimplemented firmware call" message. Intended for
 * batch callers making millions of calls; see the note in hle.c. */
void psp_hle_set_quiet(int quiet);

/* Look up what is registered, for reporting. Returns NULL if absent. */
const char *psp_hle_name(uint32_t nid);
int         psp_hle_count(void);

/* Every registered entry, for the coverage report a game repo wants: which of
 * a module's imports actually exist yet. */
typedef struct {
    uint32_t    nid;
    const char *lib;
    const char *name;
} psp_hle_entry;

const psp_hle_entry *psp_hle_entries(int *count);

/* Print the recent firmware calls that returned zero. Zero is what a game most
 * often mistakes for an address, so this is the first thing to consult when a
 * wild pointer shows up far from its cause. */
void psp_mpeg_register(void);
void psp_mpeg_reset(void);
/* Host configuration, set before guest execution. -1 restores the legacy
 * environment default. Returns -1 if decoding is requested but unavailable. */
int psp_mpeg_set_decoding(int enabled);
int psp_mpeg_decoding_available(void);

void psp_hle_dump_recent(FILE *out);

/* The `top` most-called firmware functions, most first. Shows what a run spent
 * its time on -- and, by what is missing, what it never reached. */
void psp_hle_dump_calls(FILE *out, int top);

/* Whether PSPRECOMP_HLE_LOG is on. Handlers consult this to add detail that is
 * too verbose to print unconditionally. */
int psp_hle_logging(void);

/* Register everything the toolkit implements. Call once at startup. */
void psp_hle_init(void);

/* ---- o32 argument access ------------------------------------------------- */

/* Arguments 0-3 arrive in $a0-$a3; 4 and beyond are on the stack, at $sp+16
 * onward. The stack slots for the register arguments exist but are not
 * written by the caller, which is why the split is at 4 and not at 0. */
/* Firmware calls take arguments 5-8 in $t0-$t3, not on the stack.
 *
 * Plain o32 spills the fifth argument onward to `sp+16`, and that is what this
 * used to read. PSP firmware stubs do not: they load $t0-$t3 and branch, which
 * is visible in the delay slot of every such call --
 *
 *     000002EC  addu  $a3, $s1, $zero
 *     000002F0  jal   0x00091F14        ; sceKernelAllocPartitionMemory
 *     000002F4  addiu $t0, $zero, 4096  ; argument 5: the alignment
 *
 * Reading `sp+16` instead returned whatever happened to be on the stack. For
 * that call it produced 0x3AC85C where a power-of-two alignment belonged, so
 * the allocator rejected a 15.9 MB request with ILLEGAL_ATTR -- and every
 * failure this bring-up chased descended from it.
 *
 * Beyond eight, arguments do go on the stack. */
static inline uint32_t psp_arg(int n) {
    if (n < 4) return psp_cpu.r[PSP_REG_A0 + n];
    if (n < 8) return psp_cpu.r[PSP_REG_T0 + (n - 4)];
    return psp_read32(psp_cpu.r[PSP_REG_SP] + (uint32_t)n * 4);
}

static inline void psp_ret(uint32_t v) { psp_cpu.r[PSP_REG_V0] = v; }

/* Read a NUL-terminated string out of guest memory into a host buffer.
 * Always terminates; returns `dst`. */
const char *psp_str(uint32_t addr, char *dst, size_t cap);

/* ---- error codes --------------------------------------------------------- */
/* Only the ones the implemented functions can actually return. Games branch on
 * these, so returning a plausible-looking wrong value is worse than failing. */
#define SCE_KERNEL_ERROR_OK              0
#define SCE_KERNEL_ERROR_ERROR           0x80020001
#define SCE_KERNEL_ERROR_NOTIMPLEMENTED  0x80020002
#define SCE_KERNEL_ERROR_ILLEGAL_ADDR    0x80020005
#define SCE_KERNEL_ERROR_NO_MEMORY       0x80020190
#define SCE_KERNEL_ERROR_ILLEGAL_ATTR    0x80020191
#define SCE_KERNEL_ERROR_UNKNOWN_UID     0x800201A2
/* Named ILLEGAL_MEMBLOCK here on a guess, and never used: when the pools were
 * written the captures said their "not a live block of mine" code is
 * 0x800201B6, which is SCE_KERNEL_ERROR_ILLEGAL_MEMBLOCK_PTR below. This value
 * is what a wait returns when the object it was parked on was *cancelled* --
 * msgpipe/cancel prints it four times against a delete's 0x800201B5 in the
 * neighbouring test. */
#define SCE_KERNEL_ERROR_WAIT_CANCEL     0x800201A9
#define SCE_KERNEL_ERROR_ILLEGAL_THID    0x80020197
/* A thread id that names nothing, as distinct from one that is malformed.
 * threads/refer.expected answers both a deleted and an invalid id with this. */
#define SCE_KERNEL_ERROR_UNKNOWN_THID    0x80020198
/* A thread that has not been started, or has already finished. Shares its
 * value with UNKNOWN_UID above -- the uid space is one space, and the kernel
 * spends its codes on the *situation* rather than on the object. */
#define SCE_KERNEL_ERROR_DORMANT         0x800201A2
/* The exit status of a thread that is *running*: it has not exited, so there
 * is nothing to report and the kernel says so rather than answering 0.
 * threads/refer.expected reads `exit: 800201a4` for a live thread where
 * threads/create.expected reads `800201a2` for one never started. */
#define SCE_KERNEL_ERROR_UNKNOWN_CBID    0x800201A1
#define SCE_KERNEL_ERROR_NOT_DORMANT     0x800201A4
/* Not a wake code -- the *exit status* a terminated thread is left with.
 * threads/refer reads it back with sceKernelReferThreadStatus (`exit=800201ac`)
 * and threads/threadend gets the same value out of sceKernelWaitThreadEnd,
 * which returns the exit status. Two observations, one value. */
#define SCE_KERNEL_ERROR_THREAD_TERMINATED 0x800201AC
/* A thread priority outside 0x08..0x77. Zero is not in that range and is not
 * an error either -- it means "the priority I am running at". */
#define SCE_KERNEL_ERROR_ILLEGAL_PRIORITY 0x80020193
/* sceKernelCreateThread with a stack below 0x200 (threadprobe step 3, fw 6.60:
 * 0, 1, 0x100 and 0x1FF). */
#define SCE_KERNEL_ERROR_ILLEGAL_STACK_SIZE 0x80020194
/* A sceKernelGetThreadmanIdList type outside 1..14 and 0x40..0x43. */
#define SCE_KERNEL_ERROR_ILLEGAL_TYPE     0x800201BB
#define SCE_KERNEL_ERROR_SUSPEND         0x800201A3
#define SCE_KERNEL_ERROR_NOT_SUSPEND     0x800201A5
/* sceKernelReleaseWaitThread on a thread that is not waiting, and what the
 * wait it does release returns (threadprobe step 70, fw 6.60). */
#define SCE_KERNEL_ERROR_NOT_WAIT        0x800201A6
#define SCE_KERNEL_ERROR_RELEASE_WAIT    0x800201AA
/* A poll that would have blocked. Distinct from an error: it is the ordinary
 * answer to "is this free?" when it is not. */
#define SCE_KERNEL_ERROR_SEMA_ZERO       0x800201AD
/* Signalling past the maximum the semaphore was created with. PSPSDK names it
 * SEMA_OVF and semaphores/signal is what shows it does not silently clamp. */
#define SCE_KERNEL_ERROR_SEMA_OVF        0x800201AE
#define SCE_KERNEL_ERROR_WAIT_TIMEOUT    0x800201A8

/* The uid space is per object type, and a wrong-typed handle is refused with
 * the *asking* type's code. UNKNOWN_UID above is the allocator's; a semaphore
 * call is answered with UNKNOWN_SEMID whatever the handle really was.
 * Transcribed from the hardware captures that name them:
 * semaphores/wait.expected, events/wait/wait.expected. */
#define SCE_KERNEL_ERROR_UNKNOWN_SEMID   0x80020199
#define SCE_KERNEL_ERROR_UNKNOWN_EVFID   0x8002019A

/* A wait ended because the object was deleted underneath it -- distinct from
 * asking about an object that was already gone. */
#define SCE_KERNEL_ERROR_WAIT_DELETE     0x800201B5

/* Waiting for no bits at all, and a wait mode outside {WAITOR, WAITCLEAR}. */
/* A poll whose pattern is simply not there yet. Not an argument error: the
 * call did everything right and the answer is "no", so the current pattern is
 * reported through the out word where an argument error leaves it alone. */
#define SCE_KERNEL_ERROR_EVF_COND        0x800201AF
#define SCE_KERNEL_ERROR_EVF_ILPAT       0x800201B1
#define SCE_KERNEL_ERROR_ILLEGAL_MODE    0x80020195

/* A second thread waiting on an event flag created without WAITMULTIPLE. */
#define SCE_KERNEL_ERROR_EVF_MULTI       0x800201B0

/* Waiting for zero, a negative amount, or more than a semaphore's maximum. */
#define SCE_KERNEL_ERROR_ILLEGAL_COUNT   0x800201BD

/* The mutex family. Six codes for what a single "busy" would flatten, because
 * a mutex has an owner and the mistakes are therefore distinguishable: locking
 * one you already hold without the recursive attribute is a different error
 * from unlocking one you do not hold, which is different again from giving back
 * more than you took. All from threads/mutex/{create,lock,unlock}.expected. */
#define SCE_KERNEL_ERROR_NOT_FOUND_MUTEX       0x800201C3
#define SCE_KERNEL_ERROR_MUTEX_LOCKED          0x800201C4
#define SCE_KERNEL_ERROR_MUTEX_UNLOCKED        0x800201C5
#define SCE_KERNEL_ERROR_MUTEX_LOCK_OVERFLOW   0x800201C6
#define SCE_KERNEL_ERROR_MUTEX_UNLOCK_UNDERFLOW 0x800201C7
#define SCE_KERNEL_ERROR_MUTEX_RECURSIVE       0x800201C8

/* And the lwmutex family, which has its own set rather than sharing the
 * mutex's -- threads/lwmutex/{lock,unlock,delete}.expected. A null workarea is
 * ILLEGAL_SIZE, which is the odd one and is what delete.expected reports. */
/* MISNAMED, and the value is the part that is right. PSPSDK calls 0x800200D3
 * ILLEGAL_ADDR; the real ILLEGAL_SIZE is 0x800201BC, which is in this header
 * below under the invented name ILLEGAL_SIZE_MPP. Renaming is a separate audit
 * (see docs/findings/autotests.md) -- but do NOT add a correct constant for
 * either number under its proper name until that audit happens, because two
 * #defines of one name do not warn, the later one wins, and that has already
 * produced one silent wrong answer here (ILLEGAL_CONTEXT vs CPUDI). */
#define SCE_KERNEL_ERROR_ILLEGAL_SIZE            0x800200D3
#define SCE_KERNEL_ERROR_NOT_FOUND_LWMUTEX       0x800201CA
#define SCE_KERNEL_ERROR_LWMUTEX_LOCKED          0x800201CB
#define SCE_KERNEL_ERROR_LWMUTEX_UNLOCKED        0x800201CC
#define SCE_KERNEL_ERROR_LWMUTEX_LOCK_OVERFLOW   0x800201CD
#define SCE_KERNEL_ERROR_LWMUTEX_UNLOCK_UNDERFLOW 0x800201CE
#define SCE_KERNEL_ERROR_LWMUTEX_RECURSIVE       0x800201CF
/* What the *pre-6.00* sceKernelTryLockLwMutex answers for every failure it can
 * have -- all fifteen of try.expected's, where try600.expected gives the
 * specific code for the same inputs. */
#define SCE_KERNEL_ERROR_LWMUTEX_TRY_FAILED      0x800201C4

/* The memory-pool family. The two partition codes are not a range check and a
 * permission check in the usual order -- threads/vpl/create.expected refuses
 * partition 7 as out-of-range while 8 and 9 are merely forbidden. */
#define SCE_KERNEL_ERROR_ILLEGAL_PERM       0x800200D1
/* Also misnamed: PSPSDK calls 0x800200D2 ILLEGAL_ARGUMENT and puts
 * ILLEGAL_PARTITION at 0x800200D6. Same caution as above. */
#define SCE_KERNEL_ERROR_ILLEGAL_PARTITION  0x800200D2
#define SCE_KERNEL_ERROR_UNKNOWN_VPLID      0x8002019C
#define SCE_KERNEL_ERROR_ILLEGAL_MEMSIZE    0x800201B7
#define SCE_KERNEL_ERROR_UNKNOWN_MPPID      0x8002019E
#define SCE_KERNEL_ERROR_MSGPIPE_FULL       0x800201B3
#define SCE_KERNEL_ERROR_MSGPIPE_EMPTY      0x800201B4
#define SCE_KERNEL_ERROR_ILLEGAL_SIZE_MPP   0x800201BC
#define SCE_KERNEL_ERROR_UNKNOWN_MBXID      0x8002019B
/* The queue is not what the mailbox thinks it is: a message sent twice, or a
 * ring the guest edited into disagreeing with the count. mbx/send produces
 * both on purpose. */
#define SCE_KERNEL_ERROR_MBX_CORRUPT        0x800201C9
#define SCE_KERNEL_ERROR_MBOX_NOMSG         0x800201B2
#define SCE_KERNEL_ERROR_UNKNOWN_FPLID      0x8002019D
/* Freeing a pointer that is real but is not the start of one of this pool's
 * live blocks. Distinct from the allocator's ILLEGAL_MEMBLOCK above, which is
 * a different number for a different question. Both pools use this one. */
#define SCE_KERNEL_ERROR_ILLEGAL_MEMBLOCK_PTR 0x800201B6
#define SCE_KERNEL_ERROR_UNKNOWN_TLSPLID    0x800201D0
/* No slot left for another pool. Not in PSPSDK's pspkerror.h, whose TLS entries
 * stop at the kernel-side trio -- ILLEGAL_KTLSID, KTLS_FULL and KTLS_BUSY at
 * 0x800201C0..C2. The user-side family sits one row down in the same shape, and
 * what pins this member of it is tls/create's seventeenth pool. */
#define SCE_KERNEL_ERROR_TLSPL_FULL         0x800201D1
/* Someone else's block is still out. Same row as TLSPL_FULL above and the same
 * shape as the kernel-side KTLS_BUSY; tls/delete is what pins it. */
#define SCE_KERNEL_ERROR_TLSPL_BUSY         0x800201D2
#define SCE_KERNEL_ERROR_UNKNOWN_ALMID      0x8002019F
#define SCE_KERNEL_ERROR_UNKNOWN_VTID       0x800201BE
/* A uid of zero, which is not the same as one that names nothing. PSPSDK's
 * pspkerror.h has both names in this order. Which of the two a call answers is
 * per-call and the tests disagree deliberately: start, stop, sethandler and
 * cancelhandler answer ILLEGAL for a null uid where delete, gettime, getbase,
 * refer and settime answer UNKNOWN. */
#define SCE_KERNEL_ERROR_ILLEGAL_VTID       0x800201BF
/* Called from somewhere the call is not allowed to be made -- a handler, an
 * interrupt. PSPSDK names it and vtimers/delete is where it shows up here. */
#define SCE_KERNEL_ERROR_ILLEGAL_CONTEXT    0x80020064
/* A blocking call made while dispatch is suspended. */
#define SCE_KERNEL_ERROR_CAN_NOT_WAIT       0x800201A7
/* Suspending dispatch when it is already suspended, or resuming it with
 * something that is not a state this returned. PSPSDK names 0x80020066 CPUDI
 * -- interrupts disabled -- and this used to carry the name ILLEGAL_CONTEXT,
 * which belongs to 0x80020064 two lines up. Defining both under one name meant
 * the second definition quietly won and a vtimer handler's refused delete
 * answered the dispatch code. */
#define SCE_KERNEL_ERROR_CPUDI              0x80020066

/* ---- the subsystems ------------------------------------------------------ */

void psp_sysmem_init(void);
void psp_sysmem_register(void);
void psp_sysmem_reset(void);

void psp_display_init(void);
void psp_display_register(void);
void psp_display_reset(void);
int      psp_display_capture(const char *path);
uint64_t psp_display_vblanks(void);
uint32_t psp_display_framebuffer(void);
/* The fullest frame the run ever presented, and how many non-black pixels it
 * had. Dumping the buffer at the end samples whichever frame the run stopped
 * on; this answers whether anything was ever drawn at all. */
uint64_t        psp_display_best_score(void);
uint32_t        psp_display_best_addr(void);
const uint32_t *psp_display_best(void);
/* The stride in pixels and the pixel format the display is scanning out, so a
 * caller can read the framebuffer without guessing its shape. */
uint32_t psp_display_stride(void);
uint32_t psp_display_format(void);

void psp_ge_init(void);
void psp_ge_register(void);
void psp_ge_reset(void);
void psp_ge_dump_stats(FILE *out);
uint64_t psp_ge_command_count(void);
uint64_t psp_ge_vertex_count(void);
uint64_t psp_ge_pixels(void);
/* The address the GE last rendered into, VRAM base applied. */
uint32_t psp_ge_target(void);
/* Run every queued list to FINISH/stall, in order. For present paths and
 * tests that must see finished pixels without going through Sync. */
void psp_ge_drain_all(void);

/* The GE's register state, for capture and replay. Commands are differential,
 * so a frame only means anything against the state it started from -- a replay
 * that begins from a reset draws something the run never did. Save before the
 * frame's commands, load before replaying them. */
/* Replay one list, by address, without the firmware call around it -- what a
 * capture holds is addresses, since the words themselves live in the guest
 * memory the capture carries. */
void psp_ge_replay_list(uint32_t list, uint32_t stall, uint32_t base);

/* What the GE is currently drawing into. */
void psp_ge_current_target(uint32_t *addr, uint32_t *stride, int *fmt);

size_t psp_ge_state_size(void);
void   psp_ge_state_save(void *buf);
void   psp_ge_state_load(const void *buf);

/* Push the restored registers at the backend. Loading state tells the GE what
 * it believes; only a register write tells the backend, so a replay has to do
 * this explicitly or its pixels land wherever the backend was last pointed. */
void   psp_ge_sync_backend(void);

void psp_sas_init(void);
void psp_sas_register(void);
void psp_sas_reset(void);
uint64_t psp_sas_frames(void);
uint64_t psp_sas_nonzero(void);

void psp_io_init(void);
void psp_io_register(void);
void psp_io_reset(void);
void psp_io_set_root(const char *root);

/* Back the raw UMD block device with a disc image.
 *
 * `disc0:` is the ISO9660 filesystem and maps to a directory; `umd0:` and
 * `umd1:` are the block device underneath it, and a game opens those by bare
 * name to read sectors -- which is how a PSP title reaches its own data when it
 * does not want the filesystem. With no image set those opens fail, and a game
 * that retries on failure never gets past its first read. */
void psp_io_set_umd_image(const char *path);
uint64_t psp_io_bytes_read(void);
/* Map a guest path to its host path (same rewriting opens use). For layers
 * like the savedata utility that do their own host file I/O. */
void psp_io_host_path(const char *guest, char *out, size_t cap);
/* Make a guest directory and every missing level above it (same empty-tree
 * deviation hle_Mkdir documents). */
void psp_io_mkdir_all(const char *guest);
/* Remove a guest file or, recursively, a directory tree. Returns 0, or -1
 * when nothing was there. */
int psp_io_remove_tree(const char *guest);
/* Size and kind of a host-side guest path. Returns 0, or -1 when missing. */
int psp_io_path_info(const char *guest, uint64_t *size, int *is_dir);
/* Names directly under a guest directory: up to cap entries of 63 chars.
 * Returns the count, or -1 when not a directory. */
int psp_io_list_names(const char *guest, char names[][64], int cap);

void psp_misc_init(void);
void psp_misc_register(void);
void psp_misc_reset(void);

/* ---- sceNet / sceNetAdhoc / sceNetAdhocctl / sceWlanDrv ------------------ */
void psp_net_register(void);

/* ---- sceUmdUser ---------------------------------------------------------- */
void psp_umd_init(void);
void psp_umd_register(void);
void psp_umd_reset(void);

/* Savedata file services and asynchronous utility dialogs. See savedata.h. */
void psp_utility_init(void);
void psp_utility_register(void);

/* sceAtrac3plus without a decoder: the stream opens and its header is read,
 * and the decode itself fails -- the one failure this game's player handles.
 * A decoder that returns zero and writes nothing is the worse lie. See atrac.c. */
void psp_atrac_init(void);
void psp_atrac_register(void);
int  psp_exit_requested(void);
void psp_ctrl_set(uint32_t buttons, uint8_t ax, uint8_t ay);
uint64_t psp_audio_blocks(void);

/* ---- the pad, for scripted input ------------------------------------------
 *
 * Three lanes reach the guest's pad read and are OR'd there: a hold
 * (PSPRECOMP_PAD), a script, and the host's live gamepad (psp_ctrl_set).
 * They are separate so that they compose -- one shared word meant the SDL
 * thread's store erased the hold, and the timed press's clear released it.
 *
 * psp_ctrl_polls counts *calls* to sceCtrlPeek/ReadBufferPositive, which is
 * the timebase a scripted input is keyed on: it is one thing the guest did,
 * and it does not move when a game changes its buffer depth. psp_ctrl_samples
 * counts SceCtrlData entries written, which is the guest-visible timestamp
 * and counts what it always counted. */
uint32_t psp_ctrl_polls(void);
/* The stick as the guest last read it (merged lane, 0..255 centred on 128),
 * for native replacements that want the magnitude the game's own control code
 * discards. Same value a recording holds, so a replay reproduces it. */
void psp_ctrl_last_stick(uint8_t *ax, uint8_t *ay);
uint32_t psp_ctrl_samples(void);

/* The look channel: a second stick (0..255 centred on 128) and mouse motion,
 * carried through the same lanes as the pad so a recording holds them and a
 * replay reproduces them. The PSP has neither, and nothing here reaches
 * SceCtrlData; native replacements read them with psp_ctrl_last_look. Mouse
 * motion is a running sum the host adds to and each poll takes, because it
 * arrives at the mouse's rate and is read at the guest's, and a delta written
 * and overwritten between two polls is travel that silently never happened. */
void psp_ctrl_set_look(uint8_t rx, uint8_t ry);
void psp_ctrl_add_mouse(int dx, int dy);
/* Drop accumulated host motion when a modal host UI takes input. */
void psp_ctrl_clear_mouse(void);
void psp_ctrl_last_look(uint8_t *rx, uint8_t *ry, int *mdx, int *mdy);

/* Publish into the script lane. `analog_owned` non-zero takes the stick away
 * from live input until a later call hands it back. */
void psp_ctrl_script_set(uint32_t buttons, int analog_owned,
                         uint8_t ax, uint8_t ay);
/* The script lane's look channel. `owned` takes the second stick away from
 * live input; the mouse delta is delivered once, at the next poll. */
void psp_ctrl_script_set_look(int owned, uint8_t rx, uint8_t ry, int mdx, int mdy);

/* A button name from the shared table ("cross", "ltrigger", case-insensitive),
 * or 0x<hex> for a bit the table does not name. 0 if unrecognised. `n` is the
 * length, so a caller can pass a slice of a larger string without copying. */
uint32_t psp_pad_bit(const char *s, size_t n);

/* ---- scripted pad input (src/hle/ctrl_replay.c) ---------------------------
 *
 * PSPRECOMP_REPLAY=<file> drives the script lane from a scenario;
 * PSPRECOMP_REPLAY_REC=<file> writes one out from what the guest saw. Both
 * are driven from the guest's pad read so that they share one timebase --
 * see the header comment in ctrl_replay.c for why that is the whole design. */
void psp_ctrl_replay_init(void);
void psp_ctrl_replay_reset(void);
/* Advance the scenario to (polls, us), applying at most one visible edge. */
void psp_ctrl_replay_step(uint32_t polls, uint64_t us);
/* Record the composed pad state, look channel included; writes a line only
 * when it changes. */
void psp_ctrl_replay_record(uint32_t polls, uint64_t us,
                            uint32_t buttons, uint8_t ax, uint8_t ay,
                            uint8_t rx, uint8_t ry, int mdx, int mdy);
/* Live input arrived while a scenario was driving. Reported once. */
void psp_ctrl_replay_taint(uint32_t polls);
/* Close the recording and report. `summary` may be NULL. */
void psp_ctrl_replay_finish(FILE *summary);
int  psp_ctrl_replay_active(void);
/* The scenario's own `drain <s>`, or 0 if it did not say. A host may use it
 * as the default so a scenario carries the run length it needs. */
int  psp_ctrl_replay_drain(void);

/* ---- host presentation hooks ----------------------------------------------
 *
 * A windowed host registers these to receive what the game produces as it
 * produces it. Both callees run on a guest thread holding the scheduler
 * token, so they must convert and hand off, never block. Passing NULL
 * unregisters. */
/* Each presented frame: the buffer the game last set, as the game supplied
 * it. Called from sceDisplaySetFrameBuf -- the frame flip, and the only
 * cadence this game keeps; it never asks for a vblank. */
void psp_display_set_present(void (*fn)(uint32_t addr, uint32_t stride,
                                        uint32_t fmt));
/* Each output buffer: the channel, its reserved shape (sample count per call
 * and PSP_AUDIO_FORMAT_STEREO 0 / MONO 0x10), where the PCM lives in guest
 * memory, and the left and right volumes on the 0..0x8000 scale the call was
 * given. Returns the playback backlog in microseconds, which the blocking
 * output calls pay with a scheduler delay. */
void psp_audio_set_output(int64_t (*fn)(int ch, uint32_t samples,
                                        uint32_t fmt, uint32_t buf,
                                        uint32_t lvol, uint32_t rvol));
/* The host-clock gaps between each channel's outputs; see audio_note_gap. */
void psp_audio_dump_gaps(FILE *out);
/* A movie's audio clock against its picture; see mpeg.c. */
void psp_mpeg_dump_sync(FILE *out);

/* ---- the ATRAC3 / ATRAC3+ decoder, shared -----------------------------------
 *
 * libavcodec behind an opaque handle, owned by atrac.c and used by mpeg.c for
 * a movie's audio too. Without libavcodec open fails and the rest are inert.
 * `codec` is PSP_ATRAC_AT3PLUS (0x1000) or PSP_ATRAC_AT3 (0x1001);
 * `block_align` the bytes per frame the decoder is handed; `extradata` the
 * ATRAC3 WAVE fmt tail or NULL. A frame decodes to interleaved stereo S16,
 * mono played on both sides; returns the sample count or -1. */
typedef struct psp_at3_dec psp_at3_dec;
psp_at3_dec *psp_at3_open(uint32_t codec, uint32_t block_align, uint32_t channels,
                          uint32_t sample_rate, const uint8_t *extradata, uint32_t extradata_size);
int  psp_at3_decode(psp_at3_dec *d, const uint8_t *in, uint32_t len, int16_t *out, uint32_t cap);
void psp_at3_flush(psp_at3_dec *d);
void psp_at3_close(psp_at3_dec *d);

/* The priority of whatever is running now, or the module entry thread's when
 * the scheduler holds nothing. Needed by a spawn hook deciding whether a newly
 * started thread outranks its starter. */
uint32_t psp_threadman_current_priority(void);
/* Deliver the current thread's pending callbacks, returning whether any ran.
 * Every firmware call whose name ends in CB is a wait that does this first. */
int psp_threadman_run_callbacks(void);
/* The same, on the way *out* of a firmware call and only if that call did what
 * it was asked. This is how a CB wait delivers: see the comment on it. */
/* Every CB wait brackets itself with this pair; the delivery happens in
 * between, at the moment the wait is about to start. See waitq.h. */
void psp_threadman_cb_begin(void);
void psp_threadman_cb_end(void);
/* Raise a callback from outside threadman -- scePowerRegisterCallback fires one
 * as it registers. Returns what sceKernelNotifyCallback would. */
uint32_t psp_threadman_notify_callback(uint32_t cbid, uint32_t arg);

/* Shared with the other kernel object types: one uid space, and one way of
 * writing a name into a SceKernel*Info block. */
uint32_t psp_threadman_next_uid(void);

/* ---- enumerating live kernel objects --------------------------------------
 *
 * sceKernelGetThreadmanIdList asks for every uid of one type, and the objects
 * are spread over four files with their tables private to each. Rather than
 * export the tables, each module registers a lister and answers for the types
 * it owns: `count` is the running total across all of them and keeps counting
 * past `max`, because the call reports how many *exist* while filling only as
 * much of the buffer as it was given. */
/* `out` is a *guest* address, or 0 for a caller that wants only the count. */
typedef void (*psp_uid_lister)(int type, uint32_t out, int max, int *count);
void psp_threadman_add_lister(psp_uid_lister fn);
/* What a lister does with each uid it owns of the asked type: append it to
 * `out` if there is room, and count it either way. Through one function rather
 * than written out in each lister, because sceKernelGetThreadmanIdType asks
 * the same listers a different question -- "is this uid one of yours?" -- and
 * this is where that question is answered. */
void psp_threadman_list_put(uint32_t uid, uint32_t out, int max, int *count);

/* The types the kernel knows. 1..14 are object kinds; 0x40..0x43 select
 * threads by what they are doing. Anything else is ILLEGAL_TYPE -- measured,
 * threads/threadmanidlist sweeps 0, 15..24, 0x44..0x48 and a spread of large
 * values and refuses every one.
 *
 * From 9 up these were one too high (alarm 11, vtimer 12, mutex 13, with a
 * gap at 10). threadprobe steps 99-100 (fw 6.60) list and type an alarm as
 * 0x0A, a vtimer 0x0B, a mutex 0x0C, an lwmutex under 0x0D and a tlspl 0x0E,
 * with nothing under 9; PSPSDK's pspthreadman.h also has Alarm = 10 and
 * VTimer = 11. */
enum {
    PSP_TMID_THREAD = 1, PSP_TMID_SEMA = 2, PSP_TMID_EVENTFLAG = 3,
    PSP_TMID_MBX = 4, PSP_TMID_VPL = 5, PSP_TMID_FPL = 6, PSP_TMID_MSGPIPE = 7,
    PSP_TMID_CALLBACK = 8, PSP_TMID_THREVENT = 9,
    PSP_TMID_ALARM = 10, PSP_TMID_VTIMER = 11, PSP_TMID_MUTEX = 12,
    PSP_TMID_LWMUTEX = 13, PSP_TMID_TLSPL = 14,
    PSP_TMID_SLEEPING = 0x40, PSP_TMID_DELAYING = 0x41,
    PSP_TMID_SUSPENDED = 0x42, PSP_TMID_DORMANT = 0x43,
};
/* What sceKernelReferThreadStatus reports as waitType for a thread waiting
 * on each object kind, and waitId the object's uid. threadprobe (fw 6.60)
 * steps 9-12 measured sleep 1, delay 2, sema 3, evf 4 and thread end 9; step
 * 17 of version 3 measured mbx 5, vpl 6, fpl 7, msgpipe 8, mutex 0x0C,
 * lwmutex 0x0D and tlspl 0x0E. From mbx on, each is its id-list type plus 1
 * up to msgpipe and equal to it from mutex on. */
enum {
    PSP_WAITTYPE_SLEEP = 1, PSP_WAITTYPE_DELAY = 2, PSP_WAITTYPE_SEMA = 3,
    PSP_WAITTYPE_EVF = 4, PSP_WAITTYPE_MBX = 5, PSP_WAITTYPE_VPL = 6,
    PSP_WAITTYPE_FPL = 7, PSP_WAITTYPE_MSGPIPE = 8, PSP_WAITTYPE_THREADEND = 9,
    PSP_WAITTYPE_MUTEX = 0x0C, PSP_WAITTYPE_LWMUTEX = 0x0D,
    PSP_WAITTYPE_TLSPL = 0x0E,
};
/* Record what the current thread is about to wait on, for ReferThreadStatus,
 * and (0, 0) once the wait is over. */
void     psp_threadman_wait_mark(uint32_t type, uint32_t id);
void     psp_threadman_write_name(uint32_t dst, const char *name);
/* Copy a Refer*Status result into the caller's block. `img` is the whole
 * struct as the kernel builds it, first word = its own size; hardware copies
 * min(the caller's size word, len) bytes of it (syncprobe step 24: size 4
 * gets only the size word). A size of 0 therefore gets nothing. */
void     psp_refer_put(uint32_t info, const uint8_t *img, uint32_t len);
static inline void psp_refer_img32(uint8_t *img, uint32_t off, uint32_t v) {
    img[off] = (uint8_t)v; img[off + 1] = (uint8_t)(v >> 8);
    img[off + 2] = (uint8_t)(v >> 16); img[off + 3] = (uint8_t)(v >> 24);
}
/* The 32-byte name at offset 4, truncated to 31 characters. */
static inline void psp_refer_imgname(uint8_t *img, const char *name) {
    for (int i = 0; i < 32; i++) img[4 + i] = 0;
    for (int i = 0; i < 31 && name[i]; i++) img[4 + i] = (uint8_t)name[i];
}

void psp_kernlock_register(void);
void psp_kernlock_register_lw(void);
void psp_kernobj_register(void);
void psp_kernobj_register_mpp(void);
void psp_kernobj_register_mbx(void);
void psp_kernobj_register_fpl(void);
void psp_kernobj_register_tls(void);
void psp_ktimer_register(void);
void psp_ktimer_register_vtimer(void);
void psp_ktimer_reset(void);
/* Fire any timer object whose moment has passed. Called from the firmware-call
 * path, which is the only place guest time is observed to move. */
void psp_ktimer_tick(void);
void psp_kernobj_reset(void);
/* A thread has ended: return anything it still holds. Thread-local storage
 * does, and so does a mutex -- syncprobe step 90 (fw 6.60): a mutex whose
 * owner exits reads back free (owner -1) and the next TryLock succeeds. A
 * pool block outlives its owner. */
void psp_kernobj_thread_ended(uint32_t uid);
void psp_kernlock_thread_ended(uint32_t uid);
void psp_kernlock_reset(void);

void psp_threadman_init(void);
void psp_threadman_register(void);
void psp_threadman_reset(void);

/* Bytes of user memory still available — the cheapest end-to-end check that
 * the allocator is behaving. */
uint32_t psp_sysmem_free(void);

/* Tell the allocator where the loaded module sits, so the user heap can be
 * everything else. Call after psp_hle_init(), which resets the allocator.
 *
 * Without this the heap floor is a guess -- "modules load around 0x08800000
 * and are a few megabytes" -- which costs a megabyte of a 24 MB partition and
 * is simply wrong for a module that is not in user RAM at all. */
void psp_sysmem_reserve_module(uint32_t lo, uint32_t hi);

/* Whether an alarm or vtimer handler is running right now. Nonzero means the
 * caller is on no thread at all -- see the header of src/hle/ktimer.c -- which
 * sceKernelGetThreadId has to report and cannot work out for itself. */
int psp_ktimer_in_handler(void);

/* For the scheduler's idle path, where no thread will make the firmware call
 * that would notice a timer: the guest moment the next alarm or vtimer
 * handler is due (0 for none), and a call that runs whatever is due now and
 * returns how many handlers ran. */
uint64_t psp_ktimer_next_due(void);
int      psp_ktimer_fire_idle(void);

/* Raw allocation for use by other HLE subsystems (thread stacks, mostly).
 * Returns 0 on failure. These bypass the UID table because nothing in the
 * guest ever refers to them. */
uint32_t psp_sysmem_alloc(uint32_t size, int from_high);
void     psp_sysmem_release(uint32_t addr);

#ifdef __cplusplus
}
#endif

#endif /* PSPRECOMP_HLE_H */
