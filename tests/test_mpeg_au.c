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
/* An MPEG-2 pack header at the start of the packet, its clock at `scr`. */
static unsigned pack(uint32_t scr) {
    unsigned char *p = packet;
    p[0]=0; p[1]=0; p[2]=1; p[3]=0xba;
    p[4]=0x44|((scr>>27)&0x38)|((scr>>28)&3); p[5]=scr>>20;
    p[6]=((scr>>12)&0xf8)|4|((scr>>13)&3); p[7]=scr>>5;
    p[8]=((scr<<3)&0xf8)|4; p[9]=1;
    p[10]=0x01; p[11]=0x89; p[12]=0xc3; p[13]=0xf8;
    return 14;
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
    /* An IDR cut short after the three black pictures: what it lacks is
     * concealed from the picture before it, black, not filled with mid-gray
     * -- the band that flashed over WipEout Pulse's menu where its movie
     * loops. */
    {
        enum { MPEG3 = BASE + 0x480, RB3 = BASE + 0x400, DATA3 = BASE + 0x48000, WORK3 = BASE + 0x50000,
               PIC = BASE + 0x40000, PICP = BASE + 0x2F0, INIT = BASE + 0x2F8 };
        static const unsigned char cut[] = {
            0,0,0,1,9,0x10, 0,0,0,1,0x65,0x88,0x84,          /* the IDR's slice, two bytes of it */
            0,0,0,1,9,0x30, 0,0,0,1,0x41,0x9a,0x38,0x15,0xea,
            0,0,0,1,9,0x30
        };
        unsigned char es[sizeof video + sizeof cut];
        memcpy(es, video, sizeof video); memcpy(es + sizeof video, cut, sizeof cut);
        const uint32_t ring3 = call(0xD7A29F46,4,0,0,0,0,0);
        CHECK(call(0x37295ED8,RB3,4,DATA3,ring3,CALLBACK,0)==0);
        CHECK(call(0xD8C5F121,MPEG3,WORK3,0x10000,RB3,512,0)==0);
        CHECK(call(0x42560F23,MPEG3,0,0,0,0,0)!=0);   /* RegistStream: video */
        memset(packet,0,sizeof packet);
        pes(0,0xe0,90000,es,sizeof es);
        CHECK(call(0xB240A59E,RB3,1,4,0,0,0)==1);
        psp_write32(PICP,PIC);
        uint32_t pixel[4] = {0};
        for (unsigned i=0;i<4;i++) {
            psp_write32(PIC,0x12345678);
            CHECK(call(0xFE246728,MPEG3,0,AU,0,0,0)==0);              /* GetAvcAu */
            CHECK(call(0x0E3C2E9D,MPEG3,AU,16,PICP,INIT,0)==0);      /* AvcDecode */
            CHECK(psp_read32(INIT)==1);
            pixel[i]=psp_read32(PIC);
        }
        CHECK(pixel[2]==0xFF000000u);
        CHECK(pixel[3]==0xFF000000u);
    }
    /* Two packs, the second opening inside a NAL. With their clock rising
     * they are one stream, and the slice the first ends on runs on into the
     * second's bytes and decodes as a fifth picture. With the clock going
     * back between them they are a splice -- WipEout Pulse's menu movie at
     * its loop -- and that slice is left out with the bytes that open the
     * second, four pictures. */
    {
        enum { M = BASE + 0x500, R = BASE + 0x580, D = BASE + 0x60000, WK = BASE + 0x70000,
               PIC = BASE + 0x40000, PICP = BASE + 0x2F0, INIT = BASE + 0x2F8 };
        static const unsigned char aud[] = {0,0,0,1,9,0x30},
                                   slice[] = {0,0,0,1,0x41,0x9a,0x38,0x15,0xea},
                                   junk[] = {0xff,0xff,0x13,0x37};
        unsigned char first[sizeof video + sizeof aud + sizeof slice];
        unsigned char second[sizeof junk + sizeof aud + sizeof slice + sizeof aud];
        memcpy(first, video, sizeof video);
        memcpy(first + sizeof video, aud, sizeof aud);
        memcpy(first + sizeof video + sizeof aud, slice, sizeof slice);
        memcpy(second, junk, sizeof junk);
        memcpy(second + sizeof junk, aud, sizeof aud);
        memcpy(second + sizeof junk + sizeof aud, slice, sizeof slice);
        memcpy(second + sizeof junk + sizeof aud + sizeof slice, aud, sizeof aud);
        CHECK(call(0x606A4649,BASE + 0x80,0,0,0,0,0)==0);    /* Delete the two above: room */
        CHECK(call(0x606A4649,BASE + 0x480,0,0,0,0,0)==0);
        for (int back = 0; back < 2; back++) {
            const uint32_t size = call(0xD7A29F46,4,0,0,0,0,0);
            CHECK(call(0x37295ED8,R,4,D,size,CALLBACK,0)==0);
            CHECK(call(0xD8C5F121,M,WK,0x10000,R,512,0)==0);
            CHECK(call(0x42560F23,M,0,0,0,0,0)!=0);   /* RegistStream: video */
            memset(packet,0,sizeof packet);
            pes(pack(9000),0xe0,90000,first,sizeof first);
            CHECK(call(0xB240A59E,R,1,4,0,0,0)==1);
            memset(packet,0,sizeof packet);
            pes(pack(back ? 3000 : 12000),0xe0,99009,second,sizeof second);
            CHECK(call(0xB240A59E,R,1,3,0,0,0)==1);
            psp_write32(PICP,PIC);
            int pictures = 0;
            while (pictures < 8 && call(0xFE246728,M,0,AU,0,0,0)==0) {
                CHECK(call(0x0E3C2E9D,M,AU,16,PICP,INIT,0)==0);
                pictures++;
            }
            CHECK(pictures == (back ? 4 : 5));
            CHECK(call(0x606A4649,M,0,0,0,0,0)==0);
        }
    }
    call(0x874624D6,0,0,0,0,0,0);
    psp_mem_free();
    puts(failed ? "mpeg_au: FAILED" : "mpeg_au: passed"); return failed!=0;
}
