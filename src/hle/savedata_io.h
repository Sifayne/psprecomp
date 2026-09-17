/* Private savedata transaction helpers; included by utility.c after its writers.
 * A sibling staging directory holds the COMPLETE new save. The old directory
 * survives until commit. Recovery restores a backup if commit never landed.
 * This protects ordinary I/O failures/process interruption; directory fsync
 * semantics still depend on the host filesystem. */
static int sd_component(const char *s, int empty) {
    if (!*s) return empty;
    if (s[0] == '.') return 0;
    for (; *s; s++) if ((unsigned char)*s < 32 || strchr("/\\:*?\"<>|", *s)) return 0;
    return 1;
}
static int sd_plain_path(const char *guest, int directory) {
    char host[1024]; struct stat st;
    psp_io_host_path(guest, host, sizeof host);
#ifdef _WIN32
    DWORD attr=GetFileAttributesA(host);
    if (attr!=INVALID_FILE_ATTRIBUTES && (attr&FILE_ATTRIBUTE_REPARSE_POINT)) return -1;
    if (stat(host, &st)) return errno == ENOENT ? 0 : -1;
#else
    if (lstat(host, &st)) return errno == ENOENT ? 0 : -1;
    if (S_ISLNK(st.st_mode)) return -1;
#endif
    if (directory ? !S_ISDIR(st.st_mode) : !S_ISREG(st.st_mode)) return -1;
    return 1;
}
/* Delete only entries owned by the selected directory. lstat/reparse checks
 * prevent a link inside a save from turning deletion into a different tree. */
static int sd_remove_tree(const char *guest,int depth) {
    if (depth>8) return -1;
    char host[1024]; struct stat st;
    psp_io_host_path(guest,host,sizeof host);
#ifdef _WIN32
    if (stat(host,&st)) return -1;
    DWORD attr=GetFileAttributesA(host);
    if (attr!=INVALID_FILE_ATTRIBUTES && (attr&FILE_ATTRIBUTE_REPARSE_POINT))
        return (attr&FILE_ATTRIBUTE_DIRECTORY) ? _rmdir(host) : remove(host);
#else
    if (lstat(host,&st)) return -1;
#endif
    if (!S_ISDIR(st.st_mode)) return remove(host);
    char names[257][64]; int n=psp_io_list_names(guest,names,257);
    if (n<0 || n>256) return -1;
    for (int i=0;i<n;i++) {
        char child[1024];
        if (snprintf(child,sizeof child,"%s/%.63s",guest,names[i])>=(int)sizeof child ||
            sd_remove_tree(child,depth+1)) return -1;
    }
#ifdef _WIN32
    return _rmdir(host);
#else
    return rmdir(host);
#endif
}
static int sd_rename(const char *a, const char *b) {
    char ah[1024], bh[1024];
    psp_io_host_path(a, ah, sizeof ah); psp_io_host_path(b, bh, sizeof bh);
    return rename(ah, bh);
}
/* A process-wide card lock also prevents recovery from touching another
 * running game's staging directories. Nonblocking: report I/O failure. */
