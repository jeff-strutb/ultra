// Drivers for the probe groups after the boot state and the loops. Each records raw results;
// interpretation is the decoder's job. docs/timing/hwprobe.md describes every group.
#include <libdragon.h>
#include <string.h>

#include "hwp.h"

typedef uint32_t timed_fn(void);

// ---- probes_asm.S ----
extern void hwp_tlb_write(int index, const uint64_t e[4]);
extern void hwp_tlb_read(int index, uint64_t e[4]);
extern timed_fn hwp_x_syscall, hwp_x_syscall_base, hwp_x_addov, hwp_x_addov_base, hwp_x_adel,
    hwp_x_adel_base, hwp_x_break, hwp_x_break_base;
extern void hwp_int_nop(uint32_t k, uint32_t out[2]);
extern void hwp_int_lw(uint32_t k, uint32_t out[2]);
extern void hwp_int_div(uint32_t k, uint32_t out[2]);
extern uint32_t hwp_itlb_time(uint32_t vaddr);
extern void hwp_itlb_action(int kind, const uint64_t same[4], const uint64_t other[4], int index);
extern uint32_t hwp_hz_lo0_0(uint32_t*), hwp_hz_lo0_1(uint32_t*), hwp_hz_lo0_2(uint32_t*),
    hwp_hz_lo0_3(uint32_t*), hwp_hz_watch_0(uint32_t*), hwp_hz_watch_1(uint32_t*),
    hwp_hz_watch_2(uint32_t*), hwp_hz_watch_3(uint32_t*), hwp_hz_count_0(uint32_t*),
    hwp_hz_count_1(uint32_t*), hwp_hz_count_2(uint32_t*), hwp_hz_count_3(uint32_t*);
extern uint32_t hwp_ov_lw_mult_indep(uint32_t*), hwp_ov_lw_mult_dep(uint32_t*),
    hwp_ov_lw_div_indep(uint32_t*), hwp_ov_lw_div_dep(uint32_t*);
extern uint32_t hwp_ovi_mult(int cold), hwp_ovi_nop(int cold);
extern timed_fn hwp_br_beql_nt, hwp_br_nop, hwp_br_beq_t, hwp_br_bnel_t, hwp_eret_next, hwp_j_next;
extern timed_fn hwp_ce_syscall_nop, hwp_ce_syscall_mfc1, hwp_ce_syscall_mfc2, hwp_ce_syscall_lwc1,
    hwp_ce_syscall_mtc0, hwp_ce_addov_nop, hwp_ce_addov_mfc1, hwp_ce_addov_mfc2,
    hwp_ce_addov_lwc1, hwp_ce_addov_mtc0, hwp_ce_ctc1_nop, hwp_ce_ctc1_mfc1, hwp_ce_ctc1_mfc2,
    hwp_ce_ctc1_lwc1, hwp_ce_ctc1_mtc0;
extern void hwp_jr_j(uint32_t or_mask, uint32_t out[2]);
extern void hwp_jr_jal(uint32_t or_mask, uint32_t out[2]);
extern void hwp_cvt_s_l(int64_t in, uint64_t out[2]);
extern void hwp_cvt_d_l(int64_t in, uint64_t out[2]);
extern uint32_t hwp_cop_nop(uint32_t), hwp_cop_i_index_inv(uint32_t), hwp_cop_i_hit_inv(uint32_t),
    hwp_cop_i_fill(uint32_t), hwp_cop_d_index_wbinv(uint32_t), hwp_cop_d_hit_inv(uint32_t),
    hwp_cop_d_hit_wb(uint32_t);
extern void hwp_nop_sled(void);
extern void hwp_tlb_shutdown(const uint64_t e[4], uint32_t out[8]);
extern uint32_t hwp_rd_load(uint32_t base, uint32_t stride, uint32_t mask);
extern uint32_t hwp_rd_store(uint32_t base, uint32_t stride, uint32_t mask);

static uint32_t buf32[1024];

static uint64_t entryhi_asid(void) {
  uint64_t v;
  __asm__ volatile("dmfc0 %0, $10" : "=r"(v));
  return v & 0xFF;
}

