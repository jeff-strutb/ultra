// hwprobe: shared declarations. docs/timing/hwprobe.md is the specification.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "series.h"

// ---- Boot capture (entry.S). Offsets are used by the assembly; keep them in step. ----
typedef struct {
  uint64_t gpr[34];      // 0: r0..r31, HI, LO
  uint64_t cop0[32];     // 272
  uint32_t fcr[2];       // 528
  uint64_t tlb[32][4];   // 536: PageMask, EntryHi, EntryLo0, EntryLo1
  uint32_t count[2];     // 1560
} hwp_boot_t;
_Static_assert(sizeof(hwp_boot_t) == 1568, "entry.S offsets");
extern hwp_boot_t hwp_boot;

// ---- Low-RAM words that survive a soft reset (libdragon IPL3 clears RDRAM from 0x400 up). ----
#define HWP_LOW_MAGIC     ((volatile uint32_t*)0xA0000380) // HWP_WAIT_MAGIC while stage 1 waits
#define HWP_LOW_PRENMI    ((volatile uint32_t*)0xA0000384) // Count at the pre-NMI interrupt
#define HWP_LOW_LASTCOUNT ((volatile uint32_t*)0xA0000388) // last Count the wait loop stored
#define HWP_LOW_PRENMI_N  ((volatile uint32_t*)0xA000038C) // pre-NMI interrupts taken
#define HWP_LOW_LOOPS     ((volatile uint32_t*)0xA0000390) // wait-loop iterations (wraps)
#define HWP_WAIT_MAGIC    0x48575057u // "HWPW"

// ---- Record (record.c) ----
#define HWP_STAGE1_BASE 0x00000
#define HWP_STAGE1_SIZE 0x1C000
#define HWP_STAGE2_BASE 0x1C000
#define HWP_STAGE2_SIZE 0x04000
#define HWP_HEADER_SIZE 64

void rec_begin(int stage, uint64_t silicon_id);
void rec_series(uint16_t id, uint8_t esize, uint32_t count, const void* data);
void rec_series_packed(uint16_t id, uint8_t mode, uint32_t count, const uint8_t* data);
void rec_series_packed_split(uint16_t id, uint8_t mode, uint32_t count, uint32_t flash_count,
                             const uint8_t* data);
void rec_series_full(uint16_t id, uint8_t esize, uint32_t count, const void* data); // full record only
void rec_flush(void);             // program every complete page not yet programmed

// ---- Full record (sink.c): SD card file or ISViewer stream ----
const char* sink_open(int stage, bool sd, bool isviewer); // returns the SD path, or NULL
bool sink_active(void);
void sink_series(uint16_t id, uint8_t esize, uint8_t flags, uint32_t count, const void* data,
                 uint32_t data_len);
bool sink_close(uint32_t status, uint64_t silicon);
uint32_t sink_bytes(void);
bool rec_finish(uint32_t status); // program the tail and the header; true if it reads back intact
bool rec_program_errors(void);    // some erase/program reported failure in its status bits
uint32_t rec_bytes(void);
uint32_t rec_crc(void);
uint32_t rec_series_count(void);
uint32_t rec_room(void);
uint64_t rec_silicon(void);
const uint8_t* rec_image(void); // the area as programmed: header (after rec_finish) and series
uint32_t crc32_update(uint32_t crc, const void* data, uint32_t len);

// ---- FlashRAM (flash.c), n64brew "Flash" ----
uint64_t flash_init(void);  // configures PI domain 2, returns the silicon ID
bool flash_present(uint64_t id);
bool flash_erase_chip(void);
bool flash_erase_sector(uint32_t page);
bool flash_program_page(uint32_t page, const void* data128);
void flash_read(uint32_t offset, void* dst, uint32_t len);
uint32_t flash_last_status(void);
const uint32_t* flash_times(uint32_t* n); // erase ticks, then page program ticks

// ---- PI / SI helpers (pi.c) ----
void pi_dma_from_cart(void* rdram, uint32_t cart, uint32_t len);
void pi_dma_to_cart(uint32_t cart, const void* rdram, uint32_t len);
void pi_wait(void);

