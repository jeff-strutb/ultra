#pragma once
// VR4300 System Control Coprocessor register layouts.
// Source: NEC VR4300 User's Manual (U10504EJ7V0UM00), 5.4 and 6.3.

#include "common/types.hpp"

namespace ultra::cop0 {

enum Reg : u32 {
  Index = 0,
  Random = 1,
  EntryLo0 = 2,
  EntryLo1 = 3,
  Context = 4,
  PageMask = 5,
  Wired = 6,
  BadVAddr = 8,
  Count = 9,
  EntryHi = 10,
  Compare = 11,
  Status = 12,
  Cause = 13,
  EPC = 14,
  PRId = 15,
  Config = 16,
  LLAddr = 17,
  WatchLo = 18,
  WatchHi = 19,
  XContext = 20,
  ParityError = 26,
  CacheError = 27,
  TagLo = 28,
  TagHi = 29,
  ErrorEPC = 30,
};

// 6.3.5 Status Register.
struct Status {
  using IE = Bit<0>;
  using EXL = Bit<1>;
  using ERL = Bit<2>;
  using KSU = Bits<3, 2>;
  using UX = Bit<5>;
  using SX = Bit<6>;
  using KX = Bit<7>;
  using IM = Bits<8, 8>;
  using DS = Bits<16, 9>;  // diagnostic status
  using DE = Bit<16>;
  using CE = Bit<17>;
  using CH = Bit<18>;
  using SR = Bit<20>;
  using TS = Bit<21>;
  using BEV = Bit<22>;
  using ITS = Bit<24>;
  using RE = Bit<25>;
  using FR = Bit<26>;
  using RP = Bit<27>;
  using CU0 = Bit<28>;
  using CU1 = Bit<29>;
  using CU2 = Bit<30>;
  using CU3 = Bit<31>;
  // Bit 19 (NMI on VR4400) and bit 23 are RFU. TS (21) is read-only from software.
  static constexpr u32 kWriteMask = 0xFF57FFFF & ~TS::mask;
};

// 6.3.6 Cause Register.
struct Cause {
  using ExcCode = Bits<2, 5>;
  using IP = Bits<8, 8>;
  using IPSoft = Bits<8, 2>;  // IP0, IP1 are software writable
  using IPExt = Bits<10, 5>;  // IP2..IP6: external interrupt pins Int0..Int4
  using IP7 = Bit<15>;        // timer interrupt (Count == Compare)
  using CE = Bits<28, 2>;
  using BD = Bit<31>;
  static constexpr u32 kWriteMask = IPSoft::mask;
};

enum class ExcCode : u32 {
  Int = 0,
  Mod = 1,
  TLBL = 2,
  TLBS = 3,
  AdEL = 4,
  AdES = 5,
  IBE = 6,
  DBE = 7,
  Sys = 8,
  Bp = 9,
  RI = 10,
  CpU = 11,
  Ov = 12,
  Tr = 13,
  FPE = 15,
  WATCH = 23,
};

// Table 5-6: cache algorithm 2 is "cache is not used"; every other value is cached. Used by
// Config.K0, EntryLo.C and the xkphys CCA field.
inline constexpr u32 kCacheAlgUncached = 2;

// 5.4.6 Config Register: bits 31, 23..16 (00000110), 14..4 (11001000110) are fixed.
// EC (30:28) = 111b for a 1:1.5 MasterClock:PClock ratio (mPD30200-100 table).
struct Config {
  using K0 = Bits<0, 3>;
  using CU = Bit<3>;
  using BE = Bit<15>;
  using EP = Bits<24, 4>;
  using EC = Bits<28, 3>;
  static constexpr u32 kFixed = (0b00000110u << 16) | (0b11001000110u << 4);
  static constexpr u32 kWriteMask = K0::mask | CU::mask | BE::mask | EP::mask;
  static constexpr u32 kEcNintendo = 0b111;
};

// 5.4.3 PageMask, EntryHi, EntryLo.
struct PageMask {
  using Mask = Bits<13, 12>;
  static constexpr u32 kWriteMask = Mask::mask;
};
struct EntryLo {
  using G = Bit<0, u64>;
  using V = Bit<1, u64>;
  using D = Bit<2, u64>;
  using C = Bits<3, 3, u64>;
  using PFN = Bits<6, 20, u64>;
  static constexpr unsigned kPfnShift = 12;  // PFN is physical address bits 31:12 (5.4.3)
  // A TLB entry keeps PFN, C, D, V, G only. Source: n64-systemtest tlb "EntryLo0/1/Hi after
  // TLBWI/TLBR".
  static constexpr u64 kTlbMask = PFN::mask | C::mask | D::mask | V::mask | G::mask;
  // Bits 29:0 are writable: G..PFN and also bits 29:26, which manual Figure 5-10 shows as zero
  // but which read back on hardware. Source: n64-systemtest tlb EntryLo0/1Masking(64); ledger
  // "entrylo-write-mask".
  static constexpr u64 kWriteMask = 0x3FFFFFFF;
};
struct EntryHi {
  using ASID = Bits<0, 8, u64>;
  using VPN2 = Bits<13, 27, u64>;
  using R = Bits<62, 2, u64>;
  static constexpr u64 kVpnMask = R::mask | VPN2::mask;  // the parts taken from a faulting address
  static constexpr u64 kWriteMask = kVpnMask | ASID::mask;
};
static_assert(EntryHi::kWriteMask == 0xC00000FFFFFFE0FFull);
static_assert(EntryLo::kTlbMask == 0x03FFFFFF);

// Virtual address fields (5.2, Figures 5-2 / 5-3 and 5-7).
struct VAddr {
  using VPN2 = Bits<13, 27, u64>;       // virtual page pair number (bits 39:13)
  using Region = Bits<62, 2, u64>;      // 0 user, 1 supervisor, 2 xkphys, 3 kernel
  using XkphysCca = Bits<59, 3, u64>;   // cache algorithm of an xkphys address
  using XkphysHole = Bits<32, 27, u64>; // must be zero in xkphys (else address error)
};

// 6.3.1 Context, 6.3.9 XContext.
struct Context {
  using BadVPN2 = Bits<4, 19, u64>;
  using PTEBase = Bits<23, 41, u64>;
};
struct XContext {
  using BadVPN2 = Bits<4, 27, u64>;
  using R = Bits<31, 2, u64>;
  using PTEBase = Bits<33, 31, u64>;
};

// 6.3.8 WatchLo / WatchHi.
struct WatchLo {
  using W = Bit<0>;
  using R = Bit<1>;
  using PAddr0 = Bits<3, 29>;
  static constexpr u32 kWriteMask = 0xFFFFFFFB;
};

// 6.3.8 WatchHi: physical address bits 35:32. 6.3.10 Parity Error: 8 diagnostic bits.
struct WatchHi {
  using PAddr1 = Bits<0, 4>;
};
struct ParityErrorReg {
  using Diagnostic = Bits<0, 8>;
};

// 5.4.7 LLAddr: physical address bits 31:4 of the last LL, in bits 27:0.
struct LLAddrReg {
  using FromPAddr = Bits<4, 28>;  // the field of the physical address that LLAddr holds
};

// 5.4.8 TagLo.
struct TagLo {
  using PState = Bits<6, 2>;
  using PTagLo = Bits<8, 20>;
  static constexpr u32 kWriteMask = 0x0FFFFFC0;
};

// Index register (5.4.1).
struct IndexReg {
  using Index = Bits<0, 6>;
  using P = Bit<31>;
  static constexpr u32 kWriteMask = 0x8000003F;
};

}  // namespace ultra::cop0
