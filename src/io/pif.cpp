#include "io/pif.hpp"

#include <cstring>

#include "core/savestate.hpp"

namespace ultra {

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

// Joybus frame in PIF RAM (n64brew "PIF-NUS", Joybus frame): per channel either an escape
// byte or a TX length byte, an RX length byte, TX bytes and RX bytes. RX bit 7 reports "no
// device"; TX bit 7 marks a channel to skip.
constexpr u32 kJoybusChannels = 5;
constexpr u8 kJoybusSkipChannel = 0x00;
constexpr u8 kJoybusResetChannel = 0xFD;
constexpr u8 kJoybusEndFrame = 0xFE;
constexpr u8 kJoybusNop = 0xFF;
using JoybusLen = Bits<0, 6, u8>;
using JoybusFlag = Bit<7, u8>;  // TX: skip; RX: no device

// The PIF halts the console if boot is not terminated "before 5 seconds from boot".
// Derived: n64brew "PIF-NUS", command 0x08. Exact firmware count is a Guess (ledger "pif-boot-timeout").
constexpr Tick kBootTimeoutTicks = 5 * kRcpClockHz;
}  // namespace

Pif::Pif(Scheduler& sched, CpuPins& pins) : sched_(sched), pins_(pins) {
  sched_.add(timeout_ev_, "pif.boot_timeout", &Pif::timeout_event, this);
  sched_.add(nmi_ev_, "pif.nmi", &Pif::nmi_event, this);
}

void Pif::power_on(CicType cic, VideoStandard tv, std::vector<u32> boot_rom,
                   std::optional<u64> checksum) {
  cic_ = cic;
  expected_checksum_ = checksum.value_or(cic_info(cic).ipl2_checksum);
  tv_ = tv;
  rom_.fill(0);
  for (usize i = 0; i < boot_rom.size() && (i + 1) * 4 <= kRomBytes; ++i)
    store_be32(rom_.data() + i * 4, boot_rom[i]);
  ram_.fill(0);
  halt_cpu_ = false;
  reset_pending_ = false;
  sched_.cancel(nmi_ev_);
  publish_boot_info(false);
}

void Pif::publish_boot_info(bool nmi_reset) {
  // Enter reset mode: the ROM is readable again and the boot must be terminated anew
  // (n64brew "PIF-NUS": command 0x08 "must also be set again after soft resets"; the NMI
  // "unlocks the internal PIF ROM"). The CIC handshake result is published to PIF RAM
  // 0x24..0x27, then command bit 0x80 is set (Console startup steps 3-5). After an NMI the
  // CIC is not asked again (n64brew "CIC-NUS": the soft reset does not repeat its boot
  // sequence); the PIF republishes what it learned at power on.
  rom_locked_ = false;
  boot_terminated_ = false;
  checksum_from_cpu_ = 0;
  const CicInfo& info = cic_info(cic_);
  const u8 tv_type = tv_ == VideoStandard::Pal ? 0 : tv_ == VideoStandard::Mpal ? 2 : 1;
  // ROM type 0 (cartridge); reset type 1 after an NMI.
  ram_[kBootInfoOffset + 0] = static_cast<u8>(tv_type | (nmi_reset ? kBootInfoResetNmi : 0));
  ram_[kBootInfoOffset + 1] = info.seed;
  ram_[kBootInfoOffset + 2] = info.seed;
  ram_[kBootInfoOffset + 3] = 0;
  ram_[kCommandByte] = kCmdDone;
  // The 5-second limit after a soft reset as after power on: Guess, ledger "pif-boot-timeout".
  sched_.schedule(timeout_ev_, sched_.now() + kBootTimeoutTicks);
}