// EntryLo: PFN in bits 29:6, C in 5:3, D bit 2, V bit 1, G bit 0 (VR4300 manual 5.4.2).
static uint64_t entrylo(uint32_t phys, uint32_t c, bool d, bool v) {
  return ((uint64_t)(phys >> 12) << 6) | (c << 3) | (d ? 4 : 0) | (v ? 2 : 0);
}

// ---------------------------------------------------------------------------------------------
void grp_exc(void* unused) {
  (void)unused;
  static timed_fn* const fns[8] = {hwp_x_syscall, hwp_x_syscall_base, hwp_x_addov,
                                   hwp_x_addov_base, hwp_x_adel, hwp_x_adel_base,
                                   hwp_x_break, hwp_x_break_base};
  // Information pass with the full handler: what each exception records.
  static uint32_t info[4 * 4];
  for (int k = 0; k < 4; ++k) {
    hwp_exc_reset(HWP_EXPECT_RESUME);
    fns[2 * k]();
    info[4 * k + 0] = hwp_exc.first_cause;
    info[4 * k + 1] = (uint32_t)hwp_exc.first_epc - (uint32_t)(uintptr_t)fns[2 * k];
    info[4 * k + 2] = (uint32_t)hwp_exc.first_badvaddr;
    info[4 * k + 3] = hwp_exc.first_status;
  }
  rec_series(S_EXC_REDIRECT_INFO, 4, 16, info);

  hwp_vector180_minimal(true);
  hwp_exc_reset(HWP_EXPECT_RESUME);
  for (int f = 0; f < 8; ++f)
    for (int r = 0; r < 64; ++r) buf32[64 * f + r] = fns[f]();
  hwp_vector180_minimal(false);
  rec_series(S_EXC_REDIRECT, 4, 8 * 64, buf32);

  // Interrupt acceptance: 64 values of k per sled.
  static void (*const sleds[3])(uint32_t, uint32_t*) = {hwp_int_nop, hwp_int_lw, hwp_int_div};
  uint32_t first_cause[3] = {0, 0, 0};
  for (int s = 0; s < 3; ++s) {
    for (int k = 2; k < 66; ++k) {
      uint32_t out[2];
      hwp_exc_reset(HWP_EXPECT_RESUME);
      sleds[s](k, out);
      uint32_t* o = &buf32[2 * (64 * s + (k - 2))];
      if (hwp_exc.n) {
        o[0] = (uint32_t)hwp_exc.first_epc - out[1];
        o[1] = hwp_exc.first_count - out[0];
        if (k == 2) first_cause[s] = hwp_exc.first_cause;
      } else {
        o[0] = 0xFFFFFFFFu;  // the sled finished first
        o[1] = 0;
      }
    }
  }
  rec_series(S_INT_ACCEPT, 4, 2 * 64 * 3, buf32);
  rec_series(S_INT_ACCEPT_CAUSE, 4, 3, first_cause);
}

// ---------------------------------------------------------------------------------------------
// Micro-TLB: code blocks in an 8 KiB-aligned even/odd page pair plus a third page, mapped at
// virtual 0x00400000 through TLB entries 30 (pages 0, 1) and 28 (page 2).
static uint32_t itlb_code[3 * 1024] __attribute__((aligned(8192)));
#define ITLB_VA 0x00400000u
#define ITLB_INDEX 30

static void itlb_emit(void) {
  const uint32_t NOP = 0, JR_RA = 0x03E00008u;
  memset(itlb_code, 0, sizeof itlb_code);
  // Two-page block at VA + 0xFF8: nop, nop | nop, jr ra, nop
  itlb_code[0x3FE] = NOP;
  itlb_code[0x3FF] = NOP;
  itlb_code[0x400] = NOP;
  itlb_code[0x401] = JR_RA;
  itlb_code[0x402] = NOP;
  // Three-page block at VA + 0xFE8: nop, nop, j VA + 0x1800, nop | (page 1 + 0x800) j VA +
  // 0x2000, nop | (page 2) nop, jr ra, nop
  itlb_code[0x3FA] = NOP;
  itlb_code[0x3FB] = NOP;
  itlb_code[0x3FC] = 0x08000000u | (((ITLB_VA + 0x1800) >> 2) & 0x03FFFFFFu);
  itlb_code[0x3FD] = NOP;
  itlb_code[0x600] = 0x08000000u | (((ITLB_VA + 0x2000) >> 2) & 0x03FFFFFFu);  // j page 2
  itlb_code[0x601] = NOP;
  itlb_code[0x800] = NOP;
  itlb_code[0x801] = JR_RA;
  itlb_code[0x802] = NOP;
  dcache_wb(itlb_code, sizeof itlb_code);
  icache_inv(itlb_code, sizeof itlb_code);
}

