# VR4300 CPU

Facts the CPU model (`src/cpu/`) relies on, each with its source. Primary sources:

- **[UM]** NEC VR4300 User's Manual, U10504EJ7V0UM00 (`third_party/manuals`, not committed).
- **[n64brew]** n64brew wiki pages "VR4300", "Memory map", "SysAD Interface", "Initial Program
  Load" (mirrored in `third_party/n64brew`, not committed).
- **[st]** n64-systemtest (`tests/roms/n64-systemtest`), whose expectations were measured on
  hardware. Cited by source path.
- **[krom]** krom's CPUTest ROMs and their hardware-captured outputs (`tests/roms/krom`).

Where a source documents a quirk, the source is the authority; the model follows it rather
than the manual. Values without a primary source are in `docs/timing/ledger.md`.

## Clocks

| Fact | Value | Source |
|---|---|---|
| MasterClock (SClock) | 62.5 MHz, from the RCP | [n64brew] "Clock Timing", "SysAD Interface" |
| PClock | 93.75 MHz = 3/2 MasterClock (Config.EC = 111b, 1:1.5) | [UM] 5.4.6, [n64brew] "Clock Timing" |
| Count register | increments every 2 PClocks | [UM] 6.3.4; [n64brew] "ROM Header" |

The model keeps CPU time in PClocks and converts exactly (3 PClocks = 2 ticks).

## Identification and reset

| Register | Value | Source |
|---|---|---|
| PRId | 0x00000B22 | [n64brew] "VR4300", Revision Identifiers; [krom] COP0Register |
| FCR0 | 0x00000A00 | [st] `cop1` CFC1CTC1_0 ("CFC1 0 returns 0xA00") |
| Config after IPL1 | 0x7006E463 (`MTC0 0x0006E463` plus fixed bits) | [n64brew] "Initial Program Load" IPL1; [st] `startup` |
| Status after IPL1 | 0x34000000 (CU1, CU0, FR) | [n64brew] "Initial Program Load" IPL1; [st] `startup` |

Cold reset (UM 6.4.4): Status.BEV = ERL = 1, SR = TS = RP = 0; Random = 31; Wired = 0; Config
EP = 0, BE = 1; ErrorEPC = PC; PC = 0xFFFFFFFF_BFC00000. Soft reset and NMI (6.4.5, 6.4.6) set
SR and keep the other registers. Undefined power-on values are ledger entries
(`cop0-cause-poweron`, `cop0-lladdr-poweron`, `cop0-undefined-poweron`).

## Pipeline timing model

The interpreter executes one instruction per step, in program order, and accounts for time by
pipeline stage (IC RF EX DC WB; ADR 0005). In one pipeline cycle DC holds instruction i-1, EX
holds i and RF holds i+1 ([UM] 4.1, Figure 4-2). So a step for i spends:

1. one run cycle (i in EX);
2. i's EX interlocks: LDI, MCI, the FPU bypass stall;
3. the RF fetch of i+1: I-cache refill, uncached fetch, micro-TLB miss. It starts at the run
   cycle, so its cost overlaps step 2 ([UM] 4.7.3: RF and EX interlocks are resolved at the same
   time). The fetched word waits in a one-entry latch for the next step;
4. i's DC work: translation, D-cache, SysAD.

Hence:

- **Fetch order.** i+1's instruction fetch reaches the bus before i's data access. A DC stall is
  resolved before a simultaneous RF stall ([UM] 4.7.3), which is the order of i-1's DC (in its
  own step) and i+1's RF.
- **MCI overlaps the previous DC stall.** EX completes a multicycle instruction while DC resolves
  a stall, unless it uses that load's result ([UM] 4.6.1). A pipeline cycle's stall is therefore
  `max(MCI, DC + RF)`.
