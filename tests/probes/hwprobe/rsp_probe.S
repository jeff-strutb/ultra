# hwprobe RSP timing microcode (docs/timing/hwprobe.md, "RSP"). Each case runs its body
# ITER times in a counted loop and BREAKs. The CPU sets SP_PC to the case's entry (from
# CASE_TABLE), clears HALT, and times the run with Count until HALT returns; the case also
# stores the DP_CLOCK delta it saw to RESULT. Case 0 is the empty loop, the baseline.
#include <rsp.inc>

	.set noreorder
	.set at

	.data
	.align 4
	.globl CASE_TABLE
CASE_TABLE:                 # IMEM addresses of the cases, in series.h S_RSP_CASE order
	.word c_empty, c_su_addu, c_su_load_use, c_su_store_after_load, c_su_branch
	.word c_vu_indep, c_vu_dep, c_dual, c_dual_dep, c_vu_lqv_use, c_vu_sqv_after_op
	.word c_vu_mfc2, c_vu_mac, c_vu_rcp, c_vu_ctc2, c_su_dmem, c_vu_ldv_ltv
	.word c_dma_in, c_dma_out, c_dma_loop
	.word 0
	.align 4
	.globl PARAMS
PARAMS:                     # written by the CPU: iterations, DMA RDRAM address, DMA length - 1
	.word 256, 0, 0x0BFF, 0
	.globl RESULT
RESULT:                     # DP_CLOCK at the end minus at the start
	.word 0, 0, 0, 0
	.align 4
VDATA:
	.half 1, 2, 3, 4, 5, 6, 7, 8
	.half 0x7FFF, 0x4000, 0x2000, 0x1000, 0x0800, 0x0400, 0x0200, 0x0100
	.align 4
SCRATCH:
	.ds.b 256
	.align 4
DMABUF:
	.ds.b 0xC00

	.text

	.macro begin
	lw    t0, %lo(PARAMS)
	li    s0, %lo(VDATA)
	lqv   $v01, 0, s0
	lqv   $v02, 0x10, s0
	li    t1, 12345
	li    t2, 3
	li    s1, %lo(SCRATCH)
	mfc0  t8, COP0_DP_CLOCK
	.endm

	.macro end
	mfc0  t9, COP0_DP_CLOCK
	sub   t9, t9, t8
	sw    t9, %lo(RESULT)
	break
	nop
	.endm

	.macro loop label
	addiu t0, t0, -1
	bnez  t0, \label
	nop
	.endm

c_empty:
	begin
1:	loop 1b
	end

c_su_addu:                  # 8 independent scalar ALU ops
	begin
1:	.rept 8
	addu  t3, t1, t2
	.endr
	loop 1b
	end

c_su_load_use:              # load then immediate use, 8 times
	begin
1:	.rept 8
	lw    t3, 0(s1)
	addu  t4, t3, t1
	.endr
	loop 1b
	end

c_su_store_after_load:      # load then store, 8 times
	begin
1:	.rept 8
	lw    t3, 0(s1)
	sw    t1, 4(s1)
	.endr
	loop 1b
	end

c_su_branch:                # 8 taken branches to the next pair
	begin
1:	.rept 8
	beq   zero, zero, 2f
	nop
2:
	.endr
	loop 1b
	end

c_vu_indep:                 # 8 independent vector adds
	begin
1:	vadd  $v03, $v01, $v02
	vadd  $v04, $v01, $v02
	vadd  $v05, $v01, $v02
	vadd  $v06, $v01, $v02
	vadd  $v07, $v01, $v02
	vadd  $v08, $v01, $v02
	vadd  $v09, $v01, $v02
	vadd  $v10, $v01, $v02
	loop 1b
	end

c_vu_dep:                   # a chain of 8 dependent vector adds
	begin
1:	vadd  $v03, $v01, $v02
	vadd  $v03, $v03, $v02
	vadd  $v03, $v03, $v02
	vadd  $v03, $v03, $v02
	vadd  $v03, $v03, $v02
	vadd  $v03, $v03, $v02
	vadd  $v03, $v03, $v02
	vadd  $v03, $v03, $v02
	loop 1b
	end

c_dual:                     # 8 SU/VU pairs, independent
	begin
1:	.rept 8
	addu  t3, t1, t2
	vadd  $v03, $v01, $v02
	.endr
	loop 1b
	end

c_dual_dep:                 # 8 VU/SU pairs where the SU op stores the VU result
	begin
