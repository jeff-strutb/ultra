// RSP functional sweep: every vector compute funct with edge-case inputs and every element
// selector, the divide unit's tables, the vector and scalar load/store rules, COP2 moves and
// COP0 as the RSP reads it. The CPU patches the instruction under test into a slot of
// rsp_func_ucode.S, so this file holds every encoding. Results go to the full record only.
// docs/timing/hwprobe.md, "RSP functional sweep", describes each series.
#include <string.h>

#include "hwp.h"

#define TIMEOUT_TICKS(ms) ((uint32_t)(ms) * 46875u)  // Count runs at 46.875 MHz

// ---- SP interface (n64brew "Reality Signal Processor/Interface") ----
#define RF_DMEM ((volatile uint32_t*)0xA4000000u)
#define RF_IMEM ((volatile uint32_t*)0xA4001000u)
#define RF_SP_STATUS (*(volatile uint32_t*)0xA4040010u)
#define RF_SP_SEMAPHORE (*(volatile uint32_t*)0xA404001Cu)
#define RF_SP_PC (*(volatile uint32_t*)0xA4080000u)
#define RF_SPS_HALT 0x1u           // SP_STATUS read
#define RF_SPS_BROKE 0x2u
#define RF_SPW_CLR_HALT 0x1u       // SP_STATUS write
#define RF_SPW_SET_HALT 0x2u
#define RF_SPW_CLR_BROKE 0x4u
#define RF_SPW_CLR_INTR 0x8u
#define RF_SPW_CLR_INTBREAK 0x80u
#define RF_SPW_CLR_SIGS 0x00AAAA00u  // clear signals 0-7: bits 9, 11, ... 23

// ---- rsp_func_ucode.S DMEM layout ----
#define RF_TABLE 0x040u   // entry addresses, then slot addresses
#define RF_PARAMS 0x060u  // count, address, value, VCO, VCC, VCE, magic
#define RF_MAGIC 0x52535046u
#define RF_VINIT 0x0A0u
#define RF_SOUT 0x120u
#define RF_REGION 0x220u
#define RF_BUF 0x260u
#define RF_WRAP_HI 0xFE0u  // the 32 bytes before the end of DMEM...
#define RF_WRAP_LO 0x000u  // ...and the 32 after it wraps
enum { E_VOP = 0, E_DIV, E_SINGLE, E_COP0 };

// ---- Encodings (n64brew "Reality Signal Processor/CPU Core" and ".../Vector Unit") ----
#define OP_COP2 0x12u   // COP2: compute when bit 25 is set, otherwise MFC2/CFC2/MTC2/CTC2
#define OP_LWC2 0x32u
#define OP_SWC2 0x3Au
#define COP2_MF 0x00u
#define COP2_CF 0x02u
#define COP2_MT 0x04u
#define COP2_CT 0x06u
#define F_VRCP 0x30u
#define F_VRCPL 0x31u
#define F_VRCPH 0x32u
#define F_VRSQ 0x34u
#define F_VRSQL 0x35u
#define F_VRSQH 0x36u
#define F_VNOP 0x37u
#define E_LANE0 8u      // element field for lane 0 of a single-lane op (rsp.inc writes .e0 as 8)
#define R_T5 13u        // scalar registers the slot uses (rsp_func_ucode.S)
#define R_S4 20u
#define VR_LS 8u        // $v08: the register of the load/store tests

static uint32_t enc_vop(uint32_t funct, uint32_t e, uint32_t vd, uint32_t vs, uint32_t vt) {
  return (OP_COP2 << 26) | (1u << 25) | ((e & 15) << 21) | ((vt & 31) << 16) | ((vs & 31) << 11) |
         ((vd & 31) << 6) | (funct & 63);
}
static uint32_t enc_move(uint32_t kind, uint32_t rt, uint32_t rd, uint32_t e) {
  return (OP_COP2 << 26) | (kind << 21) | (rt << 16) | (rd << 11) | ((e & 15) << 7);
}
static uint32_t enc_lsv(bool store, uint32_t op, uint32_t e) {
  return ((store ? OP_SWC2 : OP_LWC2) << 26) | (R_S4 << 21) | (VR_LS << 16) | (op << 11) |
         ((e & 15) << 7);
}
static uint32_t enc_scalar(uint32_t op) { return (op << 26) | (R_S4 << 21) | (R_T5 << 16); }
#define VNOP enc_vop(F_VNOP, 0, 0, 0, 0)

