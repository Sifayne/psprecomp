/* psprecomp — Allegrex CPU state. See include/psprecomp/cpu.h. */

#include "psprecomp/cpu.h"

#include <string.h>

psp_cpu_state psp_cpu;

const char *const psp_reg_names[PSP_NUM_GPR] = {
    "zero","at","v0","v1","a0","a1","a2","a3",
    "t0","t1","t2","t3","t4","t5","t6","t7",
    "s0","s1","s2","s3","s4","s5","s6","s7",
    "t8","t9","k0","k1","gp","sp","fp","ra"
};

/* 0x7F800001 is what the PSP fills a new thread's float and vector registers
 * with -- an exponent of all ones with a non-zero mantissa, so it is a NaN
 * rather than an infinity, and it is signalling rather than quiet. */
#define PSP_FP_INIT 0x7F800001u

void psp_cpu_reset_vfpu_ctrl(void) {
    /* Measured on the main thread before its first VFPU instruction and on a
     * freshly created thread (vfpuprobe, the lines before step 1 and step 147,
     * fw 6.60): pfxs 000000E4 pfxt 000000E4 pfxd 00000000 cc 0000003F, the
     * reserved 4..6 zero, rev 7772CEAB, rcx0-7 3F800001 3F800002 3F800004
     * 3F800008 3F800000 3F800000 3F800000 3F800000.
     *
     * The prefixes matter most. They used to start at zero, which as a source
     * prefix is the swizzle x,x,x,x: the first VFPU instruction of every
     * program broadcast lane x until something consumed the prefix. */
    memset(psp_cpu.vfpu_ctrl, 0, sizeof psp_cpu.vfpu_ctrl);
    psp_cpu.vfpu_ctrl[PSP_VFPU_PFXS] = 0xE4u;
    psp_cpu.vfpu_ctrl[PSP_VFPU_PFXT] = 0xE4u;
    psp_cpu.vfpu_ctrl[PSP_VFPU_PFXD] = 0x00u;
    psp_cpu.vfpu_ctrl[PSP_VFPU_REV]  = 0x7772CEABu;
    for (int i = 0; i < 8; i++)
        psp_cpu.vfpu_ctrl[PSP_VFPU_RCX0 + i] = 0x3F800000u | (i < 4 ? 1u << i : 0u);
    /* The condition codes come up with all six bits *set*, not clear: 0x3F on
     * both threads above. */
    psp_cpu.vfpu_cc = 0x3Fu;
}

void psp_cpu_reset_fp(void) {
    psp_cpu.fcr31 = PSP_FCR31_RESET;
    for (int i = 0; i < 32; i++)  memcpy(&psp_cpu.f[i], &(uint32_t){PSP_FP_INIT}, 4);
    for (int i = 0; i < 128; i++) memcpy(&psp_cpu.v[i], &(uint32_t){PSP_FP_INIT}, 4);
    psp_cpu_reset_vfpu_ctrl();
}

void psp_cpu_reset_thread(void) {
    memset(&psp_cpu, 0, sizeof psp_cpu);
    for (int i = 1; i < PSP_NUM_GPR; i++) psp_cpu.r[i] = PSP_GPR_FRESH;
    psp_cpu.r[PSP_REG_K1] = 0;
    psp_cpu.hi = psp_cpu.lo = PSP_GPR_FRESH;
    psp_cpu_reset_fp();
}

void psp_cpu_reset(void) {
    memset(&psp_cpu, 0, sizeof psp_cpu);
    /* $sp is set by the loader from the module's stack allocation, not here —
     * a reset CPU with a null stack is the correct starting point, and a game
     * that faults on a null $sp is telling us the loader did not run. */
}
