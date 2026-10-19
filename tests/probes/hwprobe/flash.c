// FlashRAM driver. Protocol: n64brew "Flash" (TLDR for programmer, Command Internal Register,
// Status Mode, PI Configuration). The chip is at PI 0x08000000 and its command register at
// 0x08010000. Byte-indexed chips are assumed for programming (by page index, so it does not
// matter) and detected for reads.
#include <string.h>

#include "hwp.h"

#define FLASH_BASE 0x08000000u
#define FLASH_CMD  ((volatile uint32_t*)0xA8010000u)
#define FLASH_IO   ((volatile uint32_t*)0xA8000000u)

// n64brew "Flash", Command Internal Register table.
#define CMD_CHIP_ERASE_SETUP   0x3C000000u
#define CMD_SECTOR_ERASE_SETUP 0x4B000000u
#define CMD_ERASE              0x78000000u
#define CMD_PROGRAM_PAGE       0xA5000000u
#define CMD_LOAD_PAGE          0xB4000000u
#define CMD_STATUS             0xD2000000u
#define CMD_SILICON_ID         0xE1000000u
#define CMD_READ_ARRAY         0xF0000000u
// Status register bits (n64brew "Flash", Status Mode).
#define ST_ERASE_OK     0x08u
#define ST_PROGRAM_OK   0x04u
#define ST_ERASE_BUSY   0x02u
#define ST_PROGRAM_BUSY 0x01u

// PI domain 2 timing for Flash (n64brew "Flash", PI Configuration): LAT 5, PWD 12, PGS 15, RLS 2.
#define PI_BSD_DOM2_LAT ((volatile uint32_t*)0xA4600024u)
#define PI_BSD_DOM2_PWD ((volatile uint32_t*)0xA4600028u)
#define PI_BSD_DOM2_PGS ((volatile uint32_t*)0xA460002Cu)
#define PI_BSD_DOM2_RLS ((volatile uint32_t*)0xA4600030u)

// Count ticks are 2 PCycles: 46.875 MHz (n64brew "ROM Header", Clock Rate).
#define TICKS_PER_MS 46875u

// CPU IO on the PI bus: a write while the previous one is still in flight is ignored, and a
// read then returns the value being written (n64brew "Parallel Interface", PI_STATUS IO busy).
// Every access waits for the PI to be idle first.
static void cmd(uint32_t v) {
  pi_wait();
  *FLASH_CMD = v;
}
static void io_w(uint32_t v) {
  pi_wait();
  *FLASH_IO = v;
}
static uint32_t io_r(void) {
  pi_wait();
  return *FLASH_IO;
}

static bool word_indexed;
static uint32_t last_status;

static uint64_t id_buf[2] __attribute__((aligned(16)));

uint32_t flash_last_status(void) { return last_status; }

static uint32_t read_status(void) {
  cmd(CMD_STATUS);
  cmd(CMD_STATUS);  // MX29L1100 may need two writes (n64brew "Flash", CIR quirks)
  return io_r() & 0xFF;
}

static bool wait_ready(uint32_t busy_mask, uint32_t timeout_ms) {
  const uint32_t start = c0_count();
  for (;;) {
    last_status = io_r() & 0xFF;
    if (!(last_status & busy_mask)) return true;
    if (c0_count() - start > timeout_ms * TICKS_PER_MS) return false;
  }
}

static void clear_status(void) {
  cmd(CMD_STATUS);
  io_w(0);
}

uint64_t flash_init(void) {
  *PI_BSD_DOM2_LAT = 0x05;
  *PI_BSD_DOM2_PWD = 0x0C;
  *PI_BSD_DOM2_PGS = 0x0F;
  *PI_BSD_DOM2_RLS = 0x02;
  cmd(CMD_SILICON_ID);
  cmd(CMD_SILICON_ID);
  // The silicon ID must be read by an 8-byte DMA (n64brew "Flash", TLDR).
  pi_dma_from_cart(id_buf, FLASH_BASE, 8);
  const uint64_t id = id_buf[0];
  const uint32_t dev = (uint32_t)id & 0xFFFF;
  const uint32_t man = (uint32_t)(id >> 16) & 0xFFFF;
  // Word-indexed models: MX29L0000, MX29L0001, MX29L1100 (n64brew "Flash", Silicon ID table).
  word_indexed = man == 0x00C2 && (dev == 0x0000 || dev == 0x0001 || dev == 0x001E);
  cmd(CMD_READ_ARRAY);
  return id;
}

bool flash_present(uint64_t id) { return (uint32_t)(id >> 32) == 0x11118001u; }

static bool erase(uint32_t setup) {
  clear_status();
  cmd(setup);
  cmd(CMD_ERASE);
  bool ok = wait_ready(ST_ERASE_BUSY, 2000);
  last_status = read_status();
  ok = ok && (last_status & ST_ERASE_OK);
  clear_status();
  cmd(CMD_READ_ARRAY);
  return ok;
}

bool flash_erase_chip(void) { return erase(CMD_CHIP_ERASE_SETUP); }
bool flash_erase_sector(uint32_t page) { return erase(CMD_SECTOR_ERASE_SETUP | (page & 0x3FF)); }

bool flash_program_page(uint32_t page, const void* data128) {
  clear_status();
  cmd(CMD_LOAD_PAGE);
  pi_dma_to_cart(FLASH_BASE, data128, 128);
  cmd(CMD_PROGRAM_PAGE | (page & 0x3FF));
  bool ok = wait_ready(ST_PROGRAM_BUSY, 50);
  last_status = read_status();
  ok = ok && (last_status & ST_PROGRAM_OK);
  clear_status();
  cmd(CMD_READ_ARRAY);
  return ok;
}

void flash_read(uint32_t offset, void* dst, uint32_t len) {
  cmd(CMD_READ_ARRAY);
  uint8_t* d = dst;
  while (len) {
    // A burst must not cross a 0x8000 boundary (n64brew "Flash", TLDR and Read Array Mode).
    uint32_t n = 0x8000 - (offset & 0x7FFF);
    if (n > len) n = len;
    pi_dma_from_cart(d, FLASH_BASE + (word_indexed ? offset / 2 : offset), n);
    d += n;
    offset += n;
    len -= n;
  }
}