// ---- Microcode control ----
extern uint8_t rsp_func_ucode_text_start[], rsp_func_ucode_text_end[];
extern uint8_t rsp_func_ucode_data_start[], rsp_func_ucode_data_end[];

static uint32_t rf_runs, rf_good, rf_timeouts, rf_streak;
static bool rf_dead;  // 4 runs in a row did not reach the BREAK: skip the rest

// A sweep series. If the RSP never reached a BREAK and no run will be tried again (an emulator
// without an RSP), the series is recorded with no elements: megabytes of HWP_TIMEOUT carry
// nothing and would overflow an emulator's capture of the ISViewer stream.
static void rf_emit(uint16_t id, uint8_t esize, uint32_t count, const void* data) {
  rec_series_full(id, esize, rf_dead && rf_good == 0 ? 0 : count, data);
}

static void dmem_write(uint32_t off, const void* src, uint32_t len) {
  const uint32_t* s = src;
  for (uint32_t i = 0; i < len / 4; ++i) RF_DMEM[off / 4 + i] = s[i];
}
static void dmem_read(uint32_t off, void* dst, uint32_t len) {
  uint32_t* d = dst;
  for (uint32_t i = 0; i < len / 4; ++i) d[i] = RF_DMEM[off / 4 + i];
}

static void rf_halt(void) {
  RF_SP_STATUS = RF_SPW_SET_HALT;
  const uint32_t t0 = c0_count();
  while (!(RF_SP_STATUS & RF_SPS_HALT))
    if (c0_count() - t0 > TIMEOUT_TICKS(1)) break;
}

static bool rf_load(void) {
  rf_halt();
  const uint32_t tlen = rsp_func_ucode_text_end - rsp_func_ucode_text_start;
  const uint32_t dlen = rsp_func_ucode_data_end - rsp_func_ucode_data_start;
  if (tlen > 4096 || dlen > 4096) return false;
  uint32_t w;
  for (uint32_t i = 0; i < tlen; i += 4) {
    memcpy(&w, rsp_func_ucode_text_start + i, 4);
    RF_IMEM[i / 4] = w;
  }
  for (uint32_t i = 0; i < dlen; i += 4) {
    memcpy(&w, rsp_func_ucode_data_start + i, 4);
    RF_DMEM[i / 4] = w;
  }
  memcpy(&w, rsp_func_ucode_text_start, 4);
  return RF_DMEM[(RF_PARAMS + 24) / 4] == RF_MAGIC && RF_IMEM[0] == w;
}

static void rf_slot(int entry, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3) {
  const uint32_t a = (RF_DMEM[RF_TABLE / 4 + 4 + entry] & 0xFFCu) / 4;
  RF_IMEM[a + 0] = w0;
  RF_IMEM[a + 1] = w1;
  RF_IMEM[a + 2] = w2;
  RF_IMEM[a + 3] = w3;
}

// Runs one entry to its BREAK. False if it did not get there in 5 ms (or halted without it).
static bool rf_run(int entry) {
  if (rf_dead) return false;
  ++rf_runs;
  rf_halt();
  RF_SP_PC = RF_DMEM[RF_TABLE / 4 + entry] & 0xFFCu;
  RF_SP_STATUS = RF_SPW_CLR_BROKE | RF_SPW_CLR_INTR | RF_SPW_CLR_INTBREAK;
  const uint32_t t0 = c0_count();
  RF_SP_STATUS = RF_SPW_CLR_HALT;
  for (;;) {
    const uint32_t st = RF_SP_STATUS;
    if (st & RF_SPS_HALT) {
      if (st & RF_SPS_BROKE) {
        rf_streak = 0;
        ++rf_good;
        return true;
      }
      break;
    }
    if (c0_count() - t0 > TIMEOUT_TICKS(5)) break;
  }
  ++rf_timeouts;
  rf_halt();
  if (++rf_streak >= 4) rf_dead = true;
  return false;
}

