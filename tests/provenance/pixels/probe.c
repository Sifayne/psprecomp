/* Project-authored pixel experiment: texture registers and sampling, palette
 * indexing, block transfers, colour masks, clear mode and the depth test.
 * Command numbers, field packing and vertex declarations come from BSD PSPSDK
 * guInternal.h / pspgu.h and the sceGu* producers, not an emulator. The probe
 * contains no game data: every texel, palette entry and transfer word is a
 * function of its own coordinates, so a recorded pixel names what was read. */
typedef unsigned int u32;
extern int sceIoWrite(int,const void*,unsigned);
extern void sceKernelExitGame(void);
extern int sceGeListEnQueue(const void*,void*,int,void*);
extern int sceGeListSync(int,int);
enum { B_TEXTURE, B_PALETTE, B_XSRC, B_XDST, B_VERTS, B_COUNT };
#define FB_WORDS (64*64)
static u32 texture_store[8192] __attribute__((aligned(64)));
static u32 palette_store[256] __attribute__((aligned(64)));
static u32 xsrc_store[4096] __attribute__((aligned(64)));
static u32 xdst_store[4096] __attribute__((aligned(64)));
static u32 verts_store[512] __attribute__((aligned(64)));
static u32 commands[2048] __attribute__((aligned(16)));
#ifdef PROBE_NATIVE
extern volatile u32 *buffer(int which);
extern u32 buffer_address(int which);
extern volatile u32 *framebuffer(void);
#else
static u32 *store(int which) {
    switch (which) {
    case B_TEXTURE: return texture_store;
    case B_PALETTE: return palette_store;
    case B_XSRC: return xsrc_store;
    case B_XDST: return xdst_store;
    default: return verts_store;
    }
}
/* CPU accesses go through the uncached alias; the GE is given the cached
 * address, as the SDK producers pass it. */
static volatile u32 *buffer(int which) { return (volatile u32*)(0x40000000u|(u32)store(which)); }
static u32 buffer_address(int which) { return (u32)store(which); }
static volatile u32 *framebuffer(void) { return (volatile u32*)0x44000000; }
#endif
#define FB_ADDRESS 0x04000000u
#define ZB_ADDRESS 0x04088000u
static unsigned used, vcursor;
static void command(unsigned reg,u32 value) { commands[used++]=(reg<<24)|(value&0xffffff); }
static void address(unsigned reg,u32 value) { command(0x10,(value>>8)&0xf0000);command(reg,value&0xffffff); }
static void output(u32 kind,u32 id,u32 a,u32 b,u32 c) {
    u32 words[]={kind,id,a,b,c};char text[48];unsigned length=0;
    const char *digits="0123456789abcdef";
    for (unsigned i=0;i<5;i++) {
        for (int bit=28;bit>=0;bit-=4) text[length++]=digits[(words[i]>>bit)&15];
        text[length++]=' ';
    }
    text[length++]='\n';sceIoWrite(1,text,length);
}
static u32 hash(volatile const u32 *words,unsigned count) {
    u32 h=2166136261u;
    for (unsigned i=0;i<count;i++) { h^=words[i];h*=16777619u; }
    return h;
}
static void fill(volatile u32 *p,unsigned count,u32 value) { for (unsigned i=0;i<count;i++) p[i]=value; }
/* Every case starts from the same explicit state: optional stages off, an
 * 8888 target of 64 pixels at the start of VRAM, a depth buffer behind it,
 * colour masks clear, depth writes off, texturing off, replace mode. */
