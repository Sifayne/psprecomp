/* psprecomp — sceMpeg.
 *
 * ## What this is, and what it is not
 *
 * This is the bookkeeping half of MPEG playback: init, contexts, the ring
 * buffer, stream registration, and the PSMF header queries. By default it is
 * **not** a decoder: no video frame is produced and no audio is decoded.
 *
 * PSPRECOMP_MPEG_DECODE=1 turns on the other half -- a PSMF demuxer, an
 * openh264 video path and, through atrac.c's libavcodec wrapper, the ATRAC3+
 * audio -- which does produce frames and samples and hand them back.
 * It is opt-in because it does not yet get the game further; see "What the
 * decode path does not do" at the end of this block.
 *
 * That is a deliberate split rather than an unfinished one. A game reaches
 * sceMpeg long before it plays a movie, because the class that owns FMV is
 * constructed during startup -- Armored Core's said so out loud, on its own
 * stdout:
 *
 *     CFsMpegSound::StartThread() is failed...
 *
 * and every one of these calls was returning zero from the unimplemented-call
 * path, which reads as success. So the game believed it had a working MPEG
 * context, a ring buffer it never got, and a handle that was zero. Answering
 * the bookkeeping honestly is what lets startup finish; decoding is a separate,
 * much larger piece of work that only matters once a movie actually plays.
 *
 * The calls that would hand back decoded data report SCE_MPEG_ERROR_INVALID_VALUE
 * rather than SCE_MPEG_ERROR_NO_DATA, which is not the obvious choice and was
 * arrived at by reading what this game actually does with each:
 *
 *     002751A4  bne   $s3, $zero, 0x00275288    ; an error at all?
 *     00275288  addiu $a0, $a0, -32767          ; a0 = 0x80618001, NO_DATA
 *     0027528C  beq   $s3, $a0, 0x00275268      ; exactly NO_DATA -> go round again
 *     00275290  ...                             ; anything else -> report and stop
 *
 * NO_DATA means "not yet", so a player waits on it -- correctly. Returning it
 * from a decoder that will never produce a frame is therefore an instruction to
 * spin forever, and that is what happened: fifteen million ring buffer queries
 * and nearly eight million GetAvcAu calls in a single minute, with the game
 * never leaving its intro movie.
 *
 * Any other code ends playback, so the honest one is used: the request cannot
 * be satisfied. The game prints
 *
 *     sceMpegGetAvcAu() is failed...ret=806101FE
 *
 * and gives up on the movie, which is the desired outcome and says out loud
 * what happened rather than pretending a stream ended.
 *
 * sceMpeg has no published specification. The constants below are the ones the
 * community's reverse engineering settled on, and what validates them here is
 * end to end rather than by citation: the game queries a size, allocates it,
 * hands back a ring buffer built to it, and the movie plays. A wrong value
 * does not misbehave subtly -- the allocation is the wrong size and playback
 * never starts.
 *
 * ## What the decode path does not do
 *
 * Stated rather than hidden, because each is correct for the one situation this
 * game creates and wrong in general. None is worth fixing while the path is
 * opt-in and single-context; all of them are worth knowing before it is not.
 *
 *   - `ctx_for_ringbuffer` matches on the ring buffer the game passed to
 *     sceMpegCreate, and otherwise falls back to the first context in use. With
 *     one context that is unambiguous. With two, RingbufferPut and
 *     AvailableSize both misroute to whichever occupies the lower slot.
 *   - PACKETS_READ is advanced in RingbufferPut alongside PACKETS_WRITTEN,
 *     rather than when the decoder consumes. So read == written always and
 *     neither counter carries information. AvailableSize synthesises the
 *     answer from the elementary stream instead. Hardware does not work this
 *     way; decrementing a real free-count per put walked it to zero and ended
 *     the movie twenty puts in, which is why it is done this way here.
 *   - Presentation timestamps are `frames * 3600` -- 90kHz at a hardcoded
 *     25fps. The rate is never read from the bitstream and the PES headers'
 *     own PTS fields are discarded by the demuxer.
 *   - `atrac_pts` advances on every successful GetAtracAu with no idempotency
 *     key, so a caller that retries the same access unit advances the audio
 *     clock twice. It is decoupled from the video frame on purpose -- pinned to
 *     video it stood still and SoundThread synced on it -- but decoupling it
 *     from any consumption signal is what allows the double-advance.
 */

#include "psprecomp/hle.h"
#include "psprecomp/cpu.h"
#include "psprecomp/dispatch.h"
#include "psprecomp/mem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- constants ------------------------------------------------------------
 *
 * Two of these are derivations rather than lookups, and are marked. The rest
 * are reverse-engineered values validated by the movie playing; see the header
 * comment. */
#define MPEG_MEMSIZE            0x10000u    /* MPEG_MEMSIZE_0105: what Create needs */
#define MPEG_AVC_ES_SIZE        2048
#define MPEG_ATRAC_ES_SIZE      2112
#define MPEG_ATRAC_ES_OUT_SIZE  8192
/* 90000 * 2048 / 44100: one ATRAC3+ frame, in PSP's 90kHz timestamp units. */
#define MPEG_ATRAC_PTS_STEP     4180u
#define MPEG_RINGBUFFER_PACKET  (104 + 2048)  /* a 2048-byte sector plus a 104-byte header */

#define PSMF_MAGIC              0x464D5350u  /* "PSMF" */
#define PSMF_STREAM_OFFSET_OFF  0x08         /* big-endian u32 */
#define PSMF_STREAM_SIZE_OFF    0x0C         /* big-endian u32 */

#define SCE_MPEG_ERROR_NO_DATA       0x80618001u
#define SCE_MPEG_ERROR_NOT_YET_INIT  0x80618009u
#define SCE_MPEG_ERROR_INVALID_VALUE 0x806101FEu
#define SCE_MPEG_ERROR_NO_MEMORY     0x80610022u

/* SceMpegRingbuffer, as the guest sees it. Field order matters: the game reads
 * these directly, and packetsFree in particular is how it decides whether it
 * may queue more data. */
#define RB_PACKETS          0
#define RB_PACKETS_READ     4
#define RB_PACKETS_WRITTEN  8
#define RB_PACKETS_FREE    12
#define RB_PACKET_SIZE     16
#define RB_DATA            20
#define RB_CALLBACK        24
#define RB_CALLBACK_ARG    28
#define RB_DATA_UPPER      32
#define RB_SEMA_ID         36
#define RB_MPEG            40

/* SceMpegAu: two 64-bit timestamps then the elementary-stream buffer. */
#define AU_PTS       0
#define AU_DTS       8
#define AU_ES_BUFFER 16
#define AU_ES_SIZE   20

/* An unset timestamp. A player compares against this to decide it has none. */
#define MPEG_TIMESTAMP_UNSET 0xFFFFFFFFu

#define MAX_MPEG    4
#define MAX_ES_BUF  8

