#pragma once
// Clock domains and their exact rational relationship to the master tick.
//
// The scheduler's unit of time is one RCP clock (MClock, 62.5 MHz). Every other clock in the
// machine is an exact rational multiple of it, so conversions are integer arithmetic with no
// accumulated drift. See docs/adr/0001-scheduler.md.

#include "common/types.hpp"

namespace ultra {

// One RCP / SysAD MasterClock cycle. 64 bits covers ~9,300 years at 62.5 MHz.
using Tick = u64;
// One VR4300 pipeline clock (PClock).
using CpuCycle = u64;

inline constexpr Tick kTickNever = ~Tick{0};

// RCLK = 250 MHz, MClock = RCLK / 4 = 62.5 MHz.
// Source: n64brew "Clock Timing", section "Crystal 2 (X2)", table of derived clocks.
inline constexpr u64 kRdramClockHz = 250'000'000;
inline constexpr u64 kRcpClockHz = 62'500'000;
inline constexpr u64 kRdramClocksPerTick = kRdramClockHz / kRcpClockHz;  // 4

// PClock = MClock * 3/2 = 93.75 MHz (DivMode pins select 3:2).
// Source: n64brew "Clock Timing", X2 table, CPU row; NEC VR4300 User's Manual
// (U10504EJ7V0UM00) 10.2 "Basic System Clocks".
inline constexpr u64 kCpuCyclesPerRatio = 3;
inline constexpr u64 kTicksPerRatio = 2;

// The CPU's SysAD bus is clocked by MasterClock, so a CPU-initiated bus transaction starts on
// the first MasterClock edge at or after the current PClock edge.
constexpr Tick cpu_to_tick_ceil(CpuCycle c) noexcept {
  return (c * kTicksPerRatio + kCpuCyclesPerRatio - 1) / kCpuCyclesPerRatio;
}
constexpr Tick cpu_to_tick_floor(CpuCycle c) noexcept {
  return (c * kTicksPerRatio) / kCpuCyclesPerRatio;
}
// First PClock edge at or after a MasterClock edge.
constexpr CpuCycle tick_to_cpu_ceil(Tick t) noexcept {
  return (t * kCpuCyclesPerRatio + kTicksPerRatio - 1) / kTicksPerRatio;
}

// Serial Interface clock = MClock / 4; PIF / cartridge serial = SI / 8.
// Source: n64brew "Clock Timing", X2 table.
inline constexpr u64 kTicksPerSiClock = 4;
inline constexpr u64 kTicksPerPifClock = kTicksPerSiClock * 8;

// VI clock (VCLK) derives from crystal X1, not from MClock. NTSC: X1 = 315/22 MHz,
// VCLK = X1 * 17 / 5. VCLK / MClock = (315 * 17) / (22 * 5 * 62.5) = 1071 / 1375.
// Source: n64brew "Clock Timing", "Crystal 1 (X1)" and "Clock Synthesis" tables.
struct VideoClockRatio {
  u64 vclk_num;   // VCLK cycles ...
  u64 tick_den;   // ... per this many MClock ticks
};
inline constexpr VideoClockRatio kVclkNtsc{1071, 1375};
// PAL: X1 = 17,734,475 Hz, VCLK = X1 * 14 / 5 = 49,656,530 Hz; ratio to 62.5 MHz reduces to
// 4,965,653 / 6,250,000.
inline constexpr VideoClockRatio kVclkPal{4'965'653, 6'250'000};
// MPAL: X1 = 2,045,250,000 / 143 Hz, VCLK = X1 * 17 / 5; ratio to 62.5 MHz =
// (2,045,250,000 * 17) / (143 * 5 * 62,500,000) reduced.
inline constexpr VideoClockRatio kVclkMpal{556'308, 715'000};

}  // namespace ultra
