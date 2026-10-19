#pragma once
// Test fixture: a whole machine booted through PIF + IPL3 from a tracked ROM, so that RDRAM is
// initialised exactly as on hardware, after which tests place their own code in RDRAM and drive
// the CPU instruction by instruction.

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>
#include <vector>

#include "common/mips_asm.hpp"
#include "core/system.hpp"
#include "cpu/cop0_regs.hpp"

namespace ultra::test {

inline std::string rom_path(const std::string& rel) { return std::string(ULTRA_TEST_ROMS) + "/" + rel; }

// Booted by machine-level tests. It is tracked (libdragon's open IPL3, tests/roms/README.md), so
// the tests run from a fresh clone; krom ROMs are not.
inline constexpr const char* kBootRom = "n64-systemtest/n64-systemtest-cpu.z64";

inline std::unique_ptr<System> boot(const std::string& rel, double seconds) {
  std::string err;
  auto cart = Cartridge::load(rom_path(rel), &err);
  REQUIRE(cart);
  auto sys = std::make_unique<System>(SystemConfig{}, std::move(cart));
  sys->power_on();
  sys->run_until(static_cast<Tick>(seconds * static_cast<double>(kRcpClockHz)));
  return sys;
}

// Boots `rel` until IPL3 hands over: the CPU at the entry point in ROM header word 0x08
// (n64brew "ROM Header"), with RDRAM initialised and the program loaded (about 0.43 s for
// kBootRom). Whole-machine slices run the PIF boot ROM (0xBFC0_0000..0xBFC0_07FF, n64brew
// "Memory map"); IPL3 is then single-stepped, and each step runs the devices up to the CPU's
// time, so the boot stops exactly on the entry instruction.
inline std::unique_ptr<System> boot_to_entry(const std::string& rel) {
  std::string err;
  auto cart = Cartridge::load(rom_path(rel), &err);
  REQUIRE(cart);
  const auto rom = cart->rom();
  const u32 entry32 = load_be32(rom.data() + 0x08);
  const u64 entry = static_cast<u64>(static_cast<s64>(static_cast<s32>(entry32)));
  auto sys = std::make_unique<System>(SystemConfig{}, std::move(cart));
  sys->power_on();
  constexpr u64 kPifRomBase = 0xFFFF'FFFF'BFC0'0000ull, kPifRomEnd = 0xFFFF'FFFF'BFC0'0800ull;
  const Tick limit = 2 * kRcpClockHz;
  auto in_pif_rom = [&] { return sys->cpu().pc() >= kPifRomBase && sys->cpu().pc() < kPifRomEnd; };
  while (in_pif_rom() && sys->now() < limit) sys->run_until(sys->now() + 64);
  while (sys->cpu().pc() != entry && !sys->cpu().frozen() && sys->now() < limit) sys->cpu().step();
  REQUIRE(sys->cpu().pc() == entry);
  return sys;
}

// A booted console taken over by the test the way boot code takes over a CPU: kernel mode,
// interrupts off, CU0/CU1, FR=1; then, from code in RSP IMEM (outside RDRAM and the caches),
// every I-cache line invalidated, every D-cache line written back and invalidated, and every TLB
// entry made unmatchable. Nothing the booted ROM left in the caches or TLB can reach a test.
struct Machine {
  static constexpr u64 kCodeBase = 0xFFFF'FFFF'8020'0000ull;  // cached KSEG0, paddr 0x0020_0000
  std::unique_ptr<System> sys = boot_to_entry(kBootRom);

  Machine() {
    sys->cpu().cop0_write64(cop0::Status, 0x3400'0000);
    reset_caches_and_tlb();
  }

  // VR4300 manual: I-cache 16 KiB in 32-byte lines, D-cache 8 KiB in 16-byte lines (11.2);
  // CACHE op 0 = I Index_Invalidate, op 1 = D Index_Write_Back_Invalidate (16, CACHE); 32 TLB
  // entries (5.1). EntryHi VPN2s in KSEG0 never match, since KSEG0 is not translated.
  void reset_caches_and_tlb() {
    constexpr u64 kImem = 0xFFFF'FFFF'A400'1000ull;
    constexpr u32 kICacheBytes = 16 * 1024, kICacheLine = 32;
    constexpr u32 kDCacheBytes = 8 * 1024, kDCacheLine = 16;
    constexpr u32 kTlbEntries = 32, kPagePairBytes = 0x2000;
    using namespace mips;
    Assembler a(kImem);
    const auto iloop = a.new_label(), dloop = a.new_label(), tloop = a.new_label();
    const auto done = a.new_label();
    a.lui(t0, 0x8000);
    a.li(t1, 0x8000'0000u + kICacheBytes);
    a.bind(iloop);
    a.cache(0x00, 0, t0);
    a.addiu(t0, t0, kICacheLine);
    a.bne(t0, t1, iloop);
    a.nop();
    a.lui(t0, 0x8000);
    a.li(t1, 0x8000'0000u + kDCacheBytes);
    a.bind(dloop);
    a.cache(0x01, 0, t0);
    a.addiu(t0, t0, kDCacheLine);
    a.bne(t0, t1, dloop);
    a.nop();
    a.mtc0(zero, cop0::PageMask);
    a.mtc0(zero, cop0::EntryLo0);
    a.mtc0(zero, cop0::EntryLo1);
    a.mtc0(zero, cop0::Wired);
    a.move(t0, zero);
    a.ori(t1, zero, kTlbEntries);
    a.lui(t2, 0x8000);
    a.bind(tloop);
    a.mtc0(t0, cop0::Index);
    a.mtc0(t2, cop0::EntryHi);
    a.nop();
    a.tlbwi();
    a.addiu(t0, t0, 1);
    a.bne(t0, t1, tloop);
    a.addiu(t2, t2, kPagePairBytes);
    a.mtc0(zero, cop0::EntryHi);
    a.bind(done);
    a.nop();
    const std::vector<u32> code = a.finish();
    u8* imem = sys->sp().imem();
    for (usize i = 0; i < code.size(); ++i) store_be32(imem + i * 4, code[i]);
    cpu().set_pc(kImem);
    REQUIRE(run_to(a.address_of(done)));
  }

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

  // Leaves the CPU spinning in a branch-to-self at kCodeBase, so whole-machine runs exercise the
  // devices while the CPU touches nothing.
  void park() {
    mips::Assembler a(kCodeBase);
    const auto self = a.new_label();
    a.bind(self);
    a.b(self);
    a.nop();
    load(a.finish());
  }
};

}  // namespace ultra::test
