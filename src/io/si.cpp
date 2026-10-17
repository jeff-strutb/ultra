#include "io/si.hpp"

#include "core/savestate.hpp"
#include "io/mi.hpp"
#include "io/pif.hpp"
#include "memory/ri.hpp"

namespace reality {

namespace {
enum class SiReg : u32 {
  DramAddr = 0x00,
  PifAdRd64b = 0x04,
  PifAdWr4b = 0x08,
  PifAdWr64b = 0x10,
  PifAdRd4b = 0x14,
  Status = 0x18,
};

// Serial packet durations. The SI-PIF link is clocked at SI/8 = MClock/32 (n64brew "Clock
// Timing"); packet sizes from n64brew "Serial Interface": command (2 bits) + address
// (9 bits) + ack + 32 data bits for 4-byte packets, 512 data bits for 64-byte packets.
// Guess: PIF firmware processing time on top of the wire time is unmeasured (ledger "si-pif").
constexpr u64 kTicksPerBit = kTicksPerPifClock;
constexpr u64 k4bTicks = (2 + 9 + 1 + 32) * kTicksPerBit;
constexpr u64 k64bTicks = (2 + 9 + 1 + 512) * kTicksPerBit;
}  // namespace

Si::Si(Scheduler& sched, Mi& mi, Ri& ri, Pif& pif) : sched_(sched), mi_(mi), ri_(ri), pif_(pif) {
  sched_.add(finish_ev_, "si.finish", &Si::finish_event, this);
}

u32 Si::read_reg(u32 paddr, Tick t) {
  switch (static_cast<SiReg>(paddr & 0x1C)) {
    case SiReg::DramAddr: return dram_addr_;
    case SiReg::PifAdRd64b:
    case SiReg::PifAdWr4b:
    case SiReg::PifAdWr64b:
    case SiReg::PifAdRd4b: return data_;
    case SiReg::Status: {
      // Bit 0 DMA busy (also set for direct writes), 1 IO busy, 3 DMA error,
      // 12 interrupt (n64brew "Serial Interface", SI_STATUS).
      u32 s = 0;
      if (busy(t)) {
        s |= 1u;
        if (op_ == Op::Wr4) s |= 2u;
      }
      if (dma_error_) s |= 8u;
      if (mi_.pending(MiIntr::SI)) s |= 1u << 12;
      return s;
    }
  }
  return 0;
}

void Si::write_reg(u32 paddr, u32 v, Tick t) {
  switch (static_cast<SiReg>(paddr & 0x1C)) {
    case SiReg::DramAddr: dram_addr_ = v & 0x00FF'FFFF; break;
    case SiReg::PifAdRd64b:
      if (busy(t)) {
        dma_error_ = true;
        break;
      }
      pif_addr_ = v & 0x7FC;
      op_ = Op::Rd64;
      busy_until_ = t + k64bTicks;
      sched_.schedule(finish_ev_, busy_until_);
      break;
    case SiReg::PifAdWr64b:
      if (busy(t)) {
        dma_error_ = true;
        break;
      }
      pif_addr_ = v & 0x7FC;
      ri_.read(Master::Si, dram_addr_ & ~3u, buf_.data(), 64, t);
      op_ = Op::Wr64;
      busy_until_ = t + k64bTicks;
      sched_.schedule(finish_ev_, busy_until_);
      break;
    case SiReg::PifAdWr4b:
      // Writes the "current data" register and sends a WR4B packet to the current address.
      if (busy(t)) break;
      data_ = v;
      op_ = Op::Wr4;
      busy_until_ = t + k4bTicks;
      sched_.schedule(finish_ev_, busy_until_);
      break;
    case SiReg::PifAdRd4b:
      data_ = pif_.read_word(pif_addr_);
      break;
    case SiReg::Status: mi_.lower(MiIntr::SI); break;  // any write acknowledges
  }
}

Tick Si::io_read(u32 paddr, u32& out, Tick t) {
  // Reads wait for a pending write to finish, then issue an RD4B packet.
  Tick start = t;
  if (busy(t)) {
    // The CPU stays stalled until the pending transfer has finished, so complete it now.
    start = busy_until_;
    sched_.cancel(finish_ev_);
    finish(busy_until_);
  }
  pif_addr_ = paddr & 0x7FC;
  out = pif_.read_word(pif_addr_);
  data_ = out;
  return start + k4bTicks;
}

Tick Si::io_write(u32 paddr, u32 value, Tick t) {
  // Asynchronous: the SI latches the word and releases the CPU. Writes during a pending
  // write are ignored (n64brew "Memory map", SI external bus).
  if (busy(t)) return t + 1;
  pif_addr_ = paddr & 0x7FC;
  data_ = value;
  op_ = Op::Wr4;
  busy_until_ = t + k4bTicks;
  sched_.schedule(finish_ev_, busy_until_);
  return t + 1;
}

void Si::finish(Tick t) {
  switch (op_) {
    case Op::Rd64:
      pif_.dma_read(buf_.data(), t);
      ri_.write(Master::Si, dram_addr_ & ~3u, buf_.data(), 64, t);
      break;
    case Op::Wr64: pif_.dma_write(buf_.data(), t); break;
    case Op::Wr4: pif_.write_word(pif_addr_, data_, t); break;
    case Op::None: return;
  }
  op_ = Op::None;
  // DMAs and direct writes raise the SI interrupt (n64brew "Serial Interface").
  mi_.raise(MiIntr::SI);
}

void Si::serialize(SaveState& s) {
  s.section("si", 1);
  s.value(dram_addr_);
  s.value(pif_addr_);
  s.value(data_);
  s.value(op_);
  s.value(busy_until_);
  s.value(dma_error_);
  s.array(buf_);
}

}  // namespace reality
