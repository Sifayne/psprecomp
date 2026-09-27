# vfpuprobe

A PSP homebrew program, built from PSPSDK (BSD) alone, that records what the
VFPU and the FPU compute, bit for bit. It covers the parts of `src/vfpu.c`
and `include/psprecomp/recomp_rt.h` that rest on the host's math library or
on pspautotests captures:

1. vsin, vcos, vnsin, vasin, vexp2, vrexp2, vlog2, vrcp, vnrcp, vrsq,
   vsqrt, vsat0 and vsat1 over 26,800 inputs each (48 special values, a dense
   sweep of [-4, 4) and a sweep across exponents).
2. Every packing and unpacking conversion, with the destination pre-filled so
   lanes written by mistake show, plus vf2i rounding and vi2f scaling.
3. All 32 vcst constants.
4. The random generator (vrnds, vrndi, vrndf1, vrndf2) and its state words.
5. Arithmetic, dot products, sums and cross products on operands built to
   expose rounding, NaN, signed zero, infinity and denormal handling.
6. Every vcmp condition on every pair of interesting values, and which CC
   bits a narrow compare leaves alone.
7. Matrix multiply orientation and the transforms.
8. vrot patterns.
9. Source, target and destination prefixes.
10. What a freshly created thread's general, FPU and VFPU registers hold.
11. FPU control registers, exception bits, rounding modes, flush-to-zero and
    all 16 compare conditions.

## Build

Needs the pspdev toolchain (`psp-gcc`, `psp-config` and PSPSDK) on `PATH`:

    make

This produces `EBOOT.PBP`. The CMake build does not include it.

## Run

Copy `EBOOT.PBP` to `ms0:/PSP/GAME/vfpuprobe/` and start it from the XMB. It
needs no input, takes a few seconds, and returns to the XMB by itself.

It writes, beside the EBOOT:

- `vfpuprobe.txt`, the log;
- `vfpu_inputs.bin`, the 26,800 input words of section 1;
- `vfpu_<op>.bin`, one output word per input for each op in section 1.

The log is written through before every step, so if the PSP switches off,
its last line names the step that did it. The last step of all writes the
FPU's E cause bit, which may trap; everything else is saved before it.

## Compare with psprecomp

    allegrexrecomp interp vfpuprobe.prx --dispatch --budget 4000000000

The same lines go to stderr and to `./ms/PSP/GAME/vfpuprobe/`, next to the
same `.bin` files, so the hardware copies can be diffed against them.