void grp_itlb(void* unused) {
  (void)unused;
  itlb_emit();
  const uint32_t phys = PHYS(itlb_code);
  const uint64_t asid = entryhi_asid();
  uint64_t same[4] = {0, (ITLB_VA & ~0x1FFFu) | asid, entrylo(phys, 3, true, true),
                      entrylo(phys + 0x1000, 3, true, true)};
  uint64_t page2[4] = {0, ((ITLB_VA + 0x2000) & ~0x1FFFu) | asid,
                       entrylo(phys + 0x2000, 3, true, true), 0};
  uint64_t other[4] = {0, 0x00800000u | asid, 0, 0};  // unrelated VPN2, invalid
  hwp_tlb_write(ITLB_INDEX, same);
  hwp_tlb_write(28, page2);
  hwp_tlb_write(ITLB_INDEX + 1, other);

  const uint32_t two = ITLB_VA + 0xFF8, three = ITLB_VA + 0xFE8;
  for (int a = 0; a < 8; ++a) {
    const uint32_t entry = a == 7 ? three : two;
    for (int r = 0; r < 32; ++r) {
      hwp_itlb_time(entry);  // warm: I-cache and both micro-TLB entries
      hwp_itlb_time(entry);
      hwp_itlb_action(a == 7 ? 0 : a, same, other, ITLB_INDEX);
      buf32[32 * a + r] = hwp_itlb_time(entry);
    }
  }
  rec_series(S_ITLB, 4, 8 * 32, buf32);
  const uint32_t setup[4] = {ITLB_VA, phys, sizeof itlb_code, ITLB_INDEX};
  rec_series(S_ITLB_SETUP, 4, 4, setup);

  const uint64_t inval[4] = {0, 0x00A00000u, 0, 0};
  hwp_tlb_write(ITLB_INDEX, inval);
  hwp_tlb_write(28, (const uint64_t[4]){0, 0x00A04000u, 0, 0});
  hwp_tlb_write(ITLB_INDEX + 1, (const uint64_t[4]){0, 0x00A08000u, 0, 0});
}

// ---------------------------------------------------------------------------------------------
void grp_cp0(void* unused) {
  (void)unused;
  typedef uint32_t hz_fn(uint32_t*);
  static hz_fn* const lo0[4] = {hwp_hz_lo0_0, hwp_hz_lo0_1, hwp_hz_lo0_2, hwp_hz_lo0_3};
  static hz_fn* const watch[4] = {hwp_hz_watch_0, hwp_hz_watch_1, hwp_hz_watch_2, hwp_hz_watch_3};
  static hz_fn* const cnt[4] = {hwp_hz_count_0, hwp_hz_count_1, hwp_hz_count_2, hwp_hz_count_3};
  hz_fn* const* sets[2] = {lo0, watch};
  const uint16_t ids[2] = {S_CP0_HAZARD, S_CP0_HAZARD_WATCH};
  for (int s = 0; s < 2; ++s) {
    uint32_t n = 0;
    for (int g = 0; g < 4; ++g) {
      uint32_t vals[16];
      for (int r = 0; r < 16; ++r) buf32[n++] = sets[s][g](vals);
      for (int i = 0; i < 16; ++i) buf32[n++] = vals[i];
    }
    rec_series(ids[s], 4, n, buf32);
  }
  __asm__ volatile("mtc0 $0, $18");  // WatchLo off
  // Count: write then read; the reads are the data. Count is restored afterwards by its
  // running on; group times around this group are not meaningful.
  uint32_t n = 0;
  for (int g = 0; g < 4; ++g) {
    uint32_t vals[16];
    cnt[g](vals);
    for (int i = 0; i < 16; ++i) {
      buf32[n++] = i & 1 ? 0x20000000u : 0x10000000u;
      buf32[n++] = vals[i];
    }
  }
  rec_series(S_CP0_HAZARD_COUNT, 4, n, buf32);
}

