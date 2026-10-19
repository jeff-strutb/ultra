# hwprobe CPU functional sweeps: run one instruction word built at runtime (cpu_func.c).
# The 16-byte buffer holds {insn, nop, jr $16, nop}; branches and jumps encoded with a zero
# offset land on the NOP, and $16 (s0) holds the way back, so an instruction that writes $31
# cannot break the return. An exception resumes at the same place through k1 (the full
# handler, HWP_EXPECT_RESUME).

	.set noreorder
	.set noat
	.set mips3
	.text

# void hwp_exec_one(uint32_t* buf): all GPRs the instruction can name are as the caller left
# them; the sweep encodes rs = rt = rd = 0, so only $0 is read or written.
	.global hwp_exec_one
hwp_exec_one:
	daddiu $29, $29, -16
	sd    $16, 0($29)
	sd    $31, 8($29)
	la    $16, 1f
	move  $27, $16
	jr    $4
	nop
1:	ld    $31, 8($29)
	ld    $16, 0($29)
	jr    $31
	daddiu $29, $29, 16

# void hwp_exec_fp(uint32_t* buf, uint64_t io[6]): io = {fs, ft, fcsr} in, {$f6, FCSR, $f2}
# out at io[3..5]. $f6 starts as 0xDEADBEEF_DEADBEEF; the instruction names fs = 2, ft = 4,
# fd = 6.
	.global hwp_exec_fp
hwp_exec_fp:
	daddiu $29, $29, -24
	sd    $16, 0($29)
	sd    $31, 8($29)
	sd    $5, 16($29)
	ld    $8, 0($5)
	dmtc1 $8, $f2
	ld    $8, 8($5)
	dmtc1 $8, $f4
	li    $9, 0xDEADBEEF
	dsll32 $10, $9, 0
	dsll32 $9, $9, 0
	dsrl32 $9, $9, 0
	or    $9, $9, $10
	dmtc1 $9, $f6
	ld    $8, 16($5)
	ctc1  $8, $31
	la    $16, 1f
	move  $27, $16
	jr    $4
	nop
1:	cfc1  $9, $31
	ctc1  $0, $31
	ld    $8, 16($29)
	dmfc1 $10, $f6
	sd    $10, 24($8)
	sd    $9, 32($8)
	dmfc1 $10, $f2
	sd    $10, 40($8)
	ld    $31, 8($29)
	ld    $16, 0($29)
	jr    $31
	daddiu $29, $29, 24

# void hwp_exec_int(uint32_t* buf, uint64_t rs, uint64_t rt, uint64_t out[3]): $8 = rs, $9 = rt,
# $10 = 0x5A5A..., HI = LO = 0xA5A5...; the instruction names rs = 8, rt = 9, rd = 10;
# out = {$10, HI, LO}.
	.global hwp_exec_int
hwp_exec_int:
	daddiu $29, $29, -24
	sd    $16, 0($29)
	sd    $31, 8($29)
	sd    $7, 16($29)
	li    $10, 0xA5A5A5A5
	dsll32 $11, $10, 0
	dsll32 $10, $10, 0
	dsrl32 $10, $10, 0
	or    $10, $10, $11
	mthi  $10
	mtlo  $10
	li    $11, 0x5A5A5A5A
	dsll32 $12, $11, 0
	dsll32 $11, $11, 0
	dsrl32 $11, $11, 0
	or    $10, $11, $12
	move  $8, $5
	move  $9, $6
	la    $16, 1f
	move  $27, $16
	jr    $4
	nop
1:	ld    $7, 16($29)
	sd    $10, 0($7)
	mfhi  $11
	sd    $11, 8($7)
	mflo  $11
	sd    $11, 16($7)
	ld    $31, 8($29)
	ld    $16, 0($29)
	jr    $31
	daddiu $29, $29, 24

# void hwp_unaligned(int op, uint32_t off, uint8_t* buf16, uint64_t out[1]): $9 preset to
# 0x1122334455667788, then the op at buf16 + off; out = $9 (loads; the caller reads memory for
# stores). op: 0 LWL, 1 LWR, 2 LDL, 3 LDR, 4 SWL, 5 SWR, 6 SDL, 7 SDR.
	.global hwp_unaligned
hwp_unaligned:
	li    $9, 0x11223344
	dsll32 $9, $9, 0
	li    $10, 0x55667788
	dsll32 $10, $10, 0
	dsrl32 $10, $10, 0
	or    $9, $9, $10
	addu  $8, $6, $5
	sll   $4, $4, 3            # 8 bytes per table entry below
	la    $10, 2f
	addu  $10, $10, $4
	jr    $10
	nop
2:	b     3f
	lwl   $9, 0($8)
	b     3f
	lwr   $9, 0($8)
	b     3f
	ldl   $9, 0($8)
	b     3f
	ldr   $9, 0($8)
	b     3f
	swl   $9, 0($8)
	b     3f
	swr   $9, 0($8)
	b     3f
	sdl   $9, 0($8)
	b     3f
	sdr   $9, 0($8)
3:	jr    $31
	sd    $9, 0($7)

# void hwp_llsc(int kind, uint32_t* word, uint64_t out[3]): out = {SC result, word after,
# LLAddr}. kind: 0 LL then SC; 1 SC without LL (after an ERET cleared LLbit); 2 LL, store to
# the word, SC; 3 LLD then SCD; 4 LL, ERET, SC.
	.global hwp_llsc
hwp_llsc:
	li    $9, 0x1234
	li    $11, 1
	beq   $4, $11, 1f
	li    $11, 2
	beq   $4, $11, 2f
	li    $11, 3
	beq   $4, $11, 3f
	li    $11, 4
	beq   $4, $11, 4f
	nop
	ll    $10, 0($5)           # 0
	b     9f
	sc    $9, 0($5)
1:	la    $12, 5f              # 1: ERET clears LLbit
	dmtc0 $12, $14
	nop
	nop
	eret
5:	b     9f
	sc    $9, 0($5)
2:	ll    $10, 0($5)           # 2
	sw    $0, 0($5)
	b     9f
	sc    $9, 0($5)
3:	lld   $10, 0($5)           # 3 (word must be 8-aligned)
	b     9f
	scd   $9, 0($5)
4:	ll    $10, 0($5)           # 4
	la    $12, 6f
	dmtc0 $12, $14
	nop
	nop
	eret
6:	sc    $9, 0($5)
9:	sd    $9, 0($6)
	lw    $10, 0($5)
	sd    $10, 8($6)
	mfc0  $10, $17
	jr    $31
	sd    $10, 16($6)

# uint32_t hwp_cache_tag(uint32_t op, uint32_t addr): CACHE op (Index_Load_Tag 0x04 for I,
# 0x05 for D) at addr; returns TagLo.
	.global hwp_cache_tag
hwp_cache_tag:
	mtc0  $0, $28
	nop
	nop
	li    $8, 0x04
	beq   $4, $8, 1f
	nop
	cache 0x05, 0($5)
	b     2f
	nop
1:	cache 0x04, 0($5)
2:	nop
	nop
	mfc0  $2, $28
	jr    $31
	nop
