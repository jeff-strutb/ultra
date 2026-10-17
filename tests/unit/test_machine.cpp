// Whole-machine properties: determinism, save-state round trip, PI DMA, VI field rate, CIC.

#include <catch2/catch_test_macros.hpp>

#include "io/cic.hpp"
#include "machine.hpp"

using namespace reality;
using reality::test::boot;

namespace {
constexpr const char* kRom = "krom/CPUTest/CPU/ADD/CPUADD.N64";
Tick seconds(double s) { return static_cast<Tick>(s * static_cast<double>(kRcpClockHz)); }
}  // namespace

TEST_CASE("machine: two runs of the same ROM reach identical states") {
  auto a = boot(kRom, 0.4);
  auto b = boot(kRom, 0.4);
  REQUIRE(a->now() == b->now());
  CHECK(a->cpu().instructions_retired() == b->cpu().instructions_retired());
  CHECK(a->save_state() == b->save_state());
}

TEST_CASE("machine: a loaded save state continues exactly like the original") {
  auto a = boot(kRom, 0.3);
  const std::vector<u8> mid = a->save_state();
  a->run_until(seconds(0.6));
  const std::vector<u8> end_a = a->save_state();

  std::string err;
  auto cart = Cartridge::load(reality::test::rom_path(kRom), &err);
  REQUIRE(cart);
  System b(SystemConfig{}, std::move(cart));
  b.power_on();
  b.load_state(mid);
  REQUIRE(b.now() == seconds(0.3));
  b.run_until(seconds(0.6));
  CHECK(b.save_state() == end_a);
}

TEST_CASE("machine: a save state from a different layout is rejected") {
  auto a = boot(kRom, 0.05);
  std::vector<u8> state = a->save_state();
  state[0] ^= 0xFF;  // magic
  CHECK_THROWS(a->load_state(state));
  std::vector<u8> truncated(a->save_state());
  truncated.resize(truncated.size() / 2);
  CHECK_THROWS(a->load_state(truncated));
}

TEST_CASE("machine: PI DMA copies cartridge ROM into RDRAM and raises the PI interrupt") {
  // IPL3's own 1 MiB ROM copy runs from about 0.23 s; wait until the PI is idle.
  auto sys = boot(kRom, 1.0);
  Pi& pi = sys->pi();
  REQUIRE_FALSE(pi.dma_busy());
  const Tick t = sys->now();
  sys->mi().lower(MiIntr::PI);
  pi.write_reg(0x0460'0000, 0x0030'0000, t);  // PI_DRAM_ADDR
  pi.write_reg(0x0460'0004, 0x1000'1000, t);  // PI_CART_ADDR
  pi.write_reg(0x0460'000C, 0x100 - 1, t);    // PI_WR_LEN: 256 bytes cart -> RDRAM
  CHECK(pi.dma_busy());
  sys->run_until(t + seconds(0.001));
  CHECK_FALSE(pi.dma_busy());
  CHECK(sys->mi().pending(MiIntr::PI));
  u32 avail = 0;
  const u8* dram = sys->rdram().resolve(0x0030'0000, &avail);
  REQUIRE(dram);
  const auto rom = sys->cart().rom();
  CHECK(std::equal(dram, dram + 0x100, rom.begin() + 0x1000));
  // n64brew "Parallel Interface": PI_WR_LEN reads back 0x7F after a DMA.
  CHECK(pi.read_reg(0x0460'000C, sys->now()) == 0x7F);
  CHECK(pi.read_reg(0x0460'0000, sys->now()) == 0x0030'0100);
}

TEST_CASE("machine: VI field period follows H_TOTAL and V_TOTAL in VI clocks") {
  // krom programs NTSC 640x480 with H_TOTAL = 0xC15 (3094 VCLKs per line) and V_TOTAL = 0x20D:
  // odd, so progressive, 263 lines per field (n64brew "Video Interface", VI_V_CURRENT). At
  // VCLK/MClock = 1071/1375 a field lasts 263 * 3094 * 1375 / 1071 = 1044676.3 ticks.
  auto sys = boot(kRom, 1.0);  // krom enables the VI at about 0.53 s
  REQUIRE(sys->vi().ctrl() != 0);
  sys->run_fields(1, seconds(0.1));
  const Tick start = sys->now();
  const u64 f0 = sys->vi().field_count();
  sys->run_fields(600, seconds(11.0));
  REQUIRE(sys->vi().field_count() - f0 == 600);
  const double per_field = static_cast<double>(sys->now() - start) / 600.0;
  // run_fields stops within one 64 Ki-tick slice of the field boundary.
  CHECK(per_field > 1044676.3 - 65536.0 / 600 - 1);
  CHECK(per_field < 1044676.3 + 65536.0 / 600 + 1);
}

TEST_CASE("cic: IPL2 checksum and detection of the 6102 IPL3s") {
  std::string err;
  // krom ROMs carry Nintendo's 6102 IPL3; n64-systemtest carries libdragon's open IPL3, which
  // is built to produce the same checksum.
  for (const char* rel : {kRom, "n64-systemtest/n64-systemtest-cpu.z64"}) {
    auto cart = Cartridge::load(reality::test::rom_path(rel), &err);
    REQUIRE(cart);
    const auto rom = cart->rom();
    INFO(rel);
    CHECK(ipl2_checksum(rom.subspan(0x40, 0xFC0), cic_info(CicType::Cic6102).seed) ==
          cic_info(CicType::Cic6102).ipl2_checksum);
    CHECK(detect_cic(rom, false) == CicType::Cic6102);
    CHECK(detect_cic(rom, true) == CicType::Cic7101);
  }
  CHECK(cic_info(CicType::Cic6102).ipl2_checksum == 0xA536'C0F1'D859ull);
}
