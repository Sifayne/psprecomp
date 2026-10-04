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
name_2: .asciz "sceSuspendForUser"
.align 2
nids_2:
.word 0x3E0271D3
.word 0xA14F40B2
.word 0xA569E425
.section .sceStub.text,"ax",@progbits
stubs_2:
.global sceKernelVolatileMemLock
sceKernelVolatileMemLock: jr $ra
nop
.global sceKernelVolatileMemTryLock
sceKernelVolatileMemTryLock: jr $ra
nop
.global sceKernelVolatileMemUnlock
sceKernelVolatileMemUnlock: jr $ra
nop
.section .rodata,"a"
name_3: .asciz "ThreadManForUser"
.align 2
nids_3:
.word 0x446D8DE6
.word 0xF475845D
.word 0x9FA03CD3
.word 0x278C0DF5
.word 0x840E8133
.word 0x293B45B8
.word 0x71BC9871
.word 0xCEADEB47
.word 0x383F7BCC
.section .sceStub.text,"ax",@progbits
stubs_3:
.global sceKernelCreateThread
sceKernelCreateThread: jr $ra
nop
.global sceKernelStartThread
sceKernelStartThread: jr $ra
nop
.global sceKernelDeleteThread
sceKernelDeleteThread: jr $ra
nop
.global sceKernelWaitThreadEnd
sceKernelWaitThreadEnd: jr $ra
nop
.global sceKernelWaitThreadEndCB
sceKernelWaitThreadEndCB: jr $ra
nop
.global sceKernelGetThreadId
sceKernelGetThreadId: jr $ra
nop
.global sceKernelChangeThreadPriority
sceKernelChangeThreadPriority: jr $ra
nop
.global sceKernelDelayThread
sceKernelDelayThread: jr $ra
nop
.global sceKernelTerminateDeleteThread
sceKernelTerminateDeleteThread: jr $ra
nop
.section .rodata,"a"
name_4: .asciz "SysMemUserForUser"
.align 2
nids_4:
.word 0x7591C7DB
.word 0xFC114573
.word 0x91de343c
.section .sceStub.text,"ax",@progbits
stubs_4:
.global sceKernelSetCompiledSdkVersion
sceKernelSetCompiledSdkVersion: jr $ra
nop
.global sceKernelGetCompiledSdkVersion
sceKernelGetCompiledSdkVersion: jr $ra
nop
.global probeOpaque91de
probeOpaque91de: jr $ra
nop
.section .lib.stub,"a",@progbits
imports_begin:
.word name_0, 0x00010000, 65541, nids_0, stubs_0
.word name_1, 0x00010000, 65541, nids_1, stubs_1
.word name_2, 0x00010000, 196613, nids_2, stubs_2
.word name_3, 0x00010000, 589829, nids_3, stubs_3
.word name_4, 0x00010000, 196613, nids_4, stubs_4
imports_end:
.section .rodata.sceModuleInfo,"a",@progbits
.align 2
.word 0
.asciz "IndependentKernelProbe"
.space 5
.word 0,0,0,imports_begin,imports_end
