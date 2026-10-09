/* Private launcher helpers: the importer's game library, the ISO browser's
 * folder listing and the import it starts. Included by launcher.c after its
 * launcher type; the drawing is launcher.c's. */
static void activate_back(launcher *a);
static void packs_load(launcher *a);

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
    if (!a->importer) return;
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
        execl(a->importer,a->importer,commands[work],what,(char *)NULL); _exit(127);
    }
    setpgid(pid,pid); sigprocmask(SIG_SETMASK,&previous,NULL);
    close(pipes[1]); fcntl(pipes[0],F_SETFL,O_NONBLOCK);
    a->import_pid=pid; a->import_fd=pipes[0]; a->import_used=0;
    a->import_cancelled=0; a->work=work; a->view=VIEW_PREPARING; psp_ui_focus_next();
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
    close(a->import_fd); a->import_pid=0; a->view=VIEW_SETTINGS; psp_ui_focus_next();
    const int ok=!a->import_cancelled && WIFEXITED(status) && WEXITSTATUS(status)==0;
    if (a->import_cancelled) snprintf(a->status,sizeof a->status,"%s",a->work==WORK_IMPORT?
        "Preparation canceled. Existing games and saves are unchanged.":"Canceled. Your packs are unchanged.");
    else if (!ok) snprintf(a->status,sizeof a->status,"%.250s",a->import_status);
    else if (a->work==WORK_IMPORT) {
        if (!library_load(a,1)) snprintf(a->status,sizeof a->status,"Your game is ready. Choose Save and play.");
    } else if (a->work==WORK_INSTALL) {
        const int before=pack_count;
        packs_load(a);
        library_load(a,0);
        if (pack_count>before) snprintf(a->status,sizeof a->status,"Added %.200s. Add a game of it with Add game.",packs[pack_count-1].info->name);
        else snprintf(a->status,sizeof a->status,"The pack was added, but this launcher could not load it. See the log.");
    } else {
        packs[a->remove_pack].removed=1;
        if (a->pack==a->remove_pack) use_pack(a,-1);
        library_load(a,0);
        snprintf(a->status,sizeof a->status,"Removed %.200s.",packs[a->remove_pack].info->name);
    }
    if (a->quit_after_import) { a->quit_after_import=0; activate_back(a); }
}

static void pack_remove(launcher *a,int p) {
    if (p<0 || p>=pack_count || !packs[p].handle) return;
    a->remove_pack=p;
    import_start(a,WORK_REMOVE,packs[p].settings->id);
}
