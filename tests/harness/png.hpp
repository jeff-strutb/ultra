#pragma once
// Reference-image reader used by the test harness (krom expected screenshots).

#include <stdexcept>
#include <string>
#include <vector>

#include "common/types.hpp"

namespace ultra::harness {

struct Image {
  u32 width = 0;
  u32 height = 0;
  std::vector<u32> rgb;  // 0xRRGGBB, row-major
};

// Throws std::runtime_error on unsupported or malformed files.
Image read_png(const std::string& path);

}  // namespace ultra::harness
