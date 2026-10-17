#include "io/ai.hpp"

#include "core/savestate.hpp"
#include "io/mi.hpp"
#include "memory/ri.hpp"

namespace ultra {

namespace {
enum class AiReg : u32 { DramAddr = 0x00, Length = 0x04, Control = 0x08, Status = 0x0C,
                         DacRate = 0x10, BitRate = 0x14 };
// The DMA fetches one octbyte (two stereo 16-bit samples) at a time.
// Guess (ledger "ai-fetch"): the fetch granularity is unmeasured; probe "ai-dma" (Phase 1).
constexpr u32 kFetchBytes = 8;
constexpr u32 kBytesPerSample = 4;
}  // namespace

Ai::Ai(Scheduler& sched, Mi& mi, Ri& ri, VideoStandard std)
    : sched_(sched),
      mi_(mi),
      ri_(ri),
      clk_(std == VideoStandard::Pal ? kVclkPal : std == VideoStandard::Mpal ? kVclkMpal : kVclkNtsc) {
  sched_.add(fetch_ev_, "ai.fetch", &Ai::fetch_event, this);
}

Tick Ai::period_ticks(u32 samples) {
  // One sample every DACRATE + 1 VI clocks (n64brew "Audio Interface", AI_DACRATE).
  const u64 num = u64{samples} * (dacrate_ + 1) * clk_.tick_den + vclk_remainder_;
  vclk_remainder_ = num % clk_.vclk_num;
  const u64 t = num / clk_.vclk_num;
  return t ? t : 1;
}

u32 Ai::read_reg(u32 paddr, Tick) {
  const auto reg = static_cast<AiReg>(paddr & 0x1C);
  if (reg == AiReg::Status) {
    // Bits 31/0 FULL, 30 BUSY, 25 ENABLED, 24 reads 1 (n64brew AI_STATUS). COUNT, BC and
    // WC are not modelled yet (ledger "ai-status-count").
    u32 s = 1u << 24;
    if (full_) s |= (1u << 31) | 1u;
    if (busy_) s |= 1u << 30;
    if (control_ & 1) s |= 1u << 25;
    return s;
  }
  // AI_LENGTH returns the bytes remaining; the write-only registers mirror it.
  return busy_ ? cur_.len : 0;
}

void Ai::write_reg(u32 paddr, u32 v, Tick t) {
  switch (static_cast<AiReg>(paddr & 0x1C)) {
    case AiReg::DramAddr: dram_addr_ = v & 0x00FF'FFF8; break;
    case AiReg::Length: {
      const u32 len = v & 0x3FFF8;
      if (len == 0) break;
      Buffer b{dram_addr_, len};
      if (!busy_) {
        next_ = b;
        full_ = true;
        start_next(t);
      } else {
        next_ = b;
        full_ = true;
      }
      break;
    }
    case AiReg::Control: control_ = v & 1; break;
    case AiReg::Status: mi_.lower(MiIntr::AI); break;
    case AiReg::DacRate: dacrate_ = v & 0x3FFF; break;
    case AiReg::BitRate: bitrate_ = v & 0xF; break;
  }
}

void Ai::start_next(Tick t) {
  if (!full_) {
    busy_ = false;
    return;
  }
  cur_ = next_;
  full_ = false;
  if (carry_) {
    // Delayed carry: a buffer that ended on an 8 KiB boundary adds 0x2000 to the next
    // buffer's address (n64brew "Audio Interface", Delayed-carry hardware bug).
    cur_.addr = (cur_.addr + 0x2000) & 0x00FF'FFF8;
    carry_ = false;
  }
  busy_ = true;
  // The interrupt fires when a buffer starts playing.
  mi_.raise(MiIntr::AI);
  sched_.schedule(fetch_ev_, t + period_ticks(kFetchBytes / kBytesPerSample));
}

void Ai::on_fetch(Tick t) {
  if (!(control_ & 1)) {
    // DMA disabled: playback pauses until AI_CONTROL is set again.
    sched_.schedule(fetch_ev_, t + period_ticks(kFetchBytes / kBytesPerSample));
    return;
  }
  u8 buf[kFetchBytes];
  ri_.read(Master::Ai, cur_.addr, buf, kFetchBytes, t);
  samples_ += kFetchBytes / kBytesPerSample;
  cur_.addr = (cur_.addr + kFetchBytes) & 0x00FF'FFFF;
  cur_.len -= kFetchBytes;
  if (cur_.len == 0) {
    if ((cur_.addr & 0x1FFF) == 0) carry_ = true;
    start_next(t);
    return;
  }
  sched_.schedule(fetch_ev_, t + period_ticks(kFetchBytes / kBytesPerSample));
}

void Ai::serialize(SaveState& s) {
  s.section("ai", 1);
  s.value(dram_addr_);
  s.value(control_);
  s.value(dacrate_);
  s.value(bitrate_);
  s.value(cur_.addr);
  s.value(cur_.len);
  s.value(next_.addr);
  s.value(next_.len);
  s.value(busy_);
  s.value(full_);
  s.value(carry_);
  s.value(vclk_remainder_);
  s.value(samples_);
}

}  // namespace ultra
