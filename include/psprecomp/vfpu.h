/* psprecomp — the VFPU.
 *
 * 128 single-precision registers, addressed as 8 matrices of 4x4. A single
 * 7-bit register field can name a scalar, a 2/3/4-element row, a column, or a
 * whole matrix, depending on the instruction's width bits and a transpose bit.
 *
 * ## Register layout
 *
 * The file is indexed `v[matrix*4 + column*32 + row]`. That looks arbitrary
 * and is not: it is the layout that makes a row and a column of the same
 * matrix alias the same storage the way the hardware does. A game that writes
 * a matrix by columns and reads it by rows -- which is exactly what a
 * transpose does -- only works if this matches.
 *
 * ## What is implemented, and what deliberately is not
 *
 * Measured against a real module (WTF's Lumberjack, 1276 VFPU-space words in
 * .text), the distribution is heavily skewed: quad load/store is 48% of it,
 * and vscl/vmul/vadd/vdot another 18%. Those, plus the compare/min/max family,
 * are implemented here and unit-tested.
 *
 * The **prefix** instructions are implemented too, as of the pspautotests
 * work. `vpfxs`/`vpfxt`/`vpfxd` compute nothing themselves -- they set a
 * register that rewrites the *operands of the next instruction*, swizzling
 * lanes, taking absolute values, substituting constants, negating, saturating
 * and masking lanes out of the write. An arithmetic op executed while a prefix
 * is pending computes something different from the same op without one, and
 * nothing in that op's own encoding says so.
 *
 * They were deliberately absent for a long time, and a pending prefix made the
 * next arithmetic op report and skip rather than compute a number that ignored
 * it -- loud beats plausible. That policy was right and it was also the last
 * thing standing between the VFPU and the hardware tests: pspgl's `glRotatef`
 * is two `vmov.p` under a `vpfxs` whose entire content is a lane negation, so
 * skipping them leaves the identity behind.
 *
 * Anything still missing traps the same way; see psp_vfpu_unimplemented.
 */
#ifndef PSPRECOMP_VFPU_H
#define PSPRECOMP_VFPU_H

#include "cpu.h"
#include "mem.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Resolve a 7-bit register field and width into up to four indices into
 * psp_cpu.v[]. Returns the number of lanes. */
int psp_vfpu_regs(uint32_t vreg, int size, int out[4]);

/* Load/store. Quad forms are 16-byte aligned on hardware; the address is
 * masked rather than faulting, which is what the hardware does. */
void psp_lv_s(uint32_t vt, uint32_t addr);
void psp_lv_q(uint32_t vt, uint32_t addr);
void psp_sv_s(uint32_t vt, uint32_t addr);
void psp_sv_q(uint32_t vt, uint32_t addr);

/* Unaligned quad load/store.
 *
 * A quad is 16-byte aligned, so a vector that straddles two of those blocks
 * takes two instructions: `lvl.q` fills the lanes at and below the address,
 * `lvr.q` the lanes at and above, and between them they move an arbitrarily
 * aligned quad. The lanes each one does not touch keep their old contents,
 * which is why both read the destination first. The store pair is the mirror.
 *
 * The address is *not* masked down: which lanes move is exactly what its low
 * bits select. */
void psp_lvl_q(uint32_t vt, uint32_t addr);
void psp_lvr_q(uint32_t vt, uint32_t addr);
void psp_svl_q(uint32_t vt, uint32_t addr);
void psp_svr_q(uint32_t vt, uint32_t addr);

/* Horizontal reductions to a single lane: vfad sums, vavg averages.
 *
 * Both are the same dot product against a constant vector on hardware -- ones
 * for vfad, 1/size for vavg -- which is why a single-lane vavg is zero rather
 * than the value itself: the constant for size 1 is 0, not 1. */
void psp_vfad(uint32_t vd, uint32_t vs, int size);
void psp_vavg(uint32_t vd, uint32_t vs, int size);

/* Pack four 8888 pixels into four 16-bit ones, two per destination lane.
 * `fmt` is 1 for 4444, 2 for 5551, 3 for 5650 -- the low two bits of the
 * instruction's rt field, which is where the hardware keeps it. */
void psp_vcolor(uint32_t vd, uint32_t vs, int fmt, int size);

/* Element-wise arithmetic across `size` lanes. */
void psp_vadd(uint32_t vd, uint32_t vs, uint32_t vt, int size);
void psp_vsub(uint32_t vd, uint32_t vs, uint32_t vt, int size);
void psp_vmul(uint32_t vd, uint32_t vs, uint32_t vt, int size);
void psp_vdiv(uint32_t vd, uint32_t vs, uint32_t vt, int size);
void psp_vmin(uint32_t vd, uint32_t vs, uint32_t vt, int size);
void psp_vmax(uint32_t vd, uint32_t vs, uint32_t vt, int size);

