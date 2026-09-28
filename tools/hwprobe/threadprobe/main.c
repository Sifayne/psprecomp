/* threadprobe -- the thread manager and time, measured on a PSP.
 *
 * Written against PSPSDK (BSD) only. Every step re-measures something
 * psprecomp's runtime (src/hle/threadman.c, sched.c, kernobj.c, ktimer.c,
 * clock.c, sysmem.c, misc.c) claims; the comment above a step names the
 * psprecomp file:line whose claim it checks.
 *
 * How ordering is observed: worker threads run little scripts (script_entry)
 * that append a tag to one shared sequence buffer (g_seq) as they reach each
 * point, and the main thread tags its own points too. Nothing is printed while
 * a scenario is live -- printing is file I/O, which blocks the main thread and
 * would let ready workers run -- so each scenario collects its results in
 * memory (rec) and they are written after it has cleaned up. The order the
 * firmware chose is the "seq:" line.
 *
 * Normalisation: UIDs are logged as names the probe gave them ("main", "sema",
 * "uid?" for one it does not know), addresses as a class ("user", "kseg") or an
 * offset from the thread's own stack base ("stk+XXXX"), times only as
 * relations. Every wait has a timeout, and every object is deleted before the
 * scenario ends.
 *
 * The last section passes NULL and small bad pointers; it runs after
 * everything else so that a crash there loses nothing.
 */
#include <pspkernel.h>
#include <pspthreadman.h>
#include <pspsysmem.h>
#include <psputils.h>
#include <psppower.h>
#include <psprtc.h>
#include <pspiofilemgr.h>
#include <pspintrman.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "probe.h"

