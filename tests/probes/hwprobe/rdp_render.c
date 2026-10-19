// RDP render matrix: small renders across every cycle type, texture format, filter, wrap mode,
// LOD mode, a representative set of combiner, blender, coverage, dither and Z modes, and the
// triangle kinds. Each case's colour buffer (and Z buffer when the case uses Z) goes to the full
// record with the exact command words that drew it, so hardware pixels are the RDP raster oracle
// beside Angrylion. docs/timing/hwprobe.md, "RDP render matrix".
//
// Encodings: n64brew "Reality Display Processor/Commands"; other-mode, combiner and blender
// fields from libdragon's rdpq_macros.h. Every wait is bounded: in an emulator without an RDP the
// buffers keep the CPU's initial pattern and S_RDPR_INFO records HWP_TIMEOUT.
#include <libdragon.h>
#include <rdpq_macros.h>
#include <string.h>

#include "hwp.h"

#define RR_TIMEOUT_TICKS 937500u  // 20 ms: Count runs at 46.875 MHz (n64brew "ROM Header")

// Render target: every case draws into one RR_W x RR_H image.
#define RR_W 32
#define RR_H 32
// RDRAM, inside the 480i framebuffer area, which nothing uses while the text screen shows
// (display.c). 64-byte aligned (n64brew "Set Color Image", "Set Depth Image" hazards).
#define RR_FB_PHYS   0x00100000u
#define RR_ZB_PHYS   0x00110000u
#define RR_ZERO_PHYS 0x0011F000u  // 4 KiB of zeros: cleared into TMEM before every case
#define RR_TEX_PHYS  0x00120000u  // texture k at RR_TEX_PHYS + k * RR_TEX_SLOT
#define RR_TEX_SLOT  0x800u

// DP command registers (n64brew "Reality Display Processor/Interface").
#define RR_DP(i) (((volatile uint32_t*)0xA4100000u)[i])
enum { RR_DPC_START = 0, RR_DPC_END, RR_DPC_CURRENT, RR_DPC_STATUS, RR_DPC_CLOCK };
#define RR_DPC_STATUS_DMA_BUSY 0x100u
// DPC_STATUS write: clear XBUS (commands from RDRAM), FREEZE, FLUSH and the four counters.
#define RR_DPC_STATUS_RESET 0x3D5u
#define RR_MI_MODE (*(volatile uint32_t*)0xA4300000u)
#define RR_MI_INTR (*(volatile uint32_t*)0xA4300008u)
#define RR_MI_INTR_DP 0x20u
#define RR_MI_MODE_CLR_DP 0x800u

// Command ids (n64brew "Reality Display Processor/Commands").
enum {
  OP_TRI = 0x08, OP_TEXRECT = 0x24, OP_TEXRECT_FLIP = 0x25, OP_SYNC_LOAD = 0x26,
  OP_SYNC_PIPE = 0x27, OP_SYNC_TILE = 0x28, OP_SYNC_FULL = 0x29, OP_KEY_GB = 0x2A,
  OP_KEY_R = 0x2B, OP_CONVERT = 0x2C, OP_SCISSOR = 0x2D, OP_PRIM_DEPTH = 0x2E,
  OP_OTHER_MODES = 0x2F, OP_LOAD_TLUT = 0x30, OP_TILE_SIZE = 0x32, OP_LOAD_BLOCK = 0x33,
  OP_LOAD_TILE = 0x34, OP_SET_TILE = 0x35, OP_FILL_RECT = 0x36, OP_FILL_COLOR = 0x37,
  OP_FOG_COLOR = 0x38, OP_BLEND_COLOR = 0x39, OP_PRIM_COLOR = 0x3A, OP_ENV_COLOR = 0x3B,
  OP_COMBINE = 0x3C, OP_TEX_IMAGE = 0x3D, OP_Z_IMAGE = 0x3E, OP_COLOR_IMAGE = 0x3F,
};
// Triangle command bits 58:56 (n64brew "Fill Triangle").
enum { TRI_Z = 1, TRI_TEX = 2, TRI_SHADE = 4 };
// Image formats and sizes (n64brew "Set Tile").
enum { F_RGBA = 0, F_YUV = 1, F_CI = 2, F_IA = 3, F_I = 4 };
enum { SZ_4 = 0, SZ_8 = 1, SZ_16 = 2, SZ_32 = 3 };

// Set Other Modes bits the hardware defines: libdragon keeps its own state in bits 35:32 and 15
// (rdpq_macros.h SOMX_*), and command bits 63:56 are added here.
#define RR_SOM_HW 0x00FFFFF0FFFF7FFFull
// key_en, bit 40 (n64brew "Set Other Modes"); rdpq_macros.h has no name for it.
#define RR_SOM_KEY_EN (1ull << 40)
#define RR_CC_MASK 0x00FFFFFFFFFFFFFFull  // Set Combine Mode bits 55:0

// Base modes: dither off, so that only the dither cases depend on the dither pattern.
#define DITHER_OFF (SOM_RGBDITHER_NONE | SOM_ALPHADITHER_NONE)
#define M1 (SOM_CYCLE_1 | SOM_TF0_RGB | SOM_TF1_RGB | DITHER_OFF)
#define M2 (SOM_CYCLE_2 | SOM_TF0_RGB | SOM_TF1_RGB | DITHER_OFF)
#define MFILL SOM_CYCLE_FILL
#define MCOPY SOM_CYCLE_COPY
#define ZCU (SOM_Z_COMPARE | SOM_Z_WRITE)
#define BL_ALPHA RDPQ_BLENDER((IN_RGB, IN_ALPHA, MEMORY_RGB, INV_MUX_ALPHA))
#define BL_AA RDPQ_BLENDER((IN_RGB, IN_ALPHA, MEMORY_RGB, MEMORY_CVG))
#define AA_STD (SOM_AA_ENABLE | SOM_READ_ENABLE | SOM_BLALPHA_CVG | SOM_COLOR_ON_CVG_OVERFLOW)

#define Q(px) ((int32_t)(px) * 4)  // pixels (or texels) to the u10.2 / s11.2 quarter unit
#define S5(t) ((int32_t)(t) * 32)  // texels to s10.5
#define D10(num, den) ((int32_t)((num) * 1024 / (den)))  // a ratio in s5.10 (dsdx, dtdy)

// ---- Textures: deterministic, generated on the CPU, each recorded in S_RDPR_TEX ----
enum {
  TX_RGBA16, TX_RGBA32, TX_IA4, TX_IA8, TX_IA16, TX_I4, TX_I8, TX_CI4, TX_CI8,
  TX_TLUT16, TX_TLUT256, TX_YUV16, TX_MIP0, TX_MIP1, TX_MIP2, TX_MIP3, TX_MIP4, TX_MIP5,
  TX_COUNT
};
typedef struct {
  uint8_t fmt, size;
  uint16_t w, h;
} rr_tex_t;
static const rr_tex_t rr_tex[TX_COUNT] = {
    [TX_RGBA16] = {F_RGBA, SZ_16, 16, 16}, [TX_RGBA32] = {F_RGBA, SZ_32, 16, 16},
    [TX_IA4] = {F_IA, SZ_4, 16, 16},       [TX_IA8] = {F_IA, SZ_8, 16, 16},
    [TX_IA16] = {F_IA, SZ_16, 16, 16},     [TX_I4] = {F_I, SZ_4, 16, 16},
    [TX_I8] = {F_I, SZ_8, 16, 16},         [TX_CI4] = {F_CI, SZ_4, 16, 16},
    [TX_CI8] = {F_CI, SZ_8, 16, 16},       [TX_TLUT16] = {F_RGBA, SZ_16, 16, 1},
    [TX_TLUT256] = {F_RGBA, SZ_16, 256, 1}, [TX_YUV16] = {F_YUV, SZ_16, 16, 16},
    [TX_MIP0] = {F_RGBA, SZ_16, 32, 32},   [TX_MIP1] = {F_RGBA, SZ_16, 16, 16},
    [TX_MIP2] = {F_RGBA, SZ_16, 8, 8},     [TX_MIP3] = {F_RGBA, SZ_16, 4, 4},
    [TX_MIP4] = {F_RGBA, SZ_16, 2, 2},     [TX_MIP5] = {F_RGBA, SZ_16, 1, 1},
};

static uint32_t hash32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7FEB352Du;
  x ^= x >> 15;
  x *= 0x846CA68Bu;
  x ^= x >> 16;
  return x;
}

static uint32_t tex_bits(int k) { return 4u << rr_tex[k].size; }
static uint32_t tex_bytes(int k) { return rr_tex[k].w * rr_tex[k].h * tex_bits(k) / 8; }
static uint32_t tex_phys(int k) { return RR_TEX_PHYS + (uint32_t)k * RR_TEX_SLOT; }
// TMEM words per row. RGBA32 splits each texel into two 16-bit halves in the low and high
// banks, so a row takes width * 2 bytes of each (n64brew "Set Tile", line).
static uint32_t tex_line(int k) {
  const uint32_t bits = rr_tex[k].size == SZ_32 ? 16 : tex_bits(k);
  return (rr_tex[k].w * bits + 63) / 64;
}

