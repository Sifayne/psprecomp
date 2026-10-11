/* The player's launcher: the window an app opens before a game, drawn with
 * the in-game menu's toolkit (ui.h) and its settings pages (pages.h). No
 * game, guest clock or GL backend runs here; the game's boot host gets the
 * preferences file when it starts.
 *
 * Its packs (psprecomp/host/launcher.h) are those it was linked with -- a
 * game's own development launcher has its pack -- and those installed in the
 * folder --packs names, each with the launcher.so the importer built from
 * the pack's sources (built/launcher.so). Every game is a tab: each pack's,
 * then those with no pack, which play as the plain recompiled game. The
 * side list has the player's pages, for every game, then the selected
 * game's pack's, then Packs and About. One preferences file holds them all (psprecomp/host/settings.h):
 * the launcher keeps it in memory with every section, edits the player's and
 * the selected pack's, and writes it whole. */
#include "psprecomp/host/launcher.h"
#include "psprecomp/host/settings.h"
#include "psprecomp/hle.h"
#include "pages.h"
#include "ui.h"
#include <SDL2/SDL.h>
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

enum { UI_W = 1120, UI_H = 800, MAX_GAMES = 32, MAX_FILES = 1024, MAX_PACKS = 16 };
enum { VIEW_SETTINGS, VIEW_BROWSE };
enum { GROUP_PLAYER, GROUP_PACK, GROUP_PACKS, GROUP_ABOUT };
enum { CONFIRM_NONE, CONFIRM_RESET, CONFIRM_DISCARD, CONFIRM_REMOVE, CONFIRM_STOP };
/* What the browser looks for, and what the importer is doing. */
enum { BROWSE_ISO, BROWSE_PACK };
enum { WORK_IMPORT, WORK_INSTALL, WORK_REMOVE };
/* UI units: the side list's width, what the side list and its page leave
 * below them for the status, the buttons and the hints, and what more while
 * the importer works, for its progress. */
static const float SIDE = 240, FOOTER = 104, WORK = 66;

/* One built title, from a --game argument or the importer's library: the
 * slug and name, the boot host to exec, its module, its disc (NULL or empty
 * for none) and its pack. The strings point into argv or the library's
 * buffer. */
typedef struct { const char *slug, *title, *boot, *module, *iso; int pack; } game_entry;
typedef struct { char name[256]; int directory; } browser_file;
/* A pack: linked, or loaded from its folder (handle). A removed one stays
 * loaded until the launcher closes, out of every list. */
typedef struct {
    const psp_launcher *info;
    const psp_settings_schema *settings;
    void *handle;
    char dir[1024];
    int removed;
} pack_entry;

static pack_entry packs[MAX_PACKS];
static int pack_count;
/* The player's options alone: what the launcher edits with no game. */
static const psp_settings_schema PLAYER_ONLY = { .title = "psprecomp" };

typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_GameController *pad;
    psp_settings_file *file;    /* every section, as read, with edits put back on a pack switch */
    psp_settings edit;          /* the player's and the selected pack's settings, being edited */
    int pack, editing;          /* edit's pack (-1: the player's alone); whether edit holds anything */
    char path[4096];
    const char *packs_dir;
    const char *boot, *module, *iso;
    game_entry games[MAX_GAMES]; int game_count, game;
    const char *library, *importer;
    char *library_buffer;
    browser_file files[MAX_FILES]; int file_count, browse;
    char browser_path[4096], browser_error[256];
    pid_t import_pid; int import_fd, import_cancelled, quit_after_import, work;
    char import_line[1024], import_status[1024]; size_t import_used;
    int view, group;
    const char *page;           /* the page shown, in group */
    int dirty, running, valid, movie_available, confirm, remove_pack;
    int load_failed;            /* a malformed file must never be overwritten */
    char status[PSP_SETTINGS_ERROR], validation[PSP_SETTINGS_ERROR];
    pid_t child; int child_error_fd;
    char child_error[2048]; size_t child_error_len;
} launcher;

/* The status line under the page, as printf would write it. */
__attribute__((format(printf, 2, 3)))
static void set_status(launcher *a, const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(a->status, sizeof a->status, format, args);
    va_end(args);
}

/* ---- packs ------------------------------------------------------------------------ */

static int pack_add(const psp_launcher *info, const psp_settings_schema *settings, void *handle, const char *dir) {
    for (int p = 0; p < pack_count; p++)
        if (!packs[p].removed && packs[p].settings->id && settings->id && !strcmp(packs[p].settings->id, settings->id)) {
            fprintf(stderr, "launcher: pack %s is here already; %s is left out\n", settings->id, dir && *dir ? dir : "the linked one");
            return -1;
        }
    if (pack_count == MAX_PACKS || !info || !settings || !settings->count || !settings->id) return -1;
    packs[pack_count++] = (pack_entry){ info, settings, handle, "", 0 };
    snprintf(packs[pack_count - 1].dir, sizeof packs[0].dir, "%s", dir ? dir : "");
    return 0;
}

/* An added pack: <packs>/<id>/built/launcher.so, which the importer built
 * for this API. */
static void pack_load(const char *dir) {
    for (int p = 0; p < pack_count; p++) if (!strcmp(packs[p].dir, dir)) return;
    char so[1100];
    snprintf(so, sizeof so, "%s/built/launcher.so", dir);
    if (access(so, R_OK)) return;
    void *h = dlopen(so, RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "launcher: cannot load the pack in %s: %s\n", dir, dlerror()); return; }
    const int *api = dlsym(h, "psp_pack_api");
    const psp_launcher *info = dlsym(h, "psp_launcher_info");
    const psp_settings_schema *settings = dlsym(h, "psp_title_settings");
    if (!api || *api != PSP_PACK_API || !info || !settings) {
        fprintf(stderr, "launcher: the pack in %s was built for another version of psprecomp (API %d, this is %d)\n",
                dir, api ? *api : -1, PSP_PACK_API);
        dlclose(h);
        return;
    }
    if (pack_add(info, settings, h, dir)) dlclose(h);
}

static int dir_order(const void *l, const void *r) { return strcmp(*(char *const *)l, *(char *const *)r); }

/* The linked packs, then those installed, by folder name. Again after an
 * install: only the new ones load. */
static void packs_load(launcher *a) {
    if (!pack_count)
        for (int i = 0; i < psp_pack_count; i++) pack_add(psp_packs[i].info, psp_packs[i].settings, NULL, NULL);
    if (!a->packs_dir) return;
    DIR *d = opendir(a->packs_dir);
    if (!d) return;
    char *names[MAX_PACKS * 2]; int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < (int)(sizeof names / sizeof names[0])) {
        if (e->d_name[0] == '.') continue;
        if ((names[n] = strdup(e->d_name))) n++;
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof names[0], dir_order);
    for (int i = 0; i < n; i++) {
        char dir[1024];
        if (snprintf(dir, sizeof dir, "%s/%s", a->packs_dir, names[i]) < (int)sizeof dir) pack_load(dir);
        free(names[i]);
    }
}

static const psp_settings_schema *pack_settings(int p) { return p >= 0 ? packs[p].settings : &PLAYER_ONLY; }

/* ---- settings ---------------------------------------------------------------------- */

static void movie_off(launcher *a) {
    char error[PSP_SETTINGS_ERROR];
    if (!a->movie_available && a->edit.source[PSP_OPT_MPEG_DECODE] != PSP_SOURCE_ENV)
        psp_settings_set(&a->edit, PSP_OPT_MPEG_DECODE, "0", PSP_SOURCE_FILE, error);
}

/* A path written into out: 0 when all of it fits, else -1. */
__attribute__((format(printf, 3, 4)))
static int format_path(char *out, size_t cap, const char *format, ...) {
    va_list args;
    va_start(args, format);
    const int n = vsnprintf(out, cap, format, args);
    va_end(args);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}

/* An XDG base folder with name under it: the variable when it is absolute,
 * else fallback under $HOME. */
