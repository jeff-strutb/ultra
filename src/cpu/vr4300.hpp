#pragma once
// NEC VR4300 interpreter with a pipeline timing model.
// Facts and sources: docs/hardware/cpu.md. Manual: NEC VR4300 User's Manual U10504EJ7V0UM00.
//
// Execution model (docs/adr/0005-cpu-pipeline-stages.md): one instruction executes per step(),
// in program order. Time is accounted by pipeline stage. Step i spends one run cycle (i in
// EX), i's EX interlocks (LDI, MCI, FPU bypass), then the RF fetch of instruction i+1 (I-cache
// miss, uncached fetch, micro-TLB miss), then i's DC work (D-cache miss/busy, SysAD
// transactions). Stalls of one pipeline cycle combine by the manual's rules: the RF fetch
// overlaps i's EX interlocks, and i's MCI overlaps i-1's DC stall. Exceptions cost by the
// stage that detects them. Bus transactions are issued at MasterClock edges and the CPU
// resumes at the first PCycle after completion, so all timing is integer and deterministic.

#include <array>

#include "common/types.hpp"
#include "core/bus.hpp"
#include "core/clock.hpp"
#include "core/cpu_pins.hpp"
#include "cpu/cop0_regs.hpp"
#include "cpu/timing.hpp"

namespace ultra {

class SaveState;
class Scheduler;

class Vr4300 {
 public:
  Vr4300(Bus& bus, Scheduler& sched, CpuPins& pins);

  // Cold reset: PC = 0xBFC0_0000, Status.BEV/ERL set, Random = 31, Wired = 0, Config
  // EP/BE initialised (VR4300 manual 6.4.4, 5.4.2, 5.4.4, 5.4.6).
  void cold_reset();
  void soft_reset();
  void nmi();

  // Execute until the CPU's time reaches `limit` (in MasterClock ticks) or it freezes.
  void run_until(Tick limit);
  // Execute exactly one instruction (tests, debugger).
  void step();

  Tick now_tick() const noexcept { return cpu_to_tick_floor(cycles_); }
  CpuCycle cycles() const noexcept { return cycles_; }
  bool frozen() const noexcept { return frozen_; }
  u64 instructions_retired() const noexcept { return retired_; }

  // Register access for tests, harness and boot setup.
  u64 gpr(u32 i) const noexcept { return gpr_[i]; }
  void set_gpr(u32 i, u64 v) noexcept {
    if (i) gpr_[i] = v;
  }
  u64 pc() const noexcept { return pc_; }
  void set_pc(u64 pc) noexcept {
    pc_ = pc;
    npc_ = pc + 4;
    in_delay_slot_ = false;
    branch_pending_ = false;
    fetch_latch_.valid = false;
  }
  u64 hi() const noexcept { return hi_; }
  u64 lo() const noexcept { return lo_; }
  u64 cop0_read64(u32 reg);
  void cop0_write64(u32 reg, u64 v);
  u64 fpr_raw(u32 i) const noexcept { return fpr_[i]; }
  u32 fcr31() const noexcept { return fcr31_; }

  // The PIF can hold the CPU (failed IPL2 checksum, boot timeout): it never runs again until
  // a reset.
  void freeze() noexcept { frozen_ = true; }

  void serialize(SaveState& s);

  // Debug instruction history (off by default; costs one branch per instruction when off).
  struct HistoryEntry {
    u64 pc;
    u32 instr;
    CpuCycle cycle;
  };
  void enable_history(bool on) noexcept { history_on_ = on; }
  // Debug: freeze on the first exception so the history shows its cause.
  void break_on_exception(bool on) noexcept { break_on_exception_ = on; }
  template <typename F>
  void for_each_history(F&& f) const {
    for (u32 i = 0; i < kHistory; ++i) {
      const HistoryEntry& e = history_[(history_pos_ + i) % kHistory];
      if (e.cycle || e.pc) f(e);
    }
  }

  // ---- constants -------------------------------------------------------------------------
  // PRId as read on revision 2.2 retail units (n64brew "VR4300", Revision Identifiers).
  static constexpr u32 kPRId = 0x0000'0B22;
  // FCR0: implementation 0x0B, revision 0x00 (n64brew "VR4300", Revision Identifiers).
  static constexpr u32 kFcr0 = 0x0000'0B00;

 private:
  // ---- caches (VR4300 manual chapter 11) ---------------------------------------------------
  // I-cache: 16 KiB, direct mapped, 32-byte lines, virtually indexed, physically tagged.
  // D-cache: 8 KiB, direct mapped, 16-byte lines, write-back.
  static constexpr u32 kICacheLines = 512;
  static constexpr u32 kICacheLineBytes = 32;
  static constexpr u32 kDCacheLines = 512;
  static constexpr u32 kDCacheLineBytes = 16;

