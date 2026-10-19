// hwprobe series ids: the single list of what the record holds.
//
// tools/hwprobe.py parses this file. Every id is `#define S_<NAME> 0x<id> // <esize> <layout>`,
// where <esize> is the element size in bytes and <layout> describes the elements in order.
// Format: docs/timing/hwprobe.md. Ranged ids (0x3MRO, 0x40MO, ...) are composed by the macros
// below and listed by their base.
#pragma once

// ---- Boot state, captured by hwp_entry before libdragon's _start. Stage 2 adds 0x0100. ----
#define S_BOOT_GPR        0x0100 // 8 r0..r31 as IPL3 left them (k0 = 0: used by the capture), then HI, LO
#define S_BOOT_COP0       0x0101 // 8 DMFC0 of COP0 registers 0..31
#define S_BOOT_FCR        0x0102 // 4 FCR0, FCR31 (CU1 set only for the read)
#define S_BOOT_TLB        0x0103 // 8 entries 0..31: PageMask, EntryHi, EntryLo0, EntryLo1
#define S_BOOT_COUNT      0x0104 // 4 Count at hwp_entry's first instruction, Count after the TLB dump
#define S_BOOT_RCP        0x0105 // 4 MI 0..3, VI 0..13, AI 0..5, PI 0..12, RI 0..7, SI DRAM_ADDR, SI STATUS, SP STATUS, SP DMA_FULL, SP DMA_BUSY, SP PC
#define S_BOOT_RDRAM      0x0106 // 4 per RDRAM device id 0, 2, 4 ... (to memsize): registers 0..9 (DeviceType, DeviceId, Delay, Mode, RefInterval, RefRow, RasInterval, MinInterval, AddressSelect, DeviceManufacturer)
#define S_BOOT_FLAGS      0x0107 // 4 memsize, tvtype, resettype, consoletype, stage, 0xA0000380..0xA00003BC (16 words)
#define S_BOOT_COLD       0x0108 // 4 only from hwprobe-cold.z64: the CPU/RCP state at IPL3's first instruction (ipl3/cold-capture.patch): "HWC0", r1..r31 low words (k0 = 0), HI, LO, COP0 0..31 low words, upper words of BadVAddr EPC ErrorEPC XContext Context EntryHi, FCR31, FCR0, RI 0..7, MI 0..3, SP_STATUS, PI_BSD_DOM1 LAT PWD PGS RLS, PI_STATUS, VI_CTRL, VI_V_CURRENT, SI_STATUS, Count at the end of the capture
#define S_BOOT2_GPR       0x0200 // 8 as S_BOOT_GPR, after the soft reset
#define S_BOOT2_COP0      0x0201 // 8 as S_BOOT_COP0
#define S_BOOT2_FCR       0x0202 // 4 as S_BOOT_FCR
#define S_BOOT2_TLB       0x0203 // 8 as S_BOOT_TLB; stage 1 wrote the pattern in S_NMI_TLB_PATTERN before waiting
#define S_BOOT2_COUNT     0x0204 // 4 as S_BOOT_COUNT
#define S_BOOT2_RCP       0x0205 // 4 as S_BOOT_RCP
#define S_BOOT2_RDRAM     0x0206 // 4 as S_BOOT_RDRAM
#define S_BOOT2_FLAGS     0x0207 // 4 as S_BOOT_FLAGS
#define S_BOOT2_COLD      0x0208 // 4 as S_BOOT_COLD: the state IPL3 received after the soft reset
#define S_NMI_TLB_PATTERN 0x0210 // 8 the 32 x 4 TLB words stage 1 wrote before waiting for RESET
#define S_NMI_WAIT_LOOP   0x0211 // 4 virtual address of the wait loop's first instruction, its length in bytes
#define S_NMI_TLB_READBACK 0x0212 // 8 TLBR of all 32 entries right after writing S_NMI_TLB_PATTERN (stage 1, after the TLB shutdown group)

