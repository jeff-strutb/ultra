// RDRAM devices, the RI and the v1 arbiter in isolation. Facts: docs/hardware/memory.md;
// timing model: docs/adr/0004-arbiter.md and memory/arbiter_params.h.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <vector>

#include "core/trace.hpp"
#include "memory/arbiter_params.h"
#include "memory/base_rdram_arbiter.hpp"
#include "memory/rdram.hpp"
#include "memory/ri.hpp"
#include "memory_fixture.hpp"

using namespace ultra;
namespace ap = arbiter_params;

using namespace ultra::test;

TEST_CASE("rdram: devices answer only once enabled, at the address their id gives") {
  Rdram r(4 * kMiB);
  REQUIRE(r.device_count() == 2);
  u8 b[8] = {};
  CHECK_FALSE(r.mem_read(0, b, 8, nullptr));  // no device enabled after reset
  CHECK(reg_read(r, 0, RdramReg::DeviceType) == 0);

  init_like_ipl3(r);
  // Register reads: DeviceType of a retail 2 MiB module; Delay with its read-only width
  // fields; Mode with X2 and CC inverted; MinInterval's fixed pattern (memory.md).
  CHECK(reg_read(r, 0, RdramReg::DeviceType) == 0xB419'0010);
  CHECK(reg_read(r, 2, RdramReg::Delay) ==
        (RdramDelay::AckWinDelay::place(4) | RdramDelay::ReadDelay::place(7) |
         RdramDelay::WriteDelay::place(1) | RdramDelay::kReadOnly));
  CHECK(reg_read(r, 2, RdramReg::Mode) ==
        (RdramMode::DE::mask | RdramMode::X2::mask | RdramMode::kCcMask));
  CHECK(reg_read(r, 0, RdramReg::MinInterval) == 0x0040'C0E0);
  // 2 MiB devices ignore Adr[20]: id 1 reaches the device at 0, id 3 the one at 2 MiB.
  CHECK(reg_read(r, 3, RdramReg::DeviceType) == 0xB419'0010);

  // Device 1 holds 0..2 MiB and device 0 holds 2..4 MiB.
  const u8 lo[8] = {1, 2, 3, 4, 5, 6, 7, 8}, hi[8] = {9, 10, 11, 12, 13, 14, 15, 16};
  REQUIRE(r.mem_write(0x10, lo, nullptr, 8, nullptr));
  REQUIRE(r.mem_write(2 * kMiB + 0x10, hi, nullptr, 8, nullptr));
  u8 out[8];
  REQUIRE(r.mem_read(0x10, out, 8, nullptr));
  CHECK(std::equal(out, out + 8, lo));
  REQUIRE(r.mem_read(2 * kMiB + 0x10, out, 8, nullptr));
  CHECK(std::equal(out, out + 8, hi));
  CHECK(r.raw()[1 * Rdram::kDeviceBytes + 0x10] == 1);  // stored in device 1
  CHECK(r.raw()[0 * Rdram::kDeviceBytes + 0x10] == 9);  // stored in device 0
  CHECK_FALSE(r.mem_read(4 * kMiB, out, 8, nullptr));   // no device at 4 MiB
}

TEST_CASE("rdram: 9th bits default to each byte's sign and bursts wrap within the 2 KiB row") {
  Rdram r(4 * kMiB);
  init_like_ipl3(r);
  const u8 data[4] = {0x7F, 0x80, 0x01, 0xFF};
  REQUIRE(r.mem_write(0x100, data, nullptr, 4, nullptr));
  u8 out[4], eb[4];
  REQUIRE(r.mem_read(0x100, out, 4, eb));
  CHECK((eb[0] == 0 && eb[1] == 1 && eb[2] == 0 && eb[3] == 1));
  const u8 given[4] = {1, 0, 1, 0};
  REQUIRE(r.mem_write(0x100, data, given, 4, nullptr));
  REQUIRE(r.mem_read(0x100, out, 4, eb));
  CHECK(std::equal(eb, eb + 4, given));

  // 16 bytes starting 8 below a row end: the second half lands at the row's start.
  std::array<u8, 16> burst{};
  for (u32 i = 0; i < burst.size(); ++i) burst[i] = static_cast<u8>(0xA0 + i);
  const u32 row_end = ap::kRowBytes * 3;
  REQUIRE(r.mem_write(row_end - 8, burst.data(), nullptr, 16, nullptr));
  CHECK(r.raw()[Rdram::kDeviceBytes + row_end - 8] == 0xA0);
  CHECK(r.raw()[Rdram::kDeviceBytes + row_end - ap::kRowBytes] == 0xA8);
  CHECK(r.raw()[Rdram::kDeviceBytes + row_end] == 0);
}

TEST_CASE("rdram: '1' bits are lost below the drive-current threshold") {
  // Output current is proportional to 63 - CC; the threshold is ledger "rdram-cc" (16).
  Rdram r(4 * kMiB);
  init_like_ipl3(r);
  const u8 ones[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  REQUIRE(r.mem_write(0, ones, nullptr, 4, nullptr));
  u8 out[4];
  reg_write(r, 0, RdramReg::Mode, RdramMode::DE::mask | RdramMode::place_cc(63 - 16));
  REQUIRE(r.mem_read(0, out, 4, nullptr));
  CHECK(out[0] == 0xFF);
  reg_write(r, 0, RdramReg::Mode, RdramMode::DE::mask | RdramMode::place_cc(63 - 15));
  REQUIRE(r.mem_read(0, out, 4, nullptr));
  CHECK(out[0] == 0);
}

TEST_CASE("ri: register write masks, readbacks and mirrors") {
  Channel c;
  Ri& ri = c.ri;
  constexpr u32 kBase = 0x0470'0000;
  auto at = [](RiRegister r) { return kBase + static_cast<u32>(r); };
  CHECK(ri.read_reg(at(RiRegister::Latency)) == 0xF);  // "Defaults to 0xf"
  ri.write_reg(at(RiRegister::Latency), 0xFFFF'FFF3);
  CHECK(ri.read_reg(at(RiRegister::Latency)) == 0x3);
  ri.write_reg(at(RiRegister::Mode), 0xFFFF'FFFF);
  CHECK(ri.read_reg(at(RiRegister::Mode)) == RiMode::kMask);
  ri.write_reg(at(RiRegister::Select), 0x0000'0114);
  CHECK(ri.read_reg(at(RiRegister::Select)) == 0x14);
  // Registers mirror with mask 0x001FFFE0.
  CHECK(ri.read_reg(kBase + 0x20 + static_cast<u32>(RiRegister::Select)) == 0x14);
  // CURRENT_LOAD read: ErrorAck, 1, 1, Mode.StopR, Select.TSel[0] in bits 0..4.
  ri.write_reg(at(RiRegister::Mode), RiMode::StopR::mask);
  CHECK(ri.read_reg(at(RiRegister::CurrentLoad)) == 0b11110);  // TSel = 1 (0x14 >> 4)
  ri.write_reg(at(RiRegister::Mode), 0);
  ri.write_reg(at(RiRegister::Select), 0);
  CHECK(ri.read_reg(at(RiRegister::CurrentLoad)) == 0b00110);
  // Any write to BANK_STATUS invalidates every bank and marks it dirty.
  ri.write_reg(at(RiRegister::BankStatus), 0);
  CHECK(ri.read_reg(at(RiRegister::BankStatus)) == RiBankStatus::Dirty::mask);
}

TEST_CASE("ri: memory errors, bank tracking and RDRAM register addressing") {
  Channel c;
  Ri& ri = c.ri;
  constexpr u32 kError = 0x0470'0000 + static_cast<u32>(RiRegister::Error);
  constexpr u32 kBankStatus = 0x0470'0000 + static_cast<u32>(RiRegister::BankStatus);
  u8 buf[8] = {};
  // No device enabled yet: MissingAck.
  ri.read(Master::CpuData, 0, buf, 8, 0);
  CHECK(ri.read_reg(kError) == RiError::MissingAck::mask);
  ri.write_reg(kError, 0);
  CHECK(ri.read_reg(kError) == 0);
  init_like_ipl3(c.rdram);
  ri.read(Master::CpuData, 0x0010'0000, buf, 8, 0);
  CHECK(ri.read_reg(kError) == 0);
  // The RI tracks the rows it addresses, answered or not: banks 0 and 1 are open and clean;
  // a write marks bank 1 dirty.
  CHECK(ri.read_reg(kBankStatus) == RiBankStatus::Valid::place(0b11));
  ri.write(Master::CpuData, 0x0010'0008, buf, 8, 0);
  CHECK(ri.read_reg(kBankStatus) ==
        (RiBankStatus::Valid::place(0b11) | RiBankStatus::Dirty::place(0b10)));
  // At or above 8 MiB in memory space: OverRange (and nothing answers there).
  ri.read(Master::CpuData, 8 * kMiB, buf, 8, 0);
  CHECK(ri.read_reg(kError) == (RiError::OverRange::mask | RiError::MissingAck::mask));

  // Register space: Adr[28:20] = device id, bit 19 = broadcast. Id 2 is device 0.
  constexpr u32 kRegSpace = 0x03F0'0000;
  u8 word[8];
  ri.reg_read(kRegSpace + RdramRegAddr::Device::place(2) + reg_offset(RdramReg::DeviceType),
              word, 0);
  CHECK(load_be32(word) == 0xB419'0010);
  // A broadcast RefRow write reaches both devices.
  std::array<u8, 8> packet{};
  store_be32(packet.data(), 0x1234'0000);
  ri.reg_write(kRegSpace + 0x0008'0000 + reg_offset(RdramReg::RefRow), packet.data(), 8, 0);
  CHECK(reg_read(c.rdram, 0, RdramReg::RefRow) == 0x1234'0000);
  CHECK(reg_read(c.rdram, 2, RdramReg::RefRow) == 0x1234'0000);
}

TEST_CASE("ri: HSYNC refreshes only with RI_REFRESH enabled, and refresh closes the banks") {
  Channel c;
  init_like_ipl3(c.rdram);
  Ri& ri = c.ri;
  constexpr u32 kRefresh = 0x0470'0000 + static_cast<u32>(RiRegister::Refresh);
  constexpr u32 kBankStatus = 0x0470'0000 + static_cast<u32>(RiRegister::BankStatus);
  u8 buf[8] = {};
  ri.read(Master::CpuData, 0, buf, 8, 0);
  ri.hsync(1000);
  CHECK(ri.read_reg(kBankStatus) == RiBankStatus::Valid::place(1));  // disabled: no refresh
  ri.write_reg(kRefresh, RiRefresh::Enable::mask);
  ri.hsync(2000);
  CHECK(ri.read_reg(kBankStatus) == 0);
  CHECK(RiRefresh::Bank::test(ri.read_reg(kRefresh)));  // oscillates per refresh
  ri.hsync(3000);
  CHECK_FALSE(RiRefresh::Bank::test(ri.read_reg(kRefresh)));
}

TEST_CASE("arbiter: row hits, misses, dirty rows, octbyte rounding and channel occupancy") {
  BaseRdramArbiter a;
  auto ticks = [](u64 tcycles) {
    return (tcycles + ap::kTCyclesPerTick - 1) / ap::kTCyclesPerTick;
  };
  auto data = [](u64 octbytes) { return octbytes * 8 / ap::kBytesPerTCycle; };
  constexpr u64 kRead = ap::kRequestTCycles + ap::kReadDelayTCycles;
  constexpr u64 kWrite = ap::kRequestTCycles + ap::kWriteDelayTCycles;

  // First access to bank 0: a row miss on a clean bank.
  u64 end = 100 * ap::kTCyclesPerTick + ap::kRiOverheadTCycles + ap::kRowMissTCycles + kWrite +
            data(1);
  CHECK(a.request({Master::CpuData, 0, 8, AccessKind::Write, 100}) == ticks(end));
  // Same row, presented while the channel is still busy: waits, then hits.
  end += ap::kRiOverheadTCycles + kRead + data(1);
  CHECK(a.request({Master::CpuData, 8, 8, AccessKind::Read, 100}) == ticks(end));
  // Another row of the same (now dirty) bank: miss plus the dirty write-back.
  u64 start = 1000 * ap::kTCyclesPerTick;
  end = start + ap::kRiOverheadTCycles + ap::kRowMissTCycles + ap::kRowMissDirtyExtraTCycles +
        kRead + data(1);
  CHECK(a.request({Master::CpuData, ap::kRowBytes, 8, AccessKind::Read, 1000}) == ticks(end));
  // The RI moves whole octbytes: 2 bytes at offset 7 span two of them.
  start = 2000 * ap::kTCyclesPerTick;
  end = start + ap::kRiOverheadTCycles + kRead + data(2);
  CHECK(a.request({Master::CpuData, ap::kRowBytes + 7, 2, AccessKind::Read, 2000}) == ticks(end));
  // Above the 8 tracked banks there is no row state: never a miss.
  start = 3000 * ap::kTCyclesPerTick;
  end = start + ap::kRiOverheadTCycles + kRead + data(16);
  CHECK(a.request({Master::Pi, 8 * ap::kBankBytes, 128, AccessKind::Read, 3000}) == ticks(end));
  // A refresh occupies the channel and closes every row.
  a.refresh(4000);
  start = 4000 * ap::kTCyclesPerTick + ap::kRefreshTCycles;
  end = start + ap::kRiOverheadTCycles + ap::kRowMissTCycles + kRead + data(1);
  CHECK(a.request({Master::CpuData, ap::kRowBytes, 8, AccessKind::Read, 4000}) == ticks(end));
}
