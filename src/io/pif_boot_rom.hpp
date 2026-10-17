#pragma once
// Clean-room PIF boot ROM (IPL1 + IPL2) image, as 32-bit big-endian words for PIF ROM
// offsets 0x000 upward. See pif_boot_rom.cpp for what it does and why.

#include <vector>

#include "common/types.hpp"

namespace ultra {

std::vector<u32> make_pif_boot_rom();

}  // namespace ultra
