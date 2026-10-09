/* Interpreter oracle tests. Every encoding here is hand-written from the
 * MIPS32r2 manual and the Allegrex extensions — no game data, no ROM, no disc
 * image.
 *
 * An oracle nobody has checked is worse than no oracle: it turns every real
 * divergence into an argument about which side is wrong. So these tests focus
 * on the parts of interp.c that are *not* shared with the emitter, because the
 * shared parts (decode, the recomp_rt helpers, memory) are covered elsewhere
 * and cannot produce a false divergence anyway.
 *
 * That means, above all, delay slots — described in the architecture notes as
 * "the single most error-prone thing in MIPS recompilation".
 */

#include "interp.h"
#include "decode.h"
#include "crypto/sha1.h"

#include "psprecomp/cpu.h"
#include "psprecomp/dispatch.h"
#include "psprecomp/hle.h"
#include "psprecomp/mem.h"
#include "psprecomp/sched.h"
#include "psprecomp/vfpu.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);        \
            printf(__VA_ARGS__);                               \
            printf("\n");                                      \
            failures++;                                        \
        }                                                      \
    } while (0)

/* Somewhere in mapped RAM to assemble test programs into. */
#define CODE  0x08900000u
#define DATA  0x08910000u
#define DONE  0x0DEAD000u   /* $ra sentinel: never mapped to code */
#define THUNK 0x08920000u   /* a stand-in .sceStub.text entry */
#define TARGET 0x08930000u  /* what a firmware callback dispatches into */
#define THREAD 0x08931000u  /* a thread body */
#define THREAD_SP 0x08980000u

static void load(uint32_t addr, const uint32_t *words, unsigned n) {
    for (unsigned i = 0; i < n; i++) psp_write32(addr + i * 4, words[i]);
}

/* Run a word sequence from CODE with a clean register file. Returns the run. */
static psp_interp run(const uint32_t *words, unsigned n, uint64_t budget) {
    memset(&psp_cpu, 0, sizeof psp_cpu);
    load(CODE, words, n);
    psp_interp it;
    psp_interp_init(&it, CODE, DONE, budget);
    psp_interp_run(&it);
    return it;
}

#define R(name) psp_cpu.r[PSP_REG_##name]

/* ---- basics -------------------------------------------------------------- */

static void test_alu(void) {
    /* addiu $v0,$zero,5 ; addiu $v1,$zero,7 ; addu $a0,$v0,$v1 ; jr $ra ; nop */
    static const uint32_t p[] = {
        0x24020005, 0x24030007, 0x00432021, 0x03E00008, 0x00000000,
    };
    psp_interp it = run(p, 5, 100);
    CHECK(it.status == I_OK_RETURN, "alu: %s", psp_interp_status_str(it.status));
    CHECK(R(V0) == 5, "addiu v0: got %u", R(V0));
    CHECK(R(V1) == 7, "addiu v1: got %u", R(V1));
    CHECK(R(A0) == 12, "addu a0: got %u", R(A0));
}

static void test_vfpu_random_pipeline(void) {
    /* Pipeline instructions in a delay slot must not trap or consume the
     * destination prefix. The following random triple masks lane 0, which
     * on a PSP keeps the last lane, where the first draw would go
     * (vfpuprobe v6 step 212, fw 6.60). */
    const uint32_t barriers[] = {0xFFFF0000,0xFFFF0320,0xFFFF040D};
    for (int i=0;i<3;i++) {
        const uint32_t code[] = {0xDE000100,0x10000001,barriers[i],
                                0xD0238000,0x03E00008,0};
        psp_vfpu_reset();
        psp_interp it=run(code,6,100);
        CHECK(it.status==I_OK_RETURN,"VFPU barrier/random interpreter path");
        int r[4]; psp_vfpu_regs(0,3,r);
        int drawn=0;
        for (int l=0;l<2;l++) drawn += psp_cpu.v[r[l]]>=2 && psp_cpu.v[r[l]]<4;
        CHECK(psp_cpu.v[r[2]]==0 && drawn==2,
              "VFPU barrier preserves prefix through branch delay slot");
    }
}

