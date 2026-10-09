/* Emitter tests — synthetic code only, no game data.
 *
 * The function under test is eight instructions of hand-assembled MIPS chosen
 * to exercise the parts of emission that are actually hard: a conditional
 * branch with a delay slot, a backward-reachable label, and a `jr $ra` whose
 * delay slot must run before the return.
 *
 * The assertions are about the *shape* of the generated C, because that shape
 * is the contract. In particular the branch must read its condition into a
 * temporary before the delay slot runs — if that ever regresses, a delay slot
 * that writes a condition register silently changes which way the branch goes,
 * once in a thousand iterations, in a game nobody can debug.
 */

#include "analyze.h"
#include "emit.h"

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

#define BASE 0x08804000u

/* addiu $sp,$sp,-16      prologue
 * sw    $ra,12($sp)
 * addu  $v0,$a0,$a1
 * beq   $v0,$zero,+2     -> the `jr $ra` at BASE+24
 * addiu $v0,$v0,1        delay slot: runs either way, and writes $v0 which
 *                        the branch above just read
 * lw    $ra,12($sp)
 * jr    $ra
 * addiu $sp,$sp,16       delay slot: runs before the return
 */
static const uint32_t CODE[] = {
    0x27BDFFF0u, 0xAFBF000Cu, 0x00851021u, 0x10400002u,
    0x24420001u, 0x8FBF000Cu, 0x03E00008u, 0x27BD0010u,
};

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *b = (char *)malloc((size_t)n + 1);
    if (!b) { fclose(f); return NULL; }
    size_t got = fread(b, 1, (size_t)n, f);
    b[got] = '\0';
    fclose(f);
    if (len) *len = got;
    return b;
}

/* Assert a substring appears, reporting what was missing if not. */
static void expect_contains(const char *hay, const char *needle, const char *why) {
    if (!strstr(hay, needle)) {
        printf("FAIL %s\n  expected to find: %s\n", why, needle);
        failures++;
    }
}


/* NOTE: the fall-through-into-a-label bug that the differential oracle found in
 * memset (see the comment at the fix site in emit.c) has no test here.
 *
 * Reproducing it needs discovery to leave the fall-through target as a *label*
 * inside a neighbouring function. Several synthetic shapes were tried and
 * discovery promoted the target to its own entry every time, which exercises
 * the path that always worked -- a test that passes both before and after the
 * fix, which is worse than no test because it implies coverage that is not
 * there. Building one needs a_discover's ownership rules pinned down first.
 *
 * The fix is verified against the real module instead: memset returned $v0 = 0
 * before and $v0 = $a0 after, and corpus divergences fell from 17 to 8 over the
 * same 400 functions. */


/* ---- a return's delay slot must be emitted even when another function owns it
 *
 * Regression test for the third bug the differential oracle found.
 *
 * The delay slot used to be emitted only when discovery had assigned that word
 * to the same function. But a delay slot executes because the hardware
 * executes it; ownership is an artifact of the analysis. Where the two
 * disagreed the instruction was silently dropped.
 *
 * Which instruction that is matters: a MIPS compiler puts the stack restore in
 * the delay slot of `jr $ra`, so the dropped instruction is typically
 * `addiu $sp, $sp, N` and the function returns without releasing its frame.
 *
 * Two seeds, the second landing on the delay slot itself, is enough to make
 * ownership and execution disagree. */

#define DS_BASE 0x08820000u

static const uint32_t DS_CODE[] = {
    0x03E00008,  /* +00  jr    $ra              function A                     */
    0x27BD0010,  /* +04  addiu $sp, $sp, 16     A's delay slot -- and B's entry */
    0x03E00008,  /* +08  jr    $ra              B returns                      */
    0x00000000,  /* +0C  nop                                                   */
};

