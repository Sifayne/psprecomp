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
name_2: .asciz "Kernel_Library"
.align 2
nids_2:
.word 0x092968F4
.word 0x5F10D406
.word 0xB55249D2
.section .sceStub.text,"ax",@progbits
stubs_2:
.global sceKernelCpuSuspendIntr
sceKernelCpuSuspendIntr: jr $ra
nop
.global sceKernelCpuResumeIntr
sceKernelCpuResumeIntr: jr $ra
nop
.global sceKernelIsCpuIntrEnable
sceKernelIsCpuIntrEnable: jr $ra
nop
.section .rodata,"a"
name_3: .asciz "sceGe_user"
.align 2
nids_3:
.word 0xA4FC06A4
.word 0x05DB22CE
.word 0xAB49E76A
.word 0x03444EB4
.word 0x4C06E472
.section .sceStub.text,"ax",@progbits
stubs_3:
.global sceGeSetCallback
sceGeSetCallback: jr $ra
nop
.global sceGeUnsetCallback
sceGeUnsetCallback: jr $ra
nop
.global sceGeListEnQueue
sceGeListEnQueue: jr $ra
nop
.global sceGeListSync
sceGeListSync: jr $ra
nop
.global sceGeContinue
sceGeContinue: jr $ra
nop
.section .rodata,"a"
name_4: .asciz "SysMemUserForUser"
.align 2
nids_4:
.word 0x7591C7DB
.word 0xFC114573
.word 0x1b4217bc
.section .sceStub.text,"ax",@progbits
stubs_4:
.global sceKernelSetCompiledSdkVersion
sceKernelSetCompiledSdkVersion: jr $ra
nop
.global sceKernelGetCompiledSdkVersion
sceKernelGetCompiledSdkVersion: jr $ra
nop
.global probeSdk630
probeSdk630: jr $ra
nop
.section .lib.stub,"a",@progbits
imports_begin:
.word name_0, 0x00010000, 65541, nids_0, stubs_0
.word name_1, 0x00010000, 65541, nids_1, stubs_1
.word name_2, 0x00010000, 196613, nids_2, stubs_2
.word name_3, 0x00010000, 327685, nids_3, stubs_3
.word name_4, 0x00010000, 196613, nids_4, stubs_4
imports_end:
.section .rodata.sceModuleInfo,"a",@progbits
.align 2
.word 0
.asciz "IndependentGeProbe"
.space 9
.word 655894552,0,0,imports_begin,imports_end
