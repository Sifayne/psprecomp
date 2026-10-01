/* imports.S -- the kernel calls syncprobe needs that PSPSDK's stub libraries
 * do not provide, declared with PSPSDK's pspimport.s IMPORT_FUNC.
 *
 * They get import entries of their own. Adding functions to libpspuser's
 * ThreadManForUser / Kernel_Library entries would put this file's stubs
 * apart from the library's, and psp-fixup-imports refuses stubs of one entry
 * that are not contiguous. pspimport.s's IMPORT_START names the entry's
 * symbol after the library, which would clash with libpspuser's, so the entry
 * is written out here in the same form under a different symbol:
 * IMPORT_FUNC only uses its first argument to name that symbol. The flags
 * words are the ones libpspuser's own entries carry.
 *
 * The NIDs are the ones psprecomp registers (src/hle/kernlock.c
 * psp_kernlock_register and psp_kernlock_register_lw). Which library exports
 * each one on a real PSP is from uofw's export tables (MIT):
 * src/kd/threadman/exports.exp and src/kd/usersystemlib/exports.exp. */

	.set noreorder

#include "pspimport.s"

.macro SYNC_IMPORT_START module, sym, flags_ver
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

/* ThreadManForUser: the mutex family, and the by-id lwmutex status. */
	SYNC_IMPORT_START "ThreadManForUser",sync_ThreadManForUser,0x40090000
	IMPORT_FUNC	sync_ThreadManForUser,0xB7D098C6,sceKernelCreateMutex
	IMPORT_FUNC	sync_ThreadManForUser,0xF8170FBE,sceKernelDeleteMutex
	IMPORT_FUNC	sync_ThreadManForUser,0xB011B11F,sceKernelLockMutex
	IMPORT_FUNC	sync_ThreadManForUser,0x0DDCD2C9,sceKernelTryLockMutex
	IMPORT_FUNC	sync_ThreadManForUser,0x6B30100F,sceKernelUnlockMutex
	IMPORT_FUNC	sync_ThreadManForUser,0x87D9223C,sceKernelCancelMutex
	IMPORT_FUNC	sync_ThreadManForUser,0xA9C2CB9A,sceKernelReferMutexStatus
	IMPORT_FUNC	sync_ThreadManForUser,0x4C145944,sceKernelReferLwMutexStatusByID

/* Kernel_Library: the user-mode half of the lwmutex. */
	SYNC_IMPORT_START "Kernel_Library",sync_Kernel_Library,0x00090000
	IMPORT_FUNC	sync_Kernel_Library,0xC1734599,sceKernelReferLwMutexStatus
	IMPORT_FUNC	sync_Kernel_Library,0x37431849,sceKernelTryLockLwMutex_600
