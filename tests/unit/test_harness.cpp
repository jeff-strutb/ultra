// The ROM harness's own machinery, driven by real CPU code on a booted console.

#include <catch2/catch_test_macros.hpp>
#include <sstream>
#include <string>
#include <string_view>

#include "io/cart.hpp"
#include "io/pi.hpp"
#include "isviewer.hpp"
#include "machine.hpp"

using namespace ultra;
using namespace ultra::mips;

namespace {

// PI_STATUS through KSEG1 (n64brew "Parallel Interface": PI registers at 0x0460_0000, STATUS at
// +0x10). A CPU write to the PI bus while a previous one is pending is ignored (pi.cpp io_write),
// so the program waits for both busy bits to clear before each one.
constexpr u32 kPiStatusKseg1 = 0xA460'0000u + static_cast<u32>(PiRegister::Status);
constexpr u32 kPiBusy = PiStatusRead::DmaBusy::mask | PiStatusRead::IoBusy::mask;
constexpr u32 kKseg1 = 0xA000'0000u;  // n64brew "Memory map": KSEG1, unmapped and uncached
constexpr u32 kIsvKseg1 = kKseg1 | Cartridge::kIsvBase;
constexpr u32 kIsvLenOffset = Cartridge::kIsvLenReg - Cartridge::kIsvBase;
constexpr u32 kIsvBufOffset = Cartridge::kIsvBuffer - Cartridge::kIsvBase;
constexpr u32 kIsvBufBytes = Cartridge::kIsvSize - kIsvBufOffset;

u32 be_word(std::string_view s, usize at) {
  u32 w = 0;
  for (usize i = 0; i < 4; ++i) w = (w << 8) | static_cast<u8>(at + i < s.size() ? s[at + i] : ' ');
  return w;
}

}  // namespace

// hwprobe streams its full record over ISViewer before its completion line, and the stream grows
// with every probe group. The cartridge keeps only kIsvOutCapacity bytes, so a harness that
// scans the output without draining it never sees a completion line that comes later.
TEST_CASE("harness finds an ISViewer marker after more output than the cartridge holds", "[harness]") {
  test::Machine m;
  Cartridge& cart = m.sys->cart();
  cart.clear_isviewer_output();

  // The buffer holds 32-byte text lines; the program emits the whole buffer kFloods times.
  constexpr u32 kLine = 32;
  static_assert(kIsvBufBytes % kLine == 0);
  for (u32 i = 0; i < kIsvBufBytes; i += 2) {
    auto ch = [&](u32 k) -> u8 { return (k % kLine) == kLine - 1 ? '\n' : static_cast<u8>('A' + (k / kLine) % 26); };
    cart.write16(Cartridge::kIsvBuffer + i, static_cast<u16>((ch(i) << 8) | ch(i + 1)));
  }
  constexpr u32 kFloods = Cartridge::kIsvOutCapacity / kIsvBufBytes + 16;
  constexpr usize kFloodBytes = usize{kFloods} * kIsvBufBytes;
  static_assert(kFloodBytes > Cartridge::kIsvOutCapacity);

  // The marker goes out in two messages with a delay between them far longer than a harness
  // slice, so it is split across two drains.
  constexpr std::string_view kHead = "HWP RECORD ", kRest = "STAGE 1\n";
  constexpr u32 kDelayLoops = 200'000;
  constexpr Tick kStep = 2'000;

  Assembler a(test::Machine::kCodeBase);
  auto pi_wait = [&] {
    const auto wait = a.new_label();
    a.bind(wait);
    a.lw(t9, 0, s1);
    a.andi(t9, t9, kPiBusy);
    a.bne(t9, zero, wait);
    a.nop();
  };
  auto emit = [&](std::string_view text) {
    for (usize i = 0; i < text.size(); i += 4) {
      pi_wait();
      a.li(t0, be_word(text, i));
      a.sw(t0, static_cast<s32>(kIsvBufOffset + i), s0);
    }
    pi_wait();
    a.li(t0, static_cast<u32>(text.size()));
    a.sw(t0, kIsvLenOffset, s0);
  };
  a.li(s0, kIsvKseg1);
  a.li(s1, kPiStatusKseg1);
  a.li(s2, kFloods);
  a.li(s3, kIsvBufBytes);
  const auto flood = a.new_label();
  a.bind(flood);
  pi_wait();
  a.sw(s3, kIsvLenOffset, s0);
  a.addiu(s2, s2, -1);
  a.bne(s2, zero, flood);
  a.nop();
  emit(kHead);
  a.li(t0, kDelayLoops);
  const auto delay = a.new_label();
  a.bind(delay);
  a.addiu(t0, t0, -1);
  a.bne(t0, zero, delay);
  a.nop();
  emit(kRest);
  const auto self = a.new_label();
  a.bind(self);
  a.b(self);
  a.nop();
  m.load(a.finish());

  std::ostringstream log;
  const Tick limit = m.sys->now() + kRcpClockHz;
  REQUIRE(harness::run_until_isviewer(*m.sys, "HWP RECORD STAGE 1", limit, kStep, &log));
  CHECK(cart.isviewer_dropped() == 0);
  const std::string text = log.str();
  REQUIRE(text.size() == kFloodBytes + kHead.size() + kRest.size());
  CHECK(text.ends_with(std::string(kHead) + std::string(kRest)));
}
