# hwprobe measurement routines. Each timed routine brackets its subject with MFC0 Count and
# returns the delta in v0. docs/timing/hwprobe.md describes every group; probes.c drives them.
# Register numbers are written out: $8-$15 t0-t7, $24/$25 t8/t9, $26/$27 k0/k1, $31 ra.

	.set noreorder
	.set noat
	.set mips3
	.text

	.macro count r
	mfc0  \r, $9
	.endm

	.macro ret_delta
	count $9
	jr    $31
	subu  $2, $9, $8
	.endm

	.macro nops n
	.rept \n
	nop
	.endr
	.endm

# ---------------------------------------------------------------------------------------------
# TLB access. void hwp_tlb_write(int index, uint32_t pagemask, uint64_t hi, uint64_t lo0,
# uint64_t lo1) is not o64-friendly with five arguments, so it takes a pointer:
# void hwp_tlb_write(int index, const uint64_t e[4])   e = PageMask, EntryHi, EntryLo0, EntryLo1
# void hwp_tlb_read(int index, uint64_t e[4])
# Both preserve EntryHi (the current ASID).
	.global hwp_tlb_write
hwp_tlb_write:
	dmfc0 $8, $10
	mtc0  $4, $0
	ld    $9, 0($5)
	mtc0  $9, $5
	ld    $9, 8($5)
	dmtc0 $9, $10
	ld    $9, 16($5)
	dmtc0 $9, $2
	ld    $9, 24($5)
	dmtc0 $9, $3
	nops  2
	tlbwi
	nops  4
	dmtc0 $8, $10
	nops  4
	jr    $31
	nop

	.global hwp_tlb_read
hwp_tlb_read:
	dmfc0 $8, $10
	mtc0  $4, $0
	nops  2
	tlbr
	nops  4
	dmfc0 $9, $5
	sd    $9, 0($5)
	dmfc0 $9, $10
	sd    $9, 8($5)
	dmfc0 $9, $2
	sd    $9, 16($5)
	dmfc0 $9, $3
	sd    $9, 24($5)
	dmtc0 $8, $10
	nops  4
	jr    $31
	nop

# ---------------------------------------------------------------------------------------------
# Exception redirect time (S_EXC_REDIRECT). 0x80000180 holds {mtc0 k1, EPC; nop; nop; eret}.
# Each baseline executes the same instructions with no exception: the faulting instruction is
# replaced by its non-faulting twin (or a NOP) and the handler's four instructions are inlined.
	.global hwp_x_syscall
hwp_x_syscall:
	la    $27, 1f
	count $8
	syscall
1:	ret_delta

	.global hwp_x_syscall_base
hwp_x_syscall_base:
	la    $27, 1f
	count $8
	nop
	mtc0  $27, $14
	nop
	nop
	eret
1:	ret_delta

	.global hwp_x_addov
hwp_x_addov:
	la    $27, 1f
	li    $10, 0x7FFFFFFF
	li    $11, 1
	count $8
	add   $12, $10, $11
1:	ret_delta

	.global hwp_x_addov_base
hwp_x_addov_base:
	la    $27, 1f
	li    $10, 0x7FFFFFFF
	li    $11, 1
	count $8
	addu  $12, $10, $11
	mtc0  $27, $14
	nop
	nop
	eret
1:	ret_delta

	.global hwp_x_adel
hwp_x_adel:
	la    $27, 1f
	la    $10, 2f
	addiu $10, $10, 2          # misaligned: the fetch at the target raises AdEL
	count $8
	jr    $10
	nop
2:	nop
1:	ret_delta

	.global hwp_x_adel_base
hwp_x_adel_base:
	la    $27, 1f
	la    $10, 2f
	count $8
	jr    $10
	nop
2:	mtc0  $27, $14
	nop
	nop
	eret
1:	ret_delta

	.global hwp_x_break
hwp_x_break:
	la    $27, 1f
	count $8
	break
1:	ret_delta

	.global hwp_x_break_base
hwp_x_break_base:
	la    $27, 1f
	count $8
	nop
	mtc0  $27, $14
	nop
	nop
	eret
