/* allegrexrecomp — the Allegrex interpreter oracle. See interp.h.
 *
 * The switch below deliberately mirrors emit.c's emit_simple() case for case.
 * That looks like duplication and is not: emit_simple *writes C text*, this
 * *executes*, and neither can be expressed in terms of the other without a
 * refactor that would put a layer of indirection between the emitter and the
 * code it generates.
 *
 * What stops the two drifting is that neither one implements any semantics.
 * Both call the same helpers out of recomp_rt.h and vfpu.h, so a bug in `psp_sra`
 * or `psp_div` is present identically on both sides and cannot show up as a
 * false divergence. The parts that *can* drift are the dispatch and the
 * sequencing — which is exactly what test_interp.c's coverage guard checks.
 */

#include "interp.h"

#include "decode.h"

#include "psprecomp/cpu.h"
#include "psprecomp/dispatch.h"
#include "psprecomp/hle.h"
#include "psprecomp/mem.h"
#include "psprecomp/recomp_rt.h"
#include "psprecomp/sched.h"
#include "psprecomp/vfpu.h"

#include <stdlib.h>
#include <string.h>

/* ---- register access ------------------------------------------------------
 *
 * $zero is hardwired. The hardware discards writes to it; emit.c does the same
 * by refusing to emit them (its DEST_ZERO check). Dropping them here keeps the
 * two paths identical -- and a stray write would corrupt a register the rest of
 * the code is entitled to assume is always zero. */

#define R(i)   (psp_cpu.r[(i)])

static void setr(unsigned idx, uint32_t v) {
    if (idx != 0) psp_cpu.r[idx] = v;
}

/* ---- the firmware boundary ------------------------------------------------
 *
 * See interp.h. Thunks are dense and 4-aligned inside .sceStub.text, so a flat
 * array indexed by word offset from the lowest one is both the simplest and the
 * fastest structure: this is consulted once per instruction, and a linear scan
 * over a couple of hundred imports would dominate the interpreter's run time. */

static uint32_t *g_imp_nid;    /* [ (addr - lo) / 4 ] -> NID, 0 = not a thunk */
static uint32_t  g_imp_lo, g_imp_hi;

void psp_interp_free_imports(void) {
    free(g_imp_nid);
    g_imp_nid = NULL;
    g_imp_lo = g_imp_hi = 0;
}

int psp_interp_set_imports(const psp_interp_import *tbl, int n) {
    psp_interp_free_imports();
    if (!tbl || n <= 0) return 0;

    uint32_t lo = UINT32_MAX, hi = 0;
    for (int i = 0; i < n; i++) {
        if (tbl[i].addr < lo) lo = tbl[i].addr;
        if (tbl[i].addr + 4 > hi) hi = tbl[i].addr + 4;
    }
    if (lo >= hi) return 0;

    const size_t slots = (hi - lo) / 4;
    g_imp_nid = (uint32_t *)calloc(slots, sizeof *g_imp_nid);
    if (!g_imp_nid) return -1;
    g_imp_lo = lo;
    g_imp_hi = hi;

    /* A NID of 0 would be indistinguishable from an empty slot. No real import
     * hashes to 0, but rather than rely on that, such an entry is dropped and
     * the thunk simply executes as ordinary code. */
    int kept = 0;
    for (int i = 0; i < n; i++) {
        if (!tbl[i].nid) continue;
        g_imp_nid[(tbl[i].addr - lo) / 4] = tbl[i].nid;
        kept++;
    }
    return kept;
}

/* Which firmware call a run leaned on hardest.
 *
 * A run that exhausts its instruction budget has looped, and the useful
 * question is what it was waiting for. A spin against an unimplemented
 * function -- which returns zero forever, so the guest's condition never
 * changes -- shows up here as one NID with an enormous count. That names the
 * blocker instead of just reporting that something hung. */
static uint32_t g_hot_nid, g_hot_count, g_last_nid, g_last_run;

void psp_interp_hle_reset(void) { g_hot_nid = g_hot_count = g_last_nid = g_last_run = 0; }

uint32_t psp_interp_hot_nid(uint32_t *count) {
    if (count) *count = g_hot_count;
    return g_hot_nid;
}

static void note_hle(uint32_t nid) {
    if (nid == g_last_nid) g_last_run++;
    else { g_last_nid = nid; g_last_run = 1; }
    if (g_last_run > g_hot_count) { g_hot_count = g_last_run; g_hot_nid = nid; }
}

/* ---- execution profile ---------------------------------------------------
 *
 * See interp.h. A flat count per instruction word: the module is a few million
 * words, so this is a handful of megabytes and one increment per step. */

static uint32_t *g_prof;
static uint32_t  g_prof_base, g_prof_words;

void psp_interp_profile(uint32_t base, uint32_t words) {
    free(g_prof);
    g_prof = NULL;
    g_prof_base = base;
    g_prof_words = words;
    if (words) g_prof = (uint32_t *)calloc(words, sizeof *g_prof);
    if (!g_prof) g_prof_words = 0;
}

void psp_interp_profile_reset(void) {
    if (g_prof) memset(g_prof, 0, (size_t)g_prof_words * sizeof *g_prof);
}