static void rf_params(uint32_t count, uint32_t addr, uint32_t value, uint32_t vco, uint32_t vcc,
                      uint32_t vce) {
  const uint32_t p[6] = {count, addr, value, vco, vcc, vce};
  dmem_write(RF_PARAMS, p, sizeof p);
}

static uint32_t rf_out[16384];  // one series at a time

// ---------------------------------------------------------------------------------------------
// Vector compute. Case layout (halfwords): vs[8], vt[8], vd before[8], H'[8], M[8], L[8], VCO,
// VCC, VCE, 0 x5. Cases 0-127 pair every edge value with every other (pair p = 8 (k % 32) + lane:
// vs = EDGE[p / 16], vt = EDGE[p % 16]) under four accumulator preloads; cases 128-255 are
// pseudo-random, half the lanes drawn from EDGE.
#define VOP_CASES 256
#define VOP_ELEM_FIRST 128  // cases the element sweep (e = 1..15) uses
#define VOP_ELEM_CASES 16
#define VOP_BATCH 16
#define VOP_CASE_HW 56
#define VOP_OUT_HW 36   // recorded halfwords per case: vd, ACC hi, mid, lo, VCO, VCC, VCE, 0
static const uint16_t EDGE[16] = {0x0000, 0x0001, 0xFFFF, 0x7FFF, 0x8000, 0x8001, 0x4000, 0xC000,
                                  0x00FF, 0xFF00, 0x0100, 0x5555, 0xAAAA, 0x7FFE, 0x0002, 0xFFFE};
static const uint16_t ACC_SEED[4][3] = {  // H, M, L
    {0x0000, 0x0000, 0x0000}, {0x0000, 0x1234, 0x5678}, {0xFFFF, 0x8000, 0x0001},
    {0x7FFF, 0xFFFF, 0xFFFF}};
static uint16_t vcases[VOP_CASES][VOP_CASE_HW] __attribute__((aligned(16)));

static uint32_t xs_state;
static uint32_t xs(void) {
  xs_state ^= xs_state << 13;
  xs_state ^= xs_state >> 17;
  xs_state ^= xs_state << 5;
  return xs_state;
}

static void vop_cases(void) {
  xs_state = 0x13579BDFu;
  memset(vcases, 0, sizeof vcases);
  for (int k = 0; k < VOP_CASES; ++k) {
    uint16_t* c = vcases[k];
    for (int l = 0; l < 8; ++l) {
      uint16_t h, m, lo;
      if (k < 128) {
        const int p = 8 * (k % 32) + l;
        c[l] = EDGE[p >> 4];
        c[8 + l] = EDGE[p & 15];
        const int s = (k / 32 + l) % 4;
        h = ACC_SEED[s][0];
        m = ACC_SEED[s][1];
        lo = ACC_SEED[s][2];
      } else {
        c[l] = (xs() & 1) ? EDGE[xs() & 15] : (uint16_t)xs();
        c[8 + l] = (xs() & 1) ? EDGE[xs() & 15] : (uint16_t)xs();
        h = (xs() & 1) ? (uint16_t)xs() : ((xs() & 1) ? 0xFFFF : 0);
        m = (uint16_t)xs();
        lo = (uint16_t)xs();
      }
      c[16 + l] = (uint16_t)(0xD000 | (l << 8) | (k & 0xFF));
      c[24 + l] = (uint16_t)(h + (m >> 15));  // VMADH of M adds sext(M) << 16
      c[32 + l] = m;
      c[40 + l] = lo;
    }
    c[48] = (uint16_t)xs();
    c[49] = (uint16_t)xs();
    c[50] = (uint16_t)(xs() & 0xFF);
  }
}