typedef struct {
    int      used;
    uint32_t handle;       /* what the guest passes back as `mpeg` */
    uint32_t ringbuffer;   /* guest address of its SceMpegRingbuffer */
    uint32_t stream_size;
    uint32_t stream_offset;
    int      es_used[MAX_ES_BUF];

    /* The demuxed AVC elementary stream, accumulated across ring buffer puts.
     * The game hands us an MPEG program stream a few packets at a time; the
     * video is one substream inside it, and a decoder wants it contiguous. */
    uint8_t *es;
    size_t   es_len, es_cap;
    int      es_eof;       /* the ring callback has short-delivered: the whole
                           * stream has now been fed, and whatever is still
                           * undecoded in `es` is the tail of the file */
    size_t   es_pos;       /* how far the decoder has consumed */
    /* A video PES is routinely larger than one ring-buffer put -- an IDR can
     * be fifty times the packet -- so payload bytes of the current PES that
     * have not arrived yet are counted here and consumed as payload, not
     * parsed as structure, when they come in. */
    size_t   es_pes_left;
    /* A PES or pack header that spanned the end of the previous chunk is held
     * here until its second half arrives. Small by construction: structure,
     * never payload. */
    uint8_t  es_pend[64];
    size_t   es_pend_len;
    /* Which substream the in-flight PES belongs to: 0 video, 1 audio. */
    int      es_pes_audio;
    /* The audio elementary stream, the same way: ATRAC3+ frames back to back,
     * each an 8-byte 0F D0 header and its payload, with the 4-byte private
     * header every audio PES starts with taken off. */
    uint8_t *aes;
    size_t   aes_len, aes_cap, aes_pos;
    size_t   aes_skip;     /* private-header bytes still to drop from the in-flight audio PES */
    void    *adec;         /* psp_at3_dec*, opened from the first frame's header */
    uint32_t adec_block;   /* the payload size it was opened for */

    void    *dec;          /* ISVCDecoder*, opaque here so the header stays out */
    int      dec_failed;
    int      frames;       /* decoded so far */

    /* The most recently decoded picture, copied out of the decoder.
     * openh264 hands back pointers into its own buffers that stay valid only
     * until the next decode, and the game fetches an access unit and decodes it
     * as two separate calls -- so it is copied rather than borrowed. */
    uint8_t *pic;          /* I420, planes back to back */
    int      pic_w, pic_h;
    int      pic_ready;
    uint32_t pts;          /* 90kHz, derived from the frame number */
    uint32_t atrac_pts;    /* 90kHz, advancing at the audio rate -- see below */
} mpeg_ctx;

static mpeg_ctx g_mpeg[MAX_MPEG];
static int      g_inited;

void psp_mpeg_reset(void) {
    for (int i = 0; i < MAX_MPEG; i++) {
        psp_at3_close((psp_at3_dec *)g_mpeg[i].adec);
        free(g_mpeg[i].aes);
    }
    memset(g_mpeg, 0, sizeof g_mpeg);
    g_inited = 0;
}

/* Look up the context for a `mpeg` argument.
 *
 * Every call after Create takes what SceMpeg actually is: a *pointer to* the
 * handle, not the handle. Create is handed somewhere to put it, and the game
 * keeps passing that same address back -- so the value has to be read through
 * it. Comparing the argument directly matched nothing, and the game said so
 * on its own stdout, sceMpegRegistStream() is failed..., which is what a
 * returned handle of zero means to it.
 *
 * The raw value is still accepted too, because it costs nothing and a caller
 * that kept the handle itself would otherwise be turned away for no reason. */
static mpeg_ctx *ctx_of(uint32_t mpeg_addr) {
    if (!mpeg_addr) return NULL;

    const uint32_t handle = psp_read32(mpeg_addr);
    for (int i = 0; i < MAX_MPEG; i++)
        if (g_mpeg[i].used && g_mpeg[i].handle == handle) return &g_mpeg[i];
    for (int i = 0; i < MAX_MPEG; i++)
        if (g_mpeg[i].used && g_mpeg[i].handle == mpeg_addr) return &g_mpeg[i];
    return NULL;
}

/* The PSMF header stores its two size fields big-endian, unlike everything else
 * on the machine. */
static uint32_t read_be32(uint32_t addr) {
    return ((uint32_t)psp_read8(addr)     << 24) |
           ((uint32_t)psp_read8(addr + 1) << 16) |
           ((uint32_t)psp_read8(addr + 2) <<  8) |
           ((uint32_t)psp_read8(addr + 3));
}

/* ---- lifecycle ------------------------------------------------------------ */

static void hle_MpegInit(void)   { g_inited = 1; psp_ret(SCE_KERNEL_ERROR_OK); }

