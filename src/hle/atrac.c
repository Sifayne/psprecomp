/* psprecomp — sceAtrac3plus without a decoder: a stream that opens and then
 * refuses to decode.
 *
 * Why not simply refuse everything. An unimplemented firmware call returns
 * zero and writes nothing to its out-parameters (see psp_hle_call), and for
 * this library both halves are lies a game acts on: zero from GetAtracID is a
 * valid ID, and every later call answers a question about the stream through
 * a pointer it never writes. Armored Core's music pump advances a ring cursor
 * by the sample count it is handed; handed nothing, it advances by garbage.
 *
 * The first attempt at honesty was to fail GetAtracID -- no decoder, no ID --
 * and the game did not survive it. Its player thread (CSoundAtrac3Player,
 * priority 16) ticks in a lock / pump / unlock loop whose only throttle is a
 * delay taken when the player is idle or has PCM buffered, and the game's own
 * open-failure path leaves the player *enabled* with remainFrame still zero.
 * In that state the pump does nothing, delays nothing, and the priority-40
 * main thread underneath it never runs again: 890 million semaphore
 * operations, 336 pad polls, a black screen. A SetData failure lands in the
 * same state. On hardware neither ever fails, so the game has no working
 * failure path for either.
 *
 * What the game does handle is a decoder that opens and then cannot decode: a
 * negative return from sceAtracDecodeData takes its stop path, which reads the
 * internal error, releases the ID and idles the player. So that is what this
 * is. Everything up to the decode is real -- the RIFF header is parsed, IDs
 * are handed out and taken back, remainFrame is what hardware reports for the
 * bytes present -- and the decode itself says, truthfully, that it failed.
 *
 * What is measured and what is not is marked at each handler. Every error code
 * is PSPSDK's pspatrac3.h (BSD), by name:
 * https://github.com/pspdev/pspsdk/blob/master/src/atrac3/pspatrac3.h
 *
 * The decoder itself is roadmap item M3, and it plugs in at hle_DecodeData;
 * the bookkeeping here is what it will sit on. Until then the game is silent
 * where the music would be, and says so once. */

#include "psprecomp/hle.h"
#include "psprecomp/cpu.h"
#include "psprecomp/mem.h"

#include <stdio.h>
#include <string.h>

#define PSP_ATRAC_ERROR_API_FAIL              0x80630002u
#define PSP_ATRAC_ERROR_NO_ATRACID            0x80630003u
#define PSP_ATRAC_ERROR_BAD_CODECTYPE         0x80630004u
#define PSP_ATRAC_ERROR_BAD_ATRACID           0x80630005u
#define PSP_ATRAC_ERROR_UNKNOWN_FORMAT        0x80630006u
#define PSP_ATRAC_ERROR_UNMATCH_FORMAT        0x80630007u
#define PSP_ATRAC_ERROR_UNSET_DATA            0x80630010u
#define PSP_ATRAC_ERROR_READSIZE_IS_TOO_SMALL 0x80630011u
#define PSP_ATRAC_ERROR_ADD_DATA_IS_TOO_BIG   0x80630018u
#define PSP_ATRAC_ERROR_UNSET_PARAM           0x80630021u

#define PSP_ATRAC_AT3PLUS 0x1000u
#define PSP_ATRAC_AT3     0x1001u

/* Hardware hands out IDs 0 and 1 for ATRAC3+ and 2 and 3 for ATRAC3 at its
 * default split, and refuses a third of either -- audio/atrac/ids.expected,
 * "Initial ids: ATRAC3+: 0 1, ATRAC3: 2 3". sceAtracReinit can change the
 * split; the game does not import it and it is not registered. */
#define ATRAC_IDS       4
#define ATRAC_PER_CODEC 2

typedef struct {
    int      used;
    uint32_t codec;

    /* Set by SetData / SetHalfwayBuffer. */
    int      has_data;
    uint32_t buf, buf_size;    /* the guest buffer and its capacity */
    uint32_t read_size;        /* bytes of the file present in it */
    uint32_t file_size;        /* RIFF size + 8 */
    uint32_t data_off;         /* file offset of the data chunk's body */
    uint32_t data_size;
    uint32_t block_align;      /* bytes per frame */
    uint32_t channels;
    uint32_t end_sample;
    int      has_loop;
    uint32_t loop_start, loop_end;
    int      loop_num;
    uint32_t internal_error;
} atrac_ctx;

static atrac_ctx g_id[ATRAC_IDS];

void psp_atrac_init(void) { memset(g_id, 0, sizeof g_id); }

/* Said once. A title screen restarts its music every time it comes back. */
static void no_decoder(void) {
    static int said;
    if (!said++)
        fprintf(stderr,
            "psprecomp: sceAtrac3plus has no decoder. The stream opens and its\n"
            "  header is read; sceAtracDecodeData then fails, which is the path\n"
            "  this game's player handles (it stops the track). Music is silent;\n"
            "  everything else continues.\n");
}

