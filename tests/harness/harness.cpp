// reality_harness: boots ROMs headless and checks them against their oracles.
//
//   reality_harness systemtest ROM [--seconds S]
//       Runs n64-systemtest until it prints its summary. Passes when it reports
//       "Base: Failed 0 of N" (the ROM ran to its end and every test passed).
//   reality_harness screen ROM PNG [--seconds S]
//       Runs the ROM and compares the raw framebuffer (VI origin/width/type) with a reference
//       image of the same size. Passes on an exact match. --dump OUT.ppm writes the capture.
//   reality_harness kromcount ROM FONT COUNTS TOLERANCE [--seconds S]
//       krom timing ROMs (CPUTIMINGNTSC, CP1TIMINGNTSC) print a loop count per instruction as
//       eight hex digits. Reads them back from the framebuffer with the ROM's own 8x8 font and
//       compares each with the hardware count listed in COUNTS. Passes when every count is
//       within TOLERANCE percent (the ledger states the tolerance and why).
//   reality_harness manifest FILE [--rom-dir DIR]
//       Runs every entry of a manifest and checks the outcome against its expectation
//       (pass / fail). An unexpected pass fails just like an unexpected failure, so the
//       manifest (mirrored in docs/timing/ledger.md) must stay exact.
//
// Manifest lines: `<id> <expect pass|fail> systemtest <rom> [seconds]` or
//                 `<id> <expect pass|fail> screen <rom> <png> [seconds]` or
//                 `<id> <expect pass|fail> kromcount <rom> <font> <counts> <tolerance%> [seconds]`.
//                 '#' starts a comment.
//
// COUNTS file lines: `x <column>` once, then `<label> <row> <hex count>`.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "core/system.hpp"
#include "png.hpp"

using namespace reality;

