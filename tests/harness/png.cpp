// Minimal PNG reader for reference screenshots: 8-bit grayscale, RGB, palette, gray+alpha
// and RGBA, non-interlaced. Includes a small DEFLATE decoder (RFC 1951) so the harness needs
// no third-party image library.

#include "png.hpp"

#include <array>
#include <cstdio>
#include <cstring>

namespace ultra::harness {

namespace {

class BitReader {
 public:
  BitReader(const u8* p, usize n) : p_(p), n_(n) {}
  u32 bits(u32 count) {
    u32 v = 0;
    for (u32 i = 0; i < count; ++i) {
      if (pos_ >= n_ * 8) throw std::runtime_error("inflate: truncated");
      v |= u32{(p_[pos_ >> 3] >> (pos_ & 7)) & 1u} << i;
      ++pos_;
    }
    return v;
  }
  void align() { pos_ = (pos_ + 7) & ~usize{7}; }
  usize byte_pos() const { return pos_ >> 3; }
  void skip_bytes(usize k) { pos_ += k * 8; }
  const u8* data() const { return p_; }
  usize size() const { return n_; }

 private:
  const u8* p_;
  usize n_;
  usize pos_ = 0;
};

struct Huffman {
  std::array<u16, 16> count{};
  std::array<u16, 320> symbol{};

  void build(const u8* lengths, u32 n) {
    count.fill(0);
    for (u32 i = 0; i < n; ++i) ++count[lengths[i]];
    count[0] = 0;
    std::array<u16, 16> offs{};
    for (u32 i = 1; i < 16; ++i) offs[i] = static_cast<u16>(offs[i - 1] + count[i - 1]);
    for (u32 i = 0; i < n; ++i)
      if (lengths[i]) symbol[offs[lengths[i]]++] = static_cast<u16>(i);
  }
  u32 decode(BitReader& br) const {
    int code = 0, first = 0, index = 0;
    for (u32 len = 1; len < 16; ++len) {
      code |= static_cast<int>(br.bits(1));
      const int c = count[len];
      if (code - c < first) return symbol[static_cast<usize>(index + (code - first))];
      index += c;
      first += c;
      first <<= 1;
      code <<= 1;
    }
    throw std::runtime_error("inflate: bad code");
  }
};

constexpr u16 kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                              35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr u8 kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                              3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr u16 kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                               193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                               6145, 8193, 12289, 16385, 24577};
constexpr u8 kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                               6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void inflate_block(BitReader& br, const Huffman& lit, const Huffman& dist, std::vector<u8>& out) {
  for (;;) {
    const u32 sym = lit.decode(br);
    if (sym < 256) {
      out.push_back(static_cast<u8>(sym));
    } else if (sym == 256) {
      return;
    } else {
      const u32 li = sym - 257;
      if (li >= 29) throw std::runtime_error("inflate: bad length");
      const u32 len = kLenBase[li] + br.bits(kLenExtra[li]);
      const u32 di = dist.decode(br);
      if (di >= 30) throw std::runtime_error("inflate: bad distance");
      const u32 d = kDistBase[di] + br.bits(kDistExtra[di]);
      if (d > out.size()) throw std::runtime_error("inflate: distance too far");
      const usize from = out.size() - d;
      for (u32 i = 0; i < len; ++i) out.push_back(out[from + i]);
    }
  }
}

std::vector<u8> zlib_inflate(const std::vector<u8>& in) {
  if (in.size() < 2) throw std::runtime_error("zlib: short");
  BitReader br(in.data() + 2, in.size() - 2);  // skip CMF/FLG
  std::vector<u8> out;
  for (;;) {
    const u32 final = br.bits(1);
    const u32 type = br.bits(2);
    if (type == 0) {
      br.align();
      const usize p = br.byte_pos();
      if (p + 4 > br.size()) throw std::runtime_error("inflate: stored header");
      const u32 len = br.data()[p] | (u32{br.data()[p + 1]} << 8);
      br.skip_bytes(4);
      const usize q = br.byte_pos();
      if (q + len > br.size()) throw std::runtime_error("inflate: stored data");
      out.insert(out.end(), br.data() + q, br.data() + q + len);
      br.skip_bytes(len);
    } else if (type == 1) {
      u8 l[288];
      for (u32 i = 0; i < 144; ++i) l[i] = 8;
      for (u32 i = 144; i < 256; ++i) l[i] = 9;
      for (u32 i = 256; i < 280; ++i) l[i] = 7;
      for (u32 i = 280; i < 288; ++i) l[i] = 8;
      Huffman lit, dist;
      lit.build(l, 288);
      u8 d[30];
      std::memset(d, 5, sizeof d);
      dist.build(d, 30);
      inflate_block(br, lit, dist, out);
    } else if (type == 2) {
      const u32 hlit = br.bits(5) + 257, hdist = br.bits(5) + 1, hclen = br.bits(4) + 4;
      static constexpr u8 kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
      u8 cl[19] = {};
      for (u32 i = 0; i < hclen; ++i) cl[kOrder[i]] = static_cast<u8>(br.bits(3));
      Huffman clh;
      clh.build(cl, 19);
      u8 lens[320] = {};
      u32 n = 0;
      while (n < hlit + hdist) {
        const u32 sym = clh.decode(br);
        if (sym < 16) {
          lens[n++] = static_cast<u8>(sym);
        } else if (sym == 16) {
          if (n == 0) throw std::runtime_error("inflate: repeat with no previous");
          const u8 prev = lens[n - 1];
          for (u32 r = 3 + br.bits(2); r-- > 0;) lens[n++] = prev;
        } else if (sym == 17) {
          for (u32 r = 3 + br.bits(3); r-- > 0;) lens[n++] = 0;
        } else {
          for (u32 r = 11 + br.bits(7); r-- > 0;) lens[n++] = 0;
        }
      }
      Huffman lit, dist;
      lit.build(lens, hlit);
      dist.build(lens + hlit, hdist);
      inflate_block(br, lit, dist, out);
    } else {
      throw std::runtime_error("inflate: bad block type");
    }
    if (final) break;
  }
  return out;
}

u8 paeth(u8 a, u8 b, u8 c) {
  const int p = int{a} + int{b} - int{c};
  const int pa = std::abs(p - int{a}), pb = std::abs(p - int{b}), pc = std::abs(p - int{c});
  if (pa <= pb && pa <= pc) return a;
  if (pb <= pc) return b;
  return c;
}

}  // namespace

