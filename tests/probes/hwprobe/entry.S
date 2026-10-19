# hwprobe ELF entry point. IPL3 jumps here; we capture the CPU state it left before libdragon's
# _start changes Status, then continue to _start. Layout: hwp_boot_t in hwp.h.
#
# libdragon's IPL3 has already written Cause, Count, Compare and WatchLo to 0 and invalidated
# the caches (boot/ipl3.c stage1, loader.c stage3). k0 is the only register we lose.

	.set noreorder
	.set noat
	.set mips3

	.section .data
	.balign 8
	.global hwp_boot
hwp_boot:
	.space 1568

	.text
	.global hwp_entry
	.type hwp_entry, @function
hwp_entry:
	lui   $26, %hi(hwp_boot)
	addiu $26, $26, %lo(hwp_boot)
	sd    $1, 8($26)
	mfc0  $1, $9               # Count, as early as possible
	sw    $1, 1560($26)
	sd    $0, 0($26)
	sd    $2, 16($26)
	sd    $3, 24($26)
	sd    $4, 32($26)
	sd    $5, 40($26)
	sd    $6, 48($26)
	sd    $7, 56($26)
	sd    $8, 64($26)
	sd    $9, 72($26)
	sd    $10, 80($26)
	sd    $11, 88($26)
	sd    $12, 96($26)
	sd    $13, 104($26)
	sd    $14, 112($26)
	sd    $15, 120($26)
	sd    $16, 128($26)
	sd    $17, 136($26)
	sd    $18, 144($26)
	sd    $19, 152($26)
	sd    $20, 160($26)
	sd    $21, 168($26)
	sd    $22, 176($26)
	sd    $23, 184($26)
	sd    $24, 192($26)
	sd    $25, 200($26)
	sd    $0, 208($26)         # k0: used above
	sd    $27, 216($26)
	sd    $28, 224($26)
	sd    $29, 232($26)
	sd    $30, 240($26)
	sd    $31, 248($26)
	mfhi  $1
	sd    $1, 256($26)
	mflo  $1
	sd    $1, 264($26)

	.macro c0 n
	dmfc0 $1, $\n
	sd    $1, (272 + 8 * \n)($26)
	.endm
	c0 0
	c0 1
	c0 2
	c0 3
	c0 4
	c0 5
	c0 6
	c0 7
	c0 8
	c0 9
	c0 10
	c0 11
	c0 12
	c0 13
	c0 14
	c0 15
	c0 16
	c0 17
	c0 18
	c0 19
	c0 20
	c0 21
	c0 22
	c0 23
	c0 24
	c0 25
	c0 26
	c0 27
	c0 28
	c0 29
	c0 30
	c0 31

	# FCR0 and FCR31: CU1 only for the read.
	mfc0  $8, $12
	lui   $9, 0x2000
	or    $9, $8, $9
	mtc0  $9, $12
	nop
	nop
	nop
	nop
	cfc1  $10, $0
	sw    $10, 528($26)
	cfc1  $10, $31
	sw    $10, 532($26)
	mtc0  $8, $12
	nop
	nop
	nop
	nop

	# TLB: TLBR every entry, then restore Index, EntryHi, EntryLo0/1 and PageMask.
	addiu $11, $26, 536
	li    $12, 0
1:	mtc0  $12, $0
	nop
	nop
	tlbr
	nop
	nop
	nop
	nop
	dmfc0 $1, $5
	sd    $1, 0($11)
	dmfc0 $1, $10
	sd    $1, 8($11)
	dmfc0 $1, $2
	sd    $1, 16($11)
	dmfc0 $1, $3
	sd    $1, 24($11)
	addiu $12, $12, 1
	slti  $1, $12, 32
	bnez  $1, 1b
	addiu $11, $11, 32
	ld    $1, (272 + 8 * 0)($26)
	mtc0  $1, $0
	ld    $1, (272 + 8 * 10)($26)
	dmtc0 $1, $10
	ld    $1, (272 + 8 * 2)($26)
	dmtc0 $1, $2
	ld    $1, (272 + 8 * 3)($26)
	dmtc0 $1, $3
	ld    $1, (272 + 8 * 5)($26)
	mtc0  $1, $5
	nop
	nop
	mfc0  $1, $9
	sw    $1, 1564($26)

	la    $1, _start
	jr    $1
	nop
	.size hwp_entry, . - hwp_entry
