#include "memory/rdram.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "core/savestate.hpp"
#include "memory/arbiter_params.h"

namespace ultra {

namespace {
// DeviceType as read from a retail 2 MiB module: ColumnBits=11, Bn=1 (9-bit bytes),
// BankBits=1, RowBits=9, Version=1 (Base RDRAM), Type=0.
// Source: n64brew "RDRAM Interface", hardware dump shows 0xb4190010 as DeviceType.
constexpr u32 kDeviceType = 0xB4190010;

// MinInterval read-only fields MinAckDelay=3, MinReadDelay=7, MinWriteDelay=1, scattered
// over bits 31/23/15/7, 30/22/14/6, 29/21/13/5.
// Source: n64brew "RDRAM", 0x07 MinInterval default bit values.
using MinAckDelay = SlicedField<7, 4>;
using MinReadDelay = SlicedField<6, 4>;
using MinWriteDelay = SlicedField<5, 4>;
constexpr u32 kMinIntervalRO =
    MinAckDelay::place(3) | MinReadDelay::place(7) | MinWriteDelay::place(1);
static_assert(kMinIntervalRO == 0x0040'C0E0);

// DeviceManufacturer: NEC (0x0005), ManufactureCode unknown.
// Guess (ledger "rdram-manufacturer"): the value differs per console; no software is known
// to depend on it.
constexpr u32 kDeviceManufacturer = 0x00000500;

// Reset value of Delay: AckWinDelay=100b, ReadDelay=001b, AckDelay=00b, WriteDelay=100b.
// Source: n64brew "RDRAM", 0x02 Delay (RW-n default columns).
constexpr u32 kDelayReset = RdramDelay::AckWinDelay::place(4) | RdramDelay::ReadDelay::place(1) |
                            RdramDelay::AckDelay::place(0) | RdramDelay::WriteDelay::place(4);

// Reset value of Mode: CE=1, X2=1, AS=1, CC=all ones (bits 23,22,15,14,7,6).
// Source: n64brew "RDRAM", 0x03 Mode (RW-n default columns).
constexpr u32 kModeReset = RdramMode::CE::mask | RdramMode::X2::mask | RdramMode::AS::mask |
                           RdramMode::kCcMask;

// The RI is hardwired to WriteDelay = 1 TCycle and expects ReadDelay = 7 TCycles; data moves
// 2 bytes per TCycle (memory/arbiter_params.h).
constexpr u32 kRiWriteDelay = arbiter_params::kWriteDelayTCycles;
constexpr u32 kRiReadDelay = arbiter_params::kReadDelayTCycles;
constexpr u32 kBytesPerTCycle = arbiter_params::kBytesPerTCycle;
// Bursts wrap within a 2 KiB row (n64-systemtest "MI Repeat: Wrap2KiB").
constexpr u32 kRowBytes = arbiter_params::kRowBytes;
constexpr u32 row_wrap(u32 base, u32 i) {
  return (base & ~(kRowBytes - 1)) | ((base + i) % kRowBytes);
}
// The data byte's top bit, which is the 9th bit's default (see mem_write).
using DataSign = Bit<7>;
}  // namespace

Rdram::Rdram(u32 installed_bytes) {
  if (installed_bytes != 4 * kMiB && installed_bytes != 8 * kMiB)
    throw std::invalid_argument("RDRAM size must be 4 or 8 MiB");
  device_count_ = installed_bytes / kDeviceBytes;
  data_ = std::make_unique<u8[]>(usize{installed_bytes});
  ebits_ = std::make_unique<u8[]>(usize{installed_bytes});
  for (usize i = 0; i < device_count_; ++i) {
    devs_[i].delay = kDelayReset;
    devs_[i].mode = kModeReset;
  }
  rebuild_map();
}

void Rdram::rebuild_map() {
  map_.fill(-1);
  // Lowest-numbered matching device wins if two share an id (channel contention is not
  // modelled; IPL3 never leaves two enabled devices on one id).
  for (usize i = device_count_; i-- > 0;) {
    if (!enabled(i)) continue;
    u32 base = RdramDeviceId::id_field(devs_[i].device_id) & ~1u;
    if (base < 64) {
      map_[base] = static_cast<s8>(i);
      if (base + 1 < 64) map_[base + 1] = static_cast<s8>(i);
    }
  }
}

int Rdram::find_memory_device(u32 addr) const noexcept {
  const u32 region = (addr / kMiB) % static_cast<u32>(map_.size());
  return map_[region];
}

bool Rdram::drives_ones(usize i) const noexcept {
  // Output current is proportional to (63 - CC) in both manual and automatic mode
  // (n64brew "RDRAM", Mode register C[5:0] description).
  u32 cc = RdramMode::cc(devs_[i].mode);
  return (63 - cc) >= kMinCurrentUnits;
}

u8* Rdram::resolve(u32 addr, u32* avail) noexcept {
  int d = find_memory_device(addr);
  if (d < 0) return nullptr;
  u32 off = addr & (kDeviceBytes - 1);
  if (avail) *avail = kDeviceBytes - off;
  return data_.get() + usize(d) * kDeviceBytes + off;
}

u8* Rdram::resolve_ebits(u32 addr) noexcept {
  int d = find_memory_device(addr);
  if (d < 0) return nullptr;
  return ebits_.get() + usize(d) * kDeviceBytes + (addr & (kDeviceBytes - 1));
}

bool Rdram::mem_read(u32 addr, u8* out, u32 len, u8* ebits_out) {
  int d = find_memory_device(addr);
  if (d < 0) {
    std::memset(out, 0, len);
    if (ebits_out) std::memset(ebits_out, 0, len);
    return false;
  }
  const usize dev = static_cast<usize>(d);
  const u8* src = data_.get() + dev * kDeviceBytes;
  const u8* esrc = ebits_.get() + dev * kDeviceBytes;
  const u32 base = addr & (kDeviceBytes - 1);
  // Data the device drives later than the RI samples is lost at the front of the window.
  const u32 shift =
      (RdramDelay::read_delay_tcycles(devs_[dev].delay) - kRiReadDelay) * kBytesPerTCycle;
  const bool ones = drives_ones(dev);
  for (u32 i = 0; i < len; ++i) {
    if (i < shift) {
      out[i] = 0;
      if (ebits_out) ebits_out[i] = 0;
      continue;
    }
    const u32 off = row_wrap(base, i - shift);
    out[i] = ones ? src[off] : 0;
    if (ebits_out) ebits_out[i] = ones ? esrc[off] : 0;
  }
  return true;
}

bool Rdram::mem_write(u32 addr, const u8* data, const u8* ebits, u32 len, const u8* mask) {
  int d = find_memory_device(addr);
  if (d < 0) return false;
  const usize dev = static_cast<usize>(d);
  u8* dst = data_.get() + dev * kDeviceBytes;
  u8* edst = ebits_.get() + dev * kDeviceBytes;
  const u32 base = addr & (kDeviceBytes - 1);
  // A device whose WriteDelay exceeds the RI's samples the stream late.
  const u32 skew =
      (RdramDelay::write_delay_tcycles(devs_[dev].delay) - kRiWriteDelay) * kBytesPerTCycle;
  for (u32 i = 0; i < len; ++i) {
    if (mask && !mask[i]) continue;
    u32 j = i + skew;
    u8 v = j < len ? data[j] : 0;
    // CPU writes carry the 9th bit as the sign extension of each byte unless the writer
    // supplies it (n64brew "MIPS Interface", MI_MODE: "EBus appears to default to the sign
    // extension of each byte").
    const u8 e = ebits ? (j < len ? ebits[j] : 0) : static_cast<u8>(DataSign::get(v));
    const u32 off = row_wrap(base, i);
    dst[off] = v;
    edst[off] = e;
  }
  return true;
}

u32 Rdram::read_register_value(usize i, u32 reg) const noexcept {
  const Device& d = devs_[i];
  switch (static_cast<RdramReg>(reg)) {
    case RdramReg::DeviceType: return kDeviceType;
    case RdramReg::DeviceId: return d.device_id;
    case RdramReg::Delay: return d.delay | RdramDelay::kReadOnly;
    case RdramReg::Mode: {
      // X2 reads inverted; CC reads inverted in manual mode (n64brew "RDRAM", 0x03 Mode). In
      // automatic mode the device returns an internally generated value; it is modelled as
      // the programmed value inverted, i.e. proportional to the drive current, which is what
      // IPL3's calibration search requires (it raises CCi until the readback reaches its
      // target). Guess: ledger "rdram-cc-auto".
      return d.mode ^ RdramMode::X2::mask ^ RdramMode::kCcMask;
    }
    case RdramReg::RefInterval: return d.ref_interval;
    case RdramReg::RefRow: return d.ref_row;
    case RdramReg::RasInterval: return d.ras_interval;
    case RdramReg::MinInterval: return kMinIntervalRO;
    case RdramReg::AddressSelect: return d.address_select;
    case RdramReg::DeviceManufacturer: return kDeviceManufacturer;
    case RdramReg::Row: return 0;
  }
  return 0;  // registers 10..15 read as zero
}

void Rdram::write_register_value(usize i, u32 reg, u32 value) {
  Device& d = devs_[i];
  switch (static_cast<RdramReg>(reg)) {
    case RdramReg::DeviceId:
      d.device_id = value & RdramDeviceId::kWriteMask;
      rebuild_map();
      break;
    case RdramReg::Delay: d.delay = value & RdramDelay::kWriteMask; break;
    case RdramReg::Mode:
      d.mode = value & RdramMode::kWriteMask;
      rebuild_map();
      break;
    case RdramReg::RefInterval: d.ref_interval = value; break;
    case RdramReg::RefRow: d.ref_row = value; break;
    case RdramReg::RasInterval: d.ras_interval = value; break;
    case RdramReg::MinInterval: break;  // SpecFunc commands: refresh/powerdown, no stored state
    case RdramReg::AddressSelect: d.address_select = value; break;
    default: break;
  }
}

namespace {
u32 decode_reg(u32 offset) {
  // Registers mirror every 0x40 bytes; at offset >= 0x200 the DeviceType slot becomes Row.
  // Source: n64brew "RDRAM", Register summary.
  using Word = Bits<2, 4>;
  constexpr u32 kRowRegionOffset = 0x200;
  const u32 word = Word::get(offset);
  if (offset >= kRowRegionOffset && word == 0) return static_cast<u32>(RdramReg::Row);
  return word;
}
}  // namespace

u64 Rdram::reg_read(u32 id, u32 offset) {
  for (usize i = 0; i < device_count_; ++i) {
    // The register word is the first on the bus: the upper half of the octbyte.
    if (enabled(i) && id_match(i, id)) return make64(read_register_value(i, decode_reg(offset)), 0);
  }
  return 0;
}

void Rdram::reg_write(u32 id, u32 offset, const u8* stream, u32 stream_len, bool broadcast) {
  const u32 reg = decode_reg(offset);
  // Each device samples 4 bytes starting where its WriteDelay says the data begins.
  auto sampled = [&](usize i) {
    u32 skew = (RdramDelay::write_delay_tcycles(devs_[i].delay) - kRiWriteDelay) *
               kBytesPerTCycle;
    u8 b[4];
    for (u32 k = 0; k < 4; ++k) b[k] = skew + k < stream_len ? stream[skew + k] : 0;
    return load_be32(b);
  };
  if (broadcast) {
    // Values are sampled before any device's Delay changes (all devices see the same packet).
    std::array<u32, kMaxDevices> v{};
    for (usize i = 0; i < device_count_; ++i) v[i] = sampled(i);
    for (usize i = 0; i < device_count_; ++i) write_register_value(i, reg, v[i]);
    return;
  }
  // Enabled devices respond on an id match. The first not-yet-enabled device whose SIn is
  // high also responds (n64brew "RDRAM", Reset and initialization).
  for (usize i = 0; i < device_count_; ++i) {
    if (enabled(i) ? id_match(i, id) : (sin_high(i) && id_match(i, id))) {
      write_register_value(i, reg, sampled(i));
      return;
    }
  }
}

void Rdram::refresh() {}

void Rdram::serialize(SaveState& s) {
  s.section("rdram", 1);
  u64 n = device_count_;
  s.value(n);
  if (s.loading() && n != device_count_) throw std::runtime_error("save state: RDRAM size");
  for (usize i = 0; i < device_count_; ++i) {
    Device& d = devs_[i];
    s.value(d.device_id);
    s.value(d.delay);
    s.value(d.mode);
    s.value(d.ref_interval);
    s.value(d.ref_row);
    s.value(d.ras_interval);
    s.value(d.min_interval);
    s.value(d.address_select);
  }
  s.bytes(data_.get(), device_count_ * kDeviceBytes);
  s.bytes(ebits_.get(), device_count_ * kDeviceBytes);
  if (s.loading()) rebuild_map();
}

}  // namespace ultra