static void begin(void) {
    used=0;vcursor=0;
    for (unsigned reg=0x17;reg<=0x28;reg++) command(reg,0);
    command(0xd3,0);command(0xe8,0);command(0xe9,0);command(0xe7,1);command(0xde,7);
    command(0xd2,3);command(0x9c,FB_ADDRESS&0xffffff);command(0x9d,((FB_ADDRESS>>8)&0xff0000)|64);
    command(0x9e,ZB_ADDRESS&0xffffff);command(0x9f,((ZB_ADDRESS>>8)&0xff0000)|64);
    command(0x15,0);command(0x16,63|(63<<10));
    command(0xd4,0);command(0xd5,63|(63<<10));
    command(0x13,0);command(0x4c,0);command(0x4d,0);
    command(0x55,0xffffff);command(0x58,255);command(0x53,0);command(0x51,0);
    command(0xc0,0);command(0x48,0x3f8000);command(0x49,0x3f8000);command(0x4a,0);command(0x4b,0);
    command(0xc6,0);command(0xc7,0);command(0xc9,3|(1<<8));command(0xca,0);
    command(0xc2,0);command(0xc8,0);command(0xd0,0);
}
static void texture(u32 addr,u32 bufwidth,unsigned wexp,unsigned hexp,unsigned format,unsigned mode) {
    command(0x1e,1);command(0xc3,format);command(0xc2,mode);
    command(0xa0,addr&0xffffff);command(0xa8,bufwidth);command(0xb8,wexp|(hexp<<8));command(0xcb,0);
}
static void palette(unsigned blocks,u32 mode) {
    u32 p=buffer_address(B_PALETTE);
    command(0xb0,p&0xffffff);command(0xb1,(p>>8)&0xf0000);command(0xc4,blocks);command(0xc5,mode);
}
/* GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, a
 * sprite from two vertices; through-mode coordinates are texels. */
