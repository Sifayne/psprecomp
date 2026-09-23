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
name_2: .asciz "sceDisplay"
.align 2
nids_2:
.word 0x984C27E7
.word 0x9C6EAAD7
.section .sceStub.text,"ax",@progbits
stubs_2:
.global sceDisplayWaitVblankStart
sceDisplayWaitVblankStart: jr $ra
nop
.global sceDisplayGetVcount
sceDisplayGetVcount: jr $ra
nop
.section .rodata,"a"
name_3: .asciz "sceCtrl"
.align 2
nids_3:
.word 0x6A2774F3
.word 0x1F4011E6
.word 0x1F803938
.word 0x3A622550
.section .sceStub.text,"ax",@progbits
stubs_3:
.global sceCtrlSetSamplingCycle
sceCtrlSetSamplingCycle: jr $ra
nop
.global sceCtrlSetSamplingMode
sceCtrlSetSamplingMode: jr $ra
nop
.global sceCtrlReadBufferPositive
sceCtrlReadBufferPositive: jr $ra
nop
.global sceCtrlPeekBufferPositive
sceCtrlPeekBufferPositive: jr $ra
nop
.section .lib.stub,"a",@progbits
imports_begin:
.word name_0, 0x00010000, 65541, nids_0, stubs_0
.word name_1, 0x00010000, 65541, nids_1, stubs_1
.word name_2, 0x00010000, 131077, nids_2, stubs_2
.word name_3, 0x00010000, 262149, nids_3, stubs_3
imports_end:
.section .rodata.sceModuleInfo,"a",@progbits
.align 2
.word 0
.asciz "IndependentCtrlProbe"
.space 7
.word 0,0,0,imports_begin,imports_end
