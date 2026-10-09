/* imports.S -- the calls sysprobe needs that PSPSDK has no stub for.
 *
 * SysMemUserForUser 0x342061E5 is SetCompiledSdkVersion's variant for SDK
 * 3.70 (psprecomp src/hle/sysmem.c; WipEout Pulse imports it and passes
 * 0x03070010), and 0xFC114573 is sceKernelGetCompiledSdkVersion. The entry
 * is written out under a symbol of its own, as in saveprobe/imports.S, with
 * the flags word libpspuser's SysMemUserForUser entry carries. */
	.set noreorder

#include "pspimport.s"

	.set push
	.section .rodata.sceResident, "a"
	.word   0
__stub_modulestr_sys_SysMemUserForUser:
	.asciz  "SysMemUserForUser"
	.align  2

	.section .lib.stub, "a", @progbits
	.global __stub_module_sys_SysMemUserForUser
__stub_module_sys_SysMemUserForUser:
	.word   __stub_modulestr_sys_SysMemUserForUser
	.word   0x40080000
	.word   0x5
	.word   __executable_start
	.word   __executable_start
	.set pop

	IMPORT_FUNC	sys_SysMemUserForUser,0x342061E5,sceKernelSetCompiledSdkVersion370
	IMPORT_FUNC	sys_SysMemUserForUser,0xFC114573,sceKernelGetCompiledSdkVersion