static uint16_t rgba16(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
  return (uint16_t)(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (a >> 7));
}

// Texel (s, t) of texture k as big-endian bytes at p. Gradients in s and t (visible through
// filtering) mixed with hashed low bits (so every bit of every format is exercised).
static void texel(int k, uint32_t s, uint32_t t, uint8_t* img) {
  const rr_tex_t* d = &rr_tex[k];
  const uint32_t h = hash32((uint32_t)k << 24 | t << 12 | s);
  const uint32_t r = (s * 256 / d->w + (h & 15)) & 0xFF, g = (t * 256 / d->h + ((h >> 4) & 15)) & 0xFF;
  const uint32_t b = (h >> 8) & 0xFF, a = (h >> 16) & 0xFF, i = (r + g) >> 1;
  const uint32_t idx = t * d->w + s;
  uint32_t v;
  switch (k) {
    case TX_RGBA16: case TX_TLUT16: case TX_TLUT256: v = rgba16(r, g, b, a); break;
    case TX_RGBA32: v = r << 24 | g << 16 | b << 8 | a; break;
    case TX_IA4: v = (i >> 5) << 1 | a >> 7; break;
    case TX_IA8: v = (i >> 4) << 4 | a >> 4; break;
    case TX_IA16: v = i << 8 | a; break;
    case TX_I4: v = i >> 4; break;
    case TX_I8: v = i; break;
    case TX_CI4: v = (s * 3 + t + (h >> 31)) & 15; break;
    case TX_CI8: v = (s ^ t << 4) & 0xFF; break;
    case TX_YUV16: v = ((s & 1) ? b : a) << 8 | i; break;  // U Y0 V Y1 pairs
    default: {  // mip level L: one hue per level, a checkerboard, hashed low bits
      const uint32_t L = (uint32_t)(k - TX_MIP0), c = ((s ^ t) & 1) ? 0x40 : 0;
      v = rgba16((L * 0x53 + c + (h & 31)) & 0xFF, (0xF0 - L * 0x28 + c) & 0xFF, (L * 0x91 + 0x30) & 0xFF, 0xFF);
    }
  }
  const uint32_t bits = tex_bits(k);
  if (bits == 4) {
    uint8_t* p = &img[idx / 2];
    *p = (idx & 1) ? (uint8_t)((*p & 0xF0) | v) : (uint8_t)((*p & 0x0F) | v << 4);
  } else {
    for (uint32_t n = 0; n < bits / 8; ++n) img[idx * (bits / 8) + n] = (uint8_t)(v >> (bits - 8 - 8 * n));
  }
}

static void tex_generate(void) {
  memset((void*)(0x80000000u | RR_ZERO_PHYS), 0, 0x1000);
  dcache_wb((void*)(0x80000000u | RR_ZERO_PHYS), 0x1000);
  for (int k = 0; k < TX_COUNT; ++k) {
    uint8_t* img = (uint8_t*)(0x80000000u | tex_phys(k));
    memset(img, 0, RR_TEX_SLOT);
    for (uint32_t t = 0; t < rr_tex[k].h; ++t)
      for (uint32_t s = 0; s < rr_tex[k].w; ++s) texel(k, s, t, img);
    dcache_wb(img, RR_TEX_SLOT);
  }
}

// ---- Command list ----
#define RR_CMD_MAX 640
static uint64_t rr_cmd[RR_CMD_MAX] __attribute__((aligned(16)));
static uint32_t rr_n;
static bool rr_trunc;
static int rr_bpp;   // colour image bits per pixel: 8, 16 or 32
static bool rr_zuse; // the case reads or writes Z: its Z buffer is recorded

static void C(uint64_t w) {
  if (rr_n < RR_CMD_MAX - 2) rr_cmd[rr_n++] = w;  // two words stay free for the closing syncs
  else rr_trunc = true;
}
static uint64_t op(uint32_t o) { return (uint64_t)o << 56; }
static uint64_t rc(int32_t x, int32_t y) { return ((uint64_t)(x & 0xFFF) << 12) | (uint64_t)(y & 0xFFF); }

static void sync_pipe(void) { C(op(OP_SYNC_PIPE)); }
static void scissor(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int field, int odd) {
  C(op(OP_SCISSOR) | rc(x0, y0) << 32 | (uint64_t)field << 25 | (uint64_t)odd << 24 | rc(x1, y1));
}
// Other modes, then the scissor that covers the image: FILL and COPY include the right edge,
// 1- and 2-cycle exclude it (n64brew "Set Scissor" hazards).
static void som(uint64_t m) {
  sync_pipe();
  C(op(OP_OTHER_MODES) | (m & RR_SOM_HW));
  const bool fillcopy = ((m & SOM_CYCLE_MASK) >> SOM_CYCLE_SHIFT) >= 2;
  scissor(0, 0, fillcopy ? Q(RR_W - 1) : Q(RR_W), Q(RR_H), 0, 0);
  if (m & (SOM_Z_COMPARE | SOM_Z_WRITE)) rr_zuse = true;
}
static void cc(uint64_t c) { C(op(OP_COMBINE) | (c & RR_CC_MASK)); }
static void color(uint32_t o, uint32_t rgba) { C(op(o) | rgba); }
static void prim_color(uint32_t min_level, uint32_t lod_frac, uint32_t rgba) {
  C(op(OP_PRIM_COLOR) | (uint64_t)min_level << 40 | (uint64_t)lod_frac << 32 | rgba);
}
static void prim_depth(uint32_t z, uint32_t dz) { C(op(OP_PRIM_DEPTH) | z << 16 | dz); }
static void key_gb(uint32_t wg, uint32_t wb, uint32_t cg, uint32_t sg, uint32_t cb, uint32_t sb) {
  C(op(OP_KEY_GB) | (uint64_t)wg << 44 | (uint64_t)wb << 32 | cg << 24 | sg << 16 | cb << 8 | sb);
}
static void key_r(uint32_t wr, uint32_t cr, uint32_t sr) { C(op(OP_KEY_R) | wr << 16 | cr << 8 | sr); }
static void convert(int32_t k0, int32_t k1, int32_t k2, int32_t k3, int32_t k4, int32_t k5) {
  const uint64_t m = 0x1FF;
  C(op(OP_CONVERT) | (k0 & m) << 45 | (k1 & m) << 36 | (k2 & m) << 27 | (k3 & m) << 18 | (k4 & m) << 9 | (k5 & m));
}

// Clamp/mirror/mask/shift for one axis of Set Tile.
#define TM(clamp, mirror, mask, shift) (((clamp) << 9) | ((mirror) << 8) | ((mask) << 4) | (shift))
#define WRAP4 TM(0, 0, 4, 0)  // wrap at 16 texels
static void set_tile(int tile, int fmt, int size, uint32_t line, uint32_t tmem, int pal, uint32_t tms, uint32_t tmt) {
  C(op(OP_SET_TILE) | (uint64_t)fmt << 53 | (uint64_t)size << 51 | (uint64_t)line << 41 | (uint64_t)tmem << 32 |
    (uint64_t)tile << 24 | (uint64_t)pal << 20 | (uint64_t)tmt << 10 | tms);
}
static void tile_size(int tile, int32_t sl, int32_t tl, int32_t sh, int32_t th) {
  C(op(OP_TILE_SIZE) | rc(sl, tl) << 32 | (uint64_t)tile << 24 | rc(sh, th));
}
static void tex_image(int fmt, int size, uint32_t width, uint32_t phys) {
  C(op(OP_TEX_IMAGE) | (uint64_t)fmt << 53 | (uint64_t)size << 51 | (uint64_t)(width - 1) << 32 | phys);
}

// Loads texture k into TMEM word tmem through tile 7. 4-bit images load as 8-bit at half the
// width; a TLUT loads through a 4-bit I tile (n64brew "Load TLUT" hazards).
static void load_tex(int k, uint32_t tmem) {
  const rr_tex_t* d = &rr_tex[k];
  if (k == TX_TLUT16 || k == TX_TLUT256) {
    tex_image(F_RGBA, SZ_16, d->w, tex_phys(k));
    set_tile(7, F_I, SZ_4, 0, tmem, 0, 0, 0);
    C(op(OP_SYNC_LOAD));
    C(op(OP_LOAD_TLUT) | (uint64_t)7 << 24 | rc(Q(d->w - 1), 0));
  } else {
    const bool four = d->size == SZ_4;
    const uint32_t w = four ? d->w / 2u : d->w;
    tex_image(d->fmt, four ? SZ_8 : d->size, w, tex_phys(k));
    set_tile(7, d->fmt, four ? SZ_8 : d->size, tex_line(k), tmem, 0, 0, 0);
    C(op(OP_SYNC_LOAD));
    C(op(OP_LOAD_TILE) | rc(0, 0) << 32 | (uint64_t)7 << 24 | rc(Q(w - 1), Q(d->h - 1)));
  }
  C(op(OP_SYNC_TILE));
}
// Texture k in TMEM at tmem, described by tile with its full extent as the tile size.
static void use_tex(int k, int tile, uint32_t tmem, int pal, uint32_t tms, uint32_t tmt) {
  load_tex(k, tmem);
  set_tile(tile, rr_tex[k].fmt, rr_tex[k].size, tex_line(k), tmem, pal, tms, tmt);
  tile_size(tile, 0, 0, Q(rr_tex[k].w - 1), Q(rr_tex[k].h - 1));
}