static int sd_card_lock(void) {
    psp_io_mkdir_all("ms0:/PSP/SAVEDATA");
#ifndef _WIN32
    char path[1024]; psp_io_host_path("ms0:/PSP/.savedata-lock", path, sizeof path);
    int fd = open(path, O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
    if (fd >= 0 && flock(fd, LOCK_EX | LOCK_NB)) { close(fd); return -1; }
    return fd;
#else
    char path[1024]; psp_io_host_path("ms0:/PSP/.savedata-lock",path,sizeof path);
    return _sopen(path,_O_CREAT|_O_RDWR|_O_BINARY,_SH_DENYRW,_S_IREAD|_S_IWRITE);
#endif
}
static void sd_card_unlock(int fd) {
#ifndef _WIN32
    if (fd >= 0) { flock(fd, LOCK_UN); close(fd); }
#else
    if (fd>=0) _close(fd);
#endif
}
static void sd_sync_dir(const char *guest) {
#ifndef _WIN32
    char host[1024]; psp_io_host_path(guest, host, sizeof host);
    int fd = open(host, O_RDONLY | O_DIRECTORY);
    if (fd >= 0) { if (fsync(fd)) sd_io_error = 1; close(fd); }
    else sd_io_error=1;
#else
    (void)guest;
#endif
}
static void sd_sync_card(void) { sd_sync_dir("ms0:/PSP/SAVEDATA"); }
static int sd_copy_tree(const char *src, const char *dst, int depth) {
    if (depth > 8 || sd_plain_path(src, 1) != 1) return -1;
    psp_io_mkdir_all(dst);
    if (sd_plain_path(dst, 1) != 1) return -1;
    char names[257][64]; int n = psp_io_list_names(src, names, 257);
    if (n < 0 || n > 256) return -1;
    for (int i=0; i<n; i++) {
        /* Existing unrelated files are retained, but links are never followed. */
        if (!sd_component(names[i]+(names[i][0]=='.'), 0)) return -1;
        char a[512], b[512];
        if (snprintf(a,sizeof a,"%s/%s",src,names[i]) >= (int)sizeof a ||
            snprintf(b,sizeof b,"%s/%s",dst,names[i]) >= (int)sizeof b) return -1;
        if (sd_plain_path(a,1) == 1) {
            if (sd_copy_tree(a,b,depth+1)) return -1;
        } else {
            if (sd_plain_path(a,0) != 1) return -1;
            char ah[1024], bh[1024];
            psp_io_host_path(a,ah,sizeof ah); psp_io_host_path(b,bh,sizeof bh);
            FILE *in=fopen(ah,"rb"), *out=NULL;
            if (in) out=fopen(bh,"wb");
            if (!in || !out) { if (in) fclose(in); return -1; }
            unsigned char bytes[16384]; size_t got; int bad=0;
            while ((got=fread(bytes,1,sizeof bytes,in)))
                if (fwrite(bytes,1,got,out)!=got) { bad=1; break; }
            if (ferror(in)) bad=1;
            if (fclose(in)) bad=1;
            if (fflush(out)) bad=1;
#ifndef _WIN32
            if (fsync(fileno(out))) bad=1;
#endif
            if (fclose(out)) bad=1;
            if (bad) return -1;
        }
    }
    sd_sync_dir(dst);
    return sd_io_error ? -1 : 0;
}
static void sd_tx_paths(const char *dir, char *pending, char *backup) {
    const char *name=strrchr(dir,'/'); name=name ? name+1 : dir;
    snprintf(pending,512,"ms0:/PSP/SAVEDATA/.pending-%s",name);
    snprintf(backup,512,"ms0:/PSP/SAVEDATA/.backup-%s",name);
}
/* Caller holds the card lock. Recognize only our bounded save components. */
static int sd_recover_one(const char *dir) {
    char pending[512], backup[512]; sd_tx_paths(dir,pending,backup);
    int old=sd_plain_path(backup,1), current=sd_plain_path(dir,1);
    if (old<0 || current<0 || sd_plain_path(pending,1)<0) return -1;
    if (old==1) {
        if (!current) { if (sd_rename(backup,dir)) return -1; }
        else if (sd_remove_tree(backup,0)) return -1;
    }
    if (sd_exists(pending) && sd_remove_tree(pending,0)) return -1;
    return 0;
}
static int sd_recover_card(void) {
    /* Nothing to recover on a card without the tree, and nothing to create:
     * a read-only load must not build the directory it reads. */
    int tree=sd_plain_path("ms0:/PSP/SAVEDATA",1);
    if (tree<0) return -1;
    if (!tree) return 0;
    int lock=sd_card_lock(); if (lock<0) return -1;
    char names[1024][64]; int n=psp_io_list_names("ms0:/PSP/SAVEDATA",names,1024), bad=0;
    sd_io_error=0;
    if (n<0) bad=1;
    if (n==1024) fprintf(stderr,"savedata: recovery scanned only the first 1024 entries under PSP/SAVEDATA\n");
    for (int i=0; i<n && !bad; i++) {
        if (strncmp(names[i],".backup-",8) && strncmp(names[i],".pending-",9)) continue;
        const char *name=names[i]+(names[i][1]=='b'?8:9);
        if (!sd_component(name,0) || strlen(name)>33) continue;
        char dir[512]; snprintf(dir,sizeof dir,"ms0:/PSP/SAVEDATA/%s",name);
        if (sd_recover_one(dir)) bad=1;
    }
    sd_sync_card(); sd_card_unlock(lock); return bad || sd_io_error ? -1 : 0;
}
static void sd_transaction_write(uint32_t param, const char *dir, const char *file) {
    int lock=sd_card_lock();
    if (lock<0) { sd_io_error=1; return; }
    char pending[512], backup[512]; sd_tx_paths(dir,pending,backup);
    if (sd_recover_one(dir)) { sd_io_error=1; goto done; }
    int existed=sd_plain_path(dir,1);
    if (existed<0 || (existed && sd_copy_tree(dir,pending,0))) {
        sd_io_error=1; goto cleanup;
    }
    psp_io_mkdir_all(pending);
    if (sd_plain_path(pending,1)!=1) { sd_io_error=1; goto cleanup; }
    sd_write_save(param,pending,file);
    /* PARAM.SFO must name the final directory, not its staging name. */
    const char *leaf=strrchr(dir,'/');
    sd_write_sfo(pending,leaf?leaf+1:dir,param);
    sd_sync_dir(pending);
    if (sd_io_error) goto cleanup;
    if (existed && sd_rename(dir,backup)) { sd_io_error=1; goto cleanup; }
    sd_sync_card();
    if (sd_rename(pending,dir)) {
        sd_io_error=1;
        if (existed && sd_rename(backup,dir))
            fprintf(stderr,"savedata: previous save retained at %s; recovery required\n",backup);
        goto cleanup;
    }
    /* Committed: the new save is the directory. What follows is cleanup, and
     * a failure there must not be reported as a failed save. */
    if (sd_io_error) fprintf(stderr,"savedata: directory sync failed before commit; the save is in place\n");
    sd_io_error=0;
    sd_sync_card();
    if (existed && sd_remove_tree(backup,0))
        fprintf(stderr,"savedata: could not remove backup %s; the new save is intact\n",backup);
    sd_sync_card();
    if (sd_io_error) fprintf(stderr,"savedata: post-commit directory sync failed; the save is in place\n");
    sd_io_error=0;
    goto done;
cleanup:
    if (sd_exists(pending)) sd_remove_tree(pending,0);
done:
    sd_card_unlock(lock);
}