  struct ILine {
    u32 ptag = 0;  // physical address bits 31:12
    bool valid = false;
    std::array<u8, kICacheLineBytes> data{};
  };
  struct DLine {
    u32 ptag = 0;
    bool valid = false;
    bool dirty = false;
    std::array<u8, kDCacheLineBytes> data{};
  };

  struct TlbEntry {
    u64 entry_hi = 0;
    u64 entry_lo0 = 0;
    u64 entry_lo1 = 0;
    u32 page_mask = 0;
    // Derived lookup helpers.
    u64 vpn2_mask = 0;  // mask applied to the virtual address (bits 39:13 minus page bits)
    bool global = false;
  };

  enum class Access : u8 { Fetch, Load, Store };

  struct Translation {
    u32 paddr;
    bool cached;
  };

  // ---- execution ---------------------------------------------------------------------------
  void execute(u32 instr);
  void exec_special(u32 instr);
  void exec_regimm(u32 instr);
  void exec_cop0(u32 instr);
  void exec_cop1(u32 instr);
  void exec_cop2(u32 instr);
  void exec_cache(u32 instr);

  // `report` false: an address or TLB fault fails without raising (the instruction behind
  // one that takes an exception is discarded, so its fetch fault is never taken).
  bool fetch(u64 vaddr, u32& instr, bool report = true);
  // `report` false: fail without raising the exception (a fetch the pipeline discards).
  bool translate(u64 vaddr, Access acc, Translation& out, bool report = true);
  bool tlb_translate(u64 vaddr, Access acc, Translation& out, bool report = true);
  void tlb_refresh_entry(u32 i);
  void tlb_probe();
  void tlb_read();
  void tlb_write(u32 index);
  u32 random_reg() const noexcept;
  // Random after `n` instructions counting down from 31 with the given Wired.
  static u32 random_value(u64 n, u32 wired) noexcept;

  // Data access. Return false after raising an exception.
  bool load_bytes(u64 vaddr, u32 size, u8* out_word, u32 lane_bytes);
  // `partial`: LWL/LWR/LDL/LDR access the aligned unit containing vaddr but translate (and
  // report BadVAddr for) the unaligned address itself.
  template <typename T>
  bool load(u64 vaddr, T& out, bool partial = false);
  // Stores the low sizeof(T) bytes of `reg`. For sub-word stores the VR4300 drives SysAD with
  // the register shifted into the target byte lanes, so its other bytes leak into lanes that
  // devices ignoring the access size will see (n64brew "Memory map", Physical Memory Map
  // accesses; n64-systemtest cart-writing / pifram SB tests).
  template <typename T>
  bool store(u64 vaddr, u64 reg);
  // `xlate_vaddr` is the address translated (and reported on exceptions); `vaddr` the address
  // presented on the bus (they differ for SWL/SWR/SDL/SDR).
  bool store_bus(u64 vaddr, u32 size, const u8* bus_word, u32 cache_offset, u32 cache_len,
                 const u8* cache_bytes, u64 xlate_vaddr);
  bool load_aligned_word(u64 vaddr, u32& out, Translation* tr = nullptr);
  bool load_aligned_dword(u64 vaddr, u64& out);
  bool check_watch(u32 paddr, Access acc);

  // Cache helpers.
  DLine& dline(u64 vaddr) noexcept { return dcache_[(vaddr >> 4) & (kDCacheLines - 1)]; }
  ILine& iline(u64 vaddr) noexcept { return icache_[(vaddr >> 5) & (kICacheLines - 1)]; }
  bool dcache_fill(DLine& line, u32 paddr);
  void dcache_writeback(DLine& line, u32 index);
  bool icache_fill(ILine& line, u32 paddr);

  // Bus helpers: issue a SysAD transaction at the next MasterClock edge and stall until done.
  bool bus_read(Master m, u32 paddr, u32 size, u8* out);
  bool bus_write(Master m, u32 paddr, u32 size, const u8* data);
  void drain_write_buffer();
  void stall_until_tick(Tick done);

  // Pipeline stages (docs/adr/0005-cpu-pipeline-stages.md).
  enum class Stage : u8 { RF, EX, DC, Interrupt };
  static Stage stage_of(cop0::ExcCode code) noexcept;
  // Issues the RF fetch of the instruction behind the executing one into fetch_latch_. It starts
  // at the run cycle, so its stall overlaps the EX interlocks already charged.
  void rf_fetch();
  // Marks the start of the executing instruction's DC stage; issues the RF fetch first.
  void dc_stage();
  // Multicycle interlock of `extra` PCycles beyond the run cycle (manual 4.6.4).
  void mci(u64 extra);
  // Charges the time from detection in `stage` until the handler's first instruction is in RF.
  void exception_timing(Stage stage);
  // Micro-TLB lookup for a fetch translated by the joint TLB; charges ITM on a miss.
  void itlb_access(u64 vaddr);
  void itlb_flush() noexcept;
  void set_entry_hi(u64 v) noexcept;