uint32_t psp_interp_hot_pc(uint32_t *count) {
    uint32_t best = 0, best_n = 0;
    for (uint32_t i = 0; i < g_prof_words; i++)
        if (g_prof[i] > best_n) { best_n = g_prof[i]; best = i; }
    if (count) *count = best_n;
    return g_prof_base + best * 4;
}

static void note_pc(uint32_t pc) {
    if (!g_prof) return;
    const uint32_t i = (pc - g_prof_base) / 4;
    if (i < g_prof_words) g_prof[i]++;
}

static uint32_t import_nid_at(uint32_t pc) {
    if (!g_imp_nid || pc < g_imp_lo || pc >= g_imp_hi) return 0;
    return g_imp_nid[(pc - g_imp_lo) / 4];
}

/* ---- tracing --------------------------------------------------------------
 *
 * The trace is one line per step: address, mnemonic, then whatever state the
 * step changed. Memory writes are logged where they happen rather than
 * reconstructed afterwards, so a store to an address nothing later reads is
 * still visible -- those are exactly the ones worth seeing.
 *
 * `g_trace` is a file-static rather than a parameter so the executor's
 * signature stays about instructions. This is a single-threaded development
 * tool; if that ever changes, this is the thing to fix. */

static psp_interp *g_trace;

static void trace_mem(char kind, uint32_t addr, uint32_t val) {
    if (g_trace && g_trace->trace)
        fprintf(g_trace->trace, " M%c[%08X]=%08X", kind, addr, val);
}

/* ---- store helpers --------------------------------------------------------
 *
 * Wrapped only so stores land in the trace. The write itself goes through the
 * same psp_write* the recompiled code uses. */

static void st8 (uint32_t a, uint8_t  v) { psp_write8 (a, v); trace_mem('8', a, v); }
static void st16(uint32_t a, uint16_t v) { psp_write16(a, v); trace_mem('H', a, v); }
static void st32(uint32_t a, uint32_t v) { psp_write32(a, v); trace_mem('W', a, v); }

/* ---- one instruction, no control flow -------------------------------------
 *
 * Returns I_RUNNING when the instruction executed, or a trap status. Control
 * transfer is not handled here: the caller owns pc.
 *
 * Case order follows emit.c so the two can be diffed by eye when either
 * changes. */

