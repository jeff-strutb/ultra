// SI and PIF, AI, SP and DP interfaces in isolation. Facts: docs/hardware/io.md, pif.md,
// rsp.md and rdp.md; n64brew register descriptions as cited per check.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <vector>

#include "core/cpu_pins.hpp"
#include "core/scheduler.hpp"
#include "io/ai.hpp"
#include "io/mi.hpp"
#include "io/pif.hpp"
#include "io/si.hpp"
#include "memory_fixture.hpp"
#include "rdp/dp_interface.hpp"
#include "rsp/sp_interface.hpp"

using namespace ultra;
using namespace ultra::test;

namespace {
// SI packet durations: command (2 bits) + address (9) + ack (1) + data, at MClock/32 per bit
// (n64brew "Serial Interface", "Clock Timing").
constexpr Tick k4bTicks = (2 + 9 + 1 + 32) * kTicksPerPifClock;
constexpr Tick k64bTicks = (2 + 9 + 1 + 512) * kTicksPerPifClock;

constexpr u32 kSiBase = 0x0480'0000;
constexpr u32 kSiDramAddr = kSiBase + 0x00, kSiPifAdRd64b = kSiBase + 0x04,
              kSiPifAdWr64b = kSiBase + 0x10, kSiStatus = kSiBase + 0x18;
constexpr u32 kSiStatusDmaBusy = 1u << 0, kSiStatusIoBusy = 1u << 1, kSiStatusDmaError = 1u << 3,
              kSiStatusIntr = 1u << 12;

// PIF RAM is at 0x7C0 in the PIF's word window; the command byte is RAM 0x3F.
constexpr u32 kPifRam = Pif::kRomBytes;
constexpr u32 kPifCommandWord = kPifRam + 0x3C;

struct Rcp {
  Scheduler sched;
  Channel ch;
  CpuPins pins;
  Mi mi{ch.ri, pins};
  Pif pif{sched, pins};
  Si si{sched, mi, ch.ri, pif};
  Rcp() {
    init_like_ipl3(ch.rdram);
    pif.power_on(CicType::Cic6102, VideoStandard::Ntsc, {0x1111'1111, 0x2222'2222});
  }
};

// Records the RDRAM requests a module makes, answering each one tick later.
class RecordingArbiter final : public IArbiter {
 public:
  std::vector<ArbiterRequest> requests;
  Tick request(const ArbiterRequest& r) override {
    requests.push_back(r);
    return r.tick + 1;
  }
  void refresh(Tick) override {}
  std::span<const TimingParam> params() const override { return {}; }
  void serialize(SaveState&) override {}
};
}  // namespace