1:	ret_delta

# ---------------------------------------------------------------------------------------------
# Interrupt acceptance (S_INT_ACCEPT). void hwp_int_<sled>(uint32_t k, uint32_t out[2])
# Arms Compare = Count + k, enables IE|IM7, and runs a 256-instruction sled. The full handler
# (HWP_EXPECT_RESUME) resumes at the exit label. out[0] = Count at arm, out[1] = sled base.
	.macro intsled name, body:vararg
	.global hwp_int_\name
	.balign 32
hwp_int_\name:
	la    $27, 3f
	la    $13, 2f
	sw    $13, 4($5)
	lui   $14, 0xA000          # uncached RDRAM for the LW sled
	li    $15, 7
	li    $24, 3
	mfc0  $10, $12
	ori   $11, $10, 0x8001
	count $8
	addu  $9, $8, $4
	mtc0  $9, $11
	sw    $8, 0($5)
	mtc0  $11, $12
	.balign 32
2:	.rept 256
	\body
	.endr
3:	mtc0  $10, $12             # interrupts off again
	nops  4
	jr    $31
	nop
	.endm

	intsled nop, nop
	intsled lw, lw $12, 0($14)
	intsled div, div $0, $15, $24

# ---------------------------------------------------------------------------------------------
# Micro-TLB. uint32_t hwp_itlb_time(uint32_t vaddr): Count delta of calling mapped code.
	.global hwp_itlb_time
hwp_itlb_time:
	move  $25, $31
	count $8
	jalr  $4
	nop
	move  $31, $25
	ret_delta

# void hwp_itlb_action(int kind, const uint64_t same[4], const uint64_t other[4], int index)
#   0 none, 1 TLBWI of `other` at index + 1, 2 TLBWI of `same` at index, 3 EntryHi rewrite with
#   the same value, 4 EntryHi with ASID + 1 then back, 5 TLBP of `same`, 6 TLBR of index.
	.global hwp_itlb_action
hwp_itlb_action:
	dmfc0 $8, $10              # current EntryHi (ASID)
	li    $9, 1
	beq   $4, $9, 1f
	li    $9, 2
	beq   $4, $9, 2f
	li    $9, 3
	beq   $4, $9, 3f
	li    $9, 4
	beq   $4, $9, 4f
	li    $9, 5
	beq   $4, $9, 5f
	li    $9, 6
	beq   $4, $9, 6f
	nop
	b     9f                   # 0: nothing
	nop
1:	move  $10, $6              # TLBWI of `other` at index + 1
	b     7f
	addiu $11, $7, 1
2:	move  $10, $5              # TLBWI of `same` at index
	move  $11, $7
7:	mtc0  $11, $0
	ld    $9, 0($10)
	mtc0  $9, $5
	ld    $9, 8($10)
	dmtc0 $9, $10
	ld    $9, 16($10)
	dmtc0 $9, $2
	ld    $9, 24($10)
	dmtc0 $9, $3
	nops  2
	tlbwi
	nops  4
	b     8f
	nop
3:	dmtc0 $8, $10              # EntryHi rewritten, same value
	nops  4
	b     9f
	nop
4:	andi  $9, $8, 0xFF         # ASID + 1, then back
	addiu $9, $9, 1
	andi  $9, $9, 0xFF
	dsrl  $10, $8, 8
	dsll  $10, $10, 8
	or    $9, $9, $10
	dmtc0 $9, $10
	nops  4
	b     8f
	nop
5:	ld    $9, 8($5)            # TLBP of `same`
	dmtc0 $9, $10
	nops  2
	tlbp
	nops  4
	b     8f
	nop
6:	mtc0  $7, $0               # TLBR of index
	nops  2
	tlbr
	nops  4
8:	dmtc0 $8, $10              # restore EntryHi
	nops  4
9:	jr    $31
	nop

# ---------------------------------------------------------------------------------------------
# CP0 hazards. uint32_t hwp_hz_lo0_<gap>(uint32_t out[16]): 16 x {MTC0 EntryLo0 (alternating
# values); gap NOPs; MFC0 EntryLo0; SW}; also _watch_ (WatchLo, R = W = 0) and _count_ (Count:
# out[i] = value read, the delta is meaningless).
	.macro hz name, reg, gap, a, b
	.global hwp_hz_\name\()_\gap
