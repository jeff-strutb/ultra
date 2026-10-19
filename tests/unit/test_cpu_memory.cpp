// VR4300 caches, write buffer and COP0 register write masks, on a booted machine.
// Facts: docs/hardware/cpu.md (caches: VR4300 manual chapter 11; write buffer: 4.9;
// registers: 5.4 and 6.3, n64-systemtest `cop0` and `tlb`).

#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "machine.hpp"

using namespace ultra;
using namespace ultra::mips;
using ultra::test::Machine;

namespace {
// CACHE op field: bits 1:0 the cache (0 = I, 1 = D), bits 4:2 the operation (manual 16, CACHE).
constexpr u32 cache_op(u32 func, u32 which) { return func * 4 + which; }
constexpr u32 kI = 0, kD = 1;
constexpr u32 kIndexLoadTag = 1, kHitInvalidate = 4, kHitWriteBack = 6;

constexpr u32 kData = 0x0030'0000;  // a free RDRAM area above the test code
constexpr u16 kDataHiCached = 0x8030, kDataHiUncached = 0xA030;

// Each program gets its own 4 KiB of code space: a program loaded where an earlier one ran
// would execute the earlier one's lines still in the I-cache.
u64 program(u32 n) { return Machine::kCodeBase + u64{n} * 0x1000; }

// Runs the code assembled at `base` to its end.
void run(Machine& m, Assembler& a, u64 base) {
  const u64 end = a.pc();
  a.nop();
  m.load(a.finish(), base);
  REQUIRE(m.run_to(end));
}
}  // namespace

