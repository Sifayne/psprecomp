/* Project-authored disc stimuli: the filesystem view of a mounted image, its
 * raw sector device, file stat fields, directory reading and the asynchronous
 * I/O calls. Declarations come from BSD PSPSDK pspiofilemgr.h and
 * pspiofilemgr_stat.h. The image is built by make_iso.py: sector s holds
 * words (s << 16) | j, so every read names the sector it came from. */
typedef unsigned int u32;
typedef long long s64;
extern int sceIoWrite(int,const void*,unsigned);
extern void sceKernelExitGame(void);
extern int sceKernelDelayThread(unsigned);
extern int sceIoOpen(const char*,int,int);
extern int sceIoClose(int);
extern int sceIoRead(int,void*,unsigned);
extern s64 probeLseek(int,s64,int);          /* sceIoLseek with the fifth argument bridged */
extern int sceIoLseek32(int,int,int);
extern int sceIoGetstat(const char*,void*);
extern int sceIoDopen(const char*);
extern int sceIoDread(int,void*);
extern int sceIoDclose(int);
extern int sceIoOpenAsync(const char*,int,int);
extern int sceIoReadAsync(int,void*,unsigned);
extern int probeLseekAsync(int,s64,int);     /* sceIoLseekAsync, likewise */
extern int sceIoCloseAsync(int);
extern int sceIoPollAsync(int,s64*);
extern int sceIoWaitAsync(int,s64*);
extern int sceIoWaitAsyncCB(int,s64*);
extern int sceIoGetAsyncStat(int,int,s64*);
extern int sceUmdActivate(int,const char*);
extern int sceUmdDeactivate(int,const char*);
extern int sceUmdWaitDriveStatWithTimer(int,unsigned);
extern int sceUmdGetDriveStat(void);
#define O_RDONLY 1
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
static u32 data[4096] __attribute__((aligned(64)));   /* 16 KiB */
static u32 stat_words[22] __attribute__((aligned(16)));
static u32 dirent_words[88] __attribute__((aligned(16)));
static s64 result;
#ifdef PROBE_NATIVE
extern volatile u32 *buffer(void);
extern u32 *stat_buffer(void);
extern u32 *dirent_buffer(void);
extern s64 *result_slot(void);
#else
static volatile u32 *buffer(void) { return (volatile u32*)(0x40000000u|(u32)data); }
static u32 *stat_buffer(void) { return stat_words; }
static u32 *dirent_buffer(void) { return dirent_words; }
static s64 *result_slot(void) { return &result; }
#endif
static void output(u32 kind,u32 id,const u32 *words,unsigned count) {
    char text[256];const char *digits="0123456789abcdef";unsigned length=0;
    if (count>26) count=26;
    for (unsigned i=0;i<count+2;i++) {
        u32 value=i==0?kind:i==1?id:words[i-2];
        for (int bit=28;bit>=0;bit-=4) text[length++]=digits[(value>>bit)&15];
        text[length++]=' ';
    }
    text[length++]='\n';sceIoWrite(1,text,length);
}
static void out1(u32 kind,u32 id,u32 a) { output(kind,id,&a,1); }
static void out2(u32 kind,u32 id,u32 a,u32 b) { u32 w[]={a,b};output(kind,id,w,2); }
static void out3(u32 kind,u32 id,u32 a,u32 b,u32 c) { u32 w[]={a,b,c};output(kind,id,w,3); }
/* Descriptors differ between hosts; what is compared is success and errors. */
static u32 ok(int fd) { return fd>=0?1u:(u32)fd; }
static void fill(volatile u32 *p,unsigned n,u32 v) { for (unsigned i=0;i<n;i++) p[i]=v; }
static const char *const paths[]={
    "disc0:/ALPHA.BIN","disc0:/SUB/GAMMA.BIN","disc0:/BETA.TXT","disc0:/NOPE.BIN","disc0:/",
    "umd0:","umd1:","umd0:/ALPHA.BIN","disc0:/alpha.bin","disc0:/ALPHA.BIN;1","disc0:/SUB",
    "disc0:/SUB/","umd0:/","disc0:ALPHA.BIN","umd1:/ALPHA.BIN"};
