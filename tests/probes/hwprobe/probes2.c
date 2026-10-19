// Groups added for a complete dump: instruction retirement, cache/TLB refill, register access
// latency, SP DMA, RSP microcode timing, RDP command cost, RDRAM contention and VI HSYNC.
// docs/timing/hwprobe.md describes each. Every wait on the RSP or RDP is bounded, so a run on
// an emulator without them still completes (the series then hold HWP_TIMEOUT).
#include <libdragon.h>
#include <rdpq_macros.h>
#include <string.h>

#include "hwp.h"

#define TIMEOUT_TICKS(ms) ((uint32_t)(ms) * 46875u)  // Count runs at 46.875 MHz

static uint32_t out[2048];

// ---- probes2_asm.S ----
typedef uint32_t rt_fn(uint32_t*, uint32_t);
extern rt_fn* const hwp_rt_table[];
extern const uint32_t hwp_rt_count[];  // unsized: not in small data
extern uint32_t hwp_icold(int cold);
extern uint32_t hwp_dfill(uint32_t* lines);
extern uint32_t hwp_tlbx_load(uint32_t vaddr);
extern uint32_t hwp_rd64(uint32_t addr);
extern uint32_t hwp_wr64(uint32_t addr, uint32_t value, uint32_t drain);
extern void hwp_tlb_write(int index, const uint64_t e[4]);
extern uint32_t hwp_rd_load(uint32_t base, uint32_t stride, uint32_t mask);

// ---------------------------------------------------------------------------------------------
static uint32_t rt_buf[4] __attribute__((aligned(16)));

void grp_retire(void* unused) {
  (void)unused;
  const uint32_t n = hwp_rt_count[0];
  for (uint32_t c = 0; c < n; ++c)
    for (int r = 0; r < 8; ++r) out[8 * c + r] = hwp_rt_table[c](rt_buf, 0xA0240000u);
  rec_series(S_RETIRE, 4, 8 * n, out);
}

// ---------------------------------------------------------------------------------------------
static uint32_t df_lines[32 * 4] __attribute__((aligned(16)));

void grp_refill(void* unused) {
  (void)unused;
  uint32_t n = 0;
  hwp_icold(0);
  for (int r = 0; r < 16; ++r) out[n++] = hwp_icold(1);  // 32 I-cache misses
  for (int r = 0; r < 16; ++r) out[n++] = hwp_icold(0);  // all hits
  for (int r = 0; r < 16; ++r) {
    dcache_wbinv(df_lines, sizeof df_lines);
    out[n++] = hwp_dfill(df_lines);                       // 32 D-cache misses
  }
  for (int r = 0; r < 16; ++r) out[n++] = hwp_dfill(df_lines);  // all hits

  // TLB: a load that refills (no entry), one that hits, one that finds V = 0. The refill and
  // invalid exceptions go to minimal handlers at 0x000, 0x080 and 0x180.
  uint64_t asid;
  __asm__ volatile("dmfc0 %0, $10" : "=r"(asid));
  asid &= 0xFF;
  const uint32_t phys = PHYS(df_lines) & ~0xFFFu;  // even page of each entry
  const uint64_t hit[4] = {0, 0x00500000u | asid, ((uint64_t)(phys >> 12) << 6) | (3 << 3) | 6,
                           0};
  const uint64_t inv[4] = {0, 0x00502000u | asid, ((uint64_t)(phys >> 12) << 6) | (3 << 3) | 4,
                           0};
  hwp_tlb_write(24, hit);
  hwp_tlb_write(25, inv);
  hwp_vector_minimal_at(0x000, true);
  hwp_vector_minimal_at(0x080, true);
  hwp_vector_minimal_at(0x180, true);
  const uint32_t off = PHYS(df_lines) & 0x0FFFu;
  for (int r = 0; r < 16; ++r) out[n++] = hwp_tlbx_load(0x00600000u);        // refill
  for (int r = 0; r < 16; ++r) out[n++] = hwp_tlbx_load(0x00500000u + off);  // hit
  for (int r = 0; r < 16; ++r) out[n++] = hwp_tlbx_load(0x00502000u + off);  // invalid
  hwp_vector_minimal_at(0x000, false);
  hwp_vector_minimal_at(0x080, false);
  hwp_vector_minimal_at(0x180, false);
  // What each TLB case raised, through the full handler.
  const uint32_t va[3] = {0x00600000u, 0x00500000u + off, 0x00502000u + off};
  for (int k = 0; k < 3; ++k) {
    hwp_exc_reset(HWP_EXPECT_RESUME);
    hwp_tlbx_load(va[k]);
    out[n++] = hwp_exc.n ? hwp_exc.first_cause : 0xFFFFFFFFu;
  }
  hwp_tlb_write(24, (const uint64_t[4]){0, 0x00A10000u, 0, 0});
  hwp_tlb_write(25, (const uint64_t[4]){0, 0x00A12000u, 0, 0});
  rec_series(S_REFILL, 4, n, out);
}

