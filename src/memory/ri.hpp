#pragma once
// RDRAM Interface: RI registers, CPU-physical to channel address translation, bank status
// tracking, error flags, and the single entry point every RDRAM requester goes through.
// Each transaction is charged to the arbiter and recorded in the trace.

#include <array>

#include "common/types.hpp"
#include "core/clock.hpp"
#include "core/trace.hpp"
#include "memory/arbiter.hpp"
#include "memory/rdram.hpp"

namespace ultra {

class SaveState;

// Register layouts. Source: n64brew "RDRAM Interface", Registers.
struct RiMode {
  using OpMode = Bits<0, 2>;
  using StopT = Bit<2>;
  using StopR = Bit<3>;
  static constexpr u32 kMask = 0xF;
};
struct RiConfig {
  using CC = Bits<0, 6>;
  using AutoCC = Bit<6>;
  static constexpr u32 kMask = 0x7F;
};
struct RiSelect {
  using RSel = Bits<0, 4>;
  using TSel = Bits<4, 4>;
  static constexpr u32 kMask = 0xFF;
};
struct RiRefresh {
  using CleanDelay = Bits<0, 8>;
  using DirtyDelay = Bits<8, 8>;
  using Bank = Bit<16>;
  using Enable = Bit<17>;
  using Optimize = Bit<18>;
  using MultiBank = Bits<19, 4>;
  static constexpr u32 kMask = 0x7FFFFF;
};
struct RiError {
  using MissingAck = Bit<0>;
  using UnexpectedNack = Bit<1>;
  using OverRange = Bit<2>;
};

enum class RiRegister : u32 {
  Mode = 0x00,
  Config = 0x04,
  CurrentLoad = 0x08,
  Select = 0x0C,
  Refresh = 0x10,
  Latency = 0x14,
  Error = 0x18,
  BankStatus = 0x1C,
};

class Ri {
 public:
  Ri(Rdram& rdram, IArbiter& arbiter, TraceSink& trace);

  // ---- RI registers (CPU via the RCP register bus) ----------------------------------------
  u32 read_reg(u32 paddr);
  void write_reg(u32 paddr, u32 value);

  // ---- channel transactions ------------------------------------------------------------------
  // `paddr` is a CPU-physical address in 0x0000_0000..0x03FF_FFFF. Transfers are at most one
  // RI burst (128 bytes) and must not cross a 2 KiB row. Returns the completion tick.
  Tick read(Master m, u32 paddr, u8* out, u32 len, Tick t, u8* ebits_out = nullptr,
            TraceKind kind = TraceKind::Read);
  Tick write(Master m, u32 paddr, const u8* data, u32 len, Tick t, const u8* mask = nullptr,
             const u8* ebits = nullptr, TraceKind kind = TraceKind::Write);

  // Register-space octbyte transactions (CPU via MI only).
  Tick reg_read(u32 paddr, u8 out[8], Tick t);
  Tick reg_write(u32 paddr, const u8* stream, u32 stream_len, Tick t);

  // VI HSYNC drives refresh when RI_REFRESH.En is set.
  void hsync(Tick t);

  Rdram& rdram() noexcept { return rdram_; }
  IArbiter& arbiter() noexcept { return arbiter_; }
  u32 select() const noexcept { return select_; }

  void serialize(SaveState& s);

 private:
  void track_bank(u32 paddr, bool write);
  void trace(Master m, TraceKind k, u32 addr, const u8* data, u32 len, Tick t, Tick done);

  Rdram& rdram_;
  IArbiter& arbiter_;
  TraceSink& trace_;

  u32 mode_ = RiMode::StopT::mask | RiMode::StopR::mask;
  u32 config_ = 0;
  u32 select_ = 0;
  u32 refresh_ = 0;
  u32 latency_ = 0xF;  // n64brew: "Defaults to 0xf"
  u32 error_ = 0;
  u32 bank_valid_ = 0;
  u32 bank_dirty_ = 0;
  std::array<u32, 8> bank_row_{};
};

}  // namespace ultra