// Rectangles: coordinates in quarter pixels.
static void fill_rect(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
  C(op(OP_FILL_RECT) | rc(x1, y1) << 32 | rc(x0, y0));
}
static void tex_rect(int tile, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t s, int32_t t,
                     int32_t dsdx, int32_t dtdy, bool flip) {
  C(op(flip ? OP_TEXRECT_FLIP : OP_TEXRECT) | rc(x1, y1) << 32 | (uint64_t)tile << 24 | rc(x0, y0));
  C((uint64_t)(uint16_t)s << 48 | (uint64_t)(uint16_t)t << 32 | (uint64_t)(uint16_t)dsdx << 16 | (uint16_t)dtdy);
}

// ---- Triangles from three vertices ----
// x, y: quarter pixels. r, g, b, a: 0..255. s, t: texels in s10.5. w: perspective scale in
// s10.21 (0 means RR_W_ONE); s and t are multiplied by w / RR_W_ONE. z: 0..0x7FFF.
typedef struct {
  int32_t x, y, r, g, b, a, s, t, w, z;
} rr_vtx;
#define RR_W_ONE 0x7FFF0000ll

typedef struct {
  int64_t x[3], y[3], cross, dxhdy, xh, ytop;
} rr_edge;

// One attribute's start value at (xh, floor(yh)) and its d/dx, d/de, d/dy, in the attribute's own
// fixed-point unit. v[] are the vertex values in that unit.
static void plane(const rr_edge* e, const int64_t v[3], int64_t out[4]) {
  const int64_t dx2 = e->x[1] - e->x[0], dy2 = e->y[1] - e->y[0];
  const int64_t dx3 = e->x[2] - e->x[0], dy3 = e->y[2] - e->y[0];
  const int64_t da2 = v[1] - v[0], da3 = v[2] - v[0];
  const int64_t dadx = (da2 * dy3 - da3 * dy2) * 4 / e->cross;  // per pixel (coordinates are quarters)
  const int64_t dady = (dx2 * da3 - dx3 * da2) * 4 / e->cross;
  out[0] = v[0] + dadx * (e->xh - (e->x[0] << 14)) / 65536 + dady * (e->ytop - e->y[0]) / 4;
  out[1] = dadx;
  out[2] = dady + dadx * e->dxhdy / 65536;
  out[3] = dady;
}

// Four attributes a[ch][start, dx, de, dy] as the 8-word shade / texture block (n64brew "Fill
// Triangle", optional shading and texturing properties): integer halves, then fractions.
static void put_block(const int64_t a[4][4]) {
  static const int order[8][2] = {{0, 1}, {1, 1}, {0, 0}, {1, 0}, {2, 1}, {3, 1}, {2, 0}, {3, 0}};
  for (int w = 0; w < 8; ++w) {
    uint64_t word = 0;
    for (int ch = 0; ch < 4; ++ch) {
      const uint32_t v = (uint32_t)a[ch][order[w][0]];
      word |= (uint64_t)(order[w][1] ? v >> 16 : v & 0xFFFF) << (48 - 16 * ch);
    }
    C(word);
  }
}

static void tri(int flags, int level, int tile, rr_vtx a, rr_vtx b, rr_vtx c) {
  rr_vtx v[3] = {a, b, c};
  for (int i = 0; i < 2; ++i)  // stable sort by y
    for (int j = 0; j < 2 - i; ++j)
      if (v[j + 1].y < v[j].y) {
        const rr_vtx tmp = v[j];
        v[j] = v[j + 1];
        v[j + 1] = tmp;
      }
  rr_edge e;
  for (int i = 0; i < 3; ++i) {
    e.x[i] = v[i].x;
    e.y[i] = v[i].y;
  }
  const int64_t x1 = e.x[0], y1 = e.y[0], x2 = e.x[1], y2 = e.y[1], x3 = e.x[2], y3 = e.y[2];
  e.cross = (x2 - x1) * (y3 - y1) - (x3 - x1) * (y2 - y1);
  if (y3 == y1 || e.cross == 0) return;
  e.dxhdy = ((x3 - x1) << 16) / (y3 - y1);  // s15.16 pixels per line
  const int64_t dxmdy = y2 != y1 ? ((x2 - x1) << 16) / (y2 - y1) : 0;
  const int64_t dxldy = y3 != y2 ? ((x3 - x2) << 16) / (y3 - y2) : 0;
  e.ytop = (y1 >> 2) << 2;  // floor(yh)
  e.xh = (x1 << 14) + e.dxhdy * (e.ytop - y1) / 4;
  const int64_t xm = y2 != y1 ? (x1 << 14) + dxmdy * (e.ytop - y1) / 4 : x2 << 14;
  const int64_t xl = x2 << 14;
  const bool lmajor = e.cross > 0;  // middle vertex right of the major edge
  C(op(OP_TRI | (uint32_t)flags) | (uint64_t)lmajor << 55 | (uint64_t)(level & 7) << 51 |
    (uint64_t)(tile & 7) << 48 | (uint64_t)(y3 & 0x3FFF) << 32 | (uint64_t)(y2 & 0x3FFF) << 16 | (uint64_t)(y1 & 0x3FFF));
  C((uint64_t)(uint32_t)xl << 32 | (uint32_t)dxldy);
  C((uint64_t)(uint32_t)e.xh << 32 | (uint32_t)e.dxhdy);
  C((uint64_t)(uint32_t)xm << 32 | (uint32_t)dxmdy);
  int64_t blk[4][4], vals[3];
  if (flags & TRI_SHADE) {
    for (int ch = 0; ch < 4; ++ch) {
      for (int i = 0; i < 3; ++i) {
        const int32_t comp[4] = {v[i].r, v[i].g, v[i].b, v[i].a};
        vals[i] = (int64_t)comp[ch] << 16;  // s15.16
      }
      plane(&e, vals, blk[ch]);
    }
    put_block(blk);
  }
  if (flags & TRI_TEX) {
    for (int ch = 0; ch < 3; ++ch) {
      for (int i = 0; i < 3; ++i) {
        const int64_t w = v[i].w ? v[i].w : RR_W_ONE;
        const int64_t st = (int64_t)(ch == 0 ? v[i].s : v[i].t) << 16;  // s10.21
        vals[i] = ch == 2 ? w : st * w / RR_W_ONE;
      }
      plane(&e, vals, blk[ch]);
    }
    for (int k = 0; k < 4; ++k) blk[3][k] = 0;
    put_block(blk);
  }
  if (flags & TRI_Z) {
    for (int i = 0; i < 3; ++i) vals[i] = (int64_t)v[i].z << 16;  // s15.16
    plane(&e, vals, blk[0]);
    C((uint64_t)(uint32_t)blk[0][0] << 32 | (uint32_t)blk[0][1]);
    C((uint64_t)(uint32_t)blk[0][2] << 32 | (uint32_t)blk[0][3]);
    rr_zuse = true;
  }
}
// Quad of two triangles: v00 top left, v10 top right, v01 bottom left, v11 bottom right.
static void quad(int flags, int level, int tile, rr_vtx v00, rr_vtx v10, rr_vtx v01, rr_vtx v11) {
  tri(flags, level, tile, v00, v10, v01);
  tri(flags, level, tile, v10, v11, v01);
}
static rr_vtx V(int32_t xq, int32_t yq) { return (rr_vtx){.x = xq, .y = yq, .w = 0}; }
static rr_vtx VC(int32_t xq, int32_t yq, uint32_t rgba) {
  rr_vtx v = V(xq, yq);
  v.r = (int32_t)(rgba >> 24);
  v.g = (int32_t)(rgba >> 16 & 0xFF);
  v.b = (int32_t)(rgba >> 8 & 0xFF);
  v.a = (int32_t)(rgba & 0xFF);
  return v;
}
static rr_vtx VT(int32_t xq, int32_t yq, int32_t s, int32_t t) {
  rr_vtx v = VC(xq, yq, 0xFFFFFFFFu);
  v.s = s;
  v.t = t;
  return v;
}
static rr_vtx VZ(rr_vtx v, int32_t z) {
  v.z = z;
  return v;
}

// Full-image quads.
static void shade_quad(uint32_t c00, uint32_t c10, uint32_t c01, uint32_t c11) {
  quad(TRI_SHADE, 0, 0, VC(0, 0, c00), VC(Q(RR_W), 0, c10), VC(0, Q(RR_H), c01), VC(Q(RR_W), Q(RR_H), c11));
}
#define GRAD shade_quad(0x00000000u, 0xFF4000FFu, 0x00FF8080u, 0xFFFFFF40u)
// Texture quad mapping (s0, t0) .. (s1, t1) texels (s10.5) over the image, with w per row.
static void tex_quad(int flags, int level, int32_t s0, int32_t t0, int32_t s1, int32_t t1, int32_t wtop, int32_t wbot) {
  rr_vtx a = VT(0, 0, s0, t0), b = VT(Q(RR_W), 0, s1, t0), c = VT(0, Q(RR_H), s0, t1), d = VT(Q(RR_W), Q(RR_H), s1, t1);
  a.w = b.w = wtop;
  c.w = d.w = wbot;
  quad(flags, level, 0, a, b, c, d);
}

