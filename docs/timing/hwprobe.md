# hwprobe: the console measurement ROM

`tests/probes/hwprobe/` builds one libdragon ROM that measures, on a real N64, every ledger
entry a CPU-side probe can settle, and writes the raw results to the cartridge's FlashRAM.
The flashcart stores FlashRAM as a 128 KiB `.sav` file, which `tools/hwprobe.py` decodes. The
same ROM runs headless in Ultra (`ultra_harness hwprobe`), which dumps the FlashRAM it
emulated, and `tools/hwprobe.py compare` diffs the two records series by series.

The oracle is the console's record. Ultra's record is never an oracle.

## Why FlashRAM

- 128 KiB holds the raw per-iteration samples (stage 1 uses about 96 KiB of its 112 KiB) the `cpu-timing-residual` question needs; 32 KiB
  SRAM does not.
- Every flashcart emulates it, real carts use it, and its protocol is documented (n64brew
  "Flash"). 1 Mbit SRAM is also offered by flashcarts, but its address map is not documented.
- The ROM declares it through the Advanced Homebrew ROM Header (`N64_ROM_SAVETYPE = flashram`,
  byte 0x3F = 0x50, n64brew "ROM Header"), so the flashcart configures it without a database.

## Running it

No controller is needed and nothing is chosen on screen. RESET is the only control.

1. Copy `tests/probes/hwprobe/hwprobe.z64` to the flashcart and boot it. Stage 1 measures and
   writes its record (under 15 s), then shows `STAGE 1 DONE. PRESS RESET.`
2. Press RESET. On a flashcart that resets into the game, stage 2 records the state after the
   soft reset, then shows `ALL DONE. PRESS RESET, THEN POWER OFF.` On one that resets into its
   menu, stage 2 does not run and the stage 1 record is complete on its own.
3. Press RESET once more, so that every flashcart writes the save to its SD card (some write it
   only on a reset), then power off. Any later boot after a RESET only shows `ALL DONE`. Send
   the `.sav` file.

The ROM decides what to do from what FlashRAM already holds. A cold boot always starts a new
stage 1 and erases the old record. A RESET after stage 1 runs stage 2. A RESET after stage 2
changes nothing.

If the screen stops on a group, the record still holds every group before it: pages are
programmed as each group finishes, and each series has its own CRC. Report the group name
shown on the screen.

**Cold boot.** For the power-on rows (`tlb-poweron`, `cop0-cause-poweron`,
`cop0-lladdr-poweron`, `cop0-undefined-poweron`), boot the ROM with no menu code first: a
flashcart direct-boot mode (SC64), or a cart whose menu is known not to touch the TLB. The
record does not know how it was booted, so say which. libdragon's IPL3 runs before our first
instruction. It writes Cause = 0, Count = 0, Compare = 0 and WatchLo = 0, and invalidates both
caches (`third_party/libdragon/boot/ipl3.c`, `stage1()`). Only Cause's IP1:0 bits are writable,
so its BD, CE and ExcCode fields keep their power-on values. IPL3 never touches the TLB,
EntryHi/EntryLo, PageMask, Wired, Context, XContext, BadVAddr, EPC, ErrorEPC, LLAddr or Status.
Count at entry is the time IPL3 took after zeroing it. The time from reset to IPL3
(`boot-rom-timing`) is not measurable this way.

## Record format (version 1)

All fields are big-endian, as the CPU writes them.

FlashRAM 0x00000-0x1BFFF holds stage 1 and 0x1C000-0x1FFFF (sector 7) holds stage 2. Each area
starts with a 64-byte header:

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | magic `ULTRAHW1` (stage 1) or `ULTRAHW2` (stage 2) |
| 8 | 4 | format version (1) |
| 12 | 4 | bytes of series that follow the header |
| 16 | 4 | CRC-32 (IEEE) of those bytes |
| 20 | 4 | series count |
| 24 | 4 | status: 0 = complete, otherwise the id of the group that failed |
| 28 | 16 | build id: first 12 hex digits of the SHA-1 of the ROM's sources (Makefile), ASCII, NUL padded |
| 44 | 8 | FlashRAM silicon ID as read by the ROM |
| 52 | 12 | reserved, zero |