  // Exceptions. `stage` defaults to the stage that detects `code` (Figure 4-12).
  void raise(cop0::ExcCode code, u32 ce = 0);
  void raise_in(cop0::ExcCode code, u32 ce, Stage stage);
  // Cause.CE for an exception raised by CTC1: from the instruction behind it.
  u32 next_instr_cop();
  void raise_address_error(u64 vaddr, Access acc);
  void raise_tlb(u64 vaddr, Access acc, bool miss, bool modification);
  u64 exception_vector(bool tlb_refill, bool xtlb) const noexcept;
  bool check_interrupts();

  // Mode helpers.
  bool kernel_mode() const noexcept;
  bool supervisor_mode() const noexcept;
  bool user_mode() const noexcept;
  bool addr64() const noexcept;  // 64-bit addressing in the current mode
  bool ops64() const noexcept;   // 64-bit operations permitted (else RI for 64-bit opcodes)
  bool cop_usable(u32 n) const noexcept;
  // Status.RE in user mode reverses the endianness of data and instruction accesses
  // (VR4300 manual 6.3.5; behaviour per n64-systemtest src/tests/endian_re).
  bool reverse_endian() const noexcept { return user_mode() && cop0::Status::RE::test(status_); }
  // Partial-word store: writes lanes [lane_lo, lane_hi] of `value` laid out big-endian in a
  // `unit`-byte (4 or 8) aligned unit at `unit_vaddr`; exceptions report `xlate_vaddr`.
  bool store_lanes(u64 unit_vaddr, u32 unit, u64 value, u32 lane_lo, u32 lane_hi, u64 xlate_vaddr);

  // COP0.
  u64 cop0_get(u32 reg);
  void cop0_set(u32 reg, u64 value);
  void update_timer() noexcept;
  u32 count_reg() const noexcept;
  void eret();

  // COP1 (cop1.cpp).
  bool fpu_check_usable();
  void fpu_execute(u32 instr);
  u64 fpr_get_s(u32 r) const noexcept;
  u64 fpr_get_d(u32 r) const noexcept;
  void fpr_set_s(u32 r, u32 v) noexcept;
  void fpr_set_d(u32 r, u64 v) noexcept;
  // Computational operands: with Status.FR = 0 the low bit of the fs index is ignored (ft is
  // used as-is); results write all 64 bits of the destination index (single results
  // zero-extended). Source: n64-systemtest src/tests/cop1/full_vs_half_mode.rs.
  u64 fpu_src(u32 r) const noexcept { return fpr_[cop0::Status::FR::test(status_) ? r : r & ~1u]; }
  u64 fpu_src_ft(u32 r) const noexcept { return fpr_[r]; }
  void fpu_dst(u32 r, u64 v) noexcept { fpr_[r] = v; }
  void ctc1(u32 r, u32 v);
  u32 cfc1(u32 r) const;

  // Load interlock bookkeeping (n64brew "VR4300", Load Delay Interlock).
  enum class LoadTarget : u8 { None, Gpr, Fpr };
  void note_load(LoadTarget kind, u32 reg) noexcept {
    pending_load_kind_ = kind;
    pending_load_reg_ = reg;
  }

  // ---- state -------------------------------------------------------------------------------
  Bus& bus_;
  Scheduler& sched_;
  CpuPins& pins_;

  std::array<u64, 32> gpr_{};
  u64 hi_ = 0, lo_ = 0;
  u64 pc_ = 0;    // address of the instruction about to execute
  u64 npc_ = 0;   // address of the instruction after it
  bool in_delay_slot_ = false;   // instruction at pc_ is in a branch delay slot
  bool branch_pending_ = false;  // set by a branch/jump executing now
  u64 cur_pc_ = 0;               // address of the instruction executing now
  bool cur_in_delay_ = false;
  bool exception_ = false;       // the current instruction raised an exception
  bool llbit_ = false;

