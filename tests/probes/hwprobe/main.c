// hwprobe driver: stage selection, group sequencing, record finish, reset wait.
// Specification: docs/timing/hwprobe.md.
#include <libdragon.h>
#include <string.h>

#include "hwp.h"

int hwp_stage;
uint32_t hwp_memsize;

extern int __boot_memsize, __boot_resettype;
extern char __bss_end[];

static const hwp_group_t stage1_groups[] = {
    {0x01, "boot state", grp_boot},
    {0x30, "uncached-read loops", grp_loops},
    {0x50, "exceptions", grp_exc},
    {0x51, "micro-TLB", grp_itlb},
    {0x52, "CP0 hazards", grp_cp0},
    {0x53, "pipeline overlap", grp_overlap},
    {0x54, "CPU quirks", grp_quirks},
    {0x60, "RDRAM patterns", grp_rdram},
    {0x70, "PI", grp_pi},
    {0x71, "SI and AI", grp_si_ai},
    {0x55, "TLB shutdown", grp_tlb_shutdown},  // last: may disable the TLB until reset
};
static const hwp_group_t stage2_groups[] = {
    {0x02, "boot state after reset", grp_boot},
    {0x55, "TLB shutdown (V=0)", grp_tlb_shutdown},
};

static uint32_t group_log[3 * 16];
static uint32_t fault_log[5 * 16];
static int ngroups, nfaults;

static void run_groups(const hwp_group_t* g, int n, int first_row) {
  for (int i = 0; i < n; ++i) {
    hwp_screen_show();
    hwp_print(first_row + i, "%-24s ...", g[i].name);
    hwp_exc_reset(HWP_EXPECT_NONE);
    const uint32_t faults0 = hwp_exc.faults;
    const uint32_t t0 = c0_count();
    const int r = hwp_try(g[i].run, NULL);
    const uint32_t t1 = c0_count();
    hwp_screen_show();
    group_log[3 * ngroups + 0] = g[i].id;
    group_log[3 * ngroups + 1] = t1 - t0;
    group_log[3 * ngroups + 2] = hwp_exc.faults - faults0;
    if (ngroups < 15) ++ngroups;
    if (r != 0) {
      uint32_t* f = &fault_log[5 * nfaults];
      f[0] = g[i].id;
      f[1] = hwp_exc.cause;
      f[2] = (uint32_t)hwp_exc.epc;
      f[3] = (uint32_t)hwp_exc.badvaddr;
      f[4] = hwp_exc.status;
      if (nfaults < 15) ++nfaults;
      hwp_print(first_row + i, "%-24s FAULT %08lx", g[i].name, (unsigned long)hwp_exc.cause);
    } else {
      hwp_print(first_row + i, "%-24s ok", g[i].name);
    }
    rec_flush();
  }
}

static void finish(int row) {
  rec_series(S_GROUP_TIME, 4, 3 * ngroups, group_log);
  if (nfaults) rec_series(S_FAULT, 4, 5 * nfaults, fault_log);
  const bool ok = rec_finish(0);
  hwp_print(row, "record %lu B, %lu series, crc %08lx", (unsigned long)rec_bytes(),
            (unsigned long)rec_series_count(), (unsigned long)rec_crc());
  if (ok)
    hwp_print(row + 1, rec_program_errors() ? "written and verified (odd status %02lx)"
                                            : "written and verified",
              (unsigned long)flash_last_status());
  else
    hwp_print(row + 1, "FLASHRAM WRITE FAILED (status %02lx)", (unsigned long)flash_last_status());
  // The record, for emulator runs and USB-attached carts.
  debugf("HWP RECORD STAGE %d BYTES %lu CRC %08lx\n", hwp_stage, (unsigned long)rec_bytes(),
         (unsigned long)rec_crc());
}

// Stage 1 ends here: wait for RESET with the pre-NMI interrupt armed. The loop stores Count to
// low RAM every iteration so stage 2 can time pre-NMI to NMI.
extern void hwp_reset_wait(void);
extern char hwp_reset_wait_end[];

