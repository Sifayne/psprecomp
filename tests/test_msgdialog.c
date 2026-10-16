/* The message dialog (sceUtilityMsgDialog, src/hle/utility.c) through the
 * real firmware calls: the status words and results a PSP gives in
 * pspautotests' utility/msgdialog, and the pad, read by the dialog itself as
 * one poll per Update. No game data. */
#include "psprecomp/hle.h"
#include "psprecomp/msgdialog.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

enum { INIT_START = 0x2AD8E239, GET_STATUS = 0x9A1C91D7, UPDATE = 0x95FC253B,
       SHUTDOWN_START = 0x67AF3428, ABORT = 0x4928BD96, WAIT_VBLANK = 0x984C27E7 };
enum { CROSS = 0x4000, CIRCLE = 0x2000, LEFT = 0x80, RIGHT = 0x20 };
static const uint32_t P = 0x08810000u;

static uint32_t call(uint32_t nid, uint32_t arg) {
    psp_cpu.r[PSP_REG_A0] = arg;
    psp_hle_call(nid);
    return psp_cpu.r[PSP_REG_V0];
}
static uint32_t status(void) { return call(GET_STATUS, 0); }
static void frame(void) { call(UPDATE, 1); call(WAIT_VBLANK, 0); }
static void press(uint32_t buttons) { psp_ctrl_set(buttons, 128, 128); frame(); psp_ctrl_set(0, 128, 128); frame(); }

/* A request as the test's: text mode, `size` bytes, cross confirming. */
static void request(uint32_t size, uint32_t mode, uint32_t error, uint32_t options) {
    for (uint32_t i = 0; i < 708; i++) psp_write8(P + i, 0);
    psp_write32(P, size);
    psp_write32(P + 4, 1);              /* language */
    psp_write32(P + 8, 1);              /* buttonSwap: cross confirms */
    psp_write32(P + 48, 0x1337);        /* the result word */
    psp_write32(P + 52, mode);
    psp_write32(P + 56, error);
    const char *msg = "Saving. Do not remove the memory stick.";
    for (uint32_t i = 0; msg[i]; i++) psp_write8(P + 60 + i, (uint8_t)msg[i]);
    if (size >= 580) psp_write32(P + 572, options);
}

/* Up to VISIBLE: 1 for 34 frames, then 2, and the result word 0. */
static void show(void) {
    CHECK(call(INIT_START, P) == 0, "InitStart");
    int frames = 0;
    while (status() == 1 && frames < 100) { frame(); frames++; }
    CHECK(frames == 34 && status() == 2, "visible after %d frames, not 34", frames);
    CHECK(psp_read32(P + 48) == 0, "the result word is 0 once visible: %08X", psp_read32(P + 48));
    frame();                            /* drawn once before anyone presses */
}

/* From 3: ShutdownStart, 4 until the caller stands aside, then 0. */
static void finish(void) {
    CHECK(status() == 3, "status %u, not 3", status());
    CHECK(call(SHUTDOWN_START, 0) == 0, "ShutdownStart");
    CHECK(status() == 4 && status() == 4, "4 until the caller stands aside");
    call(WAIT_VBLANK, 0);
    CHECK(status() == 0, "0 after a vblank");
}

static void yes_no(void) {
    psp_msgdialog_view v;
    memset(&v, 0, sizeof v);
    CHECK(psp_msgdialog_snapshot(&v) == 0, "nothing to draw before InitStart");
    request(708, 1, 0, 0x01 | 0x10);
    show();
    CHECK(psp_msgdialog_snapshot(&v) == 2 && v.active && v.yes_no && !v.selected_no && !v.confirm_circle &&
          !strcmp(v.message, "Saving. Do not remove the memory stick."), "the view of a Yes/No dialog");
    CHECK(psp_msgdialog_snapshot(&v) == 1, "an unchanged view is not copied again");
    press(RIGHT);
    CHECK(psp_msgdialog_snapshot(&v) == 2 && v.selected_no, "right moves the cursor to No");
    press(CROSS);
    CHECK(psp_read32(P + 576) == 2, "confirmed on No: buttonPressed %u", psp_read32(P + 576));
    CHECK(psp_msgdialog_snapshot(&v) == 0, "a closed dialog is not drawn");
    finish();

    request(708, 1, 0, 0x01 | 0x10 | 0x100);
    show();
    psp_msgdialog_snapshot(&v);
    CHECK(v.selected_no, "DEFAULT_NO starts the cursor on No");
    press(LEFT);
    press(CROSS);
    CHECK(psp_read32(P + 576) == 1, "left, then confirm: Yes (%u)", psp_read32(P + 576));
    finish();
}

