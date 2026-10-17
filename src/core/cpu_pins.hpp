#pragma once
// External input pins of the VR4300 driven by the rest of the machine.
// Int0..Int4 map to Cause.IP2..IP6 (NEC VR4300 User's Manual 6.3.6 Cause Register,
// 14.2 External Normal Interrupts). On the N64, Int0 is the RCP (MI) interrupt and Int2 is
// the PIF pre-NMI (reset button) interrupt (n64brew "PIF-NUS", Console Reset).

#include "common/types.hpp"

namespace reality {

struct CpuPins {
  enum : u8 { kRcp = 1u << 0, kCart = 1u << 1, kPreNmi = 1u << 2, kInt3 = 1u << 3, kInt4 = 1u << 4 };
  u8 int_lines = 0;
  bool nmi = false;

  void set(u8 line, bool level) noexcept {
    int_lines = level ? static_cast<u8>(int_lines | line) : static_cast<u8>(int_lines & ~line);
  }
};

}  // namespace reality