// Runs cases [first, first + VOP_BATCH) under one instruction word; appends to o.
static uint16_t* vop_batch(int first, uint32_t insn, uint16_t* o) {
  bool ok = false;
  if (!rf_dead) {
    dmem_write(RF_BUF, vcases[first], VOP_BATCH * VOP_CASE_HW * 2);
    rf_slot(E_VOP, insn, VNOP, VNOP, VNOP);
    rf_params(VOP_BATCH, 0, 0, 0, 0, 0);
    ok = rf_run(E_VOP);
  }
  const uint32_t out0 = RF_BUF + VOP_BATCH * VOP_CASE_HW * 2;
  for (int i = 0; i < VOP_BATCH; ++i, o += VOP_OUT_HW) {
    if (ok) {
      uint32_t w[VOP_OUT_HW / 2];
      dmem_read(out0 + 80 * i, w, sizeof w);
      memcpy(o, w, sizeof w);
    } else {
      memset(o, 0xFF, VOP_OUT_HW * 2);
    }
  }
  return o;
}

static void sweep_vop(void) {
  uint16_t* const base = (uint16_t*)rf_out;
  _Static_assert((VOP_CASES + 15 * VOP_ELEM_CASES) * VOP_OUT_HW * 2 <= sizeof rf_out, "rf_out");
  for (uint32_t f = 0; f < 64; ++f) {
    uint16_t* o = base;
    for (int b = 0; b < VOP_CASES; b += VOP_BATCH) o = vop_batch(b, enc_vop(f, 0, 3, 1, 2), o);
    for (uint32_t e = 1; e < 16; ++e) o = vop_batch(VOP_ELEM_FIRST, enc_vop(f, e, 3, 1, 2), o);
    rf_emit(S_RSPF_VOP_ID(f), 2, (uint32_t)(o - base), base);
  }
}

// ---------------------------------------------------------------------------------------------
// Divide unit. Items are (hi, lo) halfword pairs; results are ($v04.e0 << 16) | $v03.e0.
#define DIV_BATCH 512
#define DIV_CHUNK 4096

static void div_run(const uint32_t* in, uint32_t* res, uint32_t n) {
  bool ok = false;
  if (!rf_dead) {
    dmem_write(RF_BUF, in, n * 4);
    rf_params(n, 0, 0, 0, 0, 0);
    ok = rf_run(E_DIV);
  }
  if (ok)
    dmem_read(RF_BUF, res, n * 4);
  else
    memset(res, 0xFF, n * 4);
}

// Single precision: every 16-bit input x as $v02.e0, VRCP (VRSQ) then VRCPH (VRSQH) of $v00.
static void sweep_div_single(bool rsq, uint16_t id0) {
  rf_slot(E_DIV, enc_vop(rsq ? F_VRSQ : F_VRCP, E_LANE0, 3, E_LANE0, 2),
          enc_vop(rsq ? F_VRSQH : F_VRCPH, E_LANE0, 4, E_LANE0, 0), VNOP, VNOP);
  static uint32_t in[DIV_BATCH];
  for (uint32_t c = 0; c < 65536 / DIV_CHUNK; ++c) {
    for (uint32_t b = 0; b < DIV_CHUNK; b += DIV_BATCH) {
      for (uint32_t i = 0; i < DIV_BATCH; ++i) in[i] = c * DIV_CHUNK + b + i;
      div_run(in, rf_out + b, DIV_BATCH);
    }
    rf_emit(id0 + c, 4, DIV_CHUNK, rf_out);
  }
}

