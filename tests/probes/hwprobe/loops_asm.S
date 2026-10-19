# Uncached-read timing loops (docs/timing/hwprobe.md, "Uncached-read loop timing").
# The loop body is krom CPUTest TIMINGNTSC's {op; lw VI_V_CURRENT; sync; bne; addiu}, with
# krom's operands: t1 = -123456789, t2 = 1 (VALUEWORDA/B), f0 = 0.0, f1 = 1.0 (VALUEFLOATA/B).
#
# void hwp_raw_<op>(uint8_t* buf, uint8_t* end, uint32_t reg, uint32_t counts[2])
#   one Count low byte per iteration into buf (pre-loaded into the D-cache by the caller);
#   counts = Count before the first iteration and after the last.
# void hwp_line_<op>(uint8_t* buf, uint8_t* end, uint32_t vi_v_current)
#   waits for VI_V_CURRENT == 0, then stores the iteration count of each VI_V_CURRENT value.

	.set noreorder
	.set noat
	.set mips3
	.text

	.macro operands
	li    $9, -123456789
	li    $10, 1
	mtc1  $0, $f0
	lui   $8, 0x3F80           # 1.0f
	mtc1  $8, $f1
	nop
	.endm

	.macro rawloop name, op:vararg
	.global hwp_raw_\name
	.type hwp_raw_\name, @function
	.balign 32
hwp_raw_\name:
	operands
	mfc0  $8, $9
	b     1f
	sw    $8, 0($7)
	.balign 32
1:	\op
	lw    $14, 0($6)
	sync
	mfc0  $8, $9
	addiu $4, $4, 1
	bne   $4, $5, 1b
	sb    $8, -1($4)
	mfc0  $8, $9
	jr    $31
	sw    $8, 4($7)
	.size hwp_raw_\name, . - hwp_raw_\name
	.endm

	.macro lineloop name, op:vararg
	.global hwp_line_\name
	.type hwp_line_\name, @function
	.balign 32
hwp_line_\name:
	operands
2:	lw    $14, 0($6)           # krom's WAITSTART: wait for half-line 0
	sync
	bnez  $14, 2b
	nop
	move  $12, $14
	b     1f
	li    $8, 0
	.balign 32
1:	\op
	lw    $14, 0($6)
	sync
	beq   $14, $12, 1b
	addiu $8, $8, 1
	sb    $8, 0($4)
	addiu $4, $4, 1
	move  $12, $14
	bne   $4, $5, 1b
	li    $8, 0
	jr    $31
	nop
	.size hwp_line_\name, . - hwp_line_\name
	.endm

	rawloop alu,   add $9, $9, $10
	rawloop muls0, mul.s $f0, $f0, $f1
	rawloop adds,  add.s $f0, $f0, $f1
	rawloop mult,  mult $9, $10
	rawloop dmult, dmult $9, $10
	rawloop div,   div $0, $9, $10
	rawloop ddiv,  ddiv $0, $9, $10

	lineloop alu,   add $9, $9, $10
	lineloop muls0, mul.s $f0, $f0, $f1
	lineloop adds,  add.s $f0, $f0, $f1
	lineloop mult,  mult $9, $10
	lineloop dmult, dmult $9, $10
	lineloop div,   div $0, $9, $10
	lineloop ddiv,  ddiv $0, $9, $10
