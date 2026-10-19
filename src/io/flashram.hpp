#pragma once
// Cartridge FlashRAM: 128 KiB of byte-indexed flash at PI 0x0800_0000 with its command register
// at 0x0801_0000. Facts and sources: docs/hardware/io.md (FlashRAM).

#include <array>

#include "common/types.hpp"

namespace ultra {

class SaveState;

// Command Internal Register value: the command in bits 31:24, a page index in 9:0
// (n64brew "Flash", Command Internal Register).
struct FlashCommand {
  using Op = Bits<24, 8>;
  using Page = Bits<0, 10>;
};

// Status register (n64brew "Flash", Status Mode).
struct FlashStatus {
  using ProgramBusy = Bit<0, u8>;
  using EraseBusy = Bit<1, u8>;
  using ProgramOk = Bit<2, u8>;
  using EraseOk = Bit<3, u8>;
  using WsmReady = Bit<7, u8>;
};

class FlashRam {
 public:
  static constexpr u32 kBase = 0x0800'0000;   // n64brew "Flash", TLDR
  static constexpr u32 kSize = 0x2'0000;      // 1 Mbit
  static constexpr u32 kCmdReg = 0x0801'0000; // CIR
  static constexpr u32 kPageSize = 128;       // 8 sectors x 128 pages x 128 bytes
  static constexpr u32 kPagesPerSector = 128;
  // MX29L1101_A, byte-indexed (n64brew "Flash", Silicon ID table). Which model a cart carries
  // is the cart's choice; ledger "flash-model".
  static constexpr u64 kSiliconId = 0x1111'8001'00C2'001Dull;

  // n64brew "Flash", Command Internal Register table.
  enum : u8 {
    kCmdChipEraseSetup = 0x3C,
    kCmdSectorEraseSetup = 0x4B,
    kCmdErase = 0x78,
    kCmdProgramPage = 0xA5,
    kCmdLoadPage = 0xB4,
    kCmdStatus = 0xD2,
    kCmdSiliconId = 0xE1,
    kCmdReadArray = 0xF0,
  };

  enum class Mode : u8 { ReadArray, Status, SiliconId, LoadPage };

  FlashRam();

  static bool claims(u32 addr) noexcept { return addr >= kBase && addr < kBase + kSize; }
  u16 read16(u32 addr) const noexcept;
  void write16(u32 addr, u16 v) noexcept;

  // The array as a .sav file stores it: big-endian bytes, offset 0 first.
  std::array<u8, kSize>& data() noexcept { return array_; }
  const std::array<u8, kSize>& data() const noexcept { return array_; }
  Mode mode() const noexcept { return mode_; }
  u8 status() const noexcept { return status_; }

  void serialize(SaveState& s);

 private:
  void command(u32 cmd) noexcept;

  enum class EraseSetup : u8 { None, Chip, Sector };

  std::array<u8, kSize> array_;
  std::array<u8, kPageSize> page_;
  Mode mode_ = Mode::ReadArray;
  EraseSetup erase_setup_ = EraseSetup::None;
  u16 erase_page_ = 0;
  u8 status_ = 0;
  u16 cmd_hi_ = 0;
};

}  // namespace ultra