1:	.rept 8
	vadd  $v03, $v01, $v02
	sqv   $v03, 0, s1
	.endr
	loop 1b
	end

c_vu_lqv_use:               # vector load then use
	begin
1:	.rept 8
	lqv   $v03, 0, s0
	vadd  $v04, $v03, $v02
	.endr
	loop 1b
	end

c_vu_sqv_after_op:          # vector op then store of an unrelated register
	begin
1:	.rept 8
	vadd  $v03, $v01, $v02
	sqv   $v05, 0x10, s1
	.endr
	loop 1b
	end

c_vu_mfc2:                  # vector op then MFC2 of its result
	begin
1:	.rept 8
	vadd  $v03, $v01, $v02
	mfc2  t3, $v03.e0
	.endr
	loop 1b
	end

c_vu_mac:                   # multiply-accumulate chain
	begin
1:	vmudh $v03, $v01, $v02
	vmadh $v03, $v01, $v02
	vmadh $v03, $v01, $v02
	vmadh $v03, $v01, $v02
	vmudn $v04, $v01, $v02
	vmadn $v04, $v01, $v02
	vmadn $v04, $v01, $v02
	vmadn $v04, $v01, $v02
	loop 1b
	end

c_vu_rcp:                   # reciprocal and square root pairs
	begin
1:	vrcph $v03.e0, $v01.e0
	vrcpl $v03.e0, $v01.e0
	vrcph $v03.e0, $v00.e0
	vrsqh $v04.e0, $v01.e0
	vrsql $v04.e0, $v01.e0
	vrsqh $v04.e0, $v00.e0
	vrcp  $v05.e0, $v01.e1
	vrsq  $v06.e0, $v01.e2
	loop 1b
	end

c_vu_ctc2:                  # flag moves around a compare
	begin
1:	.rept 4
	vlt   $v03, $v01, $v02
	cfc2  t3, COP2_CTRL_VCC
	ctc2  zero, COP2_CTRL_VCC
	.endr
	loop 1b
	end

c_su_dmem:                  # 8 scalar stores and loads to DMEM
	begin
1:	.rept 4
	sw    t1, 0(s1)
	sh    t1, 6(s1)
	lw    t3, 8(s1)
	lbu   t4, 13(s1)
	.endr
	loop 1b
	end

c_vu_ldv_ltv:               # other vector load/store forms
	begin
1:	ldv   $v03, 0, 0, s0
	lpv   $v04, 0, 0, s0
	luv   $v05, 0, 0, s0
	lrv   $v06, 0, 0x10, s0
	sdv   $v03, 0, 0, s1
	spv   $v04, 0, 0, s1
	suv   $v05, 0, 0, s1
	sqv   $v06, 0, 0x20, s1
	loop 1b
	end

# DMA from RSP COP0: PARAMS[1] = RDRAM address, PARAMS[2] = length - 1 (with count/skip in the
# upper bits as SP_RD_LEN). Each iteration issues one transfer and waits for it.
c_dma_in:
	begin
	lw    t3, %lo(PARAMS) + 4
	lw    t4, %lo(PARAMS) + 8
	li    t5, %lo(DMABUF)
1:	mtc0  t5, COP0_DMA_SPADDR
	mtc0  t3, COP0_DMA_RAMADDR
	mtc0  t4, COP0_DMA_READ
2:	mfc0  t6, COP0_DMA_BUSY
	bnez  t6, 2b
	nop
	loop 1b
	end

c_dma_out:
	begin
	lw    t3, %lo(PARAMS) + 4
	lw    t4, %lo(PARAMS) + 8
	li    t5, %lo(DMABUF)
1:	mtc0  t5, COP0_DMA_SPADDR
	mtc0  t3, COP0_DMA_RAMADDR
	mtc0  t4, COP0_DMA_WRITE
2:	mfc0  t6, COP0_DMA_BUSY
	bnez  t6, 2b
	nop
	loop 1b
	end

# Background RDRAM traffic for S_RDRAM_CONTEND: 3 KiB RDRAM -> DMEM transfers back to back
# until the CPU sets HALT.
c_dma_loop:
	lw    t3, %lo(PARAMS) + 4
	li    t4, 0x0BFF
	li    t5, %lo(DMABUF)
1:	mtc0  t5, COP0_DMA_SPADDR
	mtc0  t3, COP0_DMA_RAMADDR
	mtc0  t4, COP0_DMA_READ
2:	mfc0  t6, COP0_DMA_BUSY
	bnez  t6, 2b
	nop
	b     1b
	nop