- **Exceptions cost by stage** (Figure 4-12): RF detects instruction address, TLB and bus
  errors; EX detects SYSCALL, BREAK, coprocessor unusable and reserved instruction; DC detects
  the rest, interrupts included. After the detection cycle the pipeline stalls 2 PCycles and the
  excepting and younger instructions are aborted ([UM] 4.7). The handler then takes IC and RF.
  From the start of the excepting instruction's step that is 4 (RF), 5 (EX) or 6 (DC)
  PCycles, plus the DC stage's own stalls before detection. The IC/stall overlap is ledgered
  (`exception-redirect-cycle`).
  - An EX exception outranks LDI ([UM] 4.7.7). It cancels i+1's fetch, which then makes no
    memory request ([UM] 4.7.3).
  - A fetch fault found by the early fetch is raised by that instruction's own step, in program
    order.
  - An interrupt is detected in the cycle after a step boundary. It is accepted only after a run
    cycle, so it is one cycle later when the previous cycle stalled ([UM] 4.7.1).
- **Killed delay slots cost a cycle.** A branch-likely not taken nullifies a slot that was
  already fetched; ERET does not execute the instruction behind it ([UM] 4.2, chapter 16 ERET).
- CP0I ([UM] 4.6.9) is not modelled (ledger `cp0-bypass-interlock`).

Stall amounts:

| Stall | Cycles | Source |
|---|---|---|
| Load interlock: next non-FPU instruction whose rs or rt names the loaded GPR (r0 exempt); next FPU instruction whose fs or ft names a loaded FPR | 1 | [UM] 4.6.5; detection rule [n64brew] "VR4300", Load Delay Interlock |
| MULT/MULTU, DMULT/DMULTU, DIV/DIVU, DDIV/DDIVU | 5, 5, 8, 8, 37, 37, 69, 69 total | [UM] Table 3-12 |
| FPU operations | Table 7-14 (add/sub 3; mul 5 S / 8 D; div, sqrt 29 S / 58 D; round/trunc/ceil/floor/cvt to integer 5; cvt.s.d 2; cvt.d.s 1; cvt from integer 5; abs/mov/neg/c.cond 1) | [UM] Table 7-14 |
| FPU early completion (2 cycles): add/sub with a zero or infinite operand; multiply with result zero/infinity or a power-of-two operand; divide/sqrt with result zero or infinity; trivial converts (zero/infinite operand, integer zero) | 2 | [UM] 7.5; confirmed by [krom] CP1TIMINGNTSC counts |
| FPU result used by the next FPU instruction | +1 | [UM] 7.5.6 / Table 7-14 note 1 |
| Store followed by a D-cache access | 1 | [UM] 4.6.7 |
| D-cache CACHE op | 1 extra (2 total) | [UM] 4.6.8 |
| Exception abort stall, then the handler's IC and RF | 2 + 2 after the detection cycle | [UM] 4.7, 4.1 |
| Instruction micro-TLB miss (mapped fetch) | 3 | [UM] 4.6.2 |
| Killed delay slot | 1 | [UM] 4.2 |
| SysAD read (cache refill, uncached load or fetch) | 5 + MasterClock alignment, then the device's response, then 1 | [UM] Tables 11-1, 11-2 |
| Write buffer full | until the oldest write is accepted | [UM] 4.9 |

`SYNC` is a NOP on the VR4300 ([UM] SYNC instruction page).

## SysAD bus

- Uncached reads stall the pipeline for the full round trip; uncached writes enter a 4-entry
  write buffer and reach the bus in order, before any later read ([UM] 4.9). The model
  presents each buffered write to the bus at its true issue time, so devices observe it then.
- Sub-word stores drive SysAD with the register shifted into the addressed byte lanes; devices
  that ignore the access size (all RCP registers, PIF RAM, cartridge writes) see the other
  lanes too ([n64brew] "Memory map"; [st] `cart_memory`, `pif_memory`).
- RCP register accesses ignore the size: reads return the 32-bit word; 64-bit reads never
  complete and freeze the CPU; 64-bit writes store the most significant word ([n64brew]
  "Memory map", Range 0x0400_0000-0x04FF_FFFF).
- Cached (block) accesses outside RDRAM are never answered and freeze the CPU ([n64brew]
  "Memory map").
- RCP register read response: 10 MasterClocks (Measured, ledger `rcp-reg-read`).

## Address translation

