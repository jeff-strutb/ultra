#include "io/cart.hpp"

#include <cstdio>

#include "core/savestate.hpp"

namespace ultra {

Cartridge::Cartridge(std::vector<u8> rom) : rom_(std::move(rom)), isv_(kIsvSize, 0) {
  isv_out_.reserve(kIsvOutCapacity);
}

std::unique_ptr<Cartridge> Cartridge::from_bytes(std::vector<u8> rom) {
  if (rom.size() >= 4) {
    const u32 magic = load_be32(rom.data());
    if (magic == 0x37804012) {  // .v64: 16-bit byte swapped
      for (usize i = 0; i + 1 < rom.size(); i += 2) std::swap(rom[i], rom[i + 1]);
    } else if (magic == 0x40123780) {  // .n64: 32-bit little endian
      for (usize i = 0; i + 3 < rom.size(); i += 4) {
        std::swap(rom[i], rom[i + 3]);
        std::swap(rom[i + 1], rom[i + 2]);
      }
    }
  }
  return std::unique_ptr<Cartridge>(new Cartridge(std::move(rom)));
}

std::unique_ptr<Cartridge> Cartridge::load(const std::string& path, std::string* error) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    if (error) *error = "cannot open " + path;
    return nullptr;
  }
  std::vector<u8> data;
  u8 buf[1 << 16];
  usize n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) data.insert(data.end(), buf, buf + n);
  std::fclose(f);
  if (data.size() < 0x1000) {
    if (error) *error = "ROM too small: " + path;
    return nullptr;
  }
  return from_bytes(std::move(data));
}

u32 Cartridge::header_word(u32 offset) const noexcept {
  if (offset + 4 > rom_.size()) return 0;
  return load_be32(rom_.data() + offset);
}

bool Cartridge::read16(u32 addr, u16& out) const noexcept {
  addr &= ~1u;
  if (isviewer_enabled_ && addr >= kIsvBase && addr < kIsvBase + kIsvSize) {
    out = load_be16(isv_.data() + (addr - kIsvBase));
    return true;
  }
  if (addr >= kRomBase && addr - kRomBase < rom_.size()) {
    const u32 off = addr - kRomBase;
    out = static_cast<u16>((u16{rom_[off]} << 8) | (off + 1 < rom_.size() ? rom_[off + 1] : 0));
    return true;
  }
  return false;
}

void Cartridge::write16(u32 addr, u16 v) {
  addr &= ~1u;
  if (isviewer_enabled_ && addr >= kIsvBase && addr < kIsvBase + kIsvSize) {
    store_be16(isv_.data() + (addr - kIsvBase), v);
    // The length register is a 32-bit word: the high half arrives first on the 16-bit bus.
    if (addr == kIsvLenReg) {
      isv_len_hi_ = v;
    } else if (addr == kIsvLenReg + 2) {
      const u32 len = (u32{isv_len_hi_} << 16) | v;
      const u32 avail = kIsvSize - (kIsvBuffer - kIsvBase);
      for (u32 i = 0; i < len && i < avail; ++i) {
        if (isv_out_.size() < kIsvOutCapacity)
          isv_out_.push_back(static_cast<char>(isv_[kIsvBuffer - kIsvBase + i]));
        else
          ++isv_dropped_;
      }
    }
  }
  // ROM ignores writes.
}

void Cartridge::serialize(SaveState& s) {
  s.section("cart", 1);
  s.bytes(isv_.data(), isv_.size());
  s.value(isv_len_hi_);
}

}  // namespace ultra
