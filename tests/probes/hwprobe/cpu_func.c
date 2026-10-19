// CPU and FPU functional sweeps (series 0xA000-0xA0FF): results, not timing. Every result
// goes to the full record only (rec_series_full). docs/timing/hwprobe.md, "CPU functional".
//
// Instructions are built at runtime into a 16-byte buffer (cpu_func_asm.S) so that the sweeps
// can cover every encoding of a class, including the reserved ones. Encodings: VR4300 manual
// chapter 16 (CPU) and chapter 17 (FPU) opcode tables.
#include <libdragon.h>
#include <string.h>

#include "hwp.h"

extern void hwp_exec_one(uint32_t* buf);
extern void hwp_exec_fp(uint32_t* buf, uint64_t* io);
extern void hwp_exec_int(uint32_t* buf, uint64_t rs, uint64_t rt, uint64_t* out);
extern void hwp_unaligned(int op, uint32_t off, uint8_t* buf16, uint64_t* out);
extern void hwp_llsc(int kind, uint32_t* word, uint64_t* out);
extern uint32_t hwp_cache_tag(uint32_t op, uint32_t addr);
extern void hwp_tlb_write(int index, const uint64_t e[4]);

static uint32_t code[8] __attribute__((aligned(32)));
static uint64_t big[8192 * 3];

static void place(uint32_t insn) {
  code[0] = insn;
  code[1] = 0;            // nop: where a zero-offset branch or jump lands
  code[2] = 0x02000008u;  // jr $16
  code[3] = 0;
  dcache_wb(code, 16);
  icache_inv(code, 32);
}

// Cause, EPC - buffer, BadVAddr of the exception the last run took, or all ones for none.
static void exc_result(uint32_t* o) {
  if (hwp_exc.n) {
    o[0] = hwp_exc.first_cause;
    o[1] = (uint32_t)hwp_exc.first_epc - (uint32_t)(uintptr_t)code;
    o[2] = (uint32_t)hwp_exc.first_badvaddr;
  } else {
    o[0] = o[1] = o[2] = 0xFFFFFFFFu;
  }
}

// All 32 TLB entries invalid at distinct VPN2s far from anything used, so that the zero-base
// loads, stores and CACHE ops of the opcode sweep take a TLB miss instead of reaching memory.
static void tlb_quiet(void) {
  for (int i = 0; i < 32; ++i) {
    const uint64_t e[4] = {0, 0x7F000000u + (uint32_t)i * 0x20000u, 0, 0};
    hwp_tlb_write(i, e);
  }
  __asm__ volatile("dmtc0 $0, $10; dmtc0 $0, $2; dmtc0 $0, $3; mtc0 $0, $5; nop; nop");
}

// ---------------------------------------------------------------------------------------------
// Opcode sweep (S_OPCODE_SWEEP): every encoding of a class with all register fields 0 and a
// zero immediate. Record per encoding: the word, Cause, EPC offset, BadVAddr.
static uint32_t sweep_out[4 * 2600];
static uint32_t nsweep;

static void sweep_one(uint32_t insn) {
  place(insn);
  // ERET returns to EPC: point it at the way back. TLBR changes EntryHi: restored after.
  __asm__ volatile("dmtc0 %0, $14; nop; nop" ::"r"((uint64_t)(int32_t)(uintptr_t)&code[2]));
  hwp_exc_reset(HWP_EXPECT_RESUME);
  hwp_exec_one(code);
  __asm__ volatile("dmtc0 $0, $10; nop; nop");
  uint32_t* o = &sweep_out[4 * nsweep++];
  o[0] = insn;
  exc_result(o + 1);
}

