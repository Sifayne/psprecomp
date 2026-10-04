/* Bridge compiler o32 stack arguments to the PSP syscall register ABI. */
.set noreorder
.text
.global probeCreateThread
probeCreateThread:
    lw $t0,16($sp)
    j sceKernelCreateThread
    lw $t1,20($sp)