PSP_MODULE_INFO("threadprobe", PSP_MODULE_USER, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
/* A small fixed libc heap, so user memory is left free for the thread
 * stacks, TLS pools and sysmem steps (PSPSDK's default heap takes nearly
 * all of it). */
PSP_HEAP_SIZE_KB(256);

#define PROBE_VERSION 3

typedef unsigned int w32;

/* Imports PSPSDK does not declare; stubs.S has them, with psprecomp's NIDs. */
SceUID sceKernelCreateTlspl(const char *name, int part, SceUInt attr,
                            SceUInt blockSize, SceUInt count, void *opt);
int    sceKernelDeleteTlspl(SceUID uid);
void  *sceKernelGetTlsAddr(SceUID uid);
int    sceKernelFreeTlspl(SceUID uid);
int    sceKernelReferTlsplStatus(SceUID uid, void *info);
SceUID sceKernelCreateMutex(const char *name, SceUInt attr, int count, void *opt);
int    sceKernelDeleteMutex(SceUID uid);
int    sceKernelLockMutex(SceUID uid, int count, SceUInt *timeout);
int    sceKernelUnlockMutex(SceUID uid, int count);
/* 56 bytes: size, name[32], attr, initCount, currentCount, lockThread,
 * numWaitThreads (word 13). */
int    sceKernelReferMutexStatus(SceUID uid, void *info);

/* stubs.S */
extern w32 g_entry_regs[68];
int regs_entry(SceSize len, void *argp);
int regs_body(SceSize len, void *argp);
w32 g_entry_regs[68];

#define ATTR_NO_FILLSTACK 0x00100000u
#define ATTR_CLEAR_STACK  0x00200000u
#define ATTR_LOW_STACK    0x00400000u

/* ---- output ---------------------------------------------------------------
 *
 * rec() collects lines in memory only (no firmware call); ST() and SEC() write
 * what was collected and then start the next step or section. */

static char g_rec[16384];
static int  g_recn;

static void rec(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void rec(const char *fmt, ...) {
    va_list ap;
    int room = (int)sizeof g_rec - g_recn;
    if (room <= 1) return;
    va_start(ap, fmt);
    int n = vsnprintf(g_rec + g_recn, room, fmt, ap);
    va_end(ap);
    if (n > 0) g_recn += n < room ? n : room - 1;
}

static void rec_flush(void) {
    int i = 0;
    while (i < g_recn) {
        int j = i;
        while (j < g_recn && g_rec[j] != '\n') j++;
        out("%.*s\n", j - i, g_rec + i);
        i = j + 1;
    }
    g_recn = 0;
}

/* Returns 1 when the step is to be skipped because an earlier run of this
 * version stopped in it (see step() in probe.h). */
static int ST(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int ST(const char *fmt, ...) {
    char buf[240];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    rec_flush();
    return step("%s", buf);
}

static void SEC(const char *name) {
    rec_flush();
    section("%s", name);
}

/* Rotating scratch strings for formatting helpers. */
static char g_sb[24][40];
static int  g_sbi;
static char *sb(void) { return g_sb[g_sbi++ % 24]; }

static const char *hx(int v) { char *b = sb(); snprintf(b, 40, "%08X", (w32)v); return b; }
static const char *hx64(unsigned long long v) {
    char *b = sb();
    snprintf(b, 40, "%08X_%08X", (w32)(v >> 32), (w32)v);
    return b;
}
/* A create result: a UID (logged only as valid) or an error code. */
static const char *cu(int v) { return v > 0 ? "uid" : hx(v); }
/* A timeout word after a wait, as a relation to what it held before: what is
 * left after a wait that blocked depends on timing. */
static const char *ta(w32 after, w32 before) {
    if (after == before) return "unchanged";
    if (after == 0) return "0";
    return after < before ? "reduced" : "grew";
}

/* Names for UIDs the probe knows, so a waitId or threadId reads as what it is. */
static struct { int v; const char *n; } g_nm[64];
static int g_nnm, g_nperm;
static void nm_add(int v, const char *n) {
    if (v <= 0) return;
    for (int i = 0; i < g_nnm; i++) if (g_nm[i].v == v) { g_nm[i].n = n; return; }
    if (g_nnm < 64) { g_nm[g_nnm].v = v; g_nm[g_nnm].n = n; g_nnm++; }
}
static void nm_reset(void) { g_nnm = g_nperm; }
static const char *nm(int v) {
    if (v == 0) return "0";
    for (int i = 0; i < g_nnm; i++) if (g_nm[i].v == v) return g_nm[i].n;
    return v < 0 ? hx(v) : "uid?";
}

static w32 g_gp;
static w32 get_gp(void) { w32 v; __asm__ volatile("move %0, $gp" : "=r"(v)); return v; }

static const char *cls(w32 v) {
    if (!v) return "0";
    if (v >= 0x80000000u) return "kseg";
    if (v >= 0x08800000u && v < 0x0A000000u) return "user";
    if (v >= 0x08000000u && v < 0x08800000u) return "kram";
    if (v >= 0x00010000u && v < 0x00014000u) return "scratch";
    return "other";
}
/* A word that should have been a known value: RAM addresses are logged by
 * class, anything else (codes, small integers, fill patterns) as is. */
static const char *pv(w32 v) {
    const char *c = cls(v);
    if (!strcmp(c, "user") || !strcmp(c, "kram") || !strcmp(c, "scratch")) return c;
    return hx((int)v);
}

/* An address relative to a stack [base, base+size]. */
static const char *stkrel(w32 v, w32 base, w32 size) {
    char *b = sb();
    if (!v) return "NULL";
    if (v >= base && v <= base + size) { snprintf(b, 40, "stk+%X", v - base); return b; }
    return cls(v);
}

/* ---- the shared sequence ----------------------------------------------------
 *
 * Appended to by any thread (and by callback handlers), read by main after the
 * scenario. No firmware calls in here. */

static char g_seq[1024];
static volatile int g_seqn;

static void seq_clear(void) { g_seqn = 0; g_seq[0] = 0; }
static void seq_put(const char *s) {
    int n = g_seqn;
    while (*s && n < (int)sizeof g_seq - 2) g_seq[n++] = *s++;
    g_seq[n] = 0;
    g_seqn = n;
}
static void seq_hex(w32 v) {
    char b[9];
    for (int i = 0; i < 8; i++) b[i] = "0123456789ABCDEF"[(v >> (28 - 4 * i)) & 15];
    b[8] = 0;
    seq_put(b);
}
static void tag(const char *s) { if (!s) return; if (g_seqn) seq_put(" "); seq_put(s); }
static void tagv(const char *s, int v) { if (!s) return; tag(s); seq_put("="); seq_hex((w32)v); }
static void tagr(const char *s, int r) { if (r) tagv(s, r); else tag(s); }
static void rec_seq(void) { rec("  seq: %s\n", g_seqn ? g_seq : "(none)"); }

/* ---- small helpers ---------------------------------------------------------- */

static SceUID g_main;
static int    g_starts;          /* successful sceKernelStartThread calls */
static int    g_argc;            /* main()'s */
static char **g_argv;

static void spin_us(w32 us) {
    w32 t0 = sceKernelGetSystemTimeLow();
    while ((w32)(sceKernelGetSystemTimeLow() - t0) < us) { }
}

/* A scheduling point before an ordering scenario starts. Nothing else is
 * runnable at that moment, so on hardware it only costs 100us; under
 * psprecomp it restarts the main thread's timeslice, so a slice that happened
 * to expire mid-scenario does not show up as a reordering. */
static void fresh(void) { sceKernelDelayThread(100); }

static int wait_end(SceUID th, w32 us) {
    SceUInt t = us;
    return sceKernelWaitThreadEnd(th, &t);
}

static void reap(SceUID th) {
    if (th <= 0) return;
    if (sceKernelTerminateDeleteThread(th) < 0) sceKernelDeleteThread(th);
}

/* ---- script workers ------------------------------------------------------- */

enum {
    O_END = 0, O_TAG, O_SLEEP, O_SLEEPCB, O_DELAY, O_DELAYCB, O_SPIN,
    O_WAITSEMA, O_SIGNAL, O_WAKEUP, O_EXIT, O_EXITDEL, O_ROTATE, O_CHPRI,
    O_CHECKCB, O_MKCB, O_WAITEND, O_TLSGET, O_TLSFREE, O_REFERSELF,
    O_WAITEF, O_SELFOPS, O_ATTRSWEEP, O_STACKFREE, O_RELEASE,
    /* version 3: waits on the other object kinds, each with timeout `a` */
    O_RECVMBX, O_ALLOCVPL, O_ALLOCFPL, O_RECVMPP, O_LOCKMTX, O_LOCKLW,
};
typedef struct { int op; int a; const char *s; } op_t;

static SceUID g_uid[8];          /* threads a script can name: [0] is main */
static SceUID g_sema, g_evf, g_tls;
static SceUID g_mbx, g_vpl, g_fpl, g_mpp, g_mtx;
static SceLwMutexWorkarea g_lw;
/* The timeout word after a script's last timed wait (0xEEEEEEEE: none ran). */
static volatile SceUInt g_tleft;
static SceUID g_cbw[4];          /* callbacks created by workers */
static w32    g_tlsaddr[8];
static int    g_selfres[32];
static int    g_attrres[64];
static SceKernelThreadInfo g_wref;
static int    g_wref_ret;

static volatile int g_cbhits;
static struct { int a0, a1; w32 common; int tid; } g_cbh[8];
static volatile int g_cbret;
static int h_count(int a0, int a1, void *common) {
    int i = g_cbhits;
    if (i < 8) {
        g_cbh[i].a0 = a0; g_cbh[i].a1 = a1;
        g_cbh[i].common = (w32)common;
        g_cbh[i].tid = sceKernelGetThreadId();
    }
    g_cbhits = i + 1;
    tag("h");
    return g_cbret;
}

static void self_ops(void) {
    SceUInt t;
    SceUID me = sceKernelGetThreadId();
    for (int i = 0; i < 32; i++) g_selfres[i] = (int)0xEEEEEEEE;
    g_selfres[0]  = me > 0 && me != g_main;
    g_selfres[1]  = sceKernelDeleteThread(0);
    g_selfres[2]  = sceKernelDeleteThread(me);
    t = 1000; g_selfres[3] = sceKernelWaitThreadEnd(0, &t);
    t = 1000; g_selfres[4] = sceKernelWaitThreadEnd(me, &t);
    g_selfres[5]  = sceKernelStartThread(me, 0, NULL);
    g_selfres[6]  = sceKernelResumeThread(0);
    g_selfres[7]  = sceKernelResumeThread(me);
    g_selfres[8]  = sceKernelGetThreadExitStatus(0);
    g_selfres[9]  = sceKernelGetThreadExitStatus(me);
    g_selfres[10] = sceKernelWakeupThread(0);
    g_selfres[11] = sceKernelCancelWakeupThread(0);
    g_selfres[12] = sceKernelWakeupThread(me);
    g_selfres[13] = sceKernelCancelWakeupThread(me);
    g_selfres[14] = sceKernelChangeThreadPriority(0, 0x11);
    g_selfres[15] = sceKernelGetThreadCurrentPriority();
    g_selfres[16] = sceKernelChangeThreadPriority(me, 0x10);
    g_selfres[17] = sceKernelGetThreadCurrentPriority();
    /* Last, because on a firmware that allowed them the thread would stop. */
    g_selfres[18] = sceKernelSuspendThread(0);
    g_selfres[19] = sceKernelSuspendThread(me);
    g_selfres[20] = sceKernelTerminateThread(0);
    g_selfres[21] = sceKernelTerminateThread(me);
    g_selfres[22] = sceKernelTerminateDeleteThread(0);
    g_selfres[23] = sceKernelTerminateDeleteThread(me);
    g_selfres[24] = 0x600D;
}

static int script_entry(SceSize len, void *argp) {
    if (len < sizeof(void *) || !argp) return 0x0BAD;
    const op_t *p = *(const op_t *const *)argp;
    for (;; p++) {
        int r;
        SceUInt t;
        switch (p->op) {
        case O_END:     return p->a;
        case O_TAG:     tag(p->s); break;
        case O_SLEEP:   r = sceKernelSleepThread();   tagr(p->s, r); break;
        case O_SLEEPCB: r = sceKernelSleepThreadCB(); tagr(p->s, r); break;
        case O_DELAY:   r = sceKernelDelayThread(p->a); tagr(p->s, r); break;
        case O_DELAYCB: {
            w32 t0 = sceKernelGetSystemTimeLow();
            r = sceKernelDelayThreadCB(p->a);
            w32 dt = sceKernelGetSystemTimeLow() - t0;
            tagr(p->s, r);
            if (dt < (w32)p->a) seq_put("(short)");
            break;
        }
        case O_SPIN:    spin_us(p->a); tag(p->s); break;
        case O_WAITSEMA:
            t = p->a; r = sceKernelWaitSema(g_sema, 1, &t); g_tleft = t; tagr(p->s, r); break;
        case O_SIGNAL:  r = sceKernelSignalSema(g_sema, 1); tagr(p->s, r); break;
        case O_WAKEUP:  r = sceKernelWakeupThread(g_uid[p->a]); tagr(p->s, r); break;
        case O_EXIT:    r = sceKernelExitThread(p->a); tagv(p->s, r); break;
        case O_EXITDEL: r = sceKernelExitDeleteThread(p->a); tagv(p->s, r); break;
        case O_ROTATE:  r = sceKernelRotateThreadReadyQueue(p->a); tagr(p->s, r); break;
        case O_CHPRI:   r = sceKernelChangeThreadPriority(0, p->a); tagr(p->s, r); break;
        case O_CHECKCB: r = sceKernelCheckCallback(); tagv(p->s, r); break;
        case O_MKCB:
            g_cbw[p->a] = sceKernelCreateCallback(p->s, h_count, (void *)(p->a + 1));
            break;
        case O_WAITEND:
            t = 500000; r = sceKernelWaitThreadEnd(g_uid[p->a], &t); g_tleft = t; tagv(p->s, r); break;
        case O_TLSGET: {
            void *q = sceKernelGetTlsAddr(g_tls);
            g_tlsaddr[p->a] = (w32)q;
            if (q) tag(p->s); else tagv(p->s, 0);
            break;
        }
        case O_TLSFREE: r = sceKernelFreeTlspl(g_tls); tagr(p->s, r); break;
        case O_REFERSELF:
            memset(&g_wref, 0, sizeof g_wref);
            g_wref.size = sizeof g_wref;
            g_wref_ret = sceKernelReferThreadStatus(0, &g_wref);
            break;
        case O_WAITEF: {
            u32 bits = 0;
            t = p->a;
            r = sceKernelWaitEventFlag(g_evf, 1, PSP_EVENT_WAITAND, &bits, &t);
            g_tleft = t;
            tagr(p->s, r);
            break;
        }
        case O_SELFOPS: self_ops(); break;
        case O_ATTRSWEEP:
            for (int b = 0; b < 32; b++)
                g_attrres[b] = sceKernelChangeCurrentThreadAttr(0, 1u << b);
            for (int b = 0; b < 32; b++)
                g_attrres[32 + b] = sceKernelChangeCurrentThreadAttr(1u << b, 0);
            memset(&g_wref, 0, sizeof g_wref);
            g_wref.size = sizeof g_wref;
            g_wref_ret = sceKernelReferThreadStatus(0, &g_wref);
            break;
        case O_STACKFREE:
            g_selfres[0] = sceKernelGetThreadStackFreeSize(0);
            g_selfres[1] = sceKernelGetThreadStackFreeSize(sceKernelGetThreadId());
            g_selfres[2] = sceKernelCheckThreadStack();
            break;
        case O_RELEASE: r = sceKernelReleaseWaitThread(g_uid[p->a]); tagr(p->s, r); break;
        /* What a wait takes, it gives back, so the object is as it was. */
        case O_RECVMBX: {
            void *m = NULL;
            t = p->a; r = sceKernelReceiveMbx(g_mbx, &m, &t); g_tleft = t; tagr(p->s, r);
            break;
        }
        case O_ALLOCVPL: {
            void *d = NULL;
            t = p->a; r = sceKernelAllocateVpl(g_vpl, 0x800, &d, &t); g_tleft = t; tagr(p->s, r);
            if (r == 0 && d) sceKernelFreeVpl(g_vpl, d);
            break;
        }
        case O_ALLOCFPL: {
            void *d = NULL;
            t = p->a; r = sceKernelAllocateFpl(g_fpl, &d, &t); g_tleft = t; tagr(p->s, r);
            if (r == 0 && d) sceKernelFreeFpl(g_fpl, d);
            break;
        }
        case O_RECVMPP: {
            char m[8];
            t = p->a; r = sceKernelReceiveMsgPipe(g_mpp, m, 4, 0, NULL, &t); g_tleft = t; tagr(p->s, r);
            break;
        }
        case O_LOCKMTX:
            t = p->a; r = sceKernelLockMutex(g_mtx, 1, &t); g_tleft = t; tagr(p->s, r);
            if (r == 0) sceKernelUnlockMutex(g_mtx, 1);
            break;
        case O_LOCKLW:
            t = p->a; r = sceKernelLockLwMutex(&g_lw, 1, &t); g_tleft = t; tagr(p->s, r);
            if (r == 0) sceKernelUnlockLwMutex(&g_lw, 1);
            break;
        default: return 0x0BAD;
        }
    }
}

/* The script pointer is passed by address, and the kernel copies the four
 * bytes onto the new thread's stack. psprecomp hands over the address itself
 * (threadman.c:335), so it has to stay valid after the caller moves on:
 * hence static slots rather than a local. */
static const op_t *g_argslot[256];
static int g_argn;

static SceUID mk(const char *name, int prio) {
    SceUID th = sceKernelCreateThread(name, script_entry, prio, 0x2000, 0, NULL);
    nm_add(th, name);
    return th;
}
static int go(SceUID th, const op_t *ops) {
    const op_t **slot = &g_argslot[g_argn++ & 255];
    *slot = ops;
    int r = sceKernelStartThread(th, sizeof *slot, slot);
    if (r == 0) g_starts++;
    return r;
}
static SceUID spawn(const char *name, int prio, const op_t *ops) {
    SceUID th = mk(name, prio);
    if (th > 0) go(th, ops);
    return th;
}

/* ---- report helpers --------------------------------------------------------- */

/* Every word of SceKernelThreadInfo: UIDs by name, addresses by class, the
 * run clock and interrupt preemptions (which depend on timing) as zero or not. */
static void rec_thinfo(const SceKernelThreadInfo *ti, const void *entry) {
    char name[33];
    memcpy(name, ti->name, 32);
    name[32] = 0;
    w32 st = (w32)ti->stack, en = (w32)ti->entry;
    rec("    size=%d name=\"%s\" attr=%08X status=%08X\n",
        (int)ti->size, name, (w32)ti->attr, (w32)ti->status);
    rec("    entry=%s stack=%s(&FF=%02X) stackSize=%08X gp=%s\n",
        en == (w32)entry ? "fn" : cls(en), cls(st), st & 0xFF,
        (w32)ti->stackSize, (w32)ti->gpReg == g_gp ? "gp" : pv((w32)ti->gpReg));
    rec("    init=%02X cur=%02X waitType=%08X waitId=%s wakeup=%d exit=%08X\n",
        (w32)ti->initPriority, (w32)ti->currentPriority, (w32)ti->waitType,
        nm(ti->waitId), ti->wakeupCount, (w32)ti->exitStatus);
    rec("    runClocks=%s intrPreempt=%s threadPreempt=%u release=%u\n",
        (ti->runClocks.low | ti->runClocks.hi) ? "nz" : "0",
        ti->intrPreemptCount ? "nz" : "0",
        (w32)ti->threadPreemptCount, (w32)ti->releaseCount);
}

static int refer(SceUID th, SceKernelThreadInfo *ti) {
    memset(ti, 0, sizeof *ti);
    ti->size = sizeof *ti;
    return sceKernelReferThreadStatus(th, ti);
}

static void rec_refer(const char *label, SceUID th) {
    SceKernelThreadInfo ti;
    int r = refer(th, &ti);
    rec("  %s: refer=%s\n", label, hx(r));
    if (r == 0) rec_thinfo(&ti, (const void *)script_entry);
}

/* Just status, priority and exit status. */
static void rec_brief(const char *label, SceUID th) {
    SceKernelThreadInfo ti;
    int r = refer(th, &ti);
    if (r) { rec("  %s: refer=%s\n", label, hx(r)); return; }
    rec("  %s: status=%08X init=%02X cur=%02X wakeup=%d exit=%08X\n", label,
        (w32)ti.status, (w32)ti.initPriority, (w32)ti.currentPriority,
        ti.wakeupCount, (w32)ti.exitStatus);
}

/* ---- version 3: one waitable object of each kind ----------------------------
 *
 * Each is set up so that a thread asking for it waits: a semaphore at 0, an
 * event flag at 0, an empty mbx, a vpl and an fpl main has taken, an empty
 * pipe, a mutex and an lwmutex main holds, a one-block TLS pool main holds. */

static void *g_vplmain, *g_fplmain;

static void objs_make(void) {
    g_sema = sceKernelCreateSema("o_sema", 0, 0, 1, NULL);            nm_add(g_sema, "sema");
    g_evf  = sceKernelCreateEventFlag("o_evf", 0, 0, NULL);           nm_add(g_evf, "evf");
    g_mbx  = sceKernelCreateMbx("o_mbx", 0, NULL);                    nm_add(g_mbx, "mbx");
    g_vpl  = sceKernelCreateVpl("o_vpl", 2, 0, 0x1000, NULL);         nm_add(g_vpl, "vpl");
    g_vplmain = NULL;
    sceKernelTryAllocateVpl(g_vpl, 0xC00, &g_vplmain);
    g_fpl  = sceKernelCreateFpl("o_fpl", 2, 0, 0x100, 1, NULL);       nm_add(g_fpl, "fpl");
    g_fplmain = NULL;
    sceKernelTryAllocateFpl(g_fpl, &g_fplmain);
    g_mpp  = sceKernelCreateMsgPipe("o_mpp", 2, 0, (void *)0x100, NULL); nm_add(g_mpp, "msgpipe");
    g_mtx  = sceKernelCreateMutex("o_mtx", 0, 0, NULL);               nm_add(g_mtx, "mutex");
    sceKernelLockMutex(g_mtx, 1, NULL);
    memset(&g_lw, 0, sizeof g_lw);
    sceKernelCreateLwMutex(&g_lw, "o_lw", 0, 0, NULL);                nm_add(g_lw.uid, "lwmutex");
    sceKernelLockLwMutex(&g_lw, 1, NULL);
    g_tls  = sceKernelCreateTlspl("o_tls", 2, 0, 0x10, 1, NULL);      nm_add(g_tls, "tlspl");
    sceKernelGetTlsAddr(g_tls);
}

/* Deleting an object releases whoever waits on it. The mutexes and the TLS
 * pool are held by main; if a delete is refused for that, main lets go and
 * the waiter, which takes the object and gives it straight back, runs first. */
static void objs_free(void) {
    sceKernelDeleteSema(g_sema);
    sceKernelDeleteEventFlag(g_evf);
    sceKernelDeleteMbx(g_mbx);
    sceKernelDeleteVpl(g_vpl);
    sceKernelDeleteFpl(g_fpl);
    sceKernelDeleteMsgPipe(g_mpp);
    if (sceKernelDeleteMutex(g_mtx) != 0) {
        sceKernelUnlockMutex(g_mtx, 1);
        sceKernelDelayThread(1000);
        sceKernelDeleteMutex(g_mtx);
    }
    if (sceKernelDeleteLwMutex(&g_lw) != 0) {
        sceKernelUnlockLwMutex(&g_lw, 1);
        sceKernelDelayThread(1000);
        sceKernelDeleteLwMutex(&g_lw);
    }
    if (sceKernelDeleteTlspl(g_tls) != 0) {
        sceKernelFreeTlspl(g_tls);
        sceKernelDelayThread(1000);
        sceKernelDeleteTlspl(g_tls);
    }
}

/* How many threads wait on each object, by its own status call. */
static int wn_sema(void) {
    SceKernelSemaInfo i; i.size = sizeof i;
    return sceKernelReferSemaStatus(g_sema, &i) ? -1 : i.numWaitThreads;
}
static int wn_evf(void) {
    SceKernelEventFlagInfo i; i.size = sizeof i;
    return sceKernelReferEventFlagStatus(g_evf, &i) ? -1 : i.numWaitThreads;
}
static int wn_mbx(void) {
    SceKernelMbxInfo i; i.size = sizeof i;
    return sceKernelReferMbxStatus(g_mbx, &i) ? -1 : i.numWaitThreads;
}
static int wn_vpl(void) {
    SceKernelVplInfo i; i.size = sizeof i;
    return sceKernelReferVplStatus(g_vpl, &i) ? -1 : i.numWaitThreads;
}
static int wn_fpl(void) {
    SceKernelFplInfo i; i.size = sizeof i;
    return sceKernelReferFplStatus(g_fpl, &i) ? -1 : i.numWaitThreads;
}
static int wn_mpp(void) {
    SceKernelMppInfo i; i.size = sizeof i;
    return sceKernelReferMsgPipeStatus(g_mpp, &i) ? -1 : i.numReceiveWaitThreads;
}
static int wn_mtx(void) {
    w32 i[14];
    i[0] = 56;
    return sceKernelReferMutexStatus(g_mtx, i) ? -1 : (int)i[13];
}
static int wn_lw(void) { return g_lw.numWaitThreads; }
static int wn_tls(void) {
    w32 i[16];
    i[0] = 64;
    return sceKernelReferTlsplStatus(g_tls, i) ? -1 : (int)i[14];
}

/* A 0x30 thread runs `ops` (one wait, 1s timeout, tagged with its return);
 * main lets it park, releases it, and then lets it run. */
static void rw_one(const char *label, const op_t *ops, int (*waiters)(void)) {
    seq_clear();
    g_tleft = 0xEEEEEEEE;
    SceUID th = spawn("rw", 0x30, ops);
    sceKernelDelayThread(1000);
    int before = waiters ? waiters() : -1;
    int r = sceKernelReleaseWaitThread(th);
    int after = waiters ? waiters() : -1;
    wait_end(th, 200000);
    rec("  %-9s release=%s waiters %d->%d  %s  timeout-after=%s\n", label, hx(r), before, after,
        g_seqn ? g_seq : "(no tag)",
        g_tleft == 0xEEEEEEEE ? "-" : ta(g_tleft, 1000000));
    reap(th);
}

static const op_t S_RW_SEMA[] = { { O_WAITSEMA, 1000000, "sema" }, { O_END, 0, 0 } };
static const op_t S_RW_EVF[]  = { { O_WAITEF, 1000000, "evf" }, { O_END, 0, 0 } };
static const op_t S_RW_MBX[]  = { { O_RECVMBX, 1000000, "mbx" }, { O_END, 0, 0 } };
static const op_t S_RW_VPL[]  = { { O_ALLOCVPL, 1000000, "vpl" }, { O_END, 0, 0 } };
static const op_t S_RW_FPL[]  = { { O_ALLOCFPL, 1000000, "fpl" }, { O_END, 0, 0 } };
static const op_t S_RW_MPP[]  = { { O_RECVMPP, 1000000, "msgpipe" }, { O_END, 0, 0 } };
static const op_t S_RW_MTX[]  = { { O_LOCKMTX, 1000000, "mutex" }, { O_END, 0, 0 } };
static const op_t S_RW_LW[]   = { { O_LOCKLW, 1000000, "lwmutex" }, { O_END, 0, 0 } };
static const op_t S_RW_TLS[]  = { { O_TLSGET, 1, "tlspl" }, { O_END, 0, 0 } };
static const op_t S_RW_END[]  = { { O_WAITEND, 1, "threadend" }, { O_END, 0, 0 } };
static const op_t S_RW_DELAY[] = { { O_DELAY, 1000000, "delay" }, { O_END, 0, 0 } };

/* ======================================================================= */

static void sec_basics(void) {
    SEC("basics");
    SceKernelThreadInfo ti;

    ST("main thread: GetThreadId, GetThreadCurrentPriority, ReferThreadStatus(0)");
    g_main = sceKernelGetThreadId();
    g_uid[0] = g_main;
    rec("  GetThreadId: %s\n", g_main > 0 ? "valid" : hx(g_main));
    nm_add(g_main, "main");
    g_gp = get_gp();
    /* threadman.c:885 -- a user module's main thread runs at 0x20. */
    rec("  GetThreadCurrentPriority = %s\n", hx(sceKernelGetThreadCurrentPriority()));
    int r = refer(0, &ti);
    rec("  ReferThreadStatus(0) = %s\n", hx(r));
    if (r == 0) rec_thinfo(&ti, NULL);
    SceKernelThreadInfo t2;
    r = refer(g_main, &t2);
    rec("  ReferThreadStatus(own id) = %s, same entry/stack as (0): %s\n", hx(r),
        (t2.entry == ti.entry && t2.stack == ti.stack) ? "yes" : "no");
    g_nperm = g_nnm;

    /* tools/allegrexrecomp/main.c:1097 -- psprecomp's interp calls
     * module_start with $a0 = $a1 = 0, so user_main gets no argument block;
     * a PSP passes the EBOOT's path. Step 85's free stack differs by it. The
     * strings are logged by length and shape, not content: the folder the
     * probe was run from is in the path. */
    ST("main: argc, and each argv string's length, device, file name and place on main's stack");
    rec("  argc=%d\n", g_argc);
    {
        w32 base = (w32)ti.stack, size = (w32)ti.stackSize;
        int total = 0;
        for (int i = 0; i < g_argc && i < 4 && g_argv; i++) {
            const char *s = g_argv[i];
            if (!s) { rec("  argv[%d]=NULL\n", i); continue; }
            int n = (int)strlen(s);
            total += n + 1;
            const char *colon = strchr(s, ':'), *slash = strrchr(s, '/');
            char dev[12] = "-";
            if (colon && colon - s < (int)sizeof dev) { memcpy(dev, s, colon - s); dev[colon - s] = 0; }
            int next = i + 1 < g_argc && g_argv[i + 1] == s + n + 1;
            rec("  argv[%d]: length %d, device \"%s\", file \"%s\", at %s, next string right after: %s\n",
                i, n, dev, slash ? slash + 1 : s, stkrel((w32)s, base, size),
                i + 1 < g_argc ? (next ? "yes" : "no") : "-");
        }
        rec("  strings with their NULs: %d bytes\n", total);
    }
}

/* ======================================================================= */

static void sec_create(void) {
    SEC("create");
    SceKernelThreadInfo ti;
    int r;

    /* threadman.c:282-317 -- create checks nothing but the table slot and the
     * stack allocation; hardware's argument checks are measured here. */
    ST("create: priority 0, 1, 7, 8, 0x20, 0x77, 0x78, 0x7F, 0x80, -1");
    static const int pr[] = { 0, 1, 7, 8, 0x20, 0x77, 0x78, 0x7F, 0x80, -1 };
    for (int i = 0; i < (int)(sizeof pr / sizeof pr[0]); i++) {
        SceUID th = sceKernelCreateThread("prio", script_entry, pr[i], 0x1000, 0, NULL);
        if (th > 0) {
            r = refer(th, &ti);
            if (r != 0) rec("  prio %08X: uid, refer %s\n", (w32)pr[i], hx(r));
            else rec("  prio %08X: uid init=%02X cur=%02X\n", (w32)pr[i],
                     (w32)ti.initPriority, (w32)ti.currentPriority);
            sceKernelDeleteThread(th);
        } else rec("  prio %08X: %s\n", (w32)pr[i], hx(th));
    }

    /* threadman.c:296-302 -- the size is reported verbatim ("stackSize=10000"),
     * with a floor of 0x200 below which it is raised. */
    ST("create: stack size 0, 1, 0x100, 0x1FF, 0x200, 0x201, 0x1001, 0x10000, 0x7FFFFFFF, -1");
    static const w32 ss[] = { 0, 1, 0x100, 0x1FF, 0x200, 0x201, 0x1001, 0x10000,
                              0x7FFFFFFF, 0xFFFFFFFF };
    for (int i = 0; i < (int)(sizeof ss / sizeof ss[0]); i++) {
        SceUID th = sceKernelCreateThread("stk", script_entry, 0x30, (int)ss[i], 0, NULL);
        if (th > 0) {
            r = refer(th, &ti);
            if (r != 0) rec("  size %08X: uid, refer %s\n", ss[i], hx(r));
            else rec("  size %08X: uid stackSize=%08X stack&FF=%02X\n", ss[i],
                     (w32)ti.stackSize, (w32)ti.stack & 0xFF);
            sceKernelDeleteThread(th);
        } else rec("  size %08X: %s\n", ss[i], hx(th));
    }

    /* threadman.c:1118-1126 -- names are reported truncated to 31 characters
     * and NUL-terminated. */
    ST("create: names of length 0, 1, 31, 32 and 40, and a duplicate name");
    static const char *const names[] = {
        "", "a", "abcdefghijklmnopqrstuvwxyz01234",
        "abcdefghijklmnopqrstuvwxyz012345",
        "abcdefghijklmnopqrstuvwxyz0123456789ABCD",
    };
    for (int i = 0; i < 5; i++) {
        SceUID th = sceKernelCreateThread(names[i], script_entry, 0x30, 0x1000, 0, NULL);
        if (th <= 0) { rec("  len %d: %s\n", (int)strlen(names[i]), hx(th)); continue; }
        refer(th, &ti);
        const w32 *nw = (const w32 *)ti.name;
        rec("  len %d: uid name words %08X %08X .. %08X %08X\n", (int)strlen(names[i]),
            nw[0], nw[1], nw[6], nw[7]);
        sceKernelDeleteThread(th);
    }
    {
        SceUID a = sceKernelCreateThread("dup", script_entry, 0x30, 0x1000, 0, NULL);
        SceUID b = sceKernelCreateThread("dup", script_entry, 0x30, 0x1000, 0, NULL);
        rec("  duplicate: %s %s\n", cu(a), cu(b));
        if (a > 0) sceKernelDeleteThread(a);
        if (b > 0) sceKernelDeleteThread(b);
    }

    /* threadman.c:1764-1768 -- the attribute reads back ORed with 0x800000FF:
     * 0 -> 800000ff, 0x700000 -> 807000ff. */
    ST("create: attribute 0, 0x700000, then each single bit (ret / reported attr)");
    {
        static const w32 at[] = { 0, 0x00700000 };
        for (int i = 0; i < 2; i++) {
            SceUID th = sceKernelCreateThread("attr", script_entry, 0x30, 0x1000, at[i], NULL);
            if (th > 0) { refer(th, &ti); rec("  attr %08X: uid attr=%08X\n", at[i], (w32)ti.attr); sceKernelDeleteThread(th); }
            else rec("  attr %08X: %s\n", at[i], hx(th));
        }
        char line[160];
        int n = 0;
        for (int b = 0; b < 32; b++) {
            SceUID th = sceKernelCreateThread("attr", script_entry, 0x30, 0x1000, 1u << b, NULL);
            w32 a = 0;
            if (th > 0) { refer(th, &ti); a = ti.attr; sceKernelDeleteThread(th); }
            n += snprintf(line + n, sizeof line - n, " b%02d:%s", b, th > 0 ? hx((int)a) : hx(th));
            if ((b & 3) == 3) { rec(" %s\n", line); n = 0; }
        }
    }

    ST("create: entry NULL");
    {
        SceUID th = sceKernelCreateThread("noentry", NULL, 0x30, 0x1000, 0, NULL);
        rec("  entry NULL: %s\n", cu(th));
        if (th > 0) sceKernelDeleteThread(th);
    }

    /* threadman.c:39 -- psprecomp's table holds 128 threads in all. */
    ST("create: 200 dormant threads with 0x200-byte stacks (how many succeed)");
    {
        static SceUID many[200];
        int ok = 0, first_fail = 0;
        for (int i = 0; i < 200; i++) {
            many[i] = sceKernelCreateThread("many", script_entry, 0x30, 0x200, 0, NULL);
            if (many[i] > 0) ok++;
            else if (!first_fail) first_fail = many[i];
        }
        int del_fail = 0;
        for (int i = 0; i < 200; i++)
            if (many[i] > 0 && sceKernelDeleteThread(many[i]) != 0) del_fail++;
        rec("  created %d of 200, first failure %s, delete failures %d\n",
            ok, hx(first_fail), del_fail);
    }

    /* threadman.c:343-351 -- the name first (step 150), then priority, stack
     * size and attribute, in the order the single-argument steps were written;
     * which answer wins when several are wrong is unmeasured. A kernel-space
     * entry is not checked at all. Never started, deleted at once. */
    if (!ST("create: check order with two or more bad arguments (NULL name, priority 0, stack 0x100, attr 0x100, entry 0x88000000)")) {
        static const struct { const char *l; int noname; int prio; int size; w32 attr; w32 entry; } cs[] = {
            { "name+prio",      1, 0,    0x1000, 0,     0 },
            { "name+size",      1, 0x30, 0x100,  0,     0 },
            { "name+attr",      1, 0x30, 0x1000, 0x100, 0 },
            { "prio+size",      0, 0,    0x100,  0,     0 },
            { "prio+attr",      0, 0,    0x1000, 0x100, 0 },
            { "size+attr",      0, 0x30, 0x100,  0x100, 0 },
            { "prio+size+attr", 0, 0,    0x100,  0x100, 0 },
            { "kentry",         0, 0x30, 0x1000, 0,     0x88000000 },
            { "kentry+prio",    0, 0,    0x1000, 0,     0x88000000 },
            { "name+kentry",    1, 0x30, 0x1000, 0,     0x88000000 },
        };
        for (int i = 0; i < (int)(sizeof cs / sizeof cs[0]); i++) {
            SceKernelThreadEntry e = cs[i].entry ? (SceKernelThreadEntry)cs[i].entry : script_entry;
            SceUID th = sceKernelCreateThread(cs[i].noname ? NULL : "order", e, cs[i].prio,
                                              cs[i].size, cs[i].attr, NULL);
            rec("  %-15s %s\n", cs[i].l, cu(th));
            if (th > 0) sceKernelDeleteThread(th);
        }
    }
}

/* ======================================================================= */

static unsigned char g_rb1[256] __attribute__((aligned(16)));
static unsigned char g_rb2[256] __attribute__((aligned(16)));

/* sceKernelReferThreadStatus with the size field set to `room`, twice with
 * two different fills, to see exactly how far it wrote. */
static void refer_room(SceUID th, int room) {
    memset(g_rb1, 0xEE, sizeof g_rb1);
    memset(g_rb2, 0x11, sizeof g_rb2);
    memcpy(g_rb1, &room, 4);
    memcpy(g_rb2, &room, 4);
    int r1 = sceKernelReferThreadStatus(th, (SceKernelThreadInfo *)g_rb1);
    int r2 = sceKernelReferThreadStatus(th, (SceKernelThreadInfo *)g_rb2);
    unsigned char pre[4];
    memcpy(pre, &room, 4);
    int ext = 0;
    for (int i = 0; i < 256; i++) {
        unsigned char e1 = i < 4 ? pre[i] : 0xEE, e2 = i < 4 ? pre[i] : 0x11;
        if (g_rb1[i] != e1 || g_rb2[i] != e2) ext = i + 1;
    }
    w32 sz, ex;
    memcpy(&sz, g_rb1, 4);
    memcpy(&ex, g_rb1 + 80, 4);
    rec("  room %3d: ret=%s/%s written-to=%d size=%08X word80=%08X\n",
        room, hx(r1), hx(r2), ext, sz, ex);
}

static const op_t S_SLEEP42[] = { { O_SLEEP, 0, "slept" }, { O_END, 0x42, 0 } };
static const op_t S_DELAY1S[] = { { O_DELAY, 1000000, "delayed" }, { O_END, 0, 0 } };
static const op_t S_WAITSEMA[] = { { O_WAITSEMA, 10000000, "semawoke" }, { O_END, 0, 0 } };
static const op_t S_WAITEF[] = { { O_WAITEF, 10000000, "efwoke" }, { O_END, 0, 0 } };
static const op_t S_WAITEND1[] = { { O_WAITEND, 1, "endwoke" }, { O_END, 0, 0 } };
static const op_t S_REFERSELF[] = { { O_REFERSELF, 0, 0 }, { O_END, 0, 0 } };
static const op_t S_RET42[] = { { O_END, 0x42, 0 } };

static void sec_refer(void) {
    SEC("refer");
    nm_reset();

    /* threadman.c:1750-1756, 1826-1834 -- the structure is 104 bytes, and only
     * as many bytes as the size field allows are written: exit word (offset
     * 80) untouched at 80, half written at 82, whole at 104+. */
    ST("ReferThreadStatus on a dormant thread with size field 0, 1, 4, 5, 80, 82, 84, 104, 108, 200");
    SceUID th = mk("rf_size", 0x30);
    static const int rooms[] = { 0, 1, 4, 5, 80, 82, 84, 104, 108, 200 };
    for (int i = 0; i < 10; i++) refer_room(th, rooms[i]);
    sceKernelDeleteThread(th);

    /* threadman.c:1770-1797 -- status is the kernel's enumeration: a created
     * thread reports 16, a sleeping one 4 ("after start status=00000004").
     * threadman.c:1799-1806 -- exitStatus is DORMANT (800201a2) before any
     * start and NOT_DORMANT (800201a4) while alive. threadman.c:1818 --
     * psprecomp writes waitType and waitId as 0. */
    ST("ReferThreadStatus: every field of a thread asleep in SleepThread");
    th = spawn("rf_sleep", 0x30, S_SLEEP42);
    sceKernelDelayThread(1000);
    rec_refer("sleeping", th);
    sceKernelWakeupThread(th);
    wait_end(th, 200000);
    rec_refer("after it returned 0x42", th);
    reap(th);

    ST("ReferThreadStatus: a thread in DelayThread(1s)");
    th = spawn("rf_delay", 0x30, S_DELAY1S);
    sceKernelDelayThread(1000);
    rec_refer("delaying", th);
    reap(th);

    ST("ReferThreadStatus: a thread in WaitSema, and one in WaitEventFlag");
    g_sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    nm_add(g_sema, "sema");
    g_evf = sceKernelCreateEventFlag("evf", 0, 0, NULL);
    nm_add(g_evf, "evf");
    th = spawn("rf_sema", 0x30, S_WAITSEMA);
    SceUID th2 = spawn("rf_evf", 0x30, S_WAITEF);
    sceKernelDelayThread(1000);
    rec_refer("waiting on sema", th);
    rec_refer("waiting on event flag", th2);
    reap(th); reap(th2);
    sceKernelDeleteSema(g_sema);
    sceKernelDeleteEventFlag(g_evf);

    ST("ReferThreadStatus: a thread in WaitThreadEnd on a sleeping thread");
    SceUID target = spawn("rf_target", 0x30, S_SLEEP42);
    g_uid[1] = target;
    th = spawn("rf_waitend", 0x30, S_WAITEND1);
    sceKernelDelayThread(1000);
    rec_refer("waiting for thread end", th);
    reap(th); reap(target);

    ST("ReferThreadStatus(0) from inside a running 0x10 thread");
    th = spawn("rf_self", 0x10, S_REFERSELF);
    rec("  refer(0) from itself = %s\n", hx(g_wref_ret));
    if (g_wref_ret == 0) rec_thinfo(&g_wref, (const void *)script_entry);
    reap(th);

    ST("ReferThreadStatus: unknown id (a deleted thread)");
    th = mk("rf_gone", 0x30);
    sceKernelDeleteThread(th);
    SceKernelThreadInfo ti;
    rec("  deleted: %s\n", hx(refer(th, &ti)));
    (void)S_RET42;

    /* threadman.c:2184-2190 -- psprecomp reports a waitType only for the
     * waits threadman.c owns (sleep 1, delay 2, sema 3, evf 4, thread end 9:
     * steps 9-12); the object waits in kernobj.c and kernlock.c report 0.
     * Deleting each object then releases its waiter (the seq line). */
    ST("ReferThreadStatus: waitType and waitId of threads waiting on a mbx, vpl, fpl, msgpipe, mutex, lwmutex and TLS pool");
    fresh(); seq_clear();
    objs_make();
    {
        static const op_t *const sc[] = { S_RW_MBX, S_RW_VPL, S_RW_FPL, S_RW_MPP, S_RW_MTX, S_RW_LW, S_RW_TLS };
        static const char *const tn[] = { "w_mbx", "w_vpl", "w_fpl", "w_msgpipe", "w_mutex", "w_lwmutex", "w_tlspl" };
        SceUID w[7];
        for (int i = 0; i < 7; i++) w[i] = spawn(tn[i], 0x30, sc[i]);
        sceKernelDelayThread(1000);
        for (int i = 0; i < 7; i++) {
            int rr = refer(w[i], &ti);
            rec("  %-10s refer=%s status=%08X waitType=%08X waitId=%s\n", tn[i], hx(rr),
                (w32)ti.status, (w32)ti.waitType, nm(ti.waitId));
        }
        objs_free();
        for (int i = 0; i < 7; i++) wait_end(w[i], 200000);
        rec_seq();
        for (int i = 0; i < 7; i++) reap(w[i]);
    }
}

/* ======================================================================= */

/* regs_body runs on the thread regs_entry started. It records what the thread
 * can see of itself, then writes through its argument pointer to find out
 * whether the block it was handed is a copy. */
static SceKernelThreadInfo g_rb_info;
static int g_rb_refer, g_rb_uid, g_rb_hasarg;
static w32 g_rb_argw[2], g_rb_base0, g_rb_base1, g_rb_mid, g_rb_top[16];

int regs_body(SceSize len, void *argp) {
    g_rb_uid = sceKernelGetThreadId();
    memset(&g_rb_info, 0, sizeof g_rb_info);
    g_rb_info.size = sizeof g_rb_info;
    g_rb_refer = sceKernelReferThreadStatus(0, &g_rb_info);
    if (g_rb_refer == 0 && g_rb_info.stack) {
        volatile w32 *base = (volatile w32 *)g_rb_info.stack;
        w32 words = (w32)g_rb_info.stackSize / 4;
        g_rb_base0 = base[0];
        g_rb_base1 = base[1];
        g_rb_mid   = base[0x40];
        for (int i = 0; i < 16; i++) g_rb_top[i] = base[words - 16 + i];
    }
    g_rb_hasarg = argp != NULL;
    if (argp) {
        volatile w32 *a = (volatile w32 *)argp;
        g_rb_argw[0] = a[0];
        g_rb_argw[1] = a[1];
        if (len >= 4) a[0] = 3;
    }
    return 0x77;
}

static w32 g_argbuf[0x200] __attribute__((aligned(16)));

static void regs_clear(void) {
    memset(g_entry_regs, 0xEE, sizeof g_entry_regs);
    memset(&g_rb_info, 0, sizeof g_rb_info);
    g_rb_refer = (int)0xEEEEEEEE;
    g_rb_uid = 0; g_rb_hasarg = -1;
    g_rb_argw[0] = g_rb_argw[1] = 0xEEEEEEEE;
    g_rb_base0 = g_rb_base1 = g_rb_mid = 0xEEEEEEEE;
    memset(g_rb_top, 0xEE, sizeof g_rb_top);
}

static const char *stkword(w32 v, w32 base, w32 size) {
    if (g_rb_uid && v == (w32)g_rb_uid) return "uid";
    if (v == base) return "base";
    if (v > base && v < base + size) { char *b = sb(); snprintf(b, 40, "stk+%X", v - base); return b; }
    return pv(v);
}

static void rec_stackwords(void) {
    w32 base = (w32)g_rb_info.stack, size = (w32)g_rb_info.stackSize;
    rec("    stack[0]=%s stack[1]=%s stack[+0x100]=%s\n",
        stkword(g_rb_base0, base, size), stkword(g_rb_base1, base, size),
        stkword(g_rb_mid, base, size));
    for (int i = 0; i < 16; i += 4)
        rec("    top-%02X: %s %s %s %s\n", (16 - i) * 4,
            stkword(g_rb_top[i], base, size), stkword(g_rb_top[i + 1], base, size),
            stkword(g_rb_top[i + 2], base, size), stkword(g_rb_top[i + 3], base, size));
}

/* Start a regs_entry thread (0x10, so it runs inside the start call) with the
 * given argument block and report what it saw. */
static void regs_case(SceSize len, void *argp, SceUInt attr, int full) {
    regs_clear();
    if (argp == (void *)g_argbuf) { g_argbuf[0] = 0x11111111; g_argbuf[1] = 0x22222222; }
    SceUID th = sceKernelCreateThread("regs", regs_entry, 0x10, 0x1000, attr, NULL);
    nm_add(th, "regs");
    int r = sceKernelStartThread(th, len, argp);
    if (r == 0) g_starts++;
    int e = wait_end(th, 200000);
    rec("  start=%s end=%s\n", hx(r), hx(e));
    if (r == 0 && g_rb_refer == 0) {
        w32 base = (w32)g_rb_info.stack, size = (w32)g_rb_info.stackSize;
        const w32 *g = g_entry_regs;
        /* The words behind a bad pointer are whatever lies there, so they
         * are only logged for the probe's own buffers. */
        char aw[40];
        if (argp && (w32)argp < 0x10000)
            snprintf(aw, sizeof aw, "(from a bad pointer, not logged)");
        else
            snprintf(aw, sizeof aw, "%08X %08X", g_rb_argw[0], g_rb_argw[1]);
        rec("    a0=%08X a1=%s argwords=%s mainbuf[0] now %08X\n",
            g[4], g[5] == (w32)argp && argp ? "caller's buffer" : stkrel(g[5], base, size),
            aw, g_argbuf[0]);
        rec("    sp=%s k0=%s gp=%s ra=%s\n", stkrel(g[29], base, size),
            stkrel(g[26], base, size), g[28] == g_gp ? "main's" : pv(g[28]),
            cls(g[31]));
        if (full) {
            w32 mask = 0;
            for (int i = 1; i < 32; i++) {
                if (i == 4 || i == 5 || i == 26 || i == 28 || i == 29 || i == 31) continue;
                if (g[i]) mask |= 1u << i;
            }
            rec("    nonzero GPRs (excluding a0 a1 k0 gp sp ra): mask %08X; k1=%s hi=%s lo=%s\n",
                mask, cls(g[27]), g[32] ? "nz" : "0", g[33] ? "nz" : "0");
            rec("    fcr31=%08X\n", g[34]);
            for (int i = 0; i < 32; i += 8)
                rec("    f%02d..: %08X %08X %08X %08X %08X %08X %08X %08X\n", i,
                    g[35 + i], g[36 + i], g[37 + i], g[38 + i],
                    g[39 + i], g[40 + i], g[41 + i], g[42 + i]);
            rec_stackwords();
        }
    } else if (r == 0) {
        rec("    thread did not report (refer=%s)\n", hx(g_rb_refer));
    } else {
        SceKernelThreadInfo ti;
        if (refer(th, &ti) == 0) rec("    after the refused start: status=%08X\n", (w32)ti.status);
    }
    reap(th);
}

static const op_t S_TAGW[] = { { O_TAG, 0, "W" }, { O_END, 0, 0 } };
static const op_t S_RUN3[] = { { O_TAG, 0, "run" }, { O_END, 3, 0 } };
static const op_t S_SELFOPS[] = { { O_SELFOPS, 0, 0 }, { O_END, 0, 0 } };

static void sec_start(void) {
    SEC("start");
    nm_reset();
    int r;

    /* threadman.c:376-388 -- 0 answers 80020197, an id naming nothing
     * 80020198, a thread already running 800201a4. */
    ST("StartThread: id 0, a deleted thread's id, a thread already started");
    rec("  id 0: %s\n", hx(sceKernelStartThread(0, 0, NULL)));
    SceUID th = mk("st_gone", 0x30);
    sceKernelDeleteThread(th);
    rec("  deleted: %s\n", hx(sceKernelStartThread(th, 0, NULL)));
    th = spawn("st_twice", 0x30, S_TAGW);
    rec("  already started (ready): %s\n", hx(sceKernelStartThread(th, 0, NULL)));
    reap(th);

    /* threadman.c:477-493 and tools/allegrexrecomp/interp.c:765 -- starting an
     * equal or less urgent thread does not run it; sched.c:462-478 -- starting
     * a more urgent one switches to it inside the call. */
    ST("StartThread ordering: a 0x20 thread started by main (0x20)");
    fresh(); seq_clear();
    th = mk("st_eq", 0x20);
    tag("m1"); r = go(th, S_TAGW); tag("m2");
    sceKernelDelayThread(1000); tag("m3");
    wait_end(th, 200000); reap(th);
    rec("  start=%s\n", hx(r)); rec_seq();

    ST("StartThread ordering: a 0x30 thread started by main (0x20)");
    fresh(); seq_clear();
    th = mk("st_lo", 0x30);
    tag("m1"); r = go(th, S_TAGW); tag("m2");
    sceKernelDelayThread(1000); tag("m3");
    wait_end(th, 200000); reap(th);
    rec("  start=%s\n", hx(r)); rec_seq();

    ST("StartThread ordering: a 0x10 thread started by main (0x20)");
    fresh(); seq_clear();
    th = mk("st_hi", 0x10);
    tag("m1"); r = go(th, S_TAGW); tag("m2");
    wait_end(th, 200000); reap(th);
    rec("  start=%s\n", hx(r)); rec_seq();

    /* threadman.c:451-475 -- the block is copied to top - 0x100 -
     * roundup(len, 16) (1..8 bytes -> +0xEF0 of a 0x1000 stack, 80 -> +0xEB0,
     * 90 -> +0xEA0, 0x600 -> +0x900) and $sp starts below the copy.
     * threadman.c:327-333 -- the thread gets the copy; writing through it
     * leaves the caller's variable alone (the note at threadman.c:335-346
     * says psprecomp passes the original; the code at 462-472 copies).
     * sched.c:364-381 -- $gp is the starter's, $ra is 0 in psprecomp, and
     * sched.c:375 -- a fresh thread's float registers are NaN.
     * sched.h:75-80 -- $k0 addresses the control block at top - 0x100.
     * threadman.c:415-427 -- stack[0] and top-0x40 hold the uid, top-0x38 the
     * stack base, the last two words 0xFFFFFFFF; the rest is filled 0xFF. */
    ST("start args: 8 bytes; full register file at entry, stack words");
    regs_case(8, g_argbuf, 0, 1);

    /* threadman.c:329-331 -- one byte of 0x4567 reads back as 0xFFFFFF67. */
    ST("start args: 1 byte of a word holding 0x4567");
    g_argbuf[0] = 0x4567;
    {
        static w32 one = 0x4567;
        regs_case(1, &one, 0, 0);
        rec("    caller's word now %08X\n", one);
    }
    ST("start args: 80 bytes");
    regs_case(80, g_argbuf, 0, 0);
    ST("start args: 90 bytes");
    regs_case(90, g_argbuf, 0, 0);
    ST("start args: 0x600 bytes");
    regs_case(0x600, g_argbuf, 0, 0);

    /* threadman.c:323-326 -- a zero length with a pointer arrives as a NULL
     * pointer. (NULL with a length, and a length of -1, are in the last
     * section with the other bad arguments.) */
    ST("start args: length 0 with a real pointer");
    regs_case(0, g_argbuf, 0, 0);

    /* threadman.c:410-414 -- PSP_THREAD_ATTR_NO_FILLSTACK leaves the stack as
     * it was (the test sees its own 0xCC scribble), but the kernel's own words
     * at the top are still written. */
    ST("stack: NO_FILLSTACK thread whose stack main scribbled with 0xCC before starting it");
    regs_clear();
    th = sceKernelCreateThread("nofill", regs_entry, 0x10, 0x1000, ATTR_NO_FILLSTACK, NULL);
    {
        SceKernelThreadInfo ti;
        r = refer(th, &ti);
        if (r == 0 && ti.stack) memset(ti.stack, 0xCC, ti.stackSize);
        r = sceKernelStartThread(th, 0, NULL);
        if (r == 0) g_starts++;
        wait_end(th, 200000);
        rec("  create=%s start=%s\n", cu(th), hx(r));
        if (r == 0 && g_rb_refer == 0) rec_stackwords();
        reap(th);
    }

    /* threadman.c:266-280 -- CLEAR_STACK zeroes the stack when the thread is
     * deleted; without it the memory is left as it was. */
    ST("stack: marker written into a finished thread's stack, then the thread deleted (CLEAR_STACK and not)");
    for (int k = 0; k < 2; k++) {
        SceUInt attr = k ? ATTR_CLEAR_STACK : 0;
        th = mk("clr", 0x10);
        sceKernelDeleteThread(th);
        th = sceKernelCreateThread("clr", script_entry, 0x10, 0x1000, attr, NULL);
        r = go(th, S_RET42);
        wait_end(th, 200000);
        SceKernelThreadInfo ti;
        int rr = refer(th, &ti);
        volatile w32 *base = (volatile w32 *)ti.stack;
        if (rr == 0 && base) base[8] = 0x12345678;
        int d = sceKernelDeleteThread(th);
        rec("  attr %08X: create=%s start=%s delete=%s marker after delete=%s\n", (w32)attr,
            cu(th), hx(r), hx(d), (rr == 0 && base) ? hx((int)base[8]) : "n/a");
    }

    /* threadman.c:303-310 -- LOW_STACK takes the stack from the low end, so it
     * lands below a block allocated PSP_SMEM_Low just before. */
    ST("stack: LOW_STACK attribute versus a PSP_SMEM_Low block");
    {
        SceUID blk = sceKernelAllocPartitionMemory(2, "lowmark", PSP_SMEM_Low, 0x100, NULL);
        w32 L = blk > 0 ? (w32)sceKernelGetBlockHeadAddr(blk) : 0;
        SceUID a = sceKernelCreateThread("hi", script_entry, 0x30, 0x1000, 0, NULL);
        SceUID b = sceKernelCreateThread("lo", script_entry, 0x30, 0x1000, ATTR_LOW_STACK, NULL);
        SceKernelThreadInfo ta, tb;
        int ra = refer(a, &ta), rb = refer(b, &tb);
        rec("  block=%s default=%s low=%s\n", cu(blk), cu(a), cu(b));
        if (L && ra == 0 && rb == 0)
            rec("  default stack above block: %s; low stack above block: %s; low below default: %s\n",
                (w32)ta.stack > L ? "yes" : "no", (w32)tb.stack > L ? "yes" : "no",
                (w32)tb.stack < (w32)ta.stack ? "yes" : "no");
        if (a > 0) sceKernelDeleteThread(a);
        if (b > 0) sceKernelDeleteThread(b);
        if (blk > 0) sceKernelFreePartitionMemory(blk);
    }

    /* threadman.c:494-507 -- a finished thread is dormant and can be started
     * again; its second run's exit status replaces the first. */
    ST("StartThread: restart a finished thread, and restart a terminated one");
    fresh(); seq_clear();
    th = spawn("rst", 0x30, S_RUN3);
    int e1 = wait_end(th, 200000);
    r = go(th, S_RUN3);
    int e2 = wait_end(th, 200000);
    SceUID th2 = spawn("rst2", 0x30, S_RUN3);
    int t = sceKernelTerminateThread(th2);
    int r2 = go(th2, S_RUN3);
    int e3 = wait_end(th2, 200000);
    rec("  finished: end=%s restart=%s end=%s\n", hx(e1), hx(r), hx(e2));
    rec("  never ran, terminated: terminate=%s restart=%s end=%s\n", hx(t), hx(r2), hx(e3));
    rec_seq();
    reap(th); reap(th2);

    /* threadman.c:382-388 ("Current: 800201a4"), 548-564, 715-756, 904-961 --
     * the operations a thread may not apply to itself, by 0 and by its own id. */
    ST("self: a 0x10 thread applies delete/wait/start/resume/exit-status/wakeup/priority/suspend/terminate to 0 and to itself");
    for (int i = 0; i < 32; i++) g_selfres[i] = (int)0xEEEEEEEE;
    th = spawn("selfops", 0x10, S_SELFOPS);
    wait_end(th, 200000);
    {
        static const char *const lab[] = {
            "own id valid", "DeleteThread(0)", "DeleteThread(self)",
            "WaitThreadEnd(0)", "WaitThreadEnd(self)", "StartThread(self)",
            "ResumeThread(0)", "ResumeThread(self)", "GetThreadExitStatus(0)",
            "GetThreadExitStatus(self)", "WakeupThread(0)", "CancelWakeupThread(0)",
            "WakeupThread(self)", "CancelWakeupThread(self)",
            "ChangeThreadPriority(0,0x11)", "  current priority",
            "ChangeThreadPriority(self,0x10)", "  current priority",
            "SuspendThread(0)", "SuspendThread(self)", "TerminateThread(0)",
            "TerminateThread(self)", "TerminateDeleteThread(0)",
            "TerminateDeleteThread(self)", "reached the end",
        };
        for (int i = 0; i < 25; i++) rec("  %-28s %s\n", lab[i], hx(g_selfres[i]));
    }
    reap(th);
}

/* ======================================================================= */

/* The state matrix: the same calls applied to one thread in each state. */
static void matrix(SceUID th, int live, int susp) {
    SceKernelThreadInfo ti;
    int r, r2, r3;
    SceUInt t;

    r = refer(th, &ti);
    rec("  refer=%s\n", hx(r));
    if (r == 0) rec_thinfo(&ti, (const void *)script_entry);
    rec("  GetThreadExitStatus=%s\n", hx(sceKernelGetThreadExitStatus(th)));
    if (!live) {
        t = 0;
        r = sceKernelWaitThreadEnd(th, &t);
        rec("  WaitThreadEnd(timeout 0)=%s timeout-after=%s\n", hx(r), ta(t, 0));
    }
    r = sceKernelChangeThreadPriority(th, 0x31);
    r2 = refer(th, &ti) == 0 ? (int)ti.currentPriority : -1;
    r3 = r == 0 ? sceKernelChangeThreadPriority(th, 0x30) : 0;
    rec("  ChangeThreadPriority(0x31)=%s cur=%02X restore=%s\n", hx(r), (w32)r2, hx(r3));
    r = sceKernelWakeupThread(th);
    r2 = sceKernelCancelWakeupThread(th);
    rec("  WakeupThread=%s then CancelWakeupThread=%s\n", hx(r), hx(r2));
    r = sceKernelSuspendThread(th);
    r2 = sceKernelResumeThread(th);
    r3 = susp ? sceKernelSuspendThread(th) : sceKernelResumeThread(th);
    rec("  SuspendThread=%s ResumeThread=%s %s=%s\n", hx(r), hx(r2),
        susp ? "SuspendThread again" : "ResumeThread again", hx(r3));
    if (live) rec("  StartThread=%s\n", hx(sceKernelStartThread(th, 0, NULL)));
    rec("  DeleteThread=%s\n", hx(sceKernelDeleteThread(th)));
    r = sceKernelTerminateThread(th);
    rec("  TerminateThread=%s\n", hx(r));
    if (r == 0) {
        r = refer(th, &ti);
        rec("    after terminate: refer=%s status=%08X exit=%08X GetThreadExitStatus=%s\n",
            hx(r), (w32)ti.status, (w32)ti.exitStatus, hx(sceKernelGetThreadExitStatus(th)));
    }
    rec("  TerminateDeleteThread=%s\n", hx(sceKernelTerminateDeleteThread(th)));
    rec("  then refer=%s\n", hx(refer(th, &ti)));
}

static const op_t S_M_SLEEP[] = { { O_SLEEP, 0, 0 }, { O_END, 0, 0 } };

static void sec_states(void) {
    SEC("thread states x calls");
    nm_reset();
    g_sema = sceKernelCreateSema("msema", 0, 0, 1, NULL);
    nm_add(g_sema, "sema");

    /* threadman.c:534-564 (delete refuses anything alive: 800201a4),
     * 715-756 (suspend: Created/Finished 800201a2, Ready 0, Suspended
     * 800201a3; resume anything not suspended 800201a5), 758-770 (priority:
     * DORMANT for Created/Finished, 0 for Ready/Suspended/Waiting), 900-961
     * (terminate: 800201a2 for dormant; terminate-delete accepts dormant),
     * 1964-1978 (exit status), 628-632 (WaitThreadEnd on a never-started
     * thread: DORMANT). */
    ST("states: created (never started)");
    SceUID c = mk("m_created", 0x30);
    matrix(c, 0, 0);
    ST("states: deleted (the id of the thread above)");
    matrix(c, 0, 0);
    ST("states: created, TerminateDeleteThread directly");
    c = mk("m_created2", 0x30);
    rec("  TerminateDeleteThread=%s\n", hx(sceKernelTerminateDeleteThread(c)));

    ST("states: ready (0x30, started, has not run)");
    SceUID th = spawn("m_ready", 0x30, S_M_SLEEP);
    matrix(th, 1, 0);
    reap(th);

    ST("states: waiting (in WaitSema with a 10s timeout)");
    th = spawn("m_wait", 0x30, S_WAITSEMA);
    sceKernelDelayThread(1000);
    matrix(th, 1, 0);
    reap(th);

    ST("states: suspended (started, then suspended before it ran)");
    th = spawn("m_susp", 0x30, S_M_SLEEP);
    rec("  SuspendThread=%s\n", hx(sceKernelSuspendThread(th)));
    matrix(th, 1, 1);
    reap(th);

    ST("states: waiting and suspended");
    th = spawn("m_wsusp", 0x30, S_WAITSEMA);
    sceKernelDelayThread(1000);
    rec("  SuspendThread=%s\n", hx(sceKernelSuspendThread(th)));
    matrix(th, 1, 1);
    reap(th);

    ST("states: finished (returned 0x42)");
    th = spawn("m_done", 0x30, S_RET42);
    wait_end(th, 200000);
    matrix(th, 0, 0);
    ST("states: finished, TerminateDeleteThread directly");
    th = spawn("m_done2", 0x30, S_RET42);
    wait_end(th, 200000);
    rec("  TerminateDeleteThread=%s\n", hx(sceKernelTerminateDeleteThread(th)));

    ST("states: TerminateDeleteThread straight on ready, waiting, suspended, waiting+suspended");
    {
        SceKernelThreadInfo ti;
        SceUID a = spawn("td_ready", 0x30, S_M_SLEEP);
        rec("  ready: %s", hx(sceKernelTerminateDeleteThread(a)));
        rec(" refer after=%s\n", hx(refer(a, &ti)));
        SceUID b = spawn("td_wait", 0x30, S_WAITSEMA);
        SceUID s = spawn("td_susp", 0x30, S_M_SLEEP);
        SceUID ws = spawn("td_wsusp", 0x30, S_WAITSEMA);
        sceKernelSuspendThread(s);
        sceKernelDelayThread(1000);
        sceKernelSuspendThread(ws);
        rec("  waiting: %s\n", hx(sceKernelTerminateDeleteThread(b)));
        rec("  suspended: %s\n", hx(sceKernelTerminateDeleteThread(s)));
        rec("  waiting+suspended: %s\n", hx(sceKernelTerminateDeleteThread(ws)));
        reap(a); reap(b); reap(s); reap(ws);
    }
    sceKernelDeleteSema(g_sema);

    /* threadman.c:1964-1978 on the main thread, and id 0 from main. */
    ST("states: GetThreadExitStatus and ReferThreadStatus on main (running), and on id 0");
    rec("  GetThreadExitStatus(main)=%s (0)=%s\n",
        hx(sceKernelGetThreadExitStatus(g_main)), hx(sceKernelGetThreadExitStatus(0)));
    {
        SceKernelThreadInfo ti;
        int r = refer(g_main, &ti);
        rec("  refer(main)=%s status=%08X exit=%08X\n", hx(r), (w32)ti.status, (w32)ti.exitStatus);
    }
}

/* ======================================================================= */

static const op_t S_RET5[] = { { O_END, 5, 0 } };
static const op_t S_EXIT1234[] = { { O_EXIT, 0x1234, "exit-returned" }, { O_END, 6, 0 } };
static const op_t S_EXITNEG[] = { { O_EXIT, -5, "exit-returned" }, { O_END, 6, 0 } };
static const op_t S_EXITDEL[] = { { O_EXITDEL, 7, "exitdelete-returned" }, { O_END, 8, 0 } };
static const op_t S_D2000R9[] = { { O_DELAY, 2000, 0 }, { O_END, 9, 0 } };
static const op_t S_TW_SLEEP[] = { { O_TAG, 0, "W" }, { O_SLEEP, 0, "Ws" }, { O_END, 0, 0 } };
static const op_t S_H_WAITEND1[] = { { O_WAITEND, 1, "H" }, { O_END, 0, 0 } };

static void sec_exit(void) {
    SEC("exit and WaitThreadEnd");
    nm_reset();
    SceUID th;
    int r;
    SceUInt t;

    /* threadman.c:583-612 -- returning from the entry leaves $v0 as the exit
     * status, and WaitThreadEnd returns it ("Already ended: 00000005"). */
    ST("exit: entry returns 5; WaitThreadEnd, GetThreadExitStatus, refer");
    th = spawn("ex_ret", 0x30, S_RET5);
    r = wait_end(th, 200000);
    rec("  WaitThreadEnd=%s GetThreadExitStatus=%s\n", hx(r), hx(sceKernelGetThreadExitStatus(th)));
    rec_brief("after", th);
    reap(th);

    ST("exit: ExitThread(0x1234)");
    seq_clear();
    th = spawn("ex_1234", 0x30, S_EXIT1234);
    r = wait_end(th, 200000);
    rec("  WaitThreadEnd=%s GetThreadExitStatus=%s\n", hx(r), hx(sceKernelGetThreadExitStatus(th)));
    rec_seq();
    reap(th);

    ST("exit: ExitThread(-5)");
    seq_clear();
    th = spawn("ex_neg", 0x30, S_EXITNEG);
    r = wait_end(th, 200000);
    rec("  WaitThreadEnd=%s GetThreadExitStatus=%s\n", hx(r), hx(sceKernelGetThreadExitStatus(th)));
    rec_seq();
    reap(th);

    /* Not implemented in psprecomp: an unimplemented call returns 0 and the
     * thread carries on, which the seq line shows. */
    ST("exit: ExitDeleteThread(7) while main waits for it");
    seq_clear();
    th = mk("ex_del", 0x30);
    go(th, S_EXITDEL);
    r = wait_end(th, 200000);
    {
        SceKernelThreadInfo ti;
        rec("  WaitThreadEnd=%s then refer=%s GetThreadExitStatus=%s\n", hx(r),
            hx(refer(th, &ti)), hx(sceKernelGetThreadExitStatus(th)));
    }
    rec_seq();
    reap(th);

    /* threadman.c:617-632 -- Zero 80020197, Self 80020197, Invalid 80020198,
     * never started DORMANT; waitq.h:51-53 -- an argument failure leaves the
     * timeout word alone. */
    ST("WaitThreadEnd: id 0, main's own id, a deleted id, a never-started thread (timeout 1000)");
    th = mk("we_gone", 0x30);
    sceKernelDeleteThread(th);
    SceUID nev = mk("we_never", 0x30);
    t = 1000; r = sceKernelWaitThreadEnd(0, &t);      rec("  0: %s timeout-after=%s\n", hx(r), ta(t, 1000));
    t = 1000; r = sceKernelWaitThreadEnd(g_main, &t); rec("  own: %s timeout-after=%s\n", hx(r), ta(t, 1000));
    t = 1000; r = sceKernelWaitThreadEnd(th, &t);     rec("  deleted: %s timeout-after=%s\n", hx(r), ta(t, 1000));
    t = 1000; r = sceKernelWaitThreadEnd(nev, &t);    rec("  never started: %s timeout-after=%s\n", hx(r), ta(t, 1000));
    sceKernelDeleteThread(nev);

    /* waitq.h:45-49 -- a wait that ran out writes 0 back; one satisfied after
     * blocking writes back what was left; an immediate one leaves it all. */
    ST("WaitThreadEnd: a sleeping thread, timeout 5000us");
    th = spawn("we_sleep", 0x30, S_TW_SLEEP);
    t = 5000; r = sceKernelWaitThreadEnd(th, &t);
    rec("  %s timeout-after=%s\n", hx(r), ta(t, 5000));
    reap(th);

    ST("WaitThreadEnd: a thread that returns 9 after a 2000us delay, timeout 100000us, then again");
    th = spawn("we_delay", 0x30, S_D2000R9);
    t = 100000; r = sceKernelWaitThreadEnd(th, &t);
    rec("  %s timeout-after: >0 %s, <=98000 %s\n", hx(r), t > 0 ? "yes" : "no", t <= 98000 ? "yes" : "no");
    t = 100000; r = sceKernelWaitThreadEnd(th, &t);
    rec("  again (already ended): %s timeout-after=%s\n", hx(r), ta(t, 100000));
    r = sceKernelWaitThreadEnd(th, NULL);
    rec("  again with no timeout: %s\n", hx(r));
    reap(th);

    /* threadman.c:586-597 -- terminating a thread releases whoever waits for
     * it; threadman.c:659-664 -- a thread deleted under the wait answers
     * 800201ac. */
    ST("WaitThreadEnd: a 0x28 waiter on a sleeping thread that main terminates");
    fresh(); seq_clear();
    th = spawn("tw_target", 0x30, S_TW_SLEEP);
    g_uid[1] = th;
    SceUID h = spawn("tw_waiter", 0x28, S_H_WAITEND1);
    sceKernelDelayThread(1000);
    tag("m1"); r = sceKernelTerminateThread(th); tag("m2");
    sceKernelDelayThread(1000); tag("m3");
    wait_end(h, 200000);
    rec("  terminate=%s\n", hx(r)); rec_seq();
    reap(h); reap(th);

    ST("WaitThreadEnd: a 0x28 waiter on a sleeping thread that main terminate-deletes");
    fresh(); seq_clear();
    th = spawn("tw_target", 0x30, S_TW_SLEEP);
    g_uid[1] = th;
    h = spawn("tw_waiter", 0x28, S_H_WAITEND1);
    sceKernelDelayThread(1000);
    tag("m1"); r = sceKernelTerminateDeleteThread(th); tag("m2");
    sceKernelDelayThread(1000); tag("m3");
    wait_end(h, 200000);
    rec("  terminate-delete=%s\n", hx(r)); rec_seq();
    reap(h); reap(th);

    /* sched.c:494-509, waitq.c:84-95 -- a zero timeout does not reschedule
     * ([x]); a 1us one parks, and an equal-priority thread gets the CPU. */
    ST("WaitThreadEnd with timeout 0, then 1us, on a ready 0x20 thread");
    fresh(); seq_clear();
    th = mk("tz", 0x20);
    go(th, S_TW_SLEEP);
    tag("m1"); t = 0; r = sceKernelWaitThreadEnd(th, &t); tag("m2");
    int r1; SceUInt t1 = 1;
    tag("n1"); r1 = sceKernelWaitThreadEnd(th, &t1); tag("n2");
    sceKernelWakeupThread(th);
    wait_end(th, 200000);
    rec("  timeout 0: %s after=%s; timeout 1: %s after=%s\n", hx(r), ta(t, 0), hx(r1), ta(t1, 1));
    rec_seq();
    reap(th);

    /* threadman.c:583-612 -- whatever the entry returns is the exit status,
     * negative or an error code alike; ExitThread(-5) (above) is the other
     * way in. */
    ST("exit: entry returns -5, and returns 0x80020001; WaitThreadEnd, GetThreadExitStatus, refer");
    {
        static const op_t S_RETNEG[] = { { O_END, -5, 0 } };
        static const op_t S_RETERR[] = { { O_END, (int)0x80020001, 0 } };
        for (int k = 0; k < 2; k++) {
            th = spawn(k ? "ex_err" : "ex_neg", 0x30, k ? S_RETERR : S_RETNEG);
            r = wait_end(th, 200000);
            rec("  returns %s: WaitThreadEnd=%s GetThreadExitStatus=%s\n", k ? "80020001" : "-5",
                hx(r), hx(sceKernelGetThreadExitStatus(th)));
            rec_brief("after", th);
            reap(th);
        }
    }
}

/* ======================================================================= */

static const op_t S_TAGW_END[] = { { O_TAG, 0, "W" }, { O_END, 0, 0 } };
static const op_t S_SLEEP_WS[] = { { O_SLEEP, 0, "Ws" }, { O_END, 0, 0 } };

static void sec_suspend(void) {
    SEC("suspend and resume ordering");
    nm_reset();
    SceUID th;
    int r, r2, r3;

    /* threadman.c:726-740, sched.c:918-941 -- a suspended ready thread does
     * not run until resumed. */
    ST("suspend a ready 0x30 thread, main delays, then resumes it");
    fresh(); seq_clear();
    th = spawn("su_ready", 0x30, S_TAGW_END);
    r = sceKernelSuspendThread(th);
    sceKernelDelayThread(2000);
    tag("m1"); r2 = sceKernelResumeThread(th); tag("m2");
    wait_end(th, 200000);
    rec("  suspend=%s resume=%s\n", hx(r), hx(r2)); rec_seq();
    reap(th);

    /* threadman.c:742-756 -- resume does not preempt in psprecomp; on a
     * kernel that reschedules at the call, a resumed thread that outranks the
     * caller runs inside ResumeThread. */
    ST("suspend a ready 0x30 thread, raise it to 0x10 while suspended, resume it");
    fresh(); seq_clear();
    th = spawn("su_pre", 0x30, S_TAGW_END);
    r = sceKernelSuspendThread(th);
    r2 = sceKernelChangeThreadPriority(th, 0x10);
    tag("m1"); r3 = sceKernelResumeThread(th); tag("m2");
    wait_end(th, 200000);
    rec("  suspend=%s priority=%s resume=%s\n", hx(r), hx(r2), hx(r3)); rec_seq();
    reap(th);

    /* sched.c:929-936 -- psprecomp forgets what a suspended thread was parked
     * on, so resuming it ends its wait. */
    ST("suspend a sleeping 0x10 thread, resume it, main delays, then wakes it");
    fresh(); seq_clear();
    th = spawn("su_sleep", 0x10, S_SLEEP_WS);
    r = sceKernelSuspendThread(th);
    rec_brief("suspended sleeper", th);
    r2 = sceKernelResumeThread(th);
    rec_brief("resumed", th);
    tag("m1"); sceKernelDelayThread(1000); tag("m2");
    r3 = sceKernelWakeupThread(th); tag("m3");
    wait_end(th, 200000);
    rec("  suspend=%s resume=%s wakeup=%s\n", hx(r), hx(r2), hx(r3)); rec_seq();
    reap(th);

    ST("suspend a sleeping 0x10 thread, wake it while suspended, then resume it");
    fresh(); seq_clear();
    th = spawn("su_wake", 0x10, S_SLEEP_WS);
    r = sceKernelSuspendThread(th);
    r2 = sceKernelWakeupThread(th);
    rec_brief("woken while suspended", th);
    tag("m1"); sceKernelDelayThread(1000); tag("m2");
    r3 = sceKernelResumeThread(th); tag("m3");
    wait_end(th, 200000);
    rec("  suspend=%s wakeup=%s resume=%s\n", hx(r), hx(r2), hx(r3)); rec_seq();
    reap(th);

    /* sched.h -- suspension is a flag over the wait (psp_thread.suspended):
     * psprecomp lets a deadline or a signal end the wait underneath it, and
     * the thread runs once resumed. */
    static const op_t S_WSEMA10MS[] = { { O_WAITSEMA, 10000, "W" }, { O_END, 0, 0 } };
    static const op_t S_WSEMA10S[] = { { O_WAITSEMA, 10000000, "W" }, { O_END, 0, 0 } };
    SceKernelSemaInfo si;
    ST("suspend a 0x30 thread waiting on a semaphore (10ms timeout), let the timeout pass, resume");
    fresh(); seq_clear();
    g_sema = sceKernelCreateSema("ssema", 0, 0, 1, NULL);
    th = spawn("su_tmo", 0x30, S_WSEMA10MS);
    sceKernelDelayThread(1000);
    r = sceKernelSuspendThread(th);
    sceKernelDelayThread(30000);
    rec_brief("suspended, 30ms later", th);
    si.size = sizeof si;
    sceKernelReferSemaStatus(g_sema, &si);
    rec("  sema: count=%d waiters=%d\n", si.currentCount, si.numWaitThreads);
    tag("m1"); r2 = sceKernelResumeThread(th); tag("m2");
    wait_end(th, 200000);
    rec("  suspend=%s resume=%s\n", hx(r), hx(r2)); rec_seq();
    reap(th);

    ST("suspend a 0x30 thread waiting on a semaphore, signal it while suspended, resume");
    fresh(); seq_clear();
    th = spawn("su_sig", 0x30, S_WSEMA10S);
    sceKernelDelayThread(1000);
    r = sceKernelSuspendThread(th);
    r2 = sceKernelSignalSema(g_sema, 1);
    rec_brief("suspended, signalled", th);
    si.size = sizeof si;
    sceKernelReferSemaStatus(g_sema, &si);
    rec("  sema: count=%d waiters=%d\n", si.currentCount, si.numWaitThreads);
    tag("m1"); r3 = sceKernelResumeThread(th); tag("m2");
    wait_end(th, 200000);
    rec("  suspend=%s signal=%s resume=%s\n", hx(r), hx(r2), hx(r3)); rec_seq();
    reap(th);
    sceKernelDeleteSema(g_sema);
}

/* ======================================================================= */

static void sec_priority(void) {
    SEC("priority");
    nm_reset();
    SceUID th;
    SceKernelThreadInfo ti;
    int r;

    /* threadman.c:772-790 -- outside 0x08..0x77 is ILLEGAL_PRIORITY; 0 means
     * the caller's current priority (reads back 0x18 after the caller set
     * itself to 0x18); the initial priority is untouched. */
    ST("ChangeThreadPriority on a sleeping 0x30 thread: 0, 1, 7, 8, 0x10, 0x77, 0x78, 0x7F, 0x80, -1, -2");
    th = spawn("pr_sweep", 0x30, S_SLEEP_WS);
    sceKernelDelayThread(1000);
    static const int pv[] = { 0, 1, 7, 8, 0x10, 0x77, 0x78, 0x7F, 0x80, -1, -2 };
    for (int i = 0; i < 11; i++) {
        r = sceKernelChangeThreadPriority(th, pv[i]);
        refer(th, &ti);
        rec("  %08X: %s init=%02X cur=%02X\n", (w32)pv[i], hx(r), (w32)ti.initPriority,
            (w32)ti.currentPriority);
    }
    ST("ChangeThreadPriority(t, 0) after main set itself to 0x1E");
    r = sceKernelChangeThreadPriority(0, 0x1E);
    int r2 = sceKernelChangeThreadPriority(th, 0);
    refer(th, &ti);
    int r3 = sceKernelChangeThreadPriority(0, 0x20);
    rec("  main->1E=%s t->0=%s t cur=%02X main->20=%s main now %02X\n", hx(r), hx(r2),
        (w32)ti.currentPriority, hx(r3), (w32)sceKernelGetThreadCurrentPriority());
    r = sceKernelChangeThreadPriority(th, 0x30);
    sceKernelWakeupThread(th);
    wait_end(th, 200000);
    reap(th);

    ST("ChangeThreadPriority: a deleted id, and main to its own priority");
    th = mk("pr_gone", 0x30);
    sceKernelDeleteThread(th);
    rec("  deleted: %s\n", hx(sceKernelChangeThreadPriority(th, 0x30)));
    rec("  main 0x20 -> 0x20: %s\n", hx(sceKernelChangeThreadPriority(0, 0x20)));

    /* threadman.c:395-400 -- a (re)start restores the created priority:
     * "After restart: Current=30, init=30". */
    ST("priority after terminate and restart: created 0x30, changed to 0x2A while sleeping");
    th = spawn("pr_rst", 0x30, S_SLEEP_WS);
    sceKernelDelayThread(1000);
    r = sceKernelChangeThreadPriority(th, 0x2A);
    rec_brief("changed", th);
    r2 = sceKernelTerminateThread(th);
    rec_brief("terminated", th);
    r3 = go(th, S_SLEEP_WS);
    rec_brief("restarted (not yet run)", th);
    rec("  change=%s terminate=%s restart=%s\n", hx(r), hx(r2), hx(r3));
    reap(th);

    /* threadman.c:791-803, sched.c:516-529 -- raising another thread above
     * the caller runs it at once, and the displaced caller stays at the head
     * of its own queue (an equal thread already waiting there does not
     * overtake it). */
    ST("reschedule: raise a ready 0x30 thread to 0x10 while an equal 0x20 thread is ready");
    fresh(); seq_clear();
    SceUID a = mk("A", 0x20), b = mk("B", 0x30);
    static const op_t SA[] = { { O_TAG, 0, "A" }, { O_END, 0, 0 } };
    static const op_t SB[] = { { O_TAG, 0, "B" }, { O_END, 0, 0 } };
    go(a, SA); go(b, SB);
    tag("m1"); r = sceKernelChangeThreadPriority(b, 0x10); tag("m2");
    sceKernelDelayThread(1000); tag("m3");
    wait_end(a, 200000); wait_end(b, 200000);
    rec("  change=%s\n", hx(r)); rec_seq();
    reap(a); reap(b);

    /* threadman.c:791-793 -- lowering your own priority is a reschedule point. */
    ST("reschedule: main lowers itself to 0x30 with a ready 0x28 thread");
    fresh(); seq_clear();
    th = mk("W", 0x28);
    go(th, S_TAGW_END);
    tag("m1"); r = sceKernelChangeThreadPriority(0, 0x30); tag("m2");
    r2 = sceKernelChangeThreadPriority(0, 0x20);
    wait_end(th, 200000);
    rec("  lower=%s restore=%s\n", hx(r), hx(r2)); rec_seq();
    reap(th);

    /* threadman.c:802 -- psprecomp yields whenever a thread changes its own
     * priority, even to the same value. */
    ST("reschedule: main sets its own priority to 0x20 (unchanged) with a ready 0x20 thread");
    fresh(); seq_clear();
    th = mk("W", 0x20);
    go(th, S_TAGW_END);
    tag("m1"); r = sceKernelChangeThreadPriority(0, 0x20); tag("m2");
    sceKernelDelayThread(1000); tag("m3");
    wait_end(th, 200000);
    rec("  change=%s\n", hx(r)); rec_seq();
    reap(th);

    ST("reschedule: main raises itself to 0x1F, then back to 0x20, with a ready 0x20 thread");
    fresh(); seq_clear();
    th = mk("W", 0x20);
    go(th, S_TAGW_END);
    tag("m1"); r = sceKernelChangeThreadPriority(0, 0x1F); tag("m2");
    r2 = sceKernelChangeThreadPriority(0, 0x20); tag("m3");
    sceKernelDelayThread(1000); tag("m4");
    wait_end(th, 200000);
    rec("  raise=%s restore=%s\n", hx(r), hx(r2)); rec_seq();
    reap(th);

    ST("reschedule: a ready 0x30 thread changed to 0x20 (equal to main)");
    fresh(); seq_clear();
    th = mk("W", 0x30);
    go(th, S_TAGW_END);
    tag("m1"); r = sceKernelChangeThreadPriority(th, 0x20); tag("m2");
    sceKernelDelayThread(1000); tag("m3");
    wait_end(th, 200000);
    rec("  change=%s\n", hx(r)); rec_seq();
    reap(th);
}

/* ======================================================================= */

static void sec_rotate(void) {
    SEC("rotate ready queue");
    nm_reset();
    int r;

    /* threadman.c:1231-1239 -- 0 is always allowed; otherwise 0x08..0x77. */
    ST("RotateThreadReadyQueue: 0, 7, 8, 0x20, 0x77, 0x78, 0x7F, 0x80, -1 with nobody else ready");
    static const int rv[] = { 0, 7, 8, 0x20, 0x77, 0x78, 0x7F, 0x80, -1 };
    for (int i = 0; i < 9; i++)
        rec("  %08X: %s\n", (w32)rv[i], hx(sceKernelRotateThreadReadyQueue(rv[i])));

    /* threadman.c:1227-1230 -- rotating the caller's own level lets the rest
     * of that level run first. */
    ST("rotate own level: two ready 0x20 threads, the first rotates too");
    fresh(); seq_clear();
    static const op_t S1[] = { { O_TAG, 0, "W1" }, { O_ROTATE, 0, 0 }, { O_TAG, 0, "W1b" }, { O_END, 0, 0 } };
    static const op_t S2[] = { { O_TAG, 0, "W2" }, { O_END, 0, 0 } };
    SceUID w1 = mk("W1", 0x20), w2 = mk("W2", 0x20);
    go(w1, S1); go(w2, S2);
    tag("m1"); r = sceKernelRotateThreadReadyQueue(0); tag("m2");
    sceKernelDelayThread(1000); tag("m3");
    wait_end(w1, 200000); wait_end(w2, 200000);
    rec("  rotate=%s\n", hx(r)); rec_seq();
    reap(w1); reap(w2);

    /* threadman.c:1240 -- psprecomp yields rather than rotating the named
     * level, so another level's order is unchanged. */
    ST("rotate another level: three ready 0x30 threads, main rotates 0x30");
    fresh(); seq_clear();
    static const op_t T1[] = { { O_TAG, 0, "W1" }, { O_END, 0, 0 } };
    static const op_t T2[] = { { O_TAG, 0, "W2" }, { O_END, 0, 0 } };
    static const op_t T3[] = { { O_TAG, 0, "W3" }, { O_END, 0, 0 } };
    w1 = mk("W1", 0x30); w2 = mk("W2", 0x30);
    SceUID w3 = mk("W3", 0x30);
    go(w1, T1); go(w2, T2); go(w3, T3);
    tag("m1"); r = sceKernelRotateThreadReadyQueue(0x30); tag("m2");
    wait_end(w1, 200000); wait_end(w2, 200000); wait_end(w3, 200000);
    rec("  rotate=%s\n", hx(r)); rec_seq();
    reap(w1); reap(w2); reap(w3);
}

/* ======================================================================= */

static const op_t S_SLEEP3[] = {
    { O_SLEEP, 0, "s1" }, { O_SLEEP, 0, "s2" }, { O_SLEEP, 0, "s3" }, { O_END, 0, 0 } };
static const op_t S_SAFETY[] = {
    { O_DELAY, 100000, 0 }, { O_WAKEUP, 0, "safety-woke-main" }, { O_END, 0, 0 } };

static void sec_sleep(void) {
    SEC("sleep and wakeup");
    nm_reset();
    int r, r2;
    SceUID th;

    /* threadman.c:806-850 -- wakeups bank; a sleep with a banked wakeup
     * returns at once; CancelWakeupThread returns the count and zeroes it. */
    ST("banked wakeups: ready 0x30 thread woken 3x, cancelled, woken 2x, then runs three sleeps");
    fresh(); seq_clear();
    th = spawn("sl", 0x30, S_SLEEP3);
    int w1 = sceKernelWakeupThread(th), w2 = sceKernelWakeupThread(th), w3 = sceKernelWakeupThread(th);
    rec_brief("after 3 wakeups", th);
    r = sceKernelCancelWakeupThread(th);
    r2 = sceKernelCancelWakeupThread(th);
    rec("  wakeups=%s %s %s cancel=%s cancel again=%s\n", hx(w1), hx(w2), hx(w3), hx(r), hx(r2));
    sceKernelWakeupThread(th); sceKernelWakeupThread(th);
    rec_brief("after 2 more", th);
    sceKernelDelayThread(1000);
    rec_brief("after it ran", th);
    tag("m1"); r = sceKernelWakeupThread(th);
    wait_end(th, 200000);
    rec("  last wakeup=%s\n", hx(r)); rec_seq();
    reap(th);

    ST("main: WakeupThread(own id) x2, CancelWakeupThread(0), wake once more, SleepThread (safety waker at 100ms)");
    fresh(); seq_clear();
    w1 = sceKernelWakeupThread(g_main);
    w2 = sceKernelWakeupThread(g_main);
    r = sceKernelCancelWakeupThread(0);
    w3 = sceKernelWakeupThread(g_main);
    th = spawn("safety", 0x30, S_SAFETY);
    r2 = sceKernelSleepThread();
    tag("m-after-sleep");
    int c = sceKernelCancelWakeupThread(0);
    reap(th);
    sceKernelCancelWakeupThread(0);
    rec("  wake=%s %s cancel(0)=%s wake=%s sleep=%s cancel(0) after=%s\n",
        hx(w1), hx(w2), hx(r), hx(w3), hx(r2), hx(c));
    rec_seq();

    ST("WakeupThread(0) from main, then CancelWakeupThread(0); WakeupThread on a deleted id");
    r = sceKernelWakeupThread(0);
    r2 = sceKernelCancelWakeupThread(0);
    th = mk("sl_gone", 0x30);
    sceKernelDeleteThread(th);
    rec("  wakeup(0)=%s cancel(0)=%s deleted=%s cancel(deleted)=%s\n", hx(r), hx(r2),
        hx(sceKernelWakeupThread(th)), hx(sceKernelCancelWakeupThread(th)));

    /* Not in psprecomp: ReleaseWaitThread forces a waiting thread out. */
    ST("ReleaseWaitThread on a sleeping 0x30 thread, on a ready one, on main");
    fresh(); seq_clear();
    th = spawn("rw", 0x30, S_SLEEP_WS);
    sceKernelDelayThread(1000);
    r = sceKernelReleaseWaitThread(th);
    sceKernelDelayThread(1000);
    SceUID th2 = spawn("rw2", 0x30, S_SLEEP_WS);
    r2 = sceKernelReleaseWaitThread(th2);
    int r3 = sceKernelReleaseWaitThread(g_main);
    rec_brief("sleeper after release", th);
    reap(th); reap(th2);
    rec("  sleeping=%s ready=%s main=%s\n", hx(r), hx(r2), hx(r3)); rec_seq();

    /* threadman.c:628-660 -- NOT_WAIT for anything not waiting; a delay ends
     * like any other wait (RELEASE_WAIT); a suspended waiter's wait ends
     * underneath its suspension and it stays suspended. */
    ST("ReleaseWaitThread on a dormant thread, a suspended ready one, one in DelayThread(1s), one waiting and suspended");
    {
        SceUID d = mk("rw_dorm", 0x30);
        int rd = sceKernelReleaseWaitThread(d);
        sceKernelDeleteThread(d);
        SceUID s = spawn("rw_susp", 0x30, S_SLEEP_WS);
        sceKernelSuspendThread(s);
        int rs = sceKernelReleaseWaitThread(s);
        reap(s);
        rec("  dormant=%s suspended ready=%s\n", hx(rd), hx(rs));
        rw_one("delay", S_RW_DELAY, NULL);

        fresh(); seq_clear();
        g_tleft = 0xEEEEEEEE;
        g_sema = sceKernelCreateSema("rw_sema", 0, 0, 1, NULL);
        SceUID ws = spawn("rw_ws", 0x30, S_RW_SEMA);
        sceKernelDelayThread(1000);
        int su = sceKernelSuspendThread(ws);
        int rws = sceKernelReleaseWaitThread(ws);
        rec_brief("waiting+suspended, released", ws);
        tag("m1"); int res = sceKernelResumeThread(ws); tag("m2");
        wait_end(ws, 200000);
        rec("  waiting+suspended: suspend=%s release=%s resume=%s timeout-after=%s\n", hx(su), hx(rws),
            hx(res), g_tleft == 0xEEEEEEEE ? "-" : ta(g_tleft, 1000000));
        rec_seq();
        reap(ws);
        sceKernelDeleteSema(g_sema);
    }

    /* kernobj.c:21-28 (wait_end_code), kernlock.c:237-243, 650-651 -- every
     * object wait answers RELEASE_WAIT and leaves its queue; unmeasured until
     * this step. A released GetTlsAddr is NULL here (kernobj.c). */
    ST("ReleaseWaitThread on 0x30 threads waiting (1s) on a sema, evf, mbx, vpl, fpl, msgpipe, mutex, lwmutex, TLS pool, thread end");
    objs_make();
    rw_one("sema", S_RW_SEMA, wn_sema);
    rw_one("evf", S_RW_EVF, wn_evf);
    rw_one("mbx", S_RW_MBX, wn_mbx);
    rw_one("vpl", S_RW_VPL, wn_vpl);
    rw_one("fpl", S_RW_FPL, wn_fpl);
    rw_one("msgpipe", S_RW_MPP, wn_mpp);
    rw_one("mutex", S_RW_MTX, wn_mtx);
    rw_one("lwmutex", S_RW_LW, wn_lw);
    rw_one("tlspl", S_RW_TLS, wn_tls);
    g_uid[1] = spawn("rw_target", 0x30, S_SLEEP_WS);
    rw_one("threadend", S_RW_END, NULL);
    reap(g_uid[1]);
    objs_free();
}

/* ======================================================================= */

static char g_iobuf[0x8000] __attribute__((aligned(64)));

static const op_t S_TSA[] = { { O_TAG, 0, "A0" }, { O_SPIN, 20000, "A1" }, { O_END, 0, 0 } };
static const op_t S_TSB[] = { { O_TAG, 0, "B" }, { O_END, 0, 0 } };
static const op_t S_WSEMA_W[] = { { O_WAITSEMA, 500000, "W" }, { O_END, 0, 0 } };

static void sec_preempt(void) {
    SEC("preemption and wake order");
    nm_reset();
    int r;
    SceUID a, b, th;
    static const op_t SA[] = { { O_TAG, 0, "A" }, { O_END, 0, 0 } };
    static const op_t SB[] = { { O_TAG, 0, "B" }, { O_END, 0, 0 } };
    static const op_t SBS[] = { { O_SLEEP, 0, "B" }, { O_END, 0, 0 } };

    /* sched.c:43-50, 211-214, 462-478 -- a thread displaced by a more urgent
     * one it started stays at the head of its queue. */
    ST("preempt by start: main (0x20) starts a 0x10 thread while an equal 0x20 thread is ready");
    fresh(); seq_clear();
    a = mk("A", 0x20); b = mk("B", 0x10);
    go(a, SA);
    tag("m1"); go(b, SB); tag("m2");
    sceKernelDelayThread(1000); tag("m3");
    wait_end(a, 200000); wait_end(b, 200000);
    rec_seq();
    reap(a); reap(b);

    /* sched.c:516-529, threadman.c:832-839 -- waking a more urgent sleeper
     * switches to it at once, and the waker keeps the head of its queue. */
    ST("preempt by wakeup: main wakes a sleeping 0x10 thread while an equal 0x20 thread is ready");
    fresh(); seq_clear();
    b = spawn("B", 0x10, SBS);
    a = mk("A", 0x20);
    go(a, SA);
    tag("m1"); r = sceKernelWakeupThread(b); tag("m2");
    sceKernelDelayThread(1000); tag("m3");
    wait_end(a, 200000); wait_end(b, 200000);
    rec("  wakeup=%s\n", hx(r)); rec_seq();
    reap(a); reap(b);

    ST("wakeup of an equal-priority (0x20) sleeper");
    fresh(); seq_clear();
    th = mk("W", 0x20);
    go(th, S_SLEEP_WS);
    sceKernelDelayThread(1000);
    tag("m1"); r = sceKernelWakeupThread(th); tag("m2");
    sceKernelDelayThread(1000); tag("m3");
    wait_end(th, 200000);
    rec("  wakeup=%s\n", hx(r)); rec_seq();
    reap(th);

    /* threadman.c:1188-1192 -- a signal that releases a more urgent waiter
     * runs it before SignalSema returns. */
    ST("preempt by SignalSema: a 0x10 thread waiting on the semaphore");
    fresh(); seq_clear();
    g_sema = sceKernelCreateSema("psema", 0, 0, 1, NULL);
    th = spawn("W", 0x10, S_WSEMA_W);
    tag("m1"); r = sceKernelSignalSema(g_sema, 1); tag("m2");
    wait_end(th, 200000);
    rec("  signal=%s\n", hx(r)); rec_seq();
    reap(th);
    sceKernelDeleteSema(g_sema);

    /* sched.c:608-641 -- psprecomp gives equal-priority threads a 5ms slice
     * of guest time. A thread that spins 20ms making firmware calls shows
     * whether hardware ever hands its equal the CPU before it blocks. */
    ST("timeslice: two ready 0x30 threads, the first spins 20ms on GetSystemTimeLow");
    fresh(); seq_clear();
    a = mk("A", 0x30); b = mk("B", 0x30);
    go(a, S_TSA); go(b, S_TSB);
    wait_end(b, 500000); wait_end(a, 500000);
    rec_seq();
    reap(a); reap(b);

    /* threadman.c:2209 -- psprecomp keeps the three counters; step 1 read
     * main's as intrPreempt 0, threadPreempt 1. Whether a thread preempted by
     * a more urgent one waking counts a thread preemption, an interrupt one,
     * or a release, is what this settles. */
    ST("counters: a 0x30 thread spins 30ms while main wakes twice from 5ms delays; its and main's preempt and release counts");
    {
        static const op_t S_SPINREF[] = { { O_SPIN, 30000, "W" }, { O_REFERSELF, 0, 0 }, { O_END, 0, 0 } };
        SceKernelThreadInfo m0, m1;
        fresh(); seq_clear();
        g_wref_ret = (int)0xEEEEEEEE;
        refer(0, &m0);
        th = spawn("W", 0x30, S_SPINREF);
        tag("m1"); sceKernelDelayThread(5000); tag("m2"); sceKernelDelayThread(5000); tag("m3");
        refer(0, &m1);
        wait_end(th, 500000);
        rec("  W: refer=%s threadPreempt=%u intrPreempt=%s release=%u\n", hx(g_wref_ret),
            (w32)g_wref.threadPreemptCount, g_wref.intrPreemptCount ? "nz" : "0",
            (w32)g_wref.releaseCount);
        rec("  main across its two delays: threadPreempt+%u intrPreempt+%s release+%u\n",
            (w32)(m1.threadPreemptCount - m0.threadPreemptCount),
            m1.intrPreemptCount != m0.intrPreemptCount ? "nz" : "0",
            (w32)(m1.releaseCount - m0.releaseCount));
        rec_seq();
        reap(th);
    }

    /* sched.c -- no timeslice between equals (step 76 spun 20ms); 200ms is
     * long enough for any slice a kernel might have. */
    ST("timeslice: two ready 0x30 threads, the first spins 200ms on GetSystemTimeLow");
    {
        static const op_t S_TSA200[] = { { O_TAG, 0, "A0" }, { O_SPIN, 200000, "A1" }, { O_END, 0, 0 } };
        fresh(); seq_clear();
        a = mk("A", 0x30); b = mk("B", 0x30);
        go(a, S_TSA200); go(b, S_TSB);
        wait_end(b, 1000000); wait_end(a, 1000000);
        rec_seq();
        reap(a); reap(b);
    }

    /* iofilemgr.c -- psprecomp's sceIo calls finish inside the call, so an
     * equal-priority ready thread never runs during one. On a PSP a call that
     * waits for the memory stick blocks its caller. The file is the probe's
     * own, beside the EBOOT, and is removed by the last call. */
    ST("sceIo from main (0x20) with a ready 0x20 thread: did it run inside open, write, close, open, read, lseek, read, close, getstat, remove; main's releaseCount across each");
    {
        static const char *const lab[] = { "open(w)", "write 32K", "close", "open(r)", "read 32K",
                                           "lseek 0", "read 16", "close", "getstat", "remove" };
        char path[128];
        snprintf(path, sizeof path, "%su2.bin", probe_dir());
        SceUID fd = -1;
        for (int i = 0; i < 0x8000; i++) g_iobuf[i] = (char)i;
        for (int k = 0; k < 10; k++) {
            SceKernelThreadInfo i0, i1;
            SceUID w = sceKernelCreateThread("W", script_entry, 0x20, 0x2000, 0, NULL);
            go(w, S_TAGW_END);
            seq_clear();
            refer(0, &i0);
            tag("m");
            int rr = 0;
            SceIoStat st;
            switch (k) {
            case 0: rr = fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777); break;
            case 1: rr = sceIoWrite(fd, g_iobuf, 0x8000); break;
            case 2: rr = sceIoClose(fd); break;
            case 3: rr = fd = sceIoOpen(path, PSP_O_RDONLY, 0); break;
            case 4: rr = sceIoRead(fd, g_iobuf, 0x8000); break;
            case 5: rr = (int)sceIoLseek32(fd, 0, PSP_SEEK_SET); break;
            case 6: rr = sceIoRead(fd, g_iobuf, 16); break;
            case 7: rr = sceIoClose(fd); break;
            case 8: rr = sceIoGetstat(path, &st); break;
            case 9: rr = sceIoRemove(path); break;
            }
            tag("c");
            refer(0, &i1);
            sceKernelDelayThread(1000);
            wait_end(w, 200000);
            reap(w);
            rec("  %-10s = %s  seq: %s  release+%u\n", lab[k],
                (k == 0 || k == 3) && rr > 0 ? "fd" : hx(rr), g_seq,
                (w32)(i1.releaseCount - i0.releaseCount));
        }
    }
}

/* ======================================================================= */

static void sec_delay(void) {
    SEC("delay");
    nm_reset();
    SceUID a, b, c;
    static const op_t A20[] = { { O_DELAY, 20000, 0 }, { O_TAG, 0, "A" }, { O_END, 0, 0 } };
    static const op_t B20[] = { { O_DELAY, 20000, 0 }, { O_TAG, 0, "B" }, { O_END, 0, 0 } };
    static const op_t C20[] = { { O_DELAY, 20000, 0 }, { O_TAG, 0, "C" }, { O_END, 0, 0 } };
    static const op_t A30[] = { { O_DELAY, 30000, 0 }, { O_TAG, 0, "A" }, { O_END, 0, 0 } };
    static const op_t C10[] = { { O_DELAY, 10000, 0 }, { O_TAG, 0, "C" }, { O_END, 0, 0 } };
    static const op_t A10[] = { { O_DELAY, 10000, 0 }, { O_TAG, 0, "A" }, { O_END, 0, 0 } };
    static const op_t B10[] = { { O_DELAY, 10000, 0 }, { O_TAG, 0, "B" }, { O_END, 0, 0 } };

    /* sched.c:189-215, 566-606 -- expired delays become ready in the order
     * psprecomp's handoff scans its slots, round-robin from the current one;
     * on a kernel with a timer they queue in deadline order. The main thread
     * spins (more urgent, never blocking) until all have expired, so the
     * order they then run in is the ready-queue order. */
    ST("delay: three 0x30 threads each delay 20ms (started A, B, C); main spins 45ms, then waits");
    fresh(); seq_clear();
    a = mk("A", 0x30); b = mk("B", 0x30); c = mk("C", 0x30);
    go(a, A20); go(b, B20); go(c, C20);
    sceKernelDelayThread(2000);
    spin_us(45000);
    wait_end(a, 500000); wait_end(b, 500000); wait_end(c, 500000);
    rec_seq(); reap(a); reap(b); reap(c);

    ST("delay: 0x30 threads delay 30ms, 20ms, 10ms (started A, B, C); main spins 45ms, then waits");
    fresh(); seq_clear();
    a = mk("A", 0x30); b = mk("B", 0x30); c = mk("C", 0x30);
    go(a, A30); go(b, B20); go(c, C10);
    sceKernelDelayThread(2000);
    spin_us(45000);
    wait_end(a, 500000); wait_end(b, 500000); wait_end(c, 500000);
    rec_seq(); reap(a); reap(b); reap(c);

    ST("delay: A (0x30) delays 10ms, B (0x2C) delays 20ms; main spins 45ms, then waits");
    fresh(); seq_clear();
    a = mk("A", 0x30); b = mk("B", 0x2C);
    go(a, A10); go(b, B20);
    sceKernelDelayThread(2000);
    spin_us(45000);
    wait_end(a, 500000); wait_end(b, 500000);
    rec_seq(); reap(a); reap(b);

    ST("delay: A (0x30) 30ms, B and C (0x28) 10ms each; main just waits");
    fresh(); seq_clear();
    a = mk("A", 0x30); b = mk("B", 0x28); c = mk("C", 0x28);
    go(a, A30); go(b, B10); go(c, C10);
    wait_end(a, 500000); wait_end(b, 500000); wait_end(c, 500000);
    rec_seq(); reap(a); reap(b); reap(c);

    /* sched.c:580-606 -- a delay of 0 is still a request to stand aside. */
    ST("delay: main DelayThread(0) with a ready 0x20 thread");
    fresh(); seq_clear();
    a = mk("W", 0x20);
    go(a, S_TAGW_END);
    tag("m1"); int r = sceKernelDelayThread(0); tag("m2");
    sceKernelDelayThread(1000); tag("m3");
    wait_end(a, 200000);
    rec("  delay(0)=%s\n", hx(r)); rec_seq(); reap(a);

    ST("delay: main DelayThread(1) with a ready 0x20 thread");
    fresh(); seq_clear();
    a = mk("W", 0x20);
    go(a, S_TAGW_END);
    tag("m1"); r = sceKernelDelayThread(1); tag("m2");
    sceKernelDelayThread(1000); tag("m3");
    wait_end(a, 200000);
    rec("  delay(1)=%s\n", hx(r)); rec_seq(); reap(a);

    ST("delay: elapsed system time across DelayThread(10000) and DelaySysClockThread(5000)");
    {
        SceInt64 t0 = sceKernelGetSystemTimeWide();
        r = sceKernelDelayThread(10000);
        SceInt64 t1 = sceKernelGetSystemTimeWide();
        SceKernelSysClock clk = { 5000, 0 };
        int r2 = sceKernelDelaySysClockThread(&clk);
        SceInt64 t2 = sceKernelGetSystemTimeWide();
        rec("  DelayThread(10000)=%s elapsed>=10000 %s\n", hx(r), t1 - t0 >= 10000 ? "yes" : "no");
        rec("  DelaySysClockThread(5000)=%s elapsed>=5000 %s clock after: %s\n", hx(r2),
            t2 - t1 >= 5000 ? "yes" : "no",
            clk.hi == 0 && clk.low == 5000 ? "unchanged" : "changed");
    }

    /* threadman.c:2530-2556 -- a CB delay delivers first, then sleeps even
     * for 0 (DelayThread(0) does not park: step 80). */
    ST("delay: DelayThreadCB(0) with a pending callback notify, then with a ready 0x20 thread");
    {
        g_cbhits = 0; g_cbret = 0;
        SceUID cb = sceKernelCreateCallback("dcb", h_count, NULL);
        fresh(); seq_clear();
        sceKernelNotifyCallback(cb, 1);
        tag("m1"); r = sceKernelDelayThreadCB(0); tag("m2");
        int hits = g_cbhits;
        a = mk("W", 0x20);
        go(a, S_TAGW_END);
        tag("n1"); int r2 = sceKernelDelayThreadCB(0); tag("n2");
        sceKernelDelayThread(1000); tag("n3");
        wait_end(a, 200000);
        rec("  DelayThreadCB(0)=%s delivered=%d; with a ready thread=%s\n", hx(r), hits, hx(r2));
        rec_seq();
        reap(a);
        sceKernelDeleteCallback(cb);
    }

    /* Step 81 (DelayThread(1) with a ready 0x20 thread) came out both ways on
     * two runs: the wait can end before the other thread is dispatched. */
    ST("delay: DelayThread(n) for n = 0, 1, 2, 5, 10, 50, 100 with a ready 0x20 thread, 4 times each: how often it ran before main came back");
    {
        static const int ns[] = { 0, 1, 2, 5, 10, 50, 100 };
        for (int i = 0; i < 7; i++) {
            int first = 0;
            for (int k = 0; k < 4; k++) {
                fresh(); seq_clear();
                a = sceKernelCreateThread("W", script_entry, 0x20, 0x2000, 0, NULL);
                go(a, S_TAGW_END);
                sceKernelDelayThread(ns[i]);
                tag("m");
                sceKernelDelayThread(1000);
                wait_end(a, 200000);
                reap(a);
                if (g_seq[0] == 'W') first++;
            }
            rec("  n=%3d: %d of 4\n", ns[i], first);
        }
    }
}

/* ======================================================================= */

static void sec_dispatch(void) {
    SEC("dispatch");
    nm_reset();

    /* sched.h:277-293, threadman.c:852-873 -- suspend returns the previous
     * state; a second suspend is an error (these do not nest); while
     * suspended every wait answers CAN_NOT_WAIT, even one that would
     * succeed; resume with anything a suspend did not return is an error. */
    ST("SuspendDispatchThread twice, waits and a 0x10 start while suspended, then resume");
    fresh(); seq_clear();
    SceUID s = sceKernelCreateSema("dsema", 0, 1, 1, NULL);
    SceUID th = mk("W", 0x10);
    int d1 = sceKernelSuspendDispatchThread();
    int d2 = sceKernelSuspendDispatchThread();
    int dl = sceKernelDelayThread(1000);
    SceUInt t = 1000;
    int ws = sceKernelWaitSema(s, 1, &t);
    SceUInt t2 = 1000;
    int wbig = sceKernelWaitSema(s, 5, &t2);
    int ps = sceKernelPollSema(s, 1);
    int st = go(th, S_TAGW_END);
    int ro = sceKernelRotateThreadReadyQueue(0);
    tag("m1");
    int rs2 = sceKernelResumeDispatchThread(d2);
    tag("m2");
    int rs1 = sceKernelResumeDispatchThread(d1);
    tag("m3");
    int chk = sceKernelSuspendDispatchThread();
    int chk2 = sceKernelResumeDispatchThread(chk);
    if (chk != d1) sceKernelResumeDispatchThread(d1);
    wait_end(th, 200000);
    rec("  suspend=%s suspend again=%s\n", hx(d1), hx(d2));
    rec("  while suspended: DelayThread=%s WaitSema(count 1)=%s timeout-after=%s WaitSema(5 > max)=%s PollSema=%s\n",
        hx(dl), hx(ws), ta(t, 1000), hx(wbig), hx(ps));
    rec("  StartThread(0x10)=%s Rotate(0)=%s\n", hx(st), hx(ro));
    rec("  resume(second)=%s resume(first)=%s; check suspend=%s resume=%s\n",
        hx(rs2), hx(rs1), hx(chk), hx(chk2));
    rec_seq();
    reap(th);
    sceKernelDeleteSema(s);

    /* threadman.c:696-701 -- dispatch is checked before a 0 delay returns;
     * threadman.c:1142-1145 -- a resume with anything but 0 or 1 is CPUDI
     * (0x80020066); misc.c:53-62 -- interrupts off changes nothing. All three
     * unmeasured. Each resume that might have been refused is followed by a
     * suspend that says what state it left, and resume(1). */
    ST("dispatch: DelayThread(0) while suspended; ResumeDispatchThread(2) and (-1); SuspendDispatchThread with interrupts suspended");
    {
        int d = sceKernelSuspendDispatchThread();
        int dz = sceKernelDelayThread(0);
        int rz = sceKernelResumeDispatchThread(d);
        sceKernelSuspendDispatchThread();
        int r2 = sceKernelResumeDispatchThread(2);
        int c2 = sceKernelSuspendDispatchThread();
        sceKernelResumeDispatchThread(1);
        sceKernelSuspendDispatchThread();
        int rm = sceKernelResumeDispatchThread(-1);
        int cm = sceKernelSuspendDispatchThread();
        sceKernelResumeDispatchThread(1);
        int on = sceKernelSuspendDispatchThread();
        sceKernelResumeDispatchThread(on);
        unsigned fl = sceKernelCpuSuspendIntr();
        int si = sceKernelSuspendDispatchThread();
        int ri = si >= 0 ? sceKernelResumeDispatchThread(si) : 0;
        sceKernelCpuResumeIntr(fl);
        rec("  while suspended: DelayThread(0)=%s; resume=%s\n", hx(dz), hx(rz));
        rec("  ResumeDispatchThread(2)=%s, then suspend answers %s; ResumeDispatchThread(-1)=%s, then suspend answers %s; afterwards suspend answers %s\n",
            hx(r2), hx(c2), hx(rm), hx(cm), hx(on));
        rec("  interrupts suspended: SuspendDispatchThread=%s ResumeDispatchThread=%s\n", hx(si),
            si >= 0 ? hx(ri) : "-");
    }

    /* misc.c:53-62 -- psprecomp has no interrupts, so a wait with them
     * suspended waits as usual. A PSP might refuse (CPUDI) or might park a
     * thread that no timer can wake; the step is skipped on a restart. */
    if (!ST("dispatch: DelayThread(1000), and WaitSema with a 1000us timeout, with interrupts suspended")) {
        SceUID is = sceKernelCreateSema("isema", 0, 0, 1, NULL);
        unsigned fl = sceKernelCpuSuspendIntr();
        int dl = sceKernelDelayThread(1000);
        SceUInt t3 = 1000;
        int ws2 = sceKernelWaitSema(is, 1, &t3);
        sceKernelCpuResumeIntr(fl);
        rec("  DelayThread=%s WaitSema=%s timeout-after=%s\n", hx(dl), hx(ws2), ta(t3, 1000));
        sceKernelDeleteSema(is);
    }
}

/* ======================================================================= */

static const op_t S_ATTRSWEEP[] = { { O_ATTRSWEEP, 0, 0 }, { O_END, 0, 0 } };
static const op_t S_STACKFREE[] = { { O_STACKFREE, 0, 0 }, { O_END, 0, 0 } };

static void sec_attr(void) {
    SEC("thread attribute and stack");
    nm_reset();

    /* threadman.c:963-989 -- only PSP_THREAD_ATTR_VFPU may be changed; every
     * other bit answers 80020191 in both directions. */
    ST("ChangeCurrentThreadAttr: a VFPU thread adds, then removes, each single bit");
    for (int i = 0; i < 64; i++) g_attrres[i] = (int)0xEEEEEEEE;
    SceUID th = sceKernelCreateThread("attrsw", script_entry, 0x10, 0x2000,
                                      PSP_THREAD_ATTR_VFPU, NULL);
    go(th, S_ATTRSWEEP);
    wait_end(th, 200000);
    for (int half = 0; half < 2; half++) {
        for (int b = 0; b < 32; b += 8) {
            rec("  %s b%02d..: %s %s %s %s %s %s %s %s\n", half ? "remove" : "add   ", b,
                hx(g_attrres[half * 32 + b]), hx(g_attrres[half * 32 + b + 1]),
                hx(g_attrres[half * 32 + b + 2]), hx(g_attrres[half * 32 + b + 3]),
                hx(g_attrres[half * 32 + b + 4]), hx(g_attrres[half * 32 + b + 5]),
                hx(g_attrres[half * 32 + b + 6]), hx(g_attrres[half * 32 + b + 7]));
        }
    }
    rec("  attr afterwards: refer=%s attr=%08X\n", hx(g_wref_ret), (w32)g_wref.attr);
    reap(th);

    /* threadman.c:991-998 -- psprecomp reports the whole stack as free, where
     * the firmware measures the unused 0xFF fill. */
    ST("GetThreadStackFreeSize: main (0), a created 0x1000 thread, a running 0x1000 thread, a deleted id; CheckThreadStack");
    SceUID c = sceKernelCreateThread("sf", script_entry, 0x30, 0x1000, 0, NULL);
    rec("  main(0)=%s main(id)=%s created=%s\n",
        hx(sceKernelGetThreadStackFreeSize(0)), hx(sceKernelGetThreadStackFreeSize(g_main)),
        hx(sceKernelGetThreadStackFreeSize(c)));
    for (int i = 0; i < 3; i++) g_selfres[i] = (int)0xEEEEEEEE;
    th = sceKernelCreateThread("sf2", script_entry, 0x10, 0x1000, 0, NULL);
    go(th, S_STACKFREE);
    wait_end(th, 200000);
    rec("  running itself: (0)=%s (id)=%s CheckThreadStack=%s\n",
        hx(g_selfres[0]), hx(g_selfres[1]), hx(g_selfres[2]));
    rec("  after it finished=%s\n", hx(sceKernelGetThreadStackFreeSize(th)));
    reap(th);
    sceKernelDeleteThread(c);
    rec("  deleted=%s main CheckThreadStack=%s\n", hx(sceKernelGetThreadStackFreeSize(c)),
        hx(sceKernelCheckThreadStack()));

    /* threadman.c:1281-1291 -- the free size is the run of 0xFF bytes above
     * the bottom 0x10, whatever put them there, so a NO_FILLSTACK stack
     * scribbled with 0xCC reads 0 and one set to 0xFF reads nearly all. */
    ST("GetThreadStackFreeSize from inside running 0x1000 threads: plain, NO_FILLSTACK over 0xCC, NO_FILLSTACK over 0xFF, CLEAR_STACK");
    {
        static const struct { w32 attr; int fill; const char *l; } v[] = {
            { 0, -1, "plain" }, { ATTR_NO_FILLSTACK, 0xCC, "nofill+CC" },
            { ATTR_NO_FILLSTACK, 0xFF, "nofill+FF" }, { ATTR_CLEAR_STACK, -1, "clear" } };
        for (int i = 0; i < 4; i++) {
            for (int k = 0; k < 3; k++) g_selfres[k] = (int)0xEEEEEEEE;
            th = sceKernelCreateThread("sf3", script_entry, 0x10, 0x1000, v[i].attr, NULL);
            if (v[i].fill >= 0) {
                SceKernelThreadInfo ti;
                if (refer(th, &ti) == 0 && ti.stack) memset(ti.stack, v[i].fill, ti.stackSize);
            }
            go(th, S_STACKFREE);
            wait_end(th, 200000);
            rec("  %-10s create=%s (0)=%s CheckThreadStack=%s\n", v[i].l, cu(th),
                hx(g_selfres[0]), hx(g_selfres[2]));
            reap(th);
        }
    }
}

/* ======================================================================= */

static w32 g_cbcommon_obj;

static void rec_cbinfo(SceUID cb, const void *fn, const void *common) {
    SceKernelCallbackInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.size = sizeof ci;
    int r = sceKernelReferCallbackStatus(cb, &ci);
    rec("  refer=%s", hx(r));
    if (r) { rec("\n"); return; }
    char name[33];
    memcpy(name, ci.name, 32);
    name[32] = 0;
    rec(" size=%d name=\"%s\" thread=%s func=%s common=%s count=%d arg=%08X\n",
        (int)ci.size, name, nm(ci.threadId),
        (w32)ci.callback == (w32)fn ? "fn" : cls((w32)ci.callback),
        (w32)ci.common == (w32)common ? "given" : pv((w32)ci.common),
        ci.notifyCount, (w32)ci.notifyArg);
}

static void rec_cbhits(int from) {
    for (int i = from; i < g_cbhits && i < 8; i++)
        rec("    hit %d: arg1=%08X arg2=%08X common=%s thread=%s\n", i, (w32)g_cbh[i].a0,
            (w32)g_cbh[i].a1,
            g_cbh[i].common == (w32)&g_cbcommon_obj ? "given" : pv(g_cbh[i].common),
            nm(g_cbh[i].tid));
}

static SceUID g_renotify_cb;
static volatile int g_renotify_hits;
static int h_renotify(int a0, int a1, void *c) {
    (void)a0; (void)a1; (void)c;
    int n = ++g_renotify_hits;
    if (n == 1) sceKernelNotifyCallback(g_renotify_cb, 0x99);
    return 0;
}

static const op_t S_CBOWN[] = {
    { O_MKCB, 0, "cbW" }, { O_TAG, 0, "W0" }, { O_SLEEPCB, 0, "Wslept" }, { O_END, 0, 0 } };
static const op_t S_CBDELAY[] = {
    { O_MKCB, 1, "cbD" }, { O_DELAYCB, 50000, "Wd" }, { O_END, 0, 0 } };

static void sec_callbacks(void) {
    SEC("callbacks");
    nm_reset();
    int r, r2, r3;
    SceUID cb, th;

    ST("callback: create in main, refer, count");
    g_cbhits = 0; g_cbret = 0;
    cb = sceKernelCreateCallback("cbA", h_count, &g_cbcommon_obj);
    nm_add(cb, "cbA");
    rec("  create=%s\n", cu(cb));
    rec_cbinfo(cb, (const void *)h_count, &g_cbcommon_obj);
    rec("  GetCallbackCount=%s\n", hx(sceKernelGetCallbackCount(cb)));

    /* threadman.c:1937-1942, 1980-1999 -- notifies accumulate: the handler
     * runs once with (count, last arg, common); CheckCallback returns 1 if
     * anything ran. */
    ST("callback: notify 0x11, 0x22, 0x33; count; refer; CheckCallback twice");
    seq_clear();
    r = sceKernelNotifyCallback(cb, 0x11);
    r2 = sceKernelNotifyCallback(cb, 0x22);
    r3 = sceKernelNotifyCallback(cb, 0x33);
    rec("  notify=%s %s %s count=%s\n", hx(r), hx(r2), hx(r3), hx(sceKernelGetCallbackCount(cb)));
    rec_cbinfo(cb, (const void *)h_count, &g_cbcommon_obj);
    r = sceKernelCheckCallback();
    r2 = sceKernelCheckCallback();
    rec("  CheckCallback=%s then %s hits=%d\n", hx(r), hx(r2), g_cbhits);
    rec_cbhits(0);
    rec_cbinfo(cb, (const void *)h_count, &g_cbcommon_obj);

    /* threadman.c:2068-2078 -- cancel zeroes the count, and the arg with it. */
    ST("callback: notify 0x44 then CancelCallback");
    r = sceKernelNotifyCallback(cb, 0x44);
    r2 = sceKernelCancelCallback(cb);
    rec("  notify=%s cancel=%s count=%s\n", hx(r), hx(r2), hx(sceKernelGetCallbackCount(cb)));
    rec_cbinfo(cb, (const void *)h_count, &g_cbcommon_obj);
    rec("  CheckCallback=%s hits=%d\n", hx(sceKernelCheckCallback()), g_cbhits);

    /* threadman.c:2028-2036 -- a handler returning nonzero deletes its
     * callback ("Notify #2: Failed (800201a1)"). */
    ST("callback: handler returns 1; then notify, count, refer, delete");
    g_cbret = 1;
    int h0 = g_cbhits;
    r = sceKernelNotifyCallback(cb, 0x55);
    r2 = sceKernelCheckCallback();
    g_cbret = 0;
    rec("  notify=%s CheckCallback=%s hits+%d\n", hx(r), hx(r2), g_cbhits - h0);
    rec("  then notify=%s count=%s\n", hx(sceKernelNotifyCallback(cb, 1)), hx(sceKernelGetCallbackCount(cb)));
    rec_cbinfo(cb, (const void *)h_count, &g_cbcommon_obj);
    rec("  delete=%s\n", hx(sceKernelDeleteCallback(cb)));

    /* threadman.c:1244-1251, 1728-1735 -- a deleted callback answers
     * 800201a1 to everything, and so do 0 and ids that name nothing. */
    ST("callback: after delete; and id 0");
    cb = sceKernelCreateCallback("cbB", h_count, NULL);
    r = sceKernelDeleteCallback(cb);
    rec("  delete=%s notify=%s count=%s cancel=%s delete again=%s\n", hx(r),
        hx(sceKernelNotifyCallback(cb, 1)), hx(sceKernelGetCallbackCount(cb)),
        hx(sceKernelCancelCallback(cb)), hx(sceKernelDeleteCallback(cb)));
    rec_cbinfo(cb, NULL, NULL);
    rec("  id 0: notify=%s count=%s cancel=%s delete=%s\n", hx(sceKernelNotifyCallback(0, 1)),
        hx(sceKernelGetCallbackCount(0)), hx(sceKernelCancelCallback(0)),
        hx(sceKernelDeleteCallback(0)));
    rec_cbinfo(0, NULL, NULL);

    /* threadman.c:1943-1958, 1988-1990, 2126-2155 -- a callback belongs to the
     * thread that created it; a notify ends that thread's SleepThreadCB long
     * enough to run the handler, and it goes back to sleep; main's
     * CheckCallback does not run it. threadman.c:539-546 -- the callback goes
     * with its thread. */
    ST("callback: owned by a 0x30 thread in SleepThreadCB; main notifies, checks, delays, wakes it");
    fresh(); seq_clear();
    g_cbhits = 0;
    g_cbw[0] = 0;
    th = spawn("cbowner", 0x30, S_CBOWN);
    sceKernelDelayThread(1000);
    nm_add(g_cbw[0], "cbW");
    r = sceKernelNotifyCallback(g_cbw[0], 7);
    r2 = sceKernelCheckCallback();
    int hits_after_check = g_cbhits;
    tag("m1"); sceKernelDelayThread(1000); tag("m2");
    SceKernelThreadInfo ti;
    refer(th, &ti);
    int st_after = ti.status;
    r3 = sceKernelWakeupThread(th); tag("m3");
    wait_end(th, 200000);
    rec("  worker's callback: %s\n", cu(g_cbw[0]));
    rec("  notify=%s main CheckCallback=%s hits after check=%d; owner status after handler=%08X wakeup=%s\n",
        hx(r), hx(r2), hits_after_check, (w32)st_after, hx(r3));
    rec_cbhits(0);
    rec_seq();
    rec_cbinfo(g_cbw[0], (const void *)h_count, (void *)1);
    reap(th);
    rec("  after the owner is deleted: delete callback=%s\n", hx(sceKernelDeleteCallback(g_cbw[0])));

    /* threadman.c:2093-2122 -- a notify runs the handler during
     * DelayThreadCB, which then sleeps out the rest of its delay. */
    ST("callback: owner in DelayThreadCB(50ms); main notifies, delays 5ms, then waits for it");
    fresh(); seq_clear();
    g_cbhits = 0;
    g_cbw[1] = 0;
    th = spawn("cbdelay", 0x30, S_CBDELAY);
    sceKernelDelayThread(1000);
    r = sceKernelNotifyCallback(g_cbw[1], 1);
    tag("m1"); sceKernelDelayThread(5000); tag("m2");
    wait_end(th, 500000);
    rec("  notify=%s hits=%d\n", hx(r), g_cbhits);
    rec_seq();
    reap(th);
    sceKernelDeleteCallback(g_cbw[1]);

    /* waitq.h:64-73, threadman.c:2041-2062 -- a CB wait delivers after its
     * argument checks and before blocking: not for an illegal count, yes for
     * a wait that succeeds at once or times out. */
    ST("callback: which CB waits deliver a pending notify (WaitSemaCB, WaitThreadEndCB, DelayThreadCB)");
    g_cbhits = 0;
    cb = sceKernelCreateCallback("cbM", h_count, NULL);
    SceUID s = sceKernelCreateSema("cbsema", 0, 0, 1, NULL);
    SceUID gone = sceKernelCreateSema("gone", 0, 0, 1, NULL);
    sceKernelDeleteSema(gone);
    SceUID fin = spawn("fin", 0x30, S_RET5);
    wait_end(fin, 200000);
    SceUID sl = spawn("slp", 0x30, S_SLEEP_WS);
    sceKernelDelayThread(1000);
    SceUInt t;
    int h;
#define CBW(label, call) do { SceUInt t_in = t; h = g_cbhits; sceKernelNotifyCallback(cb, 1); \
        r = (call); \
        rec("  %-36s = %s timeout-after=%s delivered=%d pending=%d\n", label, hx(r), ta(t, t_in), \
            g_cbhits - h, sceKernelGetCallbackCount(cb)); sceKernelCancelCallback(cb); } while (0)
    t = 1000; CBW("WaitSemaCB(count 0)", sceKernelWaitSemaCB(s, 0, &t));
    t = 1000; CBW("WaitSemaCB(count 2 > max 1)", sceKernelWaitSemaCB(s, 2, &t));
    t = 1000; CBW("WaitSemaCB(deleted sema)", sceKernelWaitSemaCB(gone, 1, &t));
    sceKernelSignalSema(s, 1);
    t = 1000; CBW("WaitSemaCB(available)", sceKernelWaitSemaCB(s, 1, &t));
    t = 1000; CBW("WaitSemaCB(times out, 1000us)", sceKernelWaitSemaCB(s, 1, &t));
    t = 0;    CBW("WaitSemaCB(timeout 0)", sceKernelWaitSemaCB(s, 1, &t));
    t = 1000; CBW("WaitThreadEndCB(finished, returned 5)", sceKernelWaitThreadEndCB(fin, &t));
    t = 1000; CBW("WaitThreadEndCB(0)", sceKernelWaitThreadEndCB(0, &t));
    t = 2000; CBW("WaitThreadEndCB(sleeping, 2000us)", sceKernelWaitThreadEndCB(sl, &t));
    t = 0;    CBW("DelayThreadCB(1000)", sceKernelDelayThreadCB(1000));
    t = 0;    CBW("DelayThread(1000) (not CB)", sceKernelDelayThread(1000));
#undef CBW
    reap(fin); reap(sl);
    sceKernelDeleteSema(s);
    sceKernelDeleteCallback(cb);

    /* threadman.c:1995-1999 -- a handler that notifies itself is entered
     * again by the next CheckCallback, not in a loop inside this one. */
    ST("callback: handler notifies itself once; CheckCallback three times");
    g_renotify_hits = 0;
    g_renotify_cb = sceKernelCreateCallback("cbR", h_renotify, NULL);
    sceKernelNotifyCallback(g_renotify_cb, 1);
    r = sceKernelCheckCallback(); int n1 = g_renotify_hits;
    r2 = sceKernelCheckCallback(); int n2 = g_renotify_hits;
    r3 = sceKernelCheckCallback(); int n3 = g_renotify_hits;
    rec("  check=%s hits=%d; check=%s hits=%d; check=%s hits=%d\n", hx(r), n1, hx(r2), n2, hx(r3), n3);
    sceKernelDeleteCallback(g_renotify_cb);

    /* misc.c:32-43 -- registering a power callback notifies it once. */
    ST("callback: scePowerRegisterCallback(-1, cb) notifies it?");
    g_cbhits = 0;
    cb = sceKernelCreateCallback("cbP", h_count, NULL);
    r = scePowerRegisterCallback(-1, cb);
    r2 = sceKernelGetCallbackCount(cb);
    r3 = sceKernelCheckCallback();
    rec("  register=%s count=%s CheckCallback=%s hits=%d first-arg=%08X\n", hx(r), hx(r2), hx(r3),
        g_cbhits, g_cbhits ? (w32)g_cbh[0].a0 : 0);
    rec("  unregister=%s", r >= 0 ? hx(scePowerUnregisterCallback(r)) : "(not registered)");
    rec(" delete=%s\n", hx(sceKernelDeleteCallback(cb)));
    rec("  main CheckCallback with nothing pending=%s\n", hx(sceKernelCheckCallback()));
}

/* ======================================================================= */

static SceUInt h_alarm(void *common);

static void sec_idlist(void) {
    SEC("thread manager id lists");
    nm_reset();
    static SceUID buf[128];
    int cnt, r;

    /* hle.h:549-561, threadman.c:1883-1907 -- types 1..14 and 0x40..0x43 are
     * valid, everything else ILLEGAL_TYPE; a bad type or negative size leaves
     * the count word alone; the count is how many exist, the return how many
     * were written. */
    ST("GetThreadmanIdList: invalid types 0, 15, 16, 0x3F, 0x44, 0x80, -1; size -1; NULL buffer size 0; size 1");
    static const int bad[] = { 0, 15, 16, 0x3F, 0x44, 0x80, -1 };
    for (int i = 0; i < 7; i++) {
        cnt = 0x55AA;
        r = sceKernelGetThreadmanIdList(bad[i], buf, 128, &cnt);
        rec("  type %08X: %s count word=%08X\n", (w32)bad[i], hx(r), (w32)cnt);
    }
    cnt = 0x55AA; r = sceKernelGetThreadmanIdList(1, buf, -1, &cnt);
    rec("  size -1: %s count word=%08X\n", hx(r), (w32)cnt);
    cnt = 0x55AA; r = sceKernelGetThreadmanIdList(1, NULL, 0, &cnt);
    rec("  NULL, size 0: %s count>=2 %s\n", hx(r), cnt >= 2 && cnt != 0x55AA ? "yes" : "no");
    cnt = 0x55AA; r = sceKernelGetThreadmanIdList(1, buf, 1, &cnt);
    rec("  size 1: %s count>1 %s\n", hx(r), cnt > 1 && cnt != 0x55AA ? "yes" : "no");

    static const int types[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 0x40, 0x41, 0x42, 0x43 };
    enum { NT = 18 };
    int base[NT], baseret[NT];
    ST("GetThreadmanIdList: baseline count for types 1..14 and 0x40..0x43");
    for (int i = 0; i < NT; i++) {
        cnt = -1;
        baseret[i] = sceKernelGetThreadmanIdList(types[i], buf, 128, &cnt);
        base[i] = cnt;
        rec("  type %02X: %s count=%d\n", types[i], baseret[i] < 0 ? hx(baseret[i]) : "ok", cnt);
    }

    ST("GetThreadmanIdList: one object of each kind, and threads sleeping, delaying, suspended, dormant");
    static SceLwMutexWorkarea lw;
    struct { SceUID id; const char *n; } o[20];
    int no = 0;
#define OBJ(expr, name) do { o[no].id = (expr); o[no].n = name; nm_add(o[no].id, name); \
        rec("  %-8s %s\n", name, cu(o[no].id)); no++; } while (0)
    OBJ(sceKernelCreateSema("il_sema", 0, 0, 1, NULL), "sema");
    OBJ(sceKernelCreateEventFlag("il_evf", 0, 0, NULL), "evf");
    OBJ(sceKernelCreateMbx("il_mbx", 0, NULL), "mbx");
    OBJ(sceKernelCreateVpl("il_vpl", 2, 0, 0x1000, NULL), "vpl");
    OBJ(sceKernelCreateFpl("il_fpl", 2, 0, 0x100, 4, NULL), "fpl");
    OBJ(sceKernelCreateMsgPipe("il_mpp", 2, 0, (void *)0x100, NULL), "msgpipe");
    OBJ(sceKernelCreateCallback("il_cb", h_count, NULL), "callback");
    OBJ(sceKernelSetAlarm(10000000, h_alarm, NULL), "alarm");
    OBJ(sceKernelCreateVTimer("il_vt", NULL), "vtimer");
    OBJ(sceKernelCreateMutex("il_mtx", 0, 0, NULL), "mutex");
    r = sceKernelCreateLwMutex(&lw, "il_lw", 0, 0, NULL);
    rec("  lwmutex  %s\n", hx(r));
    OBJ(sceKernelCreateTlspl("il_tls", 2, 0, 0x10, 2, NULL), "tlspl");
    OBJ(spawn("il_sleep", 0x30, S_SLEEP_WS), "t_sleep");
    OBJ(spawn("il_delay", 0x30, S_DELAY1S), "t_delay");
    OBJ(spawn("il_susp", 0x30, S_SLEEP_WS), "t_susp");
    OBJ(mk("il_dorm", 0x30), "t_dormant");
#undef OBJ
    /* o[]: 0 sema 1 evf 2 mbx 3 vpl 4 fpl 5 msgpipe 6 callback 7 alarm
     * 8 vtimer 9 mutex 10 tlspl 11 t_sleep 12 t_delay 13 t_susp 14 t_dormant
     * 15 main (the lwmutex is not a threadman object with an id here). */
    o[no].id = g_main; o[no].n = "main"; no++;
    sceKernelSuspendThread(o[13].id);
    sceKernelDelayThread(1000);

    ST("GetThreadmanIdList: per type, count change and which of the new objects are listed");
    for (int i = 0; i < NT; i++) {
        cnt = -1;
        r = sceKernelGetThreadmanIdList(types[i], buf, 128, &cnt);
        char has[200];
        int n = 0;
        has[0] = 0;
        for (int k = 0; k < no; k++)
            for (int j = 0; j < r && j < 128; j++)
                if (buf[j] == o[k].id) { n += snprintf(has + n, sizeof has - n, " %s", o[k].n); break; }
        rec("  type %02X: %s delta=%+d ret==min(count,128) %s has:%s\n", types[i],
            r < 0 ? hx(r) : "ok", cnt - base[i], r == (cnt < 128 ? cnt : 128) ? "yes" : "no",
            n ? has : " -");
    }

    ST("GetThreadmanIdType of each object, main, 0 and a deleted id");
    for (int k = 0; k < no; k++)
        rec("  %-9s %s\n", o[k].n, hx(sceKernelGetThreadmanIdType(o[k].id)));
    rec("  zero      %s\n", hx(sceKernelGetThreadmanIdType(0)));

    sceKernelDeleteSema(o[0].id);
    sceKernelDeleteEventFlag(o[1].id);
    sceKernelDeleteMbx(o[2].id);
    sceKernelDeleteVpl(o[3].id);
    sceKernelDeleteFpl(o[4].id);
    sceKernelDeleteMsgPipe(o[5].id);
    sceKernelDeleteCallback(o[6].id);
    sceKernelCancelAlarm(o[7].id);
    sceKernelDeleteVTimer(o[8].id);
    sceKernelDeleteMutex(o[9].id);
    sceKernelDeleteLwMutex(&lw);
    sceKernelDeleteTlspl(o[10].id);
    for (int k = 11; k <= 14; k++) reap(o[k].id);
    rec("  deleted sema: %s\n", hx(sceKernelGetThreadmanIdType(o[0].id)));
}

