// Console reset: the PIF's pre-NMI interrupt, the NMI it drives 0.5 s later, the warm reboot it
// publishes, and the VR4300's NMI latch (docs/hardware/pif.md, Console reset).

#include <catch2/catch_test_macros.hpp>

#include "machine.hpp"

using namespace ultra;

namespace {
Tick seconds(double s) { return static_cast<Tick>(s * static_cast<double>(kRcpClockHz)); }

// PIF RAM 0x24..0x27 and the command byte 0x3F, as the CPU sees them through the SI window
// (n64brew "PIF-NUS"; RAM starts at PIF offset 0x7C0).
constexpr u32 kBootInfoWord = 0x7C0 + Pif::kBootInfoOffset;
constexpr u32 kCommandWord = 0x7FC;
constexpr u32 kCmdTerminateBoot = 0x08;
constexpr u32 kCmdDone = 0x80;
constexpr u64 kResetVector = 0xFFFF'FFFF'BFC0'0000ull;  // manual 6.4.6
constexpr u64 kPifRomEnd = 0xFFFF'FFFF'BFC0'0800ull;
}  // namespace

TEST_CASE("pif: RESET raises pre-NMI, then NMI after the grace time, and reboots warm") {
  Scheduler sched;
  CpuPins pins;
  Pif pif(sched, pins);
  pif.power_on(CicType::Cic6102, VideoStandard::Ntsc, {});
  const u32 cold_info = pif.read_word(kBootInfoWord);
  CHECK((cold_info >> 24) == 1);  // NTSC, reset type 0, cartridge

  // The button does nothing until the boot is terminated (command 0x08).
  CHECK_FALSE(pif.press_reset(sched.now()));
  CHECK(pins.int_lines == 0);
  pif.write_word(kCommandWord, kCmdTerminateBoot, sched.now());
  REQUIRE(pif.boot_terminated());
  pif.write_word(0x7C0, 0xDEAD'BEEF, sched.now());  // a game's Joybus frame over the boot info
  pif.write_word(kBootInfoWord, 0, sched.now());

  const Tick press = 1000;
  sched.run_until(press);
  REQUIRE(pif.press_reset(press));
  CHECK(pins.int_lines == CpuPins::kPreNmi);
  CHECK_FALSE(pins.nmi);
  CHECK(pif.reset_pending());
  CHECK_FALSE(pif.press_reset(press + 1));  // already under way

  sched.run_until(press + Pif::kPreNmiToNmiTicks - 1);
  CHECK(pins.int_lines == CpuPins::kPreNmi);
  CHECK_FALSE(pins.nmi);
  CHECK(pif.boot_terminated());

  sched.run_until(press + Pif::kPreNmiToNmiTicks);
  CHECK(pins.nmi);
  CHECK(pins.int_lines == 0);
  CHECK_FALSE(pif.reset_pending());
  // Reset mode again: ROM readable, boot to be terminated anew, boot info with reset type 1
  // (bit 2) and the seeds learned at power on, command byte 0x80.
  CHECK_FALSE(pif.rom_locked());
  CHECK_FALSE(pif.boot_terminated());
  CHECK(pif.read_word(kBootInfoWord) == (cold_info | (u32{Pif::kBootInfoResetNmi} << 24)));
  CHECK((pif.read_word(kCommandWord) & 0xFF) == kCmdDone);
  // The button stays disabled until the warm boot terminates.
  CHECK_FALSE(pif.press_reset(sched.now()));
}

TEST_CASE("pif: the RESET button is ignored after the PIF halted the console") {
  Scheduler sched;
  CpuPins pins;
  Pif pif(sched, pins);
  pif.power_on(CicType::Cic6102, VideoStandard::Pal, {});
  sched.run_until(seconds(5.0));  // boot never terminated: the PIF halts the CPU
  REQUIRE(pif.halted_cpu());
  CHECK_FALSE(pif.press_reset(sched.now()));
  CHECK(pins.int_lines == 0);
}

TEST_CASE("cpu: the NMI latch is taken whatever IE, EXL and ERL are, and is cleared") {
  ultra::test::Machine m;
  mips::Assembler a(ultra::test::Machine::kCodeBase);
  const auto self = a.new_label();
  a.nop();
  a.nop();
  a.bind(self);
  a.b(self);
  a.nop();
  m.load(a.finish());
  // Interrupts disabled and ERL set: an ordinary interrupt could not be taken.
  m.cpu().cop0_write64(cop0::Status, 0x3400'0006);
  m.cpu().step();
  m.cpu().step();
  const u64 at = m.cpu().pc();
  REQUIRE(at == ultra::test::Machine::kCodeBase + 8);
  m.sys->pins().nmi = true;
  m.cpu().step();
  CHECK_FALSE(m.sys->pins().nmi);
  CHECK(m.cpu().pc() == kResetVector);
  CHECK(m.cpu().cop0_read64(cop0::ErrorEPC) == at);
  const u32 status = static_cast<u32>(m.cpu().cop0_read64(cop0::Status));
  CHECK(cop0::Status::BEV::test(status));
  CHECK(cop0::Status::SR::test(status));
  CHECK(cop0::Status::ERL::test(status));
}

TEST_CASE("machine: RESET reboots through the PIF ROM and a save state mid-reset continues exactly") {
  auto sys = ultra::test::boot(ultra::test::kBootRom, 1.0);
  REQUIRE(sys->pif().boot_terminated());  // libdragon's entry point sends command 0x08
  const Tick press = sys->now();
  REQUIRE(sys->press_reset());
  const std::vector<u8> mid = sys->save_state();

  // Cause.IP4 shows the pre-NMI line while it is held (n64brew "PIF-NUS", Console Reset).
  sys->run_until(press + 2000);
  CHECK((sys->cpu().cop0_read64(cop0::Cause) >> 12) & 1);

  const Tick after_nmi = press + Pif::kPreNmiToNmiTicks + 2000;
  sys->run_until(after_nmi);
  CHECK(sys->cpu().pc() >= kResetVector);
  CHECK(sys->cpu().pc() < kPifRomEnd);
  CHECK_FALSE(sys->pif().boot_terminated());
  CHECK_FALSE(sys->pif().rom_locked());
  CHECK(((sys->pif().read_word(kBootInfoWord) >> 24) & Pif::kBootInfoResetNmi) != 0);
  CHECK(((sys->cpu().cop0_read64(cop0::Cause) >> 12) & 1) == 0);
  const std::vector<u8> at_a = sys->save_state();

  std::string err;
  auto cart = Cartridge::load(ultra::test::rom_path(ultra::test::kBootRom), &err);
  REQUIRE(cart);
  System b(SystemConfig{}, std::move(cart));
  b.power_on();
  b.load_state(mid);
  b.run_until(after_nmi);
  CHECK(b.save_state() == at_a);

  // The warm boot runs IPL1, IPL2 and IPL3 again and the program terminates the boot anew.
  sys->run_until(after_nmi + seconds(1.0));
  CHECK(sys->pif().boot_terminated());
  CHECK(sys->pif().rom_locked());
}
