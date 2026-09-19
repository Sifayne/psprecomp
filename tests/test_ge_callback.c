/* Run the exact project-authored GE probe against our HLE and compare with
 * recorded output from an external executable. No game data is required. */
#include "psprecomp/hle.h"
#include "psprecomp/interrupt.h"
#include "psprecomp/dispatch.h"
#include "psprecomp/clock.h"
#include "psprecomp/sched.h"
#include "psprecomp/vfpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIST 0x08802000u
#define SUBLIST 0x08803000u
#define CALLBACK 0x08804000u
#define SIGNAL 0x08810000u
#define FINISH 0x08811000u
#define PROBE_NATIVE
#define module_start probe_main
#include "provenance/ge/probe.c"
#undef module_start
static FILE *observations;
static unsigned failures,records,checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
    fprintf(stderr,"line %u: %s\n",__LINE__,#c); } } while (0)

static uint32_t invoke(uint32_t nid,uint32_t a,uint32_t b,uint32_t c,uint32_t d) {
    psp_cpu.r[PSP_REG_A0]=a;psp_cpu.r[PSP_REG_A1]=b;
    psp_cpu.r[PSP_REG_A2]=c;psp_cpu.r[PSP_REG_A3]=d;
    psp_hle_call(nid);return psp_cpu.r[PSP_REG_V0];
}
u32 read_gp(void) { return psp_cpu.r[PSP_REG_GP]; }
void write_gp(u32 value) { psp_cpu.r[PSP_REG_GP]=value; }
void read_controls(u32 *values) {
    for (unsigned i=0;i<3;i++) values[i]=psp_mfvc(i);
    values[3]=psp_mfvc(8);
}
void write_controls(u32 a,u32 b,u32 c,u32 d) {
    psp_mtvc(0,a);psp_mtvc(1,b);psp_mtvc(2,c);psp_mtvc(8,d);
}
u32 guest_address(const void *pointer) {
    if (pointer==list) return LIST;
    if (pointer==sublist) return SUBLIST;
    CHECK(0);return 0;
}
/* The probe ran under an emulator, which keeps an rcx control register as
 * written. A PSP keeps only its 20 state bits, reading back 1.0-shaped:
 * 12345678 -> 3F845678 (vfpuprobe v3 step 185, fw 6.60). Expected rcx0
 * values go through that rule; 0 means no handler has written one yet. */
static unsigned long hw_rcx(unsigned long e) { return e ? 0x3F800000ul | (e & 0xFFFFFul) : 0; }
int sceIoWrite(int fd,const void *data,unsigned count) {
    CHECK(fd==1);
    char reference[256],actual[256];
    CHECK(count<sizeof actual);
    memcpy(actual,data,count);actual[count]=0;
    if (!fgets(reference,sizeof reference,observations)) { CHECK(0);return -1; }
    char *a=actual,*b=reference;unsigned field=0;unsigned long kind=0;
    while (*a && *a!='\n') {
        char *ae,*be;
        unsigned long av=strtoul(a,&ae,16),bv=strtoul(b,&be,16);
        if (ae==a) break;
        if (field==0) kind=bv;
        /* rcx0: a header's after the run, an event's as its handler saw it. */
        if ((kind==0 && field==8) || (kind==1 && field==10)) bv=hw_rcx(bv);
        checks++;
        if (be==b || av!=bv) {
            if (failures++<16) fprintf(stderr,"record %u field %u: %08lx != %08lx\n",records,field,av,bv);
        }
        a=ae;b=be;field++;
    }
    char *end;strtoul(b,&end,16);CHECK(end==b);
    records++;return count;
}
void sceKernelExitGame(void) {}
u32 sceKernelCpuSuspendIntr(void) { return invoke(0x092968F4,0,0,0,0); }
void sceKernelCpuResumeIntr(u32 flags) { invoke(0x5F10D406,flags,0,0,0); }
int sceKernelIsCpuIntrEnable(void) { return invoke(0xB55249D2,0,0,0,0); }
int sceKernelSetCompiledSdkVersion(int value) { return invoke(0x7591C7DB,value,0,0,0); }
int sceKernelGetCompiledSdkVersion(void) { return invoke(0xFC114573,0,0,0,0); }
int probeSdk630(int value) { return invoke(0x1B4217BC,value,0,0,0); }
int sceGeSetCallback(Callbacks *data) {
    psp_write32(CALLBACK,data->signal?SIGNAL:0);
    psp_write32(CALLBACK+4,(u32)(uintptr_t)data->signal_arg);
    psp_write32(CALLBACK+8,data->finish?FINISH:0);
    psp_write32(CALLBACK+12,(u32)(uintptr_t)data->finish_arg);
    return invoke(0xA4FC06A4,CALLBACK,0,0,0);
}
int sceGeUnsetCallback(int id) { return invoke(0x05DB22CE,id,0,0,0); }
int sceGeListEnQueue(const void *data,void *stall,int cb,void *args) {
    CHECK(data==list && !stall && !args);
    psp_mem_write_block(LIST,list,sizeof list);
    psp_mem_write_block(SUBLIST,sublist,sizeof sublist);
    return invoke(0xAB49E76A,LIST,0,cb,0);
}
int sceGeListSync(int id,int mode) { return invoke(0x03444EB4,id,mode,0,0); }
int sceGeContinue(void) { return invoke(0x4C06E472,0,0,0,0); }
static void native_signal(void) {
    CHECK(psp_interrupt_in_handler());
    signal_handler(psp_arg(0),(void*)(uintptr_t)psp_arg(1),(void*)(uintptr_t)psp_arg(2));
}
static void native_finish(void) {
    CHECK(psp_interrupt_in_handler());
    finish_handler(psp_arg(0),(void*)(uintptr_t)psp_arg(1),(void*)(uintptr_t)psp_arg(2));
}
int main(int argc,char **argv) {
    if (argc!=2 || !(observations=fopen(argv[1],"r"))) return 2;
    if (psp_mem_init()!=0) return 2;
    psp_hle_init();psp_cpu_reset();psp_vfpu_reset();psp_sched_set_threading(0);
    psp_clock_reset();psp_display_reset();psp_ge_reset();psp_interrupt_reset();
    psp_cpu.r[PSP_REG_SP]=0x09fff000;
    CHECK(psp_interrupt_set_module(0x08800000,0x08900000,0x27182818)==0);
    psp_register(SIGNAL,native_signal);psp_register(FINISH,native_finish);
    probe_main(0,NULL);
    CHECK(records==112);CHECK(fgetc(observations)==EOF);fclose(observations);
    psp_sched_reset();psp_mem_free();
    printf("ge callback: %u observations, %u checks, %u failures\n",records,checks,failures);
    return failures?1:0;
}