The header page is programmed last, so a record that stopped early has an erased (all 0xFF)
header and a valid series stream that the decoder still reads.

A series is a 12-byte header and then its data, padded to 4 bytes:

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | series id (`tests/probes/hwprobe/series.h`) |
| 2 | 1 | element size in bytes: 1, 2, 4 or 8 |
| 3 | 1 | flags, 0 |
| 4 | 4 | element count |
| 8 | 4 | CRC-32 of the data |

`series.h` is the single list of series ids, with each one's meaning and element layout in
its comment. The decoder reads its names from that file.

Flags 1 and 2 mark nibble-packed byte series, used for the loop samples. The data is: the first
byte of the sequence (0 for flag 2), a base value, a 16-bit escape count, one nibble per item
(high nibble first) holding item - base, or 15 for "the next escape byte", and then the escape
bytes. With flag 1 (deltas), the items are the n - 1 differences mod 256 between consecutive
bytes. With flag 2 (values), they are the n bytes themselves. The element count is n.

## Timing method

Every timed region is bracketed by `MFC0 Count`. Count advances once every two PCycles,
which is 0.75 MasterClocks per tick (n64brew "ROM Header", Clock Rate). Each measurement is
repeated (usually 32 or 64 times), and every repetition is stored, so the first, cold
repetition and the spread are both in the record. Interrupts are disabled in every timed
region, except where the probe's subject is the interrupt. Probes that take exceptions point
the vectors at 0x80000000, 0x80000080, 0x80000100 and 0x80000180 at their own handler, and
restore libdragon's afterwards. An unexpected exception is recorded (Cause, EPC, BadVAddr,
group id), and the group is abandoned rather than the run.

## Groups

The id in brackets is the series id prefix in `series.h`.

### Boot state [0x01, stage 2: 0x02]

Captured by `hwp_entry`, linked as the ELF entry point so that it runs before libdragon's
`_start`:

- All 32 COP0 registers by DMFC0 (the unimplemented numbers 7, 21-25 and 31 included),
  FCR0/FCR31, GPRs and HI/LO as IPL3 left them.
- All 32 TLB entries (PageMask, EntryHi, EntryLo0, EntryLo1) by TLBR, restoring Index and
  EntryHi afterwards. → `tlb-poweron`, `cop0-lladdr-poweron`, `cop0-undefined-poweron`
- Then, from `main`: the MI, VI, AI, PI, RI, SI and SP registers, the RDRAM device registers
  of every module IPL3 configured (→ `rdram-manufacturer`, `rdram-cc`, `rdram-cc-auto`), and the
  boot flags (memory size, TV type, reset type).