// ---------------------------------------------------------------------------------------------
static uint32_t ov_line[4] __attribute__((aligned(16)));

void grp_overlap(void* unused) {
  (void)unused;
  typedef uint32_t ov_fn(uint32_t*);
  // miss/hit x MULT/DIV x independent/dependent, in S_OVERLAP order.
  static ov_fn* const fns[6] = {hwp_ov_lw_mult_indep, hwp_ov_lw_mult_dep, hwp_ov_lw_mult_indep,
                                hwp_ov_lw_div_indep,  hwp_ov_lw_div_dep,  hwp_ov_lw_div_indep};
  static const bool miss[6] = {true, true, false, true, true, false};
  for (int c = 0; c < 6; ++c) {
    for (int r = 0; r < 64; ++r) {
      if (miss[c])
        dcache_wbinv(ov_line, 16);
      else
        (void)*(volatile uint32_t*)ov_line;
      buf32[64 * c + r] = fns[c](ov_line);
    }
  }
  rec_series(S_OVERLAP, 4, 6 * 64, buf32);

  for (int c = 0; c < 4; ++c) {
    uint32_t (*f)(int) = c < 2 ? hwp_ovi_mult : hwp_ovi_nop;
    const int cold = !(c & 1);
    f(0);  // all three lines resident
    for (int r = 0; r < 64; ++r) buf32[64 * c + r] = f(cold);
  }
  rec_series(S_OVERLAP_ILINE, 4, 4 * 64, buf32);

  static timed_fn* const br[4] = {hwp_br_beql_nt, hwp_br_nop, hwp_br_beq_t, hwp_br_bnel_t};
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 64; ++r) buf32[64 * c + r] = br[c]();
  rec_series(S_OVERLAP_BRANCH, 4, 4 * 64, buf32);

  for (int r = 0; r < 64; ++r) buf32[r] = hwp_eret_next();
  for (int r = 0; r < 64; ++r) buf32[64 + r] = hwp_j_next();
  rec_series(S_OVERLAP_ERET, 4, 2 * 64, buf32);
}