static void test_zero_is_hardwired(void) {
    /* addiu $zero,$zero,42 — the hardware discards this, and emit.c refuses to
     * emit it. If the interpreter let it through, every later comparison
     * against $zero would silently disagree with the recompiled path. */
    static const uint32_t p[] = { 0x24000000 | 42, 0x03E00008, 0x00000000 };
    psp_interp it = run(p, 3, 100);
    CHECK(it.status == I_OK_RETURN, "zero: %s", psp_interp_status_str(it.status));
    CHECK(psp_cpu.r[0] == 0, "$zero took a write: got %u", psp_cpu.r[0]);
}

static void test_shift_masks_amount(void) {
    /* srlv with a shift amount of 33: MIPS masks to 5 bits, so this is a shift
     * by 1, not undefined behaviour. This is why psp_srl exists at all. */
    static const uint32_t p[] = {
        0x24020010,             /* addiu $v0,$zero,16   */
        0x24030021,             /* addiu $v1,$zero,33   */
        0x00622006 | (4 << 11), /* srlv  $a0,$v0,$v1    */
        0x03E00008, 0x00000000,
    };
    psp_interp it = run(p, 5, 100);
    CHECK(it.status == I_OK_RETURN, "shift: %s", psp_interp_status_str(it.status));
    CHECK(R(A0) == 8, "srlv masked to 5 bits: got %u, want 8", R(A0));
}

static void test_load_store(void) {
    static const uint32_t p[] = {
        0x3C080891,             /* lui   $t0,0x0891     */
        0x2409ABCD,             /* addiu $t1,$zero,0xFFFFABCD (sign-extended) */
        0xAD090000,             /* sw    $t1,0($t0)     */
        0x8D0A0000,             /* lw    $t2,0($t0)     */
        0x910B0000,             /* lbu   $t3,0($t0)     */
        0x03E00008, 0x00000000,
    };
    psp_interp it = run(p, 7, 100);
    CHECK(it.status == I_OK_RETURN, "ldst: %s", psp_interp_status_str(it.status));
    CHECK(R(T2) == 0xFFFFABCDu, "lw round-trip: got %08X", R(T2));
    CHECK(R(T3) == 0xCD, "lbu low byte (LE): got %02X", R(T3));
}

/* ---- delay slots: the part that can actually be wrong --------------------- */

static void test_branch_taken_runs_slot(void) {
    /* beq $zero,$zero,+2 with `addiu $v0,$zero,1` in the slot.
     * Taken branch: the slot still executes. */
    static const uint32_t p[] = {
        0x10000002,             /* beq   $zero,$zero, +2 -> CODE+0x0C */
        0x24020001,             /* addiu $v0,$zero,1   (delay slot)   */
        0x24030001,             /* addiu $v1,$zero,1   (skipped)      */
        0x03E00008, 0x00000000,
    };
    psp_interp it = run(p, 5, 100);
    CHECK(it.status == I_OK_RETURN, "btaken: %s", psp_interp_status_str(it.status));
    CHECK(R(V0) == 1, "delay slot ran on taken branch");
    CHECK(R(V1) == 0, "branch skipped the instruction after the slot");
}

static void test_branch_not_taken_still_runs_slot(void) {
    /* bne $zero,$zero (never taken). A *plain* branch runs its delay slot
     * regardless — this is the case people get wrong by assuming the slot
     * belongs to the taken path. */
    static const uint32_t p[] = {
        0x14000002,             /* bne   $zero,$zero,+2 (not taken)   */
        0x24020001,             /* addiu $v0,$zero,1   (delay slot)   */
        0x24030001,             /* addiu $v1,$zero,1   (falls through)*/
        0x03E00008, 0x00000000,
    };
    psp_interp it = run(p, 5, 100);
    CHECK(it.status == I_OK_RETURN, "bnottaken: %s", psp_interp_status_str(it.status));
    CHECK(R(V0) == 1, "plain branch must run its slot even when not taken");
    CHECK(R(V1) == 1, "execution fell through past the slot");
}

