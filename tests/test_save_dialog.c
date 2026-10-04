/* The savedata dialog (src/host/save_dialog.c), built only with
 * PSPRECOMP_HOST: a dialog with no usable font stays out and the runtime
 * cancels the request rather than saving unconfirmed, and a virtual game
 * controller drives a save through it. Ported from Last Raven's
 * host/save_dialog_tests.c, without the window and GL halves that belong to
 * a game's host.
 *
 * Runs under SDL_VIDEODRIVER=dummy on a memory-stick directory it removes.
 * The controller half draws text, so it needs a system font; without one the
 * test skips (77) after the no-font half. */

#define _XOPEN_SOURCE 700   /* nftw, to remove the memory-stick directory */

#include "psprecomp/host/save_dialog.h"
#include "psprecomp/hle.h"

#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

enum {
    INIT_START = 0x50C4CD57u, GET_STATUS = 0x8874DBE0u, UPDATE = 0xD4B95FFBu,
    SHUTDOWN_START = 0x9790B33Cu, WAIT_VBLANK_START = 0x984C27E7u,
};
static const uint32_t param = 0x08810000u, names = 0x08811000u, data = 0x08812000u;
static psp_savedata_view view;
static char root[] = "/tmp/psprecomp-save-dialog-XXXXXX";

static uint32_t call(uint32_t nid, uint32_t arg) {
    psp_cpu.r[PSP_REG_A0] = arg;
    psp_hle_call(nid);
    return psp_cpu.r[PSP_REG_V0];
}
static void string(uint32_t addr, const char *s) {
    do { psp_write8(addr++, (unsigned char)*s); } while (*s++);
}

/* A SAVE (mode 5) or LOAD (4) list request over six slots. */
static void request(int mode, int circle) {
    memset(psp_mem_ptr(param, 1536), 0, 1536);
    psp_write32(param, 1536);
    psp_write32(param + 8, circle ? 0 : 1);
    psp_write32(param + 48, (uint32_t)mode);
    string(param + 60, "UITEST002");
    string(param + 76, "SLOT00");
    string(param + 100, "DATA.BIN");
    psp_write32(param + 96, names);
    for (int i = 0; i < 6; i++) {
        char n[20];
        snprintf(n, sizeof n, "SLOT%02d", i);
        string(names + (uint32_t)i * 20, n);
    }
    psp_write8(names + 120, 0);
    string(param + 128, "Savedata fixture");
    string(param + 256, "Save Data 01");
    string(param + 384, "Player: Test player\nPlay time: 12:34:56");
    string(data, "independent progress");
    psp_write32(param + 116, data);
    /* A game key: fw 6.60 refuses a SAVE-family request with the zero key
     * (tests/test_savedata.c, saveprobe step 1). */
    for (unsigned i = 0; i < 16; i++) psp_write8(param + 1500 + i, (uint8_t)i);
    psp_write32(param + 120, 128);
    psp_write32(param + 124, 21);
    CHECK(call(INIT_START, param) == 0, "InitStart accepts the request");
    CHECK(call(GET_STATUS, 0) == 1 && call(GET_STATUS, 0) == 2, "the dialog comes up");
    psp_savedata_snapshot(&view);
}
static void tick(void) {
    CHECK(call(UPDATE, 1) == 0, "Update");
    psp_savedata_snapshot(&view);
    SDL_Delay(25);
}
/* Polled back to back after ShutdownStart, fw 6.60 reads 4 until the caller
 * waits; a vblank wait lets the shutdown finish (saveprobe steps 77-78). */
