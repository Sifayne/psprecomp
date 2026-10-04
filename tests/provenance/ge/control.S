.set noreorder
.text
.global read_controls
read_controls:
.word 0x48680080
sw $8,0($4)
.word 0x48680081
sw $8,4($4)
.word 0x48680082
sw $8,8($4)
.word 0x48680088
sw $8,12($4)
jr $ra
nop
.global write_controls
write_controls:
.word 0x48e40080
.word 0x48e50081
.word 0x48e60082
.word 0x48e70088
jr $ra
nop
