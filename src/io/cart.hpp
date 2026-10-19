#pragma once
// Devices on the PI external bus: cartridge ROM, FlashRAM save memory, an ISViewer-style debug
// port, and open bus.
// The PI bus is 16-bit; every device answers 16-bit reads/writes at 16-bit aligned PI-bus
// addresses. Source: n64brew "Parallel Interface", The PI Bus / Open bus behavior.

#include <memory>
#include <span>
#include <string>
#include <vector>

#include "common/types.hpp"
#include "io/flashram.hpp"

namespace ultra {

class SaveState;

class Cartridge {
 public:
  // Loads a ROM image and normalises it to big-endian (.z64) byte order. Accepts .z64
  // (0x80371240), .v64 (byte-swapped 16-bit) and .n64 (little-endian 32-bit) images.
  static std::unique_ptr<Cartridge> load(const std::string& path, std::string* error);
  static std::unique_ptr<Cartridge> from_bytes(std::vector<u8> rom);

  std::span<const u8> rom() const noexcept { return rom_; }
  u32 rom_size() const noexcept { return static_cast<u32>(rom_.size()); }

  // Header fields (n64brew "ROM Header").
  u32 header_word(u32 offset) const noexcept;
  char region_code() const noexcept { return rom_.size() > 0x3E ? static_cast<char>(rom_[0x3E]) : 0; }

  // Save type declared by the Advanced Homebrew ROM Header: "ED" at 0x3C, save type in bits 7:4
  // of 0x3F (n64brew "ROM Header"). 0 when the header is absent.
  u8 homebrew_save_type() const noexcept;
  static constexpr u8 kSaveTypeFlashRam = 5;

  // FlashRAM at PI 0x0800_0000. Present when the header declares it, or when enabled here.
  void enable_flashram(bool on);
  FlashRam* flashram() noexcept { return flash_.get(); }

  // ISViewer debug port (IS-Viewer 64 compatible): a 64 KiB buffer in the PI address space at
  // 0x13FF_0000; writing a length to 0x13FF_0014 emits that many bytes from 0x13FF_0020.
  void enable_isviewer(bool on) noexcept { isviewer_enabled_ = on; }
  // Text emitted so far. Its capacity is reserved at construction so the timing thread never
  // allocates; output beyond kIsvOutCapacity is dropped and counted.
  const std::string& isviewer_output() const noexcept { return isv_out_; }
  u64 isviewer_dropped() const noexcept { return isv_dropped_; }
  void clear_isviewer_output() noexcept { isv_out_.clear(); }

  // PI bus 16-bit accesses. `addr` is a PI-bus address (bit 0 ignored). Returns false when
  // no device claims the address (open bus).
  bool read16(u32 addr, u16& out) const noexcept;
  void write16(u32 addr, u16 v);

  void serialize(SaveState& s);

  static constexpr u32 kRomBase = 0x1000'0000;
  static constexpr u32 kIsvBase = 0x13FF'0000;
  static constexpr u32 kIsvSize = 0x1'0000;
  static constexpr u32 kIsvLenReg = 0x13FF'0014;
  static constexpr u32 kIsvBuffer = 0x13FF'0020;
  static constexpr usize kIsvOutCapacity = 4u << 20;

 private:
  explicit Cartridge(std::vector<u8> rom);

  std::vector<u8> rom_;
  std::unique_ptr<FlashRam> flash_;
  bool isviewer_enabled_ = true;
  std::vector<u8> isv_;
  std::string isv_out_;
  u64 isv_dropped_ = 0;
  u16 isv_len_hi_ = 0;
};

}  // namespace ultra
