// VR4300 floating-point unit (COP1).
//
// Host IEEE-754 arithmetic is used with explicit rounding-mode control (docs/SPEC.md coding
// rule 7); VR4300-specific behaviour is applied around it:
//  - denormal operands and MIPS-quiet NaN operands (fraction MSB clear) raise the
//    Unimplemented Operation exception (VR4300 manual 8.2.7);
//  - MIPS-signalling NaN operands (fraction MSB set) are Invalid Operation;
//  - denormal/underflowing results raise Unimplemented unless FS=1 and the Underflow and
//    Inexact enables are clear, in which case they flush per Table 7-2;
//  - conversions to integer that overflow, or whose source is infinite or NaN, raise
//    Unimplemented (8.2.7 Restrictions);
//  - generated NaNs are 0x7FBF_FFFF / 0x7FF7_FFFF_FFFF_FFFF;
//  - FCR0 reads 0xA00, FCR31 has write mask 0x0183_FFFF.
// The last four rules come from n64-systemtest (src/tests/cop1/mod.rs, "Lessons learned"),
// whose expectations are hardware-measured. docs/hardware/cpu.md has the full list.

#include <cfenv>
#include <cmath>
#include <cstring>
#include <limits>

#include "cpu/decode.hpp"
#include "cpu/timing.hpp"
#include "cpu/vr4300.hpp"

// Clang honours the standard pragma; GCC has no support for it and relies on -frounding-math
// and -fsignaling-nans (src/CMakeLists.txt).
#if defined(__clang__)
#pragma STDC FENV_ACCESS ON
#endif