// ---------------------------------------------------------------------------------------------
// Register access latency. Reads and writes are chosen to have no side effects beyond clearing
// an interrupt that is not in use.
static const uint32_t rd_addr[] = {
    0xA4400010u /* VI_V_CURRENT */, 0xA4400000u /* VI_CTRL */,    0xA4300004u /* MI_VERSION */,
    0xA4300008u /* MI_INTR */,      0xA4040010u /* SP_STATUS */,  0xA4040018u /* SP_DMA_BUSY */,
    0xA4000000u /* DMEM */,         0xA4001000u /* IMEM */,       0xA410000Cu /* DP_STATUS */,
    0xA4100010u /* DP_CLOCK */,     0xA4500004u /* AI_LEN */,     0xA450000Cu /* AI_STATUS */,
    0xA4600010u /* PI_STATUS */,    0xA4600014u /* PI_BSD_DOM1_LAT */, 0xA4700000u /* RI_MODE */,
    0xA4700010u /* RI_REFRESH */,   0xA4800018u /* SI_STATUS */,  0xBFC007FCu /* PIF RAM */,
    0xB0000000u /* ROM */,          0xA0240000u /* RDRAM */,      0xA3F00000u /* RDRAM regs */};
static const uint32_t wr_addr[] = {
    0xA4400010u /* VI_V_CURRENT: ack */, 0xA4300000u /* MI_MODE: 0 */, 0xA404001Cu /* SP_SEMAPHORE */,
    0xA410000Cu /* DP_STATUS: 0 */,      0xA450000Cu /* AI_STATUS: ack */, 0xA4600010u /* PI_STATUS: 0 */,
    0xA4800018u /* SI_STATUS: ack */,    0xA0240000u /* RDRAM */,       0xA4000F00u /* DMEM */};

void grp_reglat(void* unused) {
  (void)unused;
  hwp_vi_mode(HWP_VI_OFF);
  const uint32_t nr = sizeof rd_addr / sizeof rd_addr[0], nw = sizeof wr_addr / sizeof wr_addr[0];
  uint32_t n = 0;
  for (uint32_t i = 0; i < nr; ++i)
    for (int r = 0; r < 8; ++r) out[n++] = hwp_rd64(rd_addr[i]);
  rec_series(S_REG_READ, 4, n, out);
  rec_series(S_REG_READ_ADDR, 4, nr, rd_addr);
  n = 0;
  for (uint32_t i = 0; i < nw; ++i)
    for (int r = 0; r < 8; ++r) out[n++] = hwp_wr64(wr_addr[i], 0, 0xA0240000u);
  rec_series(S_REG_WRITE, 4, n, out);
  rec_series(S_REG_WRITE_ADDR, 4, nw, wr_addr);
  hwp_screen_show();
}

// ---------------------------------------------------------------------------------------------
// SP registers (n64brew "Reality Signal Processor/Interface").
#define SP_REG(i) (((volatile uint32_t*)0xA4040000u)[i])
enum { SPR_MEM = 0, SPR_DRAM, SPR_RD, SPR_WR, SPR_STATUS, SPR_FULL, SPR_BUSY, SPR_SEM };
#define HSP_PC (*(volatile uint32_t*)0xA4080000u)
#define SPS_HALT 0x1u
#define SPW_CLR_HALT 0x1u
#define SPW_SET_HALT 0x2u
#define SPW_CLR_BROKE 0x4u
#define SPW_CLR_INTR 0x8u
#define SPW_CLR_INTBREAK 0x80u

