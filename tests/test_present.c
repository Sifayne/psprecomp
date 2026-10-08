/* Exercise the actual presentation input and shutdown path without game data.
 * Includes present.c and input.c to reach their internals, with the
 * runtime's pad caught here. From Last Raven's host/present_tests.c, with a
 * schema and a title of its own.
 *
 *   test_present                  the quit shortcuts alone
 *   test_present keyboard         the real SDL loop, closed by Ctrl+Shift+Q
 *   test_present controller       every button of the PSP layout on a virtual
 *                                 controller, View + Menu held, hot-plug
 *   test_present modern           every button and trigger of the modern layout
 *   test_present rebind           a preset's bind.pad.* keys, through the loop
 *   test_present last             ACTIVE_PAD=last: the pad pressed last has the lane */
#define psp_sched_stop_all fixture_stop_all
#define psp_ctrl_set fixture_ctrl_set
#define psp_ctrl_set_look fixture_ctrl_set_look
#include "../src/host/present.c"
#include "../src/host/input.c"
#include <assert.h>

static _Atomic uint32_t published;
void fixture_ctrl_set(uint32_t buttons, uint8_t ax, uint8_t ay) { (void)ax; (void)ay; atomic_store(&published, buttons); }
void fixture_ctrl_set_look(uint8_t rx, uint8_t ry) { (void)rx; (void)ry; }

/* The player options present.c reads. A title's real schema has more. */
static int resolve_gamepad(psp_settings *s, char *error) {
    (void)error;
    s->gamepad = s->input = s->number[3] != 0;
    return 0;
}
static const psp_option_def options[] = {
    {.key="WINDOW_SIZE",.env="PSPRECOMP_WINDOW_SIZE",.label="Window size",.page="Graphics",.help="",
     .type=PSP_OPTION_SIZE,.dflt="960x544",.min=1,.max=16384},
    {.key="KEYS",.env="PSPRECOMP_KEYS",.label="Keyboard",.page="Controls",.help="",
     .type=PSP_OPTION_CHOICE,.dflt="classic",.choices="classic|wasd",.labels="Classic|WASD"},
    {.key="MOUSE",.env="PSPRECOMP_MOUSE",.label="Mouse",.page="Controls",.help="",
     .type=PSP_OPTION_CHOICE,.dflt="0",.choices="0|1",.labels="Off|On"},
    {.key="GAMEPAD",.env="PSPRECOMP_GAMEPAD",.label="Controller",.page="Controls",.help="",
     .type=PSP_OPTION_CHOICE,.dflt="classic",.choices="classic|modern",.labels="Classic|Modern"},
    {.key="ACTIVE_PAD",.env="PSPRECOMP_ACTIVE_PAD",.label="Active controller",.page="Controls",.help="",
     .type=PSP_OPTION_CHOICE,.dflt="first",.choices="first|last",.labels="First connected|Last used"},
};
const psp_settings_schema psp_title_settings = {
    .title = "Presentation test", .options = options, .count = 5, .resolve = resolve_gamepad,
};
/* Every carrier named, as a title with Modern controls names them. */
static const psp_title_action ACTIONS[10] = {
    { "a", "A", PSP_PAD_A, PSP_ACTION_KEEP }, { "b", "B", PSP_PAD_B, PSP_ACTION_KEEP },
    { "x", "X", PSP_PAD_X, PSP_ACTION_KEEP }, { "y", "Y", PSP_PAD_Y, PSP_ACTION_KEEP },
    { "lb", "LB", PSP_PAD_LB, PSP_ACTION_KEEP }, { "rb", "RB", PSP_PAD_RB, PSP_ACTION_KEEP },
    { "lt", "LT", PSP_PAD_LT, PSP_ACTION_KEEP }, { "rt", "RT", PSP_PAD_RT, PSP_ACTION_KEEP },
    { "l3", "L3", PSP_PAD_L3, PSP_ACTION_KEEP }, { "r3", "R3", PSP_PAD_R3, PSP_ACTION_KEEP },
};
static void title_input(const psp_settings *s, psp_title_input *out) {
    (void)s;
    out->actions = ACTIONS; out->action_count = 10;
}
const psp_title psp_title_info = {
    .name = "Presentation test", .capabilities = PSP_TITLE_MODERN_CONTROLS, .input = title_input,
};

static _Atomic int stop_calls;
void fixture_stop_all(const char *reason) {
    assert(!strcmp(reason, "window closed"));
    atomic_fetch_add(&stop_calls, 1);
}

static SDL_Event button_event(int button, int is_down) {
    SDL_Event e; memset(&e, 0, sizeof e);
    e.type = is_down ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP;
    e.cbutton.which = g_controller_id; e.cbutton.button = (Uint8)button; e.cbutton.state = (Uint8)is_down;
    return e;
}

