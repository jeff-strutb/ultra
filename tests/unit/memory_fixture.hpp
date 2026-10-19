#pragma once
// RDRAM brought up for module tests the way IPL3 does it, plus register-packet helpers.
// Facts: docs/hardware/memory.md.

#include <array>

#include "core/trace.hpp"
#include "memory/base_rdram_arbiter.hpp"
#include "memory/rdram.hpp"
#include "memory/ri.hpp"

namespace ultra::test {

inline constexpr u32 kMiB = Rdram::kMiB;

inline constexpr u32 reg_offset(RdramReg r) { return static_cast<u32>(r) * 4; }

// A register write packet: the word at byte `at` of an otherwise zero stream.
inline void reg_write(Rdram& r, u32 id, RdramReg reg, u32 value, bool broadcast = false,
                      u32 at = 0) {
  std::array<u8, 16> stream{};
  store_be32(stream.data() + at, value);
  r.reg_write(id, reg_offset(reg), stream.data(), static_cast<u32>(stream.size()), broadcast);
}
inline u32 reg_read(Rdram& r, u32 id, RdramReg reg) {
  return hi32(r.reg_read(id, reg_offset(reg)));
}

// IdField[35:20] = n MiB.
inline constexpr u32 device_id(u32 mib) { return RdramDeviceId::IdLow::place(mib); }

// Brings a 4 MiB console's two devices up the way IPL3 does (n64brew "RDRAM", Reset and
// initialization; docs/hardware/pif.md): a broadcast Delay write that the devices, still at
// their reset WriteDelay of 4, sample (4 - 1) x 2 = 6 bytes late; then each not-yet-enabled
// device in SIn order takes an id and is enabled with full drive current (CC = 0).
// Device 0 ends at 2 MiB and device 1 at 0 MiB.
inline void init_like_ipl3(Rdram& r) {
  const u32 delay = RdramDelay::AckWinDelay::place(4) | RdramDelay::ReadDelay::place(7) |
                    RdramDelay::WriteDelay::place(1);
  reg_write(r, 0, RdramReg::Delay, delay, /*broadcast=*/true, /*at=*/6);
  reg_write(r, 0, RdramReg::DeviceId, device_id(2));  // device 0: first in the chain
  reg_write(r, 2, RdramReg::Mode, RdramMode::DE::mask);
  reg_write(r, 0, RdramReg::DeviceId, device_id(0));  // device 1: SIn now high
  reg_write(r, 0, RdramReg::Mode, RdramMode::DE::mask);
}

struct Channel {
  Rdram rdram{4 * kMiB};
  BaseRdramArbiter arbiter;
  TraceSink trace;
  Ri ri{rdram, arbiter, trace};
};

}  // namespace ultra::test
