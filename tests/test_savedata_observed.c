/* Run our own savedata probe against the native HLE. Permission failures are
 * explicit host-policy cases; the selected observable result contract is exact. */
#include "psprecomp/hle.h"
#include "psprecomp/clock.h"
#include "psprecomp/sched.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define fixture_chmod _chmod
#define fixture_rmdir _rmdir
#else
#include <unistd.h>
#define fixture_chmod chmod
#define fixture_rmdir rmdir
#endif
#define PROBE_NATIVE
#define module_start probe_main
#include "provenance/savedata/probe.c"
#undef module_start
enum { PARAM=0x08804000, PAYLOAD=0x08805000, FILES=0x08806000, PATH=0x08807000 };
static u32 expected[4][32][3];
static unsigned checks,failures,compared,host_policy;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
    fprintf(stderr,"line %u: %s\n",__LINE__,#c); } } while (0)
static uint32_t invoke(u32 nid,u32 a,u32 b,u32 c) {
    psp_cpu.r[PSP_REG_A0]=a;psp_cpu.r[PSP_REG_A1]=b;psp_cpu.r[PSP_REG_A2]=c;
    psp_hle_call(nid);return psp_cpu.r[PSP_REG_V0];
}
u32 savedata_guest_address(const void *p) { return p==payload?PAYLOAD:FILES; }
int sceUtilitySavedataInitStart(void *p) {
    CHECK(p==parameter);
    psp_mem_write_block(PARAM,parameter,sizeof parameter);
    psp_mem_write_block(PAYLOAD,payload,sizeof payload);
    psp_mem_write_block(FILES,files,sizeof files);
    return invoke(0x50c4cd57,PARAM,0,0);
}
int sceUtilitySavedataGetStatus(void) {
    int result=invoke(0x8874dbe0,0,0,0);
    parameter[7]=psp_read32(PARAM+28);
    return result;
}
void sceUtilitySavedataUpdate(int step) {
    invoke(0xd4b95ffb,step,0,0);parameter[7]=psp_read32(PARAM+28);
}
int sceUtilitySavedataShutdownStart(void) { return invoke(0x9790b33c,0,0,0); }
int sceKernelDelayThread(unsigned us) { psp_clock_advance_to(psp_clock_peek()+us);return 0; }
int sceIoRemove(const char *path) {
    psp_mem_write_block(PATH,path,(u32)strlen(path)+1);
    return invoke(0xf27a9c51,PATH,0,0);
}
void sceKernelExitGame(void) {}
int sceIoWrite(int fd,const void *data,unsigned count) {
    CHECK(fd==1);CHECK(count<64);
    char line[64];memcpy(line,data,count);line[count]=0;
    unsigned kind,id,a,b,c;CHECK(sscanf(line,"%x %x %x %x %x",&kind,&id,&a,&b,&c)==5);
    CHECK(kind<4 && id<32);
    if (kind==0 && (id==12 || (id>=13 && id<=16) || id==18)) {
        /* Our transactional replacement differs from the external in-place
         * writes: renaming a read-only slot is allowed when its parent is
         * writable. Verify the actual native outcome, not a false failure. */
#ifndef _WIN32
        if (id==13) {
            uint64_t size=0;int directory=0;
            CHECK(b==0);
            CHECK(psp_io_path_info("ms0:/PSP/SAVEDATA/PRV260919BLOCKED/DATA.BIN",&size,&directory)==0);
            CHECK(size==64 && !directory);
        } else if (id==14) {
            CHECK(b==0);
            CHECK(psp_io_path_info("ms0:/PSP/SAVEDATA/PRV260919BLOCKED",NULL,NULL)<0);
        } else CHECK((int32_t)b<0);
#endif
        host_policy++;return count;
    }
    u32 values[]={a,b,c};compared++;
    for (unsigned i=0;i<3;i++) if (values[i]!=expected[kind][id][i]) {
        failures++;fprintf(stderr,"case %u/%u word %u: %08x != %08x\n",
            kind,id,i,values[i],expected[kind][id][i]);
    }
    return count;
}
static void fixture(const char *name,int directory,int denied,int blocked) {
    char guest[256],host[1024];
    snprintf(guest,sizeof guest,"ms0:/PSP/SAVEDATA/PRV260919%s",name);
    psp_io_mkdir_all(guest);
    const char *files_to_write[]={"PARAM.SFO","DATA.BIN"};
    for (unsigned i=0;i<2;i++) {
        char file[300];snprintf(file,sizeof file,"%s/%s",guest,files_to_write[i]);
        if (i && directory) { psp_io_mkdir_all(file);continue; }
        psp_io_host_path(file,host,sizeof host);
        FILE *f=fopen(host,"wb");CHECK(f!=NULL);if (!f) continue;
        if (!i) fputs("original malformed metadata stimulus",f);
        else for (unsigned b=0;b<64;b++) fputc(b,f);
        fclose(f);
        if (i && denied) fixture_chmod(host,0);
    }
    if (blocked) { psp_io_host_path(guest,host,sizeof host);fixture_chmod(host,0500); }
}
int main(int argc,char **argv) {
    if (argc!=2) return 2;
    FILE *in=fopen(argv[1],"r");if (!in) return 2;
    unsigned k,id,a,b,c,rows=0;
    while (fscanf(in,"%x %x %x %x %x",&k,&id,&a,&b,&c)==5) {
        if (k>=4 || id>=32) return 2;
        expected[k][id][0]=a;expected[k][id][1]=b;expected[k][id][2]=c;rows++;
    }
    fclose(in);if (rows!=29 || psp_mem_init()!=0) return 2;
    psp_hle_init();psp_cpu_reset();psp_sched_set_threading(0);psp_clock_reset();
    psp_io_set_root("savedata-observed-fixture");psp_io_reset();
    fixture("DIRFILE",1,0,0);fixture("READDENIED",0,1,0);
    fixture("BLOCKED",0,0,1);fixture("BROKEN",0,0,0);
    probe_main(0,NULL);
    CHECK(compared+host_policy==rows);
    char host[1024];
    psp_io_host_path("ms0:/PSP/SAVEDATA/PRV260919BLOCKED",host,sizeof host);fixture_chmod(host,0700);
    psp_io_host_path("ms0:/PSP/SAVEDATA/PRV260919READDENIED/DATA.BIN",host,sizeof host);fixture_chmod(host,0600);
    CHECK(psp_io_remove_tree("ms0:/")==0);
    psp_io_reset();psp_io_set_root(".");CHECK(fixture_rmdir("savedata-observed-fixture")==0);
    psp_mem_free();
    printf("Savedata result probe: %u compared records, %u host-policy cases, %u checks, %u failures\n",
        compared,host_policy,checks,failures);
    return failures?1:0;
}
