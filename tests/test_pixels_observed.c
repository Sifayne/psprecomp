/* Run the project-authored pixel probe against our HLE and software renderer
 * and compare every record with the recorded external-executable output. */
#include "psprecomp/hle.h"
#include "psprecomp/render.h"
#include "psprecomp/clock.h"
#include "psprecomp/sched.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PROBE_NATIVE
#define module_start probe_main
#include "provenance/pixels/probe.c"
#undef module_start
#define LIST 0x08802000u
static const u32 bases[B_COUNT]={0x08900000u,0x08910000u,0x08920000u,0x08930000u,0x08940000u};
static const u32 sizes[B_COUNT]={sizeof texture_store,sizeof palette_store,sizeof xsrc_store,
                                 sizeof xdst_store,sizeof verts_store};
static FILE *reference;
static unsigned records,checks,failures;
static unsigned case_failures[1024],case_shown[1024],hardware_differs;
/* Transfers whose stride a 6.60 PSP uses as given and the emulator these
 * records came from does not: 0x408 (206, 217) and 0x7F8 (220). Measured
 * with tools/hwprobe/mpegprobe; see xfer_stride in src/hle/ge.c. */
static int hardware_case(unsigned id) { return id==206 || id==217 || id==220; }
#define CHECK(c) do { checks++; if (!(c)) { failures++; if (failures<16) fprintf(stderr,"%d: %s\n",__LINE__,#c); } } while (0)
volatile u32 *buffer(int which) { return psp_mem_ptr(bases[which],sizes[which]); }
u32 buffer_address(int which) { return bases[which]; }
volatile u32 *framebuffer(void) { return psp_mem_ptr(0x04000000u,FB_WORDS*4); }
static uint32_t invoke(uint32_t nid,uint32_t a,uint32_t b,uint32_t c,uint32_t d) {
    psp_cpu.r[PSP_REG_A0]=a;psp_cpu.r[PSP_REG_A1]=b;
    psp_cpu.r[PSP_REG_A2]=c;psp_cpu.r[PSP_REG_A3]=d;
    psp_hle_call(nid);return psp_cpu.r[PSP_REG_V0];
}
int sceIoWrite(int fd,const void *text,unsigned size) {
    char actual[256],expected[256];CHECK(fd==1 && size<sizeof actual);
    memcpy(actual,text,size);actual[size]=0;
    if (!fgets(expected,sizeof expected,reference)) { CHECK(0);return (int)size; }
    unsigned av[5]={0},ev[5]={0};
    CHECK(sscanf(actual,"%x %x %x %x %x",av,av+1,av+2,av+3,av+4)==5);
    CHECK(sscanf(expected,"%x %x %x %x %x",ev,ev+1,ev+2,ev+3,ev+4)==5);
    for (unsigned i=0;i<5;i++) {
        checks++;
        if (av[i]==ev[i]) continue;
        if (hardware_case(ev[1])) { hardware_differs++; continue; }
        failures++;
        unsigned id=ev[1]<1024?ev[1]:1023;
        case_failures[id]++;
        if (case_shown[id]++<3)
            fprintf(stderr,"case %u kind %u field %u at %u (%u,%u): ours %08x, recorded %08x\n",
                    ev[1],ev[0],i,ev[2],ev[2]%64,ev[2]/64,av[i],ev[i]);
    }
    records++;return (int)size;
}
void sceKernelExitGame(void) {}
int sceGeListEnQueue(const void *data,void *stall,int cb,void *args) {
    CHECK(data==commands && !stall && cb==-1 && !args);
    psp_mem_write_block(LIST,commands,used*sizeof(u32));
    return (int)invoke(0xAB49E76A,LIST,0,(u32)cb,0);
}
int sceGeListSync(int id,int mode) { return (int)invoke(0x03444EB4,(u32)id,(u32)mode,0,0); }
int main(int argc,char **argv) {
    if (argc!=2 || !(reference=fopen(argv[1],"r"))) return 2;
    if (psp_mem_init()!=0) return 2;
    psp_hle_init();psp_cpu_reset();psp_sched_set_threading(0);psp_clock_reset();
    psp_display_reset();psp_ge_reset();
    CHECK(psp_render_select("software")==0);
    probe_main(0,NULL);
    CHECK(fgetc(reference)==EOF);fclose(reference);psp_mem_free();
    for (unsigned id=0;id<1024;id++)
        if (case_failures[id]) fprintf(stderr,"case %u: %u mismatching fields\n",id,case_failures[id]);
    printf("pixel observations: %u records, %u checks, %u failures; %u fields where hardware differs\n",
           records,checks,failures,hardware_differs);
    return failures?1:0;
}
