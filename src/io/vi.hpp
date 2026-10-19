#pragma once
// Video Interface: registers, scanline timing in the VI clock domain, V_CURRENT / field /
// V_INTR behaviour, HSYNC-driven RDRAM refresh, and framebuffer capture for tests.
// Facts and sources: docs/hardware/io.md (VI). Primary: n64brew "Video Interface",
// "Clock Timing". Scanout filtering (AA, gamma, divot, dedither) arrives with the VI phase.

#include <array>
#include <vector>

#include "common/types.hpp"
#include "core/clock.hpp"
#include "core/scheduler.hpp"

namespace ultra {

class Mi;
class Ri;
class SaveState;

enum class ViRegister : u32 {
  Ctrl = 0x00,
  Origin = 0x04,
  Width = 0x08,
  VIntr = 0x0C,
  VCurrent = 0x10,
  Burst = 0x14,
  VTotal = 0x18,
  HTotal = 0x1C,
  HTotalLeap = 0x20,
  HVideo = 0x24,
  VVideo = 0x28,
  VBurst = 0x2C,
  XScale = 0x30,
  YScale = 0x34,
  TestAddr = 0x38,
  StagedData = 0x3C,
};

// Register offset within the VI block: 16 registers, 4 bytes apart (mirrored above).
inline constexpr u32 kViRegOffsetMask = 0x3C;

// Register layouts and writable bits. Source: n64brew "Video Interface", the register table of
// each register (bits marked Undefined are not writable).
struct ViCtrl {
  using Type = Bits<0, 2>;
  using GammaDither = Bit<2>;
  using Gamma = Bit<3>;
  using Divot = Bit<4>;
  using VbusClock = Bit<5>;
  using Serrate = Bit<6>;
  using TestMode = Bit<7>;
  using AaMode = Bits<8, 2>;
  using KillWe = Bit<11>;
  using PixelAdvance = Bits<12, 4>;
  using Dedither = Bit<16>;
  static constexpr u32 kWriteMask = Type::mask | GammaDither::mask | Gamma::mask | Divot::mask |
                                    VbusClock::mask | Serrate::mask | TestMode::mask |
                                    AaMode::mask | KillWe::mask | PixelAdvance::mask |
                                    Dedither::mask;
};
struct ViOrigin {
  using Origin = Bits<0, 24>;
};
struct ViWidth {
  using Width = Bits<0, 12>;
};
struct ViVIntr {  // also the layout of VI_V_CURRENT: a half-line number
  using HalfLine = Bits<0, 10>;
  using Line = Bits<1, 9>;
  using Field = Bit<0>;
};
struct ViBurst {
  using HsyncWidth = Bits<0, 8>;
  using BurstWidth = Bits<8, 8>;
  using VsyncHeight = Bits<16, 4>;
  using BurstStart = Bits<20, 10>;
  static constexpr u32 kWriteMask =
      HsyncWidth::mask | BurstWidth::mask | VsyncHeight::mask | BurstStart::mask;
};
struct ViVTotal {
  using Total = Bits<0, 10>;
};
struct ViHTotal {
  using Total = Bits<0, 12>;
  using Leap = Bits<16, 5>;
  static constexpr u32 kWriteMask = Total::mask | Leap::mask;
};
struct ViHTotalLeap {
  using LeapB = Bits<0, 12>;
  using LeapA = Bits<16, 12>;
  static constexpr u32 kWriteMask = LeapB::mask | LeapA::mask;
};
struct ViStartEnd {  // VI_H_VIDEO, VI_V_VIDEO, VI_V_BURST
  using End = Bits<0, 10>;
  using Start = Bits<16, 10>;
  static constexpr u32 kWriteMask = End::mask | Start::mask;
};
struct ViScale {  // VI_X_SCALE, VI_Y_SCALE (Y keeps bits 27:26 too: "holds state")
  using Scale = Bits<0, 12>;  // 2.10 fixed point
  using Offset = Bits<16, 12>;
  static constexpr u32 kWriteMask = Scale::mask | Offset::mask;
  static constexpr u32 kOne = 1u << 10;  // 1.0 in 2.10
};
struct ViTestAddr {
  using Addr = Bits<0, 7>;
};
static_assert(ViCtrl::kWriteMask == 0x1FBFF && ViBurst::kWriteMask == 0x3FFF'FFFF &&
              ViHTotal::kWriteMask == 0x1F'0FFF && ViHTotalLeap::kWriteMask == 0x0FFF'0FFF &&
              ViStartEnd::kWriteMask == 0x03FF'03FF && ViScale::kWriteMask == 0x0FFF'0FFF);

enum class VideoStandard : u8 { Ntsc, Pal, Mpal };

class Vi {
 public:
  Vi(Scheduler& sched, Mi& mi, Ri& ri, VideoStandard std);

