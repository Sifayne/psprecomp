# sasprobe

A PSP homebrew program, built from PSPSDK (BSD) alone, that measures what the
firmware's sceSasCore voice mixer does. Each step re-measures a rule that
`src/hle/sascore.c` states as a hardware fact, and the comment above it in
`main.c` names the line. It covers:

- **init, args, grain**: every argument check, with values just inside and
  just outside each limit, and which bad argument wins when several are
  bad. Also whether Init resets a playing or paused voice, and a voice's
  defaults after Init.
- **layout**: the words of the caller's SasCore struct that each setter
  writes, and where the ADSR fields sit.
- **adsrmode, simpleadsr**: the curve matrix (which curve each phase
  accepts), and every field of `__sceSasSetSimpleADSR` as read back from the
  struct.
- **adsr**: 60 envelope sweeps covering each curve in each phase at several
  rates. Each logs the height after every core.
- **keys, pause, endflag, heights**: key on/off rules, the 32-sample key-on
  delay, the release timing, the pause and end-flag bits, and
  `__sceSasGetAllEnvelopeHeights`.
- **envscale, vag, vagflags, pcm, pitch, noise, outmode, mix**: rendered
  audio. This covers the height and volume gain, the 16 ADPCM filters and 16
  shifts, block flags and loops, PCM loop positions, pitch steps, noise,
  output modes 0 and 1, and `__sceSasCoreWithMix`.
- **reverb, waves**: argument checks, the impulse response of every effect
  type, and the waves at several duties and pitches. psprecomp plays the
  waves but renders no wet signal.
- **badptr**: null pointers, last of all.

The run should take about a minute. Every render uses grain 256 at 44100 Hz
unless the test is about those, one voice unless it says otherwise, and a
flat envelope.

### Version 3

Version 3 adds steps at the end of their sections for what run 1 (firmware
6.60) left open. The older steps and their titles are unchanged, so both
runs can be compared.

| Section | New steps | What they settle |
| --- | --- | --- |
| init | maxVoices 1 with voices 0, 8, 16 and 31 keyed on | whether every voice renders whatever the count, or the count is rounded up |
| adsr | six "sustain from SL" sweeps: a direct decay puts the height at SL, then lin-dec 0x40000 and 0x30000, exp-rev 0x1000000 and 0, direct 0 and 0x7FFFFFFF | falling sustain curves from a nonzero height. Run 1's sustain sweeps all started at 0 |
| vagflags, pcm | SetVoice or SetVoicePCM to a shorter one-shot while a loop plays, then KeyOff+KeyOn | whether the old stream takes up the new size or loop mode, and whether a key-on starts the new data |
| pitch | pitch 0x100 on a falling ramp with steps of -4, -3, -7 and +5 | how the interpolation rounds on a falling slope |
| noise | the first 12 ticks at each of the 64 frequencies. Re-keys after 1, 2 and 3 cores, after a SetNoise, and after a core with the voice off. SetNoise with no key-on. Two voices together, at two frequencies, and a core apart | the tick clock at every frequency, what restarts the clock and the register, and whether each voice has its own generator |
| outmode | SetVolume 0x80000000 as dry and as send, in modes 0 and 1, and the volume words | what that accepted volume is kept as and sounds like |
| mix | a CoreWithMix refused for its mix, and one refused in mode 1, between two cores of a ramp | whether a refused call still moves the voices on |
| pause | constants 32767 and -32768 paused, with R at half volume. A pitch-0x800 ramp paused a core later | the pause fade's law, its rounding, whether it comes before or after the volume, and that it starts from the sample due |
| reverb | RevParam with feedback 128, delay 127 and delay 128 on their own. The dry pulse. A 2-frame pulse through each of the 9 types (64 cores). Echo again with delay 8, and with feedback 0. The hall with a one-frame impulse on an odd frame and on an even one | the RevParam limits, each type's impulse response for fitting the wet signal, the delay and feedback parameters, and how the half-rate engine takes its input |
| waves | steep and triangular waves at duty 0, 25, 75 and 100 (pitch 441), duty 50 at pitches 1000 and 150, and duty 25 at pitch 150 | the other duties' shapes, and whether the triangle's last-quarter offset depends on the pitch |

## Build

