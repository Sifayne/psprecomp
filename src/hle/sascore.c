/* psprecomp — sceSasCore.
 *
 * The PSP's hardware voice mixer: 32 voices, each playing 4-bit ADPCM (Sony's
 * VAG format) through an ADSR envelope, summed to a stereo buffer. A game sets
 * voices up, keys them on, and then calls __sceSasCore once per audio frame to
 * render `grain` samples.
 *
 * Unlike the GE, this one is worth implementing *properly* rather than
 * summarising, because it is small and self-contained: ADPCM decode is a
 * documented four-line recurrence, and audio either comes out sounding like
 * the game or it does not. There is no half-working state that misleads you.
 */

#include "psprecomp/hle.h"

#include <stdio.h>
#include <string.h>

#define SAS_VOICES     32
#define SAS_MAX_GRAIN  2048

/* The library's own error codes, each measured in audio/sascore: sascore.expected
 * refuses a grain outside 64..2048 or off a multiple of 32 with 80420001, a
 * voice count outside 1..32 with 80420002, an output mode other than 0 or 1 with
 * 80420003, a sample rate it does not list with 80420004, and a null or
 * unaligned core with 80420005; vag.expected refuses a voice index outside 0..31
 * with 80420010 and a sample size that is zero or not a multiple of 16 with
 * 80420014 (negative multiples of 16 are accepted, so the check is on the low
 * bits, not the sign). Named here by what they refuse. */
#define SAS_ERROR_GRAIN        0x80420001u
#define SAS_ERROR_MAX_VOICES   0x80420002u
#define SAS_ERROR_OUTPUT_MODE  0x80420003u
#define SAS_ERROR_SAMPLE_RATE  0x80420004u
#define SAS_ERROR_CORE         0x80420005u
#define SAS_ERROR_VOICE        0x80420010u
#define SAS_ERROR_SIZE         0x80420014u
/* pcm.expected: a PCM loop position at or past the sample count is refused
 * with 15, and a PCM size outside 1..0x10000 with 1A. Both comparisons are
 * signed -- a loop position of -1, or of 0x80000001, is accepted, while
 * 0x40000001 is not. keyon.expected: keying on a voice that is already on
 * is refused with 16. */
#define SAS_ERROR_LOOP_POS     0x80420015u
#define SAS_ERROR_ALREADY_ON   0x80420016u
#define SAS_ERROR_PCM_SIZE     0x8042001Au

static int grain_ok(uint32_t g) { return g >= 64 && g <= SAS_MAX_GRAIN && (g % 32) == 0; }

/* VAG ADPCM predictor coefficients. Each 16-byte block picks one of five
 * filters; the decoded sample is the shifted nibble plus a weighted sum of the
 * previous two outputs. The weights are /64. */
static const int VAG_F0[5] = { 0, 60, 115,  98, 122 };
static const int VAG_F1[5] = { 0,  0, -52, -55, -60 };

enum { ENV_OFF = 0, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };

typedef struct {
    /* A voice plays one of two things. __sceSasSetVoice hands it VAG ADPCM,
     * which is decoded a 16-byte block at a time; __sceSasSetVoicePCM hands
     * it raw signed 16-bit samples, which are read as they are. The game uses
     * both -- its menu sounds are VAG and its voice clips PCM -- and the PCM
     * path was missing entirely, so those voices were silent. */
    int      is_pcm;
    uint32_t pcm_addr;
    int32_t  pcm_size;      /* samples */
    int32_t  pcm_loop;      /* first sample of the loop; negative means none */
    int32_t  pcm_pos;       /* the sample about to be played */

    uint32_t vag_addr;      /* guest address of the sample data */
    uint32_t vag_size;
    int      loop;          /* loop mode: a block flagged 3 jumps back */
    uint32_t loop_start;    /* byte offset of the block flagged 6, else 0 */
    int      last_block;    /* the block just decoded ended the sample */
    uint32_t pos;           /* byte offset of the current 16-byte block */
    int      sample_idx;    /* 0..27 within the block */
    int      hist1, hist2;  /* ADPCM history */
    int16_t  decoded[28];
    int      decoded_valid;

    uint32_t pitch;         /* 0x1000 == 1.0 */
    uint32_t frac;          /* resampling accumulator, 12-bit fraction */

    int32_t  vol_l, vol_r;  /* 0x1000 == unity */
    int      env_state;
    int32_t  env;           /* 0 .. 0x40000000 */
    int32_t  attack_rate, decay_rate, sustain_level, release_rate;

    int      playing;
    int      ended;
    int      paused;
    /* Samples still to wait before this voice starts. Keying on does not
     * take effect at once: hardware holds the voice for 32 samples, and
     * both the sound and the envelope begin together at the end of it.
     *
     * Two measurements pin it. keyon.expected reads the envelope 0 before a
     * core and 0x60000 after one, which at rate 0x1000 is 96 steps in a
     * 128-sample grain -- 32 short; getheight.expected reads 0x1e0000 after
     * four such cores, which is 96 + 128 + 128 + 128, the same 32 missing
     * once rather than per core. And pcm.expected's rendered output puts the
     * sample the voice starts from at output index 32, with the loop
     * arriving 32 late to match. */
    int32_t  start_delay;
} sas_voice;