static void test_return_delay_slot_not_owned(void) {
    uint8_t code[sizeof DS_CODE];
    for (size_t i = 0; i < sizeof DS_CODE / sizeof DS_CODE[0]; i++) {
        code[i * 4 + 0] = (uint8_t)(DS_CODE[i]);
        code[i * 4 + 1] = (uint8_t)(DS_CODE[i] >> 8);
        code[i * 4 + 2] = (uint8_t)(DS_CODE[i] >> 16);
        code[i * 4 + 3] = (uint8_t)(DS_CODE[i] >> 24);
    }

    a_analysis an;
    memset(&an, 0, sizeof an);
    an.code = code;
    an.base = DS_BASE;
    an.size = (uint32_t)sizeof code;

    const uint32_t seeds[2] = { DS_BASE, DS_BASE + 4 };
    CHECK(a_discover(&an, seeds, 2, 2) == 0, "delay slot: discovery runs");

    /* Zero-initialised: the optional fields (imports, replace) are read by the
     * emitter, and leaving them as whatever was on the stack made these tests
     * depend on luck. */
    emit_opts o = {0};
    o.outdir = ".";
    o.prefix = "t_ds";
    o.module = "synthetic";
    CHECK(a_emit(&an, &o) == 0, "delay slot: emission succeeds");

    char *src = slurp("./t_ds_funcs.c", NULL);
    CHECK(src != NULL, "delay slot: generated .c is readable");
    if (!src) { a_analysis_free(&an); return; }

    const char *body = strstr(src, "psp_body_08820000");
    CHECK(body != NULL, "delay slot: function A was emitted");
    if (body) {
        const char *end = strstr(body, "\n}\n");
        if (end) {
            char *tail = strndup(body, (size_t)(end - body));
            CHECK(tail && strstr(tail, "r_sp = r_sp + 16;"),
                  "delay slot: `jr $ra` must emit its delay slot even when\n"
                  "  another function owns that word -- dropping it returns\n"
                  "  without releasing the frame");
            free(tail);
        }
    }
    free(src);
    a_analysis_free(&an);
}


/* ---- an indirect call is not the end of a function ------------------------
 *
 * Regression test for the fourth bug the oracle found.
 *
 * The emitter decided a block was terminal with
 *
 *     last_terminal = in.is_return || in.is_indirect || (is_jump && !is_call)
 *
 * but `is_indirect` covers `jr` and `jalr` alike, and only one of them ends
 * anything. `jalr` is a call: it returns, and execution continues after its
 * delay slot. Marking it terminal suppressed the end-of-function continuation,
 * so when a function's extent ended right after an indirect call -- which is
 * where discovery routinely splits -- whatever followed was never reached.
 *
 * What followed was usually the epilogue, so the function returned without
 * releasing its frame. The oracle found it as clusters of $sp disagreements.
 *
 * The decoder already draws the distinction: it sets ends_block for `jr` and
 * not for `jalr`. Two seeds put the continuation in a separate function so the
 * extent ends exactly at the point that used to be dropped. */

#define IC_BASE 0x08830000u

static const uint32_t IC_CODE[] = {
    0x0320F809,  /* +00  jalr  $ra, $t9      indirect call -- returns here     */
    0x00000000,  /* +04  nop                 delay slot; A's extent ends after */
    0x27BD0010,  /* +08  addiu $sp, $sp, 16  the continuation -- B's entry     */
    0x03E00008,  /* +0C  jr    $ra                                             */
    0x00000000,  /* +10  nop                                                   */
};

