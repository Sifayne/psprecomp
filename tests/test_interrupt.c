/* Execute the same locally authored SDK probe as the external capture, with
 * imports adapted to our HLE. The oracle comes only from its recorded output.
 * Native CPU/scheduler ownership checks follow separately. */
#include "psprecomp/hle.h"
#include "psprecomp/interrupt.h"
#include "psprecomp/dispatch.h"
#include "psprecomp/clock.h"
#include "psprecomp/sched.h"
#include "psprecomp/vfpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HANDLER 0x08804000u
#define STACK 0x09fff000u
static FILE *observations;
static unsigned failures, records, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
    fprintf(stderr,"line %u: %s\n",__LINE__,#c); } } while (0)

static uint32_t invoke(uint32_t nid,uint32_t a,uint32_t b,uint32_t c,uint32_t d) {
    psp_cpu.r[PSP_REG_A0]=a;psp_cpu.r[PSP_REG_A1]=b;
    psp_cpu.r[PSP_REG_A2]=c;psp_cpu.r[PSP_REG_A3]=d;
    psp_hle_call(nid);return psp_cpu.r[PSP_REG_V0];
}
uint32_t read_gp(void) { return psp_cpu.r[PSP_REG_GP]; }
void write_gp(uint32_t value) { psp_cpu.r[PSP_REG_GP]=value; }
void read_controls(uint32_t *values) {
    for (unsigned i=0;i<3;i++) values[i]=psp_mfvc(i);
    values[3]=psp_mfvc(8);
}
void write_controls(uint32_t a,uint32_t b,uint32_t c,uint32_t d) {
    psp_mtvc(0,a);psp_mtvc(1,b);psp_mtvc(2,c);psp_mtvc(8,d);
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
    char *a=actual,*b=reference;
    for (unsigned i=0;i<18;i++) {
        char *ae,*be;
        unsigned long av=strtoul(a,&ae,16), bv=strtoul(b,&be,16);
        if (i==13 || i==17) bv=hw_rcx(bv);   /* rcx0 seen in the handler, and now */
        checks++;
        if (ae==a || be==b || av!=bv) {
            if (failures++<12) fprintf(stderr,"record %u field %u: %08lx != %08lx\n",records,i,av,bv);
        }
        a=ae;b=be;
    }
    records++;return count;
}
void sceKernelExitGame(void) {}
uint32_t sceKernelGetSystemTimeLow(void) { return invoke(0x369ED59D,0,0,0,0); }
int sceDisplayWaitVblankStart(void) { return invoke(0x984C27E7,0,0,0,0); }
uint32_t sceKernelCpuSuspendIntr(void) { return invoke(0x092968F4,0,0,0,0); }
void sceKernelCpuResumeIntr(uint32_t flags) { invoke(0x5F10D406,flags,0,0,0); }
void sceKernelCpuResumeIntrWithSync(uint32_t flags) { invoke(0x3B84732D,flags,0,0,0); }
int sceKernelIsCpuIntrSuspended(uint32_t flags) { return invoke(0x47A0B729,flags,0,0,0); }
int sceKernelIsCpuIntrEnable(void) { return invoke(0xB55249D2,0,0,0,0); }
int sceKernelRegisterSubIntrHandler(int irq,int sub,void *fn,void *data) {
    return invoke(0xCA04A2B9,irq,sub,fn?HANDLER:0,(uint32_t)(uintptr_t)data);
}
int sceKernelReleaseSubIntrHandler(int irq,int sub) { return invoke(0xD61E6961,irq,sub,0,0); }
int sceKernelEnableSubIntr(int irq,int sub) { return invoke(0xFB8E22EC,irq,sub,0,0); }
int sceKernelDisableSubIntr(int irq,int sub) { return invoke(0x8A389411,irq,sub,0,0); }
#define PROBE_NATIVE
#define module_start probe_main
#include "provenance/intr/probe.c"
#undef module_start

static void native_handler(void) {
    CHECK(psp_interrupt_in_handler());
    handler(psp_arg(0),(void*)(uintptr_t)psp_arg(1));
}
static void reset(void) {
    psp_sched_reset();psp_sched_set_threading(0);psp_clock_reset();psp_display_reset();
    psp_interrupt_reset();psp_ktimer_reset();psp_cpu_reset();psp_vfpu_reset();
    psp_cpu.r[PSP_REG_SP]=STACK;
    CHECK(psp_interrupt_set_module(0x08800000,0x08900000,0x31415926)==0);
}
static void destructive_handler(void) {
    CHECK(psp_interrupt_in_handler());
    CHECK(!psp_interrupt_call(HANDLER,0,0,0,0));
    CHECK(psp_cpu.r[PSP_REG_GP]==0x31415926);
    memset(&psp_cpu,0xa5,sizeof psp_cpu);
    for (unsigned i=0;i<16;i++) psp_mtvc(i,0);
}
static void native_context(void) {
    reset();psp_register(HANDLER,destructive_handler);
    memset(&psp_cpu,0x35,sizeof psp_cpu);psp_cpu.r[PSP_REG_SP]=STACK;
    psp_cpu.r[PSP_REG_ZERO]=0;
    for (unsigned i=0;i<16;i++) psp_mtvc(i,0x123400+i);
    const psp_cpu_state before=psp_cpu;
    uint32_t controls[16];for (unsigned i=0;i<16;i++) controls[i]=psp_mfvc(i);
    CHECK(psp_interrupt_call(HANDLER,psp_interrupt_handler_gp(HANDLER),3,5,7));
    CHECK(memcmp(&before,&psp_cpu,sizeof before)==0);
    for (unsigned i=0;i<16;i++) CHECK(psp_mfvc(i)==controls[i]);
    CHECK(!psp_interrupt_in_handler());
    sceKernelCpuSuspendIntr();
    CHECK(!psp_interrupt_call(HANDLER,0,0,0,0));
    sceKernelCpuResumeIntr(1);
}
/* SDK samples/gu/vsync30FPS uses a Vblank handler to signal a semaphore.
 * Exercise that pattern while every guest worker is asleep: delivery must
 * wake the worker without transferring the scheduler token inside the IRQ. */
static uint32_t idle_sema;
static unsigned idle_hits,idle_completed;
static void idle_handler(void) {
    CHECK(psp_interrupt_in_handler());
    idle_hits++;
    CHECK(invoke(0x3F53E640,idle_sema,1,0,0)==0);
    if (idle_hits==2) sceKernelDisableSubIntr(30,2);
}
static void idle_worker(void) {
    for (unsigned i=0;i<2;i++) {
        CHECK(invoke(0x4E3A1105,idle_sema,1,0,0)==0);
        CHECK(!psp_interrupt_in_handler());
    }
    idle_completed=1;
}
static void native_idle_wakeup(void) {
    reset();idle_hits=idle_completed=0;
    psp_mem_write_block(0x08805000,"IRQ semaphore",14);
    idle_sema=invoke(0xD6DA4BA1,0x08805000,0,0,1);
    CHECK((int32_t)idle_sema>0);
    psp_register(HANDLER,idle_handler);
    CHECK(invoke(0xCA04A2B9,30,2,HANDLER,0)==0);
    CHECK(sceKernelEnableSubIntr(30,2)==0);
    psp_register(0x08806000,idle_worker);
    psp_sched_set_threading(1);
    CHECK(psp_sched_spawn(0x7000,0x08806000,0x09ffe000,0,0,0,32)==0);
    CHECK(psp_sched_drain(2)==0);
    CHECK(idle_completed && idle_hits==2);
    psp_sched_join_all();psp_sched_reset();
    invoke(0x28B6489C,idle_sema,0,0,0);
}
int main(int argc,char **argv) {
    if (argc!=2 || !(observations=fopen(argv[1],"r"))) return 2;
    if (psp_mem_init()!=0) return 2;
    psp_hle_init();reset();psp_register(HANDLER,native_handler);
    probe_main(0,NULL);
    CHECK(records==68);CHECK(fgetc(observations)==EOF);fclose(observations);
    native_context();
    native_idle_wakeup();
    psp_sched_reset();psp_mem_free();
    printf("interrupt: %u observations, %u checks, %u failures\n",records,checks,failures);
    return failures?1:0;
}
