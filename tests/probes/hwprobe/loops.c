// Uncached-read loop group (series 0x3MRO, 0x40MO, 0x40F0, 0x40F1).
#include <libdragon.h>
#include <string.h>

#include "hwp.h"

typedef void raw_fn(uint8_t*, uint8_t*, uint32_t, uint32_t*);
typedef void line_fn(uint8_t*, uint8_t*, uint32_t);
extern raw_fn hwp_raw_alu, hwp_raw_muls0, hwp_raw_adds, hwp_raw_mult, hwp_raw_dmult, hwp_raw_div,
    hwp_raw_ddiv;
extern line_fn hwp_line_alu, hwp_line_muls0, hwp_line_adds, hwp_line_mult, hwp_line_dmult,
    hwp_line_div, hwp_line_ddiv;

static raw_fn* const raw[HWP_NUM_OPS] = {hwp_raw_alu,   hwp_raw_muls0, hwp_raw_adds, hwp_raw_mult,
                                         hwp_raw_dmult, hwp_raw_div,   hwp_raw_ddiv};
static line_fn* const line[HWP_NUM_OPS] = {hwp_line_alu,   hwp_line_muls0, hwp_line_adds,
                                           hwp_line_mult,  hwp_line_dmult, hwp_line_div,
                                           hwp_line_ddiv};

// KSEG1 addresses of the register each loop reads (n64brew "Memory map").
static const uint32_t reg_addr[4] = {0xA4400010u /* VI_V_CURRENT */, 0xA4300004u /* MI_VERSION */,
                                     0xA4040010u /* SP_STATUS */, 0xA4600010u /* PI_STATUS */};

// The sample buffer fills half of the 8 KiB D-cache and is loaded before each run, so every SB
// in the loop hits (VR4300 manual 11.2: direct mapped, 16-byte lines, write back).
static uint8_t samples[HWP_RAW_SAMPLES_VI] __attribute__((aligned(8192)));

static void warm(uint8_t* p, uint32_t n) {
  volatile uint32_t sink = 0;
  for (uint32_t i = 0; i < n; i += 16) sink += *(volatile uint32_t*)(p + i);
  (void)sink;
}

void grp_loops(void* unused) {
  (void)unused;
  static uint32_t totals[3 * 4 * HWP_NUM_OPS * 2];
  static uint32_t vi_regs[3 * 14];
  uint32_t nt = 0;
  for (int m = 0; m < 3; ++m) {
    hwp_vi_regs(m, vi_regs + 14 * m);
    hwp_vi_mode(m);
    for (int r = 0; r < 4; ++r) {
      // Every run keeps 4096 samples in the full record; FlashRAM keeps 256 of the others.
      const uint32_t n = HWP_RAW_SAMPLES_VI;
      const uint32_t nflash = r == HWP_REG_VI_V_CURRENT ? HWP_RAW_SAMPLES_VI : HWP_RAW_SAMPLES_OTHER;
      for (int o = 0; o < HWP_NUM_OPS; ++o) {
        uint32_t counts[2];
        raw_fn* const f = raw[o];
        const uint32_t ra = reg_addr[r];
        // Load everything the call needs before warming, so no data access between the warm-up
        // and the loop can evict a sample line.
        __asm__ volatile("" ::"r"(f), "r"(ra));
        warm(samples, n);
        f(samples, samples + n, ra, counts);
        totals[nt++] = counts[0];
        totals[nt++] = counts[1];
        rec_series_packed_split(S_LOOP_RAW_ID(m, r, o), HWP_PACK_DELTAS, n, nflash, samples);
      }
    }
    if (m != HWP_VI_OFF) {
      for (int o = 0; o < HWP_NUM_OPS; ++o) {
        line_fn* const f = line[o];
        __asm__ volatile("" ::"r"(f));
        warm(samples, HWP_LINE_SAMPLES);
        f(samples, samples + HWP_LINE_SAMPLES, 0xA4400010u);
        rec_series_packed(S_LOOP_LINE_ID(m, o), HWP_PACK_VALUES, HWP_LINE_SAMPLES, samples);
      }
    }
    hwp_screen_show();
    rec_flush();
  }
  rec_series(S_LOOP_VI_REGS, 4, 3 * 14, vi_regs);
  rec_series(S_LOOP_RAW_TOTAL, 4, nt, totals);
}
