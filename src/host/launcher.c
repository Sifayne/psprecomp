/* The player's launcher: the pre-launch UI every pack's app opens, from Last
 * Raven's. It owns its SDL window. No game, guest clock or GL backend runs
 * here; the child boot process receives an explicit preset on launch.
 *
 * Its pages and their rows come from the title's settings schema, the rest
 * of what is the app's own from psp_launcher_info (psprecomp/host/launcher.h).
 * A few options it knows by key, the ones the shared host consumes too:
 * RENDER, WINDOW_MODE, WINDOW_SIZE, DISPLAY and MPEG_DECODE. INPUT's page
 * carries the quit note. A schema without one of them just has nothing for
 * that rule to act on. */
#include "psprecomp/host/launcher.h"
#include "psprecomp/host/settings.h"
#include "psprecomp/hle.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <ctype.h>
#include <errno.h>
#include <dirent.h>
#include <signal.h>
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

enum { UI_W=1120, UI_H=800, VISIBLE_ROWS=6, MAX_GAMES=8, MAX_FILES=1024, MAX_PAGES=8, MAX_STOPS=16 };
enum { NEW=100, DUPLICATE, RENAME, DELETE, RESET, SAVE, PLAY, CANCEL,
       MODAL_OK, MODAL_CANCEL, ABOUT, ADD_GAME, PRESET_BASE=200, PAGE_BASE=300, GAME_BASE=700,
       BROWSER_UP=900, BROWSER_HOME, BROWSER_DRIVES, BROWSER_OPEN, BROWSER_CANCEL, FILE_BASE=1000 };
enum { MODAL_NONE, MODAL_NEW, MODAL_DUPLICATE, MODAL_RENAME, MODAL_VALUE,
       MODAL_DELETE, MODAL_RESET, MODAL_CANCEL_DIRTY, MODAL_BROWSE, MODAL_PREPARING };
typedef struct { SDL_Rect rect; int id; } hit;
/* One built title, from a --game argument or the importer's library: the
 * slug and name, the boot host to exec, its module, and its disc (NULL or
 * empty for none). The strings point into argv or the library's buffer. */
typedef struct { const char *slug, *title, *boot, *module, *iso; } game_entry;
typedef struct { char name[256]; int directory; } browser_file;
typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    TTF_Font *small, *body, *heading;
    SDL_GameController *pad;
    psp_presets book;
    psp_settings effective;
    char path[4096];
    const char *boot, *module, *iso;
    game_entry games[MAX_GAMES]; int game_count, game;
    const char *library,*importer;
    char *library_buffer;
    browser_file files[MAX_FILES]; int file_count,file_selected,file_scroll;
    char browser_path[4096],browser_error[256];
    pid_t import_pid; int import_fd,import_cancelled,quit_after_import;
    char import_line[1024],import_status[1024]; size_t import_used;
    int page, scroll, selected_row, focus, dirty, running, valid, movie_available;
    int load_failed; /* A malformed file must never be overwritten by defaults. */
    char status[PSP_SETTINGS_ERROR], validation[PSP_SETTINGS_ERROR];
    hit hits[128]; int hit_count;
    int modal, edit_id, select_text;
    char edit[PSP_SETTINGS_VALUE], modal_error[PSP_SETTINGS_ERROR];
    pid_t child; int child_error_fd;
    char child_error[2048]; size_t child_error_len;
    Uint32 stick_repeat;
} launcher;

static const SDL_Color BG={16,22,29,255}, PANEL={24,32,42,255},
    ROW={30,40,52,255}, BORDER={49,64,79,255}, TEXT={230,237,240,255},
    MUTED={151,169,181,255}, ACCENT={239,184,90,255}, GOOD={110,208,191,255};

static void box(launcher *a, SDL_Rect r, SDL_Color c) {
    SDL_SetRenderDrawColor(a->renderer,c.r,c.g,c.b,c.a);
    SDL_RenderFillRect(a->renderer,&r);
}
static void outline(launcher *a, SDL_Rect r, SDL_Color c) {
    SDL_SetRenderDrawColor(a->renderer,c.r,c.g,c.b,c.a);
    SDL_RenderDrawRect(a->renderer,&r);
}
static void label(launcher *a, TTF_Font *font, int x,int y,int width,const char *s,SDL_Color c) {
    if (!s || !*s) return;
    SDL_Surface *surface=TTF_RenderUTF8_Blended_Wrapped(font,s,c,(Uint32)width);
    if (!surface) return;
    SDL_Texture *texture=SDL_CreateTextureFromSurface(a->renderer,surface);
    if (texture) {
        SDL_Rect dst={x,y,surface->w,surface->h};
        SDL_RenderCopy(a->renderer,texture,NULL,&dst); SDL_DestroyTexture(texture);
    }
    SDL_FreeSurface(surface);
}
static void add_hit(launcher *a,int id,SDL_Rect rect) {
    if (a->hit_count<(int)(sizeof a->hits/sizeof a->hits[0])) a->hits[a->hit_count++]=(hit){rect,id};
}
static void button(launcher *a,int id,int x,int y,int w,int h,const char *s,int primary) {
    SDL_Rect r={x,y,w,h}; box(a,r,primary?ACCENT:ROW);
    outline(a,r,a->focus==id?ACCENT:BORDER);
    label(a,a->body,x+12,y+(h-24)/2,w-24,s,primary?BG:TEXT);
    add_hit(a,id,r);
}
/* The schema's options, and the ones this screen knows by key (-1: none). */
static const psp_option_def *option(int id) { return &psp_settings_active_schema()->options[id]; }
static int option_count(void) { return psp_settings_active_schema()->count; }
static int known(const char *key) { return psp_settings_find(key); }
static int is_option(int id) { return id>=0 && id<option_count(); }

static psp_settings *editing(launcher *a) { return &a->book.presets[a->book.selected].settings; }
static int overridden(int id) { const char *v=getenv(option(id)->env); return v && *v; }
static int unavailable(launcher *a,int id) { return id==known("MPEG_DECODE") && !a->movie_available; }
/* A preset this build cannot honour plays without the intro movie. */
static void movie_off(launcher *a,psp_settings *s,char *error) {
    int movie=known("MPEG_DECODE");
    if (movie>=0 && !a->movie_available) psp_settings_set(s,movie,"0",PSP_SOURCE_PRESET,error);
}
static void display_label(const psp_settings *s,char *out,size_t size) {
    int id=known("DISPLAY");
    psp_option_label(s,id,out,size);
    int screen=(int)s->number[id];
    if (screen<0) return;
    if (screen>SDL_GetNumVideoDisplays()) {
        snprintf(out,size,"Display %d (unavailable)",screen);
        return;
    }
    const char *name=SDL_GetDisplayName(screen-1);
    if (name && *name) snprintf(out,size,"%d: %s",screen,name);
}
static void refresh(launcher *a) {
    a->effective=*editing(a); a->validation[0]=0;
    a->valid=!psp_settings_env(&a->effective,a->validation) &&
             !psp_settings_resolve(&a->effective,a->validation);
    int movie=known("MPEG_DECODE"), mode=known("WINDOW_MODE"), size=known("WINDOW_SIZE");
    if (a->valid && movie>=0 && !a->movie_available && a->effective.number[movie]) {
        strcpy(a->validation,"Intro decoding is unavailable in this build. Turn it off or remove its environment override.");
        a->valid=0;
    }
    if (mode>=0 && size>=0 && a->effective.number[mode]) {
        if (a->focus==size) a->focus=mode;
        if (a->selected_row==size) a->selected_row=mode;
    }
}
/* The pages: the schema's page names, in the order they first appear among
 * the options shown. */
