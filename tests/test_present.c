/* Exercise the actual presentation input and shutdown path without game data.
 * Includes present.c to reach its internals. From Last Raven's
 * host/present_tests.c, with a schema and a title of its own. */
#define psp_sched_stop_all fixture_stop_all
#include "../src/host/present.c"
#include <assert.h>

/* The player options present.c reads. A title's real schema has more. */
static const psp_option_def options[] = {
    {.key="WINDOW_SIZE",.env="PSPRECOMP_WINDOW_SIZE",.label="Window size",.page="Graphics",.help="",
     .type=PSP_OPTION_SIZE,.dflt="960x544",.min=1,.max=16384},
    {.key="KEYS",.env="PSPRECOMP_KEYS",.label="Keyboard",.page="Controls",.help="",
     .type=PSP_OPTION_CHOICE,.dflt="classic",.choices="classic|wasd",.labels="Classic|WASD"},
    {.key="MOUSE",.env="PSPRECOMP_MOUSE",.label="Mouse",.page="Controls",.help="",
     .type=PSP_OPTION_CHOICE,.dflt="0",.choices="0|1",.labels="Off|On"},
};
const psp_settings_schema psp_title_settings = {
    .title = "Presentation test", .options = options, .count = 3,
};
const psp_title psp_title_info = { .name = "Presentation test" };

static _Atomic int stop_calls;
void fixture_stop_all(const char *reason) {
    assert(!strcmp(reason, "window closed"));
    atomic_fetch_add(&stop_calls, 1);
}

static void reset_chord(void) { clear_controller(); assert(!quit_chord_due(100000)); }

static void shortcuts(void) {
    reset_chord();
    quit_chord_button(SDL_CONTROLLER_BUTTON_BACK, 1, 0);
    assert(!quit_chord_due(5000));
    quit_chord_button(SDL_CONTROLLER_BUTTON_START, 1, 5000);
    assert(!quit_chord_due(6999)); assert(quit_chord_due(7000));
    /* Repeated down events do not restart the hold. */
    quit_chord_button(SDL_CONTROLLER_BUTTON_START, 1, 6000);
    assert(quit_chord_due(7000));
    /* Releasing either half cancels; time from earlier taps never accumulates. */
    quit_chord_button(SDL_CONTROLLER_BUTTON_BACK, 0, 7001);
    assert(!quit_chord_due(10000));
    quit_chord_button(SDL_CONTROLLER_BUTTON_BACK, 1, 10000);
    assert(!quit_chord_due(11999)); assert(quit_chord_due(12000));
    quit_chord_button(SDL_CONTROLLER_BUTTON_START, 0, 12001);
    assert(!quit_chord_due(20000));
    reset_chord();  /* Focus loss or controller disconnection uses this reset. */
    quit_chord_button(SDL_CONTROLLER_BUTTON_START, 1, 0);
    quit_chord_button(SDL_CONTROLLER_BUTTON_A, 1, 1);
    assert(!quit_chord_due(5000));
    quit_chord_button(SDL_CONTROLLER_BUTTON_BACK, 1, 6000);
    assert(!quit_chord_due(7999)); assert(quit_chord_due(8000));
    reset_chord();
    assert(!quit_chord_due(20000));
    /* Physical shortcut works with both mappings; game/replay pad words do
     * not touch its state. Start and Select individually retain their bits. */
    for (int modern=0;modern<2;modern++) {
        g_gamepad_modern=modern; clear_controller();
        set_button(SDL_CONTROLLER_BUTTON_START,1);
        assert(atomic_load(&g_controller_buttons)==0x8 && !g_quit_chord.timing);
        set_button(SDL_CONTROLLER_BUTTON_BACK,1);
        assert(atomic_load(&g_controller_buttons)==0x9 && g_quit_chord.timing);
        clear_controller(); assert(!g_quit_chord.timing);
        psp_ctrl_set(0x9,128,128); assert(!g_quit_chord.timing);
    }
    SDL_Event event; memset(&event,0,sizeof event); event.type=SDL_KEYDOWN; event.key.keysym.sym=SDLK_q;
    assert(!quit_key_event(&event));
    event.key.keysym.mod=KMOD_CTRL; assert(!quit_key_event(&event));
    event.key.keysym.mod=KMOD_SHIFT; assert(!quit_key_event(&event));
    event.key.keysym.mod=KMOD_LCTRL|KMOD_RSHIFT; assert(quit_key_event(&event));
    event.key.repeat=1; assert(!quit_key_event(&event)); event.key.repeat=0;
    event.type=SDL_KEYUP; assert(!quit_key_event(&event)); event.type=SDL_KEYDOWN;
    event.key.keysym.sym=SDLK_ESCAPE; assert(!quit_key_event(&event));
    close_game_window(); assert(atomic_load(&g_quit) && atomic_load(&stop_calls)==1);
    puts("presentation: two-second hold, release/reset, mappings, replay isolation and graceful quit passed");
}

