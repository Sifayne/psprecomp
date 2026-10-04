.set noreorder
.text
.global rng_revision
rng_revision:
.word 0x48620087
jr $ra
nop
.global rng_seed
rng_seed:
.word 0x48e40000
.word 0xd0200000
jr $ra
nop
.global rng_next
rng_next:
.word 0xd0210000
.word 0x48620000
jr $ra
nop
.global rng_read
rng_read:
.word 0x48680088
sw $8,0($4)
.word 0x48680089
sw $8,4($4)
.word 0x4868008a
sw $8,8($4)
.word 0x4868008b
sw $8,12($4)
.word 0x4868008c
sw $8,16($4)
.word 0x4868008d
sw $8,20($4)
.word 0x4868008e
sw $8,24($4)
.word 0x4868008f
sw $8,28($4)
jr $ra
nop
.global rng_write
rng_write:
lw $8,0($4)
.word 0x48e80088
lw $8,4($4)
.word 0x48e80089
lw $8,8($4)
.word 0x48e8008a
lw $8,12($4)
.word 0x48e8008b
lw $8,16($4)
.word 0x48e8008c
lw $8,20($4)
.word 0x48e8008d
lw $8,24($4)
.word 0x48e8008e
lw $8,28($4)
.word 0x48e8008f
jr $ra
nop