static psp_interp_status exec_simple(const a_insn *in) {
    switch (in->op) {
    case A_NOP:
        return I_RUNNING;

    /* --- ALU, register --- */
    case A_ADD: case A_ADDU: setr(in->rd, R(in->rs) + R(in->rt));  return I_RUNNING;
    case A_SUB: case A_SUBU: setr(in->rd, R(in->rs) - R(in->rt));  return I_RUNNING;
    case A_AND:  setr(in->rd, R(in->rs) &  R(in->rt));             return I_RUNNING;
    case A_OR:   setr(in->rd, R(in->rs) |  R(in->rt));             return I_RUNNING;
    case A_XOR:  setr(in->rd, R(in->rs) ^  R(in->rt));             return I_RUNNING;
    case A_NOR:  setr(in->rd, ~(R(in->rs) | R(in->rt)));           return I_RUNNING;
    case A_SLT:  setr(in->rd, psp_slt (R(in->rs), R(in->rt)));     return I_RUNNING;
    case A_SLTU: setr(in->rd, psp_sltu(R(in->rs), R(in->rt)));     return I_RUNNING;
    case A_MAX:  setr(in->rd, psp_max (R(in->rs), R(in->rt)));     return I_RUNNING;
    case A_MIN:  setr(in->rd, psp_min (R(in->rs), R(in->rt)));     return I_RUNNING;
    case A_MOVZ: if (R(in->rt) == 0) setr(in->rd, R(in->rs));      return I_RUNNING;
    case A_MOVN: if (R(in->rt) != 0) setr(in->rd, R(in->rs));      return I_RUNNING;

    /* --- ALU, immediate --- */
    case A_ADDI: case A_ADDIU: setr(in->rt, R(in->rs) + (uint32_t)in->imm); return I_RUNNING;
    case A_SLTI:  setr(in->rt, psp_slt (R(in->rs), (uint32_t)in->imm)); return I_RUNNING;
    case A_SLTIU: setr(in->rt, psp_sltu(R(in->rs), (uint32_t)in->imm)); return I_RUNNING;
    case A_ANDI:  setr(in->rt, R(in->rs) & (uint32_t)in->imm);          return I_RUNNING;
    case A_ORI:   setr(in->rt, R(in->rs) | (uint32_t)in->imm);          return I_RUNNING;
    case A_XORI:  setr(in->rt, R(in->rs) ^ (uint32_t)in->imm);          return I_RUNNING;
    case A_LUI:   setr(in->rt, (uint32_t)in->imm << 16);                return I_RUNNING;

    /* --- shifts. The helpers exist because C leaves shift-by->=32 undefined
       while MIPS masks the amount to five bits. --- */
    case A_SLL:   setr(in->rd, psp_sll (R(in->rt), in->sa));       return I_RUNNING;
    case A_SRL:   setr(in->rd, psp_srl (R(in->rt), in->sa));       return I_RUNNING;
    case A_SRA:   setr(in->rd, psp_sra (R(in->rt), in->sa));       return I_RUNNING;
    case A_ROTR:  setr(in->rd, psp_rotr(R(in->rt), in->sa));       return I_RUNNING;
    case A_SLLV:  setr(in->rd, psp_sll (R(in->rt), R(in->rs)));    return I_RUNNING;
    case A_SRLV:  setr(in->rd, psp_srl (R(in->rt), R(in->rs)));    return I_RUNNING;
    case A_SRAV:  setr(in->rd, psp_sra (R(in->rt), R(in->rs)));    return I_RUNNING;
    case A_ROTRV: setr(in->rd, psp_rotr(R(in->rt), R(in->rs)));    return I_RUNNING;

    /* --- bit manipulation --- */
    case A_CLZ:    setr(in->rd, psp_clz   (R(in->rs))); return I_RUNNING;
    case A_CLO:    setr(in->rd, psp_clo   (R(in->rs))); return I_RUNNING;
    case A_SEB:    setr(in->rd, psp_seb   (R(in->rt))); return I_RUNNING;
    case A_SEH:    setr(in->rd, psp_seh   (R(in->rt))); return I_RUNNING;
    case A_WSBH:   setr(in->rd, psp_wsbh  (R(in->rt))); return I_RUNNING;
    case A_WSBW:   setr(in->rd, psp_wsbw  (R(in->rt))); return I_RUNNING;
    case A_BITREV: setr(in->rd, psp_bitrev(R(in->rt))); return I_RUNNING;
    case A_EXT:
        setr(in->rt, psp_ext(R(in->rs), in->sa, (unsigned)in->rd + 1u));
        return I_RUNNING;
    case A_INS:
        setr(in->rt, psp_ins(R(in->rt), R(in->rs), in->sa,
                             (unsigned)in->rd - in->sa + 1u));
        return I_RUNNING;

    /* --- multiply / divide. These write HI/LO, never a GPR. --- */
    case A_MULT:  psp_mult (R(in->rs), R(in->rt)); return I_RUNNING;
    case A_MULTU: psp_multu(R(in->rs), R(in->rt)); return I_RUNNING;
    case A_DIV:   psp_div  (R(in->rs), R(in->rt)); return I_RUNNING;
    case A_DIVU:  psp_divu (R(in->rs), R(in->rt)); return I_RUNNING;
    case A_MADD:  psp_madd (R(in->rs), R(in->rt)); return I_RUNNING;
    case A_MADDU: psp_maddu(R(in->rs), R(in->rt)); return I_RUNNING;
    case A_MSUB:  psp_msub (R(in->rs), R(in->rt)); return I_RUNNING;
    case A_MSUBU: psp_msubu(R(in->rs), R(in->rt)); return I_RUNNING;
    case A_MFHI:  setr(in->rd, psp_cpu.hi);       return I_RUNNING;
    case A_MFLO:  setr(in->rd, psp_cpu.lo);       return I_RUNNING;
    case A_MTHI:  psp_cpu.hi = R(in->rs);         return I_RUNNING;
    case A_MTLO:  psp_cpu.lo = R(in->rs);         return I_RUNNING;

    /* --- loads --- */
    case A_LB:
        setr(in->rt, (uint32_t)(int32_t)(int8_t)psp_read8(R(in->rs) + in->imm));
        return I_RUNNING;
    case A_LBU:
        setr(in->rt, psp_read8(R(in->rs) + in->imm));
        return I_RUNNING;
    case A_LH:
        setr(in->rt, (uint32_t)(int32_t)(int16_t)psp_read16(R(in->rs) + in->imm));
        return I_RUNNING;
    case A_LHU:
        setr(in->rt, psp_read16(R(in->rs) + in->imm));
        return I_RUNNING;
    case A_LW: case A_LL:
        setr(in->rt, psp_read32(R(in->rs) + in->imm));
        return I_RUNNING;
    case A_LWL:
        setr(in->rt, psp_lwl(R(in->rt), R(in->rs) + in->imm));
        return I_RUNNING;
    case A_LWR:
        setr(in->rt, psp_lwr(R(in->rt), R(in->rs) + in->imm));
        return I_RUNNING;

    /* --- stores --- */
    case A_SB: st8 (R(in->rs) + in->imm, (uint8_t )R(in->rt)); return I_RUNNING;
    case A_SH: st16(R(in->rs) + in->imm, (uint16_t)R(in->rt)); return I_RUNNING;
    case A_SW: st32(R(in->rs) + in->imm,           R(in->rt)); return I_RUNNING;
    case A_SWL:
        psp_swl(R(in->rt), R(in->rs) + in->imm);
        trace_mem('L', R(in->rs) + in->imm, R(in->rt));
        return I_RUNNING;
    case A_SWR:
        psp_swr(R(in->rt), R(in->rs) + in->imm);
        trace_mem('R', R(in->rs) + in->imm, R(in->rt));
        return I_RUNNING;
    case A_SC:
        /* No multiprocessor to contend with, so the store always succeeds. */
        st32(R(in->rs) + in->imm, R(in->rt));
        setr(in->rt, 1);
        return I_RUNNING;

    /* Cache and prefetch hints have no meaning without a cache model. */
    case A_CACHE: case A_PREF: case A_SYNC:
        return I_RUNNING;

    /* --- COP1, single precision only --- */
    case A_MTC1: psp_cpu.f[in->fs] = psp_bits_to_f32(R(in->rt));      return I_RUNNING;
    case A_MFC1: setr(in->rt, psp_f32_to_bits(psp_cpu.f[in->fs]));    return I_RUNNING;
    case A_CTC1: psp_cpu.fcr31 = R(in->rt);                           return I_RUNNING;
    case A_CFC1: setr(in->rt, psp_cpu.fcr31);                         return I_RUNNING;
    case A_LWC1: psp_cpu.f[in->ft] = psp_read_f32(R(in->rs) + in->imm); return I_RUNNING;
    case A_SWC1:
        psp_write_f32(R(in->rs) + in->imm, psp_cpu.f[in->ft]);
        trace_mem('F', R(in->rs) + in->imm, psp_f32_to_bits(psp_cpu.f[in->ft]));
        return I_RUNNING;
    case A_ADD_S: psp_cpu.f[in->fd] = psp_cpu.f[in->fs] + psp_cpu.f[in->ft]; return I_RUNNING;
    case A_SUB_S: psp_cpu.f[in->fd] = psp_cpu.f[in->fs] - psp_cpu.f[in->ft]; return I_RUNNING;
    case A_MUL_S: psp_cpu.f[in->fd] = psp_cpu.f[in->fs] * psp_cpu.f[in->ft]; return I_RUNNING;
    case A_DIV_S: psp_cpu.f[in->fd] = psp_cpu.f[in->fs] / psp_cpu.f[in->ft]; return I_RUNNING;
    case A_MOV_S: psp_cpu.f[in->fd] =  psp_cpu.f[in->fs];                    return I_RUNNING;
    case A_NEG_S: psp_cpu.f[in->fd] = -psp_cpu.f[in->fs];                    return I_RUNNING;
    case A_ABS_S: psp_cpu.f[in->fd] = psp_fabs (psp_cpu.f[in->fs]);          return I_RUNNING;
    case A_SQRT_S:psp_cpu.f[in->fd] = psp_fsqrt(psp_cpu.f[in->fs]);          return I_RUNNING;
    case A_CVT_S_W:
        psp_cpu.f[in->fd] = (float)(int32_t)psp_f32_to_bits(psp_cpu.f[in->fs]);
        return I_RUNNING;
    case A_CVT_W_S: case A_TRUNC_W_S:
        psp_cpu.f[in->fd] = psp_bits_to_f32((uint32_t)(int32_t)psp_cpu.f[in->fs]);
        return I_RUNNING;
    case A_C_COND_S:
        psp_fpu_set_cond(psp_fcmp(in->fcond, psp_cpu.f[in->fs], psp_cpu.f[in->ft]));
        return I_RUNNING;

    /* --- COP0. There is no privileged state to model. --- */
    case A_MFC0: case A_CFC0: case A_MFIC: setr(in->rt, 0); return I_RUNNING;
    case A_MTC0: case A_CTC0: case A_MTIC:                  return I_RUNNING;

    /* --- VFPU: the subset with a real implementation behind it. --- */
    case A_LV_S: psp_lv_s(in->vt, R(in->rs) + (in->imm & ~3)); return I_RUNNING;
    case A_LV_Q: psp_lv_q(in->vt, R(in->rs) + (in->imm & ~3)); return I_RUNNING;
    case A_SV_S: psp_sv_s(in->vt, R(in->rs) + (in->imm & ~3)); return I_RUNNING;
    case A_SV_Q: psp_sv_q(in->vt, R(in->rs) + (in->imm & ~3)); return I_RUNNING;

    case A_VADD: psp_vadd(in->vd, in->vs, in->vt, in->vsize); return I_RUNNING;
    case A_VSUB: psp_vsub(in->vd, in->vs, in->vt, in->vsize); return I_RUNNING;
    case A_VMUL: psp_vmul(in->vd, in->vs, in->vt, in->vsize); return I_RUNNING;
    case A_VDIV: psp_vdiv(in->vd, in->vs, in->vt, in->vsize); return I_RUNNING;
    case A_VMIN: psp_vmin(in->vd, in->vs, in->vt, in->vsize); return I_RUNNING;
    case A_VMAX: psp_vmax(in->vd, in->vs, in->vt, in->vsize); return I_RUNNING;
    case A_VDOT: psp_vdot(in->vd, in->vs, in->vt, in->vsize); return I_RUNNING;
    case A_VSCL: psp_vscl(in->vd, in->vs, in->vt, in->vsize); return I_RUNNING;
    case A_VCMP: psp_vcmp(in->vd & 0xF, in->vs, in->vt, in->vsize); return I_RUNNING;

    /* VFPU4 unary ops, all through one runtime entry point. The selector table
     * mirrors emit.c's; keeping the same order makes them diffable. */
    case A_VMOV: case A_VABS: case A_VNEG: case A_VZERO: case A_VONE:
    case A_VRCP: case A_VRSQ: case A_VSQRT: case A_VSIN: case A_VCOS:
    case A_VEXP2: case A_VLOG2: case A_VSAT0: case A_VSAT1:
    case A_VNRCP: case A_VNSIN: case A_VASIN: case A_VF2IZ: case A_VI2F: {
        static const struct { a_op op; int sel; } U[] = {
            { A_VMOV, PSP_VU_MOV },   { A_VABS, PSP_VU_ABS },
            { A_VNEG, PSP_VU_NEG },   { A_VZERO,PSP_VU_ZERO },
            { A_VONE, PSP_VU_ONE },   { A_VRCP, PSP_VU_RCP },
            { A_VRSQ, PSP_VU_RSQ },   { A_VSQRT,PSP_VU_SQRT },
            { A_VSIN, PSP_VU_SIN },   { A_VCOS, PSP_VU_COS },
            { A_VEXP2,PSP_VU_EXP2 },  { A_VLOG2,PSP_VU_LOG2 },
            { A_VSAT0,PSP_VU_SAT0 },  { A_VSAT1,PSP_VU_SAT1 },
            { A_VNRCP,PSP_VU_NRCP },  { A_VNSIN,PSP_VU_NSIN },
            { A_VASIN,PSP_VU_ASIN },  { A_VF2IZ,PSP_VU_F2IZ },
            { A_VI2F, PSP_VU_I2F },
        };
        for (size_t k = 0; k < sizeof U / sizeof U[0]; k++) {
            if (U[k].op != in->op) continue;
            psp_vunary(U[k].sel, in->vd, in->vs, in->vsize);
            return I_RUNNING;
        }
        return I_TRAP_VFPU;
    }

    case A_VIDT: psp_vidt(in->vd, in->vsize);            return I_RUNNING;
    case A_VCST: psp_vcst(in->vd, in->vs, in->vsize);    return I_RUNNING;
    case A_VIIM: psp_vimm(in->vd, (float)in->imm);       return I_RUNNING;
    case A_VFIM: psp_vimm(in->vd, a_half_to_float((uint16_t)in->imm)); return I_RUNNING;

    case A_VMMUL: psp_vmmul(in->vd, in->vs, in->vt, in->vsize); return I_RUNNING;
    case A_VTFM2: case A_VTFM3: case A_VTFM4:
        psp_vtfm(in->vd, in->vs, in->vt, in->vsize); return I_RUNNING;
    case A_VMSCL: psp_vmscl(in->vd, in->vs, in->vt, in->vsize); return I_RUNNING;
    case A_VROT:  psp_vrot(in->vd, in->vs, (uint32_t)in->imm & 0x1F, in->vsize); return I_RUNNING;
    case A_VMMOV: psp_vmmov(in->vd, in->vs, in->vsize);  return I_RUNNING;
    case A_VMIDT: psp_vmidt(in->vd, in->vsize);          return I_RUNNING;
    case A_VMZERO:psp_vmzero(in->vd, in->vsize);         return I_RUNNING;
    case A_VMONE: psp_vmone(in->vd, in->vsize);          return I_RUNNING;

    /* Prefixes compute nothing: they arm a register that rewrites the operands
     * of the *next* vector instruction. Skipping them would not "lose a
     * no-op" -- it would make every following prefixed op silently compute the
     * unprefixed answer, which is worse than trapping. */
    case A_VPFXS: psp_vfpu_set_prefix(0, in->raw & 0xFFFFFF); return I_RUNNING;
    case A_VPFXT: psp_vfpu_set_prefix(1, in->raw & 0xFFFFFF); return I_RUNNING;
    case A_VPFXD: psp_vfpu_set_prefix(2, in->raw & 0xFFFFFF); return I_RUNNING;

    /* A control transfer in a delay slot. The architecture leaves this
     * undefined and no compiler emits it, so reaching here almost always means
     * data is being decoded as code. Naming it separately matters: it used to
     * fall through to the VFPU trap below and get reported as
     * "unimplemented VFPU instruction (jal)", which is nonsense and sent the
     * reader looking at the vector unit. */
    case A_J: case A_JAL: case A_JR: case A_JALR:
    case A_BEQ: case A_BNE: case A_BLEZ: case A_BGTZ:
    case A_BEQL: case A_BNEL: case A_BLEZL: case A_BGTZL:
    case A_BLTZ: case A_BGEZ: case A_BLTZL: case A_BGEZL:
    case A_BLTZAL: case A_BGEZAL: case A_BLTZALL: case A_BGEZALL:
    case A_BC1F: case A_BC1T: case A_BC1FL: case A_BC1TL:
        return I_TRAP_BRANCH_IN_SLOT;

    case A_SYSCALL: return I_TRAP_SYSCALL;
    case A_BREAK:   return I_TRAP_BREAK;
    case A_INVALID: return I_TRAP_INVALID;

    default:
        /* Everything the decoder names but nothing implements -- the rest of
         * the VFPU, mostly. Trapping matches what emit.c generates
         * (psp_unimplemented), so both paths stop at the same instruction
         * instead of one of them inventing an answer. */
        return I_TRAP_VFPU;
    }
}