static atrac_ctx *ctx_arg(void) {
    const uint32_t id = psp_arg(0);
    return (id < ATRAC_IDS && g_id[id].used) ? &g_id[id] : NULL;
}

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
    c->data_off = c->data_size = c->block_align = c->channels = c->end_sample = 0;
    c->has_loop = 0;
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
            c->block_align = psp_read16(addr + body + 12);
            if (tag == 0x0270) codec = PSP_ATRAC_AT3;
            else if (tag == 0xFFFE && sz >= 40 &&
                     psp_read32(addr + body + 24) == 0xE923AABFu) codec = PSP_ATRAC_AT3PLUS;
        } else if (id == 0x74636166u /* fact */ && sz >= 4) {
            c->end_sample = psp_read32(addr + body);
        } else if (id == 0x6C706D73u /* smpl */ && sz >= 60) {
            /* manufacturer, product, period, unity note, pitch fraction, SMPTE
             * format, SMPTE offset, loop count, sampler data; then per loop:
             * id, type, start, end, fraction, play count. */
            if (psp_read32(addr + body + 28) >= 1) {
                c->has_loop   = 1;
                c->loop_start = psp_read32(addr + body + 44);
                c->loop_end   = psp_read32(addr + body + 48);
            }
        }
        off = body + sz + (sz & 1);
    }
    if (!c->data_off || !c->block_align || !codec) return PSP_ATRAC_ERROR_UNKNOWN_FORMAT;
    if (codec != c->codec) return PSP_ATRAC_ERROR_UNMATCH_FORMAT;
    return 0;
}

/* Hands out the lowest free ID of the requested codec, two per codec -- see
 * ATRAC_PER_CODEC. A codec that is neither ATRAC3 nor ATRAC3+ is refused by
 * name; that code is the header's, not measured. */
static void hle_GetAtracID(void) {
    const uint32_t codec = psp_arg(0);
    if (codec != PSP_ATRAC_AT3PLUS && codec != PSP_ATRAC_AT3) {
        psp_ret(PSP_ATRAC_ERROR_BAD_CODECTYPE);
        return;
    }
    int of_codec = 0;
    for (int i = 0; i < ATRAC_IDS; i++)
        if (g_id[i].used && g_id[i].codec == codec) of_codec++;
    if (of_codec >= ATRAC_PER_CODEC) { psp_ret(PSP_ATRAC_ERROR_NO_ATRACID); return; }
    for (int i = 0; i < ATRAC_IDS; i++) {
        if (g_id[i].used) continue;
        memset(&g_id[i], 0, sizeof g_id[i]);
        g_id[i].used  = 1;
        g_id[i].codec = codec;
        psp_ret((uint32_t)i);
        return;
    }
    psp_ret(PSP_ATRAC_ERROR_NO_ATRACID);
}