/* ======================================================================= */

static w32 g_tlsinfo[32];

static void rec_tlsinfo(SceUID t) {
    memset(g_tlsinfo, 0xEE, sizeof g_tlsinfo);
    g_tlsinfo[0] = 64;
    int r = sceKernelReferTlsplStatus(t, g_tlsinfo);
    rec("  refer=%s", hx(r));
    if (r) { rec("\n"); return; }
    char name[33];
    memcpy(name, &g_tlsinfo[1], 32);
    name[32] = 0;
    rec(" size=%u name=\"%s\" +36..: %08X %08X %08X %08X %08X %08X %08X\n", g_tlsinfo[0], name,
        g_tlsinfo[9], g_tlsinfo[10], g_tlsinfo[11], g_tlsinfo[12], g_tlsinfo[13],
        g_tlsinfo[14], g_tlsinfo[15]);
}

static const char *off(w32 a, w32 ref) {
    char *b = sb();
    if (!a) return "NULL";
    snprintf(b, 40, "%+d", (int)(a - ref));
    return b;
}

static void tls_waiters(SceUInt attr) {
    static const op_t Q1[] = { { O_TLSGET, 1, "W30" }, { O_DELAY, 1000, 0 }, { O_END, 0, 0 } };
    static const op_t Q2[] = { { O_TLSGET, 2, "W34" }, { O_DELAY, 1000, 0 }, { O_END, 0, 0 } };
    static const op_t Q3[] = { { O_TLSGET, 3, "W31" }, { O_DELAY, 1000, 0 }, { O_END, 0, 0 } };
    fresh(); seq_clear();
    g_tls = sceKernelCreateTlspl("tq", 2, attr, 0x10, 1, NULL);
    void *mine = sceKernelGetTlsAddr(g_tls);
    SceUID w34 = spawn("W34", 0x34, Q2); sceKernelDelayThread(1000);
    SceUID w30 = spawn("W30", 0x30, Q1); sceKernelDelayThread(1000);
    SceUID w31 = spawn("W31", 0x31, Q3); sceKernelDelayThread(1000);
    rec("  create=%s main's block %s\n", cu(g_tls), mine ? "ok" : "NULL");
    rec_tlsinfo(g_tls);
    tag("m-free");
    int r = sceKernelFreeTlspl(g_tls);
    wait_end(w30, 500000); wait_end(w31, 500000); wait_end(w34, 500000);
    rec("  free=%s\n", hx(r));
    rec_seq();
    reap(w30); reap(w31); reap(w34);
    rec("  delete=%s\n", hx(sceKernelDeleteTlspl(g_tls)));
}

