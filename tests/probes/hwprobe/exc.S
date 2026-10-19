# hwprobe exception handling: one dispatch for all four vectors, a protected call, and the
# minimal 0x180 handler used by the redirect-time probe. Layout of hwp_exc: hwp_exc_t in hwp.h.

	.set noreorder
	.set noat
	.set mips3

	.section .data
	.balign 8
	.global hwp_exc
hwp_exc:
	.space 88
	.balign 8
hwp_jmp:
	.space 96                  # s0-s7, gp, sp, fp, ra

	.text

# Copied to 0x80000000, 0x80000080, 0x80000100 and 0x80000180 (4 words).
	.global hwp_vec_stub
hwp_vec_stub:
	lui   $26, %hi(hwp_exc_dispatch)
	addiu $26, $26, %lo(hwp_exc_dispatch)
	jr    $26
	nop

# Copied to 0x80000180 for S_EXC_REDIRECT: resume at k1 at once.
	.global hwp_vec_minimal
hwp_vec_minimal:
	mtc0  $27, $14
	nop
	nop
	eret

	.global hwp_exc_dispatch
hwp_exc_dispatch:
	lui   $26, %hi(hwp_exc)
	addiu $26, $26, %lo(hwp_exc)
	sd    $1, 0($26)
	mfc0  $1, $9
	sw    $1, 8($26)
	mfc0  $1, $13
	sw    $1, 12($26)
	dmfc0 $1, $14
	sd    $1, 16($26)
	dmfc0 $1, $8
	sd    $1, 24($26)
	mfc0  $1, $12
	sw    $1, 32($26)
	srl   $1, $1, 29           # CU1: save and clear FCSR so an FPE does not repeat
	andi  $1, $1, 1
	beqz  $1, 1f
	nop
	cfc1  $1, $31
	sw    $1, 36($26)
	ctc1  $0, $31
1:	lw    $1, 40($26)
	bnez  $1, 2f
	nop
	lw    $1, 12($26)
	sw    $1, 56($26)
	lw    $1, 8($26)
	sw    $1, 60($26)
	ld    $1, 16($26)
	sd    $1, 64($26)
	ld    $1, 24($26)
	sd    $1, 72($26)
	lw    $1, 32($26)
	sw    $1, 80($26)
2:	lw    $1, 40($26)
	addiu $1, $1, 1
	sw    $1, 40($26)
	lw    $1, 12($26)          # timer interrupt: a Compare write clears IP7
	andi  $1, $1, 0x8000
	beqz  $1, 3f
	nop
	mtc0  $0, $11
3:	lw    $1, 44($26)
	beqz  $1, unexpected
	addiu $1, $1, -1
	beqz  $1, resume
	nop
	# HWP_EXPECT_PRENMI: record Count in low RAM, mask IM4 (the pre-NMI line stays asserted
	# until the reset), return to the interrupted instruction.
	lw    $1, 8($26)
	lui   $26, 0xA000
	sw    $1, 0x384($26)
	lw    $1, 0x38C($26)
	addiu $1, $1, 1
	sw    $1, 0x38C($26)
	mfc0  $1, $12
	ori   $1, $1, 0x1000
	xori  $1, $1, 0x1000
	mtc0  $1, $12
	lui   $26, %hi(hwp_exc)
	addiu $26, $26, %lo(hwp_exc)
	b     out
	nop
resume:
	beqz  $27, 4f
	nop
	dmtc0 $27, $14
	b     out
	nop
4:	dmfc0 $1, $14
	daddiu $1, $1, 4
	dmtc0 $1, $14
	b     out
	nop
unexpected:
	lw    $1, 52($26)
	addiu $1, $1, 1
	sw    $1, 52($26)
	lw    $1, 48($26)
	mtc0  $1, $14
out:
	ld    $1, 0($26)
	nop
	nop
	eret

# int hwp_try(void (*fn)(void*), void* arg)
	.global hwp_try
	.type hwp_try, @function
hwp_try:
	lui   $8, %hi(hwp_jmp)
	addiu $8, $8, %lo(hwp_jmp)
	sd    $16, 0($8)
	sd    $17, 8($8)
	sd    $18, 16($8)
	sd    $19, 24($8)
	sd    $20, 32($8)
	sd    $21, 40($8)
	sd    $22, 48($8)
	sd    $23, 56($8)
	sd    $28, 64($8)
	sd    $29, 72($8)
	sd    $30, 80($8)
	sd    $31, 88($8)
	la    $9, hwp_try_abort
	lui   $10, %hi(hwp_exc)
	addiu $10, $10, %lo(hwp_exc)
	sw    $9, 48($10)
	move  $25, $4
	jalr  $25
	move  $4, $5
	lui   $8, %hi(hwp_jmp)
	addiu $8, $8, %lo(hwp_jmp)
	ld    $31, 88($8)
	jr    $31
	li    $2, 0
hwp_try_abort:
	lui   $8, %hi(hwp_jmp)
	addiu $8, $8, %lo(hwp_jmp)
	ld    $16, 0($8)
	ld    $17, 8($8)
	ld    $18, 16($8)
	ld    $19, 24($8)
	ld    $20, 32($8)
	ld    $21, 40($8)
	ld    $22, 48($8)
	ld    $23, 56($8)
	ld    $28, 64($8)
	ld    $29, 72($8)
	ld    $30, 80($8)
	ld    $31, 88($8)
	jr    $31
	li    $2, -1
	.size hwp_try, . - hwp_try
