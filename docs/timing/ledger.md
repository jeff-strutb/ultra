# Timing and behaviour ledger

Every hardware parameter or behaviour that is not taken directly from a primary source is listed
here with its confidence, its evidence and the probe that will settle it. Source comments name
their entry as `ledger "<key>"`. Confidence levels (`src/memory/arbiter.hpp`):

- **Measured**: fitted to a hardware measurement made by a test ROM.
- **Derived**: computed from a primary source (manual, datasheet, wiki measurement).
- **Guess**: placeholder; the probe column names what will measure it.

The second half lists every test-manifest entry whose expected outcome is `fail`
(`tests/roms/manifest.txt`). The harness fails on any unexpected outcome, including an
unexpected pass, so this ledger and the manifest must change together.

## Parameters

### CPU and SysAD

| Key | Parameter | Value | Confidence | Evidence | Settled by |
|---|---|---|---|---|---|
| `rcp-reg-read` | RCP register / SP memory read response (`kRcpRegReadTicks`, core/bus.cpp) | 10 ticks | Measured | krom CPUTIMINGNTSC hardware counts: about 56092 loops of {ALU op, `LW VI_V_CURRENT`, `SYNC`, `BNE`, `ADDIU`} per 256 NTSC lines, a loop of 18 MasterClocks; minus the VR4300 stall of Tables 11-1/11-2 (`sysad-read-stall`) leaves 10 ticks. n64brew "Memory map" says "5-6 PClock cycles", which does not fit this measurement | Probe 1 (CPU bus latency), exact |
| `mi-reg-read` | MI register read response (`kMiRegReadTicks`) | 7 ticks | Guess | n64brew "Memory map": MI is "a bit faster" (about 2 vs 5-6 PClocks), applied as 3 ticks less than `rcp-reg-read` | Probe 1 |
| `rcp-reg-write` | RCP / MI register write acceptance (`kRcpRegWriteTicks`, `kMiRegWriteTicks`) | 4 / 2 ticks | Derived | n64brew "Memory map" ("5-6 PClock cycles", MI "about 2"), rounded up to ticks. Writes go through the write buffer, so this only paces back-to-back writes | Probe 1 |
| `sysad-read-stall` | VR4300 stall around a SysAD read (`kSysAdReadIssueCycles`, `kSysAdReadReturnCycles`) | 5 PCycles + MasterClock alignment, then 1 PCycle | Derived | VR4300 manual Tables 11-1 / 11-2: stage stall 1, address to write buffer 1, SClock sync and internal bus 1-2, external bus 2; data to cache/register 1. The tables' memory-to-SysAD cycles are counted in the device response | Probe 1 |
| `cpu-timing-residual` | Unmodelled per-iteration delay in uncached-read loops | 0.7-1.7 % of loop time | Open | krom CPU/CP1 TIMINGNTSC: every count is 0.14-1.72 % higher than hardware. In MasterClocks the hardware loops are 18.13 (ALU), 20.34 (MULT, CVT.W), 22.10 (DMULT), 42.06 (DIV), 63.13 (DDIV) per iteration: whole numbers plus a fraction that varies with the loop period rather than per line, so it is not a VI line-length error. Likely a periodic RCP-side busy window (refresh or VI fetch); not identified | Probe 1 + probe 6 |
| `cache-op-icache` | I-cache CACHE op stall (`kCacheOpICycles`) | 1 PCycle | Guess | VR4300 manual 4.6.8 gives D-cache tag ops 2 PCycles; I-cache ops unspecified | Probe 2 (cache and TLB refill) |
| `fpu-early-completion` | FPU ops finishing in the second cycle | 2 PCycles | Derived | VR4300 manual 7.5: add/sub with a zero or infinite operand; multiply with result zero/infinity or a power-of-two operand; divide/sqrt with result zero or infinity; trivial converts. krom CP1TIMINGNTSC confirms for MUL/DIV/SQRT of 0.0 and CVT.S/D.W/L of 0; ROUND/TRUNC/CEIL/FLOOR/CVT.W/L of 0.0 take the full 5 cycles on hardware and do here | — |
| `cop0-cause-poweron` | Cause after reset | 0xB000007C | Guess | Undefined after reset (manual 6.3.6). krom COP0Cause expected output; COP0Register's EverDrive capture shows 0xB0000078 (both pass the ROM's mask 0x033F0083). The Exceptions references show launcher-left values 0x00000000, 0xB0000074, 0xB000007C and 0xB000003C at the same point, so no EverDrive capture is evidence for the power-on value | Probe on cold boot |
| `cop0-lladdr-poweron` | LLAddr after reset | 0xFFFFFFFF | Guess | Undefined (manual 5.4.7); krom LL_LLD_SC_SCD expected output | Probe on cold boot |
| `cop0-undefined-poweron` | BadVAddr, EPC, ErrorEPC, Context.BadVPN2, XContext.BadVPN2/R after first power-on | all ones | Derived | n64-systemtest `src/tests/startup/mod.rs`: BadVAddr "has a known value after turning on" (all ones), ExceptPC/ErrorEPC "usually 0xFFFFFFFF_FFFFFFFF after first turn on"; krom COP0Register capture shows Context 0x007FFFF0, XContext low word 0xFFFFFFF0 | — |
| `cop1-cvt-from-l` | CVT.S/D.L raise Unimplemented outside ±2^55 | ±2^55 | Guess | n64-systemtest covers the 2^53 limit of the reverse conversion only | Probe in Phase 1 |
| `cop1-fcr-unimplemented` | CFC1 of FCR1..30 | last CTC1 value | Guess | No source; no known software reads them | — |
| `cop2-latch` | COP2 64-bit latch semantics beyond n64-systemtest COP2LWC2Behavior | as tested | Guess | n64-systemtest calls the quirk "poorly understood" | — |
| `micro-tlb` | VR4300 micro-TLBs (ITLB/DTLB) | not modelled | Guess | Manual 5.3: micro-TLB misses refill from the JTLB in 1-3 cycles; timing effect only | Probe 2 |
| `tlb-poweron` | TLB entries after cold reset (`Vr4300` constructor) | all 32 entries zero (VPN2 0, ASID 0, G 0, V 0, PageMask 0) | Guess | Undefined (manual 6.4.4 Cold Reset Exception: "caches and virtual memory are in an undefined state"); no hardware dump is known. Consequence: an access to VPN2 0 with ASID 0 before software writes the TLB matches an invalid entry and takes a TLB Invalid exception (vector +0x180) where a console whose entries do not match would take a refill (+0x000, or +0x080 with KX/SX/UX set). This decides the Handler PC and nested-access rows of krom's Exceptions/TLB ROMs (`krom-reference-exceptions-tlb`) | Probe: cold boot through the PIF (no launcher), TLBR all 32 entries before any TLB write, print EntryHi/EntryLo0/EntryLo1/PageMask |
| `tlb-shutdown` | TLB shutdown (Status.TS) when several entries match one address | not modelled: the lowest-index match wins | Guess | Manual 6.3.5 Status.TS: set when more than one entry matches, "even when ... the V bit of the entry is cleared". With `tlb-poweron`'s 32 identical entries, hardware might shut the TLB down on the first access to VPN2 0 | Probe: write two matching entries, access the page, read Status.TS |
| `cause-ce-next-instr` | Cause.CE taken from the instruction behind the one raising the exception | modelled only for an FPE raised by CTC1: the next instruction's COPz number, else 0 | Guess | n64-systemtest `cop1` FireExceptionViaCTC1 (NOP: 0), ...FollowedByMFC1 / MFC2 (1 / 2). Whether LWC1/SWC1 or COP0 count, and whether other exceptions (SYSCALL, overflow, a CTC1 in a branch-likely slot) take CE the same way, is unmeasured | Probe: SYSCALL, ADD overflow and CTC1 each followed by MFC1, MFC2, LWC1 and MTC0; read Cause.CE |
| `jump-region-in-delay-slot` | J/JAL target region (bits 63:28) when the jump is in another jump's delay slot | the jump's own address + 4 | Guess | Manual chapter 16 forms the target from the delay-slot address. n64-systemtest shows conditional branches in a delay slot offset from the outer target (`cpu.md`, Integer quirks), so J may take its region from it too; only visible across a 256 MiB boundary | Probe: J in the delay slot of a J into another 256 MiB region |

