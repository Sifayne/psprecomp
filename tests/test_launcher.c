/* The player's launcher with a schema of its own: the pages and rows it
 * derives, the stops, the options it knows by key, the pack's New and Reset
 * presets and its title order. Includes launcher.c for its internals. It
 * draws, so it needs a font (PSPRECOMP_UI_FONT or a system one) and skips
 * (77) without one. Last Raven's host/launcher_tests.c drives a whole
 * session, with screenshots, on its own schema. */
#include <SDL2/SDL.h>
#define main launcher_entry
#include "../src/host/launcher.c"
#undef main
#include <assert.h>

enum { RESOLUTION, WINDOW_SIZE, WINDOW_MODE, DISPLAY, CAP, INPUT, GAMEPAD, KEYS, RENDER, MOVIE, WINDOW,
       OPTION_COUNT };
#define CHOICE(k,pg,d_,ch_,...) {.key=k,.env="PSPRECOMP_TEST_" k,.label=k,.page=pg,.help="",\
    .type=PSP_OPTION_CHOICE,.dflt=d_,.choices=ch_,.labels=ch_, __VA_ARGS__}
static const psp_option_def options[OPTION_COUNT] = {
    [RESOLUTION]=CHOICE("RESOLUTION","Picture","psp","psp|window"),
    [WINDOW_SIZE]={.key="WINDOW_SIZE",.env="PSPRECOMP_TEST_WINDOW_SIZE",.label="Window size",.page="Picture",
                   .help="",.type=PSP_OPTION_SIZE,.dflt="960x544",.min=1,.max=16384},
    [WINDOW_MODE]=CHOICE("WINDOW_MODE","Picture","windowed","windowed|borderless"),
    [DISPLAY]={.key="DISPLAY",.env="PSPRECOMP_TEST_DISPLAY",.label="Display",.page="Picture",.help="",
               .type=PSP_OPTION_INTEGER,.dflt="primary",.min=1,.max=65535,.step=1,.special="primary"},
    [CAP]={.key="CAP",.env="PSPRECOMP_TEST_CAP",.label="Cap",.page="Picture",.help="",
           .type=PSP_OPTION_INTEGER,.dflt="60",.min=30,.max=1000,.step=1,.special="unlimited",
           .stops="30|60|120|unlimited"},
    [INPUT]=CHOICE("INPUT","Gameplay","classic","classic|modern"),
    [GAMEPAD]=CHOICE("GAMEPAD","Controller","auto","auto|classic|modern"),
    [KEYS]=CHOICE("KEYS","Keyboard & Mouse","classic","classic|wasd"),
    [RENDER]=CHOICE("RENDER","Advanced","auto","auto|software|gl|null"),
    [MOVIE]=CHOICE("MPEG_DECODE","Advanced","0","0|1"),
    [WINDOW]=CHOICE("WINDOW","Launch","0","0|1",.flags=PSP_OPTION_HIDDEN),
};
static const psp_preset_def presets[] = {{"Default", ""}};
const psp_settings_schema psp_title_settings = {
    .title = "Launcher test", .options = options, .count = OPTION_COUNT,
    .presets = presets, .preset_count = 1,
};
static const char *const titles[] = {"second", "first", NULL};
const psp_launcher psp_launcher_info = {
    .name = "Launcher test", .id = "launcher-test", .about = "About the launcher test.",
    .titles = titles,
    .new_preset = "WINDOW=1 RENDER=gl MPEG_DECODE=1", .reset_preset = "WINDOW=1 MPEG_DECODE=1",
};

static void press(launcher *a,SDL_Keycode sym) {
    SDL_Event e={0}; e.type=SDL_KEYDOWN; e.key.keysym.sym=sym; event(a,&e);
}
static void pad_press(launcher *a,Uint8 button) {
    SDL_Event e={0}; e.type=SDL_CONTROLLERBUTTONDOWN; e.cbutton.button=button; event(a,&e);
}
static void click(launcher *a,int x,int y) {
    SDL_Event e={0}; e.type=SDL_MOUSEBUTTONDOWN; e.button.button=SDL_BUTTON_LEFT;
    e.button.x=x; e.button.y=y; event(a,&e);
}
static void step(launcher *a,int id,int direction,const char *expect) {
    adjust(a,id,direction);
    if (strcmp(editing(a)->value[id],expect)) {
        fprintf(stderr,"%s: stepped %+d to %s, expected %s\n",options[id].key,direction,editing(a)->value[id],expect);
        assert(0);
    }
}