static void test_indirect_call_is_not_terminal(void) {
    uint8_t code[sizeof IC_CODE];
    for (size_t i = 0; i < sizeof IC_CODE / sizeof IC_CODE[0]; i++) {
        code[i * 4 + 0] = (uint8_t)(IC_CODE[i]);
        code[i * 4 + 1] = (uint8_t)(IC_CODE[i] >> 8);
        code[i * 4 + 2] = (uint8_t)(IC_CODE[i] >> 16);
        code[i * 4 + 3] = (uint8_t)(IC_CODE[i] >> 24);
    }

    a_analysis an;
    memset(&an, 0, sizeof an);
    an.code = code;
    an.base = IC_BASE;
    an.size = (uint32_t)sizeof code;

    const uint32_t seeds[2] = { IC_BASE, IC_BASE + 8 };
    CHECK(a_discover(&an, seeds, 2, 2) == 0, "indirect call: discovery runs");

    /* Zero-initialised: the optional fields (imports, replace) are read by the
     * emitter, and leaving them as whatever was on the stack made these tests
     * depend on luck. */
    emit_opts o = {0};
    o.outdir = ".";
    o.prefix = "t_ic";
    o.module = "synthetic";
    CHECK(a_emit(&an, &o) == 0, "indirect call: emission succeeds");

    char *src = slurp("./t_ic_funcs.c", NULL);
    CHECK(src != NULL, "indirect call: generated .c is readable");
    if (!src) { a_analysis_free(&an); return; }

    const char *body = strstr(src, "psp_body_08830000");
    CHECK(body != NULL, "indirect call: function A was emitted");
    if (body) {
        const char *end = strstr(body, "\n}\n");
        if (end) {
            char *tail = strndup(body, (size_t)(end - body));
            CHECK(tail && strstr(tail, "psp_dispatch(r_t9)"),
                  "indirect call: the call itself must be emitted");
            CHECK(tail && (strstr(tail, "psp_func_08830008()") ||
                           strstr(tail, "psp_dispatch(0x08830008u)")),
                  "indirect call: execution must continue after `jalr` --\n"
                  "  a call returns, so the following code is still reached");
            free(tail);
        }
    }
    free(src);
    a_analysis_free(&an);
}

/* ---- the VFPU branches have a condition ------------------------------------
 *
 * Regression test for the branch the emitter did not translate.
 *
 * bvt/bvf and their likely forms test one of the six VFPU condition codes,
 * indexed by bits 18..20 of the word. branch_cond() had no case for them and
 * fell to its default, which emits a condition of `0` with a comment -- a
 * branch never taken, with no trap and no diagnostic, in a program that runs.
 * Armored Core's polygon clipper skips a store with bvf; never skipping it
 * doubled the vertex count at every clip plane and ran over the caller's
 * frame. The interpreter had the same gap, so the oracle saw two translations
 * agreeing.
 *
 * Both senses, two different code indices, so a mix-up between them fails. */

#define BV_BASE 0x08840000u

static const uint32_t BV_CODE[] = {
    0x49090002,  /* +00  bvt   2, +2          cc 2, true  -> +0C            */
    0x00000000,  /* +04  nop                  delay slot                    */
    0x24420001,  /* +08  addiu $v0, $v0, 1    skipped when taken            */
    0x49000002,  /* +0C  bvf   0, +2          cc 0, false -> +18            */
    0x00000000,  /* +10  nop                                                */
    0x24420002,  /* +14  addiu $v0, $v0, 2                                  */
    0x03E00008,  /* +18  jr    $ra                                          */
    0x00000000,  /* +1C  nop                                                */
};

static void test_vfpu_branch_condition(void) {
    uint8_t code[sizeof BV_CODE];
    for (size_t i = 0; i < sizeof BV_CODE / sizeof BV_CODE[0]; i++) {
        code[i * 4 + 0] = (uint8_t)(BV_CODE[i]);
        code[i * 4 + 1] = (uint8_t)(BV_CODE[i] >> 8);
        code[i * 4 + 2] = (uint8_t)(BV_CODE[i] >> 16);
        code[i * 4 + 3] = (uint8_t)(BV_CODE[i] >> 24);
    }

    a_analysis an;
    memset(&an, 0, sizeof an);
    an.code = code;
    an.base = BV_BASE;
    an.size = (uint32_t)sizeof code;

    const uint32_t seed = BV_BASE;
    CHECK(a_discover(&an, &seed, 1, 1) == 0, "vfpu branch: discovery runs");

    /* Zero-initialised: the optional fields (imports, replace) are read by the
     * emitter, and leaving them as whatever was on the stack made these tests
     * depend on luck. */
    emit_opts o = {0};
    o.outdir = ".";
    o.prefix = "t_bv";
    o.module = "synthetic";
    CHECK(a_emit(&an, &o) == 0, "vfpu branch: emission succeeds");

    char *src = slurp("./t_bv_funcs.c", NULL);
    CHECK(src != NULL, "vfpu branch: generated .c is readable");
    if (!src) { a_analysis_free(&an); return; }

    CHECK(strstr(src, "psp_vfpu_cond(2)") != NULL,
          "vfpu branch: bvt tests the condition code its word names");
    CHECK(strstr(src, "!psp_vfpu_cond(0)") != NULL,
          "vfpu branch: bvf tests the negation of its condition code");
    CHECK(strstr(src, "unhandled branch") == NULL,
          "vfpu branch: no branch in this function is emitted as never taken");
    free(src);
    a_analysis_free(&an);
}