static void test_likely_branch_nullifies_slot(void) {
    /* bnel $zero,$zero (not taken) — a *likely* branch nullifies its slot.
     * The mirror image of the previous test, and the reason the two forms
     * cannot share a code path. */
    static const uint32_t p[] = {
        0x54000002,             /* bnel  $zero,$zero,+2 (not taken)   */
        0x24020001,             /* addiu $v0,$zero,1   (NULLIFIED)    */
        0x24030001,             /* addiu $v1,$zero,1   (falls through)*/
        0x03E00008, 0x00000000,
    };
    psp_interp it = run(p, 5, 100);
    CHECK(it.status == I_OK_RETURN, "likely: %s", psp_interp_status_str(it.status));
    CHECK(R(V0) == 0, "not-taken likely branch must nullify its slot: v0=%u", R(V0));
    CHECK(R(V1) == 1, "execution fell through past the nullified slot");
}

static void test_jal_links_past_slot(void) {
    /* jal to the epilogue. $ra must be pc+8 — past the delay slot, not pc+4. */
    static const uint32_t target = (CODE + 0x10) >> 2;
    const uint32_t p[] = {
        0x0C000000 | (target & 0x03FFFFFF),  /* jal CODE+0x10 */
        0x00000000,                          /* delay slot    */
        0x24020001,                          /* skipped       */
        0x24030001,                          /* skipped       */
        0x03E00008, 0x00000000,              /* jr $ra        */
    };
    memset(&psp_cpu, 0, sizeof psp_cpu);
    load(CODE, p, 6);
    psp_interp it;
    psp_interp_init(&it, CODE, DONE, 100);
    psp_interp_step(&it);                    /* the jal + its slot */
    CHECK(psp_cpu.r[PSP_RA_INDEX] == CODE + 8,
          "jal $ra: got %08X, want %08X", psp_cpu.r[PSP_RA_INDEX], CODE + 8);
    CHECK(it.pc == CODE + 0x10, "jal target: got %08X", it.pc);
}

static void test_jr_target_read_before_slot(void) {
    /* The classic. `jr $t9` where the delay slot reassigns $t9: the jump uses
     * the value $t9 held *before* the slot ran. An interpreter that reads the
     * register after executing the slot goes somewhere else entirely, and the
     * resulting divergence looks like a wild jump with no obvious cause. */
    static const uint32_t p[] = {
        0x3C190890,             /* lui   $t9,0x0890          */
        0x37390010,             /* ori   $t9,$t9,0x0010      -> CODE+0x10 */
        0x03200008,             /* jr    $t9                 */
        0x3C190DEA,             /* lui   $t9,0x0DEA (slot: clobbers $t9)  */
        0x24020007,             /* CODE+0x10: addiu $v0,$zero,7           */
        0x03E00008, 0x00000000,
    };
    psp_interp it = run(p, 7, 100);
    CHECK(it.status == I_OK_RETURN, "jr: %s", psp_interp_status_str(it.status));
    CHECK(R(V0) == 7, "jr used the pre-slot register value (v0=%u, want 7)", R(V0));
    CHECK(R(T9) == 0x0DEA0000u, "the slot still executed: t9=%08X", R(T9));
}

static void test_link_branch_always_links(void) {
    /* bltzal with a non-negative operand: not taken, but a *link* branch writes
     * $ra either way. Missing this leaves $ra stale and the eventual return
     * goes to whatever the last call left behind. */
    static const uint32_t p[] = {
        0x04100002,             /* bltzal $zero,+2  (not taken, $zero >= 0) */
        0x00000000,             /* delay slot */
        0x03E00008, 0x00000000,
    };
    memset(&psp_cpu, 0, sizeof psp_cpu);
    load(CODE, p, 4);
    psp_interp it;
    psp_interp_init(&it, CODE, DONE, 100);
    psp_interp_step(&it);
    CHECK(psp_cpu.r[PSP_RA_INDEX] == CODE + 8,
          "bltzal links even when not taken: got %08X", psp_cpu.r[PSP_RA_INDEX]);
}