static void hle_ReleaseAtracID(void) {
    atrac_ctx *c = ctx_arg();
    if (!c) { psp_ret(PSP_ATRAC_ERROR_BAD_ATRACID); return; }
    c->used = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* (id, buf, bytes) -- the whole file is in the buffer. */
static void hle_SetData(void) {
    atrac_ctx *c = ctx_arg();
    if (!c) { psp_ret(PSP_ATRAC_ERROR_BAD_ATRACID); return; }
    const uint32_t buf = psp_arg(1), size = psp_arg(2);
    const uint32_t err = parse_header(c, buf, size);
    if (err) { psp_ret(err); return; }
    c->has_data  = 1;
    c->buf       = buf;
    c->buf_size  = size;
    c->read_size = size;
    c->loop_num  = 0;
    c->internal_error = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* (id, buf, readBytes, bufBytes) -- the first readBytes of the file are in a
 * buffer of bufBytes, and the rest is streamed in through GetStreamDataInfo
 * and AddStreamData. */
static void hle_SetHalfwayBuffer(void) {
    atrac_ctx *c = ctx_arg();
    if (!c) { psp_ret(PSP_ATRAC_ERROR_BAD_ATRACID); return; }
    const uint32_t buf = psp_arg(1), read = psp_arg(2), size = psp_arg(3);
    const uint32_t err = parse_header(c, buf, read);
    if (err) { psp_ret(err); return; }
    c->has_data  = 1;
    c->buf       = buf;
    c->buf_size  = size;
    c->read_size = read;
    c->loop_num  = 0;
    c->internal_error = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static int all_in_memory(const atrac_ctx *c) { return c->read_size >= c->file_size; }

/* (id, int *remainFrame). Measured in audio/atrac/getremainframe.expected: a
 * buffer holding the whole file answers -1; a half-filled one answers the
 * number of whole frames present *after the first* -- 472, 473 and 847 bytes
 * of a 376-byte-frame file with its data at 96 give 0, 848 gives 1, 1975
 * gives 3, 1976 gives 4, and stream.expected's 0x800 of the same file gives 4.
 * An ID with no data set answers UNSET_DATA and writes nothing; a bad ID
 * answers BAD_ATRACID and writes nothing (remainFrame pre-seeded to -1337 in
 * the test comes back -1337 in both cases). */
static void hle_GetRemainFrame(void) {
    atrac_ctx *c = ctx_arg();
    if (!c) { psp_ret(PSP_ATRAC_ERROR_BAD_ATRACID); return; }
    if (!c->has_data) { psp_ret(PSP_ATRAC_ERROR_UNSET_DATA); return; }
    int32_t remain;
    if (all_in_memory(c)) {
        remain = -1;
    } else {
        const uint32_t present = c->read_size > c->data_off ? c->read_size - c->data_off : 0;
        const uint32_t frames  = present / c->block_align;
        remain = frames ? (int32_t)frames - 1 : 0;
    }
    put32(psp_arg(1), (uint32_t)remain);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* (id, u8 **writePtr, u32 *writableBytes, u32 *readOffset) -- where the caller
 * should put the next piece of the file, how much, and from which file offset.
 *
 * Before the buffer has wrapped that is its free tail, and the count ends on a
 * frame boundary: stream.expected's first call, after loading 0x800 bytes of a
 * 0xB508-byte file into a 0x8000 buffer, answers writePtr = buffer + 0x800,
 * 0x76B0 bytes, offset 0x800 -- and 0x76B0 is the 86 whole 376-byte frames
 * that fit above the 96-byte header, less the 0x800 already present. Bounded
 * by what is left of the file. A buffer holding the whole file has nothing to
 * write.
 *
 * The wrapped case -- where hardware moves the frames already decoded to the
 * bottom and reports the space above them -- is not modelled: nothing here
 * decodes, so the buffer never advances past what SetHalfwayBuffer and
 * AddStreamData put in it. The wrapped values in stream.expected are M3's. */
static void hle_GetStreamDataInfo(void) {
    atrac_ctx *c = ctx_arg();
    if (!c) { psp_ret(PSP_ATRAC_ERROR_BAD_ATRACID); return; }
    if (!c->has_data) { psp_ret(PSP_ATRAC_ERROR_UNSET_DATA); return; }
    uint32_t room = 0;
    if (!all_in_memory(c)) {
        /* The last frame boundary that fits in the buffer, as a file offset. */
        const uint32_t frames_fit = (c->buf_size > c->data_off ? c->buf_size - c->data_off : 0) / c->block_align;
        uint32_t end = c->data_off + frames_fit * c->block_align;
        if (end > c->file_size) end = c->file_size;
        room = end > c->read_size ? end - c->read_size : 0;
    }
    put32(psp_arg(1), c->buf + (all_in_memory(c) ? c->file_size : c->read_size));
    put32(psp_arg(2), room);
    put32(psp_arg(3), all_in_memory(c) ? c->file_size : c->read_size);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* (id, bytes) -- the caller wrote this much where GetStreamDataInfo said. */
static void hle_AddStreamData(void) {
    atrac_ctx *c = ctx_arg();
    if (!c) { psp_ret(PSP_ATRAC_ERROR_BAD_ATRACID); return; }
    if (!c->has_data) { psp_ret(PSP_ATRAC_ERROR_UNSET_DATA); return; }
    const uint32_t bytes = psp_arg(1);
    if (c->read_size + bytes > c->file_size) { psp_ret(PSP_ATRAC_ERROR_ADD_DATA_IS_TOO_BIG); return; }
    c->read_size += bytes;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* (id, u16 *pcm, int *samples, int *end, int *remainFrame). This is the refusal.
 * There is no decoder, so decoding fails -- API_FAIL is the header's name for
 * the codec call failing -- and nothing is written back, so the caller's
 * sample count, end flag and remain stay whatever it set them to. The error is
 * kept for GetInternalErrorInfo, which this game reads next. */
static void hle_DecodeData(void) {
    atrac_ctx *c = ctx_arg();
    if (!c) { psp_ret(PSP_ATRAC_ERROR_BAD_ATRACID); return; }
    if (!c->has_data) { psp_ret(PSP_ATRAC_ERROR_UNSET_DATA); return; }
    no_decoder();
    c->internal_error = PSP_ATRAC_ERROR_API_FAIL;
    psp_ret(PSP_ATRAC_ERROR_API_FAIL);
}

/* (id, int *error). The codec's own last error; zero until a decode has
 * failed. Hardware reports a codec-internal code there -- stream.expected
 * shows 0x20B after a failed decode -- and this has no codec, so the API
 * failure code stands in. What this game does with it is print it and stop. */
static void hle_GetInternalErrorInfo(void) {
    atrac_ctx *c = ctx_arg();
    if (!c) { psp_ret(PSP_ATRAC_ERROR_BAD_ATRACID); return; }
    put32(psp_arg(1), c->internal_error);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* (id, loops). Refused on a file with no loop information -- atractest.expected
 * and stream.expected both answer 80630021 for sample.at3, which has no smpl
 * chunk. The game's tracks have one, spanning the whole track. */
static void hle_SetLoopNum(void) {
    atrac_ctx *c = ctx_arg();
    if (!c) { psp_ret(PSP_ATRAC_ERROR_BAD_ATRACID); return; }
    if (!c->has_data) { psp_ret(PSP_ATRAC_ERROR_UNSET_DATA); return; }
    if (!c->has_loop) { psp_ret(PSP_ATRAC_ERROR_UNSET_PARAM); return; }
    c->loop_num = (int)psp_arg(1);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* (id, int *loopNum, int *loopStatus). stream.expected prints (0, 0) for a
 * file without loop information; the second word for a file *with* one is not
 * in the corpus, and whether a loop exists is the only thing it can mean here.
 * Unreached by this game until a decode succeeds. */
static void hle_GetLoopStatus(void) {
    atrac_ctx *c = ctx_arg();
    if (!c) { psp_ret(PSP_ATRAC_ERROR_BAD_ATRACID); return; }
    if (!c->has_data) { psp_ret(PSP_ATRAC_ERROR_UNSET_DATA); return; }
    put32(psp_arg(1), (uint32_t)c->loop_num);
    put32(psp_arg(2), (uint32_t)c->has_loop);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* Seeking. ResetPlayPosition(id, sample, bytesWrittenFirst, bytesWrittenSecond)
 * and GetBufferInfoForResetting(id, sample, info*) describe what to reload to
 * decode from `sample`. A position is the decoder's to keep and this has none,
 * so both are refused as the codec failing. audio/atrac/resetting and reset2
 * hold hardware's numbers for M3. Unreached by this game before its first
 * decode, which is where it stops. */
static void hle_no_position(void) {
    atrac_ctx *c = ctx_arg();
    if (!c) { psp_ret(PSP_ATRAC_ERROR_BAD_ATRACID); return; }
    if (!c->has_data) { psp_ret(PSP_ATRAC_ERROR_UNSET_DATA); return; }
    no_decoder();
    c->internal_error = PSP_ATRAC_ERROR_API_FAIL;
    psp_ret(PSP_ATRAC_ERROR_API_FAIL);
}

/* Names found by hashing the identifiers in pspautotests' audio/atrac sources
 * against this module's import table; tests/test_hle.c re-derives every NID
 * from its name, so a wrong pairing here fails the build's tests. */
void psp_atrac_register(void) {
    psp_hle_register(0x780F88D1, "sceAtrac3plus", "sceAtracGetAtracID",               hle_GetAtracID);
    psp_hle_register(0x61EB33F5, "sceAtrac3plus", "sceAtracReleaseAtracID",           hle_ReleaseAtracID);
    psp_hle_register(0x0E2A73AB, "sceAtrac3plus", "sceAtracSetData",                  hle_SetData);
    psp_hle_register(0x3F6E26B5, "sceAtrac3plus", "sceAtracSetHalfwayBuffer",         hle_SetHalfwayBuffer);
    psp_hle_register(0x7DB31251, "sceAtrac3plus", "sceAtracAddStreamData",            hle_AddStreamData);
    psp_hle_register(0x5D268707, "sceAtrac3plus", "sceAtracGetStreamDataInfo",        hle_GetStreamDataInfo);
    psp_hle_register(0x6A8C3CD5, "sceAtrac3plus", "sceAtracDecodeData",               hle_DecodeData);
    psp_hle_register(0x9AE849A7, "sceAtrac3plus", "sceAtracGetRemainFrame",           hle_GetRemainFrame);
    psp_hle_register(0x644E5607, "sceAtrac3plus", "sceAtracResetPlayPosition",        hle_no_position);
    psp_hle_register(0x2DD3E298, "sceAtrac3plus", "sceAtracGetBufferInfoForResetting", hle_no_position);
    psp_hle_register(0x868120B5, "sceAtrac3plus", "sceAtracSetLoopNum",               hle_SetLoopNum);
    psp_hle_register(0xFAA4F89B, "sceAtrac3plus", "sceAtracGetLoopStatus",            hle_GetLoopStatus);
    psp_hle_register(0xE88F759B, "sceAtrac3plus", "sceAtracGetInternalErrorInfo",     hle_GetInternalErrorInfo);
}
