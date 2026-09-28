/* stubs.S -- threadprobe's hand-written pieces.
 *
 * 1. Imports PSPSDK has no stubs for. The NIDs are the ones psprecomp's own
 *    registration table uses (src/hle/kernobj.c:2030-2034 for the TLS pool,
 *    src/hle/kernlock.c:341-342 for the mutex). They get an import entry of
 *    their own: adding them to libpspuser's ThreadManForUser entry would put
 *    them apart from the library's stubs, and psp-fixup-imports refuses an
 *    entry whose stubs are not contiguous. pspimport.s's IMPORT_START names
 *    the entry symbol after the library, which would clash with libpspuser's,
 *    so the entry is written out here in the same form under another symbol
 *    (IMPORT_FUNC only uses its first argument to name that symbol). The
 *    flags word is the one libpspuser's own ThreadManForUser entry carries.
 *
 * 2. regs_entry, a thread entry point that records the register file exactly
 *    as the kernel handed it over -- before any compiled prologue touches it --
 *    and then tail-calls regs_body() in main.c with $a0/$a1/$ra intact.
 *
 *    Layout of g_entry_regs (words):
 *      [0..31]  GPR 0..31 ($t0 is restored from the stash below $sp)
 *      [32] hi  [33] lo  [34] FCR31  [35..66] $f0..$f31
 */
	.set noreorder
	.set noat

#include "pspimport.s"

.macro TP_IMPORT_START module, sym, flags_ver
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

	TP_IMPORT_START "ThreadManForUser",tp_ThreadManForUser,0x40090000
	IMPORT_FUNC	tp_ThreadManForUser,0x8DAFF657,sceKernelCreateTlspl
	IMPORT_FUNC	tp_ThreadManForUser,0x32BF938E,sceKernelDeleteTlspl
	IMPORT_FUNC	tp_ThreadManForUser,0xFA835CDE,sceKernelGetTlsAddr
	IMPORT_FUNC	tp_ThreadManForUser,0x4A719FB2,sceKernelFreeTlspl
	IMPORT_FUNC	tp_ThreadManForUser,0x721067F3,sceKernelReferTlsplStatus
	IMPORT_FUNC	tp_ThreadManForUser,0xB7D098C6,sceKernelCreateMutex
	IMPORT_FUNC	tp_ThreadManForUser,0xF8170FBE,sceKernelDeleteMutex

	.text
	.globl	regs_entry
	.ent	regs_entry
	.type	regs_entry, @function
regs_entry:
	sw	$t0, -4($sp)
	lui	$t0, %hi(g_entry_regs)
	addiu	$t0, $t0, %lo(g_entry_regs)
	sw	$zero, 0($t0)
	sw	$at, 4($t0)
	sw	$v0, 8($t0)
	sw	$v1, 12($t0)
	sw	$a0, 16($t0)
	sw	$a1, 20($t0)
	sw	$a2, 24($t0)
	sw	$a3, 28($t0)
	sw	$t1, 36($t0)
	sw	$t2, 40($t0)
	sw	$t3, 44($t0)
	sw	$t4, 48($t0)
	sw	$t5, 52($t0)
	sw	$t6, 56($t0)
	sw	$t7, 60($t0)
	sw	$s0, 64($t0)
	sw	$s1, 68($t0)
	sw	$s2, 72($t0)
	sw	$s3, 76($t0)
	sw	$s4, 80($t0)
	sw	$s5, 84($t0)
	sw	$s6, 88($t0)
	sw	$s7, 92($t0)
	sw	$t8, 96($t0)
	sw	$t9, 100($t0)
	sw	$k0, 104($t0)
	sw	$k1, 108($t0)
	sw	$gp, 112($t0)
	sw	$sp, 116($t0)
	sw	$fp, 120($t0)
	sw	$ra, 124($t0)
	lw	$t1, -4($sp)
	nop
	sw	$t1, 32($t0)
	mfhi	$t1
	sw	$t1, 128($t0)
	mflo	$t1
	sw	$t1, 132($t0)
	cfc1	$t1, $31
	nop
	sw	$t1, 136($t0)
	swc1	$f0, 140($t0)
	swc1	$f1, 144($t0)
	swc1	$f2, 148($t0)
	swc1	$f3, 152($t0)
	swc1	$f4, 156($t0)
	swc1	$f5, 160($t0)
	swc1	$f6, 164($t0)
	swc1	$f7, 168($t0)
	swc1	$f8, 172($t0)
	swc1	$f9, 176($t0)
	swc1	$f10, 180($t0)
	swc1	$f11, 184($t0)
	swc1	$f12, 188($t0)
	swc1	$f13, 192($t0)
	swc1	$f14, 196($t0)
	swc1	$f15, 200($t0)
	swc1	$f16, 204($t0)
	swc1	$f17, 208($t0)
	swc1	$f18, 212($t0)
	swc1	$f19, 216($t0)
	swc1	$f20, 220($t0)
	swc1	$f21, 224($t0)
	swc1	$f22, 228($t0)
	swc1	$f23, 232($t0)
	swc1	$f24, 236($t0)
	swc1	$f25, 240($t0)
	swc1	$f26, 244($t0)
	swc1	$f27, 248($t0)
	swc1	$f28, 252($t0)
	swc1	$f29, 256($t0)
	swc1	$f30, 260($t0)
	swc1	$f31, 264($t0)
	j	regs_body
	nop
	.end	regs_entry
	.size	regs_entry, .-regs_entry