Stage 2 is the same capture after a soft reset. In addition, it records ErrorEPC (where the
NMI landed in stage 1's known wait loop), the Count at the pre-NMI interrupt and the last
Count the wait loop stored before the NMI (→ pre-NMI-to-NMI time), and whether the TLB pattern
stage 1 wrote before waiting survived the reset. libdragon's IPL3 clears RDRAM above 0x400 on a
warm boot (`ipl3.c`), so stage 1 parks these words at 0xA0000380-0xA00003FF.

### Uncached-read loop timing [0x30-0x3F, 0x40] → `cpu-timing-residual`, `rcp-reg-read`, `mi-reg-read`

The krom TIMINGNTSC loop with Count captured every iteration:

```
loop: <op>
      lw    t6, REG(t3)
      sync
      mfc0  t0, Count
      addiu a0, a0, 1
      bne   a0, a1, loop
      sb    t0, -1(a0)       # low byte of Count; deltas are taken mod 256 on decode
```

The buffer is 8 KiB-aligned and pre-loaded into the D-cache, so every SB hits. `<op>` is
each of ALU (`add t1,t2`), MUL.S of 0.0 (one PCycle longer, krom `MULS`), ADD.S, MULT, DMULT,
DIV and DDIV, with krom's operands. `REG` is each of VI_V_CURRENT, MI_VERSION, SP_STATUS and
PI_STATUS. The VI mode is each of: off (VI_CONTROL type 0), 320x240 16 bpp progressive, and
640x480 32 bpp interlaced (krom's configuration). Each VI_V_CURRENT run keeps 4096 samples;
each run reading another register keeps 256. Series id `0x3MRO`: M = VI mode, R = register,
O = op.

A second loop counts iterations per VI_V_CURRENT value for one whole frame. It is krom's
exact loop body, with a store only when the line changes: series `0x40MO` (M = 1, 2). Summed
over half-lines 0-0x1FF, it reproduces krom's counts.

### Exceptions [0x50] → `exception-redirect-cycle`

Count across SYSCALL, ADD overflow, an instruction-fetch address error (JR to an odd word)
and BREAK, each with a 4-instruction handler (`mtc0 k1, EPC`, two NOPs, `eret`). The
baseline is the same code with the faulting instruction replaced by a NOP and the handler's
four instructions inlined. 64 repetitions each.

Interrupt acceptance (manual 4.7.1): Compare is set to Count + k for k = 2..65, then a sled
runs: NOPs, uncached LW/NOP pairs, or DIVs. The handler records EPC (which sled slot was
interrupted) and Count at entry.

### Micro-TLB [0x51] → `itlb-invalidation`

A code block that straddles the even/odd pages of one mapped TLB entry is timed after: nothing;
TLBWI to an unrelated index; TLBWI of the same entry; an EntryHi write with the same ASID;
EntryHi with another ASID and back; TLBP; TLBR. A three-page block that must miss in the
2-entry ITLB gives the refill cost.

### CP0 hazards [0x52] → `cp0-bypass-interlock`

MTC0 then MFC0 of EntryLo0, with 0, 1, 2 and 3 NOPs between, unrolled 16 times. The record
holds the time and the value each MFC0 read (old or new), the same for WatchLo, and MTC0
Count then MFC0 Count.

### Pipeline overlap [0x53] (ADR 0005 rules)

- MULT and DIV right after a load that misses the D-cache: independent of the load, and
  dependent on it.
- MULT as the last word of an I-cache line whose next line is cold, and the same with a warm
  next line and with a NOP instead of the MULT.
- Branch-likely not taken (BEQL ×16) against 16 NOPs and 16 taken BEQs.
- ERET with no exception (EXL set by MTC0) against `j` + NOP.

### CPU quirks [0x54] → `cause-ce-next-instr`, `jump-region-in-delay-slot`, `cop1-cvt-from-l`, `cache-op-icache`, `tlb-shutdown`

- Cause after SYSCALL, ADD overflow and a CTC1-raised FPE, each followed by MFC1, MFC2, LWC1,
  MTC0 or NOP.
- JR into the uncached alias (0xA...) with a J in its delay slot, and the reverse (0x8 from
  0xA). The target's code reports through BAL which region it ran in.
- CVT.S.L and CVT.D.L of ±(2^53, 2^55 - 1, 2^55, 2^55 + 1, 2^62, 2^63 - 1): FCSR, whether
  an exception was taken, and the result.
- 64 CACHE ops of each kind (I: Index_Invalidate, Hit_Invalidate, Fill; D:
  Index_Writeback_Invalidate, Hit_Invalidate, Hit_Writeback on clean and dirty lines) against
  64 NOPs.
- TLB shutdown, run last because it may disable the TLB until reset. Two TLBWIs of the same
  VPN2 with V = 1 (stage 1) or V = 0 (stage 2): Status after each write, and Status, Cause and
  BadVAddr after a load from the page.

### RDRAM access patterns [0x60] → `arb-row-miss`, `arb-row-miss-dirty`, `arb-ri-overhead`

256 uncached LWs at strides 0, 4, 8 ... 8 KiB, 1 MiB and 2 MiB. The same for cached line
fills (each line Hit_Invalidated first), for line fills that evict a dirty line, and for
uncached SWs drained by SYNC and an uncached load. 8 repetitions, each with VI off and on.

### PI [0x70] → `pi-address-phase`, `pi-busy-writes`

ROM-to-RDRAM DMA time for 2 B to 8 KiB under domain 1 timings from fast (LAT 5, PWD 12, RLS 2)
to slow (LAT 0xFF, PWD 0xFF). Register behaviour of writes to DRAM_ADDR, CART_ADDR, WR_LEN and
STATUS while busy, and an IO read of ROM during a DMA. RD_LEN is left out, because if the PI
accepted it the write would reach cartridge ROM space, which a flashcart may let through to
the image a soft reset boots from.

### SI and AI [0x71, 0x72] → `si-pif`, `ai-fetch`, `ai-status-count`

SI DMA to and from PIF RAM with a controller-status command block, 32 repetitions, plus the
controller-presence bytes, so that the emulator run can be configured the same. AI_LEN and
AI_STATUS sampled 512 times at a fixed Count spacing while a 4 KiB buffer plays silence.

### Instruction retirement [0x56] → SPEC probe 1

32 copies of one instruction per class, 59 classes: integer ALU and shifts, every multiply and
divide, cached loads and stores (with a dependent use), branches taken and not taken, branch
likely, jumps, CP0 and HI/LO moves, every FPU arithmetic and convert class (independent and
dependent), FPU loads, stores and moves, uncached loads and stores, and SYNC.

### Cache and TLB refill [0x57] → SPEC probe 2

32 I-cache line misses against 32 hits, 32 D-cache line misses against hits, and a load that
takes a TLB refill, one that hits, and one that finds an invalid entry (minimal handlers at
0x000, 0x080 and 0x180), plus the Cause each raised.

### Register latency [0x58] → `rcp-reg-read`, `mi-reg-read`, `rcp-reg-write`

64 back-to-back uncached loads from each of 21 addresses (every RCP block, SP DMEM and IMEM,
the DP registers, PIF RAM, cartridge ROM, RDRAM and the RDRAM registers). The same with stores
to 9 registers whose writes have no effect beyond clearing an unused interrupt.

### RSP [0x73, 0x74] → SPEC probe 3

CPU-initiated SP DMA in both directions, for lengths 8 B to 4 KiB, three count/skip shapes, and
IMEM. A microcode (`rsp_probe.S`) of 17 timed cases (scalar ALU, load-use, store after load,
branches, independent and dependent vector ops, dual issue, vector loads and stores, MFC2,
multiply-accumulate, reciprocal/square root, flag moves, DMEM access), and RSP-initiated DMA
at four lengths. Each case runs 256 iterations, and the CPU times it from clearing HALT to the
BREAK.

### RDP [0x75] → SPEC probe 5

Command lists built on the CPU and sent from RDRAM, each timed to the full-sync interrupt and
with DP_CLOCK, BUFBUSY, PIPEBUSY and TMEM read afterwards. The 13 configurations: setup only;
fill rectangles at three sizes in 16 bpp and one in 32 bpp; tile loads; copy-mode,
1-cycle, 1-cycle with Z, 1-cycle blended and 2-cycle textured rectangles; and flat triangles at
two sizes. The triangle encoding is unverified: Ultra has no RDP yet.

### RDRAM contention and line length [0x61, 0x62] → SPEC probes 4 and 6

The uncached load loop at four strides while idle, while the RSP streams 3 KiB DMAs, and while
the RDP fills the screen. The same loop with VI_H_SYNC set to four line lengths, since RDRAM
refresh runs once per line.

### FlashRAM [0x7F] → `flash-op-time`

The erase time and each page program time of the record writer itself.

## Emulator run

`ultra_harness hwprobe ROM SAV` runs stage 1 headless (about 5 s emulated, about a second of
wall time) and writes the emulated FlashRAM. Ultra does not execute RSP microcode or RDP
commands yet, so those runs record `HWP_TIMEOUT`; every wait on them is bounded. ctest `hwprobe-emulated` checks that the record is
complete and every CRC holds. Ultra has no reset button yet, so stage 2 has not run in the
emulator. Ultra starts with no controllers attached. Before comparing SI timings, configure it
like the console in `S_SI_RESULT`.

## Not covered

`boot-rom-timing` (needs our own IPL3), `mi-ebus` (the EBus test mode is not documented
well enough to drive safely), PAL timing (needs a PAL console), RDP pixel output (Angrylion is
the oracle for that), and the bus-capture residuals (logic analyzer).
