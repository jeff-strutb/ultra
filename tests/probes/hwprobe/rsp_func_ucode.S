# hwprobe RSP functional-sweep microcode (docs/timing/hwprobe.md, "RSP functional sweep").
# Each entry runs a few fixed instructions around a 4-word slot in IMEM that the CPU patches
# with the instruction under test, then BREAKs. The CPU writes the inputs into DMEM, sets SP_PC
# to the entry from FUNC_TABLE, clears HALT, waits for HALT, and reads the outputs from DMEM.
# Nothing here selects behaviour: every encoding under test comes from rsp_func.c.
#include <rsp.inc>

	.set noreorder
	.set at

	.data
# DMEM 0x000-0x03F and 0xFC0-0xFFF are kept free for the wraparound tests, which store across
# the end of DMEM.
WRAP_LO:
	.space 0x40
	.org 0x40
	.globl FUNC_TABLE
FUNC_TABLE:                 # 0x040: entries, then the IMEM address of each entry's slot
	.word e_vop, e_div, e_single, e_cop0
	.word s_vop, s_div, s_single, 0
	.org 0x60
FUNC_PARAMS:                # 0x060: count, address (s4), value (t5), VCO, VCC, VCE, magic, 0
	.word 0, 0, 0, 0, 0, 0, 0x52535046, 0
	.org 0x80
CONSTS:                     # 0x080: accumulator preload multipliers
	.half 0x4000, 0x4000, 0x4000, 0x4000, 0x4000, 0x4000, 0x4000, 0x4000
	.half 1, 1, 1, 1, 1, 1, 1, 1
	.org 0xA0
VINIT:                      # 0x0A0: $v08-$v15 before e_single's slot
	.space 0x80
	.org 0x120
SOUT:                       # 0x120: e_single and e_cop0 outputs
	.space 0x100
	.org 0x220
REGION:                     # 0x220: e_single's load/store window
	.space 0x40
	.org 0x260
BUF:                        # 0x260: e_vop cases and outputs, e_div items
	.space 0xFC0 - 0x260
	.org 0xFC0
WRAP_HI:
	.space 0x40

	.text

# Vector compute: FUNC_PARAMS[0] cases of 112 bytes at BUF (vs, vt, vd before, accumulator
# preload H', M, L, then VCO VCC VCE halfwords), 80-byte outputs right after them (vd, ACC
# high, middle, low, then VCO VCC VCE halfwords). The accumulator is preloaded with
# (H' << 32) + (sext(M) << 16) + L: VMUDH and three VMADH of H' x 0x4000, VMADH of M x 1,
# VMADN of L x 1. rsp_func.c computes H' so that the middle slice comes out as M.
	.globl e_vop
e_vop:
	vxor  $v00, $v00, $v00
	li    s0, %lo(BUF)
	lw    t0, %lo(FUNC_PARAMS)
	sll   t1, t0, 7
	sll   t2, t0, 4
	subu  t1, t1, t2            # count * 112
	addu  s1, s0, t1
	li    t3, %lo(CONSTS)
	lqv   $v30, 0x00, t3
	lqv   $v31, 0x10, t3
1:	vrcp  $v07.e0, $v00.e0      # the same divide state before every case
	lqv   $v20, 0x30, s0
	lqv   $v21, 0x40, s0
	lqv   $v22, 0x50, s0
	vmudh $v23, $v20, $v30
	vmadh $v23, $v20, $v30
	vmadh $v23, $v20, $v30
	vmadh $v23, $v20, $v30
	vmadh $v23, $v21, $v31
	vmadn $v23, $v22, $v31
	lhu   t1, 0x60(s0)
	ctc2  t1, COP2_CTRL_VCO
	lhu   t1, 0x62(s0)
	ctc2  t1, COP2_CTRL_VCC
	lhu   t1, 0x64(s0)
	ctc2  t1, COP2_CTRL_VCE
	lqv   $v01, 0x00, s0
	lqv   $v02, 0x10, s0
	lqv   $v03, 0x20, s0
s_vop:
	vnop
	vnop
	vnop
	vnop
	sqv   $v03, 0x00, s1
	vsar  $v04, COP2_ACC_HI
	vsar  $v05, COP2_ACC_MD
	vsar  $v06, COP2_ACC_LO
	sqv   $v04, 0x10, s1
	sqv   $v05, 0x20, s1
	sqv   $v06, 0x30, s1
	cfc2  t1, COP2_CTRL_VCO
	sh    t1, 0x40(s1)
	cfc2  t1, COP2_CTRL_VCC
	sh    t1, 0x42(s1)
	cfc2  t1, COP2_CTRL_VCE
	sh    t1, 0x44(s1)
	sh    zero, 0x46(s1)
	addiu s0, s0, 112
	addiu t0, t0, -1
	bnez  t0, 1b
	addiu s1, s1, 80
	break
	nop