/* ---- a branch in a delay slot does not hide the word after it ---------------
 *
 * Regression test for WipEout Pulse's compile error, `use of undeclared label
 * 'L_0029148C'`.
 *
 * Real code never puts a branch in a delay slot, but data decoded as code
 * does. The first pass marked the slot of *every* transfer as consumed,
 * including a transfer that was itself a slot -- which the main pass never
 * emits as a branch, so nothing consumes its slot. The word after it was then
 * skipped as a slot that no branch emitted, and when another branch targeted
 * it the `goto` named a label no one printed.
 *
 * Only the slots the main pass really consumes count: a slot's own slot is an
 * ordinary instruction. */

#define SS_BASE 0x08850000u

static const uint32_t SS_CODE[] = {
    0x50850003,  /* +00  beql  $a0, $a1, +10   targets the word after +0C   */
    0x00000000,  /* +04  nop                                                */
    0x1CC00002,  /* +08  bgtz  $a2, +14                                     */
    0x14E00001,  /* +0C  bne   $a3, $zero, +14 a branch in a delay slot     */
    0x24420001,  /* +10  addiu $v0, $v0, 1     not a slot: +0C's own slot    */
    0x03E00008,  /* +14  jr    $ra                                          */
    0x00000000,  /* +18  nop                                                */
};

static void test_branch_in_slot_keeps_next_label(void) {
    uint8_t code[sizeof SS_CODE];
    for (size_t i = 0; i < sizeof SS_CODE / sizeof SS_CODE[0]; i++) {
        code[i * 4 + 0] = (uint8_t)(SS_CODE[i]);
        code[i * 4 + 1] = (uint8_t)(SS_CODE[i] >> 8);
        code[i * 4 + 2] = (uint8_t)(SS_CODE[i] >> 16);
        code[i * 4 + 3] = (uint8_t)(SS_CODE[i] >> 24);
    }

    a_analysis an;
    memset(&an, 0, sizeof an);
    an.code = code;
    an.base = SS_BASE;
    an.size = (uint32_t)sizeof code;

    const uint32_t seed = SS_BASE;
    CHECK(a_discover(&an, &seed, 1, 1) == 0, "slot branch: discovery runs");

    emit_opts o = {0};
    o.outdir = ".";
    o.prefix = "t_ss";
    o.module = "synthetic";
    CHECK(a_emit(&an, &o) == 0, "slot branch: emission succeeds");

    char *src = slurp("./t_ss_funcs.c", NULL);
    CHECK(src != NULL, "slot branch: generated .c is readable");
    if (!src) { a_analysis_free(&an); return; }

    CHECK(strstr(src, "goto L_08850010;") != NULL,
          "slot branch: the beql jumps to the word after the slot branch");
    CHECK(strstr(src, "L_08850010:") != NULL,
          "slot branch: the word after a branch in a delay slot is emitted\n"
          "  under its label -- otherwise the `goto` does not compile");

    /* Every goto in the file names a label the file declares. */
    for (const char *p = src; (p = strstr(p, "goto L_")) != NULL; p += 7) {
        char label[16];
        memcpy(label, p + 5, 10);
        label[10] = ':';
        label[11] = 0;
        CHECK(strstr(src, label) != NULL, "slot branch: `goto %.10s` has a label", p + 5);
    }
    free(src);
    a_analysis_free(&an);
}

