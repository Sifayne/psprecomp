/* Project-authored stubs; NIDs from BSD PSPSDK via make_imports.py. */
.set noreorder
.section .rodata,"a"
name_0: .asciz "IoFileMgrForUser"
.align 2
nids_0:
.word 0x42EC03AC
.word 0xF27A9C51
.section .sceStub.text,"ax",@progbits
stubs_0:
.global sceIoWrite
sceIoWrite: jr $ra
nop
.global sceIoRemove
sceIoRemove: jr $ra
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
.word 0xCEADEB47
.section .sceStub.text,"ax",@progbits
stubs_2:
.global sceKernelDelayThread
sceKernelDelayThread: jr $ra
nop
.section .rodata,"a"
name_3: .asciz "sceUtility"
.align 2
nids_3:
.word 0x50C4CD57
.word 0x8874DBE0
.word 0xD4B95FFB
.word 0x9790B33C
.section .sceStub.text,"ax",@progbits
stubs_3:
.global sceUtilitySavedataInitStart
sceUtilitySavedataInitStart: jr $ra
nop
.global sceUtilitySavedataGetStatus
sceUtilitySavedataGetStatus: jr $ra
nop
.global sceUtilitySavedataUpdate
sceUtilitySavedataUpdate: jr $ra
nop
.global sceUtilitySavedataShutdownStart
sceUtilitySavedataShutdownStart: jr $ra
nop
.section .lib.stub,"a",@progbits
imports_begin:
.word name_0, 0x00010000, 131077, nids_0, stubs_0
.word name_1, 0x00010000, 65541, nids_1, stubs_1
.word name_2, 0x00010000, 65541, nids_2, stubs_2
.word name_3, 0x00010000, 262149, nids_3, stubs_3
imports_end:
.section .rodata.sceModuleInfo,"a",@progbits
.align 2
.word 0
.asciz "IndependentSaveProbe"
.space 7
.word 0,0,0,imports_begin,imports_end