static int shown(int id) { return option(id)->page && !(option(id)->flags & PSP_OPTION_HIDDEN); }
static int pages(const char **names) {
    int n=0;
    for (int i=0;i<option_count();i++) {
        if (!shown(i)) continue;
        int k=0; while (k<n && strcmp(names[k],option(i)->page)) k++;
        if (k==n && n<MAX_PAGES) names[n++]=option(i)->page;
    }
    return n;
}
static int page_count(void) { const char *names[MAX_PAGES]; return pages(names); }
static int rows(launcher *a,int *ids) {
    const char *names[MAX_PAGES]; int count=pages(names), n=0;
    int mode=known("WINDOW_MODE"), size=known("WINDOW_SIZE");
    if (a->page>=count) return 0;
    for (int i=0;i<option_count();i++) {
        if (!shown(i) || (i==size && mode>=0 && a->effective.number[mode])) continue;
        if (!strcmp(option(i)->page,names[a->page])) ids[n++]=i;
    }
    return n;
}
static void show_row(launcher *a,int id) {
    int ids[PSP_SETTINGS_MAX], n=rows(a,ids);
    for (int k=0;k<n;k++) if (ids[k]==id) {
        a->selected_row=id;
        if (k<a->scroll) a->scroll=k;
        if (k>=a->scroll+VISIBLE_ROWS) a->scroll=k-VISIBLE_ROWS+1;
        break;
    }
}
static void open_modal(launcher *a,int kind,const char *text) {
    a->modal=kind; a->modal_error[0]=0; a->focus=MODAL_OK;
    snprintf(a->edit,sizeof a->edit,"%s",text?text:""); a->select_text=1;
    if (kind<=MODAL_VALUE) SDL_StartTextInput();
}
static void close_modal(launcher *a) {
    SDL_StopTextInput(); a->modal=MODAL_NONE; a->focus=a->selected_row;
}

/* Titles: each --game, or each game in the importer's library. The tabs
 * switch which boot host, module and disc a launch uses, and the chosen slug
 * is saved beside the presets so the next start opens on the same game.
 * Without a title, or in a single-title pack, there are no tabs. */
static const char *game_title(const launcher *a) {
    return a->game_count?a->games[a->game].title:psp_launcher_info.name;
}
/* A pack with one title shows no tabs: the heading already names it. */
static int tab_count(const launcher *a) {
    const char *const *titles=psp_launcher_info.titles; int n=0;
    while (titles && titles[n]) n++;
    return n==1?0:a->game_count;
}
static void select_game(launcher *a,int at,int quiet) {
    if (at<0 || at>=a->game_count) return;
    const game_entry *g=&a->games[at];
    a->game=at; a->boot=g->boot && *g->boot?g->boot:NULL; a->module=g->module; a->iso=g->iso && *g->iso?g->iso:NULL;
    if (strcmp(a->book.game,g->slug)) {
        snprintf(a->book.game,sizeof a->book.game,"%s",g->slug);
        if (!quiet) { a->dirty=1; snprintf(a->status,sizeof a->status,"Game: %s",g->title); refresh(a); }
    }
    if (a->window) {
        char title[192]; snprintf(title,sizeof title,"%s - Settings",g->title);
        SDL_SetWindowTitle(a->window,title);
    }
}

#include "launcher_library.h"

