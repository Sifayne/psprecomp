/* psprecomp -- sceAtrac3plus: the streaming contract, with FFmpeg's libavcodec
 * behind it for the frames.
 *
 * How this got here. All 13 imports were unimplemented once, so each returned
 * zero and wrote nothing -- and for this library both halves are lies a game
 * acts on: zero from GetAtracID is a valid ID, and every later call answers
 * through a pointer it never writes. Refusing GetAtracID instead hung the game
 * in its player thread's lock / pump / unlock loop, which has no working
 * failure path because on hardware nothing here ever fails. What the game does
 * handle is a track that ends, so the stand-in that followed opened the stream
 * honestly and had the first decode report it over: ALLDATA_WAS_DECODED, zero
 * samples, the end flag, the drained remainFrame. Silence, but a run that
 * reached the menus and the mission. Findings items 27 and 33 have the detail;
 * that behaviour is still what a build without libavcodec does.
 *
 * With the decoder, a decode is a decode. The rules are read off pspautotests'
 * audio/atrac suite -- stream.expected above all, 10,000 lines of hardware
 * narrating its buffer -- and they come to this:
 *
 *  - A frame is block_align bytes and decodes to 2048 samples (1024 for
 *    plain ATRAC3), always as interleaved stereo PCM. The stream's first
 *    frame is entirely encoder warm-up and the second starts 368 samples in:
 *    decoding begins at decoded sample 2416 ("firstValidSample: 0970"), so
 *    the first decode a caller sees answers 1680 ("sceAtracGetNextSample:
 *    1680") and the total is the fact chunk's count exactly -- 247,501 for
 *    sample.at3, as 1680 + 120 x 2048 + 61, the 61 being the last decode
 *    (replay.expected, "samples: 0000003d, end: 00000001"). ATRAC3's warm-up
 *    is not in the corpus; 1024 + 69 is a guess.
 *  - Setting the data decodes that first frame at once, so the header and
 *    one frame are consumed before any DecodeData: GetRemainFrame on 848
 *    bytes of a 376-byte-frame file at data offset 96 answers 1 (two frames
 *    present, one gone) and 847 answers 0. Each DecodeData consumes one
 *    more. Consumed bytes are free to write.
 *  - Streaming is a ring over the file's bytes, and frames are contiguous in
 *    it: a lap ends at the last whole-frame boundary that fits, never mid
 *    frame. The first lap's frames start at the data offset; later laps' at
 *    zero. When the initial load ran past that boundary the partial frame's
 *    head moves to the ring's start -- stream.expected's first
 *    GetStreamDataInfo after a 0x4000 load of a file whose frames end at
 *    0x3FCC answers writePtr = buffer + 0x34, and 0x34 is exactly the
 *    partial. The free space reported is the contiguous run from the write
 *    position: to the lap's end while the decoder is on the same lap, or up
 *    to the decoder's next frame when it is a lap behind, and never beyond
 *    what the file has left.
 *  - remainFrame is the whole frames present that the decoder has not taken;
 *    -1 when the buffer holds the whole file, and once a streamed file has
 *    all been read, -2 without loop information and -3 with it. A decode past
 *    the end answers ALLDATA_WAS_DECODED and still writes its three outputs:
 *    zero samples, the end flag, the drained remainFrame -- the game reads
 *    the sample count before the return code.
 *  - Loops: SetLoopNum(-1) is forever, n is n more passes; refused on a file
 *    without a smpl chunk (80630021). The smpl chunk's points are 2048 above
 *    the output-sample positions hardware reports for them
 *    (getsoundsample.expected: a loop written at 0 reads back as -2048, and
 *    at 0x800 as 0). The jump lands on the frame holding the start and skips
 *    into it, after re-decoding the frame before for the overlap. Where
 *    hardware cuts is measured only to the frame.
 *  - Seeking (ResetPlayPosition) is the same landing at any sample, for a
 *    buffer holding the whole file. Streaming seeks want the reload numbers
 *    of GetBufferInfoForResetting, which are not modelled; audio/atrac's
 *    resetting and reset2 hold them.
 *
 * This game holds each track whole in one buffer, sets an infinite loop, and
 * decodes -- the simple path. The streaming ring exists for the oracle and
 * for the day a track does not fit.
 *
 * Every error code is PSPSDK's pspatrac3.h (BSD), by name:
 * https://github.com/pspdev/pspsdk/blob/master/src/atrac3/pspatrac3.h
 * BUFFER_IS_EMPTY (0x80630023) is not in that header; it sits between
 * NO_SECOND_BUF (0x22, which stream.expected shows) and ALLDATA_WAS_DECODED
 * (0x24), and nothing here has measured it. */

#include "psprecomp/hle.h"
#include "psprecomp/cpu.h"
#include "psprecomp/mem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if PSPRECOMP_HAVE_FFMPEG
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#endif

#define PSP_ATRAC_ERROR_API_FAIL              0x80630002u
#define PSP_ATRAC_ERROR_NO_ATRACID            0x80630003u
#define PSP_ATRAC_ERROR_BAD_CODECTYPE         0x80630004u
#define PSP_ATRAC_ERROR_BAD_ATRACID           0x80630005u
#define PSP_ATRAC_ERROR_UNKNOWN_FORMAT        0x80630006u
#define PSP_ATRAC_ERROR_UNMATCH_FORMAT        0x80630007u
#define PSP_ATRAC_ERROR_UNSET_DATA            0x80630010u
#define PSP_ATRAC_ERROR_READSIZE_IS_TOO_SMALL 0x80630011u
#define PSP_ATRAC_ERROR_ALL_DATA_LOADED       0x80630009u
#define PSP_ATRAC_ERROR_ADD_DATA_IS_TOO_BIG   0x80630018u
#define PSP_ATRAC_ERROR_UNSET_PARAM           0x80630021u
#define PSP_ATRAC_ERROR_BUFFER_IS_EMPTY       0x80630023u
#define PSP_ATRAC_ERROR_ALLDATA_WAS_DECODED   0x80630024u

#define PSP_ATRAC_AT3PLUS 0x1000u
#define PSP_ATRAC_AT3     0x1001u

#define AT3P_FRAME_SAMPLES 2048u
#define AT3_FRAME_SAMPLES  1024u
#define AT3P_DELAY         368u
#define AT3_DELAY          69u     /* unmeasured; see the header */
#define ATRAC_LOOP_BIAS    2048u   /* smpl points sit this far above output positions */
#define ATRAC_OUT_CHANNELS 2

/* Hardware hands out IDs 0 and 1 for ATRAC3+ and 2 and 3 for ATRAC3 at its
 * default split, and refuses a third of either -- audio/atrac/ids.expected,
 * "Initial ids: ATRAC3+: 0 1, ATRAC3: 2 3". sceAtracReinit can change the
 * split; the game does not import it and it is not registered. */