// ---- Exceptions (exc.S) ----
typedef struct {
  uint64_t at;          // 0: saved $at
  uint32_t count;       // 8: Count at handler entry
  uint32_t cause;       // 12
  uint64_t epc;         // 16
  uint64_t badvaddr;    // 24
  uint32_t status;      // 32
  uint32_t fcsr;        // 36: FCR31 at entry (when CU1 is set)
  uint32_t n;           // 40: exceptions taken since the last reset of this frame
  uint32_t expect;      // 44: HWP_EXPECT_*
  uint32_t abort_pc;    // 48: where an unexpected exception resumes
  uint32_t faults;      // 52: unexpected exceptions
  uint32_t first_cause; // 56: Cause at the first entry after hwp_exc_reset
  uint32_t first_count; // 60
  uint64_t first_epc;   // 64
  uint64_t first_badvaddr; // 72
  uint32_t first_status;   // 80
  uint32_t pad;            // 84
} hwp_exc_t;
_Static_assert(sizeof(hwp_exc_t) == 88, "exc.S offsets");
extern volatile hwp_exc_t hwp_exc;

#define HWP_EXPECT_NONE   0 // any exception is a fault: resume at abort_pc
#define HWP_EXPECT_RESUME 1 // expected: resume at k1 (or EPC + 4 when k1 is 0)
#define HWP_EXPECT_PRENMI 2 // stage 1 wait: record pre-NMI, mask IM4, return to EPC

void hwp_vectors_install(void);
void hwp_vectors_restore(void);
void hwp_vector180_minimal(bool on); // 0x180 = {mtc0 k1, EPC; nop; nop; eret}
void hwp_vector_minimal_at(uint32_t vec, bool on); // the same at 0x000, 0x080, 0x100 or 0x180
void hwp_exc_reset(uint32_t expect);
// Runs fn(arg) and returns 0, or returns -1 if an unexpected exception aborted it.
int hwp_try(void (*fn)(void*), void* arg);

static inline uint32_t c0_count(void) {
  uint32_t c;
  __asm__ volatile("mfc0 %0, $9" : "=r"(c));
  return c;
}

// ---- Display (display.c) ----
void hwp_screen_init(void);
void hwp_screen_show(void); // reprogram the VI for the text screen
void hwp_print(int row, const char* fmt, ...);
void hwp_print_col(int row, int col, const char* fmt, ...);  // half-width cell, col 0 or 1
void hwp_vi_mode(int mode);       // HWP_VI_*: program the VI for a timing mode
void hwp_vi_regs(int mode, uint32_t out[14]);
void hwp_wait_vblank(void);

// ---- Groups ----
typedef struct {
  uint16_t id;
  const char* name;
  void (*run)(void*);
} hwp_group_t;

void grp_boot(void*);
void grp_loops(void*);
void grp_exc(void*);
void grp_itlb(void*);
void grp_cp0(void*);
void grp_overlap(void*);
void grp_quirks(void*);
void grp_tlb_shutdown(void*);
void grp_rdram(void*);
void grp_pi(void*);
void grp_si_ai(void*);
void grp_retire(void*);
void grp_refill(void*);
void grp_reglat(void*);
void grp_sp_dma(void*);
void grp_rsp(void*);
void grp_rdp(void*);
void grp_rdram_contend(void*);
void grp_vi_hsync(void*);
void grp_cpu_func(void*);
void grp_rdp_render(void*);
void grp_rsp_func(void*);
void grp_peripherals(void*);

extern int hwp_stage;
extern uint32_t hwp_memsize;
void hwp_snapshot_early(void); // RCP registers and low RAM as IPL3 left them (top of main)

// Cache maintenance over [p, p + len).
void dcache_wb(const void* p, uint32_t len);
void dcache_inv(const void* p, uint32_t len);
void dcache_wbinv(const void* p, uint32_t len);
void icache_inv(const void* p, uint32_t len);

#define KSEG1(p) ((void*)(((uint32_t)(p) & 0x1FFFFFFF) | 0xA0000000))
#define PHYS(p) ((uint32_t)(p) & 0x1FFFFFFF)