Needs the pspdev toolchain (`psp-gcc`, `psp-config` and PSPSDK) on `PATH`:

    make

This produces `EBOOT.PBP` and `sasprobe.prx`. The CMake build does not
include it.

## Run

Copy `EBOOT.PBP` to `ms0:/PSP/GAME/sasprobe/` and start it from the XMB. It
needs no input. It loads `PSP_MODULE_AV_AVCODEC` and `PSP_MODULE_AV_SASCORE`
itself, and returns to the XMB when done.

The log, `sasprobe.txt` beside the EBOOT, is only appended to, and it is
saved before every step. If the PSP switches off, the log's last line names
the step that did it.

The same PRX runs under psprecomp:

    allegrexrecomp interp sasprobe.prx --dispatch --budget 4000000000 --drain 200

## Audio files

The probe also writes these files beside the EBOOT, about 650 KB in all
(version 2 wrote about 400 KB). Each is raw signed 16-bit little-endian
audio, one core after another, starting with the first core after the
key-on. Frames are stereo L,R, with two exceptions. In `outmode1.bin`, each
core is four mono blocks of 256 samples: dry L, dry R, send L, send R. The
`rev_imp_*.bin` files keep only the even frames (frames 0, 2, 4, ...) as L,R
pairs, and the log counts the odd frames that were not silent. The log gives
each file's checksum and first samples.

| File | What plays |
| --- | --- |
| `vag_filter00.bin` .. `vag_filter15.bin` | ADPCM filter 0..15. The blocks are an impulse, zeros, nibbles 0..15, all 7s, then an end block. 2 cores. |
| `vag_shifts.bin` | Shifts 0..15 with filter 0, one block each. 3 cores. |
| `vag_pitch0800.bin`, `vag_pitch2000.bin` | A VAG triangle wave at pitch 0x800 and 0x2000. |
| `vag_flags00.bin` .. `vag_flags18.bin` | Block-flag cases, in the order the log lists them. Block k is a DC level of 256*(k+1). 4 cores. |
| `vag_setvoice_playing.bin` | SetVoice to other data mid-play. |
| `pcm_loop_none.bin`, `pcm_loop0.bin`, `pcm_loop50.bin`, `pcm_loop99.bin`, `pcm_size1_loop0.bin`, `pcm_loop_m2.bin` | PCM loop positions. Sample i is 16*(i+1). |
| `pitch_XXXX.bin`, `pitch_change.bin` | A PCM ramp, sample i = 4*(i+1), at pitch 0xXXXX, and with the pitch changed between cores. |
| `noise_f00.bin` .. `noise_f63.bin` | `__sceSasSetNoise` at frequency 0, 1, 8, 16, 32, 48 and 63. 4 cores. |
| `env_ramp_7fff.bin`, `env_ramp_m8000.bin` | A linear attack on a full-scale constant. |
| `outmode0.bin`, `outmode1.bin` | The same voice in output mode 0 and mode 1, at volumes 0x1000/0xC00/0x800/0x400. |
| `pause_ramp.bin` | A ramp that is paused for two cores, then resumed. |
| `rev_hall.bin` | A 100-sample burst with the hall reverb. 16 cores. |
| `wave_steep50.bin`, `wave_tri50.bin` | Steep and triangular waves. |
| `vag_setvoice_rekey.bin`, `pcm_setvoice_rekey.bin` | v3: SetVoice or SetVoicePCM mid-play, then KeyOff+KeyOn. 6 and 5 cores. |
| `pitch_0100_fall.bin` | v3: the falling ramp at pitch 0x100. 4 cores. |
| `rev_imp_t0.bin` .. `rev_imp_t8.bin`, `rev_imp_t6_d8.bin`, `rev_imp_t6_fb0.bin` | v3: the wet signal alone, for a 2-frame pulse of 32767 at frames 33-34, sent at 0x1000 with dry volume 0. Effect type 0..8 with RevParam(16, 64), then echo with (8, 64) and (16, 0). 64 cores, even frames only (32 KB each). |
| `wave_steep00.bin` .. `wave_tri100.bin`, `wave_*_p150.bin`, `wave_*_p1000.bin` | v3: the waves at duty 0, 25, 75 and 100 (pitch 441), and at pitches 150 and 1000. 2 cores. |
