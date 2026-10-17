#pragma once
// RDRAM bus arbiter: the single place where RDRAM bus latency is decided.
// See docs/adr/0004-arbiter.md and docs/SPEC.md "Unknowns".
//
// IArbiter is one of the two virtual boundaries the spec allows; it is called once per bus
// transaction, never per tick. Swapping the implementation changes timing, never results:
// data movement happens in the requester, the arbiter only answers "when does it complete".

#include <span>
#include <string_view>

#include "common/types.hpp"
#include "core/clock.hpp"
#include "core/trace.hpp"

namespace reality {

class SaveState;

enum class Confidence : u8 {
  Measured,  // matched against a hardware measurement by a probe ROM
  Derived,   // computed from a primary source (datasheet, manual, patent, wiki measurement)
  Guess,     // placeholder; a probe that would measure it is named in docs/timing/ledger.md
};

constexpr std::string_view to_string(Confidence c) {
  switch (c) {
    case Confidence::Measured: return "Measured";
    case Confidence::Derived: return "Derived";
    case Confidence::Guess: return "Guess";
  }
  return "?";
}

struct TimingParam {
  std::string_view name;
  u64 value;
  std::string_view unit;
  Confidence confidence;
  std::string_view source;
};

enum class AccessKind : u8 { Read, Write };

struct ArbiterRequest {
  Master master;
  u32 address;  // physical RDRAM address
  u32 size;     // bytes moved on the RDRAM bus (1..128)
  AccessKind kind;
  Tick tick;    // earliest tick the request can be presented to the RI
};

class IArbiter {
 public:
  virtual ~IArbiter() = default;
  // Returns the tick at which the transaction completes (data delivered for reads, data
  // accepted for writes). Must be >= req.tick. Implementations may keep state (open rows,
  // bus occupancy) and must be deterministic functions of the request sequence.
  virtual Tick request(const ArbiterRequest& req) = 0;
  // RDRAM refresh triggered by VI HSYNC (n64brew "RDRAM Interface", RI_REFRESH).
  virtual void refresh(Tick tick) = 0;
  virtual std::span<const TimingParam> params() const = 0;
  virtual void serialize(SaveState& s) = 0;
};

}  // namespace reality
