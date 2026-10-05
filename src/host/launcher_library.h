/* Private launcher UI helpers. Included after launcher and drawing helpers. */
static void activate(launcher *a,int id);

/* The pack's order (psp_launcher_info.titles), then any other slug. */
static int title_rank(const char *slug) {
    const char *const *order=psp_launcher_info.titles; int at=0;
    while (order && order[at] && strcmp(order[at],slug)) at++;
    return at;
}
static int title_order(const void *left,const void *right) {
    const game_entry *a=left,*b=right; int x=title_rank(a->slug),y=title_rank(b->slug);
    return x==y?strcmp(a->slug,b->slug):x-y;
}
static char *library_field(char **cursor,char *end) {
    if (*cursor>=end) return NULL;
    char *value=*cursor,*stop=memchr(value,0,(size_t)(end-value));
    if (!stop || stop-value>=4096) return NULL;
    *cursor=stop+1; return value;
}
static int library_load(launcher *a,int imported) {
    if (!a->library) return 0;
    FILE *f=fopen(a->library,"rb");
    if (!f) { if (errno==ENOENT) return 0; snprintf(a->status,sizeof a->status,"Cannot read game library: %s",strerror(errno)); return -1; }
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
            !psp_presets_name_valid(g->slug)) goto invalid;
        count++;
    }
    const char *selected=imported && *wanted?wanted:a->book.game;
    int at=0;
    qsort(entries,(size_t)count,sizeof entries[0],title_order);
    for (int k=0;k<count;k++) if (!strcmp(entries[k].slug,selected)) at=k;
    free(a->library_buffer); a->library_buffer=data;
    memcpy(a->games,entries,(size_t)count*sizeof entries[0]); a->game_count=count;
    if (count) select_game(a,at,!imported);
    else {
        /* The launch targets pointed into the buffer just freed. A valid but
         * empty library leaves nothing to launch, so say so rather than keep
         * dangling strings that draw() and launch_game() would read. */
        a->game=0; a->boot=NULL; a->module=NULL; a->iso=NULL;
    }
    return 0;