// Two shaded triangles that share an edge, with subpixel vertices (coverage and AA cases).
static void aa_pair(uint32_t ca, uint32_t cb, int flags, int32_t za, int32_t zb) {
  tri(TRI_SHADE | flags, 0, 0, VZ(VC(9, 6, ca), za), VZ(VC(119, 25, ca), za + 0x800), VZ(VC(38, 123, ca), za + 0x400));
  tri(TRI_SHADE | flags, 0, 0, VZ(VC(119, 25, cb), zb), VZ(VC(122, 116, cb), zb + 0x200), VZ(VC(38, 123, cb), zb + 0x600));
}

// The mip chain RGBA16 32x32 .. 1x1 in tiles 0..5 (TMEM words 0, 256, 320, 336, 340, 342).
static void use_mips(void) {
  static const uint16_t tmem[6] = {0, 256, 320, 336, 340, 342};
  for (int L = 0; L < 6; ++L) {
    const int k = TX_MIP0 + L;
    const uint32_t mask = (uint32_t)(5 - L);  // log2 of the level's width
    use_tex(k, L, tmem[L], 0, TM(L == 5, 0, mask, 0), TM(L == 5, 0, mask, 0));
  }
}

// Every case starts here: colour and Z image, every colour register, all eight tile
// descriptors and all of TMEM set to fixed values, so a case depends on nothing before it.
static void begin(int bpp) {
  rr_n = 0;
  rr_trunc = false;
  rr_zuse = false;
  rr_bpp = bpp;
  const int fmt = bpp == 8 ? F_I : F_RGBA, size = bpp == 8 ? SZ_8 : bpp == 16 ? SZ_16 : SZ_32;
  C(op(OP_COLOR_IMAGE) | (uint64_t)fmt << 53 | (uint64_t)size << 51 | (uint64_t)(RR_W - 1) << 32 | RR_FB_PHYS);
  C(op(OP_Z_IMAGE) | RR_ZB_PHYS);
  color(OP_FILL_COLOR, 0);
  prim_color(0, 0, 0xFF8040C0u);
  color(OP_ENV_COLOR, 0x20C0FF60u);
  color(OP_BLEND_COLOR, 0x80808080u);
  color(OP_FOG_COLOR, 0x4060A0B0u);
  prim_depth(0, 0);
  key_gb(0, 0, 0, 0, 0, 0);
  key_r(0, 0, 0);
  convert(175, -43, -89, 222, 114, 42);
  for (int t = 0; t < 8; ++t) set_tile(t, F_RGBA, SZ_16, 0, 0, 0, 0, 0);
  // 2048 16-bit zero texels: all 4 KiB of TMEM (n64brew "Load Block").
  tex_image(F_RGBA, SZ_16, 1, RR_ZERO_PHYS);
  set_tile(7, F_RGBA, SZ_16, 0, 0, 0, 0, 0);
  C(op(OP_SYNC_LOAD));
  C(op(OP_LOAD_BLOCK) | (uint64_t)7 << 24 | (uint64_t)2047 << 12);
  C(op(OP_SYNC_LOAD));
}

// One texture format, point sampled and magnified 2x by a texture rectangle.
static void fmt_case(int k, int pal, uint64_t tlut) {
  begin(16);
  som(M1 | tlut);
  cc(RDPQ_COMBINER_TEX);
  use_tex(k, 0, 0, pal, WRAP4, WRAP4);
  if (k == TX_CI8) load_tex(TX_TLUT256, 0x100);
  if (k == TX_CI4) load_tex(TX_TLUT16, 0x100 + 16u * (uint32_t)pal);
  tex_rect(0, 0, 0, Q(RR_W), Q(RR_H), 0, 0, D10(1, 2), D10(1, 2), false);
}
// Filtering: texture k at a quarter-texel offset and a 7/16 step, so every fraction appears.
static void filter_case(int k, uint64_t sample) {
  begin(16);
  som(M1 | sample | (k == TX_CI8 ? SOM_TLUT_RGBA16 : 0));
  cc(RDPQ_COMBINER_TEX);
  use_tex(k, 0, 0, 0, WRAP4, WRAP4);
  if (k == TX_CI8) load_tex(TX_TLUT256, 0x100);
  tex_rect(0, 0, 0, Q(RR_W), Q(RR_H), 8, 8, D10(7, 16), D10(7, 16), false);
}
// Clamp / mirror / mask / shift: s and t from -16 to +48 texels over the image.
static void wrap_case(uint32_t tms, uint32_t tmt, uint64_t sample) {
  begin(16);
  som(M1 | sample);
  cc(RDPQ_COMBINER_TEX);
  use_tex(TX_RGBA16, 0, 0, 0, tms, tmt);
  tex_rect(0, 0, 0, Q(RR_W), Q(RR_H), S5(-16), S5(-16), D10(2, 1), D10(2, 1), false);
}
// A combiner on the 2x magnified RGBA16 texture.
static void cc_case(uint64_t combiner, uint64_t modes) {
  begin(16);
  som(M1 | modes);
  cc(combiner);
  use_tex(TX_RGBA16, 0, 0, 0, WRAP4, WRAP4);
  tex_rect(0, 0, 0, Q(RR_W), Q(RR_H), 0, 0, D10(1, 2), D10(1, 2), false);
}
// Alpha compare on the RGBA32 texture, whose alpha is hashed.
static void acmp_case(uint64_t modes, uint32_t blend_alpha) {
  begin(16);
  som(modes | SOM_ALPHACOMPARE_THRESHOLD);
  color(OP_BLEND_COLOR, 0x40C08000u | blend_alpha);
  cc(RDPQ_COMBINER_TEX);
  use_tex(TX_RGBA32, 0, 0, 0, WRAP4, WRAP4);
  tex_rect(0, 0, 0, Q(RR_W), Q(RR_H), 0, 0, D10(1, 2), D10(1, 2), false);
}
// Dither: the gradient quad, optionally alpha-blended with memory so alpha dither shows.
static void dither_case(int bpp, uint64_t dither, bool blend) {
  begin(bpp);
  som((M1 & ~(SOM_RGBDITHER_MASK | SOM_ALPHADITHER_MASK)) | dither |
      (blend ? SOM_BLENDING | BL_ALPHA : 0));
  cc(RDPQ_COMBINER_SHADE);
  GRAD;
}
// Blender: the gradient quad (shade alpha ramps) or the RGBA32 texture.
static void blend_case(int bpp, uint64_t modes, bool textured) {
  begin(bpp);
  som(modes);
  if (textured) {
    cc(RDPQ_COMBINER_TEX);
    use_tex(TX_RGBA32, 0, 0, 0, WRAP4, WRAP4);
    tex_rect(0, 0, 0, Q(RR_W), Q(RR_H), 0, 0, D10(1, 2), D10(1, 2), false);
  } else {
    cc(RDPQ_COMBINER_SHADE);
    GRAD;
  }
}
// Coverage: the shared-edge pair, flat colours with partial alpha.
static void aa_case(int bpp, uint64_t modes) {
  begin(bpp);
  som(M1 | modes);
  cc(RDPQ_COMBINER_SHADE);
  aa_pair(0xF0402080u, 0x30E060C0u, 0, 0, 0);
}
// Z: a shaded triangle pair with sloped Z against the CPU's Z ramp.
static void z_case(int bpp, uint64_t modes, int32_t za, int32_t zb) {
  begin(bpp);
  som(M1 | modes);
  cc(RDPQ_COMBINER_SHADE);
  aa_pair(0xF0402080u, 0x30E060C0u, TRI_Z, za, zb);
}
// LOD: the mip chain on a quad that spans scale x 32 texels, optionally with perspective.
static void lod_case(uint64_t modes, uint64_t combiner, int scale, int32_t wbot) {
  begin(16);
  som(modes | SOM_TEXTURE_LOD);
  cc(combiner);
  prim_color(1, 0x80, 0xFFFFFFFFu);
  use_mips();
  tex_quad(TRI_TEX, 5, 0, 0, S5(32 * scale), S5(32 * scale), 0, wbot);
}

