#include "io/pi.hpp"

#include <algorithm>
#include <cstring>

#include "core/savestate.hpp"
#include "io/cart.hpp"
#include "io/mi.hpp"
#include "memory/ri.hpp"

namespace ultra {

namespace {
// PI internal buffer is 128 bytes; blocks never cross a 2 KiB RDRAM page.
// Source: n64brew "Parallel Interface", DMA Transfers / Internal process.
constexpr u32 kBufferBytes = 128;
constexpr u32 kRdramPage = 0x800;

// Address phase (ALEH/ALEL) cost per new PI page address, in MasterClock ticks.
// Guess: n64brew describes ALEH high >= 7 and ALEL high >= 14 "FSB cycles" without a unit
// conversion; probe "pi-dma" (Phase 1 probe 3 family) measures it. Ledger "pi-address-phase".
constexpr u64 kAddressPhaseTicks = 14;
}  // namespace

Pi::Pi(Scheduler& sched, Mi& mi, Ri& ri, Cartridge* cart)
    : sched_(sched), mi_(mi), ri_(ri), cart_(cart) {
  sched_.add(dma_ev_, "pi.dma", &Pi::dma_event, this);
}

const Pi::Domain& Pi::domain_for(u32 a) const noexcept {
  // Domain 2: 0x0500_0000-0x05FF_FFFF and 0x0800_0000-0x0FFF_FFFF; everything else domain 1.
  // Source: n64brew "Parallel Interface", Domains table.
  if ((a >= 0x0500'0000 && a < 0x0600'0000) || (a >= 0x0800'0000 && a < 0x1000'0000))
    return dom_[1];
  return dom_[0];
}

u64 Pi::bus_ticks(u32 pi_addr, u32 bytes) const noexcept {
  // Per n64brew "Parallel Interface" register descriptions: after the address phase the PI
  // waits LAT+1 cycles, holds /RD or /WR low PWD+1 cycles per 16-bit word and high RLS+1
  // cycles between words. A new address phase is issued at every page boundary
  // (page = 2^(PGS+2) bytes).
  const Domain& d = domain_for(pi_addr);
  const u32 page = 1u << (d.pgs + 2);
  u64 t = 0;
  u32 a = pi_addr & ~1u;
  u32 words = (bytes + 1) / 2;
  while (words > 0) {
    const u32 in_page = std::min<u32>(words, (page - (a & (page - 1)) + 1) / 2);
    t += kAddressPhaseTicks + (d.lat + 1);
    t += u64{in_page} * (d.pwd + 1) + u64{in_page - 1} * (d.rls + 1);
    a += in_page * 2;
    words -= in_page;
  }
  return t;
}

u16 Pi::bus_read16(u32 addr) const noexcept {
  u16 v;
  if (cart_ && cart_->read16(addr, v)) return v;
  // Open bus: the low 16 bits of the address (n64brew "Parallel Interface", Open bus).
  return static_cast<u16>(addr);
}

u32 Pi::read_reg(u32 paddr, Tick t) {
  switch (static_cast<PiRegister>(paddr & 0x3C)) {
    case PiRegister::DramAddr: return dram_addr_;
    case PiRegister::CartAddr: return cart_addr_;
    // The length registers read back 0x7F, or 0x7F minus the RDRAM misalignment after a
    // transfer shorter than 8 bytes (n64brew "Parallel Interface", PI_WR_LEN readbacks).
    case PiRegister::RdLen:
    case PiRegister::WrLen: return len_readback_;
    case PiRegister::Status: {
      u32 s = 0;
      if (dma_.active) s |= PiStatusRead::DmaBusy::mask;
      if (t < io_busy_until_) s |= PiStatusRead::IoBusy::mask;
      if (error_) s |= PiStatusRead::Error::mask;
      if (mi_.pending(MiIntr::PI)) s |= PiStatusRead::Interrupt::mask;
      return s;
    }
    case PiRegister::Dom1Lat: return dom_[0].lat;
    case PiRegister::Dom1Pwd: return dom_[0].pwd;
    case PiRegister::Dom1Pgs: return dom_[0].pgs;
    case PiRegister::Dom1Rls: return dom_[0].rls;
    case PiRegister::Dom2Lat: return dom_[1].lat;
    case PiRegister::Dom2Pwd: return dom_[1].pwd;
    case PiRegister::Dom2Pgs: return dom_[1].pgs;
    case PiRegister::Dom2Rls: return dom_[1].rls;
  }
  return 0;
}

void Pi::write_reg(u32 paddr, u32 v, Tick t) {
  const auto reg = static_cast<PiRegister>(paddr & 0x3C);
  if (reg == PiRegister::Status) {
    if (PiStatusWrite::Reset::test(v)) {
      // Reset the DMA controller and stop any transfer (n64brew PI_STATUS write bit 0).
      if (dma_.active) sched_.cancel(dma_ev_);
      dma_ = Dma{};
      error_ = false;
    }
    if (PiStatusWrite::ClearIntr::test(v)) mi_.lower(MiIntr::PI);
    return;
  }
  // Writes to DMA registers while a DMA is running are ignored and flag an error.
  // Guess (ledger "pi-busy-writes"): the error bit's exact trigger is unmeasured.
  if (dma_.active &&
      (reg == PiRegister::DramAddr || reg == PiRegister::CartAddr || reg == PiRegister::RdLen ||
       reg == PiRegister::WrLen)) {
    error_ = true;
    return;
  }
  switch (reg) {
    case PiRegister::DramAddr: dram_addr_ = v & 0x00FF'FFFE; break;
    case PiRegister::CartAddr: cart_addr_ = v & 0xFFFF'FFFE; break;
    case PiRegister::RdLen:
    case PiRegister::WrLen: {
      const u32 len = (v & 0x00FF'FFFF) + 1;
      len_readback_ = len < 8 ? 0x7F - (dram_addr_ & 7) : 0x7F;
      dma_.remaining = len;
      start_dma(reg == PiRegister::WrLen, t);
      break;
    }
    case PiRegister::Dom1Lat: dom_[0].lat = v & 0xFF; break;
    case PiRegister::Dom1Pwd: dom_[0].pwd = v & 0xFF; break;
    case PiRegister::Dom1Pgs: dom_[0].pgs = v & 0xF; break;
    case PiRegister::Dom1Rls: dom_[0].rls = v & 0x3; break;
    case PiRegister::Dom2Lat: dom_[1].lat = v & 0xFF; break;
    case PiRegister::Dom2Pwd: dom_[1].pwd = v & 0xFF; break;
    case PiRegister::Dom2Pgs: dom_[1].pgs = v & 0xF; break;
    case PiRegister::Dom2Rls: dom_[1].rls = v & 0x3; break;
    default: break;
  }
}

Tick Pi::io_read(u32 paddr, u32& out, Tick t) {
  // A read during a pending write returns the value being written.
  if (t < io_busy_until_) {
    out = io_latch_;
    return io_busy_until_;
  }
  // Two 16-bit bus reads: the MSB at the (16-bit aligned) address, the LSB at address + 2.
  // Source: n64brew "Memory map", PI external bus.
  const u32 a = paddr & ~1u;
  out = (u32{bus_read16(a)} << 16) | bus_read16(a + 2);
  cart_addr_ = a + 4;
  return t + bus_ticks(a, 4);
}

Tick Pi::io_write(u32 paddr, u32 value, Tick t) {
  // Writes are latched and performed in the background; writes during a pending write are
  // ignored. The CPU is released immediately.
  if (t < io_busy_until_) return t + kWriteAcceptTicks;
  const u32 a = paddr & ~1u;
  if (cart_) {
    cart_->write16(a, static_cast<u16>(value >> 16));
    cart_->write16(a + 2, static_cast<u16>(value));
  }
  cart_addr_ = a + 4;
  io_latch_ = value;
  io_busy_until_ = t + bus_ticks(a, 4);
  return t + kWriteAcceptTicks;
}

void Pi::start_dma(bool to_rdram, Tick t) {
  dma_.active = true;
  dma_.to_rdram = to_rdram;
  dma_.first = true;
  dma_.first_index = dram_addr_ & 7;
  dma_.block = 0;
  // The first block's bus time elapses before its RDRAM write.
  const u32 n = std::min<u32>(dma_.remaining, kBufferBytes);
  sched_.schedule(dma_ev_, t + bus_ticks(cart_addr_, n));
}

void Pi::dma_step(Tick t) {
  u8 buf[kBufferBytes + 2] = {};
  Tick done = t;
  if (dma_.to_rdram) {
    // Cart -> RDRAM. Rules fitted to n64-systemtest's hardware-derived PI DMA cases
    // (src/tests/cart_memory/dma.rs: small sizes, misaligned, misaligned cross page,
    // misaligned end of page); see docs/hardware/io.md "PI DMA block rules":
    //  - the 128-byte buffer is filled from index `first_index` (the starting RDRAM
    //    misalignment) until a block extends past the buffer's first octword, then from 0;
    //  - a block is limited by the remaining length, the RDRAM 2 KiB page and the buffer end;
    //  - odd lengths round up to 16-bit words except in the first block when it does not
    //    reach the buffer's last word;
    //  - the write-back stores L - (DRAM_ADDR & 7) bytes at DRAM_ADDR (the hardware counts
    //    the block end from buffer index 0);
    //  - DRAM_ADDR advances by the bytes written, rounded up to 8; CART_ADDR advances by the
    //    (16-bit rounded) block length.
    const u32 idx = dma_.first ? dma_.first_index : 0;
    const u32 page_left = kRdramPage - (dram_addr_ & (kRdramPage - 1));
    u32 n = std::min({dma_.remaining, kBufferBytes - idx, page_left});
    const bool round_odd = dma_.block > 0 || idx + n >= kBufferBytes - 1;
    const u32 len = round_odd ? (n + 1) & ~1u : n;
    const u32 fetch = (len + 1) & ~1u;
    for (u32 i = 0; i < fetch; i += 2) {
      const u16 w = bus_read16(cart_addr_ + i);
      buf[i] = static_cast<u8>(w >> 8);
      buf[i + 1] = static_cast<u8>(w);
    }
    const u32 mis = dram_addr_ & 7;
    const u32 written = len > mis ? len - mis : 0;
    if (written) done = ri_.write(Master::Pi, dram_addr_, buf, written, t);
    cart_addr_ += fetch;
    dram_addr_ = ((dram_addr_ + written + 7) & ~7u) & 0x00FF'FFFE;
    dma_.remaining -= std::min(dma_.remaining, n);
    if (idx + n > 8) dma_.first = false;
    ++dma_.block;
  } else {
    // RDRAM -> cart.
    const u32 page_left = kRdramPage - (dram_addr_ & (kRdramPage - 1));
    const u32 n = std::min({dma_.remaining, kBufferBytes, page_left});
    const u32 len = (n + 1) & ~1u;
    done = ri_.read(Master::Pi, dram_addr_, buf, len, t);
    for (u32 i = 0; i < len; i += 2)
      if (cart_) cart_->write16(cart_addr_ + i, static_cast<u16>((buf[i] << 8) | buf[i + 1]));
    cart_addr_ += len;
    dram_addr_ = (dram_addr_ + len) & 0x00FF'FFFE;
    dma_.remaining -= std::min(dma_.remaining, n);
    dma_.first = false;
  }

  if (dma_.remaining == 0) {
    dma_.active = false;
    mi_.raise(MiIntr::PI);
    return;
  }
  const u32 next = std::min<u32>(dma_.remaining, kBufferBytes);
  sched_.schedule(dma_ev_, std::max(done, t) + bus_ticks(cart_addr_, next));
}

void Pi::serialize(SaveState& s) {
  s.section("pi", 1);
  s.value(dram_addr_);
  s.value(cart_addr_);
  s.value(len_readback_);
  s.value(error_);
  for (Domain& d : dom_) {
    s.value(d.lat);
    s.value(d.pwd);
    s.value(d.pgs);
    s.value(d.rls);
  }
  s.value(io_busy_until_);
  s.value(io_latch_);
  s.value(dma_.active);
  s.value(dma_.to_rdram);
  s.value(dma_.first);
  s.value(dma_.first_index);
  s.value(dma_.block);
  s.value(dma_.remaining);
}

}  // namespace ultra