static void shortcuts(void) {
    /* The menu chord: the press that completes View + Menu, once a pair;
     * either half alone is the game's. */
    clear_controller();
    assert(!chord_button(SDL_CONTROLLER_BUTTON_BACK, 1));
    assert(chord_button(SDL_CONTROLLER_BUTTON_START, 1));
    assert(!chord_button(SDL_CONTROLLER_BUTTON_START, 1));     /* a repeat is not another */
    assert(!chord_button(SDL_CONTROLLER_BUTTON_BACK, 0));
    assert(chord_button(SDL_CONTROLLER_BUTTON_BACK, 1));       /* completed again */
    assert(!chord_button(SDL_CONTROLLER_BUTTON_A, 1));
    clear_controller();      /* focus loss or a disconnection forgets the pair */
    assert(!chord_button(SDL_CONTROLLER_BUTTON_START, 1));
    assert(chord_button(SDL_CONTROLLER_BUTTON_BACK, 1));
    clear_controller();
    /* With both mappings, through the input layer: Start and Select keep
     * their bits, the pair is the menu, and the game's pad words never
     * touch it. */
    g_controller_id = 1;
    for (int modern=0;modern<2;modern++) {
        g_gamepad_modern=modern; build_defaults(); clear_controller();
        SDL_Event e = button_event(SDL_CONTROLLER_BUTTON_START, 1);
        assert(input_event(&e)==INPUT_ACTION_NONE && atomic_load(&published)==0x8);
        e = button_event(SDL_CONTROLLER_BUTTON_BACK, 1);
        assert(input_event(&e)==INPUT_ACTION_MENU && atomic_load(&published)==0x9);
        assert(input_event(&e)==INPUT_ACTION_NONE);
        clear_controller(); assert(!g_chord.buttons);
        fixture_ctrl_set(0x9,128,128); assert(!g_chord.buttons);
    }
    /* Escape is the menu's key: a keycode, which SDL places on the keyboard
     * once video is up, as it is in a game. */
    assert(SDL_InitSubSystem(SDL_INIT_VIDEO)==0);
    find_keys();
    SDL_Event key; memset(&key,0,sizeof key); key.type=SDL_KEYDOWN; key.key.state=SDL_PRESSED;
    key.key.keysym.sym=SDLK_ESCAPE; key.key.keysym.scancode=SDL_SCANCODE_ESCAPE;
    assert(input_event(&key)==INPUT_ACTION_MENU);
    key.type=SDL_KEYUP; key.key.state=SDL_RELEASED; assert(input_event(&key)==INPUT_ACTION_NONE);
    g_controller_id = -1;
    SDL_Event event; memset(&event,0,sizeof event); event.type=SDL_KEYDOWN; event.key.keysym.sym=SDLK_q;
    assert(!input_quit_event(&event));
    event.key.keysym.mod=KMOD_CTRL; assert(!input_quit_event(&event));
    event.key.keysym.mod=KMOD_SHIFT; assert(!input_quit_event(&event));
    event.key.keysym.mod=KMOD_LCTRL|KMOD_RSHIFT; assert(input_quit_event(&event));
    event.key.repeat=1; assert(!input_quit_event(&event)); event.key.repeat=0;
    event.type=SDL_KEYUP; assert(!input_quit_event(&event)); event.type=SDL_KEYDOWN;
    event.key.keysym.sym=SDLK_ESCAPE; assert(!input_quit_event(&event));
    close_game_window(); assert(atomic_load(&g_quit) && atomic_load(&stop_calls)==1);
    puts("presentation: the menu chord, Escape, mappings, replay isolation and graceful quit passed");
}

static void virtual_button(SDL_Joystick *joystick,int button,int down) {
    assert(SDL_JoystickSetVirtualButton(joystick,button,(Uint8)down)==0);
}
static void virtual_axis(SDL_Joystick *joystick,int axis,Sint16 value) {
    assert(SDL_JoystickSetVirtualAxis(joystick,axis,value)==0);
}

/* What the SDL thread published, once it has: a press on a virtual pad goes
 * through SDL's event queue and that thread before it shows. */
static int published_becomes(uint32_t want) {
    Uint64 deadline=SDL_GetTicks64()+1000;
    while (atomic_load(&published)!=want && SDL_GetTicks64()<deadline) SDL_Delay(2);
    return atomic_load(&published)==want;
}

static SDL_Joystick *attach(int *index) {
    *index=SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
        SDL_CONTROLLER_AXIS_MAX,SDL_CONTROLLER_BUTTON_MAX,0);
    assert(*index>=0 && SDL_IsGameController(*index));
    SDL_Joystick *joy=SDL_JoystickOpen(*index); assert(joy);
    /* A virtual trigger rests at its axis's centre, which a controller reads
     * as half pulled: put both at rest. */
    virtual_axis(joy,SDL_CONTROLLER_AXIS_TRIGGERLEFT,-32768);
    virtual_axis(joy,SDL_CONTROLLER_AXIS_TRIGGERRIGHT,-32768);
    return joy;
}

