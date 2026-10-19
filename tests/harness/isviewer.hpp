#pragma once
// Waiting on a ROM's ISViewer output without the cartridge's capacity limit.

#include <algorithm>
#include <ostream>
#include <string>
#include <string_view>

#include "core/system.hpp"

namespace ultra::harness {

// Runs `sys` in `step`-tick slices until its ISViewer output contains `marker`, the CPU freezes,
// or `limit` is reached; returns whether the marker was seen. The cartridge keeps at most
// Cartridge::kIsvOutCapacity bytes and drops the rest, so output is drained after every slice
// and the stream has no size limit. The last marker.size() - 1 bytes are carried over, so a
// marker split across two slices is still found. When `log` is given, every byte goes to it.
inline bool run_until_isviewer(System& sys, std::string_view marker, Tick limit, Tick step,
                               std::ostream* log = nullptr) {
  std::string tail;
  while (sys.now() < limit && !sys.cpu().frozen()) {
    sys.run_until(std::min(limit, sys.now() + step));
    const std::string& out = sys.cart().isviewer_output();
    if (log) log->write(out.data(), static_cast<std::streamsize>(out.size()));
    tail += out;
    sys.cart().clear_isviewer_output();
    if (tail.find(marker) != std::string::npos) return true;
    const usize keep = marker.empty() ? 0 : marker.size() - 1;
    if (tail.size() > keep) tail.erase(0, tail.size() - keep);
  }
  return false;
}

}  // namespace ultra::harness