# Divide unit: FUNC_PARAMS[0] items of two halfwords at BUF, loaded into $v01.e0 and $v02.e0;
# after the slot, $v04.e0 and $v03.e0 replace them in place.
	.globl e_div
e_div:
	vxor  $v00, $v00, $v00
	li    s0, %lo(BUF)
	lw    t0, %lo(FUNC_PARAMS)
1:	lsv   $v01.e0, 0, s0
	lsv   $v02.e0, 2, s0
s_div:
	vnop
	vnop
	vnop
	vnop
	ssv   $v04.e0, 0, s0
	ssv   $v03.e0, 2, s0
	addiu t0, t0, -1
	bnez  t0, 1b
	addiu s0, s0, 4
	break
	nop

# One instruction: $v08-$v15 from VINIT, s4 = FUNC_PARAMS[1], t5 = FUNC_PARAMS[2], flags from
# FUNC_PARAMS[3..5]. Afterwards SOUT holds t5, VCO VCC VCE (halfwords at +4), then $v08-$v15
# at +0x10.
	.globl e_single
e_single:
	li    t3, %lo(VINIT)
	lqv   $v08, 0x00, t3
	lqv   $v09, 0x10, t3
	lqv   $v10, 0x20, t3
	lqv   $v11, 0x30, t3
	lqv   $v12, 0x40, t3
	lqv   $v13, 0x50, t3
	lqv   $v14, 0x60, t3
	lqv   $v15, 0x70, t3
	lw    t1, %lo(FUNC_PARAMS) + 12
	ctc2  t1, COP2_CTRL_VCO
	lw    t1, %lo(FUNC_PARAMS) + 16
	ctc2  t1, COP2_CTRL_VCC
	lw    t1, %lo(FUNC_PARAMS) + 20
	ctc2  t1, COP2_CTRL_VCE
	lw    s4, %lo(FUNC_PARAMS) + 4
	lw    t5, %lo(FUNC_PARAMS) + 8
	nop
s_single:
	nop
	nop
	nop
	nop
	nop
	li    t3, %lo(SOUT)
	sw    t5, 0(t3)
	cfc2  t1, COP2_CTRL_VCO
	sh    t1, 4(t3)
	cfc2  t1, COP2_CTRL_VCC
	sh    t1, 6(t3)
	cfc2  t1, COP2_CTRL_VCE
	sh    t1, 8(t3)
	sqv   $v08, 0x10, t3
	sqv   $v09, 0x20, t3
	sqv   $v10, 0x30, t3
	sqv   $v11, 0x40, t3
	sqv   $v12, 0x50, t3
	sqv   $v13, 0x60, t3
	sqv   $v14, 0x70, t3
	sqv   $v15, 0x80, t3
	break
	nop

# COP0 as the RSP sees it. SOUT: MFC0 $0-$15 (the $7 read takes the semaphore), $7 again, $7
# after MTC0 $7 = 0, then per signal 0-7: SP_STATUS after setting it from the RSP and after
# clearing it. The semaphore is released before the BREAK.
#define SPW_CLR_SIG0 (1 << 9)   // n64brew "RSP/Interface", SP_STATUS write
#define SPW_SET_SIG0 (1 << 10)
	.globl e_cop0
e_cop0:
	li    s1, %lo(SOUT)
	.irp  r, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
	mfc0  t1, $\r
	sw    t1, (\r * 4)(s1)
	.endr
	mfc0  t1, $7
	sw    t1, 0x40(s1)
	mtc0  zero, $7
	mfc0  t1, $7
	sw    t1, 0x44(s1)
	mtc0  zero, $7
	li    t2, SPW_SET_SIG0
	li    t3, SPW_CLR_SIG0
	li    t4, 8
	addiu s2, s1, 0x48
1:	mtc0  t2, $4
	mfc0  t1, $4
	sw    t1, 0(s2)
	mtc0  t3, $4
	mfc0  t1, $4
	sw    t1, 4(s2)
	sll   t2, t2, 2
	sll   t3, t3, 2
	addiu t4, t4, -1
	bnez  t4, 1b
	addiu s2, s2, 8
	break
	nop