static void opcode_sweep(void) {
  tlb_quiet();
  nsweep = 0;
  // Primary opcodes. J and JAL (2, 3) are left out: a zero target is the exception vector page.
  for (uint32_t op = 0; op < 64; ++op)
    if (op != 2 && op != 3) sweep_one(op << 26);
  // SPECIAL functions (opcode 0), REGIMM rt values (opcode 1).
  for (uint32_t f = 1; f < 64; ++f) sweep_one(f);
  for (uint32_t rt = 0; rt < 32; ++rt) sweep_one((1u << 26) | (rt << 16));
  // COP0: every rs with rd = 0 (Index), except MT/DMT/CT which would write it; CO functions.
  for (uint32_t rs = 0; rs < 16; ++rs)
    if (rs != 4 && rs != 5 && rs != 6) sweep_one((0x10u << 26) | (rs << 21));
  for (uint32_t f = 0; f < 64; ++f) sweep_one((0x10u << 26) | (0x10u << 21) | f);
  // COP1: every format and function with fs = ft = fd = 0.
  for (uint32_t fmt = 0; fmt < 32; ++fmt)
    for (uint32_t f = 0; f < 64; ++f) sweep_one((0x11u << 26) | (fmt << 21) | f);
  // COP2 and COP3 move/branch formats.
  for (uint32_t rs = 0; rs < 32; ++rs) {
    sweep_one((0x12u << 26) | (rs << 21));
    sweep_one((0x13u << 26) | (rs << 21));
  }
  tlb_quiet();
  __asm__ volatile("ctc1 $0, $31");
  rec_series_full(S_OPCODE_SWEEP, 4, 4 * nsweep, sweep_out);
}

// ---------------------------------------------------------------------------------------------
// FPU (S_FPU_*): operands from an edge-case set, every rounding mode, flush-to-zero off and on.
// Single values sit in the low word of the 64-bit register (FR = 1). MIPS legacy NaN encoding:
// a quiet NaN has the fraction MSB clear (VR4300 manual 7.2, 0x7FBFFFFF is its default NaN).
static const uint32_t es[16] = {0x00000000, 0x80000000, 0x3F800000, 0xBF800000,
                                0x00000001, 0x807FFFFF, 0x00800000, 0x7F7FFFFF,
                                0xFF7FFFFF, 0x7F800000, 0xFF800000, 0x7FBFFFFF,
                                0x7FC00000, 0x3FC00000, 0x40400000, 0x3EAAAAAB};
static const uint64_t ed[16] = {
    0x0000000000000000ull, 0x8000000000000000ull, 0x3FF0000000000000ull, 0xBFF0000000000000ull,
    0x0000000000000001ull, 0x800FFFFFFFFFFFFFull, 0x0010000000000000ull, 0x7FEFFFFFFFFFFFFFull,
    0xFFEFFFFFFFFFFFFFull, 0x7FF0000000000000ull, 0xFFF0000000000000ull, 0x7FF7FFFFFFFFFFFFull,
    0x7FF8000000000000ull, 0x3FF8000000000000ull, 0x4008000000000000ull, 0x3FD5555555555555ull};
static const uint64_t ei[16] = {0, 1, 0xFFFFFFFFFFFFFFFFull, 0x7FFFFFFF, 0xFFFFFFFF80000000ull,
                                0x01000001, 0x0020000000000001ull, 0x7FFFFFFFFFFFFFFFull,
                                0x8000000000000000ull, 0x80000000ull, 0x00000000FFFFFFFFull,
                                0x0080000000000000ull, 0xFF7FFFFFFFFFFFFFull, 3, 0x40000000,
                                0x0000000100000000ull};

#define FMT_S 16
#define FMT_D 17
#define FMT_W 20
#define FMT_L 21

static uint32_t cop1(uint32_t fmt, uint32_t funct) {
  return (0x11u << 26) | (fmt << 21) | (4u << 16) | (2u << 11) | (6u << 6) | funct;
}

// One FPU run; o = {result, FCSR after, Cause or ~0 for none}.
static void fp_run(uint32_t insn, uint64_t a, uint64_t b, uint32_t fcsr, uint64_t* o) {
  place(insn);
  hwp_exc_reset(HWP_EXPECT_RESUME);
  uint64_t io[6] = {a, b, fcsr, 0, 0, 0};
  hwp_exec_fp(code, io);
  const uint64_t* r = io + 3;
  o[0] = r[0];
  o[1] = hwp_exc.n ? ((uint64_t)hwp_exc.fcsr << 32) | r[1] : r[1];
  o[2] = hwp_exc.n ? hwp_exc.first_cause : 0xFFFFFFFFu;
}