hwp_hz_\name\()_\gap:
	li    $10, \a
	li    $11, \b
	count $8
	.set  i, 0
	.rept 8
	mtc0  $10, \reg
	nops  \gap
	mfc0  $12, \reg
	sw    $12, (8 * i)($4)
	mtc0  $11, \reg
	nops  \gap
	mfc0  $12, \reg
	sw    $12, (8 * i + 4)($4)
	.set  i, i + 1
	.endr
	ret_delta
	.endm

	.irp g, 0, 1, 2, 3
	hz lo0, $2, \g, 0x01234567, 0x00ABCDE5
	hz watch, $18, \g, 0x00001000, 0x00002000
	hz count, $9, \g, 0x10000000, 0x20000000
	.endr

# ---------------------------------------------------------------------------------------------
# Pipeline overlap. uint32_t hwp_ov_<case>(uint32_t* line): the caller has made `line` miss or
# hit in the D-cache.
	.macro ovl name, load, mul
	.global hwp_ov_\name
hwp_ov_\name:
	li    $10, 12345
	li    $11, 3
	count $8
	\load
	\mul
	mflo  $13
	ret_delta
	.endm

	ovl lw_mult_indep, "lw $12, 0($4)", "mult $10, $11"
	ovl lw_mult_dep,   "lw $12, 0($4)", "mult $12, $11"
	ovl lw_div_indep,  "lw $12, 0($4)", "div $0, $10, $11"
	ovl lw_div_dep,    "lw $12, 0($4)", "div $0, $12, $11"

# uint32_t hwp_ovi_mult(int cold) / hwp_ovi_nop(int cold): the timed instruction is the last
# word of an I-cache line (32 bytes, VR4300 manual 11.2); with cold != 0 the next line is
# invalidated first.
	.macro ovi name, last
	.balign 32
	.global hwp_ovi_\name
hwp_ovi_\name:
	la    $25, 2f              # line A: 8 words
	beqz  $4, 1f
	nop
	cache 0x10, 0($25)         # Hit_Invalidate_I on line C
1:	nop
	nop
	nop
	li    $10, 12345           # line B: 8 words, the timed one last
	li    $11, 3
	count $8
	nop
	nop
	nop
	nop
	\last
2:	mflo  $13                  # line C
	ret_delta
	.endm

	ovi mult, "mult $10, $11"
	ovi nop, nop

# Branches. uint32_t hwp_br_<case>(void): 16 pairs.
	.global hwp_br_beql_nt
hwp_br_beql_nt:
	li    $10, 1
	count $8
	.rept 16
	beql  $10, $0, 9f
	nop
	.endr
	ret_delta
9:	break                      # never reached

	.global hwp_br_nop
hwp_br_nop:
	li    $10, 1
	count $8
	nops  32
	ret_delta

	.global hwp_br_beq_t
hwp_br_beq_t:
	li    $10, 1
	count $8
	.rept 16
	beq   $0, $0, 1f
	nop
1:
	.endr
	ret_delta

	.global hwp_br_bnel_t
hwp_br_bnel_t:
	li    $10, 1
	count $8
	.rept 16
	bnel  $10, $0, 1f
	nop
1:
	.endr
	ret_delta

# ERET with no exception (EXL set by MTC0) against J.
	.global hwp_eret_next
hwp_eret_next:
	mfc0  $10, $12
	ori   $11, $10, 0x0002     # EXL
	la    $12, 1f
	dmtc0 $12, $14
	mtc0  $11, $12
	nops  4
	count $8
	eret
1:	count $9
	mtc0  $10, $12             # ERET cleared EXL; restore the rest
	nops  4
	jr    $31
	subu  $2, $9, $8

	.global hwp_j_next
hwp_j_next:
	mfc0  $10, $12
	ori   $11, $10, 0x0000
	la    $12, 1f
	dmtc0 $12, $14
	mtc0  $11, $12
	nops  4
	count $8
	j     1f
	nop