// ---- Uncached-read loops: krom TIMINGNTSC with Count captured each iteration ----
// 0x3MRO, M = VI mode (HWP_VI_*), R = register read (HWP_REG_*), O = op (HWP_OP_*).
// 1 byte per iteration: Count bits 7:0 after the iteration's SYNC. Delta mod 256 = loop time.
// Stored nibble-packed (series flag HWP_PACK_DELTAS, docs/timing/hwprobe.md).
#define S_LOOP_RAW        0x3000 // 1 Count low byte per iteration (4096; in FlashRAM 256 for R != 0), packed deltas
#define S_LOOP_RAW_ID(m, r, o) (0x3000 | ((m) << 8) | ((r) << 4) | (o))
// 0x40MO: iterations of krom's exact loop body per VI_V_CURRENT value, from the change to 0
// through HWP_LINE_SAMPLES changes. M = 1 or 2.
#define S_LOOP_LINE       0x4000 // 1 iterations while VI_V_CURRENT held one value (mod 256), packed values
#define S_LOOP_LINE_ID(m, o) (0x4000 | ((m) << 4) | (o))
#define S_LOOP_VI_REGS    0x40F0 // 4 VI registers 0..13 as programmed for mode M, for M = 0, 1, 2
#define S_LOOP_RAW_TOTAL  0x40F1 // 4 per raw run in id order: Count before the first iteration, Count after the last (32-bit)

// ---- Exceptions ----
#define S_EXC_REDIRECT    0x5000 // 4 64 Count deltas each: SYSCALL, baseline; ADD overflow, baseline; fetch AdEL, baseline; BREAK, baseline
#define S_EXC_REDIRECT_INFO 0x5001 // 4 per kind (SYSCALL, ADD ov, AdEL, BREAK): Cause, EPC - probe base, BadVAddr low word, Status from the first handler entry
#define S_INT_ACCEPT      0x5010 // 4 per sled (NOP, uncached LW+NOP, DIV) per k = 2..65: EPC - sled base, Count at handler entry - Count at arm
#define S_INT_ACCEPT_CAUSE 0x5011 // 4 per sled: Cause at the k = 2 entry

// ---- Micro-TLB ----
#define S_ITLB            0x5100 // 4 32 Count deltas per action: none, TLBWI other, TLBWI same, EntryHi same ASID, EntryHi other ASID and back, TLBP, TLBR, 3-page block (none)
#define S_ITLB_SETUP      0x5101 // 4 virtual base, physical base of the block, block length, TLB index used

// ---- CP0 hazards ----
#define S_CP0_HAZARD      0x5200 // 4 per gap 0..3: 16 x (MTC0 EntryLo0, gap, MFC0 EntryLo0) Count delta x 16 reps, then the 16 values read in the last rep
#define S_CP0_HAZARD_WATCH 0x5201 // 4 per gap 0..3: Count delta x 16 reps, 16 values (WatchLo)
#define S_CP0_HAZARD_COUNT 0x5202 // 4 per gap 0..3: value written to Count, value MFC0 returned (16 pairs)

// ---- Pipeline overlap ----
#define S_OVERLAP         0x5300 // 4 64 Count deltas per case: LW miss+MULT indep, LW miss+MULT dep, LW hit+MULT indep, LW miss+DIV indep, LW miss+DIV dep, LW hit+DIV indep
#define S_OVERLAP_ILINE   0x5301 // 4 64 Count deltas per case: MULT at line end next cold, MULT next warm, NOP next cold, NOP next warm
#define S_OVERLAP_BRANCH  0x5302 // 4 64 Count deltas per case: 16 BEQL not taken, 16 NOP, 16 BEQ taken (to next+4), 16 BNEL taken
#define S_OVERLAP_ERET    0x5303 // 4 64 Count deltas per case: ERET to next (EXL set), J to next + NOP

