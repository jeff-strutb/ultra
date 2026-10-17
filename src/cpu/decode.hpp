#pragma once
// Instruction field accessors (VR4300 manual 16.2 Instruction Formats).

#include "common/types.hpp"

namespace ultra::decode {

using Op = Bits<26, 6>;
using Rs = Bits<21, 5>;
using Rt = Bits<16, 5>;
using Rd = Bits<11, 5>;
using Sa = Bits<6, 5>;
using Funct = Bits<0, 6>;
using Imm = Bits<0, 16>;
using Target = Bits<0, 26>;
// FPU register fields: fmt in rs, ft in rt, fs in rd, fd in sa.
using Fmt = Rs;
using Ft = Rt;
using Fs = Rd;
using Fd = Sa;

constexpr u32 opcode(u32 i) noexcept { return Op::get(i); }
constexpr u32 rs(u32 i) noexcept { return Rs::get(i); }
constexpr u32 rt(u32 i) noexcept { return Rt::get(i); }
constexpr u32 rd(u32 i) noexcept { return Rd::get(i); }
constexpr u32 sa(u32 i) noexcept { return Sa::get(i); }
constexpr u32 funct(u32 i) noexcept { return Funct::get(i); }
constexpr u16 imm(u32 i) noexcept { return static_cast<u16>(Imm::get(i)); }
constexpr u64 simm(u32 i) noexcept { return sext16(imm(i)); }
constexpr u32 target(u32 i) noexcept { return Target::get(i); }
constexpr u32 fmt(u32 i) noexcept { return Fmt::get(i); }
constexpr u32 ft(u32 i) noexcept { return Ft::get(i); }
constexpr u32 fs(u32 i) noexcept { return Fs::get(i); }
constexpr u32 fd(u32 i) noexcept { return Fd::get(i); }

}  // namespace ultra::decode
