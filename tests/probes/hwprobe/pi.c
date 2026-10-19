// Polled PI DMA and cache helpers. Registers: n64brew "Parallel Interface".
#include <libdragon.h>

#include "hwp.h"

#define P_DRAM_ADDR ((volatile uint32_t*)0xA4600000u)
#define P_CART_ADDR ((volatile uint32_t*)0xA4600004u)
#define P_RD_LEN    ((volatile uint32_t*)0xA4600008u)
#define P_WR_LEN    ((volatile uint32_t*)0xA460000Cu)
#define P_STATUS    ((volatile uint32_t*)0xA4600010u)

void pi_wait(void) {
  while (*P_STATUS & 3) {}  // DMA busy, IO busy
}

// DMAs are done in 8-byte-aligned bounce-free blocks: callers pass 8-aligned RDRAM.
void pi_dma_from_cart(void* rdram, uint32_t cart, uint32_t len) {
  dcache_wbinv(rdram, len);
  pi_wait();
  *P_DRAM_ADDR = PHYS(rdram);
  *P_CART_ADDR = cart;
  *P_WR_LEN = len - 1;
  pi_wait();
  dcache_inv(rdram, len);
}

void pi_dma_to_cart(uint32_t cart, const void* rdram, uint32_t len) {
  dcache_wb(rdram, len);
  pi_wait();
  *P_DRAM_ADDR = PHYS(rdram);
  *P_CART_ADDR = cart;
  *P_RD_LEN = len - 1;
  pi_wait();
}

// The D-cache line is 16 bytes and the I-cache line 32 (VR4300 manual 11.2). libdragon's hit
// helpers require whole lines, so ranges are widened to them. Invalidation also writes back:
// a DMA target is written back and invalidated before the transfer, so its lines are clean and
// a widened edge line keeps any neighbouring data the CPU wrote.
static void lines(const void* p, uint32_t len, uint32_t line, uint32_t* a, uint32_t* n) {
  const uint32_t s = (uint32_t)p & ~(line - 1);
  const uint32_t e = ((uint32_t)p + len + line - 1) & ~(line - 1);
  *a = s;
  *n = e - s;
}
void dcache_wb(const void* p, uint32_t len) {
  uint32_t a, n;
  lines(p, len, 16, &a, &n);
  data_cache_hit_writeback((void*)a, n);
}
void dcache_inv(const void* p, uint32_t len) { dcache_wbinv(p, len); }
void dcache_wbinv(const void* p, uint32_t len) {
  uint32_t a, n;
  lines(p, len, 16, &a, &n);
  data_cache_hit_writeback_invalidate((void*)a, n);
}
void icache_inv(const void* p, uint32_t len) {
  uint32_t a, n;
  lines(p, len, 32, &a, &n);
  inst_cache_hit_invalidate((void*)a, n);
}
