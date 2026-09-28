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
/* pitch.expected: a pitch above 0x4000 is refused with 12, and the check is
 * unsigned -- 0xFFFFFFFF and 0x80000001 are refused alongside 0x4001.
 * noise.expected: a noise frequency outside 0..63 is refused with 11, on the
 * same unsigned footing. */
#define SAS_ERROR_PITCH        0x80420012u
#define SAS_ERROR_NOISE_FREQ   0x80420011u
/* sascore.expected: a volume outside -0x1000..0x1000 is refused with 18, and
 * all four -- the two channel volumes and the two reverb sends -- are
 * checked. */
#define SAS_ERROR_VOLUME       0x80420018u
/* Not a sascore code at all: mixing in output mode 1 comes back as a plain
 * "not supported" from the layer below. */
#define SAS_ERROR_MIX_MODE     0x80000004u
/* A negative ADSR rate -- setadsr.expected's "Value ffffffff". */
#define SAS_ERROR_ADSR_VALUE   0x80420019u

static int grain_ok(uint32_t g) { return g >= 64 && g <= SAS_MAX_GRAIN && (g % 32) == 0; }

/* The guest's SasCore, which is not just a handle: hardware keeps the caller's
 * struct up to date and a game -- or a test -- may read fields straight out of
 * it rather than through a getter. setadsr.expected does exactly that, which
 * is why every value it prints used to be zero here.
 *
 * The layout is pspautotests' sascore.h: a 20-byte header, then 56-byte
 * voices. Only the ADSR block is mirrored, because that is the part the
 * corpus reads and the part a setter is expected to have written by the time
 * it returns. */
#define SAS_G_HEADER          20u
#define SAS_G_VOICE           56u
#define SAS_G_ATTACK_RATE     24u   /* four int rates: attack, decay, sustain, release */
#define SAS_G_SUSTAIN_LEVEL   40u
#define SAS_G_ATTACK_TYPE     44u   /* four bytes, same order */

/* VAG ADPCM predictor coefficients. Each 16-byte block names a filter in the
 * top nibble of its header; the decoded sample is the shifted nibble plus a
 * weighted sum of the previous two outputs, the weights being /64.
 *
 * Five filters are documented, and a block may name sixteen. What hardware
 * does with the other eleven is not a special case: it reads past the end of
 * its table. The two coefficients live as contiguous runs of five int16, the
 * first weight then the second, and index i takes `W[i]` and `W[i + 5]` --
 * so filter 7's first weight is 52, which is filter 2's second weight, and
 * filter 9's second weight is 125, which is filter 14's first. vag.expected
 * sweeps all sixteen and every one of those coincidences holds, which is what
 * says the shape of the overrun is right rather than eleven numbers that
 * happen to fit.
 *
 * The second weight is stored positive and subtracted, which is why the
 * documented pairs read (115, 52) here and (115, -52) elsewhere.
 *
 * Entries past the ninth are measured rather than derived -- they are the
 * module's own data, whatever sits after the table. W[19] is the one value
 * the corpus underdetermines: filter 14 clamps, so anything from 82 to 85
 * gives its output. Nothing an encoder emits reaches past filter 4. */
static const int VAG_W[21] = {
    /* first  weights, 0..4 */    0, 60, 115,  98, 122,
    /* second weights, 0..4 */    0,  0,  52,  55,  60,
    /* past the table          */ 0,  0,   0,   2, 125,
                                  0, 91,   0, 216,  85, 151,
};

enum { ENV_OFF = 0, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };

/* The envelope's shape, per phase. __sceSasSetADSRmode names one curve for
 * each of attack, decay, sustain and release.
 *
 * Which curve a phase will accept is decided by the curve's parity, and
 * setadsr.expected pins the whole matrix: the even ones -- linear increase,
 * bent, exponent -- are the rising shapes and are taken by attack and
 * sustain; the odd ones -- linear decrease, exponent-rev, direct -- are the
 * falling shapes and are taken by decay, sustain and release. Sustain takes
 * either, since it may rise or fall. Anything above 5 is refused, and so is
 * any bit outside the low three and the sign: 0x80000001 is accepted as
 * mode 1 while 0x40000001 is not. */
enum { CURVE_LINEAR_INC = 0, CURVE_LINEAR_DEC = 1, CURVE_LINEAR_BENT = 2,
       CURVE_EXP_REV = 3, CURVE_EXP = 4, CURVE_DIRECT = 5 };
#define SAS_ERROR_ADSR_MODE 0x80420013u
#define ENV_MAX 0x40000000