/* ---- branch conditions ----------------------------------------------------
 *
 * Mirrors emit.c's branch_cond(). Evaluated against register state *before*
 * the delay slot runs, which is the whole subtlety of the thing. */

static int branch_taken(const a_insn *in) {
    switch (in->op) {
    case A_BEQ:  case A_BEQL:  return R(in->rs) == R(in->rt);
    case A_BNE:  case A_BNEL:  return R(in->rs) != R(in->rt);
    case A_BLEZ: case A_BLEZL: return (int32_t)R(in->rs) <= 0;
    case A_BGTZ: case A_BGTZL: return (int32_t)R(in->rs) >  0;
    case A_BLTZ: case A_BLTZL:
    case A_BLTZAL: case A_BLTZALL: return (int32_t)R(in->rs) <  0;
    case A_BGEZ: case A_BGEZL:
    case A_BGEZAL: case A_BGEZALL: return (int32_t)R(in->rs) >= 0;
    case A_BC1T: case A_BC1TL: return  psp_fpu_cond();
    case A_BC1F: case A_BC1FL: return !psp_fpu_cond();
    default:                   return 0;
    }
}

/* ---- guest re-entry from HLE ----------------------------------------------
 *
 * Some firmware handlers call back into guest code: a thread entry point, a
 * registered callback, a comparator handed to a sort. They do it through
 * psp_dispatch(), which knows only about *recompiled* functions.
 *
 * Left alone that is doubly wrong during an interpreter run. It mixes the two
 * translations inside a single execution, so a differential comparison is no
 * longer comparing anything; and recompiled code carries no instruction
 * budget, so a callee that does not return takes the whole run with it. The
 * harness previously had to bound this externally and throw the function away
 * -- 6,254 of them on this module, the largest skip category by far.
 *
 * With a hook installed, the interpreter runs the target itself. The nested run
 * gets what is left of the outer budget and adds its cost back, so total work
 * stays bounded however deep the nesting goes.
 *
 * $ra is saved across the call. psp_interp_init() plants the sentinel there so
 * the callee has somewhere to return to, which is what a `jal` would do -- but
 * the caller's own return address has to survive it. */

