.set noreorder
.section .rodata,"a"
name_0: .asciz "IoFileMgrForUser"
.align 2
nids_0:
.word 0x42EC03AC
.section .sceStub.text,"ax",@progbits
stubs_0:
.global sceIoWrite
sceIoWrite: jr $ra
nop
.section .rodata,"a"
name_1: .asciz "LoadExecForUser"
.align 2
nids_1:
.word 0x05572A5F
.section .sceStub.text,"ax",@progbits
stubs_1:
.global sceKernelExitGame
sceKernelExitGame: jr $ra
nop
.section .rodata,"a"
name_2: .asciz "ThreadManForUser"
.align 2
nids_2:
.word 0xE81CAF8F
.word 0x349D6D6C
.word 0xEDBA5844
.section .sceStub.text,"ax",@progbits
stubs_2:
.global sceKernelCreateCallback
sceKernelCreateCallback: jr $ra
nop
.global sceKernelCheckCallback
sceKernelCheckCallback: jr $ra
nop
.global sceKernelDeleteCallback
sceKernelDeleteCallback: jr $ra
nop
.section .rodata,"a"
name_3: .asciz "sceUmdUser"
.align 2
nids_3:
.word 0x20628E6F
.word 0x340B7686
.word 0x46EBB729
.word 0x4A9E5E29
.word 0x56202973
.word 0x6B4A146C
.word 0xAEE7404D
.word 0xBD2BDE07
.word 0xC6183D47
.word 0xE83742BA
.section .sceStub.text,"ax",@progbits
stubs_3:
.global sceUmdGetErrorStat
sceUmdGetErrorStat: jr $ra
nop
.global sceUmdGetDiscInfo
sceUmdGetDiscInfo: jr $ra
nop
.global sceUmdCheckMedium
sceUmdCheckMedium: jr $ra
nop
.global sceUmdWaitDriveStatCB
sceUmdWaitDriveStatCB: jr $ra
nop
.global sceUmdWaitDriveStatWithTimer
sceUmdWaitDriveStatWithTimer: jr $ra
nop
.global sceUmdGetDriveStat
sceUmdGetDriveStat: jr $ra
nop
.global sceUmdRegisterUMDCallBack
sceUmdRegisterUMDCallBack: jr $ra
nop
.global sceUmdUnRegisterUMDCallBack
sceUmdUnRegisterUMDCallBack: jr $ra
nop
.global sceUmdActivate
sceUmdActivate: jr $ra
nop
.global sceUmdDeactivate
sceUmdDeactivate: jr $ra
nop
.section .lib.stub,"a",@progbits
imports_begin:
.word name_0, 0x00010000, 65541, nids_0, stubs_0
.word name_1, 0x00010000, 65541, nids_1, stubs_1
.word name_2, 0x00010000, 196613, nids_2, stubs_2
.word name_3, 0x00010000, 655365, nids_3, stubs_3
imports_end:
.section .rodata.sceModuleInfo,"a",@progbits
.align 2
.word 0
.asciz "IndependentUmdProbe"
.space 8
.word 0,0,0,imports_begin,imports_end