static sas_voice g_voice[SAS_VOICES];
static uint32_t  g_grain = 256;
static uint32_t  g_max_voices = SAS_VOICES;
static uint32_t  g_output_mode;
static uint32_t  g_sample_rate = 44100;
static uint64_t  g_frames_rendered;
static uint64_t  g_samples_nonzero;

void psp_sas_reset(void) {
    memset(g_voice, 0, sizeof g_voice);
    for (int i = 0; i < SAS_VOICES; i++) {
        g_voice[i].pitch = 0x1000;
        g_voice[i].vol_l = 0x1000;
        g_voice[i].vol_r = 0x1000;
    }
    g_grain = 256;
    g_max_voices = SAS_VOICES;
    g_output_mode = 0;
    g_sample_rate = 44100;
    g_frames_rendered = 0;
    g_samples_nonzero = 0;
}

void psp_sas_init(void) { psp_sas_reset(); }

uint64_t psp_sas_frames(void)   { return g_frames_rendered; }
uint64_t psp_sas_nonzero(void)  { return g_samples_nonzero; }

static int clamp16(int v) {
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return v;
}

/* Decode the 16-byte ADPCM block at the voice's current position into its
 * 28-sample buffer. Returns 0 when the voice has ended.
 *
 * Where a voice ends is measured where the corpus reaches, and conventional
 * where it does not. audio/sascore/vag.expected plays a 16-block sample in
 * loop mode 1 with every block after the first carrying one flag value, and
 * 16 blocks are 448 samples, inside its one 512-sample grain -- so with flags
 * 0, 1, 7, 0x41 and 0x87 the voice reports ended, and the only thing that is
 * measured by that is that **the buffer end ends a voice whatever the loop
 * mode**, and that the flagged block is decoded (its samples are printed).
 * Two flag facts are measured: exactly 3 keeps the voice playing -- a loop
 * end that jumps back -- and 0x41 does not end it: the same test plays
 * music.vag with its file header still in front, so block 0's flag byte is
 * the 'A' of "VAGp", and the voice is still playing a grain later. So the
 * check is on exact values, not bit 0. Exactly 1 and exactly 7 ending the
 * voice after their block, and 6 marking the loop start, are the format's
 * convention and not reached by any test here.
 *
 * This used to restart from byte 0 at both the buffer end and flag 7 whenever
 * loop mode was set. Armored Core starts its menu sounds with loop mode set,
 * so its "decide" sound played forever, and the game waits for that sound to
 * finish before it leaves the title screen: NEW GAME hung on a sound effect. */
static int decode_block(sas_voice *v) {
    if (v->last_block) return 0;
    if (v->pos + 16 > v->vag_size) return 0;

    uint32_t at = v->vag_addr + v->pos;
    uint8_t hdr   = psp_read8(at);
    uint8_t flags = psp_read8(at + 1);

    int shift  = hdr & 0x0F;
    int filter = (hdr >> 4) & 0x0F;
    if (filter > 4) filter = 0;          /* out of range: treat as no prediction */

    for (int i = 0; i < 28; i++) {
        uint8_t byte = psp_read8(at + 2 + (uint32_t)(i / 2));
        int nib = (i & 1) ? (byte >> 4) : (byte & 0x0F);

        /* Sign-extend the 4-bit sample into the top of a 16-bit word, then
         * shift down. Shifting the nibble directly loses the sign. */
        int s = (int)((int16_t)(nib << 12)) >> shift;
        s += (VAG_F0[filter] * v->hist1 + VAG_F1[filter] * v->hist2) >> 6;
        s = clamp16(s);

        v->decoded[i] = (int16_t)s;
        v->hist2 = v->hist1;
        v->hist1 = s;
    }

    if (flags == 6) v->loop_start = v->pos;
    v->pos += 16;
    if (flags == 3 && v->loop) v->pos = v->loop_start;
    else if (flags == 1 || flags == 3 || flags == 7) v->last_block = 1;
    v->decoded_valid = 1;
    return 1;
}