static void tex_sprite(int x0,int y0,int x1,int y1,int u0,int v0,int u1,int v1,u32 colour,unsigned z) {
    volatile u32 *v=buffer(B_VERTS)+vcursor;
    v[0]=(u0&0xffff)|((u32)(v0&0xffff)<<16);v[1]=colour;v[2]=(x0&0xffff)|((u32)(y0&0xffff)<<16);v[3]=z&0xffff;
    v[4]=(u1&0xffff)|((u32)(v1&0xffff)<<16);v[5]=colour;v[6]=(x1&0xffff)|((u32)(y1&0xffff)<<16);v[7]=z&0xffff;
    command(0x12,2|(7<<2)|(2<<7)|(1<<23));
    address(0x01,buffer_address(B_VERTS)+vcursor*4);
    command(0x04,(6<<16)|2);
    vcursor+=8;
}
/* The SDK's own clear vertex: GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D. */
static void flat_sprite(int x0,int y0,int x1,int y1,u32 colour,unsigned z) {
    volatile u32 *v=buffer(B_VERTS)+vcursor;
    v[0]=colour;v[1]=(x0&0xffff)|((u32)(y0&0xffff)<<16);v[2]=z&0xffff;
    v[3]=colour;v[4]=(x1&0xffff)|((u32)(y1&0xffff)<<16);v[5]=z&0xffff;
    command(0x12,(7<<2)|(2<<7)|(1<<23));
    address(0x01,buffer_address(B_VERTS)+vcursor*4);
    command(0x04,(6<<16)|2);
    vcursor+=8;
}
static void run_list(unsigned id) {
    command(0x0f,0);command(0x0c,0);
    int queue=sceGeListEnQueue(commands,(void*)0,-1,(void*)0);
    int status=sceGeListSync(queue,0);
    volatile u32 *pixels=framebuffer();
    unsigned count=0;
    for (unsigned i=0;i<FB_WORDS;i++) if (pixels[i]) count++;
    output(0,id,(u32)status,count,hash(pixels,FB_WORDS));
}
static void window(unsigned id,unsigned x0,unsigned y0,unsigned w,unsigned h) {
    volatile u32 *pixels=framebuffer();
    for (unsigned y=y0;y<y0+h;y++) for (unsigned x=x0;x<x0+w;x++) output(1,id,y*64+x,pixels[y*64+x],0);
}
static void finish(unsigned id,unsigned w,unsigned h) { run_list(id);window(id,0,0,w,h); }
static u32 T,P,S,D;
static u32 high(u32 addr) { return (addr>>8)&0x0f0000; }
/* ---- texture registers and sampling ---------------------------------- */
static void tex_default(void) { texture(T,high(T)|32,5,5,3,0); }
static void tex_draw(unsigned id,unsigned w,unsigned h) { tex_sprite(0,0,16,16,0,0,16,16,0xffffffff,0);finish(id,w,h); }
/* Texture cases start from a zeroed target. */
static void begin(void);
static void tbegin(void) { fill(framebuffer(),FB_WORDS,0);begin(); }
#define begin tbegin
static void texture_cases(void) {
    /* 100: the SDK encoding. 101-103: other bytes above the address nibble. */
    begin();tex_default();tex_draw(100,8,8);
    begin();texture(T,(0x18u<<16)|32,5,5,3,0);tex_draw(101,8,8);
    begin();texture(T,(0x88u<<16)|32,5,5,3,0);tex_draw(102,8,8);
    begin();texture(T,(0x48u<<16)|32,5,5,3,0);tex_draw(103,8,8);
    /* 104-105: address low bits. */
    begin();texture(T+8,high(T)|32,5,5,3,0);tex_draw(104,8,8);
    begin();texture(T+16,high(T)|32,5,5,3,0);tex_draw(105,8,8);
    /* 106-108: stride field: a bit above ten, and two odd strides. */
    begin();texture(T,high(T)|32|0x800,5,5,3,0);tex_draw(106,8,8);
    begin();texture(T,high(T)|48,5,5,3,0);tex_draw(107,8,8);
    begin();texture(T,high(T)|40,5,5,3,0);tex_draw(108,8,8);
    /* 109: a bit above the four-bit size exponent. */
    begin();texture(T,high(T)|32,5|0x10,5,3,0);tex_draw(109,8,8);
    /* 110-111: a declared 1024-texel row sampled past 512, repeating and clamping. */
    begin();texture(T,high(T)|1024,10,0,3,0);tex_sprite(0,0,16,1,600,0,616,1,0xffffffff,0);finish(110,16,1);
    begin();texture(T,high(T)|1024,10,0,3,0);command(0xc7,1|(1<<8));tex_sprite(0,0,16,1,600,0,616,1,0xffffffff,0);finish(111,16,1);
    /* 112-114: TEX_MODE bits 0, 1 and 8. */
    begin();texture(T,high(T)|32,5,5,3,1);tex_draw(112,16,16);
    begin();texture(T,high(T)|32,5,5,3,2);tex_draw(113,8,8);
    begin();texture(T,high(T)|32,5,5,3,0x100);tex_draw(114,8,8);
    /* 115-117: 16-bit texel formats over the same bytes. */
    begin();texture(T,high(T)|32,5,5,0,0);tex_draw(115,8,8);
    begin();texture(T,high(T)|32,5,5,1,0);tex_draw(116,8,8);
    begin();texture(T,high(T)|32,5,5,2,0);tex_draw(117,8,8);
    /* 118-125: indexed formats; palette mode shift, mask and start. */
    begin();palette(32,3|(0<<2)|(0xff<<8)|(0<<16));texture(T,high(T)|32,5,5,5,0);tex_draw(118,8,8);
    begin();palette(32,3|(0<<2)|(0x0f<<8)|(0<<16));texture(T,high(T)|32,5,5,5,0);tex_draw(119,8,8);
    begin();palette(32,3|(4<<2)|(0xff<<8)|(0<<16));texture(T,high(T)|32,5,5,5,0);tex_draw(120,8,8);
    begin();palette(32,3|(0<<2)|(0xff<<8)|(1<<16));texture(T,high(T)|32,5,5,5,0);tex_draw(121,8,8);
    begin();palette(32,3|(0<<2)|(0xff<<8)|(0<<16));texture(T,high(T)|32,5,5,4,0);tex_draw(122,8,8);
    begin();palette(32,3|(0<<2)|(0xff<<8)|(0<<16));texture(T,high(T)|32,5,5,6,0);tex_draw(123,8,8);
    begin();palette(32,3|(0<<2)|(0xff<<8)|(0<<16));texture(T,high(T)|32,5,5,7,0);tex_draw(124,8,8);
    begin();palette(32,3|(8<<2)|(0xff<<8)|(0<<16));texture(T,high(T)|32,5,5,7,0);tex_draw(125,8,8);
    /* 126-128: 16-bit palette formats. */
    begin();palette(32,0|(0<<2)|(0xff<<8)|(0<<16));texture(T,high(T)|32,5,5,5,0);tex_draw(126,8,8);
    begin();palette(32,1|(0<<2)|(0xff<<8)|(0<<16));texture(T,high(T)|32,5,5,5,0);tex_draw(127,8,8);
    begin();palette(32,2|(0<<2)|(0xff<<8)|(0<<16));texture(T,high(T)|32,5,5,5,0);tex_draw(128,8,8);
    /* 129-134: texture functions with a vertex colour and an environment colour. */
    begin();tex_default();command(0xc9,0|(1<<8));tex_sprite(0,0,16,16,0,0,16,16,0x80c08040,0);finish(129,8,8);
    begin();tex_default();command(0xc9,1|(1<<8));tex_sprite(0,0,16,16,0,0,16,16,0x80c08040,0);finish(130,8,8);
    begin();tex_default();command(0xc9,2|(1<<8));command(0xca,0x00ff40);tex_sprite(0,0,16,16,0,0,16,16,0x80c08040,0);finish(131,8,8);
    begin();tex_default();command(0xc9,4|(1<<8));tex_sprite(0,0,16,16,0,0,16,16,0x80c08040,0);finish(132,8,8);
    begin();tex_default();command(0xc9,3|(0<<8));tex_sprite(0,0,16,16,0,0,16,16,0x80c08040,0);finish(133,8,8);
    begin();tex_default();command(0xc9,0|(1<<8)|(1<<16));tex_sprite(0,0,16,16,0,0,16,16,0x80c08040,0);finish(134,8,8);
    /* 135-136: clamping and repeating past the width. */
    begin();tex_default();command(0xc7,1|(1<<8));tex_sprite(0,0,16,4,24,0,40,4,0xffffffff,0);finish(135,16,2);
    begin();tex_default();command(0xc7,0);tex_sprite(0,0,16,4,24,0,40,4,0xffffffff,0);finish(136,16,2);
    /* 137-138: magnification by two, linear then nearest. */
    begin();tex_default();command(0xc6,1|(1<<8));tex_sprite(0,0,16,16,0,0,8,8,0xffffffff,0);finish(137,8,8);
    begin();tex_default();command(0xc6,0);tex_sprite(0,0,16,16,0,0,8,8,0xffffffff,0);finish(138,8,8);
    /* 139-140: the SDK's colour mask register, then the register the runtime
     * had labelled as the mask, over a known target. */
    begin();fill(framebuffer(),FB_WORDS,0x99c0c0c0);command(0xe8,0xff00ff);flat_sprite(0,0,16,16,0xffffffff,0);finish(139,4,2);
    begin();fill(framebuffer(),FB_WORDS,0x99c0c0c0);command(0xd8,0xff00ff);flat_sprite(0,0,16,16,0xffffffff,0);finish(140,4,2);
    /* 141-142: modulation of full-scale palette texels by a full and a half
     * vertex colour separates a shift from a division by 255. 143: the mask
     * register with a single bit per channel. */
    begin();palette(32,3|(0xff<<8));texture(T,high(T)|32,5,5,5,0);command(0xc9,0|(1<<8));
    tex_sprite(0,0,16,16,0,0,16,16,0xffffffff,0);finish(141,4,1);
    begin();palette(32,3|(0xff<<8));texture(T,high(T)|32,5,5,5,0);command(0xc9,0|(1<<8));
    tex_sprite(0,0,16,16,0,0,16,16,0x80808080,0);finish(142,4,1);
    begin();fill(framebuffer(),FB_WORDS,0x99c0c0c0);command(0xe8,0x010203);command(0xe9,0x0f);flat_sprite(0,0,16,16,0xffffffff,0);finish(143,4,1);
}
#undef begin
/* ---- block transfers --------------------------------------------------- */
static void transfer(u32 src,u32 srcw,u32 srcpos,u32 dst,u32 dstw,u32 dstpos,u32 size,u32 start) {
    command(0xb2,src&0xffffff);command(0xb3,srcw);command(0xeb,srcpos);
    command(0xb4,dst&0xffffff);command(0xb5,dstw);command(0xec,dstpos);
    command(0xee,size);command(0xea,start);
}
static u32 wide(u32 addr,unsigned stride) { return ((addr&0xff000000u)>>8)|stride; }
static void xfer_case(unsigned id,u32 src,u32 srcw,u32 srcpos,u32 dst,u32 dstw,u32 dstpos,u32 size,u32 start) {
    fill(buffer(B_XDST),4096,0);
    begin();transfer(src,srcw,srcpos,dst,dstw,dstpos,size,start);
    run_list(id);
    volatile u32 *d=buffer(B_XDST);
    for (unsigned y=0;y<8;y++) for (unsigned x=0;x<16;x++) output(2,id,y*64+x,d[y*64+x],0);
    output(3,id,hash(d,4096),hash(buffer(B_XSRC),4096),0);
}
static void transfer_cases(void) {
    const u32 pos44=4|(4<<10), pos00=0, size16=15|(15<<10);
    xfer_case(200,S,wide(S,64),pos44,D,wide(D,64),pos00,size16,1);
    xfer_case(201,S,wide(S,64),pos44,D,wide(D,64),pos00,size16,0);
    xfer_case(202,S,wide(S,64),pos44,D,wide(D,64),pos00,size16,2);
    xfer_case(203,S,wide(S,64),pos44,D,wide(D,64),pos00,size16,3);
    xfer_case(204,S,wide(S,64),pos00,D,wide(D,64),2|(3<<10),size16,1);
    xfer_case(205,S,wide(S,64),pos44,D,wide(D,64|0x800),pos00,size16,1);
    xfer_case(206,S,wide(S,64),pos44,D,wide(D,0x408),pos00,size16,1);
    xfer_case(207,S,wide(S,64),pos44,D,wide(D,68),pos00,size16,1);
    xfer_case(208,S,wide(S,64),pos44,D,wide(D,8),pos00,7|(3<<10),1);
    xfer_case(209,S+4,wide(S,64),pos00,D,wide(D,64),pos00,size16,1);
    xfer_case(210,S+16,wide(S,64),pos00,D,wide(D,64),pos00,size16,1);
    xfer_case(211,S,(0x18u<<16)|64,pos44,D,wide(D,64),pos00,size16,1);
    xfer_case(212,S,(0x88u<<16)|64,pos44,D,wide(D,64),pos00,size16,1);
    xfer_case(213,S,wide(S,64),pos44,D,wide(D,64),pos00,16|(2<<10),1);
    xfer_case(214,S,wide(S,64),1|(0<<10),D,wide(D,64),pos00,size16,0);
    /* 216-217: the uncached alias byte on the source, and a zero source stride. */
    xfer_case(216,S,(0x48u<<16)|64,pos44,D,wide(D,64),pos00,size16,1);
    xfer_case(217,S,wide(S,0x408),pos44,D,wide(D,64),pos00,size16,1);
    /* 218-221: two rows at destination strides around the limit; the second
     * row is read back where the stride would put it. */
    const unsigned strides[]={0x400,0x3f8,0x7f8,0x404};
    for (unsigned i=0;i<4;i++) {
        fill(buffer(B_XDST),4096,0);
        begin();transfer(S,wide(S,64),pos44,D,wide(D,strides[i]),pos00,15|(1<<10),1);
        run_list(218+i);
        volatile u32 *d=buffer(B_XDST);
        for (unsigned x=0;x<16;x++) output(2,218+i,x,d[x],0);
        unsigned at=strides[i]&0x7f8;
        for (unsigned x=0;x<16;x++) output(4,218+i,at+x,d[at+x],0);
        output(3,218+i,hash(d,4096),hash(buffer(B_XSRC),4096),0);
    }
    /* 215: into the framebuffer itself, read back as pixels. */
    fill(framebuffer(),FB_WORDS,0);
    begin();transfer(S,wide(S,64),pos44,FB_ADDRESS,wide(FB_ADDRESS,64),pos00,size16,1);
    finish(215,16,8);
}
/* ---- colour and stencil writes, clear mode, the depth test ----------------- */
static void reference_clear(void) {
    command(0xd3,(5<<8)|1);flat_sprite(0,0,16,16,0x00101010,0x8000);command(0xd3,0);
}
static void depth_test(unsigned func,unsigned write_off) { command(0x23,1);command(0xde,func);command(0xe7,write_off); }
static void clear_case(unsigned id,u32 flags,u32 colour,unsigned z,unsigned func,unsigned write_off,int test_draw) {
    fill(framebuffer(),FB_WORDS,0x44444444);
    begin();reference_clear();
    if (func!=8) depth_test(func,write_off);
    command(0xd3,(flags<<8)|1);flat_sprite(0,0,16,16,colour,z);command(0xd3,0);
    if (test_draw) { depth_test(7,0);flat_sprite(0,0,16,16,0x0000ff00,0x5000); }
    finish(id,8,2);
}
static void clear_cases(void) {
    /* 300-301: a clear's own z is what later draws are tested against. */
    fill(framebuffer(),FB_WORDS,0x44444444);
    begin();reference_clear();depth_test(7,0);flat_sprite(0,0,16,16,0x0000ff00,0x7000);finish(300,8,2);
    fill(framebuffer(),FB_WORDS,0x44444444);
    begin();reference_clear();depth_test(7,0);flat_sprite(0,0,16,16,0x0000ff00,0x9000);finish(301,8,2);
    /* 302-304: which buffers each clear bit writes. */
    clear_case(302,1,0x00202020,0x2000,8,1,1);
    clear_case(303,4,0x00303030,0x2000,8,1,1);
    clear_case(304,3,0x77404040,0x2000,8,1,0);
    /* 305-307: the depth test and depth mask during a clear. */
    clear_case(305,5,0x00505050,0x2000,0,0,1);
    clear_case(306,5,0x00606060,0x2000,1,1,1);
    clear_case(307,1,0x00707070,0x2000,1,0,1);
    /* 320-321: the same depth-only and NEVER-tested clears, colour left alone
     * to show whether the clear touched it. */
    clear_case(320,4,0x00303030,0x2000,8,1,0);
    clear_case(321,5,0x00505050,0x2000,0,0,0);
    /* 308: an ordinary draw's alpha against the framebuffer's. */
    fill(framebuffer(),FB_WORDS,0x44444444);
    begin();flat_sprite(0,0,16,16,0xab0000ff,0);finish(308,4,2);
    /* 309-316: every comparison, at z below, equal to and above the cleared 0x8000. */
    for (unsigned func=0;func<8;func++) {
        fill(framebuffer(),FB_WORDS,0);
        begin();reference_clear();depth_test(func,0);
        flat_sprite(0,0,4,4,0x000000ff,0x7000);flat_sprite(4,0,8,4,0x0000ff00,0x8000);flat_sprite(8,0,12,4,0x00ff0000,0x9000);
        finish(309+func,12,1);
    }
    /* 317: the test disabled with NEVER selected. 318-319: the depth mask. */
    fill(framebuffer(),FB_WORDS,0);
    begin();reference_clear();command(0x23,0);command(0xde,0);command(0xe7,0);flat_sprite(0,0,16,16,0x0000ff00,0x7000);finish(317,4,1);
    fill(framebuffer(),FB_WORDS,0);
    begin();reference_clear();depth_test(7,1);flat_sprite(0,0,16,16,0x000000ff,0x9000);
    depth_test(7,0);flat_sprite(0,0,16,16,0x0000ff00,0x8800);finish(318,4,1);
    fill(framebuffer(),FB_WORDS,0);
    begin();reference_clear();depth_test(7,0);flat_sprite(0,0,16,16,0x000000ff,0x9000);
    flat_sprite(0,0,16,16,0x0000ff00,0x8800);finish(319,4,1);
}
int module_start(unsigned args,void *argp) {
    (void)args;(void)argp;
    T=buffer_address(B_TEXTURE);P=buffer_address(B_PALETTE);S=buffer_address(B_XSRC);D=buffer_address(B_XDST);
    volatile u32 *t=buffer(B_TEXTURE),*p=buffer(B_PALETTE),*s=buffer(B_XSRC);
    /* Texels name their coordinates in a 32-wide layout; palette entries name
     * their index; transfer words name their 64-wide coordinates. */
    for (unsigned i=0;i<8192;i++) t[i]=0xff400000u|(i&31)|(((i>>5)&0xff)<<8);
    for (unsigned i=0;i<256;i++) p[i]=0xff000000u|(i<<16)|((255-i)<<8)|i;
    for (unsigned i=0;i<4096;i++) s[i]=0xaa000000u|(i&63)|((i>>6)<<8);
    fill(framebuffer(),FB_WORDS,0);
    texture_cases();
    transfer_cases();
    clear_cases();
    output(9,0,0,0,0);sceKernelExitGame();return 0;
}