// The matrix. Returns false past the last case. Case numbers are series ids: keep the order.
static bool build(int n) {
  switch (n) {
    // ---- FILL ----
    case 0:  // full image, two colours (even and odd pixels)
      begin(16); som(MFILL); color(OP_FILL_COLOR, 0xF80107C1u); fill_rect(0, 0, Q(31), Q(31)); break;
    case 1:  // subpixel corners: FILL rounds them
      begin(16); som(MFILL); color(OP_FILL_COLOR, 0x0001003Fu); fill_rect(0, 0, Q(31), Q(31));
      sync_pipe(); color(OP_FILL_COLOR, 0xFFC1FFC1u); fill_rect(Q(3) + 1, Q(2) + 2, Q(12) + 3, Q(9) + 1);
      sync_pipe(); color(OP_FILL_COLOR, 0x7BDF7BDFu); fill_rect(Q(17) + 2, Q(11) + 3, Q(29) + 1, Q(27) + 2); break;
    case 2:  // 32 bpp
      begin(32); som(MFILL); color(OP_FILL_COLOR, 0x12345678u); fill_rect(0, 0, Q(31), Q(31));
      sync_pipe(); color(OP_FILL_COLOR, 0x9ABCDEF0u); fill_rect(Q(4), Q(4), Q(19), Q(27)); break;
    case 3:  // 8 bpp
      begin(8); som(MFILL); color(OP_FILL_COLOR, 0x10305070u); fill_rect(0, 0, Q(31), Q(31));
      sync_pipe(); color(OP_FILL_COLOR, 0xF0E0D0C0u); fill_rect(Q(5), Q(3), Q(26), Q(20)); break;
    case 4:  // scissored to x 4..27, y 6..24
      begin(16); som(MFILL); scissor(Q(4), Q(6), Q(27), Q(25), 0, 0);
      color(OP_FILL_COLOR, 0x5555AAAAu); fill_rect(0, 0, Q(31), Q(31)); break;
    // ---- COPY ----
    case 5:  // RGBA16 16x16 at (8, 8)
      begin(16); som(MCOPY); use_tex(TX_RGBA16, 0, 0, 0, WRAP4, WRAP4);
      tex_rect(0, Q(8), Q(8), Q(23), Q(23), 0, 0, D10(4, 1), D10(1, 1), false); break;
    case 6:  // alpha compare: texels with alpha bit 0 are not written
      begin(16); som(MCOPY | SOM_ALPHACOMPARE_THRESHOLD); use_tex(TX_RGBA16, 0, 0, 0, WRAP4, WRAP4);
      tex_rect(0, Q(8), Q(8), Q(23), Q(23), 0, 0, D10(4, 1), D10(1, 1), false); break;
    case 7:  // CI8 through the RGBA16 TLUT
      begin(16); som(MCOPY | SOM_TLUT_RGBA16); use_tex(TX_CI8, 0, 0, 0, WRAP4, WRAP4);
      load_tex(TX_TLUT256, 0x100); tex_rect(0, Q(8), Q(8), Q(23), Q(23), 0, 0, D10(4, 1), D10(1, 1), false); break;
    case 8:  // I8 into an 8 bpp image, 8 texels per step
      begin(8); som(MCOPY); use_tex(TX_I8, 0, 0, 0, WRAP4, WRAP4);
      tex_rect(0, Q(8), Q(8), Q(23), Q(23), 0, 0, D10(8, 1), D10(1, 1), false); break;
    case 9:  // at x = 3 (not a multiple of 4), wrapping s past the texture
      begin(16); som(MCOPY); use_tex(TX_RGBA16, 0, 0, 0, WRAP4, WRAP4);
      tex_rect(0, Q(3), Q(5), Q(30), Q(20), S5(2), 0, D10(4, 1), D10(1, 1), false); break;
    case 10:  // t stepping 0.5 per line
      begin(16); som(MCOPY); use_tex(TX_RGBA16, 0, 0, 0, WRAP4, WRAP4);
      tex_rect(0, 0, 0, Q(15), Q(31), 0, 0, D10(4, 1), D10(1, 2), false); break;
    // ---- 1-cycle texture formats ----
    case 11: fmt_case(TX_RGBA16, 0, 0); break;
    case 12: fmt_case(TX_RGBA32, 0, 0); break;
    case 13: fmt_case(TX_IA4, 0, 0); break;
    case 14: fmt_case(TX_IA8, 0, 0); break;
    case 15: fmt_case(TX_IA16, 0, 0); break;
    case 16: fmt_case(TX_I4, 0, 0); break;
    case 17: fmt_case(TX_I8, 0, 0); break;
    case 18: fmt_case(TX_CI4, 2, SOM_TLUT_RGBA16); break;
    case 19: fmt_case(TX_CI8, 0, SOM_TLUT_RGBA16); break;
    case 20: fmt_case(TX_CI8, 0, SOM_TLUT_IA16); break;
    case 21: fmt_case(TX_CI8, 0, 0); break;  // TLUT off: the raw index
    case 22:  // YUV16 with the colour-space conversion
      begin(16); som((M1 & ~SOM_TF_MASK) | SOM_TF0_YUV | SOM_TF1_YUV);
      cc(RDPQ_COMBINER1((TEX0, K4, K5, TEX0), (0, 0, 0, ONE)));
      use_tex(TX_YUV16, 0, 0, 0, WRAP4, WRAP4);
      tex_rect(0, 0, 0, Q(RR_W), Q(RR_H), 0, 0, D10(1, 2), D10(1, 2), false); break;
    case 23:  // RGBA16 loaded by Load Block (dxt = 2048 / 4 words per line)
      begin(16); som(M1); cc(RDPQ_COMBINER_TEX);
      tex_image(F_RGBA, SZ_16, 16, tex_phys(TX_RGBA16)); set_tile(7, F_RGBA, SZ_16, 0, 0, 0, 0, 0);
      C(op(OP_SYNC_LOAD)); C(op(OP_LOAD_BLOCK) | (uint64_t)7 << 24 | (uint64_t)255 << 12 | 512); C(op(OP_SYNC_TILE));
      set_tile(0, F_RGBA, SZ_16, 4, 0, 0, WRAP4, WRAP4); tile_size(0, 0, 0, Q(15), Q(15));
      tex_rect(0, 0, 0, Q(RR_W), Q(RR_H), 0, 0, D10(1, 2), D10(1, 2), false); break;
    case 24:  // RGBA16 data sampled through an IA16 tile
      begin(16); som(M1); cc(RDPQ_COMBINER_TEX); load_tex(TX_RGBA16, 0);
      set_tile(0, F_IA, SZ_16, 4, 0, 0, WRAP4, WRAP4); tile_size(0, 0, 0, Q(15), Q(15));
      tex_rect(0, 0, 0, Q(RR_W), Q(RR_H), 0, 0, D10(1, 2), D10(1, 2), false); break;
    // ---- Filtering ----
    case 25: filter_case(TX_RGBA16, SOM_SAMPLE_POINT); break;
    case 26: filter_case(TX_RGBA16, SOM_SAMPLE_BILINEAR); break;
    case 27: filter_case(TX_RGBA16, SOM_SAMPLE_MEDIAN); break;
    case 28: filter_case(TX_RGBA32, SOM_SAMPLE_BILINEAR); break;
    case 29: filter_case(TX_IA8, SOM_SAMPLE_BILINEAR); break;
    case 30: filter_case(TX_CI8, SOM_SAMPLE_BILINEAR); break;
    case 31: filter_case(TX_I4, SOM_SAMPLE_BILINEAR); break;
    case 32: filter_case(TX_IA16, SOM_SAMPLE_MEDIAN); break;
    // ---- Clamp, mirror, wrap, mask, shift ----
    case 33: wrap_case(WRAP4, WRAP4, 0); break;
    case 34: wrap_case(TM(0, 1, 4, 0), TM(0, 1, 4, 0), 0); break;
    case 35: wrap_case(TM(1, 0, 4, 0), TM(1, 0, 4, 0), 0); break;
    case 36: wrap_case(TM(1, 1, 4, 0), TM(1, 1, 4, 0), 0); break;
    case 37: wrap_case(TM(0, 0, 3, 0), TM(0, 1, 3, 0), 0); break;  // mask 8 of 16
    case 38: wrap_case(TM(0, 0, 4, 1), TM(0, 0, 4, 15), 0); break;  // s >> 1, t << 1
    case 39: wrap_case(TM(1, 0, 0, 0), TM(0, 0, 0, 0), 0); break;   // mask 0: clamp s, t unmasked
    case 40: wrap_case(WRAP4, TM(1, 1, 4, 0), SOM_SAMPLE_BILINEAR); break;  // seams
    case 41:  // tile size (2.5, 3) .. (12.5, 13) with clamp
      begin(16); som(M1); cc(RDPQ_COMBINER_TEX); use_tex(TX_RGBA16, 0, 0, 0, TM(1, 0, 4, 0), TM(1, 0, 4, 0));
      tile_size(0, Q(2) + 2, Q(3), Q(12) + 2, Q(13));
      tex_rect(0, 0, 0, Q(RR_W), Q(RR_H), S5(-4), S5(-4), D10(3, 4), D10(3, 4), false); break;
    case 42:  // flipped texture rectangle (s along y)
      begin(16); som(M1); cc(RDPQ_COMBINER_TEX); use_tex(TX_RGBA16, 0, 0, 0, WRAP4, WRAP4);
      tex_rect(0, Q(2), Q(4), Q(30), Q(28), S5(1), S5(3), D10(1, 2), D10(3, 4), true); break;
    case 43:  // negative steps
      begin(16); som(M1); cc(RDPQ_COMBINER_TEX); use_tex(TX_RGBA16, 0, 0, 0, WRAP4, WRAP4);
      tex_rect(0, 0, 0, Q(RR_W), Q(RR_H), S5(15) + 16, S5(15) + 16, D10(-1, 2), D10(-1, 2), false); break;
    case 44:  // subpixel rectangle corners in 1-cycle
      begin(16); som(M1); cc(RDPQ_COMBINER_TEX); use_tex(TX_RGBA16, 0, 0, 0, WRAP4, WRAP4);
      tex_rect(0, Q(3) + 1, Q(2) + 3, Q(29) + 2, Q(30) + 1, 0, 0, D10(1, 2), D10(1, 2), false); break;
    // ---- Combiner ----
    case 45: cc_case(RDPQ_COMBINER1((TEX0, ENV, PRIM_ALPHA, ENV), (TEX0, 0, PRIM, 0)), 0); break;
    case 46: cc_case(RDPQ_COMBINER1((PRIM, ENV, TEX0_ALPHA, ENV), (TEX0, ENV, PRIM, 0)), 0); break;
    case 47: cc_case(RDPQ_COMBINER1((ONE, ZERO, PRIM, TEX0), (ONE, 0, PRIM, TEX0)), 0); break;  // overflow
    case 48: cc_case(RDPQ_COMBINER1((ZERO, TEX0, PRIM, ZERO), (0, TEX0, PRIM, 0)), 0); break;  // underflow
    case 49:  // key centre and scale as inputs
      begin(16); som(M1); cc(RDPQ_COMBINER1((TEX0, KEYCENTER, KEYSCALE, ZERO), (0, 0, 0, TEX0)));
      key_gb(0x123, 0x456, 0x60, 0xC0, 0x90, 0x50); key_r(0x789, 0x80, 0xA0);
      use_tex(TX_RGBA16, 0, 0, 0, WRAP4, WRAP4);
      tex_rect(0, 0, 0, Q(RR_W), Q(RR_H), 0, 0, D10(1, 2), D10(1, 2), false); break;
    case 50:  // chroma key
      begin(16); som(M1 | RR_SOM_KEY_EN | SOM_BLENDING | BL_ALPHA); cc(RDPQ_COMBINER_TEX);
      key_gb(0x400, 0x300, 0x80, 0x20, 0x60, 0x18); key_r(0x500, 0x80, 0x10);
      use_tex(TX_RGBA16, 0, 0, 0, WRAP4, WRAP4);
      tex_rect(0, 0, 0, Q(RR_W), Q(RR_H), 0, 0, D10(1, 2), D10(1, 2), false); break;
    case 51: cc_case(RDPQ_COMBINER1((TEX0, K4, K5, ZERO), (0, 0, 0, TEX0)), 0); break;
    case 52:
      begin(16); som(M1); prim_color(3, 0x60, 0xFF8040C0u);
      cc(RDPQ_COMBINER1((TEX0, ENV, PRIM_LOD_FRAC, ENV), (TEX0, 0, PRIM_LOD_FRAC, 0)));
      use_tex(TX_RGBA16, 0, 0, 0, WRAP4, WRAP4);
      tex_rect(0, 0, 0, Q(RR_W), Q(RR_H), 0, 0, D10(1, 2), D10(1, 2), false); break;
    case 53: cc_case(RDPQ_COMBINER1((NOISE, ZERO, TEX0, ZERO), (0, 0, 0, TEX0)), 0); break;
    case 54: cc_case(RDPQ_COMBINER1((NOISE, ZERO, TEX0, ZERO), (0, 0, 0, TEX0)), 0); break;  // repeat: is noise reproducible
    case 55:  // alpha formula into 32 bpp
      begin(32); som(M1); cc(RDPQ_COMBINER1((0, 0, 0, TEX0), (TEX0, ENV, PRIM, ENV)));
      use_tex(TX_RGBA32, 0, 0, 0, WRAP4, WRAP4);
      tex_rect(0, 0, 0, Q(RR_W), Q(RR_H), 0, 0, D10(1, 2), D10(1, 2), false); break;
    case 56:  // 2-cycle: TEX0 x TEX1 x PRIM, two tiles
      begin(16); som(M2);
      cc(RDPQ_COMBINER2((TEX0, 0, TEX1, 0), (TEX0, 0, TEX1, 0), (COMBINED, 0, PRIM, 0), (COMBINED, 0, PRIM, 0)));
      use_tex(TX_RGBA16, 0, 0, 0, WRAP4, WRAP4); use_tex(TX_I8, 1, 64, 0, TM(0, 1, 4, 0), WRAP4);
      tex_rect(0, 0, 0, Q(RR_W), Q(RR_H), 0, 0, D10(1, 2), D10(1, 2), false); break;
    case 57:  // 2-cycle: second-cycle TEX0 (reads the next pixel)
      begin(16); som(M2);
      cc(RDPQ_COMBINER2((TEX0, 0, ENV, 0), (0, 0, 0, TEX0), (TEX0_BUG, COMBINED, PRIM_ALPHA, COMBINED), (0, 0, 0, COMBINED)));
      use_tex(TX_RGBA16, 0, 0, 0, WRAP4, WRAP4);
      tex_rect(0, 0, 0, Q(RR_W), Q(RR_H), 0, 0, D10(1, 2), D10(1, 2), false); break;
    // ---- Alpha compare ----
    case 58: acmp_case(M1, 0x80); break;
    case 59: acmp_case(M1, 0x10); break;
    case 60: acmp_case(M1 | SOM_ALPHACOMPARE_NOISE, 0x80); break;  // threshold from noise
    case 61: acmp_case(M2, 0x80); break;
    // ---- Dither ----
    case 62: dither_case(16, SOM_RGBDITHER_SQUARE | SOM_ALPHADITHER_NONE, false); break;
    case 63: dither_case(16, SOM_RGBDITHER_BAYER | SOM_ALPHADITHER_NONE, false); break;
    case 64: dither_case(16, SOM_RGBDITHER_NOISE | SOM_ALPHADITHER_NONE, false); break;
    case 65: dither_case(16, SOM_RGBDITHER_NONE | SOM_ALPHADITHER_NONE, false); break;
    case 66: dither_case(16, SOM_RGBDITHER_BAYER | SOM_ALPHADITHER_SAME, true); break;
    case 67: dither_case(16, SOM_RGBDITHER_BAYER | SOM_ALPHADITHER_INVERT, true); break;
    case 68: dither_case(16, SOM_RGBDITHER_NONE | SOM_ALPHADITHER_NOISE, true); break;
    case 69: dither_case(16, SOM_RGBDITHER_SQUARE | SOM_ALPHADITHER_SAME, true); break;
    case 70: dither_case(32, SOM_RGBDITHER_BAYER | SOM_ALPHADITHER_SAME, false); break;
    // ---- Blender ----
    case 71: blend_case(16, M1 | SOM_BLENDING | BL_ALPHA, true); break;
    case 72: blend_case(32, M1 | SOM_BLENDING | BL_ALPHA, true); break;
    case 73: blend_case(16, M1 | SOM_BLENDING | RDPQ_BLENDER((IN_RGB, SHADE_ALPHA, FOG_RGB, INV_MUX_ALPHA)), false); break;
    case 74: blend_case(16, M1 | SOM_BLENDING | RDPQ_BLENDER((IN_RGB, FOG_ALPHA, MEMORY_RGB, ONE)), false); break;
    case 75: blend_case(16, M1 | SOM_BLENDING | RDPQ_BLENDER((BLEND_RGB, IN_ALPHA, MEMORY_RGB, MEMORY_CVG)), false); break;
    case 76: blend_case(16, M1 | SOM_BLENDING | RDPQ_BLENDER((IN_RGB, ZERO, BLEND_RGB, ONE)), false); break;
    case 77: blend_case(16, M1 | RDPQ_BLENDER((IN_RGB, SHADE_ALPHA, FOG_RGB, INV_MUX_ALPHA)), false); break;  // no force-blend
    case 78:  // 2-cycle: fog, then alpha blend with memory
      begin(16);
      som(M2 | SOM_BLENDING | RDPQ_BLENDER2((IN_RGB, SHADE_ALPHA, FOG_RGB, INV_MUX_ALPHA), (CYCLE1_RGB, IN_ALPHA, MEMORY_RGB, INV_MUX_ALPHA)));
      cc(RDPQ_COMBINER2((0, 0, 0, SHADE), (0, 0, 0, SHADE), (0, 0, 0, COMBINED), (0, 0, 0, COMBINED)));
      GRAD; break;
    // ---- Coverage and anti-aliasing ----
    case 79: aa_case(16, 0); break;  // AA off
    case 80: aa_case(16, AA_STD | BL_AA | SOM_COVERAGE_DEST_CLAMP); break;
    case 81: aa_case(32, AA_STD | BL_AA | SOM_COVERAGE_DEST_CLAMP); break;
    case 82: aa_case(16, AA_STD | BL_AA | SOM_COVERAGE_DEST_WRAP); break;
    case 83: aa_case(16, AA_STD | BL_AA | SOM_COVERAGE_DEST_ZAP); break;
    case 84: aa_case(16, AA_STD | BL_AA | SOM_COVERAGE_DEST_SAVE); break;
    case 85: aa_case(16, (AA_STD & ~SOM_COLOR_ON_CVG_OVERFLOW) | BL_AA | SOM_COVERAGE_DEST_CLAMP); break;
    case 86: aa_case(16, SOM_AA_ENABLE | SOM_READ_ENABLE | SOM_BLALPHA_CVG_TIMES_CC | SOM_BLENDING | BL_ALPHA); break;
    case 87: aa_case(32, SOM_AA_ENABLE | SOM_COVERAGE_DEST_CLAMP | BL_AA); break;  // no read
    case 88:  // AA fill rectangles at subpixel corners in 1-cycle
      begin(16); som(M1 | AA_STD | BL_AA); cc(RDPQ_COMBINER_FLAT);
      fill_rect(Q(2) + 1, Q(3) + 2, Q(20) + 3, Q(17) + 1); fill_rect(Q(11) + 2, Q(9) + 3, Q(29) + 1, Q(30) + 2); break;
    // ---- Z ----
    case 89: z_case(16, ZCU, 0x1000, 0x5000); break;
    case 90: z_case(16, SOM_Z_COMPARE, 0x1000, 0x5000); break;
    case 91: z_case(16, SOM_Z_WRITE, 0x1000, 0x5000); break;
    case 92: z_case(16, ZCU | SOM_ZMODE_INTERPENETRATING | AA_STD | BL_AA, 0x2000, 0x2400); break;
    case 93: z_case(16, ZCU | SOM_ZMODE_TRANSPARENT | AA_STD | BL_AA, 0x2000, 0x2400); break;
    case 94: z_case(32, ZCU, 0x1000, 0x5000); break;
    case 95:  // primitive Z source on rectangles, two dz values
      begin(16); som(M1 | ZCU | SOM_ZSOURCE_PRIM); cc(RDPQ_COMBINER_FLAT);
      prim_depth(0x3000, 1); fill_rect(Q(2), Q(2), Q(22), Q(22));
      prim_depth(0x4800, 0x4000); fill_rect(Q(10), Q(10), Q(30), Q(30)); break;
    case 96:  // decal: a triangle, then the same plane in decal mode in another colour
      begin(16); som(M1 | ZCU); cc(RDPQ_COMBINER_SHADE);
      aa_pair(0xF0402080u, 0x30E060C0u, TRI_Z, 0x1800, 0x1800);
      som(M1 | SOM_Z_COMPARE | SOM_ZMODE_DECAL);
      aa_pair(0x2020F0FFu, 0xF0F020FFu, TRI_Z, 0x1800, 0x1800); break;
    case 97:  // intersecting triangles with steep and shallow Z (per-pixel deltaZ)
      begin(16); som(M1 | ZCU); cc(RDPQ_COMBINER_SHADE);
      tri(TRI_SHADE | TRI_Z, 0, 0, VZ(VC(0, 0, 0xFF0000FFu), 0x0400), VZ(VC(Q(32), Q(4), 0xFF0000FFu), 0x7C00), VZ(VC(Q(4), Q(32), 0xFF0000FFu), 0x0800));
      tri(TRI_SHADE | TRI_Z, 0, 0, VZ(VC(Q(30), 0, 0x00FF00FFu), 0x3C00), VZ(VC(Q(32), Q(30), 0x00FF00FFu), 0x3E00), VZ(VC(0, Q(26), 0x00FF00FFu), 0x3D00)); break;
    case 98:  // Z in 2-cycle with a textured, shaded triangle
      begin(16); som(M2 | ZCU);
      cc(RDPQ_COMBINER2((TEX0, 0, SHADE, 0), (0, 0, 0, SHADE), (COMBINED, 0, PRIM, 0), (0, 0, 0, COMBINED)));
      use_tex(TX_RGBA16, 0, 0, 0, WRAP4, WRAP4);
      { rr_vtx a = VZ(VT(Q(1), Q(1), 0, 0), 0x0800), b = VZ(VT(Q(31), Q(6), S5(16), 0), 0x6000), c = VZ(VT(Q(6), Q(31), 0, S5(16)), 0x3000);
        a.r = 255; b.g = 64; c.b = 32; a.a = 0x80;
        tri(TRI_SHADE | TRI_TEX | TRI_Z, 0, 0, a, b, c); }
      break;
    // ---- Triangles ----
    case 99: begin(16); som(M1); cc(RDPQ_COMBINER_SHADE);
      tri(TRI_SHADE, 0, 0, VC(Q(2), Q(1), 0xFF000080u), VC(Q(30), Q(9), 0x00FF00FFu), VC(Q(9), Q(31), 0x0000FF40u)); break;
    case 100: begin(32); som(M1); cc(RDPQ_COMBINER_SHADE);
      tri(TRI_SHADE, 0, 0, VC(Q(2), Q(1), 0xFF000080u), VC(Q(30), Q(9), 0x00FF00FFu), VC(Q(9), Q(31), 0x0000FF40u)); break;
    case 101:  // textured, point, rotated coordinates
      begin(16); som(M1); cc(RDPQ_COMBINER_TEX); use_tex(TX_RGBA16, 0, 0, 0, WRAP4, WRAP4);
      tri(TRI_TEX, 0, 0, VT(Q(2), Q(1), S5(4), 0), VT(Q(30), Q(9), S5(20), S5(9)), VT(Q(9), Q(31), 0, S5(18))); break;
    case 102:  // textured, bilinear, rotated
      begin(16); som(M1 | SOM_SAMPLE_BILINEAR); cc(RDPQ_COMBINER_TEX); use_tex(TX_RGBA16, 0, 0, 0, WRAP4, WRAP4);
      tri(TRI_TEX, 0, 0, VT(Q(2), Q(1), S5(4), 0), VT(Q(30), Q(9), S5(20), S5(9)), VT(Q(9), Q(31), 0, S5(18))); break;
    case 103:  // perspective-correct quad, w from 1.0 at the top to 0.25 at the bottom
      begin(16); som(M1 | SOM_TEXTURE_PERSP | SOM_SAMPLE_BILINEAR); cc(RDPQ_COMBINER_TEX);
      use_tex(TX_RGBA16, 0, 0, 0, WRAP4, WRAP4);
      tex_quad(TRI_TEX, 0, 0, 0, S5(16), S5(16), 0x7FFF0000, 0x1FFF0000); break;
    case 104:  // shade x texture with Z
      begin(16); som(M1 | ZCU); cc(RDPQ_COMBINER_TEX_SHADE); use_tex(TX_RGBA16, 0, 0, 0, WRAP4, WRAP4);
      { rr_vtx a = VZ(VT(Q(2), Q(1), S5(4), 0), 0x0400), b = VZ(VT(Q(30), Q(9), S5(20), S5(9)), 0x7000), c = VZ(VT(Q(9), Q(31), 0, S5(18)), 0x3800);
        b.r = 0x40; c.g = 0x80;
        tri(TRI_SHADE | TRI_TEX | TRI_Z, 0, 0, a, b, c); }
      break;
    case 105:  // right-major (middle vertex left of the major edge), subpixel vertices
      begin(16); som(M1); cc(RDPQ_COMBINER_SHADE);
      tri(TRI_SHADE, 0, 0, VC(Q(20) + 1, Q(1) + 2, 0xFFFF00FFu), VC(Q(2) + 3, Q(16) + 1, 0x00FFFFFFu), VC(Q(29) + 2, Q(30) + 3, 0xFF00FFFFu)); break;
    case 106:  // sliver
      begin(16); som(M1); cc(RDPQ_COMBINER_FLAT);
      tri(0, 0, 0, V(Q(1), Q(2)), V(Q(31), Q(29)), V(Q(2) + 2, Q(5))); break;
    case 107:  // vertices outside the scissor on every side
      begin(16); som(M1); cc(RDPQ_COMBINER_SHADE);
      tri(TRI_SHADE, 0, 0, VC(Q(-10), Q(-6), 0xFF0000FFu), VC(Q(50), Q(12), 0x00FF00FFu), VC(Q(8), Q(44), 0x0000FFFFu)); break;
    case 108:  // flat top and flat bottom
      begin(16); som(M1); cc(RDPQ_COMBINER_SHADE);
      tri(TRI_SHADE, 0, 0, VC(Q(3), Q(2), 0xFF0000FFu), VC(Q(28), Q(2), 0x00FF00FFu), VC(Q(14), Q(15), 0x0000FFFFu));
      tri(TRI_SHADE, 0, 0, VC(Q(15), Q(17), 0xFFFF00FFu), VC(Q(30), Q(30), 0x00FFFFFFu), VC(Q(1), Q(30), 0xFF00FFFFu)); break;
    // ---- LOD ----
    case 109: lod_case(M1, RDPQ_COMBINER_TEX, 1, 0); break;
    case 110: lod_case(M1, RDPQ_COMBINER_TEX, 2, 0); break;
    case 111: lod_case(M1, RDPQ_COMBINER_TEX, 4, 0); break;
    case 112: lod_case(M1 | SOM_TEXTURE_PERSP, RDPQ_COMBINER_TEX, 2, 0x0FFF0000); break;
    case 113:  // trilinear: blend TEX0 and TEX1 by LOD_FRAC
      lod_case(M2 | SOM_TEXTURE_PERSP | SOM_SAMPLE_BILINEAR,
               RDPQ_COMBINER2((TEX1, TEX0, LOD_FRAC, TEX0), (TEX1, TEX0, LOD_FRAC, TEX0), (0, 0, 0, COMBINED), (0, 0, 0, COMBINED)), 2, 0x0FFF0000);
      break;
    case 114: lod_case(M2 | SOM_TEXTURE_SHARPEN | SOM_TEXTURE_PERSP,
                       RDPQ_COMBINER2((TEX1, TEX0, LOD_FRAC, TEX0), (0, 0, 0, TEX0), (0, 0, 0, COMBINED), (0, 0, 0, COMBINED)), 1, 0x2FFF0000); break;
    case 115: lod_case(M2 | SOM_TEXTURE_DETAIL | SOM_TEXTURE_PERSP,
                       RDPQ_COMBINER2((TEX1, TEX0, LOD_FRAC, TEX0), (0, 0, 0, TEX0), (0, 0, 0, COMBINED), (0, 0, 0, COMBINED)), 1, 0x2FFF0000); break;
    case 116:  // LOD enabled on a texture rectangle stepping 4 texels per pixel
      begin(16); som(M1 | SOM_TEXTURE_LOD); cc(RDPQ_COMBINER_TEX); use_mips();
      tex_rect(0, 0, 0, Q(RR_W), Q(RR_H), 0, 0, D10(4, 1), D10(4, 1), false); break;
    // ---- Scissor fields ----
    case 117: begin(16); som(M1); cc(RDPQ_COMBINER_SHADE); scissor(0, 0, Q(RR_W), Q(RR_H), 1, 0); GRAD; break;
    case 118: begin(16); som(M1); cc(RDPQ_COMBINER_SHADE); scissor(0, 0, Q(RR_W), Q(RR_H), 1, 1); GRAD; break;
    // ---- Hazards last: n64brew lists their results as unknown ----
    case 119:  // I4 copied into an 8 bpp image
      begin(8); som(MCOPY); use_tex(TX_I4, 0, 0, 0, WRAP4, WRAP4);
      tex_rect(0, Q(8), Q(8), Q(23), Q(23), 0, 0, D10(8, 1), D10(1, 1), false); break;
    case 120:  // 1-cycle into an 8 bpp image
      begin(8); som(M1); cc(RDPQ_COMBINER_SHADE); GRAD; break;
    default: return false;
  }
  return true;
}