static void draw(launcher *a) {
    refresh(a); a->hit_count=0;
    box(a,(SDL_Rect){0,0,UI_W,UI_H},BG);
    char kicker[PSP_SETTINGS_VALUE]; snprintf(kicker,sizeof kicker,"%s  /  PC SETTINGS",psp_settings_active_schema()->title);
    for (char *c=kicker;*c;c++) *c=(char)toupper((unsigned char)*c);
    label(a,a->small,28,22,800,kicker,ACCENT);
    char heading[PSP_SETTINGS_VALUE]; snprintf(heading,sizeof heading,"%s",game_title(a));
    for (char *c=heading;*c;c++) *c=(char)toupper((unsigned char)*c);
    label(a,a->heading,26,48,a->importer?770:930,heading,TEXT);
    if (a->importer) button(a,ADD_GAME,822,44,138,40,"Add Game",1);
    button(a,ABOUT,972,44,120,40,"About",0);
    int tabs=tab_count(a);
    if (!tabs) label(a,a->body,28,94,1000,a->boot && a->module?
        "Choose a setup. Make it yours. Launch when you're ready.":
        a->game_count && a->importer?"Choose Prepare game to get your game ready for play.":
        a->importer?"Choose Add Game to select your PSP ISO and prepare it for play.":
        "No game installed. You can set up and save your presets.",MUTED);
    /* One tab per built title, where the tagline goes otherwise. */
    int tab_w=tabs?(936-8*(tabs-1))/tabs:0;
    if (tab_w>300) tab_w=300;
    for (int k=0;k<tabs;k++) {
        SDL_Rect r={28+k*(tab_w+8),90,tab_w,40}; box(a,r,ROW);
        outline(a,r,a->focus==GAME_BASE+k?ACCENT:BORDER);
        if (k==a->game) box(a,(SDL_Rect){r.x,r.y+37,r.w,3},ACCENT);
        SDL_RenderSetClipRect(a->renderer,&r);
        label(a,a->body,r.x+12,r.y+8,2000,a->games[k].title,k==a->game?TEXT:MUTED);
        SDL_RenderSetClipRect(a->renderer,NULL);
        add_hit(a,GAME_BASE+k,r);
    }
    box(a,(SDL_Rect){28,144,242,544},PANEL);
    label(a,a->small,44,160,210,"SAVED PRESETS",MUTED);
    int start=a->book.selected>7?a->book.selected-7:0;
    for (int k=start;k<a->book.count && k<start+8;k++) {
        int y=192+(k-start)*43; SDL_Rect r={40,y,218,39};
        box(a,r,k==a->book.selected?ROW:PANEL);
        if (k==a->book.selected) box(a,(SDL_Rect){40,y,3,39},ACCENT);
        if (a->focus==PRESET_BASE+k) outline(a,r,ACCENT);
        SDL_RenderSetClipRect(a->renderer,&r);
        label(a,a->body,52,y+7,900,a->book.presets[k].name,k==a->book.selected?TEXT:MUTED);
        SDL_RenderSetClipRect(a->renderer,NULL);
        add_hit(a,PRESET_BASE+k,r);
    }
    if (a->book.count>8) label(a,a->small,44,540,210,"Scroll here for more presets",MUTED);
    button(a,NEW,40,580,103,38,"New",0); button(a,DUPLICATE,151,580,107,38,"Duplicate",0);
    button(a,RENAME,40,626,103,38,"Rename",0); button(a,DELETE,151,626,107,38,"Delete",0);
    /* Up to four page tabs keep their size; more share the row's width, and
     * a name too long for its tab takes the small font. */
    const char *names[MAX_PAGES]; int page_n=pages(names), step=page_n>4?808/page_n:180;
    for (int k=0;k<page_n;k++) {
        int x=294+k*step, w=step-10, text_w=0, text_h=0;
        TTF_SizeUTF8(a->body,names[k],&text_w,&text_h);
        if (text_w<=w-24) button(a,PAGE_BASE+k,x,144,w,44,names[k],0);
        else {
            SDL_Rect r={x,144,w,44}; box(a,r,ROW);
            outline(a,r,a->focus==PAGE_BASE+k?ACCENT:BORDER);
            label(a,a->small,x+12,158,w-24,names[k],TEXT); add_hit(a,PAGE_BASE+k,r);
        }
        if (k==a->page) box(a,(SDL_Rect){x,185,w,3},ACCENT);
    }
    int ids[PSP_SETTINGS_MAX], n=rows(a,ids), display=known("DISPLAY"), input=known("INPUT");
    if (a->scroll>n-VISIBLE_ROWS) a->scroll=n>VISIBLE_ROWS?n-VISIBLE_ROWS:0;
    if (a->scroll<0) a->scroll=0;
    for (int k=a->scroll;k<n && k<a->scroll+VISIBLE_ROWS;k++) {
        int id=ids[k],y=204+(k-a->scroll)*56;
        SDL_Rect r={294,y,798,50}; box(a,r,ROW);
        if (a->focus==id) outline(a,r,ACCENT);
        int locked=overridden(id), missing=unavailable(a,id);
        int disabled=missing && !editing(a)->number[id];
        label(a,a->body,310,y+5,300,option(id)->label,locked||disabled?MUTED:TEXT);
        label(a,a->small,310,y+29,360,locked?"Environment override":missing?
              (disabled?"Not available in this build":"Decoder unavailable; switch off"):"Applies on next launch",locked?ACCENT:MUTED);
        char value[PSP_SETTINGS_VALUE]; psp_option_label(&a->effective,id,value,sizeof value);
        if (id==display) display_label(&a->effective,value,sizeof value);
        SDL_Rect clip={662,y,330,50}; SDL_RenderSetClipRect(a->renderer,&clip);
        label(a,a->body,668,y+13,600,value,locked?ACCENT:TEXT);
        SDL_RenderSetClipRect(a->renderer,NULL);
        add_hit(a,id,r);
        if (!locked && !disabled) {
            /* These hits deliberately follow the row hit, so the small
             * controls win hit testing without adding keyboard focus stops. */
            label(a,a->body,1001,y+12,35,"-",MUTED);
            label(a,a->body,1050,y+12,35,"+",MUTED);
            add_hit(a,400+id,(SDL_Rect){986,y,48,50});
            add_hit(a,500+id,(SDL_Rect){1034,y,58,50});
        }
    }
    if (n>VISIBLE_ROWS) {
        char range[80]; snprintf(range,sizeof range,"%d-%d of %d  /  Scroll or use Up / Down",a->scroll+1,
                                a->scroll+VISIBLE_ROWS<n?a->scroll+VISIBLE_ROWS:n,n);
        label(a,a->small,310,546,650,range,MUTED);
    }
    box(a,(SDL_Rect){294,580,798,108},PANEL);
    int id=a->selected_row;
    if (is_option(id)) {
        char info[640];
        snprintf(info,sizeof info,"%s%s%s",option(id)->help,
                 overridden(id)?"  Locked for this run by ":"",overridden(id)?option(id)->env:"");
        if (id==display) {
            int screen=(int)a->effective.number[id];
            if (screen<0) screen=1;
            const char *name=screen<=SDL_GetNumVideoDisplays()?SDL_GetDisplayName(screen-1):NULL;
            if (name) {
                size_t used=strlen(info);
                snprintf(info+used,sizeof info-used,"\nDisplay %d: %s",screen,name);
            }
        }
        label(a,a->small,310,594,762,info,MUTED);
    }
    if (input>=0 && shown(input) && a->page<page_n && !strcmp(option(input)->page,names[a->page]))
        label(a,a->small,310,664,762,"Quit game: hold View + Menu (Select + Start) 2s  |  Ctrl+Shift+Q",MUTED);
    const char *status=a->load_failed?a->status:!a->valid?a->validation:*a->status?a->status:
                       a->dirty?"Unsaved changes":"Presets are ready. Changes take effect when you launch.";
    SDL_Rect status_clip={28,695,1064,30}; SDL_RenderSetClipRect(a->renderer,&status_clip);
    label(a,a->small,28,700,1064,status,!a->valid||a->load_failed?ACCENT:GOOD);
    SDL_RenderSetClipRect(a->renderer,NULL);
    button(a,RESET,28,737,155,43,"Reset preset",0);
    button(a,CANCEL,700,737,110,43,"Cancel",0);
    button(a,SAVE,822,737,100,43,"Save",0);
    if (a->boot && a->module) button(a,PLAY,934,737,158,43,"Save & Play",1);
    else if (a->game_count && a->importer) button(a,PLAY,934,737,158,43,"Prepare game",1);
    else label(a,a->small,943,750,149,"No game installed",MUTED);
    label(a,a->small,205,737,465,"Tab: focus   Arrows: adjust   Enter: edit\nController: D-pad / A / B   Bumpers: pages\nQuit game: hold View + Menu 2s / Ctrl+Shift+Q",MUTED);

    if (a->modal==MODAL_BROWSE || a->modal==MODAL_PREPARING) { library_modal_draw(a); return; }
    if (a->modal) {
        SDL_SetRenderDrawBlendMode(a->renderer,SDL_BLENDMODE_BLEND);
        box(a,(SDL_Rect){0,0,UI_W,UI_H},(SDL_Color){0,0,0,190});
        SDL_SetRenderDrawBlendMode(a->renderer,SDL_BLENDMODE_NONE);
        a->hit_count=0;
        box(a,(SDL_Rect){270,239,580,320},PANEL); outline(a,(SDL_Rect){270,239,580,320},BORDER);
        const char *titles[]={"","New preset","Duplicate preset","Rename preset","Edit value",
                              "Delete preset?","Reset this preset?","Discard unsaved changes?"};
        label(a,a->heading,296,258,530,titles[a->modal],TEXT);
        if (a->modal<=MODAL_VALUE) {
            char hint[256];
            if (a->modal==MODAL_VALUE) {
                const psp_option_def *d=option(a->edit_id);
                if (d->type==PSP_OPTION_SIZE) snprintf(hint,sizeof hint,"%s: WIDTHxHEIGHT",d->label);
                else snprintf(hint,sizeof hint,"%s: %g to %g%s%s",d->label,d->min,d->max,
                              d->special?", or ":"",d->special?d->special:"");
            } else strcpy(hint,"Preset name (up to 63 UTF-8 bytes)");
            label(a,a->small,296,307,530,hint,MUTED);
            SDL_Rect entry={296,340,528,50}; box(a,entry,ROW); outline(a,entry,ACCENT);
            SDL_RenderSetClipRect(a->renderer,&entry);
            label(a,a->body,308,352,1500,a->edit,a->select_text?ACCENT:TEXT);
            SDL_RenderSetClipRect(a->renderer,NULL);
            label(a,a->small,296,402,526,a->modal_error,MUTED);
        } else {
            const char *explain=a->modal==MODAL_DELETE?"The preset will be removed from this session. Save to make the deletion permanent.":
                a->modal==MODAL_RESET?"Restore this preset to the game's original graphics and controls. Save to keep the change.":
                "Your unsaved preset edits will be discarded. The saved file will stay as it was.";
            label(a,a->body,296,321,526,explain,MUTED);
            label(a,a->small,296,419,526,a->modal_error,ACCENT);
        }
        button(a,MODAL_CANCEL,576,495,114,42,"Cancel",0);
        button(a,MODAL_OK,702,495,122,42,a->modal<=MODAL_VALUE?"Accept":"Confirm",1);
    }
}

