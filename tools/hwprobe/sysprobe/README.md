# sysprobe

A PSP homebrew program, built from PSPSDK (BSD) alone, for firmware calls
psprecomp answers without a measurement and no other probe reaches.

1. **sceIoMkdir and sceIoChstat** on the memory stick, in a `scratch` folder
   beside the EBOOT. Mkdir with a missing parent, an existing directory, a
   trailing slash; Rmdir of a directory that is not empty. Chstat with each
   bit on its own: mode, attributes, size, the three times (2001-02-03
   04:05:06.000007, to see how FAT rounds each), none, private. Also Chstat
   of a missing file and of a directory. `src/hle/iofilemgr.c` makes a
   missing parent and makes Chstat a no-op.
2. **Files named by their extent**, `disc0:/sce_lbn<sector>_size<bytes>`, as
   WipEout Pulse opens its archives:
   - Getstat of the disc's own `PSP_GAME/PARAM.SFO`, for its size and start
     sector;
   - opening that extent in each spelling: hex, decimal, capitals, no
     slash, `umd0:`, no size, text after the size, a short size, a size
     past the file, a sector past the disc;
   - Getstat and Dopen on an extent.

   Each open logs its Read and SEEK_END, and whether the bytes match
   PARAM.SFO's. The disc's contents are never logged. **These steps need a
   UMD in the drive, any game**; without one they log "not run".
3. **Nothing connected**: sceKernelGetGPI and sceKernelSetGPO; the headphone
   remote's IsRemoteExist, IsHeadphoneExist, IsMicrophoneExist and
   PeekCurrentKey. **Run it with nothing plugged into the headphone socket.**
4. **SetCompiledSdkVersion's 3.70 variant** (SysMemUserForUser 0x342061E5),
   which WipEout Pulse calls with 0x03070010, with
   sceKernelGetCompiledSdkVersion before and after. Last, since it changes
   the process.

## Build

Needs the pspdev toolchain (`psp-gcc`, `psp-config` and PSPSDK) on `PATH`:

    make

Under psprecomp it runs as the other probes do:

    allegrexrecomp interp sysprobe.prx --dispatch --base 0x08804000
