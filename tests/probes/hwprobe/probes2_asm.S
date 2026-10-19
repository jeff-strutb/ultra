# hwprobe: per-class instruction retirement (S_RETIRE), cache and TLB refill (S_REFILL), and
# register access latency (S_REG_READ / S_REG_WRITE). Each routine returns a Count delta.
# Register numbers as in probes_asm.S.

	.set noreorder
	.set noat
	.set mips3
	.text

	.macro count r
	mfc0  \r, $9
	.endm

# ---------------------------------------------------------------------------------------------
# uint32_t hwp_rt_<n>(uint32_t* cached, uint32_t uncached): 32 copies of a body.
# Operands: $10 = 12345, $11 = 3, $13 = FCSR, $f0 = 1.5f, $f1 = 2.25f, $f2 = (int)7,
# $f8 = 1.5, $f9 = 2.25 (double), $f4/$f5 scratch. a0 is a D-cache-resident buffer, a1 an
# uncached RDRAM address.
	.macro rt_setup
	li    $10, 12345
	li    $11, 3
	cfc1  $13, $31
	lui   $8, 0x3FC0           # 1.5f
	mtc1  $8, $f0
	lui   $8, 0x4010           # 2.25f
	mtc1  $8, $f1
	li    $8, 7
	mtc1  $8, $f2
	lui   $8, 0x3FF8           # 1.5 (double, high word)
	dsll32 $8, $8, 0
	dmtc1 $8, $f8
	lui   $8, 0x4002           # 2.25 (double)
	dsll32 $8, $8, 0
	dmtc1 $8, $f9
	mtc1  $0, $f4
	move  $25, $31
	lw    $12, 0($4)           # make sure the buffer line is resident
	nop
	.endm

	.macro rt name, body
	.balign 32
	.global hwp_rt_\name
hwp_rt_\name:
	rt_setup
	count $8
	.rept 32
	\body
	.endr
9:	count $9
	move  $31, $25
	jr    $31
	subu  $2, $9, $8
	.endm

	rt nop,      "nop"
	rt addu,     "addu $12, $10, $11"
	rt sll,      "sll $12, $10, 3"
	rt dsll32,   "dsll32 $12, $10, 3"
	rt lui,      "lui $12, 0x1234"
	rt slt,      "slt $12, $10, $11"
	rt daddu,    "daddu $12, $10, $11"
	rt mult,     "mult $10, $11"
	rt multu,    "multu $10, $11"
	rt dmult,    "dmult $10, $11"
	rt dmultu,   "dmultu $10, $11"
	rt div,      "div $0, $10, $11"
	rt divu,     "divu $0, $10, $11"
	rt ddiv,     "ddiv $0, $10, $11"
	rt ddivu,    "ddivu $0, $10, $11"
	rt mult_mflo, "mult $10, $11; mflo $12"
	rt lw,       "lw $12, 0($4)"
	rt ld,       "ld $12, 0($4)"
	rt lb,       "lb $12, 1($4)"
	rt lwl_lwr,  "lwl $12, 1($4); lwr $12, 4($4)"
	rt sw,       "sw $10, 0($4)"
	rt sd,       "sd $10, 0($4)"
	rt sb,       "sb $10, 1($4)"
	rt load_use, "lw $12, 0($4); addu $14, $12, $11"
	rt beq_t,    "beq $0, $0, 1f; nop; 1:"
	rt bne_nt,   "bne $0, $0, 9f; nop"
	rt beql_nt,  "beql $10, $0, 9f; nop"
	rt j,        "j 1f; nop; 1:"
	rt jal,      "jal 1f; nop; 1:"
	rt jr,       "la $24, 1f; jr $24; nop; 1:"
	rt mfc0,     "mfc0 $12, $12"
	rt mtc0,     "mtc0 $0, $2"
	rt mfhi,     "mfhi $12"
	rt add_s,    "add.s $f4, $f0, $f1"
	rt add_d,    "add.d $f4, $f8, $f9"
	rt mul_s,    "mul.s $f4, $f0, $f1"
	rt mul_d,    "mul.d $f4, $f8, $f9"
	rt div_s,    "div.s $f4, $f0, $f1"
	rt div_d,    "div.d $f4, $f8, $f9"
	rt sqrt_s,   "sqrt.s $f4, $f1"
	rt sqrt_d,   "sqrt.d $f4, $f9"
	rt cvt_s_w,  "cvt.s.w $f4, $f2"
	rt cvt_w_s,  "cvt.w.s $f4, $f0"
	rt cvt_d_s,  "cvt.d.s $f4, $f0"
	rt mov_s,    "mov.s $f4, $f0"
	rt neg_s,    "neg.s $f4, $f0"
	rt c_eq_bc1t, "c.eq.s $f0, $f1; bc1t 9f; nop"
	rt add_s_dep, "add.s $f4, $f4, $f1"
	rt mul_d_dep, "mul.d $f9, $f9, $f8"
	rt lwc1,     "lwc1 $f5, 0($4)"
	rt swc1,     "swc1 $f5, 0($4)"
	rt ldc1,     "ldc1 $f5, 0($4)"
	rt mfc1,     "mfc1 $12, $f0"
	rt mtc1,     "mtc1 $10, $f5"
	rt cfc1,     "cfc1 $12, $31"
	rt ctc1,     "ctc1 $13, $31"
	rt lw_unc,   "lw $12, 0($5)"
	rt sw_unc,   "sw $10, 0($5)"
	rt sync,     "sync"

	.section .rodata
	.balign 4
	.global hwp_rt_table
