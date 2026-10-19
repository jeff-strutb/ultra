# hwprobe: the gold-standard console ROM

`tests/probes/hwprobe/` builds one libdragon ROM that measures, on a real N64, every ledger
entry a CPU-side probe can settle, and writes the raw results to the cartridge's FlashRAM.
The flashcart stores FlashRAM as a 128 KiB `.sav` file, which `tools/hwprobe.py` decodes. The
same ROM runs headless in Ultra (`ultra_harness hwprobe`), which dumps the FlashRAM it
emulated, and `tools/hwprobe.py compare` diffs the two records series by series.

The oracle is the console's record. Ultra's record is never an oracle. hwprobe is the
project's gold standard: a new hardware unknown gets a series here, and console records are
committed under `tests/traces/hwprobe/` (see the README there).

## Why FlashRAM

- 128 KiB holds the raw per-iteration samples (stage 1 uses about 100 KiB of its 112 KiB) the `cpu-timing-residual` question needs; 32 KiB
  SRAM does not.
- Every flashcart emulates it, real carts use it, and its protocol is documented (n64brew
  "Flash"). 1 Mbit SRAM is also offered by flashcarts, but its address map is not documented.
- The ROM declares it through the Advanced Homebrew ROM Header (`N64_ROM_SAVETYPE = flashram`,
  byte 0x3F = 0x50, n64brew "ROM Header"), so the flashcart configures it without a database.

## Running it

No controller is needed and nothing is chosen on screen. RESET is the only control.

1. Copy `tests/probes/hwprobe/hwprobe.z64` to the flashcart and boot it. Stage 1 measures and
   writes its record (about a minute, most of it writing the SD card), then shows `STAGE 1 DONE. PRESS RESET.`
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

**Cold boot: `hwprobe-cold.z64`.** The same program behind libdragon's IPL3 patched
(`tests/probes/hwprobe/ipl3/cold-capture.patch`) to capture the CPU and RCP state at IPL3's
first instruction, before IPL3 changes anything (series `S_BOOT_COLD`). Run it with SC64
direct boot, so no menu code runs first:

```bash
sc64deployer upload --direct --save-type flashram tests/probes/hwprobe/hwprobe-cold.z64
```

No retail CIC accepts the patched IPL3. In direct-boot mode the SC64 programs its CIC emulation
with the checksum of the ROM's own IPL3 (SummerCart64 `sw/deployer`, `sign_ipl3` and
`CIC_PARAMS_SET`), so it boots. A flashcart menu or a stock cart halts it at the IPL2 check.

The capture holds:
- GPRs and HI/LO as IPL2 left them;
- every COP0 register, including Count (so the IPL1 + IPL2 run time, `boot-rom-timing`),
  Cause, Compare, WatchLo and Random, and the upper words of the 64-bit ones;
- FCR0 and FCR31;
- RI, MI, SP_STATUS, PI domain 1 timing, VI and SI as the PIF and IPL2 leave them.

It runs from DMEM and stashes the capture there. Once RDRAM is up, IPL3 copies it to RDRAM
0x200-0x37F, which IPL3's memory clear and the ELF loader both leave alone. After a RESET it
captures again, so stage 2 also gets the state IPL3 receives after an NMI (`S_BOOT2_COLD`).

