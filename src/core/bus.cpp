#include "core/bus.hpp"

#include <cstring>

#include "core/scheduler.hpp"
#include "io/ai.hpp"
#include "io/mi.hpp"
#include "io/pi.hpp"
#include "io/si.hpp"
#include "io/vi.hpp"
#include "memory/ri.hpp"
#include "rdp/dp_interface.hpp"
#include "rsp/sp_interface.hpp"

namespace ultra {

namespace {
// RCP register / SP memory read response: from the SysAD address cycle to the data word on
// SysAD. Fitted to krom CPUTIMINGNTSC, whose hardware-measured counts (about 56092 loops of
// {ALU op, LW VI_V_CURRENT, SYNC, BNE, ADDIU} per 256 NTSC lines) put the loop at 18
// MasterClocks; with the VR4300-side stall of Tables 11-1 (cpu/timing.hpp) that leaves 10 ticks
// for the RCP. Measured: ledger "rcp-reg-read".
constexpr Tick kRcpRegReadTicks = 10;
// MI registers answer faster: n64brew "Memory map" gives "5-6 PClock cycles" for standard
// registers and "about 2" for MI, a difference of 3-4 PClocks = 2.0-2.7 ticks, taken as 3.
// Guess: ledger "mi-reg-read".
constexpr Tick kMiRegReadTicks = kRcpRegReadTicks - 3;
// Register writes: "5-6 PClock cycles (MI ... about 2)" = 4 / 2 ticks, rounded up.
// Derived: n64brew "Memory map", Range 0x0400'0000 - 0x04FF'FFFF.
constexpr Tick kRcpRegWriteTicks = 4;
constexpr Tick kMiRegWriteTicks = 2;

// Physical address map. Source: n64brew "Memory map", Physical Memory Map table.
constexpr u32 kRdramEnd = 0x0400'0000;    // RDRAM and RDRAM registers below
constexpr u32 kSpMemEnd = 0x0404'0000;    // SP DMEM/IMEM (and mirrors) from kRdramEnd
constexpr u32 kSpRegEnd = 0x040C'0000;    // SP registers; SP_PC/SP_IBIST from 0x0408'0000
constexpr u32 kSpPcRegsBit = 0x0008'0000;  // set for the SP_PC/IBIST block within SP registers
// RCP interface register blocks, 1 MiB each, from 0x0410'0000.
constexpr u32 kRcpBlockBytes = 0x0010'0000;
constexpr u32 kDpCmdRegs = 0x0410'0000;
constexpr u32 kDpSpanRegs = 0x0420'0000;
constexpr u32 kMiRegs = 0x0430'0000;
constexpr u32 kViRegs = 0x0440'0000;
constexpr u32 kAiRegs = 0x0450'0000;
constexpr u32 kPiRegs = 0x0460'0000;
constexpr u32 kRiRegs = 0x0470'0000;
constexpr u32 kSiRegs = 0x0480'0000;
constexpr u32 kRcpEnd = kSiRegs + kRcpBlockBytes;  // 0x0490'0000..0x04FF'FFFF: unmapped
constexpr u32 kPiBus1 = 0x0500'0000;      // PI external bus: 64DD, SRAM, cartridge ROM
constexpr u32 kSiBus = 0x1FC0'0000;       // SI external bus: PIF ROM and RAM
constexpr u32 kPiBus2 = 0x1FD0'0000;      // PI external bus again, up to kPiBusEnd
constexpr u32 kPiBusEnd = 0x8000'0000;

bool is_block(u32 size) { return size > 8; }
}  // namespace

void Bus::trace_io(Master m, TraceKind k, u32 paddr, const u8* data, u32 len, Tick t, Tick done) {
  if (!d_.trace->enabled()) return;
  TraceRecord r{};
  r.tick = t;
  r.address = paddr;
  r.duration = done == kTickNever ? ~0u : static_cast<u32>(done - t);
  u8 first[8] = {};
  std::memcpy(first, data, len < 8 ? len : 8);
  r.data = load_be64(first);
  r.size = static_cast<u16>(len);
  r.master = m;
  r.kind = k;
  d_.trace->record(r);
}

Tick Bus::cpu_read(Master m, u32 paddr, u32 size, u8* out, Tick t) {
  d_.sched->run_until(t);
  if (paddr < kRdramEnd) return d_.mi->cpu_read(m, paddr, size, out, t);

  // Everything outside RDRAM: cached (block) accesses are never answered, and 64-bit reads
  // hang waiting for a second word the RCP never sends (n64brew "Memory map").
  if (is_block(size) || size == 8) return kTickNever;

  u32 w = 0;
  Tick done;
  if (paddr < kRcpEnd) {
    done = rcp_read(paddr, size, out, t);
    if (done == kTickNever) return done;
    w = load_be32(out);
  } else if (paddr >= kSiBus && paddr < kPiBus2) {
    done = d_.si->io_read(paddr, w, t);
  } else if (paddr >= kPiBus1 && paddr < kPiBusEnd) {
    done = d_.pi->io_read(paddr, w, t);
  } else {
    return kTickNever;  // 0x0490_0000..0x04FF_FFFF and 0x8000_0000+ are not answered
  }
  store_be32(out, w);
  trace_io(m, TraceKind::Read, paddr, out, 4, t, done);
  return done;
}

Tick Bus::cpu_write(Master m, u32 paddr, u32 size, const u8* data, Tick t) {
  d_.sched->run_until(t);
  if (paddr < kRdramEnd) return d_.mi->cpu_write(m, paddr, size, data, t);
  if (is_block(size)) return kTickNever;
  // Access size is ignored: the 32-bit SysAD word is written as-is; for 64-bit writes the
  // most significant word (first on the bus) is used (n64brew "Memory map").
  const u32 w = load_be32(data);
  Tick done;
  if (paddr < kRcpEnd) {
    done = rcp_write(paddr, size, data, t);
  } else if (paddr >= kSiBus && paddr < kPiBus2) {
    done = d_.si->io_write(paddr, w, t);
  } else if (paddr >= kPiBus1 && paddr < kPiBusEnd) {
    done = d_.pi->io_write(paddr, w, t);
  } else if (paddr >= kPiBusEnd) {
    done = t + kWriteAcceptTicks;  // writes to 0x8000_0000+ are ignored by the RCP
  } else {
    done = kTickNever;
  }
  trace_io(m, TraceKind::Write, paddr, data, 4, t, done);
  return done;
}

Tick Bus::rcp_read(u32 paddr, u32, u8* out, Tick t) {
  // Blocks in address order: each test is against the start of the next block.
  u32 v = 0;
  Tick done = t + kRcpRegReadTicks;
  if (paddr < kSpMemEnd) {
    v = d_.sp->read_mem(paddr);
  } else if (paddr < kSpRegEnd) {
    v = (paddr & kSpPcRegsBit) ? d_.sp->read_pc_reg(paddr) : d_.sp->read_reg(paddr, t);
  } else if (paddr < kDpCmdRegs) {
    return kTickNever;
  } else if (paddr < kDpSpanRegs) {
    v = d_.dp->read_reg(paddr, t);
  } else if (paddr < kMiRegs) {
    v = 0;  // DP span registers: not modelled (ledger "dp-span")
  } else if (paddr < kViRegs) {
    v = d_.mi->read_reg(paddr);
    done = t + kMiRegReadTicks;
  } else if (paddr < kAiRegs) {
    v = d_.vi->read_reg(paddr, t);
  } else if (paddr < kPiRegs) {
    v = d_.ai->read_reg(paddr, t);
  } else if (paddr < kRiRegs) {
    v = d_.pi->read_reg(paddr, t);
  } else if (paddr < kSiRegs) {
    v = d_.ri->read_reg(paddr);
  } else {
    v = d_.si->read_reg(paddr, t);
  }
  store_be32(out, v);
  return done;
}

Tick Bus::rcp_write(u32 paddr, u32, const u8* data, Tick t) {
  // Blocks in address order: each test is against the start of the next block.
  const u32 v = load_be32(data);
  Tick done = t + kRcpRegWriteTicks;
  if (paddr < kSpMemEnd) {
    d_.sp->write_mem(paddr, v);
  } else if (paddr < kSpRegEnd) {
    if (paddr & kSpPcRegsBit)
      d_.sp->write_pc_reg(paddr, v);
    else
      d_.sp->write_reg(paddr, v, t);
  } else if (paddr < kDpCmdRegs) {
    return kTickNever;
  } else if (paddr < kDpSpanRegs) {
    d_.dp->write_reg(paddr, v, t);
  } else if (paddr < kMiRegs) {
    // DP span registers: not modelled
  } else if (paddr < kViRegs) {
    d_.mi->write_reg(paddr, v);
    done = t + kMiRegWriteTicks;
  } else if (paddr < kAiRegs) {
    d_.vi->write_reg(paddr, v, t);
  } else if (paddr < kPiRegs) {
    d_.ai->write_reg(paddr, v, t);
  } else if (paddr < kRiRegs) {
    d_.pi->write_reg(paddr, v, t);
  } else if (paddr < kSiRegs) {
    d_.ri->write_reg(paddr, v);
  } else {
    d_.si->write_reg(paddr, v, t);
  }
  return done;
}

}  // namespace ultra