// ---- CPU quirks ----
#define S_CAUSE_CE        0x5400 // 4 Cause for exception (SYSCALL, ADD ov, CTC1 FPE) x follower (NOP, MFC1, MFC2, LWC1, MTC0)
#define S_JUMP_REGION     0x5410 // 4 per case (JR 8->A with J, JR A->8 with J, JR 8->A with JAL, JR A->8 with JAL): ra of the target's BAL, ra of the JAL (or 0)
#define S_CVT_L           0x5420 // 8 per input: input, CVT.S.L result, FCSR after, exception code + 1 (0 = none), CVT.D.L result, FCSR after, exception code + 1
#define S_CACHE_OPS       0x5430 // 4 32 Count deltas per op: 64 NOP, I Index_Invalidate, I Hit_Invalidate (hit), I Hit_Invalidate (miss), I Fill, D Index_WB_Inv (clean), D Index_WB_Inv (dirty), D Hit_Invalidate, D Hit_WB (clean), D Hit_WB (dirty)
#define S_TLB_SHUTDOWN    0x5440 // 4 V flag used, Status before, after 1st TLBWI, after 2nd TLBWI, after load, Cause, BadVAddr, EPC - probe base, value loaded, TLBP Index after

// ---- RDRAM access patterns ----
#define S_RDRAM           0x6000 // 4 per kind (uncached LW, cached fill, fill evicting dirty, uncached SW) per VI (off, 240p) per stride: 8 Count deltas for 256 accesses
#define S_RDRAM_STRIDES   0x6001 // 4 the stride list in bytes

// ---- PI ----
#define S_PI_DMA          0x7000 // 4 per timing set per size: 8 Count deltas from WR_LEN write to STATUS idle
#define S_PI_DMA_CONFIG   0x7001 // 4 per timing set: LAT, PWD, PGS, RLS; then the size list
#define S_PI_BUSY         0x7002 // 4 per case (write to DRAM_ADDR, CART_ADDR, WR_LEN, STATUS = 2 while busy): STATUS right after the write, STATUS when idle, DRAM_ADDR, CART_ADDR, first word at the original and at the written DRAM address
#define S_PI_IO_DURING_DMA 0x7003 // 4 value of an IO read of ROM 0x10000000 during a DMA, Count delta of that read, STATUS after
// ---- SI / AI ----
#define S_SI_DMA          0x7100 // 4 32 Count deltas each: SI write 64 B, SI read 64 B (controller status block)
#define S_SI_RESULT       0x7101 // 1 the 64-byte PIF RAM after the last status command
#define S_AI_SAMPLES      0x7200 // 4 256 x (Count - Count at start, AI_LEN, AI_STATUS)
#define S_AI_CONFIG       0x7201 // 4 DACRATE, BITRATE, buffer length, sample spacing in Count ticks