static int save(launcher *a) {
    if (a->load_failed) return -1;
    if (psp_presets_save(&a->book,a->path,a->status)) return -1;
    a->dirty=0; snprintf(a->status,sizeof a->status,"Saved preset: %.63s",a->book.presets[a->book.selected].name);
    return 0;
}
static void changed(launcher *a) { a->dirty=1; a->status[0]=0; refresh(a); }

/* The option's stops (psp_option_def.stops): their text, and their values
 * with the special word as -1. */
static int stops(const psp_option_def *d,const char **text,size_t *length,double *at) {
    int n=0;
    for (const char *v=d->stops;v && n<MAX_STOPS;n++) {
        text[n]=v; length[n]=strcspn(v,"|");
        int special=d->special && strlen(d->special)==length[n] && !strncmp(v,d->special,length[n]);
        at[n]=special?-1:strtod(v,NULL);
        v=v[length[n]]?v+length[n]+1:NULL;
    }
    return n;
}

static void adjust(launcher *a,int id,int direction) {
    if (!is_option(id)) return;
    a->focus=id; show_row(a,id);
    if (overridden(id)) return;
    psp_settings *s=editing(a); const psp_option_def *d=option(id); char value[PSP_SETTINGS_VALUE];
    if (unavailable(a,id)) {
        /* A preset from a decoder-equipped build can still be repaired here. */
        if (s->number[id] && !psp_settings_set(s,id,"0",PSP_SOURCE_PRESET,a->status)) changed(a);
        return;
    }
    const char *text[MAX_STOPS]; size_t length[MAX_STOPS]; double at[MAX_STOPS];
    int count=d->stops?stops(d,text,length,at):0;
    if (count) {
        /* From the special stop, or from a value between two, the arrows
         * reach the adjacent stops; the ends wrap. */
        int last=at[count-1]<0?count-2:count-1;
        int pick=s->number[id]<0 && last<count-1?count-1:0;
        while (pick<last && s->number[id]>at[pick]) pick++;
        if (s->number[id]==at[pick] || direction<0) pick=(pick+direction+count)%count;
        snprintf(value,sizeof value,"%.*s",(int)length[pick],text[pick]);
    } else if (id==known("DISPLAY")) {
        int count=SDL_GetNumVideoDisplays();
        if (count<0) count=0;
        int pick=(int)s->number[id];
        if (pick<0 || pick>count) pick=0; /* Primary or disconnected. */
        pick=(pick+direction+count+1)%(count+1);
        if (!pick) snprintf(value,sizeof value,"primary");
        else snprintf(value,sizeof value,"%d",pick);
    } else if (d->type==PSP_OPTION_CHOICE) {
        int count=1; for (const char *v=d->choices;*v;v++) if (*v=='|') count++;
        if (id==known("RENDER")) count--; /* Null is useful in files/CLI, not for play. */
        int pick=((int)s->number[id]+direction+count)%count;
        const char *v=d->choices; while (pick--) v=strchr(v,'|')+1;
        size_t n=strcspn(v,"|"); memcpy(value,v,n); value[n]=0;
    } else if (d->type==PSP_OPTION_SIZE) {
        const char *sizes[]={"960x544","1280x720","1600x900","1920x1080","2560x1440","3840x2160"};
        int pick=0; for (int i=0;i<6;i++) if (!strcmp(sizes[i],s->value[id])) pick=i;
        snprintf(value,sizeof value,"%s",sizes[(pick+direction+6)%6]);
    } else {
        /* A special word shown the same as 0 (both "Off", say) is one stop
         * with it, not two. */
        int as_zero=d->special && d->special_label && d->zero_label && !strcmp(d->special_label,d->zero_label);
        double n=s->number[id];
        if (n<0) n=direction>0?(as_zero?d->min+d->step:d->min):d->max;
        else n=round((n+direction*d->step)/d->step)*d->step;
        if (d->special && (n<d->min || (as_zero && n<=d->min))) snprintf(value,sizeof value,"%s",d->special);
        else snprintf(value,sizeof value,"%.9g",fmin(d->max,fmax(d->min,n)));
    }
    if (!psp_settings_set(s,id,value,PSP_SOURCE_PRESET,a->status)) changed(a);
}

static void modal_accept(launcher *a) {
    int kind=a->modal;
    if (kind==MODAL_NEW || kind==MODAL_DUPLICATE) {
        psp_settings s;
        if (kind==MODAL_DUPLICATE) s=*editing(a);
        else {
            psp_settings_defaults(&s);
            psp_settings_assign(&s,psp_launcher_info.new_preset,PSP_SOURCE_PRESET,a->modal_error);
            movie_off(a,&s,a->modal_error);
        }
        if (psp_presets_add(&a->book,a->edit,&s,a->modal_error)) return;
        a->book.selected=a->book.count-1;
    } else if (kind==MODAL_RENAME) {
        int existing=psp_presets_find(&a->book,a->edit);
        if (!psp_presets_name_valid(a->edit) || (existing>=0 && existing!=a->book.selected)) {
            strcpy(a->modal_error,"Use a unique name without brackets, =, ; or #."); return;
        }
        strcpy(a->book.presets[a->book.selected].name,a->edit);
    } else if (kind==MODAL_VALUE) {
        if (psp_settings_set(editing(a),a->edit_id,a->edit,PSP_SOURCE_PRESET,a->modal_error)) return;
    } else if (kind==MODAL_DELETE) {
        if (a->book.count==1) { strcpy(a->modal_error,"Keep at least one preset."); return; }
        int at=a->book.selected;
        memmove(a->book.presets+at,a->book.presets+at+1,(size_t)(a->book.count-at-1)*sizeof(psp_preset));
        a->book.count--; if (at>=a->book.count) a->book.selected=a->book.count-1;
    } else if (kind==MODAL_RESET) {
        psp_settings_defaults(editing(a));
        psp_settings_assign(editing(a),psp_launcher_info.reset_preset,PSP_SOURCE_PRESET,a->modal_error);
        movie_off(a,editing(a),a->modal_error);
    } else if (kind==MODAL_CANCEL_DIRTY) { a->running=0; close_modal(a); return; }
    changed(a); close_modal(a);
}

