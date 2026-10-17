#pragma once
// v1 arbiter: Base RDRAM protocol model with a single shared channel, per-bank open-row
// tracking, and requests served in arrival order (the timing thread presents requests in
// strict tick order, so arrival order is fixed priority by time). Constants live in
// arbiter_params.h with their confidence.

#include <array>

#include "memory/arbiter.hpp"

namespace ultra {

class BaseRdramArbiter final : public IArbiter {
 public:
  BaseRdramArbiter();

  Tick request(const ArbiterRequest& req) override;
  void refresh(Tick tick) override;
  std::span<const TimingParam> params() const override;
  void serialize(SaveState& s) override;

  // Channel occupancy in TCycles (RDRAM clocks), for tests.
  u64 channel_free_tcycle() const noexcept { return channel_free_; }

 private:
  struct Bank {
    u32 open_row = ~0u;  // ~0 = no row open (after refresh / reset)
    u8 dirty = 0;
  };

  u64 channel_free_ = 0;  // first TCycle the channel is idle
  std::array<Bank, 8> banks_{};
};

}  // namespace ultra