static void sec_tls(void) {
    SEC("TLS pools");
    nm_reset();
    int r;
    SceUID t;

    /* kernobj.c:1702-1740, 2011-2023 -- status is size, name, attr, index,
     * blockSize, numBlocks, freeBlocks, numWaitThreads (60 bytes). */
    ST("tlspl: create (partition 2, attr 0, 3 blocks of 0x10), refer");
    t = sceKernelCreateTlspl("tls", 2, 0, 0x10, 3, NULL);
    rec("  create=%s\n", cu(t));
    rec_tlsinfo(t);

    /* kernobj.c:1704-1707 -- the same block on every call ("Twice: OK
     * (+0000)"); kernobj.c:1896-1903 -- after a free the search starts past
     * the last block handed out: +0000, +0010, +0020, +0000. */
    ST("tlspl: GetTlsAddr twice; then free/get four times (offsets from the first block)");
    w32 a0 = (w32)sceKernelGetTlsAddr(t);
    w32 a1 = (w32)sceKernelGetTlsAddr(t);
    rec("  first=%s second=%s\n", a0 ? "ok" : "NULL", off(a1, a0));
    rec_tlsinfo(t);
    for (int i = 0; i < 4; i++) {
        int f = sceKernelFreeTlspl(t);
        w32 b = (w32)sceKernelGetTlsAddr(t);
        rec("  free=%s get=%s\n", hx(f), off(b, a0));
    }

    /* kernobj.c:1905-1913, 1985-1992 -- handed out zeroed; a second get does
     * not clear; the free itself wipes the block. */
    ST("tlspl: write 0xCCCCCCCC into the block, get again, free, read, get, read");
    {
        volatile w32 *p = (volatile w32 *)sceKernelGetTlsAddr(t);
        if (p) {
            p[0] = 0xCCCCCCCC;
            volatile w32 *q = (volatile w32 *)sceKernelGetTlsAddr(t);
            w32 v1 = q ? q[0] : 0;
            int f = sceKernelFreeTlspl(t);
            w32 v2 = p[0];
            volatile w32 *s = (volatile w32 *)sceKernelGetTlsAddr(t);
            w32 v3 = s ? s[0] : 0;
            rec("  same block=%s read=%08X free=%s read after free=%08X next get=%s read=%08X\n",
                q == p ? "yes" : "no", v1, hx(f), v2, off((w32)s, (w32)p), v3);
        } else rec("  get returned NULL\n");
    }

    /* kernobj.c:1993-1998 -- free with nothing held is OK. */
    ST("tlspl: free twice in a row, free on a deleted pool id");
    r = sceKernelFreeTlspl(t);
    int r2 = sceKernelFreeTlspl(t);
    SceUID g = sceKernelCreateTlspl("gone", 2, 0, 4, 1, NULL);
    sceKernelDeleteTlspl(g);
    rec("  free=%s free=%s deleted: free=%s get=%08X refer=%s delete=%s\n", hx(r), hx(r2),
        hx(sceKernelFreeTlspl(g)), (w32)sceKernelGetTlsAddr(g),
        hx(sceKernelReferTlsplStatus(g, g_tlsinfo)), hx(sceKernelDeleteTlspl(g)));

    /* kernobj.c:1709-1711, 1807-1817 -- the index is a slot: the second pool
     * reports 1, and a third made after the first is deleted reports 0. */
    ST("tlspl: index of a second pool, and of a third after the first is deleted");
    SceUID t2 = sceKernelCreateTlspl("tls2", 2, 0, 0x10, 1, NULL);
    rec("  second: %s", cu(t2)); rec_tlsinfo(t2);
    rec("  delete first=%s\n", hx(sceKernelDeleteTlspl(t)));
    SceUID t3 = sceKernelCreateTlspl("tls3", 2, 0, 0x10, 1, NULL);
    rec("  third: %s", cu(t3)); rec_tlsinfo(t3);
    sceKernelDeleteTlspl(t2);
    sceKernelDeleteTlspl(t3);

    /* kernobj.c:1712-1716 -- attribute mask 0x41FF. */
    ST("tlspl: create with each single attribute bit");
    for (int b = 0; b < 32; b += 8) {
        char line[200];
        int n = 0;
        for (int k = b; k < b + 8; k++) {
            SceUID x = sceKernelCreateTlspl("attr", 2, 1u << k, 4, 1, NULL);
            n += snprintf(line + n, sizeof line - n, " %s", x > 0 ? "ok" : hx(x));
            if (x > 0) sceKernelDeleteTlspl(x);
        }
        rec("  b%02d..:%s\n", b, line);
    }

    /* kernobj.c:131-143 -- 2 and 6 allowed, 1/3/4 ILLEGAL_PERM, the rest
     * (including 8 and 9) ILLEGAL_PARTITION. 5 is not tried. */
    ST("tlspl: partitions -1, 0, 1, 2, 3, 4, 6, 7, 8, 9, 10");
    {
        static const int parts[] = { -1, 0, 1, 2, 3, 4, 6, 7, 8, 9, 10 };
        char line[200];
        int n = 0;
        for (int i = 0; i < 11; i++) {
            SceUID x = sceKernelCreateTlspl("part", parts[i], 0, 4, 1, NULL);
            n += snprintf(line + n, sizeof line - n, " %d:%s", parts[i], x > 0 ? "ok" : hx(x));
            if (x > 0) sceKernelDeleteTlspl(x);
        }
        rec(" %s\n", line);
    }

    /* kernobj.c:1776-1777, 1797-1803 -- zero or negative sizes are
     * ILLEGAL_MEMSIZE; 0x1000 x 0x100 succeeds, 0x10000 x 0x100 is
     * NO_MEMORY, 0x1000000 x 0x100 (2^32) ILLEGAL_MEMSIZE. */
    ST("tlspl: block size / count 0/1, 1/0, -1/1, 1/-1, 0x100/0x1000, 0x100/0x10000, 0x100/0x1000000");
    {
        static const w32 bs[][2] = { { 0, 1 }, { 1, 0 }, { 0xFFFFFFFF, 1 }, { 1, 0xFFFFFFFF },
                                     { 0x100, 0x1000 }, { 0x100, 0x10000 }, { 0x100, 0x1000000 } };
        for (int i = 0; i < 7; i++) {
            SceUID x = sceKernelCreateTlspl("size", 2, 0, bs[i][0], bs[i][1], NULL);
            rec("  %08X x %08X: %s\n", bs[i][0], bs[i][1], cu(x));
            if (x > 0) sceKernelDeleteTlspl(x);
        }
    }

    /* kernobj.c:1780-1795 -- the option block's alignment rounds each block
     * up: 0x100 -> 0x100 apart, 1 and 0 -> 4 apart; a non-power of two is
     * refused with ILLEGAL_PARTITION. */
    ST("tlspl: option alignment 0x100, 1, 0, 3 with 1-byte blocks (distance between two threads' blocks)");
    {
        static const w32 al[] = { 0x100, 1, 0, 3 };
        static const op_t G1[] = { { O_TLSGET, 1, 0 }, { O_END, 0, 0 } };
        for (int i = 0; i < 4; i++) {
            w32 opt[2] = { 8, al[i] };
            g_tls = sceKernelCreateTlspl("align", 2, 0, 1, 2, opt);
            if (g_tls <= 0) { rec("  align %X: %s\n", al[i], hx(g_tls)); continue; }
            w32 m = (w32)sceKernelGetTlsAddr(g_tls);
            g_tlsaddr[1] = 0;
            SceUID w = spawn("tlsw", 0x10, G1);
            wait_end(w, 200000);
            reap(w);
            rec("  align %X: ok, other thread's block at %s, main's block &0xFF=%02X\n", al[i],
                off(g_tlsaddr[1], m), m & 0xFF);
            sceKernelDeleteTlspl(g_tls);
        }
    }

    /* kernobj.c:1875-1885 -- with no block free, GetTlsAddr waits; the
     * release order follows attribute 0x100; kernobj.c:1966-1983 -- a block
     * comes back when its thread ends. Waiters arrive 0x34, 0x30, 0x31. */
    ST("tlspl: one block held by main; waiters arrive 0x34, 0x30, 0x31; FIFO pool; main frees");
    tls_waiters(0);
    ST("tlspl: the same with attribute 0x100 (priority order)");
    tls_waiters(0x100);

    /* kernobj.c:1838-1850 -- a block lent to another thread makes delete
     * answer TLSPL_BUSY; the caller's own does not count; waiters do not. */
    ST("tlspl: delete while a 0x10 thread holds a block (main holds one too), then after it exits");
    {
        static const op_t H1[] = { { O_TLSGET, 1, "W" }, { O_SLEEP, 0, "Ws" }, { O_END, 0, 0 } };
        g_tls = sceKernelCreateTlspl("busy", 2, 0, 0x10, 2, NULL);
        SceUID w = spawn("holder", 0x10, H1);
        void *mine = sceKernelGetTlsAddr(g_tls);
        r = sceKernelDeleteTlspl(g_tls);
        sceKernelWakeupThread(w);
        wait_end(w, 200000);
        r2 = sceKernelDeleteTlspl(g_tls);
        rec("  main's block %s; delete while lent=%s; after the holder exited=%s\n",
            mine ? "ok" : "NULL", hx(r), hx(r2));
        reap(w);
        if (r2 != 0) sceKernelDeleteTlspl(g_tls);
    }

    ST("tlspl: delete while a 0x30 thread waits for the only block");
    {
        static const op_t W1[] = { { O_TLSGET, 1, "W" }, { O_END, 0, 0 } };
        fresh(); seq_clear();
        g_tls = sceKernelCreateTlspl("waitdel", 2, 0, 0x10, 1, NULL);
        void *mine = sceKernelGetTlsAddr(g_tls);
        SceUID w = spawn("waiter", 0x30, W1);
        sceKernelDelayThread(1000);
        rec_tlsinfo(g_tls);
        tag("m1"); r = sceKernelDeleteTlspl(g_tls); tag("m2");
        wait_end(w, 200000);
        rec("  main's block %s; delete=%s\n", mine ? "ok" : "NULL", hx(r));
        rec_seq();
        reap(w);
        if (r != 0) { sceKernelFreeTlspl(g_tls); sceKernelDeleteTlspl(g_tls); }
    }

    /* kernobj.c:1716-1719 -- sixteen pools at most; the 17th fails. */
    ST("tlspl: create pools until one fails (at most 40)");
    {
        static SceUID many[40];
        int n = 0, fail = 0;
        for (int i = 0; i < 40; i++) {
            many[i] = sceKernelCreateTlspl("many", 2, 0, 4, 1, NULL);
            if (many[i] > 0) n++;
            else { fail = many[i]; break; }
        }
        g_tlsinfo[10] = 0xEEEEEEEE;
        if (n) rec_tlsinfo(many[n - 1]);
        for (int i = 0; i < n; i++) sceKernelDeleteTlspl(many[i]);
        rec("  created %d, then %s\n", n, hx(fail));
    }

    /* kernobj.c:140-146 -- the vpl table since step 107; its 5 is ILLEGAL_PERM,
     * on no measurement. Risky: kernobj.c:129-130 records that a vpl in
     * partition 5 is said to crash hardware. */
    if (!ST("tlspl: partition 5")) {
        SceUID x = sceKernelCreateTlspl("part5", 5, 0, 4, 1, NULL);
        rec("  5:%s\n", x > 0 ? "ok" : hx(x));
        if (x > 0) sceKernelDeleteTlspl(x);
    }
}

