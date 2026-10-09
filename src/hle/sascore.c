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
#include "psprecomp/state.h"

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
 * checked. 0x80000000 is the exception (see hle_SetVolume). CoreWithMix
 * refuses a mix level with the same code. */
#define SAS_ERROR_VOLUME       0x80420018u
/* Not a sascore code at all: mixing in output mode 1 comes back as a plain
 * "not supported" from the layer below. */
#define SAS_ERROR_MIX_MODE     0x80000004u
/* A negative ADSR rate -- setadsr.expected's "Value ffffffff". */
#define SAS_ERROR_ADSR_VALUE   0x80420019u
/* A wave duty outside 0..100, from __sceSasSetSteepWave and
 * __sceSasSetTrianglarWave alike (sasprobe step 229, fw 6.60). Not in
 * PSPSDK's list of codes. */
#define SAS_ERROR_WAVE_DUTY    0x80420017u
/* The reverb calls' codes, PSPSDK's FX_TYPE, FX_FEEDBACK, FX_DELAY and
 * FX_VOLUME_VAL; which call refuses what with each is sasprobe's (steps
 * 224-226, fw 6.60). */
#define SAS_ERROR_REV_TYPE     0x80420020u
#define SAS_ERROR_REV_FEEDBACK 0x80420021u
#define SAS_ERROR_REV_DELAY    0x80420022u
#define SAS_ERROR_REV_VOLUME   0x80420023u

static int grain_ok(uint32_t g) { return g >= 64 && g <= SAS_MAX_GRAIN && (g % 32) == 0; }

/* The guest's SasCore, which is not just a handle: hardware keeps the caller's
 * struct up to date and a game -- or a test -- may read fields straight out of
 * it rather than through a getter. Here it is a mirror, written from the
 * state this file plays from; nothing is read back out of it but the voices
 * a core copies.
 *
 * Init writes all 0xE20 bytes, PSPSDK's size for it, and each setter and
 * core writes the words below. Firmware 6.60's: sasprobe step 4 is Init's
 * image, and sasprobe 4 steps 18-38 each setter's words and steps 356-359
 * every word a core changes. Step numbers below are sasprobe 4's.
 *
 *   +000  00180990
 *   +004  0BFF after Init, and 00FF by the layout section, whose setup is
 *         an Init, a default for every setter and one core. Which of those
 *         cleared 0x800, 0x200 and 0x100 is not measured; here the core
 *         does. SetOutputmode sets 0x200, whichever mode it is: 02FF after
 *         SetOutputmode(1) and still 02FF after SetOutputmode(0) (steps
 *         35-36)
 *   +008  0x10000 | output mode << 8 | grain / 32 (steps 35-38)
 *   +00C  0
 *   +010  the end flags, as of the last core (FFFFFFFB with voice 2 keyed
 *         on, step 357)
 *   +014  32 voices of 0x38 bytes, as the setters leave them
 *   +714  the same 32 voices as of the last core, which copies them (357)
 *   +E14  the end flags again, as of the last core (357, 359)
 *   +E18  0, +E1C 0
 *
 * A voice:
 *
 *   +00  the sample's address; noise writes EAEAEAEA (step 29)
 *   +04  VAG: the address plus the size. PCM: loop << 16 | size - 1
 *        (100 samples looping at 50 read 00320063, step 20). Noise: 400 at
 *        frequency 17 (step 29)
 *   +08  pitch << 16 | flags: 0x1 a sample, 0x4 PCM, 0x2 noise, 0x10 paused,
 *        0x100 a loop (steps 18-21, 29, 33-34)
 *   +0C  volume, right << 16 | left, 16 bits each (step 22)
 *   +10  the two sends, the same way
 *   +14  00180010 + 0x4C * voice, an address in the engine's own memory
 *   +18  attack, decay, sustain and release rates (steps 23-25)
 *   +28  sustain level (27)
 *   +2C  the four curves, a byte each in the same order (26)
 *   +30  the key << 24 | 0x0707: FF off, FE keyed on, 00 playing, 03 keyed
 *        off (steps 30-32, 357, 359)
 *   +34  0
 *
 * +000, +004's 0BFF, +008's 0x10000 and the voices' +14 are as Init(256,
 * 32, 0, 44100) wrote them, the one Init sasprobe dumps, and are written as
 * measured whatever Init's arguments; whether they depend on the voice count
 * or anything else is not measured. Nor are the steep and triangular waves'
 * words, so a wave setter writes nothing, nor a release's key byte between
 * the core that takes up its key-off and its end, which reads 03 here. */
#define SAS_S_SIZE     0xE20u
#define SAS_S_VOICES   0x014u   /* the voices, as the setters leave them */
#define SAS_S_COPY     0x714u   /* the voices, as of the last core */
#define SAS_S_TAIL     0xE14u
#define SAS_S_VOICE    0x38u

#define SV_SRC         0x00u
#define SV_SRC_END     0x04u
#define SV_FLAGS       0x08u
#define SV_VOLUME      0x0Cu
#define SV_SEND        0x10u
#define SV_ENGINE      0x14u
#define SV_RATES       0x18u    /* four: attack, decay, sustain, release */
#define SV_SL          0x28u
#define SV_CURVES      0x2Cu    /* four bytes, same order */
#define SV_KEY         0x30u
#define SV_LAST        0x34u

/* +08's flags, below the pitch. */
#define SF_SAMPLE      0x001u
#define SF_NOISE       0x002u
#define SF_PCM         0x004u
#define SF_PAUSED      0x010u
#define SF_LOOP        0x100u

/* +004's bits that a core clears. */
#define SH_CORE_CLEARS 0xB00u

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
 * module's own data, whatever sits after the table, so another firmware may
 * differ. sasprobe fits both weights of every filter from its output on
 * firmware 6.60 (steps 133-148): each has exactly one fit, and all sixteen
 * are this table's. W[19], filter 14's second weight, is 6: the corpus's
 * filter-14 run clamps before that weight shows and allowed 82 to 85, but
 * sasprobe's input reaches it first, and 85 oscillates where hardware
 * saturates at 32767 (step 147). Nothing an encoder emits reaches past
 * filter 4. */
