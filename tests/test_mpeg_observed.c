#include "psprecomp/hle.h"
#include "psprecomp/dispatch.h"
#include "psprecomp/sched.h"
#include "crypto/sha1.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PROBE_NATIVE
#define module_start probe_main
#include "provenance/mpeg/probe.c"
#undef module_start
enum { CTX=0x08900000, DATA=0x08a00000, RING=0x08810000, AU=0x08810100,
       HANDLE=0x08810200, ALIAS=0x08810204, OUTPUT=0x08810300, CALLBACK=0x08811000 };
static FILE *reference;
static unsigned records,checks,failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr,"%d: %s\n",__LINE__,#c); } } while (0)
u32 guest_address(const void *p) {
    uintptr_t value=(uintptr_t)p;
    if (p==(void*)callback) return CALLBACK;
    if (p==&handle) return HANDLE;
    const void *buffers[]={context,data,ring,au};
    const unsigned sizes[]={sizeof context,sizeof data,sizeof ring,sizeof au};
    const u32 addresses[]={CTX,DATA,RING,AU};
    for (unsigned i=0;i<4;i++) {
        uintptr_t begin=(uintptr_t)buffers[i];
        if (value>=begin && value<begin+sizes[i]) return addresses[i]+(u32)(value-begin);
    }
    CHECK(0);return 0;
}
void set_gp(u32 value) { psp_cpu.r[PSP_REG_GP]=value; }
static u32 call(const char *name,u32 a,u32 b,u32 c,u32 d) {
    psp_cpu.r[PSP_REG_A0]=a;psp_cpu.r[PSP_REG_A1]=b;
    psp_cpu.r[PSP_REG_A2]=c;psp_cpu.r[PSP_REG_A3]=d;
    psp_hle_call(psp_nid(name));return psp_cpu.r[PSP_REG_V0];
}
static void read_ring(void) { psp_mem_read_block(ring,RING,sizeof ring); }
static u32 mpeg_address(u32 *p) {
    u32 where=p==&handle?HANDLE:ALIAS;psp_write32(where,*p);return where;
}
int sceIoWrite(int fd,const void *text,unsigned size) {
    char actual[256],expected[256];CHECK(fd==1 && size<sizeof actual);
    memcpy(actual,text,size);actual[size]=0;CHECK(fgets(expected,sizeof expected,reference)!=NULL);
    char *a=actual,*e=expected;unsigned field=0;
    for (;;) {
        char *an,*en;u32 av=strtoul(a,&an,16),ev=strtoul(e,&en,16);
        if (an==a || en==e) { CHECK(an==a && en==e);break; }
        checks++;
        if (av!=ev && failures++<20) fprintf(stderr,"record %u field %u: %08x != %08x\n",records,field,av,ev);
        a=an;e=en;field++;
    }
    records++;return size;
}
void sceKernelExitGame(void) {}
/* The native harness links these HLE modules directly. External firmware
 * module loading is probe setup, not part of this test's result oracle. */
