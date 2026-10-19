#pragma once
// Serial Interface: SI registers, 64-byte DMA to/from PIF RAM, and the CPU's word window onto
// the PIF (asynchronous writes, delayed reads). Facts and sources: docs/hardware/io.md (SI).
// Primary: n64brew "Serial Interface", "Memory map".

#include <array>

#include "common/types.hpp"
#include "core/clock.hpp"
#include "core/scheduler.hpp"

namespace ultra {

class Mi;
class Pif;
class Ri;
class SaveState;

class Si {
 public:
  Si(Scheduler& sched, Mi& mi, Ri& ri, Pif& pif);

  u32 read_reg(u32 paddr, Tick t);
  void write_reg(u32 paddr, u32 value, Tick t);

  // CPU access to physical 0x1FC0_0000..0x1FCF_FFFF. Returns completion tick.
  Tick io_read(u32 paddr, u32& out, Tick t);
  Tick io_write(u32 paddr, u32 value, Tick t);

  void serialize(SaveState& s);

 private:
  enum class Op : u8 { None, Rd64, Wr64, Wr4 };
  void finish(Tick t);
  static void finish_event(void* ctx, Tick t) { static_cast<Si*>(ctx)->finish(t); }
  bool busy(Tick t) const noexcept { return op_ != Op::None && t < busy_until_; }

  Scheduler& sched_;
  Mi& mi_;
  Ri& ri_;
  Pif& pif_;

  u32 dram_addr_ = 0;
  u32 pif_addr_ = 0;     // internal "current address" register
  u32 data_ = 0;         // internal "current data" register
  Op op_ = Op::None;
  Tick busy_until_ = 0;
  bool dma_error_ = false;
  std::array<u8, 64> buf_{};
  Event finish_ev_;  // not state: saved by the Scheduler
};

}  // namespace ultra