- 32-bit and 64-bit segments per [UM] Tables 5-3/5-4 and Figure 5-7; in 32-bit mode an address
  that is not a sign-extended 32-bit value raises an address error.
- With Status.ERL = 1, kuseg is an unmapped, uncached 2 GiB window ([UM] 5.2.4).
- KSEG0 cacheability follows Config.K0 (2 = uncached).
- TLB: 32 entries, each mapping an even/odd page pair; compare R (63:62) and VPN2 (39:13) above
  the page mask, ASID unless both G bits are set ([UM] 5.3, 5.4.9). Lookups take the first
  matching entry.
- TLBWI/TLBWR store PageMask per bit pair (each pair takes its upper bit), EntryLo masked to
  PFN, C, D, V, G (0x03FFFFFF) ([st] `tlb` "PageMask after TLBWI/TLBR", "EntryLo0/1/Hi after
  TLBWI/TLBR"); G reads back in both EntryLo registers ([UM] 5.3.1).
- Random counts down from 31 to Wired once per retired instruction ([UM] 5.4.2). Writing Wired
  resets it to 31 as read by the second instruction after the MTC0; the first still reads the
  old sequence, wrapping at the old Wired ([st] `cop0` RandomDecrement, RandomMasking,
  RandomReadEarly).
- On address errors and TLB exceptions BadVAddr, Context.BadVPN2 and XContext (BadVPN2, R) are
  written, also for address errors ([st] `tlb64` "LW TLB Miss or Address Exception", "LW with
  address not sign extended"). Partial loads/stores (LWL/LWR/LDL/LDR/SWL/SWR/SDL/SDR) translate
  and report the unaligned address itself.
- The instruction micro-TLB (2 entries, 4 KiB, LRU; [UM] 1.5.1) is modelled for timing only:
  translation always reads the joint TLB, and a micro-TLB miss on a fetch through the JTLB costs
  3 PCycles in RF ([UM] 4.6.2). When it is flushed is a Guess (ledger `itlb-invalidation`).
  There is no data micro-TLB: data accesses read the joint TLB ([UM] Table 4-1).

## Caches

- I-cache 16 KiB, direct mapped, 32-byte lines; D-cache 8 KiB, direct mapped, 16-byte lines,
  write-back; both virtually indexed, physically tagged ([UM] chapter 11).
- A D-cache miss writes back a dirty victim, then refills the line ([UM] 11.3.2). Cache state is
  not invalidated by reset ([UM] 11.4).
- CACHE operations per [UM] 16 (CACHE); the I-cache op stall is a Guess (`cache-op-icache`).

## Exceptions

- Vectors: base 0x80000000 (BEV = 0) or 0xBFC00200 (BEV = 1); TLB refill +0x000, XTLB refill
  +0x080 (only with EXL = 0), all others +0x180 ([UM] Tables 6-3, 6-4).
- EPC and Cause.BD are written only when EXL = 0; EPC points at the branch when the faulting
  instruction is in a delay slot ([UM] 6.3.6, 6.3.7).
- Count == Compare sets Cause.IP7; writing Compare clears it ([UM] 6.3.4).
- An FPE raised by CTC1 (a FCSR write with a cause bit enabled, or E) sets Cause.CE to the
  coprocessor number of the next instruction when it is a COPz instruction, else 0; when CTC1
  is in a delay slot, the next instruction is the branch target ([st] `cop1`
  FireExceptionViaCTC1, ...FollowedByMFC1, ...FollowedByMFC2, ...Delay). Other cases: ledger
  `cause-ce-next-instr`.
- Interrupt pins: Int0 = RCP (MI), Int2 = PIF pre-NMI ([n64brew] "PIF-NUS").
- Watch: WatchLo.PAddr0 compares physical address bits 31:3 for loads (R) and stores (W);
  deferred while EXL is set ([UM] 6.4.17).
- 64-bit operations are valid in kernel mode regardless of KX ([UM] 6.3.5); otherwise they raise
  RI when 64-bit operations are disabled for the current mode.

## Integer quirks

| Behaviour | Source |
|---|---|
| SRA/SRAV shift the full 64-bit register, then sign-extend bit 31 | [n64brew] "VR4300", 32-bit Shift Right Arithmetic Bug |
| MULT multiplies 64-bit rs by rt sign-extended from bit 34; DIV divides the 32-bit dividend by a divisor sign-extended from bit 34; DIV by zero gives LO = (n < 0 ? 1 : −1), HI = n | [n64brew] "VR4300", Sign extension bugs |
| LWR that does not include the word's most significant byte keeps the upper 32 bits of rt; only a full-word LWR sign-extends | [UM] LWR ("the remaining portion of the register is not affected"); [st] `tlb64` load matrix |
| JAL/JALR/BxxAL link to the instruction after the delay slot, also when the jump itself is in a delay slot | [UM] chapter 16; [st] `jumps` |
| A conditional branch adds its offset to the address of the instruction fetched after it, and BxxAL links to that address + 4. That is the branch address + 4, except for a branch in another jump's or branch's delay slot, where it is the outer target. J/JAL in that position: ledger `jump-region-in-delay-slot` | [st] `jumps/conditionals.rs` BEQWithinDelay, BEQWithinDelayOfJR, BGEZALWithinDelay, BGEZALWithinDelayOfBEQ |
| Status.RE in user mode reverses endianness: data addresses XOR (8 − size) for sub-doubleword accesses, instruction fetches XOR 4 | [UM] 6.3.5; [st] `endian_re` |
| COP2 with CU2 = 1: MFC2/DMFC2/MTC2/DMTC2 and LWC2/LDC2/SWC2/SDC2 move through one 64-bit latch; CFC2/CTC2 do nothing; anything else is RI with Cause.CE = 2. COP3 is RI with CE = 0 | [st] `cop_unusable` (COP2MFCBehavior, COP2LWC2Behavior) |

## FPU

| Behaviour | Source |
|---|---|
| Arithmetic uses host IEEE-754 with the FCR31 rounding mode applied explicitly | spec rule 7 |
| Status.FR = 0: computational operands read `fs` with its low bit cleared; `ft` is used as given; results write all 64 bits of the destination (singles zero-extended); MOV.S/MOV.D copy 64 bits | [st] `cop1/full_vs_half_mode.rs`, "Upper bits of 32 bit operation" |
| Denormal operands and MIPS-quiet NaN operands (fraction MSB clear) raise Unimplemented Operation; MIPS-signalling NaNs (fraction MSB set) are Invalid | [UM] 8.2.7; [st] `cop1` "Lessons learned" |
| Denormal or underflowing results raise Unimplemented unless FS = 1 and the Underflow and Inexact enables are clear, in which case they flush per Table 7-2 | [UM] Table 7-2; [st] `cop1` |
| Generated NaNs: 0x7FBFFFFF (S), 0x7FF7FFFFFFFFFFFF (D) | [st] `cop1` |
| FCR31 write mask 0x0183FFFF; enabled cause bits (or E) raise the exception at CTC1 | [st] `cop1` CFC1CTC1_31 |
| Conversions to W: Unimplemented outside [−2^31, 2^31); to L: Unimplemented for \|x\| ≥ 2^53; from L: Unimplemented outside ±2^55 (Guess, `cop1-cvt-from-l`) | [UM] 8.2.7; [st] "COP1: CVT.L, ROUND.L, ..." |
| CVT.S.S, CVT.D.D and reserved formats are Unimplemented | [UM] 8.2.7 |

## Boot

The CPU enters IPL3 at 0xA4000040 with s3 = ROM type, s4 = TV type, s5 = reset type, s6 = CIC
seed, s7 = 0, sp = 0xA4001FF0 (docs/hardware/pif.md).

## Implementation note: decode

`Vr4300::execute` decodes with a switch on the primary opcode and a second switch on the
function / rs / rt field (compiled to jump tables). The spec asks for "a precomputed decode
table; decoding is a 64K-entry switch on opcode bits". No 16-bit key covers every MIPS
format (SPECIAL needs opcode and funct, COP1 needs opcode, fmt and funct), so this has not been
changed without a decision on the intended key. Open for review.