/* ---- code nothing reaches is swept, stubs and data are not ------------------
 *
 * WipEout Pulse's MD5 enters its round helpers by adding to an address and
 * jumping through it, so no walk, call or pointer names them and a run
 * stopped on a dispatch miss at 0x0000BFA0. The gap sweep seeds such code by
 * its shape. Two shapes it must leave alone: an unreachable `b .` just before
 * a function, which the walk would carry on from into that function and
 * merge with it, and an unclaimed run that starts with data. */

#define GS_BASE 0x08860000u

static const uint32_t GS_CODE[] = {
    0x03E00008,  /* +00  jr    $ra             A, seeded                    */
    0x00000000,  /* +04  nop                                                */
    0x24420001,  /* +08  addiu $v0, $v0, 1     B, nothing names it: swept   */
    0x03E00008,  /* +0C  jr    $ra                                          */
    0x00000000,  /* +10  nop                                                */
    0x1000FFFF,  /* +14  b     .               a stub just before C: not    */
    0x00000000,  /* +18  nop                                                */
    0x03E00008,  /* +1C  jr    $ra             C, seeded                    */
    0x00000000,  /* +20  nop                                                */
    0x72616552,  /* +24  "Rear"                data: the run is not code    */
    0x24420001,  /* +28  addiu $v0, $v0, 1                                  */
    0x03E00008,  /* +2C  jr    $ra                                          */
    0x00000000,  /* +30  nop                                                */
};

static void test_gap_sweep(void) {
    uint8_t code[sizeof GS_CODE];
    for (size_t i = 0; i < sizeof GS_CODE / sizeof GS_CODE[0]; i++) {
        code[i * 4 + 0] = (uint8_t)(GS_CODE[i]);
        code[i * 4 + 1] = (uint8_t)(GS_CODE[i] >> 8);
        code[i * 4 + 2] = (uint8_t)(GS_CODE[i] >> 16);
        code[i * 4 + 3] = (uint8_t)(GS_CODE[i] >> 24);
    }

    a_analysis an;
    memset(&an, 0, sizeof an);
    an.code = code;
    an.base = GS_BASE;
    an.size = (uint32_t)sizeof code;
    an.sweep_gaps = 1;

    const uint32_t seeds[2] = { GS_BASE, GS_BASE + 0x1C };
    CHECK(a_discover(&an, seeds, 2, 2) == 0, "gap sweep: discovery runs");
    CHECK(an.nswept == 1, "gap sweep: one block swept, got %d", an.nswept);

    emit_opts o = {0};
    o.outdir = ".";
    o.prefix = "t_gs";
    o.module = "synthetic";
    CHECK(a_emit(&an, &o) == 0, "gap sweep: emission succeeds");

    char *src = slurp("./t_gs_funcs.c", NULL);
    CHECK(src != NULL, "gap sweep: generated .c is readable");
    if (!src) { a_analysis_free(&an); return; }

    CHECK(strstr(src, "psp_body_08860008") != NULL,
          "gap sweep: code nothing names becomes a function");
    CHECK(strstr(src, "psp_body_0886001C") != NULL && strstr(src, "psp_at_0886001C") == NULL,
          "gap sweep: the function after a `b .` stub stays a function of its own");
    CHECK(strstr(src, "08860014u") == NULL,
          "gap sweep: the `b .` stub is not an entry");
    CHECK(strstr(src, "08860028u") == NULL,
          "gap sweep: code after data in an unclaimed run is not swept");
    free(src);
    a_analysis_free(&an);
}

/* ---- CC latency and FPU traps -----------------------------------------------
 *
 * An mfvc of CC as the word straight after a vcmp reads CC from before that
 * vcmp (vfpuprobe v3 step 117, fw 6.60); one word later it reads the new
 * value. And a COP1 operation that raises an enabled exception stops the
 * program (v3 steps 189-191), so each one is followed by the check. */

#define CL_BASE 0x08850000u