static const int VAG_W[21] = {
    /* first  weights, 0..4 */    0, 60, 115,  98, 122,
    /* second weights, 0..4 */    0,  0,  52,  55,  60,
    /* past the table          */ 0,  0,   0,   2, 125,
                                  0, 91,   0, 216,   6, 151,
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

/* What a voice plays. __sceSasSetVoice hands it VAG ADPCM, which is decoded
 * a 16-byte block at a time; __sceSasSetVoicePCM hands it raw signed 16-bit
 * samples, which are read as they are. The game uses both -- its menu sounds
 * are VAG and its voice clips PCM -- and the PCM path was missing entirely,
 * so those voices were silent. __sceSasSetNoise and the two wave calls make
 * the voice a generator instead. */
enum { SRC_NONE = 0, SRC_VAG, SRC_PCM, SRC_NOISE, SRC_STEEP, SRC_TRIANGLE };
typedef struct {
    int      kind;
    uint32_t addr;
    uint32_t size;          /* VAG: bytes; PCM: samples, 1..0x10000 */
    int32_t  loop;          /* VAG: loop mode 0/1; PCM: loop position, negative for none */
    uint32_t param;         /* noise: the frequency, 0..63; waves: the duty, 0..100 */
} sas_source;

typedef struct {
    /* The sample the setters name and the sample that is playing are two
     * things. A SetVoice or SetVoicePCM on a playing voice does not touch
     * what it plays: firmware 6.60 goes on looping the old VAG blocks
     * (sasprobe step 171) and the old PCM loop (step 178) after the call, for
     * as long as the probe listened. `next` is what the setters wrote, and a
     * key-on makes it `src`. */
    sas_source next;
    sas_source src;

    int32_t  pcm_pos;       /* the resampler's sample index; -1 past the end */
    int      pcm_adv;       /* the last step consumed a sample (see pcm_fetch) */

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

    uint32_t pitch;         /* 0x1000 == 1.0; a wave's frequency in Hz */
    uint32_t frac;          /* resampling accumulator, 12-bit fraction */

    int32_t  vol_l, vol_r;      /* 0x1000 == unity */
    int32_t  vol_el, vol_er;    /* the two reverb sends, same scale */
    int      env_state;
    int32_t  env;           /* 0 .. 0x40000000; a direct decay may hold it higher */
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
     * `keyoff_pending` carries. A key-on waits for the next core the same
     * way, in `keyon_pending`, although its key goes down at once (so a
     * second KeyOn is refused and a KeyOff accepted): until that core the
     * height, the end flag and the sound are the old voice's. Firmware 6.60
     * reads the old height straight after a KeyOff+KeyOn (001E0000, and
     * 30000000 in the middle of a release) and the old end flag straight
     * after a KeyOn (sasprobe steps 71, 73, 75 and 213); the struct shows the
     * key-on as pending (step 29). `playing` outlives the key either way: the
     * release is still audible after the key is up, and a fresh key-on may
     * arrive while it still is. adsrcurve needs exactly that: it starts each
     * of its 53 sweeps with a key-off and one core, and a release that has
     * not finished must not stop the next sweep from keying on. Tying the
     * refusal to `playing` instead left the old envelope running and the
     * sweep measured the previous section's curve. */
    int      on;
    int      keyon_pending;
    int      keyoff_pending;
    int      playing;
    int      ended;
    int      paused;
    int      pause_faded;   /* the pause's fade has played (see pause_fade) */
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
     * arriving 32 late to match. sasprobe measured the same on firmware 6.60
     * (steps 69-70), counting from the core that takes up the key-on. */
    int32_t  start_delay;

    /* The noise generator (see noise_advance): the register and its clock,
     * both of which each key-on restarts. */
    uint16_t noise;
    int32_t  noise_cnt;     /* samples left to the next half-tick */
    uint32_t noise_j;       /* that half-tick's place in the eight-tick table */
    uint8_t  noise_tbl;     /* the table, as the key-on's frequency picked it */

    /* A wave's phase, as 44100 times its 16-bit phase (see wave_fetch). */
    uint32_t wave_acc;

    /* The struct's +08 flags that say what the last source setter named:
     * SF_SAMPLE, SF_PCM, SF_NOISE and SF_LOOP. The pause bit and the pitch
     * come from `paused` and `pitch`. Which of them a later setter of
     * another kind clears is not measured; here it replaces them all. */
    uint32_t sflags;
} sas_voice;

enum { VAG_NXT_UNREAD = 0, VAG_NXT_OK, VAG_NXT_END };

static sas_voice g_voice[SAS_VOICES];
static uint32_t  g_grain = 256;
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

static void rev_reset(void);

void psp_sas_reset(void) {
    reset_voices();
    rev_reset();
    g_grain = 256;
    g_output_mode = 0;
    g_sample_rate = 44100;
    g_frames_rendered = 0;
    g_samples_nonzero = 0;
}

void psp_sas_init(void) { psp_sas_reset(); }

uint64_t psp_sas_frames(void)   { return g_frames_rendered; }
uint64_t psp_sas_nonzero(void)  { return g_samples_nonzero; }

static int clamp16(int64_t v) {
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
    if (v->pos + 16 > v->src.size) { v->vag_end = 1; return 0; }

    uint32_t at = v->src.addr + v->pos;
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
    if (flags == 3 && v->src.loop) v->pos = v->loop_start;
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
 * six pcm_loop_* ones come out exact. A falling slope rounds up too, toward
 * the earlier sample: at pitch 0x100 a ramp stepping down by 4 plays 96 96
 * 96 95 95 95 95 94, where rounding down would reach 95 a sample after 96
 * (sasprobe 3 step 201, fw 6.60; pitch_0100_fall.bin exact).
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
    const int32_t size = (int32_t)v->src.size, loop = v->src.loop;
    if (i + 1 < size) return i + 1;
    return (loop >= 0 && loop < size) ? loop : -1;
}

/* A PCM voice whose address is zero is accepted by hardware (pcm.expected,
 * "Zero: OK"); it plays silence here rather than reading address zero. */
static int32_t pcm_at(const sas_voice *v, int32_t i) {
    return v->src.addr ? (int16_t)psp_read16(v->src.addr + (uint32_t)i * 2u) : 0;
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

/* ---- noise ---------------------------------------------------------------
 *
 * __sceSasSetNoise's generator, as firmware 6.60 plays it. A 16-bit register
 * starts at 0, and on each tick of its clock shifts left one bit, taking in
 *
 *     1 ^ bit 9 ^ bit 10 ^ bit 11 ^ bit 14     (of the value before the shift)
 *
 * which is exact over all 496 ticks of the frequency-63 capture and the only
 * recurrence of order 15 or less that is (sasprobe step 198, fw 6.60). The
 * voice plays the register as a signed 16-bit sample, through the envelope
 * and volumes like any other.
 *
 * The clock, from the first ticks of all 64 frequencies (steps 211-274 of
 * sasprobe 3, run 4). Frequency f splits into k = f >> 2 and m = f & 3. For
 * k up to 14 the clock runs in half-ticks H = 2^(14-k) + 1 samples apart,
 * the first after the voice's sample H (its first sample being 0, 32 frames
 * into the key-on core). Half-tick j, counting from 0 there, moves the
 * register if bit j % 8 of
 *
 *     m = 0: 0x55   1: 0x75   2: 0x77   3: 0x7F
 *
 * is set: every other half-tick at m = 0, and one, three or five more of
 * every eight at m = 1, 2, 3 -- gaps of 2,2,1,1,2 half-ticks at m = 1, 1,1,2
 * at m = 2 and six 1s and a 2 at m = 3. That is exact for all 60 of those
 * frequencies, out to frequency 0, which first ticks at frame 16418 and then
 * every 32770 samples (step 211). At k = 15, frequencies 60-63,
 * the register moves after the voice's sample 0 and every second sample
 * after that, whatever m is (steps 271-274), which with step 279 below reads
 * as half-ticks 2 samples apart and a table of all ones.
 *
 * Each voice has its own generator (steps 281-283: two voices keyed on
 * together tick independently, each from its own start), and each key-on
 * starts both the register and the clock over, whether or not a SetNoise
 * came before it (steps 278 and 280; the "different" re-key of round 1's
 * step 199 was the re-key fade, see rekey_fade). A SetNoise on a voice that
 * is playing noise changes its frequency at once, with no key-on, and that
 * changes the spacing but not the table, which stays the one the key-on's
 * frequency picked. Step 279 went from 48 to 63: the half-tick already due
 * came when it was due, 2 samples into the next core, and ticked, and then
 * the register moved every 4 samples -- 63's spacing of 2 with 48's 0x55 --
 * where a voice keyed on at 63 moves every 2. That one switch is all that
 * is measured of it. */

static const uint8_t NOISE_TABLE[4] = { 0x55, 0x75, 0x77, 0x7F };

static int32_t noise_half(uint32_t freq) {
    const uint32_t k = freq >> 2;
    return k >= 15 ? 2 : (int32_t)(1u << (14u - k)) + 1;
}

static void noise_restart(sas_voice *v) {
    const uint32_t k = v->src.param >> 2;
    v->noise = 0;
    v->noise_j = 0;
    v->noise_tbl = k >= 15 ? 0xFF : NOISE_TABLE[v->src.param & 3u];
    v->noise_cnt = k >= 15 ? 1 : noise_half(v->src.param) + 1;
}

/* Called once per output sample, after the sample is taken. */
static void noise_advance(sas_voice *v) {
    if (--v->noise_cnt > 0) return;
    v->noise_cnt = noise_half(v->src.param);
    const uint32_t j = v->noise_j;
    v->noise_j = (j + 1u) & 7u;
    if (!((v->noise_tbl >> j) & 1u)) return;
    const uint32_t n = v->noise;
    const uint32_t in = 1u ^ (((n >> 9) ^ (n >> 10) ^ (n >> 11) ^ (n >> 14)) & 1u);
    v->noise = (uint16_t)((n << 1) | in);
}

/* ---- steep and triangular waves --------------------------------------------
 *
 * __sceSasSetSteepWave and __sceSasSetTrianglarWave make the voice a
 * generator whose frequency is its pitch in Hz, as PSPSDK's header says:
 * SetPitch 441 gives a 100-sample period. The phase is kept exactly, as
 * acc = n * pitch * 65536 modulo P = 65536 * 44100, so u = acc / P is the
 * fraction of the period after n samples; a pitch change mid-play carries on
 * from the same phase. With d = duty / 100, firmware 6.60 plays (sasprobe
 * steps 335-350 of version 3, run 4; all sixteen wave_*.bin captures):
 *
 *   steep       +8192 while u < d, else -8192. Duty 0 is -8192 throughout
 *               and duty 100 +8192.
 *   triangular  three straight lines through (0, 0), (d/2, 16384),
 *               (1 - d/2, -16384) and (1, 0):
 *                 u < d/2        floor(u * 32768 / d)
 *                 up to 1 - d/2  floor(A - x / (2 (1 - d))), x = (u - d/2) * 65536
 *                 past that      floor((acc - P - C) * 32768 / (d P))
 *
 * Two constants in that are not the geometry's. The last segment sits
 * C = 0x04CC0000 lower in acc than the line through (1, 0) -- 1824.9 in
 * 16-bit phase, which is 3 P modulo 2^32, so the look of a 32-bit overflow
 * somewhere -- at duties 25, 50 and 75 alike, and duty 100 subtracts C but
 * not P (its values climb to 31527 before the wrap to 0). And the falling
 * line's intercept A is 16384 + 1 - 2d, a hair over: the tri25 captures
 * allow 16384.5011 to 16384.5066 and tri75 16383.36 to 16383.52, and
 * 1 - 2d + 1/256 is taken. Duty 50 falls as 16384 - floor(x) instead, A just
 * under 16385, and duty 0 as 16384 - floor(u * 32768.7): its slope is 1.00002
 * times the geometry's, and 327687 / 655360 is the only such ratio in steps
 * of 1/655360 that fits (step 344). All of that is fitted, not understood;
 * with it all sixteen wave captures come out exact. Other duties run the
 * same formulas, unmeasured. */

#define WAVE_WRAP (65536u * 44100u)
#define WAVE_C    0x04CC0000ll

static int64_t floor_div(int64_t x, int64_t d) {
    int64_t q = x / d;
    if ((x % d) != 0 && x < 0) q--;
    return q;
}

static int32_t wave_value(const sas_voice *v) {
    const int64_t acc = v->wave_acc, P = WAVE_WRAP, duty = v->src.param;
    if (v->src.kind == SRC_STEEP) return acc * 100 < duty * P ? 8192 : -8192;
    if (duty > 0 && acc * 200 < duty * P)
        return (int32_t)floor_div(acc * 3276800, duty * P);
    if (duty == 0) return 16384 - (int32_t)(acc * 327687 / (44100ll * 655360));
    if (acc * 200 < (200 - duty) * P) {
        const int64_t x200 = acc * 200 - duty * P;    /* x * 200 * 44100 */
        if (duty == 50) return 16384 - (int32_t)floor_div(x200, 200 * 44100);
        /* A = 16384 + 1 - 2d + 1/256 */
        return (int32_t)floor_div((1638500 - 2 * duty) * 176400 * (100 - duty) * 256
                                      + 176400 * (100 - duty) * 100 - x200 * 25600,
                                  176400 * (100 - duty) * 25600);
    }
    return (int32_t)floor_div((acc - (duty < 100 ? P : 0) - WAVE_C) * 3276800, duty * P);
}

static void wave_advance(sas_voice *v) {
    v->wave_acc = (uint32_t)(((uint64_t)v->wave_acc + (uint64_t)v->pitch * 65536u) % WAVE_WRAP);
}

/* Take up the sample the setters last named, from its start, with the
 * resampler at rest. */
static void restart_source(sas_voice *v) {
    v->src = v->next;
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
    v->wave_acc = 0;
    noise_restart(v);
}

/* The voice's next output sample, before the envelope; 0 when the voice has
 * run out of sample. */
static int fetch_sample(sas_voice *v, int32_t *s) {
    switch (v->src.kind) {
    case SRC_PCM:   return pcm_fetch(v, s);
    case SRC_VAG:   return vag_fetch(v, s);
    case SRC_NOISE: *s = (int16_t)v->noise; return 1;
    case SRC_STEEP:
    case SRC_TRIANGLE: *s = wave_value(v); return 1;
    default:        return 0;
    }
}

static void advance_source(sas_voice *v) {
    switch (v->src.kind) {
    case SRC_PCM:      pcm_advance(v); break;
    case SRC_VAG:      vag_advance(v); break;
    case SRC_NOISE:    noise_advance(v); break;
    case SRC_STEEP:
    case SRC_TRIANGLE: wave_advance(v); break;
    default:           break;
    }
}

/* The source sample the resampler would start its next output from -- the
 * left end of its window, not interpolated. What a pause holds (see
 * pause_fade). */
static int32_t window_sample(sas_voice *v) {
    switch (v->src.kind) {
    case SRC_PCM: {
        const int32_t j = v->pcm_adv ? v->pcm_pos : pcm_next(v, v->pcm_pos);
        return j < 0 ? 0 : pcm_at(v, j);
    }
    case SRC_VAG:   return v->vag_cur_ok ? v->vag_cur : 0;
    case SRC_NOISE: return (int16_t)v->noise;
    case SRC_STEEP:
    case SRC_TRIANGLE: return wave_value(v);
    default:        return 0;
    }
}

/* ---- the fades ------------------------------------------------------------
 *
 * Two things cut a sounding voice off, and hardware fades both over 20
 * samples rather than stopping dead (sasprobe 3, run 4):
 *
 *   - A pause (steps 305 and 310-312). The first core after SetPause plays
 *     the voice's next sample as usual, then 20 samples of the *held* source
 *     sample fading, then silence; later paused cores are silent. Nothing
 *     moves meanwhile -- not the envelope (its height reads the same through
 *     the pause) and not the source: on resume the voice plays that first
 *     sample again and carries on (196 at the pause and 196 again at the
 *     resume in pause_ramp.bin; 964, then 964 970 968 974 at pitch 0x800).
 *     The held sample is the left end of the resampler's window one step on,
 *     uninterpolated: 904 of the ramp where 900 was playing at pitch 0x1000,
 *     968 where 964 was playing at 0x800.
 *   - A key-on taken up while the voice still sounds -- a KeyOff and KeyOn
 *     with no core between, or a KeyOn on a voice in its release (steps
 *     170, 177 and 275-278). The old voice plays sample 0 of that core as
 *     usual and goes on playing samples 1-20 -- its source advancing, noise
 *     ticking -- through the same fade, and the new voice starts at 32 as
 *     it always does. Round 1 read the noise re-key of step 199 as a clock
 *     that ran on; it was this fade.
 *
 * The fade's gain after k samples is G_k = floor(16384 * 0.625^k), 16384
 * 10240 6400 4000 2500 1562 ... 3 2 1, reaching 0 at k = 21 -- equally, an
 * exponent-rev curve at rate 0x60000000 run from 2^30, taken >> 16. Each
 * faded sample is
 *
 *     g = env12 * G_k >> 12,  gch = g * vol >> 12,  out = s * gch >> 14
 *
 * per channel, with env12 the height >> 18. That is exact on 32767 and
 * -32768 at volumes 0x1000 and 0x800 (310-311), on the envelope-scaled ramp
 * (305, height 0x0E000000) and on both re-key captures; whether the envelope
 * or the volume is applied first cannot be told from them. */

#define FADE_SAMPLES 20

static int32_t fade_gain(int k) {
    int64_t h = (int64_t)1 << 30;
    while (k-- > 0) h = curve_step(CURVE_EXP_REV, h, 0x60000000);
    return (int32_t)(h >> 16);
}

static void mix_faded(int64_t *const mix[4], uint32_t i, const sas_voice *v, int32_t s, int k) {
    const int32_t vol[4] = { v->vol_l, v->vol_r, v->vol_el, v->vol_er };
    const int64_t g = shr_floor((int64_t)(v->env >> 18) * fade_gain(k), 12);
    for (int c = 0; c < 4; c++)
        mix[c][i] += shr_floor((int64_t)s * shr_floor(g * vol[c], 12), 14);
}

static void mix_normal(int64_t *const mix[4], uint32_t i, const sas_voice *v, int32_t s) {
    /* Envelope is 30-bit; bring it down to a 12-bit multiplier before
     * applying, so the product stays inside 32 bits. */
    s = (s * (v->env >> 18)) >> 12;
    mix[0][i] += shr_floor((int64_t)s * v->vol_l,  12);
    mix[1][i] += shr_floor((int64_t)s * v->vol_r,  12);
    mix[2][i] += shr_floor((int64_t)s * v->vol_el, 12);
    mix[3][i] += shr_floor((int64_t)s * v->vol_er, 12);
}

static int voice_audible(const sas_voice *v) {
    return v->playing && v->start_delay == 0 && v->src.kind != SRC_NONE &&
           !(v->src.kind == SRC_VAG && !v->src.addr);
}

/* The old voice's last 21 samples, played on a copy of it: the key-on that
 * follows starts the voice over anyway. */
static void rekey_fade(int64_t *const mix[4], const sas_voice *old, uint32_t samples) {
    sas_voice c = *old;
    if (!voice_audible(&c)) return;
    for (uint32_t i = 0; i <= FADE_SAMPLES && i < samples; i++) {
        int32_t s;
        if (!fetch_sample(&c, &s)) return;
        if (i == 0) mix_normal(mix, i, &c, s);
        else        mix_faded(mix, i, &c, s, (int)i);
        step_envelope(&c);
        if (!c.playing) return;
        advance_source(&c);
    }
}

/* The first paused core of a voice: its sample 0 as usual, then the held
 * sample fading. The voice itself does not move. */
static void pause_fade(int64_t *const mix[4], const sas_voice *v, uint32_t samples) {
    sas_voice c = *v;
    int32_t s;
    if (!voice_audible(&c) || !fetch_sample(&c, &s)) return;
    mix_normal(mix, 0, &c, s);
    advance_source(&c);
    s = window_sample(&c);
    for (uint32_t i = 1; i <= FADE_SAMPLES && i < samples; i++) mix_faded(mix, i, &c, s, (int)i);
}

/* Render `samples` stereo frames, summing every active voice.
 *
 * All 32 of them, whatever __sceSasInit's voice count said: with a count of
 * 8, a voice 8 keyed on is heard and its height climbs like voice 7's
 * (sasprobe step 16, fw 6.60), and with a count of 1, voices 0, 8, 16 and 31
 * all play -- constants 1, 10, 100 and 4096 sum to 4207 -- and report
 * height 0x40000000 and not ended (sasprobe 3 step 17). */
static void render(int64_t *mix_l, int64_t *mix_r, int64_t *mix_el, int64_t *mix_er,
                   uint32_t samples) {
    memset(mix_l,  0, samples * sizeof *mix_l);
    memset(mix_r,  0, samples * sizeof *mix_r);
    memset(mix_el, 0, samples * sizeof *mix_el);
    memset(mix_er, 0, samples * sizeof *mix_er);

    int64_t *const mix[4] = { mix_l, mix_r, mix_el, mix_er };
    for (uint32_t vi = 0; vi < SAS_VOICES; vi++) {
        sas_voice *v = &g_voice[vi];
        /* A key-on first, then a key-off: a KeyOn and a KeyOff with no core
         * between leave the voice ended with height 0 after one core, as a
         * key-on straight into its release does (sasprobe step 74). */
        if (v->keyon_pending) {
            v->keyon_pending = 0;
            if (!v->paused) rekey_fade(mix, v, samples);
            v->playing = 1;
            v->ended = 0;
            restart_source(v);
            /* 32 samples before the voice goes live (item 44). A VAG's first
             * decoded sample is heard one output later still, at 33, but
             * that is the resampler's leading 0 (see vag_fetch), not a
             * second delay. */
            v->start_delay = 32;
            v->env = 0;
            v->env_state = ENV_ATTACK;
        }
        if (v->keyoff_pending) {
            v->keyoff_pending = 0;
            v->on = 0;
            if (v->playing) {
                v->env_state = ENV_RELEASE;
            }
        }
        if (!v->playing) continue;
        if (v->paused) {
            if (!v->pause_faded) pause_fade(mix, v, samples);
            v->pause_faded = 1;
            continue;
        }
        if (v->src.kind == SRC_NONE || (v->src.kind == SRC_VAG && !v->src.addr)) continue;
        /* A sustain or release that has fallen below 0x8000 ends at the next
         * core. Exponent-rev 0x1000000 from 0x20000000 in the sustain reads
         * 0x6A2A after core 9 and 0, ended, after core 10 (sasprobe 3 step
         * 136, fw 6.60), where the curve alone would fall only to 0x26AA; the
         * decay has no such floor (step 112 reads 0xED). Whether the check is
         * once a core, as here, or a lower floor each sample (any in 0x26AB
         * to 0x6A2A fits) is not settled. */
        if ((v->env_state == ENV_SUSTAIN || v->env_state == ENV_RELEASE) && v->env < 0x8000) {
            v->env = 0;
            end_voice(v);
            continue;
        }

        for (uint32_t i = 0; i < samples; i++) {
            if (v->start_delay > 0) { v->start_delay--; continue; }

            int32_t s;
            if (!fetch_sample(v, &s)) { end_voice(v); break; }

            /* Read the envelope, then step it -- in that order. The first
             * sample of a voice is multiplied by a height of zero and comes
             * out silent however loud the source is, which is what pcm and
             * vag show at [020]: a full-scale sample reading 0000. Stepping
             * first shifted every voice one sample earlier than hardware. */
            mix_normal(mix, i, v, s);
            step_envelope(v);
            if (!v->playing) break;   /* the envelope ended the voice */

            advance_source(v);
        }
    }
}

/* ---- the caller's struct (see SAS_S_SIZE) -------------------------------- */

/* +004 as the setters and cores have left it. */
static uint32_t g_s_header_flags;

/* A null or unaligned core: Init refuses both with 80420005 and Core a null
 * one (sasprobe step 11, sasprobe 4 steps 351 and 355, fw 6.60). The
 * setters do not check it; here they write nothing to such a struct. */
static int core_bad(void) {
    const uint32_t core = psp_arg(0);
    return !core || (core & 63u);
}

static uint32_t s_voice(uint32_t vi) { return psp_arg(0) + SAS_S_VOICES + vi * SAS_S_VOICE; }

static uint32_t end_flags(void) {
    uint32_t f = 0;
    for (int i = 0; i < SAS_VOICES; i++) if (!g_voice[i].playing) f |= 1u << i;
    return f;
}

static void s_header(void) {
    if (core_bad()) return;
    psp_write32(psp_arg(0) + 0x004u, g_s_header_flags);
    psp_write32(psp_arg(0) + 0x008u, 0x10000u | g_output_mode << 8 | g_grain / 32u);
}

static void s_flags(uint32_t vi) {
    if (core_bad() || vi >= SAS_VOICES) return;
    const sas_voice *v = &g_voice[vi];
    psp_write32(s_voice(vi) + SV_FLAGS, v->pitch << 16 | v->sflags | (v->paused ? SF_PAUSED : 0u));
}

/* A source setter's words: +00, +04, and what +08's flags say it named. */
static void s_source(uint32_t vi, uint32_t src, uint32_t src_end, uint32_t sflags) {
    g_voice[vi].sflags = sflags;
    if (core_bad()) return;
    psp_write32(s_voice(vi) + SV_SRC, src);
    psp_write32(s_voice(vi) + SV_SRC_END, src_end);
    s_flags(vi);
}

/* Each volume as 16 bits, so 0x80000000 is kept as 0: SetVolume(0x80000000,
 * 0x1000, 0x80000000, 0x1000) leaves both words 10000000 (sasprobe 3 step
 * 290, fw 6.60). */
static void s_volumes(uint32_t vi) {
    if (core_bad() || vi >= SAS_VOICES) return;
    const sas_voice *v = &g_voice[vi];
    psp_write32(s_voice(vi) + SV_VOLUME, (uint32_t)(uint16_t)v->vol_r << 16 | (uint16_t)v->vol_l);
    psp_write32(s_voice(vi) + SV_SEND, (uint32_t)(uint16_t)v->vol_er << 16 | (uint16_t)v->vol_el);
}

static void s_adsr(uint32_t vi) {
    if (core_bad() || vi >= SAS_VOICES) return;
    const sas_voice *v = &g_voice[vi];
    const uint32_t a = s_voice(vi);
    psp_write32(a + SV_RATES,      (uint32_t)v->attack_rate);
    psp_write32(a + SV_RATES + 4,  (uint32_t)v->decay_rate);
    psp_write32(a + SV_RATES + 8,  (uint32_t)v->sustain_rate);
    psp_write32(a + SV_RATES + 12, (uint32_t)v->release_rate);
    psp_write32(a + SV_SL,         (uint32_t)v->sustain_level);
    psp_write8 (a + SV_CURVES,     (uint8_t)v->mode_attack);
    psp_write8 (a + SV_CURVES + 1, (uint8_t)v->mode_decay);
    psp_write8 (a + SV_CURVES + 2, (uint8_t)v->mode_sustain);
    psp_write8 (a + SV_CURVES + 3, (uint8_t)v->mode_release);
}

/* The key byte: FE from a KeyOn until the core that takes it up, then 00;
 * 03 from a KeyOff; FF once the voice has ended (sasprobe 4 steps 30-32 and
 * 356-359, fw 6.60). A KeyOn and a KeyOff with no core between read as the
 * later of the two, which is not measured. */
static uint32_t key_byte(const sas_voice *v) {
    if (v->keyoff_pending) return 0x03;
    if (v->keyon_pending)  return 0xFE;
    if (!v->playing)       return 0xFF;
    return v->on ? 0x00 : 0x03;
}

static void s_key(uint32_t vi) {
    if (core_bad() || vi >= SAS_VOICES) return;
    psp_write32(s_voice(vi) + SV_KEY, key_byte(&g_voice[vi]) << 24 | 0x0707u);
}

/* Each voice's key byte, the end flags at +010 and +E14, and a copy of the
 * 32 voices at +714. */
static void s_snapshot(void) {
    const uint32_t core = psp_arg(0);
    for (uint32_t vi = 0; vi < SAS_VOICES; vi++) s_key(vi);
    psp_write32(core + 0x010u, end_flags());
    for (uint32_t o = 0; o < SAS_VOICES * SAS_S_VOICE; o += 4)
        psp_write32(core + SAS_S_COPY + o, psp_read32(core + SAS_S_VOICES + o));
    psp_write32(core + SAS_S_TAIL, end_flags());
}

/* What a core leaves: the snapshot, taken after the core has taken up its
 * key-ons and key-offs, and three of +004's bits cleared (see SAS_S_SIZE).
 * The first core after the layout setters changes exactly the snapshot's
 * 38 words that differ, a second core nothing, and a key-off with four
 * cores after it the key byte in both copies and the two end-flag words
 * (sasprobe 4 steps 357-359, fw 6.60). */
static void s_core(void) {
    if (core_bad()) return;
    s_snapshot();
    g_s_header_flags &= ~SH_CORE_CLEARS;
    psp_write32(psp_arg(0) + 0x004u, g_s_header_flags);
}

/* Init's image (sasprobe step 4, fw 6.60): every voice as reset_voices left
 * it, in both copies, and the header for this grain and output mode. A
 * second Init on a 0xCC-filled struct writes the same 904 words (step 5). */
static void s_init(void) {
    const uint32_t core = psp_arg(0);
    g_s_header_flags = 0xBFFu;
    psp_write32(core + 0x000u, 0x00180990u);
    s_header();
    psp_write32(core + 0x00Cu, 0);
    for (uint32_t vi = 0; vi < SAS_VOICES; vi++) {
        const uint32_t a = s_voice(vi);
        psp_write32(a + SV_SRC, 0);
        psp_write32(a + SV_SRC_END, 0);
        s_flags(vi);
        s_volumes(vi);
        psp_write32(a + SV_ENGINE, 0x00180010u + vi * 0x4Cu);
        s_adsr(vi);
        psp_write32(a + SV_LAST, 0);
    }
    s_snapshot();
    psp_write32(core + SAS_S_TAIL + 4, 0);
    psp_write32(core + SAS_S_TAIL + 8, 0);
}

/* ---- the calls ----------------------------------------------------------- */

static sas_voice *voice_arg(void) {
    uint32_t i = psp_arg(1);
    return (i < SAS_VOICES) ? &g_voice[i] : NULL;
}

static void hle_Init(void) {
    /* (sasCore, grain, maxVoices, outputMode, sampleRate). Checked core,
     * grain, then the rate, then the voice count and mode: firmware 6.60
     * answers the rate's 80420004 to (256, 0 voices, mode 2, rate 0) and to
     * (256, 32, mode 2, rate 0) and (256, 0 voices, mode 0, 48000) (sasprobe
     * step 12). Which of the voice count and the mode is checked first is not
     * measured. */
    const uint32_t core = psp_arg(0), grain = psp_arg(1), voices = psp_arg(2),
                   mode = psp_arg(3), rate = psp_arg(4);
    if (!core || (core & 63))          { psp_ret(SAS_ERROR_CORE); return; }
    if (!grain_ok(grain))              { psp_ret(SAS_ERROR_GRAIN); return; }
    /* 44100 and nothing else. The accepted list this once read as "the two
     * rates this renders at" is not hardware's: sascore.expected refuses
     * 48000 along with every other rate it tries, and so does firmware 6.60
     * (sasprobe step 10). */
    if (rate != 44100) { psp_ret(SAS_ERROR_SAMPLE_RATE); return; }
    /* Checked, and then not kept: the count does not limit the voices
     * rendered (see render). */
    if (voices < 1 || voices > SAS_VOICES) { psp_ret(SAS_ERROR_MAX_VOICES); return; }
    if (mode > 1)                      { psp_ret(SAS_ERROR_OUTPUT_MODE); return; }
    /* Init starts every voice over, whatever it was doing: a voice keyed on
     * and playing reads height 0 and ended afterwards, its key is gone
     * (KeyOff is refused), the next core is silent, and a paused voice is
     * unpaused (sasprobe steps 15 and 222, fw 6.60). Every Init rewrites the
     * whole struct with the defaults (steps 4-5; s_init). */
    reset_voices();
    /* The effect goes back to off with its lines silent. That every capture
     * after an Init and a RevType starts from silence is measured (steps
     * 321-333: "0 samples not silent" before each key-on, after the long
     * tails of the type before); which of the two calls clears it, and what
     * EVOL and VON read after an Init, is not. */
    rev_reset();
    g_grain       = grain;
    g_output_mode = mode;
    g_sample_rate = rate;
    s_init();
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* Writes +008 alone: SetGrain(512) and (256) change nothing else (sasprobe
 * 4 steps 37-38, fw 6.60). +004 already had 0x200 up then, so whether a
 * grain sets that bit as SetOutputmode does is not measured; here it does
 * not. */
static void hle_SetGrain(void) {
    const uint32_t grain = psp_arg(1);
    if (!grain_ok(grain)) { psp_ret(SAS_ERROR_GRAIN); return; }
    g_grain = grain;
    s_header();
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_GetGrain(void) { psp_ret(g_grain); }

/* outputmode.expected: 0 and 1 accepted, everything else 80420003, and
 * GetOutputmode answers the mode set. What the mode changes about the mix
 * is not modelled here. In the struct it sets +004's 0x200, whichever mode
 * it is, and the mode bit in +008 (sasprobe 4 steps 35-36, fw 6.60). */
static void hle_SetOutputmode(void) {
    const uint32_t mode = psp_arg(1);
    if (mode > 1) { psp_ret(SAS_ERROR_OUTPUT_MODE); return; }
    g_output_mode = mode;
    g_s_header_flags |= 0x200u;
    s_header();
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_GetOutputmode(void) { psp_ret(g_output_mode); }

static void hle_SetVoice(void) {
    /* (sasCore, voice, vagAddr, size, loopmode). The size is checked before
     * the voice index: voice 32 with size 0 gets the size's 80420014 on
     * firmware 6.60 (sasprobe step 42). Where the loop mode's check falls
     * against the voice index's is not measured. */
    const uint32_t size = psp_arg(3);
    if (size == 0 || (size & 15)) { psp_ret(SAS_ERROR_SIZE); return; }
    sas_voice *v = voice_arg();
    if (!v) { psp_ret(SAS_ERROR_VOICE); return; }
    /* The fifth argument is a loop *mode* here, not the sample position
     * __sceSasSetVoicePCM takes: vag.expected accepts 0 and 1 and refuses
     * everything else, -1 included, with the same code that call uses for a
     * bad position. */
    if (psp_arg(4) > 1u) { psp_ret(SAS_ERROR_LOOP_POS); return; }
    /* Played from the next key-on, not now (see sas_voice). The old stream
     * does not take up the new size or loop mode at its own loop point
     * either: after a SetVoice to a 2-block one-shot, a 4-block loop goes
     * on through its loop point five more times until a KeyOff and KeyOn two
     * cores later, which fade it out and then play the new blocks once
     * (sasprobe 3 step 179, fw 6.60; step 188 is the same for SetVoicePCM). */
    v->next.kind = SRC_VAG;
    v->next.addr = psp_arg(2);
    v->next.size = size;
    v->next.loop = (int32_t)psp_arg(4);
    /* In the struct, the address, where it ends and the flags: vag+0, vag+50
     * and 0x001, or 0x101 looping (sasprobe 4 steps 18-19, fw 6.60). */
    s_source(psp_arg(1), psp_arg(2), psp_arg(2) + size, SF_SAMPLE | (psp_arg(4) ? SF_LOOP : 0u));
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
    /* Played from the next key-on, not now (see sas_voice). */
    v->next.kind = SRC_PCM;
    v->next.addr = psp_arg(2);
    v->next.size = (uint32_t)size;
    v->next.loop = loop;
    /* In the struct, the address, loop << 16 | size - 1, and flags 0x105: 100
     * samples looping at 50 read pcm0+0, 00320063 and 10000105 (sasprobe 4
     * step 20, fw 6.60). Without a loop it is taken as the same halfwords
     * and no 0x100, which is not measured. */
    s_source(psp_arg(1), psp_arg(2), (uint32_t)(uint16_t)loop << 16 | (uint16_t)(size - 1),
             SF_SAMPLE | SF_PCM | (loop >= 0 ? SF_LOOP : 0u));
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_SetPitch(void) {
    sas_voice *v = voice_arg();
    if (!v) { psp_ret(SAS_ERROR_VOICE); return; }
    if (psp_arg(2) > 0x4000u) { psp_ret(SAS_ERROR_PITCH); return; }
    v->pitch = psp_arg(2);
    s_flags(psp_arg(1));   /* 10000001 to 12340001 (sasprobe 4 step 21, fw 6.60) */
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* __sceSasSetNoise(sasCore, voice, freq): the voice plays noise instead of
 * its sample (see noise_advance), frequency 0..63. Like SetVoice it names
 * what the next key-on plays, and a SetVoicePCM after it goes back to the
 * sample (sasprobe step 200). A voice already playing noise takes the new
 * frequency at once, without a key-on (sasprobe 3 step 279, fw 6.60); what
 * it does to a voice playing a sample is not measured, and that waits for
 * the key-on here.
 *
 * In the struct, EAEAEAEA, 400 and flags 0x002 at frequency 17 (sasprobe 4
 * step 29, fw 6.60). The 400 is taken as the clock's half-tick spacing less
 * one, 2^(14-4) at 17; other frequencies are not measured. */
static void hle_SetNoise(void) {
    sas_voice *v = voice_arg();
    if (!v) { psp_ret(SAS_ERROR_VOICE); return; }
    if (psp_arg(2) > 63u) { psp_ret(SAS_ERROR_NOISE_FREQ); return; }
    v->next.kind  = SRC_NOISE;
    v->next.param = psp_arg(2);
    if (v->src.kind == SRC_NOISE) v->src.param = psp_arg(2);
    s_source(psp_arg(1), 0xEAEAEAEAu, (uint32_t)noise_half(psp_arg(2)) - 1u, SF_NOISE);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* __sceSasSetSteepWave / __sceSasSetTrianglarWave(sasCore, voice, duty): the
 * voice plays a wave (see wave_fetch). A duty outside 0..100 is refused,
 * -1 included (sasprobe step 229). Like SetNoise, what the next key-on
 * plays; on a playing voice that is not measured. What they write to the
 * struct is not measured either, and here they write nothing. */
static void set_wave(int kind) {
    sas_voice *v = voice_arg();
    if (!v) { psp_ret(SAS_ERROR_VOICE); return; }
    if (psp_arg(2) > 100u) { psp_ret(SAS_ERROR_WAVE_DUTY); return; }
    v->next.kind  = kind;
    v->next.param = psp_arg(2);
    psp_ret(SCE_KERNEL_ERROR_OK);
}
static void hle_SetSteepWave(void)      { set_wave(SRC_STEEP); }
static void hle_SetTrianglarWave(void)  { set_wave(SRC_TRIANGLE); }

static void hle_SetVolume(void) {
    /* (sasCore, voice, l, r, el, er) -- the last two are the reverb sends.
     * All four are bounded at plus or minus unity and all four are checked,
     * whether or not this renderer uses them. The bound is on the magnitude,
     * taken the way abs() takes it: firmware 6.60 refuses 0x1001, -0x1001
     * and 0x7FFFFFFF in each of the four places but accepts 0x80000000,
     * whose negation is itself and still negative (sasprobe step 49).
     *
     * Each is kept as 16 bits, two to a word of the voice's struct (0x111,
     * 0x222 read back as 02220111, sasprobe 4 step 22), so 0x80000000 is
     * kept as 0 and plays silence: a constant 1000 or -1000 comes out 0 on
     * that side and in that send, with the struct word reading 10000000
     * (sasprobe 3 steps 290-291, fw 6.60; s_volumes). */
    sas_voice *v = voice_arg();
    if (!v) { psp_ret(SAS_ERROR_VOICE); return; }
    for (int i = 2; i <= 5; i++) {
        const uint32_t vol = psp_arg(i);
        const int32_t mag = (int32_t)((vol & 0x80000000u) ? 0u - vol : vol);
        if (mag > 0x1000) { psp_ret(SAS_ERROR_VOLUME); return; }
    }
    v->vol_l  = (int16_t)psp_arg(2);
    v->vol_r  = (int16_t)psp_arg(3);
    v->vol_el = (int16_t)psp_arg(4);
    v->vol_er = (int16_t)psp_arg(5);
    s_volumes(psp_arg(1));
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* The ADSR setters write the voice's ADSR block back into the caller's
 * struct, so a read straight after a setter sees what hardware would have
 * left there. */
static void mirror_adsr(const sas_voice *v) { s_adsr((uint32_t)(v - g_voice)); }

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
    if (flags & 4) v->mode_sustain = su & 7u;
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

/* __sceSasSetSL(sasCore, voice, level): the sustain level on its own. A level
 * above the envelope's top, 0x40000000, is refused as an ADSR value and
 * nothing is stored; the comparison is unsigned, so 0x80000000 and -1 are
 * refused too (sasprobe step 51, fw 6.60). */
static void hle_SetSL(void) {
    sas_voice *v = voice_arg();
    if (!v) { psp_ret(SAS_ERROR_VOICE); return; }
    if (psp_arg(2) > (uint32_t)ENV_MAX) { psp_ret(SAS_ERROR_ADSR_VALUE); return; }
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
    /* The key goes down now; the voice starts at the next core (see `on`). */
    v->on = 1;
    v->keyon_pending = 1;
    v->keyoff_pending = 0;
    s_key(psp_arg(1));
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* A voice whose key is not down has nothing to lift, and keyoff.expected
 * refuses that with the same code an already-on key-on gets -- including
 * while paused. A paused voice whose key *is* down is refused as well, and
 * keeps its key: firmware 6.60 answers 80420016 and, once resumed, the voice
 * is still at full height, with no release (sasprobe step 219).
 *
 * The key comes up here and now. Only the *release* waits for the next core:
 * pcm.expected and vag.expected both key a voice off and straight back on
 * without a core between, and hardware restarts it, which it could not do if
 * the key were still down. Holding `on` until the core made the second
 * key-on fail and left the voice playing the previous section's sound. */
static void hle_SetKeyOff(void) {
    sas_voice *v = voice_arg();
    if (!v) { psp_ret(SAS_ERROR_VOICE); return; }
    if (!v->on || v->paused) { psp_ret(SAS_ERROR_ALREADY_ON); return; }
    v->on = 0;
    v->keyoff_pending = 1;
    s_key(psp_arg(1));
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* Each voice in the mask has 0x10 of its struct flags set or cleared: voice
 * 2's 10000105 reads 10000115 paused and 10000105 again resumed (sasprobe 4
 * steps 33-34, fw 6.60). */
static void hle_SetPause(void) {
    /* (sasCore, voiceBitmask, pause) */
    uint32_t mask = psp_arg(1);
    int pause = (int)psp_arg(2);
    for (int i = 0; i < SAS_VOICES; i++)
        if (mask & (1u << i)) {
            g_voice[i].paused = pause;
            if (!pause) g_voice[i].pause_faded = 0;
            s_flags((uint32_t)i);
        }
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
    psp_ret(end_flags());
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

/* ---- reverb -------------------------------------------------------------
 *
 * The wet signal the two send volumes feed. Everything below is fitted to
 * sasprobe 3's impulse responses on firmware 6.60 -- a 32767 pulse through
 * each of the nine types, 64 cores each (steps 321-331) -- and reproduces
 * every one of those eleven captures bit for bit, and run 1's and run 4's
 * hall burst (step 318, 8000 at RevParam(64, 64)) as well. It is a model
 * that computes the same numbers, not a description of the firmware's
 * buffer layout, and nothing in it comes from anywhere but those captures.
 *
 * Half rate: each pair of frames is one step, taking the sends of the odd
 * frame and adding the wet sample to the even one; the odd frames get
 * nothing. A one-frame pulse on an odd frame gives exactly the two-frame
 * pulse's response, one on an even frame gives silence (steps 332-333), and
 * no odd frame of any capture is touched. The pairs run from the start of
 * each core, whose grain is always even.
 *
 * Per side, in 16-bit steps with every product floored (>> 15):
 *
 *   in    = send * 7936 >> 15                (32767 -> 7935, 8000 -> 1937)
 *   line  X(t) = ((acc - P) * I >> 15) + P,  acc = in + (X(t - wall) * W >> 15),
 *                                            P = X(t - prev)
 *         one or two lines (A, B) a side, each fed by its own side's send
 *   comb  c = sum(V_k * X_line(t - tau_k)) >> 15, summed before the shift
 *   APF   r = w(t - D); w = x - (G * r >> 15); y = (G * w >> 15) + r,
 *         APF1 (D1 per side, G1) and then APF2 (D2, G2)
 *   out   y * 31 * EVOL >> 16                (y * 31 / 16 at EVOL 0x1000)
 *
 * The gains are those the data leaves. Pipe's all, and hall's I, W, V0, G1
 * and G2, are the only values that fit; every other is one of a few
 * neighbours that all fit exactly. Of those, G1 is taken as 2 * D2, which
 * is among them for all seven types that have an APF2, and the rest as the
 * one with the most low zero bits (medium's I also fits at a few far-off
 * values; 28912, shared with small and pipe, is kept).
 *
 * Echo and delay are one type as far as the data goes -- identical captures
 * at RevParam(16, 64) -- and are the only ones RevParam moves: a single line
 * a side through which the pulse comes back every 16d + 4 steps, scaled by
 * W = -256 * feedback (-16384 exactly at 64; nothing at 0), read at 16d + 7,
 * with a faint APF1 (D1 16d + 24 left, 16d + 20 right, G1 8 -- 5 to 8 all
 * fit) and no APF2. Delays 16 and 8, feedback 64 and 0, all exact. The hall
 * burst at RevParam(64, 64) is what the (16, 64) fit gives, so the other
 * types are taken not to use it.
 *
 * Not measured, and chosen here: which side's send feeds which line (the
 * captures fed both the same); EVOL other than 0x1000 (taken as a straight
 * product); VON, which only gates the wet signal here and leaves the dry
 * mix alone (every capture used VON(1, 1)); the defaults after an Init
 * (off, EVOL 0, VON 0); clamping of a send past 16
 * bits (clamped); output mode 1, which writes the sends out raw and gets no
 * wet signal here. */

#define REV_BUF 4096u   /* a power of two past the longest delay, space's 2784 */

typedef struct { int16_t wall, prev; } rev_line;
typedef struct { uint8_t line, gain; int16_t delay; } rev_tap;   /* line A 0, B 1 */

typedef struct {
    int32_t iir, wall_gain;       /* I and W */
    int32_t tap_gain[4];          /* V0..V3 */
    int32_t g1, g2;
    int16_t d1[2], d2;            /* d2 0: no APF2 */
    uint8_t nlines, ntaps;        /* per side */
    rev_line line[2][2];          /* [side][A, B] */
    rev_tap  tap[2][4];           /* [side][k] */
} rev_type;

/* Types 0-5 and 8, from steps 321-326 and 329. Echo and delay (6, 7) are
 * built from RevParam by rev_params. */
static const rev_type REV_TYPES[9] = {
    [0] = { /* room */
        28032, -17792, { 21688, -16688, 0, 0 }, 182, 21248, { 436, 310 }, 91, 1, 2,
        { { { 416, 416 } }, { { 380, 380 } } },
        { { { 0, 0, 230 }, { 0, 1, 354 } }, { { 0, 0, 268 }, { 0, 1, 324 } } } },
    [1] = { /* small */
        28912, -25600, { 17424, -16144, 20392, -17184 }, 74, 20160, { 180, 128 }, 37, 2, 4,
        { { { 205, 205 }, { 200, 200 } }, { { 217, 217 }, { 190, 190 } } },
        { { { 0, 0, 45 }, { 0, 1, 138 }, { 1, 2, 64 }, { 1, 3, 114 } },
          { { 0, 0, 89 }, { 0, 1, 215 }, { 1, 2, 108 }, { 1, 3, 181 } } } },
    [2] = { /* medium */
        28912, -19264, { 17680, -16656, 20392, -17184 }, 254, 20160, { 612, 434 }, 127, 2, 4,
        { { { 445, 445 }, { 408, 408 } }, { { 457, 457 }, { 382, 382 } } },
        { { { 0, 0, 221 }, { 0, 1, 394 }, { 1, 2, 224 }, { 1, 3, 354 } },
          { { 0, 0, 297 }, { 0, 1, 375 }, { 1, 2, 268 }, { 1, 3, 341 } } } },
    [3] = { /* large */
        28512, -22912, { 17680, -16656, 20392, -17184 }, 338, 21184, { 796, 568 }, 169, 2, 4,
        { { { 751, 751 }, { 674, 674 } }, { { 716, 716 }, { 638, 638 } } },
        { { { 0, 0, 237 }, { 0, 1, 490 }, { 1, 2, 242 }, { 1, 3, 546 } },
          { { 0, 0, 313 }, { 0, 1, 535 }, { 1, 2, 284 }, { 1, 3, 485 } } } },
    [4] = { /* hall */
        24576, -16384, { 20480, 19456, -18432, -17408 }, 626, 23552, { 1472, 1050 }, 313, 2, 4,
        { { { 1018, 1022 }, { 1022, 1022 } }, { { 1016, 1018 }, { 1024, 1024 } } },
        { { { 0, 0, 248 }, { 0, 1, 1022 }, { 1, 2, 508 }, { 1, 3, 960 } },
          { { 0, 0, 254 }, { 0, 1, 1018 }, { 1, 2, 512 }, { 1, 3, 756 } } } },
    [5] = { /* space */
        32256, -20480, { 20480, -19456, -20480, 19456 }, 1122, 21504, { 2784, 1954 }, 561, 2, 4,
        { { { 1188, 1188 }, { 1432, 1432 } }, { { 1090, 1090 }, { 1396, 1396 } } },
        { { { 0, 0, 450 }, { 0, 1, 788 }, { 1, 2, 698 }, { 1, 3, 1016 } },
          { { 0, 0, 502 }, { 0, 1, 895 }, { 1, 2, 296 }, { 1, 3, 1016 } } } },
    [8] = { /* pipe: the only type whose lines read P at another delay than W */
        28912, -31488, { 17680, -16656, 20392, -17184 }, 38, 21696, { 88, 64 }, 19, 2, 4,
        { { { 54, 183 }, { 25, 193 } }, { { 59, 197 }, { 69, 216 } } },
        { { { 0, 0, 169 }, { 0, 1, 183 }, { 1, 2, 140 }, { 1, 3, 193 } },
          { { 0, 0, 109 }, { 0, 1, 197 }, { 1, 2, 208 }, { 1, 3, 216 } } } },
};

static struct {
    int32_t  type;                /* -1: off */
    uint32_t delay, feedback;
    uint32_t evol_l, evol_r;
    uint32_t von_dry, von_wet;
    uint32_t t;                   /* steps taken since the lines were cleared */
    int32_t  line[2][2][REV_BUF]; /* [side][A, B] */
    int32_t  apf1[2][REV_BUF], apf2[2][REV_BUF];
} g_rev;

static void rev_clear(void) {
    memset(g_rev.line, 0, sizeof g_rev.line);
    memset(g_rev.apf1, 0, sizeof g_rev.apf1);
    memset(g_rev.apf2, 0, sizeof g_rev.apf2);
    g_rev.t = 0;
}

static void rev_reset(void) {
    g_rev.type = -1;
    g_rev.delay = g_rev.feedback = 0;
    g_rev.evol_l = g_rev.evol_r = 0;
    g_rev.von_dry = g_rev.von_wet = 0;
    rev_clear();
}

static rev_type rev_params(void) {
    if (g_rev.type != 6 && g_rev.type != 7) return REV_TYPES[g_rev.type];
    const int16_t d = (int16_t)(16 * g_rev.delay);
    const rev_type e = {
        32768, -256 * (int32_t)g_rev.feedback, { 32768, 0, 0, 0 }, 8, 0,
        { (int16_t)(d + 24), (int16_t)(d + 20) }, 0, 1, 1,
        { { { (int16_t)(d + 4), (int16_t)(d + 4) } }, { { (int16_t)(d + 4), (int16_t)(d + 4) } } },
        { { { 0, 0, (int16_t)(d + 7) } }, { { 0, 0, (int16_t)(d + 7) } } } };
    return e;
}

static int64_t rev_apf(int32_t *w, uint32_t t, int d, int32_t g, int64_t x) {
    const int64_t r = w[(t - (uint32_t)d) & (REV_BUF - 1)];
    const int64_t v = x - shr_floor(g * r, 15);
    w[t & (REV_BUF - 1)] = (int32_t)v;
    return shr_floor(g * v, 15) + r;
}

/* One step of one side: the send in, the APFs' output out. */
static int64_t rev_side(const rev_type *p, int s, int32_t send, uint32_t t) {
    const uint32_t m = REV_BUF - 1;
    const int64_t in = shr_floor((int64_t)send * 7936, 15);
    for (int k = 0; k < p->nlines; k++) {
        int32_t *x = g_rev.line[s][k];
        const int64_t acc  = in + shr_floor((int64_t)x[(t - (uint32_t)p->line[s][k].wall) & m] *
                                            p->wall_gain, 15);
        const int64_t prev = x[(t - (uint32_t)p->line[s][k].prev) & m];
        x[t & m] = (int32_t)(shr_floor((acc - prev) * p->iir, 15) + prev);
    }
    int64_t sum = 0;
    for (int k = 0; k < p->ntaps; k++) {
        const rev_tap *tp = &p->tap[s][k];
        sum += (int64_t)g_rev.line[s][tp->line][(t - (uint32_t)tp->delay) & m] *
               p->tap_gain[tp->gain];
    }
    int64_t y = rev_apf(g_rev.apf1[s], t, p->d1[s], p->g1, shr_floor(sum, 15));
    if (p->d2) y = rev_apf(g_rev.apf2[s], t, p->d2, p->g2, y);
    return y;
}

/* The wet signal of one core, added to the dry mix `l`/`r` from the sends. */
static void rev_render(int64_t *l, int64_t *r, const int64_t *el, const int64_t *er,
                       uint32_t n) {
    if (g_rev.type < 0) return;
    const rev_type p = rev_params();
    for (uint32_t i = 0; i + 1 < n; i += 2) {
        const uint32_t t = g_rev.t++;
        const int64_t yl = rev_side(&p, 0, clamp16(el[i + 1]), t);
        const int64_t yr = rev_side(&p, 1, clamp16(er[i + 1]), t);
        if (!g_rev.von_wet) continue;
        l[i] += shr_floor(yl * 31 * (int64_t)g_rev.evol_l, 16);
        r[i] += shr_floor(yr * 31 * (int64_t)g_rev.evol_r, 16);
    }
}

/* `mix_l`/`mix_r` scale what is already in the buffer, not what is rendered
 * into it: outputmode.expected's mix sections pass 0 for both and get the
 * rendered samples back unchanged, which is only possible if the zero applies
 * to the other side. The scale is 12-bit, as everywhere else here: firmware
 * 6.60 turns a buffer of 1000/-2000 into 500/-500 at 0x800/0x400, and adds a
 * voice of 2000 to it to make 2500/1000 at 0x800 (sasprobe steps 207-208). */
static void mix_to_guest(uint32_t out_addr, int add, int32_t mix_l, int32_t mix_r) {
    int64_t l[SAS_MAX_GRAIN], r[SAS_MAX_GRAIN], el[SAS_MAX_GRAIN], er[SAS_MAX_GRAIN];
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
        const int64_t *block[4] = { l, r, el, er };
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

    rev_render(l, r, el, er, n);
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

/* A null core is refused by __sceSasCore as by Init, with 80420005 (sasprobe
 * 3 step 355, fw 6.60). The unaligned case Init also refuses is assumed the
 * same here (core_bad), and CoreWithMix is given the same check; neither is
 * measured. A core that renders writes the struct (s_core). */
static void hle_Core(void) {
    /* (sasCore, sampleBuffer) */
    uint32_t out = psp_arg(1);
    if (core_bad()) { psp_ret(SAS_ERROR_CORE); return; }
    if (out) { mix_to_guest(out, 0, 0, 0); s_core(); }
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* (sasCore, sampleBuffer, leftMix, rightMix). Refused outright in output mode
 * 1 -- outputmode.expected's last section gets 0x80000004 and a buffer
 * nothing has touched. A mix above 0x1000 on either side, compared unsigned,
 * is refused with the volume code and the buffer is left as it was: -1,
 * 0x1001, 0x2000 and 0x7FFFFFFF all are on firmware 6.60 (sasprobe step 207).
 * Nothing is rendered then either, and the voices do not advance: a voice
 * playing a ramp picks up after a refused call exactly where it left off
 * (sasprobe 3 steps 295-296, fw 6.60). */
static void hle_CoreWithMix(void) {
    uint32_t out = psp_arg(1);
    if (core_bad()) { psp_ret(SAS_ERROR_CORE); return; }
    if (g_output_mode != 0) { psp_ret(SAS_ERROR_MIX_MODE); return; }
    if (psp_arg(2) > 0x1000u || psp_arg(3) > 0x1000u) { psp_ret(SAS_ERROR_VOLUME); return; }
    if (out) { mix_to_guest(out, 1, (int32_t)psp_arg(2), (int32_t)psp_arg(3)); s_core(); }
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* Reverb: the four calls check their arguments as firmware 6.60 does, and
 * set what rev_render plays (see the reverb section above).
 *
 * __sceSasRevType(sasCore, type): -1..8, signed (step 224). A new type
 * starts from silent lines; the same type again is taken to leave them be. */
static void hle_RevType(void) {
    const int32_t type = (int32_t)psp_arg(1);
    if (type < -1 || type > 8) { psp_ret(SAS_ERROR_REV_TYPE); return; }
    if (type != g_rev.type) rev_clear();
    g_rev.type = type;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* __sceSasRevParam(sasCore, delay, feedback): both unsigned, the delay
 * checked first, and both 0..127, though PSPSDK's header allows 0..128:
 * delay 128, 129 and -1 are refused, and so are feedback 128 (alone, as
 * (0, 128) and (127, 128)), 129 and -1 (sasprobe step 225, and sasprobe 3
 * step 319, fw 6.60). */
static void hle_RevParam(void) {
    if (psp_arg(1) > 127u) { psp_ret(SAS_ERROR_REV_DELAY); return; }
    if (psp_arg(2) > 127u) { psp_ret(SAS_ERROR_REV_FEEDBACK); return; }
    g_rev.delay    = psp_arg(1);
    g_rev.feedback = psp_arg(2);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* __sceSasRevEVOL(sasCore, left, right): each at most 0x1000, unsigned, so
 * every negative volume is refused too -- unlike SetVolume's (step 226). */
static void hle_RevEVOL(void) {
    if (psp_arg(1) > 0x1000u || psp_arg(2) > 0x1000u) { psp_ret(SAS_ERROR_REV_VOLUME); return; }
    g_rev.evol_l = psp_arg(1);
    g_rev.evol_r = psp_arg(2);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* __sceSasRevVON(sasCore, dry, wet): anything is accepted (step 227). */
static void hle_RevVON(void) {
    g_rev.von_dry = psp_arg(1);
    g_rev.von_wet = psp_arg(2);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

void psp_sas_register(void) {
    PSP_STATE_KEEP(g_voice);            /* a save state's (psprecomp/state.h) */
    PSP_STATE_KEEP(g_grain);
    PSP_STATE_KEEP(g_output_mode);
    PSP_STATE_KEEP(g_sample_rate);
    PSP_STATE_KEEP(g_s_header_flags);
    PSP_STATE_KEEP(g_rev);
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
    /* NIDs from PSPSDK's stub library, libpspsascore.a. */
    psp_hle_register(0xD5EBBBCD, "sceSasCore", "__sceSasSetSteepWave",      hle_SetSteepWave);
    psp_hle_register(0xA232CBE6, "sceSasCore", "__sceSasSetTrianglarWave",  hle_SetTrianglarWave);
    psp_hle_register(0x33D4AB37, "sceSasCore", "__sceSasRevType",           hle_RevType);
    psp_hle_register(0x267A6DD2, "sceSasCore", "__sceSasRevParam",          hle_RevParam);
    psp_hle_register(0xD5A229C9, "sceSasCore", "__sceSasRevEVOL",           hle_RevEVOL);
    psp_hle_register(0xF983B186, "sceSasCore", "__sceSasRevVON",            hle_RevVON);
}
