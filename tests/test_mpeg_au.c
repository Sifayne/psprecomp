/* Synthetic MPEG packets through the public HLE API. No game/movie data.
 * SceMpegAu's timestamp words are MSB first (PSPSDK pspmpeg.h), not a native
 * little-endian uint64_t. The movie player reads the low word at +4/+12. */
#include "psprecomp/hle.h"
#include "psprecomp/dispatch.h"
#include <stdio.h>
#include <string.h>

#define BASE 0x08810000u
#define MPEG BASE
#define RB (BASE + 0x100)
#define DATA (BASE + 0x1000)
#define WORK (BASE + 0x10000)
#define AU (BASE + 0x200)
#define ES (BASE + 0x3000)
#define CALLBACK 0x08900000u
static int failed;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); failed++; } } while (0)
static uint32_t call(uint32_t nid, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f) {
    uint32_t args[] = {a,b,c,d,e,f};
    for (int i = 0; i < 6; i++) psp_cpu.r[PSP_REG_A0+i] = args[i];
    psp_hle_call(nid); return psp_cpu.r[PSP_REG_V0];
}
/* Three 16x16 black frames, generated with ffmpeg color and libx264 baseline,
 * bframes=0:aud=1, then removing the optional SEI encoder-identification NAL. */
static const unsigned char video[] = {
  0,0,0,1,9,0x10,0,0,0,1,0x67,0x42,0xc0,0x0a,0xd9,0x1e,0xc0,0x44,
  0,0,3,0,4,0,0,3,0,0xf0,0x3c,0x48,0x99,0x20,0,0,0,1,0x68,0xcb,
  0x83,0xcb,0x20,0,0,0,1,0x65,0x88,0x84,0x0b,0xf2,0x62,0x80,0,
  0xab,0xce,0,0,0,1,9,0x30,0,0,0,1,0x41,0x9a,0x38,0x15,0xea,
  0,0,0,1,9,0x30,0,0,0,1,0x41,0x9a,0x54,4,0xfa,0x80
};
static unsigned char packet[2048];
static unsigned pes(unsigned at, unsigned id, uint32_t pts, const void *payload, unsigned size) {
    unsigned char *p = packet + at;
    unsigned len = size + 8;
    p[0]=0; p[1]=0; p[2]=1; p[3]=id; p[4]=len>>8; p[5]=len;
    p[6]=0x80; p[7]=0x80; p[8]=5;
    p[9]=0x21|((pts>>29)&0x0e); p[10]=pts>>22;
    p[11]=((pts>>14)&0xfe)|1; p[12]=pts>>7; p[13]=(pts<<1)|1;
    memcpy(p+14,payload,size);
    return at+14+size;
}
static void feed(void) {
    psp_mem_write_block(psp_arg(0), packet, sizeof packet);
    psp_ret(1);
}
int main(void) {
    CHECK(psp_mem_init()==0); psp_cpu_reset(); psp_hle_init();
    psp_cpu.r[PSP_REG_SP]=BASE+0x8000;
    if (!psp_mpeg_decoding_available()) { puts("mpeg_au: skipped (no OpenH264)"); return 77; }
    CHECK(psp_mpeg_set_decoding(1)==0);
    psp_register(CALLBACK,feed);
    CHECK(call(0x682A619B,0,0,0,0,0,0)==0); // MpegInit
    uint32_t ring_size=call(0xD7A29F46,2,0,0,0,0,0);
    CHECK(call(0x37295ED8,RB,2,DATA,ring_size,CALLBACK,0)==0);
    CHECK(call(0xD8C5F121,MPEG,WORK,0x10000,RB,512,0)==0);
    // Private-stream prefix then two framed ATRAC payloads, fetched not decoded.
    const unsigned char audio[36]={0x80,0,0,0, 0x0f,0xd0,0x28,0,0,0,0,0,0,0,0,0,0,0,0,0,
                                  0x0f,0xd0,0x28,0,0,0,0,0,0,0,0,0,0,0,0,0};
    unsigned end=pes(0,0xe0,90000,video,sizeof video);
    end=pes(end,0xbd,85069,audio,sizeof audio); (void)end;
    CHECK(call(0xB240A59E,RB,1,2,0,0,0)==1); // RingbufferPut
    CHECK(call(0x167AFD9E,MPEG,ES,AU,0,0,0)==0);
    /* InitAu initialization is checked by the independent MPEG probe. */
    CHECK(call(0xFE246728,MPEG,0,AU,0,0,0)==0); // GetAvcAu
    CHECK(psp_read32(AU)==0 && psp_read32(AU+4)==90000);
    CHECK(psp_read32(AU+8)==0 && psp_read32(AU+12)==90000);
    CHECK(call(0x167AFD9E,MPEG,ES,AU,0,0,0)==0);
    for (unsigned i=0;i<2;i++) {
        CHECK(call(0xE1CE83A7,MPEG,0,AU,0,0,0)==0);
        CHECK(psp_read32(AU)==0 && psp_read32(AU+4)==85069+i*4180);
        /* An audio AU has no DTS on a 6.60 PSP (mpegprobe). */
        CHECK(psp_read32(AU+8)==0xFFFFFFFFu && psp_read32(AU+12)==0xFFFFFFFFu);
        CHECK(psp_read32(AU+20)==16);
        CHECK(psp_read8(ES)==0x0f && psp_read8(ES+1)==0xd0);
    }
    /* A movie with only its video registered: nothing has read the packets
     * yet, so the ring holds them. The audio, which no one reads, is no
     * consumer -- counted as one, it was past every put and the ring read
     * empty for ever (WipEout Pulse's title screen). */
    {
        enum { MPEG2 = BASE + 0x80, RB2 = BASE + 0x180, DATA2 = BASE + 0x20000, WORK2 = BASE + 0x30000 };
        const uint32_t ring2 = call(0xD7A29F46,4,0,0,0,0,0);
        CHECK(call(0x37295ED8,RB2,4,DATA2,ring2,CALLBACK,0)==0);
        CHECK(call(0xD8C5F121,MPEG2,WORK2,0x10000,RB2,512,0)==0);
        CHECK(call(0x42560F23,MPEG2,0,0,0,0,0)!=0);   /* RegistStream: video */
        memset(packet,0,sizeof packet);
        pes(0,0xe0,90000,video,sizeof video);
        CHECK(call(0xB240A59E,RB2,1,4,0,0,0)==1);
        CHECK(call(0xB240A59E,RB2,1,3,0,0,0)==1);
        CHECK(call(0xB5F6DC87,RB2,0,0,0,0,0)==2);     /* AvailableSize */
    }
    call(0x874624D6,0,0,0,0,0,0);
    psp_mem_free();
    puts(failed ? "mpeg_au: FAILED" : "mpeg_au: passed"); return failed!=0;
}
