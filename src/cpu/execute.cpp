// VR4300 integer instruction set. Semantics: VR4300 manual chapter 16; hardware deviations
// from the manual (SRA/SRAV, MULT/DIV with non-sign-extended inputs) from n64brew "VR4300",
// Known Bugs. See docs/hardware/cpu.md.

#include <array>

#include "cpu/decode.hpp"
#include "cpu/timing.hpp"
#include "cpu/vr4300.hpp"

namespace ultra {

using namespace cop0;
using namespace decode;

namespace {

// Opcode (bits 31:26) values. VR4300 manual Figure 16-1.
enum : u32 {
  OP_SPECIAL = 0x00, OP_REGIMM = 0x01, OP_J = 0x02, OP_JAL = 0x03, OP_BEQ = 0x04, OP_BNE = 0x05,
  OP_BLEZ = 0x06, OP_BGTZ = 0x07, OP_ADDI = 0x08, OP_ADDIU = 0x09, OP_SLTI = 0x0A,
  OP_SLTIU = 0x0B, OP_ANDI = 0x0C, OP_ORI = 0x0D, OP_XORI = 0x0E, OP_LUI = 0x0F,
  OP_COP0 = 0x10, OP_COP1 = 0x11, OP_COP2 = 0x12, OP_COP3 = 0x13, OP_BEQL = 0x14,
  OP_BNEL = 0x15, OP_BLEZL = 0x16, OP_BGTZL = 0x17, OP_DADDI = 0x18, OP_DADDIU = 0x19,
  OP_LDL = 0x1A, OP_LDR = 0x1B, OP_LB = 0x20, OP_LH = 0x21, OP_LWL = 0x22, OP_LW = 0x23,
  OP_LBU = 0x24, OP_LHU = 0x25, OP_LWR = 0x26, OP_LWU = 0x27, OP_SB = 0x28, OP_SH = 0x29,
  OP_SWL = 0x2A, OP_SW = 0x2B, OP_SDL = 0x2C, OP_SDR = 0x2D, OP_SWR = 0x2E, OP_CACHE = 0x2F,
  OP_LL = 0x30, OP_LWC1 = 0x31, OP_LWC2 = 0x32, OP_LLD = 0x34, OP_LDC1 = 0x35, OP_LDC2 = 0x36,
  OP_LD = 0x37, OP_SC = 0x38, OP_SWC1 = 0x39, OP_SWC2 = 0x3A, OP_SCD = 0x3C, OP_SDC1 = 0x3D,
  OP_SDC2 = 0x3E, OP_SD = 0x3F,
};

enum : u32 {
  FN_SLL = 0x00, FN_SRL = 0x02, FN_SRA = 0x03, FN_SLLV = 0x04, FN_SRLV = 0x06, FN_SRAV = 0x07,
  FN_JR = 0x08, FN_JALR = 0x09, FN_SYSCALL = 0x0C, FN_BREAK = 0x0D, FN_SYNC = 0x0F,
  FN_MFHI = 0x10, FN_MTHI = 0x11, FN_MFLO = 0x12, FN_MTLO = 0x13, FN_DSLLV = 0x14,
  FN_DSRLV = 0x16, FN_DSRAV = 0x17, FN_MULT = 0x18, FN_MULTU = 0x19, FN_DIV = 0x1A,
  FN_DIVU = 0x1B, FN_DMULT = 0x1C, FN_DMULTU = 0x1D, FN_DDIV = 0x1E, FN_DDIVU = 0x1F,
  FN_ADD = 0x20, FN_ADDU = 0x21, FN_SUB = 0x22, FN_SUBU = 0x23, FN_AND = 0x24, FN_OR = 0x25,
  FN_XOR = 0x26, FN_NOR = 0x27, FN_SLT = 0x2A, FN_SLTU = 0x2B, FN_DADD = 0x2C,
  FN_DADDU = 0x2D, FN_DSUB = 0x2E, FN_DSUBU = 0x2F, FN_TGE = 0x30, FN_TGEU = 0x31,
  FN_TLT = 0x32, FN_TLTU = 0x33, FN_TEQ = 0x34, FN_TNE = 0x36, FN_DSLL = 0x38, FN_DSRL = 0x3A,
  FN_DSRA = 0x3B, FN_DSLL32 = 0x3C, FN_DSRL32 = 0x3E, FN_DSRA32 = 0x3F,
};

bool is_float_instr(u32 op) {
  return op == OP_COP1 || op == OP_LWC1 || op == OP_LDC1 || op == OP_SWC1 || op == OP_SDC1;
}

}  // namespace

void Vr4300::execute(u32 instr) {
  const u32 op = opcode(instr);

  // Load interlock (n64brew "VR4300", Load Delay Interlock): a GPR load stalls the next
  // non-float instruction whose rs or rt field names the loaded register (r0 exempt); an FPR
  // load stalls the next float instruction whose fs or ft field names it.
  if (pending_load_kind_ == LoadTarget::Gpr) {
    load_use_ = !is_float_instr(op) && (rs(instr) == pending_load_reg_ || rt(instr) == pending_load_reg_);
  } else if (pending_load_kind_ == LoadTarget::Fpr) {
    load_use_ = is_float_instr(op) && (fs(instr) == pending_load_reg_ || rt(instr) == pending_load_reg_);
  }
  if (load_use_) cycles_ += timing::kLoadInterlockCycles;

  u64& rt_ref = gpr_[rt(instr)];
  const u64 s = gpr_[rs(instr)];
  const u64 t = gpr_[rt(instr)];
  auto write_rt = [&](u64 v) {
    if (rt(instr)) rt_ref = v;
  };
  // The offset is added to the address of the instruction fetched after the branch (pc_), which
  // is the branch address + 4 except for a branch in another jump's delay slot, where it is
  // the outer jump's target (docs/hardware/cpu.md, Integer quirks).
  auto branch = [&](bool taken) {
    branch_pending_ = true;
    if (taken) npc_ = pc_ + (simm(instr) << 2);
  };
  auto branch_likely = [&](bool taken) {
    if (taken) {
      branch_pending_ = true;
      npc_ = pc_ + (simm(instr) << 2);
    } else {
      // Not taken: the delay slot, already fetched, is nullified.
      pc_ = npc_;
      npc_ += 4;
      kill_slot_ = true;
    }
  };
  auto ea = [&]() { return s + simm(instr); };

  switch (op) {
    case OP_SPECIAL: exec_special(instr); return;
    case OP_REGIMM: exec_regimm(instr); return;
    case OP_J:  // region from this jump's address + 4: ledger "jump-region-in-delay-slot"
      branch_pending_ = true;
      npc_ = ((cur_pc_ + 4) & ~u64{0x0FFF'FFFF}) | (u64{target(instr)} << 2);
      return;
    case OP_JAL:
      branch_pending_ = true;
      gpr_[31] = pc_ + 4;  // next fetch address + 4 (a JAL in a delay slot links past the outer target)
      npc_ = ((cur_pc_ + 4) & ~u64{0x0FFF'FFFF}) | (u64{target(instr)} << 2);
      return;
    case OP_BEQ: branch(s == t); return;
    case OP_BNE: branch(s != t); return;
    case OP_BLEZ: branch(as_s64(s) <= 0); return;
    case OP_BGTZ: branch(as_s64(s) > 0); return;
    case OP_BEQL: branch_likely(s == t); return;
    case OP_BNEL: branch_likely(s != t); return;
    case OP_BLEZL: branch_likely(as_s64(s) <= 0); return;
    case OP_BGTZL: branch_likely(as_s64(s) > 0); return;

    case OP_ADDI: {
      const u32 a = lo32(s), b = lo32(simm(instr));
      const u32 r = a + b;
      if (((a ^ r) & (b ^ r)) >> 31) return raise(ExcCode::Ov);
      write_rt(sext32(r));
      return;
    }
    case OP_ADDIU: write_rt(sext32(lo32(s) + lo32(simm(instr)))); return;
    case OP_SLTI: write_rt(as_s64(s) < as_s64(simm(instr)) ? 1 : 0); return;
    case OP_SLTIU: write_rt(s < simm(instr) ? 1 : 0); return;
    case OP_ANDI: write_rt(s & imm(instr)); return;
    case OP_ORI: write_rt(s | imm(instr)); return;
    case OP_XORI: write_rt(s ^ imm(instr)); return;
    case OP_LUI: write_rt(sext32(u32{imm(instr)} << 16)); return;

    case OP_DADDI: {
      if (!ops64()) return raise(ExcCode::RI);
      const u64 b = simm(instr);
      const u64 r = s + b;
      if (((s ^ r) & (b ^ r)) >> 63) return raise(ExcCode::Ov);
      write_rt(r);
      return;
    }
    case OP_DADDIU:
      if (!ops64()) return raise(ExcCode::RI);
      write_rt(s + simm(instr));
      return;

    case OP_COP0: exec_cop0(instr); return;
    case OP_COP1: exec_cop1(instr); return;
    case OP_COP2: exec_cop2(instr); return;
    case OP_COP3: raise(ExcCode::RI); return;  // COP3 does not exist: RI, CE = 0

    // ---- loads ----
    case OP_LB: {
      u8 v;
      if (!load(ea(), v)) return;
      write_rt(sext8(v));
      cur_load_kind_ = LoadTarget::Gpr;
      cur_load_reg_ = rt(instr);
      return;
    }
    case OP_LBU: {
      u8 v;
      if (!load(ea(), v)) return;
      write_rt(v);
      cur_load_kind_ = LoadTarget::Gpr;
      cur_load_reg_ = rt(instr);
      return;
    }
    case OP_LH: {
      u16 v;
      if (!load(ea(), v)) return;
      write_rt(sext16(v));
      cur_load_kind_ = LoadTarget::Gpr;
      cur_load_reg_ = rt(instr);
      return;
    }
    case OP_LHU: {
      u16 v;
      if (!load(ea(), v)) return;
      write_rt(v);
      cur_load_kind_ = LoadTarget::Gpr;
      cur_load_reg_ = rt(instr);
      return;
    }
    case OP_LW: {
      u32 v;
      if (!load(ea(), v)) return;
      write_rt(sext32(v));
      cur_load_kind_ = LoadTarget::Gpr;
      cur_load_reg_ = rt(instr);
      return;
    }
    case OP_LWU: {
      if (!ops64()) return raise(ExcCode::RI);
      u32 v;
      if (!load(ea(), v)) return;
      write_rt(v);
      cur_load_kind_ = LoadTarget::Gpr;
      cur_load_reg_ = rt(instr);
      return;
    }
    case OP_LD: {
      if (!ops64()) return raise(ExcCode::RI);
      u64 v;
      if (!load(ea(), v)) return;
      write_rt(v);
      cur_load_kind_ = LoadTarget::Gpr;
      cur_load_reg_ = rt(instr);
      return;
    }
    case OP_LL: {
      const u64 a = ea();
      u32 v;
      if (!load(a, v)) return;
      Translation tr;
      // The access above already succeeded, so this translation cannot fault.
      translate(a, Access::Load, tr);
      lladdr_ = tr.paddr >> 4;
      llbit_ = true;
      write_rt(sext32(v));
      cur_load_kind_ = LoadTarget::Gpr;
      cur_load_reg_ = rt(instr);
      return;
    }
    case OP_LLD: {
      if (!ops64()) return raise(ExcCode::RI);
      const u64 a = ea();
      u64 v;
      if (!load(a, v)) return;
      Translation tr;
      translate(a, Access::Load, tr);
      lladdr_ = tr.paddr >> 4;
      llbit_ = true;
      write_rt(v);
      cur_load_kind_ = LoadTarget::Gpr;
      cur_load_reg_ = rt(instr);
      return;
    }
    case OP_LWL: {
      const u64 a = ea();
      u32 w;
      if (!load(a, w, true)) return;
      const u32 k = static_cast<u32>(a & 3);
      // Big endian: bytes k..3 fill rt from the top. Reverse endian (little-endian view of
      // the word): bytes 0..k fill rt from the top.
      const u32 sh = 8 * (reverse_endian() ? 3 - k : k);
      const u32 keep = sh ? (lo32(t) & ((1u << sh) - 1)) : 0;
      write_rt(sext32((w << sh) | keep));
      cur_load_kind_ = LoadTarget::Gpr;
      cur_load_reg_ = rt(instr);
      return;
    }
    case OP_LWR: {
      const u64 a = ea();
      u32 w;
      if (!load(a, w, true)) return;
      const u32 k = static_cast<u32>(a & 3);
      const bool le = reverse_endian();
      const u32 sh = 8 * (le ? k : 3 - k);
      const u32 mask = sh ? ~(0xFFFF'FFFFu >> sh) : 0;
      const u32 merged = (lo32(t) & mask) | (w >> sh);
      // Only a load that includes the word's most significant byte sign-extends; otherwise
      // the upper 32 bits of rt are unchanged (VR4300 manual LWR "The remaining portion of the
      // register is not affected"; n64-systemtest tlb64 load matrix).
      write_rt(sh == 0 ? sext32(merged) : (t & 0xFFFF'FFFF'0000'0000ull) | merged);
      cur_load_kind_ = LoadTarget::Gpr;
      cur_load_reg_ = rt(instr);
      return;
    }
    case OP_LDL: {
      if (!ops64()) return raise(ExcCode::RI);
      const u64 a = ea();
      u64 d;
      if (!load(a, d, true)) return;
      const u32 k = static_cast<u32>(a & 7);
      const u32 sh = 8 * (reverse_endian() ? 7 - k : k);
      const u64 keep = sh ? (t & ((u64{1} << sh) - 1)) : 0;
      write_rt((d << sh) | keep);
      cur_load_kind_ = LoadTarget::Gpr;
      cur_load_reg_ = rt(instr);
      return;
    }
    case OP_LDR: {
      if (!ops64()) return raise(ExcCode::RI);
      const u64 a = ea();
      u64 d;
      if (!load(a, d, true)) return;
      const u32 k = static_cast<u32>(a & 7);
      const u32 sh = 8 * (reverse_endian() ? k : 7 - k);
      const u64 mask = sh ? ~(~u64{0} >> sh) : 0;
      write_rt((t & mask) | (d >> sh));
      cur_load_kind_ = LoadTarget::Gpr;
      cur_load_reg_ = rt(instr);
      return;
    }

    // ---- stores ----
    case OP_SB: store<u8>(ea(), t); return;
    case OP_SH: store<u16>(ea(), t); return;
    case OP_SW: store<u32>(ea(), t); return;
    case OP_SD:
      if (!ops64()) return raise(ExcCode::RI);
      store<u64>(ea(), t);
      return;
    case OP_SC:
      if (llbit_) {
        if (!store<u32>(ea(), t)) return;
      }
      write_rt(llbit_ ? 1 : 0);
      return;
    case OP_SCD:
      if (!ops64()) return raise(ExcCode::RI);
      if (llbit_) {
        if (!store<u64>(ea(), t)) return;
      }
      write_rt(llbit_ ? 1 : 0);
      return;
    case OP_SWL: {
      // Big endian: rt's top (4 - k) bytes go to lanes k..3. Reverse endian: rt's top (k + 1)
      // bytes go to the little-endian bytes 0..k, i.e. lanes 3-k..3 of the XOR-adjusted word.
      const u64 a = ea();
      const u32 k = static_cast<u32>(a & 3);
      if (reverse_endian())
        store_lanes((a & ~u64{3}) ^ 4, 4, lo32(t) >> (8 * (3 - k)), 3 - k, 3, a);
      else
        store_lanes(a & ~u64{3}, 4, lo32(t) >> (8 * k), k, 3, a);
      return;
    }
    case OP_SWR: {
      const u64 a = ea();
      const u32 k = static_cast<u32>(a & 3);
      if (reverse_endian())
        store_lanes((a & ~u64{3}) ^ 4, 4, lo32(t) << (8 * k), 0, 3 - k, a);
      else
        store_lanes(a & ~u64{3}, 4, lo32(t) << (8 * (3 - k)), 0, k, a);
      return;
    }
    case OP_SDL: {
      if (!ops64()) return raise(ExcCode::RI);
      const u64 a = ea();
      const u32 k = static_cast<u32>(a & 7);
      if (reverse_endian())
        store_lanes(a & ~u64{7}, 8, t >> (8 * (7 - k)), 7 - k, 7, a);
      else
        store_lanes(a & ~u64{7}, 8, t >> (8 * k), k, 7, a);
      return;
    }
    case OP_SDR: {
      if (!ops64()) return raise(ExcCode::RI);
      const u64 a = ea();
      const u32 k = static_cast<u32>(a & 7);
      if (reverse_endian())
        store_lanes(a & ~u64{7}, 8, t << (8 * k), 0, 7 - k, a);
      else
        store_lanes(a & ~u64{7}, 8, t << (8 * (7 - k)), 0, k, a);
      return;
    }

    case OP_CACHE: exec_cache(instr); return;

    case OP_LWC1:
    case OP_LDC1:
    case OP_SWC1:
    case OP_SDC1: fpu_execute(instr); return;

    case OP_LWC2:
    case OP_LDC2: {
      // With CU2 set these move a doubleword into the COP2 latch (n64-systemtest
      // COP2LWC2Behavior, a "poorly understood" quirk; Guess beyond that test, ledger
      // "cop2-latch").
      if (!cop_usable(2)) return raise(ExcCode::CpU, 2);
      u64 v;
      if (!load(ea() & ~u64{7}, v)) return;
      cop2_latch_ = v;
      return;
    }
    case OP_SWC2:
      if (!cop_usable(2)) return raise(ExcCode::CpU, 2);
      store<u32>(ea(), cop2_latch_);
      return;
    case OP_SDC2:
      if (!cop_usable(2)) return raise(ExcCode::CpU, 2);
      store<u64>(ea(), cop2_latch_);
      return;

    default: raise(ExcCode::RI); return;
  }
}

void Vr4300::exec_special(u32 instr) {
  const u32 fn = funct(instr);
  const u64 s = gpr_[rs(instr)];
  const u64 t = gpr_[rt(instr)];
  const u32 d = rd(instr);
  auto write_rd = [&](u64 v) {
    if (d) gpr_[d] = v;
  };
  auto trap = [&](bool cond) {
    if (cond) raise(ExcCode::Tr);
  };

  switch (fn) {
    case FN_SLL: write_rd(sext32(lo32(t) << sa(instr))); return;
    case FN_SRL: write_rd(sext32(lo32(t) >> sa(instr))); return;
    // SRA/SRAV shift the full 64-bit register, then sign-extend bit 31 (n64brew "VR4300",
    // 32-bit Shift Right Arithmetic Bug).
    case FN_SRA: write_rd(sext32(lo32(as_u64(as_s64(t) >> sa(instr))))); return;
    case FN_SLLV: write_rd(sext32(lo32(t) << (s & 31))); return;
    case FN_SRLV: write_rd(sext32(lo32(t) >> (s & 31))); return;
    case FN_SRAV: write_rd(sext32(lo32(as_u64(as_s64(t) >> (s & 31))))); return;
    case FN_JR:
      branch_pending_ = true;
      npc_ = s;
      return;
    case FN_JALR:
      branch_pending_ = true;
      npc_ = s;
      write_rd(pc_ + 4);
      return;
    case FN_SYSCALL: raise(ExcCode::Sys); return;
    case FN_BREAK: raise(ExcCode::Bp); return;
    case FN_SYNC: return;
    case FN_MFHI: write_rd(hi_); return;
    case FN_MTHI: hi_ = s; return;
    case FN_MFLO: write_rd(lo_); return;
    case FN_MTLO: lo_ = s; return;
    case FN_DSLLV:
      if (!ops64()) return raise(ExcCode::RI);
      write_rd(t << (s & 63));
      return;
    case FN_DSRLV:
      if (!ops64()) return raise(ExcCode::RI);
      write_rd(t >> (s & 63));
      return;
    case FN_DSRAV:
      if (!ops64()) return raise(ExcCode::RI);
      write_rd(as_u64(as_s64(t) >> (s & 63)));
      return;
    case FN_MULT: {
      // 64-bit rs times rt sign-extended from bit 34 (n64brew "VR4300", Sign extension bugs).
      const u64 r = wrap_mul(s, sext_bits<35>(t));
      lo_ = sext32(lo32(r));
      hi_ = sext32(hi32(r));
      mci(timing::kMult - 1);
      return;
    }
    case FN_MULTU: {
      const u64 r = u64{lo32(s)} * u64{lo32(t)};
      lo_ = sext32(lo32(r));
      hi_ = sext32(hi32(r));
      mci(timing::kMultu - 1);
      return;
    }
    case FN_DIV: {
      // Dividend sign-extended from bit 31, divisor from bit 34 (n64brew "VR4300", Sign
      // extension bugs). Divide by zero yields LO = (n < 0 ? 1 : -1), HI = n.
      const s64 n = as_s64(sext32(lo32(s)));
      const s64 dv = as_s64(sext_bits<35>(t));
      if (dv == 0) {
        lo_ = n < 0 ? 1 : ~u64{0};
        hi_ = as_u64(n);
      } else {
        const s64 q = n / dv;
        lo_ = sext32(lo32(as_u64(q)));
        hi_ = sext32(lo32(as_u64(n) - as_u64(q) * as_u64(dv)));
      }
      mci(timing::kDiv - 1);
      return;
    }
    case FN_DIVU: {
      const u32 n = lo32(s), dv = lo32(t);
      if (dv == 0) {
        lo_ = ~u64{0};
        hi_ = sext32(n);
      } else {
        lo_ = sext32(n / dv);
        hi_ = sext32(n % dv);
      }
      mci(timing::kDivu - 1);
      return;
    }
    case FN_DMULT: {
      if (!ops64()) return raise(ExcCode::RI);
      hi_ = mul_s64_wide(s, t, lo_);
      mci(timing::kDmult - 1);
      return;
    }
    case FN_DMULTU: {
      if (!ops64()) return raise(ExcCode::RI);
      hi_ = mul_u64_wide(s, t, lo_);
      mci(timing::kDmultu - 1);
      return;
    }
    case FN_DDIV: {
      if (!ops64()) return raise(ExcCode::RI);
      const s64 n = as_s64(s), dv = as_s64(t);
      if (dv == 0) {
        lo_ = n < 0 ? 1 : ~u64{0};
        hi_ = s;
      } else if (n == INT64_MIN && dv == -1) {
        lo_ = s;
        hi_ = 0;
      } else {
        lo_ = as_u64(n / dv);
        hi_ = as_u64(n % dv);
      }
      mci(timing::kDdiv - 1);
      return;
    }
    case FN_DDIVU: {
      if (!ops64()) return raise(ExcCode::RI);
      if (t == 0) {
        lo_ = ~u64{0};
        hi_ = s;
      } else {
        lo_ = s / t;
        hi_ = s % t;
      }
      mci(timing::kDdivu - 1);
      return;
    }
    case FN_ADD: {
      const u32 a = lo32(s), b = lo32(t), r = a + b;
      if (((a ^ r) & (b ^ r)) >> 31) return raise(ExcCode::Ov);
      write_rd(sext32(r));
      return;
    }
    case FN_ADDU: write_rd(sext32(lo32(s) + lo32(t))); return;
    case FN_SUB: {
      const u32 a = lo32(s), b = lo32(t), r = a - b;
      if (((a ^ b) & (a ^ r)) >> 31) return raise(ExcCode::Ov);
      write_rd(sext32(r));
      return;
    }
    case FN_SUBU: write_rd(sext32(lo32(s) - lo32(t))); return;
    case FN_AND: write_rd(s & t); return;
    case FN_OR: write_rd(s | t); return;
    case FN_XOR: write_rd(s ^ t); return;
    case FN_NOR: write_rd(~(s | t)); return;
    case FN_SLT: write_rd(as_s64(s) < as_s64(t) ? 1 : 0); return;
    case FN_SLTU: write_rd(s < t ? 1 : 0); return;
    case FN_DADD: {
      if (!ops64()) return raise(ExcCode::RI);
      const u64 r = s + t;
      if (((s ^ r) & (t ^ r)) >> 63) return raise(ExcCode::Ov);
      write_rd(r);
      return;
    }
    case FN_DADDU:
      if (!ops64()) return raise(ExcCode::RI);
      write_rd(s + t);
      return;
    case FN_DSUB: {
      if (!ops64()) return raise(ExcCode::RI);
      const u64 r = s - t;
      if (((s ^ t) & (s ^ r)) >> 63) return raise(ExcCode::Ov);
      write_rd(r);
      return;
    }
    case FN_DSUBU:
      if (!ops64()) return raise(ExcCode::RI);
      write_rd(s - t);
      return;
    case FN_TGE: trap(as_s64(s) >= as_s64(t)); return;
    case FN_TGEU: trap(s >= t); return;
    case FN_TLT: trap(as_s64(s) < as_s64(t)); return;
    case FN_TLTU: trap(s < t); return;
    case FN_TEQ: trap(s == t); return;
    case FN_TNE: trap(s != t); return;
    case FN_DSLL:
      if (!ops64()) return raise(ExcCode::RI);
      write_rd(t << sa(instr));
      return;
    case FN_DSRL:
      if (!ops64()) return raise(ExcCode::RI);
      write_rd(t >> sa(instr));
      return;
    case FN_DSRA:
      if (!ops64()) return raise(ExcCode::RI);
      write_rd(as_u64(as_s64(t) >> sa(instr)));
      return;
    case FN_DSLL32:
      if (!ops64()) return raise(ExcCode::RI);
      write_rd(t << (sa(instr) + 32));
      return;
    case FN_DSRL32:
      if (!ops64()) return raise(ExcCode::RI);
      write_rd(t >> (sa(instr) + 32));
      return;
    case FN_DSRA32:
      if (!ops64()) return raise(ExcCode::RI);
      write_rd(as_u64(as_s64(t) >> (sa(instr) + 32)));
      return;
    default: raise(ExcCode::RI); return;
  }
}

void Vr4300::exec_regimm(u32 instr) {
  const s64 s = as_s64(gpr_[rs(instr)]);
  const u64 imm_s = simm(instr);
  auto branch = [&](bool taken, bool link, bool likely) {
    if (link) gpr_[31] = pc_ + 4;
    if (taken) {
      branch_pending_ = true;
      npc_ = pc_ + (imm_s << 2);  // relative to the next fetch address, as in execute()
    } else if (likely) {
      pc_ = npc_;
      npc_ += 4;
      kill_slot_ = true;
    } else {
      branch_pending_ = true;
    }
  };
  auto trap = [&](bool cond) {
    if (cond) raise(ExcCode::Tr);
  };
  switch (rt(instr)) {
    case 0x00: branch(s < 0, false, false); return;   // BLTZ
    case 0x01: branch(s >= 0, false, false); return;  // BGEZ
    case 0x02: branch(s < 0, false, true); return;    // BLTZL
    case 0x03: branch(s >= 0, false, true); return;   // BGEZL
    case 0x08: trap(s >= as_s64(imm_s)); return;                  // TGEI
    case 0x09: trap(as_u64(s) >= imm_s); return;                  // TGEIU
    case 0x0A: trap(s < as_s64(imm_s)); return;                   // TLTI
    case 0x0B: trap(as_u64(s) < imm_s); return;                   // TLTIU
    case 0x0C: trap(as_u64(s) == imm_s); return;                  // TEQI
    case 0x0E: trap(as_u64(s) != imm_s); return;                  // TNEI
    case 0x10: branch(s < 0, true, false); return;    // BLTZAL
    case 0x11: branch(s >= 0, true, false); return;   // BGEZAL
    case 0x12: branch(s < 0, true, true); return;     // BLTZALL
    case 0x13: branch(s >= 0, true, true); return;    // BGEZALL
    default: raise(ExcCode::RI); return;
  }
}

void Vr4300::exec_cop0(u32 instr) {
  if (!cop_usable(0)) return raise(ExcCode::CpU, 0);
  const u32 sub = rs(instr);
  switch (sub) {
    case 0x00:  // MFC0
      if (rt(instr)) gpr_[rt(instr)] = sext32(lo32(cop0_get(rd(instr))));
      return;
    case 0x01:  // DMFC0
      if (!ops64()) return raise(ExcCode::RI);
      if (rt(instr)) gpr_[rt(instr)] = cop0_get(rd(instr));
      return;
    case 0x04:  // MTC0
      cop0_set(rd(instr), sext32(lo32(gpr_[rt(instr)])));
      return;
    case 0x05:  // DMTC0
      if (!ops64()) return raise(ExcCode::RI);
      cop0_set(rd(instr), gpr_[rt(instr)]);
      return;
    default: break;
  }
  if (sub >= 0x10) {
    // CO functions. Undefined function codes are "phi": invalid but no RI on the VR4300
    // (VR4300 manual Figure 16-1 key). Function 0x10 (RFE) is "xi": RI on the VR4300.
    switch (funct(instr)) {
      case 0x01: tlb_read(); return;              // TLBR
      case 0x02: tlb_write(index_ & 31); return;  // TLBWI
      case 0x06: tlb_write(random_reg() & 31); return;  // TLBWR
      case 0x08: tlb_probe(); return;             // TLBP
      case 0x18:  // ERET: the instruction behind it was fetched and is not executed
        rf_fetch();  // in RF during ERET's EX, before the mode changes
        eret();
        kill_slot_ = true;
        return;
      case 0x10: raise(ExcCode::RI); return;
      default: return;
    }
  }
  raise(ExcCode::RI);
}

void Vr4300::exec_cop2(u32 instr) {
  // The VR4300 has no COP2, but with CU2 set the move instructions talk to a single 64-bit
  // latch; anything else is RI with Cause.CE = 2.
  // Source: n64-systemtest src/tests/cop_unusable ("Some findings", COP2MFCBehavior).
  if (!cop_usable(2)) return raise(ExcCode::CpU, 2);
  switch (rs(instr)) {
    case 0x00:  // MFC2
      if (rt(instr)) gpr_[rt(instr)] = sext32(lo32(cop2_latch_));
      return;
    case 0x01:  // DMFC2
      if (rt(instr)) gpr_[rt(instr)] = cop2_latch_;
      return;
    case 0x02:  // CFC2
    case 0x06:  // CTC2
      return;
    case 0x04:  // MTC2
    case 0x05:  // DMTC2
      cop2_latch_ = gpr_[rt(instr)];
      return;
    default: raise(ExcCode::RI, 2); return;
  }
}

void Vr4300::exec_cop1(u32 instr) { fpu_execute(instr); }

void Vr4300::exec_cache(u32 instr) {
  if (!cop_usable(0)) return raise(ExcCode::CpU, 0);
  dc_stage();  // CACHE operates in the DC stage (manual 4.6.8)
  const u64 vaddr = gpr_[rs(instr)] + simm(instr);
  const u32 which = rt(instr) & 3;
  const u32 func = rt(instr) >> 2;
  // Cache ops on caches other than primary I/D (2, 3) are no-ops on the VR4300.
  if (which > 1) return;

  // Index operations (0-2) use the virtual address directly as the cache index; the others
  // translate it and may raise TLB / address exceptions.
  Translation tr{0, true};
  if (func > 2) {
    if (!translate(vaddr, Access::Load, tr)) return;
  }

  if (which == 0) {
    ILine& line = iline(vaddr);
    cycles_ += timing::kCacheOpICycles;
    switch (func) {
      case 0: line.valid = false; return;  // Index_Invalidate
      case 1:                              // Index_Load_Tag
        tag_lo_ = TagLo::PTagLo::place(line.ptag) | TagLo::PState::place(line.valid ? 2 : 0);
        tag_hi_ = 0;
        return;
      case 2:  // Index_Store_Tag
        line.ptag = TagLo::PTagLo::get(tag_lo_);
        line.valid = (TagLo::PState::get(tag_lo_) & 2) != 0;
        return;
      case 4:  // Hit_Invalidate
        if (line.valid && line.ptag == (tr.paddr >> 12)) line.valid = false;
        return;
      case 5:  // Fill
        icache_fill(line, tr.paddr);
        return;
      case 6:  // Hit_Write_Back
        if (line.valid && line.ptag == (tr.paddr >> 12)) {
          const u32 base = tr.paddr & ~(kICacheLineBytes - 1);
          bus_write(Master::CpuInstr, base, 16, line.data.data());
          bus_write(Master::CpuInstr, base + 16, 16, line.data.data() + 16);
        }
        return;
      default: return;
    }
  }

  DLine& line = dline(vaddr);
  const u32 index = static_cast<u32>((vaddr >> 4) & (kDCacheLines - 1));
  cycles_ += timing::kCacheOpDCycles;
  const bool hit = line.valid && line.ptag == (tr.paddr >> 12);
  switch (func) {
    case 0:  // Index_Write_Back_Invalidate
      if (line.valid && line.dirty) dcache_writeback(line, index);
      line.valid = false;
      return;
    case 1:  // Index_Load_Tag
      tag_lo_ = TagLo::PTagLo::place(line.ptag) | TagLo::PState::place(line.valid ? 3 : 0);
      tag_hi_ = 0;
      return;
    case 2:  // Index_Store_Tag
      line.ptag = TagLo::PTagLo::get(tag_lo_);
      line.valid = TagLo::PState::get(tag_lo_) != 0;
      line.dirty = TagLo::PState::get(tag_lo_) == 3;
      return;
    case 3:  // Create_Dirty_Exclusive
      if (!hit) {
        if (line.valid && line.dirty) dcache_writeback(line, index);
        line.ptag = tr.paddr >> 12;
        line.valid = true;
      }
      line.dirty = true;
      return;
    case 4:  // Hit_Invalidate
      if (hit) line.valid = false;
      return;
    case 5:  // Hit_Write_Back_Invalidate
      if (hit) {
        if (line.dirty) dcache_writeback(line, index);
        line.valid = false;
      }
      return;
    case 6:  // Hit_Write_Back
      if (hit && line.dirty) dcache_writeback(line, index);
      return;
    default: return;
  }
}

}  // namespace ultra