// Double precision: VRCPH (VRSQH) of the high half, VRCPL (VRSQL) of the low half, VRCPH
// (VRSQH) of $v00 for the result's high half. Inputs: per sign (+, -) per shift s in DIV_SHIFTS
// per fill (low s bits 0, low s bits 1) per table index i: ((512 | i) << s) | fill, negated for
// the - sign. Recorded as (input, result) pairs.
static const uint8_t DIV_SHIFTS[8] = {0, 1, 6, 7, 14, 15, 20, 21};
#define DIV_DOUBLE_N 16384
static const uint32_t DIV_SPECIAL[32] = {
    0x00000000u, 0x00000001u, 0x00000002u, 0x00007FFFu, 0x00008000u, 0x0000FFFFu, 0x00010000u,
    0x7FFFFFFFu, 0x80000000u, 0x80000001u, 0xFFFFFFFFu, 0xFFFFFFFEu, 0xFFFF0000u, 0xFFFF8000u,
    0xFFFF7FFFu, 0x0000C000u, 0x40000000u, 0xC0000000u, 0x00010001u, 0x12345678u, 0xEDCBA988u,
    0x00000003u, 0xFFFFFFFDu, 0x00400000u, 0xFFC00000u, 0x00200001u, 0xFFDFFFFFu, 0x55555555u,
    0xAAAAAAAAu, 0xFFFF4000u, 0x00004000u, 0x7FFF8000u};

static uint32_t div_double_input(uint32_t n) {
  const uint32_t i = n & 511, fill = (n >> 9) & 1, s = DIV_SHIFTS[(n >> 10) & 7], neg = n >> 13;
  uint32_t x = ((512u | i) << s) | (fill ? (1u << s) - 1 : 0);
  return neg ? (uint32_t)-x : x;
}

static void sweep_div_double(bool rsq, uint16_t id0, uint16_t id_special) {
  rf_slot(E_DIV, enc_vop(rsq ? F_VRSQH : F_VRCPH, E_LANE0, 4, E_LANE0, 1),
          enc_vop(rsq ? F_VRSQL : F_VRCPL, E_LANE0, 3, E_LANE0, 2),
          enc_vop(rsq ? F_VRSQH : F_VRCPH, E_LANE0, 4, E_LANE0, 0), VNOP);
  static uint32_t in[DIV_BATCH], res[DIV_BATCH];
  for (uint32_t c = 0; c < DIV_DOUBLE_N / DIV_CHUNK; ++c) {
    for (uint32_t b = 0; b < DIV_CHUNK; b += DIV_BATCH) {
      for (uint32_t i = 0; i < DIV_BATCH; ++i) in[i] = div_double_input(c * DIV_CHUNK + b + i);
      div_run(in, res, DIV_BATCH);
      for (uint32_t i = 0; i < DIV_BATCH; ++i) {
        rf_out[2 * (b + i)] = in[i];
        rf_out[2 * (b + i) + 1] = res[i];
      }
    }
    rf_emit(id0 + c, 4, 2 * DIV_CHUNK, rf_out);
  }
  div_run(DIV_SPECIAL, res, 32);
  for (int i = 0; i < 32; ++i) {
    rf_out[2 * i] = DIV_SPECIAL[i];
    rf_out[2 * i + 1] = res[i];
  }
  rf_emit(id_special, 4, 64, rf_out);
}

// ---------------------------------------------------------------------------------------------
// One-instruction tests (e_single). $v08-$v15 start as byte j of $vR = (R << 4) | j. The
// normal window DMEM 0x220-0x25F holds 0x40 + offset; the wrap window holds 0x40 + i at
// 0xFE0 + i and 0x60 + i at 0x000 + i (i < 32).
static void single_setup(void) {
  uint8_t v[128] __attribute__((aligned(4)));
  for (int r = 0; r < 8; ++r)
    for (int j = 0; j < 16; ++j) v[16 * r + j] = (uint8_t)(((8 + r) << 4) | j);
  dmem_write(RF_VINIT, v, sizeof v);
}

static void windows_reset(void) {
  uint8_t w[64] __attribute__((aligned(4)));
  for (int i = 0; i < 64; ++i) w[i] = (uint8_t)(0x40 + i);
  dmem_write(RF_REGION, w, 64);
  dmem_write(RF_WRAP_HI, w, 32);
  dmem_write(RF_WRAP_LO, w + 32, 32);
}