/* ---- termination --------------------------------------------------------- */

static void test_budget_stops_infinite_loop(void) {
    /* b . — branches to itself forever. The budget is what stops a bring-up
     * run from hanging instead of reporting. */
    static const uint32_t p[] = { 0x1000FFFF, 0x00000000 };
    psp_interp it = run(p, 2, 50);
    CHECK(it.status == I_BUDGET, "infinite loop must hit the budget, got %s",
          psp_interp_status_str(it.status));
    CHECK(it.executed >= 50, "budget counted %llu", (unsigned long long)it.executed);
}

static void test_syscall_traps(void) {
    /* syscall must stop the run rather than be skipped. A skipped syscall is
     * an HLE call that silently did not happen — the worst failure mode, since
     * execution continues on state that was never produced. */
    static const uint32_t p[] = { 0x0000000C, 0x03E00008, 0x00000000 };
    psp_interp it = run(p, 3, 100);
    CHECK(it.status == I_TRAP_SYSCALL, "syscall trap: got %s",
          psp_interp_status_str(it.status));
    CHECK(it.fault_pc == CODE, "fault pc: got %08X", it.fault_pc);
}

/* ---- drift guard ---------------------------------------------------------
 *
 * The oracle is only useful while the interpreter and the emitter implement
 * the same instruction set. If the emitter grows a case the interpreter lacks,
 * the interpreter traps on real code and the divergence report blames the game
 * for a hole in the oracle.
 *
 * These are real encodings for the ops emit.c translates. Any that trap here
 * are ops the emitter can emit and the interpreter cannot run. */

static void test_no_drift_from_emitter(void) {
    static const struct { uint32_t word; const char *what; } ops[] = {
        { 0x00851020, "add"   }, { 0x00851022, "sub"   }, { 0x00851024, "and"   },
        { 0x00851025, "or"    }, { 0x00851026, "xor"   }, { 0x00851027, "nor"   },
        { 0x0085102A, "slt"   }, { 0x0085102B, "sltu"  },
        { 0x0085102C, "max"   }, { 0x0085102D, "min"   },
        { 0x0085100A, "movz"  }, { 0x0085100B, "movn"  },
        { 0x24850010, "addiu" }, { 0x28850010, "slti"  }, { 0x2C850010, "sltiu" },
        { 0x30850010, "andi"  }, { 0x34850010, "ori"   }, { 0x38850010, "xori"  },
        { 0x3C050010, "lui"   },
        { 0x00052880, "sll"   }, { 0x00052882, "srl"   }, { 0x00052883, "sra"   },
        { 0x00A42804, "sllv"  }, { 0x00A42806, "srlv"  }, { 0x00A42807, "srav"  },
        { 0x00052046, "rotr"  },
        { 0x00A02816, "clz"   }, { 0x00A02817, "clo"   },
        { 0x7C052420, "seb"   }, { 0x7C052620, "seh"   },
        { 0x7C0528A0, "wsbh"  }, { 0x7C0528E0, "wsbw"  },
        { 0x7CA53C00, "ext"   }, { 0x7CA53C04, "ins"   },
        { 0x00A40018, "mult"  }, { 0x00A40019, "multu" },
        { 0x00A4001A, "div"   }, { 0x00A4001B, "divu"  },
        { 0x00A4001C, "madd"  }, { 0x00A4001D, "maddu" },
        { 0x00002810, "mfhi"  }, { 0x00002812, "mflo"  },
        { 0x00A00011, "mthi"  }, { 0x00A00013, "mtlo"  },
        { 0x80A40000, "lb"    }, { 0x90A40000, "lbu"   },
        { 0x84A40000, "lh"    }, { 0x94A40000, "lhu"   },
        { 0x8CA40000, "lw"    },
        { 0x88A40000, "lwl"   }, { 0x98A40000, "lwr"   },
        { 0xA0A40000, "sb"    }, { 0xA4A40000, "sh"    }, { 0xACA40000, "sw"    },
        { 0xA8A40000, "swl"   }, { 0xB8A40000, "swr"   },
        { 0xBCA40000, "cache" }, { 0xCCA40000, "pref"  },
        { 0x44852000, "mtc1"  }, { 0x44052000, "mfc1"  },
        { 0xC4A40000, "lwc1"  }, { 0xE4A40000, "swc1"  },
        { 0x46062100, "add.s" }, { 0x46062101, "sub.s" },
        { 0x46062102, "mul.s" }, { 0x46062103, "div.s" },
        { 0x46002106, "mov.s" }, { 0x46002107, "neg.s" },
        { 0x46002105, "abs.s" }, { 0x46002104, "sqrt.s"},
        { 0x40056000, "mfc0"  }, { 0x40856000, "mtc0"  },
    };

    CHECK(psp_mem_init() == 0, "memory init for drift guard");
    memset(&psp_cpu, 0, sizeof psp_cpu);

    for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++) {
        a_insn in;
        /* Re-seed the base register every iteration: several entries in the
         * table write $a1, and a clobbered base turns every later load and
         * store into an unmapped access that has nothing to do with drift. */
        psp_cpu.r[PSP_REG_A1] = DATA;
        a_decode(ops[i].word, CODE, &in);
        CHECK(in.op != A_INVALID, "%s: 0x%08X did not decode",
              ops[i].what, ops[i].word);
        if (in.op == A_INVALID) continue;
        psp_interp_status st = psp_interp_exec_one(&in);
        CHECK(st == I_RUNNING, "%s: emitter translates it, interpreter returned %s",
              ops[i].what, psp_interp_status_str(st));
    }
    psp_mem_free();
}

