#pragma once
// MIPS Interface: interrupt collection and masking, the CPU's IP2 line, MI_MODE (repeat,
// upper, EBus) and the mapping of 32-bit SysAD transfers onto the 64-bit RDRAM data bus.
// Facts and sources: docs/hardware/io.md (MI section).

#include "common/types.hpp"
#include "core/clock.hpp"
#include "core/cpu_pins.hpp"
#include "core/trace.hpp"

namespace reality {

class Ri;
class SaveState;

enum class MiIntr : u32 { SP = 1u << 0, SI = 1u << 1, AI = 1u << 2, VI = 1u << 3, PI = 1u << 4, DP = 1u << 5 };

// Source: n64brew "MIPS Interface", MI_MODE read and write layouts.
struct MiModeRead {
  using RepeatCount = Bits<0, 7>;
  using Repeat = Bit<7>;
  using Ebus = Bit<8>;
  using Upper = Bit<9>;
};
struct MiModeWrite {
  using RepeatCount = Bits<0, 7>;
  using ClearRepeat = Bit<7>;
  using SetRepeat = Bit<8>;
  using ClearEbus = Bit<9>;
  using SetEbus = Bit<10>;
  using ClearDp = Bit<11>;
  using ClearUpper = Bit<12>;
  using SetUpper = Bit<13>;
};

class Mi {
 public:
  Mi(Ri& ri, CpuPins& pins);

  u32 read_reg(u32 paddr);
  void write_reg(u32 paddr, u32 value);

  void raise(MiIntr i) noexcept;
  void lower(MiIntr i) noexcept;
  bool pending(MiIntr i) const noexcept { return (intr_ & static_cast<u32>(i)) != 0; }
  u32 interrupts() const noexcept { return intr_; }
  u32 mask() const noexcept { return mask_; }

  // CPU (SysAD) access to the RDRAM address range 0x0000_0000..0x03FF_FFFF.
  // size: 1,2,3,4 (single word transfer), 8 (doubleword), 16 or 32 (cache block).
  // Reads deliver what SysAD carries: the aligned 32-bit word for size <= 4, else `size` bytes.
  // Writes take the same layout (for size <= 4, the 4 bytes of the aligned word lanes).
  Tick cpu_read(Master m, u32 paddr, u32 size, u8* out, Tick t);
  Tick cpu_write(Master m, u32 paddr, u32 size, const u8* bus, Tick t);

  void serialize(SaveState& s);

 private:
  void update_line() noexcept;

  Ri& ri_;
  CpuPins& pins_;
  u32 mode_ = 0;  // read layout
  u32 intr_ = 0;
  u32 mask_ = 0;
};

}  // namespace reality
