# PIF, CIC and the boot path

Sources: **[n64brew]** pages "PIF-NUS", "CIC-NUS", "Initial Program Load", "Serial Interface",
"Memory map"; **[spec]** `docs/hardware/cic-algorithms.md`, a clean-room functional
specification of the IPL2 checksum and the 6105 challenge, written by a separate party from
jago85/PifChecksum and jago85/UltraCIC_C (those sources are cited there, not here); **[dis]** our
own disassembly of the 6102 IPL3 (as embedded in the krom ROMs) and of libdragon's IPL3 (as
embedded in n64-systemtest).

`io/cic.cpp` and the checksum routine in `io/pif_boot_rom.cpp` were written from [spec] alone
by an implementer who did not read the jago85 sources. (Earlier versions of both mirrored
PifChecksum.c and cic_c.c statement by statement; they were removed from the repository and
its history. Phase 0 audit finding F1.)

## Boot sequence

1. Power on: the PIF runs the CIC handshake, writes the boot info into PIF RAM and sets the
   command byte's 0x80 bit; the CPU cold-resets to 0xBFC00000 ([n64brew] "PIF-NUS", Console
   startup steps 3-5).
2. **IPL1** (runs from the PIF ROM over SI, uncached): Status = 0x34000000, Config = 0x0006E463,
   wait for RSP halt, halt it and clear its interrupt, wait for SP DMA idle, reset PI
   (PI_STATUS = 3), VI_V_INTR = 0x3FF, VI_H_VIDEO = 0, VI_V_CURRENT = 0, AI_DRAM_ADDR =
   AI_LENGTH = 0, copy IPL2 to IMEM 0xA4001000 and jump there ([n64brew] "Initial Program
   Load", IPL1).
3. **IPL2** (runs from IMEM): program PI domain 1 from the ROM header word 0, read the boot
   info, lock the PIF ROM (command 0x10), copy IPL3 (ROM 0x40-0xFFF) to DMEM, compute the IPL2
   checksum over IPL3 with the CIC seed and hand it to the PIF (command 0x20 to acquire, 0x40
   to check), then jump to IPL3 at 0xA4000040 ([n64brew] "PIF-NUS", "Initial Program Load").
4. **IPL3** (the cartridge's own code) initialises RDRAM, copies the game and terminates the
   boot (command 0x08). If it does not do so within 5 seconds the PIF halts the console
   ([n64brew] "PIF-NUS", command 0x08).

Nintendo's IPL1/IPL2 are not shipped. `io/pif_boot_rom.cpp` assembles a clean-room program
that performs steps 2-3 with `mips::Assembler`. Its timing differs from the real ROM (ledger
`boot-rom-timing`).

## IPL3 entry contract ([dis])

| Register | Content |
|---|---|
| s3 | ROM type (0 = cartridge, 1 = 64DD) |
| s4 | TV type (0 = PAL, 1 = NTSC, 2 = MPAL) |
| s5 | reset type (0 = cold, 1 = NMI) |
| s6 | CIC IPL3 seed |
| s7 | version (0; ledger `ipl3-s7`) |
| sp | 0xA4001FF0 |

Both the 6102 and the libdragon IPL3 read these registers and nothing else from IPL2.
The boot-info word our PIF publishes at PIF RAM 0x24 (TV type, reset type, ROM type, seeds) is
a private contract between our PIF model and our IPL2 (ledger `pif-boot-info`).

## IPL2 checksum and CIC variants

- The checksum is a 48-bit value computed over the 0xFC0 bytes of IPL3 with a 32-bit state of
  16 words, each initialised to `(seed × 0x6C078965 + 1) XOR` the first IPL3 word ([n64brew] "PIF-NUS", IPL3 checksum algorithm;
  [spec] section 1). Verified: 0xA536C0F1D859 for both 6102 IPL3s
  (`tests/unit/test_machine.cpp`).
- Variants ([n64brew] "PIF-NUS" table): 6101 seed 0x3F checksum 0x45CC73EE317A; 6102/7101 0x3F
  0xA536C0F1D859; 7102 0x3F 0x44160EC5D9AF; 6103/7103 0x78 0x586FD4709867; 6105/7105 0x91
  0x8618A45BC2D3; 6106/7106 0x85 0x2BBAD4E6EB74.
- The emulator identifies the CIC by computing the checksum for each seed and matching the
  table, as the PIF does; PAL/NTSC pairs share an IPL3 and are split by the ROM header region.
- A checksum mismatch halts the CPU until power off ([n64brew] "PIF-NUS").

## Commands and RAM

| Command bit | Effect | Source |
|---|---|---|
| 0x01 | Joybus: parse the frame now, execute on the next 64-byte read | [n64brew] "PIF-NUS" |
| 0x02 | Challenge: 6105/7105 transform 30 nibbles with the CIC's 4-bit algorithm; other CICs invert the 15 bytes | [n64brew] "CIC-NUS"; [spec] section 2 |
| 0x08 | Terminate boot (cancels the 5-second timeout) | [n64brew] "PIF-NUS" |
| 0x10 | Lock the PIF ROM: the ROM area then reads as zero | [n64brew] "PIF-NUS", "Serial Interface" |
| 0x20 | Acquire checksum: read and clear PIF RAM 0x32-0x37, answer with 0x80 | [n64brew] "PIF-NUS" |
| 0x40 | Run checksum: compare with the CIC's value | [n64brew] "PIF-NUS" |

The Joybus frame walk: up to 5 channels; 0x00 skips a channel, 0xFD resets, 0xFE ends, 0xFF is
a no-op; a handshake is TX, RX, TX bytes, RX bytes ([n64brew] "PIF-NUS", Joybus). No devices
are attached in Phase 0, so every handshake reports "no device" (RX bit 7).

## Flashcart CIC emulation (host option)

`SystemConfig::cic_checksum_from_ipl3` makes the CIC report the IPL2 checksum of the ROM's
own IPL3, so an IPL3 that no retail CIC accepts still boots. This models a flashcart's CIC
emulation in direct-boot mode, not console hardware: SummerCart64 computes that checksum
(`sw/deployer`, `sign_ipl3`) and sets it with its `CIC_PARAMS_SET` command. By default the PIF
halts the CPU on a mismatch, as a console with a retail CIC does.