// Vector loads and stores: LWC2/SWC2 opcodes 0-11 (lbv lsv llv ldv lqv lrv lpv luv lhv lfv lwv
// ltv; sbv ... stv) with $v08, base s4 at DMEM 0x230 + off (set 0) or 0xFF0 + off (set 1), per
// off 0..15 per element 0..15. Loads record $v08 (all of $v08-$v15 for opcodes 10 and 11),
// stores the 64-byte window.
static void sweep_ls(void) {
  single_setup();
  for (int store = 0; store < 2; ++store)
    for (uint32_t op = 0; op < 12; ++op)
      for (uint32_t set = 0; set < 2; ++set) {
        const uint32_t rec = store ? 64 : (op >= 10 ? 128 : 16);
        uint8_t* o = (uint8_t*)rf_out;
        for (uint32_t off = 0; off < 16; ++off)
          for (uint32_t e = 0; e < 16; ++e, o += rec) {
            bool ok = false;
            if (!rf_dead) {
              windows_reset();
              rf_slot(E_SINGLE, enc_lsv(store, op, e), 0, 0, 0);
              rf_params(0, (set ? 0xFF0u : 0x230u) + off, 0, 0, 0, 0);
              ok = rf_run(E_SINGLE);
            }
            if (!ok) {
              memset(o, 0xFF, rec);
            } else if (!store) {
              dmem_read(RF_SOUT + 0x10, o, rec);
            } else if (set == 0) {
              dmem_read(RF_REGION, o, 64);
            } else {
              dmem_read(RF_WRAP_HI, o, 32);
              dmem_read(RF_WRAP_LO, o + 32, 32);
            }
          }
        const uint16_t id = (uint16_t)(S_RSPF_LS + (store << 5) + (set << 4) + op);
        rf_emit(id, 2, (uint32_t)(o - (uint8_t*)rf_out) / 2, rf_out);
      }
}

// Scalar loads and stores at unaligned addresses, across the end of DMEM, and with address
// bits above 11 set. t5 = 0x13579BDF before the slot.
static const uint32_t SC_OPS[9] = {0x20, 0x21, 0x23, 0x24, 0x25, 0x27, 0x28, 0x29, 0x2B};
static const uint32_t SC_ADDR[18] = {0x230, 0x231, 0x232, 0x233, 0x234, 0x235, 0x236, 0x237,
                                     0xFF9, 0xFFA, 0xFFB, 0xFFC, 0xFFD, 0xFFE, 0xFFF,
                                     0x1231, 0x80000232u, 0xFFFFF233u};

static void sweep_scalar(void) {
  single_setup();
  uint32_t n = 0;
  for (int k = 0; k < 9; ++k)
    for (int a = 0; a < 18; ++a, n += 13) {
      bool ok = false;
      if (!rf_dead) {
        windows_reset();
        rf_slot(E_SINGLE, enc_scalar(SC_OPS[k]), 0, 0, 0);
        rf_params(0, SC_ADDR[a], 0x13579BDFu, 0, 0, 0);
        ok = rf_run(E_SINGLE);
      }
      if (!ok) {
        memset(&rf_out[n], 0xFF, 13 * 4);
        continue;
      }
      rf_out[n] = RF_DMEM[RF_SOUT / 4];
      dmem_read(0x230, &rf_out[n + 1], 16);
      dmem_read(0xFF0, &rf_out[n + 5], 16);
      dmem_read(0x000, &rf_out[n + 9], 16);
    }
  rf_emit(S_RSPF_SCALAR, 4, n, rf_out);
  rec_series_full(S_RSPF_SCALAR_ADDR, 4, 18, SC_ADDR);
}

// COP2 moves: MFC2 and MTC2 at every element of $v08, CTC2 of all ones and CFC2 of each
// control register number 0-3.
static uint32_t single_go(uint32_t insn, uint32_t value, uint32_t vco, uint32_t vcc, uint32_t vce) {
  if (rf_dead) return 0;
  rf_slot(E_SINGLE, insn, 0, 0, 0);
  rf_params(0, 0, value, vco, vcc, vce);
  return rf_run(E_SINGLE);
}