// ---------------------------------------------------------------------------------------------
void grp_quirks(void* unused) {
  (void)unused;
  static timed_fn* const ce[15] = {
      hwp_ce_syscall_nop, hwp_ce_syscall_mfc1, hwp_ce_syscall_mfc2, hwp_ce_syscall_lwc1,
      hwp_ce_syscall_mtc0, hwp_ce_addov_nop, hwp_ce_addov_mfc1, hwp_ce_addov_mfc2,
      hwp_ce_addov_lwc1, hwp_ce_addov_mtc0, hwp_ce_ctc1_nop, hwp_ce_ctc1_mfc1,
      hwp_ce_ctc1_mfc2, hwp_ce_ctc1_lwc1, hwp_ce_ctc1_mtc0};
  for (int i = 0; i < 15; ++i) {
    hwp_exc_reset(HWP_EXPECT_RESUME);
    ce[i]();
    buf32[i] = hwp_exc.n ? hwp_exc.first_cause : 0xFFFFFFFFu;
  }
  rec_series(S_CAUSE_CE, 4, 15, buf32);

  // Jump region: 8 -> A (or_mask 0x20000000), A -> 8 (called through the 0xA alias).
  {
    uint32_t out[8];
    typedef void jr_fn(uint32_t, uint32_t*);
    jr_fn* const jr_j_a = (jr_fn*)(((uintptr_t)hwp_jr_j & 0x1FFFFFFFu) | 0xA0000000u);
    jr_fn* const jr_jal_a = (jr_fn*)(((uintptr_t)hwp_jr_jal & 0x1FFFFFFFu) | 0xA0000000u);
    hwp_exc_reset(HWP_EXPECT_RESUME);
    hwp_jr_j(0x20000000u, out + 0);
    jr_j_a(0, out + 2);
    hwp_jr_jal(0x20000000u, out + 4);
    jr_jal_a(0, out + 6);
    rec_series(S_JUMP_REGION, 4, 8, out);
  }

  // CVT.S.L / CVT.D.L around the 2^53 and 2^55 limits.
  static const int64_t in[12] = {
      (1LL << 53),  -(1LL << 53),  (1LL << 55) - 1,  -((1LL << 55) - 1), (1LL << 55),
      -(1LL << 55), (1LL << 55) + 1, -((1LL << 55) + 1), (1LL << 62), -(1LL << 62),
      0x7FFFFFFFFFFFFFFFLL, (int64_t)0x8000000000000000ULL};
  static uint64_t cv[12 * 7];
  for (int i = 0; i < 12; ++i) {
    uint64_t out[2];
    uint64_t* o = &cv[7 * i];
    o[0] = (uint64_t)in[i];
    hwp_exc_reset(HWP_EXPECT_RESUME);
    hwp_cvt_s_l(in[i], out);
    o[1] = out[0];
    o[2] = hwp_exc.n ? hwp_exc.fcsr : out[1];
    o[3] = hwp_exc.n ? ((hwp_exc.first_cause >> 2) & 0x1F) + 1 : 0;
    hwp_exc_reset(HWP_EXPECT_RESUME);
    hwp_cvt_d_l(in[i], out);
    o[4] = out[0];
    o[5] = hwp_exc.n ? hwp_exc.fcsr : out[1];
    o[6] = hwp_exc.n ? ((hwp_exc.first_cause >> 2) & 0x1F) + 1 : 0;
  }
  rec_series(S_CVT_L, 8, 12 * 7, cv);

  // CACHE op cost. I-cache ops work on the NOP sled's lines (2 KiB = 64 lines), D-cache ops on
  // a 1 KiB buffer (64 lines).
  static uint32_t dbuf[256] __attribute__((aligned(16)));
  const uint32_t sled = (uint32_t)(uintptr_t)hwp_nop_sled;
  for (int op = 0; op < 10; ++op) {
    for (int r = 0; r < 32; ++r) {
      uint32_t t = 0;
      switch (op) {
        case 0: t = hwp_cop_nop(sled); break;
        case 1: hwp_nop_sled(); t = hwp_cop_i_index_inv(sled); break;
        case 2: hwp_nop_sled(); t = hwp_cop_i_hit_inv(sled); break;
        case 3: icache_inv((void*)sled, 2048); t = hwp_cop_i_hit_inv(sled); break;
        case 4: icache_inv((void*)sled, 2048); t = hwp_cop_i_fill(sled); break;
        case 5:
          dcache_wbinv(dbuf, sizeof dbuf);
          for (int i = 0; i < 256; i += 4) (void)((volatile uint32_t*)dbuf)[i];
          t = hwp_cop_d_index_wbinv((uint32_t)dbuf);
          break;
        case 6:
          for (int i = 0; i < 256; i += 4) ((volatile uint32_t*)dbuf)[i] = i;
          t = hwp_cop_d_index_wbinv((uint32_t)dbuf);
          break;
        case 7:
          dcache_wbinv(dbuf, sizeof dbuf);
          for (int i = 0; i < 256; i += 4) (void)((volatile uint32_t*)dbuf)[i];
          t = hwp_cop_d_hit_inv((uint32_t)dbuf);
          break;
        case 8:
          dcache_wbinv(dbuf, sizeof dbuf);
          for (int i = 0; i < 256; i += 4) (void)((volatile uint32_t*)dbuf)[i];
          t = hwp_cop_d_hit_wb((uint32_t)dbuf);
          break;
        case 9:
          for (int i = 0; i < 256; i += 4) ((volatile uint32_t*)dbuf)[i] = i;
          t = hwp_cop_d_hit_wb((uint32_t)dbuf);
          break;
      }
      buf32[32 * op + r] = t;
    }
  }
  rec_series(S_CACHE_OPS, 4, 10 * 32, buf32);
}

// ---------------------------------------------------------------------------------------------
// TLB shutdown: two entries for one VPN2. Stage 1 uses V = 1, stage 2 V = 0.
static uint32_t shutdown_page[2048] __attribute__((aligned(8192)));