static void opens(void) {
    for (unsigned i=0;i<sizeof paths/sizeof *paths;i++) {
        int fd=sceIoOpen(paths[i],O_RDONLY,0);
        out1(0,i,ok(fd));
        if (fd>=0) sceIoClose(fd);
    }
}
static void stat_case(u32 id,const char *path) {
    u32 *s=stat_buffer();
    for (unsigned j=0;j<22;j++) s[j]=0xcdcdcdcd;
    int r=sceIoGetstat(path,s);
    /* result, mode, attr, size lo/hi, the three date/time records as four
     * words each, then the six private words: the whole block. */
    u32 words[23];words[0]=(u32)r;
    for (unsigned j=0;j<22;j++) words[1+j]=s[j];
    output(1,id,words,23);
}
static void stats(void) {
    const unsigned which[]={0,1,2,3,4,10,11,6};
    for (unsigned i=0;i<sizeof which/sizeof *which;i++) stat_case(which[i],paths[which[i]]);
}
static void read_case(u32 id,int fd,unsigned words_wanted,unsigned show) {
    volatile u32 *b=buffer();fill(b,4096,0xeeeeeeee);
    int got=sceIoRead(fd,(void*)buffer(),words_wanted*4);
    u32 w[10];w[0]=(u32)got;
    for (unsigned i=0;i<show && i<9;i++) w[1+i]=b[i];
    output(2,id,w,1+(show<9?show:9));
}
static void file_reads(void) {
    int fd=sceIoOpen("disc0:/ALPHA.BIN",O_RDONLY,0);
    read_case(0,fd,4,4);                                  /* the first words of the first sector */
    s64 pos=probeLseek(fd,2048,SEEK_SET);out2(3,0,(u32)pos,(u32)(pos>>32));
    read_case(1,fd,2,2);                                  /* the next sector */
    pos=probeLseek(fd,-4,SEEK_CUR);out2(3,1,(u32)pos,(u32)(pos>>32));
    read_case(2,fd,1,1);
    pos=probeLseek(fd,0,SEEK_END);out2(3,2,(u32)pos,(u32)(pos>>32));
    read_case(3,fd,4,1);                                  /* nothing past the end */
    out1(3,3,(u32)sceIoLseek32(fd,-8,SEEK_END));
    read_case(4,fd,4,2);                                  /* the last two words, then the end */
    out1(3,4,(u32)sceIoLseek32(fd,7,SEEK_SET));
    read_case(5,fd,2,2);                                  /* unaligned position */
    sceIoClose(fd);
    fd=sceIoOpen("disc0:/BETA.TXT",O_RDONLY,0);
    read_case(6,fd,32,3);                                 /* 128 asked of a 100-byte file */
    sceIoClose(fd);
    fd=sceIoOpen("disc0:/SUB/GAMMA.BIN",O_RDONLY,0);
    read_case(7,fd,2048,2);                               /* 8192 asked of 6151 */
    read_case(8,fd,4,1);
    sceIoClose(fd);
    out1(2,9,(u32)sceIoRead(fd,(void*)buffer(),16));      /* a closed descriptor */
    out1(2,10,(u32)sceIoRead(-1,(void*)buffer(),16));
    fd=sceIoOpen("disc0:/SUB",O_RDONLY,0);                /* a directory opened as a file */
    read_case(11,fd,4,1);
    pos=probeLseek(fd,0,SEEK_END);out2(3,5,(u32)pos,(u32)(pos>>32));
    out1(3,6,(u32)sceIoClose(fd));
}
static void raw_device(const char *name,u32 base) {
    int fd=sceIoOpen(name,O_RDONLY,0);
    out1(4,base,ok(fd));
    if (fd<0) return;
    s64 pos=probeLseek(fd,22,SEEK_SET);out2(4,base+1,(u32)pos,(u32)(pos>>32));
    read_case(base+2,fd,2,3);                             /* two units from sector 22 */
    pos=probeLseek(fd,0,SEEK_CUR);out2(4,base+3,(u32)pos,(u32)(pos>>32));
    out1(4,base+4,(u32)sceIoLseek32(fd,30,SEEK_SET));
    read_case(base+5,fd,1,2);
    pos=probeLseek(fd,0,SEEK_SET);out2(4,base+6,(u32)pos,(u32)(pos>>32));
    read_case(base+7,fd,1,2);                             /* sector 0 */
    pos=probeLseek(fd,100,SEEK_SET);out2(4,base+8,(u32)pos,(u32)(pos>>32));
    read_case(base+9,fd,1,1);                             /* beyond the image */
    pos=probeLseek(fd,0,SEEK_END);out2(4,base+10,(u32)pos,(u32)(pos>>32));
    sceIoClose(fd);
}
static void asyncs(void) {
    s64 *res=result_slot();
    int fd=sceIoOpen("disc0:/ALPHA.BIN",O_RDONLY,0);
    volatile u32 *b=buffer();fill(b,4096,0xeeeeeeee);
    *res=-1;
    int r=sceIoReadAsync(fd,(void*)buffer(),4096);out1(5,0,(u32)r);
    unsigned polls=0;int p=1;
    while (polls<64 && (p=sceIoPollAsync(fd,res))==1) { polls++;sceKernelDelayThread(200); }
    out3(5,1,(u32)p,(u32)*res,(u32)(*res>>32));
    out2(5,2,polls?1u:0u,b[0]);                           /* whether any poll found it running */
    *res=-1;r=sceIoPollAsync(fd,res);out3(5,3,(u32)r,(u32)*res,(u32)(*res>>32));   /* nothing pending */
    *res=-1;r=sceIoWaitAsync(fd,res);out3(5,4,(u32)r,(u32)*res,(u32)(*res>>32));
    *res=-1;r=sceIoGetAsyncStat(fd,1,res);out3(5,5,(u32)r,(u32)*res,(u32)(*res>>32));
    r=sceIoReadAsync(fd,(void*)buffer(),2048);out1(5,6,(u32)r);
    r=sceIoReadAsync(fd,(void*)buffer(),2048);out1(5,7,(u32)r);           /* a second before the first is taken */
    *res=-1;r=sceIoWaitAsync(fd,res);out3(5,8,(u32)r,(u32)*res,(u32)(*res>>32));
    *res=-1;r=sceIoGetAsyncStat(fd,0,res);out3(5,9,(u32)r,(u32)*res,(u32)(*res>>32));   /* wait through GetAsyncStat */
    r=probeLseekAsync(fd,4096,SEEK_SET);out1(5,10,(u32)r);
    *res=-1;r=sceIoWaitAsyncCB(fd,res);out3(5,11,(u32)r,(u32)*res,(u32)(*res>>32));
    r=sceIoRead(fd,(void*)buffer(),4);out2(5,12,(u32)r,b[0]);            /* the position it left */
    r=sceIoCloseAsync(fd);out1(5,13,(u32)r);
    *res=-1;r=sceIoWaitAsync(fd,res);out3(5,14,(u32)r,(u32)*res,(u32)(*res>>32));
    *res=-1;r=sceIoPollAsync(fd,res);out3(5,15,(u32)r,(u32)*res,(u32)(*res>>32));   /* after the close */
    r=sceIoOpenAsync("disc0:/BETA.TXT",O_RDONLY,0);out1(5,16,ok(r));
    *res=-1;int w=sceIoWaitAsync(r,res);out3(5,17,(u32)w,(u32)(*res==r),(u32)(*res>>32));
    sceIoClose(r);
    r=sceIoOpenAsync("disc0:/NOPE.BIN",O_RDONLY,0);out1(5,18,ok(r));
    if (r>=0) {
        *res=-1;w=sceIoWaitAsync(r,res);out3(5,19,(u32)w,(u32)*res,(u32)(*res>>32));
        out1(5,21,(u32)sceIoClose(r));                    /* does the failed open leave a descriptor */
        *res=-1;w=sceIoPollAsync(r,res);out3(5,22,(u32)w,(u32)*res,(u32)(*res>>32));
    }
    *res=-1;r=sceIoWaitAsync(-1,res);out3(5,20,(u32)r,(u32)*res,(u32)(*res>>32));
}
static void listing(u32 id,const char *dir) {
    int dfd=sceIoDopen(dir);
    out1(6,id,ok(dfd));
    if (dfd<0) return;
    for (unsigned n=0;n<8;n++) {
        u32 *d=dirent_buffer();
        for (unsigned j=0;j<88;j++) d[j]=0xcdcdcdcd;
        int r=sceIoDread(dfd,d);
        /* result, mode, attr, size lo, private[0], the name's first 12 bytes,
         * and the first creation-time word. */
        u32 words[9]={(u32)r,d[0],d[1],d[2],d[16],d[22],d[23],d[24],d[4]};
        output(7,id*16+n,words,9);
        if (r<=0) break;
    }
    out1(6,id+8,(u32)sceIoDclose(dfd));
}
static void opens_as(u32 base) {
    for (unsigned i=0;i<sizeof paths/sizeof *paths;i++) {
        int fd=sceIoOpen(paths[i],O_RDONLY,0);
        out1(8,base+i,ok(fd));
        if (fd>=0) sceIoClose(fd);
    }
}
/* A game activates the drive before touching it; everything above ran
 * without. What activation and deactivation change about the names. */