`hwprobe.z64` (libdragon's stock IPL3) boots from any cart. For it, the TLB, LLAddr, EPC,
ErrorEPC, BadVAddr, Context, XContext and Status at program entry are still the power-on
values, and so are Cause's BD, CE and ExcCode fields: only its IP1:0 bits are writable. Count,
Compare and WatchLo are not; IPL3 zeroes them.

## Full record

Every series also goes to a full record with no size limit (`sink.c`). On a flashcart with
an SD card (SC64, EverDrive-64 X7, 64drive), the files are `ultra_hwprobe/stage1.bin` and
`stage2.bin`. In an emulator with an ISViewer port, the record streams as `HWPX` hex lines
that `ultra_harness hwprobe-full` collects into the same file. The full record holds what
FlashRAM cannot: the CPU functional sweeps and 4096 samples for every loop run, the RDP renders and the RSP sweeps (about 1.7 MB in
all). Its header has the FlashRAM layout below, with magic `ULTRAHF1` or `ULTRAHF2`.
`tools/hwprobe.py` reads either form.

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

### RSP functional sweep [0x90] → the Phase 1 RSP oracle

What every RSP instruction computes, not how long it takes. The results are megabytes, so they
go only to the full record (`rec_series_full`, series 0x9000-0x9FFF in `series.h`). FlashRAM
gains only this group's `S_GROUP_TIME` entry.

The microcode is `rsp_func_ucode.S`. (libdragon builds any `.S` whose name starts with `rsp` as
RSP code. Its object would be `rsp_func.o`, the same name as the C driver's, so the microcode
cannot be called `rsp_func.S`.) Each of its four entries runs a fixed frame around a 4-word
IMEM slot. While the RSP is halted, the CPU writes the instruction under test into that slot
and the inputs into DMEM. It then starts the entry and reads the outputs after the BREAK.
Every encoding is built in `rsp_func.c`. They match libdragon's assembler bit for bit (checked
for each instruction form). The CPU never waits longer than 5 ms per run. After 4 runs in a
row that miss the BREAK, the group stops starting the RSP. DMEM 0x000-0x03F and 0xFC0-0xFFF
hold no microcode data, so that the wraparound tests can store there.

- **Vector compute** (`S_RSPF_VOP` 0x91ff, one series per funct f = 0x00-0x3F, the undefined
  ones included). Each funct runs as `vd=$v03 vs=$v01 vt=$v02`. It runs over 256 cases with
  e = 0, then over cases 128-143 with each of e = 1-15. Cases 0-127 pair each of 16 edge
  values (0, ±1, 0x7FFF, 0x8000, 0x8001, 0x4000, 0xC000, 0x00FF, 0xFF00, 0x0100, 0x5555,
  0xAAAA, 0x7FFE, 2, 0xFFFE) with every other, under four accumulator values per lane. Cases
  128-255 are xorshift-random. VCO, VCC and VCE are set per case with CTC2. Before each case,
  the frame runs `vrcp $v07.e0, $v00.e0` (a fixed divide state). It then preloads the
  accumulator with VMUDH and VMADH ×3 of H' × 0x4000, VMADH of M × 1, and VMADN of L × 1,
  which gives (H << 32) | (M << 16) | L when H' = H + M[15]. Each case records vd, the three
  accumulator slices (VSAR 8, 9, 10) and VCO/VCC/VCE. The inputs are `S_RSPF_VCASES`. The
  VNOP series (f = 0x37) shows each case's preloaded accumulator and flags as the console
  holds them.
- **Divide tables**. VRCP and VRSQ run on every 16-bit input (`S_RSPF_RCP`, `S_RSPF_RSQ`, 16
  chunks of 4096), each followed by VRCPH/VRSQH of $v00 for the high half. That covers all
  512 entries of both ROM tables at every shift. The double-precision path is VRCPH (VRSQH)
  of the high half, VRCPL (VRSQL) of the low half, then VRCPH (VRSQH) of $v00. It runs on
  16384 inputs: both signs, shifts 0, 1, 6, 7, 14, 15, 20 and 21 (both parities, for the
  square root), low bits all 0 or all 1, and every table index (`S_RSPF_RCPD`, `S_RSPF_RSQD`,
  stored as input/result pairs; the 1024 shift-0 inputs with low bits set repeat the shift-0
  ones). There are 32 more special inputs (`*_SPECIAL`). The divide state carries from one
  input to the next in the order listed, which matters when emulating it.
- **Vector loads and stores** (`S_RSPF_LS` 0x93SO). LWC2 and SWC2 opcodes 0-11 (lbv lsv llv
  ldv lqv lrv lpv luv lhv lfv lwv ltv, and sbv … stv) on $v08, at base 0x230 + off and
  0xFF0 + off (across the end of DMEM), for every off 0-15 and element 0-15. $v08-$v15 start
  with byte j of $vR = (R << 4) | j. The DMEM windows hold 0x40 + i. Loads record $v08, or
  $v08-$v15 for lwv and ltv. Stores record the 64-byte window.
