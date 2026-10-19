#pragma once
// PIF-NUS: boot ROM window, 64-byte PIF RAM, the boot-time command protocol (checksum
// acquire/verify, ROM lockout, boot termination), CIC communication, and Joybus frames.
// Facts and sources: docs/hardware/pif.md. Primary: n64brew "PIF-NUS".
//
// The PIF's own firmware (SM5 microcontroller) is modelled behaviourally; its processing time
// is a named Guess constant. The PIF ROM contents (IPL1/IPL2, Nintendo code) are not shipped:
// pif_boot_rom.cpp generates a clean-room replacement that performs the same documented
// hardware sequence and the same PIF protocol, executed by the emulated CPU.

#include <array>
#include <optional>
#include <vector>

#include "common/types.hpp"
#include "core/clock.hpp"
#include "core/cpu_pins.hpp"
#include "core/scheduler.hpp"
#include "io/cic.hpp"
#include "io/vi.hpp"

namespace ultra {

class SaveState;

class Pif {
 public:
  static constexpr u32 kRomBytes = 0x7C0;
  static constexpr u32 kRamBytes = 0x40;

  Pif(Scheduler& sched, CpuPins& pins);

  // Power-on: loads the boot ROM image, runs the CIC handshake and publishes the boot info.
  // `checksum` replaces the CIC's retail IPL2 checksum, as a flashcart's CIC emulation does
  // when it is told the checksum of the ROM's own IPL3 (SC64 direct boot).
  void power_on(CicType cic, VideoStandard tv, std::vector<u32> boot_rom,
                std::optional<u64> checksum = std::nullopt);

  // Word access through the SI window (address 0x000..0x7FF within the PIF).
  u32 read_word(u32 addr) const noexcept;
  void write_word(u32 addr, u32 value, Tick t);

  // SI 64-byte DMA. A read first executes pending Joybus work (n64brew "Serial Interface").
  void dma_read(u8 out[64], Tick t);
  void dma_write(const u8 in[64], Tick t);

  // The console's RESET button, pressed and released at once (n64brew "PIF-NUS", Console
  // Reset): the PIF raises the pre-NMI interrupt now and, kPreNmiToNmiTicks later, takes the
  // CPU through an NMI and restarts the boot with reset type 1 (NMI) in the boot info. The
  // button only works once the boot was terminated (command 0x08); a press before that, or
  // while a reset is already under way, is ignored. Returns whether the press was accepted.
  bool press_reset(Tick t);
  bool reset_pending() const noexcept { return reset_pending_; }

  bool rom_locked() const noexcept { return rom_locked_; }
  bool boot_terminated() const noexcept { return boot_terminated_; }
  bool halted_cpu() const noexcept { return halt_cpu_; }
  // The IPL2 checksum the CPU last handed over with command 0x20 (test observation).
  u64 checksum_from_cpu() const noexcept { return checksum_from_cpu_; }
  CicType cic() const noexcept { return cic_; }

  void serialize(SaveState& s);

  // Boot-info word published at PIF RAM 0x24..0x27 (read by IPL2). Layout defined by this
  // emulator for its clean-room IPL2 (docs/hardware/pif.md, ledger "pif-boot-info"): byte 0x24
  // bits 1:0 TV type, bit 2 reset type (1 = NMI), bit 3 ROM type (1 = 64DD); 0x25 IPL3 seed;
  // 0x26 IPL2 seed.
  static constexpr u32 kBootInfoOffset = 0x24;
  static constexpr u8 kBootInfoResetNmi = 1u << 2;

  // RESET press to NMI: the CIC's 500 ms grace time (n64brew "PIF-NUS", Console Reset). The
  // firmware's exact count is unmeasured: Guess, ledger "pif-prenmi-to-nmi".
  static constexpr Tick kPreNmiToNmiTicks = kRcpClockHz / 2;

 private:
  void process_command(Tick t);
  void run_joybus();
  void on_timeout(Tick t);
  static void timeout_event(void* ctx, Tick t) { static_cast<Pif*>(ctx)->on_timeout(t); }
  void on_nmi(Tick t);
  static void nmi_event(void* ctx, Tick t) { static_cast<Pif*>(ctx)->on_nmi(t); }
  void publish_boot_info(bool nmi_reset);

  Scheduler& sched_;
  CpuPins& pins_;
  std::array<u8, kRomBytes> rom_{};
  std::array<u8, kRamBytes> ram_{};
  CicType cic_ = CicType::Cic6102;
  VideoStandard tv_ = VideoStandard::Ntsc;
  bool rom_locked_ = false;
  bool boot_terminated_ = false;
  bool halt_cpu_ = false;
  u64 checksum_from_cpu_ = 0;
  u64 expected_checksum_ = 0;  // what the CIC reports: retail, or a flashcart's override
  bool reset_pending_ = false;  // pre-NMI raised, NMI not yet taken
  Event timeout_ev_;
  Event nmi_ev_;
};

}  // namespace ultra
