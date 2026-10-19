// ultra: headless command-line runner. The SDL3 frontend (Phase 4) builds on the same core.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
#include <string>
#include <string_view>

#include "core/system.hpp"
#include "cpu/disasm.hpp"

using namespace ultra;

namespace {

void usage() {
  std::puts(
      "usage: ultra [options] ROM\n"
      "  --seconds S         run S emulated seconds (default 10)\n"
      "  --fields N          run until N VI fields have elapsed\n"
      "  --until TEXT        stop as soon as the ISViewer output contains TEXT\n"
      "  --rdram 4|8         RDRAM size in MiB (default 8)\n"
      "  --tv ntsc|pal|mpal  console region (default: from ROM header)\n"
      "  --trace FILE        write the bus-transaction trace\n"
      "  --screenshot FILE   write the raw framebuffer as a binary PPM at exit\n"
      "  --height H          framebuffer height for --screenshot (default 240)\n"
      "  --threads N         worker threads (accepted; RDP workers arrive in Phase 2)\n"
      "  --timing-report     print the timing model parameters and their confidence\n"
      "  --quiet             do not echo ISViewer output\n"
      "  --verbose           print CPU and interface state at exit\n");
}

bool write_ppm(const std::string& path, const std::vector<u32>& rgb, u32 w, u32 h) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  std::fprintf(f, "P6\n%u %u\n255\n", w, h);
  for (u32 v : rgb) {
    const u8 px[3] = {static_cast<u8>(v >> 16), static_cast<u8>(v >> 8), static_cast<u8>(v)};
    std::fwrite(px, 1, 3, f);
  }
  std::fclose(f);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  SystemConfig cfg;
  std::string rom_path, screenshot, until;
  double seconds = 10.0;
  u64 fields = 0;
  u32 height = 240;
  bool quiet = false, timing_report = false, verbose = false, history = false;
  bool break_exc = false;

  for (int i = 1; i < argc; ++i) {
    std::string_view a = argv[i];
    auto next = [&]() -> const char* {
      if (i + 1 >= argc) {
        usage();
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--seconds") seconds = std::atof(next());
    else if (a == "--fields") fields = std::strtoull(next(), nullptr, 10);
    else if (a == "--until") until = next();
    else if (a == "--rdram") cfg.rdram_bytes = static_cast<u32>(std::atoi(next())) * Rdram::kMiB;
    else if (a == "--tv") {
      std::string_view t = next();
      cfg.tv = t == "pal" ? VideoStandard::Pal : t == "mpal" ? VideoStandard::Mpal : VideoStandard::Ntsc;
    } else if (a == "--trace") cfg.trace_path = next();
    else if (a == "--screenshot") screenshot = next();
    else if (a == "--height") height = static_cast<u32>(std::atoi(next()));
    else if (a == "--threads") (void)next();
    else if (a == "--timing-report") timing_report = true;
    else if (a == "--quiet") quiet = true;
    else if (a == "--verbose") verbose = true;
    else if (a == "--history") history = verbose = true;
    else if (a == "--break-exc") break_exc = true;
    else if (a == "-h" || a == "--help") {
      usage();
      return 0;
    } else if (!a.empty() && a[0] == '-') {
      fmt::print(stderr, "unknown option {}\n", a);
      usage();
      return 2;
    } else rom_path = a;
  }
  if (rom_path.empty()) {
    usage();
    return 2;
  }

  std::string err;
  auto cart = Cartridge::load(rom_path, &err);
  if (!cart) {
    fmt::print(stderr, "{}\n", err);
    return 1;
  }
  System sys(cfg, std::move(cart));
  if (timing_report) {
    for (const TimingParam& p : sys.arbiter().params())
      fmt::print("{:<28} {:>8} {:<12} {:<9} {}\n", p.name, p.value, p.unit, to_string(p.confidence), p.source);
  }
  sys.power_on();
  sys.cpu().enable_history(history);
  sys.cpu().break_on_exception(break_exc);

  const auto t0 = std::chrono::steady_clock::now();
  const Tick limit = static_cast<Tick>(seconds * static_cast<double>(kRcpClockHz));
  usize printed = 0;
  bool found = false;
  const Tick step = kRcpClockHz / 100;  // 10 ms slices
  const u64 field_target = sys.vi().field_count() + fields;
  while (sys.now() < limit) {
    sys.run_until(std::min(limit, sys.now() + step));
    const std::string& out = sys.cart().isviewer_output();
    if (!quiet && out.size() > printed) {
      std::fwrite(out.data() + printed, 1, out.size() - printed, stdout);
      std::fflush(stdout);
      printed = out.size();
    }
    if (!until.empty() && out.find(until) != std::string::npos) {
      found = true;
      break;
    }
    if (fields && sys.vi().field_count() >= field_target) break;
    if (sys.cpu().frozen()) break;
  }
  const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  const double emu = static_cast<double>(sys.now()) / static_cast<double>(kRcpClockHz);
  fmt::print(stderr, "\n[ultra] emulated {:.3f}s in {:.3f}s wall ({:.1f}% speed), {} instructions, CIC {}, cpu {}\n",
             emu, wall, wall > 0 ? 100.0 * emu / wall : 0.0, sys.cpu().instructions_retired(),
             cic_info(sys.cic()).name, sys.cpu().frozen() ? "frozen" : "running");

  if (verbose) {
    Vr4300& c = sys.cpu();
    if (history) {
      char buf[96];
      c.for_each_history([&](const Vr4300::HistoryEntry& e) {
        fmt::print(stderr, "  {:10} {:016x}: {:08x}  {}\n", e.cycle, e.pc, e.instr,
                   disassemble(e.instr, e.pc, buf, sizeof buf));
      });
    }
    fmt::print(stderr, "pc={:016x} status={:08x} cause={:08x} epc={:016x}\n", c.pc(),
               c.cop0_read64(12), c.cop0_read64(13), c.cop0_read64(14));
    for (u32 r = 0; r < 32; r += 4)
      fmt::print(stderr, "  r{:02}={:016x} r{:02}={:016x} r{:02}={:016x} r{:02}={:016x}\n", r, c.gpr(r), r + 1,
                 c.gpr(r + 1), r + 2, c.gpr(r + 2), r + 3, c.gpr(r + 3));
    fmt::print(stderr, "  VI:");
    for (u32 r = 0; r < 0x38; r += 4) fmt::print(stderr, " {:08x}", sys.vi().read_reg(0x0440'0000 + r, sys.now()));
    fmt::print(stderr, "\n");
    fmt::print(stderr, "  MI intr={:02x} mask={:02x}  PIF locked={} terminated={} halted={}\n",
               sys.mi().interrupts(), sys.mi().mask(), sys.pif().rom_locked(), sys.pif().boot_terminated(),
               sys.pif().halted_cpu());
  }
  if (!screenshot.empty()) {
    std::vector<u32> rgb;
    u32 w = 0;
    if (!sys.capture_framebuffer(height, rgb, w) || !write_ppm(screenshot, rgb, w, height)) {
      fmt::print(stderr, "screenshot failed (VI off?)\n");
      return 1;
    }
  }
  if (!until.empty() && !found) return 3;
  return 0;
}
