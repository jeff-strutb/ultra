// VR4300 timing by pipeline stage (docs/adr/0005-cpu-pipeline-stages.md): fetch order, stall
// overlap, killed delay slots, exception cost by detecting stage, the instruction micro-TLB.

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>

#include "core/clock.hpp"
#include "cpu/timing.hpp"
#include "machine.hpp"

using namespace ultra;
using namespace ultra::mips;
using ultra::test::Machine;

namespace {

// I-cache: 32-byte lines (VR4300 manual 11.2).
constexpr u64 kILineBytes = 32;
// Handler reaches RF this long after the end of the detection cycle (manual 4.7).
constexpr CpuCycle kExceptionEntry = timing::kExceptionAbortCycles + timing::kExceptionRefetchCycles;

CpuCycle step_cost(Machine& m) {
  const CpuCycle before = m.cpu().cycles();
  m.cpu().step();
  return m.cpu().cycles() - before;
}

// Pads with NOPs until the next instruction is word `slot` (0..7) of an I-cache line.
void pad_to_slot(Assembler& a, u64 slot) {
  while ((a.pc() & (kILineBytes - 1)) != slot * 4) a.nop();
}

// Handler at the general exception vector: records Cause in k0 and EPC in k1, then jumps to
// `resume` with EXL still set (no ERET), so nothing else is taken.
void install_handler(Machine& m, u64 resume) {
  Assembler h(0xFFFF'FFFF'8000'0180ull);
  h.mfc0(k0, cop0::Cause);
  h.mfc0(k1, cop0::EPC);
  h.li(t9, static_cast<u32>(resume));
  h.jr(t9);
  h.nop();
  const auto code = h.finish();
  for (usize i = 0; i < code.size(); ++i) m.poke32(0x180 + static_cast<u32>(i) * 4, code[i]);
}

}  // namespace

TEST_CASE("pipeline: a nullified branch-likely delay slot costs one cycle") {
  // The slot was fetched before the branch resolved in EX; killing it leaves a bubble.
  Machine m;
  Assembler a(Machine::kCodeBase);
  const auto end = a.new_label();
  a.li(t0, 1);
  pad_to_slot(a, 0);
  const u64 beq = a.pc();
  a.beq(zero, t0, end);  // not taken; the slot executes
  a.nop();
  const u64 beql = a.pc();
  a.beql(zero, t0, end);  // not taken; the slot is nullified
  a.nop();
  a.nop();
  a.bind(end);
  a.b(end);
  a.nop();
  m.load(a.finish());
  REQUIRE(m.run_to(a.address_of(end)));  // first pass fills the I-cache
  m.cpu().set_pc(Machine::kCodeBase);
  REQUIRE(m.run_to(beq));
  CHECK(step_cost(m) == 1);
  REQUIRE(m.run_to(beql));
  CHECK(step_cost(m) == 1 + timing::kKilledSlotCycles);
  CHECK(m.cpu().pc() == beql + 8);
}

TEST_CASE("pipeline: an exception costs by the stage that detects it") {
  // Figure 4-12: SYSCALL is detected in EX, overflow in DC, a fetch address error in RF.
  SECTION("EX: SYSCALL") {
    Machine m;
    Assembler a(Machine::kCodeBase);
    const auto end = a.new_label();
    pad_to_slot(a, 2);
    const u64 at = a.pc();
    a.syscall();
    a.nop();
    a.bind(end);
    a.b(end);
    a.nop();
    install_handler(m, a.address_of(end));
    m.load(a.finish());
    REQUIRE(m.run_to(at));
    CHECK(step_cost(m) == 1 + kExceptionEntry);
    REQUIRE(m.run_to(a.address_of(end)));
    CHECK(((m.gpr(k0) >> 2) & 31) == 8);  // ExcCode Sys
  }
  SECTION("DC: ADD overflow") {
    Machine m;
    Assembler a(Machine::kCodeBase);
    const auto end = a.new_label();
    a.li(t0, 0x7FFF'FFFF);
    a.li(t1, 1);
    pad_to_slot(a, 2);  // the instruction behind ADD is in the same, already filled line
    const u64 at = a.pc();
    a.add(t2, t0, t1);
    a.nop();
    a.bind(end);
    a.b(end);
    a.nop();
    install_handler(m, a.address_of(end));
    m.load(a.finish());
    REQUIRE(m.run_to(at));
    CHECK(step_cost(m) == 1 + timing::kDcDetectAfterEx + kExceptionEntry);
    REQUIRE(m.run_to(a.address_of(end)));
    CHECK(((m.gpr(k0) >> 2) & 31) == 12);  // ExcCode Ov
  }
  SECTION("RF: fetch from an unaligned address") {
    Machine m;
    Assembler a(Machine::kCodeBase);
    const auto end = a.new_label();
    a.li(t3, static_cast<u32>(Machine::kCodeBase) + 0x102);
    a.jr(t3);
    a.nop();
    a.bind(end);
    a.b(end);
    a.nop();
    install_handler(m, a.address_of(end));
    m.load(a.finish());
    REQUIRE(m.run_to(Machine::kCodeBase + 0x102));
    CHECK(step_cost(m) == kExceptionEntry);
    REQUIRE(m.run_to(a.address_of(end)));
    CHECK(((m.gpr(k0) >> 2) & 31) == 4);  // ExcCode AdEL
  }
}

TEST_CASE("pipeline: an interrupt waits for a run cycle after a stall") {
  // Manual 4.7.1: an interrupt is accepted only if the previous PCycle was a run cycle. It is
  // detected in the cycle after the step boundary (DC). Software interrupt IP0 is pending
  // before Status enables it; the step after the MTC0 takes it.
  constexpr u32 kStatus = 0x3400'0000u | (1u << 8) | 1u;  // CU0/CU1/FR, IM0, IE
  for (const bool stalled : {false, true}) {
    Machine m;
    m.poke32(0x21'0000, kStatus);
    Assembler a(Machine::kCodeBase);
    const auto end = a.new_label();
    a.li(t0, 1u << 8);
    a.mtc0(t0, cop0::Cause);  // IP0
    a.li(t2, 0xA021'0000);
    a.li(t1, kStatus);
    pad_to_slot(a, 2);
    if (stalled) {
      a.lw(t1, 0, t2);  // uncached; the MTC0 behind it waits one cycle for t1 (LDI)
    } else {
      a.nop();
    }
    const u64 mtc0 = a.pc();
    a.mtc0(t1, cop0::Status);
    a.nop();
    a.bind(end);
    a.b(end);
    a.nop();
    install_handler(m, a.address_of(end));
    m.load(a.finish());
    REQUIRE(m.run_to(mtc0));
    m.cpu().step();  // MTC0 Status: its successor is in the same filled line, so no RF stall
    INFO("stalled " << stalled);
    CHECK(step_cost(m) == timing::kDcDetectAfterEx + (stalled ? 1 : 0) + kExceptionEntry);
    CHECK(((m.cpu().cop0_read64(cop0::Cause) >> 2) & 31) == 0);  // ExcCode Int
    CHECK(m.cpu().cop0_read64(cop0::EPC) == mtc0 + 4);
  }
}

TEST_CASE("pipeline: the next instruction is fetched before this instruction's data access") {
  // In one pipeline cycle EX holds i and RF holds i+1, and i's DC stage comes in the next cycle
  // (manual 4.1, Figure 4-2). Uncached code puts every fetch on the bus.
  Machine m;
  constexpr u64 kBase = 0xFFFF'FFFF'A020'0000ull;
  Assembler a(kBase);
  const auto end = a.new_label();
  a.li(t0, 0xA440'0010);  // VI_V_CURRENT
  a.nop();
  const u64 load = a.pc();
  a.lw(t1, 0, t0);
  a.nop();
  a.bind(end);
  a.b(end);
  a.nop();
  m.load(a.finish(), kBase);
  REQUIRE(m.run_to(load));
  const auto path = std::filesystem::temp_directory_path() / "ultra-pipeline-order.bin";
  REQUIRE(m.sys->trace().open(path.string()));
  m.cpu().step();
  m.sys->trace().close();
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in);
  TraceFileHeader h{};
  in.read(reinterpret_cast<char*>(&h), sizeof h);
  std::vector<TraceRecord> recs;
  for (TraceRecord r{}; in.read(reinterpret_cast<char*>(&r), sizeof r);) recs.push_back(r);
  in.close();
  std::filesystem::remove(path);
  REQUIRE(recs.size() == 2);
  CHECK(recs[0].master == Master::CpuInstr);
  CHECK(recs[0].address == (static_cast<u32>(load + 4) & 0x1FFF'FFFF));
  CHECK(recs[1].master == Master::CpuData);
  CHECK(recs[1].address == 0x0440'0010u);
  CHECK(recs[0].tick + recs[0].duration <= recs[1].tick);
}

TEST_CASE("pipeline: a multicycle instruction overlaps the next instruction's I-cache refill") {
  // Manual 4.7.3: RF (ICB) and EX (MCI) interlocks are resolved at the same time. MULT is the
  // last word of a line and the next line has never been fetched; the same program with a NOP
  // in MULT's place measures the refill alone.
  auto cost_at_line_end = [](bool mult, bool warm) {
    Machine m;
    Assembler a(Machine::kCodeBase);
    const auto end = a.new_label();
    a.li(t0, 3);
    a.li(t1, 5);
    pad_to_slot(a, 7);
    const u64 at = a.pc();
    if (mult) {
      a.mult(t0, t1);
    } else {
      a.nop();
    }
    a.nop();
    a.bind(end);
    a.b(end);
    a.nop();
    m.load(a.finish());
    if (warm) {
      REQUIRE(m.run_to(a.address_of(end)));
      m.cpu().set_pc(Machine::kCodeBase);
    }
    REQUIRE(m.run_to(at));
    return step_cost(m);
  };
  const CpuCycle refill = cost_at_line_end(false, false) - 1;
  REQUIRE(refill > timing::kMult - 1);
  CHECK(cost_at_line_end(true, false) == 1 + refill);
  CHECK(cost_at_line_end(true, true) == timing::kMult);
}

TEST_CASE("pipeline: a multicycle instruction overlaps the previous load's D-cache miss") {
  // Manual 4.6.1: EX completes a multicycle instruction while DC resolves a stall, unless it
  // waits for that load's result (then LDI first, manual 4.6.5).
  auto costs = [](bool dependent) {
    Machine m;
    m.poke32(0x21'0000, 7);
    Assembler a(Machine::kCodeBase);
    const auto end = a.new_label();
    a.li(t3, 0x8021'0000);  // cached; the D-cache was emptied by the fixture
    a.li(t1, 3);
    pad_to_slot(a, 2);
    const u64 at = a.pc();
    a.lw(t2, 0, t3);
    a.div(dependent ? t2 : t1, t1);
    a.nop();
    a.bind(end);
    a.b(end);
    a.nop();
    m.load(a.finish());
    REQUIRE(m.run_to(at));
    const CpuCycle lw = step_cost(m);
    const CpuCycle div = step_cost(m);
    return std::pair{lw, div};
  };
  const auto [lw, div] = costs(false);
  const CpuCycle miss = lw - 1;
  REQUIRE(miss > 0);
  REQUIRE(miss < timing::kDiv - 1);
  CHECK(div == 1 + (timing::kDiv - 1 - miss));
  const auto [lw2, div2] = costs(true);
  CHECK(lw2 == lw);
  CHECK(div2 == 1 + timing::kLoadInterlockCycles + timing::kDiv - 1);
}

TEST_CASE("pipeline: a mapped fetch that misses the instruction micro-TLB stalls") {
  // Manual 4.6.2 / 1.5.1: 2 entries of 4 KiB, refilled from the joint TLB in 3 PCycles. Code
  // at vaddr 0x0001_0FF8..0x0001_1007 spans two pages, mapped by one global entry to paddr
  // 0x0030_0000 / 0x0030_1000, cached. An EntryHi ASID change flushes the micro-TLB (ledger
  // "itlb-invalidation"); the global entry still maps.
  Machine m;
  constexpr u32 kVpn2 = 0x0001'0000;
  Assembler mapped(kVpn2 + 0x0FF8);
  mapped.nop();
  const u64 cross = mapped.pc();  // its RF fetch is the first word of the odd page
  mapped.nop();
  mapped.jr(ra);
  mapped.nop();
  const auto mcode = mapped.finish();
  for (usize i = 0; i < mcode.size(); ++i) m.poke32(0x30'0FF8 + static_cast<u32>(i) * 4, mcode[i]);

  Assembler a(Machine::kCodeBase);
  const auto end = a.new_label();
  a.mtc0(zero, cop0::PageMask);
  a.li(t0, kVpn2);
  a.mtc0(t0, cop0::EntryHi);
  a.li(t0, (0x300u << 6) | (3u << 3) | 0x7);  // PFN, C=3 cacheable, D, V, G
  a.mtc0(t0, cop0::EntryLo0);
  a.li(t0, (0x301u << 6) | (3u << 3) | 0x7);
  a.mtc0(t0, cop0::EntryLo1);
  a.mtc0(zero, cop0::Index);
  a.tlbwi();
  a.li(t9, kVpn2 + 0x0FF8);
  const u64 call1 = a.pc();
  a.jalr(ra, t9);  // first pass fills the I-cache and the micro-TLB
  a.nop();
  const u64 call2 = a.pc();
  a.jalr(ra, t9);  // both pages in the micro-TLB
  a.nop();
  a.li(t0, kVpn2 | 1);  // ASID 1
  a.mtc0(t0, cop0::EntryHi);
  a.nop();
  const u64 call3 = a.pc();
  a.jalr(ra, t9);  // micro-TLB flushed
  a.nop();
  a.bind(end);
  a.b(end);
  a.nop();
  m.load(a.finish());
  REQUIRE(m.run_to(call1));
  REQUIRE(m.run_to(call2));
  REQUIRE(m.run_to(cross));
  CHECK(step_cost(m) == 1);
  REQUIRE(m.run_to(call3));
  m.cpu().step();  // JALR
  // The delay slot's RF fetch is the first mapped word: a micro-TLB miss after the flush.
  CHECK(step_cost(m) == 1 + timing::kItlbMissCycles);
  REQUIRE(m.run_to(cross));
  CHECK(step_cost(m) == 1 + timing::kItlbMissCycles);
  REQUIRE(m.run_to(a.address_of(end)));
}
