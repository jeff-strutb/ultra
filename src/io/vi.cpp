#include "io/vi.hpp"

#include "core/savestate.hpp"
#include "io/mi.hpp"
#include "memory/ri.hpp"

namespace ultra {

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
  return u64{h_total_ & 0xFFF} + 1;
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
  if (next > (v_total_ & 0x3FF)) {
    if (interlaced()) field_ ^= 1;
    next = field_;
    ++fields_;
  }
  half_line_ = next;

  {
    // V_INTR match rules (n64brew "Video Interface", VI_V_INTR).
    const u32 cur = half_line_;
    bool fire;
    if (!interlaced() || (v_intr_ & 1)) {
      fire = (cur >> 1) == ((v_intr_ & 0x3FF) >> 1);
    } else {
      // Interlaced with V_INTR bit 0 clear: odd-field interrupts fire one line early.
      u32 nxt = cur + 2;
      u32 nxt_field = field_;
      if (nxt > (v_total_ & 0x3FF)) {
        nxt_field ^= 1;
        nxt = nxt_field;
      }
      fire = (field_ == 0 && cur == (v_intr_ & 0x3FE)) ||
             (nxt_field == 1 && (nxt >> 1) == ((v_intr_ & 0x3FF) >> 1));
    }
    if (fire) mi_.raise(MiIntr::VI);
  }
  schedule_next_line(t);
}

u32 Vi::read_reg(u32 paddr, Tick) {
  switch (static_cast<ViRegister>(paddr & 0x3C)) {
    case ViRegister::Ctrl: return ctrl_;
    case ViRegister::Origin: return origin_;
    case ViRegister::Width: return width_;
    case ViRegister::VIntr: return v_intr_;
    case ViRegister::VCurrent:
      // Fixed to 0 while the VI is off (n64brew, VI_V_CURRENT).
      return ViCtrl::Type::get(ctrl_) ? (half_line_ & 0x3FF) : 0;
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
  switch (static_cast<ViRegister>(paddr & 0x3C)) {
    case ViRegister::Ctrl: ctrl_ = v & 0x1FBFF; break;
    case ViRegister::Origin: origin_ = v & 0x00FF'FFFF; break;
    case ViRegister::Width: width_ = v & 0xFFF; break;
    case ViRegister::VIntr: v_intr_ = v & 0x3FF; break;
    case ViRegister::VCurrent: mi_.lower(MiIntr::VI); break;  // any write acknowledges
    case ViRegister::Burst: burst_ = v & 0x3FFF'FFFF; break;
    case ViRegister::VTotal: v_total_ = v & 0x3FF; break;
    case ViRegister::HTotal: h_total_ = v & 0x1F'0FFF; break;
    case ViRegister::HTotalLeap: h_total_leap_ = v & 0x0FFF'0FFF; break;
    case ViRegister::HVideo: h_video_ = v & 0x03FF'03FF; break;
    case ViRegister::VVideo: v_video_ = v & 0x03FF'03FF; break;
    case ViRegister::VBurst: v_burst_ = v & 0x03FF'03FF; break;
    case ViRegister::XScale: x_scale_ = v & 0x0FFF'0FFF; break;
    case ViRegister::YScale: y_scale_ = v & 0x0FFF'0FFF; break;
    case ViRegister::TestAddr: test_addr_ = v & 0x7F; break;
    case ViRegister::StagedData: staged_data_ = v; break;
  }
}

bool Vi::capture_raw(const u8* rdram, u32 rdram_size, u32 height, std::vector<u32>& out,
                     u32& width) const {
  const u32 type = ViCtrl::Type::get(ctrl_);
  if (type < 2 || width_ == 0) return false;
  width = width_;
  out.assign(usize{width} * height, 0);
  const u32 bpp = type == 3 ? 4 : 2;
  for (u32 y = 0; y < height; ++y) {
    for (u32 x = 0; x < width; ++x) {
      const u32 a = origin_ + (y * width_ + x) * bpp;
      if (a + bpp > rdram_size) continue;
      u32 rgb;
      if (bpp == 4) {
        rgb = load_be32(rdram + a) >> 8;
      } else {
        const u16 p = load_be16(rdram + a);
        auto ex = [](u32 c) { return (c << 3) | (c >> 2); };
        rgb = (ex((p >> 11) & 31) << 16) | (ex((p >> 6) & 31) << 8) | ex((p >> 1) & 31);
      }
      out[usize{y} * width + x] = rgb;
    }
  }
  return true;
}

bool Vi::capture_presented(const u8* rdram, u32 rdram_size, u32 out_w, u32 out_h,
                           std::vector<u32>& out) const {
  const u32 type = ViCtrl::Type::get(ctrl_);
  const u32 xs = x_scale_ & 0xFFF, ys = y_scale_ & 0xFFF;
  if (type < 2 || width_ == 0 || xs == 0 || ys == 0) return false;
  const u32 bpp = type == 3 ? 4 : 2;
  // Displayed framebuffer region: 640 x 240 output samples scaled by X_SCALE / Y_SCALE.
  const u32 fb_w = (640 * xs) >> 10, fb_h = (240 * ys) >> 10;
  out.assign(usize{out_w} * out_h, 0);
  for (u32 y = 0; y < out_h; ++y) {
    const u32 fy = static_cast<u32>(u64{y} * fb_h / out_h);
    for (u32 x = 0; x < out_w; ++x) {
      const u32 fx = static_cast<u32>(u64{x} * fb_w / out_w);
      const u32 a = origin_ + (fy * width_ + fx) * bpp;
      if (a + bpp > rdram_size) continue;
      u32 rgb;
      if (bpp == 4) {
        rgb = load_be32(rdram + a) >> 8;
      } else {
        const u16 p = load_be16(rdram + a);
        auto ex = [](u32 c) { return (c << 3) | (c >> 2); };
        rgb = (ex((p >> 11) & 31) << 16) | (ex((p >> 6) & 31) << 8) | ex((p >> 1) & 31);
      }
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
