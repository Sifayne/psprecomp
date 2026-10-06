/* Resuming a guest call chain natively (docs/PLAYER-LAYER.md §5).
 *
 * The program in resume_program.h, emitted with --resume by gen_resume at
 * build time and compiled in here, runs main -> A -> B -> C -> a firmware
 * call. Three runs must end identically, registers and memory:
 *
 *   1. native, uninterrupted;
 *   2. native, interrupted inside the firmware call -- the snapshot a save
 *      state would hold is taken there and the host frames are abandoned --
 *      then everything scrambled, the snapshot restored, and the chain
 *      resumed from the firmware call's return site with psp_resume_chain;
 *   3. the interpreter, from the same restored snapshot and site.
 *
 * Run 2 is the save state; run 3 is the oracle saying it is right. */
#include "interp.h"
#include "resume_program.h"
#include "t_resume_funcs.h"

#include "psprecomp/cpu.h"
#include "psprecomp/dispatch.h"
#include "psprecomp/hle.h"
#include "psprecomp/mem.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
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

#define STACK_TOP 0x09F00000u
#define STACK_WORDS 64           /* the window the four frames live in */

typedef struct {
    psp_cpu_state cpu;
    uint32_t stack[STACK_WORDS], data;
} snapshot;

static void take(snapshot *s) {
    s->cpu = psp_cpu;
    for (int i = 0; i < STACK_WORDS; i++) s->stack[i] = psp_read32(STACK_TOP - 4u * STACK_WORDS + 4u * i);
    s->data = psp_read32(RP_DATA);
}
static void restore(const snapshot *s) {
    psp_cpu = s->cpu;
    for (int i = 0; i < STACK_WORDS; i++) psp_write32(STACK_TOP - 4u * STACK_WORDS + 4u * i, s->stack[i]);
    psp_write32(RP_DATA, s->data);
}
static void scramble(void) {
    for (int i = 1; i < 32; i++) psp_cpu.r[i] = 0xBAD00000u + (uint32_t)i;
    for (int i = 0; i < STACK_WORDS; i++) psp_write32(STACK_TOP - 4u * STACK_WORDS + 4u * i, 0xFEEDF00Du);
    psp_write32(RP_DATA, 0xFEEDF00Du);
}

/* The state each run starts from: every register distinct, $ra 0 -- the
 * thread-entry sentinel -- and the frames' memory cleared. */
static void fresh(void) {
    memset(&psp_cpu, 0, sizeof psp_cpu);
    for (int i = 1; i < 32; i++) psp_cpu.r[i] = 0x1000u + (uint32_t)i;
    psp_cpu.r[RP_SP] = STACK_TOP;
    psp_cpu.r[RP_RA] = 0;
    for (int i = 0; i < STACK_WORDS; i++) psp_write32(STACK_TOP - 4u * STACK_WORDS + 4u * i, 0);
    psp_write32(RP_DATA, 0);
}

/* The firmware call: v0 = a0 * 7 + 1. Interrupting, it is where a save is
 * taken -- the call has finished, so its result is in the snapshot -- and
 * the native run is abandoned, host frames and all. */
static int g_interrupt;
static snapshot g_saved;
static jmp_buf g_abandon;
static void firmware(void) {
    psp_cpu.r[RP_V0] = psp_cpu.r[RP_A0] * 7u + 1u;
    if (g_interrupt) { take(&g_saved); longjmp(g_abandon, 1); }
}

static int same(const snapshot *a, const snapshot *b, const char *what) {
    int ok = 1;
    for (int i = 0; i < 32; i++)
        if (a->cpu.r[i] != b->cpu.r[i]) {
            printf("  %s: r%d 0x%08X, expected 0x%08X\n", what, i, b->cpu.r[i], a->cpu.r[i]);
            ok = 0;
        }
    for (int i = 0; i < STACK_WORDS; i++)
        if (a->stack[i] != b->stack[i]) {
            printf("  %s: stack word %d 0x%08X, expected 0x%08X\n", what, i, b->stack[i], a->stack[i]);
            ok = 0;
        }
    if (a->data != b->data) { printf("  %s: result 0x%08X, expected 0x%08X\n", what, b->data, a->data); ok = 0; }
    return ok;
}

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    rewind(f);
    char *b = malloc((size_t)n + 1);
    if (b) b[fread(b, 1, (size_t)n, f)] = 0;
    fclose(f);
    return b;
}

