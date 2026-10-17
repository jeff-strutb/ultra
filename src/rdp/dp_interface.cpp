#include "rdp/dp_interface.hpp"

#include "core/savestate.hpp"

namespace reality {

namespace {
enum class DpReg : u32 { Start = 0x00, End = 0x04, Current = 0x08, Status = 0x0C,
                         Clock = 0x10, BufBusy = 0x14, PipeBusy = 0x18, TmemBusy = 0x1C };
constexpr u32 kXbus = 1u << 0, kFreeze = 1u << 1, kFlush = 1u << 2;
constexpr u32 kStartPending = 1u << 10;
}  // namespace

u32 DpInterface::read_reg(u32 paddr, Tick t) const noexcept {
  // Registers mirror every 0x20 bytes (n64brew RDP interface).
  switch (static_cast<DpReg>(paddr & 0x1C)) {
    case DpReg::Start: return start_;
    case DpReg::End: return end_;
    case DpReg::Current: return current_;
    case DpReg::Status: return status_ | (start_pending_ ? kStartPending : 0);
    case DpReg::Clock: return static_cast<u32>(t - clock_base_) & 0xFF'FFFF;  // 24-bit, RCP clock
    case DpReg::BufBusy:
    case DpReg::PipeBusy:
    case DpReg::TmemBusy: return 0;
  }
  return 0;
}

void DpInterface::write_reg(u32 paddr, u32 v, Tick t) noexcept {
  switch (static_cast<DpReg>(paddr & 0x1C)) {
    case DpReg::Start:
      start_ = v & 0x00FF'FFF8;
      start_pending_ = true;
      break;
    case DpReg::End:
      end_ = v & 0x00FF'FFF8;
      if (start_pending_) {
        current_ = start_;
        start_pending_ = false;
      }
      break;
    case DpReg::Status: {
      auto apply = [&](u32 clr_bit, u32 set_bit, u32 flag) {
        const bool clr = v & clr_bit, set = v & set_bit;
        if (set && !clr) status_ |= flag;
        if (clr && !set) status_ &= ~flag;
      };
      apply(1u << 0, 1u << 1, kXbus);
      apply(1u << 2, 1u << 3, kFreeze);
      apply(1u << 4, 1u << 5, kFlush);
      if (v & (1u << 9)) clock_base_ = t;  // CLR_CLOCK
      break;
    }
    default: break;
  }
}

void DpInterface::serialize(SaveState& s) {
  s.section("dp_interface", 1);
  s.value(start_);
  s.value(end_);
  s.value(current_);
  s.value(status_);
  s.value(start_pending_);
  s.value(clock_base_);
}

}  // namespace reality
