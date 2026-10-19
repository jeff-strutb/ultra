// FlashRAM command state machine (docs/hardware/io.md, FlashRAM) and its cartridge wiring.

#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "core/savestate.hpp"
#include "io/cart.hpp"
#include "io/flashram.hpp"

using namespace ultra;

namespace {

void cmd(FlashRam& f, u32 v) {
  f.write16(FlashRam::kCmdReg, static_cast<u16>(v >> 16));
  f.write16(FlashRam::kCmdReg + 2, static_cast<u16>(v));
}

void load_page(FlashRam& f, const u8* bytes) {
  cmd(f, 0xB400'0000);
  for (u32 i = 0; i < FlashRam::kPageSize; i += 2)
    f.write16(FlashRam::kBase + i, static_cast<u16>((u16{bytes[i]} << 8) | bytes[i + 1]));
}

}  // namespace

TEST_CASE("flashram: powers on erased in Read Array mode and reports its silicon ID") {
  FlashRam f;
  CHECK(f.mode() == FlashRam::Mode::ReadArray);
  CHECK(f.read16(FlashRam::kBase) == 0xFFFF);
  CHECK(f.read16(FlashRam::kBase + FlashRam::kSize - 2) == 0xFFFF);
  cmd(f, 0xE100'0000);
  u64 id = 0;
  for (u32 i = 0; i < 8; i += 2) id = (id << 16) | f.read16(FlashRam::kBase + i);
  CHECK(id == FlashRam::kSiliconId);
}

TEST_CASE("flashram: program page clears bits from the page buffer, then reports status") {
  FlashRam f;
  std::vector<u8> page(FlashRam::kPageSize);
  for (u32 i = 0; i < page.size(); ++i) page[i] = static_cast<u8>(i * 7 + 1);
  load_page(f, page.data());
  CHECK(f.read16(FlashRam::kBase + 2) == ((u16{page[2]} << 8) | page[3]));  // buffer readable
  cmd(f, 0xA500'0000 | 0x123);
  CHECK(f.mode() == FlashRam::Mode::Status);
  CHECK(f.read16(FlashRam::kBase) == (FlashStatus::WsmReady::mask | FlashStatus::ProgramOk::mask));
  f.write16(FlashRam::kBase, 0);  // clear status
  CHECK(f.status() == FlashStatus::WsmReady::mask);
  cmd(f, 0xF000'0000);
  const u32 off = 0x123 * FlashRam::kPageSize;
  for (u32 i = 0; i < page.size(); ++i) CHECK(f.data()[off + i] == page[i]);
  CHECK(f.data()[off - 1] == 0xFF);
  CHECK(f.data()[off + FlashRam::kPageSize] == 0xFF);

  // Programming again ANDs: bits never return to 1 without an erase.
  std::vector<u8> second(FlashRam::kPageSize, 0xF0);
  load_page(f, second.data());
  cmd(f, 0xA500'0000 | 0x123);
  CHECK(f.data()[off + 5] == (page[5] & 0xF0));
}

TEST_CASE("flashram: sector and chip erase need their setup command") {
  FlashRam f;
  std::vector<u8> zero(FlashRam::kPageSize, 0);
  for (u32 p : {0x000u, 0x07Fu, 0x080u, 0x3FFu}) {
    load_page(f, zero.data());
    cmd(f, 0xA500'0000 | p);
  }
  cmd(f, 0x7800'0000);  // Erase with no setup: ignored
  CHECK(f.data()[0] == 0);
  cmd(f, 0x4B00'0000 | 0x05);  // a page in sector 0
  cmd(f, 0x7800'0000);
  CHECK(f.data()[0] == 0xFF);
  CHECK(f.data()[0x7F * FlashRam::kPageSize] == 0xFF);
  CHECK(f.data()[0x80 * FlashRam::kPageSize] == 0);  // sector 1 untouched
  CHECK((f.status() & FlashStatus::EraseOk::mask) != 0);
  cmd(f, 0x3C00'0000);
  cmd(f, 0x7800'0000);
  CHECK(f.data()[0x80 * FlashRam::kPageSize] == 0xFF);
  CHECK(f.data()[0x3FF * FlashRam::kPageSize] == 0xFF);
}

TEST_CASE("flashram: state round-trips through the save state") {
  FlashRam a;
  std::vector<u8> page(FlashRam::kPageSize, 0x5A);
  load_page(a, page.data());
  cmd(a, 0xA500'0000 | 0x200);
  load_page(a, page.data());  // leave a loaded buffer and Load Page mode behind
  SaveState out = SaveState::saver();
  a.serialize(out);
  FlashRam b;
  SaveState in = SaveState::loader(out.data());
  b.serialize(in);
  CHECK(b.data() == a.data());
  CHECK(b.mode() == FlashRam::Mode::LoadPage);
  CHECK(b.read16(FlashRam::kBase + 4) == 0x5A5A);
}

TEST_CASE("cart: the homebrew header's save type enables FlashRAM on the PI bus") {
  std::vector<u8> rom(0x1000, 0);
  rom[0] = 0x80;
  rom[1] = 0x37;
  rom[2] = 0x12;
  rom[3] = 0x40;
  auto plain = Cartridge::from_bytes(rom);
  CHECK(plain->flashram() == nullptr);
  u16 v = 0;
  CHECK_FALSE(plain->read16(FlashRam::kBase, v));

  rom[0x3C] = 'E';
  rom[0x3D] = 'D';
  rom[0x3F] = 0x52;  // save type 5 (FlashRAM), region-free flag
  auto cart = Cartridge::from_bytes(rom);
  REQUIRE(cart->flashram() != nullptr);
  REQUIRE(cart->read16(FlashRam::kBase, v));
  CHECK(v == 0xFFFF);
  cart->write16(FlashRam::kCmdReg, 0xE100);
  cart->write16(FlashRam::kCmdReg + 2, 0x0000);
  REQUIRE(cart->read16(FlashRam::kBase, v));
  CHECK(v == 0x1111);
}