// ---- Running a case ----
static uint32_t rr_info[8 * 128];
static uint8_t rr_dump[RR_W * RR_H * 4] __attribute__((aligned(16)));

// The CPU's initial images: hashed colour (so blends and coverage reads see varied memory) and
// a Z ramp across x that spans every exponent, with varied dz bits.
static uint32_t init_pixel(int bpp, uint32_t x, uint32_t y) {
  const uint32_t h = hash32(x | y << 8 | (uint32_t)bpp << 16);
  return bpp == 32 ? h : bpp == 16 ? (h & 0xFFFF) : (h & 0xFF);
}
static uint16_t init_z(uint32_t x, uint32_t y) { return (uint16_t)(x * 2048 + y * 60 + ((x + y) & 3)); }

static void init_images(int bpp) {
  volatile uint8_t* fb = KSEG1(RR_FB_PHYS);
  volatile uint16_t* zb = KSEG1(RR_ZB_PHYS);
  for (uint32_t y = 0; y < RR_H; ++y)
    for (uint32_t x = 0; x < RR_W; ++x) {
      const uint32_t i = y * RR_W + x, v = init_pixel(bpp, x, y);
      if (bpp == 32) ((volatile uint32_t*)fb)[i] = v;
      else if (bpp == 16) ((volatile uint16_t*)fb)[i] = (uint16_t)v;
      else fb[i] = (uint8_t)v;
      zb[i] = init_z(x, y);
    }
}

