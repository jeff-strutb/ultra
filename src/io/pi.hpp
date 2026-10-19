#pragma once
// Parallel Interface: cartridge-domain timing registers, PI DMA in both directions, and the
// CPU's direct I/O window onto the PI bus. Facts and sources: docs/hardware/io.md (PI).

#include <array>

#include "common/types.hpp"
#include "core/clock.hpp"
#include "core/scheduler.hpp"

namespace ultra {

class Cartridge;
class Mi;
class Ri;
class SaveState;

enum class PiRegister : u32 {
  DramAddr = 0x00,
  CartAddr = 0x04,
  RdLen = 0x08,
  WrLen = 0x0C,
  Status = 0x10,
  Dom1Lat = 0x14,
  Dom1Pwd = 0x18,
  Dom1Pgs = 0x1C,
  Dom1Rls = 0x20,
  Dom2Lat = 0x24,
  Dom2Pwd = 0x28,
  Dom2Pgs = 0x2C,
  Dom2Rls = 0x30,
};

// Source: n64brew "Parallel Interface", PI_STATUS.
struct PiStatusRead {
  using DmaBusy = Bit<0>;
  using IoBusy = Bit<1>;
  using Error = Bit<2>;
  using Interrupt = Bit<3>;
};
struct PiStatusWrite {
  using Reset = Bit<0>;
  using ClearIntr = Bit<1>;
};

class Pi {
 public:
  Pi(Scheduler& sched, Mi& mi, Ri& ri, Cartridge* cart);

  void attach_cart(Cartridge* cart) noexcept { cart_ = cart; }

  u32 read_reg(u32 paddr, Tick t);
  void write_reg(u32 paddr, u32 value, Tick t);

  // CPU direct I/O on the PI bus (physical 0x0500_0000..0x1FBF_FFFF, 0x1FD0_0000..0x7FFF_FFFF).
  // Returns the completion tick.
  Tick io_read(u32 paddr, u32& out, Tick t);
  Tick io_write(u32 paddr, u32 value, Tick t);

  bool dma_busy() const noexcept { return dma_.active; }

  void serialize(SaveState& s);

 private:
  struct Domain {
    u32 lat = 0, pwd = 0, pgs = 0, rls = 0;
  };
  const Domain& domain_for(u32 pi_addr) const noexcept;
  // Bus time to transfer `bytes` starting at `pi_addr` (crossing PI pages costs a new address
  // phase). Units: MasterClock ticks.
  u64 bus_ticks(u32 pi_addr, u32 bytes) const noexcept;
  u16 bus_read16(u32 addr) const noexcept;

  void start_dma(bool to_rdram, Tick t);
  void dma_step(Tick t);
  static void dma_event(void* ctx, Tick t) { static_cast<Pi*>(ctx)->dma_step(t); }

  Scheduler& sched_;
  Mi& mi_;
  Ri& ri_;
  Cartridge* cart_;  // not state: wiring

  u32 dram_addr_ = 0;
  u32 cart_addr_ = 0;
  u32 len_readback_ = 0x7F;
  bool error_ = false;
  std::array<Domain, 2> dom_{};

  // Direct I/O state: a write occupies the bus until io_busy_until_; its value is what reads
  // return meanwhile (n64brew "Memory map", PI external bus).
  Tick io_busy_until_ = 0;
  u32 io_latch_ = 0;

  struct Dma {
    bool active = false;
    bool to_rdram = false;
    bool first = true;
    u32 first_index = 0;
    u32 block = 0;
    u32 remaining = 0;
  } dma_;
  Event dma_ev_;  // not state: saved by the Scheduler
};

}  // namespace ultra
