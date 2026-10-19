#include "io/flashram.hpp"

#include <algorithm>

#include "core/savestate.hpp"

namespace ultra {

FlashRam::FlashRam() {
  array_.fill(0xFF);  // erased
  page_.fill(0xFF);   // "Initial content of Internal Page is all bits set" (n64brew "Flash")
}

u16 FlashRam::read16(u32 addr) const noexcept {
  const u32 off = (addr - kBase) & ~1u;
  switch (mode_) {
    case Mode::ReadArray:
      return static_cast<u16>((u16{array_[off]} << 8) | array_[off + 1]);
    case Mode::Status:
      // "the pattern 00 <u8> ... is repeated" whatever the address (n64brew "Flash", Status Mode).
      return status_;
    case Mode::SiliconId: {
      // The ID repeats every 8 bytes of a burst; the burst position is the low address bits
      // (ledger "flash-silicon-id-burst").
      const u32 b = off & 7;
      return static_cast<u16>(kSiliconId >> (48 - 8 * b));
    }
    case Mode::LoadPage: {
      const u32 b = off & (kPageSize - 1);
      return static_cast<u16>((u16{page_[b]} << 8) | page_[b + 1]);
    }
  }
  return 0;
}

void FlashRam::write16(u32 addr, u16 v) noexcept {
  addr &= ~1u;
  if (addr == kCmdReg) {
    cmd_hi_ = v;  // the 32-bit CIR write arrives high half first on the 16-bit bus
    return;
  }
  if (addr == kCmdReg + 2) {
    command((u32{cmd_hi_} << 16) | v);
    return;
  }
  const u32 off = addr - kBase;
  if (mode_ == Mode::LoadPage) {
    // Writes set and clear bits (MX29L1100 behaviour, n64brew "Flash", Load Byte Page Mode).
    const u32 b = off & (kPageSize - 1);
    page_[b] = static_cast<u8>(v >> 8);
    page_[b + 1] = static_cast<u8>(v);
  } else if (mode_ == Mode::Status && off < 2) {
    // "writing 0 at flash address 0 while in Status Mode" clears the result bits.
    status_ = static_cast<u8>(status_ & ~(FlashStatus::EraseOk::mask | FlashStatus::ProgramOk::mask));
  }
}

void FlashRam::command(u32 cmd) noexcept {
  const u32 page = FlashCommand::Page::get(cmd);
  switch (FlashCommand::Op::get(cmd)) {
    case kCmdChipEraseSetup:
      erase_setup_ = EraseSetup::Chip;
      break;
    case kCmdSectorEraseSetup:
      erase_setup_ = EraseSetup::Sector;
      erase_page_ = static_cast<u16>(page);
      break;
    case kCmdErase:
      // Without a setup command Erase is ignored (n64brew "Flash", Erase). Erase and program
      // complete at once here (ledger "flash-op-time") and switch the chip to Status Mode.
      if (erase_setup_ == EraseSetup::Chip) {
        array_.fill(0xFF);
      } else if (erase_setup_ == EraseSetup::Sector) {
        const u32 first = (erase_page_ / kPagesPerSector) * kPagesPerSector * kPageSize;
        std::fill(array_.begin() + first, array_.begin() + first + kPagesPerSector * kPageSize, 0xFF);
      } else {
        break;
      }
      erase_setup_ = EraseSetup::None;
      status_ = static_cast<u8>(FlashStatus::WsmReady::mask | FlashStatus::EraseOk::mask);
      mode_ = Mode::Status;
      break;
    case kCmdProgramPage: {
      // Programming clears bits only (n64brew "Flash", Chip layout).
      u8* dst = array_.data() + page * kPageSize;
      for (u32 i = 0; i < kPageSize; ++i) dst[i] &= page_[i];
      page_.fill(0xFF);  // "After a Program operation, the content on Internal Page is reset"
      status_ = static_cast<u8>(FlashStatus::WsmReady::mask | FlashStatus::ProgramOk::mask);
      mode_ = Mode::Status;
      break;
    }
    case kCmdLoadPage: mode_ = Mode::LoadPage; break;
    case kCmdStatus: mode_ = Mode::Status; break;
    case kCmdSiliconId: mode_ = Mode::SiliconId; break;
    case kCmdReadArray: mode_ = Mode::ReadArray; break;
    default: break;  // unknown commands are ignored
  }
}

void FlashRam::serialize(SaveState& s) {
  s.section("flashram", 1);
  s.bytes(array_.data(), array_.size());
  s.bytes(page_.data(), page_.size());
  s.enumeration(mode_, Mode::LoadPage);
  s.enumeration(erase_setup_, EraseSetup::Sector);
  s.value(erase_page_);
  s.value(status_);
  s.value(cmd_hi_);
}

}  // namespace ultra
