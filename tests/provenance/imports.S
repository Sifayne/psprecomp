.set noreorder
.section .rodata,"a"
io_name: .asciz "IoFileMgrForUser"
exit_name: .asciz "LoadExecForUser"
.align 2
io_nid: .word 0x42ec03ac
exit_nid: .word 0x05572a5f

.section .sceStub.text,"ax",@progbits
.global sceIoWrite
sceIoWrite: jr $ra
nop
.global sceKernelExitGame
sceKernelExitGame: jr $ra
nop

.section .lib.stub,"a",@progbits
imports_begin:
.word io_name, 0x00010000, 0x00010005, io_nid, sceIoWrite
.word exit_name, 0x00010000, 0x00010005, exit_nid, sceKernelExitGame
imports_end:

.section .rodata.sceModuleInfo,"a",@progbits
.align 2
.word 0
.asciz "IndependentRngProbe"
.space 8
.word 0, 0, 0, imports_begin, imports_end
