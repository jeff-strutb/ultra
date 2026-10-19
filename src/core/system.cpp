#include "core/system.hpp"

#include <stdexcept>

#include "core/savestate.hpp"
#include "io/pif_boot_rom.hpp"

namespace ultra {

namespace {

// Console region implied by the ROM header country code (n64brew "ROM Header", byte 0x3E).
VideoStandard tv_from_region(char code) {
  switch (code) {
    case 'D': case 'F': case 'I': case 'P': case 'S': case 'U': case 'X': case 'Y':
    case 'H': case 'L': case 'W':
      return VideoStandard::Pal;
    case 'B':
      return VideoStandard::Mpal;
    default:
      return VideoStandard::Ntsc;
  }
}

// Granularity at which the run loop re-checks PIF-driven CPU holds.
constexpr Tick kSliceTicks = 4096;

}  // namespace

System::System(const SystemConfig& cfg, std::unique_ptr<Cartridge> cart)
    : cfg_(cfg),
      cart_(std::move(cart)),
      cic_(CicType::Cic6102),
      tv_(VideoStandard::Ntsc),
      rdram_(cfg.rdram_bytes),
      ri_(rdram_, arbiter_, trace_),
      mi_(ri_, pins_),
      pif_(sched_, pins_),
      si_(sched_, mi_, ri_, pif_),
      pi_(sched_, mi_, ri_, cart_.get()),
      sp_(sched_, mi_, ri_),
      vi_(sched_, mi_, ri_, cfg.tv.value_or(cart_ ? tv_from_region(cart_->region_code()) : VideoStandard::Ntsc)),
      ai_(sched_, mi_, ri_, cfg.tv.value_or(cart_ ? tv_from_region(cart_->region_code()) : VideoStandard::Ntsc)),
      bus_(BusDevices{&sched_, &mi_, &ri_, &pi_, &si_, &sp_, &dp_, &vi_, &ai_, &trace_}),
      cpu_(bus_, sched_, pins_) {
  if (!cart_) throw std::invalid_argument("System requires a cartridge");
  rom_hash_ = 14695981039346656037ull;  // FNV-1a 64
  for (const u8 b : cart_->rom()) rom_hash_ = (rom_hash_ ^ b) * 1099511628211ull;
  tv_ = cfg.tv.value_or(tv_from_region(cart_->region_code()));
  const bool pal = tv_ == VideoStandard::Pal;
  if (cfg.cic) {
    cic_ = *cfg.cic;
  } else if (auto detected = detect_cic(cart_->rom(), pal)) {
    cic_ = *detected;
  } else {
    // No retail CIC accepts this IPL3; a real console with a 6102 cartridge would halt in
    // IPL2. Keep 6102 so the PIF performs (and fails) the same check.
    cic_ = pal ? CicType::Cic7101 : CicType::Cic6102;
  }
  if (!cfg.trace_path.empty() && !trace_.open(cfg.trace_path))
    throw std::runtime_error("cannot open trace file " + cfg.trace_path);
}

System::~System() { trace_.close(); }

void System::power_on() {
  std::optional<u64> checksum;
  if (cfg_.cic_checksum_from_ipl3) {
    const auto rom = cart_->rom();
    if (rom.size() >= 0x1000) checksum = ipl2_checksum(rom.subspan(0x40, 0xFC0), cic_info(cic_).seed);
  }
  pif_.power_on(cic_, tv_, make_pif_boot_rom(), checksum);
  cpu_.cold_reset();
}

void System::run_until(Tick limit) {
  while (sched_.now() < limit) {
    const Tick slice = std::min(limit, sched_.now() + kSliceTicks);
    if (pif_.halted_cpu()) cpu_.freeze();
    if (!cpu_.frozen()) {
      cpu_.run_until(slice);
      // The CPU may stop just past `slice`; never let device time pass the CPU.
      const Tick cpu_now = cpu_.now_tick();
      sched_.run_until(std::min(slice, cpu_now));
      if (cpu_now >= slice) continue;
      // CPU froze mid-slice: let devices continue on their own.
    }
    sched_.run_until(slice);
  }
}

void System::run_fields(u64 n, Tick max_ticks) {
  const u64 target = vi_.field_count() + n;
  const Tick end = sched_.now() + max_ticks;
  while (vi_.field_count() < target && sched_.now() < end) run_until(std::min(end, sched_.now() + 65536));
}

bool System::capture_framebuffer(u32 height, std::vector<u32>& rgb, u32& width) {
  u32 avail = 0;
  const u8* base = rdram_.resolve(0, &avail);
  if (!base) return false;
  return vi_.capture_raw(base, static_cast<u32>(rdram_.installed_bytes()), height, rgb, width);
}

bool System::capture_presented(u32 out_w, u32 out_h, std::vector<u32>& rgb) {
  u32 avail = 0;
  const u8* base = rdram_.resolve(0, &avail);
  if (!base) return false;
  return vi_.capture_presented(base, static_cast<u32>(rdram_.installed_bytes()), out_w, out_h, rgb);
}

void System::serialize(SaveState& s) {
  s.header();
  // The state is only meaningful for the machine that made it (ADR 0003): same ROM, CIC and TV
  // standard (which sets the VI/AI clock ratios).
  s.section("system", 1);
  u64 rom_hash = rom_hash_;
  CicType cic = cic_;
  VideoStandard tv = tv_;
  s.value(rom_hash);
  s.enumeration(cic, CicType::Cic7106);
  s.enumeration(tv, VideoStandard::Mpal);
  if (rom_hash != rom_hash_) throw std::runtime_error("save state: made with a different cartridge ROM");
  if (cic != cic_) throw std::runtime_error("save state: made with a different CIC");
  if (tv != tv_) throw std::runtime_error("save state: made with a different TV standard");
  sched_.serialize(s);
  rdram_.serialize(s);
  arbiter_.serialize(s);
  ri_.serialize(s);
  mi_.serialize(s);
  pif_.serialize(s);
  si_.serialize(s);
  pi_.serialize(s);
  sp_.serialize(s);
  dp_.serialize(s);
  vi_.serialize(s);
  ai_.serialize(s);
  cart_->serialize(s);
  cpu_.serialize(s);
  s.section("pins", 1);
  s.value(pins_.int_lines);
  s.value(pins_.nmi);
  s.finish();
}

std::vector<u8> System::save_state() {
  SaveState s = SaveState::saver();
  serialize(s);
  return s.data();
}

void System::load_state(const std::vector<u8>& data) {
  // All or nothing: a load that fails partway (truncated, mismatched, invalid field) would
  // leave earlier modules overwritten, so the machine is restored from a copy taken first.
  const std::vector<u8> before = save_state();
  try {
    SaveState s = SaveState::loader(data);
    serialize(s);
  } catch (...) {
    SaveState r = SaveState::loader(before);
    serialize(r);
    throw;
  }
}

}  // namespace ultra
