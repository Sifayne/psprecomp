/* modulemgr.S -- sceKernelGetModuleId (ModuleMgrForUser NID 0xF0A26395),
 * which PSPSDK has no stub for. Its own entry, under a symbol of its own, as
 * in saveprobe/imports.S; the flags word is libpspuser's ModuleMgrForUser
 * entry's. */
	.set noreorder

#include "pspimport.s"

	.set push
	.section .rodata.sceResident, "a"
	.word   0
__stub_modulestr_probe_ModuleMgrForUser:
	.asciz  "ModuleMgrForUser"
	.align  2

	.section .lib.stub, "a", @progbits
	.global __stub_module_probe_ModuleMgrForUser
__stub_module_probe_ModuleMgrForUser:
	.word   __stub_modulestr_probe_ModuleMgrForUser
	.word   0x40090000
	.word   0x5
	.word   __executable_start
	.word   __executable_start
	.set pop

	IMPORT_FUNC	probe_ModuleMgrForUser,0xF0A26395,sceKernelGetModuleId
