#pragma once
// Fixed-width integer aliases and bit helpers shared by every module.
// Signed overflow is undefined behaviour in C++; all wrapping arithmetic on emulated
// registers goes through the wrap_ helpers, which operate on unsigned types.

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ultra {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;
using usize = std::size_t;

// ---- wrapping arithmetic -------------------------------------------------------------

template <std::unsigned_integral T>
constexpr T wrap_add(T a, T b) noexcept { return static_cast<T>(a + b); }
template <std::unsigned_integral T>
constexpr T wrap_sub(T a, T b) noexcept { return static_cast<T>(a - b); }
template <std::unsigned_integral T>
constexpr T wrap_mul(T a, T b) noexcept { return static_cast<T>(a * b); }

// Signed views of unsigned registers (two's complement, well defined since C++20).
constexpr s32 as_s32(u32 v) noexcept { return static_cast<s32>(v); }
constexpr s64 as_s64(u64 v) noexcept { return static_cast<s64>(v); }
constexpr u32 as_u32(s32 v) noexcept { return static_cast<u32>(v); }
constexpr u64 as_u64(s64 v) noexcept { return static_cast<u64>(v); }

// Sign-extension helpers.
constexpr u64 sext32(u32 v) noexcept { return static_cast<u64>(static_cast<s64>(static_cast<s32>(v))); }
constexpr u64 sext16(u16 v) noexcept { return static_cast<u64>(static_cast<s64>(static_cast<s16>(v))); }
constexpr u64 sext8(u8 v) noexcept { return static_cast<u64>(static_cast<s64>(static_cast<s8>(v))); }
constexpr u32 sext16_32(u16 v) noexcept { return static_cast<u32>(static_cast<s32>(static_cast<s16>(v))); }

template <unsigned Bits>
constexpr u64 sext_bits(u64 v) noexcept {
  static_assert(Bits > 0 && Bits <= 64);
  if constexpr (Bits == 64) {
    return v;
  } else {
    constexpr u64 sign = u64{1} << (Bits - 1);
    constexpr u64 mask = (u64{1} << Bits) - 1;
    v &= mask;
    return (v ^ sign) - sign;
  }
}

constexpr u32 lo32(u64 v) noexcept { return static_cast<u32>(v); }
constexpr u32 hi32(u64 v) noexcept { return static_cast<u32>(v >> 32); }
constexpr u64 make64(u32 hi, u32 lo) noexcept { return (u64{hi} << 32) | lo; }
constexpr u16 lo16(u32 v) noexcept { return static_cast<u16>(v); }
constexpr u16 hi16(u32 v) noexcept { return static_cast<u16>(v >> 16); }
constexpr u32 make32(u16 hi, u16 lo) noexcept { return (u32{hi} << 16) | lo; }

// Full 128-bit products in ISO C++ (no __int128): returns the high 64 bits, `lo` gets the low.
constexpr u64 mul_u64_wide(u64 a, u64 b, u64& lo) noexcept {
  const u64 a0 = lo32(a), a1 = hi32(a), b0 = lo32(b), b1 = hi32(b);
  const u64 p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
  const u64 mid = (p00 >> 32) + lo32(p01) + lo32(p10);
  lo = (mid << 32) | lo32(p00);
  return p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}
// Signed: the unsigned product's high half minus each negative operand's partner.
constexpr u64 mul_s64_wide(u64 a, u64 b, u64& lo) noexcept {
  u64 hi = mul_u64_wide(a, b, lo);
  if (static_cast<s64>(a) < 0) hi -= b;
  if (static_cast<s64>(b) < 0) hi -= a;
  return hi;
}

// ---- big-endian memory access ----------------------------------------------------------
// Emulated memories are stored as byte arrays in N64 (big-endian) byte order so that byte
// address N in the array is byte address N on the bus.

inline u16 load_be16(const u8* p) noexcept {
  return static_cast<u16>((u16{p[0]} << 8) | u16{p[1]});
}
inline u32 load_be32(const u8* p) noexcept {
  return (u32{p[0]} << 24) | (u32{p[1]} << 16) | (u32{p[2]} << 8) | u32{p[3]};
}
inline u64 load_be64(const u8* p) noexcept { return make64(load_be32(p), load_be32(p + 4)); }
inline void store_be16(u8* p, u16 v) noexcept {
  p[0] = static_cast<u8>(v >> 8);
  p[1] = static_cast<u8>(v);
}
inline void store_be32(u8* p, u32 v) noexcept {
  p[0] = static_cast<u8>(v >> 24);
  p[1] = static_cast<u8>(v >> 16);
  p[2] = static_cast<u8>(v >> 8);
  p[3] = static_cast<u8>(v);
}
inline void store_be64(u8* p, u64 v) noexcept {
  store_be32(p, hi32(v));
  store_be32(p + 4, lo32(v));
}

// ---- register bit fields ---------------------------------------------------------------
// Bits<Lo, Width> names a field inside a register word. Register structs expose their
// fields as Bits aliases so call sites never shift or mask by hand.

template <unsigned Lo, unsigned Width, std::unsigned_integral T = u32>
struct Bits {
  static_assert(Width > 0 && Lo + Width <= sizeof(T) * 8);
  using value_type = T;
  static constexpr unsigned lo = Lo;
  static constexpr unsigned width = Width;
  static constexpr T field_mask = Width == sizeof(T) * 8 ? static_cast<T>(~T{0})
                                                         : static_cast<T>((T{1} << Width) - 1);
  static constexpr T mask = static_cast<T>(field_mask << Lo);

  static constexpr T get(T reg) noexcept { return static_cast<T>((reg >> Lo) & field_mask); }
  static constexpr bool test(T reg) noexcept { return (reg & mask) != 0; }
  static constexpr T set(T reg, T value) noexcept {
    return static_cast<T>((reg & ~mask) | ((value & field_mask) << Lo));
  }
  static constexpr T place(T value) noexcept { return static_cast<T>((value & field_mask) << Lo); }
};

template <unsigned N, std::unsigned_integral T = u32>
using Bit = Bits<N, 1, T>;

}  // namespace ultra