/* The game runs from a per-title save folder, <data root>/saves/<slug>, so a
 * source build and the packaged AppImage write saves to the same place and
 * one folder can be synced between computers (docs/SAVE-SYNC.md). The data
 * root follows AppRun's rule: $LR_DATA_ROOT, else $XDG_DATA_HOME/<app id>,
 * else ~/.local/share/<app id>; relative values are ignored. A run without
 * a title slug (bare --boot/--module) keeps the launcher's own directory. */
static int data_root(char *out,size_t cap) {
    const char *given=getenv("LR_DATA_ROOT"),*xdg=getenv("XDG_DATA_HOME"),*home=getenv("HOME");
    int n;
    if (given && given[0]=='/') n=snprintf(out,cap,"%s",given);
    else if (xdg && xdg[0]=='/') n=snprintf(out,cap,"%s/%s",xdg,psp_launcher_info.id);
    else if (home && home[0]=='/') n=snprintf(out,cap,"%s/.local/share/%s",home,psp_launcher_info.id);
    else return -1;
    return n>0 && (size_t)n<cap?0:-1;
}
/* Empty when no title is selected; -1 when no data root can be found. */
static int game_save_root(launcher *a,char *out,size_t cap) {
    char root[4096]; out[0]=0;
    if (!a->game_count) return 0;
    if (data_root(root,sizeof root)) return -1;
    int n=snprintf(out,cap,"%s/saves/%s",root,a->games[a->game].slug);
    return n>0 && (size_t)n<cap?0:-1;
}
/* Save states, beside the saves and not among them: a state loads only into
 * the build that wrote it, so it has no business in a folder synced between
 * computers. The game makes it on its first save (psp_state_dir). */
static int game_state_root(launcher *a,char *out,size_t cap) {
    char root[4096]; out[0]=0;
    if (!a->game_count) return 0;
    if (data_root(root,sizeof root)) return -1;
    int n=snprintf(out,cap,"%s/states/%s",root,a->games[a->game].slug);
    return n>0 && (size_t)n<cap?0:-1;
}
static int make_directories(const char *path) {
    char work[4096]; size_t len=strlen(path);
    if (!len || len>=sizeof work) { errno=ENAMETOOLONG; return -1; }
    memcpy(work,path,len+1);
    for (char *p=work+1;*p;p++) if (*p=='/') {
        *p=0; if (mkdir(work,0755) && errno!=EEXIST) return -1; *p='/';
    }
    if (mkdir(work,0755) && errno!=EEXIST) return -1;
    struct stat st;
    if (stat(path,&st)) return -1;
    if (!S_ISDIR(st.st_mode)) { errno=ENOTDIR; return -1; }
    return 0;
}
/* A launch path in absolute form, so it still resolves after the child
 * changes directory. Nothing is canonicalised; the spelling is kept. */
static int absolute(const char *path,char *out,size_t cap) {
    int n;
    if (path[0]=='/') n=snprintf(out,cap,"%s",path);
    else {
        char cwd[4096];
        if (!getcwd(cwd,sizeof cwd)) return -1;
        n=snprintf(out,cap,"%s/%s",cwd,path);
    }
    return n>0 && (size_t)n<cap?0:-1;
}

static void launch_game(launcher *a) {
    refresh(a);
    if (!a->valid || a->load_failed) return;
    if (!a->boot || !a->module) {
        if (a->importer && a->iso && !access(a->iso,R_OK)) import_start(a,a->iso);
        else if (a->importer) browser_open(a);
        else strcpy(a->status,"No game installed. You can still save your presets.");
        return;
    }
    if (access(a->boot,X_OK) || access(a->module,R_OK) || (a->iso && access(a->iso,R_OK))) {
        snprintf(a->status,sizeof a->status,"Cannot access game executable, module or disc: %s",strerror(errno)); return;
    }
    if (a->effective.render==3) { strcpy(a->status,"The null renderer is for diagnostics. Select OpenGL or software before playing."); return; }
    if (save(a)) return;
    char save_root[4096],boot[4096],module[4096],iso[4096],config[4096];
    if (game_save_root(a,save_root,sizeof save_root)) {
        strcpy(a->status,"Cannot find the save folder: LR_DATA_ROOT, XDG_DATA_HOME or HOME must be an absolute path."); return;
    }
    if (*save_root && make_directories(save_root)) {
        snprintf(a->status,sizeof a->status,"Cannot create the save folder %s: %s",save_root,strerror(errno)); return;
    }
    if (absolute(a->boot,boot,sizeof boot) || absolute(a->module,module,sizeof module) ||
        (a->iso && absolute(a->iso,iso,sizeof iso)) || absolute(a->path,config,sizeof config)) {
        strcpy(a->status,"Cannot launch: a game path is too long."); return;
    }
    if (*save_root) fprintf(stderr,"launcher: saves %s\n",save_root);
    /* The game's environment, with its state folder: built before the fork,
     * since only async-signal-safe calls may follow it. */
    static char state_var[4200];
    char state_root[4096];
    size_t nenv=0;
    while (environ[nenv]) nenv++;
    char **env=malloc((nenv+2)*sizeof *env);
    if (!env) { strcpy(a->status,"Cannot launch: out of memory."); return; }
    size_t k=0;
    for (size_t i=0;i<nenv;i++) if (strncmp(environ[i],"PSPRECOMP_STATE_DIR=",20)) env[k++]=environ[i];
    if (!game_state_root(a,state_root,sizeof state_root) && *state_root) {
        snprintf(state_var,sizeof state_var,"PSPRECOMP_STATE_DIR=%s",state_root);
        env[k++]=state_var;
        fprintf(stderr,"launcher: states %s\n",state_root);
    }
    env[k]=NULL;
    int pipes[2];
    if (pipe(pipes)) { free(env); snprintf(a->status,sizeof a->status,"Cannot launch: %s",strerror(errno)); return; }
    fflush(NULL);
    pid_t child=fork();
    if (child<0) { free(env); close(pipes[0]); close(pipes[1]); snprintf(a->status,sizeof a->status,"Cannot launch: %s",strerror(errno)); return; }
    if (!child) {
        close(pipes[0]); dup2(pipes[1],STDERR_FILENO); close(pipes[1]);
        const char *args[12]; int n=0;
        args[n++]=boot; args[n++]=module; if (a->iso) args[n++]=iso;
        args[n++]="--config"; args[n++]=config;
        args[n++]="--preset"; args[n++]=a->book.presets[a->book.selected].name;
        args[n++]="--window"; args[n]=NULL;
        /* Only async-signal-safe operations between fork and exec: SDL may
         * have other threads with libc locks held at the fork boundary. */
        static const char exec_failed[]="Could not execute the game host. Check its path and permissions.\n",
                          chdir_failed[]="Could not enter the save folder. Check its permissions.\n";
        const char *message=exec_failed; size_t sent=0,total=sizeof exec_failed-1;
        if (*save_root && chdir(save_root)) { message=chdir_failed; total=sizeof chdir_failed-1; }
        else execve(boot,(char *const *)args,env);
        while (sent<total) {
            ssize_t n=write(STDERR_FILENO,message+sent,total-sent);
            if (n>0) sent+=(size_t)n;
            else if (n<0 && errno==EINTR) continue;
            else break;
        }
        _exit(127);
    }
    free(env);
    close(pipes[1]); fcntl(pipes[0],F_SETFL,O_NONBLOCK);
    a->child=child; a->child_error_fd=pipes[0]; a->child_error_len=0; a->child_error[0]=0;
    SDL_HideWindow(a->window);
}