int main(int argc, char **argv) {
    if (psp_mem_init() != 0) { printf("FAIL: psp_mem_init\n"); return 1; }
    for (int i = 0; i < RP_WORDS; i++) psp_write32(RP_BASE + 4u * i, RP_CODE[i]);
    psp_hle_register_unnamed(RP_NID, "ResumeTest", firmware);
    psp_recomp_register();

    /* The emitted shape: each return site is a case of its own function's
     * switch, has no dispatch thunk, and is in the table -- one per call. */
    CHECK(psp_resume_count() == 4, "four calls, four return sites: %d", psp_resume_count());
    const uint32_t sites[4] = { RP_MAIN + 0x10, RP_A + 0x14, RP_B + 0x14, RP_C + 0x10 };
    const uint32_t owners[4] = { RP_MAIN, RP_A, RP_B, RP_C };
    for (int i = 0; i < 4; i++)
        CHECK(psp_resume_lookup(sites[i]) != NULL, "return site 0x%08X is in the table", sites[i]);
    CHECK(psp_resume_lookup(RP_MAIN) == NULL, "a function entry is not a return site");
    if (argc > 1) {
        char path[1024];
        snprintf(path, sizeof path, "%s/t_resume_funcs.c", argv[1]);
        char *src = slurp(path);
        CHECK(src != NULL, "generated source readable at %s", path);
        for (int i = 0; src && i < 4; i++) {
            char want[96];
            snprintf(want, sizeof want, "case 0x%08Xu: goto L_%08X;", sites[i], sites[i]);
            CHECK(strstr(src, want) != NULL, "%s missing", want);
            snprintf(want, sizeof want, "psp_at_%08X", sites[i]);
            CHECK(strstr(src, want) == NULL, "%s: a return site gets no dispatch thunk", want);
            snprintf(want, sizeof want, "void psp_resume_%08X(uint32_t _site)", owners[i]);
            CHECK(strstr(src, want) != NULL, "%s missing", want);
        }
        free(src);
    }

    /* 1: uninterrupted. */
    fresh();
    g_interrupt = 0;
    psp_dispatch(RP_MAIN);
    snapshot plain;
    take(&plain);
    CHECK(plain.data == 855u, "the program computes 855, got %u", plain.data);
    CHECK(plain.cpu.r[RP_SP] == STACK_TOP, "the stack is balanced");

    /* 2: interrupted in the firmware call, every register and frame word
     * scrambled, the snapshot restored, resumed natively. */
    fresh();
    g_interrupt = 1;
    if (!setjmp(g_abandon)) {
        psp_dispatch(RP_MAIN);
        CHECK(0, "the firmware call interrupts the run");
    }
    g_interrupt = 0;
    CHECK(g_saved.cpu.r[RP_RA] == RP_C + 0x10, "the save holds C's return site: 0x%08X",
          g_saved.cpu.r[RP_RA]);
    CHECK(g_saved.data == 0, "nothing past the call has run");
    scramble();
    restore(&g_saved);
    uint32_t missing = 0;
    CHECK(psp_resume_chain(psp_cpu.r[RP_RA], 0, &missing) == 0,
          "the chain resumes to the thread entry (stopped at 0x%08X)", missing);
    snapshot resumed;
    take(&resumed);
    CHECK(same(&plain, &resumed, "resumed"), "the resumed run ends as the uninterrupted one");

    /* 3: the interpreter, from the same snapshot and site. */
    restore(&g_saved);
    const uint32_t site = psp_cpu.r[RP_RA];
    psp_interp it;
    psp_interp_init(&it, site, 0, 100000);
    psp_cpu.r[RP_RA] = site;        /* init sets $ra to the sentinel */
    psp_interp_run(&it);
    CHECK(it.status == I_OK_RETURN, "the interpreter returns to the entry: %s",
          psp_interp_status_str(it.status));
    snapshot interpreted;
    take(&interpreted);
    CHECK(same(&plain, &interpreted, "interpreted"), "the interpreter agrees");

    /* A site that is no return site is refused before anything runs. */
    restore(&g_saved);
    missing = 0;
    CHECK(psp_resume_chain(RP_C + 0x14, 0, &missing) == -1 && missing == RP_C + 0x14,
          "an address that is not a return site is refused, and named");
    CHECK(psp_read32(RP_DATA) == 0, "and nothing ran");

    psp_mem_free();
    if (failures) { printf("\n%d check(s) failed\n", failures); return 1; }
    printf("resume: a three-deep chain interrupted in a firmware call resumes natively, "
           "and agrees with the uninterrupted run and the interpreter\n");
    return 0;
}