static void sweep_moves(void) {
  single_setup();
  uint32_t n = 0;
  for (uint32_t e = 0; e < 16; ++e, ++n)
    rf_out[n] = single_go(enc_move(COP2_MF, R_T5, VR_LS, e), 0x13579BDFu, 0, 0, 0)
                    ? RF_DMEM[RF_SOUT / 4] : HWP_TIMEOUT;
  for (uint32_t e = 0; e < 16; ++e, n += 4) {
    if (single_go(enc_move(COP2_MT, R_T5, VR_LS, e), 0x13579BDFu, 0, 0, 0))
      dmem_read(RF_SOUT + 0x10, &rf_out[n], 16);
    else
      memset(&rf_out[n], 0xFF, 16);
  }
  for (uint32_t rd = 0; rd < 4; ++rd, n += 3) {
    const bool ok = single_go(enc_move(COP2_CT, R_T5, rd, 0), 0xFFFFFFFFu, 0, 0, 0);
    const uint32_t w0 = RF_DMEM[(RF_SOUT + 4) / 4], w1 = RF_DMEM[(RF_SOUT + 8) / 4];
    rf_out[n] = ok ? w0 >> 16 : HWP_TIMEOUT;
    rf_out[n + 1] = ok ? (w0 & 0xFFFF) : HWP_TIMEOUT;
    rf_out[n + 2] = ok ? w1 >> 16 : HWP_TIMEOUT;
  }
  for (uint32_t rd = 0; rd < 4; ++rd, ++n)
    rf_out[n] = single_go(enc_move(COP2_CF, R_T5, rd, 0), 0x13579BDFu, 0x1234, 0x5678, 0x9A)
                    ? RF_DMEM[RF_SOUT / 4] : HWP_TIMEOUT;
  rf_emit(S_RSPF_MOVE, 4, n, rf_out);
}

// COP0 from the RSP, then what the CPU sees.
static void sweep_cop0(void) {
  RF_SP_SEMAPHORE = 0;
  RF_SP_STATUS = RF_SPW_CLR_SIGS;
  const bool ok = rf_run(E_COP0);
  if (ok)
    dmem_read(RF_SOUT, rf_out, 34 * 4);
  else
    memset(rf_out, 0xFF, 34 * 4);
  rf_out[34] = RF_SP_STATUS;
  rf_out[35] = RF_SP_SEMAPHORE;  // reading takes it
  rf_out[36] = RF_SP_SEMAPHORE;
  RF_SP_SEMAPHORE = 0;
  RF_SP_STATUS = RF_SPW_CLR_SIGS;
  rf_emit(S_RSPF_COP0, 4, 37, rf_out);
}

// ---------------------------------------------------------------------------------------------
void grp_rsp_func(void* unused) {
  (void)unused;
  rf_runs = rf_good = rf_timeouts = rf_streak = 0;
  rf_dead = false;
  const bool loaded = rf_load();
  if (!loaded) rf_dead = true;
  vop_cases();
  rec_series_full(S_RSPF_VCASES, 2, VOP_CASES * VOP_CASE_HW, vcases);
  sweep_cop0();
  sweep_moves();
  sweep_scalar();
  sweep_ls();
  sweep_div_single(false, S_RSPF_RCP);
  sweep_div_single(true, S_RSPF_RSQ);
  sweep_div_double(false, S_RSPF_RCPD, S_RSPF_RCPD_SPECIAL);
  sweep_div_double(true, S_RSPF_RSQD, S_RSPF_RSQD_SPECIAL);
  sweep_vop();
  rf_halt();
  const uint32_t info[9] = {loaded, rf_runs, rf_timeouts, rf_dead,
                            (uint32_t)(rsp_func_ucode_text_end - rsp_func_ucode_text_start),
                            (uint32_t)(rsp_func_ucode_data_end - rsp_func_ucode_data_start), VOP_CASES,
                            VOP_ELEM_CASES, rf_good};
  rec_series_full(S_RSPF_INFO, 4, 9, info);
}