static int xdg_path(const char *variable, const char *fallback, const char *name, char *out, size_t cap) {
    const char *given = getenv(variable), *home = getenv("HOME");
    if (given && given[0] == '/') return format_path(out, cap, "%s/%s", given, name);
    if (home && home[0] == '/') return format_path(out, cap, "%s/%s/%s", home, fallback, name);
    return -1;
}

/* A pack's earlier settings come in the first time it is used here, when
 * the file has no section for it: those of its own app (<XDG
 * config>/<pack id>/settings.ini) or of its game's own launcher (<XDG
 * data>/<its earlier name>/settings.ini), bringing the player's section too
 * when the file has none. The earlier files stay as they were; until Save,
 * so does this one, and the next start brings them in again. */
static void adopt_earlier(launcher *a, int p) {
    char candidates[2][4096], name[1100];
    snprintf(name, sizeof name, "%s/settings.ini", packs[p].settings->id);
    if (xdg_path("XDG_CONFIG_HOME", ".config", name, candidates[0], sizeof candidates[0])) candidates[0][0] = 0;
    snprintf(name, sizeof name, "%s/settings.ini", packs[p].info->earlier ? packs[p].info->earlier : "");
    if (!packs[p].info->earlier ||
        xdg_path("XDG_DATA_HOME", ".local/share", name, candidates[1], sizeof candidates[1])) candidates[1][0] = 0;
    for (int c = 0; c < 2; c++) {
        if (!candidates[c][0] || access(candidates[c], R_OK)) continue;
        char error[PSP_SETTINGS_ERROR];
        psp_settings_file *old = psp_settings_file_read(candidates[c], error);
        if (!old) { fprintf(stderr, "launcher: cannot bring in %s: %s\n", candidates[c], error); continue; }
        const int rc = psp_settings_file_adopt(a->file, old, packs[p].settings->id, error);
        psp_settings_file_free(old);
        if (rc) { fprintf(stderr, "launcher: cannot bring in %s: %s\n", candidates[c], error); continue; }
        fprintf(stderr, "launcher: brought in %s\n", candidates[c]);
        set_status(a, "Brought in your earlier settings for %.200s.", packs[p].info->name);
        return;
    }
}

/* Edit pack p's settings and the player's: what is being edited goes back
 * into the file first, so a switch loses nothing; a section the file does
 * not have yet comes from the pack's earlier settings, or else starts from
 * the play defaults. */
static void use_pack(launcher *a, int p) {
    char error[PSP_SETTINGS_ERROR];
    if (a->editing && !a->load_failed) psp_settings_file_put(a->file, &a->edit, error);
    a->pack = p;
    psp_settings_schema_use(pack_settings(p));
    if (p >= 0 && !a->load_failed && !psp_settings_file_has(a->file, 1)) adopt_earlier(a, p);
    const int had_player = psp_settings_file_has(a->file, 0), had_pack = psp_settings_file_has(a->file, 1);
    if (psp_settings_file_get(a->file, &a->edit, error)) {
        a->load_failed = 1;
        set_status(a, "Cannot read your settings, so they will not be saved: %.400s", error);
        psp_settings_play_defaults(&a->edit);
    } else {
        if (!had_player) { psp_settings_reset(&a->edit, 0); movie_off(a); }
        if (!had_pack && pack_settings(p)->count) psp_settings_reset(&a->edit, PSP_PLAYER_OPTIONS);
    }
    a->editing = 1;
    /* A page of the pack before stays only if this pack has it too. */
    if (a->group == GROUP_PACK) {
        const char *names[PAGES_MAX];
        const int n = pages_list(PSP_PLAYER_OPTIONS, psp_settings_count(), NULL, names, PAGES_MAX);
        int keep = 0;
        for (int i = 0; i < n; i++) if (a->page && !strcmp(names[i], a->page)) { a->page = names[i]; keep = 1; }
        if (!keep) { a->group = GROUP_PLAYER; a->page = NULL; }
    }
}

static void refresh(launcher *a) {
    psp_settings effective = a->edit;
    a->validation[0] = 0;
    a->valid = !psp_settings_env(&effective, a->validation) && !psp_settings_resolve(&effective, a->validation);
    if (a->valid && !a->movie_available && effective.number[PSP_OPT_MPEG_DECODE]) {
        strcpy(a->validation, "This build has no movie decoder. Turn the intro movie off, or remove its environment override.");
        a->valid = 0;
    }
}

static void changed(launcher *a) { a->dirty = 1; a->status[0] = 0; refresh(a); }

/* ---- games ------------------------------------------------------------------------- */

static const char *game_title(const launcher *a) { return a->game_count ? a->games[a->game].title : "psprecomp"; }

static void select_game(launcher *a, int at) {
    if (at < 0 || at >= a->game_count) return;
    const game_entry *g = &a->games[at];
    a->game = at;
    a->boot = g->boot && *g->boot ? g->boot : NULL;
    a->module = g->module;
    a->iso = g->iso && *g->iso ? g->iso : NULL;
    char error[PSP_SETTINGS_ERROR];
    psp_settings_file_set_game(a->file, g->slug, error);
    if (g->pack != a->pack) use_pack(a, g->pack);
    refresh(a);
}

/* The importer at work -- preparing a game, adding or removing a pack. The
 * settings stay open meanwhile, with its progress under them; what would
 * start other work, or play, waits for it. */
static int working(const launcher *a) { return a->import_pid != 0; }

/* ---- the library, the browser and the importer ------------------------------------- *
 *
 * The importer's game library, the ISO browser's folder listing and the
 * import it starts; the drawing is below. */
static void activate_back(launcher *a);

/* A game's pack: the first here whose titles name its slug, else -1. */
static int pack_of(const char *slug) {
    for (int p = 0; p < pack_count; p++)
        for (const char *const *t = packs[p].info->titles; !packs[p].removed && t && *t; t++)
            if (!strcmp(*t, slug)) return p;
    return -1;
}
/* The packs in order, each pack's titles in its order, then the games with
 * no pack, by title. */
static int title_rank(const char *slug) {
    const int p = pack_of(slug);
    if (p < 0) return 1 << 20;
    int at = 0;
    for (const char *const *t = packs[p].info->titles; strcmp(*t, slug); t++) at++;
    return p * 256 + at;
}
static int title_order(const void *left,const void *right) {
    const game_entry *a=left,*b=right; int x=title_rank(a->slug),y=title_rank(b->slug);
    if (x!=y) return x-y;
    const int by_title=strcasecmp(a->title,b->title);
    return by_title?by_title:strcmp(a->slug,b->slug);
}
static char *library_field(char **cursor,char *end) {
    if (*cursor>=end) return NULL;
    char *value=*cursor,*stop=memchr(value,0,(size_t)(end-value));
    if (!stop || stop-value>=4096) return NULL;
    *cursor=stop+1; return value;
}
/* The importer's list (LRLIB1: the selected slug, then slug, title, boot
 * host, module and disc per game; NUL-separated). After an import it opens
 * on the game just prepared. */