static void buttons(void) {
    /* Cancel is the other button: Back. */
    request(580, 1, 0, 0x01);
    show();
    press(CIRCLE);
    CHECK(status() == 3 && psp_read32(P + 576) == 3, "circle cancels: Back (%u)", psp_read32(P + 576));
    finish();

    /* buttonSwap 0: circle confirms. */
    request(580, 1, 0, 0x01);
    psp_write32(P + 8, 0);
    show();
    press(CIRCLE);
    CHECK(psp_read32(P + 576) == 1, "circle confirms with buttonSwap 0 (%u)", psp_read32(P + 576));
    finish();

    /* A button held from before the dialog showed does nothing until it is
     * let go and pressed again. */
    request(580, 1, 0, 0x01);
    psp_ctrl_set(CROSS, 128, 128);
    show();
    for (int i = 0; i < 5; i++) frame();
    CHECK(status() == 2, "a held cross does not close it");
    psp_ctrl_set(0, 128, 128);
    frame();
    press(CROSS);
    CHECK(status() == 3, "pressed again, it does");
    finish();

    /* A 572-byte request has no buttonPressed: the word after it is not touched. */
    request(572, 1, 0, 0);
    psp_write32(P + 576, 0xAAAAAAAAu);
    show();
    press(CROSS);
    CHECK(psp_read32(P + 576) == 0xAAAAAAAAu, "a 572-byte request's neighbour is left alone");
    finish();
}

/* A refused request shows, ends 9 frames later with the refusal as its
 * result, and never reaches the host. */
static void refused(uint32_t size, uint32_t mode, uint32_t error, uint32_t options, uint32_t result) {
    psp_msgdialog_view v;
    memset(&v, 0, sizeof v);
    request(size, mode, error, options);
    CHECK(call(INIT_START, P) == 0, "InitStart");
    int frames = 0;
    while (status() == 1 && frames < 100) { frame(); frames++; }
    CHECK(frames == 34 && psp_read32(P + 48) == 0x1337, "visible after 34 frames, its result untouched");
    while (status() == 2 && frames < 200) { frame(); frames++; }
    CHECK(frames == 43, "3 after 9 frames more, not %d", frames - 34);
    CHECK(psp_read32(P + 48) == result, "result %08X, not %08X", psp_read32(P + 48), result);
    CHECK(psp_msgdialog_snapshot(&v) == 0, "a refused dialog is not drawn");
    finish();
}

static void refusals(void) {
    refused(580, 1, 0, 0x01 | 0x200, 0x80110501u);
    refused(708, 0, 0x80020323u, 0x01 | 0x200, 0x80110501u);
    refused(572, 0, 0, 0, 0x80110502u);
    /* No options word in 572 bytes, and none checked in an error dialog of 580. */
    request(580, 0, 0x80020323u, 0x200);
    show();
    call(ABORT, 0);
    frame();
    finish();
}

static void aborts(void) {
    request(708, 1, 0, 0);
    CHECK(call(INIT_START, P) == 0, "InitStart");
    CHECK(call(ABORT, 0) == 0x80110001u, "Abort during INIT");
    CHECK(call(INIT_START, P) == 0x80110001u, "InitStart while one is up");
    CHECK(call(SHUTDOWN_START, 0) == 0x80110001u, "ShutdownStart during INIT");
    while (status() == 1) frame();
    CHECK(call(ABORT, 0) == 0 && call(ABORT, 0) == 0, "Abort while visible, twice");
    CHECK(status() == 2, "still 2 until the next Update");
    CHECK(call(UPDATE, 1) == 0 && status() == 3, "3 after it");
    CHECK(call(ABORT, 0) == 0x80110001u, "a third Abort");
    CHECK(call(SHUTDOWN_START, 0) == 0, "ShutdownStart");
    CHECK(call(ABORT, 0) == 0x80110001u, "Abort during shutdown");
    call(WAIT_VBLANK, 0);
    CHECK(status() == 0 && call(ABORT, 0) == 0x80110001u, "after the reset");
    request(500, 1, 0, 0);
    CHECK(call(INIT_START, P) == 0x80110004u, "a request under 572 bytes");
}

int main(void) {
    if (psp_mem_init() != 0) return 2;
    psp_cpu_reset();
    psp_hle_init();
    CHECK(call(ABORT, 0) == 0x80110005u, "Abort before any message dialog");
    CHECK(status() == 0, "status 0 before any");
    yes_no();
    buttons();
    refusals();
    aborts();
    CHECK(psp_mem_bad_access == 0, "%llu bad accesses", (unsigned long long)psp_mem_bad_access);
    psp_mem_free();
    printf("msgdialog: %s\n", failures ? "FAILED" : "status, results, refusals, aborts and the pad as a PSP answers them");
    return failures ? 1 : 0;
}
