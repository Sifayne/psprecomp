/* The park census: where every guest thread waits at a save point
 * (docs/PLAYER-LAYER.md §5, "The census"). Internal to the runtime.
 *
 * A save state resumes a thread natively from the return addresses on its
 * guest stack. That only works for a thread whose C stack holds nothing but
 * guest frames down to the firmware call it is parked in. So the census
 * records, for each park: what the thread waits on, the innermost firmware
 * call and the guest return site it was made from, and every host frame
 * between guest frames -- a callback, an interrupt handler, a host
 * replacement of a guest function. sched.c stores one record per thread when
 * it parks; PSPRECOMP_PARK_CENSUS=<poll>[,<poll>...] prints them all at the
 * safe point once those polls are reached.
 *
 * Save states (psprecomp/state.h) are built on the same records, so what the
 * runtime's modules tell each other about saving and resuming is here too. */
#ifndef PSPRECOMP_HLE_CENSUS_H
#define PSPRECOMP_HLE_CENSUS_H

#include <stdint.h>
#include <stdio.h>

enum { PSP_PARK_NONE, PSP_PARK_BLOCK, PSP_PARK_DELAY, PSP_PARK_YIELD, PSP_PARK_PREEMPT };
enum { PSP_PARK_NEST_MAX = 4 };

typedef struct {
    uint8_t     kind;           /* PSP_PARK_* */
    const char *what;           /* the wait's own description */
    uint64_t    deadline;       /* guest microseconds; 0: none */
    /* The innermost firmware call on this thread, and how deep they nest:
     * 1 is a call from the thread's own guest code. */
    uint32_t    nid;
    uint32_t    site;           /* $ra when it was made: the guest resumes here */
    uint8_t     calls;
    /* Host frames between guest frames, innermost last (PSP_NEST_*). */
    uint8_t     nest;
    uint8_t     nest_kind[PSP_PARK_NEST_MAX];
    uint32_t    nest_addr[PSP_PARK_NEST_MAX];
} psp_park;

/* hle.c, around every firmware call. */
void psp_census_call_enter(uint32_t nid, uint32_t site);
void psp_census_call_leave(void);
/* sched.c, as the calling host thread parks. */
void psp_census_note_park(psp_park *out, int kind, const char *what, uint64_t deadline);

/* The safe point (psprecomp/safepoint.h): hle.c asks for it after a
 * firmware call from the thread's own guest code (depth 1) returns, while
 * anything is armed -- a pause request, a scripted hold, a pending census.
 * It runs the census there. */
extern _Atomic int psp_safepoint_armed;
void psp_safepoint(uint32_t nid);
void psp_safepoint_rearm(void);
extern uint32_t psp_census_next;
void psp_census_check(void);
/* sched.c, for state.c: why a save is impossible, the slots made ready to be
 * written, and the threads restarted last of all when a state loads. */
const char *psp_sched_state_refuse(void);
int  psp_sched_state_prepare(void);
int  psp_sched_state_load(char *why, size_t size);
/* sched.c, for the calls that finish a wait begun before a load
 * (psp_hle_register_resume): the restored thread's first wait, answered as
 * psp_sched_block_until or psp_sched_delay would have answered it, with the
 * deadline it was given; and the saving thread's token. */
int  psp_sched_resume_block(uint64_t *deadline);
int  psp_sched_resume_delay(uint64_t *deadline);
void psp_sched_resume_token(void);
/* What a save state keeps of the modules with no registration of their own
 * (psp_state_keep): clock.c's, by sched.c. */
void psp_clock_keep(void);
/* sched.c: where in its call a thread is, for a call that can park at more
 * than one place; kept with the thread, so it survives a save state. */
void     psp_sched_set_step(uint8_t step);
uint8_t  psp_sched_step(void);
/* census.c: the host frames between this thread's guest frames, innermost
 * last; returns how many. */
int  psp_census_self_nest(uint8_t *kind, uint32_t *addr, int max);
/* hle.c: whether a thread parked in this call can finish it after a load,
 * and finishing it -- then everything a call ends with -- on a restored
 * thread. `safepoint`: the thread that saved, inside psp_safepoint. */
int  psp_hle_resumable(uint32_t nid);
void psp_hle_resume(uint32_t nid, uint32_t site, int safepoint);
/* safepoint.c: the call the safe point is in, while it is; else 0. */
uint32_t psp_safepoint_nid(void);
/* state.c: PSPRECOMP_SAVE_STATE's saves, taken at the safe point. */
int  psp_state_scripted_pending(void);
void psp_state_scripted(void);

/* sched.c: one line per live thread, from the records above. */
void psp_sched_census(FILE *out, uint32_t self);
void psp_census_print_park(FILE *out, const psp_park *p);
/* One tab-separated line for collecting the census across runs. */
void psp_census_row(FILE *out, uint32_t uid, const char *name, const char *state,
                    const psp_park *p, uint32_t entry);
/* Reads PSPRECOMP_PARK_CENSUS; psp_sched_init calls it. */
void psp_census_init(void);

/* What the census asks of other modules. */
uint32_t    psp_ge_owner(void);                  /* ge.c: the last thread to run a list; 0: none */
const char *psp_threadman_thread_name(uint32_t uid);   /* threadman.c; "" when unknown */
int         psp_mpeg_census(int *fed);           /* mpeg.c: contexts in use, and those with stream data */
int         psp_utility_census(void);            /* utility.c: the savedata dialog's status */

#endif
