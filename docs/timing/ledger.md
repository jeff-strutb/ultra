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
| `cop0-cause-poweron` | Cause after reset | 0xB000007C | Guess | Undefined after reset (manual 6.3.6). krom COP0Cause expected output; COP0Register's EverDrive capture shows 0xB0000078 (both pass the ROM's mask 0x033F0083) | Probe on cold boot |
| `cop0-lladdr-poweron` | LLAddr after reset | 0xFFFFFFFF | Guess | Undefined (manual 5.4.7); krom LL_LLD_SC_SCD expected output | Probe on cold boot |
| `cop0-undefined-poweron` | BadVAddr, EPC, ErrorEPC, Context.BadVPN2, XContext.BadVPN2/R after first power-on | all ones | Derived | n64-systemtest `src/tests/startup/mod.rs`: BadVAddr "has a known value after turning on" (all ones), ExceptPC/ErrorEPC "usually 0xFFFFFFFF_FFFFFFFF after first turn on"; krom COP0Register capture shows Context 0x007FFFF0, XContext low word 0xFFFFFFF0 | — |
| `cop1-cvt-from-l` | CVT.S/D.L raise Unimplemented outside ±2^55 | ±2^55 | Guess | n64-systemtest covers the 2^53 limit of the reverse conversion only | Probe in Phase 1 |
| `cop1-fcr-unimplemented` | CFC1 of FCR1..30 | last CTC1 value | Guess | No source; no known software reads them | — |
| `cop2-latch` | COP2 64-bit latch semantics beyond n64-systemtest COP2LWC2Behavior | as tested | Guess | n64-systemtest calls the quirk "poorly understood" | — |
| `micro-tlb` | VR4300 micro-TLBs (ITLB/DTLB) | not modelled | Guess | Manual 5.3: micro-TLB misses refill from the JTLB in 1-3 cycles; timing effect only | Probe 2 |

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