static void hle_MpegFinish(void) {
    psp_mpeg_reset();
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_QueryMemSize(void) { psp_ret(MPEG_MEMSIZE); }

/* sceMpegCreate(mpeg, data, size, ringbuffer, frameWidth, mode, ddrTop)
 *
 * `mpeg` is a pointer to where the handle goes, not the handle itself -- the
 * one detail that makes the difference between a game that works and one that
 * dereferences zero. The handle is the buffer it gave us, which it can pass
 * back without us having to invent an id space. */
static void hle_MpegCreate(void) {
    const uint32_t mpeg_out = psp_arg(0);
    const uint32_t data     = psp_arg(1);
    const uint32_t size     = psp_arg(2);
    const uint32_t ringbuf  = psp_arg(3);

    if (!g_inited)          { psp_ret(SCE_MPEG_ERROR_NOT_YET_INIT); return; }
    if (!mpeg_out || !data)  { psp_ret(SCE_MPEG_ERROR_INVALID_VALUE); return; }
    if (size < MPEG_MEMSIZE) { psp_ret(SCE_MPEG_ERROR_NO_MEMORY); return; }

    mpeg_ctx *c = NULL;
    for (int i = 0; i < MAX_MPEG; i++) if (!g_mpeg[i].used) { c = &g_mpeg[i]; break; }
    if (!c) { psp_ret(SCE_MPEG_ERROR_NO_MEMORY); return; }

    memset(c, 0, sizeof *c);
    c->used       = 1;
    c->handle     = data;
    c->ringbuffer = ringbuf;

    psp_write32(mpeg_out, c->handle);
    if (ringbuf) psp_write32(ringbuf + RB_MPEG, c->handle);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_MpegDelete(void) {
    mpeg_ctx *c = ctx_of(psp_arg(0));
    if (c) c->used = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* ---- the ring buffer ------------------------------------------------------ */

static void hle_RingbufferQueryMemSize(void) {
    psp_ret((uint32_t)((int32_t)psp_arg(0) * MPEG_RINGBUFFER_PACKET));
}

/* sceMpegRingbufferConstruct(rb, packets, data, size, callback, cbArg) */
static void hle_RingbufferConstruct(void) {
    const uint32_t rb      = psp_arg(0);
    const uint32_t packets = psp_arg(1);
    const uint32_t data    = psp_arg(2);
    const uint32_t size    = psp_arg(3);

    if (!rb) { psp_ret(SCE_MPEG_ERROR_INVALID_VALUE); return; }

    psp_write32(rb + RB_PACKETS,         packets);
    psp_write32(rb + RB_PACKETS_READ,    0);
    psp_write32(rb + RB_PACKETS_WRITTEN, 0);
    psp_write32(rb + RB_PACKETS_FREE,    packets);
    psp_write32(rb + RB_PACKET_SIZE,     MPEG_AVC_ES_SIZE);
    psp_write32(rb + RB_DATA,            data);
    psp_write32(rb + RB_CALLBACK,        psp_arg(4));
    psp_write32(rb + RB_CALLBACK_ARG,    psp_arg(5));
    psp_write32(rb + RB_DATA_UPPER,      data + size);
    psp_write32(rb + RB_SEMA_ID,         0xFFFFFFFFu);
    psp_write32(rb + RB_MPEG,            0);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_RingbufferDestruct(void) {
    const uint32_t rb = psp_arg(0);
    if (rb) {
        psp_write32(rb + RB_PACKETS_READ,    0);
        psp_write32(rb + RB_PACKETS_WRITTEN, 0);
        psp_write32(rb + RB_PACKETS_FREE,    psp_read32(rb + RB_PACKETS));
    }
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static int mpeg_decoding(void);
static int mpeg_logging(void);
static mpeg_ctx *ctx_for_ringbuffer(uint32_t rb);

/* Free packets.
 *
 * Not simply "all of them". The game reads this as how much of the buffer is
 * still occupied, and treats an entirely free ring as nothing having been
 * buffered at all -- it compares the answer against the full packet count and
 * gives up when they match. Reporting everything free after each put therefore
 * ends the movie just as surely as never freeing anything did, by the opposite
 * route.
 *
 * So it is derived from the elementary stream instead: whatever the demuxer has
 * produced but the decoder has not yet consumed is still notionally sitting in
 * the buffer. That keeps the number moving the way the game expects while
 * staying true to what has actually been dealt with. */
static void hle_RingbufferAvailableSize(void) {
    const uint32_t rb = psp_arg(0);
    if (mpeg_decoding() && rb) {
        mpeg_ctx *c = ctx_for_ringbuffer(rb);
        const uint32_t packets  = psp_read32(rb + RB_PACKETS);
        const uint32_t pkt_size = psp_read32(rb + RB_PACKET_SIZE);
        if (c && packets && pkt_size) {
            const size_t pending = c->es_len > c->es_pos ? c->es_len - c->es_pos : 0;
            uint32_t held = (uint32_t)(pending / pkt_size);
            if (held > packets) held = packets;
            if (mpeg_logging()) {
                static int said;
                if (held == 0 && said++ < 5)
                    fprintf(stderr, "mpeg: AvailableSize reports ALL %u free "
                            "(es_len=%zu es_pos=%zu pending=%zu pkt=%u)\n",
                            packets, c->es_len, c->es_pos, pending, pkt_size);
            }
            psp_ret(packets - held);
            return;
        }
    }

    psp_ret(rb ? psp_read32(rb + RB_PACKETS_FREE) : 0);
}

/* sceMpegRingbufferPut(rb, numPackets, available)
 *
 * On hardware this invokes the game's own callback to copy packets in, then
 * accounts for them. Nothing consumes them here, so they are accepted and
 * immediately treated as consumed: the buffer never fills, and a feeder loop
 * that waits for space always gets it. Claiming zero were taken would stall
 * that loop forever, which is the failure this is avoiding. */
/* Run a guest function from inside a firmware call and come back with $v0.
 *
 * The ring buffer is filled by a callback the *game* supplies, so there is no
 * way to get its data without re-entering guest code from here. The register
 * file is saved and restored around it because we are already standing in the
 * middle of somebody else's call: the caller's arguments are still live in
 * a0-a3 and its return address in $ra, and the callee will use all of them.
 *
 * $ra is zeroed rather than left alone so that returning from the callback ends
 * the dispatch instead of running off into whatever the interrupted caller was
 * going to do next. */
/* PSPRECOMP_MPEG_DECODE=1 turns on the video path: the ring buffer is filled
 * from the game's callback, demuxed, and decoded.
 *
 * Opt-in because it does not get the game further, not because it does not
 * work. The frames are real -- byte-identical to an independent decoder -- and
 * they are handed back: GetAvcAu returns access units and AvcDecode writes
 * pictures. What then happens is that the game's own frame queue fills, because
 * the code that drains it is never reached, so it stops asking. Default is the
 * refusal, which at least lets the game give up cleanly instead of spinning.
 *
 * Without openh264 the decoder is a stub that reports "no picture" forever, and
 * NO_DATA means "not yet" -- so honouring the switch in that build would ask
 * the player to wait for a frame that cannot arrive, which is exactly the
 * infinite spin the choice of INVALID_VALUE above exists to avoid. Refuse the
 * override instead of quietly doing the harmful thing. */
static int mpeg_decoding(void) {
    static int done, on;
    if (!done) {
        const char *v = getenv("PSPRECOMP_MPEG_DECODE");
        on = v && *v && *v != '0';
#if !PSPRECOMP_HAVE_OPENH264
        if (on) {
            fprintf(stderr,
                    "psprecomp: PSPRECOMP_MPEG_DECODE ignored -- this build has no\n"
                    "  openh264, so no picture can ever be produced and the game\n"
                    "  would wait for one forever. Rebuild with openh264 available.\n");
            on = 0;
        }
#endif
        done = 1;
    }
    return on;
}

/* PSPRECOMP_MPEG=1 narrates the movie path. Off by default. */
static int mpeg_logging(void) {
    static int done, on;
    if (!done) { const char *v = getenv("PSPRECOMP_MPEG"); on = v && *v && *v != '0'; done = 1; }
    return on;
}

/* ---- MPEG program stream demux ---------------------------------------------
 *
 * A PSMF is an MPEG-2 program stream: pack headers, an optional system header,
 * and PES packets carrying one substream each. Only the video matters here --
 * stream id 0xE0 -- and its payload concatenated in order is the AVC elementary
 * stream, which is what a decoder takes.
 *
 * Deliberately not a general demuxer. It skips what it does not need and keeps
 * only the video payload, because the alternative is carrying a container
 * library for one file format in one game. */
#define PS_PACK_START   0xBAu
#define PS_SYSTEM_HDR   0xBBu
#define PS_PROGRAM_END  0xB9u
#define PS_VIDEO_STREAM 0xE0u
#define PS_PRIVATE_1    0xBDu  /* the audio: ATRAC3+ frames behind a 4-byte private header */

static mpeg_ctx *ctx_for_ringbuffer(uint32_t rb) {
    for (int i = 0; i < MAX_MPEG; i++)
        if (g_mpeg[i].used && g_mpeg[i].ringbuffer == rb) return &g_mpeg[i];
    /* Construct records the ring buffer against the context only once the game
     * has associated them; before that, one context is unambiguous. */
    for (int i = 0; i < MAX_MPEG; i++)
        if (g_mpeg[i].used) return &g_mpeg[i];
    return NULL;
}

static int buf_append(uint8_t **buf, size_t *len, size_t *cap, const uint8_t *p, size_t n) {
    if (*len + n > *cap) {
        size_t c = *cap ? *cap : 65536;
        while (c < *len + n) c *= 2;
        uint8_t *ne = (uint8_t *)realloc(*buf, c);
        if (!ne) return -1;
        *buf = ne;
        *cap = c;
    }
    memcpy(*buf + *len, p, n);
    *len += n;
    return 0;
}
static int es_append(mpeg_ctx *c, const uint8_t *p, size_t n) {
    return buf_append(&c->es, &c->es_len, &c->es_cap, p, n);
}
/* Audio payload, less whatever of the PES's 4-byte private header is still
 * owed: the first byte is the substream number, the fourth the offset of the
 * first frame header in the payload, and neither is needed once frames are
 * being walked by their own headers. */
static int aes_append(mpeg_ctx *c, const uint8_t *p, size_t n) {
    if (c->aes_skip) {
        const size_t k = c->aes_skip < n ? c->aes_skip : n;
        c->aes_skip -= k; p += k; n -= k;
    }
    return n ? buf_append(&c->aes, &c->aes_len, &c->aes_cap, p, n) : 0;
}

/* Pull the video payload out of a run of program stream bytes, carrying
 * across chunk boundaries whatever does not fit.
 *
 * A video PES is routinely larger than one ring-buffer put, so the parse
 * cannot treat each put as complete: payload that runs past the end is
 * counted and consumed as payload by the next calls, and a header that spans
 * the end is held until its second half arrives. Doing neither was invisible
 * for the first frames -- the first puts are large enough to hold the IDR --
 * and ruinous later: one spanning PES lost up to a whole frame of slices, the
 * decoder's references went stale, and every frame smeared until the next
 * IDR, which read as horizontal streaking in a decode whose early frames were
 * byte-perfect. */
static void ps_demux(mpeg_ctx *c, const uint8_t *buf, size_t len) {
    /* Continue a payload that did not fit in the previous chunk. */
    if (c->es_pes_left) {
        const size_t n = c->es_pes_left < len ? c->es_pes_left : len;
        if (c->es_pes_audio) aes_append(c, buf, n); else es_append(c, buf, n);
        c->es_pes_left -= n;
        buf += n;
        len -= n;
        if (c->es_pes_left) return;
    }

    /* Work over whatever a previous chunk left held, if anything. */
    const uint8_t *p = buf;
    size_t plen_all = len;
    uint8_t *work = NULL;
    if (c->es_pend_len) {
        work = malloc(c->es_pend_len + len);
        if (!work) return;
        memcpy(work, c->es_pend, c->es_pend_len);
        memcpy(work + c->es_pend_len, buf, len);
        plen_all = c->es_pend_len + len;
        c->es_pend_len = 0;
        p = work;
    }

    size_t i = 0;
    while (i + 4 <= plen_all) {
        if (!(p[i] == 0 && p[i+1] == 0 && p[i+2] == 1)) { i++; continue; }
        const uint8_t id = p[i+3];

        if (id == PS_PACK_START) {
            /* 14 fixed bytes, then however many stuffing bytes the low three
             * bits of the last one claim. */
            if (i + 14 > plen_all) break;
            i += 14 + (size_t)(p[i+13] & 7);
            continue;
        }
        if (id == PS_PROGRAM_END) { i += 4; continue; }
        if (i + 6 > plen_all) break;

        const size_t plen = ((size_t)p[i+4] << 8) | p[i+5];
        if (id == PS_SYSTEM_HDR) { i += 6 + plen; continue; }

        if (id == PS_VIDEO_STREAM || id == PS_PRIVATE_1) {
            const int audio = id == PS_PRIVATE_1;
            if (i + 9 > plen_all) break;
            const size_t hdrlen = p[i+8];
            const size_t off    = i + 9 + hdrlen;
            /* plen counts from just after itself, so the payload is what is
             * left of it once the PES header is taken off. */
            if (plen >= 3 + hdrlen) {
                if (off > plen_all) break;   /* even the header extension spans */
                const size_t pay = plen - 3 - hdrlen;
                size_t n = pay;
                if (off + n > plen_all) n = plen_all - off;
                if (audio) { c->aes_skip = 4; aes_append(c, p + off, n); }
                else       es_append(c, p + off, n);
                if (n < pay) {
                    /* Everything from off to the end is payload; the rest of
                     * it arrives in later chunks and must not be parsed. */
                    c->es_pes_left  = pay - n;
                    c->es_pes_audio = audio;
                    free(work);
                    return;
                }
            }
        }
        i += 6 + plen;
    }

    /* Hold whatever did not parse for the next put. Larger than a header can
     * be means it was not a header; the next start code resyncs, which is
     * what the byte-scan is for. */
    if (i < plen_all && plen_all - i <= sizeof c->es_pend) {
        memcpy(c->es_pend, p + i, plen_all - i);
        c->es_pend_len = plen_all - i;
    }
    free(work);
}

/* ---- H.264 decode ----------------------------------------------------------
 *
 * openh264, chosen because it is BSD and can therefore be linked into an MIT
 * library without changing what anyone downstream may do with the result. Its
 * C++ interface has a C vtable binding, which is what is used here so the
 * runtime stays C.
 *
 * Fed one NAL at a time. The decoder buffers until it has a whole picture and
 * then reports one ready, which saves this code from having to work out where
 * access units begin -- a job that needs more of the bitstream syntax than is
 * worth parsing twice. */
#if PSPRECOMP_HAVE_OPENH264
#include <wels/codec_api.h>

/* Find the next start code at or after `from`. Returns the offset of the 0x01,
 * or len if there is none. */
static size_t nal_next(const uint8_t *p, size_t len, size_t from) {
    for (size_t i = from; i + 3 <= len; i++)
        if (p[i] == 0 && p[i+1] == 0 && p[i+2] == 1) return i + 3;
    return len;
}

/* Write one decoded I420 picture as a PPM. Bring-up only: it proves the whole
 * chain -- the game's own callback, our demux, and the decoder -- produced a
 * real picture, before any of it is wired to the guest's frame buffer. */
static void dump_ppm(const char *path, unsigned char *pl[3], const SSysMEMBuffer *b) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    const int w = b->iWidth, h = b->iHeight;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            /* BT.601, and *limited* range: this stream declares tv range, so Y
             * runs 16..235 and passing it through unscaled lifts the blacks --
             * a first frame that should read 16 came out 30 against ffmpeg's
             * decode of the same bytes. */
            const int Y = pl[0][y * b->iStride[0] + x] - 16;
            const int U = pl[1][(y / 2) * b->iStride[1] + x / 2] - 128;
            const int V = pl[2][(y / 2) * b->iStride[1] + x / 2] - 128;
            const int Yr = 76309 * Y;
            int r  = (Yr + 104597 * V) >> 16;
            int g  = (Yr -  53279 * V - 25674 * U) >> 16;
            int bl = (Yr + 132201 * U) >> 16;
            unsigned char px[3] = {
                (unsigned char)(r < 0 ? 0 : r > 255 ? 255 : r),
                (unsigned char)(g < 0 ? 0 : g > 255 ? 255 : g),
                (unsigned char)(bl < 0 ? 0 : bl > 255 ? 255 : bl) };
            fwrite(px, 1, 3, f);
        }
    fclose(f);
}

/* Decode up to ONE picture and stop. Returns 1 if it produced one.
 *
 * Decoding everything available in a single call looks like the efficient
 * choice and is wrong twice over. Only the most recent picture is kept, so all
 * the others are decoded and thrown away -- and the caller asks for one access
 * unit at a time, so the frames it never sees are frames the game never gets.
 * The first call swallowed the whole elementary stream and produced one usable
 * frame; every call after it found nothing left and answered NO_DATA.
 *
 * It also breaks the ring buffer accounting, which reports how much has been
 * demuxed but not yet decoded. Consuming the stream as fast as it arrives makes
 * that difference zero, so the buffer always looks empty. */
static int avc_pump(mpeg_ctx *c) {
    if (c->dec_failed) return 0;
    if (!c->dec) {
        ISVCDecoder *d = NULL;
        if (WelsCreateDecoder(&d) != 0 || !d) { c->dec_failed = 1; return 0; }
        SDecodingParam prm;
        memset(&prm, 0, sizeof prm);
        prm.eEcActiveIdc = ERROR_CON_SLICE_COPY;
        prm.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_DEFAULT;
        if ((*d)->Initialize(d, &prm) != 0) {
            WelsDestroyDecoder(d); c->dec_failed = 1; return 0;
        }
        c->dec = d;
    }
    ISVCDecoder *d = (ISVCDecoder *)c->dec;

    int produced = 0;
    size_t pos = c->es_pos;
    while (pos < c->es_len) {
        const size_t start = nal_next(c->es, c->es_len, pos);
        if (start >= c->es_len) break;
        size_t end = nal_next(c->es, c->es_len, start);
        size_t nal_end;
        if (end >= c->es_len) {
            /* No following start code. Mid-stream that means "incomplete, wait
             * for more"; at end of stream it means this is the last NAL and
             * everything left is it -- a tail that would otherwise never
             * decode, and the last frames of the movie would silently drop. */
            if (!c->es_eof) break;
            nal_end = c->es_len;
        } else {
            /* end points past the next start code's 0x01; back up over it and
             * any trailing zero of a four-byte start code. */
            nal_end = end - 3;
            while (nal_end > start && c->es[nal_end - 1] == 0) nal_end--;
        }

        unsigned char *planes[3] = { 0, 0, 0 };
        SBufferInfo info;
        memset(&info, 0, sizeof info);
        (*d)->DecodeFrameNoDelay(d, c->es + start - 3, (int)(nal_end - (start - 3)),
                                 planes, &info);
        if (info.iBufferStatus == 1 && planes[0]) {
            c->frames++;
            produced++;
            {   /* Copy the picture out while it is still ours to read. */
                const SSysMEMBuffer *b = &info.UsrData.sSystemBuffer;
                const size_t need = (size_t)b->iWidth * (size_t)b->iHeight * 3 / 2;
                if (!c->pic || c->pic_w != b->iWidth || c->pic_h != b->iHeight) {
                    free(c->pic);
                    c->pic = (uint8_t *)malloc(need);
                    c->pic_w = b->iWidth;
                    c->pic_h = b->iHeight;
                }
                if (c->pic) {
                    uint8_t *d = c->pic;
                    for (int y = 0; y < b->iHeight; y++, d += b->iWidth)
                        memcpy(d, planes[0] + (size_t)y * b->iStride[0], (size_t)b->iWidth);
                    for (int pl = 1; pl <= 2; pl++)
                        for (int y = 0; y < b->iHeight / 2; y++, d += b->iWidth / 2)
                            memcpy(d, planes[pl] + (size_t)y * b->iStride[1], (size_t)b->iWidth / 2);
                    c->pic_ready = 1;
                    /* 90kHz at 25fps, which is what the stream declares. */
                    c->pts = (uint32_t)c->frames * (90000u / 25u);
                }
            }
            if (mpeg_logging() && (c->frames == 1 || c->frames % 50 == 0))
                fprintf(stderr, "mpeg: %d frames decoded\n", c->frames);
            const char *fp = getenv("PSPRECOMP_MPEG_FRAME");
            const char *fn = getenv("PSPRECOMP_MPEG_FRAME_NO");
            const int want = fn ? atoi(fn) : 1;
            if (fp && c->frames == want) {
                dump_ppm(fp, planes, &info.UsrData.sSystemBuffer);
                fprintf(stderr, "mpeg: decoded frame %d, %dx%d -> %s\n", c->frames,
                        info.UsrData.sSystemBuffer.iWidth,
                        info.UsrData.sSystemBuffer.iHeight, fp);
            }
        }
        pos = start;
        c->es_pos = pos;
        if (produced) break;          /* one picture per call */
    }
    return produced;
}
#else
static int avc_pump(mpeg_ctx *c) { (void)c; return 0; }
#endif

static uint32_t call_guest(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2) {
    const psp_cpu_state save = psp_cpu;
    psp_cpu.r[PSP_REG_A0] = a0;
    psp_cpu.r[PSP_REG_A1] = a1;
    psp_cpu.r[PSP_REG_A2] = a2;
    psp_cpu.r[PSP_REG_RA] = 0;
    psp_dispatch(fn);
    const uint32_t v0 = psp_cpu.r[PSP_REG_V0];
    psp_cpu = save;
    return v0;
}

/* sceMpegRingbufferPut(rb, numPackets, available)
 *
 * The count was being bumped without ever asking the game for the bytes, so the
 * ring buffer stayed empty and every packet the demuxer would have read was a
 * packet that was never written. The data comes from the callback the game
 * registered in Construct; calling it is the only way to get the movie.
 *
 * The callback takes (data, numPackets, arg) and answers how many packets it
 * actually supplied, which is not always what was asked for -- at the end of
 * the file it is fewer, and that is how the stream ends. */
static void hle_RingbufferPut(void) {
    const uint32_t rb    = psp_arg(0);
    const int32_t  want  = (int32_t)psp_arg(1);
    const int32_t  avail = (int32_t)psp_arg(2);

    if (mpeg_logging())
        fprintf(stderr, "mpeg: RingbufferPut(rb=0x%08X want=%d avail=%d)\n", rb, want, avail);
    if (!rb || want <= 0) { psp_ret(0); return; }
    if (!mpeg_decoding()) { psp_ret((uint32_t)want); return; }  /* pre-decoder behaviour */

    int32_t n = want < avail ? want : avail;
    /* Fetched before the callback so the short-delivery check below can reach
     * it even when the delivery is zero packets. */
    mpeg_ctx *c = ctx_for_ringbuffer(rb);
    const uint32_t cb = psp_read32(rb + RB_CALLBACK);
    if (cb) {
        const uint32_t pkt_size = psp_read32(rb + RB_PACKET_SIZE);
        const uint32_t written  = psp_read32(rb + RB_PACKETS_WRITTEN);
        const uint32_t packets  = psp_read32(rb + RB_PACKETS);
        const uint32_t base     = psp_read32(rb + RB_DATA);
        /* Write at the tail, and stop at the end of the buffer rather than
         * wrapping mid-call: the callback fills one contiguous run. */
        const uint32_t slot = packets ? (written % packets) : 0;
        if (packets && slot + (uint32_t)n > packets) n = (int32_t)(packets - slot);
        const int32_t asked = n;
        if (n > 0)
            n = (int32_t)call_guest(cb, base + slot * pkt_size, (uint32_t)n,
                                    psp_read32(rb + RB_CALLBACK_ARG));
        /* A short delivery from the callback is the end of the file. The game
         * reads the stream sequentially, so fewer packets than asked means
         * fewer exist -- this is the one signal that the stream has ended,
         * and GetAvcAu turns it into the end-of-stream answer. The clamp
         * above shorted `asked` itself, and is not this. */
        if (c && n < asked) c->es_eof = 1;
    }

    /* One-shot look at what the callback actually delivered. An MPEG program
     * stream starts 00 00 01 BA; a PSMF header starts with "PSMF". Anything
     * else means the ring buffer is not carrying the movie and nothing
     * downstream can work. */
    if (n > 0 && mpeg_logging()) {
        static int shown;
        if (!shown++) {
            const uint32_t base = psp_read32(rb + RB_DATA);
            fprintf(stderr, "mpeg: first put -> %d packets at 0x%08X:", n, base);
            for (int i = 0; i < 16; i++) fprintf(stderr, " %02X", psp_read8(base + (uint32_t)i));
            fprintf(stderr, "\n");
        }
    }

    /* Demux what just arrived. The bytes are in guest memory where the callback
     * put them, so this is the one place they are known to be both present and
     * not yet overwritten by the next put. */
    if (n > 0) {
        if (c) {
            const uint32_t pkt_size = psp_read32(rb + RB_PACKET_SIZE);
            const uint32_t base     = psp_read32(rb + RB_DATA);
            const uint32_t packets  = psp_read32(rb + RB_PACKETS);
            const uint32_t written  = psp_read32(rb + RB_PACKETS_WRITTEN);
            const uint32_t slot     = packets ? (written % packets) : 0;
            const uint32_t bytes    = (uint32_t)n * pkt_size;
            const uint8_t *host = (const uint8_t *)psp_mem_ptr(base + slot * pkt_size, bytes);
            if (host) {
                /* Demux only. Decoding is driven by sceMpegGetAvcAu, one
                 * picture at a time, so that what has arrived but not yet been
                 * decoded stays measurable -- that difference is what
                 * AvailableSize reports, and the game reads it as how full the
                 * buffer is. Decoding eagerly here empties the buffer as fast
                 * as it fills and the game concludes nothing was ever put. */
                ps_demux(c, host, bytes);
            }
            /* PSPRECOMP_MPEG_DUMP=<path> writes the demuxed elementary stream
             * once it is big enough to be worth looking at, so it can be
             * checked against a decoder that is not ours. */
            {
                const char *dp = getenv("PSPRECOMP_MPEG_DUMP");
                static int dumped;
                if (dp && !dumped && c->es_len > 1000000) {
                    FILE *f = fopen(dp, "wb");
                    if (f) { fwrite(c->es, 1, c->es_len, f); fclose(f); dumped = 1;
                             fprintf(stderr, "mpeg: wrote %zu bytes of ES to %s\n", c->es_len, dp); }
                }
            }
        }
    }

    if (n > 0) {
        const uint32_t packets = psp_read32(rb + RB_PACKETS);
        psp_write32(rb + RB_PACKETS_WRITTEN, psp_read32(rb + RB_PACKETS_WRITTEN) + (uint32_t)n);
        psp_write32(rb + RB_PACKETS_READ,    psp_read32(rb + RB_PACKETS_READ) + (uint32_t)n);
        /* The free count itself is answered by AvailableSize, which derives it
         * from how much of the stream is still undecoded. */
        (void)packets;
    }
    psp_ret((uint32_t)n);
}

/* ---- the PSMF header ------------------------------------------------------
 *
 * These two are the only calls here that report something real: the numbers
 * come out of the file the game already read from the disc. */
static void hle_QueryStreamOffset(void) {
    const uint32_t buf = psp_arg(1), out = psp_arg(2);
    if (!buf || !out) { psp_ret(SCE_MPEG_ERROR_INVALID_VALUE); return; }

    if (psp_read32(buf) != PSMF_MAGIC) {
        psp_write32(out, 0);
        psp_ret(SCE_MPEG_ERROR_INVALID_VALUE);
        return;
    }
    const uint32_t off = read_be32(buf + PSMF_STREAM_OFFSET_OFF);
    /* The offset is a whole number of 2048-byte sectors; anything else means
     * the header is not what it claims to be. */
    if (!off || (off & 2047u)) {
        psp_write32(out, 0);
        psp_ret(SCE_MPEG_ERROR_INVALID_VALUE);
        return;
    }
    mpeg_ctx *c = ctx_of(psp_arg(0));
    if (c) c->stream_offset = off;
    psp_write32(out, off);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_QueryStreamSize(void) {
    const uint32_t buf = psp_arg(0), out = psp_arg(1);
    if (!buf || !out) { psp_ret(SCE_MPEG_ERROR_INVALID_VALUE); return; }

    if (psp_read32(buf) != PSMF_MAGIC) {
        psp_write32(out, 0);
        psp_ret(SCE_MPEG_ERROR_INVALID_VALUE);
        return;
    }
    psp_write32(out, read_be32(buf + PSMF_STREAM_SIZE_OFF));
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* ---- streams and elementary-stream buffers -------------------------------- */

/* A stream handle only has to be distinct and non-null: the game hands it back
 * to GetAvcAu and friends and never looks inside it. */
static void hle_RegistStream(void) {
    static uint32_t next = 0x00010000u;
    psp_ret(ctx_of(psp_arg(0)) ? next++ : 0);
}

static void hle_UnRegistStream(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

static void hle_MallocAvcEsBuf(void) {
    mpeg_ctx *c = ctx_of(psp_arg(0));
    if (!c) { psp_ret(0); return; }
    for (int i = 0; i < MAX_ES_BUF; i++)
        if (!c->es_used[i]) { c->es_used[i] = 1; psp_ret((uint32_t)(i + 1)); return; }
    psp_ret(0);
}

static void hle_FreeAvcEsBuf(void) {
    mpeg_ctx *c = ctx_of(psp_arg(0));
    const int32_t id = (int32_t)psp_arg(1) - 1;
    if (c && id >= 0 && id < MAX_ES_BUF) c->es_used[id] = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* sceMpegQueryAtracEsSize(mpeg, esSizeOut, outSizeOut) */
static void hle_QueryAtracEsSize(void) {
    if (psp_arg(1)) psp_write32(psp_arg(1), MPEG_ATRAC_ES_SIZE);
    if (psp_arg(2)) psp_write32(psp_arg(2), MPEG_ATRAC_ES_OUT_SIZE);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* sceMpegInitAu(mpeg, esBuffer, au) */
static void hle_InitAu(void) {
    const uint32_t au = psp_arg(2);
    if (!au) { psp_ret(SCE_MPEG_ERROR_INVALID_VALUE); return; }
    psp_write32(au + AU_PTS,     MPEG_TIMESTAMP_UNSET);
    psp_write32(au + AU_PTS + 4, MPEG_TIMESTAMP_UNSET);
    psp_write32(au + AU_DTS,     MPEG_TIMESTAMP_UNSET);
    psp_write32(au + AU_DTS + 4, MPEG_TIMESTAMP_UNSET);
    psp_write32(au + AU_ES_BUFFER, psp_arg(1));
    psp_write32(au + AU_ES_SIZE,   0);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* ---- the decoder that is not here -----------------------------------------
 *
 * Every one of these reports "no data", which a player reads as the end of the
 * stream. It ends the movie on its first attempt rather than waiting for a
 * frame that will never arrive. Said once, because a title screen retries. */
static void no_decoder(const char *what) {
    static int said;
    if (!said++)
        fprintf(stderr,
            "psprecomp: %s -- sceMpeg decoding is not implemented. Playback is\n"
            "  refused outright rather than reported as empty, because a player\n"
            "  waits out an empty stream and would never stop. Video and FMV\n"
            "  audio will not play; everything else continues.\n", what);
}

/* Two answers, and which one is right depends on whether a frame can ever
 * arrive -- see the header comment for the disassembly behind this.
 *
 * NO_DATA means "not yet", so the game feeds the ring buffer and asks again.
 * That is the only way to get the movie out of it, and it is what the decode
 * path needs. But a NO_DATA that never becomes an answer is an instruction to
 * spin forever: 206 million calls in a minute, measured.
 *
 * So it is only given when decoding is actually running. Otherwise the refusal
 * stands, and the game reports the failure and tears the movie down -- worse
 * picture, honest behaviour, and no hang. */
static void hle_GetAvcAu(void) {
    mpeg_ctx *c = ctx_of(psp_arg(0));
    if (!mpeg_decoding()) {
        no_decoder("sceMpegGetAvcAu");
        psp_ret(SCE_MPEG_ERROR_INVALID_VALUE);
        return;
    }
    /* Decode forward until a picture appears. The decoder is the access-unit
     * boundary detector: it buffers NALs and reports one ready exactly when it
     * has a whole picture, which saves parsing enough slice-header syntax to
     * work out the same thing a second time.
     *
     * No picture has two causes now, and the game treats them oppositely: the
     * ring not fed far enough yet, which is what NO_DATA is for and what the
     * game answers by putting more; and the stream having ended, where NO_DATA
     * is an instruction to spin forever -- 522 million ring queries in a
     * sixty-second run, measured, with the intro never leaving. At a true end
     * -- everything fed, everything consumed, no picture -- the answer is the
     * one the game's own decode loop reads as "report and stop":
     *
     *     0027528C  beq $s3, $a0, 0x00275268   ; NO_DATA -> go round again
     *     ...                                  ; anything else -> stop
     */
    if (!c || (!c->pic_ready && avc_pump(c) == 0)) {
        if (c && c->es_eof && c->es_pos >= c->es_len && !c->pic_ready) {
            psp_ret(SCE_MPEG_ERROR_INVALID_VALUE);
            return;
        }
        psp_ret(SCE_MPEG_ERROR_NO_DATA);
        return;
    }

    const uint32_t au = psp_arg(2);
    if (au) {
        /* The timestamps are what the player paces itself on. The elementary
         * stream stays on our side: the game passes this straight back to
         * sceMpegAvcDecode and never reads through esBuffer itself. */
        psp_write32(au + AU_PTS,     c->pts);
        psp_write32(au + AU_PTS + 4, 0);
        psp_write32(au + AU_DTS,     c->pts);
        psp_write32(au + AU_DTS + 4, 0);
        psp_write32(au + AU_ES_SIZE, 0);
    }
    if (mpeg_logging() && c->frames <= 3)
        fprintf(stderr, "mpeg: GetAvcAu -> frame %d, pts %u\n", c->frames, c->pts);
    psp_ret(SCE_KERNEL_ERROR_OK);
}
/* The audio access unit: the next ATRAC3+ frame of the movie.
 *
 * The audio substream is ATRAC3+ frames back to back, each behind an 8-byte
 * header -- 0F D0, then two bytes of parameters (sample rate index, channel
 * configuration, and the payload size in 8-byte units less one), then four
 * zero bytes -- read off this game's intro: `0F D0 28 5C` is 44.1kHz, stereo,
 * 744 bytes of payload, and the frame after it starts exactly 752 bytes on.
 * The whole frame, header and all, is copied into the caller's ES buffer
 * (2112 bytes, sceMpegQueryAtracEsSize) and sceMpegAtracDecode reads it back
 * from there.
 *
 * Timestamps advance by one frame -- 90000 * 2048 / 44100 ticks -- per access
 * unit rather than being read off the PES, which is what the video does too;
 * the thread parked on Movie Sync is called SoundThread, so this is the clock
 * it waits on and it has to run. A frame that has not arrived yet is NO_DATA,
 * which the player treats as "go round again" (see the header) and now means
 * exactly that: the ring buffer feeds the stream in the game's own order and
 * the audio can be a put or two ahead. Once the ring has short-delivered and
 * the frames are gone, the stream is over. */
#define AT3P_SYNC0 0x0Fu
#define AT3P_SYNC1 0xD0u
#define AT3P_HDR   8u

/* The frame at aes_pos: its total size with the header, or 0 if there is not
 * a whole one yet. Resyncs on the 0F D0 pair if the position has drifted. */
static size_t aes_frame(mpeg_ctx *c) {
    while (c->aes_pos + AT3P_HDR <= c->aes_len) {
        const uint8_t *h = c->aes + c->aes_pos;
        if (h[0] == AT3P_SYNC0 && h[1] == AT3P_SYNC1) {
            const size_t payload = ((((size_t)h[2] & 3u) << 8) | h[3]) * 8u + 8u;
            return c->aes_pos + AT3P_HDR + payload <= c->aes_len ? AT3P_HDR + payload : 0;
        }
        c->aes_pos++;
    }
    return 0;
}

static void hle_GetAtracAu(void) {
    mpeg_ctx *c = ctx_of(psp_arg(0));
    if (!mpeg_decoding() || !c) {
        no_decoder("sceMpegGetAtracAu");
        psp_ret(SCE_MPEG_ERROR_INVALID_VALUE);
        return;
    }
    const uint32_t au = psp_arg(2);
    const size_t total = aes_frame(c);
    if (!total) {
        if (c->es_eof) { psp_ret(SCE_MPEG_ERROR_INVALID_VALUE); return; }
        psp_ret(SCE_MPEG_ERROR_NO_DATA);
        return;
    }
    if (au) {
        const uint32_t esbuf = psp_read32(au + AU_ES_BUFFER);
        if (esbuf && total <= MPEG_ATRAC_ES_SIZE)
            psp_mem_write_block(esbuf, c->aes + c->aes_pos, (uint32_t)total);
        psp_write32(au + AU_PTS,     c->atrac_pts);
        psp_write32(au + AU_PTS + 4, 0);
        psp_write32(au + AU_DTS,     c->atrac_pts);
        psp_write32(au + AU_DTS + 4, 0);
        psp_write32(au + AU_ES_SIZE, (uint32_t)total);
    }
    c->aes_pos += total;
    /* Consumed frames need not be kept: slide the buffer down now and then. */
    if (c->aes_pos > 1u << 20) {
        memmove(c->aes, c->aes + c->aes_pos, c->aes_len - c->aes_pos);
        c->aes_len -= c->aes_pos;
        c->aes_pos = 0;
    }
    c->atrac_pts += MPEG_ATRAC_PTS_STEP;
    psp_ret(SCE_KERNEL_ERROR_OK);
}
/* sceMpegAvcDecode(mpeg, au, frameWidth, bufferAddr, initAddr)
 *
 * bufferAddr points at the address of the output buffer rather than being it,
 * and frameWidth is the stride in pixels, not the picture width -- 512 for a
 * 480-wide movie. initAddr is the "first call" flag the player clears itself.
 *
 * Written as 8888 because that is the format this game's display is in; a mode
 * selector exists on hardware (sceMpegAvcDecodeMode) and would belong here if a
 * game ever set something else. */
static void write_picture(mpeg_ctx *c, uint32_t dst, uint32_t stride) {
    if (!c->pic || !dst) return;
    const int w = c->pic_w, h = c->pic_h;
    const uint8_t *Y = c->pic;
    const uint8_t *U = Y + (size_t)w * h;
    const uint8_t *V = U + (size_t)(w / 2) * (h / 2);

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const int yy = Y[(size_t)y * w + x] - 16;
            const int u  = U[(size_t)(y / 2) * (w / 2) + x / 2] - 128;
            const int v  = V[(size_t)(y / 2) * (w / 2) + x / 2] - 128;
            const int yr = 76309 * yy;
            int r  = (yr + 104597 * v) >> 16;
            int g  = (yr -  53279 * v - 25674 * u) >> 16;
            int b  = (yr + 132201 * u) >> 16;
            r = r < 0 ? 0 : r > 255 ? 255 : r;
            g = g < 0 ? 0 : g > 255 ? 255 : g;
            b = b < 0 ? 0 : b > 255 ? 255 : b;
            psp_write32(dst + ((uint32_t)y * stride + (uint32_t)x) * 4,
                        0xFF000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r);
        }
    }
}

static void hle_AvcDecode(void) {
    mpeg_ctx *c = ctx_of(psp_arg(0));
    if (!mpeg_decoding() || !c) {
        no_decoder("sceMpegAvcDecode");
        psp_ret(SCE_MPEG_ERROR_INVALID_VALUE);
        return;
    }
    const uint32_t stride   = psp_arg(2);
    const uint32_t buf_ptr  = psp_arg(3);
    const uint32_t init_ptr = psp_arg(4);
    const uint32_t dst      = buf_ptr ? psp_read32(buf_ptr) : 0;

    if (mpeg_logging()) {
        static int said;
        if (said++ < 3)
            fprintf(stderr, "mpeg: AvcDecode frame %d -> dst 0x%08X stride %u ready=%d\n",
                    c->frames, dst, stride, c->pic_ready);
    }

    const int got = c->pic_ready;
    if (got) {
        write_picture(c, dst, stride ? stride : 512);
        c->pic_ready = 0;
    }

    /* The fifth argument is where the caller learns a picture was produced, and
     * it is the difference between the movie advancing and not.
     *
     * This game reads the word back and compares it against a constant 1 that
     * its prologue put in $s2, and only then waits on its display semaphore and
     * moves on -- anything else and it goes round the loop again. Writing zero
     * here therefore looked exactly like a successful decode that produced no
     * frame, so the player kept asking and the picture never reached the screen:
     *
     *     002750D0  ori $s2, $zero, 0x1        ; the constant
     *     00275218  lw  $a0, 8($sp)            ; the word written here
     *     0027521C  bne $a0, $s2, 0x00275268   ; no match -> round again
     *     00275224  lw  $a0, 180($s0)          ; match -> wait, then proceed
     *
     * It arrives in $t0 rather than on the stack, which psp_arg already knows. */
    if (init_ptr) psp_write32(init_ptr, got ? 1u : 0u);
    psp_ret(SCE_KERNEL_ERROR_OK);
}
/* sceMpegAtracDecode(mpeg, au, buffer, init): the frame the access unit
 * holds, decoded to 2048 stereo samples in the caller's buffer
 * (sceMpegQueryAtracEsSize's second answer, 8192 bytes). The decoder is
 * opened from the first frame's own header and flushed when `init` says a
 * new stream starts. A frame it refuses, or a build with no libavcodec, is
 * silence of the same length -- the movie keeps its clock either way. */
static void hle_AtracDecode(void) {
    mpeg_ctx *c = ctx_of(psp_arg(0));
    if (!mpeg_decoding() || !c) {
        no_decoder("sceMpegAtracDecode");
        psp_ret(SCE_MPEG_ERROR_INVALID_VALUE);
        return;
    }
    const uint32_t au = psp_arg(1), dst = psp_arg(2), init = psp_arg(3);
    int16_t pcm[2048 * 2];
    int n = -1;
    if (au) {
        const uint32_t esbuf = psp_read32(au + AU_ES_BUFFER);
        const uint32_t essz  = psp_read32(au + AU_ES_SIZE);
        uint8_t frame[MPEG_ATRAC_ES_SIZE];
        if (esbuf && essz >= AT3P_HDR && essz <= sizeof frame &&
            psp_mem_read_block(frame, esbuf, essz) == 0 &&
            frame[0] == AT3P_SYNC0 && frame[1] == AT3P_SYNC1) {
            const uint32_t payload = ((((uint32_t)frame[2] & 3u) << 8) | frame[3]) * 8u + 8u;
            /* Sample rate index and channel configuration, as the OMA/AA3
             * container spells them: rates 32000, 44100, 48000, 88200, 96000;
             * channel configuration 1 is mono and 2 stereo. */
            static const uint32_t rates[8] = { 32000, 44100, 48000, 88200, 96000, 44100, 44100, 44100 };
            const uint32_t rate = rates[(frame[2] >> 5) & 7];
            const uint32_t chan = ((frame[2] >> 2) & 7) == 1 ? 1u : 2u;
            if (payload + AT3P_HDR <= essz) {
                if (c->adec && c->adec_block != payload) { psp_at3_close((psp_at3_dec *)c->adec); c->adec = NULL; }
                if (!c->adec) {
                    c->adec = psp_at3_open(0x1000u, payload, chan, rate, NULL, 0);
                    c->adec_block = payload;
                    if (!c->adec) {
                        static int said;
                        if (!said++)
                            fprintf(stderr, "psprecomp: sceMpegAtracDecode has no decoder for the movie's "
                                            "audio; it plays silent.\n");
                    }
                } else if (init) psp_at3_flush((psp_at3_dec *)c->adec);
                n = psp_at3_decode((psp_at3_dec *)c->adec, frame + AT3P_HDR, payload, pcm, 2048);
            }
        }
    }
    if (n < 0) { memset(pcm, 0, sizeof pcm); n = 2048; }
    if (dst) {
        const uint32_t bytes = (uint32_t)n * 4u;
        if (psp_mem_write_block(dst, pcm, bytes) != 0)
            for (uint32_t i = 0; i < (uint32_t)n * 2u; i++) psp_write16(dst + i * 2u, (uint16_t)pcm[i]);
        if (bytes < MPEG_ATRAC_ES_OUT_SIZE) {
            void *p = psp_mem_ptr(dst + bytes, MPEG_ATRAC_ES_OUT_SIZE - bytes);
            if (p) memset(p, 0, MPEG_ATRAC_ES_OUT_SIZE - bytes);
        }
    }
    psp_ret(SCE_KERNEL_ERROR_OK);
}
static void hle_AvcDecodeStop(void) {
    /* Reports zero frames left, so a drain loop terminates. */
    if (psp_arg(3)) psp_write32(psp_arg(3), 0);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

void psp_mpeg_register(void) {
    psp_hle_register(0x682A619B, "sceMpeg", "sceMpegInit",            hle_MpegInit);
    psp_hle_register(0x874624D6, "sceMpeg", "sceMpegFinish",          hle_MpegFinish);
    psp_hle_register(0xC132E22F, "sceMpeg", "sceMpegQueryMemSize",    hle_QueryMemSize);
    psp_hle_register(0xD8C5F121, "sceMpeg", "sceMpegCreate",          hle_MpegCreate);
    psp_hle_register(0x606A4649, "sceMpeg", "sceMpegDelete",          hle_MpegDelete);

    psp_hle_register(0xD7A29F46, "sceMpeg", "sceMpegRingbufferQueryMemSize",
                     hle_RingbufferQueryMemSize);
    psp_hle_register(0x37295ED8, "sceMpeg", "sceMpegRingbufferConstruct",
                     hle_RingbufferConstruct);
    psp_hle_register(0x13407F13, "sceMpeg", "sceMpegRingbufferDestruct",
                     hle_RingbufferDestruct);
    psp_hle_register(0xB5F6DC87, "sceMpeg", "sceMpegRingbufferAvailableSize",
                     hle_RingbufferAvailableSize);
    psp_hle_register(0xB240A59E, "sceMpeg", "sceMpegRingbufferPut",   hle_RingbufferPut);

    psp_hle_register(0x21FF80E4, "sceMpeg", "sceMpegQueryStreamOffset",
                     hle_QueryStreamOffset);
    psp_hle_register(0x611E9E11, "sceMpeg", "sceMpegQueryStreamSize", hle_QueryStreamSize);

    psp_hle_register(0x42560F23, "sceMpeg", "sceMpegRegistStream",    hle_RegistStream);
    psp_hle_register(0x591A4AA2, "sceMpeg", "sceMpegUnRegistStream",  hle_UnRegistStream);
    psp_hle_register(0xA780CF7E, "sceMpeg", "sceMpegMallocAvcEsBuf",  hle_MallocAvcEsBuf);
    psp_hle_register(0xCEB870B1, "sceMpeg", "sceMpegFreeAvcEsBuf",    hle_FreeAvcEsBuf);
    psp_hle_register(0xF8DCB679, "sceMpeg", "sceMpegQueryAtracEsSize",
                     hle_QueryAtracEsSize);
    psp_hle_register(0x167AFD9E, "sceMpeg", "sceMpegInitAu",          hle_InitAu);

    psp_hle_register(0xFE246728, "sceMpeg", "sceMpegGetAvcAu",        hle_GetAvcAu);
    psp_hle_register(0xE1CE83A7, "sceMpeg", "sceMpegGetAtracAu",      hle_GetAtracAu);
    psp_hle_register(0x0E3C2E9D, "sceMpeg", "sceMpegAvcDecode",       hle_AvcDecode);
    psp_hle_register(0x800C44DF, "sceMpeg", "sceMpegAtracDecode",     hle_AtracDecode);
    psp_hle_register(0x740FCCD1, "sceMpeg", "sceMpegAvcDecodeStop",   hle_AvcDecodeStop);
}