#define NESTED_RA  0x0DEAD100u
#define MAX_NEST   64

/* A nested run that ends badly is otherwise silent.
 *
 * Only the outermost run's status is reported, and it says "returned" whenever
 * the *outer* context got its $ra back -- which it does even when the thread it
 * started died on a bad instruction two frames down. That reads as a clean run
 * that simply printed nothing, and sends you looking at the program instead of
 * at the interpreter. Say which nested entry point stopped, and why. */
static unsigned long long g_nest_failed;

static void note_nested(const char *what, uint32_t entry, const psp_interp *sub) {
    /* I_EXIT is a finish, not a failure: a test's main thread ends by calling
     * sceKernelExitGame, and reporting that as a dead thread would flag every
     * healthy run. */
    if (sub->status == I_OK_RETURN || sub->status == I_EXIT) return;
    g_nest_failed++;
    fprintf(stderr, "interp: nested %s 0x%08X stopped: %s at pc 0x%08X\n",
            what, entry, psp_interp_status_str(sub->status), sub->fault_pc);
}

unsigned long long psp_interp_nest_failed(void) { return g_nest_failed; }

static psp_interp *g_active;      /* the run currently executing, if any */
static int         g_nest;
static uint64_t    g_nest_refused;

static int spawn_hook(uint32_t uid, uint32_t entry, uint32_t sp,
                      uint32_t a0, uint32_t a1, int priority);