static void activation(void) {
    out1(8,0,(u32)sceUmdGetDriveStat());
    out1(8,1,(u32)sceUmdActivate(1,"disc0:"));
    out1(8,2,(u32)sceUmdWaitDriveStatWithTimer(0x20,1000));
    out1(8,3,(u32)sceUmdGetDriveStat());
    opens_as(16);
    stat_case(5,paths[5]);                                /* umd0: */
    raw_device("umd0:",64);
    out1(8,4,(u32)sceUmdDeactivate(1,"disc0:"));
    out1(8,5,(u32)sceUmdGetDriveStat());
    opens_as(48);
    out1(8,6,(u32)sceUmdActivate(1,"disc0:"));
    out1(8,7,(u32)sceUmdWaitDriveStatWithTimer(0x20,1000));
    int fd=sceIoOpen("umd0:",O_RDONLY,0);out1(8,8,ok(fd));if (fd>=0) sceIoClose(fd);
}
int module_start(unsigned args,void *argp) {
    (void)args;(void)argp;
    opens();
    stats();
    file_reads();
    raw_device("umd0:",32);                               /* read records share kind 2 with file_reads: keep the ids apart */
    raw_device("umd1:",48);
    asyncs();
    listing(0,"disc0:/");listing(1,"disc0:/SUB");listing(2,"disc0:/NOPE");listing(3,"disc0:/SUB/GAMMA.BIN");
    activation();
    out1(9,0,0);sceKernelExitGame();return 0;
}