1:	count $9
	mtc0  $10, $12
	nops  4
	jr    $31
	subu  $2, $9, $8

# ---------------------------------------------------------------------------------------------
# Cause.CE (S_CAUSE_CE). uint32_t hwp_ce_<exc>_<follower>(void) returns Cause from hwp_exc;
# the caller set HWP_EXPECT_RESUME. The follower is never executed.
	.macro ce exc, fol, setup, raise, follow
	.global hwp_ce_\exc\()_\fol
hwp_ce_\exc\()_\fol:
	la    $27, 1f
	\setup
	\raise
	\follow
1:	lui   $8, %hi(hwp_exc)
	addiu $8, $8, %lo(hwp_exc)
	jr    $31
	lw    $2, 12($8)
	.endm

	.macro ce_all exc, setup, raise
	ce \exc, nop,  \setup, "\raise", nop
	ce \exc, mfc1, \setup, "\raise", "mfc1 $9, $f0"
	ce \exc, mfc2, \setup, "\raise", "mfc2 $9, $0"
	ce \exc, lwc1, \setup, "\raise", "lwc1 $f2, 0($29)"
	ce \exc, mtc0, \setup, "\raise", "mtc0 $0, $11"
	.endm

	.macro setup_none
	nop
	.endm
	.macro setup_addov
	li    $10, 0x7FFFFFFF
	li    $11, 1
	.endm
	.macro setup_fpe
	li    $10, 0x00010800      # FCSR cause V (bit 16) with enable V (bit 11)
	.endm

	ce_all syscall, setup_none, syscall
	ce_all addov, setup_addov, "add $12, $10, $11"
	ce_all ctc1, setup_fpe, "ctc1 $10, $31"

# ---------------------------------------------------------------------------------------------
# Jump region in a delay slot (S_JUMP_REGION). void hwp_jr_j(uint32_t or_mask, uint32_t out[2])
# and hwp_jr_jal: JR to the outer target `tgt` (| or_mask), with J / JAL `marker` in its delay
# slot. The marker stores ra on entry (out[1]) and the BAL return address (out[0]), whose
# bits 31:28 say which region it ran in. Call through the 0xA alias with or_mask = 0 for A->8.
	.macro jrtest name, inner
	.balign 32
	.global hwp_jr_\name
hwp_jr_\name:
	move  $25, $31
	la    $8, 2f
	or    $8, $8, $4
	jr    $8
	\inner 3f
	break                      # never reached
2:	nop                        # the inner jump's delay slot, run from the outer target
3:	sw    $31, 4($5)
	bal   4f
	nop
4:	sw    $31, 0($5)
	jr    $25
	nop
	.endm

	jrtest j, j
	jrtest jal, jal

# ---------------------------------------------------------------------------------------------
# CVT from long (S_CVT_L). void hwp_cvt_s_l(int64_t in, uint64_t out[2]) and hwp_cvt_d_l:
# out[0] = result bits ($f4 preset to 0xDEADBEEF_DEADBEEF), out[1] = FCSR after (if no
# exception). The caller set HWP_EXPECT_RESUME and reads the exception from hwp_exc.
	.macro cvtl name, op
	.global hwp_cvt_\name
hwp_cvt_\name:
	li    $9, 0xDEADBEEF
	dsll32 $9, $9, 0
	li    $10, 0xDEADBEEF
	dsll32 $10, $10, 0
	dsrl32 $10, $10, 0
	or    $9, $9, $10
	dmtc1 $9, $f4
	dmtc1 $4, $f2
	ctc1  $0, $31
	la    $27, 1f
	\op   $f4, $f2
1:	cfc1  $9, $31
	dmfc1 $10, $f4
	sd    $10, 0($5)
	jr    $31
	sd    $9, 8($5)
	.endm

	cvtl s_l, cvt.s.l
	cvtl d_l, cvt.d.l

# ---------------------------------------------------------------------------------------------
# CACHE op cost (S_CACHE_OPS). uint32_t hwp_cop_<name>(uint32_t base): 64 ops at base + k *
# stride.
	.macro cop name, op, stride
	.global hwp_cop_\name