static void virtual_button(SDL_Joystick *joystick,int button,int down) {
    assert(SDL_JoystickSetVirtualButton(joystick,button,(Uint8)down)==0);
}

static void event_loop(const char *mode) {
    psp_settings settings; char error[PSP_SETTINGS_ERROR]; psp_settings_defaults(&settings);
    assert(!psp_settings_resolve(&settings,error)); psp_settings_use(&settings);
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");
    /* Only the virtual pad below: a physical one (hidraw or evdev) would own
     * the lane before it. Override priority, so an inherited environment
     * cannot undo it. */
    SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_HIDAPI,"0",SDL_HINT_OVERRIDE);
    SDL_SetHintWithPriority(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT,"0x0000/0x0000",
                            SDL_HINT_OVERRIDE);
    assert(present_start()==0);
    if (!strcmp(mode,"keyboard")) {
        SDL_Event e; memset(&e,0,sizeof e); e.type=SDL_KEYDOWN; e.key.state=SDL_PRESSED;
        e.key.keysym.sym=SDLK_q; e.key.keysym.scancode=SDL_SCANCODE_Q;
        e.key.keysym.mod=KMOD_LCTRL|KMOD_LSHIFT;
        assert(SDL_PushEvent(&e)==1);
    } else {
        /* The package builder has no physical controllers. An SDL virtual
         * gamepad goes through real discovery, mapping and button events. */
        int index=SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
            SDL_CONTROLLER_AXIS_MAX,SDL_CONTROLLER_BUTTON_MAX,0);
        assert(index>=0 && SDL_IsGameController(index));
        SDL_Joystick *joy=SDL_JoystickOpen(index); assert(joy);
        virtual_button(joy,SDL_CONTROLLER_BUTTON_BACK,1);
        virtual_button(joy,SDL_CONTROLLER_BUTTON_START,1);
        Uint64 deadline=SDL_GetTicks64()+1000;
        while ((atomic_load(&g_controller_buttons)&9)!=9 && SDL_GetTicks64()<deadline) SDL_Delay(5);
        assert((atomic_load(&g_controller_buttons)&9)==9);
        SDL_Delay(100); virtual_button(joy,SDL_CONTROLLER_BUTTON_START,0);
        SDL_Delay(100); assert(!atomic_load(&stop_calls));
        virtual_button(joy,SDL_CONTROLLER_BUTTON_START,1);
        SDL_Delay(100); assert(!atomic_load(&stop_calls));
        /* Device removal cancels an in-progress chord. */
        SDL_JoystickClose(joy); assert(!SDL_JoystickDetachVirtual(index));
        SDL_Delay(2100); assert(!atomic_load(&stop_calls));
        index=SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
            SDL_CONTROLLER_AXIS_MAX,SDL_CONTROLLER_BUTTON_MAX,0);
        assert(index>=0); joy=SDL_JoystickOpen(index); assert(joy);
        virtual_button(joy,SDL_CONTROLLER_BUTTON_START,1);
        virtual_button(joy,SDL_CONTROLLER_BUTTON_BACK,1);
        SDL_Delay(1500); assert(!atomic_load(&stop_calls));
    }
    Uint64 deadline=SDL_GetTicks64()+3000;
    while (!atomic_load(&stop_calls) && SDL_GetTicks64()<deadline) SDL_Delay(5);
    assert(atomic_load(&stop_calls)==1 && atomic_load(&g_quit));
    assert(!pthread_join(g_thread,NULL));
    printf("presentation: %s shortcut closed the real SDL event loop cleanly\n",mode);
}

int main(int argc,char **argv) {
    if (argc==1) shortcuts();
    else { assert(argc==2 && (!strcmp(argv[1],"keyboard") || !strcmp(argv[1],"controller"))); event_loop(argv[1]); }
    return 0;
}