/* An mfvc of CC as the very next word after a vcmp reads CC from before that
 * vcmp; one word later, the new CC (vfpuprobe v3 step 117, fw 6.60). */
static void test_mfvc_right_after_vcmp(void) {
    static const uint32_t prog[] = {
        0x6C008084,  /* vcmp.q TR, C000, C000   CC = 3F                      */
        0x6C008080,  /* vcmp.q FL, C000, C000   CC = 00                      */
        0x48620083,  /* mfvc $v0, $131          straight after: 3F           */
        0x48630083,  /* mfvc $v1, $131          one later: 00                */
        0x6C008084,  /* vcmp.q TR                                            */
        0x00000000,  /* nop                                                  */
        0x48640083,  /* mfvc $a0, $131          two later: 3F                */
        0x03E00008,  /* jr $ra                                               */
        0x00000000,
    };
    CHECK(psp_mem_init() == 0, "memory init for mfvc latency");
    psp_interp it = run(prog, sizeof prog / sizeof prog[0], 100);
    (void)it;
    CHECK(R(V0) == 0x3Fu && R(V1) == 0x00u && R(A0) == 0x3Fu,
          "mfvc after vcmp: v0 %02X v1 %02X a0 %02X, want 3F 00 3F", R(V0), R(V1), R(A0));
    psp_mem_free();
}

/* A COP1 operation raising an exception whose enable is set stops the
 * program: 1/0 under the power-on 00000E00 switched a PSP off (v3 step 189). */