static const uint32_t CL_CODE[] = {
    0x6C008080,  /* +00  vcmp.q FL, C000, C000                              */
    0x48620083,  /* +04  mfvc  $v0, $131      straight after: the old CC    */
    0x48630083,  /* +08  mfvc  $v1, $131      one later: the new CC         */
    0x46062103,  /* +0C  div.s $f4, $f4, $f6                                */
    0x03E00008,  /* +10  jr    $ra                                          */
    0x00000000,  /* +14  nop                                                */
};

static void test_cc_latency_and_fpu_trap(void) {
    uint8_t code[sizeof CL_CODE];
    for (size_t i = 0; i < sizeof CL_CODE / sizeof CL_CODE[0]; i++) {
        code[i * 4 + 0] = (uint8_t)(CL_CODE[i]);
        code[i * 4 + 1] = (uint8_t)(CL_CODE[i] >> 8);
        code[i * 4 + 2] = (uint8_t)(CL_CODE[i] >> 16);
        code[i * 4 + 3] = (uint8_t)(CL_CODE[i] >> 24);
    }
    a_analysis an;
    memset(&an, 0, sizeof an);
    an.code = code;
    an.base = CL_BASE;
    an.size = (uint32_t)sizeof code;
    const uint32_t seed = CL_BASE;
    CHECK(a_discover(&an, &seed, 1, 1) == 0, "cc latency: discovery runs");

    emit_opts o = {0};
    o.outdir = ".";
    o.prefix = "t_cl";
    o.module = "synthetic";
    CHECK(a_emit(&an, &o) == 0, "cc latency: emission succeeds");

    char *src = slurp("./t_cl_funcs.c", NULL);
    CHECK(src != NULL, "cc latency: generated .c is readable");
    if (!src) { a_analysis_free(&an); return; }
    CHECK(strstr(src, "r_v0 = psp_mfvc_cc_after_vcmp();") != NULL,
          "cc latency: the mfvc right after vcmp reads the old CC");
    CHECK(strstr(src, "r_v1 = psp_mfvc(3);") != NULL,
          "cc latency: the next mfvc reads CC itself");
    CHECK(strstr(src, "if (psp_fpu_trap_pending()) psp_unimplemented(0x0885000Cu, \"FPU exception\");") != NULL,
          "fpu trap: div.s is followed by the enabled-exception check");
    free(src);
    a_analysis_free(&an);
}

/* A replaced function keeps its body under __orig and gives up its public name.
 *
 * The contract has four halves and all four matter: the body still exists (so a
 * replacement can defer to the original), the public symbol is *absent* (so the
 * host's definition is what every call site binds to), the header declares both
 * (so the two cannot disagree), and registration still names the public symbol
 * (so indirect calls reach the replacement too, not just direct ones). */