static void finish(void) {
    CHECK(call(GET_STATUS, 0) == 3, "the dialog has finished");
    CHECK(call(SHUTDOWN_START, 0) == 0, "ShutdownStart");
    CHECK(call(GET_STATUS, 0) == 4 && call(GET_STATUS, 0) == 4, "shutting down until a wait");
    call(WAIT_VBLANK_START, 0);
    CHECK(call(GET_STATUS, 0) == 0, "and gone after it");
}
static int slot_written(const char *slot) {
    char path[512];
    snprintf(path, sizeof path, "%s/ms/PSP/SAVEDATA/UITEST002%s/DATA.BIN", root, slot);
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

/* No usable font: the dialog reports why and stays out, and the runtime then
 * cancels interactive requests instead of saving without confirmation. */
static void nofont(void) {
    setenv("PSPRECOMP_UI_FONT", "/nonexistent/no-such-font.ttf", 1);
    CHECK(!SDL_Init(SDL_INIT_VIDEO), "SDL video under the dummy driver");
    CHECK(save_dialog_init() != 0 && save_dialog_error(), "a missing font is refused, with a reason");
    request(5, 0);
    for (int i = 0; i < 4; i++) tick();
    CHECK(!view.active && psp_read32(param + 28) == 1, "the request is cancelled");
    finish();
    CHECK(!slot_written("SLOT00"), "and nothing is saved");
    save_dialog_shutdown();
    SDL_Quit();
    unsetenv("PSPRECOMP_UI_FONT");
}

/* A virtual gamepad: a confirm held from before the dialog opened cannot
 * save, the stick moves and repeats on the dialog's own clock, input is
 * ignored while the window is unfocused, and A then saves. Answers 77 when no
 * system font is found. */
static int controller(void) {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER)) {
        CHECK(0, "SDL video and game controller init: %s", SDL_GetError());
        return 0;
    }
    SDL_Window *win = SDL_CreateWindow("Savedata input fixture", 0, 0, 960, 544, 0);
    CHECK(win != NULL, "a window under the dummy driver");
    if (save_dialog_init()) {
        printf("skip: %s\n", save_dialog_error() ? save_dialog_error() : "no dialog");
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 77;
    }
    const int index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
                                                SDL_CONTROLLER_AXIS_MAX,
                                                SDL_CONTROLLER_BUTTON_MAX, 0);
    SDL_GameController *pad = index >= 0 ? SDL_GameControllerOpen(index) : NULL;
    CHECK(pad != NULL, "a virtual game controller");
    if (!pad) { save_dialog_shutdown(); SDL_DestroyWindow(win); SDL_Quit(); return 0; }
    SDL_PumpEvents();
    SDL_Joystick *joy = SDL_GameControllerGetJoystick(pad);
    const SDL_JoystickID id = SDL_JoystickInstanceID(joy);
    SDL_JoystickSetVirtualAxis(joy, SDL_CONTROLLER_AXIS_TRIGGERLEFT, -32768);
    SDL_JoystickSetVirtualAxis(joy, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, -32768);
    SDL_JoystickSetVirtualButton(joy, SDL_CONTROLLER_BUTTON_A, 1);
    SDL_JoystickUpdate();

    request(5, 0);
    save_dialog_update(pad);
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_CONTROLLERBUTTONDOWN;
    e.cbutton.which = id;
    e.cbutton.button = SDL_CONTROLLER_BUTTON_A;
    CHECK(save_dialog_event(&e, id), "the dialog takes the controller's events");
    tick();
    CHECK(view.stage == PSP_SAVEDATA_LIST, "a confirm held from before cannot save");

    SDL_JoystickSetVirtualButton(joy, SDL_CONTROLLER_BUTTON_A, 0);
    SDL_JoystickUpdate();
    save_dialog_update(pad);
    SDL_JoystickSetVirtualAxis(joy, SDL_CONTROLLER_AXIS_LEFTY, 24000);
    SDL_JoystickUpdate();
    save_dialog_update(pad);
    tick();
    CHECK(view.selected == 1, "the stick moves the selection");
    /* The held stick repeats on the dialog's own clock (350 ms, then 110). */
    for (int i = 0; i < 40 && view.selected != 2; i++) {
        SDL_Delay(25);
        save_dialog_update(pad);
        tick();
    }
    CHECK(view.selected == 2, "and repeats while held");
    SDL_JoystickSetVirtualAxis(joy, SDL_CONTROLLER_AXIS_LEFTY, 0);
    SDL_JoystickUpdate();
    save_dialog_update(pad);

    e.type = SDL_WINDOWEVENT;
    e.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
    save_dialog_event(&e, id);
    e.type = SDL_CONTROLLERBUTTONDOWN;
    e.cbutton.which = id;
    e.cbutton.button = SDL_CONTROLLER_BUTTON_A;
    save_dialog_event(&e, id);
    tick();
    CHECK(view.stage == PSP_SAVEDATA_LIST, "input is ignored without focus");
    e.type = SDL_WINDOWEVENT;
    e.window.event = SDL_WINDOWEVENT_FOCUS_GAINED;
    save_dialog_event(&e, id);
    save_dialog_update(pad);
    e.type = SDL_CONTROLLERBUTTONDOWN;
    e.cbutton.which = id;
    e.cbutton.button = SDL_CONTROLLER_BUTTON_A;
    save_dialog_event(&e, id);
    tick();
    CHECK(view.stage == PSP_SAVEDATA_DONE && view.result == 0, "A saves");
    save_dialog_update(pad);

    static uint32_t pixels[SAVE_DIALOG_W * SAVE_DIALOG_H];
    uint64_t revision = 0;
    CHECK(save_dialog_copy_pixels(pixels, &revision) && revision != 0, "the overlay is drawn");
    int lit = 0;
    for (size_t i = 0; i < sizeof pixels / sizeof pixels[0] && !lit; i++) lit = pixels[i] != 0;
    CHECK(lit, "and is not blank");

    save_dialog_event(&e, id);
    tick();
    finish();
    CHECK(slot_written("SLOT02"), "the save reached the selected slot on the memory stick");
    save_dialog_update(pad);
    SDL_GameControllerClose(pad);
    SDL_JoystickDetachVirtual(index);
    save_dialog_shutdown();
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

/* The machine's own pads stay out: SDL keeps HIDAPI devices closed and passes
 * only VID/PID 0, the ID SDL_JoystickAttachVirtual gives the virtual pad.
 * Override priority, because a same-named environment variable would win
 * over SDL_SetHint. */
static void ignore_host_controllers(void) {
    SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_HIDAPI, "0", SDL_HINT_OVERRIDE);
    SDL_SetHintWithPriority(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, "0x0000/0x0000",
                            SDL_HINT_OVERRIDE);
}

static int remove_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw) {
    (void)st; (void)flag; (void)ftw;
    return remove(path);
}

int main(void) {
    setenv("SDL_VIDEODRIVER", "dummy", 0);
    if (!mkdtemp(root)) { perror("mkdtemp"); return 1; }
    ignore_host_controllers();
    if (psp_mem_init() != 0) { printf("FAIL memory init\n"); return 1; }
    psp_cpu_reset();
    psp_hle_init();
    psp_io_set_root(root);

    nofont();
    const int skip = controller();
    CHECK(psp_mem_bad_access == 0, "no bad guest accesses");
    psp_mem_free();
    nftw(root, remove_entry, 16, FTW_DEPTH | FTW_PHYS);

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    if (skip) return skip;
    printf("save dialog checks passed\n");
    return 0;
}
