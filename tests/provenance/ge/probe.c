typedef unsigned int u32;
typedef struct { void (*signal)(int,void*,void*);void *signal_arg;
                 void (*finish)(int,void*,void*);void *finish_arg; } Callbacks;
extern int sceIoWrite(int,const void*,unsigned);
extern void sceKernelExitGame(void);
extern int sceGeSetCallback(Callbacks*);
extern int sceGeUnsetCallback(int);
extern int sceGeListEnQueue(const void*,void*,int,void*);
extern int sceGeListSync(int,int);
extern int sceGeContinue(void);
extern u32 sceKernelCpuSuspendIntr(void);
extern void sceKernelCpuResumeIntr(u32);
extern int sceKernelIsCpuIntrEnable(void);
extern int sceKernelSetCompiledSdkVersion(int);
extern int sceKernelGetCompiledSdkVersion(void);
/* Opaque import copied as an identifier from this game's import table;
 * its behavior is established by this experiment, not an assigned name. */
extern int probeSdk630(int);
extern void read_controls(u32*);
extern void write_controls(u32,u32,u32,u32);
#ifdef PROBE_NATIVE
extern u32 read_gp(void);
extern void write_gp(u32);
extern u32 guest_address(const void*);
#else
static u32 read_gp(void) { u32 v;__asm__ volatile("move %0,$gp":"=r"(v));return v; }
static void write_gp(u32 v) { __asm__ volatile("move $gp,%0"::"r"(v)); }
static u32 guest_address(const void *p) { return (u32)p; }
#endif
static u32 list[16] __attribute__((aligned(16)));
static u32 sublist[16] __attribute__((aligned(16)));
static u32 events[8][10],event_count;
static u32 pc_offset(void *pc) {
    u32 value=(u32)(unsigned long)pc;
    if (value>=guest_address(list) && value<=guest_address(list)+sizeof list)
        return value-guest_address(list);
    if (value>=guest_address(sublist) && value<=guest_address(sublist)+sizeof sublist)
        return 0x10000+value-guest_address(sublist);
    return value;
}
static void event(unsigned kind,int token,void *common,void *pc) {
    unsigned index=event_count++;
    if (index>=8) return;
    u32 *data=events[index];
    data[0]=kind;data[1]=token;data[2]=(u32)(unsigned long)common;
    data[3]=pc_offset(pc);data[4]=read_gp();read_controls(data+5);
    data[9]=sceKernelIsCpuIntrEnable();
    write_controls(0x11111,0x22222,0xf0f,0x98765432);
    write_gp(0xbad0bad0);
}
static void signal_handler(int token,void *arg,void *pc) { event(1,token,arg,pc); }
static void finish_handler(int token,void *arg,void *pc) { event(2,token,arg,pc); }
static void line(const u32 *data,unsigned count) {
    const char *digits="0123456789abcdef";char text[160];unsigned used=0;
    for (unsigned i=0;i<count;i++) {
        for (int shift=28;shift>=0;shift-=4) text[used++]=digits[(data[i]>>shift)&15];
        text[used++]=' ';
    }
    text[used++]='\n';sceIoWrite(1,text,used);
}
static int measure_version(u32 version) {
    int result=sceKernelSetCompiledSdkVersion(version);
    event_count=0;list[0]=0x0f0001c8;list[1]=0x0c000000;
    Callbacks cb={signal_handler,(void*)0x12345678,finish_handler,(void*)0x87654321};
    int callback=sceGeSetCallback(&cb);
    int queue=sceGeListEnQueue(list,(void*)0,callback,(void*)0);
    sceGeListSync(queue,0);sceGeUnsetCallback(callback);
    u32 row[]={2,version,events[0][3],(u32)result,(u32)sceKernelGetCompiledSdkVersion()};
    line(row,5);
    return events[0][3]!=0;
}
int module_start(unsigned args,void *argp) {
    (void)args;(void)argp;
    Callbacks cb={signal_handler,(void*)0x12345678,finish_handler,(void*)0x87654321};
    const u32 versions[]={0,0x01ffffff,0x02000000,0x06030010};
    for (unsigned version=0;version<4;version++) {
    int sdk_result=version==3 ? probeSdk630(versions[version])
                             : sceKernelSetCompiledSdkVersion(versions[version]);
    u32 sdk_readback=sceKernelGetCompiledSdkVersion();
    for (unsigned scenario=0;scenario<5;scenario++) {
        event_count=0;
        for (unsigned i=0;i<16;i++) list[i]=sublist[i]=0;
        if (scenario==0 || scenario==4) { list[0]=0x0f0001c8;list[1]=0x0c000000; }
        else if (scenario==3) {
            u32 address=guest_address(sublist);
            list[0]=0x10000000|((address>>8)&0xf0000);
            list[1]=0x0a000000|(address&0xffffff);
            list[2]=0x0f0001c8;list[3]=0x0c000000;
            sublist[0]=0x0e01007b;sublist[1]=0x0c000000;sublist[2]=0x0b000000;
        } else {
            list[0]=0x0e00007b|(scenario<<16);list[1]=0x0c000000;
            list[2]=0x0f0001c8;list[3]=0x0c000000;
        }
        write_gp(0xaaaa1111);
        int callback=sceGeSetCallback(&cb);
        write_gp(0xbbbb2222);
        write_controls(0xabcde,0x54321,0xf05,0x12345678);
        u32 flags=0;
        if (scenario==4) flags=sceKernelCpuSuspendIntr();
        int queue=sceGeListEnQueue(list,(void*)0,callback,(void*)0);
        u32 before=event_count;
        if (scenario==4) sceKernelCpuResumeIntr(flags);
        int result=sceGeListSync(queue,0);
        u32 header[15]={0,version*16+scenario,(u32)result,event_count,read_gp(),0,0,0,0,
                       callback>=0?0:(u32)callback,queue>=0?0:(u32)queue,before,
                       (u32)sdk_result,versions[version],sdk_readback};
        read_controls(header+5);line(header,15);
        for (unsigned i=0;i<event_count && i<8;i++) {
            u32 row[12]={1,version*16+scenario};
            for (unsigned j=0;j<10;j++) row[j+2]=events[i][j];
            line(row,12);
        }
        sceGeUnsetCallback(callback);
    }
    }
    /* Locate the observed boundary without embedding a prior threshold.
     * Validate the resulting split against separate varied versions. */
    u32 low=0,high=0x06030010;
    while (high-low>1) {
        u32 mid=low+(high-low)/2;
        if (measure_version(mid)) high=mid;else low=mid;
    }
    u32 varied=0x5192026;
    for (unsigned i=0;i<32;i++) {
        varied=varied*1664525u+1013904223u;
        measure_version(varied%0x06030011);
    }
    u32 boundary[]={3,low,high};line(boundary,3);
    sceKernelExitGame();return 0;
}
