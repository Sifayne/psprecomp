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
#define SCE_KERNEL_ERROR_ILLEGAL_MEMBLOCK 0x800201A9
#define SCE_KERNEL_ERROR_ILLEGAL_THID    0x80020197
/* A thread id that names nothing, as distinct from one that is malformed.
 * threads/refer.expected answers both a deleted and an invalid id with this. */
#define SCE_KERNEL_ERROR_UNKNOWN_THID    0x80020198
/* A thread that has not been started, or has already finished. Shares its
 * value with UNKNOWN_UID above -- the uid space is one space, and the kernel
 * spends its codes on the *situation* rather than on the object. */
#define SCE_KERNEL_ERROR_DORMANT         0x800201A2
#define SCE_KERNEL_ERROR_SUSPEND         0x800201A3
#define SCE_KERNEL_ERROR_NOT_SUSPEND     0x800201A5
/* A poll that would have blocked. Distinct from an error: it is the ordinary
 * answer to "is this free?" when it is not. */
#define SCE_KERNEL_ERROR_SEMA_ZERO       0x800201AD
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

void psp_misc_init(void);
void psp_misc_register(void);
void psp_misc_reset(void);

/* ---- sceUmdUser ---------------------------------------------------------- */
void psp_umd_init(void);
void psp_umd_register(void);
void psp_umd_reset(void);
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
uint32_t psp_ctrl_samples(void);

/* Publish into the script lane. `analog_owned` non-zero takes the stick away
 * from live input until a later call hands it back. */
void psp_ctrl_script_set(uint32_t buttons, int analog_owned,
                         uint8_t ax, uint8_t ay);

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
/* Record the composed pad state; writes a line only when it changes. */
void psp_ctrl_replay_record(uint32_t polls, uint64_t us,
                            uint32_t buttons, uint8_t ax, uint8_t ay);
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
/* Each output buffer: the channel, its reserved shape, and where the PCM
 * lives in guest memory. Returns the playback backlog in microseconds, which
 * the blocking output calls pay with a scheduler delay. */
void psp_audio_set_output(int64_t (*fn)(int ch, uint32_t samples,
                                        uint32_t fmt, uint32_t buf));

/* The priority of whatever is running now, or the module entry thread's when
 * the scheduler holds nothing. Needed by a spawn hook deciding whether a newly
 * started thread outranks its starter. */
uint32_t psp_threadman_current_priority(void);

/* Shared with the other kernel object types: one uid space, and one way of
 * writing a name into a SceKernel*Info block. */
uint32_t psp_threadman_next_uid(void);
void     psp_threadman_write_name(uint32_t dst, const char *name);

void psp_kernlock_register(void);
void psp_kernlock_register_lw(void);
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

/* Raw allocation for use by other HLE subsystems (thread stacks, mostly).
 * Returns 0 on failure. These bypass the UID table because nothing in the
 * guest ever refers to them. */
uint32_t psp_sysmem_alloc(uint32_t size, int from_high);
void     psp_sysmem_release(uint32_t addr);

#ifdef __cplusplus
}
#endif

#endif /* PSPRECOMP_HLE_H */