void grp_tlb_shutdown(void* unused) {
  (void)unused;
  const bool v = hwp_stage == 1;
  shutdown_page[0] = 0x600DF00Du;
  dcache_wb(shutdown_page, 16);
  const uint32_t phys = PHYS(shutdown_page);
  const uint64_t e[4] = {0, 0x00600000u | entryhi_asid(), entrylo(phys, 3, true, v),
                         entrylo(phys + 0x1000, 3, true, v)};
  uint32_t out[8];
  hwp_exc_reset(HWP_EXPECT_RESUME);
  hwp_tlb_shutdown(e, out);
  const uint32_t rec[10] = {v,
                            out[0],
                            out[1],
                            out[2],
                            out[3],
                            hwp_exc.n ? hwp_exc.first_cause : 0,
                            hwp_exc.n ? (uint32_t)hwp_exc.first_badvaddr : 0,
                            hwp_exc.n ? (uint32_t)hwp_exc.first_epc - (uint32_t)(uintptr_t)hwp_tlb_shutdown
                                      : 0,
                            out[4],
                            out[5]};
  rec_series(S_TLB_SHUTDOWN, 4, 10, rec);
}

// ---------------------------------------------------------------------------------------------
// RDRAM patterns. Loads read anywhere in the first 4 MiB; stores stay inside a 512 KiB scratch
// window between the 480i framebuffer and the 240p one.
#define SCRATCH_PHYS 0x00240000u
static const uint32_t strides[] = {0,    4,    8,     16,    32,      64,       128,
                                   256,  512,  1024,  2048,  4096,    8192,     16384,
                                   65536, 1u << 20, 2u << 20};
#define NSTRIDES (sizeof strides / sizeof strides[0])

void grp_rdram(void* unused) {
  (void)unused;
  static uint32_t out[4 * 2 * NSTRIDES * 8];
  static uint8_t dirty[8192] __attribute__((aligned(16)));
  uint32_t n = 0;
  for (int vi = 0; vi < 2; ++vi) {
    hwp_vi_mode(vi == 0 ? HWP_VI_OFF : HWP_VI_240P);
    for (int kind = 0; kind < 4; ++kind) {
      for (uint32_t s = 0; s < NSTRIDES; ++s) {
        for (int r = 0; r < 8; ++r) {
          uint32_t t = 0;
          switch (kind) {
            case 0:  // uncached loads
              t = hwp_rd_load(0xA0000000u, strides[s], 0x003FFFFCu);
              break;
            case 1:  // cached fills of clean, invalid lines
              data_cache_writeback_invalidate_all();
              t = hwp_rd_load(0x80000000u | SCRATCH_PHYS, strides[s], 0x0007FFFCu);
              break;
            case 2:  // cached fills that each evict a dirty line
              for (int i = 0; i < 8192; i += 16) ((volatile uint8_t*)dirty)[i] = (uint8_t)i;
              t = hwp_rd_load(0x80000000u | SCRATCH_PHYS, strides[s], 0x0007FFFCu);
              break;
            case 3:  // uncached stores, drained
              t = hwp_rd_store(0xA0000000u | SCRATCH_PHYS, strides[s], 0x0007FFFCu);
              break;
          }
          out[n++] = t;
        }
      }
    }
  }
  hwp_screen_show();
  rec_series(S_RDRAM, 4, n, out);
  rec_series(S_RDRAM_STRIDES, 4, NSTRIDES, strides);
}

// ---------------------------------------------------------------------------------------------
#define PI_REG(i) (((volatile uint32_t*)0xA4600000u)[i])
enum { PIR_DRAM = 0, PIR_CART, PIR_RD, PIR_WR, PIR_STATUS, PIR_LAT1, PIR_PWD1, PIR_PGS1, PIR_RLS1 };

static uint32_t pi_buf[2][2048] __attribute__((aligned(16)));

static uint32_t pi_time(uint32_t len) {
  dcache_wbinv(pi_buf[0], len);
  while (PI_REG(PIR_STATUS) & 3) {}
  PI_REG(PIR_DRAM) = PHYS(pi_buf[0]);
  PI_REG(PIR_CART) = 0x10001000u;
  const uint32_t t0 = c0_count();
  PI_REG(PIR_WR) = len - 1;
  while (PI_REG(PIR_STATUS) & 1) {}
  return c0_count() - t0;
}

