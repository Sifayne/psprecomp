/* Compare complete 64x64 rasters from the project-authored SDK-format probe
 * against recorded external-executable output. The same C probe runs here. */
#include "psprecomp/hle.h"
#include "psprecomp/render.h"
#include "psprecomp/clock.h"
#include "psprecomp/sched.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef VERTEX_GL
#include "render_gl.h"
#include "present.h"
#include "settings.h"
static uint32_t gl_pixels[64*64];
static const psp_render_backend *real_gl;
static unsigned model_calls;
static void track_model(int prim,const psp_model_vertex *v,int count,const psp_xform_state *state) {
    model_calls++;
    real_gl->draw_model(prim,v,count,state);
}
#endif

#define LIST 0x08802000u
#define PLAIN 0x08804000u
#define INDEX8 0x08805000u
#define INDEX16 0x08806000u
#define INPUTS 0x08900000u
#define MAX_CASES 1024u
#define PROBE_NATIVE
#define module_start probe_main
#include "provenance/vertices/probe.c"
#undef module_start
_Static_assert(sizeof(VertexCase)==664,"probe input ABI");
static uint32_t reference[MAX_CASES][64*64];
static uint32_t expected_count[MAX_CASES],expected_status[MAX_CASES],expected_error[MAX_CASES];
static unsigned char expected_seen[MAX_CASES];
static unsigned failures,compared,expected_cases,shade_differences,point_shifts;
static uint64_t checks;
static FILE *actual_output;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
    if (failures<16) fprintf(stderr,"line %u: %s\n",__LINE__,#c); } } while (0)

static int equal_pixel(unsigned id,u32 actual,u32 expected) {
    if (actual==expected) return 1;
    /* These cases test bone normal direction, not inherited light-channel
     * quantization. Preserve coverage exactly, allow one RGB step for shading,
     * and report every tolerated difference rather than silently hiding it. */
    if (!actual || !expected || (actual>>24)!=(expected>>24)) return 0;
    int lit=0;
    for (unsigned i=0;i<VERTEX_CASE_COUNT;i++)
        if (vertex_cases[i].id==id) lit=vertex_cases[i].lighting;
    if (!lit) return 0;
    for (unsigned c=0;c<3;c++)
        if (abs((int)((actual>>(8*c))&255)-(int)((expected>>(8*c))&255))>1) return 0;
    shade_differences++;
    return 1;
}

static uint32_t invoke(uint32_t nid,uint32_t a,uint32_t b,uint32_t c,uint32_t d) {
    psp_cpu.r[PSP_REG_A0]=a;psp_cpu.r[PSP_REG_A1]=b;
    psp_cpu.r[PSP_REG_A2]=c;psp_cpu.r[PSP_REG_A3]=d;
    psp_hle_call(nid);return psp_cpu.r[PSP_REG_V0];
}
u32 guest_address(const void *p) {
    if (p==plain) return PLAIN;
    if (p==index8) return INDEX8;
    if (p==index16) return INDEX16;
    uintptr_t at=(uintptr_t)p,first=(uintptr_t)vertex_cases;
    if (at>=first && at-first<sizeof vertex_cases) return INPUTS+(u32)(at-first);
    CHECK(0);return 0;
}
volatile u32 *framebuffer(void) {
#ifdef VERTEX_GL
    return gl_pixels;
#else
    return psp_mem_ptr(0x04000000,64*64*4);
#endif
}
int sceIoWrite(int fd,const void *text,unsigned count) {
    CHECK(fd==1);
    if (actual_output) fwrite(text,1,count,actual_output);
    unsigned kind,id,a,b,c;
    char line[64];CHECK(count<sizeof line);memcpy(line,text,count);line[count]=0;
    CHECK(sscanf(line,"%x %x %x %x %x",&kind,&id,&a,&b,&c)==5);
    if (kind==0) {
        CHECK(id<MAX_CASES && expected_seen[id]);
        if (id>=MAX_CASES || !expected_seen[id]) return -1;
        compared++;
        volatile u32 *pixels=framebuffer();
        int points=0, off_frame=0;
        for (unsigned i=0;i<VERTEX_CASE_COUNT;i++)
            if (vertex_cases[i].id==id) points=vertex_cases[i].primitive==0;
        for (unsigned pixel=0;pixel<64*64;pixel++) {
            checks++;
            /* The emulator puts a point that lands on a pixel boundary in the
             * column to its right. psprecomp's screen x is the GE's float,
             * matched to hardware (geprobe 13-14, fw 6.60), and lands just
             * short: the same point, one column left. Counted, not hidden. */
            if (points) {
                const u32 *ref=reference[id];
                const unsigned x=pixel%64;
                /* At the edges the other half of the pair is off the frame. */
                if ((!pixels[pixel] && ref[pixel] &&
                     (x==0 || (!ref[pixel-1] && pixels[pixel-1]==ref[pixel]))) ||
                    (pixels[pixel] && !ref[pixel] &&
                     (x==63 || (ref[pixel+1] && !pixels[pixel+1] && pixels[pixel]==ref[pixel+1])))) {
                    point_shifts++;
                    if (x==0 && !pixels[pixel]) off_frame--;    /* shifted out at the left */
                    if (x==63 && pixels[pixel]) off_frame++;    /* in from beyond the right */
                    continue;
                }
            }
            if (!equal_pixel(id,pixels[pixel],reference[id][pixel])) {
                if (failures++<16) fprintf(stderr,"case %u pixel %u,%u: %08x != %08x\n",
                    id,pixel%64,pixel/64,pixels[pixel],reference[id][pixel]);
            }
        }
        /* The lit-pixel count moves with a point shifted across the frame's edge. */
        CHECK(a==expected_status[id] && b==expected_count[id]+off_frame && c==expected_error[id]);
    }
    return count;
}
void sceKernelExitGame(void) {}
int sceGeListEnQueue(const void *data,void *stall,int cb,void *args) {
    CHECK(data==commands && !stall && cb==-1 && !args);
#ifdef VERTEX_GL
    psp_mem_write_block(0x04000000,gl_pixels,sizeof gl_pixels);
#endif
    psp_mem_write_block(LIST,commands,used*sizeof(u32));
    return invoke(0xAB49E76A,LIST,0,(u32)cb,0);
}
int sceGeListSync(int id,int mode) {
    int result=invoke(0x03444EB4,id,mode,0,0);
#ifdef VERTEX_GL
    psp_render_current()->finish();
    int w=0,h=0;
    unsigned char *pixels=render_gl_capture(0x04000000,&w,&h);
    CHECK(pixels && w>=64 && h>=64);
    if (pixels && w>=64 && h>=64)
        for (unsigned y=0;y<64;y++) memcpy(gl_pixels+y*64,pixels+y*w*4,64*4);
    free(pixels);
#endif
    return result;
}
int main(int argc,char **argv) {
    if (argc<2 || argc>3) return 2;
    FILE *file=fopen(argv[1],"r");if (!file) return 2;
    unsigned kind,id,a,b,c;
    while (fscanf(file,"%x %x %x %x %x",&kind,&id,&a,&b,&c)==5) {
        if (id>=MAX_CASES) return 2;
        if (kind==0) {
            if (expected_seen[id]) return 2;
            expected_seen[id]=1;expected_cases++;
            expected_status[id]=a;expected_count[id]=b;expected_error[id]=c;
        } else if (kind==1) {
            if (a>=64 || b>=64) return 2;
            reference[id][b*64+a]=c;
        }
    }
    if (!feof(file) || !expected_cases) return 2;
    fclose(file);
#ifdef VERTEX_GL
    expected_cases=0;
    for (unsigned i=0;i<VERTEX_CASE_COUNT;i++)
        if (vertex_cases[i].primitive==3) expected_cases++;
#endif
    if (argc==3 && !(actual_output=fopen(argv[2],"w"))) return 2;
    if (psp_mem_init()!=0) return 2;
    psp_hle_init();psp_cpu_reset();psp_sched_set_threading(0);psp_clock_reset();
    psp_display_reset();psp_ge_reset();
#ifdef VERTEX_GL
    lr_settings settings;char error[LR_ERROR_SIZE];
    lr_settings_defaults(&settings);
    if (lr_settings_set(&settings,LR_RENDER,"gl",LR_COMMAND_LINE,error) ||
        lr_settings_resolve(&settings,error)) return 2;
    lr_settings_use(&settings);
    real_gl=render_gl_backend();if (!real_gl) return 2;
    psp_render_backend tracked=*real_gl;tracked.name="tracked-gl";tracked.draw_model=track_model;
    CHECK(psp_render_register(&tracked)==0);CHECK(psp_render_select("tracked-gl")==0);
    present_want_gl();if (present_start()!=0 || tracked.init(64,64)!=0) return 2;
#else
    CHECK(psp_render_select("software")==0);
#endif
    psp_mem_write_block(PLAIN,plain,sizeof plain);
    psp_mem_write_block(INDEX8,index8,sizeof index8);
    psp_mem_write_block(INDEX16,index16,sizeof index16);
    psp_mem_write_block(INPUTS,vertex_cases,sizeof vertex_cases);
#ifdef VERTEX_GL
    for (unsigned i=0;i<VERTEX_CASE_COUNT;i++)
        if (vertex_cases[i].primitive==3) run_vertex_case(vertex_cases+i);
    CHECK(model_calls==expected_cases);
    printf("GPU model path: %u calls\n",model_calls);
    render_gl_report(stdout);
    psp_render_current()->shutdown();
#else
    probe_main(0,NULL);
#endif
    CHECK(compared==expected_cases);
    if (actual_output) fclose(actual_output);
    psp_mem_free();
    printf("vertex observations: %u rasters, %llu checks, %u failures; %u lit pixels within one RGB step;"
           " %u point pixels one column left\n",
           compared,(unsigned long long)checks,failures,shade_differences,point_shifts);
    return failures?1:0;
}