int sceUtilityLoadAvModule(int module) { CHECK(module==0 || module==3);return 0; }
int sceKernelSetCompiledSdkVersion(int version) { return call("sceKernelSetCompiledSdkVersion",version,0,0,0); }
int sceMpegInit(void) { return call("sceMpegInit",0,0,0,0); }
void sceMpegFinish(void) { call("sceMpegFinish",0,0,0,0); }
int sceMpegQueryMemSize(int v) { return call("sceMpegQueryMemSize",v,0,0,0); }
int sceMpegRingbufferQueryMemSize(int n) { return call("sceMpegRingbufferQueryMemSize",n,0,0,0); }
int probeConstruct(void *r,int n,void *d,int size,int(*cb)(void*,int,void*),void *arg) {
    CHECK(r==ring && d==data && cb==callback);
    psp_mem_write_block(RING,ring,sizeof ring);psp_mem_write_block(DATA,data,sizeof data);
    psp_cpu.r[PSP_REG_T0]=CALLBACK;psp_cpu.r[PSP_REG_T1]=(u32)(uintptr_t)arg;
    int result=call("sceMpegRingbufferConstruct",RING,n,DATA,size);read_ring();return result;
}
void sceMpegRingbufferDestruct(void *r) { CHECK(r==ring);call("sceMpegRingbufferDestruct",RING,0,0,0);read_ring(); }
int sceMpegRingbufferAvailableSize(void *r) { CHECK(r==ring);return call("sceMpegRingbufferAvailableSize",RING,0,0,0); }
int probeCreate(u32 *h,void *storage,int size,void *r,int width,int mode,int top) {
    CHECK(h==&handle && r==ring);
    psp_mem_write_block(CTX,context,sizeof context);psp_write32(HANDLE,handle);
    psp_cpu.r[PSP_REG_T0]=width;psp_cpu.r[PSP_REG_T1]=mode;psp_cpu.r[PSP_REG_T2]=top;
    int result=call("sceMpegCreate",HANDLE,guest_address(storage),size,RING);
    handle=psp_read32(HANDLE);read_ring();return result;
}
void sceMpegDelete(u32 *h) { call("sceMpegDelete",mpeg_address(h),0,0,0);read_ring(); }
int sceMpegQueryAtracEsSize(u32 *h,int *es,int *out) {
    psp_write32(OUTPUT,*es);psp_write32(OUTPUT+4,*out);
    int result=call("sceMpegQueryAtracEsSize",mpeg_address(h),OUTPUT,OUTPUT+4,0);
    *es=psp_read32(OUTPUT);*out=psp_read32(OUTPUT+4);return result;
}
int sceMpegInitAu(u32 *h,void *buffer,void *unit) {
    CHECK(unit==au);psp_mem_write_block(AU,au,sizeof au);
    int result=call("sceMpegInitAu",mpeg_address(h),(u32)(uintptr_t)buffer,AU,0);
    psp_mem_read_block(au,AU,sizeof au);return result;
}
void *sceMpegMallocAvcEsBuf(u32 *h) { return (void*)(uintptr_t)call("sceMpegMallocAvcEsBuf",mpeg_address(h),0,0,0); }
void sceMpegFreeAvcEsBuf(u32 *h,void *buffer) { call("sceMpegFreeAvcEsBuf",mpeg_address(h),(u32)(uintptr_t)buffer,0,0); }
int sceMpegQueryStreamSize(void *buffer,int *out) {
    CHECK(buffer==data);psp_mem_write_block(DATA,data,sizeof data);psp_write32(OUTPUT,*out);
    int r=call("sceMpegQueryStreamSize",DATA,OUTPUT,0,0);*out=psp_read32(OUTPUT);return r;
}
int sceMpegQueryStreamOffset(u32 *h,void *buffer,int *out) {
    CHECK(buffer==data);psp_mem_write_block(DATA,data,sizeof data);psp_write32(OUTPUT,*out);
    int r=call("sceMpegQueryStreamOffset",mpeg_address(h),DATA,OUTPUT,0);*out=psp_read32(OUTPUT);return r;
}
void *sceMpegRegistStream(u32 *h,int type,int channel) { return (void*)(uintptr_t)call("sceMpegRegistStream",mpeg_address(h),type,channel,0); }
/* The probe passes the first argument as the SDK declares it and as an
 * address; both arrive here as guest words. */
void sceMpegUnRegistStream(void *mpeg,void *stream) { call("sceMpegUnRegistStream",(u32)(uintptr_t)mpeg,(u32)(uintptr_t)stream,0,0); }
int sceMpegGetAvcAu(u32 *h,void *stream,void *unit,void *unknown) {
    CHECK(unit==au && unknown==NULL);
    int r=call("sceMpegGetAvcAu",mpeg_address(h),(u32)(uintptr_t)stream,AU,0);
    psp_mem_read_block(au,AU,sizeof au);return r;
}
int sceMpegRingbufferPut(void *r,int n,int available) {
    CHECK(r==ring);int result=call("sceMpegRingbufferPut",RING,n,available,0);read_ring();return result;
}
static void native_callback(void) {
    u32 target=psp_arg(0),count=psp_arg(1),arg=psp_arg(2);
    CHECK(target>=DATA && (uint64_t)target+count*2048<=DATA+sizeof data);
    int result=callback(data+target-DATA,count,(void*)(uintptr_t)arg);
    psp_mem_write_block(DATA,data,sizeof data);psp_ret(result);
}
int main(int argc,char **argv) {
    if (argc!=2 || !(reference=fopen(argv[1],"r"))) return 2;
    CHECK(psp_mem_init()==0);psp_cpu_reset();psp_hle_init();psp_sched_set_threading(0);
    psp_mpeg_set_decoding(0);psp_register(CALLBACK,native_callback);
    probe_main(0,NULL);
    CHECK(records==148);CHECK(fgetc(reference)==EOF);fclose(reference);psp_mem_free();
    printf("mpeg observed: %u records, %u checks, %u failures\n",records,checks,failures);
    return failures?1:0;
}
