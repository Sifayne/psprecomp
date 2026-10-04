.set noreorder
.text
/* The compiler passes a fifth integer argument on the stack; the firmware
 * takes it in $t0. Bridged here for the two seek calls whose 64-bit offset
 * occupies $a2:$a3. */
.global probeLseek
probeLseek:
    lw $t0,16($sp)
    j sceIoLseek
    nop
.global probeLseekAsync
probeLseekAsync:
    lw $t0,16($sp)
    j sceIoLseekAsync
    nop
