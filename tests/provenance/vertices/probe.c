/* Project-authored rendering experiment. Command encodings and vertex field
 * declarations come from BSD PSPSDK guInternal.h / pspgu.h, not an emulator. */
typedef unsigned int u32;
extern int sceIoWrite(int,const void*,unsigned);
extern void sceKernelExitGame(void);
extern int sceGeListEnQueue(const void*,void*,int,void*);
extern int sceGeListSync(int,int);
#ifdef PROBE_NATIVE
extern u32 guest_address(const void*);
extern volatile u32 *framebuffer(void);
#else
static u32 guest_address(const void *p) { return (u32)p; }
static volatile u32 *framebuffer(void) { return (volatile u32*)0x44000000; }
#endif
static u32 commands[1024] __attribute__((aligned(16)));
static unsigned used;
#include "cases.h"
static void command(unsigned reg,u32 value) { commands[used++]=(reg<<24)|(value&0xffffff); }
static void address(unsigned reg,const void *pointer) {
    u32 value=guest_address(pointer);command(0x10,(value>>8)&0xf0000);command(reg,value);
}
static void setup(void) {
    used=0;
    /* Explicitly disable optional rendering stages for independent points. */
    for (unsigned reg=0x17;reg<=0x28;reg++) command(reg,0);
    command(0xd3,0);command(0xe8,0);command(0xe9,0);command(0xe7,1);
    command(0xd2,3);command(0x9c,0);command(0x9d,0x40000|64);
    command(0x15,0);command(0x16,63|(63<<10));
    command(0xd4,0);command(0xd5,63|(63<<10));
    command(0x13,0);command(0x4c,0);command(0x4d,0);
    command(0x55,0xffffff);command(0x58,255);command(0x53,0);command(0x51,0);
    volatile u32 *pixels=framebuffer();
    for (unsigned i=0;i<64*64;i++) pixels[i]=0;
}
static void identity(unsigned selector,unsigned components) {
    command(selector,0);
    for (unsigned i=0;i<components;i++) {
        unsigned row=i%(components==16?4:3),col=i/(components==16?4:3);
        command(selector+1,row==col?0x3f8000:0);
    }
}
static void matrices(const VertexCase *test) {
    identity(0x3a,12);identity(0x3c,12);identity(0x3e,16);
    command(0x3a,9);command(0x3b,test->world_x>>8);
    if (test->primitive==3) { command(0x3e,11);command(0x3f,0xbf8000); }
    command(0x42,0x420000);command(0x43,0xc20000);command(0x44,0x46fffe);
    command(0x45,0x420000);command(0x46,0x420000);command(0x47,0x46fffe);
    command(0xd6,0);command(0xd7,65535);
    command(0x2a,0);
    for (unsigned i=0;i<96;i++) command(0x2b,test->bones[i]>>8);
    if (test->lighting) {
        command(0x17,1);command(0x18,1);command(0x5e,0);command(0x5f,0);
        command(0x54,0);command(0x55,0);command(0x56,0xffffff);command(0x57,0);
        command(0x5c,0);command(0x5d,255);
        command(0x63,0);command(0x64,0);command(0x65,0x3f8000);
        command(0x8f,0);command(0x90,0xffffff);command(0x91,0);
    }
}
static void output(u32 kind,u32 id,u32 a,u32 b,u32 c) {
    u32 words[]={kind,id,a,b,c};char text[48];unsigned length=0;
    const char *digits="0123456789abcdef";
    for (unsigned i=0;i<5;i++) {
        for (int bit=28;bit>=0;bit-=4) text[length++]=digits[(words[i]>>bit)&15];
        text[length++]=' ';
    }
    text[length++]='\n';sceIoWrite(1,text,length);
}
static unsigned finish(unsigned id) {
    command(0x0f,0);command(0x0c,0);
    int queue=sceGeListEnQueue(commands,(void*)0,-1,(void*)0);
    int status=sceGeListSync(queue,0);
    unsigned count=0;volatile u32 *pixels=framebuffer();
    for (unsigned i=0;i<64*64;i++) if (pixels[i]) count++;
    output(0,id,(u32)status,count,queue>=0?0:(u32)queue);
    for (unsigned y=0;y<64;y++) for (unsigned x=0;x<64;x++)
        if (pixels[y*64+x]) output(1,id,x,y,pixels[y*64+x]);
    return count;
}
struct PlainVertex { u32 color;short x,y,z;unsigned short pad; };
static struct PlainVertex plain[4] __attribute__((aligned(16)))={
    {0xff0000ff,10,10,0,0},{0xff00ff00,20,10,0,0},
    {0xffff0000,30,10,0,0},{0xffffffff,40,10,0,0}};
static unsigned char index8[4] __attribute__((aligned(16)))={2,0,1,3};
static unsigned short index16[4] __attribute__((aligned(16)))={2,0,1,3};
static unsigned run_vertex_case(const VertexCase *test) {
    setup();matrices(test);command(0x12,test->type);address(1,test->bytes);
    if (test->primitive) command(4,(test->primitive<<16)|test->count);
    else for (unsigned vertex=0;vertex<test->count;vertex++) command(4,1);
    return finish(test->id);
}
int module_start(unsigned args,void *argp) {
    (void)args;(void)argp;unsigned total=0;
    for (unsigned test=0;test<6;test++) {
        setup();
        unsigned index_format=test==1?1:test==2?2:0;
        command(0x12,0x800000|(7<<2)|(2<<7)|(index_format<<11));
        address(1,plain);
        if (index_format) address(2,index_format==1?(void*)index8:(void*)index16);
        if (test==3) { command(4,0);command(4,2);command(4,1); }
        else for (unsigned i=0;i<3;i++) {
            if (test==4) address(1,plain);
            if (test==5 && i==1) command(4,0);
            command(4,1);
        }
        total+=finish(test);
    }
    for (unsigned i=0;i<VERTEX_CASE_COUNT;i++) {
        total+=run_vertex_case(vertex_cases+i);
    }
    output(2,6+VERTEX_CASE_COUNT,total,0,0);sceKernelExitGame();return 0;
}
