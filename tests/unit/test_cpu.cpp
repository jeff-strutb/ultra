// VR4300 behaviour on a booted machine: programs assembled with mips::Assembler run from RDRAM.

#include <catch2/catch_test_macros.hpp>

#include "core/clock.hpp"
#include "cpu/timing.hpp"
#include "machine.hpp"

using namespace ultra;
using namespace ultra::mips;
using ultra::test::Machine;

namespace {

// DMTC1 rt, fs (COP1 rs = 5).
u32 dmtc1(Reg rt, u32 fs) { return (0x11u << 26) | (0x05u << 21) | (u32{rt} << 16) | (fs << 11); }

// Loads a 64-bit constant whose low 32 bits are zero into rt.
void li_hi(Assembler& a, Reg rt, u32 hi) {
  a.li(rt, hi);
  a.dsll32(rt, rt, 0);
}

// Handler stub at the general exception vector (0x8000_0180): records Cause in k0 and EPC in k1,
// then jumps to `resume`.
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

TEST_CASE("cpu: ALU results, branch delay slots and branch-likely nullification") {
  Machine m;
  Assembler a(Machine::kCodeBase);
  const auto l1 = a.new_label(), l2 = a.new_label(), end = a.new_label();
  a.li(t0, 5);
  a.li(t1, 7);
  a.move(t4, zero);
  a.addu(t2, t0, t1);
  a.beq(t0, t0, l1);
  a.addiu(t3, zero, 1);   // delay slot: executes
  a.addiu(t3, zero, 99);  // skipped
  a.bind(l1);
  a.beql(t0, t1, l2);     // not taken: delay slot nullified
  a.addiu(t4, zero, 1);
  a.addiu(t5, zero, 2);
  a.bind(l2);
  a.bind(end);
  a.b(end);
  a.nop();
  m.load(a.finish());
  REQUIRE(m.run_to(a.address_of(end)));
  CHECK(m.gpr(t2) == 12);
  CHECK(m.gpr(t3) == 1);
  CHECK(m.gpr(t4) == 0);
  CHECK(m.gpr(t5) == 2);
}

TEST_CASE("cpu: ADD overflow raises Ov with EPC at the instruction and leaves rd unchanged") {
  Machine m;
  Assembler a(Machine::kCodeBase);
  const auto fault = a.new_label(), end = a.new_label();
  // The handler's I-cache line may hold stale code: invalidate it (Index_Invalidate, I-cache).
  a.li(t0, 0x8000'0180);
  a.cache(0x00, 0, t0);
  a.li(t0, 0x7FFF'FFFF);
  a.li(t1, 1);
  a.li(t2, 0x55);
  a.bind(fault);
  a.add(t2, t0, t1);
  a.nop();
  a.bind(end);
  a.b(end);
  a.nop();
  const auto code = a.finish();
  install_handler(m, a.address_of(end));
  m.load(code);
  REQUIRE(m.run_to(a.address_of(end)));
  CHECK(((m.gpr(k0) >> 2) & 31) == 12);  // ExcCode Ov
  CHECK(m.gpr(k1) == a.address_of(fault));
  CHECK(m.gpr(t2) == 0x55);
}

TEST_CASE("cpu: LWL/LWR merge big-endian bytes; partial LWR keeps the upper bits") {
  Machine m;
  m.poke32(0x21'0000, 0x0011'2233);
  m.poke32(0x21'0004, 0x4455'6677);
  Assembler a(Machine::kCodeBase);
  const auto end = a.new_label();
  a.li(t0, 0xA021'0000);  // uncached view of the data
  a.li(t1, 0xAABB'CCDD);
  a.li(t2, 0xAABB'CCDD);
  a.lwl(t1, 1, t0);
  a.lwr(t2, 1, t0);
  a.li(t3, 0);
  a.lwl(t3, 0, t0);
  a.lwr(t3, 3, t0);  // full word: equivalent to LW
  a.bind(end);
  a.b(end);
  a.nop();
  m.load(a.finish());
  REQUIRE(m.run_to(a.address_of(end)));
  CHECK(m.gpr(t1) == 0x0000'0000'1122'33DDull);
  CHECK(m.gpr(t2) == 0xFFFF'FFFF'AABB'0011ull);
  CHECK(m.gpr(t3) == 0x0000'0000'0011'2233ull);
}

TEST_CASE("cpu: a TLB entry written with TLBWI maps kuseg and TLBP finds it") {
  Machine m;
  m.poke32(0x30'0004, 0xCAFE'BABE);
  m.poke32(0x30'1008, 0x1234'5678);
  Assembler a(Machine::kCodeBase);
  const auto end = a.new_label();
  // 4 KiB pages: VPN2 0x0040_0000 -> even page 0x0030_0000, odd page 0x0030_1000; uncached (C=2),
  // dirty, valid, global.
  a.mtc0(zero, cop0::PageMask);
  a.li(t0, 0x0040'0000);
  a.mtc0(t0, cop0::EntryHi);
  a.li(t0, (0x300u << 6) | (2u << 3) | 0x7);
  a.mtc0(t0, cop0::EntryLo0);
  a.li(t0, (0x301u << 6) | (2u << 3) | 0x7);
  a.mtc0(t0, cop0::EntryLo1);
  a.li(t0, 5);
  a.mtc0(t0, cop0::Index);
  a.tlbwi();
  a.li(t0, 0x0040'0004);
  a.lw(t1, 0, t0);
  a.lw(t2, 0x1004, t0);
  a.mtc0(zero, cop0::Index);
  a.li(t0, 0x0040'0000);
  a.mtc0(t0, cop0::EntryHi);
  a.tlbp();
  a.nop();
  a.mfc0(t3, cop0::Index);
  a.bind(end);
  a.b(end);
  a.nop();
  m.load(a.finish());
  REQUIRE(m.run_to(a.address_of(end)));
  CHECK(m.gpr(t1) == 0xFFFF'FFFF'CAFE'BABEull);
  CHECK(m.gpr(t2) == 0x1234'5678ull);
  CHECK(m.gpr(t3) == 5);
}

TEST_CASE("cpu: FPU operations complete early when the result is decided by the operands") {
  // VR4300 manual Table 7-14 and section 7.5. Each operation is timed on a second pass so that
  // its fetch hits the I-cache.
  Machine m;
  Assembler a(Machine::kCodeBase);
  const auto end = a.new_label();
  a.word(dmtc1(zero, 0));  // f0 = +0.0
  li_hi(a, t0, 0x4008'0000);
  a.word(dmtc1(t0, 2));  // f2 = 3.0
  li_hi(a, t0, 0x3FF0'0000);
  a.word(dmtc1(t0, 4));  // f4 = 1.0
  a.nop();
  a.nop();
  const u64 ops = a.pc();
  constexpr u32 D = 17, kAdd = 0, kMul = 2, kDiv = 3, kSqrt = 4;
  struct Case {
    u32 fn, fd, fs, ft;
    u64 cycles;
  };
  const Case cases[] = {
      {kDiv, 6, 0, 2, timing::kFpuTrivial},   // 0 / 3 = 0
      {kDiv, 8, 2, 4, timing::kFpuDivSqrtD},  // 3 / 1
      {kMul, 10, 2, 2, timing::kFpuMulD},     // 3 * 3
      {kMul, 12, 2, 4, timing::kFpuTrivial},  // 3 * 1.0 (power of two)
      {kSqrt, 14, 0, 0, timing::kFpuTrivial}, // sqrt(0)
      {kSqrt, 16, 2, 0, timing::kFpuDivSqrtD},
      {kAdd, 18, 2, 2, timing::kFpuAddSub},   // 3 + 3
      {kAdd, 20, 2, 0, timing::kFpuTrivial},  // 3 + 0
  };
  for (const Case& c : cases) {
    a.fpu(D, c.fn, c.fd, c.fs, c.ft);
    a.nop();  // keeps every op independent of the previous result (no bypass stall)
  }
  a.bind(end);
  a.b(end);
  a.nop();
  m.load(a.finish());
  REQUIRE(m.run_to(a.address_of(end)));
  m.cpu().set_pc(Machine::kCodeBase);
  for (usize i = 0; i < std::size(cases); ++i) {
    REQUIRE(m.run_to(ops + i * 8));
    const CpuCycle before = m.cpu().cycles();
    m.cpu().step();
    INFO("case " << i);
    CHECK(m.cpu().cycles() - before == cases[i].cycles);
  }
}

TEST_CASE("cpu: an uncached RCP register read stalls for the SysAD round trip") {
  // VR4300 manual Table 11-1 around the RCP's response (core/bus.cpp): 1 issue + 5 + 0..1
  // (MasterClock alignment) + 15 (10 ticks) + 1, within the MasterClock phase rounding.
  Machine m;
  Assembler a(Machine::kCodeBase);
  const auto end = a.new_label();
  a.li(t0, 0xA440'0010);  // VI_V_CURRENT
  a.nop();
  const u64 load = a.pc();
  a.lw(t1, 0, t0);
  a.nop();
  a.bind(end);
  a.b(end);
  a.nop();
  m.load(a.finish());
  REQUIRE(m.run_to(a.address_of(end)));
  m.cpu().set_pc(Machine::kCodeBase);
  REQUIRE(m.run_to(load));
  const CpuCycle before = m.cpu().cycles();
  m.cpu().step();
  const CpuCycle d = m.cpu().cycles() - before;
  CHECK(d >= 22);
  CHECK(d <= 24);
}

TEST_CASE("cpu: a branch in a jump's delay slot offsets from the jump target") {
  // Hardware adds a branch's offset to the address of the instruction fetched after it, which is
  // the outer jump's target here, and links past that address (n64-systemtest BEQWithinDelay,
  // BGEZALWithinDelayOfBEQ). Each landing instruction sets one bit of t0.
  Machine m;
  Assembler a(Machine::kCodeBase);
  const auto outer = a.new_label(), inner = a.new_label(), end = a.new_label();
  a.move(t0, zero);
  a.j(outer);
  a.bgezal(zero, inner);  // delay slot: taken, offset 8 words from the slot after it
  for (int i = 0; i < 5; ++i) a.nop();
  a.bind(outer);
  a.ori(t0, t0, 1);       // the outer target executes as the branch's delay slot
  a.ori(t0, t0, 2);
  a.ori(t0, t0, 4);
  a.bind(inner);          // the branch's nominal target (branch address + 4 + offset)
  for (u32 bit = 3; bit <= 9; ++bit) a.ori(t0, t0, static_cast<u16>(1u << bit));
  a.bind(end);
  a.b(end);
  a.nop();
  m.load(a.finish());
  REQUIRE(m.run_to(a.address_of(end)));
  // Offset = inner - (branch + 4) = 8 words; from outer that lands on the bit-8 ORI.
  CHECK(m.gpr(t0) == (1u | 256u | 512u));
  CHECK(m.gpr(ra) == a.address_of(outer) + 4);
}

TEST_CASE("cpu: BC1T in a jump's delay slot offsets from the jump target") {
  // BC1T forms its target like every other conditional branch: the offset is added to the address
  // of the instruction fetched after it ([UM] BC1T / BEQ, docs/hardware/cpu.md Integer quirks),
  // which is the outer jump's target here. Each landing instruction sets one bit of t0.
  Machine m;
  Assembler a(Machine::kCodeBase);
  const auto outer = a.new_label(), inner = a.new_label(), end = a.new_label();
  a.lui(t1, 0x0080);  // FCR31.C
  a.ctc1(t1, 31);
  a.move(t0, zero);
  a.j(outer);
  a.bc1t(inner);  // delay slot: taken, offset 8 words from the slot after it
  for (int i = 0; i < 5; ++i) a.nop();
  a.bind(outer);
  a.ori(t0, t0, 1);  // the outer target executes as the branch's delay slot
  a.ori(t0, t0, 2);
  a.ori(t0, t0, 4);
  a.bind(inner);  // the branch's nominal target (branch address + 4 + offset)
  for (u32 bit = 3; bit <= 9; ++bit) a.ori(t0, t0, static_cast<u16>(1u << bit));
  a.bind(end);
  a.b(end);
  a.nop();
  m.load(a.finish());
  REQUIRE(m.run_to(a.address_of(end)));
  // Offset = inner - (branch + 4) = 8 words; from outer that lands on the bit-8 ORI.
  CHECK(m.gpr(t0) == (1u | 256u | 512u));
}

TEST_CASE("cpu: a Wired write resets Random two instructions later") {
  // n64-systemtest RandomDecrement / RandomReadEarly: MFC0 Random right after MTC0 Wired reads
  // the old sequence, one instruction later reads 31, and it decrements once per instruction.
  Machine m;
  Assembler a(Machine::kCodeBase);
  const auto end = a.new_label();
  a.li(t1, 20);
  a.li(t3, 5);
  a.mtc0(zero, cop0::Wired);
  a.nop();
  a.nop();
  a.mfc0(s0, cop0::Random);  // 3rd after the write: one decrement
  a.mtc0(t1, cop0::Wired);
  for (int i = 0; i < 11; ++i) a.nop();
  a.mfc0(s1, cop0::Random);  // 12th: 31 - 10
  a.nop();
  a.nop();
  a.mtc0(t3, cop0::Wired);
  a.mfc0(s2, cop0::Random);  // 1st: old sequence, wrapped at Wired 20: 21, 20, 31, 30, 29
  a.mtc0(t1, cop0::Wired);
  a.nop();
  a.mfc0(s3, cop0::Random);  // 2nd: reset
  a.bind(end);
  a.b(end);
  a.nop();
  m.load(a.finish());
  REQUIRE(m.run_to(a.address_of(end)));
  CHECK(m.gpr(s0) == 30);
  CHECK(m.gpr(s1) == 21);
  CHECK(m.gpr(s2) == 29);
  CHECK(m.gpr(s3) == 31);
}

TEST_CASE("cpu: an FPE raised by CTC1 takes Cause.CE from the next instruction") {
  // n64-systemtest FireExceptionViaCTC1 (CE 0 after NOP) and ...FollowedByMFC1 / MFC2 (CE 1 / 2).
  Machine m;
  Assembler a(Machine::kCodeBase);
  const auto end = a.new_label();
  auto mfc2 = [&](Reg rt, u32 rd) { a.word((0x12u << 26) | (u32{rt} << 16) | (rd << 11)); };
  // Handler: record Cause, clear FCSR, return past the CTC1 and the instruction after it.
  Assembler h(0xFFFF'FFFF'8000'0180ull);
  h.mfc0(k0, cop0::Cause);
  h.ctc1(zero, 31);
  h.dmfc0(k1, cop0::EPC);
  h.daddiu(k1, k1, 8);
  h.dmtc0(k1, cop0::EPC);
  h.nop();
  h.nop();
  h.eret();
  const auto handler = h.finish();
  for (usize i = 0; i < handler.size(); ++i) m.poke32(0x180 + static_cast<u32>(i) * 4, handler[i]);
  a.li(t0, 0x8000'0180);
  a.cache(0x00, 0, t0);  // drop any stale I-cache line over the handler
  a.li(t1, (1u << 9) | (1u << 14));  // FCSR Enable.V and Cause.V: fires on write
  a.ctc1(t1, 31);
  a.nop();
  a.move(s0, k0);
  a.ctc1(t1, 31);
  a.mfc1(zero, 0);
  a.move(s1, k0);
  a.ctc1(t1, 31);
  mfc2(zero, 0);
  a.move(s2, k0);
  a.bind(end);
  a.b(end);
  a.nop();
  m.load(a.finish());
  REQUIRE(m.run_to(a.address_of(end)));
  constexpr u64 kFpe = 15u << 2;
  CHECK((m.gpr(s0) & 0x3000'00FCull) == kFpe);
  CHECK((m.gpr(s1) & 0x3000'00FCull) == (kFpe | (1u << 28)));
  CHECK((m.gpr(s2) & 0x3000'00FCull) == (kFpe | (2u << 28)));
}
