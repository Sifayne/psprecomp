/* psprecomp — Allegrex CPU state.
 *
 * The PSP's main CPU is a MIPS32r2 core ("Allegrex", 222/333 MHz) with:
 *   - 32 general-purpose registers, no 64-bit integer ops
 *   - HI/LO for multiply/divide
 *   - COP1: a single-precision-only FPU (32 registers)
 *   - COP2: the VFPU, a 128-register vector unit (see vfpu.h)
 *   - no TLB — a fixed, simple memory map (see mem.h)
 *
 * Recompiled code operates on this struct directly. Generated functions are
 * plain C: they read and write psp_cpu.r[] and call the helpers in recomp_rt.h
 * so that flag/overflow/edge-case semantics live in exactly one place.
 */
#ifndef PSPRECOMP_CPU_H
#define PSPRECOMP_CPU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Register indices, by ABI name. Generated code uses these so the emitted C
 * reads like the original assembly: psp_cpu.r[PSP_REG_A0] not psp_cpu.r[4]. */
enum {
    PSP_REG_ZERO = 0, PSP_REG_AT,
    PSP_REG_V0, PSP_REG_V1,
    PSP_REG_A0, PSP_REG_A1, PSP_REG_A2, PSP_REG_A3,
    PSP_REG_T0, PSP_REG_T1, PSP_REG_T2, PSP_REG_T3,
    PSP_REG_T4, PSP_REG_T5, PSP_REG_T6, PSP_REG_T7,
    PSP_REG_S0, PSP_REG_S1, PSP_REG_S2, PSP_REG_S3,
    PSP_REG_S4, PSP_REG_S5, PSP_REG_S6, PSP_REG_S7,
    PSP_REG_T8, PSP_REG_T9,
    PSP_REG_K0, PSP_REG_K1,
    PSP_REG_GP, PSP_REG_SP, PSP_REG_FP, PSP_REG_RA,
    PSP_NUM_GPR = 32
};

/* Canonical ABI register names, indexed by the enum above. */
extern const char *const psp_reg_names[PSP_NUM_GPR];

typedef struct {
    uint32_t r[PSP_NUM_GPR];   /* r[0] is hardwired zero; see psp_set_reg() */
    uint32_t hi, lo;
    uint32_t pc;               /* only meaningful at dispatch boundaries */

    float    f[32];            /* COP1 — single precision only */
    uint32_t fcr31;            /* FPU control/status; bit 23 is the C flag */

    /* VFPU register file: 8 matrices x 4 rows x 4 columns = 128 floats.
     * Indexed linearly here; vfpu.h provides the matrix/row/column views. */
    float    v[128];
    uint32_t vfpu_cc;          /* VFPU condition codes (vcmp results) */
} psp_cpu_state;

extern psp_cpu_state psp_cpu;

/* Write a GPR, honouring the hardwired-zero rule for $zero. Generated code
 * calls this rather than assigning r[] directly, so a stray write to $zero
 * can never corrupt state. */
static inline void psp_set_reg(uint32_t idx, uint32_t val) {
    if (idx != PSP_REG_ZERO) psp_cpu.r[idx] = val;
}

void psp_cpu_reset(void);

/* Put the float and vector registers into the state a fresh PSP thread gets.
 *
 * The hardware does not hand out a zeroed register file. A new thread context
 * starts with every COP1 and VFPU register holding 0x7F800001 -- a signalling
 * NaN -- and that is observable: a test that writes one lane of a vector and
 * stores all four prints `nan` for the other three, where a zeroed file prints
 * 0.000000. pspautotests cpu/vfpu/vavg is exactly that shape.
 *
 * The general-purpose registers get 0xDEADBEEF on hardware. That is *not* done
 * here: nothing measured needs it, and seeding every GPR with a value that
 * looks like a plausible pointer would turn "the guest used an uninitialised
 * register" from a zero-page fault into a wild write. Recorded rather than
 * copied. */
void psp_cpu_reset_fp(void);

/* The FPU control registers, as the hardware presents them.
 *
 * There are 32 addressable, and exactly two exist: `fcr0` is a read-only
 * implementation/revision word, and `fcr31` is the control/status register.
 * The rest -- including 25..28, which MIPS32 defines as FCCR/FEXR/FENR --
 * read as zero on this part and ignore writes. All of them were aliased to
 * one variable here, so `cfc1 $t, $0` returned whatever had last been written
 * to `fcr31`.
 *
 * FCR31 is not fully writable either. Measured against hardware with
 * pspautotests cpu/fpu/fcr, which writes a value and reads it back:
 *
 *   RM 0x00000003, flags 0x0000007C, enables 0x00000F80,
 *   cause 0x0001F000, FCC 0x00800000, FS 0x01000000     -> kept
 *   FO 0x00400000, FN 0x00200000, FCC1-7 0xFE000000,
 *   0x001C0000                                          -> read back as zero
 *
 * so the writable mask is 0x0181FFFF and the unwritable bits are not merely
 * ignored, they read zero afterwards -- a plain mask of the incoming value,
 * with nothing preserved. */
#define PSP_FCR0_VALUE      0x00003351u
#define PSP_FCR31_WRITABLE  0x0181FFFFu
/* Power-on FCR31: the overflow, divide-by-zero and invalid *enables* are set.
 * Nothing here traps, so this matters only because it is observable. */
#define PSP_FCR31_RESET     0x00000E00u

static inline uint32_t psp_fcr_read(unsigned n) {
    if (n == 31) return psp_cpu.fcr31;
    if (n == 0)  return PSP_FCR0_VALUE;
    return 0;
}
static inline void psp_fcr_write(unsigned n, uint32_t v) {
    if (n == 31) psp_cpu.fcr31 = v & PSP_FCR31_WRITABLE;
}

/* FPU condition flag (fcr31 bit 23) — set by c.cond.s, tested by bc1t/bc1f. */
#define PSP_FCR31_C (1u << 23)

static inline int  psp_fpu_cond(void)      { return (psp_cpu.fcr31 & PSP_FCR31_C) != 0; }

/* VFPU condition code `cc` (0..5: one per lane, then any, then all) -- set by
 * vcmp, tested by bvt/bvf and their likely forms. The code index is the
 * instruction's bits 18..20. */
static inline int  psp_vfpu_cond(unsigned cc) { return (psp_cpu.vfpu_cc >> (cc & 7u)) & 1u; }
static inline void psp_fpu_set_cond(int c) {
    psp_cpu.fcr31 = c ? (psp_cpu.fcr31 | PSP_FCR31_C) : (psp_cpu.fcr31 & ~PSP_FCR31_C);
}

#ifdef __cplusplus
}
#endif

#endif /* PSPRECOMP_CPU_H */
