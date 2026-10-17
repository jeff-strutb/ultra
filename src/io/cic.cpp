// Cartridge CIC model: variant table, IPL2 checksum, CIC detection and the 6105 challenge.
// Written from docs/hardware/cic-algorithms.md (a clean-room functional specification) and
// n64brew "PIF-NUS" / "CIC-NUS". Section numbers below refer to cic-algorithms.md.

#include "io/cic.hpp"

#include <bit>
#include <cstddef>

namespace ultra {

namespace {

// [n64brew] "PIF-NUS", IPL3 checksum algorithm table (IPL2 seed and 6-byte IPL2 checksum
// columns); region per [n64brew] "CIC-NUS" (7xxx parts are PAL). Same order as CicType.
constexpr std::array<CicInfo, 10> kCics{{
    {CicType::Cic6101, "6101", false, 0x3F, 0x45CC'73EE'317Aull},
    {CicType::Cic6102, "6102", false, 0x3F, 0xA536'C0F1'D859ull},
    {CicType::Cic6103, "6103", false, 0x78, 0x586F'D470'9867ull},
    {CicType::Cic6105, "6105", false, 0x91, 0x8618'A45B'C2D3ull},
    {CicType::Cic6106, "6106", false, 0x85, 0x2BBA'D4E6'EB74ull},
    {CicType::Cic7101, "7101", true, 0x3F, 0xA536'C0F1'D859ull},
    {CicType::Cic7102, "7102", true, 0x3F, 0x4416'0EC5'D9AFull},
    {CicType::Cic7103, "7103", true, 0x78, 0x586F'D470'9867ull},
    {CicType::Cic7105, "7105", true, 0x91, 0x8618'A45B'C2D3ull},
    {CicType::Cic7106, "7106", true, 0x85, 0x2BBA'D4E6'EB74ull},
}};

constexpr bool table_matches_enum() {
  for (std::size_t i = 0; i < kCics.size(); ++i) {
    if (static_cast<std::size_t>(kCics[i].type) != i) return false;
  }
  return true;
}
static_assert(table_matches_enum());

// IPL3 occupies cartridge ROM 0x40..0xFFF ([n64brew] "ROM Header"; spec 1.1).
constexpr std::size_t kIpl3RomOffset = 0x40;
constexpr std::size_t kIpl3Bytes = 0xFC0;
constexpr u32 kIpl3Words = kIpl3Bytes / 4;  // 1008 main-phase steps (spec 1.4)

constexpr u32 kSeedMultiplier = 0x6C07'8965;  // K, spec 1.1
constexpr u32 kWordBias = 5;                  // S_3's x + 5, spec 1.4
constexpr u32 kFinalStepIndex = 16;           // third argument of the last M, spec 1.5
constexpr u64 kChecksumHighMask = 0xFFFF;     // H keeps 16 bits, spec 1.5

constexpr u32 hi32(u64 v) { return static_cast<u32>(v >> 32); }
constexpr u32 lo32(u64 v) { return static_cast<u32>(v); }
constexpr u32 low5(u32 v) { return v & 31u; }
constexpr u32 high5(u32 v) { return v >> 27; }
constexpr u32 ror(u32 v, u32 r) { return std::rotr(v, static_cast<int>(r)); }
constexpr u32 rol(u32 v, u32 r) { return std::rotl(v, static_cast<int>(r)); }

// M(a, b, c), spec 1.2: hi - lo of a * (b, or c when b is zero); a when that difference is 0.
constexpr u32 mix(u32 a, u32 b, u32 c) {
  const u64 product = u64{a} * u64{b == 0 ? c : b};
  const u32 diff = hi32(product) - lo32(product);
  return diff == 0 ? a : diff;
}
static_assert(mix(3, 0, 7) == 0xFFFF'FFEBu);
static_assert(mix(0, 5, 1) == 0);
static_assert(mix(0xFFFF'FFFFu, 0xFFFF'FFFFu, 1) == 0xFFFF'FFFDu);

u32 ipl3_word(std::span<const u8> ipl3, u32 index) {
  const std::size_t at = std::size_t{index} * 4;
  return (u32{ipl3[at]} << 24) | (u32{ipl3[at + 1]} << 16) | (u32{ipl3[at + 2]} << 8) |
         u32{ipl3[at + 3]};
}

class Ipl2Checksum {
 public:
  Ipl2Checksum(u8 seed, u32 first_word) {
    acc_.fill((kSeedMultiplier * seed + 1u) ^ first_word);  // spec 1.3
  }

  // S_0..S_9: every step, including the last (spec 1.4). `cur` is x, `prev` is v.
  void absorb(u32 step, u32 cur, u32 prev) {
    const u32 rot_a = ror(cur, low5(prev));
    const u32 rot_b = rol(cur, high5(prev));
    const u32 old6 = acc_[6];

    acc_[0] += mix(u32{kIpl3Words - 1} - step, cur, step);
    acc_[1] = mix(acc_[1], cur, step);
    acc_[2] ^= cur;
    acc_[3] += mix(cur + kWordBias, kSeedMultiplier, step);
    acc_[4] += rot_a;
    acc_[5] += rot_b;
    acc_[6] = cur < old6 ? (acc_[3] + old6) ^ (cur + step) : (acc_[4] + cur) ^ old6;
    acc_[7] = mix(acc_[7], rol(cur, low5(prev)), step);
    acc_[8] = mix(acc_[8], ror(cur, high5(prev)), step);
    acc_[9] = prev < cur ? mix(acc_[9], cur, step) : acc_[9] + cur;
    last_rot_a_ = rot_a;
    last_rot_b_ = rot_b;
  }

  // S_10..S_15: every step but the last, after absorb() of the same step (S_12 reads the new
  // S_8). `next` is n.
  void look_ahead(u32 step, u32 cur, u32 next) {
    acc_[10] = mix(acc_[10] + cur, next, step);
    acc_[11] = mix(acc_[11] ^ cur, next, step);
    acc_[12] += acc_[8] ^ cur;
    acc_[13] += ror(cur, low5(cur)) + ror(next, low5(next));
    acc_[14] = mix(mix(acc_[14], last_rot_a_, step), ror(next, low5(cur)), step);
    acc_[15] = mix(mix(acc_[15], last_rot_b_, step), rol(next, high5(cur)), step);
  }

  // Spec 1.5.
  u64 finish() const {
    u32 sum = acc_[0];
    u32 below = acc_[0];
    u32 parity = acc_[0];
    u32 odd = acc_[0];
    for (u32 j = 0; j < acc_.size(); ++j) {
      const u32 y = acc_[j];
      sum += ror(y, low5(y));
      below = y < sum ? below + y : mix(below, y, j);
      const bool bits_equal = ((y >> 1) & 1u) == (y & 1u);
      parity = bits_equal ? parity + y : mix(parity, y, j);
      odd = (y & 1u) != 0 ? odd ^ y : mix(odd, y, j);
    }
    const u64 high = mix(sum, below, kFinalStepIndex) & kChecksumHighMask;
    return (high << 32) | (parity ^ odd);
  }

 private:
  std::array<u32, 16> acc_{};
  u32 last_rot_a_ = 0;
  u32 last_rot_b_ = 0;
};

}  // namespace

std::span<const CicInfo> cic_table() { return kCics; }

const CicInfo& cic_info(CicType t) { return kCics[static_cast<std::size_t>(t)]; }

u64 ipl2_checksum(std::span<const u8> ipl3, u8 seed) {
  const u32 first = ipl3_word(ipl3, 0);
  Ipl2Checksum sum(seed, first);
  u32 prev = first;  // step 1 treats word 0 as its own predecessor (spec 1.4)
  for (u32 step = 1; step <= kIpl3Words; ++step) {
    const u32 cur = ipl3_word(ipl3, step - 1);
    sum.absorb(step, cur, prev);
    if (step < kIpl3Words) sum.look_ahead(step, cur, ipl3_word(ipl3, step));
    prev = cur;
  }
  return sum.finish();
}

std::optional<CicType> detect_cic(std::span<const u8> rom, bool pal) {
  if (rom.size() < kIpl3RomOffset + kIpl3Bytes) return std::nullopt;
  const auto ipl3 = rom.subspan(kIpl3RomOffset, kIpl3Bytes);
  // Variants share seeds, so compute each distinct seed's checksum once.
  std::array<std::optional<u64>, 256> by_seed{};
  std::optional<CicType> other_region;
  for (const CicInfo& c : kCics) {
    auto& sum = by_seed[c.seed];
    if (!sum) sum = ipl2_checksum(ipl3, c.seed);
    if (*sum != c.ipl2_checksum) continue;
    if (c.pal == pal) return c.type;
    if (!other_region) other_region = c.type;
  }
  // 6101 has no PAL twin and 7102 no NTSC twin: accept the IPL3's only variant.
  return other_region;
}

// Spec 2.2: 4-bit accumulator and carry scanned over the challenge; each response nibble
// becomes the next accumulator value.
void cic6105_challenge(std::array<u8, 30>& nibbles) {
  constexpr u8 kStartAcc = 5;
  constexpr u8 kStartCarry = 1;
  constexpr u8 kEvenAddend = 8;
  constexpr u8 kBit1ClearAddend = 4;
  constexpr u8 kNoCarryAddend = 7;
  constexpr u8 kNibble = 0xF;

  u8 acc = kStartAcc;
  u8 carry = kStartCarry;
  for (u8& q : nibbles) {
    const u8 in = q & kNibble;
    u8 a = acc;
    if ((in & 1u) == 0) a = (a + kEvenAddend) & kNibble;
    if ((a & 2u) == 0) a = (a + kBit1ClearAddend) & kNibble;
    const u8 t = (a + in) & kNibble;
    const u8 u = (2 * t + (carry == 0 ? kNoCarryAddend : 0)) & kNibble;
    const unsigned z = unsigned{u} + t + carry;
    carry = z > kNibble ? 1 : 0;
    q = kNibble - (z & kNibble);
    acc = q;
  }
}

}  // namespace ultra