### Memory (RDRAM and the v1 arbiter)

| Key | Parameter | Value | Confidence | Evidence | Settled by |
|---|---|---|---|---|---|
| `arb-row-miss` | `kRowMissTCycles` | 28 TCycles | Guess | No public measurement | Probe 4 (RDRAM access patterns) |
| `arb-row-miss-dirty` | `kRowMissDirtyExtraTCycles` | 8 TCycles | Guess | Ratio from RI_REFRESH dirty/clean delays (54 vs 52, n64brew "RDRAM Interface") | Probe 4 |
| `arb-ri-overhead` | `kRiOverheadTCycles` | 8 TCycles | Guess | No public measurement | Probe 4 |
| `arb-refresh` | `kRefreshTCycles` | 208 TCycles | Guess | 52 x 4 from RI_REFRESH CleanRefreshDelay | Probe 6 (VI and refresh coupling) |
| `rdram-cc` | Current-control threshold `Rdram::kMinCurrentUnits` | 16 | Guess | Chosen so IPL3's manual-mode CC calibration converges as on hardware (n64brew "RDRAM", current calibration) | Probe 4 |
| `rdram-cc-auto` | Mode.CC readback in automatic mode | programmed value inverted | Guess | IPL3's search raises CCi until the readback reaches its target; this is the simplest monotonic model that lets it converge | Probe 4 |
| `rdram-manufacturer` | DeviceManufacturer register | 0x00000500 (NEC) | Guess | Varies per console; nothing reads it | — |
| `mi-ebus` | MI EBus mode: 9th bits in the low nibble on reads and writes | as described in io/mi.cpp | Guess | n64brew "MIPS Interface" names the mode without bit placement | Probe 4 |

