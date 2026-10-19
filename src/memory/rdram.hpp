#pragma once
// RDRAM devices on the Rambus channel: memory arrays (with the 9th bit), per-device register
// files, Id matching, the SIn/SOut enable chain, and the delay/current-control behaviours that
// IPL3's initialisation depends on. Facts and sources: docs/hardware/memory.md.
//
// The RI (memory/ri.hpp) translates CPU-physical addresses into channel transactions; this
// class answers those transactions. Data is stored per device, so memory only appears where a
// device has been enabled and given an id, exactly like hardware.

#include <array>
#include <memory>
#include <span>

#include "common/types.hpp"

namespace ultra {

class SaveState;

// RDRAM register numbers (n64brew "RDRAM", Register summary).
enum class RdramReg : u32 {
  DeviceType = 0,
  DeviceId = 1,
  Delay = 2,
  Mode = 3,
  RefInterval = 4,
  RefRow = 5,
  RasInterval = 6,
  MinInterval = 7,
  AddressSelect = 8,
  DeviceManufacturer = 9,
  Row = 128,
};

// DeviceId register: IdField bits are scattered across the (byte-swapped) register.
// Source: n64brew "RDRAM", 0x01 DeviceId.
struct RdramDeviceId {
  using IdLow = Bits<26, 6>;    // IdField[25:20]
  using IdBit26 = Bit<23>;      // IdField[26]
  using IdMid = Bits<8, 8>;     // IdField[34:27]
  using IdBit35 = Bit<7>;       // IdField[35]
  static constexpr u32 kWriteMask = IdLow::mask | IdBit26::mask | IdMid::mask | IdBit35::mask;

  // Returns IdField[35:20] as a 16-bit number (units of 1 MiB).
  static constexpr u32 id_field(u32 reg) noexcept {
    return IdLow::get(reg) | (IdBit26::get(reg) << 6) | (IdMid::get(reg) << 7) |
           (IdBit35::get(reg) << 15);
  }
};

// Delay register. Source: n64brew "RDRAM", 0x02 Delay.
struct RdramDelay {
  using AckWinDelay = Bits<27, 3>;
  using ReadDelay = Bits<19, 3>;
  using AckDelay = Bits<11, 2>;
  using WriteDelay = Bits<3, 3>;
  // Read-only "number of bits" fields: AckWinBits=3, ReadBits=3, AckBits=2, WriteBits=3.
  static constexpr u32 kReadOnly = (3u << 24) | (3u << 16) | (2u << 8) | 3u;
  static constexpr u32 kWriteMask = AckWinDelay::mask | ReadDelay::mask | AckDelay::mask |
                                    WriteDelay::mask;
  // Encodings: WriteDelay 001b..111b = 1..7, 000b = 8. ReadDelay 111b = 7, 000b..110b = 8..14.
  static constexpr u32 write_delay_tcycles(u32 reg) noexcept {
    u32 f = WriteDelay::get(reg);
    return f == 0 ? 8 : f;
  }
  static constexpr u32 read_delay_tcycles(u32 reg) noexcept {
    u32 f = ReadDelay::get(reg);
    return f == 7 ? 7 : 8 + f;
  }
};

// Mode register. Source: n64brew "RDRAM", 0x03 Mode.
struct RdramMode {
  using CE = Bit<31>;  // current control enable (1 = auto)
  using X2 = Bit<30>;  // inverted when read
  using PL = Bit<29>;
  using SV = Bit<28>;
  using SK = Bit<27>;
  using AS = Bit<26>;
  using DE = Bit<25>;  // device enable
  using LE = Bit<24>;
  using AD = Bit<19>;
  // CC[5:0] lives in bits 23,15,7 (C5,C4,C3) and 22,14,6 (C2,C1,C0).
  static constexpr u32 kCcMask = (1u << 23) | (1u << 15) | (1u << 7) | (1u << 22) |
                                 (1u << 14) | (1u << 6);
  static constexpr u32 kWriteMask = CE::mask | X2::mask | PL::mask | SK::mask | AS::mask |
                                    DE::mask | LE::mask | AD::mask | kCcMask;
  static constexpr u32 cc(u32 reg) noexcept {
    return (((reg >> 23) & 1) << 5) | (((reg >> 15) & 1) << 4) | (((reg >> 7) & 1) << 3) |
           (((reg >> 22) & 1) << 2) | (((reg >> 14) & 1) << 1) | ((reg >> 6) & 1);
  }
  static constexpr u32 place_cc(u32 v) noexcept {
    return (((v >> 5) & 1) << 23) | (((v >> 4) & 1) << 15) | (((v >> 3) & 1) << 7) |
           (((v >> 2) & 1) << 22) | (((v >> 1) & 1) << 14) | ((v & 1) << 6);
  }
};

class Rdram {
 public:
  // All retail modules are 2 MiB (2 banks x 512 rows x 2 KiB, 9-bit bytes).
  // Source: n64brew "RDRAM" (overview; DeviceType 0xB4190010 dump in "RDRAM Interface").
  static constexpr u32 kDeviceBytes = 2u << 20;
  static constexpr u32 kMaxDevices = 4;