static void record_init(void) {
  for (int b = 0; b < 3; ++b) {
    const int bpp = b == 0 ? 16 : b == 1 ? 32 : 8;
    for (uint32_t i = 0; i < RR_W * RR_H; ++i) {
      const uint32_t v = init_pixel(bpp, i % RR_W, i / RR_W);
      if (bpp == 32) ((uint32_t*)rr_dump)[i] = v;
      else if (bpp == 16) ((uint16_t*)rr_dump)[i] = (uint16_t)v;
      else rr_dump[i] = (uint8_t)v;
    }
    rec_series_full(b == 0 ? S_RDPR_INIT16 : b == 1 ? S_RDPR_INIT32 : S_RDPR_INIT8, (uint8_t)(bpp / 8), RR_W * RR_H, rr_dump);
  }
  for (uint32_t i = 0; i < RR_W * RR_H; ++i) ((uint16_t*)rr_dump)[i] = init_z(i % RR_W, i / RR_W);
  rec_series_full(S_RDPR_INITZ, 2, RR_W * RR_H, rr_dump);
}

// Sends rr_cmd plus sync pipe and sync full from RDRAM and waits for the DP interrupt.
// o: Count ticks to the interrupt (HWP_TIMEOUT), DPC_STATUS, DPC_CLOCK.
static void rr_run(uint32_t o[3]) {
  rr_cmd[rr_n++] = op(OP_SYNC_PIPE);
  rr_cmd[rr_n++] = op(OP_SYNC_FULL);
  dcache_wb(rr_cmd, rr_n * 8);
  uint32_t t0 = c0_count();
  while ((RR_DP(RR_DPC_STATUS) & RR_DPC_STATUS_DMA_BUSY) && c0_count() - t0 < RR_TIMEOUT_TICKS) {}
  RR_DP(RR_DPC_STATUS) = RR_DPC_STATUS_RESET;
  RR_MI_MODE = RR_MI_MODE_CLR_DP;
  RR_DP(RR_DPC_START) = PHYS(rr_cmd);
  t0 = c0_count();
  RR_DP(RR_DPC_END) = PHYS(rr_cmd + rr_n);
  o[0] = HWP_TIMEOUT;
  while (c0_count() - t0 < RR_TIMEOUT_TICKS)
    if (RR_MI_INTR & RR_MI_INTR_DP) {
      o[0] = c0_count() - t0;
      break;
    }
  o[1] = RR_DP(RR_DPC_STATUS);
  o[2] = RR_DP(RR_DPC_CLOCK);
  RR_MI_MODE = RR_MI_MODE_CLR_DP;
}

