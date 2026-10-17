#pragma once
// Named timing constants of the v1 RDRAM arbiter (BaseRdramArbiter).
// Every entry carries its confidence; docs/timing/ledger.md lists the probe that will
// measure each Guess. Units: TCycle = one RDRAM clock (RCLK, 4 ns); 4 TCycles per MClock tick.

#include "common/types.hpp"

namespace reality::arbiter_params {

// RDRAM clock cycles per RCP tick: RCLK 250 MHz / MClock 62.5 MHz.
// Derived: n64brew "Clock Timing", X2 table. Also n64brew "RDRAM", Reset Complications
// ("TCycle 4, RCP Cycle 1").
inline constexpr u64 kTCyclesPerTick = 4;

// Request packet occupies 3 TCycles (6 bytes on the 9-bit, double-data-rate bus).
// Derived: n64brew "RDRAM", Delay register / Reset Complications ("Write request packet is
// send starting from (TCycle 0, RCP Cycle 0), finishing after 3 TCycles").
inline constexpr u64 kRequestTCycles = 3;

// Data moves 2 bytes per TCycle (both clock edges): 8 bytes in 4 TCycles.
// Derived: n64brew "RDRAM", Reset Complications ("64bits of Data is send starting from
// (TCycle 4 ...) finishing at (TCycle7 ...)").
inline constexpr u64 kBytesPerTCycle = 2;

// RI is hardwired to WriteDelay = 1 TCycle.
// Derived: n64brew "RDRAM", Reset Complications.
inline constexpr u64 kWriteDelayTCycles = 1;

// ReadDelay programmed by IPL3 to 7 TCycles (Delay register 0x18082838 -> ReadDelay=111b).
// Derived: n64brew "RDRAM", Delay register and Initialization Sequence step 5b.
inline constexpr u64 kReadDelayTCycles = 7;

// Maximum burst the RI issues as one RDRAM transaction (16 octbytes).
// Derived: n64brew "RDRAM Interface", Count section ("RCP supports DMA bursts upto a maximum
// of 128 bytes").
inline constexpr u32 kMaxBurstBytes = 128;

// Rows are 2 KiB; one open row per 1 MiB bank, 8 banks tracked by RI.
// Derived: n64brew "RDRAM Interface", Bank Status Tracking; DeviceType 0xB4190010
// (ColumnBits=11) from the hardware dump on the same page.
inline constexpr u32 kRowBytes = 2048;
inline constexpr u32 kBankBytes = 1u << 20;
inline constexpr u32 kTrackedBanks = 8;

// Extra latency when the addressed bank's open row differs (NAck, precharge, sense, retry).
// Guess (ledger "arb-row-miss"): no public measurement. Probe 4 (RDRAM access patterns).
inline constexpr u64 kRowMissTCycles = 28;

// Extra latency when the evicted row was dirty (write-back of the sense amps first).
// Guess: ratio taken from RI_REFRESH Dirty/Clean RefreshDelay (54 vs 52, n64brew "RDRAM
// Interface"), i.e. +2 units of 4 TCycles. Ledger "arb-row-miss-dirty"; probe 4.
inline constexpr u64 kRowMissDirtyExtraTCycles = 8;

// RI/MI pipeline overhead per transaction between the requester and the RDRAM packet.
// Guess (ledger "arb-ri-overhead"): probe 4 measures it from the CPU side.
inline constexpr u64 kRiOverheadTCycles = 8;

// Bus time consumed by one refresh (SetRR broadcast refreshes two rows in all banks).
// Guess (ledger "arb-refresh"): probe 6 (VI and refresh coupling) measures it.
inline constexpr u64 kRefreshTCycles = 52 * 4;

}  // namespace reality::arbiter_params