#define ATRAC_IDS       4
#define ATRAC_PER_CODEC 2

typedef struct {
    int      used;
    uint32_t codec;

    /* The file, from its header. */
    int      has_data;
    uint32_t buf, buf_size;    /* the guest buffer and its capacity */
    uint32_t read_size;        /* bytes of the file present (in-memory mode) */
    uint32_t file_size;        /* RIFF size + 8 */
    uint32_t data_off;         /* file offset of the data chunk's body */
    uint32_t data_size;
    uint32_t block_align;      /* bytes per frame */
    uint32_t channels;
    uint32_t sample_rate;
    uint32_t fact_samples;     /* the fact chunk's first word: the output total */
    int      has_loop;
    int32_t  loop_start, loop_end;   /* output-sample positions, smpl less the bias */
    int      loop_num;
    uint32_t internal_error;
    uint8_t  extradata[32];
    uint32_t extradata_size;

    /* Playback. */
    int      in_memory;        /* the buffer can hold the whole file: frames by file offset */
    uint32_t frame_samples, delay;
    uint32_t n_frames;         /* frames in the file */
    uint32_t total_out;        /* output samples in one pass without a loop */
    uint32_t ls_frame, le_frame, ls_skip;   /* the loop, in frames */
    uint32_t next;             /* file frame index of the next frame to decode */
    int      loops_left;       /* the decoder's loop counter: -1 forever */
    uint32_t pos;              /* output-sample position of the next sample */
    uint32_t skip;             /* samples to drop from the front of the next frame */
    int      finished;
    uint8_t *frame;            /* scratch for one frame's bytes */

    /* The streaming ring (in_memory == 0). Byte positions in the play-order
     * stream are monotonic sequence offsets; the ring holds a window of it. */
    uint64_t write_seq, cons_seq;      /* written so far; consumed so far */
    uint64_t lap_base, dec_base;       /* sequence offset at ring 0, writer's and decoder's lap */
    uint32_t wrap, dec_wrap;           /* where each lap ends */
    uint32_t write_ring;               /* the writer's ring offset */
    uint32_t wfile;                    /* file offset the writer reads next */
    int      wloops_left;              /* the writer's loop counter */

    void    *dec;
} atrac_ctx;

static atrac_ctx g_id[ATRAC_IDS];

/* ---- the decoder ---------------------------------------------------------- */

#if PSPRECOMP_HAVE_FFMPEG
typedef struct { AVCodecContext *cc; AVPacket *pkt; AVFrame *fr; } atrac_dec;

static void dec_close(atrac_ctx *c) {
    atrac_dec *d = (atrac_dec *)c->dec;
    if (!d) return;
    av_frame_free(&d->fr);
    av_packet_free(&d->pkt);
    avcodec_free_context(&d->cc);
    free(d);
    c->dec = NULL;
}

/* libavcodec's contract, read from atrac3plusdec.c and atrac3.c: ATRAC3+
 * wants block_align and the channel count and no extradata; ATRAC3 wants the
 * 14 bytes that follow the WAVE fmt chunk's cbSize as extradata, and a
 * block_align of 96, 152 or 192 per channel. Both answer planar float. */
static int dec_open(atrac_ctx *c) {
    dec_close(c);
    const AVCodec *codec = avcodec_find_decoder(
        c->codec == PSP_ATRAC_AT3PLUS ? AV_CODEC_ID_ATRAC3P : AV_CODEC_ID_ATRAC3);
    if (!codec) return -1;
    atrac_dec *d = (atrac_dec *)calloc(1, sizeof *d);
    if (!d) return -1;
    d->cc = avcodec_alloc_context3(codec);
    d->pkt = av_packet_alloc();
    d->fr  = av_frame_alloc();
    if (!d->cc || !d->pkt || !d->fr) { c->dec = d; dec_close(c); return -1; }
    d->cc->block_align = (int)c->block_align;
    d->cc->sample_rate = (int)c->sample_rate;
    av_channel_layout_default(&d->cc->ch_layout, (int)c->channels);
    if (c->extradata_size) {
        d->cc->extradata = (uint8_t *)av_mallocz(c->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (d->cc->extradata) {
            memcpy(d->cc->extradata, c->extradata, c->extradata_size);
            d->cc->extradata_size = (int)c->extradata_size;
        }
    }
    if (avcodec_open2(d->cc, codec, NULL) < 0) { c->dec = d; dec_close(c); return -1; }
    c->dec = d;
    return 0;
}

static void dec_flush(atrac_ctx *c) {
    atrac_dec *d = (atrac_dec *)c->dec;
    if (d) avcodec_flush_buffers(d->cc);
}

/* One frame in, up to `cap` interleaved stereo samples out. A mono stream
 * plays on both sides, which is what hardware's fixed two output channels
 * do with it. Returns the sample count, or -1. */
static int dec_frame(atrac_ctx *c, const uint8_t *in, int16_t *out, uint32_t cap) {
    atrac_dec *d = (atrac_dec *)c->dec;
    if (av_new_packet(d->pkt, (int)c->block_align) < 0) return -1;
    memcpy(d->pkt->data, in, c->block_align);
    int rc = avcodec_send_packet(d->cc, d->pkt);
    av_packet_unref(d->pkt);
    if (rc < 0) return -1;
    rc = avcodec_receive_frame(d->cc, d->fr);
    if (rc < 0) return -1;
    uint32_t n = (uint32_t)d->fr->nb_samples;
    if (n > cap) n = cap;
    const int nch = d->fr->ch_layout.nb_channels;
    const float *l = (const float *)d->fr->data[0];
    const float *r = nch > 1 ? (const float *)d->fr->data[1] : l;
    for (uint32_t i = 0; i < n; i++) {
        float fl = l[i] * 32768.0f, fr = r[i] * 32768.0f;
        if (fl > 32767.0f) fl = 32767.0f; else if (fl < -32768.0f) fl = -32768.0f;
        if (fr > 32767.0f) fr = 32767.0f; else if (fr < -32768.0f) fr = -32768.0f;
        out[i * 2]     = (int16_t)fl;
        out[i * 2 + 1] = (int16_t)fr;
    }
    av_frame_unref(d->fr);
    return (int)n;
}
#else
static void dec_close(atrac_ctx *c) { (void)c; }
static int  dec_open(atrac_ctx *c) { (void)c; return -1; }
static void dec_flush(atrac_ctx *c) { (void)c; }
static int  dec_frame(atrac_ctx *c, const uint8_t *in, int16_t *out, uint32_t cap) {
    (void)c; (void)in; (void)out; (void)cap; return -1;
}
#endif

void psp_atrac_init(void) {
    for (int i = 0; i < ATRAC_IDS; i++) { dec_close(&g_id[i]); free(g_id[i].frame); }
    memset(g_id, 0, sizeof g_id);
}

/* Said once. A title screen restarts its music every time it comes back. */
static void no_decoder(void) {
    static int said;
    if (!said++)
        fprintf(stderr,
#if PSPRECOMP_HAVE_FFMPEG
            "psprecomp: sceAtrac3plus could not open a decoder for this stream.\n"
#else
            "psprecomp: sceAtrac3plus has no decoder -- this build has no\n"
            "  libavcodec.\n"
#endif
            "  The stream opens and its header is read; the first\n"
            "  sceAtracDecodeData then reports the stream fully decoded, so the\n"
            "  game runs the track out at once. Music is silent; everything\n"
            "  else continues.\n");
}

static atrac_ctx *ctx_arg(void) {
    const uint32_t id = psp_arg(0);
    return (id < ATRAC_IDS && g_id[id].used) ? &g_id[id] : NULL;
}

/* PSPRECOMP_ATRAC_LOG=1 narrates every call with its arguments: which of the
 * thirteen the game makes, in what order, with what buffer. */
static int atrac_log_on(void) {
    static int on = -1;
    if (on < 0) { const char *v = getenv("PSPRECOMP_ATRAC_LOG"); on = (v && *v && *v != '0'); }
    return on;
}
#define ATRAC_LOG(name) do { if (atrac_log_on()) \
    fprintf(stderr, "atrac: %-28s a0=%08X a1=%08X a2=%08X a3=%08X\n", name, \
            psp_arg(0), psp_arg(1), psp_arg(2), psp_arg(3)); } while (0)