static int library_load(launcher *a,int imported) {
    if (!a->library) return 0;
    FILE *f=fopen(a->library,"rb");
    if (!f) { if (errno==ENOENT) return 0; set_status(a, "Cannot read game library: %s",strerror(errno)); return -1; }
    size_t cap=MAX_GAMES*5*4096+128;
    char *data=malloc(cap); if (!data) { fclose(f); return -1; }
    size_t bytes=fread(data,1,cap,f); int bad=ferror(f) || !feof(f); fclose(f);
    char *cursor=data,*end=data+bytes;
    char *magic=library_field(&cursor,end),*wanted=library_field(&cursor,end);
    game_entry entries[MAX_GAMES]; int count=0;
    if (bad || !magic || strcmp(magic,"LRLIB1") || !wanted) goto invalid;
    while (cursor<end) {
        if (count==MAX_GAMES) goto invalid;
        game_entry *g=&entries[count];
        const char **fields[]={&g->slug,&g->title,&g->boot,&g->module,&g->iso};
        for (int k=0;k<5;k++) if (!(*fields[k]=library_field(&cursor,end))) goto invalid;
        if (!*g->slug || !*g->title || !*g->module || !*g->iso ||
            !psp_settings_name_valid(g->slug)) goto invalid;
        /* A game no pack here has plays as the plain recompiled game, with
         * the settings for every game alone (stage 11). */
        g->pack=pack_of(g->slug);
        count++;
    }
    const char *selected=imported && *wanted?wanted:a->file?psp_settings_file_game(a->file):"";
    int at=0;
    qsort(entries,(size_t)count,sizeof entries[0],title_order);
    for (int k=0;k<count;k++) if (!strcmp(entries[k].slug,selected)) at=k;
    free(a->library_buffer); a->library_buffer=data;
    memcpy(a->games,entries,(size_t)count*sizeof entries[0]); a->game_count=count;
    if (count) select_game(a,at);
    else {
        /* The launch targets pointed into the buffer just freed. A valid but
         * empty library leaves nothing to launch, so say so rather than keep
         * dangling strings that draw() and launch_game() would read. */
        a->game=0; a->boot=NULL; a->module=NULL; a->iso=NULL;
    }
    return 0;
invalid:
    free(data); set_status(a, "Cannot read game library. Installed files have not been changed."); return -1;
}

static int file_order(const void *left,const void *right) {
    const browser_file *a=left,*b=right;
    return a->directory!=b->directory?b->directory-a->directory:strcasecmp(a->name,b->name);
}
static void browser_scan(launcher *a,const char *path) {
    char resolved[4096];
    if (!realpath(path,resolved)) { snprintf(a->browser_error,sizeof a->browser_error,"Cannot open folder: %s",strerror(errno)); return; }
    DIR *dir=opendir(resolved);
    if (!dir) { snprintf(a->browser_error,sizeof a->browser_error,"Cannot open folder: %s",strerror(errno)); return; }
    snprintf(a->browser_path,sizeof a->browser_path,"%s",resolved);
    a->file_count=0; a->browser_error[0]=0;
    struct dirent *entry;
    while ((entry=readdir(dir))) {
        if (entry->d_name[0]=='.') continue;
        char full[4096]; struct stat st;
        if (snprintf(full,sizeof full,"%s/%s",resolved,entry->d_name)>=(int)sizeof full || stat(full,&st)) continue;
        const char *ext=strrchr(entry->d_name,'.');
        if (!S_ISDIR(st.st_mode) && !(S_ISREG(st.st_mode) && ext && !strcasecmp(ext,a->browse==BROWSE_PACK?".zip":".iso"))) continue;
        if (a->file_count==MAX_FILES) { snprintf(a->browser_error,sizeof a->browser_error,"This folder has too many entries. Open a smaller folder."); break; }
        browser_file *item=&a->files[a->file_count++];
        snprintf(item->name,sizeof item->name,"%s",entry->d_name); item->directory=S_ISDIR(st.st_mode);
    }
    closedir(dir); qsort(a->files,(size_t)a->file_count,sizeof a->files[0],file_order);
    psp_ui_focus_next();
}
static void browser_open(launcher *a,int kind) {
    if (!a->importer || working(a)) return;
    a->view=VIEW_BROWSE; a->browse=kind;
    browser_scan(a,*a->browser_path?a->browser_path:getenv("HOME")?getenv("HOME"):"/");
}
static void browser_up(launcher *a) {
    char path[4096]; snprintf(path,sizeof path,"%s",a->browser_path);
    char *slash=strrchr(path,'/'); if (slash) { if (slash==path) slash[1]=0; else *slash=0; }
    browser_scan(a,path);
}
/* The importer at work: preparing a game from its disc, adding a pack from
 * its file, or removing one by its id. */
static void import_start(launcher *a,int work,const char *what) {
    if (!a->importer || a->import_pid || a->child) return;
    static const char *const commands[]={"import","install-pack","remove-pack"};
    int pipes[2];
    if (pipe(pipes)) { set_status(a, "Cannot prepare game: %s",strerror(errno)); return; }
    /* SDL installs a SIGTERM handler. Block cancellation across fork until
     * the child has replaced that inherited handler, so an immediate Cancel
     * cannot be consumed as an SDL event in the child before exec. */
    sigset_t blocked,previous;
    sigemptyset(&blocked); sigaddset(&blocked,SIGTERM);
    sigprocmask(SIG_BLOCK,&blocked,&previous);
    fflush(NULL); pid_t pid=fork();
    if (pid<0) { sigprocmask(SIG_SETMASK,&previous,NULL); close(pipes[0]); close(pipes[1]); return; }
    if (!pid) {
        setpgid(0,0); signal(SIGTERM,SIG_DFL); sigprocmask(SIG_SETMASK,&previous,NULL); close(pipes[0]);
        dup2(pipes[1],STDOUT_FILENO); dup2(pipes[1],STDERR_FILENO); close(pipes[1]);
        execl(a->importer,a->importer,commands[work],what,(char *)NULL); _exit(127);
    }
    setpgid(pid,pid); sigprocmask(SIG_SETMASK,&previous,NULL);
    close(pipes[1]); fcntl(pipes[0],F_SETFL,O_NONBLOCK);
    a->import_pid=pid; a->import_fd=pipes[0]; a->import_used=0;
    a->import_cancelled=0; a->work=work; a->view=VIEW_SETTINGS;
    snprintf(a->import_status,sizeof a->import_status,"%s",work==WORK_IMPORT?"Checking the selected ISO...":
             work==WORK_INSTALL?"Checking the pack...":"Removing the pack...");
}
static void import_cancel(launcher *a) {
    if (!a->import_pid) return;
    kill(-a->import_pid,SIGTERM); a->import_cancelled=1;
    snprintf(a->import_status,sizeof a->import_status,"Canceling preparation...");
}
static void browser_enter(launcher *a,int at) {
    if (at<0 || at>=a->file_count) return;
    char full[4096]; browser_file *item=&a->files[at];
    if (snprintf(full,sizeof full,"%s/%s",a->browser_path,item->name)>=(int)sizeof full) return;
    if (item->directory) browser_scan(a,full); else import_start(a,a->browse==BROWSE_PACK?WORK_INSTALL:WORK_IMPORT,full);
}
static void import_poll(launcher *a) {
    if (!a->import_pid) return;
    char buffer[512]; ssize_t n;
    while ((n=read(a->import_fd,buffer,sizeof buffer))>0) for (ssize_t i=0;i<n;i++) {
        if (buffer[i]=='\n') {
            a->import_line[a->import_used]=0;
            if (a->import_used && !a->import_cancelled) snprintf(a->import_status,sizeof a->import_status,"%s",a->import_line);
            a->import_used=0;
        } else if (a->import_used+1<sizeof a->import_line) a->import_line[a->import_used++]=buffer[i];
    }
    int status; if (waitpid(a->import_pid,&status,WNOHANG)<=0) return;
    close(a->import_fd); a->import_pid=0;
    const int ok=!a->import_cancelled && WIFEXITED(status) && WEXITSTATUS(status)==0;
    if (a->import_cancelled) set_status(a, "%s",a->work==WORK_IMPORT?
        "Preparation canceled. Existing games and saves are unchanged.":"Canceled. Your packs are unchanged.");
    else if (!ok) set_status(a, "%.250s",a->import_status);
    else if (a->work==WORK_IMPORT) {
        if (!library_load(a,1)) set_status(a, "Your game is ready. Choose Save and play.");
    } else if (a->work==WORK_INSTALL) {
        const int before=pack_count;
        packs_load(a);
        library_load(a,0);
        if (pack_count>before) set_status(a, "Added %.200s. Add a game of it with Add game.",packs[pack_count-1].info->name);
        else set_status(a, "The pack was added, but this launcher could not load it. See the log.");
    } else {
        packs[a->remove_pack].removed=1;
        if (a->pack==a->remove_pack) use_pack(a,-1);
        library_load(a,0);
        set_status(a, "Removed %.200s.",packs[a->remove_pack].info->name);
    }
    if (a->quit_after_import) { a->quit_after_import=0; activate_back(a); }
}

