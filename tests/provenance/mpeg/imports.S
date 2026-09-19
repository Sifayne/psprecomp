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
name_2: .asciz "sceUtility"
.align 2
nids_2:
.word 0xC629AF26
.section .sceStub.text,"ax",@progbits
stubs_2:
.global sceUtilityLoadAvModule
sceUtilityLoadAvModule: jr $ra
nop
.section .rodata,"a"
name_3: .asciz "SysMemUserForUser"
.align 2
nids_3:
.word 0x7591C7DB
.section .sceStub.text,"ax",@progbits
stubs_3:
.global sceKernelSetCompiledSdkVersion
sceKernelSetCompiledSdkVersion: jr $ra
nop
.section .rodata,"a"
name_4: .asciz "sceMpeg"
.align 2
nids_4:
.word 0x682A619B
.word 0x874624D6
.word 0xC132E22F
.word 0xD7A29F46
.word 0x37295ED8
.word 0x13407F13
.word 0xB5F6DC87
.word 0xD8C5F121
.word 0x606A4649
.word 0xF8DCB679
.word 0x167AFD9E
.word 0xA780CF7E
.word 0xCEB870B1
.word 0x611E9E11
.word 0x21FF80E4
.word 0x42560F23
.word 0x591A4AA2
.word 0xFE246728
.word 0xB240A59E
.section .sceStub.text,"ax",@progbits
stubs_4:
.global sceMpegInit
sceMpegInit: jr $ra
nop
.global sceMpegFinish
sceMpegFinish: jr $ra
nop
.global sceMpegQueryMemSize
sceMpegQueryMemSize: jr $ra
nop
.global sceMpegRingbufferQueryMemSize
sceMpegRingbufferQueryMemSize: jr $ra
nop
.global sceMpegRingbufferConstruct
sceMpegRingbufferConstruct: jr $ra
nop
.global sceMpegRingbufferDestruct
sceMpegRingbufferDestruct: jr $ra
nop
.global sceMpegRingbufferAvailableSize
sceMpegRingbufferAvailableSize: jr $ra
nop
.global sceMpegCreate
sceMpegCreate: jr $ra
nop
.global sceMpegDelete
sceMpegDelete: jr $ra
nop
.global sceMpegQueryAtracEsSize
sceMpegQueryAtracEsSize: jr $ra
nop
.global sceMpegInitAu
sceMpegInitAu: jr $ra
nop
.global sceMpegMallocAvcEsBuf
sceMpegMallocAvcEsBuf: jr $ra
nop
.global sceMpegFreeAvcEsBuf
sceMpegFreeAvcEsBuf: jr $ra
nop
.global sceMpegQueryStreamSize
sceMpegQueryStreamSize: jr $ra
nop
.global sceMpegQueryStreamOffset
sceMpegQueryStreamOffset: jr $ra
nop
.global sceMpegRegistStream
sceMpegRegistStream: jr $ra
nop
.global sceMpegUnRegistStream
sceMpegUnRegistStream: jr $ra
nop
.global sceMpegGetAvcAu
sceMpegGetAvcAu: jr $ra
nop
.global sceMpegRingbufferPut
sceMpegRingbufferPut: jr $ra
nop
.section .lib.stub,"a",@progbits
imports_begin:
.word name_0, 0x00010000, 65541, nids_0, stubs_0
.word name_1, 0x00010000, 65541, nids_1, stubs_1
.word name_2, 0x00010000, 65541, nids_2, stubs_2
.word name_3, 0x00010000, 65541, nids_3, stubs_3
.word name_4, 0x00010000, 1245189, nids_4, stubs_4
imports_end:
.section .rodata.sceModuleInfo,"a",@progbits
.align 2
.word 0
.asciz "IndependentMpegProbe"
.space 7
.word 0,0,0,imports_begin,imports_end