static void test_replace_leaves_the_symbol_to_the_host(void) {
    uint8_t code[sizeof CODE];
    for (size_t i = 0; i < sizeof CODE / sizeof CODE[0]; i++) {
        code[i * 4 + 0] = (uint8_t)(CODE[i]);
        code[i * 4 + 1] = (uint8_t)(CODE[i] >> 8);
        code[i * 4 + 2] = (uint8_t)(CODE[i] >> 16);
        code[i * 4 + 3] = (uint8_t)(CODE[i] >> 24);
    }

    a_analysis an;
    memset(&an, 0, sizeof an);
    an.code = code;
    an.base = BASE;
    an.size = (uint32_t)sizeof code;

    uint32_t seed = BASE;
    CHECK(a_discover(&an, &seed, 1, 1) == 0, "replace: discovery runs");

    const uint32_t replace[1] = { BASE };
    emit_opts o = {0};
    o.outdir = ".";
    o.prefix = "t_rep";
    o.module = "synthetic";
    o.replace = replace;
    o.nreplace = 1;
    CHECK(a_emit(&an, &o) == 0, "replace: emission succeeds");

    char *src = slurp("./t_rep_funcs.c", NULL);
    CHECK(src != NULL, "replace: generated .c is readable");
    if (!src) { a_analysis_free(&an); return; }

    expect_contains(src, "void psp_func_08804000__orig(void)",
                    "replace: the original body is still emitted, under __orig");
    CHECK(strstr(src, "void psp_func_08804000(void) {") == NULL,
          "replace: the public symbol is left undefined for the host");
    expect_contains(src, "psp_body_08804000",
                    "replace: the translated body itself is untouched");
    expect_contains(src, "psp_register(0x08804000u, psp_replaced_08804000);",
                    "replace: registration points at the replacement's marker, "
                    "so indirect calls count its host frame");
    expect_contains(src, "void psp_replaced_08804000(void) "
                         "{ PSP_REPLACED(0x08804000u, psp_func_08804000()); }",
                    "replace: the marker calls the public symbol, so indirect "
                    "calls still reach the replacement");
    free(src);

    char *hdr = slurp("./t_rep_funcs.h", NULL);
    CHECK(hdr != NULL, "replace: generated .h is readable");
    if (hdr) {
        expect_contains(hdr, "void psp_func_08804000(void);",
                        "replace: header still declares the public symbol");
        expect_contains(hdr, "void psp_func_08804000__orig(void);",
                        "replace: header declares the original for the host to call");
        free(hdr);
    }

    a_analysis_free(&an);
}

/* Not listing an address must change nothing at all -- this is what keeps every
 * existing gate green when the feature is compiled in but unused. */
static void test_replace_absent_changes_nothing(void) {
    uint8_t code[sizeof CODE];
    for (size_t i = 0; i < sizeof CODE / sizeof CODE[0]; i++) {
        code[i * 4 + 0] = (uint8_t)(CODE[i]);
        code[i * 4 + 1] = (uint8_t)(CODE[i] >> 8);
        code[i * 4 + 2] = (uint8_t)(CODE[i] >> 16);
        code[i * 4 + 3] = (uint8_t)(CODE[i] >> 24);
    }

    a_analysis an;
    memset(&an, 0, sizeof an);
    an.code = code;
    an.base = BASE;
    an.size = (uint32_t)sizeof code;

    uint32_t seed = BASE;
    CHECK(a_discover(&an, &seed, 1, 1) == 0, "replace/none: discovery runs");

    /* An address that names no function: the emitter must ignore it rather
     * than replace something adjacent. */
    const uint32_t bogus[1] = { BASE + 0x1000u };
    emit_opts o = {0};
    o.outdir = ".";
    o.prefix = "t_norep";
    o.module = "synthetic";
    o.replace = bogus;
    o.nreplace = 1;
    CHECK(a_emit(&an, &o) == 0, "replace/none: emission succeeds");

    char *src = slurp("./t_norep_funcs.c", NULL);
    CHECK(src != NULL, "replace/none: generated .c is readable");
    if (!src) { a_analysis_free(&an); return; }

    expect_contains(src, "void psp_func_08804000(void) {",
                    "replace/none: an unlisted function keeps its public name");
    CHECK(strstr(src, "__orig") == NULL,
          "replace/none: nothing is renamed when no listed address matches");
    free(src);
    a_analysis_free(&an);
}