namespace {

struct Outcome {
  bool pass = false;
  std::string detail;
};

std::unique_ptr<System> boot(const std::string& rom, std::string& err) {
  auto cart = Cartridge::load(rom, &err);
  if (!cart) return nullptr;
  SystemConfig cfg;
  auto sys = std::make_unique<System>(cfg, std::move(cart));
  sys->power_on();
  return sys;
}

Outcome run_systemtest(const std::string& rom, double seconds) {
  std::string err;
  auto sys = boot(rom, err);
  if (!sys) return {false, err};
  const Tick limit = static_cast<Tick>(seconds * static_cast<double>(kRcpClockHz));
  const Tick step = kRcpClockHz / 50;
  const std::string& out = sys->cart().isviewer_output();
  while (sys->now() < limit && !sys->cpu().frozen()) {
    sys->run_until(std::min(limit, sys->now() + step));
    if (out.find("Slowest tests") != std::string::npos) break;
  }
  std::smatch m;
  static const std::regex kSummary(R"(Base: Failed (\d+) of (\d+) tests)");
  if (!std::regex_search(out, m, kSummary))
    return {false, "no summary (did not run to the end); cpu " + std::string(sys->cpu().frozen() ? "frozen" : "running")};
  const std::string failed = m[1], total = m[2];
  // Collect the names of failing tests for the report.
  std::string names;
  static const std::regex kFail(R"(Test '([^']+)'[^\n]* failed)");
  for (auto it = std::sregex_iterator(out.begin(), out.end(), kFail); it != std::sregex_iterator(); ++it)
    names += "\n    " + (*it)[1].str();
  return {failed == "0", "Failed " + failed + " of " + total + names};
}

Outcome run_screen(const std::string& rom, const std::string& png, double seconds,
                   const std::string& dump = {}) {
  harness::Image ref;
  try {
    ref = harness::read_png(png);
  } catch (const std::exception& e) {
    return {false, e.what()};
  }
  std::string err;
  auto sys = boot(rom, err);
  if (!sys) return {false, err};
  sys->run_until(static_cast<Tick>(seconds * static_cast<double>(kRcpClockHz)));
  std::vector<u32> rgb;
  const u32 w = ref.width;
  if (!sys->capture_presented(ref.width, ref.height, rgb)) return {false, "VI is off"};
  if (!dump.empty()) {
    // Binary PPM of what was compared, for inspecting mismatches.
    if (std::FILE* f = std::fopen(dump.c_str(), "wb")) {
      std::fprintf(f, "P6\n%u %u\n255\n", ref.width, ref.height);
      for (u32 px : rgb) {
        const u8 c[3] = {static_cast<u8>(px >> 16), static_cast<u8>(px >> 8), static_cast<u8>(px)};
        std::fwrite(c, 1, 3, f);
      }
      std::fclose(f);
    }
  }
  usize diff = 0, first = rgb.size();
  for (usize i = 0; i < rgb.size(); ++i)
    if (rgb[i] != ref.rgb[i]) {
      if (first == rgb.size()) first = i;
      ++diff;
    }
  if (diff == 0) return {true, "identical"};
  char buf[160];
  std::snprintf(buf, sizeof buf, "%zu pixels differ; first at (%zu,%zu): got %06x want %06x", diff,
                first % w, first / w, rgb[first], ref.rgb[first]);
  return {false, buf};
}

Outcome run_kromcount(const std::string& rom, const std::string& font_path,
                      const std::string& counts_path, double tolerance_pct, double seconds) {
  constexpr u32 kW = 640, kH = 480, kGlyphBytes = 8 * 8 * 4;
  std::ifstream font_in(font_path, std::ios::binary);
  std::vector<u8> font((std::istreambuf_iterator<char>(font_in)), std::istreambuf_iterator<char>());
  if (font.size() < 128 * kGlyphBytes) return {false, "cannot read font " + font_path};
  std::ifstream counts_in(counts_path);
  if (!counts_in) return {false, "cannot open " + counts_path};
  struct Row {
    std::string label;
    u32 y;
    u32 expected;
  };
  std::vector<Row> rows;
  u32 x0 = 0;
  for (std::string line; std::getline(counts_in, line);) {
    if (auto h = line.find('#'); h != std::string::npos) line.resize(h);
    std::istringstream ss(line);
    std::string a, b, c;
    if (!(ss >> a >> b)) continue;
    if (a == "x") {
      x0 = static_cast<u32>(std::stoul(b));
    } else if (ss >> c) {
      rows.push_back({a, static_cast<u32>(std::stoul(b)), static_cast<u32>(std::stoul(c, nullptr, 16))});
    }
  }
  if (rows.empty() || x0 == 0) return {false, "no counts in " + counts_path};

  std::string err;
  auto sys = boot(rom, err);
  if (!sys) return {false, err};
  sys->run_until(static_cast<Tick>(seconds * static_cast<double>(kRcpClockHz)));
  std::vector<u32> rgb;
  if (!sys->capture_presented(kW, kH, rgb)) return {false, "VI is off"};

  // A glyph cell matches a font character when the set of lit pixels is identical.
  auto glyph_at = [&](u32 x, u32 y) -> int {
    for (const char c : std::string_view("0123456789ABCDEF")) {
      const u8* g = font.data() + static_cast<usize>(static_cast<u8>(c)) * kGlyphBytes;
      bool same = true;
      for (u32 i = 0; i < 64 && same; ++i) {
        const bool lit_font = (load_be32(g + i * 4) >> 8) != 0;
        const bool lit_fb = rgb[usize{y + i / 8} * kW + x + i % 8] != 0;
        same = lit_font == lit_fb;
      }
      if (same) return c <= '9' ? c - '0' : c - 'A' + 10;
    }
    return -1;
  };
  double worst = 0;
  std::string detail, worst_label;
  for (const Row& r : rows) {
    u32 got = 0;
    for (u32 k = 0; k < 8; ++k) {
      const int d = r.y + 8 <= kH && x0 + 8 * k + 8 <= kW ? glyph_at(x0 + 8 * k, r.y) : -1;
      if (d < 0) return {false, r.label + ": unreadable count on screen"};
      got = (got << 4) | static_cast<u32>(d);
    }
    const double dev = 100.0 * (static_cast<double>(got) - r.expected) / r.expected;
    if (std::abs(dev) >= std::abs(worst)) {
      worst = dev;
      worst_label = r.label;
    }
    if (std::abs(dev) > tolerance_pct) {
      char buf[96];
      std::snprintf(buf, sizeof buf, "\n    %s: %u, hardware %u (%+.2f%%)", r.label.c_str(), got, r.expected, dev);
      detail += buf;
    }
  }
  char buf[128];
  std::snprintf(buf, sizeof buf, "%zu counts, worst %s %+.2f%% (tolerance %.2f%%)", rows.size(),
                worst_label.c_str(), worst, tolerance_pct);
  return {detail.empty(), buf + detail};
}

int run_manifest(const std::string& path, const std::string& rom_dir) {
  std::ifstream in(path);
  if (!in) {
    std::fprintf(stderr, "cannot open manifest %s\n", path.c_str());
    return 2;
  }
  std::string line;
  int bad = 0, total = 0;
  while (std::getline(in, line)) {
    if (auto h = line.find('#'); h != std::string::npos) line.resize(h);
    std::istringstream ss(line);
    std::string id, expect, kind;
    if (!(ss >> id >> expect >> kind)) continue;
    ++total;
    Outcome o;
    if (kind == "systemtest") {
      std::string rom;
      double secs = 60;
      ss >> rom >> secs;
      o = run_systemtest(rom_dir + "/" + rom, secs);
    } else if (kind == "screen") {
      std::string rom, png;
      double secs = 3;
      ss >> rom >> png >> secs;
      o = run_screen(rom_dir + "/" + rom, rom_dir + "/" + png, secs);
    } else if (kind == "kromcount") {
      std::string rom, font, counts;
      double tol = 0, secs = 6;
      ss >> rom >> font >> counts >> tol >> secs;
      o = run_kromcount(rom_dir + "/" + rom, rom_dir + "/" + font, rom_dir + "/" + counts, tol, secs);
    } else {
      o = {false, "unknown kind " + kind};
    }
    const bool want = expect == "pass";
    const bool ok = o.pass == want;
    if (!ok) ++bad;
    std::printf("%-4s %-48s expected %-4s got %-4s  %s\n", ok ? "ok" : "BAD", id.c_str(), expect.c_str(),
                o.pass ? "pass" : "fail", o.detail.c_str());
  }
  std::printf("%d entries, %d not as expected\n", total, bad);
  return bad == 0 ? 0 : 1;
}

double opt_seconds(int argc, char** argv, double def) {
  for (int i = 1; i + 1 < argc; ++i)
    if (std::string(argv[i]) == "--seconds") return std::atof(argv[i + 1]);
  return def;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 3 && std::string(argv[1]) == "systemtest") {
    Outcome o = run_systemtest(argv[2], opt_seconds(argc, argv, 60));
    std::printf("%s: %s\n", o.pass ? "PASS" : "FAIL", o.detail.c_str());
    return o.pass ? 0 : 1;
  }
  if (argc >= 4 && std::string(argv[1]) == "screen") {
    std::string dump;
    for (int i = 4; i + 1 < argc; ++i)
      if (std::string(argv[i]) == "--dump") dump = argv[i + 1];
    Outcome o = run_screen(argv[2], argv[3], opt_seconds(argc, argv, 3), dump);
    std::printf("%s: %s\n", o.pass ? "PASS" : "FAIL", o.detail.c_str());
    return o.pass ? 0 : 1;
  }
  if (argc >= 6 && std::string(argv[1]) == "kromcount") {
    Outcome o = run_kromcount(argv[2], argv[3], argv[4], std::atof(argv[5]), opt_seconds(argc, argv, 6));
    std::printf("%s: %s\n", o.pass ? "PASS" : "FAIL", o.detail.c_str());
    return o.pass ? 0 : 1;
  }
  if (argc >= 3 && std::string(argv[1]) == "manifest") {
    std::string rom_dir = ".";
    for (int i = 3; i + 1 < argc; ++i)
      if (std::string(argv[i]) == "--rom-dir") rom_dir = argv[i + 1];
    return run_manifest(argv[2], rom_dir);
  }
  std::fprintf(stderr, "usage: reality_harness systemtest ROM | screen ROM PNG | kromcount ROM FONT COUNTS TOL | manifest FILE [--rom-dir DIR]\n");
  return 2;
}
