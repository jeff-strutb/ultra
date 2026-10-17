# RDRAM, RI and the arbiter

Facts used by `src/memory/`. Sources: **[n64brew]** pages "RDRAM", "RDRAM Interface", "MIPS
Interface", "Memory map", "Clock Timing"; **[st]** n64-systemtest. Timing parameters and their
confidence are in `memory/arbiter_params.h` and `docs/timing/ledger.md`.

## Devices

| Fact | Source |
|---|---|
| Retail consoles carry 2 MiB devices (two on the motherboard, two on the Jumper/Expansion Pak); DeviceType reads 0xB4190010 (ColumnBits = 11, i.e. 2 KiB rows) | [n64brew] "RDRAM Interface" hardware dump |
| Each byte has a 9th bit; CPU writes set it to the sign of the byte unless the writer supplies it | [n64brew] "MIPS Interface", MI_MODE ("EBus appears to default to the sign extension of each byte") |
| Reset values: Delay = AckWin 4, Read 1 (encoded), Ack 0, Write 4; Mode = CE, X2, AS, CC all ones | [n64brew] "RDRAM", Delay / Mode default columns |
| DeviceId IdField bits are scattered across the register (IdField[25:20] at 31:26, [26] at 23, [34:27] at 15:8, [35] at 7); 2 MiB devices ignore Adr[20] | [n64brew] "RDRAM", 0x01 DeviceId |
| Only enabled devices (Mode.DE) answer memory accesses; an id match selects the device | [n64brew] "RDRAM" |
| Register writes: enabled devices respond on an id match; the first not-yet-enabled device whose SIn is high also responds (SIn of device n = enabled(n − 1); device 0's is tied high). Broadcast writes (address bit 19) reach every device with the same packet | [n64brew] "RDRAM", Reset and initialization |
| Mode reads back X2 inverted and, in manual mode, CC inverted; the automatic-mode CC readback is modelled as the programmed value inverted | [n64brew] "RDRAM", 0x03 Mode; ledger `rdram-cc-auto` |
| Output current is proportional to (63 − CC); below a threshold the device's '1' bits do not survive the channel | [n64brew] "RDRAM", Mode C[5:0]; threshold ledger `rdram-cc` |
| The RI's write delay is hardwired to 1 TCycle and it samples reads at ReadDelay 7; a device whose WriteDelay is larger samples the data packet late, one whose ReadDelay differs drives data the RI samples early or late (2 bytes per TCycle) | [n64brew] "RDRAM", Reset Complications |
| Bursts wrap within the 2 KiB row | [st] `mi/repeat.rs` "MI Repeat: Wrap2KiB" |
| MinInterval reads a fixed pattern (bits 31/23/15/7, 30/22/14/6, 29/21/13/5 per default columns) | [n64brew] "RDRAM", 0x07 MinInterval |

## RI

| Fact | Source |
|---|---|
| Registers mirror with mask 0x001FFFE0 | [n64brew] "Memory map" |
| Memory space 0x0000_0000-0x03EF_FFFF; register space 0x03F0_0000-0x03FF_FFFF with Adr[28:20] = (address >> 10) & 0x1FF and register = address & 0x3FF; address bit 19 selects broadcast | [n64brew] "RDRAM Interface", Memory addressing table |
| Accesses at or above 8 MiB in memory space set RI_ERROR.OverRange; an access no device answers sets MissingAck | [n64brew] "RDRAM Interface", RI_ERROR |
| RI_CURRENT_LOAD reads back a collection of bits from other registers | [n64brew] "RDRAM Interface", RI_CURRENT_LOAD READ |
| RI_LATENCY defaults to 0xF | [n64brew] "RDRAM Interface" |
| Bank status tracks one open row per 1 MiB bank for the bottom 8 MiB; any write to RI_BANK_STATUS invalidates all banks and marks them dirty | [n64brew] "RDRAM Interface", Bank Status Tracking, RI_BANK_STATUS |
| Refresh is triggered by VI HSYNC, also while the VI is off | [n64brew] "RDRAM Interface", RI_REFRESH Extra Details |

## MI data path (CPU to RDRAM)

| Fact | Source |
|---|---|
| The RDRAM data bus is 64 bits; a 32-bit CPU read returns the half selected by Addr[2], or the upper half in Upper mode | [n64brew] "MIPS Interface", MI's Mode |
| Repeat mode: the first 64-bit value on the bus becomes the pattern (a word is duplicated into both halves; a cache block contributes its last 8 bytes); RepeatCount + 1 bytes are counted from the octbyte-aligned base and bytes [Addr[2:0], RepeatCount + 1) of the pattern stream are written; Repeat clears after one write | [n64brew] "MIPS Interface", Repeat Mode; [st] `mi/repeat.rs` |
| RDRAM register writes always deliver the written word in bytes 0-3 of the data packet, whatever the address's word select | IPL3 evidence (`docs/hardware/pif.md`: IPL3 configures odd-numbered registers such as DeviceId and Mode with plain SW and the devices must see the value) |
| EBus mode bit placement (9th bits in the low nibble) | Guess, ledger `mi-ebus` |

## Arbiter (v1)

See ADR 0004. Request packet 3 TCycles, data 2 bytes per TCycle, 4 TCycles per MClock tick,
WriteDelay 1, ReadDelay 7, bursts up to 128 bytes (Derived, [n64brew] "RDRAM", "RDRAM
Interface", "Clock Timing"). Row-miss, dirty-row, RI-overhead and refresh costs are Guess.