static void activate(launcher *a,int id) {
    if (a->modal==MODAL_BROWSE) { browser_activate(a,id); return; }
    if (a->modal==MODAL_PREPARING) { if (id==BROWSER_CANCEL || id==CANCEL) import_cancel(a); return; }
    if (a->modal) {
        if (id==MODAL_OK) modal_accept(a);
        else if (id==MODAL_CANCEL) close_modal(a);
        return;
    }
    if (id>=500 && is_option(id-500)) { adjust(a,id-500,1); return; }
    if (id>=400 && is_option(id-400)) { adjust(a,id-400,-1); return; }
    if (id>=PAGE_BASE && id<PAGE_BASE+page_count()) {
        a->page=id-PAGE_BASE; a->scroll=0;
        int ids[PSP_SETTINGS_MAX]; if (rows(a,ids)) a->focus=a->selected_row=ids[0];
        return;
    }
    if (id>=PRESET_BASE && id<PRESET_BASE+a->book.count) {
        if (a->book.selected!=id-PRESET_BASE) { a->book.selected=id-PRESET_BASE; changed(a); }
        a->focus=id; return;
    }
    if (id>=GAME_BASE && id<GAME_BASE+tab_count(a)) { select_game(a,id-GAME_BASE,0); a->focus=id; return; }
    if (is_option(id)) {
        a->selected_row=a->focus=id;
        if (overridden(id) || (unavailable(a,id) && !editing(a)->number[id])) return;
        if (option(id)->type==PSP_OPTION_CHOICE || id==known("DISPLAY")) adjust(a,id,1);
        else { a->edit_id=id; open_modal(a,MODAL_VALUE,editing(a)->value[id]); }
        return;
    }
    char name[PSP_SETTINGS_NAME];
    switch (id) {
    case NEW: case DUPLICATE:
        for (int n=1;;n++) {
            snprintf(name,sizeof name,"%s %d",id==NEW?"Preset":"Copy",n);
            if (psp_presets_find(&a->book,name)<0) break;
        }
        open_modal(a,id==NEW?MODAL_NEW:MODAL_DUPLICATE,name); break;
    case RENAME: open_modal(a,MODAL_RENAME,a->book.presets[a->book.selected].name); break;
    case DELETE: open_modal(a,MODAL_DELETE,NULL); break;
    case RESET: open_modal(a,MODAL_RESET,NULL); break;
    case SAVE: save(a); break;
    case PLAY: launch_game(a); break;
    case ADD_GAME: browser_open(a); break;
    case ABOUT:
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION,"About",psp_launcher_info.about,a->window);
        break;
    case CANCEL:
        if (a->dirty) open_modal(a,MODAL_CANCEL_DIRTY,NULL); else a->running=0;
        break;
    }
}

static void focus_next(launcher *a,int direction) {
    int ids[128],n=0;
    if (a->modal) { a->focus=a->focus==MODAL_OK?MODAL_CANCEL:MODAL_OK; return; }
    /* Include every option, including offscreen rows, so navigation can
     * scroll them into view. Small +/- mouse targets are not tab stops. */
    for (int i=0;i<tab_count(a);i++) ids[n++]=GAME_BASE+i;
    if (a->importer) ids[n++]=ADD_GAME;
    for (int i=0;i<a->book.count;i++) ids[n++]=PRESET_BASE+i;
    ids[n++]=NEW; ids[n++]=DUPLICATE; ids[n++]=RENAME; ids[n++]=DELETE;
    for (int i=0;i<page_count();i++) ids[n++]=PAGE_BASE+i;
    int option_ids[PSP_SETTINGS_MAX],count=rows(a,option_ids);
    for (int i=0;i<count;i++) ids[n++]=option_ids[i];
    ids[n++]=RESET; ids[n++]=CANCEL; ids[n++]=SAVE;
    if ((a->boot && a->module) || (a->game_count && a->importer)) ids[n++]=PLAY;
    ids[n++]=ABOUT;
    int at=0; for (int i=0;i<n;i++) if (ids[i]==a->focus) at=i;
    a->focus=ids[(at+direction+n)%n]; show_row(a,a->focus);
}

static void key(launcher *a,SDL_Keycode k,SDL_Keymod mod) {
    if (a->modal==MODAL_PREPARING) { if (k==SDLK_ESCAPE || k==SDLK_RETURN) import_cancel(a); return; }
    if (a->modal==MODAL_BROWSE) {
        if (k==SDLK_ESCAPE) close_modal(a);
        else if (k==SDLK_UP || k==SDLK_DOWN) browser_move(a,k==SDLK_UP?-1:1);
        else if (k==SDLK_PAGEUP || k==SDLK_PAGEDOWN) browser_move(a,k==SDLK_PAGEUP?-10:10);
        else if (k==SDLK_LEFT || k==SDLK_BACKSPACE) browser_up(a);
        else if (k==SDLK_RETURN || k==SDLK_KP_ENTER || k==SDLK_RIGHT) browser_enter(a);
        else if (k==SDLK_v && (mod & KMOD_CTRL)) {
            char *path=SDL_GetClipboardText(); struct stat st;
            if (path && !stat(path,&st)) { if (S_ISDIR(st.st_mode)) browser_scan(a,path); else import_start(a,path); }
            else snprintf(a->browser_error,sizeof a->browser_error,"The pasted path could not be opened.");
            SDL_free(path);
        }
        return;
    }
    if (k==SDLK_ESCAPE) { if (a->modal) close_modal(a); else activate(a,CANCEL); return; }
    if (k==SDLK_TAB) { focus_next(a,(mod & KMOD_SHIFT)?-1:1); return; }
    if (k==SDLK_RETURN || k==SDLK_KP_ENTER) { activate(a,a->focus); return; }
    if (a->modal) {
        if (a->modal<=MODAL_VALUE && k==SDLK_BACKSPACE) {
            size_t n=strlen(a->edit);
            if (a->select_text) a->edit[0]=0;
            else if (n) { do { n--; } while (n && (a->edit[n]&0xc0)==0x80); a->edit[n]=0; }
            a->select_text=0;
        }
        if (k==SDLK_a && (mod & KMOD_CTRL)) a->select_text=1;
        if (k==SDLK_LEFT || k==SDLK_RIGHT) focus_next(a,1);
        return;
    }
    if (k==SDLK_UP || k==SDLK_DOWN) { focus_next(a,k==SDLK_UP?-1:1); return; }
    if (k==SDLK_LEFT || k==SDLK_RIGHT) {
        int d=k==SDLK_LEFT?-1:1;
        if (is_option(a->focus)) adjust(a,a->focus,d);
        else if (tab_count(a) && a->focus>=GAME_BASE && a->focus<GAME_BASE+tab_count(a)) {
            /* A focused tab cycles the titles, so a controller can pick one. */
            select_game(a,(a->game+d+a->game_count)%a->game_count,0); a->focus=GAME_BASE+a->game;
        } else focus_next(a,d);
    }
}

