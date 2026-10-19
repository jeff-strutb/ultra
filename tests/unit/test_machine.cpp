// Whole-machine properties: determinism, save-state round trip, PI DMA, VI field rate, CIC.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>

#include "core/savestate.hpp"
#include "io/cic.hpp"
#include "machine.hpp"

using namespace ultra;
using ultra::test::boot;

namespace {
constexpr const char* kRom = ultra::test::kBootRom;
// Carries Nintendo's 6102 IPL3; untracked, so tests that need it skip without it.
constexpr const char* kKromRom = "krom/CPUTest/CPU/ADD/CPUADD.N64";
bool have(const char* rel) { return std::filesystem::exists(ultra::test::rom_path(rel)); }
Tick seconds(double s) { return static_cast<Tick>(s * static_cast<double>(kRcpClockHz)); }
}  // namespace

TEST_CASE("machine: two runs of the same ROM reach identical states") {
  auto a = boot(kRom, 0.4);
  auto b = boot(kRom, 0.4);
  REQUIRE(a->now() == b->now());
  CHECK(a->cpu().instructions_retired() == b->cpu().instructions_retired());
  CHECK(a->save_state() == b->save_state());
}

namespace {
// A machine that records its bus trace to a file, read back once the sink is closed.
struct Traced {
  std::filesystem::path path;
  std::unique_ptr<System> sys;
  explicit Traced(const std::string& name)
      : path(std::filesystem::temp_directory_path() / ("ultra-unit-" + name + ".trace")) {
    std::string err;
    auto cart = Cartridge::load(ultra::test::rom_path(kRom), &err);
    REQUIRE(cart);
    SystemConfig cfg;
    cfg.trace_path = path.string();
    sys = std::make_unique<System>(cfg, std::move(cart));
    sys->power_on();
  }
  ~Traced() { std::filesystem::remove(path); }
  std::vector<TraceRecord> records() {
    sys->trace().close();
    std::vector<u8> bytes(std::filesystem::file_size(path));
    std::FILE* f = std::fopen(path.string().c_str(), "rb");
    REQUIRE(f);
    REQUIRE(std::fread(bytes.data(), 1, bytes.size(), f) == bytes.size());
    std::fclose(f);
    REQUIRE(bytes.size() >= sizeof(TraceFileHeader));
    std::vector<TraceRecord> out((bytes.size() - sizeof(TraceFileHeader)) / sizeof(TraceRecord));
    std::memcpy(out.data(), bytes.data() + sizeof(TraceFileHeader),
                out.size() * sizeof(TraceRecord));
    return out;
  }
};

bool same(const TraceRecord& x, const TraceRecord& y) { return std::memcmp(&x, &y, sizeof x) == 0; }
}  // namespace

// ADR 0003: a state saved at any point continues exactly like the machine it came from. A runs
// on after saving; B loads the state into a fresh machine. Every bus transaction B makes must
// equal A's after the save point, and the final states must be equal. A field missing from a
// serializer is caught when it changes bus traffic or saved state inside a window. The save
// points cover the PIF boot ROM, a PI DMA in flight during IPL3, and the ROM's own code.
TEST_CASE("machine: a loaded save state continues exactly like the original") {
  enum class Point { PifBoot, PiDma, Program };
  const Point point = GENERATE(Point::PifBoot, Point::PiDma, Point::Program);
  constexpr Tick kWindow = kRcpClockHz / 10;  // 0.1 s

  Traced a("roundtrip-a");
  switch (point) {
    case Point::PifBoot: a.sys->run_until(seconds(0.05)); break;
    case Point::PiDma:
      while (!a.sys->pi().dma_busy() && a.sys->now() < seconds(1.0))
        a.sys->run_until(a.sys->now() + 64);
      REQUIRE(a.sys->pi().dma_busy());
      break;
    case Point::Program: a.sys->run_until(seconds(1.0)); break;
  }
  const Tick saved_at = a.sys->now();
  const u64 records_before = a.sys->trace().count();
  const std::vector<u8> mid = a.sys->save_state();
  a.sys->run_until(saved_at + kWindow);
  const std::vector<u8> end_a = a.sys->save_state();

  Traced b("roundtrip-b");
  b.sys->load_state(mid);
  REQUIRE(b.sys->now() == saved_at);
  b.sys->run_until(saved_at + kWindow);
  CHECK(b.sys->save_state() == end_a);

  const std::vector<TraceRecord> ta = a.records(), tb = b.records();
  REQUIRE(ta.size() >= records_before);
  REQUIRE(tb.size() == ta.size() - records_before);
  REQUIRE(!tb.empty());
  const auto after_save = ta.begin() + static_cast<std::ptrdiff_t>(records_before);
  const auto mismatch = std::mismatch(tb.begin(), tb.end(), after_save, same);
  CHECK(mismatch.first == tb.end());
}

