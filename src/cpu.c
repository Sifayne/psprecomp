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

void psp_cpu_reset_fp(void) {
    for (int i = 0; i < 32; i++)  memcpy(&psp_cpu.f[i], &(uint32_t){PSP_FP_INIT}, 4);
    for (int i = 0; i < 128; i++) memcpy(&psp_cpu.v[i], &(uint32_t){PSP_FP_INIT}, 4);
}

void psp_cpu_reset(void) {
    memset(&psp_cpu, 0, sizeof psp_cpu);
    /* $sp is set by the loader from the module's stack allocation, not here —
     * a reset CPU with a null stack is the correct starting point, and a game
     * that faults on a null $sp is telling us the loader did not run. */
}
