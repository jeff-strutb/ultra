#pragma once
// VR4300 pipeline timing constants. Units: PCycles. Each entry cites its source; Guess entries
// are listed in docs/timing/ledger.md with the probe that will measure them.

#include "common/types.hpp"

namespace ultra::timing {

// An exception stalls the pipeline 2 PCycles before the abort.
// Derived: VR4300 manual 4.7 "Pipeline Exceptions".
inline constexpr u64 kExceptionAbortCycles = 2;
// After the abort stall the handler's first instruction takes IC, then RF (manual 4.1, Figure
// 4-2), so it reaches RF this many PCycles after the stall. Derived; whether its IC overlaps
// the second stall cycle is unmeasured: ledger "exception-redirect-cycle".
inline constexpr u64 kExceptionRefetchCycles = 2;
// Cycles from the start of the excepting instruction's EX cycle to the end of the cycle in
// which each stage detects an exception (Figure 4-12): RF one cycle earlier, DC one later.
// Derived: manual 4.1 (one stage per PCycle) and Figure 4-12.
inline constexpr u64 kDcDetectAfterEx = 1;

// A delay slot killed after it was fetched (branch-likely not taken, the slot after ERET)
// still passes through the pipeline as a bubble. Derived: manual 4.2 (the slot is fetched
// before the branch resolves in EX) and ERET's description (16, "does not execute the
// instruction in the delay slot").
inline constexpr u64 kKilledSlotCycles = 1;

// Instruction micro-TLB miss, refilled from the joint TLB: 3 PCycles in RF.
// Derived: VR4300 manual 4.6.2 "Instruction TLB Miss (ITM)"; 2 entries, LRU, 4 KiB pages:
// 1.5.1 "Instruction Micro-TLB (ITLB)".
inline constexpr u64 kItlbMissCycles = 3;
inline constexpr u32 kItlbEntries = 2;
inline constexpr u32 kItlbPageShift = 12;

// A store into the D-cache keeps it busy in its WB stage; a following D-cache access stalls 1.
// Derived: VR4300 manual Table 3-1 / 4.6.7 "Data Cache Busy".
inline constexpr u64 kStoreLoadDcbCycles = 1;

// Load interlock: 1 PCycle when the next instruction appears to use the loaded register.
// Derived: VR4300 manual 4.6.5; detection rule from n64brew "VR4300", Load Delay Interlock.
inline constexpr u64 kLoadInterlockCycles = 1;

// SysAD read stall around the bus transaction, for cache refills and uncached loads/fetches
// alike (VR4300 manual Tables 11-1 / 11-2): DC (or RF) stage stall 1, address to the write
// buffer 1, synchronise with SClock and move to the internal SysAD bus 1 to 2, transfer to the
// external SysAD bus 2; after the device has answered, 1 to move the data from the external to
// the internal bus and into the cache or register file. The "1 to 2" is modelled by waiting for
// the next MasterClock edge after the fixed issue cycles. The table's "transfer from memory to
// SysAD" cycles are part of the device's response time in our bus model (core/bus.cpp, the
// arbiter), which counts until the last word the CPU waits for is on SysAD.
// Derived: VR4300 manual 11.3.2 / 11.3.3.
inline constexpr u64 kSysAdReadIssueCycles = 5;  // 1 + 1 + 1 + 2
inline constexpr u64 kSysAdReadReturnCycles = 1;

// Integer multiply/divide total EX cycles (pipeline stalls for count - 1).
// Derived: VR4300 manual Table 3-12.
inline constexpr u64 kMult = 5;
inline constexpr u64 kMultu = 5;
inline constexpr u64 kDiv = 37;
inline constexpr u64 kDivu = 37;
inline constexpr u64 kDmult = 8;
inline constexpr u64 kDmultu = 8;
inline constexpr u64 kDdiv = 69;
inline constexpr u64 kDdivu = 69;

// FPU instruction pipeline cycles (Table 7-14). Results needed by the next FPU instruction
// cost one more cycle: there is no EX->EX bypass for them, so the next instruction waits a
// cycle in EX for the DC->EX bypass (7.5.6, Figure 7-9). Trivial results finish in 2 cycles.
// Derived: VR4300 manual Table 7-14 and the paragraph preceding it.
inline constexpr u64 kFpuAddSub = 3;
inline constexpr u64 kFpuMulS = 5;
inline constexpr u64 kFpuMulD = 8;
inline constexpr u64 kFpuDivSqrtS = 29;
inline constexpr u64 kFpuDivSqrtD = 58;
inline constexpr u64 kFpuRoundTruncCeilFloor = 5;
inline constexpr u64 kFpuCvtSFromD = 2;
inline constexpr u64 kFpuCvtFromInt = 5;
inline constexpr u64 kFpuCvtDFromS = 1;
inline constexpr u64 kFpuCvtToInt = 5;
inline constexpr u64 kFpuSimple = 1;  // ABS, MOV, NEG, C.cond
inline constexpr u64 kFpuTrivial = 2;
inline constexpr u64 kFpuBypassStall = 1;

// CACHE instruction extra cycles (COp interlock): D-cache tag operations take 2 PCycles in DC.
// Derived: VR4300 manual 4.6.8.
inline constexpr u64 kCacheOpDCycles = 1;
// I-cache CACHE ops stall until complete. Guess (ledger "cache-op-icache"): probe 2.
inline constexpr u64 kCacheOpICycles = 1;

}  // namespace ultra::timing
