#include "io/mi.hpp"

#include <cstring>

#include "core/savestate.hpp"
#include "memory/ri.hpp"

namespace ultra {

namespace {
// MI_VERSION as reported by most retail consoles.
// Source: n64brew "MIPS Interface", MI_VERSION Extra Details ("Most consoles report
// 0x0202_0102").
constexpr u32 kMiVersion = 0x0202'0102;
// Physical start of RDRAM register space (n64brew "Memory map").
constexpr u32 kRdramRegSpace = 0x03F0'0000;

enum class MiReg : u32 { Mode = 0x0, Version = 0x4, Interrupt = 0x8, Mask = 0xC };

// RDRAM moves 8-byte words (octbytes); a 32-bit access uses the half that Addr[2] selects
// (n64brew "MIPS Interface", MI's Mode).
constexpr u32 kOctBytes = 8;
constexpr u32 kHalfSelect = 4;
constexpr u32 half_offset(u32 paddr, bool upper) {
  return (upper || !(paddr & kHalfSelect)) ? 0 : kHalfSelect;
}
constexpr u32 octbyte_base(u32 paddr) { return paddr & ~(kOctBytes - 1); }
}  // namespace

Mi::Mi(Ri& ri, CpuPins& pins) : ri_(ri), pins_(pins) {}

void Mi::update_line() noexcept { pins_.set(CpuPins::kRcp, (intr_ & mask_) != 0); }

void Mi::raise(MiIntr i) noexcept {
  intr_ |= static_cast<u32>(i);
  update_line();
}

void Mi::lower(MiIntr i) noexcept {
  intr_ &= ~static_cast<u32>(i);
  update_line();
}

u32 Mi::read_reg(u32 paddr) {
  // Only the low 4 address bits are decoded (n64brew "MIPS Interface": "Accesses beyond
  // 0x0430 0010 are mirrored").
  switch (static_cast<MiReg>(paddr & 0xC)) {
    case MiReg::Mode: return mode_;
    case MiReg::Version: return kMiVersion;
    case MiReg::Interrupt: return intr_;
    case MiReg::Mask: return mask_;
  }
  return 0;
}

void Mi::write_reg(u32 paddr, u32 v) {
  switch (static_cast<MiReg>(paddr & 0xC)) {
    case MiReg::Mode: {
      using W = MiModeWrite;
      using R = MiModeRead;
      mode_ = R::RepeatCount::set(mode_, W::RepeatCount::get(v));
      // Set and clear together leave the bit unchanged (n64brew "MIPS Interface").
      auto apply = [&](bool set, bool clr, u32 bit) {
        if (set && !clr) mode_ |= bit;
        if (clr && !set) mode_ &= ~bit;
      };
      apply(W::SetRepeat::test(v), W::ClearRepeat::test(v), R::Repeat::mask);
      apply(W::SetEbus::test(v), W::ClearEbus::test(v), R::Ebus::mask);
      apply(W::SetUpper::test(v), W::ClearUpper::test(v), R::Upper::mask);
      if (W::ClearDp::test(v)) lower(MiIntr::DP);
      break;
    }
    case MiReg::Version:
    case MiReg::Interrupt: break;
    case MiReg::Mask: {
      for (u32 i = 0; i < kMiIntrSources; ++i) {
        const bool clr = (v & MiMaskWrite::clr(i)) != 0;
        const bool set = (v & MiMaskWrite::set(i)) != 0;
        if (set && !clr) mask_ |= 1u << i;
        if (clr && !set) mask_ &= ~(1u << i);
      }
      update_line();
      break;
    }
  }
}

Tick Mi::cpu_read(Master m, u32 paddr, u32 size, u8* out, Tick t) {
  const bool upper = MiModeRead::Upper::test(mode_);
  if (paddr >= kRdramRegSpace) {
    u8 oct[8];
    Tick done = ri_.reg_read(paddr, oct, t);
    if (size <= 4) {
      std::memcpy(out, oct + half_offset(paddr, upper), 4);
    } else {
      std::memcpy(out, oct, 8);
    }
    return done;
  }
  if (size <= 4) {
    // RI reads whole octbytes; MI forwards the half selected by Addr[2] (or the upper half in
    // Upper mode). n64brew "MIPS Interface", MI's Mode.
    u8 oct[8];
    u8 eb[8];
    Tick done = ri_.read(m, octbyte_base(paddr), oct, 8, t, eb);
    const u32 half = half_offset(paddr, upper);
    std::memcpy(out, oct + half, 4);
    if (MiModeRead::Ebus::test(mode_)) {
      // Guess (ledger "mi-ebus"): the 4 EBus bits replace the low nibble of the word.
      u32 w = load_be32(out) & ~0xFu;
      for (u32 j = 0; j < 4; ++j) w |= u32{eb[half + j] & 1u} << (3 - j);
      store_be32(out, w);
    }
    return done;
  }
  return ri_.read(m, paddr, out, size, t, nullptr, size > 8 ? TraceKind::BlockRead : TraceKind::Read);
}

Tick Mi::cpu_write(Master m, u32 paddr, u32 size, const u8* bus, Tick t) {
  const bool upper = MiModeRead::Upper::test(mode_);
  const bool repeat = MiModeRead::Repeat::test(mode_);

  if (repeat) {
    // The first 64-bit value on DBus becomes the pattern: a doubleword as-is, a word (or
    // smaller, pre-shifted) duplicated into both halves, a cache block's last 8 bytes.
    // Source: n64brew "MIPS Interface", Repeat Mode table.
    u8 pattern[8];
    if (size <= 4) {
      std::memcpy(pattern, bus, 4);
      std::memcpy(pattern + 4, bus, 4);
    } else {
      std::memcpy(pattern, bus + size - 8, 8);
    }
    const u32 rc = MiModeRead::RepeatCount::get(mode_);
    mode_ &= ~MiModeRead::Repeat::mask;  // clears after a single Rambus write
    u8 stream[128];
    if (paddr >= kRdramRegSpace) {
      const u32 n = rc + 1;
      for (u32 i = 0; i < n; ++i) stream[i] = pattern[i % kOctBytes];
      return ri_.reg_write(paddr, stream, n, t);
    }
    // RepeatCount + 1 bytes are counted from the octbyte-aligned base, not from the address:
    // bytes [Addr[2:0], RepeatCount + 1) of the pattern stream are written.
    // Source: n64-systemtest src/tests/mi/repeat.rs (SB/SH/SW/SD at every start offset).
    const u32 a = paddr % kOctBytes;
    const u32 end = rc + 1;
    if (end <= a) return t + kWriteAcceptTicks;
    const u32 len = (end + 7) & ~7u;
    u8 mask[128];
    for (u32 i = 0; i < len; ++i) {
      stream[i] = pattern[i % kOctBytes];
      mask[i] = (i >= a && i < end) ? 1 : 0;
    }
    return ri_.write(m, octbyte_base(paddr), stream, len, t, mask);
  }

  if (paddr >= kRdramRegSpace) {
    // Register writes reach the device with the written word first in the data packet,
    // whatever Addr[2] is: Nintendo's 6102 IPL3 writes the odd registers DeviceId (0x4) and
    // Mode (0xC) without Upper mode and boots on hardware. Only reads of odd registers need
    // Upper mode (IPL3 wraps exactly its register reads with SetUpper/ClearUpper).
    // docs/hardware/memory.md "RDRAM register access" records the evidence.
    u8 oct[8] = {};
    std::memcpy(oct, bus, size <= 4 ? 4 : 8);
    return ri_.reg_write(paddr, oct, 8, t);
  }

  if (size <= 4) {
    u8 oct[8] = {};
    u8 mask[8] = {};
    const u32 half = half_offset(paddr, upper);
    std::memcpy(oct + half, bus, 4);
    for (u32 i = paddr % kOctBytes; i < paddr % kOctBytes + size && i < kOctBytes; ++i) mask[i] = 1;
    if (MiModeRead::Ebus::test(mode_)) {
      // Guess (ledger "mi-ebus"): the low nibble of the word supplies the 9th bits.
      u8 eb[8] = {};
      u32 w = load_be32(bus);
      for (u32 j = 0; j < 4; ++j) eb[half + j] = static_cast<u8>((w >> (3 - j)) & 1);
      return ri_.write(m, octbyte_base(paddr), oct, 8, t, mask, eb);
    }
    return ri_.write(m, octbyte_base(paddr), oct, 8, t, mask);
  }
  if (size <= 8) {
    // Doubleword transfer (or a 5..7-byte SDL/SDR piece): one octbyte with a byte mask.
    u8 mask[8] = {};
    for (u32 i = paddr % kOctBytes; i < paddr % kOctBytes + size && i < kOctBytes; ++i) mask[i] = 1;
    return ri_.write(m, octbyte_base(paddr), bus, 8, t, mask);
  }
  return ri_.write(m, paddr, bus, size, t, nullptr, nullptr, TraceKind::BlockWrite);
}

void Mi::serialize(SaveState& s) {
  s.section("mi", 1);
  s.value(mode_);
  s.value(intr_);
  s.value(mask_);
  if (s.loading()) update_line();
}

}  // namespace ultra