static int dispatch_hook(uint32_t addr) {
    if (!g_active) return 0;              /* not inside an interpreter run */

    /* At the depth limit, refuse the call rather than declining to handle it.
     *
     * Returning 0 here would fall through to the normal lookup and run
     * *recompiled* code from inside an interpreter run -- unbounded, because
     * native code has no instruction budget -- which is the exact hang this
     * hook exists to prevent. A callback that does not happen is a wrong
     * answer; a callback that never returns is no answer at all, and the
     * caller can see the first one in the count below. */
    if (g_nest >= MAX_NEST) { g_nest_refused++; return 1; }

    psp_interp *outer = g_active;
    const uint32_t saved_ra = R(PSP_RA_INDEX);

    uint64_t left = 0;
    if (outer->budget) {
        left = outer->budget > outer->executed ? outer->budget - outer->executed : 1;
    }

    psp_interp sub;
    psp_interp_init(&sub, addr, NESTED_RA, left);
    sub.trace      = outer->trace;
    sub.trace_regs = outer->trace_regs;

    g_nest++;
    g_active = &sub;
    psp_interp_run(&sub);
    g_active = outer;
    g_nest--;
    note_nested("callback", addr, &sub);

    outer->executed += sub.executed;
    psp_cpu.r[PSP_RA_INDEX] = saved_ra;
    return 1;
}

