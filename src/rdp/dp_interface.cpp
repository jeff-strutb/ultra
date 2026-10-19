#include "rdp/dp_interface.hpp"

#include "core/savestate.hpp"

namespace ultra {

namespace {
enum class DpReg : u32 { Start = 0x00, End = 0x04, Current = 0x08, Status = 0x0C,
                         Clock = 0x10, BufBusy = 0x14, PipeBusy = 0x18, TmemBusy = 0x1C };
// Register offset within the DP command block: 8 registers, mirrored every 0x20 bytes.
constexpr u32 kDpRegOffsetMask = 0x1C;

// Register layouts. Source: n64brew "Reality Display Processor/Interface", register tables.
struct DpcAddr {  // DPC_START, DPC_END: RDRAM address in 8-byte units
  using Addr = Bits<3, 21>;
};
struct DpcStatusRead {
  using Xbus = Bit<0>;
  using Freeze = Bit<1>;
  using Flush = Bit<2>;
  using StartPending = Bit<10>;
};
struct DpcStatusWrite {
  using ClrXbus = Bit<0>;
  using SetXbus = Bit<1>;
  using ClrFreeze = Bit<2>;
  using SetFreeze = Bit<3>;
  using ClrFlush = Bit<4>;
  using SetFlush = Bit<5>;
  using ClrClock = Bit<9>;
};
struct DpcCounter {  // DPC_CLOCK and the busy counters: 24 bits at the RCP clock
  using Count = Bits<0, 24>;
};
static_assert(DpcAddr::Addr::mask == 0x00FF'FFF8);
}  // namespace

u32 DpInterface::read_reg(u32 paddr, Tick t) const noexcept {
  // Registers mirror every 0x20 bytes (n64brew RDP interface).
  switch (static_cast<DpReg>(paddr & kDpRegOffsetMask)) {
    case DpReg::Start: return start_;
    case DpReg::End: return end_;
    case DpReg::Current: return current_;
    case DpReg::Status: return status_ | (start_pending_ ? DpcStatusRead::StartPending::mask : 0);
    case DpReg::Clock: return DpcCounter::Count::get(static_cast<u32>(t - clock_base_));
    case DpReg::BufBusy:
    case DpReg::PipeBusy:
    case DpReg::TmemBusy: return 0;
  }
  return 0;
}

void DpInterface::write_reg(u32 paddr, u32 v, Tick t) noexcept {
  switch (static_cast<DpReg>(paddr & kDpRegOffsetMask)) {
    case DpReg::Start:
      start_ = v & DpcAddr::Addr::mask;
      start_pending_ = true;
      break;
    case DpReg::End:
      end_ = v & DpcAddr::Addr::mask;
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
      using W = DpcStatusWrite;
      using R = DpcStatusRead;
      apply(W::ClrXbus::mask, W::SetXbus::mask, R::Xbus::mask);
      apply(W::ClrFreeze::mask, W::SetFreeze::mask, R::Freeze::mask);
      apply(W::ClrFlush::mask, W::SetFlush::mask, R::Flush::mask);
      if (W::ClrClock::test(v)) clock_base_ = t;
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

}  // namespace ultra