static void read_back(uint32_t phys, uint32_t bytes) {
  volatile uint32_t* src = KSEG1(phys);
  for (uint32_t i = 0; i < bytes / 4; ++i) ((uint32_t*)rr_dump)[i] = src[i];
}

void grp_rdp_render(void* unused) {
  (void)unused;
  if (!sink_active()) return;  // everything goes to the full record
  tex_generate();
  uint32_t texinfo[5 * TX_COUNT];
  for (int k = 0; k < TX_COUNT; ++k) {
    const uint8_t es = rr_tex[k].size == SZ_32 ? 4 : rr_tex[k].size == SZ_16 ? 2 : 1;
    rec_series_full(S_RDPR_TEX_ID(k), es, tex_bytes(k) / es, (const void*)(0x80000000u | tex_phys(k)));
    const uint32_t row[5] = {rr_tex[k].fmt, rr_tex[k].size, rr_tex[k].w, rr_tex[k].h, tex_phys(k)};
    memcpy(&texinfo[5 * k], row, sizeof row);
  }
  rec_series_full(S_RDPR_TEXINFO, 4, 5 * TX_COUNT, texinfo);
  record_init();
  int n = 0;
  for (; n < 128 && build(n); ++n) {
    init_images(rr_bpp);
    uint32_t* inf = &rr_info[8 * n];
    inf[0] = (uint32_t)n;
    inf[1] = (uint32_t)rr_bpp;
    inf[2] = RR_W;
    inf[3] = RR_H;
    inf[4] = (rr_zuse ? 1u : 0u) | (rr_trunc ? 2u : 0u);
    rr_run(&inf[5]);
    const uint32_t bytes = RR_W * RR_H * (uint32_t)rr_bpp / 8;
    read_back(RR_FB_PHYS, bytes);
    rec_series_full(S_RDPR_COLOR_ID(n), (uint8_t)(rr_bpp / 8), RR_W * RR_H, rr_dump);
    if (rr_zuse) {
      read_back(RR_ZB_PHYS, RR_W * RR_H * 2);
      rec_series_full(S_RDPR_Z_ID(n), 2, RR_W * RR_H, rr_dump);
    }
    rec_series_full(S_RDPR_CMD_ID(n), 8, rr_n, rr_cmd);
  }
  rec_series_full(S_RDPR_INFO, 4, 8 * (uint32_t)n, rr_info);
}