hwp_rt_table:               # S_RETIRE order
	.word hwp_rt_nop, hwp_rt_addu, hwp_rt_sll, hwp_rt_dsll32, hwp_rt_lui, hwp_rt_slt
	.word hwp_rt_daddu, hwp_rt_mult, hwp_rt_multu, hwp_rt_dmult, hwp_rt_dmultu, hwp_rt_div
	.word hwp_rt_divu, hwp_rt_ddiv, hwp_rt_ddivu, hwp_rt_mult_mflo, hwp_rt_lw, hwp_rt_ld
	.word hwp_rt_lb, hwp_rt_lwl_lwr, hwp_rt_sw, hwp_rt_sd, hwp_rt_sb, hwp_rt_load_use
	.word hwp_rt_beq_t, hwp_rt_bne_nt, hwp_rt_beql_nt, hwp_rt_j, hwp_rt_jal, hwp_rt_jr
	.word hwp_rt_mfc0, hwp_rt_mtc0, hwp_rt_mfhi, hwp_rt_add_s, hwp_rt_add_d, hwp_rt_mul_s
	.word hwp_rt_mul_d, hwp_rt_div_s, hwp_rt_div_d, hwp_rt_sqrt_s, hwp_rt_sqrt_d
	.word hwp_rt_cvt_s_w, hwp_rt_cvt_w_s, hwp_rt_cvt_d_s, hwp_rt_mov_s, hwp_rt_neg_s
	.word hwp_rt_c_eq_bc1t, hwp_rt_add_s_dep, hwp_rt_mul_d_dep, hwp_rt_lwc1, hwp_rt_swc1
	.word hwp_rt_ldc1, hwp_rt_mfc1, hwp_rt_mtc1, hwp_rt_cfc1, hwp_rt_ctc1, hwp_rt_lw_unc
	.word hwp_rt_sw_unc, hwp_rt_sync
	.global hwp_rt_count
hwp_rt_count:
	.word (hwp_rt_count - hwp_rt_table) / 4
	.text

# ---------------------------------------------------------------------------------------------
# uint32_t hwp_icold(int cold): runs a 32-line block of NOPs; with cold != 0 every line is
# Hit_Invalidated first (outside the timed region).
	.balign 32
	.global hwp_icold
hwp_icold:
	la    $10, 2f
	beqz  $4, 1f
	li    $11, 32
3:	cache 0x10, 0($10)
	addiu $11, $11, -1
	bnez  $11, 3b
	addiu $10, $10, 32
1:	count $8
	.balign 32
2:	.rept 256
	nop
	.endr
	count $9
	jr    $31
	subu  $2, $9, $8

# uint32_t hwp_dfill(uint32_t* lines): 32 loads, one per 16-byte line.
	.global hwp_dfill
hwp_dfill:
	count $8
	.set  k, 0
	.rept 32
	lw    $12, (16 * k)($4)
	.set  k, k + 1
	.endr
	count $9
	jr    $31
	subu  $2, $9, $8

# uint32_t hwp_tlbx_load(uint32_t vaddr): one load with k1 set for the minimal handlers.
	.global hwp_tlbx_load
hwp_tlbx_load:
	la    $27, 1f
	count $8
	lw    $12, 0($4)
1:	count $9
	jr    $31
	subu  $2, $9, $8

# ---------------------------------------------------------------------------------------------
# uint32_t hwp_rd64(uint32_t addr): 64 back-to-back uncached loads.
	.global hwp_rd64
hwp_rd64:
	count $8
	.rept 64
	lw    $12, 0($4)
	.endr
	count $9
	jr    $31
	subu  $2, $9, $8

# uint32_t hwp_wr64(uint32_t addr, uint32_t value, uint32_t drain): 64 stores, then a load
# from `drain` (uncached RDRAM) so the write buffer is empty when Count is read.
	.global hwp_wr64
hwp_wr64:
	count $8
	.rept 64
	sw    $5, 0($4)
	.endr
	sync
	lw    $12, 0($6)
	count $9
	jr    $31
	subu  $2, $9, $8
