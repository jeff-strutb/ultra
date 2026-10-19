# MI, PI, SI, VI, AI

Facts used by `src/io/`. Sources: **[n64brew]** pages "MIPS Interface", "Parallel Interface",
"Serial Interface", "Video Interface", "Audio Interface", "Memory map", "Clock Timing", "ROM
Header"; **[st]** n64-systemtest. Phase 0 scope: MI, PI (ROM), SI/PIF word and DMA paths; VI and
AI only as far as boot and the test ROMs need them.

## MI

| Fact | Source |
|---|---|
| MI_VERSION reads 0x02020102 on most consoles | [n64brew] "MIPS Interface", MI_VERSION |
| Only address bits 3:2 are decoded; the registers mirror | [n64brew] "MIPS Interface" |
| MI_MASK and MI_MODE use (clear, set) bit pairs; both set leaves the bit unchanged | [n64brew] "MIPS Interface", MI_MASK, MI_MODE |
| The CPU's Int0 is (MI_INTR & MI_MASK) != 0 | [n64brew] "MIPS Interface" |
| Data path, Upper and Repeat modes | `docs/hardware/memory.md` |

## PI

| Fact | Source |
|---|---|
| Domain 2 covers 0x05000000-0x05FFFFFF and 0x08000000-0x0FFFFFFF; everything else is domain 1 | [n64brew] "Parallel Interface", Domains |
| Bus timing per 16-bit word: address phase, LAT + 1 cycles, then PWD + 1 cycles strobe and RLS + 1 release per word; a new address phase at every 2^(PGS+2)-byte page | [n64brew] "Parallel Interface" register descriptions; address-phase length is a Guess (`pi-address-phase`) |
| Open bus reads return the low 16 bits of the address | [n64brew] "Parallel Interface", Open bus |
| CART_ADDR bit 0 and DRAM_ADDR bit 0 read as zero (masks 0xFFFFFFFE / 0x00FFFFFE) | [st] `cart_memory` "cart address (masking)" |
| PI_RD_LEN / PI_WR_LEN read 0x7F, or 0x7F minus the RDRAM misalignment after a transfer shorter than 8 bytes | [n64brew] "Parallel Interface"; [st] `cart_memory/dma.rs` |
| PI_STATUS write bit 0 resets the DMA controller, bit 1 clears the interrupt; reads report DMA busy, IO busy, error, interrupt | [n64brew] "Parallel Interface", PI_STATUS |
| A DMA raises MI PI on completion | [n64brew] "Parallel Interface" |

### PI DMA block rules (cart to RDRAM)

Fitted to n64-systemtest's hardware-measured cases (`src/tests/cart_memory/dma.rs`: small
sizes, misaligned, misaligned crossing a page, misaligned at the end of a page) together with
the 128-byte internal buffer and the rule that a block never crosses a 2 KiB RDRAM page
([n64brew] "Parallel Interface", DMA Transfers):

- The buffer is filled from index `DRAM_ADDR & 7` for the first block, until a block extends
  past the buffer's first octword; later blocks start at index 0.
- A block is limited by the remaining length, the RDRAM page and the buffer end.
- Odd lengths round up to whole 16-bit words, except in the first block when it does not reach
  the buffer's last word.
- The write-back stores `L − (DRAM_ADDR & 7)` bytes at DRAM_ADDR; DRAM_ADDR then advances by the
  bytes written rounded up to 8, CART_ADDR by the 16-bit-rounded block length.

These rules reproduce every n64-systemtest PI case; their internal mechanism is not documented.

## FlashRAM

`io/flashram.cpp`. Present when the ROM's Advanced Homebrew Header declares save type 5
(bytes 0x3C-0x3D "ED", 0x3F bits 7:4; [n64brew] "ROM Header"), or when enabled by the host.