static void test_fpu_enabled_exception_traps(void) {
    static const uint32_t prog[] = {
        0x3C013F80,  /* lui $at, 0x3F80                                      */
        0x44812000,  /* mtc1 $at, $f4          1.0                           */
        0x44803000,  /* mtc1 $zero, $f6        0.0                           */
        0x46062103,  /* div.s $f4, $f4, $f6                                  */
        0x24020001,  /* addiu $v0, $zero, 1    not reached                   */
        0x03E00008,  /* jr $ra                                               */
        0x00000000,
    };
    CHECK(psp_mem_init() == 0, "memory init for fpu trap");
    memset(&psp_cpu, 0, sizeof psp_cpu);
    load(CODE, prog, sizeof prog / sizeof prog[0]);
    psp_cpu.fcr31 = 0x00000E00u;
    psp_interp it;
    psp_interp_init(&it, CODE, DONE, 100);
    const psp_interp_status st = psp_interp_run(&it);
    CHECK(st == I_TRAP_FPU && R(V0) == 0,
          "1/0 with Z enabled: %s, v0 %u", psp_interp_status_str(st), R(V0));
    memset(&psp_cpu, 0, sizeof psp_cpu);
    load(CODE, prog, sizeof prog / sizeof prog[0]);
    psp_interp_init(&it, CODE, DONE, 100);
    const psp_interp_status st2 = psp_interp_run(&it);
    CHECK(st2 != I_TRAP_FPU && R(V0) == 1 && psp_cpu.fcr31 == 0x00008020u,
          "1/0 with nothing enabled runs on: %s, v0 %u fcr31 %08X",
          psp_interp_status_str(st2), R(V0), psp_cpu.fcr31);
    psp_mem_free();
}

/* ---- HLE re-entry: --dispatch ---------------------------------------------- */

/* The two handlers below are what a firmware function looks like to the
 * interpreter: C, running outside the instruction stream, that reaches back
 * into guest code. The first via psp_dispatch (a registered callback), the
 * second by starting a thread. With psp_interp_service_dispatch enabled both
 * run interpreted and nested; without it, a program run stops at the first
 * one -- which is exactly why pspautotests need the flag. */

static uint32_t g_dispatch_target;

static void hle_dispatch_target(void) {
    psp_dispatch(g_dispatch_target);
}

static void hle_spawn_thread(void) {
    psp_sched_spawn(0x00040010u, THREAD, THREAD_SP, 0, 7, 8, 32);
}

static void test_dispatch_serving(void) {
    psp_hle_init();
    g_dispatch_target = TARGET;

    const uint32_t nid = psp_nid("testDispatchTarget");
    psp_hle_register(nid, "test", "testDispatchTarget", hle_dispatch_target);
    psp_interp_import imp = { THUNK, nid };
    CHECK(psp_interp_set_imports(&imp, 1) == 1, "import table");

    /* The stub itself: an import entry is jr $ra with a slot, and the
     * interpreter runs it after the HLE call to get back to the caller.
     * Leaving the address zeroed means the run falls through nops instead. */
    static const uint32_t stub[] = { 0x03E00008, 0x00000000 };
    load(THUNK, stub, 2);

    /* The dispatched target: addiu $v0,$zero,42 ; jr $ra ; nop */
    static const uint32_t tgt[] = { 0x2402002A, 0x03E00008, 0x00000000 };
    load(TARGET, tgt, 3);

    /* The caller: lui $t9,THUNK ; jr $t9 ; nop. Reaching the thunk with a
     * non-linking jump leaves $ra holding the run sentinel, so the run ends
     * the moment the thunk's handler returns -- a jal would overwrite $ra
     * with its own link and jr $ra would then jump to itself forever. */
    static const uint32_t outer[] = {
        0x3C190892,             /* lui  $t9,0x0892      */
        0x03200008, 0x00000000, /* jr   $t9 ; slot      */
    };

    psp_interp_service_dispatch(1);
    psp_interp it = run(outer, 4, 1000);
    psp_interp_service_dispatch(0);
    psp_interp_free_imports();

    CHECK(it.status == I_OK_RETURN, "dispatch: %s", psp_interp_status_str(it.status));
    CHECK(R(V0) == 42, "nested dispatch left its $v0 for the caller: got %u", R(V0));
    CHECK(psp_interp_nest_refused() == 0, "nothing should be refused here");
}

/* A thread started inside a run executes to completion, nested, on its own
 * register file -- and the starter's registers survive it. */
