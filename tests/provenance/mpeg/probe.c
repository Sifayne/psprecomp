/* Original MPEG bookkeeping stimuli. BSD PSPSDK pspmpeg.h declarations. */
typedef unsigned int u32;
extern int sceIoWrite(int,const void*,unsigned);
extern void sceKernelExitGame(void);
extern int sceUtilityLoadAvModule(int);
extern int sceKernelSetCompiledSdkVersion(int);
extern int sceMpegInit(void);
extern void sceMpegFinish(void);
extern int sceMpegQueryMemSize(int);
extern int sceMpegRingbufferQueryMemSize(int);
extern int probeConstruct(void*,int,void*,int,int(*)(void*,int,void*),void*);
extern void sceMpegRingbufferDestruct(void*);
extern int sceMpegRingbufferAvailableSize(void*);
extern int probeCreate(u32*,void*,int,void*,int,int,int);
extern void sceMpegDelete(u32*);
extern int sceMpegQueryAtracEsSize(u32*,int*,int*);
extern int sceMpegInitAu(u32*,void*,void*);
extern void *sceMpegMallocAvcEsBuf(u32*);
extern void sceMpegFreeAvcEsBuf(u32*,void*);
extern int sceMpegQueryStreamSize(void*,int*);
extern int sceMpegQueryStreamOffset(u32*,void*,int*);
extern void *sceMpegRegistStream(u32*,int,int);
extern void sceMpegUnRegistStream(void*,void*);   /* SDK: (SceMpeg, SceMpegStream*) */
extern int sceMpegGetAvcAu(u32*,void*,void*,void*);
extern int sceMpegRingbufferPut(void*,int,int);
static u32 ring[16], au[8], handle;
static unsigned char context[131072] __attribute__((aligned(64)));
static unsigned char data[131072] __attribute__((aligned(64)));
#ifdef PROBE_NATIVE
extern u32 guest_address(const void*);
extern void set_gp(u32);
#else
static u32 guest_address(const void *p) { return (u32)p; }
static void set_gp(u32 v) { __asm__ volatile("move $gp,%0"::"r"(v)); }
#endif
static u32 supply,callback_data,callback_count,callback_arg;
static int callback(void *p,int n,void *arg) {
    callback_data=guest_address(p);callback_count=n;callback_arg=(u32)(unsigned long)arg;
    if (!supply) return 0;
    /* Original empty MPEG program-stream end packets, padded with zeroes. */
    unsigned char *bytes=p;
    for (int i=0;i<n*2048;i++) bytes[i]=0;
    for (int i=0;i<n;i++) { bytes[i*2048+2]=1;bytes[i*2048+3]=0xb9; }
    return n;
}
static u32 normalized(u32 v) {
    if (v==guest_address(callback)) return 0xf0000000;
    if (v==guest_address(&handle)) return 0xf1000000;
    const void *buffers[]={context,data,ring};
    const unsigned sizes[]={sizeof context,sizeof data,sizeof ring};
    for (unsigned i=0;i<3;i++) {
        u32 start=guest_address(buffers[i]);
        /* Exclusive end: adjacent buffers must not claim each other's start. */
        if (v>=start && v<start+sizes[i]) return (0xa0000000+(i<<24))+(v-start);
    }
    return v;
}
/* A pointer inside our own storage, normalized; 0 for null; otherwise a
 * marker, since addresses outside our storage are not comparable across
 * hosts. Differences between such pointers are recorded separately. */
static u32 where(const void *p) {
    u32 v=(u32)(unsigned long)p;
    if (!v) return 0;
    u32 n=normalized(v);
    return (n>>24)>=0xa0 && (n>>24)<=0xa2 ? n : 0xffffffff;
}
static void line(u32 kind,u32 id,const u32 *words,unsigned count) {
    char out[256];const char *hex="0123456789abcdef";unsigned n=0;
    for (unsigned i=0;i<count+2;i++) {
        u32 value=i==0?kind:i==1?id:normalized(words[i-2]);
        for (int s=28;s>=0;s-=4) out[n++]=hex[(value>>s)&15];
        out[n++]=' ';
    }
    out[n++]='\n';sceIoWrite(1,out,n);
}
static void fill(u32 *p,unsigned n,u32 value) { for (unsigned i=0;i<n;i++) p[i]=value; }
static void store_be(unsigned char *p,u32 v) { p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v; }
static void store_le(unsigned char *p,u32 v) { p[3]=v>>24;p[2]=v>>16;p[1]=v>>8;p[0]=v; }
/* A synthetic stream header: a leading tag of four or eight bytes, then
 * distinct words up to +128 whose bytes are asymmetric, so the recorded
 * outputs identify which word each query reads and in which byte order.
 * Nothing else is assumed about it; it holds no game data. */
