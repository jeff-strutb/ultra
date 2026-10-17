#pragma once
// Audio Interface: double-buffered DMA paced by the DAC sample clock (VCLK / (DACRATE + 1)),
// interrupt at buffer start, the 8 KiB delayed-carry bug, and a sample sink for the host.
// Facts and sources: docs/hardware/io.md (AI). Primary: n64brew "Audio Interface".

#include <array>

#include "common/types.hpp"
#include "core/clock.hpp"
#include "core/scheduler.hpp"
#include "io/vi.hpp"

namespace ultra {

class Mi;
class Ri;
class SaveState;

class Ai {
 public:
  Ai(Scheduler& sched, Mi& mi, Ri& ri, VideoStandard std);

  u32 read_reg(u32 paddr, Tick t);
  void write_reg(u32 paddr, u32 value, Tick t);

  // Samples (16-bit stereo, big-endian pairs) delivered since the last drain. The ring is
  // allocated once; the host audio thread consumes it (Phase 4).
  u64 samples_played() const noexcept { return samples_; }

  void serialize(SaveState& s);

 private:
  struct Buffer {
    u32 addr = 0;
    u32 len = 0;
  };
  void start_next(Tick t);
  void on_fetch(Tick t);
  static void fetch_event(void* ctx, Tick t) { static_cast<Ai*>(ctx)->on_fetch(t); }
  Tick period_ticks(u32 samples);

  Scheduler& sched_;
  Mi& mi_;
  Ri& ri_;
  VideoClockRatio clk_;

  u32 dram_addr_ = 0;
  u32 control_ = 0;
  u32 dacrate_ = 0;
  u32 bitrate_ = 0;

  Buffer cur_{};
  Buffer next_{};
  bool busy_ = false;
  bool full_ = false;
  bool carry_ = false;  // delayed carry into the next buffer's address
  u64 vclk_remainder_ = 0;
  u64 samples_ = 0;
  Event fetch_ev_;
};

}  // namespace ultra