static void put32(uint32_t addr, uint32_t v) { if (addr) psp_write32(addr, v); }

/* Parse the RIFF/WAVE header the guest handed over, reading at most `avail`
 * bytes of it. Returns 0 or the error hardware gives for the same input:
 *
 *   zero bytes            -> READSIZE_IS_TOO_SMALL   (setdata: "Zero length")
 *   not RIFF/WAVE         -> UNKNOWN_FORMAT          (setdata: "Zeroed data")
 *   codec != the ID's     -> UNMATCH_FORMAT          (setdata: ATRAC3 ID, AT3+ file)
 *
 * A header that runs past `avail` is reported as too small, which is the
 * nearest of the three; hardware's exact answer for a truncated header is not
 * in the corpus. The layout is plain RIFF: `fmt ` (tag 0x0270 is ATRAC3,
 * 0xFFFE is WAVE_FORMAT_EXTENSIBLE and the sub-format GUID names ATRAC3+),
 * `fact` (total samples), an optional `smpl` (the loop), then `data`. The
 * game's own tracks carry all four. */
static uint32_t parse_header(atrac_ctx *c, uint32_t addr, uint32_t avail) {
    if (avail == 0) return PSP_ATRAC_ERROR_READSIZE_IS_TOO_SMALL;
    if (avail < 12) return PSP_ATRAC_ERROR_READSIZE_IS_TOO_SMALL;
    if (psp_read32(addr) != 0x46464952u /* RIFF */ ||
        psp_read32(addr + 8) != 0x45564157u /* WAVE */)
        return PSP_ATRAC_ERROR_UNKNOWN_FORMAT;

    c->file_size = psp_read32(addr + 4) + 8;
    c->data_off = c->data_size = c->block_align = c->channels = 0;
    c->sample_rate = 44100;
    c->fact_samples = 0;
    c->has_loop = 0;
    c->extradata_size = 0;
    uint32_t codec = 0;
    uint32_t off = 12;
    while (off + 8 <= avail) {
        const uint32_t id = psp_read32(addr + off);
        const uint32_t sz = psp_read32(addr + off + 4);
        const uint32_t body = off + 8;
        if (id == 0x61746164u /* data */) {
            c->data_off  = body;
            c->data_size = sz;
            break;
        }
        if (body + sz > avail) return PSP_ATRAC_ERROR_READSIZE_IS_TOO_SMALL;
        if (id == 0x20746D66u /* fmt  */ && sz >= 16) {
            const uint32_t tag = psp_read16(addr + body);
            c->channels    = psp_read16(addr + body + 2);
            c->sample_rate = psp_read32(addr + body + 4);
            c->block_align = psp_read16(addr + body + 12);
            if (tag == 0x0270) {
                codec = PSP_ATRAC_AT3;
                /* cbSize at 16, then the 14 bytes libavcodec's ATRAC3 wants. */
                if (sz >= 32) {
                    for (uint32_t i = 0; i < 14; i++) c->extradata[i] = psp_read8(addr + body + 18 + i);
                    c->extradata_size = 14;
                }
            } else if (tag == 0xFFFE && sz >= 40 &&
                       psp_read32(addr + body + 24) == 0xE923AABFu) codec = PSP_ATRAC_AT3PLUS;
        } else if (id == 0x74636166u /* fact */ && sz >= 4) {
            c->fact_samples = psp_read32(addr + body);
        } else if (id == 0x6C706D73u /* smpl */ && sz >= 60) {
            /* manufacturer, product, period, unity note, pitch fraction, SMPTE
             * format, SMPTE offset, loop count, sampler data; then per loop:
             * id, type, start, end, fraction, play count. */
            if (psp_read32(addr + body + 28) >= 1) {
                c->has_loop   = 1;
                c->loop_start = (int32_t)(psp_read32(addr + body + 44) - ATRAC_LOOP_BIAS);
                c->loop_end   = (int32_t)(psp_read32(addr + body + 48) - ATRAC_LOOP_BIAS);
            }
        }
        off = body + sz + (sz & 1);
    }
    if (!c->data_off || !c->block_align || !codec || !c->channels) return PSP_ATRAC_ERROR_UNKNOWN_FORMAT;
    if (codec != c->codec) return PSP_ATRAC_ERROR_UNMATCH_FORMAT;
    return 0;
}

/* ---- playback state --------------------------------------------------------- */

static uint32_t frame_off(const atrac_ctx *c, uint32_t frame) { return c->data_off + frame * c->block_align; }

/* The last whole-frame boundary of a lap, as a ring offset. */
static uint32_t lap_wrap(const atrac_ctx *c, uint32_t first) {
    if (c->buf_size <= first) return first;
    return first + ((c->buf_size - first) / c->block_align) * c->block_align;
}

/* The frame after `f` in play order, applying the loop with `*loops` left. */
static uint32_t advance_frame(const atrac_ctx *c, uint32_t f, int *loops, int *jumped) {
    *jumped = 0;
    if (c->has_loop && *loops != 0 && f == c->le_frame) {
        if (*loops > 0) (*loops)--;
        *jumped = 1;
        return c->ls_frame;
    }
    return f + 1;
}

/* Is the frame the decoder wants next present in the buffer? */
static int frame_present(const atrac_ctx *c) {
    if (c->in_memory) {
        if (c->next >= c->n_frames) return 0;
        return frame_off(c, c->next) + c->block_align <= c->read_size;
    }
    return c->write_seq - c->cons_seq >= c->block_align;
}