/* ======================================================================= */

static volatile int g_alhits;
static volatile w32 g_altime[8];
static volatile w32 g_alcommon;
static volatile int g_altid;
static w32 g_alret[8];
static w32 g_alobj;

static SceUInt h_alarm(void *common) {
    int i = g_alhits;
    if (i < 8) g_altime[i] = sceKernelGetSystemTimeLow();
    g_alcommon = (w32)common;
    if (i == 0) g_altid = sceKernelGetThreadId();
    g_alhits = i + 1;
    return i < 8 ? g_alret[i] : 0;
}

static void al_reset(void) {
    g_alhits = 0;
    g_altid = 0x7EEEEEEE;
    g_alcommon = 0;
    memset((void *)g_altime, 0, sizeof g_altime);
    memset(g_alret, 0, sizeof g_alret);
}

static const char *tidclass(int v) {
    if (v == g_main) return "main";
    if (v == 0x7EEEEEEE) return "(not run)";
    if (v <= 0) return hx(v);
    return "other thread";
}

static void rec_alinfo(SceUID a, SceInt64 t0, w32 usec) {
    SceKernelAlarmInfo ai;
    memset(&ai, 0, sizeof ai);
    ai.size = sizeof ai;
    int r = sceKernelReferAlarmStatus(a, &ai);
    rec("  refer=%s", hx(r));
    if (r) { rec("\n"); return; }
    SceInt64 sched = ((SceInt64)ai.schedule.hi << 32) | ai.schedule.low;
    SceInt64 d = sched - t0;
    rec(" size=%d schedule-set_time in [%u, %u+5000): %s handler=%s common=%s\n", (int)ai.size,
        usec, usec, (d >= (SceInt64)usec && d < (SceInt64)usec + 5000) ? "yes" : "no",
        (w32)ai.handler == (w32)h_alarm ? "fn" : cls((w32)ai.handler),
        (w32)ai.common == (w32)&g_alobj ? "given" : pv((w32)ai.common));
}

