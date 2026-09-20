/* Project-authored stubs; NIDs from BSD PSPSDK via make_imports.py. */
.set noreorder
.section .rodata,"a"
name_0: .asciz "IoFileMgrForUser"
.align 2
nids_0:
.word 0x42EC03AC
.word 0x109F50BC
.word 0x810C4BC3
.word 0x6A638D83
.word 0x27EB27B8
.word 0x68963324
.word 0xACE946E8
.word 0xB29DDF9C
.word 0xE3EB004C
.word 0xEB092469
.word 0x89AA9906
.word 0xA0B5A7C2
.word 0x71B19E77
.word 0xFF5940B6
.word 0x3251EA56
.word 0xE23EEC33
.word 0x35DBD746
.word 0xCB05F8D6
.section .sceStub.text,"ax",@progbits
stubs_0:
.global sceIoWrite
sceIoWrite: jr $ra
nop
.global sceIoOpen
sceIoOpen: jr $ra
nop
.global sceIoClose
sceIoClose: jr $ra
nop
.global sceIoRead
sceIoRead: jr $ra
nop
.global sceIoLseek
sceIoLseek: jr $ra
nop
.global sceIoLseek32
sceIoLseek32: jr $ra
nop
.global sceIoGetstat
sceIoGetstat: jr $ra
nop
.global sceIoDopen
sceIoDopen: jr $ra
nop
.global sceIoDread
sceIoDread: jr $ra
nop
.global sceIoDclose
sceIoDclose: jr $ra
nop
.global sceIoOpenAsync
sceIoOpenAsync: jr $ra
nop
.global sceIoReadAsync
sceIoReadAsync: jr $ra
nop
.global sceIoLseekAsync
sceIoLseekAsync: jr $ra
nop
.global sceIoCloseAsync
sceIoCloseAsync: jr $ra
nop
.global sceIoPollAsync
sceIoPollAsync: jr $ra
nop
.global sceIoWaitAsync
sceIoWaitAsync: jr $ra
nop
.global sceIoWaitAsyncCB
sceIoWaitAsyncCB: jr $ra
nop
.global sceIoGetAsyncStat
sceIoGetAsyncStat: jr $ra
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
name_3: .asciz "sceUmdUser"
.align 2
nids_3:
.word 0xC6183D47
.word 0xE83742BA
.word 0x56202973
.word 0x6B4A146C
.section .sceStub.text,"ax",@progbits
stubs_3:
.global sceUmdActivate
sceUmdActivate: jr $ra
nop
.global sceUmdDeactivate
sceUmdDeactivate: jr $ra
nop
.global sceUmdWaitDriveStatWithTimer
sceUmdWaitDriveStatWithTimer: jr $ra
nop
.global sceUmdGetDriveStat
sceUmdGetDriveStat: jr $ra
nop
.section .lib.stub,"a",@progbits
imports_begin:
.word name_0, 0x00010000, 1179653, nids_0, stubs_0
.word name_1, 0x00010000, 65541, nids_1, stubs_1
.word name_2, 0x00010000, 65541, nids_2, stubs_2
.word name_3, 0x00010000, 262149, nids_3, stubs_3
imports_end:
.section .rodata.sceModuleInfo,"a",@progbits
.align 2
.word 0
.asciz "IndependentDiscProbe"
.space 7
.word 0,0,0,imports_begin,imports_end