/* Take the next frame's bytes out of the guest buffer, consuming it: from
 * then on its bytes are free to write, as hardware treats them. Returns 1 if
 * a frame was taken. */
static int take_frame(atrac_ctx *c) {
    if (!frame_present(c)) return 0;
    uint32_t addr;
    if (c->in_memory) {
        addr = c->buf + frame_off(c, c->next);
    } else {
        uint64_t r = c->cons_seq - c->dec_base;
        if (r >= c->dec_wrap) {
            c->dec_base += c->dec_wrap;
            c->dec_wrap = lap_wrap(c, 0);
            r = c->cons_seq - c->dec_base;
        }
        addr = c->buf + (uint32_t)r;
        c->cons_seq += c->block_align;
    }
    if (psp_mem_read_block(c->frame, addr, c->block_align) != 0) return 0;
    int jumped;
    c->next = advance_frame(c, c->next, &c->loops_left, &jumped);
    return 1;
}

/* Decode one frame into `pcm` (capacity AT3P_FRAME_SAMPLES stereo). A frame
 * the decoder refuses is silence for its length rather than a stop. */
static int decode_one(atrac_ctx *c, int16_t *pcm) {
    int n = c->dec ? dec_frame(c, c->frame, pcm, AT3P_FRAME_SAMPLES) : -1;
    if (n < 0) {
        n = (int)c->frame_samples;
        memset(pcm, 0, sizeof(int16_t) * ATRAC_OUT_CHANNELS * (size_t)n);
        if (c->dec) c->internal_error = PSP_ATRAC_ERROR_API_FAIL;
    }
    return n;
}

/* Land the decoder at output position `pos`: the frame holding it, the
 * samples to skip into it, and -- for a buffer holding the whole file -- the
 * frame before decoded and discarded so the overlap-add is right. The
 * stream's own start is the landing at 0: frame 0 whole, then 368 into
 * frame 1. Streaming cannot re-read the frame before, so it does without. */
static void seek_to(atrac_ctx *c, uint32_t pos) {
    const uint32_t d = pos + c->frame_samples + c->delay;
    const uint32_t k = d / c->frame_samples;
    dec_flush(c);
    c->pos  = pos;
    c->skip = d % c->frame_samples;
    c->finished = 0;
    if (c->in_memory) {
        c->next = k ? k - 1 : 0;
        if (k && take_frame(c)) { int16_t pcm[AT3P_FRAME_SAMPLES * ATRAC_OUT_CHANNELS]; (void)decode_one(c, pcm); }
        c->next = k;
    } else {
        /* The ring is at the stream's start: frame 0 is what is there. */
        if (take_frame(c)) { int16_t pcm[AT3P_FRAME_SAMPLES * ATRAC_OUT_CHANNELS]; (void)decode_one(c, pcm); }
    }
}

/* Has the writer nothing left to supply? */
static int writer_done(const atrac_ctx *c) {
    if (c->in_memory) return c->read_size >= c->file_size;
    return c->wfile >= c->file_size;
}

/* The remainFrame word. */
static int32_t remain_value(const atrac_ctx *c) {
    if (c->in_memory && c->read_size >= c->file_size) return -1;
    if (writer_done(c)) return c->buf_size >= c->file_size ? -1 : (c->has_loop ? -3 : -2);
    if (c->in_memory) {
        const uint32_t nxt = frame_off(c, c->next);
        return c->read_size > nxt ? (int32_t)((c->read_size - nxt) / c->block_align) : 0;
    }
    return (int32_t)((c->write_seq - c->cons_seq) / c->block_align);
}

static void set_playback(atrac_ctx *c) {
    c->frame_samples = c->codec == PSP_ATRAC_AT3PLUS ? AT3P_FRAME_SAMPLES : AT3_FRAME_SAMPLES;
    c->delay         = c->codec == PSP_ATRAC_AT3PLUS ? AT3P_DELAY : AT3_DELAY;
    c->n_frames      = c->data_size / c->block_align;
    c->total_out     = c->fact_samples ? c->fact_samples
                     : (c->n_frames > 1 ? (c->n_frames - 1) * c->frame_samples - c->delay : 0);
    if (c->has_loop) {
        const uint32_t s = (uint32_t)(c->loop_start > 0 ? c->loop_start : 0) + c->frame_samples + c->delay;
        const uint32_t e = (uint32_t)(c->loop_end   > 0 ? c->loop_end   : 0) + c->frame_samples + c->delay;
        c->ls_frame = s / c->frame_samples;
        c->ls_skip  = s % c->frame_samples;
        c->le_frame = e ? (e - 1) / c->frame_samples : 0;
        if (c->n_frames && c->le_frame >= c->n_frames) c->le_frame = c->n_frames - 1;
        if (c->ls_frame > c->le_frame) c->has_loop = 0;
    }
    c->next = 0;
    c->loops_left = c->loop_num;
    c->pos = 0;
    c->skip = 0;
    c->finished = 0;
    free(c->frame);
    c->frame = (uint8_t *)malloc(c->block_align);
}

/* Data set: the buffer holds `read` of the file's bytes in a buffer of `size`. */
static uint32_t set_data(atrac_ctx *c, uint32_t buf, uint32_t read, uint32_t size) {
    const uint32_t err = parse_header(c, buf, read);
    if (err) return err;
    c->has_data  = 1;
    c->buf       = buf;
    c->buf_size  = size;
    c->loop_num  = 0;
    c->internal_error = 0;
    c->in_memory = size >= c->file_size;
    c->read_size = read > c->file_size ? c->file_size : read;
    set_playback(c);
    if (!c->frame) return PSP_ATRAC_ERROR_API_FAIL;

    if (!c->in_memory) {
        /* The first lap: frames from the data offset, the header consumed. */
        c->lap_base   = 0;
        c->wrap       = lap_wrap(c, c->data_off);
        c->write_seq  = read;
        c->write_ring = read;
        c->wfile      = read;
        c->wloops_left = c->loop_num;
        c->dec_base   = 0;
        c->dec_wrap   = c->wrap;
        c->cons_seq   = c->data_off;
        if (c->write_ring > c->wrap) {
            /* The load ran past the lap: the partial frame's head moves to the
             * ring's start, where the next lap's frames begin. */
            const uint32_t partial = c->write_ring - c->wrap;
            uint8_t tmp[4096];
            if (partial <= sizeof tmp && psp_mem_read_block(tmp, buf + c->wrap, partial) == 0)
                psp_mem_write_block(buf, tmp, partial);
            c->lap_base   = c->wrap;
            c->write_ring = partial;
            c->wrap       = lap_wrap(c, 0);
        }
    }

    if (dec_open(c) != 0) no_decoder();
    seek_to(c, 0);
    return 0;
}

/* ---- the calls ------------------------------------------------------------- */