void grp_pi(void* unused) {
  (void)unused;
  const uint32_t orig[4] = {PI_REG(PIR_LAT1), PI_REG(PIR_PWD1), PI_REG(PIR_PGS1), PI_REG(PIR_RLS1)};
  // Timing sets: as booted, then fast to slow. LAT 5 / PWD 12 / RLS 2 are Flash's values
  // (n64brew "Flash"); 0x40 / 0x12 / 7 / 3 the usual ROM values (n64brew "Parallel Interface").
  const uint32_t sets[7][4] = {{orig[0], orig[1], orig[2], orig[3]},
                               {0x05, 0x0C, 0x07, 0x02},
                               {0x40, 0x12, 0x07, 0x03},
                               {0x40, 0x12, 0x00, 0x03},
                               {0x40, 0x12, 0x0F, 0x03},
                               {0x80, 0x40, 0x07, 0x03},
                               {0xFF, 0xFF, 0x07, 0x03}};
  static const uint32_t sizes[13] = {2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192};
  static uint32_t out[7 * 13 * 8];
  uint32_t n = 0;
  for (int s = 0; s < 7; ++s) {
    PI_REG(PIR_LAT1) = sets[s][0];
    PI_REG(PIR_PWD1) = sets[s][1];
    PI_REG(PIR_PGS1) = sets[s][2];
    PI_REG(PIR_RLS1) = sets[s][3];
    for (int z = 0; z < 13; ++z)
      for (int r = 0; r < 8; ++r) out[n++] = pi_time(sizes[z]);
  }
  for (int i = 0; i < 4; ++i) PI_REG(PIR_LAT1 + i) = orig[i];
  rec_series(S_PI_DMA, 4, n, out);
  uint32_t cfg[7 * 4 + 13];
  memcpy(cfg, sets, sizeof sets);
  memcpy(cfg + 28, sizes, sizeof sizes);
  rec_series(S_PI_DMA_CONFIG, 4, 7 * 4 + 13, cfg);

  // Register writes during a busy 8 KiB DMA: DRAM_ADDR, CART_ADDR, WR_LEN, STATUS = 2. RD_LEN
  // is left out: if the PI took it, it would write cartridge ROM space, which a flashcart may
  // let through to the image a soft reset boots from.
  static uint32_t busy[4 * 6];
  for (int c = 0; c < 4; ++c) {
    for (int i = 0; i < 2048; ++i) pi_buf[0][i] = pi_buf[1][i] = 0xA5A5A5A5u;
    dcache_wbinv(pi_buf, sizeof pi_buf);
    while (PI_REG(PIR_STATUS) & 3) {}
    PI_REG(PIR_DRAM) = PHYS(pi_buf[0]);
    PI_REG(PIR_CART) = 0x10001000u;
    PI_REG(PIR_WR) = 8192 - 1;
    switch (c) {
      case 0: PI_REG(PIR_DRAM) = PHYS(pi_buf[1]); break;
      case 1: PI_REG(PIR_CART) = 0x10003000u; break;
      case 2: PI_REG(PIR_WR) = 8 - 1; break;
      case 3: PI_REG(PIR_STATUS) = 2; break;
    }
    const uint32_t st0 = PI_REG(PIR_STATUS);
    while (PI_REG(PIR_STATUS) & 3) {}
    uint32_t* b = &busy[6 * c];
    b[0] = st0;
    b[1] = PI_REG(PIR_STATUS);
    b[2] = PI_REG(PIR_DRAM);
    b[3] = PI_REG(PIR_CART);
    dcache_inv(pi_buf, sizeof pi_buf);
    b[4] = pi_buf[0][0];
    b[5] = pi_buf[1][0];
    PI_REG(PIR_STATUS) = 3;  // reset the controller and clear the interrupt
  }
  rec_series(S_PI_BUSY, 4, 4 * 6, busy);

  // An IO read of ROM while a DMA runs.
  dcache_wbinv(pi_buf[0], 8192);
  while (PI_REG(PIR_STATUS) & 3) {}
  PI_REG(PIR_DRAM) = PHYS(pi_buf[0]);
  PI_REG(PIR_CART) = 0x10001000u;
  PI_REG(PIR_WR) = 8192 - 1;
  const uint32_t t0 = c0_count();
  const uint32_t v = *(volatile uint32_t*)0xB0000000u;
  const uint32_t t1 = c0_count();
  const uint32_t io[3] = {v, t1 - t0, PI_REG(PIR_STATUS)};
  while (PI_REG(PIR_STATUS) & 3) {}
  PI_REG(PIR_STATUS) = 2;
  rec_series(S_PI_IO_DURING_DMA, 4, 3, io);
}