namespace ultra {

using namespace cop0;
using namespace decode;

namespace {

// FCR31 fields (VR4300 manual 7.2.4, Figure 7-2).
struct Fcsr {
  using RM = Bits<0, 2>;
  using Flags = Bits<2, 5>;
  using Enables = Bits<7, 5>;
  using Cause = Bits<12, 6>;
  using C = Bit<23>;
  using FS = Bit<24>;
};
// Flag/enable/cause bit order: I=0, U=1, O=2, Z=3, V=4, (cause only) E=5.
enum : u32 { kI = 1, kU = 2, kO = 4, kZ = 8, kV = 16, kE = 32 };

// FCR31 write mask: FS, C, cause, enables, flags, RM. Source: n64-systemtest CFC1CTC1_31.
constexpr u32 kFcsrWriteMask = 0x0183'FFFF;
// FCR0 as read on hardware. Source: n64-systemtest CFC1CTC1_0 ("CFC1 0 returns 0xA00").
constexpr u32 kFcr0Value = 0x0000'0A00;

constexpr u32 kDefaultNaN32 = 0x7FBF'FFFF;
constexpr u64 kDefaultNaN64 = 0x7FF7'FFFF'FFFF'FFFFull;

enum : u32 { FMT_S = 16, FMT_D = 17, FMT_W = 20, FMT_L = 21 };

struct F32 {
  using bits = u32;
  using host = float;
  static constexpr u32 kExpMask = 0x7F80'0000;
  static constexpr u32 kFracMask = 0x007F'FFFF;
  static constexpr u32 kQuietBit = 0x0040'0000;
  static constexpr u32 kSign = 0x8000'0000;
  static constexpr u32 kDefaultNaN = kDefaultNaN32;
  static constexpr u32 kMinNormal = 0x0080'0000;
};
struct F64 {
  using bits = u64;
  using host = double;
  static constexpr u64 kExpMask = 0x7FF0'0000'0000'0000ull;
  static constexpr u64 kFracMask = 0x000F'FFFF'FFFF'FFFFull;
  static constexpr u64 kQuietBit = 0x0008'0000'0000'0000ull;
  static constexpr u64 kSign = 0x8000'0000'0000'0000ull;
  static constexpr u64 kDefaultNaN = kDefaultNaN64;
  static constexpr u64 kMinNormal = 0x0010'0000'0000'0000ull;
};

template <typename F>
bool is_nan(typename F::bits b) {
  return (b & F::kExpMask) == F::kExpMask && (b & F::kFracMask) != 0;
}
template <typename F>
bool is_denormal(typename F::bits b) {
  return (b & F::kExpMask) == 0 && (b & F::kFracMask) != 0;
}
template <typename F>
bool is_inf(typename F::bits b) {
  return (b & F::kExpMask) == F::kExpMask && (b & F::kFracMask) == 0;
}
template <typename F>
bool is_zero(typename F::bits b) {
  return (b & (F::kExpMask | F::kFracMask)) == 0;
}
// Normal number with an all-zero fraction: a power of two.
template <typename F>
bool is_pow2(typename F::bits b) {
  const auto e = b & F::kExpMask;
  return e != 0 && e != F::kExpMask && (b & F::kFracMask) == 0;
}

// Early completion of multicycle operations: they finish in the second cycle when the operand
// check already determines the result (VR4300 manual 7.5, "Floating-point Add/Subtract",
// "Multiply", "Divide/Square Root", "Convert").
enum class Early : u8 { None, AddSub, Mul, Div, Sqrt };

template <typename F>
bool completes_early(Early k, typename F::bits a, typename F::bits b) {
  const bool za = is_zero<F>(a), zb = is_zero<F>(b), ia = is_inf<F>(a), ib = is_inf<F>(b);
  switch (k) {
    case Early::None: return false;
    case Early::AddSub: return za || zb || ia || ib;
    case Early::Mul: return za || zb || ia || ib || is_pow2<F>(a) || is_pow2<F>(b);
    case Early::Div: return za || zb || ia || ib;
    case Early::Sqrt: return za || ia;
  }
  return false;
}

template <typename F>
typename F::host to_host(typename F::bits b) {
  typename F::host h;
  std::memcpy(&h, &b, sizeof h);
  return h;
}
template <typename F>
typename F::bits from_host(typename F::host h) {
  typename F::bits b;
  std::memcpy(&b, &h, sizeof b);
  return b;
}

// Operand classification for arithmetic: 0 = ok, kE = unimplemented, kV = invalid.
template <typename F>
u32 classify_operand(typename F::bits b) {
  if (is_denormal<F>(b)) return kE;
  if (is_nan<F>(b)) return (b & F::kQuietBit) ? kV : kE;
  return 0;
}

int host_rounding(u32 rm) {
  switch (rm) {
    case 0: return FE_TONEAREST;
    case 1: return FE_TOWARDZERO;
    case 2: return FE_UPWARD;
    default: return FE_DOWNWARD;
  }
}

// Compiler barrier for host floating-point operations. GCC does not implement FENV_ACCESS and
// moves FP arithmetic across fesetround/feclearexcept/fetestexcept calls (an ADD.D was
// observed after the flag read); forcing the value through memory in a volatile asm pins it
// between those calls on every compiler.
template <typename T>
void fp_barrier(T& v) noexcept {
  asm volatile("" : "+m"(v));
}

u32 host_flags() {
  int e = std::fetestexcept(FE_ALL_EXCEPT);
  u32 r = 0;
  if (e & FE_INEXACT) r |= kI;
  if (e & FE_UNDERFLOW) r |= kU;
  if (e & FE_OVERFLOW) r |= kO;
  if (e & FE_DIVBYZERO) r |= kZ;
  if (e & FE_INVALID) r |= kV;
  return r;
}

class RoundingScope {
 public:
  explicit RoundingScope(u32 rm) : saved_(std::fegetround()) {
    std::fesetround(host_rounding(rm));
    std::feclearexcept(FE_ALL_EXCEPT);
  }
  ~RoundingScope() { std::fesetround(saved_); }
  RoundingScope(const RoundingScope&) = delete;
  RoundingScope& operator=(const RoundingScope&) = delete;