hwp_cop_\name:
	count $8
	.set  k, 0
	.rept 64
	cache \op, (k * \stride)($4)
	.set  k, k + 1
	.endr
	ret_delta
	.endm

	.global hwp_cop_nop
hwp_cop_nop:
	count $8
	nops  64
	ret_delta

	cop i_index_inv, 0x00, 32
	cop i_hit_inv,   0x10, 32
	cop i_fill,      0x14, 32
	cop d_index_wbinv, 0x01, 16
	cop d_hit_inv,   0x11, 16
	cop d_hit_wb,    0x19, 16

# A 2 KiB block of NOPs to put lines into the I-cache: void hwp_nop_sled(void)
	.balign 32
	.global hwp_nop_sled
hwp_nop_sled:
	nops  510
	jr    $31
	nop

# ---------------------------------------------------------------------------------------------
# TLB shutdown (S_TLB_SHUTDOWN). void hwp_tlb_shutdown(const uint64_t e[4], uint32_t out[8])
# Writes e at index 26 and 27, loads from its virtual page with HWP_EXPECT_RESUME.
# out: Status before, after 1st TLBWI, after 2nd, after the load, value loaded, Index after TLBP.
	.global hwp_tlb_shutdown
hwp_tlb_shutdown:
	dmfc0 $8, $10
	mfc0  $9, $12
	sw    $9, 0($5)
	ld    $9, 0($4)
	mtc0  $9, $5
	ld    $9, 8($4)
	dmtc0 $9, $10
	ld    $9, 16($4)
	dmtc0 $9, $2
	ld    $9, 24($4)
	dmtc0 $9, $3
	li    $9, 26
	mtc0  $9, $0
	nops  2
	tlbwi
	nops  4
	mfc0  $9, $12
	sw    $9, 4($5)
	li    $9, 27
	mtc0  $9, $0
	nops  2
	tlbwi
	nops  4
	mfc0  $9, $12
	sw    $9, 8($5)
	ld    $10, 8($4)
	dsrl  $10, $10, 13         # VPN2 << 13 is the even page's virtual address
	dsll  $10, $10, 13
	li    $11, 0x5A5A5A5A
	la    $27, 1f
	lw    $11, 0($10)
1:	mfc0  $9, $12
	sw    $9, 12($5)
	sw    $11, 16($5)
	tlbp
	nops  4
	mfc0  $9, $0
	sw    $9, 20($5)
	dmtc0 $8, $10
	nops  4
	jr    $31
	nop

# ---------------------------------------------------------------------------------------------
# RDRAM patterns (S_RDRAM). uint32_t hwp_rd_<kind>(uint32_t base, uint32_t stride, uint32_t mask)
# 256 accesses at base | ((i * stride) & mask). The same loop body for every kind and stride.
	.macro rdloop name, access:vararg
	.global hwp_rd_\name
	.balign 32
hwp_rd_\name:
	li    $10, 256
	move  $11, $0
	count $8
1:	and   $12, $11, $6
	or    $12, $12, $4
	\access
	addiu $10, $10, -1
	bnez  $10, 1b
	addu  $11, $11, $5
	sync
	lui   $12, 0xA000          # drain the write buffer behind an uncached load
	lw    $12, 0($12)
	ret_delta
	.endm

	rdloop load, lw $13, 0($12)
	rdloop store, sw $10, 0($12)

# ---------------------------------------------------------------------------------------------
# Stage 1 end: void hwp_reset_wait(void). Arms the pre-NMI interrupt (IM4, HWP_EXPECT_PRENMI set
# by the caller) and stores Count and an iteration count to low RAM until the NMI.
	.global hwp_reset_wait
	.global hwp_reset_wait_end
	.balign 32
hwp_reset_wait:
	mfc0  $8, $12
	ori   $8, $8, 0x1001
	mtc0  $8, $12
	lui   $9, 0xA000
	move  $10, $0
1:	count $8
	sw    $8, 0x388($9)
	addiu $10, $10, 1
	b     1b
	sw    $10, 0x390($9)
hwp_reset_wait_end:
