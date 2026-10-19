#pragma once
// Minimal MIPS III assembler used to build the clean-room PIF boot ROM and CPU unit-test
// programs. Encodings follow the VR4300 manual chapter 16 instruction formats.
// Branch and jump targets are labels resolved by finish(); delay slots are explicit.

#include <cassert>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/types.hpp"

namespace ultra::mips {

enum Reg : u32 {
  zero = 0, at, v0, v1, a0, a1, a2, a3, t0, t1, t2, t3, t4, t5, t6, t7,
  s0, s1, s2, s3, s4, s5, s6, s7, t8, t9, k0, k1, gp, sp, fp, ra,
};

class Assembler {
 public:
  using Label = u32;

  explicit Assembler(u64 base = 0) : base_(base) {}

  Label new_label() {
    labels_.push_back(~0u);
    return static_cast<Label>(labels_.size() - 1);
  }
  void bind(Label l) {
    assert(labels_[l] == ~0u);
    labels_[l] = static_cast<u32>(code_.size());
  }
  u64 pc() const noexcept { return base_ + code_.size() * 4; }
  u64 address_of(Label l) const { return base_ + u64{labels_[l]} * 4; }

  // ---- encoders ----------------------------------------------------------------------------
  void word(u32 w) { code_.push_back(w); }
  void r(u32 rs, u32 rt, u32 rd, u32 sa, u32 fn) {
    word((rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | fn);
  }
  void i(u32 op, u32 rs, u32 rt, u32 imm) { word((op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)); }

  // ALU
  void sll(Reg rd, Reg rt, u32 sa) { r(0, rt, rd, sa, 0x00); }
  void srl(Reg rd, Reg rt, u32 sa) { r(0, rt, rd, sa, 0x02); }
  void sra(Reg rd, Reg rt, u32 sa) { r(0, rt, rd, sa, 0x03); }
  void sllv(Reg rd, Reg rt, Reg rs) { r(rs, rt, rd, 0, 0x04); }
  void srlv(Reg rd, Reg rt, Reg rs) { r(rs, rt, rd, 0, 0x06); }
  void srav(Reg rd, Reg rt, Reg rs) { r(rs, rt, rd, 0, 0x07); }
  void jr(Reg rs) { r(rs, 0, 0, 0, 0x08); }
  void jalr(Reg rd, Reg rs) { r(rs, 0, rd, 0, 0x09); }
  void syscall() { r(0, 0, 0, 0, 0x0C); }
  void brk() { r(0, 0, 0, 0, 0x0D); }
  void mfhi(Reg rd) { r(0, 0, rd, 0, 0x10); }
  void mthi(Reg rs) { r(rs, 0, 0, 0, 0x11); }
  void mflo(Reg rd) { r(0, 0, rd, 0, 0x12); }
  void mtlo(Reg rs) { r(rs, 0, 0, 0, 0x13); }
  void mult(Reg rs, Reg rt) { r(rs, rt, 0, 0, 0x18); }
  void multu(Reg rs, Reg rt) { r(rs, rt, 0, 0, 0x19); }
  void div(Reg rs, Reg rt) { r(rs, rt, 0, 0, 0x1A); }
  void divu(Reg rs, Reg rt) { r(rs, rt, 0, 0, 0x1B); }
  void dmult(Reg rs, Reg rt) { r(rs, rt, 0, 0, 0x1C); }
  void dmultu(Reg rs, Reg rt) { r(rs, rt, 0, 0, 0x1D); }
  void ddiv(Reg rs, Reg rt) { r(rs, rt, 0, 0, 0x1E); }
  void ddivu(Reg rs, Reg rt) { r(rs, rt, 0, 0, 0x1F); }
  void add(Reg rd, Reg rs, Reg rt) { r(rs, rt, rd, 0, 0x20); }
  void addu(Reg rd, Reg rs, Reg rt) { r(rs, rt, rd, 0, 0x21); }
  void sub(Reg rd, Reg rs, Reg rt) { r(rs, rt, rd, 0, 0x22); }
  void subu(Reg rd, Reg rs, Reg rt) { r(rs, rt, rd, 0, 0x23); }
  void and_(Reg rd, Reg rs, Reg rt) { r(rs, rt, rd, 0, 0x24); }
  void or_(Reg rd, Reg rs, Reg rt) { r(rs, rt, rd, 0, 0x25); }
  void xor_(Reg rd, Reg rs, Reg rt) { r(rs, rt, rd, 0, 0x26); }
  void nor(Reg rd, Reg rs, Reg rt) { r(rs, rt, rd, 0, 0x27); }
  void slt(Reg rd, Reg rs, Reg rt) { r(rs, rt, rd, 0, 0x2A); }
  void sltu(Reg rd, Reg rs, Reg rt) { r(rs, rt, rd, 0, 0x2B); }
  void dadd(Reg rd, Reg rs, Reg rt) { r(rs, rt, rd, 0, 0x2C); }
  void daddu(Reg rd, Reg rs, Reg rt) { r(rs, rt, rd, 0, 0x2D); }
  void dsll(Reg rd, Reg rt, u32 sa) { r(0, rt, rd, sa, 0x38); }
  void dsrl(Reg rd, Reg rt, u32 sa) { r(0, rt, rd, sa, 0x3A); }
  void dsll32(Reg rd, Reg rt, u32 sa) { r(0, rt, rd, sa, 0x3C); }
  void dsrl32(Reg rd, Reg rt, u32 sa) { r(0, rt, rd, sa, 0x3E); }
  void move(Reg rd, Reg rs) { or_(rd, rs, zero); }
  void nop() { word(0); }

  void addi(Reg rt, Reg rs, s32 imm) { i(0x08, rs, rt, static_cast<u32>(imm)); }
  void addiu(Reg rt, Reg rs, s32 imm) { i(0x09, rs, rt, static_cast<u32>(imm)); }
  void slti(Reg rt, Reg rs, s32 imm) { i(0x0A, rs, rt, static_cast<u32>(imm)); }
  void sltiu(Reg rt, Reg rs, s32 imm) { i(0x0B, rs, rt, static_cast<u32>(imm)); }
  void andi(Reg rt, Reg rs, u32 imm) { i(0x0C, rs, rt, imm); }
  void ori(Reg rt, Reg rs, u32 imm) { i(0x0D, rs, rt, imm); }
  void xori(Reg rt, Reg rs, u32 imm) { i(0x0E, rs, rt, imm); }
  void lui(Reg rt, u32 imm) { i(0x0F, 0, rt, imm); }
  void daddiu(Reg rt, Reg rs, s32 imm) { i(0x19, rs, rt, static_cast<u32>(imm)); }
  // Load a 32-bit constant (sign-extended) in two instructions.
  void li(Reg rt, u32 v) {
    lui(rt, v >> 16);
    ori(rt, rt, v & 0xFFFF);
  }

  // Memory
  void lb(Reg rt, s32 off, Reg base) { i(0x20, base, rt, static_cast<u32>(off)); }
  void lh(Reg rt, s32 off, Reg base) { i(0x21, base, rt, static_cast<u32>(off)); }
  void lwl(Reg rt, s32 off, Reg base) { i(0x22, base, rt, static_cast<u32>(off)); }
  void lw(Reg rt, s32 off, Reg base) { i(0x23, base, rt, static_cast<u32>(off)); }
  void lbu(Reg rt, s32 off, Reg base) { i(0x24, base, rt, static_cast<u32>(off)); }
  void lhu(Reg rt, s32 off, Reg base) { i(0x25, base, rt, static_cast<u32>(off)); }
  void lwr(Reg rt, s32 off, Reg base) { i(0x26, base, rt, static_cast<u32>(off)); }
  void lwu(Reg rt, s32 off, Reg base) { i(0x27, base, rt, static_cast<u32>(off)); }
  void sb(Reg rt, s32 off, Reg base) { i(0x28, base, rt, static_cast<u32>(off)); }
  void sh(Reg rt, s32 off, Reg base) { i(0x29, base, rt, static_cast<u32>(off)); }
  void swl(Reg rt, s32 off, Reg base) { i(0x2A, base, rt, static_cast<u32>(off)); }
  void sw(Reg rt, s32 off, Reg base) { i(0x2B, base, rt, static_cast<u32>(off)); }
  void swr(Reg rt, s32 off, Reg base) { i(0x2E, base, rt, static_cast<u32>(off)); }
  void cache(u32 op, s32 off, Reg base) { i(0x2F, base, op, static_cast<u32>(off)); }
  void ll(Reg rt, s32 off, Reg base) { i(0x30, base, rt, static_cast<u32>(off)); }
  void ld(Reg rt, s32 off, Reg base) { i(0x37, base, rt, static_cast<u32>(off)); }
  void sc(Reg rt, s32 off, Reg base) { i(0x38, base, rt, static_cast<u32>(off)); }
  void sd(Reg rt, s32 off, Reg base) { i(0x3F, base, rt, static_cast<u32>(off)); }
  void lwc1(u32 ft, s32 off, Reg base) { i(0x31, base, ft, static_cast<u32>(off)); }
  void ldc1(u32 ft, s32 off, Reg base) { i(0x35, base, ft, static_cast<u32>(off)); }
  void swc1(u32 ft, s32 off, Reg base) { i(0x39, base, ft, static_cast<u32>(off)); }
  void sdc1(u32 ft, s32 off, Reg base) { i(0x3D, base, ft, static_cast<u32>(off)); }

  // COP0 / COP1
  void mfc0(Reg rt, u32 rd) { word((0x10u << 26) | (0x00u << 21) | (u32{rt} << 16) | (rd << 11)); }
  void mtc0(Reg rt, u32 rd) { word((0x10u << 26) | (0x04u << 21) | (u32{rt} << 16) | (rd << 11)); }
  void dmfc0(Reg rt, u32 rd) { word((0x10u << 26) | (0x01u << 21) | (u32{rt} << 16) | (rd << 11)); }
  void dmtc0(Reg rt, u32 rd) { word((0x10u << 26) | (0x05u << 21) | (u32{rt} << 16) | (rd << 11)); }
  void eret() { word(0x4200'0018); }
  void tlbwi() { word(0x4200'0002); }
  void tlbwr() { word(0x4200'0006); }
  void tlbr() { word(0x4200'0001); }
  void tlbp() { word(0x4200'0008); }
  void mfc1(Reg rt, u32 fs) { word((0x11u << 26) | (0x00u << 21) | (u32{rt} << 16) | (fs << 11)); }
  void mtc1(Reg rt, u32 fs) { word((0x11u << 26) | (0x04u << 21) | (u32{rt} << 16) | (fs << 11)); }
  void cfc1(Reg rt, u32 fs) { word((0x11u << 26) | (0x02u << 21) | (u32{rt} << 16) | (fs << 11)); }
  void ctc1(Reg rt, u32 fs) { word((0x11u << 26) | (0x06u << 21) | (u32{rt} << 16) | (fs << 11)); }
  // COP1 arithmetic: fmt 16=S 17=D 20=W 21=L
  void fpu(u32 fmt, u32 fn, u32 fd, u32 fs, u32 ft) {
    word((0x11u << 26) | (fmt << 21) | (ft << 16) | (fs << 11) | (fd << 6) | fn);
  }

  // Branches and jumps (label targets). The delay slot instruction must follow.
  void beq(Reg rs, Reg rt, Label l) { branch(0x04, rs, rt, l); }
  void bne(Reg rs, Reg rt, Label l) { branch(0x05, rs, rt, l); }
  void blez(Reg rs, Label l) { branch(0x06, rs, zero, l); }
  void bgtz(Reg rs, Label l) { branch(0x07, rs, zero, l); }
  void beql(Reg rs, Reg rt, Label l) { branch(0x14, rs, rt, l); }
  void bnel(Reg rs, Reg rt, Label l) { branch(0x15, rs, rt, l); }
  void bltz(Reg rs, Label l) { branch(0x01, rs, static_cast<Reg>(0x00), l); }
  void bgez(Reg rs, Label l) { branch(0x01, rs, static_cast<Reg>(0x01), l); }
  void bgezal(Reg rs, Label l) { branch(0x01, rs, static_cast<Reg>(0x11), l); }
  void b(Label l) { beq(zero, zero, l); }
  void bc1t(Label l) { branch(0x11, static_cast<Reg>(0x08), static_cast<Reg>(0x01), l); }
  void j(Label l) { fixups_.push_back({static_cast<u32>(code_.size()), l, true}); word(0x02u << 26); }
  void jal(Label l) { fixups_.push_back({static_cast<u32>(code_.size()), l, true}); word(0x03u << 26); }

  // Resolve labels and return the encoded words.
  std::vector<u32> finish() {
    for (const Fixup& f : fixups_) {
      const u32 target = labels_[f.label];
      if (target == ~0u) throw std::logic_error("mips::Assembler: unbound label");
      if (f.jump) {
        const u64 addr = base_ + u64{target} * 4;
        code_[f.at] |= static_cast<u32>((addr >> 2) & 0x03FF'FFFF);
      } else {
        const s64 off = static_cast<s64>(target) - static_cast<s64>(f.at) - 1;
        if (off < -32768 || off > 32767) throw std::logic_error("mips::Assembler: branch range");
        code_[f.at] |= static_cast<u32>(off) & 0xFFFF;
      }
    }
    fixups_.clear();
    return code_;
  }

 private:
  struct Fixup {
    u32 at;
    Label label;
    bool jump;
  };
  void branch(u32 op, Reg rs, Reg rt, Label l) {
    fixups_.push_back({static_cast<u32>(code_.size()), l, false});
    i(op, rs, rt, 0);
  }

  u64 base_;
  std::vector<u32> code_;
  std::vector<u32> labels_;
  std::vector<Fixup> fixups_;
};

}  // namespace ultra::mips
