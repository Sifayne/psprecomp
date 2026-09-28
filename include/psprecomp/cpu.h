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
    uint32_t vfpu_cc;          /* VFPU condition codes (vcmp results); control register 3 */
    /* The other VFPU control registers, indexed as mfvc/mtvc number them: 0..2
     * the source, target and destination prefixes, 4..6 reserved, 7 the
     * revision word, 8..15 the random generator's state rcx0..rcx7. Slot 3 is
     * unused -- the condition codes are vfpu_cc above.
     *
     * They are thread context, so they live here and are swapped with the rest
     * of the struct. Measured (vfpuprobe steps 147 and 154, fw 6.60): a thread
     * started while main's CC held 0x15 read CC 0x3F and the reset prefixes,
     * rev and rcx, and its first two vrndi equalled main's first two -- the
     * generator state is per thread too. */
    uint32_t vfpu_ctrl[16];
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
 * NaN -- FCR31 at 0x00000E00, and the VFPU control registers at their reset
 * values (psp_cpu_reset_vfpu_ctrl). All of it measured on a thread created by
 * sceKernelCreateThread (vfpuprobe step 147, fw 6.60), and the main thread
 * reads the same control values before its first VFPU instruction. */
void psp_cpu_reset_fp(void);

/* The VFPU control registers alone, at reset: prefixes 0xE4/0xE4/0, CC 0x3F,
 * revision 0x7772CEAB, rcx0..7 3F800001 3F800002 3F800004 3F800008 3F800000
 * x4, the reserved slots 0. Part of psp_cpu_reset_fp; psp_vfpu_reset uses it
 * to reset the control state without touching the register file. */
void psp_cpu_reset_vfpu_ctrl(void);

/* The whole register file of a freshly created thread, before the creator's
 * values ($a0, $a1, $sp, $gp, $k0, $ra) are filled in by the caller.
 *
 * Every general register the thread did not get from its creator holds
 * 0xDEADBEEF on hardware, except $k1, which is 0 (vfpuprobe step 147,
 * fw 6.60: at, v0, v1, a2, a3, t0-t9, s0-s7 all read DEADBEEF). $fp read as
 * something that is neither 0 nor DEADBEEF; the callers set it to the initial
 * $sp, the simplest value that fits, and the probe did not print it. */
void psp_cpu_reset_thread(void);
#define PSP_GPR_FRESH 0xDEADBEEFu

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
/* Power-on FCR31: the overflow, divide-by-zero and invalid *enables* are set
 * (a fresh thread reads 00000E00, vfpuprobe step 147, fw 6.60; so does the
 * main thread, v3 step 161). */
#define PSP_FCR31_RESET     0x00000E00u

/* An FPU exception is taken when a cause bit is set together with its enable
 * (cause bits 12-16 against enables 7-11, I U O Z V), or when the E cause bit
 * is. On the hardware each of these switched the PSP off (vfpuprobe v3 steps
 * 188-192, fw 6.60): ctc1 of the E bit; 1/0, 0/0 and max*max in a fresh
 * thread under its own 00000E00; and ctc1 of 00008400, the Z cause with the
 * Z enable. So under the power-on enables a division by zero, an invalid
 * operation or an overflow is fatal, and the guest thread stops here too:
 * the caller checks this after each COP1 operation. The destination is
 * written before the stop; MIPS leaves it alone, and nothing runs after. */
#define PSP_FCR31_E 0x00020000u
static inline int psp_fcr31_traps(uint32_t v) {
    return ((v >> 12) & (v >> 7) & 0x1Fu) != 0u || (v & PSP_FCR31_E) != 0u;
}
static inline int psp_fpu_trap_pending(void) { return psp_fcr31_traps(psp_cpu.fcr31); }

static inline uint32_t psp_fcr_read(unsigned n) {
    if (n == 31) return psp_cpu.fcr31;
    if (n == 0)  return PSP_FCR0_VALUE;
    return 0;
}
/* Returns nonzero, and stores nothing, when the write itself is an FPU
 * exception: the E cause bit (bit 17, "unimplemented operation", which has
 * no enable; vfpuprobe step 160) or any cause bit with its enable set (v3
 * step 192: 00008400), as MIPS says. The caller stops the thread. */
static inline int psp_fcr_write(unsigned n, uint32_t v) {
    if (n != 31) return 0;
    if (psp_fcr31_traps(v)) return 1;
    psp_cpu.fcr31 = v & PSP_FCR31_WRITABLE;
    return 0;
}

/* FPU condition flag (fcr31 bit 23) — set by c.cond.s, tested by bc1t/bc1f. */
#define PSP_FCR31_C (1u << 23)

static inline int  psp_fpu_cond(void)      { return (psp_cpu.fcr31 & PSP_FCR31_C) != 0; }

/* VFPU control register indices, as mfvc/mtvc number them (the field minus
 * 128). */
enum {
    PSP_VFPU_PFXS = 0, PSP_VFPU_PFXT = 1, PSP_VFPU_PFXD = 2, PSP_VFPU_CC = 3,
    PSP_VFPU_REV = 7, PSP_VFPU_RCX0 = 8
};

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