static void pack_remove(launcher *a,int p) {
    if (p<0 || p>=pack_count || !packs[p].handle) return;
    a->remove_pack=p;
    import_start(a,WORK_REMOVE,packs[p].settings->id);
}

/* ---- saving and launching ------------------------------------------------------------ */

static int make_directories(const char *path) {
    char work[4096]; size_t len = strlen(path);
    if (!len || len >= sizeof work) { errno = ENAMETOOLONG; return -1; }
    memcpy(work, path, len + 1);
    for (char *p = work + 1; *p; p++) if (*p == '/') {
        *p = 0; if (mkdir(work, 0755) && errno != EEXIST) return -1; *p = '/';
    }
    if (mkdir(work, 0755) && errno != EEXIST) return -1;
    struct stat st;
    if (stat(path, &st)) return -1;
    if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
    return 0;
}

static int save(launcher *a) {
    if (a->load_failed) return -1;
    char dir[4096];
    snprintf(dir, sizeof dir, "%s", a->path);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) { *slash = 0; make_directories(dir); }
    if (psp_settings_file_put(a->file, &a->edit, a->status) || psp_settings_file_write(a->file, a->path, a->status))
        return -1;
    a->dirty = 0;
    set_status(a, "Settings saved.");
    return 0;
}

/* The game runs from a per-title save folder, <data root>/saves/<slug>, so a
 * source build and the packaged app write saves to the same place and one
 * folder can be synced between computers (docs/SAVE-SYNC.md). The data root
 * follows AppRun's rule: $PSPRECOMP_DATA_ROOT, else
 * $XDG_DATA_HOME/psprecomp, else ~/.local/share/psprecomp. */
static int data_root(char *out, size_t cap) {
    const char *given = getenv("PSPRECOMP_DATA_ROOT");
    if (given && given[0] == '/') return format_path(out, cap, "%s", given);
    return xdg_path("XDG_DATA_HOME", ".local/share", "psprecomp", out, cap);
}
/* Empty when no title is selected; -1 when no data root can be found. Save
 * states sit beside the saves and not among them: a state loads only into
 * the build that wrote it, so it has no business in a folder synced between
 * computers. The game makes it on its first save (psp_state_dir). */
static int game_folder(launcher *a, const char *kind, char *out, size_t cap) {
    char root[4096]; out[0] = 0;
    if (!a->game_count) return 0;
    if (data_root(root, sizeof root)) return -1;
    return format_path(out, cap, "%s/%s/%s", root, kind, a->games[a->game].slug);
}
/* A launch path in absolute form, so it still resolves after the child
 * changes directory. Nothing is canonicalised; the spelling is kept. */
static int absolute(const char *path, char *out, size_t cap) {
    if (path[0] == '/') return format_path(out, cap, "%s", path);
    char cwd[4096];
    if (!getcwd(cwd, sizeof cwd)) return -1;
    return format_path(out, cap, "%s/%s", cwd, path);
}

/* Whether the selected game can start now. When it cannot, the status says
 * why -- or the importer or the browser opens to prepare it. */
static int launch_ready(launcher *a) {
    refresh(a);
    if (working(a)) {
        set_status(a, "%s", a->work == WORK_IMPORT ? "Wait for the game to be prepared, or stop it, to play."
                                                 : "Wait for the pack to finish, to play.");
        return 0;
    }
    if (!a->valid || a->load_failed) return 0;
    if (!a->boot || !a->module) {
        if (a->importer && a->iso && !access(a->iso, R_OK)) import_start(a, WORK_IMPORT, a->iso);
        else if (a->importer) browser_open(a, BROWSE_ISO);
        else set_status(a, "No game installed. You can still save your settings.");
        return 0;
    }
    if (access(a->boot, X_OK) || access(a->module, R_OK) || (a->iso && access(a->iso, R_OK))) {
        set_status(a, "Cannot reach the game, its module or its disc: %s", strerror(errno)); return 0;
    }
    psp_settings effective = a->edit;
    char error[PSP_SETTINGS_ERROR];
    if (!psp_settings_env(&effective, error) && !psp_settings_resolve(&effective, error) && effective.render == PSP_RENDERER_NULL) {
        set_status(a, "The null renderer is for diagnostics. Choose OpenGL or software before playing."); return 0;
    }
    return 1;
}

/* What the game host is started with: its paths in absolute form, and the
 * save folder it runs in (empty when no title is selected). */
typedef struct { char save_root[4096], boot[4096], module[4096], iso[4096], config[4096]; } launch_paths;

static int find_launch_paths(launcher *a, launch_paths *p) {
    if (game_folder(a, "saves", p->save_root, sizeof p->save_root)) {
        set_status(a, "Cannot find the save folder: PSPRECOMP_DATA_ROOT, XDG_DATA_HOME or HOME must be an absolute path."); return -1;
    }
    if (*p->save_root && make_directories(p->save_root)) {
        set_status(a, "Cannot make the save folder %.300s: %s", p->save_root, strerror(errno)); return -1;
    }
    if (absolute(a->boot, p->boot, sizeof p->boot) || absolute(a->module, p->module, sizeof p->module) ||
        (a->iso && absolute(a->iso, p->iso, sizeof p->iso)) || absolute(a->path, p->config, sizeof p->config)) {
        set_status(a, "Cannot launch: a game path is too long."); return -1;
    }
    return 0;
}

/* The game's environment, with its state folder: built before the fork,
 * since only async-signal-safe calls may follow it. NULL when out of memory. */
static char **child_environment(launcher *a) {
    static char state_var[4200];
    char state_root[4096];
    size_t nenv = 0;
    while (environ[nenv]) nenv++;
    char **env = malloc((nenv + 2) * sizeof *env);
    if (!env) return NULL;
    size_t k = 0;
    for (size_t i = 0; i < nenv; i++) if (strncmp(environ[i], "PSPRECOMP_STATE_DIR=", 20)) env[k++] = environ[i];
    if (!game_folder(a, "states", state_root, sizeof state_root) && *state_root) {
        snprintf(state_var, sizeof state_var, "PSPRECOMP_STATE_DIR=%s", state_root);
        env[k++] = state_var;
        fprintf(stderr, "launcher: states %s\n", state_root);
    }
    env[k] = NULL;
    return env;
}

/* The child's side of the fork: the game host in the save folder, its
 * stderr into the pipe. Only async-signal-safe operations between fork and
 * exec: SDL may have other threads with libc locks held at the fork
 * boundary. */
static _Noreturn void exec_game(const launch_paths *p, int with_iso, char **env, const int pipes[2]) {
    close(pipes[0]); dup2(pipes[1], STDERR_FILENO); close(pipes[1]);
    const char *args[8]; int n = 0;
    args[n++] = p->boot; args[n++] = p->module; if (with_iso) args[n++] = p->iso;
    args[n++] = "--config"; args[n++] = p->config;
    args[n++] = "--window"; args[n] = NULL;
    static const char exec_failed[] = "Could not execute the game host. Check its path and permissions.\n",
                      chdir_failed[] = "Could not enter the save folder. Check its permissions.\n";
    const char *message = exec_failed; size_t sent = 0, total = sizeof exec_failed - 1;
    if (*p->save_root && chdir(p->save_root)) { message = chdir_failed; total = sizeof chdir_failed - 1; }
    else execve(p->boot, (char *const *)args, env);
    while (sent < total) {
        ssize_t w = write(STDERR_FILENO, message + sent, total - sent);
        if (w > 0) sent += (size_t)w;
        else if (w < 0 && errno == EINTR) continue;
        else break;
    }
    _exit(127);
}

/* The game host, started with env; what it writes to stderr comes back
 * through a pipe for poll_child. */