TEST_CASE("si: 64-byte DMAs move PIF RAM to and from RDRAM and raise the SI interrupt") {
  Rcp r;
  std::array<u8, 64> frame{};
  for (u32 i = 0; i < 63; ++i) frame[i] = static_cast<u8>(0x40 + i);  // command byte 0: no-op
  REQUIRE(r.ch.rdram.mem_write(0x1000, frame.data(), nullptr, 64, nullptr));

  r.si.write_reg(kSiDramAddr, 0x1000, 0);
  r.si.write_reg(kSiPifAdWr64b, 0x1FC0'07C0, 0);
  CHECK(r.si.read_reg(kSiStatus, 1) == kSiStatusDmaBusy);
  // A second DMA while busy is refused and flags an error.
  r.si.write_reg(kSiPifAdRd64b, 0x1FC0'07C0, 1);
  CHECK((r.si.read_reg(kSiStatus, 1) & kSiStatusDmaError) != 0);
  r.sched.run_until(k64bTicks - 1);
  CHECK_FALSE(r.mi.pending(MiIntr::SI));
  r.sched.run_until(k64bTicks);
  CHECK(r.mi.pending(MiIntr::SI));
  const u32 status = r.si.read_reg(kSiStatus, k64bTicks);
  CHECK((status & (kSiStatusDmaBusy | kSiStatusIntr)) == kSiStatusIntr);
  CHECK(r.pif.read_word(kPifRam) == load_be32(frame.data()));
  CHECK(r.pif.read_word(kPifRam + 0x38) == load_be32(frame.data() + 0x38));

  // Any SI_STATUS write acknowledges; then PIF RAM back to RDRAM at another address.
  r.si.write_reg(kSiStatus, 0, k64bTicks);
  CHECK_FALSE(r.mi.pending(MiIntr::SI));
  r.si.write_reg(kSiDramAddr, 0x2000, k64bTicks);
  r.si.write_reg(kSiPifAdRd64b, 0x1FC0'07C0, k64bTicks);
  r.sched.run_until(2 * k64bTicks);
  std::array<u8, 64> back{};
  REQUIRE(r.ch.rdram.mem_read(0x2000, back.data(), 64, nullptr));
  CHECK(std::equal(back.begin(), back.begin() + 63, frame.begin()));
  CHECK(r.mi.pending(MiIntr::SI));
}

TEST_CASE("si: direct PIF writes are latched and released; a read waits for them") {
  Rcp r;
  constexpr u32 kAddr = 0x1FC0'0000 + kPifRam + 0x10;
  CHECK(r.si.io_write(kAddr, 0xCAFE'F00D, 0) == kWriteAcceptTicks);
  CHECK(r.si.read_reg(kSiStatus, 1) == (kSiStatusDmaBusy | kSiStatusIoBusy));
  r.si.io_write(kAddr, 0xDEAD'BEEF, 2);  // ignored while the first is pending
  u32 v = 0;
  CHECK(r.si.io_read(kAddr, v, 10) == k4bTicks + k4bTicks);
  CHECK(v == 0xCAFE'F00D);
  CHECK(r.mi.pending(MiIntr::SI));
  // The PIF ROM is outside RAM: writes to it are ignored.
  r.pif.write_word(0, 0x5555'5555, 0);
  CHECK(r.pif.read_word(0) == 0x1111'1111);
}

TEST_CASE("pif: the boot handshake publishes the seeds and checks IPL2's checksum") {
  Rcp r;
  // Boot info at RAM 0x24: TV type (NTSC = 1), IPL3 seed, IPL2 seed; command byte 0x80.
  CHECK(r.pif.read_word(kPifRam + Pif::kBootInfoOffset) == 0x013F'3F00);
  CHECK((r.pif.read_word(kPifCommandWord) & 0xFF) == 0x80);
  // IPL2 writes the 48-bit checksum at RAM 0x32..0x37 and command 0x20: the PIF takes it,
  // clears it and acknowledges with 0x80 (n64brew "PIF-NUS", Console startup).
  const u64 sum = cic_info(CicType::Cic6102).ipl2_checksum;
  r.pif.write_word(kPifRam + 0x30, static_cast<u32>(sum >> 32), 0);
  r.pif.write_word(kPifRam + 0x34, static_cast<u32>(sum), 0);
  r.pif.write_word(kPifCommandWord, 0x20, 0);
  CHECK(r.pif.checksum_from_cpu() == sum);
  CHECK(r.pif.read_word(kPifRam + 0x30) == 0);
  CHECK(r.pif.read_word(kPifRam + 0x34) == 0);
  CHECK((r.pif.read_word(kPifCommandWord) & 0xFF) == 0x80);
  r.pif.write_word(kPifCommandWord, 0x40, 0);  // compare with the CIC's value
  CHECK_FALSE(r.pif.halted_cpu());

  Rcp bad;
  bad.pif.write_word(kPifRam + 0x34, 0x1234'5678, 0);
  bad.pif.write_word(kPifCommandWord, 0x20, 0);
  bad.pif.write_word(kPifCommandWord, 0x40, 0);
  CHECK(bad.pif.halted_cpu());
}

TEST_CASE("pif: lockout hides the boot ROM and boot must end within 5 seconds") {
  Rcp r;
  CHECK(r.pif.read_word(4) == 0x2222'2222);
  r.pif.write_word(kPifCommandWord, 0x10, 0);
  CHECK(r.pif.rom_locked());
  CHECK(r.pif.read_word(4) == 0);
  r.sched.run_until(5 * kRcpClockHz - 1);
  CHECK_FALSE(r.pif.halted_cpu());
  r.sched.run_until(5 * kRcpClockHz);
  CHECK(r.pif.halted_cpu());

  Rcp ok;
  ok.pif.write_word(kPifCommandWord, 0x08, 0);
  CHECK(ok.pif.boot_terminated());
  ok.sched.run_until(6 * kRcpClockHz);
  CHECK_FALSE(ok.pif.halted_cpu());
}

TEST_CASE("pif: Joybus channels with no device answer so, and challenges are inverted") {
  Rcp r;
  r.pif.write_word(kPifCommandWord, 0x08, 0);  // end boot: commands now run Joybus
  // Channel 0: TX 1 RX 3; channel 1 skipped (0x00); a NOP; channel 2 marked skip (TX bit 7);
  // end of frame. Command byte 0x01 = run Joybus on the next read.
  std::array<u8, 64> frame{};
  const u8 cmds[] = {0x01, 0x03, 0x00, 0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0x81, 0x03, 0x00,
                     0xFF, 0xFF, 0xFF, 0xFE};
  std::copy(std::begin(cmds), std::end(cmds), frame.begin());
  frame[63] = 0x01;
  r.pif.dma_write(frame.data(), 0);
  std::array<u8, 64> out{};
  r.pif.dma_read(out.data(), 0);
  CHECK(out[1] == 0x83);  // RX length with the no-device bit
  CHECK(out[9] == 0x03);  // skipped channel untouched

  // A challenge to a CIC other than the 6105 comes back inverted (n64brew "CIC-NUS").
  std::array<u8, 64> chal{};
  for (u32 i = 0; i < 15; ++i) chal[0x30 + i] = static_cast<u8>(i * 17);
  chal[63] = 0x02;
  r.pif.dma_write(chal.data(), 0);
  r.pif.dma_read(out.data(), 0);
  for (u32 i = 0; i < 15; ++i) CHECK(out[0x30 + i] == static_cast<u8>(~(i * 17)));
}

TEST_CASE("ai: the two-buffer DMA FIFO, its status bits and the sample clock") {
  Scheduler sched;
  Rdram rdram(4 * kMiB);
  RecordingArbiter arb;
  TraceSink trace;
  Ri ri(rdram, arb, trace);
  CpuPins pins;
  Mi mi(ri, pins);
  Ai ai(sched, mi, ri, VideoStandard::Ntsc);
  constexpr u32 kBase = 0x0450'0000;
  constexpr u32 kDramAddr = kBase, kLength = kBase + 4, kControl = kBase + 8,
                kStatus = kBase + 0xC, kDacRate = kBase + 0x10;
  constexpr u32 kFull = (1u << 31) | 1u, kBusy = 1u << 30, kEnabled = 1u << 25,
                kAlwaysOne = 1u << 24;
  constexpr u32 kDacRateValue = 99;

  CHECK(ai.read_reg(kStatus, 0) == kAlwaysOne);
  ai.write_reg(kDacRate, kDacRateValue, 0);
  ai.write_reg(kControl, 1, 0);
  // DRAM_ADDR and LENGTH are in 8-byte units.
  ai.write_reg(kDramAddr, 0x1FF7, 0);  // -> 0x1FF0
  ai.write_reg(kLength, 0xFFFC'0017, 0);  // -> 0x10 (bits 17:3)
  CHECK(mi.pending(MiIntr::AI));       // raised when a buffer starts playing
  CHECK(ai.read_reg(kStatus, 0) == (kAlwaysOne | kEnabled | kBusy));
  CHECK(ai.read_reg(kLength, 0) == 0x10);
  mi.lower(MiIntr::AI);
  ai.write_reg(kDramAddr, 0x0100, 0);
  ai.write_reg(kLength, 0x10, 0);
  CHECK(ai.read_reg(kStatus, 0) == (kAlwaysOne | kEnabled | kBusy | kFull));

  // Each 8-byte fetch is 2 samples; a sample lasts DACRATE + 1 VI clocks. The first buffer's
  // two fetches end after 4 x (DACRATE + 1) VI clocks, converted exactly to MClock ticks.
  const Tick first_done = 4 * (kDacRateValue + 1) * kVclkNtsc.tick_den / kVclkNtsc.vclk_num;
  sched.run_until(first_done - 1);
  CHECK_FALSE(mi.pending(MiIntr::AI));
  sched.run_until(first_done);
  CHECK(mi.pending(MiIntr::AI));
  CHECK(ai.samples_played() == 4);
  CHECK(ai.read_reg(kStatus, first_done) == (kAlwaysOne | kEnabled | kBusy));

  // The first buffer ended on an 8 KiB boundary (0x1FF0 + 0x10), so the delayed-carry bug
  // moves the second one from 0x0100 to 0x2100 (n64brew "Audio Interface").
  const Tick later = first_done + 1000;  // past the second buffer's first fetch
  sched.run_until(later);
  REQUIRE(arb.requests.size() >= 3);
  CHECK(arb.requests[0].address == 0x1FF0);
  CHECK(arb.requests[1].address == 0x1FF8);
  CHECK(arb.requests[2].address == 0x2100);

  // DMA disabled: playback pauses.
  ai.write_reg(kControl, 0, later);
  const u64 played = ai.samples_played();
  sched.run_until(later + 100'000);
  CHECK(ai.samples_played() == played);
}

TEST_CASE("sp: DMA rows with skip, bank wrap, the final LEN and double buffering") {
  Scheduler sched;
  Channel ch;
  init_like_ipl3(ch.rdram);
  CpuPins pins;
  Mi mi(ch.ri, pins);
  SpInterface sp(sched, mi, ch.ri);
  constexpr u32 kBase = 0x0404'0000;
  constexpr u32 kMemAddr = kBase, kDramAddr = kBase + 4, kRdLen = kBase + 8,
                kStatus = kBase + 0x10, kDmaFull = kBase + 0x14, kDmaBusy = kBase + 0x18;
  std::array<u8, 64> src{};
  for (u32 i = 0; i < src.size(); ++i) src[i] = static_cast<u8>(i + 1);
  REQUIRE(ch.rdram.mem_write(0x1000, src.data(), nullptr, 64, nullptr));

  // Two rows of 8 bytes (LEN 7, COUNT 1), RDRAM advancing by SKIP 8 after each row, starting
  // 8 bytes below the end of DMEM: the second row wraps to DMEM 0.
  sp.write_reg(kMemAddr, 0x0FF8, 0);
  sp.write_reg(kDramAddr, 0x1000, 0);
  const u32 len = SpDmaLen::Skip::place(8) | SpDmaLen::Count::place(1) | SpDmaLen::Len::place(7);
  sp.write_reg(kRdLen, len, 0);
  CHECK(sp.read_reg(kDmaBusy, 0) == 1);
  CHECK(SpStatusRead::DmaBusy::test(sp.read_reg(kStatus, 0)));
  // A second request queues behind it.
  sp.write_reg(kMemAddr, 0x1100, 0);  // IMEM
  sp.write_reg(kDramAddr, 0x1030, 0);
  sp.write_reg(kRdLen, SpDmaLen::Len::place(7), 0);
  CHECK(sp.read_reg(kDmaFull, 0) == 1);
  sched.run_until(100'000);
  CHECK(sp.read_reg(kDmaBusy, 100'000) == 0);
  CHECK(sp.read_reg(kDmaFull, 100'000) == 0);

  const u8* dmem = sp.dmem();
  CHECK(std::equal(dmem + 0xFF8, dmem + 0x1000, src.begin()));
  CHECK(std::equal(dmem, dmem + 8, src.begin() + 0x10));  // after 8 bytes + skip 8
  CHECK(std::equal(sp.imem() + 0x100, sp.imem() + 0x108, src.begin() + 0x30));
  // The second transfer's registers: LEN reads 0xFF8, COUNT 0, SKIP unchanged (0).
  CHECK(sp.read_reg(kRdLen, 100'000) == 0xFF8);
  CHECK(sp.read_reg(kMemAddr, 100'000) == 0x1108);
  CHECK(sp.read_reg(kDramAddr, 100'000) == 0x1038);
}

TEST_CASE("sp: status set/clear pairs, signals, the semaphore, SP_PC and memory mirrors") {
  Scheduler sched;
  Channel ch;
  CpuPins pins;
  Mi mi(ch.ri, pins);
  SpInterface sp(sched, mi, ch.ri);
  constexpr u32 kStatus = 0x0404'0010, kSemaphore = 0x0404'001C;
  using W = SpStatusWrite;
  using R = SpStatusRead;
  CHECK(sp.halted());  // the RSP starts halted
  sp.write_reg(kStatus, W::ClrHalt::mask | W::SetHalt::mask, 0);
  CHECK(sp.halted());  // both: no change
  sp.write_reg(kStatus, W::ClrHalt::mask, 0);
  CHECK_FALSE(sp.halted());
  sp.write_reg(kStatus, W::set_sig(3) | W::SetSStep::mask, 0);
  CHECK(sp.read_reg(kStatus, 0) == (R::sig(3) | R::SStep::mask));
  sp.write_reg(kStatus, W::clr_sig(3), 0);
  CHECK(sp.read_reg(kStatus, 0) == R::SStep::mask);
  sp.write_reg(kStatus, W::SetIntr::mask, 0);
  CHECK(mi.pending(MiIntr::SP));
  sp.write_reg(kStatus, W::ClrIntr::mask, 0);
  CHECK_FALSE(mi.pending(MiIntr::SP));

  CHECK(sp.read_reg(kSemaphore, 0) == 0);  // a read returns the bit and sets it
  CHECK(sp.read_reg(kSemaphore, 0) == 1);
  sp.write_reg(kSemaphore, 0, 0);          // a write releases it
  CHECK(sp.read_reg(kSemaphore, 0) == 0);

  sp.write_pc_reg(0x0408'0000, 0xFFFF'FFFF);
  CHECK(sp.read_pc_reg(0x0408'0000) == 0xFFC);
  CHECK(sp.read_pc_reg(0x0408'0004) == 0);  // SP_IBIST

  // DMEM then IMEM, mirrored every 8 KiB.
  sp.write_mem(0x0400'1010, 0x1234'5678);
  CHECK(load_be32(sp.imem() + 0x10) == 0x1234'5678);
  CHECK(sp.read_mem(0x0400'3010) == 0x1234'5678);
}

TEST_CASE("dp: START/END, status set/clear pairs and the 24-bit clock counter") {
  DpInterface dp;
  constexpr u32 kBase = 0x0410'0000;
  constexpr u32 kStart = kBase, kEnd = kBase + 4, kCurrent = kBase + 8, kStatus = kBase + 0xC,
                kClock = kBase + 0x10;
  constexpr u32 kXbus = 1u << 0, kFreeze = 1u << 1, kStartPending = 1u << 10;
  dp.write_reg(kStart, 0xFF12'3457, 0);
  CHECK(dp.read_reg(kStart, 0) == 0x0012'3450);  // 24-bit, 8-byte aligned
  CHECK(dp.read_reg(kStatus, 0) == kStartPending);
  dp.write_reg(kEnd, 0x0012'3500, 0);
  CHECK(dp.read_reg(kCurrent, 0) == 0x0012'3450);
  CHECK(dp.read_reg(kStatus, 0) == 0);
  CHECK(dp.read_reg(kBase + 0x20, 0) == 0x0012'3450);  // mirrored every 0x20 bytes

  dp.write_reg(kStatus, 1u << 1, 0);  // SET_XBUS
  dp.write_reg(kStatus, 1u << 3, 0);  // SET_FREEZE
  CHECK(dp.read_reg(kStatus, 0) == (kXbus | kFreeze));
  dp.write_reg(kStatus, (1u << 0) | (1u << 1), 0);  // clear and set together: no change
  CHECK(dp.read_reg(kStatus, 0) == (kXbus | kFreeze));
  dp.write_reg(kStatus, (1u << 0) | (1u << 2), 0);  // CLR_XBUS, CLR_FREEZE
  CHECK(dp.read_reg(kStatus, 0) == 0);

  CHECK(dp.read_reg(kClock, 100) == 100);
  dp.write_reg(kStatus, 1u << 9, 200);  // CLR_CLOCK
  CHECK(dp.read_reg(kClock, 250) == 50);
  CHECK(dp.read_reg(kClock, 200 + (1u << 24) + 5) == 5);
}