/* Reductions. vdot writes one lane; vscl scales a vector by a scalar. */
void psp_vdot(uint32_t vd, uint32_t vs, uint32_t vt, int size);
void psp_vscl(uint32_t vd, uint32_t vs, uint32_t vt, int size);

/* Comparison, writing the VFPU condition codes. */
void psp_vcmp(uint32_t cond, uint32_t vs, uint32_t vt, int size);

/* Unary element-wise ops (VFPU4). One entry point rather than eighteen, since
 * they differ only in the scalar function applied per lane. */
enum {
    PSP_VU_MOV = 0, PSP_VU_ABS, PSP_VU_NEG, PSP_VU_ZERO, PSP_VU_ONE,
    PSP_VU_RCP, PSP_VU_RSQ, PSP_VU_SQRT, PSP_VU_SIN, PSP_VU_COS,
    PSP_VU_EXP2, PSP_VU_LOG2, PSP_VU_SAT0, PSP_VU_SAT1,
    PSP_VU_NRCP, PSP_VU_NSIN, PSP_VU_REXP2, PSP_VU_ASIN,
    PSP_VU_F2IZ, PSP_VU_I2F
};
void psp_vunary(int op, uint32_t vd, uint32_t vs, int size);

/* Matrix ops that need no multiply: identity, zero, one, and copy. `size` is
 * the matrix order (2, 3 or 4). */
void psp_vmidt(uint32_t vd, int size);
void psp_vidt(uint32_t vd, int size);
void psp_vimm(uint32_t vd, float value);
void psp_vcst(uint32_t vd, uint32_t which, int size);
void psp_vmzero(uint32_t vd, int size);
void psp_vmone(uint32_t vd, int size);
void psp_vmmov(uint32_t vd, uint32_t vs, int size);

/* vrot -- one row of a rotation matrix.
 *
 * Takes a single angle in rs[0] and spreads its sine and cosine across the
 * destination according to a 5-bit control field: bits 0-1 pick the lane that
 * receives the cosine, bits 2-3 the lane that receives the sine, bit 4 negates
 * the sine. Lanes named by neither get zero -- except when the two selectors
 * are equal, where every lane but that one gets the sine. */
void psp_vrot(uint32_t vd, uint32_t vs, uint32_t imm, int size);

/* Matrix multiply, transform and scale. Operand orientation is checked against
 * real-hardware output (pspautotests cpu/vfpu/matrix); see vfpu.c.
 *
 * `size` is the matrix order. vtfm's `homogeneous` selects the vhtfm form,
 * which has no opcode of its own -- see vfpu.c for how to tell them apart. */
void psp_vmscl(uint32_t vd, uint32_t vs, uint32_t vt, int size);
void psp_vtfm(uint32_t vd, uint32_t vs, uint32_t vt, int size, int homogeneous);
void psp_vmmul(uint32_t vd, uint32_t vs, uint32_t vt, int size);

/* Integer/vector moves: mfv and mtv.
 *
 * The value crosses as a *bit pattern*, not a number -- these are how a game
 * gets a float into the vector file without a round trip through memory, and
 * how it reads one back out. No conversion, and no prefix: the prefix
 * registers rewrite the operands of arithmetic, and a move is not arithmetic.
 *
 * `vd` is the single-register form of the 7-bit field, so it addresses one
 * lane. The control-register variants (mfvc/mtvc, bit 7 of the field set) are
 * a different instruction and are not these. */
uint32_t psp_mfv(uint32_t vd);
void     psp_mtv(uint32_t vd, uint32_t bits);

/* Prefix state. Set by vpfxs/vpfxt/vpfxd and consumed by the next VFPU
 * instruction -- every one of them, whether it uses the prefix or not, since a
 * prefix left set would apply to whatever came after instead.
 *
 * `psp_vfpu_prefix_pending` reports whether any prefix differs from its
 * identity. Nothing in the implementation needs it now; it is kept because it
 * is the cheap way to ask, from outside, whether the next op will be rewritten. */
void psp_vfpu_set_prefix(int which, uint32_t value);
int  psp_vfpu_prefix_pending(void);
void psp_vfpu_reset(void);

/* Reports an instruction the VFPU cannot yet execute, by address and name. */
void psp_vfpu_unimplemented(uint32_t addr, const char *what);
uint64_t psp_vfpu_trap_count(void);

#ifdef __cplusplus
}
#endif

#endif /* PSPRECOMP_VFPU_H */