void psp_interp_service_dispatch(int enable) {
    /* Clear the nesting state, not just the hook.
     *
     * g_nest is incremented around a nested run and decremented after it, but
     * a caller that abandons a run mid-flight -- a wall-clock watchdog, a trap
     * handler -- leaves via longjmp and never reaches the decrement. The
     * counter then leaks upward across runs, and after MAX_NEST such
     * abandonments the hook stops accepting work forever. Every subsequent
     * re-entry falls through to recompiled code and the next one that does not
     * return hangs the process.
     *
     * That failure is invisible until it happens and then total, and it looks
     * like the harness getting slower: the corpus ran 2,000 functions normally
     * and then stopped dead. Resetting here, where a caller announces the start
     * of a fresh top-level run, is what makes an abandoned run survivable. */
    g_nest = 0;
    g_active = NULL;
    psp_set_dispatch_hook(enable ? dispatch_hook : NULL);
    psp_sched_set_spawn_hook(enable ? spawn_hook : NULL);
}

/* A started thread runs to completion here, on the interpreter's own stack.
 *
 * That is sequential semantics, not scheduling: a thread that blocks part-way
 * has nothing to be resumed into, and a second thread started before the
 * first finishes waits rather than interleaving. It is also exactly what a
 * pspautotests crt needs -- module_start creates and starts main, main runs
 * its checks, prints, and returns -- and it is honest about what it is, which
 * pretending the boot host's scheduler was in play would not be. */
static int spawn_hook(uint32_t uid, uint32_t entry, uint32_t sp,
                      uint32_t a0, uint32_t a1, int priority) {
    (void)uid; (void)priority;
    if (!g_active) return 0;              /* not inside an interpreter run */
    if (g_nest >= MAX_NEST) { g_nest_refused++; return 1; }

    psp_interp *outer = g_active;
    const psp_cpu_state saved = psp_cpu;

    uint64_t left = 0;
    if (outer->budget) {
        left = outer->budget > outer->executed ? outer->budget - outer->executed : 1;
    }

    /* The thread's own register file: arguments from StartThread, its own
     * stack, and the sentinel to return to. Everything is restored after, so
     * the starter's registers survive the call. */
    memset(&psp_cpu, 0, sizeof psp_cpu);
    R(PSP_REG_A0) = a0;
    R(PSP_REG_A1) = a1;
    R(PSP_REG_SP) = sp;
    /* $gp is per-module, not per-thread, and the starter is in the same module
     * as the thread it starts -- so inheriting it is both correct and the only
     * source available here. Zero would point the small-data area at address 0. */
    R(PSP_REG_GP) = saved.r[PSP_REG_GP];

    psp_interp sub;
    psp_interp_init(&sub, entry, NESTED_RA, left);
    sub.trace      = outer->trace;
    sub.trace_regs = outer->trace_regs;

    g_nest++;
    g_active = &sub;
    psp_interp_run(&sub);
    g_active = outer;
    g_nest--;
    note_nested("thread", entry, &sub);

    outer->executed += sub.executed;
    psp_cpu = saved;
    return 1;
}

uint64_t psp_interp_nest_refused(void) { return g_nest_refused; }

/* ---- stepping -------------------------------------------------------------
 *
 * A branch and its delay slot are one step. The hardware never exposes a point
 * between them where the branch has been decided but the slot has not run, so
 * a trace that stopped there would describe a state the machine cannot be in.
 *
 * Three things here are easy to get wrong and are the usual source of
 * recompilation bugs:
 *
 *   1. An indirect jump's target must be read *before* the delay slot runs.
 *      `jr $t9` followed by a slot that reassigns $t9 jumps to the old value.
 *   2. A *link* branch writes $ra whether or not it is taken.
 *   3. A *likely* branch nullifies its delay slot when not taken -- the slot
 *      does not execute at all. Plain branches always execute it.
 */

void psp_interp_init(psp_interp *it, uint32_t entry, uint32_t ra_sentinel,
                     uint64_t budget) {
    memset(it, 0, sizeof *it);
    it->pc          = entry;
    it->ra_sentinel = ra_sentinel;
    it->budget      = budget;
    it->status      = I_RUNNING;
    psp_cpu.r[PSP_RA_INDEX] = ra_sentinel;
}

