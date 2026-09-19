/* Project-authored stubs; NIDs from BSD PSPSDK via make_imports.py. */
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
.word 0x369ED59D
.section .sceStub.text,"ax",@progbits
stubs_2:
.global sceKernelGetSystemTimeLow
sceKernelGetSystemTimeLow: jr $ra
nop
.section .rodata,"a"
name_3: .asciz "sceDisplay"
.align 2
nids_3:
.word 0x984C27E7
.section .sceStub.text,"ax",@progbits
stubs_3:
.global sceDisplayWaitVblankStart
sceDisplayWaitVblankStart: jr $ra
nop
.section .rodata,"a"
name_4: .asciz "Kernel_Library"
.align 2
nids_4:
.word 0x092968F4
.word 0x5F10D406
.word 0x3B84732D
.word 0x47A0B729
.word 0xB55249D2
.section .sceStub.text,"ax",@progbits
stubs_4:
.global sceKernelCpuSuspendIntr
sceKernelCpuSuspendIntr: jr $ra
nop
.global sceKernelCpuResumeIntr
sceKernelCpuResumeIntr: jr $ra
nop
.global sceKernelCpuResumeIntrWithSync
sceKernelCpuResumeIntrWithSync: jr $ra
nop
.global sceKernelIsCpuIntrSuspended
sceKernelIsCpuIntrSuspended: jr $ra
nop
.global sceKernelIsCpuIntrEnable
sceKernelIsCpuIntrEnable: jr $ra
nop
.section .rodata,"a"
name_5: .asciz "InterruptManager"
.align 2
nids_5:
.word 0xCA04A2B9
.word 0xD61E6961
.word 0xFB8E22EC
.word 0x8A389411
.section .sceStub.text,"ax",@progbits
stubs_5:
.global sceKernelRegisterSubIntrHandler
sceKernelRegisterSubIntrHandler: jr $ra
nop
.global sceKernelReleaseSubIntrHandler
sceKernelReleaseSubIntrHandler: jr $ra
nop
.global sceKernelEnableSubIntr
sceKernelEnableSubIntr: jr $ra
nop
.global sceKernelDisableSubIntr
sceKernelDisableSubIntr: jr $ra
nop
.section .lib.stub,"a",@progbits
imports_begin:
.word name_0, 0x00010000, 65541, nids_0, stubs_0
.word name_1, 0x00010000, 65541, nids_1, stubs_1
.word name_2, 0x00010000, 65541, nids_2, stubs_2
.word name_3, 0x00010000, 65541, nids_3, stubs_3
.word name_4, 0x00010000, 327685, nids_4, stubs_4
.word name_5, 0x00010000, 262149, nids_5, stubs_5
imports_end:
.section .rodata.sceModuleInfo,"a",@progbits
.align 2
.word 0
.asciz "IndependentIntrProbe"
.space 7
.word 826366246,0,0,imports_begin,imports_end
