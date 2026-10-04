typedef unsigned int u32;
extern int sceIoWrite(int,const void*,unsigned);
extern void sceKernelExitGame(void);
extern u32 sceKernelGetSystemTimeLow(void);
extern int sceDisplayWaitVblankStart(void);
extern u32 sceKernelCpuSuspendIntr(void);
extern void sceKernelCpuResumeIntr(u32);
extern void sceKernelCpuResumeIntrWithSync(u32);
extern int sceKernelIsCpuIntrSuspended(u32);
extern int sceKernelIsCpuIntrEnable(void);
extern int sceKernelRegisterSubIntrHandler(int,int,void*,void*);
extern int sceKernelReleaseSubIntrHandler(int,int);
extern int sceKernelEnableSubIntr(int,int);
extern int sceKernelDisableSubIntr(int,int);
extern void read_controls(u32 *);
extern void write_controls(u32,u32,u32,u32);

static volatile u32 hits,sub,arg,gp,inside,enabled_inside;
static u32 controls_seen[4];
#ifdef PROBE_NATIVE
extern u32 read_gp(void);
extern void write_gp(u32 v);
#else
static u32 read_gp(void) { u32 v; __asm__ volatile("move %0,$gp":"=r"(v)); return v; }
static void write_gp(u32 v) { __asm__ volatile("move $gp,%0"::"r"(v)); }
#endif
static int handler(int a,void *b) {
    hits++; sub=a; arg=(u32)(unsigned long)b;gp=read_gp();
    /* The user module cannot import the SDK's kernel-only context query.
     * Keep its record field reserved; do not mistake a link error for data. */
    inside=0;enabled_inside=sceKernelIsCpuIntrEnable();
    read_controls(controls_seen);
    write_controls(0x43210,0x12345,0xa05,0x76543210);
    return 0;
}
static void record(u32 id,u32 result) {
    u32 values[18]={id,result,hits,sub,arg,gp,inside,enabled_inside,
                  (u32)sceKernelIsCpuIntrEnable(),read_gp(),
                  controls_seen[0],controls_seen[1],controls_seen[2],controls_seen[3]};
    read_controls(values+14);
    char text[176];unsigned used=0;const char *digits="0123456789abcdef";
    for (unsigned i=0;i<18;i++) {
        for (int shift=28;shift>=0;shift-=4) text[used++]=digits[(values[i]>>shift)&15];
        text[used++]=' ';
    }
    text[used++]='\n';sceIoWrite(1,text,used);
}
static void spin(u32 usec) {
    u32 begin=sceKernelGetSystemTimeLow();
    while (sceKernelGetSystemTimeLow()-begin<usec) {}
}
int module_start(unsigned args,void *argp) {
    (void)args;(void)argp;
    write_gp(0x10203040);
    write_controls(0xabcde,0x54321,0xf05,0x12345678);
    record(0,sceKernelIsCpuIntrEnable());
    u32 outer=sceKernelCpuSuspendIntr();record(1,outer);
    u32 inner=sceKernelCpuSuspendIntr();record(2,inner);
    record(3,sceKernelIsCpuIntrSuspended(outer));
    record(4,sceKernelIsCpuIntrSuspended(inner));
    sceKernelCpuResumeIntr(inner);record(5,0);
    sceKernelCpuResumeIntrWithSync(outer);record(6,0);
    record(7,0);
    record(8,sceKernelEnableSubIntr(30,3));
    record(9,sceKernelDisableSubIntr(30,3));
    record(10,sceKernelReleaseSubIntrHandler(30,3));
    record(11,sceKernelRegisterSubIntrHandler(30,3,handler,(void*)0x2468ace0));
    record(12,sceKernelRegisterSubIntrHandler(30,3,handler,(void*)0x11111111));
    write_gp(0x55667788);
    record(13,sceDisplayWaitVblankStart());
    record(14,sceKernelEnableSubIntr(30,3));
    record(15,sceDisplayWaitVblankStart());
    record(16,sceDisplayWaitVblankStart());
    record(17,sceKernelDisableSubIntr(30,3));
    record(18,sceDisplayWaitVblankStart());
    record(19,sceKernelEnableSubIntr(30,3));
    record(20,sceDisplayWaitVblankStart());
    outer=sceKernelCpuSuspendIntr();record(21,outer);
    spin(60000);record(22,0);
    sceKernelCpuResumeIntr(outer);record(23,0);
    record(24,sceDisplayWaitVblankStart());
    record(25,sceKernelReleaseSubIntrHandler(30,3));
    record(26,sceDisplayWaitVblankStart());
    record(27,sceKernelReleaseSubIntrHandler(30,3));
    record(28,sceKernelRegisterSubIntrHandler(30,3,handler,(void*)0xabcdef12));
    record(29,sceDisplayWaitVblankStart());
    record(30,sceKernelEnableSubIntr(30,3));
    record(31,sceDisplayWaitVblankStart());
    record(32,sceKernelReleaseSubIntrHandler(30,3));
    const int numbers[]={-1,0,1,25,30,66,67};
    for (unsigned i=0;i<7;i++) {
        int n=numbers[i];
        record(100+i*3,sceKernelRegisterSubIntrHandler(n,31,handler,(void*)0));
        record(101+i*3,sceKernelEnableSubIntr(n,31));
        record(102+i*3,sceKernelReleaseSubIntrHandler(n,31));
    }
    const int subs[]={-1,0,31,32,255,256};
    for (unsigned i=0;i<6;i++) {
        int n=subs[i];
        record(200+i*2,sceKernelRegisterSubIntrHandler(30,n,handler,(void*)0));
        record(201+i*2,sceKernelReleaseSubIntrHandler(30,n));
    }
    record(300,sceKernelRegisterSubIntrHandler(30,3,(void*)0,(void*)0));
    record(301,sceKernelReleaseSubIntrHandler(30,3));
    sceKernelExitGame();return 0;
}