### I/O

| Key | Parameter | Value | Confidence | Evidence | Settled by |
|---|---|---|---|---|---|
| `pi-address-phase` | PI address phase per new page (`kAddressPhaseTicks`) | 14 ticks | Guess | n64brew gives ALEH ≥ 7 and ALEL ≥ 14 "FSB cycles" without a unit | Probe 3 family (PI DMA) |
| `pi-busy-writes` | Writing a DMA register while busy sets the error bit and is ignored | as described | Guess | n64brew "Parallel Interface" mentions the error bit; trigger unmeasured | Probe 3 family |
| `si-pif` | PIF firmware processing time on top of the SI wire time | 0 | Guess | Wire time is Derived (MClock/32 per bit, n64brew "Clock Timing", "Serial Interface") | Phase 1 SI work |
| `pif-boot-timeout` | Boot-termination timeout | 5 s exactly | Guess | n64brew "PIF-NUS", command 0x08: "before 5 seconds" | — |
| `boot-rom-timing` | Time from reset to IPL3 entry | that of our clean-room IPL1/IPL2 | Guess | Nintendo's PIF ROM is not shipped; instruction count differs | Probe on cold boot |
| `pif-boot-info` | Boot-info word layout at PIF RAM 0x24 | ours (pif.hpp) | Design | Private contract between our PIF and our IPL2; IPL3 only sees the registers | — |
| `ipl3-s7` | s7 at IPL3 entry | 0 | Guess | 6102 and libdragon IPL3 disassemblies do not read s7 | — |
| `ai-fetch` | AI DMA fetch granularity | 8 bytes | Guess | Unmeasured | Probe "ai-dma" (Phase 1) |
| `ai-status-count` | AI_STATUS COUNT/BC/WC fields | not modelled (0) | Guess | n64brew "Audio Interface" | Phase 1 AI work |
| `dp-span` | DP span registers 0x0430_0000 | read 0, writes ignored | Guess | Test registers; RDP is Phase 2 | Phase 2 |

## Expected-fail manifest entries