- **Scalar DMEM rules** (`S_RSPF_SCALAR`). lb lh lw lbu lhu lwu sb sh sw at 0x230-0x237,
  0xFF9-0xFFF and three addresses with bits above 11 set. Each records the loaded register and
  DMEM around both ends. lwu is not an RSP instruction (libdragon's assembler rejects it), so
  this records what the console does with that encoding.
- **COP2 moves** (`S_RSPF_MOVE`). MFC2 and MTC2 at every byte element 0-15, CTC2 of all ones to
  control registers 0-3, and CFC2 of 0-3.
- **COP0** (`S_RSPF_COP0`). MFC0 of all 16 registers from the RSP, the semaphore taken and
  released, each signal set and cleared from the RSP with SP_STATUS read back, then SP_STATUS
  and the semaphore as the CPU sees them afterwards.

On the console the group makes about 15,000 RSP runs and writes about 3.8 MB. Ultra has no
RSP core yet: the first runs miss the BREAK, the group skips the rest, and the sweep series
have no elements (`S_RSPF_INFO` says so). The harness drains ISViewer output as it arrives,
so the full stream is collected whatever its size.

### RDRAM contention and line length [0x61, 0x62] → SPEC probes 4 and 6

The uncached load loop at four strides while idle, while the RSP streams 3 KiB DMAs, and while
the RDP fills the screen. The same loop with VI_H_SYNC set to four line lengths, since RDRAM
refresh runs once per line.

### CPU functional [0xA0] (full record only)

Results rather than timing, from instructions built at runtime so that every encoding of a
class can run:

- **Opcode sweep:** every primary opcode except J/JAL, every SPECIAL function, every REGIMM
  rt, the COP0 moves (except the ones that write registers) and functions, every COP1 format
  and function, and the COP2 and COP3 moves, all with register fields 0. Before it runs, all
  32 TLB entries are set invalid, so that zero-base loads, stores and CACHE ops raise TLB
  exceptions instead of reaching memory. Recorded: Cause, EPC and BadVAddr per encoding.
- **FPU:** add, sub, mul and div in S and D, the unary and convert functions (including from W
  and L), and all 16 compare conditions. Each runs over 16 edge operands (zeros, ±1, the
  smallest and largest denormals, the smallest normal, ±max, ±Inf, both NaN kinds, 1.5, 3,
  1/3), in all four rounding modes with flush-to-zero off and on. Recorded: result, FCSR, and
  any exception.
- **Integer:** every SPECIAL ALU, variable shift, multiply and divide over 16 x 16 operands,
  including values that are not sign-extended 32-bit (undefined for 32-bit ops), and the
  immediate shifts for every sa.
- **Memory:** LWL/LWR/LDL/LDR/SWL/SWR/SDL/SDR at every offset; LL/SC around stores and ERET;
  cache tags after loads, stores and invalidations.

Headless, every directly computable FPU result matches IEEE arithmetic. Divisions with a
denormal result trap with Unimplemented, as the VR4300 does without flush-to-zero.
### RDP render matrix [0x80, series 0x8000-0x8FFF] → RDP raster oracle

`rdp_render.c`. 121 small renders whose hardware pixels are the RDP raster oracle alongside
Angrylion. Everything goes to the full record only (SD card or ISViewer), never to FlashRAM: the
group is skipped when no full-record sink is open, and its only FlashRAM footprint is its row
in `S_GROUP_TIME`.

Every case draws into one 32 x 32 colour image at RDRAM 0x00100000 (8, 16 or 32 bpp) with its
Z image at 0x00110000. Before each case the CPU writes a hashed colour pattern and a Z ramp
across x that covers every Z exponent (`S_RDPR_INIT*`), so blends, memory-coverage reads and Z
compares see varied memory. Each command list starts by setting the colour and Z images, every
colour register, the key and convert registers, primitive depth and all eight tile descriptors
to fixed values, and by Load Block of 4 KiB of zeros into TMEM. No case depends on an earlier
one. The list is sent from RDRAM, ends in sync pipe and sync full, and is waited on for 20 ms
at most.

Per case n the record holds the colour buffer (`0x8000 | n`), the Z buffer for the cases that
use Z (`0x8400 | n`), and the exact command words sent (`0x8800 | n`). `S_RDPR_INFO` gives each
case's depth, flags, time to the full-sync interrupt, DPC_STATUS and DPC_CLOCK. The 18 CPU-generated
textures are recorded as loaded (`0x8C00 | k`, `S_RDPR_TEXINFO`): RGBA16, RGBA32, IA4, IA8,
IA16, I4, I8, CI4, CI8, a 16- and a 256-entry TLUT, YUV16, and a 32x32-to-1x1 RGBA16 mip chain.
They mix s and t gradients (visible through filtering) with hashed low bits, so every bit of
every format is exercised.

| Cases | What |
|---|---|
| 0-4 | FILL: 16, 32 and 8 bpp; subpixel corners; scissor |
| 5-10 | COPY: RGBA16, alpha compare, CI8 through a TLUT, I8 into 8 bpp, odd x, t step 0.5 |
| 11-24 | 1-cycle point-sampled textures in every format and TLUT type (RGBA16, IA16, off), YUV with conversion, Load Block, a format reinterpreted through another tile |
| 25-32 | point, bilinear and median filtering at fractional texel offsets |
| 33-44 | wrap, mirror, clamp, clamp + mirror, masks, shifts, tile-size offsets, flipped and negative-step rectangles, subpixel rectangles |
| 45-57 | combiner: lerps, overflow and underflow, key centre and scale, chroma key, K4/K5, primitive LOD fraction, noise (twice, for repeatability), alpha formula at 32 bpp, 2-cycle TEX0 x TEX1 and the second-cycle TEX0 |
| 58-61 | alpha compare against blend alpha and against noise, 1- and 2-cycle |
| 62-70 | RGB dither (magic square, Bayer, noise, off) and alpha dither (same, inverted, noise) through a memory blend, 16 and 32 bpp |
| 71-78 | blender: alpha blend, fog, additive, blend colour, memory coverage, no force-blend, 2-cycle fog then blend |
| 79-88 | coverage: AA off and on, coverage destination clamp, wrap, full and save, colour on overflow, coverage x alpha, 32 bpp, AA rectangles |
| 89-98 | Z: compare and update, compare only, update only, interpenetrating, transparent, 32 bpp, primitive Z with two delta-Z values, decal, steep against shallow delta-Z, 2-cycle |
| 99-108 | triangles: shaded (16, 32 bpp), textured point and bilinear, perspective, shade x texture with Z, right-major, sliver, off-scissor on every side, flat top and bottom |
| 109-116 | LOD: 1x, 2x, 4x minification, perspective, 2-cycle trilinear, sharpen, detail, LOD on a rectangle |
| 117-118 | scissor field mode, even and odd lines |
| 119-120 | hazards n64brew lists as unknown, last so that a hang cannot cost the others: I4 copied into 8 bpp, 1-cycle into 8 bpp |

Command encodings come from n64brew "Reality Display Processor/Commands"; other-mode,
combiner and blender fields from libdragon's `rdpq_macros.h`. Triangle coefficients are
computed on the CPU from three vertices with 64-bit integer arithmetic (edges in s15.16,
shade and Z in s15.16, texture coordinates in s10.21). Ultra has no RDP yet, so the encodings
have been checked only offline: decoding each recorded triangle recovers its vertices and their
colours, every command stream parses to its exact length, and the textures match an
independent re-derivation. In the emulator every case times out and every buffer equals its
initial pattern.

Not captured: the 16 bpp hidden (9th-bit) coverage bits, which the CPU cannot read; the
memory-coverage blender cases show them indirectly.
### Peripherals [0xB0] → `si-pif`, `ai-fetch`, `ai-status-count`; oracle for SI, PIF and accessories

`peripherals.c`. Everything goes through raw SI DMA to PIF RAM (64-byte write, then the
64-byte read during which the PIF runs the frame, n64brew "PIF-NUS"). Every SI and AI wait is
bounded (10 ms per SI DMA), so a missing device or a stuck PIF ends the group, not the run.
Plug in whatever should be documented before booting: the first series records what answered.

- **Joybus commands, one per frame.** On each controller port: info (0x00), read buttons
  (0x01), read accessory (0x02) at every address below, the same read with its address check
  bits wrong followed by info (status bit 2 reports the CRC error), twelve unknown command
  bytes (0x04 0x06 0x07 0x09 0x10 0x14 0x20 0x41 0x7F 0x80 0xA5 0xFE), reset (0xFF), info. On
  the cartridge port (channel 4): info, reset, EEPROM read of blocks 0 and 1 (0x04), RTC info
  (0x06), RTC read of blocks 0-2 (0x07), unknown 0x01 0x09 0x7F. The full record keeps the
  frame written, PIF RAM read back (rx error bits included), both DMA times and SI_STATUS;
  FlashRAM keeps the rx byte as returned, the first three response bytes and the read time.
- **Accessory identification, read only.** Addresses 0x0000-0x00E0 (Controller Pak page 0:
  ID and label), 0x8000 (Rumble/Transfer Pak identification and power), 0xA000 and 0xB000
  (Transfer Pak bank and status), 0xC000 and 0xE000 (Rumble motor, Game Boy window), with the
  5-bit address check (x^5 + x^4 + x^2 + 1 over address bits 15:5, n64brew "Joybus Protocol";
  checked in Python against polynomial division). The usual Rumble Pak identification writes
  0x80 to 0x8000 first; that write is left out, so a Rumble Pak's reads document its
  uninitialised state.
- **Timing spread.** Info and buttons on channels 0-4, 16 times each (full record).
- **AI FIFO.** 2 KiB buffers of silence at 44.1 kHz with the DMA enabled. Three runs: A, B and
  a third buffer C queued at once (both FIFO slots, and a write while full); B queued when
  AI_LEN first reads below half of A; B queued 2 ms after the AI goes idle (underrun and
  restart). Each run logs every change of (AI_LEN, AI_STATUS, MI_INTR AI bit) with Count, and
  clears the AI interrupt whenever it is seen, so each rise is a separate event. FlashRAM keeps
  AI_STATUS/AI_LEN right after each queueing and the first 12 status or interrupt changes.
- **PIF RAM frame edge cases**, last because the PIF may handle them badly: control byte 0;
  0x00 skips (leading and between commands); 0xFF pads; 0xFD first and between commands; a
  command after 0xFE; four commands with no 0xFE (rest 0x00, rest 0xFF); six commands (channel
  limit); tx bytes 0x81, 0x41, 0xC1, 0x40; rx bytes preset 0x83 and 0x43; rx 1, 8 and 0 for
  info; tx 3 and tx 32 for info; rx 32 for info; all 0xFF; all 0x00 with control 1 and with
  control 0; then a normal four-port frame. Each frame is read back twice with no write
  between, so the record shows whether a second read runs the frame again.

Safety: no command that writes is ever sent (0x03, 0x05, 0x08, 0x0A-0x0D, 0x13, 0x15 are
excluded), so a Controller Pak, EEPROM or RTC is never written, and the PIF control byte is
only ever 0x00 or 0x01. Oversize lengths stop at 32 bytes so no response can reach the control
byte. In Ultra no device is attached: every handshake returns rx bit 7.

### FlashRAM [0x7F] → `flash-op-time`

The erase time and each page program time of the record writer itself.

## Emulator run

`ultra_harness hwprobe ROM SAV` runs stage 1 headless (about 5 s emulated, about a second of
wall time) and writes the emulated FlashRAM. Ultra does not execute RSP microcode or RDP
commands yet, so those runs record `HWP_TIMEOUT`; every wait on them is bounded. With
`--stage2` the harness then presses RESET once the stage 1 wait loop is running (its iteration
count at 0xA0000390 is nonzero) and runs on until stage 2 prints its record. ctest
`hwprobe-emulated` runs both stages and `hwprobe-record` (`hwprobe.py dump SAV --stages 2`)
checks that both records are complete and every CRC holds. `ultra_harness hwprobe-full ROM OUT
[--direct]` writes the full record; `--direct` boots as SC64 direct boot does, so
`hwprobe-cold.z64` runs too (ctest `hwprobe-cold`). Ultra starts with no controllers attached. Before comparing SI timings, configure it
like the console in `S_SI_RESULT`.

## Not covered

`mi-ebus` (the EBus test mode is not documented
well enough to drive safely), PAL timing (needs a PAL console), RDP pixel output beyond the
render matrix (Angrylion is the oracle for the rest), and the bus-capture residuals (logic
analyzer).
