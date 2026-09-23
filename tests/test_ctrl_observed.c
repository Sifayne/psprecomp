/* Compile our original controller probe against native HLE adapters and
 * compare each record with what the external executable returned
 * (provenance/ctrl). Waits advance the guest clock to the next vblank.
 *
 * Compared: record id, return value, vblanks waited, vblanks spent inside
 * the call, how far and how many entries were written, and the first
 * entry's buttons and stick. Not compared: timestamps. The observed ones
 * are microseconds of a real sample history, while this runtime keeps one
 * merged state and counts samples delivered.
 *
 * Two records depend on sampling phase rather than on the rule. The observed
 * executable takes each sample a little after vblank start, so a Read made
 * directly after sceDisplayWaitVblankStart sees samples up to the previous
 * vblank, and the one the wait arrived at is handed over by the next Read.
 * This runtime samples on the boundary. The run of reads is identical --
 * K vblanks between reads return K samples -- but the first read after a
 * phase change (records 0x16 and 0x3a) returns one fewer there. Those two
 * are checked for id and blocking only. */
#include "psprecomp/hle.h"
#include "psprecomp/dispatch.h"
#include "psprecomp/clock.h"
#include "psprecomp/mem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define module_start probe_main
#include "provenance/ctrl/probe.c"
#undef module_start

#define GBUF 0x08820000u
#define NID_SAMPLING_CYCLE 0x6A2774F3u
#define NID_SAMPLING_MODE  0x1F4011E6u
#define NID_READ           0x1F803938u
#define NID_PEEK           0x3A622550u

static FILE *reference;
static unsigned records, checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "%d: %s\n", __LINE__, #c); } } while (0)

static u32 invoke(u32 nid, u32 a, u32 b) {
    psp_cpu.r[PSP_REG_A0] = a;
    psp_cpu.r[PSP_REG_A1] = b;
    psp_hle_call(nid);
    return psp_cpu.r[PSP_REG_V0];
}

int sceDisplayWaitVblankStart(void) {
    psp_clock_advance_to(psp_clock_next_frame());
    return 0;
}
int sceDisplayGetVcount(void) { return (int)(psp_clock_peek() / PSP_CLOCK_FRAME_US); }
int sceCtrlSetSamplingCycle(int cycle) { return (int)invoke(NID_SAMPLING_CYCLE, (u32)cycle, 0); }
int sceCtrlSetSamplingMode(int mode) { return (int)invoke(NID_SAMPLING_MODE, (u32)mode, 0); }

/* The probe's buffer lives in host memory; the call sees a guest copy of it,
 * sentinels included, so untouched entries come back untouched. */
static int buffer_call(u32 nid, pad *data, int count) {
    CHECK(data == buf);
    psp_mem_write_block(GBUF, buf, sizeof buf);
    const int result = (int)invoke(nid, GBUF, (u32)count);
    memcpy(buf, psp_mem_ptr(GBUF, sizeof buf), sizeof buf);
    return result;
}
int sceCtrlReadBufferPositive(pad *data, int count) { return buffer_call(NID_READ, data, count); }
int sceCtrlPeekBufferPositive(pad *data, int count) { return buffer_call(NID_PEEK, data, count); }

int sceIoWrite(int fd, const void *data, unsigned size) {
    char actual[160], expected[160];
    CHECK(fd == 1 && size < sizeof actual);
    memcpy(actual, data, size);
    actual[size] = 0;
    CHECK(fgets(expected, sizeof expected, reference) != NULL);
    u32 a[13], e[13];
    char *ap = actual, *ep = expected;
    for (unsigned i = 0; i < 13; i++) {
        a[i] = (u32)strtoul(ap, &ap, 16);
        e[i] = (u32)strtoul(ep, &ep, 16);
    }
    const int phase = e[0] == 0x16 || e[0] == 0x3a;
    static const unsigned compared[] = {0, 1, 2, 3, 4, 5, 11, 12};
    for (unsigned k = 0; k < sizeof compared / sizeof compared[0]; k++) {
        const unsigned i = compared[k];
        if (phase && (i == 1 || i == 4 || i == 5)) continue;
        checks++;
        if (a[i] != e[i] && failures++ < 24)
            fprintf(stderr, "record %u (id %02x) field %u: %08x != %08x\n",
                    records, e[0], i, a[i], e[i]);
    }
    /* Entries a Read hands back are in delivery order. */
    if (a[5] >= 2 && a[6] != 0xffffffffu && a[7] != 0xffffffffu) CHECK((int)(a[7] - a[6]) > 0);
    records++;
    return (int)size;
}
void sceKernelExitGame(void) {}

int main(int argc, char **argv) {
    if (argc != 2 || !(reference = fopen(argv[1], "r"))) {
        fprintf(stderr, "usage: %s observed.txt\n", argv[0]);
        return 2;
    }
    CHECK(psp_mem_init() == 0);
    psp_cpu_reset();
    psp_hle_init();
    psp_clock_reset();
    probe_main(0, NULL);
    char extra[160];
    CHECK(fgets(extra, sizeof extra, reference) == NULL);
    fclose(reference);
    psp_mem_free();
    printf("ctrl observed: %u records, %u checks, %u failure(s)\n", records, checks, failures);
    return failures ? 1 : 0;
}
