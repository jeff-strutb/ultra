#include "cpu/vr4300.hpp"

#include <cstring>

#include "core/savestate.hpp"
#include "core/scheduler.hpp"
#include "cpu/timing.hpp"

namespace ultra {

using namespace cop0;

Vr4300::Vr4300(Bus& bus, Scheduler& sched, CpuPins& pins) : bus_(bus), sched_(sched), pins_(pins) {
  // Power-on value of Cause. BD, CE and ExcCode are read-only and undefined after reset
  // (VR4300 manual 6.3.6); 0xB000007C is what krom's COP0Cause expected output shows surviving
  // the boot code's `mtc0 zero, Cause`. Guess: ledger "cop0-cause-poweron".
  cause_ = 0xB000'007C;
  // LLAddr is undefined after reset (5.4.7); krom's LL_LLD_SC_SCD expected output shows
  // 0xFFFFFFFF before any LL. Guess: ledger "cop0-lladdr-poweron".
  lladdr_ = 0xFFFF'FFFF;
  // Registers that are undefined at power on but observed on hardware right after the first
  // turn-on (n64-systemtest src/tests/startup/mod.rs): BadVAddr reads all ones, ExceptPC and
  // ErrorEPC "are usually 0xFFFFFFFF_FFFFFFFF". ErrorEPC takes the PC at cold reset (6.4.4), so
  // the pre-reset PC is all ones too. Context/XContext hold BadVAddr's VPN2 and region bits, as
  // krom's COP0Register expected output shows (0x007FFFF0 / ...FFFFFFF0).
  // Derived (hardware observations in n64-systemtest): ledger "cop0-undefined-poweron".
  badvaddr_ = ~u64{0};
  epc_ = ~u64{0};
  pc_ = ~u64{0};
  context_ = Context::BadVPN2::set(context_, (badvaddr_ >> 13) & 0x7FFFF);
  xcontext_ = XContext::BadVPN2::set(xcontext_, (badvaddr_ >> 13) & 0x7FFFFFF);
  xcontext_ = XContext::R::set(xcontext_, badvaddr_ >> 62);
  // TLB contents are undefined at power on (manual 5.4); ours start as all-zero entries. Their
  // compare masks must be derived like any written entry, so a never-written entry matches
  // only VPN2 0 / ASID 0 instead of every address.
  for (u32 i = 0; i < tlb_.size(); ++i) tlb_refresh_entry(i);
}

// ---- reset ---------------------------------------------------------------------------------

void Vr4300::cold_reset() {
  // 6.4.4 Cold Reset: Status.BEV=1, ERL=1, SR=0, TS=0, RP=0; Random = 31; Wired = 0;
  // Config EP=0, BE=1 (5.4.6); ErrorEPC = PC; PC = 0xFFFF_FFFF_BFC0_0000.
  status_ = (status_ & ~(Status::TS::mask | Status::SR::mask | Status::RP::mask)) |
            Status::BEV::mask | Status::ERL::mask;
  wired_ = 0;
  random_base_ = retired_;
  config_ = Config::kFixed | Config::EC::place(Config::kEcNintendo) | Config::BE::mask |
            (config_ & (Config::K0::mask | Config::CU::mask));
  error_epc_ = pc_;
  frozen_ = false;
  llbit_ = false;
  wb_count_ = 0;
  set_pc(0xFFFF'FFFF'BFC0'0000ull);
  update_timer();
}

void Vr4300::soft_reset() {
  // 6.4.5 Soft Reset: as cold reset but SR=1 and only BEV/ERL/SR/TS/RP are touched.
  status_ = (status_ & ~(Status::TS::mask | Status::RP::mask)) | Status::BEV::mask |
            Status::ERL::mask | Status::SR::mask;
  error_epc_ = in_delay_slot_ ? pc_ - 4 : pc_;
  frozen_ = false;
  set_pc(0xFFFF'FFFF'BFC0'0000ull);
}

void Vr4300::nmi() {
  // 6.4.6 NMI: ErrorEPC = PC, Status BEV/ERL/SR set, vector 0xBFC0_0000.
  status_ |= Status::BEV::mask | Status::ERL::mask | Status::SR::mask;
  status_ &= ~Status::TS::mask;
  error_epc_ = in_delay_slot_ ? pc_ - 4 : pc_;
  frozen_ = false;
  set_pc(0xFFFF'FFFF'BFC0'0000ull);
}

// ---- mode helpers ----------------------------------------------------------------------------

bool Vr4300::kernel_mode() const noexcept {
  return Status::KSU::get(status_) == 0 || Status::EXL::test(status_) ||
         Status::ERL::test(status_);
}
bool Vr4300::supervisor_mode() const noexcept {
  return !kernel_mode() && Status::KSU::get(status_) == 1;
}
bool Vr4300::user_mode() const noexcept { return !kernel_mode() && Status::KSU::get(status_) == 2; }

bool Vr4300::addr64() const noexcept {
  if (kernel_mode()) return Status::KX::test(status_);
  if (supervisor_mode()) return Status::SX::test(status_);
  return Status::UX::test(status_);
}

bool Vr4300::ops64() const noexcept {
  // "64-bit operations are always valid in Kernel mode" (6.3.5).
  if (kernel_mode()) return true;
  if (supervisor_mode()) return Status::SX::test(status_);
  return Status::UX::test(status_);
}

bool Vr4300::cop_usable(u32 n) const noexcept {
  if (n == 0) return kernel_mode() || Status::CU0::test(status_);
  return (status_ >> (28 + n)) & 1;
}

// ---- main loop -----------------------------------------------------------------------------

void Vr4300::run_until(Tick limit) {
  while (!frozen_ && cpu_to_tick_floor(cycles_) < limit) step();
}

void Vr4300::stall_until_tick(Tick done) {
  if (done == kTickNever) {
    frozen_ = true;
    return;
  }
  CpuCycle c = tick_to_cpu_ceil(done);
  if (c > cycles_) cycles_ = c;
}

void Vr4300::step() {
  if (frozen_) return;

  const Tick now = cpu_to_tick_floor(cycles_);
  if (sched_.next_tick() <= now) sched_.run_until(now);

  // Issue buffered writes whose bus slot has arrived.
  while (wb_count_ != 0 && std::max(wb_[wb_head_].ready, sysad_free_) <= now) {
    if (!wb_issue_head()) return;
  }

  if (cycles_ >= timer_cycle_) {
    cause_ |= 1u << 15;  // IP7: Count == Compare (6.3.4)
    timer_cycle_ += CpuCycle{1} << 33;  // next match after a full 2^32 Count period
  }

  cur_pc_ = pc_;
  cur_in_delay_ = in_delay_slot_;
  exception_ = false;
  cur_load_kind_ = LoadTarget::None;
  cur_dcache_store_ = false;
  cur_fpu_result_reg_ = -1;

  if (check_interrupts()) {
    pending_load_kind_ = LoadTarget::None;
    prev_dcache_store_ = false;
    fpu_result_reg_ = -1;
    return;
  }

  u32 instr;
  if (!fetch(cur_pc_, instr)) {
    pending_load_kind_ = LoadTarget::None;
    prev_dcache_store_ = false;
    fpu_result_reg_ = -1;
    return;
  }

  if (history_on_) {
    history_[history_pos_] = {cur_pc_, instr, cycles_};
    history_pos_ = (history_pos_ + 1) % kHistory;
  }
  pc_ = npc_;
  npc_ += 4;
  in_delay_slot_ = false;
  branch_pending_ = false;
  cycles_ += 1;

  execute(instr);

  if (!exception_) {
    if (branch_pending_) in_delay_slot_ = true;
    ++retired_;
  }
  pending_load_kind_ = cur_load_kind_;
  pending_load_reg_ = cur_load_reg_;
  prev_dcache_store_ = cur_dcache_store_;
  fpu_result_reg_ = cur_fpu_result_reg_;
}

bool Vr4300::check_interrupts() {
  cause_ = (cause_ & ~(0x1Fu << 10)) | (u32{pins_.int_lines} << 10);
  if (!Status::IE::test(status_) || Status::EXL::test(status_) || Status::ERL::test(status_))
    return false;
  if ((Cause::IP::get(cause_) & Status::IM::get(status_)) == 0) return false;
  raise(ExcCode::Int);
  return true;
}

// ---- exceptions ----------------------------------------------------------------------------

u64 Vr4300::exception_vector(bool tlb_refill, bool xtlb) const noexcept {
  // Tables 6-3/6-4: base 0x8000_0000 (BEV=0) or 0xBFC0_0200 (BEV=1); TLB refill offset
  // 0x000, XTLB refill 0x080, everything else 0x180. Refill offsets only apply with EXL=0.
  u64 base = Status::BEV::test(status_) ? 0xFFFF'FFFF'BFC0'0200ull : 0xFFFF'FFFF'8000'0000ull;
  if (tlb_refill && !Status::EXL::test(status_)) return base + (xtlb ? 0x080 : 0x000);
  return base + 0x180;
}

void Vr4300::raise(ExcCode code, u32 ce) {
  u64 vector = exception_vector(false, false);
  if (!Status::EXL::test(status_)) {
    epc_ = cur_in_delay_ ? cur_pc_ - 4 : cur_pc_;
    cause_ = Cause::BD::set(cause_, cur_in_delay_ ? 1 : 0);
  }
  cause_ = Cause::ExcCode::set(cause_, static_cast<u32>(code));
  cause_ = Cause::CE::set(cause_, ce);
  status_ |= Status::EXL::mask;
  set_pc(vector);
  exception_ = true;
  cycles_ += timing::kExceptionAbortCycles;
  if (break_on_exception_) frozen_ = true;
}

void Vr4300::raise_address_error(u64 vaddr, Access acc) {
  badvaddr_ = vaddr;
  // n64-systemtest "LW with address not sign extended" expects Context/XContext BadVPN2 to
  // be written on address errors as well (docs/hardware/cpu.md).
  context_ = Context::BadVPN2::set(context_, (vaddr >> 13) & 0x7FFFF);
  xcontext_ = XContext::BadVPN2::set(xcontext_, (vaddr >> 13) & 0x7FFFFFF);
  xcontext_ = XContext::R::set(xcontext_, vaddr >> 62);
  // EntryHi.VPN2/R are latched from the faulting address as for TLB misses
  // (n64-systemtest tlb64 "LW TLB Miss or Address Exception").
  entry_hi_ = (entry_hi_ & EntryHi::ASID::mask) | (vaddr & 0xC000'00FF'FFFF'E000ull);
  raise(acc == Access::Store ? ExcCode::AdES : ExcCode::AdEL);
}

void Vr4300::raise_tlb(u64 vaddr, Access acc, bool miss, bool modification) {
  badvaddr_ = vaddr;
  context_ = Context::BadVPN2::set(context_, (vaddr >> 13) & 0x7FFFF);
  xcontext_ = XContext::BadVPN2::set(xcontext_, (vaddr >> 13) & 0x7FFFFFF);
  xcontext_ = XContext::R::set(xcontext_, vaddr >> 62);
  entry_hi_ = (entry_hi_ & EntryHi::ASID::mask) | (vaddr & 0xC000'00FF'FFFF'E000ull);
  ExcCode code = modification ? ExcCode::Mod
                 : acc == Access::Store ? ExcCode::TLBS
                                        : ExcCode::TLBL;
  // Refill vector only for a TLB miss (not invalid / modified) with EXL clear.
  bool refill = miss;
  bool xtlb = addr64();
  u64 vector = exception_vector(refill, xtlb);
  if (!Status::EXL::test(status_)) {
    epc_ = cur_in_delay_ ? cur_pc_ - 4 : cur_pc_;
    cause_ = Cause::BD::set(cause_, cur_in_delay_ ? 1 : 0);
  }
  cause_ = Cause::ExcCode::set(cause_, static_cast<u32>(code));
  cause_ = Cause::CE::set(cause_, 0);
  status_ |= Status::EXL::mask;
  set_pc(vector);
  exception_ = true;
  cycles_ += timing::kExceptionAbortCycles;
  if (break_on_exception_) frozen_ = true;
}

void Vr4300::eret() {
  // 16 ERET: return to ErrorEPC when ERL is set, else EPC; clears the LL bit. No delay slot.
  if (Status::ERL::test(status_)) {
    set_pc(error_epc_);
    status_ &= ~Status::ERL::mask;
  } else {
    set_pc(epc_);
    status_ &= ~Status::EXL::mask;
  }
  llbit_ = false;
}

// ---- address translation ---------------------------------------------------------------------

bool Vr4300::translate(u64 vaddr, Access acc, Translation& out) {
  const bool k = kernel_mode();
  const bool s = supervisor_mode();
  if (!addr64()) {
    // 32-bit addressing: the address must be a sign-extended 32-bit value.
    if (vaddr != sext32(lo32(vaddr))) {
      raise_address_error(vaddr, acc);
      return false;
    }
    const u32 a = lo32(vaddr);
    if (a < 0x8000'0000u) {
      if (k && Status::ERL::test(status_)) {
        // ERL=1: kuseg is an unmapped, uncached 2 GiB window (5.2.4).
        out = {a, false};
        return true;
      }
      return tlb_translate(vaddr, acc, out);
    }
    if (k) {
      if (a < 0xA000'0000u) {
        out = {a - 0x8000'0000u, Config::K0::get(config_) != 2};
        return true;
      }
      if (a < 0xC000'0000u) {
        out = {a - 0xA000'0000u, false};
        return true;
      }
      return tlb_translate(vaddr, acc, out);
    }
    if (s && a >= 0xC000'0000u && a < 0xE000'0000u) return tlb_translate(vaddr, acc, out);
    raise_address_error(vaddr, acc);
    return false;
  }

  // 64-bit addressing (Tables 5-4 and Figure 5-7).
  constexpr u64 k1TiB = u64{1} << 40;
  const u32 region = static_cast<u32>(vaddr >> 62);
  if (region == 0) {
    if (vaddr < k1TiB) {
      if (k && Status::ERL::test(status_) && vaddr < 0x8000'0000ull) {
        out = {lo32(vaddr), false};
        return true;
      }
      return tlb_translate(vaddr, acc, out);
    }
  } else if (region == 1) {
    if ((k || s) && vaddr - 0x4000'0000'0000'0000ull < k1TiB) return tlb_translate(vaddr, acc, out);
  } else if (region == 2) {
    if (k && ((vaddr >> 32) & 0x07FF'FFFF) == 0) {
      const u32 cca = static_cast<u32>((vaddr >> 59) & 7);
      out = {lo32(vaddr), cca != 2};
      return true;
    }
  } else {
    if (vaddr >= 0xFFFF'FFFF'8000'0000ull) {
      const u32 a = lo32(vaddr);
      if (k) {
        if (a < 0xA000'0000u) {
          out = {a - 0x8000'0000u, Config::K0::get(config_) != 2};
          return true;
        }
        if (a < 0xC000'0000u) {
          out = {a - 0xA000'0000u, false};
          return true;
        }
        return tlb_translate(vaddr, acc, out);
      }
      if (s && a >= 0xC000'0000u && a < 0xE000'0000u) return tlb_translate(vaddr, acc, out);
    } else if (k && vaddr <= 0xC000'00FF'7FFF'FFFFull) {
      return tlb_translate(vaddr, acc, out);
    }
  }
  raise_address_error(vaddr, acc);
  return false;
}

void Vr4300::tlb_refresh_entry(u32 i) {
  TlbEntry& e = tlb_[i];
  // Compare R (63:62) and VPN2 (39:13) above the page-size mask (5.4.9, Figure 5-19).
  const u64 page_bits = u64{e.page_mask} | 0x1FFF;
  e.vpn2_mask = 0xC000'00FF'FFFF'E000ull & ~page_bits;
  e.global = EntryLo::G::test(e.entry_lo0) && EntryLo::G::test(e.entry_lo1);
}

bool Vr4300::tlb_translate(u64 vaddr, Access acc, Translation& out) {
  const u64 asid = EntryHi::ASID::get(entry_hi_);
  int match = -1;
  for (u32 i = 0; i < 32; ++i) {
    const TlbEntry& e = tlb_[i];
    if ((vaddr & e.vpn2_mask) != (e.entry_hi & e.vpn2_mask)) continue;
    if (!e.global && EntryHi::ASID::get(e.entry_hi) != asid) continue;
    match = static_cast<int>(i);
    break;
  }
  if (match < 0) {
    raise_tlb(vaddr, acc, true, false);
    return false;
  }
  const TlbEntry& e = tlb_[static_cast<u32>(match)];
  const u64 page_bytes = (u64{e.page_mask} >> 1 | 0xFFF) + 1;
  const u64 lo = (vaddr & page_bytes) ? e.entry_lo1 : e.entry_lo0;
  if (!EntryLo::V::test(lo)) {
    raise_tlb(vaddr, acc, false, false);
    return false;
  }
  if (acc == Access::Store && !EntryLo::D::test(lo)) {
    raise_tlb(vaddr, acc, false, true);
    return false;
  }
  const u64 pfn_addr = EntryLo::PFN::get(lo) << 12;
  out.paddr = static_cast<u32>((pfn_addr & ~(page_bytes - 1)) | (vaddr & (page_bytes - 1)));
  out.cached = EntryLo::C::get(lo) != 2;
  return true;
}

void Vr4300::tlb_probe() {
  index_ = IndexReg::P::mask;
  const u64 asid = EntryHi::ASID::get(entry_hi_);
  for (u32 i = 0; i < 32; ++i) {
    const TlbEntry& e = tlb_[i];
    if ((entry_hi_ & e.vpn2_mask) != (e.entry_hi & e.vpn2_mask)) continue;
    if (!e.global && EntryHi::ASID::get(e.entry_hi) != asid) continue;
    index_ = i;
    return;
  }
}

void Vr4300::tlb_read() {
  const TlbEntry& e = tlb_[index_ & 31];
  page_mask_ = e.page_mask;
  entry_hi_ = e.entry_hi & ~u64{e.page_mask};
  // The G bit of a TLB entry reads back in both EntryLo registers (5.3.1).
  const u64 g = e.global ? EntryLo::G::mask : 0;
  entry_lo0_ = (e.entry_lo0 & ~EntryLo::G::mask) | g;
  entry_lo1_ = (e.entry_lo1 & ~EntryLo::G::mask) | g;
}

void Vr4300::tlb_write(u32 i) {
  TlbEntry& e = tlb_[i & 31];
  // The TLB stores the page mask per bit pair (bits 14:13, 16:15, ... 24:23); each pair takes
  // the value of its upper bit. EntryLo keeps PFN(25:6), C, D, V, G only.
  // Source: n64-systemtest tlb "PageMask after TLBWI/TLBR", "EntryLo0/1/Hi after TLBWI/TLBR".
  u32 mask = 0;
  for (u32 pair = 0; pair < 6; ++pair)
    if (page_mask_ & (1u << (14 + 2 * pair))) mask |= 3u << (13 + 2 * pair);
  e.page_mask = mask;
  e.entry_hi = entry_hi_ & ~u64{mask};
  e.entry_lo0 = entry_lo0_ & 0x03FF'FFFFull;
  e.entry_lo1 = entry_lo1_ & 0x03FF'FFFFull;
  tlb_refresh_entry(i & 31);
}

u32 Vr4300::random_reg() const noexcept {
  // Random counts down from 31 to Wired once per retired instruction, then reloads 31
  // (5.4.2). Bit 5 participates when Wired > 31.
  const u64 n = retired_ - random_base_;
  const u32 w = wired_ & 63;
  if (w <= 31) {
    const u64 period = 32 - w;
    return 31 - static_cast<u32>(n % period);
  }
  const u64 period = 32 + (64 - w);
  return static_cast<u32>((31 - static_cast<s64>(n % period)) & 63);
}

// ---- bus helpers -----------------------------------------------------------------------------

bool Vr4300::wb_issue_head() {
  PendingWrite& w = wb_[wb_head_];
  const Tick start = std::max(w.ready, sysad_free_);
  const Tick done = bus_.cpu_write(w.master, w.paddr, w.size, w.data.data(), start);
  wb_head_ = (wb_head_ + 1) & 3;
  --wb_count_;
  if (done == kTickNever) {
    frozen_ = true;
    return false;
  }
  sysad_free_ = done;
  return true;
}

void Vr4300::drain_write_buffer() {
  while (wb_count_ != 0 && !frozen_) wb_issue_head();
}

bool Vr4300::bus_read(Master m, u32 paddr, u32 size, u8* out) {
  // SysAD is a single in-order bus: buffered writes go out before this read.
  drain_write_buffer();
  if (frozen_) return false;
  // Stall structure from VR4300 manual Tables 11-1 / 11-2 (timing.hpp).
  const Tick start = std::max(cpu_to_tick_ceil(cycles_ + timing::kSysAdReadIssueCycles), sysad_free_);
  const Tick done = bus_.cpu_read(m, paddr, size, out, start);
  stall_until_tick(done);
  if (done == kTickNever) return false;
  sysad_free_ = done;
  cycles_ += timing::kSysAdReadReturnCycles;
  return true;
}

bool Vr4300::bus_write(Master m, u32 paddr, u32 size, const u8* data) {
  // A full buffer stalls the pipeline until the oldest entry has completed (4.9).
  if (wb_count_ == 4) {
    if (!wb_issue_head()) return false;
    stall_until_tick(sysad_free_);
  }
  PendingWrite& w = wb_[(wb_head_ + wb_count_) & 3];
  w.paddr = paddr;
  w.size = size;
  w.master = m;
  w.ready = cpu_to_tick_ceil(cycles_);
  std::memcpy(w.data.data(), data, size <= 4 ? 4 : size <= 8 ? 8 : size);
  ++wb_count_;
  return true;
}

// ---- caches ----------------------------------------------------------------------------------

void Vr4300::dcache_writeback(DLine& line, u32 index) {
  const u32 paddr = (line.ptag << 12) | ((index << 4) & 0xFFF);
  bus_write(Master::CpuData, paddr, kDCacheLineBytes, line.data.data());
  line.dirty = false;
}

bool Vr4300::dcache_fill(DLine& line, u32 paddr) {
  const u32 index = static_cast<u32>(&line - dcache_.data());
  if (line.valid && line.dirty) dcache_writeback(line, index);
  u8 buf[kDCacheLineBytes];
  if (!bus_read(Master::CpuData, paddr & ~(kDCacheLineBytes - 1), kDCacheLineBytes, buf))
    return false;
  std::memcpy(line.data.data(), buf, kDCacheLineBytes);
  line.ptag = paddr >> 12;
  line.valid = true;
  line.dirty = false;
  return true;
}

bool Vr4300::icache_fill(ILine& line, u32 paddr) {
  u8 buf[kICacheLineBytes];
  if (!bus_read(Master::CpuInstr, paddr & ~(kICacheLineBytes - 1), kICacheLineBytes, buf))
    return false;
  std::memcpy(line.data.data(), buf, kICacheLineBytes);
  line.ptag = paddr >> 12;
  line.valid = true;
  return true;
}

bool Vr4300::fetch(u64 vaddr, u32& instr) {
  if (vaddr & 3) {
    raise_address_error(vaddr, Access::Fetch);
    return false;
  }
  Translation tr;
  if (!translate(vaddr, Access::Fetch, tr)) return false;
  if (reverse_endian()) {
    vaddr ^= 4;
    tr.paddr ^= 4;
  }
  if (tr.cached) {
    ILine& line = iline(vaddr);
    if (!line.valid || line.ptag != (tr.paddr >> 12)) {
      if (!icache_fill(line, tr.paddr)) return false;
    }
    instr = load_be32(line.data.data() + (vaddr & (kICacheLineBytes - 1)));
    return true;
  }
  u8 w[4];
  if (!bus_read(Master::CpuInstr, tr.paddr, 4, w)) return false;
  instr = load_be32(w);
  return true;
}

bool Vr4300::check_watch(u32 paddr, Access acc) {
  // 6.4.17: WatchLo.PAddr0 matches physical address bits 31:3 for loads (R) / stores (W).
  if (((paddr >> 3) & 0x1FFF'FFFF) != WatchLo::PAddr0::get(watch_lo_)) return true;
  bool hit = (acc == Access::Load && WatchLo::R::test(watch_lo_)) ||
             (acc == Access::Store && WatchLo::W::test(watch_lo_));
  if (!hit) return true;
  if (Status::EXL::test(status_)) return true;  // deferred while EXL is set
  raise(ExcCode::WATCH);
  return false;
}

// Reads `size` bytes (1, 2, 4 or 8) at an aligned vaddr through cache or bus.
template <typename T>
bool Vr4300::load(u64 vaddr, T& out, bool partial) {
  constexpr u32 size = sizeof(T);
  if (!partial && (vaddr & (size - 1))) {
    raise_address_error(vaddr, Access::Load);
    return false;
  }
  Translation tr;
  if (!translate(vaddr, Access::Load, tr)) return false;
  // Access address: the aligned unit for partial loads, XOR-adjusted under reverse endian.
  u64 av = partial ? vaddr & ~u64{size - 1} : vaddr;
  if (size < 8 && reverse_endian()) av ^= 8 - size;
  tr.paddr = static_cast<u32>(tr.paddr - vaddr + av);
  vaddr = av;
  if (!check_watch(tr.paddr, Access::Load)) return false;
  if (prev_dcache_store_) cycles_ += timing::kStoreLoadDcbCycles;
  u8 bytes[8];
  if (tr.cached) {
    DLine& line = dline(vaddr);
    if (!line.valid || line.ptag != (tr.paddr >> 12)) {
      if (!dcache_fill(line, tr.paddr)) return false;
    }
    std::memcpy(bytes, line.data.data() + (vaddr & 15), size);
  } else {
    u8 bus[8];
    if (!bus_read(Master::CpuData, tr.paddr, size, bus)) return false;
    if constexpr (size == 8) {
      std::memcpy(bytes, bus, 8);
    } else {
      std::memcpy(bytes, bus + (tr.paddr & 3), size);
    }
  }
  if constexpr (size == 1) out = bytes[0];
  if constexpr (size == 2) out = load_be16(bytes);
  if constexpr (size == 4) out = load_be32(bytes);
  if constexpr (size == 8) out = load_be64(bytes);
  return true;
}

bool Vr4300::store_bus(u64 vaddr, u32 size, const u8* bus_word, u32 cache_offset, u32 cache_len,
                       const u8* cache_bytes, u64 xlate_vaddr) {
  Translation tr;
  if (!translate(xlate_vaddr, Access::Store, tr)) return false;
  tr.paddr = static_cast<u32>(tr.paddr - (xlate_vaddr - vaddr));
  if (!check_watch(tr.paddr, Access::Store)) return false;
  if (prev_dcache_store_) cycles_ += timing::kStoreLoadDcbCycles;
  if (tr.cached) {
    DLine& line = dline(vaddr);
    if (!line.valid || line.ptag != (tr.paddr >> 12)) {
      if (!dcache_fill(line, tr.paddr)) return false;
    }
    std::memcpy(line.data.data() + cache_offset, cache_bytes, cache_len);
    line.dirty = true;
    cur_dcache_store_ = true;
    return true;
  }
  return bus_write(Master::CpuData, tr.paddr, size, bus_word);
}

template <typename T>
bool Vr4300::store(u64 vaddr, u64 reg) {
  constexpr u32 size = sizeof(T);
  if (vaddr & (size - 1)) {
    raise_address_error(vaddr, Access::Store);
    return false;
  }
  const T value = static_cast<T>(reg);
  // Reverse endian: the access lands on the XOR-adjusted address; translation and BadVAddr
  // use the original one.
  const u64 xlate = vaddr;
  if (size < 8 && reverse_endian()) vaddr ^= 8 - size;
  u8 bytes[8];
  if constexpr (size == 1) bytes[0] = value;
  if constexpr (size == 2) store_be16(bytes, value);
  if constexpr (size == 4) store_be32(bytes, value);
  if constexpr (size == 8) store_be64(bytes, value);
  u8 bus[8] = {};
  if constexpr (size == 8) {
    std::memcpy(bus, bytes, 8);
  } else {
    const u32 shift = 8 * (4 - size - static_cast<u32>(vaddr & 3));
    store_be32(bus, lo32(reg) << shift);
  }
  return store_bus(vaddr, size, bus, static_cast<u32>(vaddr & 15), size, bytes, xlate);
}

bool Vr4300::store_lanes(u64 unit_vaddr, u32 unit, u64 value, u32 lane_lo, u32 lane_hi,
                         u64 xlate_vaddr) {
  u8 bytes[8];
  if (unit == 8)
    store_be64(bytes, value);
  else
    store_be32(bytes, static_cast<u32>(value));
  const u64 start = unit_vaddr + lane_lo;
  const u32 size = lane_hi - lane_lo + 1;
  // SysAD carries the word containing the bytes (or the doubleword when they span words).
  const u8* bus = bytes;
  if (unit == 8 && (lane_lo >> 2) == (lane_hi >> 2)) bus = bytes + (lane_lo & 4);
  return store_bus(start, size, bus, static_cast<u32>(start & 15), size, bytes + lane_lo, xlate_vaddr);
}

template bool Vr4300::load<u8>(u64, u8&, bool);
template bool Vr4300::load<u16>(u64, u16&, bool);
template bool Vr4300::load<u32>(u64, u32&, bool);
template bool Vr4300::load<u64>(u64, u64&, bool);
template bool Vr4300::store<u8>(u64, u64);
template bool Vr4300::store<u16>(u64, u64);
template bool Vr4300::store<u32>(u64, u64);
template bool Vr4300::store<u64>(u64, u64);

// ---- COP0 ------------------------------------------------------------------------------------

u32 Vr4300::count_reg() const noexcept { return static_cast<u32>((cycles_ >> 1) + count_offset_); }

void Vr4300::update_timer() noexcept {
  // Next PCycle (even, Count increments every other PCycle) at which Count == Compare.
  const u64 k_now = cycles_ >> 1;
  const u32 count_now = static_cast<u32>(k_now + count_offset_);
  u64 delta = static_cast<u32>(compare_ - count_now);
  if (delta == 0) delta = u64{1} << 32;
  timer_cycle_ = (k_now + delta) << 1;
}

u64 Vr4300::cop0_get(u32 reg) {
  switch (reg) {
    case Index: return index_;
    case Random: return random_reg();
    case EntryLo0: return entry_lo0_;
    case EntryLo1: return entry_lo1_;
    case Context: return context_;
    case PageMask: return page_mask_;
    case Wired: return wired_;
    case BadVAddr: return badvaddr_;
    case Count: return count_reg();
    case EntryHi: return entry_hi_;
    case Compare: return compare_;
    case Status: return status_;
    case Cause: return cause_;
    case EPC: return epc_;
    case PRId: return kPRId;
    case Config: return config_;
    case LLAddr: return lladdr_;
    case WatchLo: return watch_lo_;
    case WatchHi: return watch_hi_;
    case XContext: return xcontext_;
    case ParityError: return parity_error_;
    case CacheError: return 0;
    case TagLo: return tag_lo_;
    case TagHi: return tag_hi_;
    case ErrorEPC: return error_epc_;
    default: return cop0_latch_;  // 7, 21-25, 31: unimplemented, read the CP0 bus latch
  }
}

void Vr4300::cop0_set(u32 reg, u64 v) {
  cop0_latch_ = v;
  const u32 w = lo32(v);
  switch (reg) {
    case Index: index_ = w & IndexReg::kWriteMask; break;
    case Random: break;
    case EntryLo0: entry_lo0_ = v & EntryLo::kWriteMask; break;
    case EntryLo1: entry_lo1_ = v & EntryLo::kWriteMask; break;
    case Context: context_ = (context_ & Context::BadVPN2::mask) | (v & Context::PTEBase::mask); break;
    case PageMask: page_mask_ = w & PageMask::kWriteMask; break;
    case Wired:
      wired_ = w & 63;
      random_base_ = retired_ + 1;  // Random reads 31 from the next instruction on
      break;
    case BadVAddr: break;  // read-only
    case Count:
      count_offset_ = static_cast<u32>(w - static_cast<u32>(cycles_ >> 1));
      update_timer();
      break;
    case EntryHi: entry_hi_ = v & EntryHi::kWriteMask; break;
    case Compare:
      compare_ = w;
      cause_ &= ~(1u << 15);
      update_timer();
      break;
    case Status: status_ = (status_ & ~Status::kWriteMask) | (w & Status::kWriteMask); break;
    case Cause: cause_ = (cause_ & ~Cause::kWriteMask) | (w & Cause::kWriteMask); break;
    case EPC: epc_ = v; break;
    case PRId: break;
    case Config:
      config_ = (config_ & ~Config::kWriteMask) | (w & Config::kWriteMask);
      break;
    case LLAddr: lladdr_ = w; break;
    case WatchLo: watch_lo_ = w & WatchLo::kWriteMask; break;
    case WatchHi: watch_hi_ = w & 0xF; break;
    case XContext:
      xcontext_ = (xcontext_ & (XContext::BadVPN2::mask | XContext::R::mask)) |
                  (v & XContext::PTEBase::mask);
      break;
    case ParityError: parity_error_ = w & 0xFF; break;
    case CacheError: break;
    case TagLo: tag_lo_ = w & TagLo::kWriteMask; break;
    case TagHi: tag_hi_ = 0; break;  // RFU, reads zero (5.4.8)
    case ErrorEPC: error_epc_ = v; break;
    default: break;
  }
}

u64 Vr4300::cop0_read64(u32 reg) { return cop0_get(reg); }
void Vr4300::cop0_write64(u32 reg, u64 v) { cop0_set(reg, v); }

// ---- save state ------------------------------------------------------------------------------

void Vr4300::serialize(SaveState& s) {
  s.section("vr4300", 1);
  s.array(gpr_);
  s.value(hi_);
  s.value(lo_);
  s.value(pc_);
  s.value(npc_);
  s.value(in_delay_slot_);
  s.value(branch_pending_);
  s.value(llbit_);
  s.value(index_);
  s.value(entry_lo0_);
  s.value(entry_lo1_);
  s.value(context_);
  s.value(page_mask_);
  s.value(wired_);
  s.value(random_base_);
  s.value(badvaddr_);
  s.value(count_offset_);
  s.value(entry_hi_);
  s.value(compare_);
  s.value(status_);
  s.value(cause_);
  s.value(epc_);
  s.value(config_);
  s.value(lladdr_);
  s.value(watch_lo_);
  s.value(watch_hi_);
  s.value(xcontext_);
  s.value(parity_error_);
  s.value(tag_lo_);
  s.value(tag_hi_);
  s.value(error_epc_);
  s.value(cop0_latch_);
  s.value(timer_cycle_);
  s.array(fpr_);
  s.value(fcr31_);
  s.value(cop1_latch_);
  s.value(cop2_latch_);
  for (u32 i = 0; i < 32; ++i) {
    TlbEntry& e = tlb_[i];
    s.value(e.entry_hi);
    s.value(e.entry_lo0);
    s.value(e.entry_lo1);
    s.value(e.page_mask);
    if (s.loading()) tlb_refresh_entry(i);
  }
  for (ILine& l : icache_) {
    s.value(l.ptag);
    s.value(l.valid);
    s.array(l.data);
  }
  for (DLine& l : dcache_) {
    s.value(l.ptag);
    s.value(l.valid);
    s.value(l.dirty);
    s.array(l.data);
  }
  s.value(cycles_);
  s.value(retired_);
  s.value(frozen_);
  s.value(pending_load_kind_);
  s.value(pending_load_reg_);
  s.value(prev_dcache_store_);
  s.value(fpu_result_reg_);
  s.value(sysad_free_);
  s.value(wb_count_);
  s.value(wb_head_);
  for (PendingWrite& w : wb_) {
    s.value(w.paddr);
    s.value(w.size);
    s.value(w.master);
    s.value(w.ready);
    s.array(w.data);
  }
}

}  // namespace ultra