TEST_CASE("machine: a failed load leaves the machine exactly as it was") {
  // ADR 0003: loading is all or nothing. Each bad state fails at a different depth: the
  // header, half way through (RDRAM loaded, the CPU not), and the very end.
  // The bad states come from a later point, so a partial load would change the machine.
  auto a = boot(kRom, 0.05);
  auto b = boot(kRom, 0.05);
  const std::vector<u8> good = a->save_state();
  const std::vector<u8> later = boot(kRom, 0.08)->save_state();
  REQUIRE(later.size() == good.size());
  REQUIRE(later != good);
  std::vector<u8> bad_magic = later;
  bad_magic[0] ^= 0xFF;
  std::vector<u8> truncated = later;
  truncated.resize(truncated.size() / 2);
  std::vector<u8> trailing = later;
  trailing.push_back(0);
  for (const auto* bad : {&bad_magic, &truncated, &trailing}) {
    CHECK_THROWS(a->load_state(*bad));
    CHECK(a->save_state() == good);
  }
  // And it still runs exactly like an untouched machine.
  a->run_until(seconds(0.1));
  b->run_until(seconds(0.1));
  CHECK(a->save_state() == b->save_state());
}

TEST_CASE("machine: a save state from another ROM, CIC or TV standard is rejected") {
  auto a = boot(kRom, 0.05);
  const std::vector<u8> state = a->save_state();
  std::string err;
  auto rom_bytes = [&] {
    auto cart = Cartridge::load(ultra::test::rom_path(kRom), &err);
    REQUIRE(cart);
    return std::vector<u8>(cart->rom().begin(), cart->rom().end());
  };
  auto rejects = [&](std::vector<u8> rom, SystemConfig cfg) {
    System other(cfg, Cartridge::from_bytes(std::move(rom)));
    other.power_on();
    const std::vector<u8> before = other.save_state();
    CHECK_THROWS(other.load_state(state));
    CHECK(other.save_state() == before);
  };
  std::vector<u8> changed = rom_bytes();
  changed.back() ^= 1;  // past IPL3: same CIC and region, different ROM
  rejects(changed, SystemConfig{});
  SystemConfig pal;
  pal.tv = VideoStandard::Pal;
  pal.cic = a->cic();  // PAL alone would also change the detected CIC
  rejects(rom_bytes(), pal);
  SystemConfig cic;
  cic.cic = a->cic() == CicType::Cic6105 ? CicType::Cic6102 : CicType::Cic6105;
  rejects(rom_bytes(), cic);
}

TEST_CASE("save state: a bool or enum outside its range is rejected") {
  auto make = [](u8 b, u8 e) {
    SaveState s = SaveState::saver();
    s.section("t", 1);
    bool flag = true;
    CicType cic = CicType::Cic6102;
    s.value(flag);
    s.enumeration(cic, CicType::Cic7106);
    std::vector<u8> bytes = s.data();
    bytes[bytes.size() - 2] = b;
    bytes[bytes.size() - 1] = e;
    return bytes;
  };
  auto load = [](const std::vector<u8>& bytes) {
    SaveState s = SaveState::loader(bytes);
    s.section("t", 1);
    bool flag = false;
    CicType cic = CicType::Cic6101;
    s.value(flag);
    s.enumeration(cic, CicType::Cic7106);
    return std::pair{flag, cic};
  };
  CHECK(load(make(1, static_cast<u8>(CicType::Cic7106))) == std::pair{true, CicType::Cic7106});
  CHECK_THROWS(load(make(2, 0)));
  CHECK_THROWS(load(make(0, static_cast<u8>(CicType::Cic7106) + 1)));
}