static const uint32_t fcsr_modes[8] = {0, 1, 2, 3, 0x01000000, 0x01000001, 0x01000002, 0x01000003};

static void fpu_binary(void) {
  // add, sub, mul, div (functions 0..3) for S and D: 16 x 16 operands x 8 modes.
  for (int fmt = 0; fmt < 2; ++fmt) {
    uint32_t n = 0;
    for (uint32_t f = 0; f < 4; ++f)
      for (int m = 0; m < 8; ++m)
        for (int i = 0; i < 16; ++i)
          for (int j = 0; j < 16; ++j, n += 3)
            fp_run(cop1(fmt ? FMT_D : FMT_S, f), fmt ? ed[i] : es[i], fmt ? ed[j] : es[j],
                   fcsr_modes[m], &big[n]);
    rec_series_full(fmt ? S_FPU_BINARY_D : S_FPU_BINARY_S, 8, n, big);
  }
}

static void fpu_unary(void) {
  // Functions with fs only: sqrt 4, abs 5, mov 6, neg 7, round.l 8, trunc.l 9, ceil.l 10,
  // floor.l 11, round.w 12, trunc.w 13, ceil.w 14, floor.w 15, cvt.s 32, cvt.d 33, cvt.w 36,
  // cvt.l 37; from S and D (FP operands) and from W and L (integer operands, cvt.s/cvt.d).
  static const uint8_t fn_fp[16] = {4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 32, 33, 36, 37};
  static const uint8_t fn_int[2] = {32, 33};
  static const uint8_t fmts[4] = {FMT_S, FMT_D, FMT_W, FMT_L};
  uint32_t n = 0;
  for (int k = 0; k < 4; ++k) {
    const bool integer = k >= 2;
    const int nf = integer ? 2 : 16;
    for (int fi = 0; fi < nf; ++fi)
      for (int m = 0; m < 8; ++m)
        for (int i = 0; i < 16; ++i, n += 3) {
          const uint64_t a = k == 0 ? es[i] : k == 1 ? ed[i] : ei[i];
          fp_run(cop1(fmts[k], integer ? fn_int[fi] : fn_fp[fi]), a, 0, fcsr_modes[m], &big[n]);
        }
  }
  rec_series_full(S_FPU_UNARY, 8, n, big);
}

static void fpu_compare(void) {
  // c.cond (functions 48..63) for S and D: the result is FCSR bit 23 (C).
  uint32_t n = 0;
  for (int fmt = 0; fmt < 2; ++fmt)
    for (uint32_t c = 0; c < 16; ++c)
      for (int i = 0; i < 16; ++i)
        for (int j = 0; j < 16; ++j, n += 3) {
          const uint32_t insn = (0x11u << 26) | ((fmt ? FMT_D : FMT_S) << 21) | (4u << 16) |
                                (2u << 11) | (48 + c);
          fp_run(insn, fmt ? ed[i] : es[i], fmt ? ed[j] : es[j], 0, &big[n]);
        }
  rec_series_full(S_FPU_COMPARE, 8, n, big);
}