.global rng_draw_0_0
rng_draw_0_0:
lui $8,0x4abc
ori $8,$8,0xdef0
.word 0x48e80000
.word 0x48e80020
.word 0x48e80040
.word 0x48e80060
lui $8,0x000f
ori $8,$8,0xffff
.word 0x48e80080
.word 0x48e80081
.word 0x48e40082
.word 0xd0210000
.word 0x48680000
sw $8,0($5)
.word 0x48680020
sw $8,4($5)
.word 0x48680040
sw $8,8($5)
.word 0x48680060
sw $8,12($5)
jr $ra
nop
.global rng_draw_0_1
rng_draw_0_1:
lui $8,0x4abc
ori $8,$8,0xdef0
.word 0x48e80000
.word 0x48e80020
.word 0x48e80040
.word 0x48e80060
lui $8,0x000f
ori $8,$8,0xffff
.word 0x48e80080
.word 0x48e80081
.word 0x48e40082
.word 0xd0210080
.word 0x48680000
sw $8,0($5)
.word 0x48680020
sw $8,4($5)
.word 0x48680040
sw $8,8($5)
.word 0x48680060
sw $8,12($5)
jr $ra
nop
.global rng_draw_0_2
rng_draw_0_2:
lui $8,0x4abc
ori $8,$8,0xdef0
.word 0x48e80000
.word 0x48e80020
.word 0x48e80040
.word 0x48e80060
lui $8,0x000f
ori $8,$8,0xffff
.word 0x48e80080
.word 0x48e80081
.word 0x48e40082
.word 0xd0218000
.word 0x48680000
sw $8,0($5)
.word 0x48680020
sw $8,4($5)
.word 0x48680040
sw $8,8($5)
.word 0x48680060
sw $8,12($5)
jr $ra
nop
.global rng_draw_0_3
rng_draw_0_3:
lui $8,0x4abc
ori $8,$8,0xdef0
.word 0x48e80000
.word 0x48e80020
.word 0x48e80040
.word 0x48e80060
lui $8,0x000f
ori $8,$8,0xffff
.word 0x48e80080
.word 0x48e80081
.word 0x48e40082
.word 0xd0218080
.word 0x48680000
sw $8,0($5)
.word 0x48680020
sw $8,4($5)
.word 0x48680040
sw $8,8($5)
.word 0x48680060
sw $8,12($5)
jr $ra
nop
.global rng_draw_1_0
rng_draw_1_0:
lui $8,0x4abc
ori $8,$8,0xdef0
.word 0x48e80000
.word 0x48e80020
.word 0x48e80040
.word 0x48e80060
lui $8,0x000f
ori $8,$8,0xffff
.word 0x48e80080
.word 0x48e80081
.word 0x48e40082
.word 0xd0220000
.word 0x48680000
sw $8,0($5)
.word 0x48680020
sw $8,4($5)
.word 0x48680040
sw $8,8($5)
.word 0x48680060
sw $8,12($5)
jr $ra
nop
.global rng_draw_1_1
rng_draw_1_1:
lui $8,0x4abc
ori $8,$8,0xdef0
.word 0x48e80000
.word 0x48e80020
.word 0x48e80040
.word 0x48e80060
lui $8,0x000f
ori $8,$8,0xffff
.word 0x48e80080
.word 0x48e80081
.word 0x48e40082
.word 0xd0220080
.word 0x48680000
sw $8,0($5)
.word 0x48680020
sw $8,4($5)
.word 0x48680040
sw $8,8($5)
.word 0x48680060
sw $8,12($5)
jr $ra
nop
.global rng_draw_1_2
rng_draw_1_2:
lui $8,0x4abc
ori $8,$8,0xdef0
.word 0x48e80000
.word 0x48e80020
.word 0x48e80040
.word 0x48e80060
lui $8,0x000f
ori $8,$8,0xffff
.word 0x48e80080
.word 0x48e80081
.word 0x48e40082
.word 0xd0228000
.word 0x48680000
sw $8,0($5)
.word 0x48680020
sw $8,4($5)
.word 0x48680040
sw $8,8($5)
.word 0x48680060
sw $8,12($5)
jr $ra
nop
.global rng_draw_1_3
rng_draw_1_3:
lui $8,0x4abc
ori $8,$8,0xdef0
.word 0x48e80000
.word 0x48e80020
.word 0x48e80040
.word 0x48e80060
lui $8,0x000f
ori $8,$8,0xffff
.word 0x48e80080
.word 0x48e80081
.word 0x48e40082
.word 0xd0228080
.word 0x48680000
sw $8,0($5)
.word 0x48680020
sw $8,4($5)
.word 0x48680040
sw $8,8($5)
.word 0x48680060
sw $8,12($5)
jr $ra
nop
.global rng_draw_2_0
rng_draw_2_0:
lui $8,0x4abc
ori $8,$8,0xdef0
.word 0x48e80000
.word 0x48e80020
.word 0x48e80040
.word 0x48e80060
lui $8,0x000f
ori $8,$8,0xffff
.word 0x48e80080
.word 0x48e80081
.word 0x48e40082
.word 0xd0230000
.word 0x48680000
sw $8,0($5)
.word 0x48680020
sw $8,4($5)
.word 0x48680040
sw $8,8($5)
.word 0x48680060
sw $8,12($5)
jr $ra
nop
.global rng_draw_2_1
rng_draw_2_1:
lui $8,0x4abc
ori $8,$8,0xdef0
.word 0x48e80000
.word 0x48e80020
.word 0x48e80040
.word 0x48e80060
lui $8,0x000f
ori $8,$8,0xffff
.word 0x48e80080
.word 0x48e80081
.word 0x48e40082
.word 0xd0230080
.word 0x48680000
sw $8,0($5)
.word 0x48680020
sw $8,4($5)
.word 0x48680040
sw $8,8($5)
.word 0x48680060
sw $8,12($5)
jr $ra
nop
.global rng_draw_2_2
rng_draw_2_2:
lui $8,0x4abc
ori $8,$8,0xdef0
.word 0x48e80000
.word 0x48e80020
.word 0x48e80040
.word 0x48e80060
lui $8,0x000f
ori $8,$8,0xffff
.word 0x48e80080
.word 0x48e80081
.word 0x48e40082
.word 0xd0238000
.word 0x48680000
sw $8,0($5)
.word 0x48680020
sw $8,4($5)
.word 0x48680040
sw $8,8($5)
.word 0x48680060
sw $8,12($5)
jr $ra
nop
.global rng_draw_2_3
rng_draw_2_3:
lui $8,0x4abc
ori $8,$8,0xdef0
.word 0x48e80000
.word 0x48e80020
.word 0x48e80040
.word 0x48e80060
lui $8,0x000f
ori $8,$8,0xffff
.word 0x48e80080
.word 0x48e80081
.word 0x48e40082
.word 0xd0238080
.word 0x48680000
sw $8,0($5)
.word 0x48680020
sw $8,4($5)
.word 0x48680040
sw $8,8($5)
.word 0x48680060
sw $8,12($5)
jr $ra
nop

.global rng_read_controls
rng_read_controls:
.word 0x48680080
sw $8,0($4)
.word 0x48680081
sw $8,4($4)
.word 0x48680082
sw $8,8($4)
jr $ra
nop
.global rng_seed_prefixed
rng_seed_prefixed:
.word 0x48e40000
lui $8,0x4000
.word 0x48e80020
lui $8,0x4040
.word 0x48e80040
lui $8,0x4080
.word 0x48e80060
.word 0x48e50080
.word 0xd0200000
jr $ra
nop
