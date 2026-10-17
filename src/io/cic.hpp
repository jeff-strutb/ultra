#pragma once
// Cartridge CIC (lockout) chip model as seen by the PIF: variant, region, IPL2/IPL3 seed,
// expected IPL2 checksum, and the 6105 challenge/response algorithm.
// Facts and sources: docs/hardware/pif.md and docs/hardware/cic-algorithms.md (clean-room
// functional specification of both algorithms); n64brew "PIF-NUS" (variant table), "CIC-NUS".

#include <array>
#include <optional>
#include <span>
#include <string_view>

#include "common/types.hpp"

namespace reality {

enum class CicType : u8 { Cic6101, Cic6102, Cic6103, Cic6105, Cic6106, Cic7101, Cic7102, Cic7103, Cic7105, Cic7106 };

struct CicInfo {
  CicType type;
  std::string_view name;
  bool pal;
  u8 seed;            // IPL2 and IPL3 seed (identical in all known variants)
  u64 ipl2_checksum;  // 48-bit checksum the CIC sends to the PIF at boot
};

// Table of retail variants. Source: n64brew "PIF-NUS", IPL3 checksum algorithm table.
std::span<const CicInfo> cic_table();
const CicInfo& cic_info(CicType t);

// The IPL2 checksum over the 0xFC0 bytes of IPL3 (ROM offsets 0x40..0xFFF), as computed by
// IPL2 on the CPU and compared by the PIF. Returns the 48-bit value.
u64 ipl2_checksum(std::span<const u8> ipl3, u8 seed);

// Identifies the CIC a cartridge carries by finding the variant whose expected checksum
// matches its IPL3 (the same test the PIF performs). `pal` selects between NTSC/PAL pairs
// that share an IPL3. Returns nullopt for an IPL3 no retail CIC accepts.
std::optional<CicType> detect_cic(std::span<const u8> rom, bool pal);

// CIC-NUS-6105 challenge: transforms 30 nibbles in place.
// Specification: docs/hardware/cic-algorithms.md section 2.
void cic6105_challenge(std::array<u8, 30>& nibbles);

}  // namespace reality