TEST_CASE("machine: PI DMA copies cartridge ROM into RDRAM and raises the PI interrupt") {
  ultra::test::Machine m;
  m.park();
  System* sys = m.sys.get();
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
  // NTSC 640x480 timing: H_TOTAL = 0xC15 (3094 VCLKs per line) and V_TOTAL = 0x20D: odd, so
  // progressive, 263 lines per field (n64brew "Video Interface", VI_V_CURRENT). At
  // VCLK/MClock = 1071/1375 a field lasts 263 * 3094 * 1375 / 1071 = 1044676.3 ticks.
  ultra::test::Machine m;
  m.park();
  System* sys = m.sys.get();
  Vi& vi = sys->vi();
  const Tick t = sys->now();
  vi.write_reg(0x0440'0018, 0x20D, t);  // VI_V_SYNC (V_TOTAL)
  vi.write_reg(0x0440'001C, 0xC15, t);  // VI_H_SYNC (H_TOTAL, no leap)
  vi.write_reg(0x0440'0000, 0x3, t);    // VI_CTRL: 32 bpp, so the VI runs
  REQUIRE(vi.ctrl() != 0);
  sys->run_fields(1, seconds(0.1));
  const Tick start = sys->now();
  const u64 f0 = vi.field_count();
  sys->run_fields(600, seconds(11.0));
  REQUIRE(vi.field_count() - f0 == 600);
  const double per_field = static_cast<double>(sys->now() - start) / 600.0;
  // run_fields stops within one 64 Ki-tick slice of the field boundary.
  CHECK(per_field > 1044676.3 - 65536.0 / 600 - 1);
  CHECK(per_field < 1044676.3 + 65536.0 / 600 + 1);
}

TEST_CASE("cic: IPL2 checksum and detection of the 6102 IPL3s") {
  // n64-systemtest carries libdragon's open IPL3, built to give the 6102 checksum; krom ROMs
  // carry Nintendo's 6102 IPL3 and are checked when present.
  std::vector<const char*> roms{kRom};
  if (have(kKromRom)) roms.push_back(kKromRom);
  for (const char* rel : roms) {
    std::string err;
    auto cart = Cartridge::load(ultra::test::rom_path(rel), &err);
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

TEST_CASE("cic: derived IPL2 checksums of the 6102 IPL3 for the other seeds") {
  // Regression values from docs/hardware/cic-algorithms.md 1.6 (derived, not hardware), over
  // Nintendo's 6102 IPL3, which only the untracked krom ROMs carry.
  if (!have(kKromRom)) SKIP("krom ROMs not present (tests/roms/README.md)");
  std::string err;
  auto cart = Cartridge::load(ultra::test::rom_path(kKromRom), &err);
  REQUIRE(cart);
  const auto ipl3 = cart->rom().subspan(0x40, 0xFC0);
  CHECK(ipl2_checksum(ipl3, 0x78) == 0xD154'D024'B1BFull);
  CHECK(ipl2_checksum(ipl3, 0x91) == 0xF1FA'53D0'DD55ull);
  CHECK(ipl2_checksum(ipl3, 0x85) == 0x5D6B'534D'2A7Full);
}

TEST_CASE("cic: the boot ROM's IPL2 hands the PIF the checksum ipl2_checksum computes") {
  // Random IPL3s booted under each seed: the PIF halts the CPU (no retail CIC accepts them),
  // but only after the clean-room IPL2 has computed and handed over its checksum.
  std::mt19937 rng(0x1F2u);
  for (const CicType type : {CicType::Cic6102, CicType::Cic6103, CicType::Cic6105,
                             CicType::Cic6106}) {
    std::vector<u8> rom(0x10'1000, 0);
    const u8 header[] = {0x80, 0x37, 0x12, 0x40};
    std::copy(std::begin(header), std::end(header), rom.begin());
    rom[0x3E] = 'E';
    for (usize i = 0x40; i < 0x1000; ++i) rom[i] = static_cast<u8>(rng());
    const u64 expected = ipl2_checksum(std::span<const u8>(rom).subspan(0x40, 0xFC0),
                                       cic_info(type).seed);
    SystemConfig cfg;
    cfg.cic = type;
    System sys(cfg, Cartridge::from_bytes(rom));
    sys.power_on();
    const Tick step = kRcpClockHz / 20;
    for (int i = 0; i < 40 && sys.pif().checksum_from_cpu() == 0; ++i) {
      sys.run_until(sys.now() + step);
    }
    INFO(cic_info(type).name);
    CHECK(sys.pif().checksum_from_cpu() == expected);
    CHECK(sys.pif().halted_cpu());
  }
}

TEST_CASE("cic: 6105 challenge vectors") {
  // docs/hardware/cic-algorithms.md 2.4 (derived, not hardware).
  const auto run = [](const char* hex) {
    std::array<u8, 30> n{};
    for (usize i = 0; i < n.size(); ++i) {
      const char c = hex[i];
      n[i] = static_cast<u8>(c <= '9' ? c - '0' : c - 'A' + 10);
    }
    cic6105_challenge(n);
    std::string out;
    for (const u8 v : n) out.push_back("0123456789ABCDEF"[v]);
    return out;
  };
  CHECK(run("000000000000000000000000000000") == "BF9F9F9F9F9F9F9F9F9F9F9F9F9F9F");
  CHECK(run("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFF") == "6949A3250F49A3250F49A3250F49A3");
  CHECK(run("0123456789ABCDEF0123456789ABCD") == "B48B98AB7E8338255A29DC41F60B18");
}

TEST_CASE("cic: an IPL3 no retail CIC accepts halts the PIF unless the CIC reports its checksum") {
  // A changed IPL3 byte, as in hwprobe-cold.z64. A console with a retail CIC halts in IPL2;
  // SC64 direct boot programs its CIC emulation with the IPL3's own checksum and boots.
  std::string err;
  auto load = [&] {
    auto cart = Cartridge::load(ultra::test::rom_path(kRom), &err);
    REQUIRE(cart);
    std::vector<u8> rom(cart->rom().begin(), cart->rom().end());
    rom[0xFF0] ^= 0x01;  // inside the IPL3 area (0x40-0xFFF), past its code
    return Cartridge::from_bytes(std::move(rom));
  };
  System retail(SystemConfig{}, load());
  retail.power_on();
  retail.run_until(seconds(0.5));
  CHECK(retail.pif().halted_cpu());

  SystemConfig direct;
  direct.cic_checksum_from_ipl3 = true;
  System sc64(direct, load());
  sc64.power_on();
  sc64.run_until(seconds(0.5));
  CHECK_FALSE(sc64.pif().halted_cpu());
  CHECK(sc64.pif().boot_terminated());
}