static void controller_open(launcher *a) {
    if (a->pad && !SDL_GameControllerGetAttached(a->pad)) { SDL_GameControllerClose(a->pad); a->pad=NULL; }
    if (!a->pad) for (int i=0;i<SDL_NumJoysticks();i++) if (SDL_IsGameController(i)) {
        a->pad=SDL_GameControllerOpen(i); if (a->pad) break;
    }
}
static void event(launcher *a,const SDL_Event *e) {
    if (e->type==SDL_CONTROLLERDEVICEADDED || e->type==SDL_CONTROLLERDEVICEREMOVED) controller_open(a);
    if (a->child) return;
    if (e->type==SDL_QUIT) {
        if (a->import_pid) { a->quit_after_import=1; import_cancel(a); }
        else { if (a->modal==MODAL_BROWSE) close_modal(a); activate(a,CANCEL); }
        return;
    }
    if (e->type==SDL_DROPFILE) { import_start(a,e->drop.file); SDL_free(e->drop.file); return; }
    if (e->type==SDL_KEYDOWN) key(a,e->key.keysym.sym,(SDL_Keymod)e->key.keysym.mod);
    if (e->type==SDL_TEXTINPUT && a->modal && a->modal<=MODAL_VALUE) {
        if (a->select_text) a->edit[0]=0;
        a->select_text=0;
        size_t n=strlen(a->edit), add=strlen(e->text.text);
        size_t max=a->modal==MODAL_VALUE?PSP_SETTINGS_VALUE:PSP_SETTINGS_NAME;
        if (n+add<max) memcpy(a->edit+n,e->text.text,add+1);
    }
    if (e->type==SDL_MOUSEBUTTONDOWN && e->button.button==SDL_BUTTON_LEFT) {
        for (int i=a->hit_count-1;i>=0;i--) {
            SDL_Point p={e->button.x,e->button.y};
            if (SDL_PointInRect(&p,&a->hits[i].rect)) { a->focus=a->hits[i].id; activate(a,a->focus); break; }
        }
    }
    if (e->type==SDL_MOUSEWHEEL && a->modal==MODAL_BROWSE)
        browser_move(a,(e->wheel.direction==SDL_MOUSEWHEEL_FLIPPED?1:-1)*e->wheel.y);
    if (e->type==SDL_MOUSEWHEEL && !a->modal) {
        int x,y; SDL_GetMouseState(&x,&y);
        float lx,ly; SDL_RenderWindowToLogical(a->renderer,x,y,&lx,&ly);
        int delta=e->wheel.direction==SDL_MOUSEWHEEL_FLIPPED?-e->wheel.y:e->wheel.y;
        if (lx<280) {
            int at=a->book.selected-delta;
            if (at<0) at=0;
            if (at>=a->book.count) at=a->book.count-1;
            activate(a,PRESET_BASE+at);
        } else a->scroll-=delta;
    }
    if (e->type==SDL_CONTROLLERBUTTONDOWN) {
        switch (e->cbutton.button) {
        case SDL_CONTROLLER_BUTTON_A: key(a,SDLK_RETURN,0); break;
        case SDL_CONTROLLER_BUTTON_B: key(a,SDLK_ESCAPE,0); break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP: key(a,SDLK_UP,0); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: key(a,SDLK_DOWN,0); break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: key(a,SDLK_LEFT,0); break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: key(a,SDLK_RIGHT,0); break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            if (a->modal==MODAL_BROWSE) browser_move(a,e->cbutton.button==SDL_CONTROLLER_BUTTON_LEFTSHOULDER?-10:10);
            if (!a->modal && page_count()) {
                int n=page_count();
                activate(a,PAGE_BASE+(a->page+(e->cbutton.button==SDL_CONTROLLER_BUTTON_LEFTSHOULDER?n-1:1))%n);
            }
            break;
        case SDL_CONTROLLER_BUTTON_START: if (!a->modal) activate(a,PLAY); break;
        }
    }
}

static void poll_child(launcher *a) {
    if (!a->child) return;
    char buf[512]; ssize_t got;
    while ((got=read(a->child_error_fd,buf,sizeof buf))>0) {
        fwrite(buf,1,(size_t)got,stderr);
        size_t keep=a->child_error_len;
        if (keep+(size_t)got>=sizeof a->child_error) {
            size_t discard=keep+(size_t)got-sizeof a->child_error+1;
            memmove(a->child_error,a->child_error+discard,keep-discard); keep-=discard;
        }
        memcpy(a->child_error+keep,buf,(size_t)got); a->child_error_len=keep+(size_t)got;
        a->child_error[a->child_error_len]=0;
    }
    int status; pid_t done=waitpid(a->child,&status,WNOHANG);
    if (done<=0) return;
    close(a->child_error_fd); a->child=0;
    /* The game's menu may have written the preset it started with: the file
     * is the truth now, unless something here is still unsaved. */
    if (!a->dirty) {
        psp_presets *fresh=malloc(sizeof *fresh); char why[PSP_SETTINGS_ERROR];
        if (fresh && !psp_presets_load(fresh,a->path,why)) { a->book=*fresh; refresh(a); }
        free(fresh);
    }
    if (WIFEXITED(status) && WEXITSTATUS(status)==0) { a->running=0; return; }
    SDL_ShowWindow(a->window); SDL_RaiseWindow(a->window);
    snprintf(a->status,sizeof a->status,"Game stopped (%s %d). See terminal output.",
             WIFEXITED(status)?"exit":"signal",WIFEXITED(status)?WEXITSTATUS(status):WTERMSIG(status));
    char title[192]; snprintf(title,sizeof title,"%s could not start",game_title(a));
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,title,*a->child_error?a->child_error:a->status,a->window);
}

static int fonts(launcher *a,const char *requested) {
    const char *paths[]={requested,"/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf","/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf","C:/Windows/Fonts/segoeui.ttf"};
    for (size_t i=0;i<sizeof paths/sizeof paths[0];i++) {
        if (!paths[i]) continue;
        a->body=TTF_OpenFont(paths[i],17);
        if (a->body) {
            a->small=TTF_OpenFont(paths[i],13); a->heading=TTF_OpenFont(paths[i],28);
            return a->small && a->heading?0:-1;
        }
        if (requested) break;
    }
    return -1;
}