  // COP0
  u32 index_ = 0;
  u64 entry_lo0_ = 0, entry_lo1_ = 0;
  u64 context_ = 0;
  u32 page_mask_ = 0;
  u32 wired_ = 0;
  // Retired-instruction count from which Random counts down from 31. A Wired write moves it
  // kRandomResetDelay instructions ahead; reads before then continue the previous sequence.
  u64 random_base_ = 0;
  u64 random_prev_base_ = 0;
  u32 random_prev_wired_ = 0;
  // Source: n64-systemtest src/tests/cop0/mod.rs RandomDecrement and RandomReadEarly (MTC0
  // Wired, then MFC0 Random: 1 instruction later reads the old sequence, 2 later reads 31).
  static constexpr u64 kRandomResetDelay = 2;
  u64 badvaddr_ = 0;
  u64 count_offset_ = 0; // Count = (cycles_ >> 1) + count_offset_ (mod 2^32)
  u64 entry_hi_ = 0;
  u32 compare_ = 0;
  u32 status_ = 0;
  u32 cause_ = 0;
  u64 epc_ = 0;
  u32 config_ = 0;
  u32 lladdr_ = 0;
  u32 watch_lo_ = 0;
  u32 watch_hi_ = 0;
  u64 xcontext_ = 0;
  u32 parity_error_ = 0;
  u32 tag_lo_ = 0;
  u32 tag_hi_ = 0;
  u64 error_epc_ = 0;
  u64 cop0_latch_ = 0;  // last value written through MTC0/DMTC0
  CpuCycle timer_cycle_ = ~CpuCycle{0};  // PCycle at which Count reaches Compare next

  // COP1
  std::array<u64, 32> fpr_{};
  u32 fcr31_ = 0;
  u32 cop1_latch_ = 0;
  u64 cop2_latch_ = 0;  // COP2 move latch (no COP2 exists on the VR4300)

  std::array<TlbEntry, 32> tlb_{};
  std::array<ILine, kICacheLines> icache_{};
  std::array<DLine, kDCacheLines> dcache_{};

  // Timing state.
  CpuCycle cycles_ = 0;
  u64 retired_ = 0;
  bool frozen_ = false;
  LoadTarget pending_load_kind_ = LoadTarget::None;  // load executed by the previous instr
  u32 pending_load_reg_ = 0;
  LoadTarget cur_load_kind_ = LoadTarget::None;
  u32 cur_load_reg_ = 0;
  bool prev_dcache_store_ = false;  // previous instruction stored into the D-cache
  bool cur_dcache_store_ = false;
  // Pipeline stage bookkeeping (ADR 0005). The latch holds the instruction fetched in RF while
  // the previous one was in EX; `ok` false means that fetch faulted, and the instruction's own
  // step fetches again and raises the fault in program order.
  struct FetchLatch {
    u64 vaddr = 0;
    u32 instr = 0;
    bool valid = false;
    bool ok = false;
  };
  FetchLatch fetch_latch_{};
  u64 next_fetch_vaddr_ = 0;     // the instruction behind the executing one, in fetch order
  bool rf_fetch_done_ = true;    // this step issued (or an EX exception cancelled) the RF fetch
  bool dc_started_ = false;      // this step reached its DC stage
  CpuCycle run_cycle_ = 0;       // end of the executing instruction's run (EX) cycle
  CpuCycle dc_begin_ = 0;        // start of its DC stage
  CpuCycle prev_dc_stall_ = 0;   // DC stall cycles of the previous instruction
  bool prev_ended_in_stall_ = false;  // the PCycle before this step boundary was a stall
  bool load_use_ = false;        // the executing instruction waits for the previous load (LDI)
  bool kill_slot_ = false;       // the executing instruction kills its fetched delay slot
  // Instruction micro-TLB: 4 KiB virtual pages (vaddr >> 12), most recently used first.
  struct ItlbEntry {
    u64 vpage = 0;
    bool valid = false;
  };
  std::array<ItlbEntry, timing::kItlbEntries> itlb_{};
  s32 fpu_result_reg_ = -1;         // FPR written by the previous FPU computational op
  s32 cur_fpu_result_reg_ = -1;
  // Write buffer (VR4300 manual 4.9): up to four pending SysAD writes. Each is presented to
  // the bus once the bus is free and the CPU has reached its issue time, so devices observe
  // the write at its true bus time.
  struct PendingWrite {
    u32 paddr = 0;
    u32 size = 0;
    Master master = Master::CpuData;
    Tick ready = 0;  // earliest MasterClock edge it can be issued
    std::array<u8, 16> data{};
  };
  Tick sysad_free_ = 0;  // first tick the SysAD bus is idle
  u32 wb_count_ = 0;
  u32 wb_head_ = 0;
  std::array<PendingWrite, 4> wb_{};

  static constexpr u32 kHistory = 256;
  bool history_on_ = false;
  bool break_on_exception_ = false;
  u32 history_pos_ = 0;
  std::array<HistoryEntry, kHistory> history_{};
  bool wb_issue_head();
};

}  // namespace ultra