static int curve_ok(uint32_t mode, int phase_is_attack, int phase_is_sustain) {
    if (mode & ~0x80000007u) return 0;
    const uint32_t m = mode & 7u;
    if (m > CURVE_DIRECT) return 0;
    if (phase_is_sustain) return 1;
    return phase_is_attack ? ((m & 1u) == 0u) : ((m & 1u) == 1u);
}


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
    int32_t  pcm_pos;       /* the resampler's sample index; -1 past the end */
    int      pcm_adv;       /* the last step consumed a sample (see pcm_fetch) */

    uint32_t vag_addr;      /* guest address of the sample data */
    uint32_t vag_size;
    int      loop;          /* loop mode: a block flagged 3 jumps back */
    uint32_t loop_start;    /* byte offset of the block flagged 6, else 0 */
    int      vag_end;       /* the stream has ended: no more blocks */
    uint32_t pos;           /* byte offset of the current 16-byte block */
    int      sample_idx;    /* 0..27 within the block */
    int      hist1, hist2;  /* ADPCM history */
    int16_t  decoded[28];
    int      decoded_valid;
    /* The VAG resampler's two-sample window, u[I] and u[I+1] of the stream
     * with a 0 in front (see vag_fetch). `vag_nxt` is read from the stream
     * only when an output first needs it. */
    int32_t  vag_cur, vag_nxt;
    int      vag_cur_ok;    /* 0 once the window has run past the stream */
    int      vag_nxt_state; /* VAG_NXT_* */

    uint32_t pitch;         /* 0x1000 == 1.0 */
    uint32_t frac;          /* resampling accumulator, 12-bit fraction */

    int32_t  vol_l, vol_r;      /* 0x1000 == unity */
    int32_t  vol_el, vol_er;    /* the two reverb sends, same scale */
    int      env_state;
    int32_t  env;           /* 0 .. 0x40000000 */
    int32_t  attack_rate, decay_rate, sustain_level, release_rate;
    /* The sustain phase runs its own curve at its own rate, like the other
     * three: sasprobe's sustain sweeps (steps 113-119, fw 6.60) climb and
     * fall at exactly sustain_rate per sample, and SetSimpleADSR's sustain
     * keeps falling after the decay (step 132). __sceSasSetADSR's fourth
     * value is this rate, not the level; SetSL sets the level. */
    int32_t  sustain_rate;
    uint32_t mode_attack, mode_decay, mode_sustain, mode_release;

    /* The key is not the sound.
     *
     * `on` is the key, and a key-on is refused while it is down (0x80420016).
     * A key-off lifts it immediately -- pcm and vag key a voice off and
     * straight back on with no core between and hardware restarts it -- while
     * the *release* it schedules waits for the next core, which is what
     * `keyoff_pending` carries. `playing` outlives the key either way: the
     * release is still audible after the key is up, and a fresh key-on may
     * arrive while it still is. adsrcurve needs exactly that: it starts each
     * of its 53 sweeps with a key-off and one core, and a release that has
     * not finished must not stop the next sweep from keying on. Tying the
     * refusal to `playing` instead left the old envelope running and the
     * sweep measured the previous section's curve. */
    int      on;
    int      keyoff_pending;
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

enum { VAG_NXT_UNREAD = 0, VAG_NXT_OK, VAG_NXT_END };

static sas_voice g_voice[SAS_VOICES];
static uint32_t  g_grain = 256;
static uint32_t  g_max_voices = SAS_VOICES;
static uint32_t  g_output_mode;
static uint32_t  g_sample_rate = 44100;
static uint64_t  g_frames_rendered;
static uint64_t  g_samples_nonzero;

/* Every voice back to the state __sceSasInit leaves it in: off, unpaused,
 * no sample, and the defaults hardware writes into each voice of the struct
 * (sasprobe step 4, fw 6.60): rates and sustain level 0, curves linear
 * increase for the attack and linear decrease for the rest, pitch 0x1000,
 * all four volumes 0x1000. Zero curves would mean linear *increase* for all
 * four, which would make a decay climb. */
static void reset_voices(void) {
    memset(g_voice, 0, sizeof g_voice);
    for (int i = 0; i < SAS_VOICES; i++) {
        g_voice[i].mode_attack  = CURVE_LINEAR_INC;
        g_voice[i].mode_decay   = CURVE_LINEAR_DEC;
        g_voice[i].mode_sustain = CURVE_LINEAR_DEC;
        g_voice[i].mode_release = CURVE_LINEAR_DEC;
        g_voice[i].pitch  = 0x1000;
        g_voice[i].vol_l  = 0x1000;
        g_voice[i].vol_r  = 0x1000;
        g_voice[i].vol_el = 0x1000;
        g_voice[i].vol_er = 0x1000;
    }
}