int main(int argc,char **argv) {
    launcher a={0}; a.running=1;
    int first[PSP_SETTINGS_MAX];
    if (rows(&a,first)) a.focus=a.selected_row=first[0];
    int check_startup=0;
    const char *preset=NULL,*font=NULL,*wanted=NULL;
    for (int i=1;i<argc;i++) {
        if (!strcmp(argv[i],"--help")) {
            puts("launcher [--config FILE] [--preset NAME] [--boot EXECUTABLE --module ELF [--iso DISC]] [--font TTF]\n"
                 "         [--game SLUG|TITLE|BOOT|MODULE[|ISO]]... [--select SLUG] [--check-startup]\n"
                 "         [--library FILE --importer EXECUTABLE]\n"
                 "Each --game adds a title tab; --select opens on that slug instead of the remembered one.\n"
                 "Keyboard: Tab, arrows, Enter, Escape. Controller: D-pad, A/B, bumpers, Start."); return 0;
        }
        if (!strcmp(argv[i],"--check-startup")) { check_startup=1; continue; }
        const char *flag=argv[i];
        if (++i==argc) { fprintf(stderr,"%s needs a value\n",flag); return 2; }
        if (!strcmp(flag,"--config")) {
            if (strlen(argv[i])>=sizeof a.path) { fprintf(stderr,"config path too long\n"); return 2; }
            strcpy(a.path,argv[i]);
        } else if (!strcmp(flag,"--preset")) preset=argv[i];
        else if (!strcmp(flag,"--boot")) a.boot=argv[i];
        else if (!strcmp(flag,"--module")) a.module=argv[i];
        else if (!strcmp(flag,"--iso")) a.iso=argv[i];
        else if (!strcmp(flag,"--font")) font=argv[i];
        else if (!strcmp(flag,"--select")) wanted=argv[i];
        else if (!strcmp(flag,"--library")) a.library=argv[i];
        else if (!strcmp(flag,"--importer")) a.importer=argv[i];
        else if (!strcmp(flag,"--game")) {
            if (a.game_count==MAX_GAMES) { fprintf(stderr,"too many --game entries (at most %d)\n",MAX_GAMES); return 2; }
            game_entry g={0}; char *spec=argv[i];
            const char **field[]={&g.slug,&g.title,&g.boot,&g.module,&g.iso};
            for (int k=0;k<5 && spec;k++) {
                *field[k]=spec; char *bar=strchr(spec,'|');
                if (bar) *bar=0;
                spec=bar?bar+1:NULL;
            }
            if (!g.slug || !*g.slug || !g.title || !*g.title || !g.boot || !*g.boot || !g.module || !*g.module ||
                !psp_presets_name_valid(g.slug)) {
                fprintf(stderr,"--game needs SLUG|TITLE|BOOT|MODULE[|ISO], got: %s\n",argv[i]); return 2;
            }
            a.games[a.game_count++]=g;
        }
        else { fprintf(stderr,"unknown option: %s\n",flag); return 2; }
    }
    if (SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMECONTROLLER) || TTF_Init()) {
        fprintf(stderr,"launcher: %s\n",SDL_GetError()); return 1;
    }
    if (!*a.path) {
        char *dir=SDL_GetPrefPath("",psp_launcher_info.name);
        if (!dir || snprintf(a.path,sizeof a.path,"%ssettings.ini",dir)>=(int)sizeof a.path) {
            fprintf(stderr,"cannot find preferences directory: %s\n",SDL_GetError()); SDL_free(dir); return 1;
        }
        SDL_free(dir);
    }
    a.movie_available=psp_mpeg_decoding_available();
    if (access(a.path,F_OK)==0 || errno!=ENOENT) {
        if (psp_presets_load(&a.book,a.path,a.status)) a.load_failed=1;
    } else a.dirty=1;
    if (!a.book.count) {
        psp_presets_defaults(&a.book);
        for (int i=0;i<a.book.count;i++) movie_off(&a,&a.book.presets[i].settings,a.validation);
    }
    if (preset) {
        int at=psp_presets_find(&a.book,preset);
        if (at<0) { fprintf(stderr,"preset does not exist: %s\n",preset); return 2; }
        if (a.book.selected!=at) a.dirty=1;
        a.book.selected=at;
    }
    if (library_load(&a,0)) return 2;
    qsort(a.games,(size_t)a.game_count,sizeof a.games[0],title_order);
    if (a.game_count) {
        /* Open on the requested title, else the remembered one, else the first. */
        int at=wanted?-1:0;
        for (int k=0;k<a.game_count;k++)
            if (!strcmp(a.games[k].slug,wanted?wanted:a.book.game)) at=k;
        if (at<0) { fprintf(stderr,"--select: no such game: %s\n",wanted); return 2; }
        select_game(&a,at,1);
    }
    char window_title[192]; snprintf(window_title,sizeof window_title,"%s - Settings",game_title(&a));
    int width=UI_W,height=UI_H;
    SDL_Rect usable;
    if (!SDL_GetDisplayUsableBounds(0,&usable)) {
        double scale=fmin(1.0,fmin((usable.w-32.0)/UI_W,(usable.h-48.0)/UI_H));
        scale=fmax(0.75,scale);
        width=(int)(UI_W*scale); height=(int)(UI_H*scale);
    }
    a.window=SDL_CreateWindow(window_title,SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,
                              width,height,SDL_WINDOW_RESIZABLE|SDL_WINDOW_ALLOW_HIGHDPI);
    if (!a.window) { fprintf(stderr,"launcher: %s\n",SDL_GetError()); return 1; }
    SDL_SetWindowMinimumSize(a.window,840,600);
    a.renderer=SDL_CreateRenderer(a.window,-1,SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
    if (!a.renderer) a.renderer=SDL_CreateRenderer(a.window,-1,SDL_RENDERER_SOFTWARE);
    if (!a.renderer || fonts(&a,font)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"Settings unavailable",
            "Could not create the renderer or load a system font. Try --font /path/to/font.ttf.",a.window);
        return 1;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY,"linear");
    SDL_RenderSetLogicalSize(a.renderer,UI_W,UI_H);
    controller_open(&a);
    fprintf(stderr,"launcher: preferences %s\n",a.path);
    if (check_startup && a.load_failed) {
        fprintf(stderr,"Cannot read saved presets: %s\n",a.status);
        return 2;
    }
    if (check_startup) {
        draw(&a); SDL_RenderPresent(a.renderer);
        SDL_RendererInfo info={0}; SDL_GetRendererInfo(a.renderer,&info);
        printf("Launcher startup OK: video=%s renderer=%s window=%dx%d controllers=%d movie_decoder=%d\n",
               SDL_GetCurrentVideoDriver(),info.name?info.name:"unknown",width,height,
               SDL_NumJoysticks(),a.movie_available);
        a.running=0;
    }
    if (a.load_failed) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"Cannot read saved presets",a.status,a.window);
    while (a.running) {
        if (!a.child) { draw(&a); SDL_RenderPresent(a.renderer); }
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e,30)) { event(&a,&e); while (SDL_PollEvent(&e)) event(&a,&e); }
        if (a.pad && !a.child && !a.modal) {
            int x=SDL_GameControllerGetAxis(a.pad,SDL_CONTROLLER_AXIS_LEFTX);
            int y=SDL_GameControllerGetAxis(a.pad,SDL_CONTROLLER_AXIS_LEFTY);
            Uint32 now=SDL_GetTicks();
            if ((abs(x)>18000 || abs(y)>18000) && SDL_TICKS_PASSED(now,a.stick_repeat)) {
                key(&a,abs(y)>abs(x)?(y<0?SDLK_UP:SDLK_DOWN):(x<0?SDLK_LEFT:SDLK_RIGHT),0);
                a.stick_repeat=now+180;
            }
        }
        import_poll(&a); poll_child(&a);
    }
    if (a.pad) SDL_GameControllerClose(a.pad);
    free(a.library_buffer);
    TTF_CloseFont(a.body); TTF_CloseFont(a.small); TTF_CloseFont(a.heading);
    SDL_DestroyRenderer(a.renderer); SDL_DestroyWindow(a.window); TTF_Quit(); SDL_Quit();
    return 0;
}