  // installed_bytes: 4 MiB (console) or 8 MiB (with Expansion Pak).
  explicit Rdram(u32 installed_bytes);

  u32 installed_bytes() const noexcept { return static_cast<u32>(device_count_) * kDeviceBytes; }
  usize device_count() const noexcept { return device_count_; }

  // ---- channel transactions (issued by the RI) -------------------------------------------
  // Memory space. `addr` is the 26-bit channel address (Adr[25:0] of AdrS, no swapping).
  // Returns false when no device responded (reads then deliver zeros).
  bool mem_read(u32 addr, u8* out, u32 len, u8* ebits_out);
  // `mask` is a per-byte write enable, `ebits` the 9th bits to store (nullable = from data).
  bool mem_write(u32 addr, const u8* data, const u8* ebits, u32 len, const u8* mask);

  // Register space. The transaction carries one octbyte; devices use bytes 0-3 as the
  // register value (n64brew "RDRAM", Address alignment issues). `id` is Adr[28:20].
  u64 reg_read(u32 id, u32 reg);
  void reg_write(u32 id, u32 reg, const u8* stream, u32 stream_len, bool broadcast);

  // Refresh broadcast (SetRR) from the RI on VI HSYNC.
  void refresh();

  // ---- direct views for DMA engines and tests ----------------------------------------------
  // Resolve a channel address to backing storage. Returns nullptr when unmapped. The returned
  // pointer is valid up to the end of the device (`*avail` bytes).
  u8* resolve(u32 addr, u32* avail) noexcept;
  u8* resolve_ebits(u32 addr) noexcept;

  // Raw device storage (device-major), for save states and tests.
  std::span<u8> raw() noexcept { return {data_.get(), device_count_ * usize{kDeviceBytes}}; }

  void serialize(SaveState& s);

  // Current-control model parameters (Guess: ledger "rdram-cc").
  // Data '1' bits only survive the channel when the driver's output current, which is
  // proportional to (63 - CC), reaches this many units.
  static constexpr u32 kMinCurrentUnits = 16;

 private:
  struct Device {
    u32 device_id = 0;
    u32 delay = 0;
    u32 mode = 0;
    u32 ref_interval = 0;
    u32 ref_row = 0;
    u32 ras_interval = 0;
    u32 min_interval = 0;
    u32 address_select = 0;
  };

  bool enabled(usize i) const noexcept { return RdramMode::DE::test(devs_[i].mode); }
  bool sin_high(usize i) const noexcept { return i == 0 || enabled(i - 1); }
  bool id_match(usize i, u32 id) const noexcept {
    // 2 MiB devices ignore Adr[20] (n64brew "RDRAM", DeviceId: k = 21 for 16/18 Mbit).
    return (RdramDeviceId::id_field(devs_[i].device_id) >> 1) == (id >> 1);
  }
  int find_memory_device(u32 addr) const noexcept;
  bool drives_ones(usize i) const noexcept;
  u32 read_register_value(usize i, u32 reg) const noexcept;
  void write_register_value(usize i, u32 reg, u32 value);
  void rebuild_map();

  usize device_count_;
  std::array<Device, kMaxDevices> devs_{};
  std::unique_ptr<u8[]> data_;
  std::unique_ptr<u8[]> ebits_;
  // Map of 1 MiB channel regions (0..63) to device index, -1 = none.
  // not state: derived from the devices' registers; rebuilt on load.
  std::array<s8, 64> map_{};
};

}  // namespace ultra
