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
- **adsr**: 54 envelope sweeps covering each curve in each phase at several
  rates. Each logs the height after every core.
- **keys, pause, endflag, heights**: key on/off rules, the 32-sample key-on
  delay, the release timing, the pause and end-flag bits, and
  `__sceSasGetAllEnvelopeHeights`.
- **envscale, vag, vagflags, pcm, pitch, noise, outmode, mix**: rendered
  audio. This covers the height and volume gain, the 16 ADPCM filters and 16
  shifts, block flags and loops, PCM loop positions, pitch steps, noise,
  output modes 0 and 1, and `__sceSasCoreWithMix`.
- **reverb, waves**: argument checks and one render each. psprecomp does not
  implement either.
- **badptr**: null pointers, last of all.

The run should take well under a minute. Every render uses grain 256 at 44100 Hz
unless the test is about those, one voice unless it says otherwise, and a
flat envelope.

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

The probe also writes these files beside the EBOOT, about 400 KB in all. Each
is raw signed 16-bit little-endian audio, one core after another, starting
with the first core after the key-on. Frames are stereo L,R. The one
exception is `outmode1.bin`, where each core is four mono blocks of 256
samples: dry L, dry R, send L, send R. The log gives each file's checksum and
first samples.

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