| Key | Manifest id | Why it fails | Evidence |
|---|---|---|---|
| `krom-reference-sb` | `krom/CPU/LOADSTORE/SB/CPUSB` | The reference PNG has one extra scanline at y = 40: every row from 40 to the end of the text is one line lower than in our capture, which otherwise matches pixel for pixel. The ROM draws each text line at a constant framebuffer address with y a multiple of 8, so no CPU behaviour can produce it; LB/LH/LW/LD/SD/SW use the same drawing code and match exactly | `tests/roms/krom/CPUTest/CPU/LOADSTORE/SB/CPUSB.asm` (PrintString y = 24, 32, 40 ...) |
| `krom-reference-sh` | `krom/CPU/LOADSTORE/SH/CPUSH` | Same capture artefact as SB, also at y = 40 | as above |
| `krom-reference-dma-rom-from` | `krom/DMAAlignment-PI-cart/DMAAlignment-PI-ROM-FROM` | The reference is inconsistent with the ROM that shipped with it: its "1->0" row shows the 2->0 data next to a green PASS, but the ROM prints PASS only when the hash equals 0x0039CF16, the 0->0 hash (an odd CART_ADDR behaves as even; consistent with n64-systemtest's hardware-verified "cart address (masking)" test, which shows PI_CART_ADDR dropping bit 0); its "double" row shows PI_DRAM_ADDR = 0x80024408 next to a PASS for a check against 0x24408. Our capture is consistent with the ROM's own checks (all PASS) | `DMAAlignment-PI-ROM-FROM.asm` lines 593-609 |
| `krom-reference-cop0register` | `krom/CP0/COP0Register` | The references were captured after an EverDrive64 launch: Status 0x241000E0 is exactly the value n64-systemtest flags as "EverDrive64 bug", where a console booting through the PIF has 0x34000000 (n64brew IPL1 disassembly, `MTC0 0x34000000, SR`). Our capture fails only that row; the other masked checks pass, and the unmasked values differ because they depend on the launcher | n64-systemtest `src/tests/startup/mod.rs`; n64brew "Initial Program Load" |
| `cpu-timing-residual` | `krom/CPU/TIMINGNTSC-exact`, `krom/CP1/TIMINGNTSC-exact` | Counts are within 2 % of hardware (the `TIMINGNTSC` entries pass at that tolerance), not within the hardware's own noise floor (about ±4 counts across identical ALU ops). See `cpu-timing-residual` above | `*.counts` files next to the ROMs |
| `krom-reference-everdrive-exceptions` | `krom/Exceptions/Syscall/ExceptionSyscall`, `…SyscallDelay`, `…SyscallDelay2`, `…SyscallWhileInException`, `krom/Exceptions/Trap/ExceptionTEQ`, `…TEQDelay`, `krom/Exceptions/Unaligned/ExceptionUnaligned`, `…UnalignedDelay`, `krom/Exceptions/Compare/ExceptionCompareRegisters`, `krom/Exceptions/VIIntr/ExceptionVIIntrDisabled` | The references were captured after an EverDrive64 launch (see `krom-reference-cop0register`). The first nine fail only the ROM's own Status row, which expects the launcher value 0x241000E0 before, during (EXL set) and after the exception; ours shows 0x34000000 / 0x34000002 / 0x34000000, and every other checked row passes. The remaining pixel differences are unchecked "before" values the launcher left: Cause (0x00000000 in VIIntrDisabled, 0xB000007C ours; ledger `cop0-cause-poweron`), BadVAddr 0xFDFFFFFF / 0xFDFFFFFD and Context 0x007EFFF0 in SyscallDelay2 and Unaligned (ours all ones, ledger `cop0-undefined-poweron`). VIIntrDisabled passes every row it checks and differs only in those Cause values | Side-by-side capture, 2026-10-19; ROM sources next to the ROMs |
| `krom-reference-exceptions-tlb` | `krom/Exceptions/TLB/ExceptionTLBReadMiss`, `…ReadMissDelay`, `…ReadMissNested`, `…ReadMissNestedDelay`, `…WriteMiss`, `…WriteMissDelay` | Status row as in `krom-reference-everdrive-exceptions`, plus: the ROMs access address 0x10 with the launcher's TLB contents and Status KX = 1, so hardware took an XTLB refill to 0x80000080 (ReadMiss/WriteMiss expect that Handler PC; the Nested pair's 0x080 handler makes a second access to 0x20, refilled through 0x180 with EXL set, and expect BadVAddr 0x20). After a PIF boot KX = 0, so a refill would go to 0x80000000 and no outcome can match. Ours takes a TLB Invalid exception to 0x80000180 because `tlb-poweron`'s zero entries match VPN2 0 / ASID 0, so the four miss ROMs fail Handler PC and the Nested pair never makes the second access and fails BadVAddr (0x10). EntryHi/EntryLo "before" rows show the launcher's values (e.g. 0xDEF5E0AB / 0x01FBFFE3 / 0x19BEFF3F); ours are zero | Side-by-side capture, 2026-10-19; manual 6.4.4; ROM sources |
| `exception-compare-latency` | `krom/Exceptions/Compare/ExceptionCompareDisabled` | Every checked row passes (Handler PC and Count delta within the ROM's 0x01000000-0x01000100 window). The displayed Count deltas past Compare differ: hardware 0x1B, 0x04, 0x1F, 0x52, 0x45 for the five runs, ours 0x06, 0x06, 0x03, 0x2F, 0x27. Runs 1-3 execute the same polling loop and the hardware spread (4-31 Count ticks, 8-62 PClocks) shows a run-to-run source of delay the model lacks; it may be the same unidentified RCP-side window as `cpu-timing-residual`. Not identified, so not modelled | Side-by-side capture, 2026-10-19; Probe 1 + probe 6 |
