// Text screen and the VI configurations of the timing modes. The VI is programmed directly,
// without libdragon's display module, so that no VI interrupt handler runs and so that the
// 480i mode is krom's exact configuration.
#include <libdragon.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "hwp.h"

#define VI ((volatile uint32_t*)0xA4400000u)
enum { VI_STATUS, VI_ORIGIN, VI_WIDTH, VI_V_INTR, VI_V_CURRENT, VI_TIMING, VI_V_SYNC, VI_H_SYNC,
       VI_H_SYNC_LEAP, VI_H_VIDEO, VI_V_VIDEO, VI_V_BURST, VI_X_SCALE, VI_Y_SCALE };

// Framebuffers at fixed physical addresses: 480i is krom's DRAM origin 0x00100000
// (CPUTIMINGNTSC.asm ScreenNTSC); the text screen and 240p mode use 0x00300000. The program and
// its heap stay below 1 MiB (checked in main).
#define FB480_PHYS 0x00100000u
#define FB240_PHYS 0x00300000u

// krom LIB/N64_GFX.INC ScreenNTSC: timing, sync and video windows for NTSC.
#define NTSC_TIMING  0x03E52239u
#define NTSC_H_SYNC  0x00000C15u
#define NTSC_LEAP    0x0C150C15u
#define NTSC_H_VIDEO 0x006C02ECu
#define NTSC_V_VIDEO 0x002501FFu
#define NTSC_V_BURST 0x000E0204u
// VI_STATUS bits (krom N64_GFX.INC constants): BPP16 = 2, BPP32 = 3, INTERLACE = 0x40,
// AA_MODE_2 (resample only) = 0x200.
#define ST_240P 0x00000202u
#define ST_480I 0x00000243u

static surface_t text_surf;

void hwp_vi_regs(int mode, uint32_t r[14]) {
  memset(r, 0, 14 * sizeof r[0]);
  r[VI_TIMING] = NTSC_TIMING;
  r[VI_H_SYNC] = NTSC_H_SYNC;
  r[VI_H_SYNC_LEAP] = NTSC_LEAP;
  r[VI_H_VIDEO] = NTSC_H_VIDEO;
  r[VI_V_VIDEO] = NTSC_V_VIDEO;
  r[VI_V_BURST] = NTSC_V_BURST;
  r[VI_V_INTR] = 0x200;
  if (mode == HWP_VI_480I) {
    r[VI_STATUS] = ST_480I;
    r[VI_ORIGIN] = 0xA0000000u | FB480_PHYS;
    r[VI_WIDTH] = 640;
    r[VI_V_SYNC] = 0x20D;          // 525 half-lines: interlaced
    r[VI_X_SCALE] = 0x100 * 640 / 160;
    r[VI_Y_SCALE] = 0x100 * 480 / 60;
  } else {
    r[VI_STATUS] = mode == HWP_VI_OFF ? 0 : ST_240P;  // type 0: blank, no fetch
    r[VI_ORIGIN] = 0xA0000000u | FB240_PHYS;
    r[VI_WIDTH] = 320;
    r[VI_V_SYNC] = 0x20C;          // 524: progressive
    r[VI_X_SCALE] = 0x100 * 320 / 160;
    r[VI_Y_SCALE] = 0x100 * 240 / 60;
  }
}

static void vi_program(const uint32_t r[14]) {
  for (int i = 0; i < 14; ++i) {
    if (i == VI_V_CURRENT) {
      VI[i] = 0;  // clears the VI interrupt
      continue;
    }
    VI[i] = r[i];
  }
}

void hwp_wait_vblank(void) {
  // Wait for half-line 0x200 to come and go; bounded so a stopped VI cannot hang us.
  const uint32_t start = c0_count();
  while ((VI[VI_V_CURRENT] & 0x3FE) != 0x200)
    if (c0_count() - start > 46875u * 40) return;
  while ((VI[VI_V_CURRENT] & 0x3FE) == 0x200)
    if (c0_count() - start > 46875u * 80) return;
}

void hwp_vi_mode(int mode) {
  uint32_t r[14];
  hwp_vi_regs(mode, r);
  vi_program(r);
  // Let the new mode run three fields before measuring.
  for (int i = 0; i < 3; ++i) hwp_wait_vblank();
}

void hwp_screen_init(void) {
  memset(KSEG1(0x80000000u | FB240_PHYS), 0, 320 * 240 * 2);
  text_surf = surface_make(KSEG1(0x80000000u | FB240_PHYS), FMT_RGBA16, 320, 240, 640);
  graphics_set_color(graphics_make_color(255, 255, 255, 255), graphics_make_color(0, 0, 0, 255));
  hwp_screen_show();
}

void hwp_screen_show(void) {
  uint32_t r[14];
  hwp_vi_regs(HWP_VI_240P, r);
  vi_program(r);
}

void hwp_print(int row, const char* fmt, ...) {
  char line[48];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  graphics_draw_box(&text_surf, 0, 16 + row * 10, 320, 10, graphics_make_color(0, 0, 0, 255));
  graphics_draw_text(&text_surf, 16, 16 + row * 10, line);
  debugf("%s\n", line);
}
