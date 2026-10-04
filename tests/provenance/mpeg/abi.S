.set noreorder
.text
.global probeConstruct
probeConstruct:
    lw $t0,16($sp)
    j sceMpegRingbufferConstruct
    lw $t1,20($sp)
.global probeCreate
probeCreate:
    lw $t0,16($sp)
    lw $t1,20($sp)
    j sceMpegCreate
    lw $t2,24($sp)