extern void hwp_tlb_write(int index, const uint64_t e[4]);
extern void hwp_tlb_read(int index, uint64_t e[4]);

// Before waiting for RESET: a distinct pattern in all 32 TLB entries (distinct VPN2s, so no
// shutdown), read back at once and again by stage 2. PFNs point at low RDRAM, uncached; nothing
// accesses the mapped addresses.
static void tlb_pattern(void) {
  static uint64_t pat[32][4], back[32][4];
  for (int i = 0; i < 32; ++i) {
    pat[i][0] = (i & 1) ? 0x6000 : 0;                       // PageMask: 16 KiB on odd entries
    pat[i][1] = 0x01000000u + (uint32_t)i * 0x40000u + i;   // VPN2, ASID = i
    pat[i][2] = ((uint64_t)(2 * i) << 6) | (2 << 3) | 0x6;  // PFN 2i, uncached, D, V
    pat[i][3] = ((uint64_t)(2 * i + 1) << 6) | (2 << 3) | 0x2;
    hwp_tlb_write(i, pat[i]);
  }
  for (int i = 0; i < 32; ++i) hwp_tlb_read(i, back[i]);
  rec_series(S_NMI_TLB_PATTERN, 8, 32 * 4, pat);
  rec_series(S_NMI_TLB_READBACK, 8, 32 * 4, back);
  const uint32_t loop[2] = {(uint32_t)(uintptr_t)hwp_reset_wait,
                            (uint32_t)(hwp_reset_wait_end - (char*)hwp_reset_wait)};
  rec_series(S_NMI_WAIT_LOOP, 4, 2, loop);
}

int main(void) {
  hwp_snapshot_early();
  // No interrupts during the run, except where a probe enables one.
  C0_WRITE_STATUS(C0_STATUS() & ~(C0_STATUS_IE | 0xFF00));
  hwp_memsize = (uint32_t)__boot_memsize;
  debug_init_isviewer();
  hwp_screen_init();
  hwp_vectors_install();

  const bool waiting = *HWP_LOW_MAGIC == HWP_WAIT_MAGIC;
  hwp_stage = (__boot_resettype == 1 && waiting) ? 2 : 1;
  *HWP_LOW_MAGIC = 0;

  hwp_print(0, "Ultra hwprobe %s  stage %d", HWP_BUILD_ID, hwp_stage);
  if ((uint32_t)__bss_end > 0x80100000u) {
    hwp_print(1, "BUILD ERROR: program overlaps 480i fb");
    for (;;) {}
  }

  const uint64_t id = flash_init();
  if (!flash_present(id)) {
    hwp_print(1, "FlashRAM id %08lx%08lx unexpected", (unsigned long)(id >> 32), (unsigned long)id);
    hwp_print(2, "save type must be FlashRAM; trying anyway");
  } else {
    hwp_print(1, "FlashRAM %08lx%08lx", (unsigned long)(id >> 32), (unsigned long)id);
  }

  if (hwp_stage == 1) {
    if (!flash_erase_chip()) hwp_print(2, "FlashRAM erase reported failure");
    rec_begin(1, id);
    run_groups(stage1_groups, sizeof stage1_groups / sizeof stage1_groups[0], 3);
    tlb_pattern();
    finish(16);
    hwp_print(19, "STAGE 1 DONE");
    hwp_print(20, "RESET: stage 2   power off: finished");
    *HWP_LOW_PRENMI = 0;
    *HWP_LOW_PRENMI_N = 0;
    *HWP_LOW_LASTCOUNT = 0;
    *HWP_LOW_MAGIC = HWP_WAIT_MAGIC;
    hwp_exc_reset(HWP_EXPECT_PRENMI);
    hwp_reset_wait();
  } else {
    if (!flash_erase_sector(HWP_STAGE2_BASE / 128)) hwp_print(2, "FlashRAM erase reported failure");
    rec_begin(2, id);
    run_groups(stage2_groups, sizeof stage2_groups / sizeof stage2_groups[0], 3);
    finish(16);
    hwp_print(19, "STAGE 2 DONE: power off");
  }
  for (;;) {}
}