/* Every button of a layout, pressed and let go on a virtual pad. */
static void every_button(SDL_Joystick *joy,const psp_pad_bind *map,size_t n) {
    for (size_t i=0;i<n;i++) {
        virtual_button(joy,map[i].button,1);
        if (!published_becomes(map[i].bit)) {
            printf("FAIL button %d: published %06X, wanted %06X\n",map[i].button,atomic_load(&published),map[i].bit);
            assert(0);
        }
        virtual_button(joy,map[i].button,0);
        assert(published_becomes(0));
    }
}

/* Ctrl+Shift+Q, through SDL's queue as a key press would come. */
static void quit_by_keyboard(void) {
    SDL_Event e; memset(&e,0,sizeof e); e.type=SDL_KEYDOWN; e.key.state=SDL_PRESSED;
    e.key.keysym.sym=SDLK_q; e.key.keysym.scancode=SDL_SCANCODE_Q;
    e.key.keysym.mod=KMOD_LCTRL|KMOD_LSHIFT;
    assert(SDL_PushEvent(&e)==1);
}

static void event_loop(const char *mode) {
    if (!strcmp(mode,"modern")) setenv("PSPRECOMP_GAMEPAD","modern",1);
    if (!strcmp(mode,"last")) setenv("PSPRECOMP_ACTIVE_PAD","last",1);
    psp_settings settings; char error[PSP_SETTINGS_ERROR]; psp_settings_defaults(&settings);
    assert(!psp_settings_env(&settings,error));
    if (!strcmp(mode,"rebind")) {
        assert(!psp_settings_bind(&settings,"pad.cross","y",error));
        assert(!psp_settings_bind(&settings,"pad.circle","",error));
        assert(!psp_settings_bind(&settings,"pad.quit","guide",error));
    }
    assert(!psp_settings_resolve(&settings,error)); psp_settings_use(&settings);
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");
    /* Only the virtual pad below: a physical one (hidraw or evdev) would own
     * the lane before it. Override priority, so an inherited environment
     * cannot undo it. */
    SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_HIDAPI,"0",SDL_HINT_OVERRIDE);
    SDL_SetHintWithPriority(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT,"0x0000/0x0000",
                            SDL_HINT_OVERRIDE);
    assert(present_start()==0);
    if (!strcmp(mode,"keyboard")) quit_by_keyboard();
    else if (!strcmp(mode,"controller")) {
        /* The package builder has no physical controllers. An SDL virtual
         * gamepad goes through real discovery, mapping and button events. */
        int index; SDL_Joystick *joy=attach(&index);
        assert(published_becomes(0));
        every_button(joy,PAD_CLASSIC,sizeof PAD_CLASSIC/sizeof *PAD_CLASSIC);
        /* The PSP layout has no triggers or stick clicks. */
        virtual_axis(joy,SDL_CONTROLLER_AXIS_TRIGGERRIGHT,32767);
        virtual_button(joy,SDL_CONTROLLER_BUTTON_LEFTSTICK,1);
        SDL_Delay(100); assert(atomic_load(&published)==0);
        virtual_axis(joy,SDL_CONTROLLER_AXIS_TRIGGERRIGHT,-32768);
        virtual_button(joy,SDL_CONTROLLER_BUTTON_LEFTSTICK,0);
        /* View + Menu is the menu's (this program has none) and no longer
         * quits however long it is held; both still reach the game. */
        virtual_button(joy,SDL_CONTROLLER_BUTTON_BACK,1);
        virtual_button(joy,SDL_CONTROLLER_BUTTON_START,1);
        assert(published_becomes(9));
        SDL_Delay(2100); assert(!atomic_load(&stop_calls));
        /* Device removal clears the pad. */
        SDL_JoystickClose(joy); assert(!SDL_JoystickDetachVirtual(index));
        assert(published_becomes(0));
        /* A pad plugged in again takes the lane. */
        joy=attach(&index);
        virtual_button(joy,SDL_CONTROLLER_BUTTON_START,1);
        virtual_button(joy,SDL_CONTROLLER_BUTTON_BACK,1);
        assert(published_becomes(9));
        quit_by_keyboard();
    } else if (!strcmp(mode,"modern")) {
        int index; SDL_Joystick *joy=attach(&index);
        assert(published_becomes(0));
        every_button(joy,PAD_MODERN,sizeof PAD_MODERN/sizeof *PAD_MODERN);
        /* Triggers press from halfway and let go below a quarter. */
        virtual_axis(joy,SDL_CONTROLLER_AXIS_TRIGGERLEFT,32767);
        assert(published_becomes(PSP_PAD_LT));
        virtual_axis(joy,SDL_CONTROLLER_AXIS_TRIGGERLEFT,-8768);        /* about 12000 */
        SDL_Delay(100); assert(atomic_load(&published)==PSP_PAD_LT);
        virtual_axis(joy,SDL_CONTROLLER_AXIS_TRIGGERLEFT,-32768);
        assert(published_becomes(0));
        virtual_axis(joy,SDL_CONTROLLER_AXIS_TRIGGERRIGHT,32767);
        assert(published_becomes(PSP_PAD_RT));
        virtual_axis(joy,SDL_CONTROLLER_AXIS_TRIGGERRIGHT,-32768);
        assert(published_becomes(0));
        quit_by_keyboard();
    } else if (!strcmp(mode,"rebind")) {
        int index; SDL_Joystick *joy=attach(&index);
        assert(published_becomes(0));
        virtual_button(joy,SDL_CONTROLLER_BUTTON_Y,1);            /* triangle, and now cross */
        assert(published_becomes(0x5000));
        virtual_button(joy,SDL_CONTROLLER_BUTTON_Y,0); assert(published_becomes(0));
        virtual_button(joy,SDL_CONTROLLER_BUTTON_A,1);            /* cross no longer */
        virtual_button(joy,SDL_CONTROLLER_BUTTON_B,1);            /* circle unbound */
        virtual_button(joy,SDL_CONTROLLER_BUTTON_X,1);            /* square, as ever */
        assert(published_becomes(0x8000));
        virtual_button(joy,SDL_CONTROLLER_BUTTON_X,0); assert(published_becomes(0));
        virtual_button(joy,SDL_CONTROLLER_BUTTON_GUIDE,1);        /* quit, bound */
    } else {
        int first_index, second_index;
        SDL_Joystick *first=attach(&first_index), *second=attach(&second_index);
        assert(published_becomes(0));
        virtual_button(first,SDL_CONTROLLER_BUTTON_A,1); assert(published_becomes(0x4000));
        virtual_button(first,SDL_CONTROLLER_BUTTON_A,0); assert(published_becomes(0));
        /* The second pad's press takes the lane, and counts. */
        virtual_button(second,SDL_CONTROLLER_BUTTON_X,1); assert(published_becomes(0x8000));
        /* The first is ignored now, until it is pressed: its release of X... */
        virtual_button(first,SDL_CONTROLLER_BUTTON_Y,1);
        assert(published_becomes(0x1000));                         /* ...its press takes it back */
        virtual_button(second,SDL_CONTROLLER_BUTTON_X,0);
        SDL_Delay(100); assert(atomic_load(&published)==0x1000);
        virtual_button(first,SDL_CONTROLLER_BUTTON_Y,0); assert(published_becomes(0));
        /* A resting stick's drift never takes the lane; a push past halfway does. */
        virtual_axis(second,SDL_CONTROLLER_AXIS_LEFTX,3000);
        SDL_Delay(100); assert(input_pad_id()!=SDL_JoystickInstanceID(second));
        virtual_axis(second,SDL_CONTROLLER_AXIS_LEFTX,30000);
        Uint64 deadline=SDL_GetTicks64()+1000;
        while (input_pad_id()!=SDL_JoystickInstanceID(second) && SDL_GetTicks64()<deadline) SDL_Delay(2);
        assert(input_pad_id()==SDL_JoystickInstanceID(second));
        virtual_axis(second,SDL_CONTROLLER_AXIS_LEFTX,0);
        /* Unplugging the active pad hands the lane to the other. */
        SDL_JoystickClose(second); assert(!SDL_JoystickDetachVirtual(second_index));
        deadline=SDL_GetTicks64()+1000;
        while (input_pad_id()!=SDL_JoystickInstanceID(first) && SDL_GetTicks64()<deadline) SDL_Delay(2);
        assert(input_pad_id()==SDL_JoystickInstanceID(first));
        quit_by_keyboard();
    }
    Uint64 deadline=SDL_GetTicks64()+3000;
    while (!atomic_load(&stop_calls) && SDL_GetTicks64()<deadline) SDL_Delay(5);
    assert(atomic_load(&stop_calls)==1 && atomic_load(&g_quit));
    assert(!pthread_join(g_thread,NULL));
    printf("presentation: %s: the real SDL event loop published every press and closed cleanly\n",mode);
}

int main(int argc,char **argv) {
    if (argc==1) shortcuts();
    else {
        assert(argc==2);
        static const char *const MODES[]={"keyboard","controller","modern","rebind","last"};
        int known=0;
        for (size_t i=0;i<sizeof MODES/sizeof *MODES;i++) known|=!strcmp(argv[1],MODES[i]);
        assert(known);
        event_loop(argv[1]);
    }
    return 0;
}