static void header(const char *tag,u32 base,u32 step,int little) {
    unsigned n=0;
    fill((u32*)data,64,0);
    while (tag[n]) { data[n]=(unsigned char)tag[n];n++; }
    for (unsigned w=n/4;w<32;w++) (little?store_le:store_be)(data+4*w,base+w*step);
}
static void queries(u32 id) {
    int output=0x13579bdf;
    u32 words[4];
    words[0]=sceMpegQueryStreamSize(data,&output);words[1]=output;
    output=0x13579bdf;
    words[2]=sceMpegQueryStreamOffset(&handle,data,&output);words[3]=output;
    line(19,id,words,4);
}
int module_start(unsigned args,void *argp) {
    (void)args;(void)argp;
    sceUtilityLoadAvModule(0);sceUtilityLoadAvModule(3);
    const u32 versions[]={0,0x01050010,0x02000010,0x05050010,0x06030010};
    for (unsigned i=0;i<5;i++) {
        sceKernelSetCompiledSdkVersion(versions[i]);
        u32 words[]={sceMpegQueryMemSize(0),sceMpegQueryMemSize(1),sceMpegQueryMemSize(-1)};
        line(0,i,words,3);
    }
    {   /* Ring construction and context creation before any initialization. */
        fill(ring,16,0xcdcdcdcd);fill((u32*)context,sizeof context/4,0xabababab);
        u32 early[4];
        early[0]=probeConstruct(ring,3,data,sceMpegRingbufferQueryMemSize(3),callback,(void*)0x13579bdf);
        handle=0xeeeeeeee;
        early[1]=probeCreate(&handle,context,sceMpegQueryMemSize(0),ring,512,0,0);
        early[2]=handle;early[3]=sceMpegRingbufferAvailableSize(ring);
        line(18,0,early,4);
        if (!(early[1]&0x80000000)) sceMpegDelete(&handle);
        if (!(early[0]&0x80000000)) sceMpegRingbufferDestruct(ring);
    }
    u32 init=sceMpegInit();line(1,0,&init,1);
    init=sceMpegInit();line(1,1,&init,1);
    set_gp(0x24681357);
    const int packets[]={0,1,2,3,17,64,-1,0x100000};
    for (unsigned i=0;i<8;i++) {
        u32 value=sceMpegRingbufferQueryMemSize(packets[i]);line(2,i,&value,1);
    }
    for (unsigned i=0;i<3;i++) {
        int size=sceMpegRingbufferQueryMemSize(3)+(i==0?-1:i==1?0:4096);
        fill(ring,16,0xcdcdcdcd);
        u32 result=probeConstruct(ring,3,data,size,callback,(void*)0x13579bdf);
        line(3,i,&result,1);line(4,i,ring,16);
        if (!(result&0x80000000)) sceMpegRingbufferDestruct(ring);
    }
    for (unsigned i=0;i<5;i++) {
        const unsigned offsets[]={0,0,1,15,64};
        unsigned offset=offsets[i];int size=sceMpegQueryMemSize(0)-(i==0);
        fill(ring,16,0xcdcdcdcd);fill((u32*)context,sizeof context/4,0xabababab);
        probeConstruct(ring,3,data,sceMpegRingbufferQueryMemSize(3),callback,(void*)0x13579bdf);
        handle=0xeeeeeeee;
        u32 result=probeCreate(&handle,context+offset,size,ring,512,0,0);
        u32 record[]={result,handle,sceMpegRingbufferAvailableSize(ring)};
        line(5,i,record,3);line(6,i,ring,16);
        if (!(result&0x80000000)) {
            int es=-1,out=-1;
            result=sceMpegQueryAtracEsSize(&handle,&es,&out);
            u32 sizes[]={result,es,out};line(7,i,sizes,3);
            const u32 buffers[]={0,1,2,7,0x088abc00};
            for (unsigned j=0;j<5;j++) {
                fill(au,8,0xdededede);
                result=sceMpegInitAu(&handle,(void*)(unsigned long)buffers[j],au);
                line(8,i*8+j,&result,1);line(9,i*8+j,au,8);
            }
            if (i==1) {
                u32 alias=handle;void *ids[5];
                for (unsigned j=0;j<5;j++) {
                    ids[j]=sceMpegMallocAvcEsBuf(&alias);
                    u32 id=(u32)(unsigned long)ids[j];line(10,j,&id,1);
                }
                for (unsigned j=0;j<5;j++) {
                    fill(au,8,0xdededede);
                    result=sceMpegInitAu(&handle,(void*)(unsigned long)buffers[j],au);
                    line(13,j,&result,1);line(14,j,au,8);
                }
                fill((u32*)data,64,0);
                int output=0x13579bdf;
                u32 query=sceMpegQueryStreamSize(data,&output);
                u32 bad_header[]={query,output};line(15,0,bad_header,2);
                query=sceMpegQueryStreamOffset(&handle,data,&output);
                bad_header[0]=query;bad_header[1]=output;line(15,1,bad_header,2);
                /* Header fields: distinct big-endian words, every word one
                 * byte off a 2048 multiple, all zero, a wrong magic, and the
                 * same words stored little-endian. */
                header("PSMF",0x00120000,0x800,0);queries(0);
                header("PSMF",0x00120001,0x800,0);queries(1);
                header("PSMF",0,0,0);queries(2);
                header("PSMX",0x00120000,0x800,0);queries(3);
                header("PSMF",0x00120000,0x800,1);queries(4);
                /* The third word alone, over the same distinct pattern: one
                 * and two sectors, one sector with a smaller fourth word, one
                 * sector over zeros, a large aligned value, and one byte past
                 * a sector. */
                header("PSMF",0x00120000,0x800,0);store_be(data+8,0x800);queries(5);
                header("PSMF",0x00120000,0x800,0);store_be(data+8,0x1000);queries(6);
                header("PSMF",0x00120000,0x800,0);store_be(data+8,0x800);store_be(data+12,0x400);queries(7);
                header("PSMF",0,0,0);store_be(data+8,0x800);queries(8);
                header("PSMF",0x00120000,0x800,0);store_be(data+8,0x7ffff800);queries(9);
                header("PSMF",0x00120000,0x800,0);store_be(data+8,0x801);queries(10);
                /* This game's header opens with the eight bytes "PSMF0015"
                 * (reports/146-header-capture.log). The same third-word
                 * variations with that tag, over zeros and over the pattern,
                 * then neighbouring tags. */
                header("PSMF0015",0,0,0);store_be(data+8,0x800);store_be(data+12,0x00121800);queries(11);
                header("PSMF0015",0x00120000,0x800,0);store_be(data+8,0x800);queries(12);
                header("PSMF0015",0x00120000,0x800,0);queries(13);
                header("PSMF0015",0x00120000,0x800,0);store_be(data+8,0x801);queries(14);
                header("PSMF0015",0x00120000,0x800,0);store_be(data+8,0);queries(15);
                header("PSMF0015",0x00120000,0x800,0);store_be(data+8,0x800);store_be(data+12,0x400);queries(16);
                header("PSMF0015",0x00120000,0x800,0);store_be(data+8,0x7ffff800);queries(17);
                header("PSMF0014",0x00120000,0x800,0);store_be(data+8,0x800);queries(18);
                header("PSMF0016",0x00120000,0x800,0);store_be(data+8,0x800);queries(19);
                /* The tag alone, swept, with an accepted offset. */
                static const char *const tags[]={
                    "PSMF0000","PSMF0001","PSMF0009","PSMF0010","PSMF0011","PSMF0012",
                    "PSMF0013","PSMF0014","PSMF0015","PSMF0016","PSMF0017","PSMF0019",
                    "PSMF0020","PSMF0099","PSMF0100","PSMF1015","PSMF001A","PSMF0 15"};
                for (unsigned t=0;t<sizeof tags/sizeof *tags;t++) {
                    header(tags[t],0x00120000,0x800,0);store_be(data+8,0x800);
                    int output=0x13579bdf;
                    u32 words[2];
                    words[0]=sceMpegQueryStreamOffset(&handle,data,&output);words[1]=output;
                    line(21,t,words,2);
                }
                void *stream=sceMpegRegistStream(&handle,0,0);
                void *audio=sceMpegRegistStream(&handle,1,0);
                void *second=sceMpegRegistStream(&handle,0,0);
                u32 streams[]={where(stream),where(audio),where(second),
                               (u32)(unsigned long)audio-(u32)(unsigned long)stream,
                               (u32)(unsigned long)second-(u32)(unsigned long)stream};
                line(20,0,streams,5);
                sceMpegInitAu(&handle,ids[0],au);
                query=sceMpegGetAvcAu(&handle,stream,au,0);line(15,2,&query,1);
                for (unsigned j=0;j<3;j++) {
                    supply=j!=0;callback_data=callback_count=callback_arg=0;
                    query=sceMpegRingbufferPut(ring,1,sceMpegRingbufferAvailableSize(ring));
                    u32 packet[]={query,callback_data,callback_count,callback_arg,
                                  sceMpegRingbufferAvailableSize(ring)};
                    line(16,j,packet,5);line(17,j,ring,16);
                }
                sceMpegFreeAvcEsBuf(&handle,ids[0]);
                u32 again=(u32)(unsigned long)sceMpegMallocAvcEsBuf(&handle);line(10,5,&again,1);
                /* Unregistration with the handle passed as the SDK declares
                 * it (by value), then as an address; each followed by a new
                 * registration of the same stream type. */
                sceMpegUnRegistStream((void*)(unsigned long)handle,stream);
                void *reuse=sceMpegRegistStream(&handle,0,0);
                u32 after[]={where(reuse),(u32)(unsigned long)reuse-(u32)(unsigned long)stream};
                line(20,1,after,2);
                sceMpegUnRegistStream((void*)(unsigned long)guest_address(&handle),audio);
                reuse=sceMpegRegistStream(&handle,1,0);
                after[0]=where(reuse);after[1]=(u32)(unsigned long)reuse-(u32)(unsigned long)stream;
                line(20,2,after,2);
            }
            sceMpegDelete(&handle);
        }
        sceMpegRingbufferDestruct(ring);line(11,i,ring,16);
    }
    sceMpegFinish();line(12,0,0,0);sceKernelExitGame();return 0;
}
