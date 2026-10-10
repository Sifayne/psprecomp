/* Original synthetic regression for the interior SIGNAL 08 / END / FINISH /
 * END sequence emitted by T3B's own command writer at 003FC590. The game
 * appends more passes after it. No game assets or external emulator code. */
#include "psprecomp/hle.h"
#include "psprecomp/render.h"
#include "psprecomp/dispatch.h"
#include "psprecomp/interrupt.h"
#include "psprecomp/sched.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIST 0x08810000u
#define VERTS 0x08820000u
#define CALLBACK 0x08830000u
#define HANDLER 0x08840000u
#define SIGNAL_HANDLER 0x08840010u
#define SIGNAL_REWRITE 0x08840020u
#define FB 0x04000000u
static unsigned failures, words, finish_calls, signal_calls, cases;
static unsigned final_finish;
#define CHECK(c) do { if (!(c)) { failures++; \
    fprintf(stderr, "line %u: %s\n", __LINE__, #c); } } while (0)

static uint32_t call(uint32_t nid, uint32_t a, uint32_t b, uint32_t c) {
    psp_cpu.r[PSP_REG_A0] = a; psp_cpu.r[PSP_REG_A1] = b;
    psp_cpu.r[PSP_REG_A2] = c; psp_cpu.r[PSP_REG_A3] = 0;
    psp_hle_call(nid);
    return psp_cpu.r[PSP_REG_V0];
}
static void cmd(uint32_t word) { psp_write32(LIST + 4 * words++, word); }
static uint32_t pixel(unsigned x) { return psp_read32(FB + (480 + x) * 4) & 0xffffffu; }
static void finish(void) {
    finish_calls++;
    CHECK(psp_arg(0) == 0x77);
    CHECK(pixel(1) == 0xff && pixel(5) == 0xff00 && pixel(9) == 0xff0000);
}
static void signal_handler(void) { signal_calls++; }
static void vertex(unsigned i, unsigned x, unsigned y, uint32_t color) {
    uint32_t a = VERTS + i * 12;
    psp_write32(a, color);
    psp_write16(a + 4, x); psp_write16(a + 6, y); psp_write16(a + 8, 0);
}
static void barrier(void) {
    cmd(0x0e080000); cmd(0x0c000000); cmd(0x0f000000); cmd(0x0c000000);
}
static void setup(void) {
    psp_ge_reset();
    memset(psp_mem_ptr(FB, 480 * 272 * 4), 0, 480 * 272 * 4);
    words = finish_calls = signal_calls = 0;
    for (unsigned i = 0; i < 4; i++) {
        uint32_t color = 0xff000000u | (i < 3 ? 0xffu << (8 * i) : 0xffffffu);
        vertex(2 * i, 4 * i, 0, color);
        vertex(2 * i + 1, 4 * i + 3, 3, color);
    }
    cmd(0x10080000);                         /* BASE */
    cmd(0x9c000000); cmd(0x9d0401e0);        /* FB, stride 480 */
    cmd(0xd2000003);                         /* 8888 */
    cmd(0x1280011c);                         /* 2D color + position */
    cmd(0x01820000);                         /* VADDR */
    cmd(0x04060002);                         /* red rectangle */
    barrier();
    cmd(0x04060002);                         /* green rectangle */
    barrier();
    cmd(0x04060002);                         /* blue rectangle */
    final_finish = words;
    cmd(0x0f000077); cmd(0x0c000000);         /* actual list completion */
    cmd(0x04060002);                         /* must never draw white */
    cmd(0x0f000099); cmd(0x0c000000);
}
static void check_pixels(void) {
    CHECK(pixel(1) == 0xff && pixel(5) == 0xff00 && pixel(9) == 0xff0000);
    CHECK(pixel(13) == 0);
    CHECK(psp_ge_pixels() == 27);
}
static void queued(unsigned streaming, unsigned callbacks) {
    setup();
    int cb = -1;
    if (callbacks) {
        psp_write32(CALLBACK, SIGNAL_HANDLER); psp_write32(CALLBACK + 4, 0);
        psp_write32(CALLBACK + 8, HANDLER); psp_write32(CALLBACK + 12, 0);
        cb = (int)call(0xa4fc06a4, CALLBACK, 0, 0);
        CHECK(cb >= 0);
    }
    uint32_t id = call(0xab49e76a, LIST, streaming ? LIST : 0, (uint32_t)cb);
    if (streaming) {
        /* Release one word at a time, including each half of both pairs.
         * A synchronization boundary must survive every suspension point. */
        for (unsigned i = 1; i <= final_finish; i++) {
            call(0xe0d68148, id, LIST + 4 * i, 0);
            /* Held at its stall, the list peeks DRAWING (geprobe 6 steps 79
             * and 80, fw 6.60). */
            CHECK(call(0x03444eb4, id, 1, 0) == 2);
            CHECK(finish_calls == 0 && signal_calls == 0);
        }
        check_pixels();
        call(0xe0d68148, id, 0, 0);
        /* The final FINISH waits for the drawing before it, so the list is
         * done by the Sync rather than by the release (geprobe 5 step 50). */
        call(0xb287bd61, 0, 0, 0);
    } else call(0xb287bd61, 0, 0, 0);
    /* Done and synced, the id is released: ListSync(peek) reads 80000100
     * (geprobe step 26, fw 6.60). */
    CHECK(call(0x03444eb4, id, 1, 0) == 0x80000100u);
    CHECK(finish_calls == callbacks && signal_calls == 0);
    check_pixels();
    call(0xe0d68148, id, 0, 0);               /* completed list stays complete */
    CHECK(finish_calls == callbacks);
    check_pixels();
    cases++;
}
/* A SIGNAL that suspends (behaviour 1) holds the GE until its handler has
 * returned, so the handler's rewrite of the words behind it is what is drawn;
 * one that continues (2) is not waited for, and the walk has drawn on
 * (geprobe v24 scenes 146, 147, 152 and 153, fw 6.60). */
static uint32_t rewrite_at;
static void rewriting_handler(void) {
    signal_calls++;
    CHECK((psp_arg(0) & 0xffff) == 0x42);
    psp_write32(rewrite_at, 0);              /* NOP: no green rectangle */
}
static void suspending(unsigned behaviour) {
    setup();                                 /* vertices and a clear frame */
    words = 0;
    cmd(0x10080000); cmd(0x9c000000); cmd(0x9d0401e0); cmd(0xd2000003); cmd(0x1280011c);
    cmd(0x01820000);
    cmd(0x04060002);                         /* red rectangle */
    cmd(0x0e000042 | behaviour << 16); cmd(0x0c000000);
    rewrite_at = LIST + 4 * words;
    cmd(0x04060002);                         /* green, unless the handler was in time */
    cmd(0x0f000000); cmd(0x0c000000);
    psp_write32(CALLBACK, SIGNAL_REWRITE); psp_write32(CALLBACK + 4, 0);
    psp_write32(CALLBACK + 8, 0); psp_write32(CALLBACK + 12, 0);
    const int cb = (int)call(0xa4fc06a4, CALLBACK, 0, 0);
    CHECK(cb >= 0);
    const uint32_t id = call(0xab49e76a, LIST, 0, (uint32_t)cb);
    call(0xb287bd61, 0, 0, 0);
    CHECK(call(0x03444eb4, id, 1, 0) == 0x80000100u);
    CHECK(signal_calls == 1);
    CHECK(pixel(1) == 0xff);
    CHECK(pixel(5) == (behaviour == 1 ? 0 : 0xff00));
    call(0x05db22ce, (uint32_t)cb, 0, 0);    /* sceGeUnsetCallback */
    cases++;
}
static void environment(const char *key, const char *value) {
#ifdef _WIN32
    _putenv_s(key, value);
#else
    setenv(key, value, 1);
#endif
}
static void capture_lifetime(const char *path) {
    setup();
    const uint32_t marker = LIST + 0x800;
    psp_write32(marker, 0x11111111);
    psp_ge_drain_all();                     /* arm the first frame */
    uint32_t id = call(0xab49e76a, LIST, LIST + 11 * 4, (uint32_t)-1);
    CHECK(pixel(1) == 0xff && pixel(5) == 0); /* stalled after first barrier */
    psp_write32(marker, 0x22222222);
    call(0xe0d68148, id, 0, 0);              /* capture at real completion */
    psp_write32(marker, 0x33333333);          /* caller may reuse memory now */
    psp_ge_drain_all();                     /* write the completed capture */
    FILE *file = fopen(path, "rb");
    CHECK(file != NULL);
    if (file) {
        uint32_t header[10] = {0}, value = 0;
        CHECK(fread(header, sizeof header, 1, file) == 1);
        CHECK(header[0] == 0x50414347 && header[1] == 2 && header[3] == 1);
        long offset = (long)(sizeof header + header[2] + header[3] * 12 + marker - header[4]);
        CHECK(fseek(file, offset, SEEK_SET) == 0);
        CHECK(fread(&value, sizeof value, 1, file) == 1);
        CHECK(value == 0x22222222);          /* neither interior nor late snapshot */
        fclose(file);
        remove(path);                      /* synthetic test output */
    }
    check_pixels();
    cases++;
}
int main(int argc, char **argv) {
    if (argc > 2) return 2;
    if (argc == 2) {
        environment("PSPRECOMP_GE_CAPTURE", argv[1]);
        environment("PSPRECOMP_GE_CAPTURE_FRAME", "1");
        environment("PSPRECOMP_GE_CAPTURE_POLLS", "");
        environment("PSPRECOMP_GE_CAPTURE_MINCMDS", "0");
        environment("PSPRECOMP_GE_CAPTURE_MINMEAN", "0");
    }
    if (psp_mem_init()) return 2;
    psp_hle_init(); psp_cpu_reset(); psp_sched_set_threading(0);
    psp_interrupt_reset();
    psp_cpu.r[PSP_REG_SP] = 0x09fff000;
    CHECK(psp_render_select("software") == 0);
    CHECK(psp_interrupt_set_module(0x08800000, 0x08900000, 0) == 0);
    psp_register(HANDLER, finish); psp_register(SIGNAL_HANDLER, signal_handler);
    psp_register(SIGNAL_REWRITE, rewriting_handler);
    if (argc == 2) capture_lifetime(argv[1]);
    else {
        for (unsigned streaming = 0; streaming < 2; streaming++)
            for (unsigned callbacks = 0; callbacks < 2; callbacks++) queued(streaming, callbacks);
        suspending(1);
        suspending(2);
        setup();
        psp_ge_replay_list(LIST, 0, 0);
        check_pixels();
        CHECK(finish_calls == 0 && signal_calls == 0);
        cases++;
    }
    psp_sched_reset(); psp_mem_free();
    printf("GE interior sync: %u cases, %u failures\n", cases, failures);
    return failures ? 1 : 0;
}