// ---- Instruction retirement, refill, register latency (probes2.c) ----
#define S_RETIRE          0x5600 // 4 8 Count deltas per class for 32 copies: nop addu sll dsll32 lui slt daddu mult multu dmult dmultu div divu ddiv ddivu mult+mflo lw ld lb lwl+lwr sw sd sb lw+use beq-taken bne-not-taken beql-not-taken j jal la+jr mfc0 mtc0 mfhi add.s add.d mul.s mul.d div.s div.d sqrt.s sqrt.d cvt.s.w cvt.w.s cvt.d.s mov.s neg.s c.eq.s+bc1t add.s-dep mul.d-dep lwc1 swc1 ldc1 mfc1 mtc1 cfc1 ctc1 lw-uncached sw-uncached sync
#define S_REFILL          0x5700 // 4 16 each: I 32 misses, I hits, D 32 misses, D hits, TLB refill load, TLB hit load, TLB invalid load (minimal handlers); then Cause for refill, hit, invalid
#define S_REG_READ        0x5800 // 4 8 Count deltas of 64 uncached loads per address in S_REG_READ_ADDR
#define S_REG_READ_ADDR   0x5801 // 4 the addresses
#define S_REG_WRITE       0x5802 // 4 8 Count deltas of 64 stores + drain per address in S_REG_WRITE_ADDR
#define S_REG_WRITE_ADDR  0x5803 // 4 the addresses
#define S_RDRAM_CONTEND   0x6100 // 4 per kind (idle, RSP DMA streaming, RDP filling) per stride: 8 Count deltas of 256 uncached loads
#define S_RDRAM_CONTEND_STRIDES 0x6101 // 4 the strides
#define S_VI_HSYNC        0x6200 // 4 per H_SYNC value per stride (0, 2048): 8 Count deltas of 256 uncached loads, VI 240p
#define S_VI_HSYNC_VALUES 0x6201 // 4 the H_SYNC values
// ---- CPU functional sweeps (cpu_func.c), full record only ----
#define S_OPCODE_SWEEP    0xA000 // 4 per encoding (primary, SPECIAL, REGIMM, COP0, COP1 fmt x funct, COP2/3; all register fields 0): word, Cause, EPC - buffer, BadVAddr (~0 = no exception)
#define S_FPU_BINARY_S    0xA010 // 8 add/sub/mul/div.s x 8 modes (FCSR RM 0..3, then FS set) x 16 x 16 operands: result, (FCSR at the exception << 32) | FCSR after, Cause (~0 = none)
#define S_FPU_BINARY_D    0xA011 // 8 as S_FPU_BINARY_S for .d
#define S_FPU_UNARY       0xA012 // 8 from S then D: sqrt abs mov neg round.l trunc.l ceil.l floor.l round.w trunc.w ceil.w floor.w cvt.s cvt.d cvt.w cvt.l; from W then L: cvt.s cvt.d; x 8 modes x 16 operands: as S_FPU_BINARY_S
#define S_FPU_COMPARE     0xA013 // 8 c.cond (cond 0..15) .s then .d x 16 x 16 operands: $f6 (unchanged), FCSR after (bit 23 = condition), Cause
#define S_FPU_OPERANDS_S  0xA014 // 4 the 16 single operands
#define S_FPU_OPERANDS_D  0xA015 // 8 the 16 double operands
#define S_INT_OPERANDS    0xA016 // 8 the 16 integer operands
#define S_INT_OPS         0xA020 // 8 SPECIAL sllv srlv srav dsllv dsrlv dsrav mult multu div divu dmult dmultu ddiv ddivu add addu sub subu and or xor nor slt sltu dadd daddu dsub dsubu x 16 rs x 16 rt: rd (0xEEEEEEEE_<Cause> if it trapped), HI, LO
#define S_INT_SHIFTS      0xA021 // 8 sll srl sra dsll dsrl dsra dsll32 dsrl32 dsra32 x sa 0..31 x 16 operands: as S_INT_OPS
#define S_UNALIGNED       0xA030 // 8 LWL LWR LDL LDR SWL SWR SDL SDR x offset 0..7: register after, then the 16 bytes from the buffer (bytes 0xA0.. before; register 0x1122334455667788)
#define S_LLSC            0xA031 // 8 per case (LL+SC, SC after ERET, LL+store+SC, LLD+SCD, LL+ERET+SC): SC result, word after, LLAddr
#define S_CACHE_TAGS      0xA032 // 4 TagLo after Index_Load_Tag: D line loaded, D dirtied, D invalidated, I line run, I line invalidated; physical address of the D line

