#pragma once
// RSP interface: DMEM/IMEM, SP registers, the double-buffered SP DMA engine and the
// semaphore. The RSP core (Phase 1) attaches to this; in Phase 0 the core is not implemented
// and the RSP never executes, so HALT only changes when software writes it.
// Facts and sources: docs/hardware/rsp.md. Primary: n64brew "Reality Signal Processor/Interface".

#include <array>

#include "common/types.hpp"
#include "core/clock.hpp"
#include "core/scheduler.hpp"

namespace ultra {

class Mi;
class Ri;
class SaveState;

// SP_STATUS read layout.
struct SpStatusRead {
  using Halt = Bit<0>;
  using Broke = Bit<1>;
  using DmaBusy = Bit<2>;
  using DmaFull = Bit<3>;
  using IoBusy = Bit<4>;
  using SStep = Bit<5>;
  using IntBreak = Bit<6>;
  using Sig = Bits<7, 8>;
};
// SP_STATUS write layout: (clear, set) pairs.
struct SpStatusWrite {
  using ClrHalt = Bit<0>;
  using SetHalt = Bit<1>;
  using ClrBroke = Bit<2>;
  using ClrIntr = Bit<3>;
  using SetIntr = Bit<4>;
  using ClrSStep = Bit<5>;
  using SetSStep = Bit<6>;
  using ClrIntBreak = Bit<7>;
  using SetIntBreak = Bit<8>;
  static constexpr u32 clr_sig(u32 i) noexcept { return 1u << (9 + 2 * i); }
  static constexpr u32 set_sig(u32 i) noexcept { return 1u << (10 + 2 * i); }
};
// SP_DMA_RDLEN / SP_DMA_WRLEN layout.
struct SpDmaLen {
  using Len = Bits<0, 12>;
  using Count = Bits<12, 8>;
  using Skip = Bits<20, 12>;
};

class SpInterface {
 public:
  static constexpr u32 kMemBytes = 0x1000;

  SpInterface(Scheduler& sched, Mi& mi, Ri& ri);

  // CPU accesses. `paddr` within 0x0400_0000..0x040F_FFFF.
  u32 read_mem(u32 paddr) const noexcept;
  void write_mem(u32 paddr, u32 value) noexcept;
  u32 read_reg(u32 paddr, Tick t);
  void write_reg(u32 paddr, u32 value, Tick t);
  u32 read_pc_reg(u32 paddr) const noexcept;
  void write_pc_reg(u32 paddr, u32 value) noexcept;

  bool halted() const noexcept { return SpStatusRead::Halt::test(status_); }
  u8* dmem() noexcept { return mem_.data(); }
  u8* imem() noexcept { return mem_.data() + kMemBytes; }

  void serialize(SaveState& s);

 private:
  struct DmaRegs {
    u32 spaddr = 0;   // bank bit 12 + address
    u32 ramaddr = 0;
    u32 len = 0;      // raw RDLEN/WRLEN layout
    bool to_sp = true;  // RDLEN: RDRAM -> SP
  };

  void start_dma(Tick t);
  void dma_row(Tick t);
  static void dma_event(void* ctx, Tick t) { static_cast<SpInterface*>(ctx)->dma_row(t); }

  Scheduler& sched_;
  Mi& mi_;
  Ri& ri_;

  std::array<u8, 2 * kMemBytes> mem_{};  // DMEM then IMEM
  u32 status_ = SpStatusRead::Halt::mask;
  u32 semaphore_ = 0;
  u32 pc_ = 0;

  DmaRegs current_{};   // visible through reads
  DmaRegs pending_{};   // latched by register writes, starts when written to a LEN register
  bool busy_ = false;
  bool full_ = false;
  u32 rows_left_ = 0;
  u32 row_done_ = 0;
  Event dma_ev_;
};

}  // namespace ultra
