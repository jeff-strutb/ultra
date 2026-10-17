#pragma once
// RDP command interface registers (DPC_*). Phase 0 models the register file and DPC_CLOCK;
// the RDP command DMA and pipeline arrive in Phase 2 (docs/SPEC.md), so commands written
// here are not fetched yet. Primary: n64brew "Reality Display Processor/Interface".

#include "common/types.hpp"
#include "core/clock.hpp"

namespace reality {

class SaveState;

class DpInterface {
 public:
  u32 read_reg(u32 paddr, Tick t) const noexcept;
  void write_reg(u32 paddr, u32 value, Tick t) noexcept;
  void serialize(SaveState& s);

 private:
  u32 start_ = 0;
  u32 end_ = 0;
  u32 current_ = 0;
  u32 status_ = 0;
  bool start_pending_ = false;
  Tick clock_base_ = 0;
};

}  // namespace reality
