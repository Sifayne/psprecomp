# vfpuprobe

A PSP homebrew program, built from PSPSDK (BSD) alone, that records what the
VFPU and the FPU compute, bit for bit. It covers the parts of `src/vfpu.c`
and `include/psprecomp/recomp_rt.h` that rest on the host's math library or
on pspautotests captures:

1. vsin, vcos, vnsin, vasin, vexp2, vrexp2, vlog2, vrcp, vnrcp, vrsq,
   vsqrt, vsat0 and vsat1 over 26,800 inputs each (48 special values, a dense
   sweep of [-4, 4) and a sweep across exponents).
2. Every packing and unpacking conversion, with the destination pre-filled so
   lanes written by mistake show, plus vf2i rounding and vi2f scaling;
   vf2h ties and near-ties (v3).
3. All 32 vcst constants.
4. The random generator (vrnds, vrndi, vrndf1, vrndf2) and its state words;
   from v3, rcx0-7 after every draw, from single-bit seeds and from
   single-bit states written with mtvc, and how soon mfvc sees a draw.
5. Arithmetic, dot products, sums and cross products on operands built to
   expose rounding, NaN, signed zero, infinity and denormal handling; from
   v3, the NaN sign of vdot/vhdp/vdet/vqmul/vcrs/vcrsp one NaN at a time,
   and vsrt/vmin/vmax/vscmp on ties that differ in their bits.
6. Every vcmp condition on every pair of interesting values, and which CC
   bits a narrow compare leaves alone; from v3, how many instructions an
   mfvc needs after each size of vcmp, and the two CC steps again with the
   reads 16 instructions later.
7. Matrix multiply orientation and the transforms.
8. vrot patterns.
9. Source, target and destination prefixes.
10. What a freshly created thread's general, FPU and VFPU registers hold;
    from v3, the main thread's starting fcr31 and where a new thread's fp,
    k0 and a1 point, against its sp and stack top.
11. FPU control registers, exception bits, rounding modes, flush-to-zero and
    all 16 compare conditions; from v3, div.s overflow and underflow in
    every mode, neg/abs/mov of signed NaNs, sqrt.s of -0 and denormals, and
    compare flags per pair.
12. Traps: the E cause bit (not run: it switched a 6.60 PSP off), then from
    v3 1/0, 0/0 and max*max in fresh threads that keep their fcr31 of
    00000E00 (O, Z and V enabled), and a ctc1 of a cause bit with its enable.
13. From v3, each transcendental over its reduced argument range, into
    `vfpu_core_<op>.bin` (about 91 MB, see below).

Version 3 runs 4, then 12, then 13 after the others.

## Build

Needs the pspdev toolchain (`psp-gcc`, `psp-config` and PSPSDK) on `PATH`:

    make

This produces `EBOOT.PBP`. The CMake build does not include it.

## Run

Copy `EBOOT.PBP` to `ms0:/PSP/GAME/vfpuprobe/` and start it from the XMB. It
needs no input and returns to the XMB by itself. Sections 0-12 take a few
seconds; section 13 writes about 91 MB, a minute or so at 1.5-2 MB/s and three
at 0.5 MB/s, so the stick needs 95 MB free.

It writes, beside the EBOOT:

- `vfpuprobe.txt`, the log;
- `vfpu_inputs.bin`, the 26,800 input words of section 1;
- `vfpu_<op>.bin`, one output word per input for each op in section 1;
- `vfpu_rng_seeds.bin`: for each seed 1<<k, k = 0..31, rcx0-7 after vrnds,
  then 16 times the vrndi value and rcx0-7 after it (152 words a seed);
- `vfpu_rng_states.bin`: for rcx i = 0..7 and bit b = 0..31, the seed-0
  state (every rcx 3F800000) with bit b of rcx i flipped by mtvc: rcx0-7
  after the write, then 8 times the vrndi value and rcx0-7 (80 words each);
- `vfpu_core_<op>.bin` for vsin, vcos, vasin, vexp2, vlog2, vrcp, vsqrt and
  vrsq: one result word per input, in segments the log lists as
  `k start stride count` (x = (start + stride*i) * 2^-23) or
  `b start stride count` (x has the bits start + stride*i). The main segments
  take every 3rd input of the reduced range (every 5th for vsqrt and vrsq,
  which cover [1,4)): all of it would be 352 MB.

The log is written through before every step, so if the PSP switches off,
its last line names the step that did it. Section 12's cases may do that;
start the probe again after each one and it skips the case that stopped it
and carries on. The last run in the log is then the whole result.

## Compare with psprecomp

    allegrexrecomp interp vfpuprobe.prx --dispatch --budget 4000000000

The same lines go to stderr and to `./ms/PSP/GAME/vfpuprobe/`, next to the
same `.bin` files, so the hardware copies can be diffed against them.