int main(void) {
    for (int k=0;k<OPTION_COUNT;k++) unsetenv(options[k].env);
    /* No host controller may press anything; see Last Raven's launcher
     * tests. SDL_Quit clears hints, so they come before SDL_Init. */
    assert(SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_HIDAPI,"0",SDL_HINT_OVERRIDE));
    assert(SDL_SetHintWithPriority(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT,"0x0000/0x0000",SDL_HINT_OVERRIDE));
    assert(!SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMECONTROLLER)); assert(!TTF_Init());
    launcher a={0}; a.running=1; a.movie_available=1;
    psp_presets_defaults(&a.book);

    /* Pages in the order they first appear; the hidden option's is none. */
    const char *names[MAX_PAGES]; int n=pages(names);
    assert(n==5 && !strcmp(names[0],"Picture") && !strcmp(names[1],"Gameplay") &&
           !strcmp(names[3],"Keyboard & Mouse") && !strcmp(names[4],"Advanced"));
    int ids[PSP_SETTINGS_MAX]; refresh(&a);
    assert(rows(&a,ids)==5 && ids[0]==RESOLUTION && ids[1]==WINDOW_SIZE && ids[4]==CAP);
    /* Windowed fullscreen has no window size to offer. */
    step(&a,WINDOW_MODE,1,"borderless");
    a.focus=a.selected_row=WINDOW_SIZE; refresh(&a);
    assert(rows(&a,ids)==4 && ids[1]==WINDOW_MODE && a.focus==WINDOW_MODE && a.selected_row==WINDOW_MODE);

    /* Stops: up and down the list, through the special word, wrapping; from
     * a value between two stops, the adjacent one. */
    step(&a,CAP,1,"120"); step(&a,CAP,1,"unlimited"); step(&a,CAP,1,"30");
    step(&a,CAP,-1,"unlimited"); step(&a,CAP,-1,"120");
    char error[PSP_SETTINGS_ERROR];
    assert(!psp_settings_set(editing(&a),CAP,"75",PSP_SOURCE_PRESET,error));
    step(&a,CAP,-1,"60");
    assert(!psp_settings_set(editing(&a),CAP,"75",PSP_SOURCE_PRESET,error));
    step(&a,CAP,1,"120");
    /* Null is for files and the command line. */
    step(&a,RENDER,-1,"gl"); step(&a,RENDER,1,"auto");

    /* New and Reset start from the pack's assignments; without a decoder
     * the movie is switched off, and its row can only switch it off. */
    a.movie_available=0;
    activate(&a,NEW); assert(a.modal==MODAL_NEW); press(&a,SDLK_RETURN);
    assert(a.book.selected==a.book.count-1);
    assert(editing(&a)->number[WINDOW]==1 && editing(&a)->number[RENDER]==2 && !editing(&a)->number[MOVIE]);
    a.movie_available=1;
    activate(&a,RESET); press(&a,SDLK_RETURN);
    assert(editing(&a)->number[WINDOW]==1 && !editing(&a)->number[RENDER] && editing(&a)->number[MOVIE]==1);
    a.movie_available=0; adjust(&a,MOVIE,1); assert(!editing(&a)->number[MOVIE]);
    adjust(&a,MOVIE,1); assert(!editing(&a)->number[MOVIE]);
    a.movie_available=1;

    /* The pack's titles first, in its order, then the rest by slug. */
    a.game_count=3;
    a.games[0]=(game_entry){"zeta","Zeta","boot","module",NULL};
    a.games[1]=(game_entry){"first","First","boot","module",NULL};
    a.games[2]=(game_entry){"second","Second","boot","module",NULL};
    qsort(a.games,3,sizeof a.games[0],title_order);
    assert(!strcmp(a.games[0].slug,"second") && !strcmp(a.games[1].slug,"first") && !strcmp(a.games[2].slug,"zeta"));
    a.game_count=0;
    assert(!strcmp(game_title(&a),"Launcher test"));

    /* Five tabs share the row; the screen draws, and its tabs and bumpers
     * reach every page. */
    if (fonts(&a,getenv("PSPRECOMP_UI_FONT"))) {
        puts("launcher: no usable font; skipping the drawn half");
        SDL_Quit(); return 77;
    }
    a.window=SDL_CreateWindow("Launcher test",0,0,UI_W,UI_H,SDL_WINDOW_HIDDEN);
    assert(a.window); a.renderer=SDL_CreateRenderer(a.window,-1,SDL_RENDERER_SOFTWARE); assert(a.renderer);
    SDL_RenderSetLogicalSize(a.renderer,UI_W,UI_H);
    draw(&a);
    int tab=808/5;
    click(&a,294+3*tab+20,166); assert(a.page==3);
    draw(&a); refresh(&a); assert(rows(&a,ids)==1 && ids[0]==KEYS && a.selected_row==KEYS);
    click(&a,294+4*tab+20,166); assert(a.page==4);
    pad_press(&a,SDL_CONTROLLER_BUTTON_RIGHTSHOULDER); assert(a.page==0);
    pad_press(&a,SDL_CONTROLLER_BUTTON_LEFTSHOULDER); assert(a.page==4);
    draw(&a);
    TTF_CloseFont(a.body); TTF_CloseFont(a.small); TTF_CloseFont(a.heading);
    SDL_DestroyRenderer(a.renderer); SDL_DestroyWindow(a.window); TTF_Quit(); SDL_Quit();
    puts("launcher: pages, stops, known options, presets and title order passed");
    return 0;
}