void psp_sas_reset(void) {
    reset_voices();
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
 * 28-sample buffer. Returns 0 when the stream has ended.
 *
 * The flag byte, as firmware 6.60 plays it (sasprobe steps 152-170, all 19
 * vag_flags*.bin captures exact):
 *
 *   7     ends the stream *before* its block: the block is never played, in
 *         either loop mode (steps 162-164)
 *   3     in loop mode 1, continues at the loop start after its block; in
 *         loop mode 0 it is ignored (156, 159)
 *   6     marks the loop start (the last block flagged 6, else block 0)
 *   else  ignored -- 1, 2, 4, 5, 0x41 and 0x87 all play straight through
 *         (154-155, 165-170). So a block flagged 1 does not end the sample.
 *
 * and the end of the buffer ends the voice whatever the loop mode (152-153).
 * That last one is the one that matters most in practice: this used to
 * restart from byte 0 at the buffer end whenever loop mode was set, and
 * Armored Core starts its menu sounds with loop mode set, so its "decide"
 * sound played forever -- and the game waits for that sound to finish before
 * it leaves the title screen: NEW GAME hung on a sound effect. */
static int decode_block(sas_voice *v) {
    if (v->vag_end) return 0;
    if (v->pos + 16 > v->vag_size) { v->vag_end = 1; return 0; }

    uint32_t at = v->vag_addr + v->pos;
    uint8_t hdr   = psp_read8(at);
    uint8_t flags = psp_read8(at + 1);
    if (flags == 7) { v->vag_end = 1; return 0; }

    int shift  = hdr & 0x0F;
    int filter = (hdr >> 4) & 0x0F;      /* all sixteen are real; see VAG_W */

    for (int i = 0; i < 28; i++) {
        uint8_t byte = psp_read8(at + 2 + (uint32_t)(i / 2));
        int nib = (i & 1) ? (byte >> 4) : (byte & 0x0F);

        /* Sign-extend the 4-bit sample into the top of a 16-bit word, then
         * shift down. Shifting the nibble directly loses the sign. */
        int s = (int)((int16_t)(nib << 12)) >> shift;
        s += (VAG_W[filter] * v->hist1 - VAG_W[filter + 5] * v->hist2) >> 6;
        s = clamp16(s);

        v->decoded[i] = (int16_t)s;
        v->hist2 = v->hist1;
        v->hist1 = s;
    }

    if (flags == 6) v->loop_start = v->pos;
    v->pos += 16;
    if (flags == 3 && v->loop) v->pos = v->loop_start;
    v->decoded_valid = 1;
    return 1;
}

/* x >> n rounding towards minus infinity whatever the sign, which C leaves
 * to the compiler for a negative x. */
static int64_t shr_floor(int64_t x, int n) {
    return x >= 0 ? (x >> n) : -((-x - 1) >> n) - 1;
}

/* One sample of one phase, by the phase's curve. Every shape is the one
 * sasprobe measured on firmware 6.60 (steps 79-132: 54 sweeps, the height
 * after every core and the end flag, all reproduced), and none of them looks
 * at where the phase is heading:
 *
 *   linear inc/dec  h +/- rate
 *   bent            h + rate up to three-quarter height, rate/4 past it
 *   exponent-rev    h - ceil(h * rate / 2^32)
 *   exponent        h + 0x4000 + ((0x40000000 - h) * rate >> 32)
 *   direct          h = rate
 *
 * Direct *is* the rate, in every phase: a direct decay of 0x20000000 holds at
 * 0x20000000 and one of 0 ends the voice (steps 109-110), a direct sustain of
 * 0x8000000 holds there (118), and a direct release of 0x10000000 holds and
 * never ends (128). Worked in 64 bits because h + rate passes INT32_MAX. */
static int64_t curve_step(uint32_t mode, int64_t h, int32_t rate) {
    switch (mode & 7u) {
    case CURVE_LINEAR_INC:  return h + rate;
    case CURVE_LINEAR_DEC:  return h - rate;
    /* At three-quarter height exactly the step is still the full rate: the
     * bend is on the way *past* the knee, not at it (sasprobe step 115). */
    case CURVE_LINEAR_BENT: return h + ((h <= (ENV_MAX / 4) * 3) ? rate : (rate >> 2));
    /* Rounded *up* (steps 102-105, 108 and 124-126 all need it), which is
     * also what stops a small height from never falling at all. h is never
     * negative when a step starts: every phase clamps at zero. */
    case CURVE_EXP_REV:     return h - ((h * (int64_t)rate + 0xFFFFFFFFll) >> 32);
    /* A fixed 0x4000 a sample plus the room left, scaled by the rate: the
     * fixed part is why rate 0 and rate 1 climb in a straight line. The room
     * is negative above the top (a direct decay can leave it there), and
     * the shift then rounds down, as hardware's does. */
    case CURVE_EXP:         return h + 0x4000 + shr_floor(((int64_t)ENV_MAX - h) * rate, 32);
    case CURVE_DIRECT:      return rate;
    default:                return h;   /* SetADSRmode refuses anything above 5 */
    }
}

static void end_voice(sas_voice *v) {
    v->env_state = ENV_OFF;
    v->playing = 0;
    v->on = 0;
    v->ended = 1;
}

/* Advance the envelope by one sample. What each phase does with the step,
 * from the same sweeps:
 *
 *   attack   at or past the top: clamp to the top, and decay from the next
 *            sample
 *   decay    below zero: clamp to zero. At or below the sustain level: go to
 *            sustain -- with *no* clamp to the level. A decay of exponent-rev
 *            0x1000000 towards 0x10000000 holds at 0FF32863, the step that
 *            crossed it (106), and a key-off there releases from that height
 *            (130).
 *   sustain  above the top: clamp. At or below zero: zero, and the voice
 *            ends, end flag and all (101, 111, 114, 116, 119). A sustain
 *            that climbs goes all the way to the top (113, 115, 117).
 *   release  the same as sustain. */
static void step_envelope(sas_voice *v) {
    int64_t h;
    switch (v->env_state) {
    case ENV_ATTACK:
        h = curve_step(v->mode_attack, v->env, v->attack_rate);
        if (h >= ENV_MAX) { h = ENV_MAX; v->env_state = ENV_DECAY; }
        break;
    case ENV_DECAY:
        h = curve_step(v->mode_decay, v->env, v->decay_rate);
        if (h < 0) h = 0;
        if (h <= v->sustain_level) v->env_state = ENV_SUSTAIN;
        break;
    case ENV_SUSTAIN:
    case ENV_RELEASE: {
        const int sus = v->env_state == ENV_SUSTAIN;
        h = curve_step(sus ? v->mode_sustain : v->mode_release, v->env,
                       sus ? v->sustain_rate : v->release_rate);
        if (h > ENV_MAX) h = ENV_MAX;
        if (h <= 0) { v->env = 0; end_voice(v); return; }
        break;
    }
    default:
        return;
    }
    v->env = (int32_t)h;
}

/* ---- the resampler ------------------------------------------------------
 *
 * Every pitch interpolates. The output is a straight line between two source
 * samples a and b at the voice's 12-bit fraction f, rounded up:
 *
 *     out = a + ceil((b - a) * f / 4096)
 *
 * sasprobe's PCM and pitch captures (steps 172-191, fw 6.60) pin it: at
 * 0x800 a ramp of 4, 8, 12 ... plays 0 10 8 14 12 18, which no nearest-sample
 * fetch gives, and the eleven pitch_*.bin captures, pitch_change.bin and the
 * six pcm_loop_* ones come out exact. The rounding is measured on rising
 * slopes only; a falling slope rounds up here too, which is not measured.
 *
 * The two kinds differ in three ways, all measured:
 *   - A PCM voice's pitch stops at 0x1000: 0x1800, 0x2000 and 0x4000 play
 *     exactly as 0x1000 does (steps 182-185, 191). A VAG voice's does not:
 *     0x2000 skips every other sample (step 151).
 *   - A VAG voice interpolates over its decoded stream with a 0 in front, the
 *     history the ADPCM decode starts from. That 0 is why a VAG's first
 *     sample is heard one output after a PCM's (33 against 32), and why the
 *     offset scales with the pitch instead of being a fixed extra sample.
 *   - A PCM voice looks one sample ahead whenever the last step consumed
 *     nothing (pcm_fetch), which is what puts 10 between 0 and 8 above.
 *
 * Both end the same way: when the sample the interpolator needs next is past
 * the end, the voice ends. So the last sample of a one-shot is never heard
 * -- a 100-sample PCM plays samples 0-98 (steps 172, 179) and a VAG's last
 * block plays 27 of its 28 (step 75, vag_flags*.bin) -- and the end flag
 * rises one sample earlier than a player that heard it. */

static int32_t ceil_frac(int32_t d, uint32_t f) {
    return (int32_t)-shr_floor(-(int64_t)d * (int64_t)f, 12);
}

/* The sample after `i` in a PCM voice: the next one, the loop position past
 * the end, or -1 when there is none. A loop position outside the sample
 * (SetVoicePCM accepts any negative one) is no loop (step 179). */
static int32_t pcm_next(const sas_voice *v, int32_t i) {
    if (i < 0) return -1;
    if (i + 1 < v->pcm_size) return i + 1;
    return (v->pcm_loop >= 0 && v->pcm_loop < v->pcm_size) ? v->pcm_loop : -1;
}

/* A PCM voice whose address is zero is accepted by hardware (pcm.expected,
 * "Zero: OK"); it plays silence here rather than reading address zero. */
static int32_t pcm_at(const sas_voice *v, int32_t i) {
    return v->pcm_addr ? (int16_t)psp_read16(v->pcm_addr + (uint32_t)i * 2u) : 0;
}

static uint32_t pcm_pitch(const sas_voice *v) { return v->pitch < 0x1000u ? v->pitch : 0x1000u; }

/* The PCM resampler keeps an index I, the fraction f and whether the last
 * step consumed a sample. It interpolates from I when it did and from the
 * sample after I when it did not. Pitch 0 plays silence (step 190). Returns
 * 0 when the voice has run out. */
static int pcm_fetch(const sas_voice *v, int32_t *out) {
    const int32_t j = v->pcm_adv ? v->pcm_pos : pcm_next(v, v->pcm_pos);
    const int32_t k = pcm_next(v, j);
    if (j < 0 || k < 0) return 0;
    if (pcm_pitch(v) == 0) { *out = 0; return 1; }
    const int32_t a = pcm_at(v, j);
    *out = a + ceil_frac(pcm_at(v, k) - a, v->frac);
    return 1;
}

static void pcm_advance(sas_voice *v) {
    v->frac += pcm_pitch(v);
    v->pcm_adv = v->frac >= 0x1000u;
    if (v->pcm_adv) {
        v->frac -= 0x1000u;
        v->pcm_pos = pcm_next(v, v->pcm_pos);
    }
}

/* The next decoded sample of a VAG voice's stream; 0 at its end. */
static int vag_stream_next(sas_voice *v, int32_t *s) {
    if (!v->decoded_valid || v->sample_idx >= 28) {
        if (!decode_block(v)) return 0;
        v->sample_idx = 0;
    }
    *s = v->decoded[v->sample_idx++];
    return 1;
}

static void vag_read_nxt(sas_voice *v) {
    if (v->vag_nxt_state == VAG_NXT_UNREAD)
        v->vag_nxt_state = vag_stream_next(v, &v->vag_nxt) ? VAG_NXT_OK : VAG_NXT_END;
}

static int vag_fetch(sas_voice *v, int32_t *out) {
    if (!v->vag_cur_ok) return 0;
    vag_read_nxt(v);
    if (v->vag_nxt_state != VAG_NXT_OK) return 0;
    *out = v->vag_cur + ceil_frac(v->vag_nxt - v->vag_cur, v->frac);
    return 1;
}

static void vag_advance(sas_voice *v) {
    v->frac += v->pitch;
    for (uint32_t n = v->frac >> 12; n > 0 && v->vag_cur_ok; n--) {
        vag_read_nxt(v);
        v->vag_cur_ok = v->vag_nxt_state == VAG_NXT_OK;
        v->vag_cur = v->vag_nxt;
        v->vag_nxt_state = VAG_NXT_UNREAD;
    }
    v->frac &= 0xFFFu;
}

/* Back to the start of the voice's sample, with the resampler at rest. */
static void restart_source(sas_voice *v) {
    v->frac = 0;
    v->pcm_pos = 0;
    v->pcm_adv = 1;
    v->pos = 0;
    v->loop_start = 0;
    v->vag_end = 0;
    v->sample_idx = 0;
    v->hist1 = v->hist2 = 0;
    v->decoded_valid = 0;
    v->vag_cur = 0;
    v->vag_cur_ok = 1;
    v->vag_nxt_state = VAG_NXT_UNREAD;
}

/* Render `samples` stereo frames, summing every active voice. */
static void render(int32_t *mix_l, int32_t *mix_r, int32_t *mix_el, int32_t *mix_er,
                   uint32_t samples) {
    memset(mix_l,  0, samples * sizeof *mix_l);
    memset(mix_r,  0, samples * sizeof *mix_r);
    memset(mix_el, 0, samples * sizeof *mix_el);
    memset(mix_er, 0, samples * sizeof *mix_er);

    for (uint32_t vi = 0; vi < g_max_voices; vi++) {
        sas_voice *v = &g_voice[vi];
        if (v->keyoff_pending) {
            v->keyoff_pending = 0;
            v->on = 0;
            if (v->playing) {
                v->env_state = ENV_RELEASE;
            }
        }
        if (!v->playing || v->paused) continue;
        if (!v->is_pcm && !v->vag_addr) continue;

        for (uint32_t i = 0; i < samples; i++) {
            if (v->start_delay > 0) { v->start_delay--; continue; }

            int32_t s;
            if (!(v->is_pcm ? pcm_fetch(v, &s) : vag_fetch(v, &s))) { end_voice(v); break; }

            /* Read the envelope, then step it -- in that order. The first
             * sample of a voice is multiplied by a height of zero and comes
             * out silent however loud the source is, which is what pcm and
             * vag show at [020]: a full-scale sample reading 0000. Stepping
             * first shifted every voice one sample earlier than hardware.
             *
             * Envelope is 30-bit; bring it down to a 12-bit multiplier before
             * applying, so the product stays inside 32 bits. */
            const int32_t env = v->env;
            s = (s * (env >> 18)) >> 12;

            mix_l[i]  += (s * v->vol_l)  >> 12;
            mix_r[i]  += (s * v->vol_r)  >> 12;
            mix_el[i] += (s * v->vol_el) >> 12;
            mix_er[i] += (s * v->vol_er) >> 12;
            step_envelope(v);
            if (!v->playing) break;   /* the envelope ended the voice */

            if (v->is_pcm) pcm_advance(v); else vag_advance(v);
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
    /* 44100 and nothing else. The accepted list this once read as "the two
     * rates this renders at" is not hardware's: sascore.expected refuses
     * 48000 along with every other rate it tries. */
    if (rate != 44100) { psp_ret(SAS_ERROR_SAMPLE_RATE); return; }
    /* Init starts every voice over, whatever it was doing: a voice keyed on
     * and playing reads height 0 and ended afterwards, its key is gone
     * (KeyOff is refused), the next core is silent, and a paused voice is
     * unpaused (sasprobe steps 15 and 222, fw 6.60). Every Init rewrites the
     * whole struct with the defaults (steps 4-5). */
    reset_voices();
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
    /* The fifth argument is a loop *mode* here, not the sample position
     * __sceSasSetVoicePCM takes: vag.expected accepts 0 and 1 and refuses
     * everything else, -1 included, with the same code that call uses for a
     * bad position. */
    if (psp_arg(4) > 1u) { psp_ret(SAS_ERROR_LOOP_POS); return; }
    v->is_pcm   = 0;
    v->vag_addr = psp_arg(2);
    v->vag_size = size;
    v->loop     = (int)psp_arg(4);
    restart_source(v);
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
    v->vag_addr = 0;
    restart_source(v);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_SetPitch(void) {
    sas_voice *v = voice_arg();
    if (!v) { psp_ret(SAS_ERROR_VOICE); return; }
    if (psp_arg(2) > 0x4000u) { psp_ret(SAS_ERROR_PITCH); return; }
    v->pitch = psp_arg(2);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* __sceSasSetNoise(sasCore, voice, freq): the voice plays noise instead of
 * its sample. The frequency is checked here -- 0..63 -- and the generator
 * itself is not implemented, so the setting is accepted and the voice keeps
 * playing what it was given. */
static void hle_SetNoise(void) {
    sas_voice *v = voice_arg();
    if (!v) { psp_ret(SAS_ERROR_VOICE); return; }
    if (psp_arg(2) > 63u) { psp_ret(SAS_ERROR_NOISE_FREQ); return; }
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_SetVolume(void) {
    /* (sasCore, voice, l, r, el, er) -- the last two are the reverb sends.
     * All four are bounded at plus or minus unity and all four are checked,
     * whether or not this renderer uses them. */
    sas_voice *v = voice_arg();
    if (!v) { psp_ret(SAS_ERROR_VOICE); return; }
    for (int i = 2; i <= 5; i++) {
        const int32_t vol = (int32_t)psp_arg(i);
        if (vol < -0x1000 || vol > 0x1000) { psp_ret(SAS_ERROR_VOLUME); return; }
    }
    v->vol_l  = (int32_t)psp_arg(2);
    v->vol_r  = (int32_t)psp_arg(3);
    v->vol_el = (int32_t)psp_arg(4);
    v->vol_er = (int32_t)psp_arg(5);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* Write the voice's ADSR block back into the caller's struct, so a read
 * straight after a setter sees what hardware would have left there. */
static void mirror_adsr(const sas_voice *v) {
    const uint32_t core = psp_arg(0);
    const uint32_t vi   = psp_arg(1);
    if (!core || vi >= SAS_VOICES) return;
    const uint32_t a = core + SAS_G_HEADER + vi * SAS_G_VOICE;
    psp_write32(a + SAS_G_ATTACK_RATE,      (uint32_t)v->attack_rate);
    psp_write32(a + SAS_G_ATTACK_RATE + 4,  (uint32_t)v->decay_rate);
    psp_write32(a + SAS_G_ATTACK_RATE + 8,  (uint32_t)v->sustain_rate);
    psp_write32(a + SAS_G_ATTACK_RATE + 12, (uint32_t)v->release_rate);
    psp_write32(a + SAS_G_SUSTAIN_LEVEL,    (uint32_t)v->sustain_level);
    psp_write8 (a + SAS_G_ATTACK_TYPE,      (uint8_t)v->mode_attack);
    psp_write8 (a + SAS_G_ATTACK_TYPE + 1,  (uint8_t)v->mode_decay);
    psp_write8 (a + SAS_G_ATTACK_TYPE + 2,  (uint8_t)v->mode_sustain);
    psp_write8 (a + SAS_G_ATTACK_TYPE + 3,  (uint8_t)v->mode_release);
}

static void hle_SetADSR(void) {
    /* (sasCore, voice, flags, attack, decay, sustain, release) */
    sas_voice *v = voice_arg();
    if (!v) { psp_ret(SAS_ERROR_VOICE); return; }
    const uint32_t flags = psp_arg(2);
    /* A rate is not signed: 0x7FFFFFFF is accepted and anything with the top
     * bit set is refused, -1 included. Checked before anything is stored, and
     * only for the fields the flags select -- setadsr.expected passes -1 for
     * all four with no flag set and gets OK with the old values intact. */
    for (int i = 0; i < 4; i++)
        if ((flags & (1u << i)) && (int32_t)psp_arg(3 + i) < 0) {
            psp_ret(SAS_ERROR_ADSR_VALUE); return;
        }
    if (flags & 1) v->attack_rate  = (int32_t)psp_arg(3);
    if (flags & 2) v->decay_rate   = (int32_t)psp_arg(4);
    if (flags & 4) v->sustain_rate  = (int32_t)psp_arg(5);
    if (flags & 8) v->release_rate = (int32_t)psp_arg(6);
    mirror_adsr(v);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* __sceSasSetADSRmode(sasCore, voice, flags, attack, decay, sustain, release)
 * -- the same flag mask __sceSasSetADSR uses, selecting which of the four to
 * set. Every named curve is checked before anything is stored, which is what
 * setadsr.expected's matrix reads back. */
static void hle_SetADSRmode(void) {
    sas_voice *v = voice_arg();
    if (!v) { psp_ret(SAS_ERROR_VOICE); return; }
    const uint32_t flags = psp_arg(2);
    const uint32_t a = psp_arg(3), d = psp_arg(4), su = psp_arg(5), r = psp_arg(6);
    if ((flags & 1) && !curve_ok(a,  1, 0)) { psp_ret(SAS_ERROR_ADSR_MODE); return; }
    if ((flags & 2) && !curve_ok(d,  0, 0)) { psp_ret(SAS_ERROR_ADSR_MODE); return; }
    if ((flags & 4) && !curve_ok(su, 0, 1)) { psp_ret(SAS_ERROR_ADSR_MODE); return; }
    if ((flags & 8) && !curve_ok(r,  0, 0)) { psp_ret(SAS_ERROR_ADSR_MODE); return; }
    if (flags & 1) v->mode_attack  = a & 7u;
    if (flags & 2) v->mode_decay   = d & 7u;
    if (flags & 4) v->mode_sustain = su & 7u;   /* stored; the shapes are findings item 45 */
    if (flags & 8) v->mode_release = r & 7u;
    mirror_adsr(v);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* The seven-bit rate the attack and sustain fields carry: a three-bit step
 * placed at bit 26 and shifted down by the exponent above it. All ones is
 * zero -- silence, not the slowest rate -- and anything that shifts away to
 * nothing is one, since a rate of zero would mean an envelope that never
 * moves at all. */
static uint32_t simple_rate7(uint32_t r) {
    if (r == 0x7Fu) return 0;
    const uint32_t v = ((7u - (r & 3u)) << 26) >> (r >> 2);
    return v ? v : 1u;
}
static uint32_t simple_sat(uint32_t v) { return v > 0x7FFFFFFFu ? 0x7FFFFFFFu : v; }

/* __sceSasSetSimpleADSR(sasCore, voice, adsr1, adsr2) -- the packed PSX-style
 * envelope, two 16-bit words:
 *
 *   adsr1  15 attack type | 14..8 attack rate | 7..4 decay rate | 3..0 level
 *   adsr2  15..14 sustain type | 13 reserved | 12..6 sustain rate |
 *           5 release type | 4..0 release rate
 *
 * Every field's decode is read off setadsr.expected, which sweeps all of them
 * and prints the 32-bit values the struct ends up holding -- 106 rows, all
 * reproduced. The four decodes are not one formula:
 *
 *   - attack and sustain use the seven-bit form above; sustain type 3 takes a
 *     further quarter of it
 *   - decay is 0x80000000 shifted down by its four bits, saturated, so a rate
 *     of 0 reads back as INT_MAX rather than a negative number
 *   - release is 0x80000000 >> rate for the exponential type and 0x40000000
 *     >> (rate + 2) for the linear one -- and that second shift is a MIPS
 *     shift, taken modulo 32, which is why release rate 30 comes back as
 *     0x40000000 instead of the 0 the arithmetic would give. All ones is
 *     zero for both.
 *
 * The types are not the raw bits either: attack picks between linear-increase
 * and bent, release between linear-decrease and exponent-rev, decay is always
 * exponent-rev, and only sustain passes its two bits through.
 *
 * What stood here before was an invented approximation -- a fast attack and a
 * slow release, chosen to sound plausible. */
static void hle_SetSimpleADSR(void) {
    sas_voice *v = voice_arg();
    if (!v) { psp_ret(SAS_ERROR_VOICE); return; }
    const uint32_t a1 = psp_arg(2) & 0xFFFFu, a2 = psp_arg(3) & 0xFFFFu;
    /* Bit 13 of the second word is reserved, and setting it is refused. */
    if (a2 & (1u << 13)) { psp_ret(SAS_ERROR_ADSR_MODE); return; }

    const uint32_t ar = (a1 >> 8) & 0x7Fu, dr = (a1 >> 4) & 0xFu, sl = a1 & 0xFu;
    const uint32_t st = (a2 >> 14) & 3u,   sr = (a2 >> 6) & 0x7Fu;
    const uint32_t rt = (a2 >> 5) & 1u,    rr = a2 & 0x1Fu;

    uint32_t sus = simple_rate7(sr);
    if (st == 3u && sr != 0x7Fu) { sus >>= 2; if (!sus) sus = 1u; }

    uint32_t rel;
    if (rr == 0x1Fu)  rel = 0;
    else if (rt)      rel = simple_sat(0x80000000u >> rr);
    else            { rel = 0x40000000u >> ((rr + 2u) & 31u); if (!rel) rel = 1u; }

    v->mode_attack  = ((a1 >> 15) & 1u) ? CURVE_LINEAR_BENT : CURVE_LINEAR_INC;
    v->mode_decay   = CURVE_EXP_REV;
    v->mode_sustain = st;
    v->mode_release = rt ? CURVE_EXP_REV : CURVE_LINEAR_DEC;
    v->attack_rate   = (int32_t)simple_rate7(ar);
    v->decay_rate    = (int32_t)simple_sat(0x80000000u >> dr);
    v->sustain_rate  = (int32_t)sus;
    v->release_rate  = (int32_t)rel;
    v->sustain_level = (int32_t)((sl + 1u) << 26);
    mirror_adsr(v);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* __sceSasSetSL(sasCore, voice, level): the sustain level on its own, the
 * same field __sceSasSetADSR's fourth argument carries. */
static void hle_SetSL(void) {
    sas_voice *v = voice_arg();
    if (!v) { psp_ret(SAS_ERROR_VOICE); return; }
    v->sustain_level = (int32_t)psp_arg(2);
    mirror_adsr(v);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_SetKeyOn(void) {
    sas_voice *v = voice_arg();
    if (!v) { psp_ret(SAS_ERROR_VOICE); return; }
    /* Keying on a voice that is already on is refused, and so is keying on a
     * voice that is paused, with the same code -- keyon.expected's "Key on
     * twice" and "While paused". The paused case is not the already-on one
     * wearing a different hat: the test keys the voice *off* before pausing
     * it, and hardware still refuses. A paused voice takes no key. */
    if (v->on || v->paused) { psp_ret(SAS_ERROR_ALREADY_ON); return; }
    v->on = 1;
    v->keyoff_pending = 0;
    v->playing = 1;
    v->ended = 0;
    restart_source(v);
    /* 32 samples before the voice goes live (item 44). A VAG's first decoded
     * sample is heard one output later still, at 33, but that is the
     * resampler's leading 0 (see vag_fetch), not a second delay. */
    v->start_delay = 32;
    v->env = 0;
    v->env_state = ENV_ATTACK;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* A voice whose key is not down has nothing to lift, and keyoff.expected
 * refuses that with the same code an already-on key-on gets -- including
 * while paused.
 *
 * The key comes up here and now. Only the *release* waits for the next core:
 * pcm.expected and vag.expected both key a voice off and straight back on
 * without a core between, and hardware restarts it, which it could not do if
 * the key were still down. Holding `on` until the core made the second
 * key-on fail and left the voice playing the previous section's sound. */
static void hle_SetKeyOff(void) {
    sas_voice *v = voice_arg();
    if (!v) { psp_ret(SAS_ERROR_VOICE); return; }
    if (!v->on) { psp_ret(SAS_ERROR_ALREADY_ON); return; }
    v->on = 0;
    v->keyoff_pending = 1;
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

/* `mix_l`/`mix_r` scale what is already in the buffer, not what is rendered
 * into it: outputmode.expected's mix sections pass 0 for both and get the
 * rendered samples back unchanged, which is only possible if the zero applies
 * to the other side. Zero is the only pair the corpus passes, so the scale
 * itself -- 12-bit, as everywhere else here -- is by analogy with the voice
 * volumes rather than measured. */
static void mix_to_guest(uint32_t out_addr, int add, int32_t mix_l, int32_t mix_r) {
    int32_t l[SAS_MAX_GRAIN], r[SAS_MAX_GRAIN], el[SAS_MAX_GRAIN], er[SAS_MAX_GRAIN];
    const uint32_t n = g_grain;
    render(l, r, el, er, n);

    /* Output mode 1 is not a different mix, it is a different *shape*: four
     * mono blocks of `grain` samples, one after another -- dry left, dry
     * right, then the two reverb sends -- where mode 0 writes one block of
     * interleaved stereo. outputmode.expected reads the same three positions
     * out of each block and finds the same samples at four volumes: a value
     * of -33 in the first block reads -25, -17 and -9 in the others, which is
     * the voice's 0x1000, 0x0C00, 0x0800 and 0x0400 with the product shifted
     * down rather than rounded. */
    if (g_output_mode == 1) {
        const int32_t *block[4] = { l, r, el, er };
        for (int b = 0; b < 4; b++) {
            const uint32_t base = out_addr + (uint32_t)b * n * 2u;
            for (uint32_t i = 0; i < n; i++) {
                const int32_t v = clamp16(block[b][i]);
                psp_write16(base + i * 2u, (uint16_t)(int16_t)v);
                if (v) g_samples_nonzero++;
            }
        }
        g_frames_rendered++;
        return;
    }

    for (uint32_t i = 0; i < n; i++) {
        int32_t sl = clamp16(l[i]), sr = clamp16(r[i]);
        if (add) {
            sl = clamp16(sl + (((int16_t)psp_read16(out_addr + i * 4)     * mix_l) >> 12));
            sr = clamp16(sr + (((int16_t)psp_read16(out_addr + i * 4 + 2) * mix_r) >> 12));
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
    if (out) mix_to_guest(out, 0, 0, 0);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* (sasCore, sampleBuffer, leftMix, rightMix). Refused outright in output mode
 * 1 -- outputmode.expected's last section gets 0x80000004 and a buffer
 * nothing has touched. */
static void hle_CoreWithMix(void) {
    uint32_t out = psp_arg(1);
    if (g_output_mode != 0) { psp_ret(SAS_ERROR_MIX_MODE); return; }
    if (out) mix_to_guest(out, 1, (int32_t)psp_arg(2), (int32_t)psp_arg(3));
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
    psp_hle_register(0x9EC3676A, "sceSasCore", "__sceSasSetADSRmode",       hle_SetADSRmode);
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
    psp_hle_register(0xB7660A23, "sceSasCore", "__sceSasSetNoise",          hle_SetNoise);
    psp_hle_register(0x33D4AB37, "sceSasCore", "__sceSasRevType",           hle_accept);
    psp_hle_register(0x267A6DD2, "sceSasCore", "__sceSasRevParam",          hle_accept);
    psp_hle_register(0xD5A229C9, "sceSasCore", "__sceSasRevEVOL",           hle_accept);
    psp_hle_register(0xF983B186, "sceSasCore", "__sceSasRevVON",            hle_accept);
}
