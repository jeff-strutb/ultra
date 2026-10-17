#include "memory/base_rdram_arbiter.hpp"

#include <algorithm>

#include "core/savestate.hpp"
#include "memory/arbiter_params.h"

namespace ultra {

namespace ap = arbiter_params;

namespace {
constexpr TimingParam kParams[] = {
    {"TCyclesPerTick", ap::kTCyclesPerTick, "tcycle/tick", Confidence::Derived,
     "n64brew Clock Timing; RDRAM Reset Complications"},
    {"RequestTCycles", ap::kRequestTCycles, "tcycle", Confidence::Derived,
     "n64brew RDRAM, Reset Complications"},
    {"BytesPerTCycle", ap::kBytesPerTCycle, "byte/tcycle", Confidence::Derived,
     "n64brew RDRAM, Reset Complications"},
    {"WriteDelayTCycles", ap::kWriteDelayTCycles, "tcycle", Confidence::Derived,
     "n64brew RDRAM, Reset Complications"},
    {"ReadDelayTCycles", ap::kReadDelayTCycles, "tcycle", Confidence::Derived,
     "n64brew RDRAM, Delay register; IPL3 init step 5b"},
    {"MaxBurstBytes", ap::kMaxBurstBytes, "byte", Confidence::Derived,
     "n64brew RDRAM Interface, Count"},
    {"RowMissTCycles", ap::kRowMissTCycles, "tcycle", Confidence::Guess, "probe 4"},
    {"RowMissDirtyExtraTCycles", ap::kRowMissDirtyExtraTCycles, "tcycle", Confidence::Guess,
     "probe 4"},
    {"RiOverheadTCycles", ap::kRiOverheadTCycles, "tcycle", Confidence::Guess, "probe 4"},
    {"RefreshTCycles", ap::kRefreshTCycles, "tcycle", Confidence::Guess, "probe 6"},
};
}  // namespace

BaseRdramArbiter::BaseRdramArbiter() = default;

Tick BaseRdramArbiter::request(const ArbiterRequest& req) {
  const u64 start = std::max(req.tick * ap::kTCyclesPerTick, channel_free_);
  u64 t = start + ap::kRiOverheadTCycles;

  // Bank status tracking covers only the bottom 8 MiB (n64brew RDRAM Interface).
  const u32 bank_index = req.address / ap::kBankBytes;
  if (bank_index < ap::kTrackedBanks) {
    Bank& bank = banks_[bank_index];
    const u32 row = (req.address % ap::kBankBytes) / ap::kRowBytes;
    if (bank.open_row != row) {
      t += ap::kRowMissTCycles;
      if (bank.dirty) t += ap::kRowMissDirtyExtraTCycles;
      bank.open_row = row;
      bank.dirty = 0;
    }
    if (req.kind == AccessKind::Write) bank.dirty = 1;
  }

  // Bytes on the bus: the RI moves whole octbytes.
  const u64 octbytes = (u64{req.size} + (req.address & 7) + 7) / 8;
  const u64 data_tcycles = octbytes * 8 / ap::kBytesPerTCycle;
  t += ap::kRequestTCycles;
  t += req.kind == AccessKind::Read ? ap::kReadDelayTCycles : ap::kWriteDelayTCycles;
  t += data_tcycles;

  channel_free_ = t;
  return (t + ap::kTCyclesPerTick - 1) / ap::kTCyclesPerTick;
}

void BaseRdramArbiter::refresh(Tick tick) {
  const u64 start = std::max(tick * ap::kTCyclesPerTick, channel_free_);
  channel_free_ = start + ap::kRefreshTCycles;
  // A refresh closes every bank's open row.
  for (Bank& b : banks_) b = Bank{};
}

std::span<const TimingParam> BaseRdramArbiter::params() const { return kParams; }

void BaseRdramArbiter::serialize(SaveState& s) {
  s.section("arbiter.base_rdram", 1);
  s.value(channel_free_);
  for (Bank& b : banks_) {
    s.value(b.open_row);
    s.value(b.dirty);
  }
}

}  // namespace ultra