// ---- RSP and RDP ----
#define S_SP_DMA          0x7300 // 4 per direction (to SP, from SP) per length in S_SP_DMA_CONFIG, then IMEM 4 KiB: 8 Count deltas
#define S_SP_DMA_CONFIG   0x7301 // 4 length register values
#define S_RSP_CASE        0x7400 // 4 per case (rsp_probe.S CASE_TABLE order, 0..16) x 4 reps: Count ticks (256 iterations), DP_CLOCK delta seen by the RSP, SP_STATUS
#define S_RSP_DMA         0x7401 // 4 per direction (in, out) per length (8, 128, 1024, 3072) x 4 reps: as S_RSP_CASE, 32 transfers per run
#define S_RSP_INFO        0x7402 // 4 microcode loaded, iterations, case count, IMEM bytes
#define S_RDP             0x7500 // 4 per configuration (probes2.c rdp_build) x 4 reps: Count ticks to the full-sync interrupt, DP_CLOCK, DP_BUFBUSY, DP_PIPEBUSY, DP_TMEM, DP_STATUS
#define S_RDP_INFO        0x7501 // 4 command words per configuration

// ---- RDP render matrix (rdp_render.c): full record only, never FlashRAM ----
// n = case number in rdp_render.c build(), k = texture number (rdp_render.c TX_* order). Images
// are 32 x 32, row-major, as RDRAM holds them. The colour series' element size is the case's
// colour depth in bytes (1, 2 or 4; S_RDPR_INFO).
#define S_RDPR_COLOR      0x8000 // 2 colour buffer of case n after its full sync (0x8000 | n)
#define S_RDPR_COLOR_ID(n) (0x8000 | (n))
#define S_RDPR_Z          0x8400 // 2 Z buffer of case n, for the cases that read or write Z (0x8400 | n)
#define S_RDPR_Z_ID(n)    (0x8400 | (n))
#define S_RDPR_CMD        0x8800 // 8 command words of case n as sent from RDRAM, closing sync pipe and sync full included (0x8800 | n)
#define S_RDPR_CMD_ID(n)  (0x8800 | (n))
#define S_RDPR_TEX        0x8C00 // 1 texture image k in RDRAM (0x8C00 | k): element size 1 for 4- and 8-bit, 2 for 16-bit, 4 for 32-bit
#define S_RDPR_TEX_ID(k)  (0x8C00 | (k))
#define S_RDPR_INFO       0x8F00 // 4 per case: n, colour bpp, width, height, flags (1 = Z recorded, 2 = command list truncated), Count ticks to the full-sync interrupt (HWP_TIMEOUT), DPC_STATUS, DPC_CLOCK
#define S_RDPR_INIT16     0x8F01 // 2 colour buffer the CPU writes before each 16 bpp case
#define S_RDPR_INIT32     0x8F02 // 4 as S_RDPR_INIT16, 32 bpp
#define S_RDPR_INIT8      0x8F03 // 1 as S_RDPR_INIT16, 8 bpp
#define S_RDPR_INITZ      0x8F04 // 2 Z buffer the CPU writes before every case
#define S_RDPR_TEXINFO    0x8F05 // 4 per texture k: format, size, width, height, RDRAM physical address
// ---- RSP functional sweep (rsp_func.c, rsp_func_ucode.S): full record only. HWP_TIMEOUT bytes
// (all ones) where the RSP did not reach the BREAK. Ranged ids are listed by their base. ----
#define S_RSPF_INFO       0x9000 // 4 microcode loaded, RSP runs, runs that failed, 1 if the RSP stopped answering (later runs skipped), IMEM bytes, DMEM bytes, vector-op cases, element-sweep cases, runs that reached the BREAK. When none did and the RSP stopped answering, every other 0x9xxx series except S_RSPF_VCASES and S_RSPF_SCALAR_ADDR has 0 elements
#define S_RSPF_VCASES     0x9001 // 2 per vector-op case 0..255: vs[8], vt[8], vd before[8], preload H'[8], M[8], L[8], VCO, VCC, VCE, 0 x5
#define S_RSPF_COP0       0x9002 // 4 RSP MFC0 $0..$15, $7 again, $7 after MTC0 $7 = 0, per signal 0..7: SP_STATUS after the RSP sets it, after it clears it; then CPU: SP_STATUS, SP_SEMAPHORE read twice
#define S_RSPF_MOVE       0x9003 // 4 MFC2 t5 of $v08 per element 0..15; $v08 (4 words) after MTC2 0x13579BDF per element 0..15; VCO, VCC, VCE after CTC2 0xFFFFFFFF to $0..$3; CFC2 of $0..$3 with VCO 0x1234, VCC 0x5678, VCE 0x9A
#define S_RSPF_SCALAR     0x9004 // 4 per op (lb lh lw lbu lhu lwu sb sh sw) per address in S_RSPF_SCALAR_ADDR: t5, DMEM 0x230..0x23F, 0xFF0..0xFFF, 0x000..0x00F
#define S_RSPF_SCALAR_ADDR 0x9005 // 4 the base register values (offset 0)
#define S_RSPF_VOP        0x9100 // 2 per funct f in id 0x91ff: COP2 vd=3 vs=1 vt=2, per case 0..255 with e = 0, then per e = 1..15 per case 128..143: vd[8], ACC high[8], middle[8], low[8], VCO, VCC, VCE, 0
#define S_RSPF_VOP_ID(f)  (0x9100 | (f))
#define S_RSPF_RCP        0x9200 // 4 VRCP then VRCPH of $v00: (high << 16) | low per input 4096 c .. 4096 c + 4095, chunk c = 0..15 in id 0x920c
#define S_RSPF_RSQ        0x9210 // 4 as S_RSPF_RCP with VRSQ and VRSQH
#define S_RSPF_RCPD       0x9220 // 4 VRCPH, VRCPL, VRCPH of $v00 per input 4096 c .. (rsp_func.c div_double_input), chunk c = 0..3: input, (high << 16) | low
#define S_RSPF_RSQD       0x9230 // 4 as S_RSPF_RCPD with VRSQH and VRSQL
#define S_RSPF_RCPD_SPECIAL 0x9240 // 4 as S_RSPF_RCPD for the 32 inputs of rsp_func.c DIV_SPECIAL
#define S_RSPF_RSQD_SPECIAL 0x9241 // 4 as S_RSPF_RSQD for DIV_SPECIAL
#define S_RSPF_LS         0x9300 // 2 id 0x93SO: S = 2 store + set, O = LWC2/SWC2 opcode 0..11; set 0 base 0x230 + off, set 1 base 0xFF0 + off; per off 0..15 per element 0..15: loads $v08 ($v08..$v15 for O = 10, 11), stores the 64-byte window

