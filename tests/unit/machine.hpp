#pragma once
// Test fixture: a whole machine booted through PIF + IPL3 from a vendored ROM, so that RDRAM is
// initialised exactly as on hardware, after which tests place their own code in RDRAM and drive
// the CPU instruction by instruction.

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>
#include <vector>

#include "common/mips_asm.hpp"
#include "core/system.hpp"
#include "cpu/cop0_regs.hpp"

namespace reality::test {

inline std::string rom_path(const std::string& rel) { return std::string(REALITY_TEST_ROMS) + "/" + rel; }

inline std::unique_ptr<System> boot(const std::string& rel, double seconds) {
  std::string err;
  auto cart = Cartridge::load(rom_path(rel), &err);
  REQUIRE(cart);
  auto sys = std::make_unique<System>(SystemConfig{}, std::move(cart));
  sys->power_on();
  sys->run_until(static_cast<Tick>(seconds * static_cast<double>(kRcpClockHz)));
  return sys;
}

// A booted console (krom's ADD test, which leaves the CPU spinning in its own code) taken over
// by the test: kernel mode, interrupts off, CU0/CU1, FR=1.
struct Machine {
  static constexpr u64 kCodeBase = 0xFFFF'FFFF'8020'0000ull;  // cached KSEG0, paddr 0x0020_0000
  std::unique_ptr<System> sys = boot("krom/CPUTest/CPU/ADD/CPUADD.N64", 0.25);

  Machine() { sys->cpu().cop0_write64(cop0::Status, 0x3400'0000); }

  Vr4300& cpu() { return sys->cpu(); }

  u8* ram(u32 paddr) {
    u32 avail = 0;
    u8* p = sys->rdram().resolve(paddr, &avail);
    REQUIRE(p);
    return p;
  }
  void poke32(u32 paddr, u32 v) { store_be32(ram(paddr), v); }
  u32 peek32(u32 paddr) { return load_be32(ram(paddr)); }

  // Copies code to kCodeBase and points the CPU at it.
  void load(const std::vector<u32>& code, u64 base = kCodeBase) {
    const u32 paddr = static_cast<u32>(base) & 0x1FFF'FFFF;
    for (usize i = 0; i < code.size(); ++i) poke32(paddr + static_cast<u32>(i) * 4, code[i]);
    cpu().set_pc(base);
  }
  // Steps until the PC reaches `end` (or `max` instructions); returns false on timeout.
  bool run_to(u64 end, u32 max = 100000) {
    for (u32 i = 0; i < max; ++i) {
      if (cpu().pc() == end) return true;
      cpu().step();
      if (cpu().frozen()) return false;
    }
    return cpu().pc() == end;
  }
  u64 gpr(mips::Reg r) { return cpu().gpr(r); }
};

}  // namespace reality::test