/* Hands out the lowest free ID of the requested codec from its own pair --
 * 0 and 1 for ATRAC3+, 2 and 3 for ATRAC3, as ids.expected has it. A codec
 * that is neither is refused by name; that code is the header's, not
 * measured. */
static void hle_GetAtracID(void) {
    ATRAC_LOG("GetAtracID");
    const uint32_t codec = psp_arg(0);
    if (codec != PSP_ATRAC_AT3PLUS && codec != PSP_ATRAC_AT3) {
        psp_ret(PSP_ATRAC_ERROR_BAD_CODECTYPE);
        return;
    }
    const int first = codec == PSP_ATRAC_AT3PLUS ? 0 : ATRAC_PER_CODEC;
    for (int i = first; i < first + ATRAC_PER_CODEC; i++) {
        if (g_id[i].used) continue;
        dec_close(&g_id[i]);
        free(g_id[i].frame);
        memset(&g_id[i], 0, sizeof g_id[i]);
        g_id[i].used  = 1;
        g_id[i].codec = codec;
        psp_ret((uint32_t)i);
        return;
    }
    psp_ret(PSP_ATRAC_ERROR_NO_ATRACID);
}

static void hle_ReleaseAtracID(void) {
    ATRAC_LOG("ReleaseAtracID");
    atrac_ctx *c = ctx_arg();
    if (!c) { psp_ret(PSP_ATRAC_ERROR_BAD_ATRACID); return; }
    dec_close(c);
    free(c->frame); c->frame = NULL;
    c->used = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* (id, buf, bytes) -- the whole file is in the buffer. */
static void hle_SetData(void) {
    ATRAC_LOG("SetData");
    atrac_ctx *c = ctx_arg();
    if (!c) { psp_ret(PSP_ATRAC_ERROR_BAD_ATRACID); return; }
    psp_ret(set_data(c, psp_arg(1), psp_arg(2), psp_arg(2)));
}

/* (id, buf, readBytes, bufBytes) -- the first readBytes of the file are in a
 * buffer of bufBytes, and the rest is streamed in through GetStreamDataInfo
 * and AddStreamData. */
static void hle_SetHalfwayBuffer(void) {
    ATRAC_LOG("SetHalfwayBuffer");
    atrac_ctx *c = ctx_arg();
    if (!c) { psp_ret(PSP_ATRAC_ERROR_BAD_ATRACID); return; }
    psp_ret(set_data(c, psp_arg(1), psp_arg(2), psp_arg(3)));
}

/* (id, int *remainFrame). Measured in audio/atrac/getremainframe.expected: a
 * buffer holding the whole file answers -1; a half-filled one answers the
 * whole frames present that the decoder has not taken -- 472, 473 and 847
 * bytes of a 376-byte-frame file with its data at 96 give 0, 848 gives 1,
 * 1975 gives 3, 1976 gives 4, and stream.expected's 0x800 of the same file
 * gives 4. An ID with no data set answers UNSET_DATA and writes nothing; a
 * bad ID answers BAD_ATRACID and writes nothing (remainFrame pre-seeded to
 * -1337 in the test comes back -1337 in both cases). */
static void hle_GetRemainFrame(void) {
    ATRAC_LOG("GetRemainFrame");
    atrac_ctx *c = ctx_arg();
    if (!c) { psp_ret(PSP_ATRAC_ERROR_BAD_ATRACID); return; }
    if (!c->has_data) { psp_ret(PSP_ATRAC_ERROR_UNSET_DATA); return; }
    put32(psp_arg(1), (uint32_t)remain_value(c));
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* The decoder's lap, found from where its next frame sits. Read-only: prime()
 * is what actually moves it. */
static void dec_lap(const atrac_ctx *c, uint64_t *base, uint32_t *wrap) {
    *base = c->dec_base; *wrap = c->dec_wrap;
    if (c->cons_seq - *base >= *wrap) { *base += *wrap; *wrap = lap_wrap(c, 0); }
}

/* Where the writer's next chunk goes, how many bytes, and from which file
 * offset. The header comment has the ring's rules. */
static void stream_info(const atrac_ctx *c, uint32_t *ptr, uint32_t *bytes, uint32_t *fpos) {
    if (c->in_memory) {
        if (c->read_size >= c->file_size) { *ptr = c->buf + c->file_size; *bytes = 0; *fpos = c->file_size; return; }
        uint32_t end = lap_wrap(c, c->data_off);
        if (end > c->file_size) end = c->file_size;
        *ptr = c->buf + c->read_size;
        *bytes = end > c->read_size ? end - c->read_size : 0;
        *fpos = c->read_size;
        return;
    }
    if (writer_done(c)) { *ptr = c->buf; *bytes = 0; *fpos = 0; return; }
    uint64_t dbase; uint32_t dwrap;
    dec_lap(c, &dbase, &dwrap);
    uint32_t free_end;
    if (dbase == c->lap_base) free_end = c->wrap;
    else {
        const uint64_t cons_ring = c->cons_seq - dbase;
        free_end = cons_ring > c->write_ring ? (uint32_t)cons_ring : c->write_ring;
        if (free_end > c->wrap) free_end = c->wrap;
    }
    uint32_t room = free_end > c->write_ring ? free_end - c->write_ring : 0;
    const int looping = c->has_loop && c->wloops_left != 0;
    const uint32_t stop = looping ? frame_off(c, c->le_frame + 1) : c->file_size;
    if (stop > c->wfile && room > stop - c->wfile) room = stop - c->wfile;
    *ptr = c->buf + c->write_ring;
    *bytes = room;
    *fpos = c->wfile;
}

/* (id, u8 **writePtr, u32 *writableBytes, u32 *readOffset). */
static void hle_GetStreamDataInfo(void) {
    ATRAC_LOG("GetStreamDataInfo");
    atrac_ctx *c = ctx_arg();
    if (!c) { psp_ret(PSP_ATRAC_ERROR_BAD_ATRACID); return; }
    if (!c->has_data) { psp_ret(PSP_ATRAC_ERROR_UNSET_DATA); return; }
    uint32_t ptr, bytes, fpos;
    stream_info(c, &ptr, &bytes, &fpos);
    put32(psp_arg(1), ptr);
    put32(psp_arg(2), bytes);
    put32(psp_arg(3), fpos);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* (id, bytes) -- the caller wrote this much where GetStreamDataInfo said. */
static void hle_AddStreamData(void) {
    ATRAC_LOG("AddStreamData");
    atrac_ctx *c = ctx_arg();
    if (!c) { psp_ret(PSP_ATRAC_ERROR_BAD_ATRACID); return; }
    if (!c->has_data) { psp_ret(PSP_ATRAC_ERROR_UNSET_DATA); return; }
    const uint32_t bytes = psp_arg(1);
    /* addstreamdata.expected: with the whole file in a buffer that holds it,
     * any add -- even of zero bytes -- answers 80630009. A streamed file that
     * has all been read is different: replay.expected keeps adding zero
     * bytes to one and reads OK each time. */
    if (c->in_memory && writer_done(c)) { psp_ret(PSP_ATRAC_ERROR_ALL_DATA_LOADED); return; }
    uint32_t ptr, room, fpos;
    stream_info(c, &ptr, &room, &fpos);
    if (bytes > room) { psp_ret(PSP_ATRAC_ERROR_ADD_DATA_IS_TOO_BIG); return; }
    if (c->in_memory) {
        c->read_size += bytes;
    } else {
        c->write_seq  += bytes;
        c->write_ring += bytes;
        c->wfile      += bytes;
        if (c->has_loop && c->wloops_left != 0 && c->wfile >= frame_off(c, c->le_frame + 1)) {
            c->wfile = frame_off(c, c->ls_frame);
            if (c->wloops_left > 0) c->wloops_left--;
        }
        if (c->write_ring >= c->wrap) {
            c->lap_base  += c->wrap;
            c->write_ring -= c->wrap;
            c->wrap       = lap_wrap(c, 0);
        }
    }
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* (id, u16 *pcm, int *samples, int *end, int *remainFrame). */
static void hle_DecodeData(void) {
    ATRAC_LOG("DecodeData");
    atrac_ctx *c = ctx_arg();
    if (!c) { psp_ret(PSP_ATRAC_ERROR_BAD_ATRACID); return; }
    if (!c->has_data) { psp_ret(PSP_ATRAC_ERROR_UNSET_DATA); return; }
    const uint32_t out_addr = psp_arg(1);

    if (!c->dec) {
        /* No decoder: the stream is over at once, with the three outputs
         * hardware writes for it. See the header. */
        no_decoder();
        c->finished = 1;
    }
    if (c->finished) {
        put32(psp_arg(2), 0);
        put32(psp_arg(3), 1);
        put32(psp_arg(4), (uint32_t)remain_value(c));
        psp_ret(PSP_ATRAC_ERROR_ALLDATA_WAS_DECODED);
        return;
    }
    /* The frame about to be decoded; its successor -- and whether the loop
     * jumps after it -- is what take_frame leaves in `next`. */
    const uint32_t this_frame = c->next;
    if (!take_frame(c)) {
        c->internal_error = PSP_ATRAC_ERROR_BUFFER_IS_EMPTY;
        psp_ret(PSP_ATRAC_ERROR_BUFFER_IS_EMPTY);
        return;
    }
    const int jumped = c->has_loop && this_frame == c->le_frame && c->next == c->ls_frame && c->next <= this_frame;

    int16_t pcm[AT3P_FRAME_SAMPLES * ATRAC_OUT_CHANNELS];
    int n = decode_one(c, pcm);
    const int16_t *src = pcm;
    if (c->skip) {
        const uint32_t drop = c->skip < (uint32_t)n ? c->skip : (uint32_t)n;
        src += drop * ATRAC_OUT_CHANNELS;
        n -= (int)drop;
        c->skip -= drop;
    }
    if (jumped) {
        const uint32_t le = (uint32_t)(c->loop_end > 0 ? c->loop_end : 0);
        if (c->pos + (uint32_t)n > le) n = (int)(le > c->pos ? le - c->pos : 0);
    } else if (c->pos + (uint32_t)n >= c->total_out) {
        n = (int)(c->total_out > c->pos ? c->total_out - c->pos : 0);
        c->finished = 1;
    }
    if (out_addr && n > 0) {
        const uint32_t bytes = (uint32_t)n * ATRAC_OUT_CHANNELS * sizeof(int16_t);
        if (psp_mem_write_block(out_addr, src, bytes) != 0)
            for (uint32_t i = 0; i < (uint32_t)n * ATRAC_OUT_CHANNELS; i++)
                psp_write16(out_addr + i * 2u, (uint16_t)src[i]);
    }
    c->pos += (uint32_t)n;
    if (jumped) {
        /* Land on the loop start. In memory that re-decodes the frame before
         * it for the overlap; the loop counter was already charged. */
        const int loops = c->loops_left;
        seek_to(c, (uint32_t)(c->loop_start > 0 ? c->loop_start : 0));
        c->loops_left = loops;
    }

    put32(psp_arg(2), (uint32_t)n);
    put32(psp_arg(3), c->finished ? 1u : 0u);
    put32(psp_arg(4), (uint32_t)remain_value(c));
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* (id, int *error). The codec's own last error -- stream.expected shows a
 * codec-internal 0x20B after a failed decode. What this game does with it is
 * print it and stop. */
static void hle_GetInternalErrorInfo(void) {
    ATRAC_LOG("GetInternalErrorInfo");
    atrac_ctx *c = ctx_arg();
    if (!c) { psp_ret(PSP_ATRAC_ERROR_BAD_ATRACID); return; }
    put32(psp_arg(1), c->internal_error);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* (id, loops). Refused on a file with no loop information -- atractest.expected
 * and stream.expected both answer 80630021 for sample.at3, which has no smpl
 * chunk. The game's tracks have one, spanning the whole track, and it asks
 * for -1: forever. */
static void hle_SetLoopNum(void) {
    ATRAC_LOG("SetLoopNum");
    atrac_ctx *c = ctx_arg();
    if (!c) { psp_ret(PSP_ATRAC_ERROR_BAD_ATRACID); return; }
    if (!c->has_data) { psp_ret(PSP_ATRAC_ERROR_UNSET_DATA); return; }
    if (!c->has_loop) { psp_ret(PSP_ATRAC_ERROR_UNSET_PARAM); return; }
    c->loop_num = (int)psp_arg(1);
    c->loops_left = c->loop_num;
    c->wloops_left = c->loop_num;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* (id, int *loopNum, int *loopStatus). stream.expected prints (0, 0) for a
 * file without loop information and (0, 1) for one with it. */
static void hle_GetLoopStatus(void) {
    ATRAC_LOG("GetLoopStatus");
    atrac_ctx *c = ctx_arg();
    if (!c) { psp_ret(PSP_ATRAC_ERROR_BAD_ATRACID); return; }
    if (!c->has_data) { psp_ret(PSP_ATRAC_ERROR_UNSET_DATA); return; }
    put32(psp_arg(1), (uint32_t)c->loop_num);
    put32(psp_arg(2), (uint32_t)c->has_loop);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* Seeking. ResetPlayPosition(id, sample, bytesWrittenFirst, bytesWrittenSecond)
 * lands the decoder at `sample`; for a buffer holding the whole file that is
 * the same landing the stream's start is (decode.expected: a reset to 0 and
 * the next decode answers 1680 again). A streamed buffer needs the reload
 * GetBufferInfoForResetting(id, sample, info*) describes, which is not
 * modelled -- this game imports both and calls neither, and audio/atrac's
 * resetting and reset2 hold hardware's numbers for when one does. Those are
 * refused as the codec failing. */
static void hle_ResetPlayPosition(void) {
    ATRAC_LOG("ResetPlayPosition");
    atrac_ctx *c = ctx_arg();
    if (!c) { psp_ret(PSP_ATRAC_ERROR_BAD_ATRACID); return; }
    if (!c->has_data) { psp_ret(PSP_ATRAC_ERROR_UNSET_DATA); return; }
    const uint32_t sample = psp_arg(1);
    if (!c->in_memory || sample >= c->total_out) {
        c->internal_error = PSP_ATRAC_ERROR_API_FAIL;
        psp_ret(PSP_ATRAC_ERROR_API_FAIL);
        return;
    }
    seek_to(c, sample);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_no_position(void) {
    ATRAC_LOG("GetBufferInfoForResetting");
    atrac_ctx *c = ctx_arg();
    if (!c) { psp_ret(PSP_ATRAC_ERROR_BAD_ATRACID); return; }
    if (!c->has_data) { psp_ret(PSP_ATRAC_ERROR_UNSET_DATA); return; }
    c->internal_error = PSP_ATRAC_ERROR_API_FAIL;
    psp_ret(PSP_ATRAC_ERROR_API_FAIL);
}

/* ---- calls the game does not make ------------------------------------------
 *
 * pspautotests' audio/atrac suite is the only hardware oracle for the decoder,
 * and it sets its data through the *AndGetID forms and asks the stream about
 * itself through these. They exist so the oracle can reach DecodeData; NIDs
 * from the suite's atrac-imports.S. */

/* Which codec a RIFF header names, or 0. */
static uint32_t header_codec(uint32_t addr, uint32_t avail) {
    if (avail < 12 || psp_read32(addr) != 0x46464952u) return 0;
    uint32_t off = 12;
    while (off + 8 <= avail) {
        const uint32_t id = psp_read32(addr + off), sz = psp_read32(addr + off + 4), body = off + 8;
        if (id == 0x20746D66u && body + 16 <= avail) {
            const uint32_t tag = psp_read16(addr + body);
            if (tag == 0x0270) return PSP_ATRAC_AT3;
            if (tag == 0xFFFE && sz >= 40 && body + 28 <= avail &&
                psp_read32(addr + body + 24) == 0xE923AABFu) return PSP_ATRAC_AT3PLUS;
            return 0;
        }
        if (id == 0x61746164u) return 0;
        off = body + sz + (sz & 1);
    }
    return 0;
}

/* GetAtracID for the header's codec, then set_data; the ID is given back if
 * the data is refused, so a bad file costs nothing (setdata.expected: "Zeroed
 * data: 80630006" from SetDataAndGetID, and the IDs after it still allocate). */
static void set_and_get_id(uint32_t buf, uint32_t read, uint32_t size) {
    uint32_t codec = header_codec(buf, read);
    if (!codec) {
        atrac_ctx tmp; memset(&tmp, 0, sizeof tmp); tmp.codec = PSP_ATRAC_AT3PLUS;
        const uint32_t err = parse_header(&tmp, buf, read);
        psp_ret(err ? err : PSP_ATRAC_ERROR_UNKNOWN_FORMAT);
        return;
    }
    int id = -1;
    const int first = codec == PSP_ATRAC_AT3PLUS ? 0 : ATRAC_PER_CODEC;
    for (int i = first; i < first + ATRAC_PER_CODEC; i++) if (!g_id[i].used) { id = i; break; }
    if (id < 0) { psp_ret(PSP_ATRAC_ERROR_NO_ATRACID); return; }
    atrac_ctx *c = &g_id[id];
    dec_close(c); free(c->frame);
    memset(c, 0, sizeof *c);
    c->used = 1; c->codec = codec;
    const uint32_t err = set_data(c, buf, read, size);
    if (err) { dec_close(c); free(c->frame); c->frame = NULL; c->used = 0; psp_ret(err); return; }
    psp_ret((uint32_t)id);
}

static void hle_SetDataAndGetID(void) {
    ATRAC_LOG("SetDataAndGetID");
    set_and_get_id(psp_arg(0), psp_arg(1), psp_arg(1));
}

static void hle_SetHalfwayBufferAndGetID(void) {
    ATRAC_LOG("SetHalfwayBufferAndGetID");
    set_and_get_id(psp_arg(0), psp_arg(1), psp_arg(2));
}

/* How many samples the next decode will answer. */
static uint32_t next_samples(const atrac_ctx *c) {
    if (c->finished || !c->dec) return 0;
    uint32_t n = c->frame_samples > c->skip ? c->frame_samples - c->skip : 0;
    if (c->pos + n > c->total_out) n = c->total_out > c->pos ? c->total_out - c->pos : 0;
    return n;
}

/* (id, int *end, int *loopStart, int *loopEnd). The last output sample's
 * position, the fact count less one (0x3C6CC for sample.at3's 0x3C6CD), and
 * the loop points on the same scale, or -1 when the file has none. */
static void hle_GetSoundSample(void) {
    ATRAC_LOG("GetSoundSample");
    atrac_ctx *c = ctx_arg();
    if (!c) { psp_ret(PSP_ATRAC_ERROR_BAD_ATRACID); return; }
    if (!c->has_data) { psp_ret(PSP_ATRAC_ERROR_UNSET_DATA); return; }
    put32(psp_arg(1), c->total_out ? c->total_out - 1 : 0);
    put32(psp_arg(2), c->has_loop ? (uint32_t)c->loop_start : 0xFFFFFFFFu);
    put32(psp_arg(3), c->has_loop ? (uint32_t)c->loop_end : 0xFFFFFFFFu);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_GetChannel(void) {
    ATRAC_LOG("GetChannel");
    atrac_ctx *c = ctx_arg();
    if (!c) { psp_ret(PSP_ATRAC_ERROR_BAD_ATRACID); return; }
    if (!c->has_data) { psp_ret(PSP_ATRAC_ERROR_UNSET_DATA); return; }
    put32(psp_arg(1), c->channels);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* The decoder's output is always two channels. */
static void hle_GetOutputChannel(void) {
    ATRAC_LOG("GetOutputChannel");
    atrac_ctx *c = ctx_arg();
    if (!c) { psp_ret(PSP_ATRAC_ERROR_BAD_ATRACID); return; }
    if (!c->has_data) { psp_ret(PSP_ATRAC_ERROR_UNSET_DATA); return; }
    put32(psp_arg(1), ATRAC_OUT_CHANNELS);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_GetMaxSample(void) {
    ATRAC_LOG("GetMaxSample");
    atrac_ctx *c = ctx_arg();
    if (!c) { psp_ret(PSP_ATRAC_ERROR_BAD_ATRACID); return; }
    if (!c->has_data) { psp_ret(PSP_ATRAC_ERROR_UNSET_DATA); return; }
    put32(psp_arg(1), c->frame_samples);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_GetNextSample(void) {
    ATRAC_LOG("GetNextSample");
    atrac_ctx *c = ctx_arg();
    if (!c) { psp_ret(PSP_ATRAC_ERROR_BAD_ATRACID); return; }
    if (!c->has_data) { psp_ret(PSP_ATRAC_ERROR_UNSET_DATA); return; }
    put32(psp_arg(1), next_samples(c));
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* Kilobits per second from the frame size: 376 bytes of ATRAC3+ at 44.1k is
 * 64 (stream.expected). Truncated, as that value says. */
static void hle_GetBitrate(void) {
    ATRAC_LOG("GetBitrate");
    atrac_ctx *c = ctx_arg();
    if (!c) { psp_ret(PSP_ATRAC_ERROR_BAD_ATRACID); return; }
    if (!c->has_data) { psp_ret(PSP_ATRAC_ERROR_UNSET_DATA); return; }
    const uint64_t bps = (uint64_t)c->block_align * 8u * c->sample_rate / c->frame_samples;
    put32(psp_arg(1), (uint32_t)(bps / 1000u));
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* (id, int *pos): the output-sample position the next decode starts at. */
static void hle_GetNextDecodePosition(void) {
    ATRAC_LOG("GetNextDecodePosition");
    atrac_ctx *c = ctx_arg();
    if (!c) { psp_ret(PSP_ATRAC_ERROR_BAD_ATRACID); return; }
    if (!c->has_data) { psp_ret(PSP_ATRAC_ERROR_UNSET_DATA); return; }
    if (c->finished) { psp_ret(PSP_ATRAC_ERROR_ALLDATA_WAS_DECODED); return; }
    put32(psp_arg(1), c->pos);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* The second buffer is for a loop that ends before the file does and needs
 * the tail held separately. Not modelled: second/getinfo.expected answers
 * 80630022 with (0, 0) written for every stream without one, and this game
 * has none. */
#define PSP_ATRAC_ERROR_NO_SECOND_BUF 0x80630022u
static void hle_GetSecondBufferInfo(void) {
    ATRAC_LOG("GetSecondBufferInfo");
    atrac_ctx *c = ctx_arg();
    if (!c) { psp_ret(PSP_ATRAC_ERROR_BAD_ATRACID); return; }
    if (!c->has_data) { psp_ret(PSP_ATRAC_ERROR_UNSET_DATA); return; }
    put32(psp_arg(1), 0);
    put32(psp_arg(2), 0);
    psp_ret(PSP_ATRAC_ERROR_NO_SECOND_BUF);
}

static void hle_IsSecondBufferNeeded(void) {
    ATRAC_LOG("IsSecondBufferNeeded");
    atrac_ctx *c = ctx_arg();
    if (!c) { psp_ret(PSP_ATRAC_ERROR_BAD_ATRACID); return; }
    if (!c->has_data) { psp_ret(PSP_ATRAC_ERROR_UNSET_DATA); return; }
    psp_ret(0);
}

/* Names found by hashing the identifiers in pspautotests' audio/atrac sources
 * against this module's import table; tests/test_hle.c re-derives every NID
 * from its name, so a wrong pairing here fails the build's tests. */
void psp_atrac_register(void) {
    psp_hle_register(0x7A20E7AF, "sceAtrac3plus", "sceAtracSetDataAndGetID",          hle_SetDataAndGetID);
    psp_hle_register(0x0FAE370E, "sceAtrac3plus", "sceAtracSetHalfwayBufferAndGetID", hle_SetHalfwayBufferAndGetID);
    psp_hle_register(0xA2BBA8BE, "sceAtrac3plus", "sceAtracGetSoundSample",           hle_GetSoundSample);
    psp_hle_register(0x31668BAA, "sceAtrac3plus", "sceAtracGetChannel",               hle_GetChannel);
    psp_hle_register(0xB3B5D042, "sceAtrac3plus", "sceAtracGetOutputChannel",         hle_GetOutputChannel);
    psp_hle_register(0xD6A5F2F7, "sceAtrac3plus", "sceAtracGetMaxSample",             hle_GetMaxSample);
    psp_hle_register(0x36FAABFB, "sceAtrac3plus", "sceAtracGetNextSample",            hle_GetNextSample);
    psp_hle_register(0xA554A158, "sceAtrac3plus", "sceAtracGetBitrate",               hle_GetBitrate);
    psp_hle_register(0xE23E3A35, "sceAtrac3plus", "sceAtracGetNextDecodePosition",    hle_GetNextDecodePosition);
    psp_hle_register(0x83E85EA0, "sceAtrac3plus", "sceAtracGetSecondBufferInfo",      hle_GetSecondBufferInfo);
    psp_hle_register(0xECA32A99, "sceAtrac3plus", "sceAtracIsSecondBufferNeeded",     hle_IsSecondBufferNeeded);
    psp_hle_register(0xCA3CA3D2, "sceAtrac3plus", "sceAtracGetBufferInfoForReseting", hle_no_position);

    psp_hle_register(0x780F88D1, "sceAtrac3plus", "sceAtracGetAtracID",               hle_GetAtracID);
    psp_hle_register(0x61EB33F5, "sceAtrac3plus", "sceAtracReleaseAtracID",           hle_ReleaseAtracID);
    psp_hle_register(0x0E2A73AB, "sceAtrac3plus", "sceAtracSetData",                  hle_SetData);
    psp_hle_register(0x3F6E26B5, "sceAtrac3plus", "sceAtracSetHalfwayBuffer",         hle_SetHalfwayBuffer);
    psp_hle_register(0x7DB31251, "sceAtrac3plus", "sceAtracAddStreamData",            hle_AddStreamData);
    psp_hle_register(0x5D268707, "sceAtrac3plus", "sceAtracGetStreamDataInfo",        hle_GetStreamDataInfo);
    psp_hle_register(0x6A8C3CD5, "sceAtrac3plus", "sceAtracDecodeData",               hle_DecodeData);
    psp_hle_register(0x9AE849A7, "sceAtrac3plus", "sceAtracGetRemainFrame",           hle_GetRemainFrame);
    psp_hle_register(0x644E5607, "sceAtrac3plus", "sceAtracResetPlayPosition",        hle_ResetPlayPosition);
    psp_hle_register(0x2DD3E298, "sceAtrac3plus", "sceAtracGetBufferInfoForResetting", hle_no_position);
    psp_hle_register(0x868120B5, "sceAtrac3plus", "sceAtracSetLoopNum",               hle_SetLoopNum);
    psp_hle_register(0xFAA4F89B, "sceAtrac3plus", "sceAtracGetLoopStatus",            hle_GetLoopStatus);
    psp_hle_register(0xE88F759B, "sceAtrac3plus", "sceAtracGetInternalErrorInfo",     hle_GetInternalErrorInfo);
}
