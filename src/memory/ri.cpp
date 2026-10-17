#include "memory/ri.hpp"

#include <cstring>

#include "core/savestate.hpp"

namespace reality {

namespace {
// Memory-space accesses at or above 8 MiB set OverRange and are outside bank tracking.
// Source: n64brew "RDRAM Interface", RI_ERROR and Memory addressing table.
constexpr u32 kTrackedLimit = 0x0080'0000;
constexpr u32 kMemorySpaceEnd = 0x03F0'0000;
constexpr u32 kBroadcastBit = 0x0008'0000;  // address bit 19 selects broadcast register writes
constexpr u32 kRowBytes = 2048;
constexpr u32 kBankBytes = 1u << 20;
}  // namespace

Ri::Ri(Rdram& rdram, IArbiter& arbiter, TraceSink& trace)
    : rdram_(rdram), arbiter_(arbiter), trace_(trace) {}

u32 Ri::read_reg(u32 paddr) {
  // Registers mirror with mask 0x001FFFE0 (n64brew "Memory map").
  switch (static_cast<RiRegister>(paddr & 0x1C)) {
    case RiRegister::Mode: return mode_;
    case RiRegister::Config: return config_;
    case RiRegister::CurrentLoad:
      // Write-only by design; reads return a collection of bits from other registers.
      // Source: n64brew "RDRAM Interface", RI_CURRENT_LOAD READ.
      return RiError::MissingAck::get(error_) | (1u << 1) | (1u << 2) |
             (RiMode::StopR::get(mode_) << 3) | ((RiSelect::TSel::get(select_) & 1) << 4);
    case RiRegister::Select: return select_;
    case RiRegister::Refresh: return refresh_;
    case RiRegister::Latency: return latency_;
    case RiRegister::Error: return error_;
    case RiRegister::BankStatus: return (bank_dirty_ << 8) | bank_valid_;
  }
  return 0;
}

void Ri::write_reg(u32 paddr, u32 value) {
  switch (static_cast<RiRegister>(paddr & 0x1C)) {
    case RiRegister::Mode: mode_ = value & RiMode::kMask; break;
    case RiRegister::Config: config_ = value & RiConfig::kMask; break;
    case RiRegister::CurrentLoad: break;  // loads the RAC current control register
    case RiRegister::Select: select_ = value & RiSelect::kMask; break;
    case RiRegister::Refresh: refresh_ = value & RiRefresh::kMask; break;
    case RiRegister::Latency: latency_ = value & 0xF; break;
    case RiRegister::Error: error_ = 0; break;
    case RiRegister::BankStatus:
      // Any write invalidates every bank and marks it dirty (n64brew, RI_BANK_STATUS).
      bank_valid_ = 0;
      bank_dirty_ = 0xFF;
      break;
  }
}

void Ri::track_bank(u32 paddr, bool write) {
  if (paddr >= kTrackedLimit) return;
  u32 bank = paddr / kBankBytes;
  u32 row = (paddr % kBankBytes) / kRowBytes;
  u32 bit = 1u << bank;
  if (!(bank_valid_ & bit) || bank_row_[bank] != row) {
    bank_valid_ |= bit;
    bank_dirty_ &= ~bit;
    bank_row_[bank] = row;
  }
  if (write) bank_dirty_ |= bit;
}

void Ri::trace(Master m, TraceKind k, u32 addr, const u8* data, u32 len, Tick t, Tick done) {
  if (!trace_.enabled()) return;
  TraceRecord r{};
  r.tick = t;
  r.address = addr;
  r.duration = static_cast<u32>(done - t);
  u8 first[8] = {};
  std::memcpy(first, data, len < 8 ? len : 8);
  r.data = load_be64(first);
  r.payload_hash = len > 8 ? TraceSink::hash_payload(data, len) : 0;
  r.size = static_cast<u16>(len);
  r.master = m;
  r.kind = k;
  trace_.record(r);
}

Tick Ri::read(Master m, u32 paddr, u8* out, u32 len, Tick t, u8* ebits_out, TraceKind kind) {
  paddr &= 0x03FF'FFFF;
  if (paddr >= kTrackedLimit && paddr < kMemorySpaceEnd) error_ |= RiError::OverRange::mask;
  track_bank(paddr, false);
  if (!rdram_.mem_read(paddr, out, len, ebits_out)) error_ |= RiError::MissingAck::mask;
  Tick done = arbiter_.request({m, paddr, len, AccessKind::Read, t});
  trace(m, kind, paddr, out, len, t, done);
  return done;
}

Tick Ri::write(Master m, u32 paddr, const u8* data, u32 len, Tick t, const u8* mask,
               const u8* ebits, TraceKind kind) {
  paddr &= 0x03FF'FFFF;
  if (paddr >= kTrackedLimit && paddr < kMemorySpaceEnd) error_ |= RiError::OverRange::mask;
  track_bank(paddr, true);
  if (!rdram_.mem_write(paddr, data, ebits, len, mask)) error_ |= RiError::MissingAck::mask;
  Tick done = arbiter_.request({m, paddr, len, AccessKind::Write, t});
  trace(m, kind, paddr, data, len, t, done);
  return done;
}

Tick Ri::reg_read(u32 paddr, u8 out[8], Tick t) {
  // Register space: Adr[28:20] = (address >> 10) & 0x1FF, register offset = address & 0x3FF.
  // Source: n64brew "RDRAM Interface", Memory addressing table.
  u32 id = (paddr >> 10) & 0x1FF;
  u64 v = rdram_.reg_read(id, paddr & 0x3FF);
  store_be64(out, v);
  Tick done = arbiter_.request({Master::CpuData, paddr & 0x03FF'FFFF, 8, AccessKind::Read, t});
  trace(Master::CpuData, TraceKind::Read, paddr & 0x03FF'FFFF, out, 8, t, done);
  return done;
}

Tick Ri::reg_write(u32 paddr, const u8* stream, u32 stream_len, Tick t) {
  u32 id = (paddr >> 10) & 0x1FF;
  bool broadcast = (paddr & kBroadcastBit) != 0;
  rdram_.reg_write(id, paddr & 0x3FF, stream, stream_len, broadcast);
  Tick done =
      arbiter_.request({Master::CpuData, paddr & 0x03FF'FFFF, stream_len, AccessKind::Write, t});
  trace(Master::CpuData, TraceKind::Write, paddr & 0x03FF'FFFF, stream, stream_len, t, done);
  return done;
}

void Ri::hsync(Tick t) {
  if (!RiRefresh::Enable::test(refresh_)) return;
  refresh_ ^= RiRefresh::Bank::mask;  // "Oscillates between 0 and 1 during operation"
  bank_valid_ = 0;
  rdram_.refresh();
  arbiter_.refresh(t);
}

void Ri::serialize(SaveState& s) {
  s.section("ri", 1);
  s.value(mode_);
  s.value(config_);
  s.value(select_);
  s.value(refresh_);
  s.value(latency_);
  s.value(error_);
  s.value(bank_valid_);
  s.value(bank_dirty_);
  s.array(bank_row_);
}

}  // namespace reality