bool Pif::press_reset(Tick t) {
  if (!boot_terminated_ || reset_pending_ || halt_cpu_) return false;
  // n64brew "PIF-NUS", Console Reset steps 3-8: pre-NMI (INT2) now; the CIC acknowledges the
  // RESET command after its grace time and the PIF, the button already released, drives NMI.
  // INT2 has no acknowledge, so it stays asserted until the reset (libdragon inthandler.S
  // masks IM4 for that reason); Guess, ledger "pif-prenmi-to-nmi".
  pins_.set(CpuPins::kPreNmi, true);
  reset_pending_ = true;
  sched_.schedule(nmi_ev_, t + kPreNmiToNmiTicks);
  return true;
}

void Pif::on_nmi(Tick) {
  // NMI* falling edge: the VR4300 latches it (manual 14.2, Figure 14-1); the CPU clears the
  // latch when it takes the exception. The RCP is not reset (n64brew "PIF-NUS").
  pins_.set(CpuPins::kPreNmi, false);
  pins_.nmi = true;
  reset_pending_ = false;
  publish_boot_info(true);
}

u32 Pif::read_word(u32 addr) const noexcept {
  addr &= kWordAddrMask;
  if (addr >= kRomBytes) return load_be32(ram_.data() + (addr - kRomBytes));
  // After lockout the ROM area reads as zero (n64brew "Serial Interface").
  return rom_locked_ ? 0 : load_be32(rom_.data() + addr);
}

void Pif::write_word(u32 addr, u32 value, Tick t) {
  addr &= kWordAddrMask;
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
        c = c * 256 + ram_[kChecksumOffset + i];  // big-endian bytes
        ram_[kChecksumOffset + i] = 0;
      }
      checksum_from_cpu_ = c;
      cmd = static_cast<u8>((cmd & ~kCmdAcquireChecksum) | kCmdDone);
    }
    if (cmd & kCmdRunChecksum) {
      // A wrong IPL3 checksum halts the CPU until power off.
      if (checksum_from_cpu_ != expected_checksum_) halt_cpu_ = true;
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
      // The challenge is 15 bytes = 30 nibbles, high nibble first.
      using Hi = Bits<4, 4, u8>;
      using Lo = Bits<0, 4, u8>;
      std::array<u8, 30> n;
      for (u32 i = 0; i < 15; ++i) {
        n[2 * i] = Hi::get(ram_[kChallengeOffset + i]);
        n[2 * i + 1] = Lo::get(ram_[kChallengeOffset + i]);
      }
      cic6105_challenge(n);
      for (u32 i = 0; i < 15; ++i)
        ram_[kChallengeOffset + i] = static_cast<u8>(Hi::place(n[2 * i]) | Lo::place(n[2 * i + 1]));
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
  while (p < kRamBytes - 1 && channel < kJoybusChannels) {
    const u8 tx = ram_[p];
    if (tx == kJoybusEndFrame) break;
    if (tx == kJoybusNop) { ++p; continue; }
    if (tx == kJoybusSkipChannel || tx == kJoybusResetChannel) { ++p; ++channel; continue; }
    if (p + 1 >= kRamBytes) break;
    const u32 txn = JoybusLen::get(tx);
    const u32 rxn = JoybusLen::get(ram_[p + 1]);
    if (p + 2 + txn + rxn > kRamBytes) break;
    if (!JoybusFlag::test(tx)) ram_[p + 1] = static_cast<u8>(rxn | JoybusFlag::mask);
    p += 2 + txn + rxn;
    ++channel;
  }
}

void Pif::on_timeout(Tick) {
  if (!boot_terminated_) halt_cpu_ = true;
}

void Pif::serialize(SaveState& s) {
  s.section("pif", 3);
  s.array(ram_);
  s.array(rom_);
  s.enumeration(cic_, CicType::Cic7106);
  s.enumeration(tv_, VideoStandard::Mpal);
  s.value(rom_locked_);
  s.value(boot_terminated_);
  s.value(halt_cpu_);
  s.value(checksum_from_cpu_);
  s.value(expected_checksum_);
  s.value(reset_pending_);
}

}  // namespace ultra