static void spawn_game(launcher *a, const launch_paths *p, char **env) {
    int pipes[2];
    if (pipe(pipes)) { set_status(a, "Cannot launch: %s", strerror(errno)); return; }
    fflush(NULL);
    pid_t child = fork();
    if (child < 0) { close(pipes[0]); close(pipes[1]); set_status(a, "Cannot launch: %s", strerror(errno)); return; }
    if (!child) exec_game(p, a->iso != NULL, env, pipes);
    close(pipes[1]); fcntl(pipes[0], F_SETFL, O_NONBLOCK);
    a->child = child; a->child_error_fd = pipes[0]; a->child_error_len = 0; a->child_error[0] = 0;
    SDL_HideWindow(a->window);
}

static void launch_game(launcher *a) {
    if (!launch_ready(a) || save(a)) return;
    launch_paths p;
    if (find_launch_paths(a, &p)) return;
    if (*p.save_root) fprintf(stderr, "launcher: saves %s\n", p.save_root);
    char **env = child_environment(a);
    if (!env) { set_status(a, "Cannot launch: out of memory."); return; }
    spawn_game(a, &p, env);
    free(env);
}

/* The file again, as the game's menu may have left it: unless something
 * here is unsaved, it is the truth now. */
static void reread(launcher *a) {
    if (a->dirty || a->load_failed) return;
    char error[PSP_SETTINGS_ERROR];
    psp_settings_file *fresh = psp_settings_file_read(a->path, error);
    if (!fresh) return;
    psp_settings_file_free(a->file);
    a->file = fresh;
    a->editing = 0;
    use_pack(a, a->pack);
    refresh(a);
}

/* The end of what the game has written to stderr, for the message box if
 * it fails: the newest bytes of child_error and buf together, as many as
 * child_error holds with its terminator. */
static void keep_child_error(launcher *a, const char *buf, size_t got) {
    const size_t room = sizeof a->child_error - 1;
    size_t keep = a->child_error_len;
    if (keep + got > room) {            /* the oldest bytes make way */
        const size_t drop = keep + got - room < keep ? keep + got - room : keep;
        memmove(a->child_error, a->child_error + drop, keep - drop);
        keep -= drop;
    }
    const size_t take = got < room - keep ? got : room - keep;     /* buf's newest */
    memcpy(a->child_error + keep, buf + got - take, take);
    a->child_error_len = keep + take;
    a->child_error[a->child_error_len] = 0;
}

static void poll_child(launcher *a) {
    if (!a->child) return;
    char buf[512]; ssize_t got;
    while ((got = read(a->child_error_fd, buf, sizeof buf)) > 0) {
        fwrite(buf, 1, (size_t)got, stderr);
        keep_child_error(a, buf, (size_t)got);
    }
    int status; pid_t done = waitpid(a->child, &status, WNOHANG);
    if (done <= 0) return;
    close(a->child_error_fd); a->child = 0;
    reread(a);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) { a->running = 0; return; }
    SDL_ShowWindow(a->window); SDL_RaiseWindow(a->window);
    set_status(a, "The game stopped (%s %d). See the log for details.",
             WIFEXITED(status) ? "exit" : "signal", WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status));
    char title[192]; snprintf(title, sizeof title, "%s could not start", game_title(a));
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, *a->child_error ? a->child_error : a->status, a->window);
}

/* ---- drawing ------------------------------------------------------------------------ */

static const char ABOUT[] =
    "psprecomp plays PSP games recompiled to run natively: MIT license.\n\n"
    "Audio uses FFmpeg libraries, copyright the FFmpeg contributors, under the GNU LGPL version 2.1 "
    "or later. https://ffmpeg.org/ See licenses/ffmpeg/ for the license and notices, and the sources "
    "package alongside the release for matching FFmpeg source and build instructions. Compatible "
    "modified shared libraries may be substituted.\n\n"
    "The recompiled game code is produced on this computer from your own disc image. Original game "
    "code and assets belong to their rights holders.";

static void upper(const char *in, char *out, size_t size) {
    size_t i = 0;
    for (; in[i] && i + 1 < size; i++) out[i] = (char)toupper((unsigned char)in[i]);
    out[i] = 0;
}

/* The pages of a group, for the side list and the bumpers. */
static int group_pages(const launcher *a, int group, const char **names) {
    if (group == GROUP_PLAYER) return pages_list(0, PSP_PLAYER_OPTIONS, NULL, names, PAGES_MAX);
    if (group == GROUP_PACK) return a->pack >= 0 ? pages_list(PSP_PLAYER_OPTIONS, psp_settings_count(), NULL, names, PAGES_MAX) : 0;
    names[0] = group == GROUP_PACKS ? "Packs" : "About";
    return 1;
}

static void show_page(launcher *a, int group, const char *page) {
    a->group = group; a->page = page;
}

/* Every page in side-list order, and the one shown's place among them. */
static int page_ring(const launcher *a, int *groups, const char **names, int *at) {
    int n = 0; *at = 0;
    for (int g = GROUP_PLAYER; g <= GROUP_ABOUT; g++) {
        const char *list[PAGES_MAX];
        const int count = group_pages(a, g, list);
        for (int i = 0; i < count && n < 4 * PAGES_MAX; i++) {
            if (g == a->group && a->page && !strcmp(list[i], a->page)) *at = n;
            groups[n] = g; names[n++] = list[i];
        }
    }
    return n;
}

static void step_page(launcher *a, int direction) {
    int groups[4 * PAGES_MAX], at;
    const char *names[4 * PAGES_MAX];
    const int n = page_ring(a, groups, names, &at);
    if (!n) return;
    at = (at + direction + n) % n;
    show_page(a, groups[at], names[at]);
    psp_ui_focus_next();
}

static void settings_page(launcher *a) {
    const int from = a->group == GROUP_PLAYER ? 0 : PSP_PLAYER_OPTIONS;
    const int to = a->group == GROUP_PLAYER ? PSP_PLAYER_OPTIONS : psp_settings_count();
    char line[160];
    psp_ui_heading(a->page);
    if (a->group == GROUP_PLAYER) psp_ui_note("For every game.");
    else { snprintf(line, sizeof line, "For %s.", pack_settings(a->pack)->title); psp_ui_note(line); }
    psp_ui_separator();
    const pages_context rows = { .menu = 0, .movie_available = a->movie_available };
    for (int k = from; k < to; k++) {
        if (!pages_on(k, a->page)) continue;
        char value[PSP_SETTINGS_VALUE];
        if (pages_option(&a->edit, k, &rows, value, sizeof value)) {
            if (psp_settings_set(&a->edit, k, value, PSP_SOURCE_FILE, a->status)) continue;
            changed(a);
        }
    }
}

static void packs_page(launcher *a) {
    psp_ui_heading("Packs");
    psp_ui_note("A pack makes its games play well -- their controls, fixes and own settings -- where "
                "a game without one plays as the plain recompiled game. Add one from its .zip file; "
                "it is built for this computer as it is added.");
    int shown = 0;
    for (int p = 0; p < pack_count; p++) {
        if (packs[p].removed) continue;
        shown++;
        psp_ui_separator();
        psp_ui_accent(packs[p].info->name);
        char games[512] = "";
        for (int t = 0; packs[p].info->titles && packs[p].info->titles[t]; t++) {
            const char *name = packs[p].info->names && packs[p].info->names[t] ? packs[p].info->names[t] : packs[p].info->titles[t];
            size_t used = strlen(games);
            snprintf(games + used, sizeof games - used, "%s%s", used ? ", " : "Games: ", name);
        }
        psp_ui_text(games);
        if (!packs[p].handle) psp_ui_note("Part of this launcher.");
        else if (a->importer) {
            char label[64];
            snprintf(label, sizeof label, "Remove##%d", p);
            psp_ui_disabled_begin(working(a));
            if (psp_ui_button(label)) { a->remove_pack = p; a->confirm = CONFIRM_REMOVE; }
            psp_ui_disabled_end();
            psp_ui_help("Remove this pack. Its games stay in your library, ready again when the pack is added back; "
                        "your saves and settings are kept.");
        }
    }
    psp_ui_separator();
    if (!shown) psp_ui_text("No packs yet.");
    if (a->importer && a->packs_dir) {
        psp_ui_disabled_begin(working(a));
        if (psp_ui_button("Add pack...")) browser_open(a, BROWSE_PACK);
        psp_ui_disabled_end();
    }
}

