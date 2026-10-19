// Boot-state group (series 0x01xx, or 0x02xx after a soft reset).
#include <libdragon.h>
#include <string.h>

#include "hwp.h"

extern int __boot_memsize, __boot_tvtype, __boot_resettype, __boot_consoletype;

// Taken at the top of main, before the text screen reprograms the VI and before main clears
// the low-RAM magic. Register lists: series.h S_BOOT_RCP.
static uint32_t rcp_snapshot[64];
static uint32_t rcp_n;
static uint32_t low_snapshot[16];
// The cold-boot capture written by the patched IPL3 of hwprobe-cold.z64 (ipl3/), at RDRAM
// 0x200-0x37F with "HWC0" in word 0. Cleared once read, so that a later boot through an IPL3
// that does not write it cannot be mistaken for a fresh capture.
static uint32_t cold_capture[96];
static bool cold_present;

static void snap(uint32_t base, int n) {
  for (int i = 0; i < n; ++i) rcp_snapshot[rcp_n++] = ((volatile uint32_t*)base)[i];
}

void hwp_snapshot_early(void) {
  rcp_n = 0;
  snap(0xA4300000u, 4);   // MI: MODE, VERSION, INTERRUPT, MASK
  snap(0xA4400000u, 14);  // VI 0x00..0x34
  snap(0xA4500000u, 6);   // AI 0x00..0x14
  snap(0xA4600000u, 13);  // PI 0x00..0x30
  snap(0xA4700000u, 8);   // RI 0x00..0x1C
  rcp_snapshot[rcp_n++] = *(volatile uint32_t*)0xA4800000u;  // SI_DRAM_ADDR
  rcp_snapshot[rcp_n++] = *(volatile uint32_t*)0xA4800018u;  // SI_STATUS
  rcp_snapshot[rcp_n++] = *(volatile uint32_t*)0xA4040010u;  // SP_STATUS
  rcp_snapshot[rcp_n++] = *(volatile uint32_t*)0xA4040014u;  // SP_DMA_FULL
  rcp_snapshot[rcp_n++] = *(volatile uint32_t*)0xA4040018u;  // SP_DMA_BUSY
  rcp_snapshot[rcp_n++] = *(volatile uint32_t*)0xA4080000u;  // SP_PC
  for (int i = 0; i < 16; ++i) low_snapshot[i] = ((volatile uint32_t*)0xA0000380u)[i];
  volatile uint32_t* cold = (volatile uint32_t*)0xA0000200u;
  cold_present = cold[0] == 0x48574330u;  // "HWC0"
  for (int i = 0; i < 96; ++i) cold_capture[i] = cold[i];
  cold[0] = 0;
}

void grp_boot(void* unused) {
  (void)unused;
  const uint16_t b = hwp_stage == 1 ? 0x0100 : 0x0200;
  rec_series(b | 0x00, 8, 34, hwp_boot.gpr);
  rec_series(b | 0x01, 8, 32, hwp_boot.cop0);
  rec_series(b | 0x02, 4, 2, hwp_boot.fcr);
  rec_series(b | 0x03, 8, 32 * 4, hwp_boot.tlb);
  rec_series(b | 0x04, 4, 2, hwp_boot.count);
  rec_series(b | 0x05, 4, rcp_n, rcp_snapshot);

  // RDRAM device registers 0..9 of every 2 MiB device IPL3 mapped (ids 0, 2, ...), with the
  // 0x400 register stride of RI version 2 (libdragon boot/rdram.c rdram_reg_init).
  static uint32_t rd[4 * 10];
  const int ndev = hwp_memsize / (2u << 20) > 4 ? 4 : (int)(hwp_memsize / (2u << 20));
  for (int d = 0; d < ndev; ++d)
    for (int r = 0; r < 10; ++r)
      rd[d * 10 + r] = *(volatile uint32_t*)(0xA3F00000u + ((uint32_t)(2 * d) << 10) + 4 * r);
  rec_series(b | 0x06, 4, ndev * 10, rd);

  uint32_t flags[5 + 16];
  flags[0] = (uint32_t)__boot_memsize;
  flags[1] = (uint32_t)__boot_tvtype;
  flags[2] = (uint32_t)__boot_resettype;
  flags[3] = (uint32_t)__boot_consoletype;
  flags[4] = (uint32_t)hwp_stage;
  memcpy(flags + 5, low_snapshot, sizeof low_snapshot);
  rec_series(b | 0x07, 4, 21, flags);
  if (cold_present) rec_series(b | 0x08, 4, 96, cold_capture);
}