  u32 read_reg(u32 paddr, Tick t);
  void write_reg(u32 paddr, u32 value, Tick t);

  // Number of fields (vertical periods) completed since power on.
  u64 field_count() const noexcept { return fields_; }

  // Raw framebuffer capture (no VI filtering): converts the framebuffer at ORIGIN with WIDTH
  // and TYPE into 0xRRGGBB values. Returns false if the VI is off.
  bool capture_raw(const u8* rdram, u32 rdram_size, u32 height, std::vector<u32>& out,
                   u32& width) const;
  // Presentation used by the krom reference screenshots: the displayed framebuffer region
  // (640 x 240 samples scaled by X_SCALE / Y_SCALE, 2.10 fixed point) resampled
  // nearest-neighbour to out_w x out_h, with no VI filtering. This is a test-oracle convention
  // (docs/hardware/io.md "Reference screenshots"), not a model of the VI's analog output,
  // which arrives with the VI phase.
  bool capture_presented(const u8* rdram, u32 rdram_size, u32 out_w, u32 out_h,
                         std::vector<u32>& out) const;
  u32 origin() const noexcept { return origin_; }
  u32 width_reg() const noexcept { return width_; }
  u32 ctrl() const noexcept { return ctrl_; }

  void serialize(SaveState& s);

 private:
  u64 line_ticks_num() const noexcept;  // VCLK cycles per line (H_TOTAL + 1)
  void on_line(Tick t);
  static void line_event(void* ctx, Tick t) { static_cast<Vi*>(ctx)->on_line(t); }
  void schedule_next_line(Tick t);
  bool interlaced() const noexcept { return (v_total_ & 1) == 0; }

  Scheduler& sched_;
  Mi& mi_;
  Ri& ri_;
  VideoClockRatio clk_;  // not state: fixed by the TV standard (identity section)

  u32 ctrl_ = 0;
  u32 origin_ = 0;
  u32 width_ = 0;
  u32 v_intr_ = 0x3FF;   // default (n64brew VI_V_INTR)
  u32 v_current_ = 0;    // half-line counter (bits 9:1) and field (bit 0)
  u32 burst_ = 0x01;     // default HSYNC_WIDTH = 1
  u32 v_total_ = 0;
  u32 h_total_ = 0x7FF;  // default (n64brew VI_H_TOTAL)
  u32 h_total_leap_ = 0;
  u32 h_video_ = 0;
  u32 v_video_ = 0;
  u32 v_burst_ = 0;
  u32 x_scale_ = 0;
  u32 y_scale_ = 0;
  u32 test_addr_ = 0;
  u32 staged_data_ = 0;

  u32 half_line_ = 0;   // internal half-line position (0 .. V_TOTAL)
  u32 field_ = 0;
  u64 fields_ = 0;
  u64 vclk_remainder_ = 0;  // fractional VCLK->tick conversion carry (exact rational)
  Event line_ev_;  // not state: saved by the Scheduler
};

}  // namespace ultra