static void about_page(void) {
    psp_ui_heading("About");
    psp_ui_text(ABOUT);
    for (int p = 0; p < pack_count; p++) {
        if (packs[p].removed || !packs[p].info->about) continue;
        psp_ui_separator();
        psp_ui_accent(packs[p].info->name);
        psp_ui_text(packs[p].info->about);
    }
}

static void browse_view(launcher *a) {
    psp_ui_heading(a->browse == BROWSE_PACK ? "Add pack" : "Add game");
    psp_ui_note(a->browse == BROWSE_PACK ? "Choose the pack's .zip file." :
                "Choose your PSP .iso. You can also drop it on the window, or paste its path with Ctrl+V.");
    if (psp_ui_button("Up")) browser_up(a);
    psp_ui_same_line();
    if (psp_ui_button("Home")) browser_scan(a, getenv("HOME") ? getenv("HOME") : "/");
    psp_ui_same_line();
    if (psp_ui_button("Drives")) browser_scan(a, access("/run/media", R_OK) ? "/" : "/run/media");
    psp_ui_same_line();
    psp_ui_text(a->browser_path);
    psp_ui_scroll_begin("##files", FOOTER + 8);
    int enter = -1;
    for (int i = 0; i < a->file_count; i++) {
        char label[300];
        snprintf(label, sizeof label, "%s%s##%d", a->files[i].directory ? "[Folder]  " : "", a->files[i].name, i);
        if (psp_ui_nav(label, 0)) enter = i;
    }
    if (!a->file_count)
        psp_ui_note(a->browse == BROWSE_PACK ? "No folders or .zip files here." : "No folders or .iso files here.");
    psp_ui_scroll_end();
    if (*a->browser_error) psp_ui_accent(a->browser_error);
    if (enter >= 0) browser_enter(a, enter);
}

/* The importer's progress, under the settings: what it is doing, how far
 * the game's compile has got (it says "Compiling game: 12/32"; before and
 * after that the bar moves with no measure), and Stop. */
static void work_strip(launcher *a) {
    static const char *const doing[] = { "Preparing your game", "Adding the pack", "Removing the pack" };
    char line[1100];
    snprintf(line, sizeof line, "%s: %s", doing[a->work], a->import_status);
    psp_ui_accent(line);
    int done = 0, all = 0;
    const float fraction = sscanf(a->import_status, "Compiling game: %d/%d", &done, &all) == 2 && all > 0
                         ? (float)done / (float)all : -1.0f;
    const int stoppable = a->work != WORK_REMOVE && !a->import_cancelled;
    psp_ui_progress(fraction, NULL, stoppable ? psp_ui_button_width("Stop") + 10 : 0);
    if (stoppable) {
        psp_ui_same_line();
        if (psp_ui_button("Stop")) import_cancel(a);
        psp_ui_help(a->work == WORK_IMPORT ? "Stop preparing. Your games and saves stay as they were."
                                           : "Stop adding the pack. Your packs stay as they were.");
    }
}

/* What the status line says: a problem first, then the last thing done. */
static const char *status_line(const launcher *a) {
    if (a->load_failed) return a->status;
    if (!a->valid) return a->validation;
    if (*a->status) return a->status;
    return a->dirty ? "Unsaved changes." : "Changes take effect when the game starts.";
}

static void footer(launcher *a) {
    if (working(a)) work_strip(a);
    if (a->load_failed || !a->valid) psp_ui_accent(status_line(a)); else psp_ui_note(status_line(a));
    if (a->view == VIEW_SETTINGS) {
        /* Packs and About have nothing to reset; the row keeps its place. */
        if (a->group > GROUP_PACK) psp_ui_gap("Reset to defaults");
        else if (psp_ui_button("Reset to defaults")) a->confirm = CONFIRM_RESET;
        const int ready = a->boot && a->module, prepare = !ready && a->game_count && a->importer;
        const char *play = working(a) && a->work == WORK_IMPORT ? "Preparing..." :
                           ready ? "Save and play" : prepare ? "Prepare game" : NULL;
        const float w = psp_ui_button_width("Cancel") + psp_ui_button_width("Save") +
                        (play ? psp_ui_button_width(play) + 10 : 0) + 10;
        psp_ui_right(w);
        if (psp_ui_button("Cancel")) activate_back(a);
        psp_ui_same_line();
        if (psp_ui_button("Save")) save(a);
        if (play) {
            psp_ui_same_line();
            psp_ui_disabled_begin(working(a));
            if (psp_ui_button_primary(play)) launch_game(a);
            psp_ui_disabled_end();
        }
        psp_ui_note("Arrows or D-pad: move   Enter or A: choose   Esc or B: back   Bumpers: pages   Start: play");
    } else {
        psp_ui_right(psp_ui_button_width("Cancel"));
        if (psp_ui_button("Cancel")) activate_back(a);
        psp_ui_note("Arrows or D-pad: move   Enter or A: open   Esc or B: back");
    }
}

static void confirm_prompt(launcher *a) {
    if (!a->confirm) { psp_ui_confirm_close(); return; }
    char text[512];
    const char *title = "", *yes = "";
    if (a->confirm == CONFIRM_RESET && a->group == GROUP_PLAYER) {
        title = "Reset to defaults?"; yes = "Reset";
        snprintf(text, sizeof text, "Put every setting for all games back to its default. This changes them for every game. "
                 "Nothing changes until you save.");
    } else if (a->confirm == CONFIRM_RESET) {
        title = "Reset to defaults?"; yes = "Reset";
        snprintf(text, sizeof text, "Put every %s setting back to its default. Bindings made in the game's menu stay as "
                 "they are. Nothing changes until you save.", pack_settings(a->pack)->title);
    } else if (a->confirm == CONFIRM_DISCARD) {
        title = "Discard your changes?"; yes = "Discard";
        snprintf(text, sizeof text, "Your unsaved changes will be lost. The saved settings stay as they were.");
    } else if (a->confirm == CONFIRM_STOP) {
        title = a->work == WORK_IMPORT ? "Stop preparing?" : "Stop adding the pack?"; yes = "Stop and close";
        snprintf(text, sizeof text, "%s", a->work == WORK_IMPORT
                 ? "Closing the launcher stops preparing your game. Your games and saves stay as they were."
                 : "Closing the launcher stops adding the pack. Your packs stay as they were.");
    } else {
        title = "Remove the pack?"; yes = "Remove";
        snprintf(text, sizeof text, "Remove %s? Its games can't be played until it is added again. Your saves and settings are kept.",
                 packs[a->remove_pack].info->name);
    }
    const int answer = psp_ui_confirm(title, text, yes, "Cancel");
    if (answer < 0) return;
    const int kind = a->confirm;
    a->confirm = CONFIRM_NONE;
    if (!answer) return;
    if (kind == CONFIRM_RESET) {
        psp_settings_reset(&a->edit, a->group == GROUP_PLAYER ? 0 : PSP_PLAYER_OPTIONS);
        if (a->group == GROUP_PLAYER) movie_off(a);
        changed(a);
        set_status(a, "Reset. Save to keep it.");
    } else if (kind == CONFIRM_DISCARD) a->running = 0;
    else if (kind == CONFIRM_STOP) { a->quit_after_import = 1; import_cancel(a); }
    else pack_remove(a, a->remove_pack);
}

/* The title at the top, and Add game beside it. */
static void title_bar(launcher *a) {
    psp_ui_accent("PSPRECOMP");
    char heading[160];
    upper(game_title(a), heading, sizeof heading);
    psp_ui_heading(heading);
    if (a->game_count && a->games[a->game].pack < 0)
        psp_ui_note("No pack: the plain recompiled game, with the settings for all games.");
    if (a->importer && a->view == VIEW_SETTINGS) {
        psp_ui_right(psp_ui_button_width("Add game"));
        psp_ui_disabled_begin(working(a));
        if (psp_ui_button_primary("Add game")) browser_open(a, BROWSE_ISO);
        psp_ui_disabled_end();
    }
}