Image read_png(const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) throw std::runtime_error("cannot open " + path);
  std::vector<u8> file;
  u8 buf[65536];
  usize n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) file.insert(file.end(), buf, buf + n);
  std::fclose(f);
  static constexpr u8 kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  if (file.size() < 8 || std::memcmp(file.data(), kSig, 8) != 0) throw std::runtime_error("not a PNG: " + path);

  u32 w = 0, h = 0;
  u8 depth = 0, ctype = 0, interlace = 0;
  std::vector<u8> idat, palette;
  usize p = 8;
  while (p + 8 <= file.size()) {
    const u32 len = load_be32(file.data() + p);
    const std::string type(reinterpret_cast<const char*>(file.data() + p + 4), 4);
    const u8* d = file.data() + p + 8;
    if (p + 12 + len > file.size()) throw std::runtime_error("PNG: truncated chunk");
    if (type == "IHDR") {
      w = load_be32(d);
      h = load_be32(d + 4);
      depth = d[8];
      ctype = d[9];
      interlace = d[12];
    } else if (type == "PLTE") {
      palette.assign(d, d + len);
    } else if (type == "IDAT") {
      idat.insert(idat.end(), d, d + len);
    } else if (type == "IEND") {
      break;
    }
    p += 12 + len;
  }
  if (depth != 8 || interlace != 0) throw std::runtime_error("PNG: only 8-bit non-interlaced supported");
  const u32 channels = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : ctype == 6 ? 4 : 0;
  if (!channels) throw std::runtime_error("PNG: unsupported colour type");

  const std::vector<u8> raw = zlib_inflate(idat);
  const usize stride = usize{w} * channels;
  if (raw.size() < (stride + 1) * h) throw std::runtime_error("PNG: short image data");
  std::vector<u8> px(stride * h);
  for (u32 y = 0; y < h; ++y) {
    const u8 filter = raw[y * (stride + 1)];
    const u8* src = raw.data() + y * (stride + 1) + 1;
    u8* dst = px.data() + y * stride;
    const u8* up = y ? px.data() + (y - 1) * stride : nullptr;
    for (usize x = 0; x < stride; ++x) {
      const u8 a = x >= channels ? dst[x - channels] : 0;
      const u8 b = up ? up[x] : 0;
      const u8 c = (up && x >= channels) ? up[x - channels] : 0;
      u8 v = src[x];
      switch (filter) {
        case 0: break;
        case 1: v = static_cast<u8>(v + a); break;
        case 2: v = static_cast<u8>(v + b); break;
        case 3: v = static_cast<u8>(v + ((int{a} + int{b}) >> 1)); break;
        case 4: v = static_cast<u8>(v + paeth(a, b, c)); break;
        default: throw std::runtime_error("PNG: bad filter");
      }
      dst[x] = v;
    }
  }

  Image img;
  img.width = w;
  img.height = h;
  img.rgb.resize(usize{w} * h);
  for (usize i = 0; i < img.rgb.size(); ++i) {
    const u8* s = px.data() + i * channels;
    u32 r, g, b;
    if (ctype == 3) {
      const usize pi = usize{s[0]} * 3;
      r = palette[pi];
      g = palette[pi + 1];
      b = palette[pi + 2];
    } else if (channels <= 2) {
      r = g = b = s[0];
    } else {
      r = s[0];
      g = s[1];
      b = s[2];
    }
    img.rgb[i] = (r << 16) | (g << 8) | b;
  }
  return img;
}

}  // namespace ultra::harness