/* Advance the envelope by one sample and return its current level, 0..0x40000000. */
static int32_t step_envelope(sas_voice *v) {
    switch (v->env_state) {
    case ENV_ATTACK:
        v->env += v->attack_rate;
        if (v->env >= 0x40000000) { v->env = 0x40000000; v->env_state = ENV_DECAY; }
        break;
    case ENV_DECAY:
        v->env -= v->decay_rate;
        if (v->env <= v->sustain_level) { v->env = v->sustain_level; v->env_state = ENV_SUSTAIN; }
        break;
    case ENV_RELEASE:
        v->env -= v->release_rate;
        if (v->env <= 0) { v->env = 0; v->env_state = ENV_OFF; v->playing = 0; v->ended = 1; }
        break;
    case ENV_SUSTAIN:
    default:
        break;
    }
    return v->env;
}

/* Render `samples` stereo frames, summing every active voice. */
static void render(int32_t *mix_l, int32_t *mix_r, uint32_t samples) {
    memset(mix_l, 0, samples * sizeof *mix_l);
    memset(mix_r, 0, samples * sizeof *mix_r);

    for (uint32_t vi = 0; vi < g_max_voices; vi++) {
        sas_voice *v = &g_voice[vi];
        if (!v->playing || v->paused) continue;
        if (!v->is_pcm && !v->vag_addr) continue;

        for (uint32_t i = 0; i < samples; i++) {
            if (v->start_delay > 0) { v->start_delay--; continue; }

            int32_t s;
            if (v->is_pcm) {
                /* A PCM voice whose address is zero is accepted by hardware
                 * (pcm.expected, "Zero: OK"); it plays silence here rather
                 * than reading whatever is at address zero. */
                s = (v->pcm_addr && v->pcm_pos < v->pcm_size)
                        ? (int16_t)psp_read16(v->pcm_addr + (uint32_t)v->pcm_pos * 2u)
                        : 0;
            } else {
                if (!v->decoded_valid || v->sample_idx >= 28) {
                    v->sample_idx = 0;
                    if (!decode_block(v)) { v->playing = 0; v->ended = 1; break; }
                }
                s = v->decoded[v->sample_idx];
            }

            int32_t env = step_envelope(v);
            /* Envelope is 30-bit; bring it down to a 12-bit multiplier before
             * applying, so the product stays inside 32 bits. */
            s = (s * (env >> 18)) >> 12;

            mix_l[i] += (s * v->vol_l) >> 12;
            mix_r[i] += (s * v->vol_r) >> 12;

            /* Pitch is a 12-bit fixed-point step: 0x1000 plays at the source
             * rate, 0x2000 an octave up. */
            v->frac += v->pitch;
            while (v->frac >= 0x1000) {
                v->frac -= 0x1000;
                if (v->is_pcm) {
                    v->pcm_pos++;
                    if (v->pcm_pos >= v->pcm_size) {
                        if (v->pcm_loop >= 0 && v->pcm_loop < v->pcm_size) v->pcm_pos = v->pcm_loop;
                        else { v->playing = 0; v->ended = 1; break; }
                    }
                } else {
                    v->sample_idx++;
                    if (v->sample_idx >= 28) {
                        v->sample_idx = 0;
                        if (!decode_block(v)) { v->playing = 0; v->ended = 1; break; }
                    }
                }
            }
            if (!v->playing) break;
        }
    }
}

/* ---- the calls ----------------------------------------------------------- */

static sas_voice *voice_arg(void) {
    uint32_t i = psp_arg(1);
    return (i < SAS_VOICES) ? &g_voice[i] : NULL;
}

