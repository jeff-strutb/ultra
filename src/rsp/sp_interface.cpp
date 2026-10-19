#include "rsp/sp_interface.hpp"

#include <algorithm>
#include <cstring>

#include "core/savestate.hpp"
#include "io/mi.hpp"
#include "memory/arbiter_params.h"
#include "memory/ri.hpp"

namespace ultra {

namespace {
enum class SpReg : u32 {
  MemAddr = 0x00,
  DramAddr = 0x04,
  RdLen = 0x08,
  WrLen = 0x0C,
  Status = 0x10,
  DmaFull = 0x14,
  DmaBusy = 0x18,
  Semaphore = 0x1C,
};
// SP DMA moves data in RDRAM bursts of at most 128 bytes (RI maximum burst).
// Derived: n64brew "RDRAM Interface", Count.
constexpr u32 kBurst = 128;
// Register offset within the SP register block: 8 registers, 4 bytes apart (mirrored above).
constexpr u32 kSpRegOffsetMask = 0x1C;
// The second register at 0x0408_0000 is SP_IBIST.
constexpr u32 kSpIbistOffset = 0x4;
// DMEM then IMEM, word aligned; mirrored every 8 KiB (n64brew "Memory map").
constexpr u32 kSpMemWordMask = 2 * SpInterface::kMemBytes - 4;
// SP DMA bursts never cross a 2 KiB RDRAM page (n64brew "Reality Signal Processor/Interface",
// DMA transfers).
constexpr u32 kRdramPage = arbiter_params::kRowBytes;
}  // namespace

SpInterface::SpInterface(Scheduler& sched, Mi& mi, Ri& ri) : sched_(sched), mi_(mi), ri_(ri) {
  sched_.add(dma_ev_, "sp.dma", &SpInterface::dma_event, this);
}

u32 SpInterface::read_mem(u32 paddr) const noexcept {
  // DMEM/IMEM mirror every 8 KiB across 0x0400_0000..0x0403_FFFF (n64brew "Memory map").
  return load_be32(mem_.data() + (paddr & kSpMemWordMask));
}

void SpInterface::write_mem(u32 paddr, u32 value) noexcept {
  store_be32(mem_.data() + (paddr & kSpMemWordMask), value);
}

u32 SpInterface::read_reg(u32 paddr, Tick) {
  switch (static_cast<SpReg>(paddr & kSpRegOffsetMask)) {
    case SpReg::MemAddr: return current_.spaddr;
    case SpReg::DramAddr: return current_.ramaddr;
    case SpReg::RdLen:
    case SpReg::WrLen: return current_.len;
    case SpReg::Status: {
      u32 s = status_;
      s = SpStatusRead::DmaBusy::set(s, busy_ ? 1 : 0);
      s = SpStatusRead::DmaFull::set(s, full_ ? 1 : 0);
      return s;
    }
    case SpReg::DmaFull: return full_ ? 1 : 0;
    case SpReg::DmaBusy: return busy_ ? 1 : 0;
    case SpReg::Semaphore: {
      // Reading returns the bit and sets it (n64brew, SP_SEMAPHORE).
      const u32 v = semaphore_;
      semaphore_ = 1;
      return v;
    }
  }
  return 0;
}

void SpInterface::write_reg(u32 paddr, u32 v, Tick t) {
  switch (static_cast<SpReg>(paddr & kSpRegOffsetMask)) {
    case SpReg::MemAddr: pending_.spaddr = v & SpMemAddr::kWriteMask; break;
    case SpReg::DramAddr: pending_.ramaddr = v & SpRamAddr::Addr::mask; break;
    case SpReg::RdLen:
    case SpReg::WrLen:
      pending_.len = v & SpDmaLen::kWriteMask;
      pending_.to_sp = (paddr & kSpRegOffsetMask) == static_cast<u32>(SpReg::RdLen);
      if (!busy_) {
        current_ = pending_;
        start_dma(t);
      } else {
        full_ = true;
      }
      break;
    case SpReg::Status: {
      using W = SpStatusWrite;
      using R = SpStatusRead;
      auto apply = [&](bool clr, bool set, u32 bit) {
        if (set && !clr) status_ |= bit;
        if (clr && !set) status_ &= ~bit;
      };
      apply(W::ClrHalt::test(v), W::SetHalt::test(v), R::Halt::mask);
      if (W::ClrBroke::test(v)) status_ &= ~R::Broke::mask;
      if (W::SetIntr::test(v) && !W::ClrIntr::test(v)) mi_.raise(MiIntr::SP);
      if (W::ClrIntr::test(v) && !W::SetIntr::test(v)) mi_.lower(MiIntr::SP);
      apply(W::ClrSStep::test(v), W::SetSStep::test(v), R::SStep::mask);
      apply(W::ClrIntBreak::test(v), W::SetIntBreak::test(v), R::IntBreak::mask);
      for (u32 i = 0; i < 8; ++i)
        apply((v & W::clr_sig(i)) != 0, (v & W::set_sig(i)) != 0, R::sig(i));
      break;
    }
    case SpReg::DmaFull:
    case SpReg::DmaBusy: break;
    case SpReg::Semaphore: semaphore_ = 0; break;  // any write releases it
  }
}

u32 SpInterface::read_pc_reg(u32 paddr) const noexcept {
  // 0x0408_0000 SP_PC, 0x0408_0004 SP_IBIST (not modelled, reads 0).
  return (paddr & kSpIbistOffset) ? 0 : pc_;
}

void SpInterface::write_pc_reg(u32 paddr, u32 value) noexcept {
  if (!(paddr & kSpIbistOffset)) pc_ = value & SpPc::Pc::mask;
}

void SpInterface::start_dma(Tick t) {
  busy_ = true;
  rows_left_ = SpDmaLen::Count::get(current_.len) + 1;
  sched_.schedule(dma_ev_, t);
}

void SpInterface::dma_row(Tick t) {
  // One RDRAM burst per event. A row is LEN + 1 rounded up to 8 bytes; after each row the
  // RDRAM address advances by skip. SP addresses wrap within the selected 4 KiB bank (n64brew,
  // DMA transfers). The data of a burst lands in SP memory when the burst completes.
  const u32 row_bytes = (SpDmaLen::LenUnits::get(current_.len) + 1) * SpDmaLen::kUnitBytes;
  const u32 skip = SpDmaLen::Skip::get(current_.len);
  const u32 bank = current_.spaddr & SpMemAddr::Bank::mask;
  const u32 sp = current_.spaddr & SpMemAddr::Addr::mask;
  const u32 ram = current_.ramaddr;
  const u32 n = std::min({kBurst, row_bytes - row_done_, kRdramPage - ram % kRdramPage});
  u8 buf[kBurst];
  Tick at;
  if (current_.to_sp) {
    at = ri_.read(Master::RspDma, ram, buf, n, t, nullptr, TraceKind::BlockRead);
    for (u32 i = 0; i < n; ++i) mem_[bank + SpMemAddr::Offset::get(sp + i)] = buf[i];
  } else {
    for (u32 i = 0; i < n; ++i) buf[i] = mem_[bank + SpMemAddr::Offset::get(sp + i)];
    at = ri_.write(Master::RspDma, ram, buf, n, t, nullptr, nullptr, TraceKind::BlockWrite);
  }
  current_.spaddr = bank | SpMemAddr::Offset::get(sp + n);
  current_.ramaddr = SpRamAddr::Wrap::get(ram + n);
  row_done_ += n;
  if (row_done_ < row_bytes) {
    sched_.schedule(dma_ev_, at);
    return;
  }
  row_done_ = 0;
  current_.ramaddr = (current_.ramaddr + skip) & SpRamAddr::Addr::mask;
  --rows_left_;
  if (rows_left_ != 0) {
    current_.len = SpDmaLen::Count::set(current_.len, rows_left_ - 1);
    sched_.schedule(dma_ev_, at);
    return;
  }
  // Transfer complete: length reads 0xFF8 (decremented past zero), count 0, skip unchanged.
  current_.len = SpDmaLen::Count::set(current_.len, 0);
  current_.len = SpDmaLen::Len::set(current_.len, SpDmaLen::LenUnits::mask);
  busy_ = false;
  if (full_) {
    full_ = false;
    current_ = pending_;
    start_dma(at);
  }
}

void SpInterface::serialize(SaveState& s) {
  s.section("sp_interface", 1);
  s.array(mem_);
  s.value(status_);
  s.value(semaphore_);
  s.value(pc_);
  for (DmaRegs* r : {&current_, &pending_}) {
    s.value(r->spaddr);
    s.value(r->ramaddr);
    s.value(r->len);
    s.value(r->to_sp);
  }
  s.value(busy_);
  s.value(full_);
  s.value(rows_left_);
  s.value(row_done_);
}

}  // namespace ultra
