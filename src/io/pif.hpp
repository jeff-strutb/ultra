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
#include "core/scheduler.hpp"
#include "io/cic.hpp"
#include "io/vi.hpp"

namespace ultra {

class SaveState;

class Pif {
 public:
  static constexpr u32 kRomBytes = 0x7C0;
  static constexpr u32 kRamBytes = 0x40;

  explicit Pif(Scheduler& sched);

  // Power-on: loads the boot ROM image, runs the CIC handshake and publishes the boot info.
  void power_on(CicType cic, VideoStandard tv, std::vector<u32> boot_rom);

  // Word access through the SI window (address 0x000..0x7FF within the PIF).
  u32 read_word(u32 addr) const noexcept;
  void write_word(u32 addr, u32 value, Tick t);

  // SI 64-byte DMA. A read first executes pending Joybus work (n64brew "Serial Interface").
  void dma_read(u8 out[64], Tick t);
  void dma_write(const u8 in[64], Tick t);

  bool rom_locked() const noexcept { return rom_locked_; }
  bool boot_terminated() const noexcept { return boot_terminated_; }
  bool halted_cpu() const noexcept { return halt_cpu_; }
  CicType cic() const noexcept { return cic_; }

  void serialize(SaveState& s);

  // Boot-info word published at PIF RAM 0x24..0x27 (read by IPL2). Layout defined by this
  // emulator for its clean-room IPL2 (docs/hardware/pif.md, ledger "pif-boot-info"): byte 0x24
  // bits 1:0 TV type, bit 2 reset type (1 = NMI), bit 3 ROM type (1 = 64DD); 0x25 IPL3 seed;
  // 0x26 IPL2 seed.
  static constexpr u32 kBootInfoOffset = 0x24;

 private:
  void process_command(Tick t);
  void run_joybus();
  void on_timeout(Tick t);
  static void timeout_event(void* ctx, Tick t) { static_cast<Pif*>(ctx)->on_timeout(t); }

  Scheduler& sched_;
  std::array<u8, kRomBytes> rom_{};
  std::array<u8, kRamBytes> ram_{};
  CicType cic_ = CicType::Cic6102;
  bool rom_locked_ = false;
  bool boot_terminated_ = false;
  bool halt_cpu_ = false;
  u64 checksum_from_cpu_ = 0;
  Event timeout_ev_;
};

}  // namespace ultra