 private:
  int saved_;
};

}  // namespace

// ---- register file -----------------------------------------------------------------------------
// With Status.FR = 0 there are 16 64-bit registers; single-precision accesses to an odd
// register use the upper half of the even register below it, and double accesses use the
// even register (VR4300 manual 7.2.1-7.2.2).

u64 Vr4300::fpr_get_s(u32 r) const noexcept {
  if (Status::FR::test(status_)) return lo32(fpr_[r]);
  return (r & 1) ? hi32(fpr_[r & ~1u]) : lo32(fpr_[r]);
}
u64 Vr4300::fpr_get_d(u32 r) const noexcept {
  return Status::FR::test(status_) ? fpr_[r] : fpr_[r & ~1u];
}
void Vr4300::fpr_set_s(u32 r, u32 v) noexcept {
  if (Status::FR::test(status_)) {
    fpr_[r] = (fpr_[r] & 0xFFFF'FFFF'0000'0000ull) | v;
  } else if (r & 1) {
    fpr_[r & ~1u] = (fpr_[r & ~1u] & 0xFFFF'FFFFull) | (u64{v} << 32);
  } else {
    fpr_[r] = (fpr_[r] & 0xFFFF'FFFF'0000'0000ull) | v;
  }
}
void Vr4300::fpr_set_d(u32 r, u64 v) noexcept {
  if (Status::FR::test(status_)) {
    fpr_[r] = v;
  } else {
    fpr_[r & ~1u] = v;
  }
}

u32 Vr4300::cfc1(u32 r) const {
  if (r == 0) return kFcr0Value;
  if (r == 31) return fcr31_;
  return cop1_latch_;  // 1..30: not implemented; last CTC1 value (Guess: ledger "cop1-fcr-unimplemented")
}

void Vr4300::ctc1(u32 r, u32 v) {
  cop1_latch_ = v;
  if (r != 31) return;
  fcr31_ = v & kFcsrWriteMask;
  // Cause bits with matching enables (or E) fire immediately (n64-systemtest
  // FireExceptionViaCTC1 / FireUnimplementedExceptionViaCTC1).
  const u32 cause = Fcsr::Cause::get(fcr31_);
  if ((cause & kE) || (cause & Fcsr::Enables::get(fcr31_) & 0x1F)) raise(ExcCode::FPE, next_instr_cop());
}

u32 Vr4300::next_instr_cop() {
  // The exception is taken with the next instruction already fetched and decoded, and Cause.CE
  // takes its coprocessor number when it is a COPz instruction, else 0 (n64-systemtest
  // FireExceptionViaCTC1FollowedByMFC1 / ...MFC2; docs/hardware/cpu.md). pc_ is the next
  // fetch address (the branch target when CTC1 is in a delay slot). A fault on that fetch is
  // never taken, since the older CTC1's exception wins. Other cases: ledger "cause-ce-next-instr".
  // The FPE is a DC-stage exception, so that instruction is the one RF fetched (ADR 0005).
  dc_stage();
  if (!fetch_latch_.valid || !fetch_latch_.ok || fetch_latch_.vaddr != pc_) return 0;
  const u32 next = fetch_latch_.instr;
  return is_copz(next) ? cop_num(next) : 0;
}

bool Vr4300::fpu_check_usable() {
  if (!cop_usable(1)) {
    raise(ExcCode::CpU, 1);
    return false;
  }
  return true;
}

void Vr4300::fpu_execute(u32 instr) {
  if (!fpu_check_usable()) return;
  const u32 op = opcode(instr);

  // FPU result bypass: an FPU instruction that reads the previous computational result waits
  // one cycle for the DC->EX bypass (VR4300 manual 7.5.6).
  auto bypass_check = [&](u32 a, u32 b) {
    if (fpu_result_reg_ >= 0 &&
        (static_cast<u32>(fpu_result_reg_) == a || static_cast<u32>(fpu_result_reg_) == b))
      cycles_ += timing::kFpuBypassStall;
  };

  // ---- loads / stores (opcodes LWC1, LDC1, SWC1, SDC1) ----
  if (op != 0x11) {
    const u64 vaddr = gpr_[rs(instr)] + simm(instr);
    const u32 r = ft(instr);
    switch (op) {
      case 0x31: {  // LWC1
        u32 v;
        if (!load(vaddr, v)) return;
        fpr_set_s(r, v);
        cur_load_kind_ = LoadTarget::Fpr;
        cur_load_reg_ = r;
        return;
      }
      case 0x35: {  // LDC1
        u64 v;
        if (!load(vaddr, v)) return;
        fpr_set_d(r, v);
        cur_load_kind_ = LoadTarget::Fpr;
        cur_load_reg_ = r;
        return;
      }
      case 0x39: store<u32>(vaddr, fpr_get_s(r)); return;  // SWC1
      case 0x3D: store<u64>(vaddr, fpr_get_d(r)); return;  // SDC1
      default: raise(ExcCode::RI); return;
    }
  }

  const u32 f = fmt(instr);
  switch (f) {
    case 0x00:  // MFC1
      if (rt(instr)) gpr_[rt(instr)] = sext32(static_cast<u32>(fpr_get_s(fs(instr))));
      return;
    case 0x01:  // DMFC1
      if (!ops64()) return raise(ExcCode::RI);
      if (rt(instr)) gpr_[rt(instr)] = fpr_get_d(fs(instr));
      return;
    case 0x02:  // CFC1
      if (rt(instr)) gpr_[rt(instr)] = sext32(cfc1(fs(instr)));
      return;
    case 0x04: fpr_set_s(fs(instr), lo32(gpr_[rt(instr)])); return;  // MTC1
    case 0x05:                                                        // DMTC1
      if (!ops64()) return raise(ExcCode::RI);
      fpr_set_d(fs(instr), gpr_[rt(instr)]);
      return;
    case 0x06: ctc1(fs(instr), lo32(gpr_[rt(instr)])); return;  // CTC1
    case 0x08: {                                                // BC1
      const bool c = Fcsr::C::test(fcr31_);
      const bool want = (rt(instr) & 1) != 0;
      const bool likely = (rt(instr) & 2) != 0;
      const bool taken = c == want;
      if (taken) {
        branch_pending_ = true;
        npc_ = cur_pc_ + 4 + (simm(instr) << 2);
      } else if (likely) {
        pc_ = npc_;
        npc_ += 4;
        kill_slot_ = true;
      } else {
        branch_pending_ = true;
      }
      return;
    }
    case FMT_S:
    case FMT_D:
    case FMT_W:
    case FMT_L: break;
    default:
      // Reserved fmt: unimplemented operation (8.2.7).
      fcr31_ = Fcsr::Cause::set(fcr31_, kE);
      raise(ExcCode::FPE);
      return;
  }

  // ---- computational ----
  const u32 fn = funct(instr);
  const u32 rd_ = fd(instr);
  const u32 rs_ = fs(instr);
  const u32 rt_ = ft(instr);
  bypass_check(rs_, rt_);

  // MOV.fmt does not touch the cause bits; every other operation clears them first
  // (n64-systemtest cop1 "Lessons learned").
  if (fn == 0x06 && (f == FMT_S || f == FMT_D)) {
    // MOV.S and MOV.D both copy all 64 bits (n64-systemtest "Upper bits of 32 bit operation").
    fpu_dst(rd_, fpu_src(rs_));
    return;
  }
  fcr31_ = Fcsr::Cause::set(fcr31_, 0);

  const u32 rm = Fcsr::RM::get(fcr31_);
  const u32 enables = Fcsr::Enables::get(fcr31_);
  const bool fs_flush = Fcsr::FS::test(fcr31_);

  auto unimplemented = [&]() {
    fcr31_ = Fcsr::Cause::set(fcr31_, kE);
    raise(ExcCode::FPE);
  };
  // Commit cause bits; returns false (and raises) if an enabled exception fires.
  auto commit = [&](u32 cause) {
    fcr31_ = Fcsr::Cause::set(fcr31_, cause);
    if (cause & enables) {
      raise(ExcCode::FPE);
      return false;
    }
    fcr31_ = Fcsr::Flags::set(fcr31_, Fcsr::Flags::get(fcr31_) | cause);
    return true;
  };
  auto finish_cycles = [&](u64 n) {
    mci(n - 1);
    cur_fpu_result_reg_ = static_cast<s32>(rd_);
  };

  // Result post-processing shared by arithmetic ops: underflow handling and NaN replacement.
  auto arith_result = [&]<typename F>(typename F::bits r, u32 host) -> bool {
    u32 cause = host & (kI | kO | kZ | kV);
    if (host & kV) r = F::kDefaultNaN;
    const bool tiny = is_denormal<F>(r) || (host & kU);
    if (tiny) {
      if (!fs_flush || (enables & (kU | kI))) {
        unimplemented();
        return false;
      }
      // Flush per Table 7-2.
      const bool neg = (r & F::kSign) != 0;
      typename F::bits flushed = neg ? F::kSign : 0;
      if (rm == 2 && !neg) flushed = F::kMinNormal;
      if (rm == 3 && neg) flushed = F::kSign | F::kMinNormal;
      r = flushed;
      cause |= kU | kI;
    }
    if (!commit(cause)) return false;
    fpu_dst(rd_, r);
    return true;
  };

  auto binary = [&]<typename F>(auto opfn, u64 cycles, Early early) {
    using B = typename F::bits;
    const B a = static_cast<B>(fpu_src(rs_));
    const B b = static_cast<B>(fpu_src_ft(rt_));
    const u32 ca = classify_operand<F>(a), cb = classify_operand<F>(b);
    if ((ca | cb) & kE) return unimplemented();
    if ((ca | cb) & kV) {
      if (!commit(kV)) return;
      fpu_dst(rd_, F::kDefaultNaN);
      
      finish_cycles(timing::kFpuTrivial);
      return;
    }
    B r;
    u32 host;
    {
      RoundingScope scope(rm);
      typename F::host x = to_host<F>(a), y = to_host<F>(b);
      fp_barrier(x);
      fp_barrier(y);
      typename F::host z = opfn(x, y);
      fp_barrier(z);
      r = from_host<F>(z);
      host = host_flags();
    }
    if (completes_early<F>(early, a, b)) cycles = std::min(cycles, timing::kFpuTrivial);
    if (arith_result.template operator()<F>(r, host)) finish_cycles(cycles);
  };

  auto unary = [&]<typename F>(auto opfn, u64 cycles, Early early) {
    using B = typename F::bits;
    const B a = static_cast<B>(fpu_src(rs_));
    const u32 ca = classify_operand<F>(a);
    if (ca & kE) return unimplemented();
    if (ca & kV) {
      if (!commit(kV)) return;
      fpu_dst(rd_, F::kDefaultNaN);
      
      finish_cycles(timing::kFpuTrivial);
      return;
    }
    B r;
    u32 host;
    {
      RoundingScope scope(rm);
      typename F::host x = to_host<F>(a);
      fp_barrier(x);
      typename F::host z = opfn(x);
      fp_barrier(z);
      r = from_host<F>(z);
      host = host_flags();
    }
    if (completes_early<F>(early, a, a)) cycles = std::min(cycles, timing::kFpuTrivial);
    if (arith_result.template operator()<F>(r, host)) finish_cycles(cycles);
  };

  // Conversion to integer with explicit rounding mode.
  auto to_int = [&]<typename F, typename I>(u32 mode, u64 cycles) {
    using B = typename F::bits;
    const B a = static_cast<B>(fpu_src(rs_));
    if (is_denormal<F>(a) || is_nan<F>(a) || is_inf<F>(a)) return unimplemented();
    typename F::host h = to_host<F>(a);
    typename F::host rounded;
    u32 host;
    {
      RoundingScope scope(mode);
      fp_barrier(h);
      rounded = std::nearbyint(h);
      fp_barrier(rounded);
      host = (rounded != h) ? u32{kI} : 0u;
    }
    // Out of range for the destination integer: unimplemented (8.2.7 Restrictions).
    // W: the full s32 range. L: the VR4300 converts through a 53-bit datapath and raises
    // Unimplemented for |x| >= 2^53 (n64-systemtest "COP1: CVT.L, ROUND.L, ...").
    constexpr typename F::host lim =
        sizeof(I) == 4 ? typename F::host(2147483648.0) : typename F::host(9007199254740992.0);
    const bool in_range = sizeof(I) == 4 ? (rounded >= -lim && rounded < lim) : (rounded > -lim && rounded < lim);
    if (!in_range) return unimplemented();
    const I v = static_cast<I>(rounded);
    if (!commit(host)) return;
    if constexpr (sizeof(I) == 4)
      fpu_dst(rd_, static_cast<u32>(v));
    else
      fpu_dst(rd_, static_cast<u64>(v));
    finish_cycles(cycles);
  };

  auto compare = [&]<typename F>(u32 cond) {
    using B = typename F::bits;
    const B a = static_cast<B>(fpu_src(rs_));
    const B b = static_cast<B>(fpu_src_ft(rt_));
    const bool unordered = is_nan<F>(a) || is_nan<F>(b);
    u32 cause = 0;
    // Signalling compares (cond bit 3) raise Invalid on any NaN; quiet compares on a
    // MIPS-signalling NaN (fraction MSB set) operand.
    if (unordered) {
      const bool snan = (is_nan<F>(a) && (a & F::kQuietBit)) || (is_nan<F>(b) && (b & F::kQuietBit));
      if ((cond & 8) || snan) cause |= kV;
    }
    bool result;
    if (unordered) {
      result = (cond & 1) != 0;
    } else {
      // Compare as host values; denormals compare by value (compares are exempt from E).
      const typename F::host ha = to_host<F>(a), hb = to_host<F>(b);
      result = ((cond & 2) && ha == hb) || ((cond & 4) && ha < hb);
    }
    if (!commit(cause)) return;
    fcr31_ = Fcsr::C::set(fcr31_, result ? 1 : 0);
    finish_cycles(timing::kFpuSimple);
    cur_fpu_result_reg_ = -1;
  };

  // Float-to-float conversion.
  auto convert = [&]<typename From, typename To>(u64 cycles) {
    using B = typename From::bits;
    const B a = static_cast<B>(fpu_src(rs_));
    const u32 ca = classify_operand<From>(a);
    if (ca & kE) return unimplemented();
    if (ca & kV) {
      if (!commit(kV)) return;
      fpu_dst(rd_, To::kDefaultNaN);
      
      finish_cycles(timing::kFpuTrivial);
      return;
    }
    typename To::host r;
    u32 host;
    {
      RoundingScope scope(rm);
      typename From::host x = to_host<From>(a);
      fp_barrier(x);
      r = static_cast<typename To::host>(x);
      fp_barrier(r);
      host = host_flags();
    }
    // Trivial conversions (zero, infinity) complete in the second cycle (7.5, "Convert").
    if (is_zero<From>(a) || is_inf<From>(a)) cycles = std::min(cycles, timing::kFpuTrivial);
    if (arith_result.template operator()<To>(from_host<To>(r), host)) finish_cycles(cycles);
  };

  auto cvt_from_int = [&]<typename F>(u64 cycles) {
    // fmt is W or L; destination format F.
    typename F::host h;
    u32 host;
    {
      RoundingScope scope(rm);
      if (f == FMT_W) {
        s32 iv = as_s32(lo32(fpu_src(rs_)));
        fp_barrier(iv);
        h = static_cast<typename F::host>(iv);
      } else {
        const u64 v = fpu_src(rs_);
        // CVT from L requires the value to fit 53 (D) / 24 (S) bits plus sign in hardware?
        // The VR4300 raises Unimplemented for L sources outside +-2^55 (Guess: ledger "cop1-cvt-from-l").
        if (as_s64(v) >= (s64{1} << 55) || as_s64(v) < -(s64{1} << 55)) return unimplemented();
        s64 iv = as_s64(v);
        fp_barrier(iv);
        h = static_cast<typename F::host>(iv);
      }
      fp_barrier(h);
      host = host_flags() & kI;
    }
    if (!commit(host)) return;
    fpu_dst(rd_, from_host<F>(h));
    // Converting integer zero is a trivial case (7.5, "Convert"); krom CP1TIMINGNTSC's hardware
    // counts show CVT.S/D.W/L of 0 costing no more than a 1-cycle instruction.
    if (h == 0) cycles = std::min(cycles, timing::kFpuTrivial);
    finish_cycles(cycles);
  };

  if (f == FMT_S || f == FMT_D) {
    const bool is_s = f == FMT_S;
    switch (fn) {
      case 0x00:  // ADD
        if (is_s) binary.template operator()<F32>([](float x, float y) { return x + y; }, timing::kFpuAddSub, Early::AddSub);
        else binary.template operator()<F64>([](double x, double y) { return x + y; }, timing::kFpuAddSub, Early::AddSub);
        return;
      case 0x01:  // SUB
        if (is_s) binary.template operator()<F32>([](float x, float y) { return x - y; }, timing::kFpuAddSub, Early::AddSub);
        else binary.template operator()<F64>([](double x, double y) { return x - y; }, timing::kFpuAddSub, Early::AddSub);
        return;
      case 0x02:  // MUL
        if (is_s) binary.template operator()<F32>([](float x, float y) { return x * y; }, timing::kFpuMulS, Early::Mul);
        else binary.template operator()<F64>([](double x, double y) { return x * y; }, timing::kFpuMulD, Early::Mul);
        return;
      case 0x03:  // DIV
        if (is_s) binary.template operator()<F32>([](float x, float y) { return x / y; }, timing::kFpuDivSqrtS, Early::Div);
        else binary.template operator()<F64>([](double x, double y) { return x / y; }, timing::kFpuDivSqrtD, Early::Div);
        return;
      case 0x04:  // SQRT
        if (is_s) unary.template operator()<F32>([](float x) { return std::sqrt(x); }, timing::kFpuDivSqrtS, Early::Sqrt);
        else unary.template operator()<F64>([](double x) { return std::sqrt(x); }, timing::kFpuDivSqrtD, Early::Sqrt);
        return;
      case 0x05:  // ABS
        if (is_s) unary.template operator()<F32>([](float x) { return std::fabs(x); }, timing::kFpuSimple, Early::None);
        else unary.template operator()<F64>([](double x) { return std::fabs(x); }, timing::kFpuSimple, Early::None);
        return;
      case 0x07:  // NEG
        if (is_s) unary.template operator()<F32>([](float x) { return -x; }, timing::kFpuSimple, Early::None);
        else unary.template operator()<F64>([](double x) { return -x; }, timing::kFpuSimple, Early::None);
        return;
      case 0x08:  // ROUND.L
      case 0x09:  // TRUNC.L
      case 0x0A:  // CEIL.L
      case 0x0B: {  // FLOOR.L
        static constexpr u32 kMode[4] = {0, 1, 2, 3};
        if (is_s) to_int.template operator()<F32, s64>(kMode[fn - 0x08], timing::kFpuRoundTruncCeilFloor);
        else to_int.template operator()<F64, s64>(kMode[fn - 0x08], timing::kFpuRoundTruncCeilFloor);
        return;
      }
      case 0x0C:  // ROUND.W
      case 0x0D:  // TRUNC.W
      case 0x0E:  // CEIL.W
      case 0x0F: {  // FLOOR.W
        static constexpr u32 kMode[4] = {0, 1, 2, 3};
        if (is_s) to_int.template operator()<F32, s32>(kMode[fn - 0x0C], timing::kFpuRoundTruncCeilFloor);
        else to_int.template operator()<F64, s32>(kMode[fn - 0x0C], timing::kFpuRoundTruncCeilFloor);
        return;
      }
      case 0x20:  // CVT.S
        if (is_s) return unimplemented();  // CVT.S.S is an invalid format (8.2.7)
        convert.template operator()<F64, F32>(timing::kFpuCvtSFromD);
        return;
      case 0x21:  // CVT.D
        if (!is_s) return unimplemented();  // CVT.D.D
        convert.template operator()<F32, F64>(timing::kFpuCvtDFromS);
        return;
      case 0x24:  // CVT.W
        if (is_s) to_int.template operator()<F32, s32>(rm, timing::kFpuCvtToInt);
        else to_int.template operator()<F64, s32>(rm, timing::kFpuCvtToInt);
        return;
      case 0x25:  // CVT.L
        if (is_s) to_int.template operator()<F32, s64>(rm, timing::kFpuCvtToInt);
        else to_int.template operator()<F64, s64>(rm, timing::kFpuCvtToInt);
        return;
      default:
        if (fn >= 0x30) {
          if (is_s) compare.template operator()<F32>(fn & 15);
          else compare.template operator()<F64>(fn & 15);
          return;
        }
        return unimplemented();
    }
  }

  // fmt W / L: only CVT.S and CVT.D are defined.
  switch (fn) {
    case 0x20: cvt_from_int.template operator()<F32>(timing::kFpuCvtFromInt); return;
    case 0x21: cvt_from_int.template operator()<F64>(timing::kFpuCvtFromInt); return;
    default: unimplemented(); return;
  }
}

}  // namespace ultra
