/* Original SDK-layout experiment. Run only in the isolated Memory Stick
 * mount constructed by run_probe.py. All file names belong to this probe. */
typedef unsigned int u32;
extern int sceIoWrite(int,const void*,unsigned);
extern int sceIoRemove(const char*);
extern void sceKernelExitGame(void);
extern int sceKernelDelayThread(unsigned);
extern int sceUtilitySavedataInitStart(void*);
extern int sceUtilitySavedataGetStatus(void);
extern void sceUtilitySavedataUpdate(int);
extern int sceUtilitySavedataShutdownStart(void);
static u32 parameter[384] __attribute__((aligned(16)));
static unsigned char payload[64] __attribute__((aligned(16)));
static u32 files[9];
#ifdef PROBE_NATIVE
extern u32 savedata_guest_address(const void*);
#else
static u32 savedata_guest_address(const void *pointer) { return (u32)pointer; }
#endif
static void string(unsigned offset,const char *value) {
    unsigned char *to=(unsigned char*)parameter+offset;
    while ((*to++=(unsigned char)*value++)) {}
}
static void setup(unsigned mode,const char *save) {
    for (unsigned i=0;i<384;i++) parameter[i]=0;
    parameter[0]=sizeof parameter;
    parameter[1]=1;parameter[2]=1;
    parameter[3]=parameter[4]=parameter[5]=parameter[6]=0x20;
    parameter[12]=mode;parameter[14]=1;
    string(60,"PRV260919");string(76,save);string(100,"DATA.BIN");
    parameter[116/4]=savedata_guest_address(payload);parameter[120/4]=sizeof payload;
    parameter[124/4]=sizeof payload;
    parameter[1528/4]=savedata_guest_address(files);
    for (unsigned i=0;i<sizeof payload;i++) payload[i]=(unsigned char)(3*i+7);
}
static void output(u32 kind,u32 id,u32 a,u32 b,u32 c) {
    const char *digits="0123456789abcdef";char text[48];unsigned length=0;
    u32 words[]={kind,id,a,b,c};
    for (unsigned i=0;i<5;i++) {
        for (int bit=28;bit>=0;bit-=4) text[length++]=digits[(words[i]>>bit)&15];
        text[length++]=' ';
    }
    text[length++]='\n';sceIoWrite(1,text,length);
}
static int finish(void) {
    int status=-1;
    for (unsigned step=0;step<2000;step++) {
        status=sceUtilitySavedataGetStatus();
        if (status==3) break;
        if (status==2) sceUtilitySavedataUpdate(1);
        sceKernelDelayThread(1000);
    }
    if (status==3) {
        sceUtilitySavedataShutdownStart();
        for (unsigned step=0;step<2000;step++) {
            int next=sceUtilitySavedataGetStatus();
            if (next==0) break;
            sceKernelDelayThread(1000);
        }
    }
    return status;
}
static void run(unsigned id,unsigned mode,const char *save) {
    setup(mode,save);
    int start=sceUtilitySavedataInitStart(parameter);
    int status=start==0?finish():-1;
    output(0,id,(u32)start,parameter[7],(u32)status);
}
int module_start(unsigned args,void *argp) {
    (void)args;(void)argp;
    run(0,0,"MISSING");run(1,16,"MISSING");run(2,20,"MISSING");
    run(3,9,"MISSING");run(4,12,"MISSING");
    run(5,1,"VALID");run(6,0,"VALID");
    output(1,0,(u32)sceIoRemove("ms0:/PSP/SAVEDATA/PRV260919VALID/DATA.BIN"),0,0);
    run(7,0,"VALID");run(8,16,"VALID");
    output(1,1,(u32)sceIoRemove("ms0:/PSP/SAVEDATA/PRV260919VALID/PARAM.SFO"),0,0);
    run(9,0,"VALID");run(10,16,"VALID");
    run(11,0,"DIRFILE");run(12,0,"READDENIED");
    run(13,1,"BLOCKED");run(14,9,"BLOCKED");
    run(15,20,"BLOCKED");run(16,18,"BLOCKED");
    run(17,16,"DIRFILE");run(18,16,"READDENIED");
    run(19,0,"BROKEN");run(20,16,"BROKEN");
    run(21,1,"SMALL");
    setup(0,"SMALL");parameter[120/4]=1;
    int small=sceUtilitySavedataInitStart(parameter);
    int state=small==0?finish():-1;output(0,22,(u32)small,parameter[7],(u32)state);
    setup(16,"SMALL");parameter[120/4]=1;
    small=sceUtilitySavedataInitStart(parameter);
    state=small==0?finish():-1;output(0,23,(u32)small,parameter[7],(u32)state);
    setup(0,"MISSING");parameter[0]=0;
    int start=sceUtilitySavedataInitStart(parameter);
    output(2,0,(u32)start,0,0);if (!start) finish();
    setup(0,"MISSING");start=sceUtilitySavedataInitStart(parameter);
    int again=sceUtilitySavedataInitStart(parameter);
    output(2,1,(u32)start,(u32)again,0);if (!start) finish();
    output(3,0,0,0,0);sceKernelExitGame();return 0;
}
