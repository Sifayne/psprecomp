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

#include "psprecomp/cpu.h"
#include "psprecomp/mem.h"

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

/* -------------------------------------------------------------------------- */

int main(void) {
    if (psp_mem_init() != 0) {
        printf("FAIL: psp_mem_init\n");
        return 1;
    }

    test_alu();
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

    psp_mem_free();
    test_no_drift_from_emitter();

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("interp: all checks passed\n");
    return 0;
}