// ---------------------------------------------------------------------------------------------
// Integer (S_INT_*): SPECIAL functions with rs = $8, rt = $9, rd = $10 over 16 x 16 operands.
static void int_ops(void) {
  // ALU, shifts by register, HI/LO producers. Functions: sllv 4, srlv 6, srav 7, dsllv 20,
  // dsrlv 22, dsrav 23, mult 24, multu 25, div 26, divu 27, dmult 28, dmultu 29, ddiv 30,
  // ddivu 31, add 32, addu 33, sub 34, subu 35, and 36, or 37, xor 38, nor 39, slt 42,
  // sltu 43, dadd 44, daddu 45, dsub 46, dsubu 47.
  static const uint8_t fn[] = {4,  6,  7,  20, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
                               32, 33, 34, 35, 36, 37, 38, 39, 42, 43, 44, 45, 46, 47};
  uint32_t n = 0;
  for (uint32_t k = 0; k < sizeof fn; ++k)
    for (int i = 0; i < 16; ++i)
      for (int j = 0; j < 16; ++j, n += 3) {
        place((8u << 21) | (9u << 16) | (10u << 11) | fn[k]);
        hwp_exc_reset(HWP_EXPECT_RESUME);
        hwp_exec_int(code, ei[i], ei[j], &big[n]);
        if (hwp_exc.n) big[n] = 0xEEEEEEEE00000000ull | hwp_exc.first_cause;  // trapped
      }
  rec_series_full(S_INT_OPS, 8, n, big);

  // Shifts by immediate: sll 0, srl 2, sra 3, dsll 56, dsrl 58, dsra 59, dsll32 60, dsrl32 62,
  // dsra32 63, with sa 0..31, on each operand.
  static const uint8_t sh[9] = {0, 2, 3, 56, 58, 59, 60, 62, 63};
  n = 0;
  for (int k = 0; k < 9; ++k)
    for (uint32_t sa = 0; sa < 32; ++sa)
      for (int i = 0; i < 16; ++i, n += 3) {
        place((9u << 16) | (10u << 11) | (sa << 6) | sh[k]);
        hwp_exc_reset(HWP_EXPECT_RESUME);
        hwp_exec_int(code, 0, ei[i], &big[n]);
      }
  rec_series_full(S_INT_SHIFTS, 8, n, big);
}

// ---------------------------------------------------------------------------------------------
static uint8_t ua_buf[32] __attribute__((aligned(16)));
static uint32_t ll_word[2] __attribute__((aligned(8)));

static void memory_ops(void) {
  // LWL/LWR/LDL/LDR/SWL/SWR/SDL/SDR at offsets 0..7: register result and the 16 bytes after.
  uint32_t n = 0;
  for (int op = 0; op < 8; ++op)
    for (uint32_t off = 0; off < 8; ++off) {
      for (int i = 0; i < 32; ++i) ua_buf[i] = 0xA0 + i;
      uint64_t r;
      hwp_unaligned(op, off, ua_buf, &r);
      big[n++] = r;
      memcpy(&big[n], ua_buf, 16);
      n += 2;
    }
  rec_series_full(S_UNALIGNED, 8, n, big);

  // LL/SC.
  n = 0;
  for (int k = 0; k < 5; ++k) {
    ll_word[0] = 0xCAFE;
    ll_word[1] = 0;
    hwp_llsc(k, ll_word, &big[n]);
    n += 3;
  }
  rec_series_full(S_LLSC, 8, n, big);

  // Cache tags (Index_Load_Tag): a D line loaded, dirtied, and invalidated; an I line run.
  static uint32_t line[4] __attribute__((aligned(16)));
  uint32_t t[6];
  dcache_wbinv(line, 16);
  (void)*(volatile uint32_t*)line;
  t[0] = hwp_cache_tag(0x05, (uint32_t)line);
  *(volatile uint32_t*)line = 1;
  t[1] = hwp_cache_tag(0x05, (uint32_t)line);
  dcache_wbinv(line, 16);
  t[2] = hwp_cache_tag(0x05, (uint32_t)line);
  t[3] = hwp_cache_tag(0x04, (uint32_t)(uintptr_t)hwp_cache_tag);
  icache_inv((void*)(uintptr_t)hwp_cache_tag, 32);
  t[4] = hwp_cache_tag(0x04, (uint32_t)(uintptr_t)hwp_cache_tag + 0x2000);
  t[5] = PHYS(line);
  rec_series_full(S_CACHE_TAGS, 4, 6, t);
}

void grp_cpu_func(void* unused) {
  (void)unused;
  opcode_sweep();
  fpu_binary();
  fpu_unary();
  fpu_compare();
  int_ops();
  memory_ops();
  rec_series_full(S_FPU_OPERANDS_S, 4, 16, es);
  rec_series_full(S_FPU_OPERANDS_D, 8, 16, ed);
  rec_series_full(S_INT_OPERANDS, 8, 16, ei);
}
