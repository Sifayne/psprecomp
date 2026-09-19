/* Original API stimuli. Declarations from BSD PSPSDK; results are observations. */
typedef unsigned int u32;
extern int sceIoWrite(int,const void*,unsigned);
extern void sceKernelExitGame(void);
extern int sceKernelVolatileMemLock(int,void**,int*);
extern int sceKernelVolatileMemTryLock(int,void**,int*);
extern int sceKernelVolatileMemUnlock(int);
extern int probeCreateThread(const char*,int (*)(unsigned,void*),int,unsigned,u32,void*);
extern int sceKernelStartThread(int,unsigned,void*);
extern int sceKernelDeleteThread(int);
extern int sceKernelWaitThreadEnd(int,u32*);
extern int sceKernelWaitThreadEndCB(int,u32*);
extern int sceKernelGetThreadId(void);
extern int sceKernelChangeThreadPriority(int,int);
extern int sceKernelDelayThread(unsigned);
extern int sceKernelTerminateDeleteThread(int);
extern int sceKernelSetCompiledSdkVersion(int);
extern int sceKernelGetCompiledSdkVersion(void);
/* Identifier independently read from ACLR_App.elf's SysMem import table. */
extern int probeOpaque91de(u32);
static volatile u32 mode, status, ran, released;
static void row(u32 kind,u32 id,u32 a,u32 b,u32 c,u32 d,u32 e,u32 f) {
    u32 values[]={kind,id,a,b,c,d,e,f};char text[73];unsigned n=0;
    const char *hex="0123456789abcdef";
    for (unsigned i=0;i<8;i++) {
        for (int s=28;s>=0;s-=4) text[n++]=hex[(values[i]>>s)&15];
        text[n++]=' ';
    }
    text[n++]='\n';sceIoWrite(1,text,n);
}
static int worker(unsigned args,void *argp) {
    (void)args;(void)argp;ran++;
    if (mode==1) sceKernelDelayThread(5000);
    if (mode==2) released=sceKernelVolatileMemUnlock(0)+1;
    return status;
}
static int make_worker(int priority) {
    return probeCreateThread("own probe",worker,priority,4096,0,(void*)0);
}
static void volatile_call(unsigned id,int op,int type,unsigned pointers) {
    void *base=(void*)0xaabbccdd;int size=0x12345678;
    int result=op==2 ? sceKernelVolatileMemUnlock(type) : op==1
        ? sceKernelVolatileMemTryLock(type,pointers&1?&base:0,pointers&2?&size:0)
        : sceKernelVolatileMemLock(type,pointers&1?&base:0,pointers&2?&size:0);
    row(0,id,result,(u32)(unsigned long)base,size,released,0,0);
}
int module_start(unsigned args,void *argp) {
    (void)args;(void)argp;sceKernelChangeThreadPriority(0,32);
    volatile_call(0,2,0,0);          /* release before acquisition */
    volatile_call(1,1,1,3);          /* nonzero type */
    volatile_call(2,1,-1,3);
    volatile_call(3,1,0,3);          /* acquire, then contention */
    volatile_call(4,1,0,3);
    volatile_call(5,2,1,0);
    volatile_call(6,1,0,3);
    volatile_call(7,2,0,0);
    volatile_call(8,2,0,0);
    for (unsigned pointers=0;pointers<4;pointers++) {
        volatile_call(9+pointers*2,0,0,pointers);
        volatile_call(10+pointers*2,2,0,0);
    }
    volatile_call(17,0,0,3);
    mode=2;status=0;ran=0;released=0;
    int child=make_worker(64);sceKernelStartThread(child,0,0);
    volatile_call(18,0,0,3); /* block until the lower-priority worker releases */
    sceKernelWaitThreadEnd(child,0);sceKernelDeleteThread(child);
    volatile_call(19,2,0,0);released=0;

    const u32 codes[]={0,1,5,0x12345678,0x80000000,0xffffffff};
    for (unsigned i=0;i<6;i++) {
        mode=0;status=codes[i];ran=0;child=make_worker(16);
        u32 timeout=123456;
        int dormant=sceKernelWaitThreadEnd(child,&timeout);
        row(1,i,dormant,timeout,ran,0,0,0);
        int start=sceKernelStartThread(child,0,0);
        int result=sceKernelWaitThreadEnd(child,&timeout);
        u32 after=timeout;timeout=0;
        int zero=sceKernelWaitThreadEndCB(child,&timeout);
        int null=sceKernelWaitThreadEnd(child,0);
        row(2,i,result,after,zero,timeout,null,start);
        sceKernelDeleteThread(child);
    }
    const int ids[]={0,sceKernelGetThreadId(),-1,0x76543210};
    for (unsigned i=0;i<4;i++) {
        u32 timeout=7654321;int r=sceKernelWaitThreadEnd(ids[i],&timeout);
        row(3,i,r,timeout,0,0,0,0);
    }
    for (unsigned cb=0;cb<2;cb++) {
        mode=1;status=0x31415926;ran=0;child=make_worker(16);
        int start=sceKernelStartThread(child,0,0);
        u32 timeout=100;
        int first=cb?sceKernelWaitThreadEndCB(child,&timeout):sceKernelWaitThreadEnd(child,&timeout);
        u32 expired=timeout;timeout=100000;
        int second=cb?sceKernelWaitThreadEndCB(child,&timeout):sceKernelWaitThreadEnd(child,&timeout);
        row(4,cb,first,expired,second,timeout>0&&timeout<100000,ran,start);
        sceKernelDeleteThread(child);
    }
    const u32 versions[]={0,1,0x05050010,0x06030010,0xffffffff,0x13579bdf};
    for (unsigned i=0;i<6;i++) {
        sceKernelSetCompiledSdkVersion(0xdeadbeef);
        int result=probeOpaque91de(versions[i]);
        row(5,i,result,sceKernelGetCompiledSdkVersion(),versions[i],0,0,0);
    }
    row(6,0,0,0,0,0,0,0);sceKernelExitGame();return 0;
}