static void hle_Init(void) {
    /* (sasCore, grain, maxVoices, outputMode, sampleRate). Checked in the order
     * sascore.expected reports them; the sample rate is only checked for the
     * two rates this library renders at, since the test's accepted list runs
     * past what was read of it. */
    const uint32_t core = psp_arg(0), grain = psp_arg(1), voices = psp_arg(2),
                   mode = psp_arg(3), rate = psp_arg(4);
    if (!core || (core & 63))          { psp_ret(SAS_ERROR_CORE); return; }
    if (!grain_ok(grain))              { psp_ret(SAS_ERROR_GRAIN); return; }
    if (voices < 1 || voices > SAS_VOICES) { psp_ret(SAS_ERROR_MAX_VOICES); return; }
    if (mode > 1)                      { psp_ret(SAS_ERROR_OUTPUT_MODE); return; }
    if (rate != 44100 && rate != 48000) { psp_ret(SAS_ERROR_SAMPLE_RATE); return; }
    g_grain       = grain;
    g_max_voices  = voices;
    g_output_mode = mode;
    g_sample_rate = rate;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_SetGrain(void) {
    const uint32_t grain = psp_arg(1);
    if (!grain_ok(grain)) { psp_ret(SAS_ERROR_GRAIN); return; }
    g_grain = grain;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_GetGrain(void) { psp_ret(g_grain); }

/* outputmode.expected: 0 and 1 accepted, everything else 80420003, and
 * GetOutputmode answers the mode set. What the mode changes about the mix
 * is not modelled here. */
static void hle_SetOutputmode(void) {
    const uint32_t mode = psp_arg(1);
    if (mode > 1) { psp_ret(SAS_ERROR_OUTPUT_MODE); return; }
    g_output_mode = mode;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_GetOutputmode(void) { psp_ret(g_output_mode); }

static void hle_SetVoice(void) {
    /* (sasCore, voice, vagAddr, size, loopmode) */
    sas_voice *v = voice_arg();
    if (!v) { psp_ret(SAS_ERROR_VOICE); return; }
    const uint32_t size = psp_arg(3);
    if (size == 0 || (size & 15)) { psp_ret(SAS_ERROR_SIZE); return; }
    v->is_pcm   = 0;
    v->vag_addr = psp_arg(2);
    v->vag_size = size;
    v->loop     = (int)psp_arg(4);
    v->pos = 0;
    v->loop_start = 0;
    v->last_block = 0;
    v->sample_idx = 0;
    v->hist1 = v->hist2 = 0;
    v->decoded_valid = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* __sceSasSetVoicePCM(sasCore, voice, addr, size, loopPos): raw signed 16-bit
 * samples rather than ADPCM. The game uses it and it was missing, so those
 * voices played nothing at all.
 *
 * The refusals are pcm.expected's, and each comparison there is signed. A
 * size of zero or below, or above 0x10000 samples, is refused; a loop
 * position at or past the size is refused, which lets -1 (no loop) and even
 * 0x80000001 through while 0x40000001 is turned away. A null address is
 * accepted. */
static void hle_SetVoicePCM(void) {
    sas_voice *v = voice_arg();
    if (!v) { psp_ret(SAS_ERROR_VOICE); return; }
    const int32_t size = (int32_t)psp_arg(3);
    const int32_t loop = (int32_t)psp_arg(4);
    if (size <= 0 || size > 0x10000) { psp_ret(SAS_ERROR_PCM_SIZE); return; }
    if (loop >= size) { psp_ret(SAS_ERROR_LOOP_POS); return; }
    v->is_pcm   = 1;
    v->pcm_addr = psp_arg(2);
    v->pcm_size = size;
    v->pcm_loop = loop;
    v->pcm_pos  = 0;
    v->vag_addr = 0;
    v->decoded_valid = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_SetPitch(void) {
    sas_voice *v = voice_arg();
    if (!v) { psp_ret(SAS_ERROR_VOICE); return; }
    v->pitch = psp_arg(2);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_SetVolume(void) {
    /* (sasCore, voice, l, r, el, er) -- the last two are the reverb sends. */
    sas_voice *v = voice_arg();
    if (!v) { psp_ret(SAS_ERROR_VOICE); return; }
    v->vol_l = (int32_t)psp_arg(2);
    v->vol_r = (int32_t)psp_arg(3);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_SetADSR(void) {
    /* (sasCore, voice, flags, attack, decay, sustain, release) */
    sas_voice *v = voice_arg();
    if (!v) { psp_ret(SAS_ERROR_VOICE); return; }
    uint32_t flags = psp_arg(2);
    if (flags & 1) v->attack_rate  = (int32_t)psp_arg(3);
    if (flags & 2) v->decay_rate   = (int32_t)psp_arg(4);
    if (flags & 4) v->sustain_level= (int32_t)psp_arg(5);
    if (flags & 8) v->release_rate = (int32_t)psp_arg(6);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_SetSimpleADSR(void) {
    sas_voice *v = voice_arg();
    if (!v) { psp_ret(SAS_ERROR_VOICE); return; }
    /* The packed form encodes rates in two 16-bit words. Without the exact
     * curve tables this is an approximation: fast attack, slow release. Audio
     * plays at the right pitch and duration; envelope shape is not exact. */
    v->attack_rate   = 0x40000000 / 64;
    v->decay_rate    = 0x40000000 / 512;
    v->sustain_level = 0x30000000;
    v->release_rate  = 0x40000000 / 256;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* __sceSasSetSL(sasCore, voice, level): the sustain level on its own, the
 * same field __sceSasSetADSR's fourth argument carries. */
static void hle_SetSL(void) {
    sas_voice *v = voice_arg();
    if (!v) { psp_ret(SAS_ERROR_VOICE); return; }
    v->sustain_level = (int32_t)psp_arg(2);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_SetKeyOn(void) {
    sas_voice *v = voice_arg();
    if (!v) { psp_ret(SAS_ERROR_VOICE); return; }
    /* Keying on a voice that is already on is refused, whether or not it is
     * paused -- keyon.expected, "Key on twice" and "While paused". */
    if (v->playing) { psp_ret(SAS_ERROR_ALREADY_ON); return; }
    v->playing = 1;
    v->ended = 0;
    v->paused = 0;
    v->pos = 0;
    v->last_block = 0;
    v->sample_idx = 0;
    v->frac = 0;
    v->hist1 = v->hist2 = 0;
    v->decoded_valid = 0;
    v->pcm_pos = 0;
    v->start_delay = 32;
    v->env = 0;
    v->env_state = ENV_ATTACK;
    if (!v->attack_rate) v->attack_rate = 0x40000000 / 64;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_SetKeyOff(void) {
    sas_voice *v = voice_arg();
    if (!v) { psp_ret(SAS_ERROR_VOICE); return; }
    v->env_state = ENV_RELEASE;
    if (!v->release_rate) v->release_rate = 0x40000000 / 256;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_SetPause(void) {
    /* (sasCore, voiceBitmask, pause) */
    uint32_t mask = psp_arg(1);
    int pause = (int)psp_arg(2);
    for (int i = 0; i < SAS_VOICES; i++)
        if (mask & (1u << i)) g_voice[i].paused = pause;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_GetPauseFlag(void) {
    uint32_t f = 0;
    for (int i = 0; i < SAS_VOICES; i++) if (g_voice[i].paused) f |= 1u << i;
    psp_ret(f);
}

static void hle_GetEndFlag(void) {
    /* A game polls this to know when a sound has finished, and often will not
     * start the next one until a voice reports ended. Reporting "never ended"
     * is a common way to make audio appear to work and then stop. */
    uint32_t f = 0;
    for (int i = 0; i < SAS_VOICES; i++) if (!g_voice[i].playing) f |= 1u << i;
    psp_ret(f);
}

static void hle_GetEnvelopeHeight(void) {
    sas_voice *v = voice_arg();
    if (!v) { psp_ret(SAS_ERROR_VOICE); return; }
    psp_ret((uint32_t)v->env);
}

/* __sceSasGetAllEnvelopeHeights(sasCore, out): the whole bank at once, one
 * int per voice. Exactly 32 are written -- getheight.expected reads the
 * 33rd back as the 0xCCCCCCCC it seeded -- and the game calls this rather
 * than asking voice by voice. */
static void hle_GetAllEnvelopeHeights(void) {
    const uint32_t out = psp_arg(1);
    if (!out) { psp_ret(SAS_ERROR_CORE); return; }
    for (int i = 0; i < SAS_VOICES; i++)
        psp_write32(out + (uint32_t)i * 4u, (uint32_t)g_voice[i].env);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void mix_to_guest(uint32_t out_addr, int add) {
    int32_t l[SAS_MAX_GRAIN], r[SAS_MAX_GRAIN];
    uint32_t n = g_grain;
    render(l, r, n);

    for (uint32_t i = 0; i < n; i++) {
        int32_t sl = clamp16(l[i]), sr = clamp16(r[i]);
        if (add) {
            sl = clamp16(sl + (int16_t)psp_read16(out_addr + i * 4));
            sr = clamp16(sr + (int16_t)psp_read16(out_addr + i * 4 + 2));
        }
        psp_write16(out_addr + i * 4,     (uint16_t)(int16_t)sl);
        psp_write16(out_addr + i * 4 + 2, (uint16_t)(int16_t)sr);
        if (sl || sr) g_samples_nonzero++;
    }
    g_frames_rendered++;
}

static void hle_Core(void) {
    /* (sasCore, sampleBuffer) */
    uint32_t out = psp_arg(1);
    if (out) mix_to_guest(out, 0);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_CoreWithMix(void) {
    uint32_t out = psp_arg(1);
    if (out) mix_to_guest(out, 1);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* Reverb and noise: accepted and recorded so a game's setup sequence completes.
 * The dry mix is what carries the music and effects; reverb is a refinement. */
static void hle_accept(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

void psp_sas_register(void) {
    psp_hle_register(0x42778A9F, "sceSasCore", "__sceSasInit",              hle_Init);
    psp_hle_register(0xD1E0A01E, "sceSasCore", "__sceSasSetGrain",          hle_SetGrain);
    psp_hle_register(0xBD11B7C2, "sceSasCore", "__sceSasGetGrain",          hle_GetGrain);
    psp_hle_register(0xE855BF76, "sceSasCore", "__sceSasSetOutputmode",     hle_SetOutputmode);
    psp_hle_register(0xE175EF66, "sceSasCore", "__sceSasGetOutputmode",     hle_GetOutputmode);
    psp_hle_register(0x99944089, "sceSasCore", "__sceSasSetVoice",          hle_SetVoice);
    psp_hle_register(0xAD84D37F, "sceSasCore", "__sceSasSetPitch",          hle_SetPitch);
    psp_hle_register(0x440CA7D8, "sceSasCore", "__sceSasSetVolume",         hle_SetVolume);
    psp_hle_register(0x019B25EB, "sceSasCore", "__sceSasSetADSR",           hle_SetADSR);
    psp_hle_register(0x9EC3676A, "sceSasCore", "__sceSasSetADSRmode",       hle_accept);
    psp_hle_register(0xCBCD4F79, "sceSasCore", "__sceSasSetSimpleADSR",     hle_SetSimpleADSR);
    psp_hle_register(0x5F9529F6, "sceSasCore", "__sceSasSetSL",             hle_SetSL);
    psp_hle_register(0x76F01ACA, "sceSasCore", "__sceSasSetKeyOn",          hle_SetKeyOn);
    psp_hle_register(0xA0CF2FA4, "sceSasCore", "__sceSasSetKeyOff",         hle_SetKeyOff);
    psp_hle_register(0xA3589D81, "sceSasCore", "__sceSasCore",              hle_Core);
    psp_hle_register(0x50A14DFC, "sceSasCore", "__sceSasCoreWithMix",       hle_CoreWithMix);
    psp_hle_register(0x68A46B95, "sceSasCore", "__sceSasGetEndFlag",        hle_GetEndFlag);
    psp_hle_register(0x787D04D5, "sceSasCore", "__sceSasSetPause",          hle_SetPause);
    psp_hle_register(0x2C8E6AB3, "sceSasCore", "__sceSasGetPauseFlag",      hle_GetPauseFlag);
    psp_hle_register(0x74AE582A, "sceSasCore", "__sceSasGetEnvelopeHeight", hle_GetEnvelopeHeight);
    psp_hle_register(0x07F58C24, "sceSasCore", "__sceSasGetAllEnvelopeHeights",
                     hle_GetAllEnvelopeHeights);
    psp_hle_register(0xE1CD9561, "sceSasCore", "__sceSasSetVoicePCM",      hle_SetVoicePCM);
    psp_hle_register(0xB7660A23, "sceSasCore", "__sceSasSetNoise",          hle_accept);
    psp_hle_register(0x33D4AB37, "sceSasCore", "__sceSasRevType",           hle_accept);
    psp_hle_register(0x267A6DD2, "sceSasCore", "__sceSasRevParam",          hle_accept);
    psp_hle_register(0xD5A229C9, "sceSasCore", "__sceSasRevEVOL",           hle_accept);
    psp_hle_register(0xF983B186, "sceSasCore", "__sceSasRevVON",            hle_accept);
}
