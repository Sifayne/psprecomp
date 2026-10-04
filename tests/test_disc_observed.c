/* Run the project-authored disc probe against our I/O adapter, with the same
 * generated image mounted, and compare every record with the recorded
 * external-executable output. */
#include "psprecomp/hle.h"
#include "psprecomp/dispatch.h"
#include "psprecomp/sched.h"
#include "crypto/sha1.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PROBE_NATIVE
#define module_start probe_main
#include "provenance/disc/probe.c"
#undef module_start
enum { NAME=0x08804000u, DATA=0x08810000u, STAT=0x08805000u, DIRENT=0x08806000u, RESULT=0x08807000u };
static FILE *reference;
static unsigned records,checks,failures;
static unsigned case_failures[4096],case_shown[4096];
#define CHECK(c) do { checks++; if (!(c)) { failures++; if (failures<16) fprintf(stderr,"%d: %s\n",__LINE__,#c); } } while (0)
volatile u32 *buffer(void) { return psp_mem_ptr(DATA,sizeof data); }
u32 *stat_buffer(void) { return psp_mem_ptr(STAT,sizeof stat_words); }
u32 *dirent_buffer(void) { return psp_mem_ptr(DIRENT,sizeof dirent_words); }
s64 *result_slot(void) { return psp_mem_ptr(RESULT,8); }
static uint32_t call(const char *api,uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t e) {
    psp_cpu.r[PSP_REG_A0]=a;psp_cpu.r[PSP_REG_A1]=b;psp_cpu.r[PSP_REG_A2]=c;psp_cpu.r[PSP_REG_A3]=d;
    psp_cpu.r[PSP_REG_T0]=e;
    psp_hle_call(psp_nid(api));return psp_cpu.r[PSP_REG_V0];
}
static uint32_t path_call(const char *api,const char *path,uint32_t b,uint32_t c) {
    psp_mem_write_block(NAME,path,(uint32_t)strlen(path)+1);return call(api,NAME,b,c,0,0);
}
static uint32_t guest_of(const void *p) {
    if (p==(const void*)buffer()) return DATA;
    if (p==stat_buffer()) return STAT;
    if (p==dirent_buffer()) return DIRENT;
    if (p==result_slot()) return RESULT;
    CHECK(0);return 0;
}
int sceIoWrite(int fd,const void *text,unsigned size) {
    char actual[256],expected[256];CHECK(fd==1 && size<sizeof actual);
    memcpy(actual,text,size);actual[size]=0;
    if (!fgets(expected,sizeof expected,reference)) { CHECK(0);return (int)size; }
    unsigned av[32]={0},ev[32]={0};unsigned na=0,ne=0;
    char *p=actual;while (na<32) { char *end;unsigned long v=strtoul(p,&end,16);if (end==p) break;av[na++]=(unsigned)v;p=end; }
    p=expected;while (ne<32) { char *end;unsigned long v=strtoul(p,&end,16);if (end==p) break;ev[ne++]=(unsigned)v;p=end; }
    checks++;
    if (na!=ne) { failures++;fprintf(stderr,"record %u: %u fields, recorded %u\n",records,na,ne); }
    for (unsigned i=0;i<na && i<ne;i++) {
        checks++;
        if (av[i]==ev[i]) continue;
        failures++;
        unsigned id=ev[0]*256+(ev[1]&255);if (id>=4096) id=4095;
        case_failures[id]++;
        if (case_shown[id]++<4)
            fprintf(stderr,"kind %u case %u field %u: ours %08x, recorded %08x\n",ev[0],ev[1],i,av[i],ev[i]);
    }
    records++;return (int)size;
}
void sceKernelExitGame(void) {}
int sceKernelDelayThread(unsigned us) { return (int)call("sceKernelDelayThread",us,0,0,0,0); }
int sceIoOpen(const char *f,int flags,int mode) { return (int)path_call("sceIoOpen",f,(uint32_t)flags,(uint32_t)mode); }
int sceIoOpenAsync(const char *f,int flags,int mode) { return (int)path_call("sceIoOpenAsync",f,(uint32_t)flags,(uint32_t)mode); }
int sceIoClose(int fd) { return (int)call("sceIoClose",(uint32_t)fd,0,0,0,0); }
int sceIoCloseAsync(int fd) { return (int)call("sceIoCloseAsync",(uint32_t)fd,0,0,0,0); }
int sceIoRead(int fd,void *b,unsigned n) { return (int)call("sceIoRead",(uint32_t)fd,guest_of(b),n,0,0); }
int sceIoReadAsync(int fd,void *b,unsigned n) { return (int)call("sceIoReadAsync",(uint32_t)fd,guest_of(b),n,0,0); }
s64 probeLseek(int fd,s64 off,int whence) {
    uint32_t lo=call("sceIoLseek",(uint32_t)fd,0,(uint32_t)off,(uint32_t)(off>>32),(uint32_t)whence);
    return (s64)(((uint64_t)psp_cpu.r[PSP_REG_V1]<<32)|lo);
}
int probeLseekAsync(int fd,s64 off,int whence) {
    return (int)call("sceIoLseekAsync",(uint32_t)fd,0,(uint32_t)off,(uint32_t)(off>>32),(uint32_t)whence);
}
int sceIoLseek32(int fd,int off,int whence) { return (int)call("sceIoLseek32",(uint32_t)fd,(uint32_t)off,(uint32_t)whence,0,0); }
int sceIoGetstat(const char *f,void *s) { return (int)path_call("sceIoGetstat",f,guest_of(s),0); }
int sceIoDopen(const char *d) { return (int)path_call("sceIoDopen",d,0,0); }
int sceIoDread(int fd,void *d) { return (int)call("sceIoDread",(uint32_t)fd,guest_of(d),0,0,0); }
int sceIoDclose(int fd) { return (int)call("sceIoDclose",(uint32_t)fd,0,0,0,0); }
int sceIoPollAsync(int fd,s64 *r) { return (int)call("sceIoPollAsync",(uint32_t)fd,guest_of(r),0,0,0); }
int sceIoWaitAsync(int fd,s64 *r) { return (int)call("sceIoWaitAsync",(uint32_t)fd,guest_of(r),0,0,0); }
int sceIoWaitAsyncCB(int fd,s64 *r) { return (int)call("sceIoWaitAsyncCB",(uint32_t)fd,guest_of(r),0,0,0); }
int sceIoGetAsyncStat(int fd,int poll,s64 *r) { return (int)call("sceIoGetAsyncStat",(uint32_t)fd,(uint32_t)poll,guest_of(r),0,0); }
static uint32_t drive_call(const char *api,int unit,const char *drive) {
    psp_mem_write_block(NAME,drive,(uint32_t)strlen(drive)+1);return call(api,(uint32_t)unit,NAME,0,0,0);
}
int sceUmdActivate(int unit,const char *drive) { return (int)drive_call("sceUmdActivate",unit,drive); }
int sceUmdDeactivate(int unit,const char *drive) { return (int)drive_call("sceUmdDeactivate",unit,drive); }
int sceUmdWaitDriveStatWithTimer(int state,unsigned timeout) { return (int)call("sceUmdWaitDriveStatWithTimer",(uint32_t)state,timeout,0,0,0); }
int sceUmdGetDriveStat(void) { return (int)call("sceUmdGetDriveStat",0,0,0,0,0); }
int main(int argc,char **argv) {
    if (argc!=3 || !(reference=fopen(argv[1],"r"))) return 2;
    if (psp_mem_init()!=0) return 2;
    psp_hle_init();psp_cpu_reset();psp_sched_set_threading(0);
    psp_io_set_umd_image(argv[2]);
    probe_main(0,NULL);
    CHECK(fgetc(reference)==EOF);fclose(reference);psp_mem_free();
    for (unsigned id=0;id<4096;id++)
        if (case_failures[id]) fprintf(stderr,"kind %u case %u: %u mismatching fields\n",id/256,id%256,case_failures[id]);
    printf("disc observations: %u records, %u checks, %u failures\n",records,checks,failures);
    return failures?1:0;
}