int main(void) {
    uint8_t code[sizeof CODE];
    for (size_t i = 0; i < sizeof CODE / sizeof CODE[0]; i++) {
        code[i * 4 + 0] = (uint8_t)(CODE[i]);
        code[i * 4 + 1] = (uint8_t)(CODE[i] >> 8);
        code[i * 4 + 2] = (uint8_t)(CODE[i] >> 16);
        code[i * 4 + 3] = (uint8_t)(CODE[i] >> 24);
    }

    a_analysis an;
    memset(&an, 0, sizeof an);
    an.code = code;
    an.base = BASE;
    an.size = (uint32_t)sizeof code;

    uint32_t seed = BASE;
    CHECK(a_discover(&an, &seed, 1, 1) == 0, "discovery runs");
    CHECK(an.nfuncs == 1, "one function found, got %d", an.nfuncs);
    if (an.nfuncs != 1) return 1;
    CHECK(an.funcs[0].addr == BASE, "function entry is the seed");
    CHECK(an.funcs[0].has_return, "function reaches a `jr $ra`");
    CHECK(an.insns == 8, "all eight instructions visited, got %llu",
          (unsigned long long)an.insns);

    /* Zero-initialised: the optional fields (imports, replace) are read by the
     * emitter, and leaving them as whatever was on the stack made these tests
     * depend on luck. */
    emit_opts o = {0};
    o.outdir = ".";
    o.prefix = "t_emit";
    o.module = "synthetic";
    CHECK(a_emit(&an, &o) == 0, "emission succeeds");

    char *src = slurp("./t_emit_funcs.c", NULL);
    CHECK(src != NULL, "generated .c is readable");
    if (!src) return 1;

    expect_contains(src, "void psp_func_08804000(void)",
                    "function is named after its address");

    /* The prologue, lowered to plain C. */
    expect_contains(src, "r_sp = r_sp + -16;", "addiu lowers to arithmetic");
    expect_contains(src, "psp_write32(r_sp + 12, r_ra);", "sw lowers to a memory write");
    expect_contains(src, "r_v0 = r_a0 + r_a1;", "addu lowers to addition");

    /* Every statement carries its address and disassembly. */
    expect_contains(src, "/* 08804000  addiu", "statements carry their disassembly");

    /* THE important one: the branch condition is captured into a temporary
     * *before* the delay slot executes, because the delay slot writes $v0 and
     * the hardware read the old value. */
    expect_contains(src, "int _c = (r_v0 == r_zero);",
                    "branch condition is captured before the delay slot");
    {
        const char *cond = strstr(src, "int _c = (r_v0 == r_zero);");
        const char *slot = strstr(src, "r_v0 = r_v0 + 1;");
        const char *jump = cond ? strstr(cond, "if (_c) goto") : NULL;
        CHECK(cond && slot && jump && cond < slot && slot < jump,
              "ordering must be condition -> delay slot -> branch");
    }

    /* The branch target is a label, and the return runs its delay slot first. */
    expect_contains(src, "L_08804018:", "branch target becomes a label");
    {
        const char *lbl = strstr(src, "L_08804018:");
        const char *slot = lbl ? strstr(lbl, "r_sp = r_sp + 16;") : NULL;
        const char *ret = slot ? strstr(slot, "return;") : NULL;
        CHECK(slot && ret, "the `jr $ra` delay slot is emitted before the return");
    }

    /* $zero is never assigned: `addu $v0,$a0,$a1` reads it, but nothing may
     * write it. A generated `r_zero =` would corrupt every later use. */
    CHECK(strstr(src, "r_zero =") == NULL, "nothing ever assigns $zero");

    free(src);

    /* The header declares the function and the registration entry point. */
    char *hdr = slurp("./t_emit_funcs.h", NULL);
    CHECK(hdr != NULL, "generated .h is readable");
    if (hdr) {
        expect_contains(hdr, "void psp_func_08804000(void);", "header declares the function");
        expect_contains(hdr, "void psp_recomp_register(void);", "header declares registration");
        expect_contains(hdr, "#define r_sp", "header defines register aliases");
        free(hdr);
    }

    /* Registration wires the address to the function. */
    src = slurp("./t_emit_funcs.c", NULL);
    if (src) {
        expect_contains(src, "psp_register(0x08804000u, psp_func_08804000);",
                        "the function is registered for indirect dispatch");
        free(src);
    }

    a_analysis_free(&an);

    test_return_delay_slot_not_owned();
    test_indirect_call_is_not_terminal();
    test_vfpu_branch_condition();
    test_branch_in_slot_keeps_next_label();
    test_gap_sweep();
    test_cc_latency_and_fpu_trap();
    test_replace_leaves_the_symbol_to_the_host();
    test_replace_absent_changes_nothing();

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("all emitter checks passed (synthetic code, no game data)\n");
    return 0;
}