/* A tab for each game, when there is more than one. */
static void game_tabs(launcher *a) {
    int pick = -1;
    for (int k = 0; k < a->game_count; k++) {
        char label[300];
        snprintf(label, sizeof label, "%s##game%d", a->games[k].title, k);
        if (k) psp_ui_same_line();
        if (psp_ui_tab(label, k == a->game) && k != a->game) pick = k;
    }
    if (pick >= 0) select_game(a, pick);
}

/* The side list of pages, group by group, and the page shown beside it. */
static void settings_view(launcher *a) {
    psp_ui_side_begin(SIDE, FOOTER + (working(a) ? WORK : 0));
    const char *names[PAGES_MAX];
    for (int g = GROUP_PLAYER; g <= GROUP_ABOUT; g++) {
        const int n = group_pages(a, g, names);
        if (!n) continue;
        if (g == GROUP_PLAYER) psp_ui_group("ALL GAMES");
        else if (g == GROUP_PACK) {
            char caption[160];
            upper(pack_settings(a->pack)->title, caption, sizeof caption);
            psp_ui_group(caption);
        } else if (g == GROUP_PACKS) psp_ui_separator();
        for (int i = 0; i < n; i++) {
            char label[160];
            snprintf(label, sizeof label, "%s##%d", names[i], g);
            if (psp_ui_nav(label, g == a->group && a->page && !strcmp(names[i], a->page))) show_page(a, g, names[i]);
        }
    }
    psp_ui_side_next();
    if (a->group == GROUP_PACKS) packs_page(a);
    else if (a->group == GROUP_ABOUT) about_page();
    else if (a->page) settings_page(a);
    psp_ui_help_area();
    psp_ui_side_end();
}

/* One frame, into the renderer; draw presents it. */
static void render(launcher *a) {
    refresh(a);
    if (!a->page) {
        const char *names[PAGES_MAX];
        a->group = GROUP_PLAYER;
        if (group_pages(a, GROUP_PLAYER, names)) a->page = names[0];
    }
    psp_ui_set_pad(a->pad);
    psp_ui_begin();
    psp_ui_screen_begin();
    title_bar(a);
    if (a->game_count > 1 && a->view == VIEW_SETTINGS) game_tabs(a);
    if (a->view == VIEW_SETTINGS) {
        settings_view(a);
    } else {
        psp_ui_scroll_begin("##work", FOOTER);
        browse_view(a);
        psp_ui_scroll_end();
    }
    footer(a);
    psp_ui_screen_end();
    confirm_prompt(a);
    psp_ui_end();
    SDL_SetRenderDrawColor(a->renderer, 16, 22, 29, 255);
    SDL_RenderClear(a->renderer);
    psp_ui_draw(a->renderer);
}

static void draw(launcher *a) {
    render(a);
    SDL_RenderPresent(a->renderer);
}

/* ---- input ------------------------------------------------------------------------- */

/* Escape, B or Cancel: out of the browser, or out of the launcher -- asking
 * first whether to stop the importer at work, and about unsaved changes. A
 * pack's removal is quick and not to be cut short: the launcher closes once
 * it is done. B on a page goes back to its entry in the list first
 * (psp_ui_back). */
static void activate_back(launcher *a) {
    if (a->view == VIEW_BROWSE) { a->view = VIEW_SETTINGS; psp_ui_focus_next(); return; }
    if (working(a)) {
        if (a->work == WORK_REMOVE) a->quit_after_import = 1;
        else a->confirm = CONFIRM_STOP;
        return;
    }
    if (a->dirty) a->confirm = CONFIRM_DISCARD;
    else a->running = 0;
}

static void controller_open(launcher *a) {
    if (a->pad && !SDL_GameControllerGetAttached(a->pad)) { SDL_GameControllerClose(a->pad); a->pad = NULL; }
    if (!a->pad) for (int i = 0; i < SDL_NumJoysticks(); i++) if (SDL_IsGameController(i)) {
        a->pad = SDL_GameControllerOpen(i); if (a->pad) break;
    }
}

static void event(launcher *a, const SDL_Event *e) {
    if (e->type == SDL_CONTROLLERDEVICEADDED || e->type == SDL_CONTROLLERDEVICEREMOVED) controller_open(a);
    if (a->child) return;
    if (e->type == SDL_QUIT) {
        if (a->import_pid) { a->quit_after_import = 1; import_cancel(a); }
        else { a->view = VIEW_SETTINGS; activate_back(a); }
        return;
    }
    if (e->type == SDL_DROPFILE) {
        const char *ext = strrchr(e->drop.file, '.');
        import_start(a, ext && !strcasecmp(ext, ".zip") ? WORK_INSTALL : WORK_IMPORT, e->drop.file);
        SDL_free(e->drop.file);
        return;
    }
    /* The screen's own keys, unless a popup -- a list opened, a question --
     * has them. */
    const int free_keys = !psp_ui_popup_open() && !a->confirm;
    if (e->type == SDL_KEYDOWN && !e->key.repeat && free_keys) {
        const SDL_Keycode k = e->key.keysym.sym;
        if (k == SDLK_ESCAPE) { activate_back(a); return; }
        if (k == SDLK_v && (e->key.keysym.mod & KMOD_CTRL) && a->view == VIEW_BROWSE) {
            char *path = SDL_GetClipboardText(); struct stat st;
            if (path && !stat(path, &st)) {
                if (S_ISDIR(st.st_mode)) browser_scan(a, path);
                else import_start(a, a->browse == BROWSE_PACK ? WORK_INSTALL : WORK_IMPORT, path);
            } else snprintf(a->browser_error, sizeof a->browser_error, "The pasted path could not be opened.");
            SDL_free(path);
            return;
        }
    }
    if (e->type == SDL_CONTROLLERBUTTONDOWN && a->pad &&
        e->cbutton.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(a->pad)) && free_keys) {
        switch (e->cbutton.button) {
        case SDL_CONTROLLER_BUTTON_B: if (!psp_ui_back()) activate_back(a); return;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            if (a->view == VIEW_SETTINGS) step_page(a, e->cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER ? -1 : 1);
            return;
        case SDL_CONTROLLER_BUTTON_START:
            if (a->view == VIEW_SETTINGS) launch_game(a);
            return;
        default: break;
        }
    }
    psp_ui_event(e);
}

/* ---- starting ----------------------------------------------------------------------- */

static int usage(void) {
    puts("launcher [--config FILE] [--packs DIR] [--boot EXECUTABLE --module ELF [--iso DISC]] [--font TTF]\n"
         "         [--game SLUG|TITLE|BOOT|MODULE[|ISO]]... [--select SLUG] [--check-startup]\n"
         "         [--library FILE --importer EXECUTABLE]\n"
         "Each --game adds a title tab; --select opens on that slug instead of the remembered one.\n"
         "--packs names the folder of installed packs. Without --config the settings are\n"
         "$XDG_CONFIG_HOME/psprecomp/settings.ini.\n"
         "Keyboard: arrows, Enter, Escape. Controller: D-pad, A/B, bumpers, Start.");
    return 0;
}

/* The game entries the command line gives: --game, or --boot and --module
 * for a title of the first pack. */
static int add_game(launcher *a, char *spec) {
    if (a->game_count == MAX_GAMES) { fprintf(stderr, "too many --game entries (at most %d)\n", MAX_GAMES); return -1; }
    game_entry g = {0};
    const char **field[] = { &g.slug, &g.title, &g.boot, &g.module, &g.iso };
    for (int k = 0; k < 5 && spec; k++) {
        *field[k] = spec; char *bar = strchr(spec, '|');
        if (bar) *bar = 0;
        spec = bar ? bar + 1 : NULL;
    }
    if (!g.slug || !*g.slug || !g.title || !*g.title || !g.boot || !*g.boot || !g.module || !*g.module ||
        !psp_settings_name_valid(g.slug)) {
        fprintf(stderr, "--game needs SLUG|TITLE|BOOT|MODULE[|ISO]\n"); return -1;
    }
    a->games[a->game_count++] = g;
    return 0;
}

