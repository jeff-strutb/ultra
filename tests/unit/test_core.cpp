// Save-state visitor, trace sink and MI register semantics in isolation.

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

#include "core/cpu_pins.hpp"
#include "core/savestate.hpp"
#include "core/trace.hpp"
#include "io/mi.hpp"
#include "memory/base_rdram_arbiter.hpp"
#include "memory/rdram.hpp"
#include "memory/ri.hpp"

using namespace ultra;

TEST_CASE("savestate: values round-trip and layout mismatches throw") {
  u32 a = 0x1234'5678;
  u64 b = 0xDEAD'BEEF'0BAD'F00Dull;
  std::array<u16, 3> c{1, 2, 3};
  SaveState out = SaveState::saver();
  out.header();
  out.section("mod", 2);
  out.value(a);
  out.value(b);
  out.array(c);
  const std::vector<u8> bytes = out.data();

  u32 a2 = 0;
  u64 b2 = 0;
  std::array<u16, 3> c2{};
  SaveState in = SaveState::loader(bytes);
  in.header();
  in.section("mod", 2);
  in.value(a2);
  in.value(b2);
  in.array(c2);
  CHECK(a2 == a);
  CHECK(b2 == b);
  CHECK(c2 == c);

  SaveState wrong_version = SaveState::loader(bytes);
  wrong_version.header();
  CHECK_THROWS(wrong_version.section("mod", 3));
  SaveState wrong_name = SaveState::loader(bytes);
  wrong_name.header();
  CHECK_THROWS(wrong_name.section("other", 2));
  SaveState short_buf = SaveState::loader(std::span<const u8>(bytes.data(), 10));
  short_buf.header();
  CHECK_THROWS(short_buf.section("mod", 2));
}

TEST_CASE("trace: records reach the file in order with the documented header") {
  const auto path = std::filesystem::temp_directory_path() / "ultra-trace-test.bin";
  constexpr u32 kRecords = 100'000;  // several times the ring, so the writer must keep up
  {
    TraceSink sink;
    REQUIRE(sink.open(path.string(), 1u << 12));
    for (u32 i = 0; i < kRecords; ++i) {
      TraceRecord r{};
      r.tick = i * 3;
      r.address = i;
      r.data = u64{i} << 32;
      r.size = 4;
      sink.record(r);
    }
    sink.close();
  }
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in);
  TraceFileHeader h{};
  in.read(reinterpret_cast<char*>(&h), sizeof h);
  CHECK(std::string(h.magic, 7) == "ULTRTRC");
  CHECK(h.version == kTraceVersion);
  CHECK(h.record_size == sizeof(TraceRecord));
  u32 n = 0;
  TraceRecord r{};
  bool ordered = true;
  while (in.read(reinterpret_cast<char*>(&r), sizeof r)) {
    ordered = ordered && r.address == n && r.tick == u64{n} * 3 && r.data == u64{n} << 32;
    ++n;
  }
  CHECK(n == kRecords);
  CHECK(ordered);
  in.close();
  std::filesystem::remove(path);
}

TEST_CASE("mi: mask set/clear pairs, interrupt line and version") {
  Rdram rdram(4u << 20);
  BaseRdramArbiter arb;
  TraceSink trace;
  Ri ri(rdram, arb, trace);
  CpuPins pins;
  Mi mi(ri, pins);

  CHECK(mi.read_reg(0x0430'0004) == 0x0202'0102);  // MI_VERSION
  mi.raise(MiIntr::VI);
  CHECK(mi.interrupts() == static_cast<u32>(MiIntr::VI));
  CHECK((pins.int_lines & CpuPins::kRcp) == 0);  // masked
  mi.write_reg(0x0430'000C, 1u << 7);             // set VI mask (bit pair 3: clear 6, set 7)
  CHECK(mi.mask() == static_cast<u32>(MiIntr::VI));
  CHECK((pins.int_lines & CpuPins::kRcp) != 0);
  mi.write_reg(0x0430'000C, (1u << 6) | (1u << 7));  // set and clear together: unchanged
  CHECK(mi.mask() == static_cast<u32>(MiIntr::VI));
  mi.lower(MiIntr::VI);
  CHECK((pins.int_lines & CpuPins::kRcp) == 0);
  // MI_MODE: repeat count and the set/clear bits (n64brew "MIPS Interface", MI_MODE).
  mi.write_reg(0x0430'0000, (1u << 8) | 0x2A);
  CHECK(mi.read_reg(0x0430'0000) == ((1u << 7) | 0x2A));
  mi.write_reg(0x0430'0000, (1u << 7) | 0x2A);
  CHECK(mi.read_reg(0x0430'0000) == 0x2A);
  // Registers mirror every 16 bytes.
  CHECK(mi.read_reg(0x0430'0014) == 0x0202'0102);
}