static void sec_alarm(void) {
    SEC("alarms");
    nm_reset();
    SceUID a;
    int r;

    /* ktimer.c:3-8 -- the handler runs on no thread (GetThreadId from it);
     * ktimer.c:108-135 -- refer is 20 bytes: size, schedule, handler, common. */
    ST("alarm: SetAlarm(2000us), refer, delay 10ms");
    al_reset();
    SceInt64 t0 = sceKernelGetSystemTimeWide();
    a = sceKernelSetAlarm(2000, h_alarm, &g_alobj);
    rec("  set=%s\n", cu(a));
    rec_alinfo(a, t0, 2000);
    sceKernelDelayThread(10000);
    /* ktimer.c:12-17 -- a deadline is only noticed at a firmware call, so
     * under psprecomp an alarm that falls due while every thread waits fires
     * when the delay returns; on hardware it is an interrupt. */
    rec("  hits=%d common=%s GetThreadId in handler=%s fired>=2000us after set: %s"
        " fired before the 10ms delay ended: %s\n", g_alhits,
        g_alcommon == (w32)&g_alobj ? "given" : pv(g_alcommon), tidclass(g_altid),
        g_alhits && (g_altime[0] - (w32)t0) >= 2000 ? "yes" : "no",
        g_alhits && (g_altime[0] - (w32)t0) < 9000 ? "yes" : "no");
    rec("  after it fired: cancel=%s", hx(sceKernelCancelAlarm(a)));
    rec_alinfo(a, t0, 2000);

    /* ktimer.c:137-141, 167-170 -- a nonzero return re-arms the alarm that
     * many microseconds later; zero retires it. */
    ST("alarm: handler returns 1000, 1000, then 0; main spins 20ms");
    al_reset();
    g_alret[0] = 1000; g_alret[1] = 1000; g_alret[2] = 0;
    a = sceKernelSetAlarm(1000, h_alarm, &g_alobj);
    spin_us(20000);
    rec("  set=%s hits=%d gaps>=1000: %s %s cancel after=%s\n", cu(a), g_alhits,
        g_alhits > 1 && g_altime[1] - g_altime[0] >= 1000 ? "yes" : "no",
        g_alhits > 2 && g_altime[2] - g_altime[1] >= 1000 ? "yes" : "no",
        hx(sceKernelCancelAlarm(a)));

    /* ktimer.c:101-106 */
    ST("alarm: cancel before it fires (20ms), cancel again, delay 30ms");
    al_reset();
    a = sceKernelSetAlarm(20000, h_alarm, &g_alobj);
    r = sceKernelCancelAlarm(a);
    int r2 = sceKernelCancelAlarm(a);
    sceKernelDelayThread(30000);
    rec("  set=%s cancel=%s again=%s hits=%d\n", cu(a), hx(r), hx(r2), g_alhits);

    /* ktimer.c:89-99 */
    ST("alarm: SetSysClockAlarm({2000, 0}), delay 10ms");
    al_reset();
    SceKernelSysClock clk = { 2000, 0 };
    t0 = sceKernelGetSystemTimeWide();
    a = sceKernelSetSysClockAlarm(&clk, h_alarm, &g_alobj);
    rec("  set=%s", cu(a));
    rec_alinfo(a, t0, 2000);
    sceKernelDelayThread(10000);
    rec("  hits=%d cancel after=%s\n", g_alhits, hx(sceKernelCancelAlarm(a)));

    ST("alarm: SetAlarm(0), SetAlarm(1); delay 1ms");
    al_reset();
    a = sceKernelSetAlarm(0, h_alarm, &g_alobj);
    int h_immediate = g_alhits;
    sceKernelDelayThread(1000);
    int h0 = g_alhits;
    SceUID a2 = sceKernelSetAlarm(1, h_alarm, &g_alobj);
    sceKernelDelayThread(1000);
    rec("  SetAlarm(0)=%s hits right after=%d after 1ms=%d; SetAlarm(1)=%s hits=%d\n", cu(a),
        h_immediate, h0, cu(a2), g_alhits);
    sceKernelCancelAlarm(a); sceKernelCancelAlarm(a2);

    /* ktimer.c:108-116 -- only as many bytes as the size field allows. */
    ST("alarm: ReferAlarmStatus with size field 0, 1, 4, 5, 20, 24");
    al_reset();
    a = sceKernelSetAlarm(10000000, h_alarm, &g_alobj);
    {
        static const int rooms[] = { 0, 1, 4, 5, 20, 24 };
        for (int i = 0; i < 6; i++) {
            memset(g_rb1, 0xEE, 64);
            memset(g_rb2, 0x11, 64);
            memcpy(g_rb1, &rooms[i], 4);
            memcpy(g_rb2, &rooms[i], 4);
            int x1 = sceKernelReferAlarmStatus(a, (SceKernelAlarmInfo *)g_rb1);
            int x2 = sceKernelReferAlarmStatus(a, (SceKernelAlarmInfo *)g_rb2);
            unsigned char pre[4];
            memcpy(pre, &rooms[i], 4);
            int ext = 0;
            for (int k = 0; k < 64; k++) {
                unsigned char e1 = k < 4 ? pre[k] : 0xEE, e2 = k < 4 ? pre[k] : 0x11;
                if (g_rb1[k] != e1 || g_rb2[k] != e2) ext = k + 1;
            }
            w32 sz;
            memcpy(&sz, g_rb1, 4);
            rec("  room %2d: ret=%s/%s written-to=%d size=%08X\n", rooms[i], hx(x1), hx(x2), ext, sz);
        }
    }
    rec("  cancel=%s\n", hx(sceKernelCancelAlarm(a)));

    ST("alarm: cancel and refer with id 0 and a cancelled id");
    {
        SceKernelAlarmInfo ai;
        ai.size = sizeof ai;
        rec("  cancel(0)=%s refer(0)=%s cancelled: refer=%s\n", hx(sceKernelCancelAlarm(0)),
            hx(sceKernelReferAlarmStatus(0, &ai)), hx(sceKernelReferAlarmStatus(a, &ai)));
    }

    /* ktimer.c:148-155, 187 -- a handler's return re-arms from the moment the
     * alarm was due, not when it ran (step 116: the first gap came out under
     * 1000us). Hit k then lands k*1000us after hit 0 plus the difference of
     * two lateness values; re-arming from the run would add every lateness. */
    ST("alarm: handler returns 1000 five times, then 0; main spins 20ms: lateness of the first hit, each gap, each hit against hit 0");
    {
        al_reset();
        for (int i = 0; i < 5; i++) g_alret[i] = 1000;
        w32 t0 = sceKernelGetSystemTimeLow();
        a = sceKernelSetAlarm(1000, h_alarm, &g_alobj);
        spin_us(20000);
        rec("  set=%s hits=%d\n", cu(a), g_alhits);
        if (g_alhits > 0) {
            w32 late = g_altime[0] - t0;
            rec("  hit 0 after set: %s\n", late < 1000 ? "<1000" : late < 1050 ? "1000..1049" :
                late < 1200 ? "1050..1199" : ">=1200");
        }
        for (int k = 1; k < g_alhits && k < 6; k++) {
            w32 gap = g_altime[k] - g_altime[k - 1];
            int drift = (int)(g_altime[k] - g_altime[0]) - k * 1000;
            rec("  hit %d: gap %s; from hit 0, k*1000 %s\n", k,
                gap < 950 ? "<950" : gap < 1000 ? "950..999" : gap < 1050 ? "1000..1049" : ">=1050",
                drift < -50 ? "-more than 50" : drift <= 50 ? "+-50" : "+more than 50");
        }
        rec("  cancel after=%s\n", hx(sceKernelCancelAlarm(a)));
    }
}