psp_interp_status psp_interp_step(psp_interp *it) {
    uint32_t before[PSP_NUM_GPR];
    a_insn in, slot;
    const uint32_t pc = it->pc;

    if (it->budget && it->executed >= it->budget)
        return it->status = I_BUDGET;

    /* A firmware thunk is intercepted rather than executed. Running it would
     * follow the unlinked `jr $ra` the linker left behind and return without
     * doing anything, while the recompiled C calls into HLE — so the two would
     * disagree at every firmware call for reasons that have nothing to do with
     * codegen. Both paths now go through psp_hle_call().
     *
     * The whole thunk is consumed: control resumes at $ra, not after the two
     * instructions, because the call has already happened. */
    const uint32_t nid = import_nid_at(pc);
    if (nid) {
        if (it->trace) fprintf(it->trace, "%08X  <hle 0x%08X>", pc, nid);
        note_hle(nid);
        psp_hle_call(nid);
        it->pc = R(PSP_RA_INDEX);
        it->executed += 2;
        if (it->trace) {
            if (it->trace_regs)
                fprintf(it->trace, " v0=%08X", R(PSP_REG_V0));
            fprintf(it->trace, " -> %08X\n", it->pc);
        }
        if (it->pc == it->ra_sentinel) return it->status = I_OK_RETURN;
        return it->status = I_RUNNING;
    }

    note_pc(pc);
    a_decode(psp_read32(pc), pc, &in);

    g_trace = it;
    if (it->trace) {
        char buf[96];
        a_format(&in, buf, sizeof buf);
        fprintf(it->trace, "%08X  %s", pc, buf);
        if (it->trace_regs) memcpy(before, psp_cpu.r, sizeof before);
    }

    psp_interp_status st = I_RUNNING;

    if (in.has_delay_slot) {
        /* Capture everything that the delay slot could invalidate, first. */
        const int taken   = in.is_branch ? branch_taken(&in) : 0;
        const int uncond  = in.is_jump;
        const uint32_t rs_now = R(in.rs);          /* for jr/jalr */
        uint32_t next;

        if (in.is_call) setr(in.is_indirect ? in.rd : PSP_RA_INDEX, pc + 8);

        if (uncond)                 next = in.is_indirect ? rs_now : in.target;
        else if (taken)             next = in.target;
        else                        next = pc + 8;

        /* A not-taken likely branch nullifies its slot entirely. */
        const int run_slot = !(in.is_likely && !uncond && !taken);
        if (run_slot) {
            a_decode(psp_read32(pc + 4), pc + 4, &slot);
            st = exec_simple(&slot);
            if (it->trace) {
                char sb[96];
                a_format(&slot, sb, sizeof sb);
                fprintf(it->trace, " ; slot: %s", sb);
            }
        } else if (it->trace) {
            fprintf(it->trace, " ; slot nullified");
        }

        it->executed += run_slot ? 2 : 1;
        it->pc = next;
    } else {
        st = exec_simple(&in);
        it->executed++;
        it->pc = pc + 4;
    }

    if (it->trace) {
        if (it->trace_regs) {
            for (unsigned i = 0; i < PSP_NUM_GPR; i++)
                if (before[i] != psp_cpu.r[i])
                    fprintf(it->trace, " %s=%08X", psp_reg_names[i], psp_cpu.r[i]);
        }
        fputc('\n', it->trace);
    }
    g_trace = NULL;

    if (st != I_RUNNING) {
        it->fault_pc = pc;
        return it->status = st;
    }
    if (it->pc == it->ra_sentinel) return it->status = I_OK_RETURN;
    return it->status = I_RUNNING;
}

psp_interp_status psp_interp_run(psp_interp *it) {
    /* Publish the active run so the dispatch hook can nest under it. Saved and
     * restored rather than assigned, because a nested run calls straight back
     * in here. */
    psp_interp *prev = g_active;
    g_active = it;
    uint64_t check_at = 0;
    while (psp_interp_step(it) == I_RUNNING) {
        /* A program that calls sceKernelExitGame is done, and the HLE records
         * that rather than killing the process. Nobody was reading the flag,
         * so the guest ran on past its own exit until the budget stopped it --
         * and "instruction budget exhausted" reads as a hang, not as a program
         * that finished and had nowhere to return to. Checked here so it ends
         * every nested run too, innermost first.
         *
         * Sampled rather than tested every step: this is the hot loop of the
         * oracle's millions of instructions, and stopping within 4K of the
         * call is as good as stopping at it for every use this has.
         *
         * A threshold, not `executed & 0xFFF`. A step is an instruction *plus
         * its delay slot*, so the counter advances by one or two, and a spin
         * loop -- which is exactly what a program sits in after asking to exit
         * -- advances by two every time. Land on the wrong parity there and no
         * value of the counter is ever a multiple of 4096, so the check never
         * fires. It was written as a mask first, and did nothing at all. */
        if (it->executed < check_at) continue;
        check_at = it->executed + 4096;
        if (psp_exit_requested()) { it->status = I_EXIT; break; }
    }
    g_active = prev;
    return it->status;
}

const char *psp_interp_status_str(psp_interp_status s) {
    switch (s) {
    case I_RUNNING:      return "running";
    case I_OK_RETURN:    return "returned";
    case I_BUDGET:       return "instruction budget exhausted";
    case I_TRAP_INVALID: return "invalid instruction";
    case I_TRAP_VFPU:    return "unimplemented VFPU instruction";
    case I_TRAP_SYSCALL: return "syscall (no HLE wired up)";
    case I_TRAP_BREAK:   return "break";
    case I_TRAP_BRANCH_IN_SLOT: return "control transfer in a delay slot";
    case I_TRAP_BADPC:   return "pc left mapped memory";
    case I_EXIT:         return "guest called sceKernelExitGame";
    }
    return "unknown";
}

psp_interp_status psp_interp_exec_one(const void *decoded_insn) {
    return exec_simple((const a_insn *)decoded_insn);
}
