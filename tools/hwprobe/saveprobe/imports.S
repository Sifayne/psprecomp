/* imports.S -- the one call saveprobe needs that PSPSDK has no stub for.
 *
 * sceKernelSetCompiledSdkVersion660 is NID 0x358CA1BB, exported by
 * SysMemUserForUser on firmware 6.60 (uofw src/kd/sysmem/exports.exp, MIT).
 * PSPSDK's sceKernelSetCompiledSdkVersion is NID 0x7591C7DB, the variant for
 * SDK versions 1.00 to 3.60; in uofw's sysmem (misc.c) every variant resets
 * the PSP when handed a version outside its range, so the 6.60 one is needed.
 *
 * The import gets an entry of its own, as in syncprobe/imports.S: adding it
 * to libpspuser's SysMemUserForUser entry would put its stub apart from that
 * entry's other stubs, which psp-fixup-imports refuses, and pspimport.s's
 * IMPORT_START names the entry symbol after the library, which would clash
 * with libpspuser's. So the entry is written out here in the same form under
 * another symbol (IMPORT_FUNC only uses its first argument to name it). The
 * flags word is the one libpspuser's SysMemUserForUser entry carries. */

	.set noreorder

#include "pspimport.s"

.macro SAVE_IMPORT_START module, sym, flags_ver
	.set push
	.section .rodata.sceResident, "a"
	.word   0
__stub_modulestr_\sym:
	.asciz  "\module"
	.align  2

	.section .lib.stub, "a", @progbits
	.global __stub_module_\sym
__stub_module_\sym:
	.word   __stub_modulestr_\sym
	.word   \flags_ver
	.word   0x5
	.word   __executable_start
	.word   __executable_start
	.set pop
.endm

	SAVE_IMPORT_START "SysMemUserForUser",save_SysMemUserForUser,0x40080000
	IMPORT_FUNC	save_SysMemUserForUser,0x358CA1BB,sceKernelSetCompiledSdkVersion660