static bool sp_wait_dma(uint32_t timeout) {
  const uint32_t t0 = c0_count();
  while (SP_REG(SPR_BUSY) | SP_REG(SPR_FULL))
    if (c0_count() - t0 > timeout) return false;
  return true;
}

static uint8_t sp_buf[8192] __attribute__((aligned(16)));

// One CPU-initiated SP DMA; returns Count ticks from the length write to DMA_BUSY clear.
static uint32_t sp_dma(bool to_sp, uint32_t sp_addr, uint32_t len_reg) {
  if (!sp_wait_dma(TIMEOUT_TICKS(5))) return HWP_TIMEOUT;
  SP_REG(SPR_MEM) = sp_addr;
  SP_REG(SPR_DRAM) = PHYS(sp_buf);
  const uint32_t t0 = c0_count();
  SP_REG(to_sp ? SPR_RD : SPR_WR) = len_reg;
  while (SP_REG(SPR_BUSY))
    if (c0_count() - t0 > TIMEOUT_TICKS(5)) return HWP_TIMEOUT;
  return c0_count() - t0;
}

void grp_sp_dma(void* unused) {
  (void)unused;
  SP_REG(SPR_STATUS) = SPW_SET_HALT;
  dcache_wbinv(sp_buf, sizeof sp_buf);
  static const uint32_t sizes[] = {8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
  // Length register: (skip << 20) | (count << 12) | (length - 1).
  static const uint32_t shaped[] = {(64u << 20) | (7u << 12) | 63u, (8u << 20) | (63u << 12) | 7u,
                                    (0u << 20) | (7u << 12) | 511u};
  uint32_t n = 0;
  for (int dir = 0; dir < 2; ++dir) {
    for (uint32_t z = 0; z < sizeof sizes / sizeof sizes[0]; ++z)
      for (int r = 0; r < 8; ++r) out[n++] = sp_dma(dir == 0, 0x0000, sizes[z] - 1);
    for (uint32_t z = 0; z < sizeof shaped / sizeof shaped[0]; ++z)
      for (int r = 0; r < 8; ++r) out[n++] = sp_dma(dir == 0, 0x0000, shaped[z]);
    // IMEM as the target or source.
    for (int r = 0; r < 8; ++r) out[n++] = sp_dma(dir == 0, 0x1000, 4096 - 1);
  }
  rec_series(S_SP_DMA, 4, n, out);
  uint32_t cfg[16];
  uint32_t m = 0;
  for (uint32_t z = 0; z < sizeof sizes / sizeof sizes[0]; ++z) cfg[m++] = sizes[z] - 1;
  for (uint32_t z = 0; z < sizeof shaped / sizeof shaped[0]; ++z) cfg[m++] = shaped[z];
  rec_series(S_SP_DMA_CONFIG, 4, m, cfg);
}

// ---------------------------------------------------------------------------------------------
// RSP microcode (rsp_probe.S). DMEM layout: case table at 0x00, PARAMS at 0x60, RESULT at 0x70.
extern uint8_t rsp_probe_text_start[], rsp_probe_text_end[];
extern uint8_t rsp_probe_data_start[], rsp_probe_data_end[];
#define DMEM ((volatile uint32_t*)0xA4000000u)
#define RSP_PARAMS (0x60 / 4)
#define RSP_RESULT (0x70 / 4)
#define RSP_CASES 20
#define RSP_CASE_DMA_IN 17
#define RSP_CASE_DMA_OUT 18
#define RSP_CASE_DMA_LOOP 19

static bool hwp_rsp_load(void) {
  SP_REG(SPR_STATUS) = SPW_SET_HALT;
  const uint32_t tlen = rsp_probe_text_end - rsp_probe_text_start;
  const uint32_t dlen = rsp_probe_data_end - rsp_probe_data_start;
  if (tlen > 4096 || dlen > 4096) return false;
  memcpy(sp_buf, rsp_probe_text_start, tlen);
  if (sp_dma(true, 0x1000, ((tlen + 7) & ~7u) - 1) == HWP_TIMEOUT) return false;
  memcpy(sp_buf, rsp_probe_data_start, dlen);
  return sp_dma(true, 0x0000, ((dlen + 7) & ~7u) - 1) != HWP_TIMEOUT;
}

static void hwp_rsp_halt(void) {
  SP_REG(SPR_STATUS) = SPW_SET_HALT;
  const uint32_t t0 = c0_count();
  while (!(SP_REG(SPR_STATUS) & SPS_HALT))
    if (c0_count() - t0 > TIMEOUT_TICKS(1)) break;
}

// Runs one case: out[0] = Count ticks from clearing HALT to seeing it set again (or
// HWP_TIMEOUT), out[1] = the case's DP_CLOCK delta, out[2] = SP_STATUS at the end.
static void hwp_rsp_run(int c, uint32_t iters, uint32_t* o) {
  hwp_rsp_halt();
  DMEM[RSP_PARAMS] = iters;
  DMEM[RSP_RESULT] = 0xFFFFFFFFu;
  HSP_PC = DMEM[c] & 0xFFC;
  SP_REG(SPR_STATUS) = SPW_CLR_BROKE | SPW_CLR_INTR | SPW_CLR_INTBREAK;
  const uint32_t t0 = c0_count();
  SP_REG(SPR_STATUS) = SPW_CLR_HALT;
  uint32_t t = HWP_TIMEOUT;
  for (;;) {
    if (SP_REG(SPR_STATUS) & SPS_HALT) {
      t = c0_count() - t0;
      break;
    }
    if (c0_count() - t0 > TIMEOUT_TICKS(5)) break;
  }
  o[0] = t;
  o[1] = DMEM[RSP_RESULT];
  o[2] = SP_REG(SPR_STATUS);
  hwp_rsp_halt();
}

static bool rsp_ok;

void grp_rsp(void* unused) {
  (void)unused;
  rsp_ok = hwp_rsp_load();
  uint32_t n = 0;
  for (int c = 0; c < RSP_CASE_DMA_IN; ++c)
    for (int r = 0; r < 4; ++r, n += 3) hwp_rsp_run(c, 256, &out[n]);
  rec_series(S_RSP_CASE, 4, n, out);

  // RSP-initiated DMA: 32 transfers per run, both directions, four lengths.
  static const uint32_t lens[4] = {8, 128, 1024, 3072};
  n = 0;
  DMEM[RSP_PARAMS + 1] = PHYS(sp_buf);
  for (int dir = 0; dir < 2; ++dir)
    for (int z = 0; z < 4; ++z) {
      DMEM[RSP_PARAMS + 2] = lens[z] - 1;
      for (int r = 0; r < 4; ++r, n += 3)
        hwp_rsp_run(dir ? RSP_CASE_DMA_OUT : RSP_CASE_DMA_IN, 32, &out[n]);
    }
  rec_series(S_RSP_DMA, 4, n, out);
  const uint32_t info[4] = {rsp_ok, 256, RSP_CASES,
                            (uint32_t)(rsp_probe_text_end - rsp_probe_text_start)};
  rec_series(S_RSP_INFO, 4, 4, info);
}

// ---------------------------------------------------------------------------------------------
// RDP: command lists built here and sent from RDRAM. Encodings: n64brew "Reality Display
// Processor/Commands"; other-mode and combiner fields from libdragon's rdpq_macros.h.
#define DP_REG(i) (((volatile uint32_t*)0xA4100000u)[i])
enum { DPR_START = 0, DPR_END, DPR_CURRENT, DPR_STATUS, DPR_CLOCK, DPR_BUFBUSY, DPR_PIPEBUSY, DPR_TMEM };
// DP_STATUS write: clear XBUS, FREEZE, FLUSH and the four counters.
#define DPW_RESET 0x3D5u
#define MI_INTR_REG (*(volatile uint32_t*)0xA4300008u)
#define MI_MODE_REG (*(volatile uint32_t*)0xA4300000u)
#define MI_INTR_DP 0x20u
#define MI_MODE_CLR_DP 0x800u

#define FB_PHYS  0x00100000u  // 320x240, 16 or 32 bpp
#define ZB_PHYS  0x00180000u
#define TEX_PHYS 0x001C0000u
#define LOAD_MASK 0x00FFFFFFFFFFFFFFull

static uint64_t cmd_buf[1024] __attribute__((aligned(16)));
static uint32_t ncmd;

static void C(uint64_t w) { cmd_buf[ncmd++] = w; }
static uint64_t xy(uint32_t x, uint32_t y) { return ((uint64_t)(x * 4) << 12) | (y * 4); }

static void setup(uint32_t bpp32, uint64_t modes, uint64_t combine) {
  ncmd = 0;
  C((0x3Full << 56) | ((uint64_t)0 << 53) | ((uint64_t)(bpp32 ? 3 : 2) << 51) | ((uint64_t)319 << 32) | FB_PHYS);
  C((0x3Eull << 56) | ZB_PHYS);
  C((0x2Dull << 56) | (xy(0, 0) << 32) | xy(320, 240));
  C((0x2Full << 56) | (modes & LOAD_MASK));
  C((0x3Cull << 56) | (combine & LOAD_MASK));
  C((0x37ull << 56) | 0x12345678u);              // fill color
  C((0x3Aull << 56) | 0x80808080u);              // prim color
  C((0x3Dull << 56) | ((uint64_t)0 << 53) | ((uint64_t)2 << 51) | ((uint64_t)31 << 32) | TEX_PHYS);
  // Tile 0: RGBA16, 32 texels per line = 8 qwords, TMEM 0.
  C((0x35ull << 56) | ((uint64_t)0 << 53) | ((uint64_t)2 << 51) | ((uint64_t)8 << 41));
  C((0x32ull << 56) | (xy(0, 0) << 32) | xy(31, 31));  // tile size
}

static void fill_rect(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
  C((0x36ull << 56) | (xy(x1, y1) << 32) | xy(x0, y0));
}
static void load_tile(void) {
  C(0x26ull << 56);                                     // sync load
  C((0x34ull << 56) | (xy(0, 0) << 32) | xy(31, 31));
}
static void tex_rect(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, bool copy) {
  C((0x24ull << 56) | (xy(x1, y1) << 32) | xy(x0, y0));
  const uint32_t dsdx = copy ? (4u << 10) : (1u << 10), dtdy = 1u << 10;
  C(((uint64_t)dsdx << 16) | dtdy);  // s = t = 0
}
// Flat triangle (command 0x08): (x, y) top, straight down to (x, y + h), across to (x + w, y + h).
static void fill_tri(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
  const uint32_t yh = y * 4, ym = (y + h) * 4, yl = (y + h) * 4;
  const int32_t slope = (int32_t)((w << 16) / h);  // dx/dy of the edge to the bottom right
  C((0x08ull << 56) | (1ull << 55) | ((uint64_t)yl << 32) | ((uint64_t)ym << 16) | yh);
  C(((uint64_t)((x + w) << 16) << 32) | 0);          // XL, DxLDy
  C(((uint64_t)(x << 16) << 32) | 0);                // XH, DxHDy: the vertical major edge
  C(((uint64_t)(x << 16) << 32) | (uint32_t)slope);  // XM, DxMDy
}

// Sends cmd_buf[0..ncmd) plus a full sync, waits for the DP interrupt. o: Count, CLOCK, BUFBUSY,
// PIPEBUSY, TMEM, STATUS at the end.
static void rdp_run(uint32_t* o) {
  C(0x27ull << 56);  // sync pipe
  C(0x29ull << 56);  // sync full
  dcache_wb(cmd_buf, ncmd * 8);
  uint32_t t0 = c0_count();
  while (DP_REG(DPR_STATUS) & 0x100u)  // DMA busy
    if (c0_count() - t0 > TIMEOUT_TICKS(20)) break;
  DP_REG(DPR_STATUS) = DPW_RESET;
  MI_MODE_REG = MI_MODE_CLR_DP;
  DP_REG(DPR_START) = PHYS(cmd_buf);
  t0 = c0_count();
  DP_REG(DPR_END) = PHYS(cmd_buf + ncmd);
  uint32_t t = HWP_TIMEOUT;
  while (c0_count() - t0 < TIMEOUT_TICKS(20)) {
    if (MI_INTR_REG & MI_INTR_DP) {
      t = c0_count() - t0;
      break;
    }
  }
  o[0] = t;
  o[1] = DP_REG(DPR_CLOCK);
  o[2] = DP_REG(DPR_BUFBUSY);
  o[3] = DP_REG(DPR_PIPEBUSY);
  o[4] = DP_REG(DPR_TMEM);
  o[5] = DP_REG(DPR_STATUS);
  MI_MODE_REG = MI_MODE_CLR_DP;
}

// The S_RDP configurations, in order.
static void rdp_build(int k) {
  const uint64_t tex1 = SOM_CYCLE_1 | SOM_SAMPLE_POINT | SOM_TF0_RGB;
  switch (k) {
    case 0: setup(0, SOM_CYCLE_FILL, 0); break;  // setup + sync only
    case 1: setup(0, SOM_CYCLE_FILL, 0); for (int i = 0; i < 64; ++i) fill_rect(0, 0, 15, 15); break;
    case 2: setup(0, SOM_CYCLE_FILL, 0); for (int i = 0; i < 16; ++i) fill_rect(0, 0, 63, 63); break;
    case 3: setup(0, SOM_CYCLE_FILL, 0); for (int i = 0; i < 4; ++i) fill_rect(0, 0, 319, 239); break;
    case 4: setup(1, SOM_CYCLE_FILL, 0); for (int i = 0; i < 16; ++i) fill_rect(0, 0, 63, 63); break;
    case 5: setup(0, SOM_CYCLE_COPY, 0); for (int i = 0; i < 16; ++i) load_tile(); break;
    case 6:
      setup(0, SOM_CYCLE_COPY, 0);
      load_tile();
      for (int i = 0; i < 16; ++i) tex_rect(0, 0, 31, 31, true);
      break;
    case 7:
      setup(0, tex1, RDPQ_COMBINER_TEX);
      load_tile();
      for (int i = 0; i < 16; ++i) tex_rect(0, 0, 32, 32, false);
      break;
    case 8:
      setup(0, tex1 | SOM_Z_COMPARE | SOM_Z_WRITE, RDPQ_COMBINER_TEX);
      load_tile();
      for (int i = 0; i < 16; ++i) tex_rect(0, 0, 32, 32, false);
      break;
    case 9:
      setup(0, tex1 | SOM_READ_ENABLE | SOM_BLENDING | RDPQ_BLENDER_MULTIPLY, RDPQ_COMBINER_TEX);
      load_tile();
      for (int i = 0; i < 16; ++i) tex_rect(0, 0, 32, 32, false);
      break;
    case 10:
      setup(0, SOM_CYCLE_2 | SOM_SAMPLE_POINT | SOM_TF0_RGB,
            RDPQ_COMBINER2((0, 0, 0, TEX0), (0, 0, 0, TEX0), (0, 0, 0, COMBINED), (0, 0, 0, COMBINED)));
      load_tile();
      for (int i = 0; i < 16; ++i) tex_rect(0, 0, 32, 32, false);
      break;
    case 11:
      setup(0, SOM_CYCLE_1, RDPQ_COMBINER_FLAT);
      for (int i = 0; i < 16; ++i) fill_tri(10, 10, 100, 100);
      break;
    case 12:
      setup(0, SOM_CYCLE_1, RDPQ_COMBINER_FLAT);
      for (int i = 0; i < 64; ++i) fill_tri(10, 10, 10, 10);
      break;
  }
}
#define RDP_CONFIGS 13

void grp_rdp(void* unused) {
  (void)unused;
  uint32_t n = 0;
  for (int k = 0; k < RDP_CONFIGS; ++k)
    for (int r = 0; r < 4; ++r, n += 6) {
      rdp_build(k);
      rdp_run(&out[n]);
    }
  rec_series(S_RDP, 4, n, out);
  uint32_t words[RDP_CONFIGS];
  for (int k = 0; k < RDP_CONFIGS; ++k) {
    rdp_build(k);
    words[k] = ncmd + 2;
  }
  rec_series(S_RDP_INFO, 4, RDP_CONFIGS, words);
}

// ---------------------------------------------------------------------------------------------
// RDRAM contention: uncached loads with the RSP DMA engine streaming, and with the RDP filling.
void grp_rdram_contend(void* unused) {
  (void)unused;
  static const uint32_t strides[4] = {0, 4, 2048, 1u << 20};
  hwp_vi_mode(HWP_VI_OFF);
  if (!rsp_ok) rsp_ok = hwp_rsp_load();
  uint32_t n = 0;
  for (int kind = 0; kind < 3; ++kind) {
    for (int s = 0; s < 4; ++s) {
      for (int r = 0; r < 8; ++r) {
        if (kind == 1) {  // RSP: back-to-back 3 KiB DMAs until halted
          hwp_rsp_halt();
          DMEM[RSP_PARAMS + 1] = PHYS(sp_buf);
          HSP_PC = DMEM[RSP_CASE_DMA_LOOP] & 0xFFC;
          SP_REG(SPR_STATUS) = SPW_CLR_BROKE | SPW_CLR_HALT;
        } else if (kind == 2) {  // RDP: four full-screen fills, started and not waited for
          setup(0, SOM_CYCLE_FILL, 0);
          for (int i = 0; i < 4; ++i) fill_rect(0, 0, 319, 239);
          C(0x29ull << 56);
          dcache_wb(cmd_buf, ncmd * 8);
          DP_REG(DPR_STATUS) = DPW_RESET;
          MI_MODE_REG = MI_MODE_CLR_DP;
          DP_REG(DPR_START) = PHYS(cmd_buf);
          DP_REG(DPR_END) = PHYS(cmd_buf + ncmd);
        }
        out[n++] = hwp_rd_load(0xA0000000u, strides[s], 0x003FFFFCu);
        if (kind == 1) {
          hwp_rsp_halt();
          sp_wait_dma(TIMEOUT_TICKS(1));
        } else if (kind == 2) {
          const uint32_t t0 = c0_count();
          while (!(MI_INTR_REG & MI_INTR_DP) && c0_count() - t0 < TIMEOUT_TICKS(20)) {}
          MI_MODE_REG = MI_MODE_CLR_DP;
        }
      }
    }
  }
  hwp_screen_show();
  rec_series(S_RDRAM_CONTEND, 4, n, out);
  rec_series(S_RDRAM_CONTEND_STRIDES, 4, 4, strides);
}

// ---------------------------------------------------------------------------------------------
// VI and refresh coupling: RDRAM refresh runs once per HSYNC (n64brew "RDRAM Interface"), so
// the line length set by VI_H_SYNC changes the refresh rate. Uncached loads per line length.
void grp_vi_hsync(void* unused) {
  (void)unused;
  // H_SYNC bits 11:0 are the line length in quarter VI pixels minus one (n64brew "Video
  // Interface", VI_H_SYNC); 0xC15 is NTSC. Values stay inside the 12-bit field.
  static const uint32_t hsync[4] = {0x0C15, 0x0A00, 0x0E00, 0x0FFF};
  static const uint32_t strides[2] = {0, 2048};
  volatile uint32_t* vi = (volatile uint32_t*)0xA4400000u;
  uint32_t n = 0;
  for (int h = 0; h < 4; ++h) {
    hwp_vi_mode(HWP_VI_240P);
    vi[7] = hsync[h];
    vi[8] = (hsync[h] << 16) | hsync[h];
    for (int i = 0; i < 2; ++i) hwp_wait_vblank();
    for (int s = 0; s < 2; ++s)
      for (int r = 0; r < 8; ++r) out[n++] = hwp_rd_load(0xA0000000u, strides[s], 0x003FFFFCu);
  }
  hwp_screen_show();
  rec_series(S_VI_HSYNC, 4, n, out);
  rec_series(S_VI_HSYNC_VALUES, 4, 4, hsync);
}