static void test_spawn_serving(void) {
    psp_hle_init();
    /* The oracle's configuration, stated rather than inherited: it disables
     * threading outright, which is what leaves spawn_hook as the only thing
     * that can service a thread start. The threaded model is the next test. */
    psp_sched_set_threading(0);

    const uint32_t nid = psp_nid("testSpawnHost");
    psp_hle_register(nid, "test", "testSpawnHost", hle_spawn_thread);
    psp_interp_import imp = { THUNK, nid };
    CHECK(psp_interp_set_imports(&imp, 1) == 1, "import table");

    static const uint32_t stub[] = { 0x03E00008, 0x00000000 };
    load(THUNK, stub, 2);

    /* Thread body: lui $t0,0x0891 ; sw $a0,0($t0) ; jr $ra ; nop */
    static const uint32_t body[] = { 0x3C080891, 0xAD040000, 0x03E00008, 0x00000000 };
    load(THREAD, body, 4);

    /* The starter: addiu $s0,$zero,99 ; lui $t9,THUNK ; jr $t9 ; nop -- the
     * same non-linking jump, so $ra keeps the sentinel. */
    static const uint32_t outer[] = {
        0x24100063,             /* addiu $s0,$zero,99   */
        0x3C190892,             /* lui   $t9,0x0892     */
        0x03200008, 0x00000000, /* jr    $t9 ; slot     */
    };

    psp_interp_service_dispatch(1);
    psp_interp it = run(outer, 4, 1000);
    /* Sampled before the hooks come down: disabling them resets the pending
     * list, the same fresh-run guarantee that resets the nesting counter. */
    const uint32_t data_before_drain = psp_read32(DATA);
    psp_interp_drain_pending(1000);
    psp_interp_service_dispatch(0);
    psp_interp_free_imports();

    CHECK(it.status == I_OK_RETURN, "spawn: %s", psp_interp_status_str(it.status));

    /* Started at priority 32, by a starter also at 32. Equal is not more
     * urgent, so the thread is runnable and has *not* run: a PSP reschedules
     * at StartThread only for a thread that outranks the caller. Running it
     * here is the behaviour pspautotests' checkpoint helper detects, and it
     * tagged every line of the threads suite `[r]` instead of `[x]`. */
    CHECK(data_before_drain != 7,
          "an equal-priority thread does not run at StartThread: DATA=%u",
          data_before_drain);

    CHECK(psp_read32(DATA) == 7,
          "and runs when the top-level run drains it, with StartThread's $a0: DATA=%u",
          psp_read32(DATA));
    CHECK(R(S0) == 99,
          "the starter's registers survived the thread: s0=%u", R(S0));
    psp_sched_set_threading(1);
}

/* ---- HLE re-entry with real threads ---------------------------------------
 *
 * The property the parked model cannot have, and the whole reason for
 * psp_interp_service_threads: a thread that *blocks* is resumed.
 *
 * Under spawn_hook a started thread runs nested to completion inside its
 * starter's frame, so a blocked one has no context to be resumed into --
 * psp_sched_block finds nothing runnable and the wait has to fail. Here each
 * guest thread gets a host thread whose C stack holds its interpreter run, so
 * the starter can park, the thread can run, and the starter can carry on from
 * the instruction after the wait.
 *
 * The blocking is done from HLE rather than from hand-assembled guest code
 * because it is the scheduling that is under test, not the encoding of a
 * semaphore protocol. The guest side is what it must be: two separate
 * interpreter runs on two host stacks. */
#define WAKER_UID 0x00040011u

static int g_starter_blocked, g_starter_resumed, g_woke_from_thread;

static void hle_wake_starter(void) {
    /* Runs on the spawned thread's own host thread, from its own guest code. */
    g_woke_from_thread = 1;
    psp_sched_wake(0);                     /* uid 0 is the main context */
}

static void hle_spawn_and_block(void) {
    psp_sched_spawn(WAKER_UID, THREAD, THREAD_SP, 0, 7, 8, 32);
    g_starter_blocked = 1;
    /* Parks the caller and hands the token to the thread just started. Returns
     * 0 once something has made this slot runnable again. */
    const int rc = psp_sched_block(0, PSP_SCHED_BLOCKED, "test-resume");
    g_starter_resumed = (rc == 0);
}

