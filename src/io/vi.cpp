#include "io/vi.hpp"

#include "core/savestate.hpp"
#include "io/mi.hpp"
#include "memory/ri.hpp"

namespace ultra {

namespace {
// Framebuffer pixel formats selected by VI_CTRL TYPE (n64brew "Video Interface"): 3 = 32-bit
// RGBA 8/8/8/8, 2 = 16-bit RGBA 5/5/5/1.
constexpr u32 kTypeRgba5551 = 2;
constexpr u32 kTypeRgba8888 = 3;
struct Rgba8888 {
  using Rgb = Bits<8, 24>;
};
struct Rgba5551 {
  using R = Bits<11, 5>;
  using G = Bits<6, 5>;
  using B = Bits<1, 5>;
};
// Capture output: 0xRRGGBB.
struct Rgb888 {
  using R = Bits<16, 8>;
  using G = Bits<8, 8>;
  using B = Bits<0, 8>;
};
// A 5-bit channel widened to 8 bits by repeating its top bits below it.
constexpr u32 expand5(u32 c) { return c * 8 + c / 4; }

u32 bytes_per_pixel(u32 type) { return type == kTypeRgba8888 ? 4 : 2; }

u32 pixel_rgb(const u8* p, u32 bpp) {
  if (bpp == 4) return Rgba8888::Rgb::get(load_be32(p));
  const u16 px = load_be16(p);
  return Rgb888::R::place(expand5(Rgba5551::R::get(px))) |
         Rgb888::G::place(expand5(Rgba5551::G::get(px))) |
         Rgb888::B::place(expand5(Rgba5551::B::get(px)));
}

// The krom reference screenshots show 640 x 240 output samples (docs/hardware/io.md,
// "Reference screenshots").
constexpr u32 kPresentedSamplesX = 640;
constexpr u32 kPresentedSamplesY = 240;
}  // namespace

Vi::Vi(Scheduler& sched, Mi& mi, Ri& ri, VideoStandard std)
    : sched_(sched),
      mi_(mi),
      ri_(ri),
      clk_(std == VideoStandard::Pal ? kVclkPal : std == VideoStandard::Mpal ? kVclkMpal : kVclkNtsc) {
  sched_.add(line_ev_, "vi.line", &Vi::line_event, this);
  schedule_next_line(0);
}

u64 Vi::line_ticks_num() const noexcept {
  // A scanline lasts H_TOTAL + 1 VI clocks (n64brew "Video Interface", VI_H_TOTAL: "One less
  // than the total length of a scanline in 1/4 pixel units" with 4 VI clocks per pixel...
  // NTSC: 3094 VI clocks = 227.5 chroma periods x 13.6).
  return u64{ViHTotal::Total::get(h_total_)} + 1;
}

void Vi::schedule_next_line(Tick t) {
  // Exact rational conversion VCLK -> MClock ticks with a carried remainder.
  const u64 num = line_ticks_num() * clk_.tick_den + vclk_remainder_;
  const u64 ticks = num / clk_.vclk_num;
  vclk_remainder_ = num % clk_.vclk_num;
  sched_.schedule(line_ev_, t + (ticks ? ticks : 1));
}

void Vi::on_line(Tick t) {
  // HSYNC: RDRAM refresh (n64brew "RDRAM Interface", RI_REFRESH Extra Details).
  ri_.hsync(t);

  // While the VI is off the line counter (and so the field) stays at 0; HSYNC keeps running
  // for refresh (n64brew "Video Interface", VI_V_CURRENT: "When the VI is turned off ... this
  // register is fixed to 0, and never changes"; "RDRAM Interface", RI_REFRESH).
  if (ViCtrl::Type::get(ctrl_) == 0) {
    half_line_ = 0;
    field_ = 0;
    schedule_next_line(t);
    return;
  }
  // Advance the half-line counter by one line (2 half-lines) and handle field wrap.
  // Source: n64brew "Video Interface", VI_V_CURRENT.
  u32 next = half_line_ + 2;
  if (next > ViVTotal::Total::get(v_total_)) {
    if (interlaced()) field_ ^= 1;
    next = field_;
    ++fields_;
  }
  half_line_ = next;

  {
    // V_INTR match rules (n64brew "Video Interface", VI_V_INTR).
    const u32 cur = half_line_;
    bool fire;
    if (!interlaced() || ViVIntr::Field::test(v_intr_)) {
      fire = ViVIntr::Line::get(cur) == ViVIntr::Line::get(v_intr_);
    } else {
      // Interlaced with V_INTR bit 0 clear: odd-field interrupts fire one line early.
      u32 nxt = cur + 2;
      u32 nxt_field = field_;
      if (nxt > ViVTotal::Total::get(v_total_)) {
        nxt_field ^= 1;
        nxt = nxt_field;
      }
      fire = (field_ == 0 && cur == (v_intr_ & ViVIntr::Line::mask)) ||
             (nxt_field == 1 && ViVIntr::Line::get(nxt) == ViVIntr::Line::get(v_intr_));
    }
    if (fire) mi_.raise(MiIntr::VI);
  }
  schedule_next_line(t);
}

u32 Vi::read_reg(u32 paddr, Tick) {
  switch (static_cast<ViRegister>(paddr & kViRegOffsetMask)) {
    case ViRegister::Ctrl: return ctrl_;
    case ViRegister::Origin: return origin_;
    case ViRegister::Width: return width_;
    case ViRegister::VIntr: return v_intr_;
    case ViRegister::VCurrent:
      // Fixed to 0 while the VI is off (n64brew, VI_V_CURRENT).
      return ViCtrl::Type::get(ctrl_) ? ViVIntr::HalfLine::get(half_line_) : 0;
    case ViRegister::Burst: return burst_;
    case ViRegister::VTotal: return v_total_;
    case ViRegister::HTotal: return h_total_;
    case ViRegister::HTotalLeap: return h_total_leap_;
    case ViRegister::HVideo: return h_video_;
    case ViRegister::VVideo: return v_video_;
    case ViRegister::VBurst: return v_burst_;
    case ViRegister::XScale: return x_scale_;
    case ViRegister::YScale: return y_scale_;
    case ViRegister::TestAddr: return test_addr_;
    case ViRegister::StagedData: return staged_data_;
  }
  return 0;
}

void Vi::write_reg(u32 paddr, u32 v, Tick) {
  switch (static_cast<ViRegister>(paddr & kViRegOffsetMask)) {
    case ViRegister::Ctrl: ctrl_ = v & ViCtrl::kWriteMask; break;
    case ViRegister::Origin: origin_ = v & ViOrigin::Origin::mask; break;
    case ViRegister::Width: width_ = v & ViWidth::Width::mask; break;
    case ViRegister::VIntr: v_intr_ = v & ViVIntr::HalfLine::mask; break;
    case ViRegister::VCurrent: mi_.lower(MiIntr::VI); break;  // any write acknowledges
    case ViRegister::Burst: burst_ = v & ViBurst::kWriteMask; break;
    case ViRegister::VTotal: v_total_ = v & ViVTotal::Total::mask; break;
    case ViRegister::HTotal: h_total_ = v & ViHTotal::kWriteMask; break;
    case ViRegister::HTotalLeap: h_total_leap_ = v & ViHTotalLeap::kWriteMask; break;
    case ViRegister::HVideo: h_video_ = v & ViStartEnd::kWriteMask; break;
    case ViRegister::VVideo: v_video_ = v & ViStartEnd::kWriteMask; break;
    case ViRegister::VBurst: v_burst_ = v & ViStartEnd::kWriteMask; break;
    case ViRegister::XScale: x_scale_ = v & ViScale::kWriteMask; break;
    case ViRegister::YScale: y_scale_ = v & ViScale::kWriteMask; break;
    case ViRegister::TestAddr: test_addr_ = v & ViTestAddr::Addr::mask; break;
    case ViRegister::StagedData: staged_data_ = v; break;
  }
}

bool Vi::capture_raw(const u8* rdram, u32 rdram_size, u32 height, std::vector<u32>& out,
                     u32& width) const {
  const u32 type = ViCtrl::Type::get(ctrl_);
  if (type < kTypeRgba5551 || width_ == 0) return false;
  width = width_;
  out.assign(usize{width} * height, 0);
  const u32 bpp = bytes_per_pixel(type);
  for (u32 y = 0; y < height; ++y) {
    for (u32 x = 0; x < width; ++x) {
      const u32 a = origin_ + (y * width_ + x) * bpp;
      if (a + bpp > rdram_size) continue;
      const u32 rgb = pixel_rgb(rdram + a, bpp);
      out[usize{y} * width + x] = rgb;
    }
  }
  return true;
}

bool Vi::capture_presented(const u8* rdram, u32 rdram_size, u32 out_w, u32 out_h,
                           std::vector<u32>& out) const {
  const u32 type = ViCtrl::Type::get(ctrl_);
  const u32 xs = ViScale::Scale::get(x_scale_), ys = ViScale::Scale::get(y_scale_);
  if (type < kTypeRgba5551 || width_ == 0 || xs == 0 || ys == 0) return false;
  const u32 bpp = bytes_per_pixel(type);
  // Displayed framebuffer region: the output samples scaled by X_SCALE / Y_SCALE.
  const u32 fb_w = kPresentedSamplesX * xs / ViScale::kOne;
  const u32 fb_h = kPresentedSamplesY * ys / ViScale::kOne;
  out.assign(usize{out_w} * out_h, 0);
  for (u32 y = 0; y < out_h; ++y) {
    const u32 fy = static_cast<u32>(u64{y} * fb_h / out_h);
    for (u32 x = 0; x < out_w; ++x) {
      const u32 fx = static_cast<u32>(u64{x} * fb_w / out_w);
      const u32 a = origin_ + (fy * width_ + fx) * bpp;
      if (a + bpp > rdram_size) continue;
      const u32 rgb = pixel_rgb(rdram + a, bpp);
      out[usize{y} * out_w + x] = rgb;
    }
  }
  return true;
}

void Vi::serialize(SaveState& s) {
  s.section("vi", 1);
  s.value(ctrl_);
  s.value(origin_);
  s.value(width_);
  s.value(v_intr_);
  s.value(v_current_);
  s.value(burst_);
  s.value(v_total_);
  s.value(h_total_);
  s.value(h_total_leap_);
  s.value(h_video_);
  s.value(v_video_);
  s.value(v_burst_);
  s.value(x_scale_);
  s.value(y_scale_);
  s.value(test_addr_);
  s.value(staged_data_);
  s.value(half_line_);
  s.value(field_);
  s.value(fields_);
  s.value(vclk_remainder_);
}

}  // namespace ultra
