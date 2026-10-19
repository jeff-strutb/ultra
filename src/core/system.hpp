#pragma once
// The whole machine on the timing thread: owns every component, wires them together, and runs
// the CPU-driven event loop. See docs/adr/0001-scheduler.md.

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/bus.hpp"
#include "core/cpu_pins.hpp"
#include "core/scheduler.hpp"
#include "core/trace.hpp"
#include "cpu/vr4300.hpp"
#include "io/ai.hpp"
#include "io/cart.hpp"
#include "io/cic.hpp"
#include "io/mi.hpp"
#include "io/pi.hpp"
#include "io/pif.hpp"
#include "io/si.hpp"
#include "io/vi.hpp"
#include "memory/base_rdram_arbiter.hpp"
#include "memory/rdram.hpp"
#include "memory/ri.hpp"
#include "rdp/dp_interface.hpp"
#include "rsp/sp_interface.hpp"

namespace ultra {

class SaveState;

struct SystemConfig {
  u32 rdram_bytes = 8u << 20;      // 4 MiB console, 8 MiB with Expansion Pak
  std::optional<VideoStandard> tv;  // default: from the ROM header's region code
  std::optional<CicType> cic;       // default: detected from IPL3 like the PIF does
  std::string trace_path;           // empty: no trace
};

class System {
 public:
  System(const SystemConfig& cfg, std::unique_ptr<Cartridge> cart);
  ~System();
  System(const System&) = delete;
  System& operator=(const System&) = delete;

  // Cold boot: PIF ROM image, CIC handshake, CPU cold reset at 0xBFC0_0000.
  void power_on();

  // Run until the master clock reaches `limit`. Returns early only if nothing can ever
  // change again (CPU frozen and no pending events).
  void run_until(Tick limit);
  // Run until the VI has completed `n` more fields (or `max_ticks` elapse).
  void run_fields(u64 n, Tick max_ticks);

  Tick now() const noexcept { return sched_.now(); }
  Vr4300& cpu() noexcept { return cpu_; }
  Cartridge& cart() noexcept { return *cart_; }
  Rdram& rdram() noexcept { return rdram_; }
  Vi& vi() noexcept { return vi_; }
  Mi& mi() noexcept { return mi_; }
  Pif& pif() noexcept { return pif_; }
  Ri& ri() noexcept { return ri_; }
  Pi& pi() noexcept { return pi_; }
  SpInterface& sp() noexcept { return sp_; }
  TraceSink& trace() noexcept { return trace_; }
  const IArbiter& arbiter() const noexcept { return arbiter_; }
  CicType cic() const noexcept { return cic_; }
  VideoStandard tv() const noexcept { return tv_; }

  // Raw framebuffer capture through the VI's current settings (no VI filtering).
  bool capture_framebuffer(u32 height, std::vector<u32>& rgb, u32& width);
  // Framebuffer resampled to out_w x out_h the way the krom reference screenshots present it.
  bool capture_presented(u32 out_w, u32 out_h, std::vector<u32>& rgb);

  std::vector<u8> save_state();
  void load_state(const std::vector<u8>& data);

 private:
  void serialize(SaveState& s);

  SystemConfig cfg_;  // not state: construction input; the identity section checks it
  std::unique_ptr<Cartridge> cart_;
  CicType cic_;
  VideoStandard tv_;
  u64 rom_hash_;  // FNV-1a over the cartridge ROM: a save state names the ROM it was made from

  TraceSink trace_;  // not state: host-side output
  Scheduler sched_;
  CpuPins pins_;
  Rdram rdram_;
  BaseRdramArbiter arbiter_;
  Ri ri_;
  Mi mi_;
  Pif pif_;
  Si si_;
  Pi pi_;
  SpInterface sp_;
  DpInterface dp_;
  Vi vi_;
  Ai ai_;
  Bus bus_;  // not state: device pointers only
  Vr4300 cpu_;
};

}  // namespace ultra