/* ======================================================================= */

static volatile int g_vthits;
static w32 g_vtsched[4][2], g_vtreal[4][2];
static volatile int g_vtuidok[4];
static volatile w32 g_vtcommon;
static w32 g_vtret[4];
static SceUID g_vtid;
static volatile int g_vtdel_ret;
static w32 g_vtobj;

static SceUInt h_vt(SceUID uid, SceKernelSysClock *s, SceKernelSysClock *r, void *common) {
    int i = g_vthits;
    if (i < 4) {
        g_vtsched[i][0] = s ? s->low : 0xEEEEEEEE;
        g_vtsched[i][1] = s ? s->hi : 0xEEEEEEEE;
        g_vtreal[i][0] = r ? r->low : 0xEEEEEEEE;
        g_vtreal[i][1] = r ? r->hi : 0xEEEEEEEE;
        g_vtuidok[i] = uid == g_vtid;
    }
    g_vtcommon = (w32)common;
    g_vthits = i + 1;
    return i < 4 ? g_vtret[i] : 0;
}

static SceUInt h_vtw(SceUID uid, SceInt64 s, SceInt64 r, void *common) {
    int i = g_vthits;
    if (i < 4) {
        g_vtsched[i][0] = (w32)s; g_vtsched[i][1] = (w32)((unsigned long long)s >> 32);
        g_vtreal[i][0] = (w32)r;  g_vtreal[i][1] = (w32)((unsigned long long)r >> 32);
        g_vtuidok[i] = uid == g_vtid;
    }
    g_vtcommon = (w32)common;
    g_vthits = i + 1;
    return i < 4 ? g_vtret[i] : 0;
}

static SceUInt h_vtdel(SceUID uid, SceKernelSysClock *s, SceKernelSysClock *r, void *common) {
    (void)s; (void)r; (void)common;
    g_vtdel_ret = sceKernelDeleteVTimer(uid);
    g_vthits++;
    return 0;
}

static void vt_reset(void) {
    g_vthits = 0; g_vtcommon = 0;
    memset(g_vtsched, 0xEE, sizeof g_vtsched);
    memset(g_vtreal, 0xEE, sizeof g_vtreal);
    memset((void *)g_vtuidok, 0, sizeof g_vtuidok);
    memset(g_vtret, 0, sizeof g_vtret);
}

static unsigned long long clk64(const SceKernelSysClock *c) {
    return ((unsigned long long)c->hi << 32) | c->low;
}

/* Every word of SceKernelVTimerInfo; base as 0 or its distance from now,
 * current as its value only while stopped. */
/* A vtimer base is a system time: logged as 0, near the current system time,
 * or other. */
static const char *vtb(unsigned long long base, SceInt64 now) {
    if (base == 0) return "0";
    if ((SceInt64)base <= now && now - (SceInt64)base < 1000000) return "near-systime";
    return "other";
}

/* lo < 0: the current value is one the probe set, logged as is; otherwise it
 * was measured over a spin or delay of lo microseconds and is logged as a
 * relation to lo. */
static void rec_vtinfo(SceUID v, int lo) {
    SceKernelVTimerInfo vi;
    memset(&vi, 0, sizeof vi);
    vi.size = sizeof vi;
    SceInt64 now = sceKernelGetSystemTimeWide();
    int r = sceKernelReferVTimerStatus(v, &vi);
    rec("  refer=%s", hx(r));
    if (r) { rec("\n"); return; }
    char name[33];
    memcpy(name, vi.name, 32);
    name[32] = 0;
    unsigned long long base = clk64(&vi.base), cur = clk64(&vi.current);
    const char *bs = vtb(base, now);
    char cs[48];
    if (vi.active) snprintf(cs, sizeof cs, "(running)");
    else if (lo < 0) snprintf(cs, sizeof cs, "%s", hx64(cur));
    else snprintf(cs, sizeof cs, "%s [%d, %d+100ms)", cur >= (unsigned long long)lo &&
                  cur < (unsigned long long)lo + 100000 ? "in" : "outside", lo, lo);
    rec(" size=%d name=\"%s\" active=%d base=%s current=%s\n", (int)vi.size, name, vi.active, bs, cs);
    rec("    schedule=%s handler=%s common=%s\n", hx64(clk64(&vi.schedule)),
        (w32)vi.handler == (w32)h_vt ? "h_vt" : (w32)vi.handler == (w32)h_vtw ? "h_vtw" : cls((w32)vi.handler),
        (w32)vi.common == (w32)&g_vtobj ? "given" : pv((w32)vi.common));
}

/* The scheduled time is printed only when it looks like one (below 2^24);
 * anything else (an address, when the arguments do not arrive where the
 * handler's type says) is printed as its class. */
static void rec_vthits(void) {
    for (int i = 0; i < g_vthits && i < 4; i++) {
        char sch[40];
        if (g_vtsched[i][1] == 0 && g_vtsched[i][0] < 0x01000000)
            snprintf(sch, sizeof sch, "%08X_%08X", g_vtsched[i][1], g_vtsched[i][0]);
        else
            snprintf(sch, sizeof sch, "(not a time: hi %s lo %s)", cls(g_vtsched[i][1]),
                     cls(g_vtsched[i][0]));
        rec("    hit %d: uid ok=%d scheduled=%s real>=scheduled %s\n", i, g_vtuidok[i], sch,
            (((unsigned long long)g_vtreal[i][1] << 32) | g_vtreal[i][0]) >=
            (((unsigned long long)g_vtsched[i][1] << 32) | g_vtsched[i][0]) ? "yes" : "no");
    }
}

static void sec_vtimer(void) {
    SEC("vtimers");
    nm_reset();
    SceKernelSysClock clk;
    int r;

    /* ktimer.c:219-223 -- a never-started timer reports base 0. */
    ST("vtimer: create, refer, reads before it is started");
    SceUID v = sceKernelCreateVTimer("vt", NULL);
    g_vtid = v;
    rec("  create=%s\n", cu(v));
    rec_vtinfo(v, -1);
    rec("  TimeWide=%s BaseWide=%s", hx64(sceKernelGetVTimerTimeWide(v)),
        vtb(sceKernelGetVTimerBaseWide(v), sceKernelGetSystemTimeWide()));
    clk.low = clk.hi = 0xEEEEEEEE;
    r = sceKernelGetVTimerTime(v, &clk);
    rec(" GetVTimerTime=%s %08X_%08X", hx(r), (w32)clk.hi, (w32)clk.low);
    clk.low = clk.hi = 0xEEEEEEEE;
    r = sceKernelGetVTimerBase(v, &clk);
    rec(" GetVTimerBase=%s %s\n", hx(r), clk.hi == 0xEEEEEEEE && clk.low == 0xEEEEEEEE ? "untouched" :
        vtb(clk64(&clk), sceKernelGetSystemTimeWide()));

    /* ktimer.c:284-306 -- start and stop return whether the timer was
     * running; stop clears base to 0. */
    ST("vtimer: start twice, delay 5ms, read, stop twice, reads while stopped");
    int s1 = sceKernelStartVTimer(v), s2 = sceKernelStartVTimer(v);
    rec("  start=%s again=%s\n", hx(s1), hx(s2));
    rec_vtinfo(v, -1);
    sceKernelDelayThread(5000);
    SceInt64 tw = sceKernelGetVTimerTimeWide(v);
    SceInt64 bw = sceKernelGetVTimerBaseWide(v);
    SceInt64 now = sceKernelGetSystemTimeWide();
    rec("  after 5ms: time>=5000 %s base near systime %s\n", tw >= 5000 ? "yes" : "no",
        bw > 0 && bw <= now && now - bw < 1000000 ? "yes" : "no");
    int p1 = sceKernelStopVTimer(v), p2 = sceKernelStopVTimer(v);
    rec("  stop=%s again=%s\n", hx(p1), hx(p2));
    rec_vtinfo(v, 5000);
    SceInt64 f1 = sceKernelGetVTimerTimeWide(v);
    sceKernelDelayThread(2000);
    SceInt64 f2 = sceKernelGetVTimerTimeWide(v);
    rec("  frozen while stopped: %s BaseWide=%s\n", f1 == f2 ? "yes" : "no",
        vtb(sceKernelGetVTimerBaseWide(v), sceKernelGetSystemTimeWide()));

    ST("vtimer: SetVTimerTime(1000000) while stopped; SetVTimerTimeWide(2000000) returns the old value");
    clk.low = 1000000; clk.hi = 0;
    r = sceKernelSetVTimerTime(v, &clk);
    rec("  SetVTimerTime=%s clock after: %s TimeWide=%s\n", hx(r),
        clk64(&clk) == 1000000 ? "unchanged" : (SceInt64)clk64(&clk) == f2 ? "the old time" : "other",
        hx64(sceKernelGetVTimerTimeWide(v)));
    SceInt64 old = sceKernelSetVTimerTimeWide(v, 2000000);
    rec("  SetVTimerTimeWide=%s TimeWide=%s", hx64(old), hx64(sceKernelGetVTimerTimeWide(v)));
    rec(" BaseWide=%s\n", vtb(sceKernelGetVTimerBaseWide(v), sceKernelGetSystemTimeWide()));
    rec_vtinfo(v, -1);

    ST("vtimer: start from 2000000, delay 3ms, stop");
    sceKernelStartVTimer(v);
    sceKernelDelayThread(3000);
    sceKernelStopVTimer(v);
    tw = sceKernelGetVTimerTimeWide(v);
    rec("  time>=2003000 %s time<2100000 %s\n", tw >= 2003000 ? "yes" : "no", tw < 2100000 ? "yes" : "no");

    /* ktimer.c:420-477 -- the handler gets (uid, &scheduled, &real, common),
     * and its return adds to the schedule. */
    ST("vtimer: time 0, handler at 5000 returning 2000 then 0; start, main spins 20ms");
    vt_reset();
    g_vtret[0] = 2000; g_vtret[1] = 0;
    sceKernelSetVTimerTimeWide(v, 0);
    clk.low = 5000; clk.hi = 0;
    r = sceKernelSetVTimerHandler(v, &clk, h_vt, &g_vtobj);
    rec("  SetVTimerHandler=%s\n", hx(r));
    rec_vtinfo(v, -1);
    sceKernelStartVTimer(v);
    spin_us(20000);
    sceKernelStopVTimer(v);
    rec("  hits=%d common=%s\n", g_vthits, g_vtcommon == (w32)&g_vtobj ? "given" : pv(g_vtcommon));
    rec_vthits();
    rec_vtinfo(v, 20000);

    /* ktimer.c:448-474 calls a wide handler exactly like the narrow one,
     * with two clock pointers; PSPSDK's SceKernelVTimerHandlerWide takes the
     * two times as 64-bit values (EABI: $a2:$a3 and $t0:$t1, common in $t2). */
    ST("vtimer: time 0, SetVTimerHandlerWide at 3000 returning 0; start, main spins 10ms");
    vt_reset();
    sceKernelSetVTimerTimeWide(v, 0);
    r = sceKernelSetVTimerHandlerWide(v, 3000, h_vtw, &g_vtobj);
    sceKernelStartVTimer(v);
    spin_us(10000);
    sceKernelStopVTimer(v);
    rec("  SetVTimerHandlerWide=%s hits=%d common=%s\n", hx(r), g_vthits,
        g_vtcommon == (w32)&g_vtobj ? "given" : "other");
    rec_vthits();
    rec_vtinfo(v, 10000);

    ST("vtimer: CancelVTimerHandler");
    sceKernelSetVTimerHandlerWide(v, 0x7FFFFFFF, h_vtw, &g_vtobj);
    r = sceKernelCancelVTimerHandler(v);
    rec("  cancel=%s again=%s\n", hx(r), hx(sceKernelCancelVTimerHandler(v)));
    rec_vtinfo(v, 10000);

    /* ktimer.c:272-277 -- a handler deleting its own vtimer gets
     * ILLEGAL_CONTEXT. */
    ST("vtimer: handler deletes its own vtimer");
    vt_reset();
    SceUID v2 = sceKernelCreateVTimer("vt2", NULL);
    clk.low = 1000; clk.hi = 0;
    sceKernelSetVTimerHandler(v2, &clk, h_vtdel, NULL);
    sceKernelStartVTimer(v2);
    sceKernelDelayThread(10000);
    SceKernelVTimerInfo vi;
    vi.size = sizeof vi;
    rec("  hits=%d delete from handler=%s; then refer=%s stop=%s delete=%s\n", g_vthits,
        hx(g_vtdel_ret), hx(sceKernelReferVTimerStatus(v2, &vi)), hx(sceKernelStopVTimer(v2)),
        hx(sceKernelDeleteVTimer(v2)));

    /* ktimer.c:286-298 (0 is ILLEGAL_VTID, unknown UNKNOWN_VTID),
     * ktimer.c:315-319 (wide reads fail as all ones). */
    ST("vtimer: id 0 and a deleted id");
    SceUID v3 = sceKernelCreateVTimer("vt3", NULL);
    sceKernelDeleteVTimer(v3);
    rec("  start: 0=%s deleted=%s; stop: 0=%s deleted=%s\n", hx(sceKernelStartVTimer(0)),
        hx(sceKernelStartVTimer(v3)), hx(sceKernelStopVTimer(0)), hx(sceKernelStopVTimer(v3)));
    rec("  TimeWide: 0=%s deleted=%s; BaseWide: 0=%s deleted=%s\n", hx64(sceKernelGetVTimerTimeWide(0)),
        hx64(sceKernelGetVTimerTimeWide(v3)), hx64(sceKernelGetVTimerBaseWide(0)),
        hx64(sceKernelGetVTimerBaseWide(v3)));
    clk.low = clk.hi = 0xEEEEEEEE;
    rec("  GetVTimerTime(deleted)=%s SetVTimerTimeWide(deleted)=%s refer=%s delete=%s cancel handler=%s delete(0)=%s\n",
        hx(sceKernelGetVTimerTime(v3, &clk)), hx64(sceKernelSetVTimerTimeWide(v3, 5)),
        hx(sceKernelReferVTimerStatus(v3, &vi)), hx(sceKernelDeleteVTimer(v3)),
        hx(sceKernelCancelVTimerHandler(v3)), hx(sceKernelDeleteVTimer(0)));
    rec("  delete vt=%s\n", hx(sceKernelDeleteVTimer(v)));

    /* sched.c:323-348 -- psprecomp runs a timer handler that falls due while
     * every thread waits at that moment, as the interrupt would; step 115
     * showed it for an alarm. */
    ST("vtimer: handler at 2000 while main sits in DelayThread(10ms): the vtimer's time when it ran");
    vt_reset();
    SceUID v4 = sceKernelCreateVTimer("vt4", NULL);
    g_vtid = v4;
    clk.low = 2000; clk.hi = 0;
    r = sceKernelSetVTimerHandler(v4, &clk, h_vt, &g_vtobj);
    sceKernelStartVTimer(v4);
    sceKernelDelayThread(10000);
    sceKernelStopVTimer(v4);
    rec("  SetVTimerHandler=%s hits=%d", hx(r), g_vthits);
    if (g_vthits)
        rec(" ran at vtimer time %s\n", g_vtreal[0][1] ? "(high word set)" : g_vtreal[0][0] < 2000 ? "<2000" :
            g_vtreal[0][0] < 3000 ? "2000..2999" : g_vtreal[0][0] < 9000 ? "3000..8999" : ">=9000");
    else
        rec("\n");
    rec("  delete=%s\n", hx(sceKernelDeleteVTimer(v4)));
}

/* ======================================================================= */

static void sec_time(void) {
    SEC("system time and clocks");
    nm_reset();

    /* threadman.c:680-705 -- Wide, Low and GetSystemTime read one clock. */
    ST("time: GetSystemTimeWide / Low / GetSystemTime agree and move forward");
    SceInt64 w1 = sceKernelGetSystemTimeWide();
    w32 lo = sceKernelGetSystemTimeLow();
    SceKernelSysClock clk = { 0xEEEEEEEE, 0xEEEEEEEE };
    int r = sceKernelGetSystemTime(&clk);
    SceInt64 w2 = sceKernelGetSystemTimeWide();
    rec("  Low within [Wide1, Wide2]: %s  GetSystemTime=%s within: %s  Wide2>Wide1: %s\n",
        lo - (w32)w1 <= (w32)(w2 - w1) ? "yes" : "no", hx(r),
        (SceInt64)clk64(&clk) >= w1 && (SceInt64)clk64(&clk) <= w2 ? "yes" : "no",
        w2 > w1 ? "yes" : "no");

    ST("time: USec2SysClock, USec2SysClockWide, SysClock2USec, SysClock2USecWide on fixed values");
    clk.low = clk.hi = 0xEEEEEEEE;
    r = sceKernelUSec2SysClock(1234567, &clk);
    rec("  USec2SysClock(1234567)=%s -> %08X_%08X\n", hx(r), (w32)clk.hi, (w32)clk.low);
    rec("  USec2SysClockWide(1234567)=%s\n", hx64(sceKernelUSec2SysClockWide(1234567)));
    unsigned s = 0xEEEEEEEE, us = 0xEEEEEEEE;
    clk.low = 1234567; clk.hi = 0;
    r = sceKernelSysClock2USec(&clk, &s, &us);
    rec("  SysClock2USec(1234567)=%s -> %u s %u us\n", hx(r), s, us);
    s = us = 0xEEEEEEEE;
    clk.low = 0x12345678; clk.hi = 0x9;
    r = sceKernelSysClock2USec(&clk, &s, &us);
    rec("  SysClock2USec(0x9_12345678)=%s -> %08X s %08X us\n", hx(r), s, us);
    s = us = 0xEEEEEEEE;
    r = sceKernelSysClock2USecWide(7654321, &s, &us);
    rec("  SysClock2USecWide(7654321)=%s -> %u s %u us\n", hx(r), s, us);

    /* misc.c:106-130 -- psprecomp answers LibcClock from the host's CPU
     * clock and LibcGettimeofday with whole seconds. */
    ST("time: LibcClock across DelayThread(20000); LibcTime; LibcGettimeofday");
    clock_t c1 = sceKernelLibcClock();
    sceKernelDelayThread(20000);
    clock_t c2 = sceKernelLibcClock();
    rec("  LibcClock advanced >=20000: %s, <1000000: %s\n", (w32)(c2 - c1) >= 20000 ? "yes" : "no",
        (w32)(c2 - c1) < 1000000 ? "yes" : "no");
    time_t tt = 0;
    time_t tr = sceKernelLibcTime(&tt);
    rec("  LibcTime: ret==out %s, after 2001 %s\n", tr == tt ? "yes" : "no",
        (w32)tr > 978307200u ? "yes" : "no");
    SceKernelTimeval tv = { 0xEEEEEEEE, 0xEEEEEEEE };
    r = sceKernelLibcGettimeofday(&tv, NULL);
    rec("  LibcGettimeofday=%s sec after 2001 %s usec<1000000 %s usec nonzero %s\n", hx(r),
        tv.tv_sec > 978307200u && tv.tv_sec != 0xEEEEEEEE ? "yes" : "no",
        tv.tv_usec < 1000000 ? "yes" : "no", tv.tv_usec ? "yes" : "no");

    /* misc.c:656-680 -- a tick is a microsecond; psprecomp's epoch is module
     * start, where a PSP counts from year 1. */
    ST("time: sceRtcGetTickResolution, GetCurrentTick across DelayThread(2000), tick epoch");
    rec("  resolution=%08X\n", (w32)sceRtcGetTickResolution());
    u64 k1 = 0, k2 = 0;
    int r1 = sceRtcGetCurrentTick(&k1);
    sceKernelDelayThread(2000);
    int r2 = sceRtcGetCurrentTick(&k2);
    rec("  GetCurrentTick=%s %s advanced>=2000 %s <1000000 %s after 1970 %s\n", hx(r1), hx(r2),
        k2 - k1 >= 2000 ? "yes" : "no", k2 - k1 < 1000000 ? "yes" : "no",
        k1 >= 62135596800000000ull ? "yes" : "no");
    ScePspDateTime dt;
    memset(&dt, 0, sizeof dt);
    r = sceRtcGetCurrentClock(&dt, 0);
    rec("  GetCurrentClock(tz 0)=%s year>=2020 %s\n", hx(r), dt.year >= 2020 ? "yes" : "no");

    /* misc.c:146-160 -- psprecomp's seconds are the Unix time, and step 133
     * read the PSP's as not after 2001: unwritten, small, or on another
     * epoch. Relations only; the second read says whether usec and sec make
     * one clock. */
    ST("time: LibcGettimeofday seconds against LibcTime and uptime; a timezone struct; two reads across DelayThread(20000)");
    {
        w32 tz[2] = { 0xEEEEEEEE, 0xEEEEEEEE };
        SceKernelTimeval a = { 0xEEEEEEEE, 0xEEEEEEEE }, b = { 0xEEEEEEEE, 0xEEEEEEEE };
        int ra = sceKernelLibcGettimeofday(&a, (struct timezone *)tz);
        w32 lt = (w32)sceKernelLibcTime(NULL);
        w32 up = (w32)(sceKernelGetSystemTimeWide() / 1000000);
        sceKernelDelayThread(20000);
        int rb = sceKernelLibcGettimeofday(&b, NULL);
        const w32 s = a.tv_sec;
        rec("  ret=%s tv_sec: %s; within 2s of LibcTime %s; within 2s of uptime %s\n", hx(ra),
            s == 0xEEEEEEEE ? "untouched" : s == 0 ? "0" : s < 86400 ? "under a day" :
            s < 978307200u ? "before 2001" : "after 2001",
            s - lt + 2 <= 4 ? "yes" : "no", s - up + 2 <= 4 ? "yes" : "no");
        rec("  timezone words: %s %s\n", tz[0] == 0xEEEEEEEE ? "untouched" : hx(tz[0]),
            tz[1] == 0xEEEEEEEE ? "untouched" : hx(tz[1]));
        long long d = ((long long)b.tv_sec - (long long)a.tv_sec) * 1000000 +
                      ((long long)b.tv_usec - (long long)a.tv_usec);
        rec("  second read=%s: advanced >=20000us %s, <1000000us %s\n", hx(rb),
            d >= 20000 ? "yes" : "no", d < 1000000 ? "yes" : "no");
    }
}

/* ======================================================================= */

static void rec_dt(const char *label, const ScePspDateTime *d) {
    rec("  %s %04u-%02u-%02u %02u:%02u:%02u.%06u\n", label, d->year, d->month, d->day, d->hour,
        d->minute, d->second, d->microsecond);
}

