/* Repeated self-deleting workers must return their guest stack allocations. */
#include "psprecomp/hle.h"
#include "psprecomp/dispatch.h"
#include "psprecomp/sched.h"
#include "crypto/sha1.h"
#include <stdio.h>

#define DRIVER 0x08801000u
#define WORKER 0x08801004u
#define NAME 0x08802000u
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); failures++; } } while (0)
static int failures, runs, returned;
static uint32_t child_callback;

static uint32_t call(const char *name, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    psp_cpu.r[PSP_REG_A0] = a; psp_cpu.r[PSP_REG_A1] = b;
    psp_cpu.r[PSP_REG_A2] = c; psp_cpu.r[PSP_REG_A3] = d;
    psp_hle_call(psp_nid(name));
    return psp_cpu.r[PSP_REG_V0];
}

static void worker(void) {
    child_callback = call("sceKernelCreateCallback", NAME, WORKER, 0, 0);
    CHECK((int32_t)child_callback > 0);
    runs++;
    call("sceKernelExitDeleteThread", 7, 0, 0, 0);
    returned++;
}

static void driver(void) {
    const uint32_t free_before = psp_sysmem_free();
    for (int i = 0; i < 64; i++) {
        psp_cpu.r[PSP_REG_T0] = 0x00200000; /* CLEAR_STACK on deletion */
        psp_cpu.r[PSP_REG_T1] = 0;
        uint32_t id = call("sceKernelCreateThread", NAME, WORKER, 16, 0x2800);
        CHECK((int32_t)id > 0);
        CHECK(psp_sysmem_free() == free_before - 0x2800);
        CHECK(call("sceKernelStartThread", id, 0, 0, 0) == 0);
        /* Higher-priority worker has finished before StartThread returns. */
        CHECK(runs == i + 1 && returned == 0);
        CHECK(psp_sysmem_free() == free_before);
        CHECK(call("sceKernelDeleteThread", id, 0, 0, 0) == SCE_KERNEL_ERROR_UNKNOWN_THID);
        CHECK(call("sceKernelDeleteCallback", child_callback, 0, 0, 0) == SCE_KERNEL_ERROR_UNKNOWN_CBID);
    }
}

int main(void) {
    CHECK(psp_mem_init() == 0); psp_hle_init();
    psp_cpu_reset(); psp_threadman_reset(); psp_sched_reset();
    psp_sysmem_reset(); psp_sched_set_threading(1);
    psp_write32(NAME, 0x726F77); /* "wor" */
    psp_register(DRIVER, driver); psp_register(WORKER, worker);
    CHECK(psp_sched_spawn(0x70000, DRIVER, 0x08810000, 0, 0, 0, 32) == 0);
    CHECK(psp_sched_drain(5) == 0);
    psp_sched_join_all();
    CHECK(runs == 64 && returned == 0);
    printf("thread_lifecycle: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