invalid:
    free(data); snprintf(a->status,sizeof a->status,"Cannot read game library. Installed files have not been changed."); return -1;
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
    a->file_count=a->file_selected=a->file_scroll=0; a->browser_error[0]=0;
    struct dirent *entry;
    while ((entry=readdir(dir))) {
        if (entry->d_name[0]=='.') continue;
        char full[4096]; struct stat st;
        if (snprintf(full,sizeof full,"%s/%s",resolved,entry->d_name)>=(int)sizeof full || stat(full,&st)) continue;
        const char *ext=strrchr(entry->d_name,'.');
        if (!S_ISDIR(st.st_mode) && !(S_ISREG(st.st_mode) && ext && !strcasecmp(ext,".iso"))) continue;
        if (a->file_count==MAX_FILES) { snprintf(a->browser_error,sizeof a->browser_error,"This folder has too many entries. Open a smaller folder."); break; }
        browser_file *item=&a->files[a->file_count++];
        snprintf(item->name,sizeof item->name,"%s",entry->d_name); item->directory=S_ISDIR(st.st_mode);
    }
    closedir(dir); qsort(a->files,(size_t)a->file_count,sizeof a->files[0],file_order);
}
static void browser_open(launcher *a) {
    if (!a->importer) return;
    a->modal=MODAL_BROWSE; a->focus=BROWSER_OPEN;
    browser_scan(a,*a->browser_path?a->browser_path:getenv("HOME")?getenv("HOME"):"/");
}
static void browser_up(launcher *a) {
    char path[4096]; snprintf(path,sizeof path,"%s",a->browser_path);
    char *slash=strrchr(path,'/'); if (slash) { if (slash==path) slash[1]=0; else *slash=0; }
    browser_scan(a,path);
}
static void import_start(launcher *a,const char *iso) {
    if (!a->importer || a->import_pid || a->child) return;
    int pipes[2];
    if (pipe(pipes)) { snprintf(a->status,sizeof a->status,"Cannot prepare game: %s",strerror(errno)); return; }
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
        execl(a->importer,a->importer,"import",iso,(char *)NULL); _exit(127);
    }
    setpgid(pid,pid); sigprocmask(SIG_SETMASK,&previous,NULL);
    close(pipes[1]); fcntl(pipes[0],F_SETFL,O_NONBLOCK);
    a->import_pid=pid; a->import_fd=pipes[0]; a->import_used=0;
    a->import_cancelled=0; a->modal=MODAL_PREPARING; a->focus=BROWSER_CANCEL;
    snprintf(a->import_status,sizeof a->import_status,"Checking the selected ISO...");
}
static void import_cancel(launcher *a) {
    if (!a->import_pid) return;
    kill(-a->import_pid,SIGTERM); a->import_cancelled=1;
    snprintf(a->import_status,sizeof a->import_status,"Canceling preparation...");
}
static void browser_enter(launcher *a) {
    if (!a->file_count) return;
    char full[4096]; browser_file *item=&a->files[a->file_selected];
    if (snprintf(full,sizeof full,"%s/%s",a->browser_path,item->name)>=(int)sizeof full) return;
    if (item->directory) browser_scan(a,full); else import_start(a,full);
}
static void browser_move(launcher *a,int delta) {
    a->file_selected+=delta;
    if (a->file_selected<0) a->file_selected=0;
    if (a->file_selected>=a->file_count) a->file_selected=a->file_count?a->file_count-1:0;
    if (a->file_selected<a->file_scroll) a->file_scroll=a->file_selected;
    if (a->file_selected>=a->file_scroll+10) a->file_scroll=a->file_selected-9;
}
static void browser_activate(launcher *a,int id) {
    if (id==BROWSER_CANCEL) close_modal(a);
    else if (id==BROWSER_UP) browser_up(a);
    else if (id==BROWSER_HOME) browser_scan(a,getenv("HOME")?getenv("HOME"):"/");
    else if (id==BROWSER_DRIVES) browser_scan(a,access("/run/media",R_OK)?"/":"/run/media");
    else if (id==BROWSER_OPEN) browser_enter(a);
    else if (id>=FILE_BASE && id<FILE_BASE+a->file_count) { a->file_selected=id-FILE_BASE; browser_enter(a); }
}
static void library_modal_draw(launcher *a) {
    box(a,(SDL_Rect){0,0,UI_W,UI_H},BG); a->hit_count=0;
    label(a,a->heading,48,62,1000,a->modal==MODAL_BROWSE?"ADD GAME":"PREPARING GAME",TEXT);
    if (a->modal==MODAL_PREPARING) {
        label(a,a->body,48,144,1000,a->import_status,ACCENT);
        label(a,a->body,48,280,1000,"Keep the app open while your game is prepared. Future launches use the saved result.",MUTED);
        button(a,BROWSER_CANCEL,922,694,150,44,"Cancel",0); return;
    }
    label(a,a->small,48,116,660,"Select your PSP ISO. D-pad: select  A: open  B: cancel  Left: parent folder",MUTED);
    button(a,BROWSER_UP,730,104,90,40,"Up",0);
    button(a,BROWSER_HOME,832,104,104,40,"Home",0);
    button(a,BROWSER_DRIVES,948,104,124,40,"Drives",0);
    SDL_Rect clip={48,158,1024,32}; SDL_RenderSetClipRect(a->renderer,&clip);
    label(a,a->body,48,160,4000,a->browser_path,TEXT); SDL_RenderSetClipRect(a->renderer,NULL);
    for (int i=a->file_scroll;i<a->file_count && i<a->file_scroll+10;i++) {
        SDL_Rect row={48,208+(i-a->file_scroll)*42,1024,38}; box(a,row,ROW);
        if (i==a->file_selected) outline(a,row,ACCENT);
        char name[300]; snprintf(name,sizeof name,"%s%s",a->files[i].directory?"[Folder]  ":"",a->files[i].name);
        SDL_RenderSetClipRect(a->renderer,&row); label(a,a->body,row.x+12,row.y+8,4000,name,TEXT);
        SDL_RenderSetClipRect(a->renderer,NULL); add_hit(a,FILE_BASE+i,row);
    }
    if (!a->file_count) label(a,a->body,60,228,1000,"No folders or ISO files here. Use Up, Home or Drives to choose another location.",MUTED);
    label(a,a->small,48,646,1000,a->browser_error,ACCENT);
    label(a,a->small,48,704,710,"Mouse wheel: scroll   Page Up/Down: move faster\nYou can also drag an ISO onto the launcher, or paste its path with Ctrl+V.",MUTED);
    button(a,BROWSER_CANCEL,784,694,126,44,"Cancel",0);
    button(a,BROWSER_OPEN,922,694,150,44,"Open",1);
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
    close(a->import_fd); a->import_pid=0; close_modal(a);
    if (a->import_cancelled) snprintf(a->status,sizeof a->status,"Preparation canceled. Existing games and saves are unchanged.");
    else if (WIFEXITED(status) && WEXITSTATUS(status)==0) {
        if (!library_load(a,1)) snprintf(a->status,sizeof a->status,"Game is ready. Choose Save & Play.");
    } else snprintf(a->status,sizeof a->status,"%.250s",a->import_status);
    if (a->quit_after_import) { a->quit_after_import=0; activate(a,CANCEL); }
}