static void test_threaded_spawn_resumes_a_blocked_starter(void) {
    psp_hle_init();
    psp_sched_set_threading(1);
    g_starter_blocked = g_starter_resumed = g_woke_from_thread = 0;
    psp_write32(DATA, 0);

    const uint32_t spawn_nid = psp_nid("testSpawnAndBlock");
    const uint32_t wake_nid  = psp_nid("testWakeStarter");
    psp_hle_register(spawn_nid, "test", "testSpawnAndBlock", hle_spawn_and_block);
    psp_hle_register(wake_nid,  "test", "testWakeStarter",   hle_wake_starter);

    /* Two thunks: one the starter jumps to, one the thread calls. */
    const psp_interp_import imps[] = { { THUNK, spawn_nid }, { TARGET, wake_nid } };
    CHECK(psp_interp_set_imports(imps, 2) == 2, "import table");

    static const uint32_t stub[] = { 0x03E00008, 0x00000000 };   /* jr $ra ; nop */
    load(THUNK,  stub, 2);
    load(TARGET, stub, 2);

    /* Thread body: record $a0, then reach the waking thunk with a *non-linking*
     * jump. A jal would overwrite $ra, and $ra is holding the sentinel that
     * ends this thread's run -- the thunk returns by setting pc to it. */
    static const uint32_t body[] = {
        0x3C080891,             /* lui  $t0,0x0891       */
        0xAD040000,             /* sw   $a0,0($t0)       */
        0x3C190893,             /* lui  $t9,0x0893       */
        0x03200008, 0x00000000, /* jr   $t9 ; slot       */
    };
    load(THREAD, body, 5);

    /* The starter: s0=99, then a non-linking jump to the thunk so $ra keeps the
     * run sentinel and the run ends when the handler returns. */
    static const uint32_t outer[] = {
        0x24100063,             /* addiu $s0,$zero,99    */
        0x3C190892,             /* lui   $t9,0x0892      */
        0x03200008, 0x00000000, /* jr    $t9 ; slot      */
    };

    psp_interp_service_dispatch(1);
    psp_interp_service_threads(1, 10000);
    psp_interp it = run(outer, 4, 10000);
    const int live = psp_sched_drain(5);
    psp_interp_service_threads(0, 0);
    psp_interp_service_dispatch(0);
    psp_interp_free_imports();

    CHECK(it.status == I_OK_RETURN, "threaded spawn: %s",
          psp_interp_status_str(it.status));
    CHECK(g_starter_blocked, "the starter never reached its block");
    CHECK(psp_read32(DATA) == 7,
          "the thread never ran with StartThread's $a0: DATA=%u", psp_read32(DATA));
    CHECK(g_woke_from_thread,
          "the thread's own guest code never reached the waking thunk");
    CHECK(g_starter_resumed,
          "the starter blocked and was never resumed -- this is the whole "
          "difference between the two models");
    CHECK(R(S0) == 99, "the starter's registers survived: s0=%u", R(S0));
    CHECK(live == 0, "%d thread(s) still alive after the drain", live);
}

/* -------------------------------------------------------------------------- */

int main(void) {
    if (psp_mem_init() != 0) {
        printf("FAIL: psp_mem_init\n");
        return 1;
    }

    test_alu();
    test_vfpu_random_pipeline();
    test_zero_is_hardwired();
    test_shift_masks_amount();
    test_load_store();

    test_branch_taken_runs_slot();
    test_branch_not_taken_still_runs_slot();
    test_likely_branch_nullifies_slot();
    test_jal_links_past_slot();
    test_jr_target_read_before_slot();
    test_link_branch_always_links();

    test_budget_stops_infinite_loop();
    test_syscall_traps();
    test_dispatch_serving();
    test_spawn_serving();
    test_threaded_spawn_resumes_a_blocked_starter();

    psp_mem_free();
    test_no_drift_from_emitter();
    test_mfvc_right_after_vcmp();
    test_fpu_enabled_exception_traps();

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("interp: all checks passed\n");
    return 0;
}
