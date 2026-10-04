typedef unsigned int u32;
extern int sceIoWrite(int fd,const void *data,unsigned count);
extern void sceKernelExitGame(void);
extern int sceKernelCreateCallback(const char *name,int (*cb)(int,int,void*),void *data);
extern int sceKernelCheckCallback(void);
extern int sceKernelDeleteCallback(int cb);
extern int sceUmdGetErrorStat(void);
extern int sceUmdGetDiscInfo(void *data);
extern int sceUmdCheckMedium(void);
extern int sceUmdWaitDriveStatCB(int state,unsigned timeout);
extern int sceUmdWaitDriveStatWithTimer(int state,unsigned timeout);
extern int sceUmdGetDriveStat(void);
extern int sceUmdRegisterUMDCallBack(int cb);
extern int sceUmdUnRegisterUMDCallBack(int cb);
extern int sceUmdActivate(int unit,const char *drive);
extern int sceUmdDeactivate(int unit,const char *drive);

static u32 calls,count,event,common;
static int callback(int a,int b,void *c) {
    calls++;count=a;event=b;common=(u32)c;return 0;
}
static void record(u32 id,u32 result) {
    u32 values[]={id,result,(u32)sceUmdGetDriveStat(),calls,count,event,common};
    char text[64];unsigned used=0;const char *digits="0123456789abcdef";
    for (unsigned i=0;i<7;i++) {
        for (int shift=28;shift>=0;shift-=4) text[used++]=digits[(values[i]>>shift)&15];
        text[used++]=' ';
    }
    text[used++]='\n';sceIoWrite(1,text,used);
}
int module_start(unsigned args,void *argp) {
    (void)args;(void)argp;
    record(0,sceUmdCheckMedium());
    record(1,sceUmdGetErrorStat());
    int cb=sceKernelCreateCallback("UMD probe",callback,(void*)0x13579bdf);
    record(2,cb>=0?0:cb);
    record(3,sceUmdRegisterUMDCallBack(cb));
    record(4,sceKernelCheckCallback());
    record(5,sceUmdActivate(1,"disc0:"));
    record(6,sceUmdWaitDriveStatWithTimer(0x20,1000));
    record(7,sceKernelCheckCallback());
    record(8,sceUmdActivate(1,"disc0:"));
    record(9,sceUmdActivate(1,"disc0:"));
    record(10,sceUmdWaitDriveStatCB(0x20,1000));
    record(11,sceUmdDeactivate(1,"disc0:"));
    record(12,sceKernelCheckCallback());
    record(13,sceUmdActivate(0,"disc0:"));
    record(14,sceKernelCheckCallback());
    record(15,sceUmdActivate(2,"disc0:"));
    record(16,sceKernelCheckCallback());
    record(17,sceUmdActivate(1,(void*)0));
    record(18,sceKernelCheckCallback());
    record(19,sceUmdActivate(1,"other:"));
    record(20,sceKernelCheckCallback());
    record(21,sceUmdActivate(1,"disc0:"));
    record(22,sceUmdWaitDriveStatCB(0x20,1000));
    u32 info[3]={8,0,0x12345678};
    record(23,sceUmdGetDiscInfo(info));
    record(24,info[0]);record(25,info[1]);record(26,info[2]);
    record(27,sceUmdUnRegisterUMDCallBack(cb));
    record(28,sceUmdActivate(1,"disc0:"));
    record(29,sceKernelCheckCallback());
    record(30,sceKernelDeleteCallback(cb));
    sceKernelExitGame();return 0;
}