| Fact | Source |
|---|---|
| 128 KiB at PI 0x0800_0000 (domain 2); write-only command register (CIR) at 0x0801_0000; 8 sectors of 128 pages of 128 bytes | [n64brew] "Flash", TLDR and Chip layout |
| CIR commands by bits 31:24: 0x3C chip erase setup, 0x4B sector erase setup (page in 9:0), 0x78 erase, 0xA5 program page (page in 9:0), 0xB4 load page, 0xD2 status, 0xE1 silicon ID, 0xF0 read array | [n64brew] "Flash", Command Internal Register |
| Power-on: Read Array mode, array readable at the base | [n64brew] "Flash", TLDR |
| Status mode returns `00 <status>` for every 16-bit word; status bits 7 WSM_READY, 3 ERASE_OK, 2 PROGRAM_OK, 1 ERASE_BUSY, 0 PROGRAM_BUSY; writing 0 at offset 0 in Status mode clears the OK bits | [n64brew] "Flash", Status Mode |
| Erase without a setup command is ignored; erase and program switch the chip to Status mode | [n64brew] "Flash", Erase, Page Program |
| The page buffer starts as 0xFF, takes writes that set and clear bits (MX29L1100 behaviour), is readable, and returns to 0xFF after a program; programming ANDs it into the page | [n64brew] "Flash", Load Byte Page Mode, Page Program |
| Silicon ID 0x11118001_00C2001D (MX29L1101_A, byte-indexed) | [n64brew] "Flash", Silicon ID table; which model: ledger `flash-model` |
| Erase and program complete at once (hardware: 85-300 ms, 0.3-3.5 ms) | Guess, ledger `flash-op-time` |
| In Silicon ID mode the burst position is taken from address bits 2:0 | Guess, ledger `flash-silicon-id-burst` |

## SI and PIF window

| Fact | Source |
|---|---|
| The SI-PIF link runs at MClock/32 per bit; a 4-byte packet is 2 command + 9 address + 1 ack + 32 data bits, a 64-byte packet 2 + 9 + 1 + 512 | [n64brew] "Clock Timing", "Serial Interface" |
| CPU writes to PIF RAM are asynchronous (the CPU is released immediately); a write while another is pending is ignored; reads wait for any pending write | [n64brew] "Memory map", SI external bus |
| 64-byte DMAs and direct writes raise the SI interrupt; SI_STATUS bit 12 mirrors it | [n64brew] "Serial Interface" |
| PIF firmware processing time is not modelled | Guess, ledger `si-pif` |

## VI

| Fact | Source |
|---|---|
| A scanline lasts H_TOTAL + 1 VCLKs; VCLK/MClock = 1071/1375 (NTSC), 4965653/6250000 (PAL), 556308/715000 (MPAL) | [n64brew] "Video Interface" VI_H_TOTAL, "Clock Timing" |
| V_CURRENT counts half-lines; progressive when V_TOTAL is odd (field stays 0), interlaced when even (field alternates; the even field is one line longer when the count is odd) | [n64brew] "Video Interface", VI_V_CURRENT |
| V_INTR fires on a V_CURRENT[9:1] match; in interlaced mode with V_INTR bit 0 clear, odd-field interrupts fire one line early | [n64brew] "Video Interface", VI_V_INTR |
| With TYPE = 0 the VI is off: V_CURRENT reads 0 and no VI interrupt is generated, but HSYNC (and so RDRAM refresh) continues | [n64brew] "Video Interface", VI_V_CURRENT; "RDRAM Interface", RI_REFRESH |
| Any write to V_CURRENT acknowledges the VI interrupt | [n64brew] "Video Interface" |
| Defaults: V_INTR 0x3FF, H_TOTAL 0x7FF, BURST HSYNC_WIDTH 1 | [n64brew] "Video Interface" |

### Reference screenshots

krom's reference PNGs show the displayed framebuffer region (640 × 240 output samples scaled by
X_SCALE / Y_SCALE in 2.10 fixed point) resampled nearest-neighbour to the PNG size, without VI
filtering. `Vi::capture_presented` reproduces that convention for the test harness; it is not a
model of the VI's analog output, which arrives with the VI phase.

## AI

| Fact | Source |
|---|---|
| One stereo sample every DACRATE + 1 VCLKs | [n64brew] "Audio Interface", AI_DACRATE |
| AI_STATUS: bits 31 and 0 FULL, 30 BUSY, 25 ENABLED, 24 reads 1 | [n64brew] "Audio Interface", AI_STATUS |
| Two-entry DMA FIFO; the delayed-carry bug affects the address when a buffer crosses an 8 KiB boundary | [n64brew] "Audio Interface" |
| Fetch granularity; COUNT/BC/WC fields | Guess, ledger `ai-fetch`, `ai-status-count` |