static void sec_rtc(void) {
    SEC("rtc arithmetic (none of it in psprecomp)");
    int r;

    ST("rtc: IsLeapYear 1900 2000 2004 2023; GetDaysInMonth 2000/2 1900/2 2023/4 2023/13 2023/0");
    rec("  leap: %s %s %s %s\n", hx(sceRtcIsLeapYear(1900)), hx(sceRtcIsLeapYear(2000)),
        hx(sceRtcIsLeapYear(2004)), hx(sceRtcIsLeapYear(2023)));
    rec("  days: %s %s %s %s %s\n", hx(sceRtcGetDaysInMonth(2000, 2)), hx(sceRtcGetDaysInMonth(1900, 2)),
        hx(sceRtcGetDaysInMonth(2023, 4)), hx(sceRtcGetDaysInMonth(2023, 13)),
        hx(sceRtcGetDaysInMonth(2023, 0)));

    ST("rtc: GetDayOfWeek 2000-01-01, 1970-01-01, 2026-09-27, 2023-02-30, 2023-13-01");
    rec("  %s %s %s %s %s\n", hx(sceRtcGetDayOfWeek(2000, 1, 1)), hx(sceRtcGetDayOfWeek(1970, 1, 1)),
        hx(sceRtcGetDayOfWeek(2026, 9, 27)), hx(sceRtcGetDayOfWeek(2023, 2, 30)),
        hx(sceRtcGetDayOfWeek(2023, 13, 1)));

    ST("rtc: CheckValid on a good date and on bad year/month/day/hour/minute/second/microsecond");
    {
        ScePspDateTime d = { 2000, 1, 1, 0, 0, 0, 0 };
        char line[200];
        int n = snprintf(line, sizeof line, " ok:%s", hx(sceRtcCheckValid(&d)));
        ScePspDateTime b;
        b = d; b.year = 0;          n += snprintf(line + n, sizeof line - n, " y0:%s", hx(sceRtcCheckValid(&b)));
        b = d; b.month = 13;        n += snprintf(line + n, sizeof line - n, " m13:%s", hx(sceRtcCheckValid(&b)));
        b = d; b.day = 32;          n += snprintf(line + n, sizeof line - n, " d32:%s", hx(sceRtcCheckValid(&b)));
        b = d; b.month = 2; b.day = 30; n += snprintf(line + n, sizeof line - n, " feb30:%s", hx(sceRtcCheckValid(&b)));
        b = d; b.hour = 24;         n += snprintf(line + n, sizeof line - n, " h24:%s", hx(sceRtcCheckValid(&b)));
        b = d; b.minute = 60;       n += snprintf(line + n, sizeof line - n, " mi60:%s", hx(sceRtcCheckValid(&b)));
        b = d; b.second = 60;       n += snprintf(line + n, sizeof line - n, " s60:%s", hx(sceRtcCheckValid(&b)));
        b = d; b.microsecond = 1000000; snprintf(line + n, sizeof line - n, " us1M:%s", hx(sceRtcCheckValid(&b)));
        rec(" %s\n", line);
    }

    ST("rtc: GetTick of 2000-01-01 00:00:00; SetTick back; GetTime_t; SetTime_t(0)");
    ScePspDateTime d = { 2000, 1, 1, 0, 0, 0, 0 };
    u64 tick = 0xEEEEEEEEEEEEEEEEull;
    r = sceRtcGetTick(&d, &tick);
    rec("  GetTick=%s tick=%s\n", hx(r), hx64(tick));
    ScePspDateTime e;
    memset(&e, 0xEE, sizeof e);
    u64 t2 = tick + 86400000000ull + 1;
    r = sceRtcSetTick(&e, &t2);
    rec("  SetTick(tick + 1 day + 1us)=%s\n", hx(r));
    rec_dt("  ->", &e);
    time_t tt = (time_t)0xEEEEEEEE;
    r = sceRtcGetTime_t(&d, &tt);
    rec("  GetTime_t=%s -> %08X\n", hx(r), (w32)tt);
    memset(&e, 0xEE, sizeof e);
    r = sceRtcSetTime_t(&e, 0);
    rec("  SetTime_t(0)=%s\n", hx(r));
    rec_dt("  ->", &e);

    ST("rtc: TickAdd Ticks(+1000) Microseconds(+1) Seconds(+1) Minutes(-1) Hours(25) Days(-1) Weeks(1) Months(1) Years(1) from 2000-01-31");
    {
        ScePspDateTime j = { 2000, 1, 31, 0, 0, 0, 0 };
        u64 base = 0, o;
        sceRtcGetTick(&j, &base);
        rec("  base=%s\n", hx64(base));
#define ADD(label, call) do { o = 0xEEEEEEEEEEEEEEEEull; r = (call); \
        rec("  %-14s %s delta=%s\n", label, hx(r), hx64(o - base)); } while (0)
        ADD("Ticks+1000", sceRtcTickAddTicks(&o, &base, 1000));
        ADD("Micro+1", sceRtcTickAddMicroseconds(&o, &base, 1));
        ADD("Seconds+1", sceRtcTickAddSeconds(&o, &base, 1));
        ADD("Minutes-1", sceRtcTickAddMinutes(&o, &base, (u64)-1));
        ADD("Hours+25", sceRtcTickAddHours(&o, &base, 25));
        ADD("Days-1", sceRtcTickAddDays(&o, &base, -1));
        ADD("Weeks+1", sceRtcTickAddWeeks(&o, &base, 1));
        ADD("Months+1", sceRtcTickAddMonths(&o, &base, 1));
        ADD("Years+1", sceRtcTickAddYears(&o, &base, 1));
#undef ADD
        o = 0;
        sceRtcTickAddMonths(&o, &base, 1);
        ScePspDateTime m;
        memset(&m, 0, sizeof m);
        sceRtcSetTick(&m, &o);
        rec_dt("  Jan 31 + 1 month ->", &m);
    }

    ST("rtc: CompareTick less, equal, greater; FormatRFC3339 of 2000-01-01 at +0 and +540 minutes");
    {
        u64 x = 100, y = 200;
        rec("  compare: %s %s %s\n", hx(sceRtcCompareTick(&x, &y)), hx(sceRtcCompareTick(&x, &x)),
            hx(sceRtcCompareTick(&y, &x)));
        char s[64];
        memset(s, 0, sizeof s);
        strcpy(s, "(untouched)");
        r = sceRtcFormatRFC3339(s, &tick, 0);
        rec("  RFC3339 +0: %s \"%s\"\n", hx(r), s);
        strcpy(s, "(untouched)");
        r = sceRtcFormatRFC3339(s, &tick, 540);
        rec("  RFC3339 +540: %s \"%s\"\n", hx(r), s);
    }

    /* misc.c:821-846 -- a year above 9999 is taken as bad, and GetTick does
     * the arithmetic on an invalid date (Feb 30 as Mar 2), both unmeasured. */
    ST("rtc: CheckValid of 9999-12-31 23:59:59.999999 and of year 10000; GetTick of 2023-02-30 and 2023-13-01");
    {
        ScePspDateTime y = { 9999, 12, 31, 23, 59, 59, 999999 };
        int v1 = sceRtcCheckValid(&y);
        y.year = 10000;
        int v2 = sceRtcCheckValid(&y);
        rec("  9999-12-31: %s  year 10000: %s\n", hx(v1), hx(v2));
        static const struct { ScePspDateTime bad, ref; const char *l; } g[] = {
            { { 2023, 2, 30, 0, 0, 0, 0 }, { 2023, 3, 2, 0, 0, 0, 0 }, "2023-02-30 vs 2023-03-02" },
            { { 2023, 13, 1, 0, 0, 0, 0 }, { 2024, 1, 1, 0, 0, 0, 0 }, "2023-13-01 vs 2024-01-01" },
        };
        for (int i = 0; i < 2; i++) {
            u64 tb = 0xEEEEEEEEEEEEEEEEull, tr = 0;
            int rb = sceRtcGetTick(&g[i].bad, &tb);
            sceRtcGetTick(&g[i].ref, &tr);
            rec("  GetTick %s: %s %s\n", g[i].l, hx(rb),
                tb == 0xEEEEEEEEEEEEEEEEull ? "untouched" : tb == tr ? "same tick" : hx64(tb - tr));
        }
    }
}

/* ======================================================================= */

static void sec_sysmem(void) {
    SEC("user memory");
    SceUID b, b2;
    int r;

    /* sysmem.c:308-319 -- psprecomp answers max == total. */
    ST("sysmem: TotalFreeMemSize versus MaxFreeMemSize");
    SceSize tot = sceKernelTotalFreeMemSize(), mx = sceKernelMaxFreeMemSize();
    rec("  total>=max %s total==max %s max>=1MB %s\n", tot >= mx ? "yes" : "no",
        tot == mx ? "yes" : "no", mx >= 0x100000 ? "yes" : "no");

    /* sysmem.c:208-211 -- blocks are 256-byte granules. */
    ST("sysmem: total free drop for sizes 1, 0x100, 0x101, 0x10000; head alignment");
    {
        static const w32 sz[] = { 1, 0x100, 0x101, 0x10000 };
        for (int i = 0; i < 4; i++) {
            SceSize before = sceKernelTotalFreeMemSize();
            b = sceKernelAllocPartitionMemory(2, "sz", PSP_SMEM_Low, sz[i], NULL);
            SceSize after = sceKernelTotalFreeMemSize();
            w32 a = b > 0 ? (w32)sceKernelGetBlockHeadAddr(b) : 0;
            rec("  size %05X: %s drop=%08X head&FF=%02X\n", sz[i], cu(b), (w32)(before - after), a & 0xFF);
            if (b > 0) sceKernelFreePartitionMemory(b);
        }
    }

    ST("sysmem: size 0; types 5 and -1; two Low blocks of 1 byte (distance); High above Low");
    b = sceKernelAllocPartitionMemory(2, "zero", PSP_SMEM_Low, 0, NULL);
    rec("  size 0: %s\n", cu(b));
    if (b > 0) sceKernelFreePartitionMemory(b);
    b = sceKernelAllocPartitionMemory(2, "t5", 5, 0x100, NULL);
    rec("  type 5: %s\n", cu(b));
    if (b > 0) sceKernelFreePartitionMemory(b);
    b = sceKernelAllocPartitionMemory(2, "tm1", -1, 0x100, NULL);
    rec("  type -1: %s\n", cu(b));
    if (b > 0) sceKernelFreePartitionMemory(b);
    b = sceKernelAllocPartitionMemory(2, "l1", PSP_SMEM_Low, 1, NULL);
    b2 = sceKernelAllocPartitionMemory(2, "l2", PSP_SMEM_Low, 1, NULL);
    SceUID h = sceKernelAllocPartitionMemory(2, "h", PSP_SMEM_High, 1, NULL);
    {
        w32 a1 = b > 0 ? (w32)sceKernelGetBlockHeadAddr(b) : 0;
        w32 a2 = b2 > 0 ? (w32)sceKernelGetBlockHeadAddr(b2) : 0;
        w32 ah = h > 0 ? (w32)sceKernelGetBlockHeadAddr(h) : 0;
        rec("  low2-low1=%s high>low2 %s\n", off(a2, a1), ah > a2 ? "yes" : "no");
    }
    sceKernelFreePartitionMemory(b); sceKernelFreePartitionMemory(b2); sceKernelFreePartitionMemory(h);

    /* sysmem.c:213-218 -- the aligned types take the alignment in `addr`; a
     * non-power of two is refused. */
    ST("sysmem: LowAligned 0x1000, HighAligned 0x10000, LowAligned 3, LowAligned 0");
    {
        static const struct { int type; w32 align; } al[] = { { 3, 0x1000 }, { 4, 0x10000 }, { 3, 3 }, { 3, 0 } };
        for (int i = 0; i < 4; i++) {
            b = sceKernelAllocPartitionMemory(2, "al", al[i].type, 0x100, (void *)al[i].align);
            w32 a = b > 0 ? (w32)sceKernelGetBlockHeadAddr(b) : 0;
            rec("  type %d align %05X: %s aligned %s\n", al[i].type, al[i].align, cu(b),
                b > 0 ? ((al[i].align && (a & (al[i].align - 1))) ? "no" : "yes") : "-");
            if (b > 0) sceKernelFreePartitionMemory(b);
        }
    }

    ST("sysmem: Addr type at a just-freed block's address, and 0x80 past it");
    b = sceKernelAllocPartitionMemory(2, "a", PSP_SMEM_Low, 0x1000, NULL);
    {
        w32 a = b > 0 ? (w32)sceKernelGetBlockHeadAddr(b) : 0;
        sceKernelFreePartitionMemory(b);
        b = sceKernelAllocPartitionMemory(2, "at", PSP_SMEM_Addr, 0x1000, (void *)a);
        w32 a2 = b > 0 ? (w32)sceKernelGetBlockHeadAddr(b) : 0;
        rec("  at freed address: %s same %s\n", cu(b), a2 == a ? "yes" : "no");
        if (b > 0) sceKernelFreePartitionMemory(b);
        b = sceKernelAllocPartitionMemory(2, "at80", PSP_SMEM_Addr, 0x1000, (void *)(a + 0x80));
        a2 = b > 0 ? (w32)sceKernelGetBlockHeadAddr(b) : 0;
        rec("  at +0x80: %s head-at=%s\n", cu(b), b > 0 ? off(a2, a) : "-");
        if (b > 0) sceKernelFreePartitionMemory(b);
    }

    ST("sysmem: partitions 0, 1, 3, 4, 6, 7, 8, -1 (0x100 bytes)");
    {
        static const int parts[] = { 0, 1, 3, 4, 6, 7, 8, -1 };
        char line[200];
        int n = 0;
        for (int i = 0; i < 8; i++) {
            b = sceKernelAllocPartitionMemory(parts[i], "p", PSP_SMEM_Low, 0x100, NULL);
            n += snprintf(line + n, sizeof line - n, " %d:%s", parts[i], cu(b));
            if (b > 0) sceKernelFreePartitionMemory(b);
        }
        rec(" %s\n", line);
    }

    ST("sysmem: allocate MaxFreeMemSize, then max+0x100; free twice; head of a freed block");
    mx = sceKernelMaxFreeMemSize();
    tot = sceKernelTotalFreeMemSize();
    b = sceKernelAllocPartitionMemory(2, "max", PSP_SMEM_Low, mx, NULL);
    SceSize mx2 = sceKernelMaxFreeMemSize(), tot2 = sceKernelTotalFreeMemSize();
    rec("  max: %s total drop==max %s max after<max %s\n", cu(b), tot - tot2 == mx ? "yes" : "no",
        mx2 < mx ? "yes" : "no");
    r = b > 0 ? sceKernelFreePartitionMemory(b) : 0;
    b2 = sceKernelAllocPartitionMemory(2, "max+", PSP_SMEM_Low, mx + 0x100, NULL);
    rec("  free=%s max+0x100: %s\n", hx(r), cu(b2));
    if (b2 > 0) sceKernelFreePartitionMemory(b2);
    rec("  free again=%s head of freed=%s\n", hx(sceKernelFreePartitionMemory(b)),
        pv((w32)sceKernelGetBlockHeadAddr(b)));

    /* sysmem.c:264-270 -- an Addr block asked for 0x80 into a granule starts
     * at the granule and keeps the rounded size, so the drop is 0x1000;
     * covering want + size would make it 0x1100. Step 141 read total > max. */
    ST("sysmem: total free drop for an Addr block 0x80 into a free granule; total minus max");
    {
        SceUID b0 = sceKernelAllocPartitionMemory(2, "a", PSP_SMEM_Low, 0x1000, NULL);
        w32 at = b0 > 0 ? (w32)sceKernelGetBlockHeadAddr(b0) : 0;
        if (b0 > 0) sceKernelFreePartitionMemory(b0);
        SceSize before = sceKernelTotalFreeMemSize();
        SceUID ab = at ? sceKernelAllocPartitionMemory(2, "at80", PSP_SMEM_Addr, 0x1000, (void *)(at + 0x80)) : -1;
        SceSize after = sceKernelTotalFreeMemSize();
        rec("  Addr at +0x80: %s drop=%08X\n", cu(ab), ab > 0 ? (w32)(before - after) : 0);
        if (ab > 0) sceKernelFreePartitionMemory(ab);
        rec("  total-max=%08X\n", (w32)(sceKernelTotalFreeMemSize() - sceKernelMaxFreeMemSize()));
    }

    /* sysmem.c:214-224 -- 5 and 9 follow the vpl table, unmeasured. Risky,
     * like the tlspl partition 5 step. */
    if (!ST("sysmem: partitions 5 and 9 (0x100 bytes)")) {
        SceUID p5 = sceKernelAllocPartitionMemory(5, "p5", PSP_SMEM_Low, 0x100, NULL);
        SceUID p9 = sceKernelAllocPartitionMemory(9, "p9", PSP_SMEM_Low, 0x100, NULL);
        rec("  partition 5: %s partition 9: %s\n", cu(p5), cu(p9));
        if (p5 > 0) sceKernelFreePartitionMemory(p5);
        if (p9 > 0) sceKernelFreePartitionMemory(p9);
    }
}

/* ======================================================================= */

static const op_t S_RELEASE_MAIN[] = {
    { O_DELAY, 200000, 0 }, { O_RELEASE, 0, "safety-released-main" }, { O_END, 0, 0 } };

static void sec_badptr(void) {
    SEC("NULL and bad pointers (last, in case one crashes)");
    nm_reset();
    int r;

    /* syncprobe's Refer*Status calls with a NULL info pointer turned the PSP
     * off on firmware 6.60 (syncprobe 1, 2026-09-28); these take the same
     * path. */
    if (!ST("NULL info pointers: ReferThreadStatus, ReferCallbackStatus, ReferAlarmStatus, ReferVTimerStatus, ReferTlsplStatus") &&
        !KNOWN_CRASH("a NULL Refer*Status info pointer switched the PSP off in syncprobe")) {
        SceUID cb = sceKernelCreateCallback("bp_cb", h_count, NULL);
        SceUID al = sceKernelSetAlarm(10000000, h_alarm, NULL);
        SceUID vt = sceKernelCreateVTimer("bp_vt", NULL);
        SceUID tl = sceKernelCreateTlspl("bp_tl", 2, 0, 4, 1, NULL);
        rec("  thread=%s callback=%s alarm=%s vtimer=%s tlspl=%s\n",
            hx(sceKernelReferThreadStatus(g_main, NULL)), hx(sceKernelReferCallbackStatus(cb, NULL)),
            hx(sceKernelReferAlarmStatus(al, NULL)), hx(sceKernelReferVTimerStatus(vt, NULL)),
            hx(sceKernelReferTlsplStatus(tl, NULL)));
        sceKernelDeleteCallback(cb);
        sceKernelCancelAlarm(al);
        sceKernelDeleteVTimer(vt);
        sceKernelDeleteTlspl(tl);
    }

    if (!ST("GetThreadmanIdList(THREAD, NULL, 4, &count)") &&
        !KNOWN_CRASH("the kernel would write through a NULL buffer, like the Refer*Status crash")) {
        int cnt = 0x55AA;
        r = sceKernelGetThreadmanIdList(1, NULL, 4, &cnt);
        rec("  %s count>=2 %s\n", hx(r), cnt >= 2 && cnt != 0x55AA ? "yes" : "no");
    }

    /* threadman.c:1081-1100 (NULL name is ERROR for sema/flag/callback),
     * ktimer.c:257 (vtimer), kernobj.c:1771 (tlspl: NO_MEMORY). */
    if (!ST("NULL names: CreateThread, CreateCallback, CreateVTimer, CreateTlspl, AllocPartitionMemory")) {
        SceUID t = sceKernelCreateThread(NULL, script_entry, 0x30, 0x1000, 0, NULL);
        rec("  thread=%s", cu(t));
        if (t > 0) sceKernelDeleteThread(t);
        SceUID c = sceKernelCreateCallback(NULL, h_count, NULL);
        rec(" callback=%s", cu(c));
        if (c > 0) sceKernelDeleteCallback(c);
        SceUID v = sceKernelCreateVTimer(NULL, NULL);
        rec(" vtimer=%s", cu(v));
        if (v > 0) sceKernelDeleteVTimer(v);
        SceUID tl = sceKernelCreateTlspl(NULL, 2, 0, 4, 1, NULL);
        rec(" tlspl=%s", cu(tl));
        if (tl > 0) sceKernelDeleteTlspl(tl);
        SceUID b = sceKernelAllocPartitionMemory(2, NULL, PSP_SMEM_Low, 0x100, NULL);
        rec(" block=%s\n", cu(b));
        if (b > 0) sceKernelFreePartitionMemory(b);
    }

    /* ktimer.c:67-69, 84 -- a NULL handler is 800200D3. Set 10s out and
     * cancelled at once in case the firmware accepts it. */
    if (!ST("alarm: NULL handler (SetAlarm 10s)")) {
        SceUID a = sceKernelSetAlarm(10000000, NULL, NULL);
        if (a > 0) sceKernelCancelAlarm(a);
        rec("  NULL handler=%s\n", cu(a));
    }
    if (!ST("alarm: NULL clock (SetSysClockAlarm)") &&
        !KNOWN_CRASH("the kernel would read a NULL clock, like the Refer*Status crash")) {
        SceUID a2 = sceKernelSetSysClockAlarm(NULL, h_alarm, NULL);
        if (a2 > 0) sceKernelCancelAlarm(a2);
        rec("  NULL clock=%s\n", cu(a2));
    }

    if (!ST("vtimer: SetVTimerTime(NULL), GetVTimerTime(NULL), SetVTimerHandler(NULL schedule)") &&
        !KNOWN_CRASH("the kernel would use a NULL clock, like the Refer*Status crash")) {
        SceUID v = sceKernelCreateVTimer("bp_vt2", NULL);
        rec("  set=%s get=%s handler=%s\n", hx(sceKernelSetVTimerTime(v, NULL)),
            hx(sceKernelGetVTimerTime(v, NULL)), hx(sceKernelSetVTimerHandler(v, NULL, h_vt, NULL)));
        sceKernelCancelVTimerHandler(v);
        sceKernelDeleteVTimer(v);
    }

    /* threadman.c:1911-1917 -- a callback function with the top bit set is
     * 800200D3; NULL is accepted. Never notified either way. */
    if (!ST("callback: function NULL and 0xDEADBEEF")) {
        SceUID c1 = sceKernelCreateCallback("bp_null", NULL, NULL);
        SceUID c2 = sceKernelCreateCallback("bp_dead", (SceKernelCallbackFunction)0xDEADBEEF, NULL);
        rec("  NULL=%s DEADBEEF=%s\n", cu(c1), cu(c2));
        if (c1 > 0) sceKernelDeleteCallback(c1);
        if (c2 > 0) sceKernelDeleteCallback(c2);
    }

    /* kernobj.c:1886-1901 -- hardware indexes the pool table by uid >> 3 and
     * does not otherwise validate: 0 and 1 answer the first pool. Main holds
     * a block in the one pool first so a lookup that lands on it cannot wait;
     * a 0x30 thread releases main after 200ms in case it does anyway. */
    if (!ST("tlspl: GetTlsAddr with ids 0 and 1 while main holds a block of the only pool")) {
        fresh(); seq_clear();
        SceUID p = sceKernelCreateTlspl("lax", 2, 0, 0x10, 2, NULL);
        w32 mine = (w32)sceKernelGetTlsAddr(p);
        SceUID sf = spawn("safety", 0x30, S_RELEASE_MAIN);
        w32 z = (w32)sceKernelGetTlsAddr(0);
        w32 o = (w32)sceKernelGetTlsAddr(1);
        reap(sf);
        SceKernelThreadInfo ti;
        refer(p, &ti);
        rec("  pool=%s main's block %s; id 0 -> %s; id 1 -> %s\n", cu(p), mine ? "ok" : "NULL",
            z == 0 ? "NULL" : z == mine ? "main's block" : off(z, mine),
            o == 0 ? "NULL" : o == mine ? "main's block" : off(o, mine));
        rec_tlsinfo(p);
        rec_seq();
        sceKernelFreeTlspl(p);
        rec("  delete=%s\n", hx(sceKernelDeleteTlspl(p)));
    }

    /* threadman.c:323-326 -- NULL with a length arrives as length 0. */
    if (!ST("start args: length 8 with a NULL pointer"))
        regs_case(8, NULL, 0, 0);
    /* threadman.c:352-362 -- a negative length is 800200d3 and the thread is
     * left unstarted. */
    if (!ST("start args: length -1 with a real pointer"))
        regs_case((SceSize)-1, g_argbuf, 0, 0);

    /* threadman.c:352-373 -- an argument pointer the kernel cannot read is
     * 800200d3 and the thread stays unstarted. Threadprobe 2 (fw 6.60)
     * switched the PSP off here instead: the kernel copies the block without
     * checking the pointer. The last step of the probe. */
    if (!ST("StartThread with argp = 0x10 (small bad pointer), length 8") &&
        !KNOWN_CRASH("argp 0x10 switched the PSP off in threadprobe 2"))
        regs_case(8, (void *)0x10, 0, 0);
}

/* ======================================================================= */

/* Leftover check: object counts per id-list type against the counts taken
 * right after probe_init, and the names of threads other than main and
 * exit_cb. Every subtest is meant to clean up, so on hardware a nonzero
 * delta shows a subtest whose cleanup the firmware refused. */
static int g_count0[15];
static void count_objects(int *c) {
    SceUID buf[160];
    for (int t = 1; t <= 14; t++) {
        int cnt = -1;
        int r = sceKernelGetThreadmanIdList(t, buf, 160, &cnt);
        c[t] = r < 0 ? r : cnt;
    }
}
static void leftovers(void) {
    int c[15];
    count_objects(c);
    rec("  count delta per id-list type 1..14:");
    for (int t = 1; t <= 14; t++) rec(" %+d", c[t] - g_count0[t]);
    rec("\n");
    SceUID buf[160];
    int cnt = 0;
    int r = sceKernelGetThreadmanIdList(1, buf, 160, &cnt);
    rec("  threads other than main and exit_cb:");
    int any = 0;
    for (int i = 0; r > 0 && i < r && i < 160; i++) {
        SceKernelThreadInfo ti;
        if (refer(buf[i], &ti) != 0) continue;
        if (!strcmp(ti.name, "user_main") || !strcmp(ti.name, "exit_cb")) continue;
        rec(" %s(status %X)", ti.name, (w32)ti.status);
        any = 1;
    }
    rec("%s\n", any ? "" : " none");
}

int main(int argc, char *argv[]) {
    g_argc = argc;
    g_argv = argv;
    probe_init("threadprobe", PROBE_VERSION, argc, argv);
    count_objects(g_count0);
#ifdef LEAKDEBUG
#define S(f) do { f(); leftovers(); rec("  DEBUG free=%X max=%X\n", (w32)sceKernelTotalFreeMemSize(), (w32)sceKernelMaxFreeMemSize()); rec_flush(); } while (0)
    S(sec_basics); S(sec_create); S(sec_refer); S(sec_start); S(sec_states);
    S(sec_exit); S(sec_suspend); S(sec_priority); S(sec_rotate); S(sec_sleep);
    S(sec_preempt); S(sec_delay); S(sec_dispatch); S(sec_attr); S(sec_callbacks);
    S(sec_idlist); S(sec_tls); S(sec_alarm); S(sec_vtimer); S(sec_time);
    S(sec_rtc); S(sec_sysmem); S(sec_badptr);
#else

    sec_basics();
    sec_create();
    sec_refer();
    sec_start();
    sec_states();
    sec_exit();
    sec_suspend();
    sec_priority();
    sec_rotate();
    sec_sleep();
    sec_preempt();
    sec_delay();
    sec_dispatch();
    sec_attr();
    sec_callbacks();
    sec_idlist();
    sec_tls();
    sec_alarm();
    sec_vtimer();
    sec_time();
    sec_rtc();
    sec_sysmem();
    sec_badptr();
#endif

    SEC("summary");
    rec("  successful StartThread calls: %d\n", g_starts);
    leftovers();
    rec_flush();
    probe_done();
    return 0;
}