/* What start found: go on, or end now -- after --help, or on an error. */
enum { START_OK, START_HELP, START_FAILED = 2 /* the exit status */ };

static int parse_args(launcher *a, int argc, char **argv, int *check_startup, const char **wanted) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help")) { usage(); return START_HELP; }
        if (!strcmp(argv[i], "--check-startup")) { *check_startup = 1; continue; }
        const char *flag = argv[i];
        if (++i == argc) { fprintf(stderr, "%s needs a value\n", flag); return START_FAILED; }
        if (!strcmp(flag, "--config")) {
            if (strlen(argv[i]) >= sizeof a->path) { fprintf(stderr, "config path too long\n"); return START_FAILED; }
            strcpy(a->path, argv[i]);
        } else if (!strcmp(flag, "--boot")) a->boot = argv[i];
        else if (!strcmp(flag, "--module")) a->module = argv[i];
        else if (!strcmp(flag, "--iso")) a->iso = argv[i];
        else if (!strcmp(flag, "--font")) setenv("PSPRECOMP_UI_FONT", argv[i], 1);
        else if (!strcmp(flag, "--select")) *wanted = argv[i];
        else if (!strcmp(flag, "--library")) a->library = argv[i];
        else if (!strcmp(flag, "--importer")) a->importer = argv[i];
        else if (!strcmp(flag, "--packs")) a->packs_dir = argv[i];
        else if (!strcmp(flag, "--game")) { if (add_game(a, argv[i])) return START_FAILED; }
        else { fprintf(stderr, "unknown option: %s\n", flag); return START_FAILED; }
    }
    return START_OK;
}

/* Every game's pack. A lone --boot and --module are a title of the first
 * pack, and a game no pack names plays under the first, when there is one. */
static int assign_packs(launcher *a) {
    if (!a->game_count && a->boot && a->module && pack_count) {
        static char spec[8192];
        snprintf(spec, sizeof spec, "%s|%s|%s|%s|%s", packs[0].info->titles && packs[0].info->titles[0] ? packs[0].info->titles[0] : "game",
                 packs[0].info->name, a->boot, a->module, a->iso ? a->iso : "");
        if (add_game(a, spec)) return -1;
    }
    for (int k = 0; k < a->game_count; k++) {
        a->games[k].pack = pack_of(a->games[k].slug);
        if (a->games[k].pack < 0) a->games[k].pack = pack_count ? 0 : -1;
    }
    return 0;
}

/* The preferences file, or a new one when there is none. One that cannot be
 * read is never written over (load_failed). */
static int open_preferences(launcher *a) {
    char error[PSP_SETTINGS_ERROR];
    if (access(a->path, F_OK) == 0 || errno != ENOENT) {
        if (!(a->file = psp_settings_file_read(a->path, error))) {
            a->load_failed = 1;
            set_status(a, "Cannot read your settings, so they will not be saved: %.400s", error);
        }
    }
    if (!a->file) a->file = psp_settings_file_new();
    if (!a->file) { fprintf(stderr, "out of memory\n"); return -1; }
    return 0;
}

/* Open on the requested title, else the remembered one, else the first. */
static int open_first_game(launcher *a, const char *wanted) {
    if (!a->game_count) return START_OK;
    int at = wanted ? -1 : 0;
    for (int k = 0; k < a->game_count; k++)
        if (!strcmp(a->games[k].slug, wanted ? wanted : psp_settings_file_game(a->file))) at = k;
    if (at < 0) { fprintf(stderr, "--select: no such game: %s\n", wanted); return START_FAILED; }
    select_game(a, at);
    return START_OK;
}

static int start(launcher *a, int argc, char **argv, int *check_startup, const char **wanted) {
    const int parsed = parse_args(a, argc, argv, check_startup, wanted);
    if (parsed != START_OK) return parsed;
    if (!*a->path && xdg_path("XDG_CONFIG_HOME", ".config", "psprecomp/settings.ini", a->path, sizeof a->path)) {
        fprintf(stderr, "cannot find the settings folder: XDG_CONFIG_HOME or HOME must be an absolute path\n");
        return START_FAILED;
    }
    packs_load(a);
    if (assign_packs(a)) return START_FAILED;
    a->movie_available = psp_mpeg_decoding_available();
    a->pack = -1;
    if (open_preferences(a)) return START_FAILED;
    /* A first start shows the play defaults, or what the earlier settings
     * bring, unsaved until Save or Save and play: nothing the player changed
     * is at stake. */
    use_pack(a, pack_count ? 0 : -1);
    if (library_load(a, 0)) return START_FAILED;
    qsort(a->games, (size_t)a->game_count, sizeof a->games[0], title_order);
    return open_first_game(a, *wanted);
}

static int open_window(launcher *a) {
    int width = UI_W, height = UI_H;
    SDL_Rect usable;
    if (!SDL_GetDisplayUsableBounds(0, &usable)) {
        double scale = SDL_min(1.0, SDL_min((usable.w - 32.0) / UI_W, (usable.h - 48.0) / UI_H));
        scale = SDL_max(0.75, scale);
        width = (int)(UI_W * scale); height = (int)(UI_H * scale);
    }
    a->window = SDL_CreateWindow("psprecomp", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                 width, height, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!a->window) { fprintf(stderr, "launcher: %s\n", SDL_GetError()); return -1; }
    SDL_SetWindowMinimumSize(a->window, 840, 600);
    a->renderer = SDL_CreateRenderer(a->window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!a->renderer) a->renderer = SDL_CreateRenderer(a->window, -1, SDL_RENDERER_SOFTWARE);
    if (!a->renderer || psp_ui_start(a->window, a->renderer)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Settings unavailable",
                                 "Could not create the renderer for the launcher.", a->window);
        return -1;
    }
    controller_open(a);
    psp_ui_focus_next();
    return 0;
}

static void close_window(launcher *a) {
    psp_ui_stop();
    if (a->pad) SDL_GameControllerClose(a->pad);
    if (a->renderer) SDL_DestroyRenderer(a->renderer);
    if (a->window) SDL_DestroyWindow(a->window);
}

int main(int argc, char **argv) {
    launcher a = {0};
    a.running = 1;
    int check_startup = 0;
    const char *wanted = NULL;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER)) { fprintf(stderr, "launcher: %s\n", SDL_GetError()); return 1; }
    const int rc = start(&a, argc, argv, &check_startup, &wanted);
    if (rc != START_OK) { SDL_Quit(); return rc == START_HELP ? 0 : rc; }
    fprintf(stderr, "launcher: preferences %s\n", a.path);
    if (check_startup && a.load_failed) { fprintf(stderr, "%s\n", a.status); return 2; }
    if (open_window(&a)) return 1;
    if (check_startup) {
        draw(&a); draw(&a);
        SDL_RendererInfo info = {0}; SDL_GetRendererInfo(a.renderer, &info);
        int w = 0, h = 0; SDL_GetWindowSize(a.window, &w, &h);
        printf("Launcher startup OK: video=%s renderer=%s window=%dx%d controllers=%d movie_decoder=%d packs=%d games=%d\n",
               SDL_GetCurrentVideoDriver(), info.name ? info.name : "unknown", w, h, SDL_NumJoysticks(),
               a.movie_available, pack_count, a.game_count);
        a.running = 0;
    }
    if (a.load_failed && !check_startup)
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Cannot read your settings", a.status, a.window);
    while (a.running) {
        if (!a.child) draw(&a);
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 30)) { event(&a, &e); while (SDL_PollEvent(&e)) event(&a, &e); }
        import_poll(&a); poll_child(&a);
    }
    close_window(&a);
    free(a.library_buffer);
    psp_settings_file_free(a.file);
    SDL_Quit();
    return 0;
}