// ---- Run log ----
#define S_GROUP_TIME      0x7F00 // 4 per group that ran: group id, Count ticks it took (wraps), unexpected exceptions
#define S_FAULT           0x7F01 // 4 per unexpected exception: group id, Cause, EPC, BadVAddr, Status
#define S_FLASH_TIMES     0x7F02 // 4 chip or sector erase Count ticks, then each page program's ticks in order (up to 512)

// A wait on the RSP, RDP or a DMA that did not finish.
#define HWP_TIMEOUT 0xFFFFFFFFu

// Indices used in composed ids.
#define HWP_VI_OFF   0
#define HWP_VI_240P  1
#define HWP_VI_480I  2
#define HWP_REG_VI_V_CURRENT 0
#define HWP_REG_MI_VERSION   1
#define HWP_REG_SP_STATUS    2
#define HWP_REG_PI_STATUS    3
#define HWP_OP_ALU   0
#define HWP_OP_MULS0 1
#define HWP_OP_ADDS  2
#define HWP_OP_MULT  3
#define HWP_OP_DMULT 4
#define HWP_OP_DIV   5
#define HWP_OP_DDIV  6
#define HWP_NUM_OPS  7

// Series header flags.
#define HWP_PACK_NONE   0
#define HWP_PACK_DELTAS 1 // nibble-packed deltas mod 256 of a byte sequence
#define HWP_PACK_VALUES 2 // nibble-packed byte values

#define HWP_RAW_SAMPLES_VI    4096
#define HWP_RAW_SAMPLES_OTHER 256
#define HWP_LINE_SAMPLES      640