// ---------------------------------------------------------------------------------------------
#define SI_REG(i) (((volatile uint32_t*)0xA4800000u)[i])
enum { SIR_DRAM = 0, SIR_RD64 = 1, SIR_WR64 = 4, SIR_STATUS = 6 };
#define PIF_RAM 0x1FC007C0u

static uint64_t pif_block[8] __attribute__((aligned(16)));

void grp_si_ai(void* unused) {
  (void)unused;
  // Controller status (Joybus command 0x00) on channels 0-3, then the end marker 0xFE and the
  // control byte 0x01 (n64brew "PIF-NUS", Joybus commands; "Joybus Protocol").
  uint8_t* p = (uint8_t*)pif_block;
  for (int r = 0; r < 64; ++r) buf32[r] = 0;
  uint32_t n = 0;
  for (int rep = 0; rep < 32; ++rep) {
    memset(p, 0, 64);
    for (int ch = 0; ch < 4; ++ch) {
      uint8_t* c = p + 6 * ch;
      c[0] = 0x01;  // tx length
      c[1] = 0x03;  // rx length
      c[2] = 0x00;  // command: status
      c[3] = c[4] = c[5] = 0xFF;
    }
    p[24] = 0xFE;
    p[63] = 0x01;
    dcache_wb(pif_block, 64);
    while (SI_REG(SIR_STATUS) & 3) {}
    SI_REG(SIR_DRAM) = PHYS(pif_block);
    uint32_t t0 = c0_count();
    SI_REG(SIR_WR64) = PIF_RAM;
    while (SI_REG(SIR_STATUS) & 3) {}
    buf32[n++] = c0_count() - t0;
    SI_REG(SIR_STATUS) = 0;  // clear the SI interrupt
    dcache_inv(pif_block, 64);
    SI_REG(SIR_DRAM) = PHYS(pif_block);
    t0 = c0_count();
    SI_REG(SIR_RD64) = PIF_RAM;
    while (SI_REG(SIR_STATUS) & 3) {}
    buf32[n++] = c0_count() - t0;
    SI_REG(SIR_STATUS) = 0;
  }
  rec_series(S_SI_DMA, 4, n, buf32);
  dcache_inv(pif_block, 64);
  rec_series(S_SI_RESULT, 1, 64, pif_block);

  // AI: 4 KiB of silence at 44100 Hz. DACRATE = VI clock / rate - 1 for NTSC (48.681812 MHz,
  // n64brew "Audio Interface"), BITRATE 15.
  static uint32_t silence[1024] __attribute__((aligned(8)));
  memset(silence, 0, sizeof silence);
  dcache_wb(silence, sizeof silence);
  volatile uint32_t* ai = (volatile uint32_t*)0xA4500000u;
  const uint32_t dacrate = 48681812u / 44100u - 1, bitrate = 15, spacing = 4000;
  ai[4] = dacrate;
  ai[5] = bitrate;
  ai[2] = 1;  // DMA enable
  ai[0] = PHYS(silence);
  const uint32_t start = c0_count();
  ai[1] = sizeof silence;
  static uint32_t smp[256 * 3];
  for (int i = 0; i < 256; ++i) {
    while (c0_count() - start < (uint32_t)i * spacing) {}
    smp[3 * i + 0] = c0_count() - start;
    smp[3 * i + 1] = ai[1];
    smp[3 * i + 2] = ai[3];
  }
  ai[2] = 0;
  ai[3] = 0;  // clear the AI interrupt
  rec_series(S_AI_SAMPLES, 4, 256 * 3, smp);
  const uint32_t cfg[4] = {dacrate, bitrate, sizeof silence, spacing};
  rec_series(S_AI_CONFIG, 4, 4, cfg);
}
