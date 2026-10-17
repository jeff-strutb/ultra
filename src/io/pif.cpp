#include "io/pif.hpp"

#include <cstring>

#include "core/savestate.hpp"

namespace reality {

namespace {
// Command byte bits (n64brew "PIF-NUS", RAM-based communication protocol).
constexpr u8 kCmdJoybus = 0x01;
constexpr u8 kCmdChallenge = 0x02;
constexpr u8 kCmdTerminateBoot = 0x08;
constexpr u8 kCmdLockout = 0x10;
constexpr u8 kCmdAcquireChecksum = 0x20;
constexpr u8 kCmdRunChecksum = 0x40;
constexpr u8 kCmdDone = 0x80;
constexpr u32 kCommandByte = 0x3F;
constexpr u32 kChecksumOffset = 0x32;
constexpr u32 kChallengeOffset = 0x30;

// The PIF halts the console if boot is not terminated "before 5 seconds from boot".
// Derived: n64brew "PIF-NUS", command 0x08. Exact firmware count is a Guess (ledger "pif-boot-timeout").
constexpr Tick kBootTimeoutTicks = 5 * kRcpClockHz;
}  // namespace

Pif::Pif(Scheduler& sched) : sched_(sched) {
  sched_.add(timeout_ev_, "pif.boot_timeout", &Pif::timeout_event, this);
}

void Pif::power_on(CicType cic, VideoStandard tv, std::vector<u32> boot_rom) {
  cic_ = cic;
  rom_.fill(0);
  for (usize i = 0; i < boot_rom.size() && (i + 1) * 4 <= kRomBytes; ++i)
    store_be32(rom_.data() + i * 4, boot_rom[i]);
  ram_.fill(0);
  rom_locked_ = false;
  boot_terminated_ = false;
  halt_cpu_ = false;
  checksum_from_cpu_ = 0;

  // CIC handshake result published to PIF RAM 0x24..0x27, then command bit 0x80 set
  // (n64brew "PIF-NUS", Console startup steps 3-5).
  const CicInfo& info = cic_info(cic);
  const u8 tv_type = tv == VideoStandard::Pal ? 0 : tv == VideoStandard::Mpal ? 2 : 1;
  ram_[kBootInfoOffset + 0] = tv_type;  // reset type 0 (cold), ROM type 0 (cartridge)
  ram_[kBootInfoOffset + 1] = info.seed;
  ram_[kBootInfoOffset + 2] = info.seed;
  ram_[kBootInfoOffset + 3] = 0;
  ram_[kCommandByte] = kCmdDone;
  sched_.schedule(timeout_ev_, sched_.now() + kBootTimeoutTicks);
}

u32 Pif::read_word(u32 addr) const noexcept {
  addr &= 0x7FC;
  if (addr >= kRomBytes) return load_be32(ram_.data() + (addr - kRomBytes));
  // After lockout the ROM area reads as zero (n64brew "Serial Interface").
  return rom_locked_ ? 0 : load_be32(rom_.data() + addr);
}

void Pif::write_word(u32 addr, u32 value, Tick t) {
  addr &= 0x7FC;
  if (addr < kRomBytes) return;  // writes to ROM are ignored by the PIF
  store_be32(ram_.data() + (addr - kRomBytes), value);
  if (addr - kRomBytes == (kCommandByte & ~3u)) process_command(t);
}

void Pif::dma_write(const u8 in[64], Tick t) {
  std::memcpy(ram_.data(), in, kRamBytes);
  process_command(t);
}

void Pif::dma_read(u8 out[64], Tick) {
  if (boot_terminated_) run_joybus();
  std::memcpy(out, ram_.data(), kRamBytes);
}

void Pif::process_command(Tick) {
  u8& cmd = ram_[kCommandByte];
  if (!boot_terminated_) {
    if (cmd & kCmdAcquireChecksum) {
      // Read and clear the 6-byte checksum written by IPL2, then acknowledge with 0x80.
      u64 c = 0;
      for (u32 i = 0; i < 6; ++i) {
        c = (c << 8) | ram_[kChecksumOffset + i];
        ram_[kChecksumOffset + i] = 0;
      }
      checksum_from_cpu_ = c;
      cmd = static_cast<u8>((cmd & ~kCmdAcquireChecksum) | kCmdDone);
    }
    if (cmd & kCmdRunChecksum) {
      // A wrong IPL3 checksum halts the CPU until power off.
      if (checksum_from_cpu_ != cic_info(cic_).ipl2_checksum) halt_cpu_ = true;
      cmd = static_cast<u8>(cmd & ~kCmdRunChecksum);
    }
    if (cmd & kCmdLockout) {
      rom_locked_ = true;
      cmd = static_cast<u8>(cmd & ~kCmdLockout);
    }
    if (cmd & kCmdTerminateBoot) {
      boot_terminated_ = true;
      sched_.cancel(timeout_ev_);
      cmd = static_cast<u8>(cmd & ~kCmdTerminateBoot);
    }
    return;
  }
  if (cmd & kCmdChallenge) {
    if (cic_ == CicType::Cic6105 || cic_ == CicType::Cic7105) {
      std::array<u8, 30> n;
      for (u32 i = 0; i < 15; ++i) {
        n[2 * i] = ram_[kChallengeOffset + i] >> 4;
        n[2 * i + 1] = ram_[kChallengeOffset + i] & 0xF;
      }
      cic6105_challenge(n);
      for (u32 i = 0; i < 15; ++i)
        ram_[kChallengeOffset + i] = static_cast<u8>((n[2 * i] << 4) | n[2 * i + 1]);
    } else {
      // Other CICs invert the challenge (n64brew "CIC-NUS", Command "Challenge").
      for (u32 i = 0; i < 15; ++i) ram_[kChallengeOffset + i] = static_cast<u8>(~ram_[kChallengeOffset + i]);
    }
    cmd = static_cast<u8>(cmd & ~kCmdChallenge);
  }
  if (cmd & kCmdJoybus) {
    // Parsing happens now; handshakes execute on the next 64-byte DMA read.
    cmd = static_cast<u8>(cmd & ~kCmdJoybus);
  }
}

void Pif::run_joybus() {
  // Walk the frame: up to 5 channels; escape codes 0x00 skip, 0xFD reset, 0xFE end,
  // 0xFF nop; a handshake is TX RX tx[TX] rx[RX]. No device is attached in this phase, so
  // every handshake reports "no device" (RX bit 7).
  // Source: n64brew "PIF-NUS", Joybus frame.
  u32 p = 0;
  u32 channel = 0;
  while (p < kRamBytes - 1 && channel < 5) {
    const u8 tx = ram_[p];
    if (tx == 0xFE) break;
    if (tx == 0xFF) { ++p; continue; }
    if (tx == 0x00 || tx == 0xFD) { ++p; ++channel; continue; }
    if (p + 1 >= kRamBytes) break;
    const u32 txn = tx & 0x3F;
    const u32 rxn = ram_[p + 1] & 0x3F;
    if (p + 2 + txn + rxn > kRamBytes) break;
    if (!(tx & 0x80)) ram_[p + 1] = static_cast<u8>(rxn | 0x80);
    p += 2 + txn + rxn;
    ++channel;
  }
}

void Pif::on_timeout(Tick) {
  if (!boot_terminated_) halt_cpu_ = true;
}

void Pif::serialize(SaveState& s) {
  s.section("pif", 1);
  s.array(ram_);
  s.array(rom_);
  s.value(cic_);
  s.value(rom_locked_);
  s.value(boot_terminated_);
  s.value(halt_cpu_);
  s.value(checksum_from_cpu_);
}

}  // namespace reality