TEST_CASE("cpu: the D-cache is write-back; Hit_Write_Back and a dirty eviction write it out") {
  Machine m;
  m.poke32(kData, 0x1111'1111);
  m.poke32(kData + 0x20, 0x3333'3333);
  const u64 a_base = program(1);
  Assembler a(a_base);
  a.lui(t1, kDataHiCached);
  a.lui(t2, kDataHiUncached);
  a.li(t0, 0x2222'2222);
  a.sw(t0, 0, t1);   // cached store: the line is now dirty
  a.lw(s0, 0, t2);   // uncached: RDRAM still holds the old word
  a.lw(s1, 0, t1);   // cached: the new word
  a.cache(cache_op(kHitWriteBack, kD), 0, t1);
  a.lw(s2, 0, t2);   // written back
  // 0x2020 is 8 KiB above 0x20: the same D-cache index (8 KiB direct-mapped, 11.2.2).
  a.sw(t0, 0x20, t1);
  a.lw(s3, 0x2020, t1);  // misses, evicting the dirty line at 0x20
  a.lw(s4, 0x20, t2);
  run(m, a, a_base);
  CHECK(lo32(m.gpr(s0)) == 0x1111'1111);
  CHECK(lo32(m.gpr(s1)) == 0x2222'2222);
  CHECK(lo32(m.gpr(s2)) == 0x2222'2222);
  CHECK(lo32(m.gpr(s4)) == 0x2222'2222);
}

TEST_CASE("cpu: a D-cache miss refills the whole line; Hit_Invalidate drops it") {
  Machine m;
  for (u32 i = 0; i < 4; ++i) m.poke32(kData + 0x40 + i * 4, 0xA0 + i);
  const u64 a_base = program(2);
  Assembler a(a_base);
  a.lui(t1, kDataHiCached);
  a.lw(s0, 0x40, t1);  // refills the 16-byte line 0x40..0x4F (11.3.2)
  run(m, a, a_base);
  m.poke32(kData + 0x44, 0xBEEF);  // behind the cache's back

  const u64 b_base = program(3);
  Assembler b(b_base);
  b.lui(t1, kDataHiCached);
  b.lw(s1, 0x44, t1);  // hits the stale line
  b.cache(cache_op(kIndexLoadTag, kD), 0x40, t1);
  b.mfc0(s2, cop0::TagLo);
  b.cache(cache_op(kHitInvalidate, kD), 0x40, t1);
  b.lw(s3, 0x44, t1);  // refills from RDRAM
  run(m, b, b_base);
  CHECK(lo32(m.gpr(s0)) == 0xA0);
  CHECK(lo32(m.gpr(s1)) == 0xA1);
  // TagLo: PTagLo = physical address bits 31:12, PState = valid (5.4.8).
  CHECK(lo32(m.gpr(s2)) ==
        (cop0::TagLo::PTagLo::place(kData >> 12) | cop0::TagLo::PState::place(3)));
  CHECK(lo32(m.gpr(s3)) == 0xBEEF);
}

TEST_CASE("cpu: the I-cache keeps executing old code until Hit_Invalidate") {
  Machine m;
  constexpr u64 kFunc = 0xFFFF'FFFF'8030'0100ull;
  auto put_func = [&](u16 value) {
    Assembler f(kFunc);
    f.ori(v0, zero, value);
    f.jr(ra);
    f.nop();
    const auto code = f.finish();
    for (usize i = 0; i < code.size(); ++i)
      m.poke32(kData + 0x100 + static_cast<u32>(i) * 4, code[i]);
  };
  put_func(1);
  const u64 a_base = program(4);
  Assembler a(a_base);
  a.li(t9, static_cast<u32>(kFunc));
  a.jalr(ra, t9);
  a.nop();
  a.move(s0, v0);
  run(m, a, a_base);
  put_func(2);  // RDRAM changes; the I-cache line does not

  const u64 b_base = program(5);
  Assembler b(b_base);
  b.li(t9, static_cast<u32>(kFunc));
  b.jalr(ra, t9);
  b.nop();
  b.move(s1, v0);
  b.cache(cache_op(kHitInvalidate, kI), 0, t9);
  b.jalr(ra, t9);
  b.nop();
  b.move(s2, v0);
  run(m, b, b_base);
  CHECK(m.gpr(s0) == 1);
  CHECK(m.gpr(s1) == 1);
  CHECK(m.gpr(s2) == 2);
}

TEST_CASE("cpu: uncached stores queue in a 4-entry write buffer, drained before a read") {
  Machine m;
  const u64 a_base = program(6);
  Assembler a(a_base);
  a.lui(t2, kDataHiUncached);
  a.li(t0, 0x5A5A'0000);
  const u64 first_sw = a.pc();
  for (u32 i = 0; i < 6; ++i) a.sw(t0, static_cast<s16>(i * 4), t2);
  a.lw(s0, 20, t2);
  const u64 end = a.pc();
  a.nop();
  m.load(a.finish(), a_base);
  REQUIRE(m.run_to(first_sw));
  std::vector<CpuCycle> cost;
  for (u32 i = 0; i < 6; ++i) {
    const CpuCycle before = m.cpu().cycles();
    m.cpu().step();
    cost.push_back(m.cpu().cycles() - before);
  }
  REQUIRE(m.run_to(end));
  // Four stores fit in the buffer and retire in a cycle each; the fifth waits for the oldest
  // to be accepted by the bus (4.9; cpu.md "Write buffer full").
  for (u32 i = 0; i < 4; ++i) CHECK(cost[i] == 1);
  CHECK(cost[4] > 1);
  CHECK(lo32(m.gpr(s0)) == 0x5A5A'0000);
  for (u32 i = 0; i < 6; ++i) CHECK(m.peek32(kData + i * 4) == 0x5A5A'0000);
}

TEST_CASE("cpu: COP0 register write masks and read-only registers") {
  // Writes all ones with MTC0/DMTC0 and reads back with DMFC0. Expected values: manual 5.4
  // and 6.3 field layouts; EntryLo per n64-systemtest (ledger "entrylo-write-mask").
  Machine m;
  struct Case {
    u32 reg;
    u64 expected;
  };
  const Case cases[] = {
      {cop0::Index, 0x8000'003Full},           // P and Index (5.4.1)
      {cop0::EntryLo0, 0x3FFF'FFFFull},        // bits 29:0
      {cop0::PageMask, 0x01FF'E000ull},        // Mask, bits 24:13 (5.4.3)
      {cop0::Wired, 0x3Full},                  // 6 bits (5.4.4)
      {cop0::EntryHi, 0xC000'00FF'FFFF'E0FFull},  // R, VPN2, ASID (5.4.3)
      {cop0::WatchLo, 0xFFFF'FFFBull},         // PAddr0, R, W; bit 2 is zero (6.3.8)
      {cop0::WatchHi, 0xFull},                 // PAddr1 (6.3.8)
      {cop0::TagLo, 0x0FFF'FFC0ull},           // PTagLo, PState (5.4.8)
      {cop0::TagHi, 0},                        // RFU (5.4.8)
      {cop0::ParityError, 0xFFull},            // Diagnostic (6.3.10)
  };
  u32 n = 10;
  for (const Case& c : cases) {
    const u64 a_base = program(n++);
    Assembler a(a_base);
    a.li(t0, 0xFFFF'FFFFu);  // sign-extends to all 64 bits
    a.dmtc0(t0, c.reg);
    a.nop();
    a.dmfc0(s0, c.reg);
    run(m, a, a_base);
    CHECK(m.gpr(s0) == c.expected);
    m.cpu().cop0_write64(cop0::WatchLo, 0);
  }

  // Read-only: PRId and BadVAddr ignore writes.
  const u64 bad = m.cpu().cop0_read64(cop0::BadVAddr);
  const u64 a_base = program(8);
  Assembler a(a_base);
  a.li(t0, 0x1234'5678u);
  a.mtc0(t0, cop0::PRId);
  a.mtc0(t0, cop0::BadVAddr);
  a.nop();
  a.mfc0(s0, cop0::PRId);
  a.dmfc0(s1, cop0::BadVAddr);
  // Cause: only IP0 and IP1 are writable (6.3.6).
  a.mfc0(s2, cop0::Cause);
  a.li(t0, 0xFFFF'FFFFu);
  a.mtc0(t0, cop0::Cause);
  a.nop();
  a.mfc0(s3, cop0::Cause);
  a.mtc0(zero, cop0::Cause);
  run(m, a, a_base);
  CHECK(lo32(m.gpr(s0)) == 0x0000'0B22);
  CHECK(m.gpr(s1) == bad);
  CHECK(lo32(m.gpr(s3)) == (lo32(m.gpr(s2)) | cop0::Cause::IPSoft::mask));

  // Status: TS (21) is read-only and bits 19 and 23 are RFU (6.3.5).
  const u32 status = static_cast<u32>(m.cpu().cop0_read64(cop0::Status));
  const u64 b_base = program(9);
  Assembler b(b_base);
  b.li(t0, status | (1u << 19) | (1u << 21) | (1u << 23));
  b.mtc0(t0, cop0::Status);
  b.nop();
  b.mfc0(s0, cop0::Status);
  run(m, b, b_base);
  CHECK(lo32(m.gpr(s0)) == status);
}
