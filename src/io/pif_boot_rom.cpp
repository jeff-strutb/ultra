// Clean-room replacement for the PIF boot ROM (IPL1 + IPL2).
//
// Nintendo's IPL1/IPL2 are not shipped. This program performs the documented boot sequence
// (n64brew "Initial Program Load" and "PIF-NUS", Console startup) on the emulated CPU:
//   IPL1 (runs from PIF ROM, uncached through SI): set Status and Config, wait for the RSP to
//     halt, halt it and clear its interrupt, wait for SP DMA idle, reset PI, blank VI, stop AI,
//     copy IPL2 to IMEM and jump there with sp = 0xA400_1FF0.
//   IPL2 (runs from IMEM): program PI domain 1 from the ROM header, read the boot info from
//     PIF RAM, lock the PIF ROM, copy IPL3 (ROM 0x40..0xFFF) to DMEM, compute the IPL2
//     checksum of IPL3 with the CIC seed, hand it to the PIF (commands 0x20 then 0x40), set the
//     IPL3 entry registers (s3 ROM type, s4 TV type, s5 reset type, s6 seed, s7 version) and
//     jump to 0xA400_0040.
// The register contract at IPL3 entry was established by disassembling the 6102 and libdragon
// IPL3s that run on it (docs/hardware/pif.md). Instruction timing differs from Nintendo's ROM,
// so time-to-IPL3 is not hardware-exact (ledger "boot-rom-timing").

#include "io/pif_boot_rom.hpp"

#include "common/mips_asm.hpp"
#include "io/cic.hpp"

namespace ultra {

using namespace mips;

namespace {

constexpr u32 kIpl2Imem = 0xA400'1000;
// IPL2 checksum state S_0..S_15 (docs/hardware/cic-algorithms.md 1.3), in IMEM above the IPL2
// code and below the stack IPL3 inherits (sp = 0xA400_1FF0); build_ipl2 checks the gap.
constexpr u32 kChecksumState = 0xA400'1F00;
constexpr u32 kChecksumStateBytes = 16 * 4;
constexpr u32 kStackTop = 0xA400'1FF0;
constexpr u32 kSeedMultiplier = kIpl2SeedMultiplier;  // K, cic-algorithms.md 1.1
constexpr u32 kLastStep = kIpl3Bytes / 4;  // 1008 IPL3 words, cic-algorithms.md 1.4

// Rotates by the low 5 bits of `amt` (the shift instructions ignore the rest); the opposite
// shift uses 32 - amt, which the hardware also reduces mod 32, so amt = 0 gives src.
// Clobber t6/t7. dst may alias src or amt.
void emit_rotr(Assembler& a, Reg dst, Reg src, Reg amt) {
  a.srlv(t6, src, amt);
  a.ori(t7, zero, 32);
  a.subu(t7, t7, amt);
  a.sllv(t7, src, t7);
  a.or_(dst, t6, t7);
}
void emit_rotl(Assembler& a, Reg dst, Reg src, Reg amt) {
  a.sllv(t6, src, amt);
  a.ori(t7, zero, 32);
  a.subu(t7, t7, amt);
  a.srlv(t7, src, t7);
  a.or_(dst, t6, t7);
}

std::vector<u32> build_ipl2() {
  Assembler a(kIpl2Imem);
  const auto mix = a.new_label();

  // ---- PI domain 1: slowest timing, then the values in ROM header bytes 1..3 ----
  a.lui(t0, 0xA460);
  a.ori(t1, zero, 0xFF);
  a.sw(t1, 0x14, t0);
  a.sw(t1, 0x18, t0);
  a.ori(t1, zero, 0x0F);
  a.sw(t1, 0x1C, t0);
  a.ori(t1, zero, 0x03);
  a.sw(t1, 0x20, t0);
  a.lui(t2, 0xB000);
  a.lw(t1, 0, t2);
  a.andi(t3, t1, 0xFF);  // LAT = byte 3
  a.sw(t3, 0x14, t0);
  a.srl(t3, t1, 8);
  a.andi(t3, t3, 0xFF);  // PWD = byte 2
  a.sw(t3, 0x18, t0);
  a.srl(t3, t1, 16);
  a.andi(t4, t3, 0x0F);  // PGS = byte 1 low nibble
  a.sw(t4, 0x1C, t0);
  a.srl(t4, t3, 4);
  a.andi(t4, t4, 0x03);  // RLS = byte 1 high nibble
  a.sw(t4, 0x20, t0);

  // ---- boot info, then PIF ROM lockout (command 0x10) ----
  a.lui(t0, 0xBFC0);
  a.lw(s0, 0x7E4, t0);
  a.ori(t1, zero, 0x10);
  a.sw(t1, 0x7FC, t0);
  a.lw(zero, 0x7FC, t0);  // a PIF read waits for the pending write

  // ---- IPL3: ROM 0x40..0xFFF -> DMEM 0x40..0xFFF ----
  {
    const auto copy = a.new_label();
    a.lui(t2, 0xB000);
    a.ori(t2, t2, 0x40);
    a.lui(t3, 0xA400);
    a.ori(t3, t3, 0x40);
    a.lui(t4, 0xB000);
    a.ori(t4, t4, 0x1000);
    a.bind(copy);
    a.lw(t1, 0, t2);
    a.addiu(t2, t2, 4);
    a.addiu(t3, t3, 4);
    a.bne(t2, t4, copy);
    a.sw(t1, -4, t3);
  }

  // ---- IPL2 checksum of IPL3 (DMEM 0x40..0xFFF): docs/hardware/cic-algorithms.md 1 ----
  // s0 boot info (kept), s1 state base, s2 x = W[i-1], s3 v = previous word, s4 n = W[i],
  // s5 step i, s6 address of W[i-1], t4/t5 the step's A/B rotates. M takes a0, a1, a2 = i.
  const auto absorb = a.new_label();
  a.srl(t0, s0, 8);
  a.andi(t0, t0, 0xFF);  // IPL2 seed (boot info byte 0x26)
  a.li(t2, kSeedMultiplier);
  a.multu(t0, t2);
  a.mflo(t0);
  a.addiu(t0, t0, 1);
  a.lui(s6, 0xA400);
  a.ori(s6, s6, 0x40);
  a.lw(s3, 0, s6);  // step 1's previous word is W[0] itself
  a.xor_(t0, t0, s3);
  a.li(s1, kChecksumState);
  for (u32 n = 0; n < 16; ++n) a.sw(t0, static_cast<s32>(4 * n), s1);
  a.ori(s5, zero, 1);
  {
    const auto step = a.new_label();
    a.bind(step);
    a.lw(s2, 0, s6);
    a.jal(absorb);
    a.move(a2, s5);
    a.lw(s4, 4, s6);
    // S_10 = M(S_10 + x, n, i)
    a.lw(a0, 40, s1);
    a.addu(a0, a0, s2);
    a.jal(mix);
    a.move(a1, s4);
    a.sw(v0, 40, s1);
    // S_11 = M(S_11 xor x, n, i)
    a.lw(a0, 44, s1);
    a.xor_(a0, a0, s2);
    a.jal(mix);
    a.move(a1, s4);
    a.sw(v0, 44, s1);
    // S_12 += new S_8 xor x
    a.lw(t0, 32, s1);
    a.xor_(t0, t0, s2);
    a.lw(t2, 48, s1);
    a.addu(t2, t2, t0);
    a.sw(t2, 48, s1);
    // S_13 += ROTR(x, lo5 x) + ROTR(n, lo5 n)
    emit_rotr(a, t0, s2, s2);
    emit_rotr(a, t2, s4, s4);
    a.addu(t0, t0, t2);
    a.lw(t2, 52, s1);
    a.addu(t2, t2, t0);
    a.sw(t2, 52, s1);
    // S_14 = M(M(S_14, A, i), ROTR(n, lo5 x), i)
    a.lw(a0, 56, s1);
    a.jal(mix);
    a.move(a1, t4);
    emit_rotr(a, a1, s4, s2);
    a.jal(mix);
    a.move(a0, v0);
    a.sw(v0, 56, s1);
    // S_15 = M(M(S_15, B, i), ROTL(n, hi5 x), i)
    a.lw(a0, 60, s1);
    a.jal(mix);
    a.move(a1, t5);
    a.srl(t2, s2, 27);
    emit_rotl(a, a1, s4, t2);
    a.jal(mix);
    a.move(a0, v0);
    a.sw(v0, 60, s1);
    // next step; the last one (i = 1008) updates S_0..S_9 only
    a.move(s3, s2);
    a.addiu(s6, s6, 4);
    a.ori(t0, zero, kLastStep - 1);
    a.bne(s5, t0, step);
    a.addiu(s5, s5, 1);
    a.lw(s2, 0, s6);
    a.jal(absorb);
    a.move(a2, s5);
  }

  // ---- reduction (cic-algorithms.md 1.5): T_0..T_3 in s2, s3, s4, s7; j in s5 ----
  a.lw(s2, 0, s1);
  a.move(s3, s2);
  a.move(s4, s2);
  a.move(s7, s2);
  a.move(s5, zero);
  a.move(s6, s1);
  {
    const auto word = a.new_label();
    a.bind(word);
    a.lw(t0, 0, s6);  // y = S_j
    a.move(a2, s5);
    emit_rotr(a, t2, t0, t0);
    a.addu(s2, s2, t2);
    // T_1: add y when y < T_0, else M
    {
      const auto use_mix = a.new_label();
      const auto done = a.new_label();
      a.sltu(t2, t0, s2);
      a.beq(t2, zero, use_mix);
      a.nop();
      a.b(done);
      a.addu(s3, s3, t0);
      a.bind(use_mix);
      a.move(a0, s3);
      a.jal(mix);
      a.move(a1, t0);
      a.move(s3, v0);
      a.bind(done);
    }
    // T_2: add y when bits 1 and 0 of y agree, else M
    {
      const auto use_mix = a.new_label();
      const auto done = a.new_label();
      a.srl(t2, t0, 1);
      a.xor_(t2, t2, t0);
      a.andi(t2, t2, 1);
      a.bne(t2, zero, use_mix);
      a.nop();
      a.b(done);
      a.addu(s4, s4, t0);
      a.bind(use_mix);
      a.move(a0, s4);
      a.jal(mix);
      a.move(a1, t0);
      a.move(s4, v0);
      a.bind(done);
    }
    // T_3: xor y when y is odd, else M
    {
      const auto use_mix = a.new_label();
      const auto done = a.new_label();
      a.andi(t2, t0, 1);
      a.beq(t2, zero, use_mix);
      a.nop();
      a.b(done);
      a.xor_(s7, s7, t0);
      a.bind(use_mix);
      a.move(a0, s7);
      a.jal(mix);
      a.move(a1, t0);
      a.move(s7, v0);
      a.bind(done);
    }
    a.addiu(s5, s5, 1);
    a.ori(t2, zero, 16);
    a.bne(s5, t2, word);
    a.addiu(s6, s6, 4);
  }
  a.move(a0, s2);
  a.move(a1, s3);
  a.jal(mix);
  a.ori(a2, zero, 16);
  a.andi(v0, v0, 0xFFFF);  // checksum bits 47..32
  a.xor_(v1, s4, s7);      // checksum bits 31..0

  // ---- hand the checksum to the PIF: RAM 0x32..0x37, then commands 0x20 and 0x40 ----
  a.lui(t0, 0xBFC0);
  a.sw(v0, 0x7F0, t0);  // bytes 0x30..0x33 = 00 00 hi hi
  a.lw(zero, 0x7F0, t0);
  a.sw(v1, 0x7F4, t0);  // bytes 0x34..0x37
  a.lw(zero, 0x7F4, t0);
  a.ori(t1, zero, 0x20);
  a.sw(t1, 0x7FC, t0);
  {
    const auto wait = a.new_label();
    a.bind(wait);
    a.lw(t1, 0x7FC, t0);
    a.andi(t1, t1, 0x80);
    a.beq(t1, zero, wait);
    a.nop();
  }
  a.ori(t1, zero, 0x40);
  a.sw(t1, 0x7FC, t0);
  a.lw(zero, 0x7FC, t0);

  // ---- IPL3 entry registers ----
  a.srl(s4, s0, 24);
  a.andi(s4, s4, 3);   // TV type
  a.srl(s5, s0, 26);
  a.andi(s5, s5, 1);   // reset type
  a.srl(s3, s0, 27);
  a.andi(s3, s3, 1);   // ROM type
  a.srl(s6, s0, 16);
  a.andi(s6, s6, 0xFF);  // IPL3 seed
  a.move(s7, zero);    // version (Guess: ledger "ipl3-s7")
  a.lui(t3, 0xA400);
  a.ori(t3, t3, 0x40);
  a.lui(sp, 0xA400);
  a.jr(t3);
  a.ori(sp, sp, 0x1FF0);

  // ---- absorb: S_0..S_9 for step i (cic-algorithms.md 1.4). In: s2 x, s3 v, a2 = s5 = i.
  // Out: t4 = A, t5 = B. Clobbers t0-t3, t6, t7 and M's registers; returns through t1. ----
  a.bind(absorb);
  a.move(t1, ra);
  emit_rotr(a, t4, s2, s3);  // A = ROTR(x, lo5 v)
  a.srl(t2, s3, 27);
  emit_rotl(a, t5, s2, t2);  // B = ROTL(x, hi5 v)
  // S_0 += M(1007 - i, x, i)
  a.ori(a0, zero, kLastStep - 1);
  a.subu(a0, a0, s5);
  a.jal(mix);
  a.move(a1, s2);
  a.lw(t0, 0, s1);
  a.addu(t0, t0, v0);
  a.sw(t0, 0, s1);
  // S_1 = M(S_1, x, i)
  a.lw(a0, 4, s1);
  a.jal(mix);
  a.move(a1, s2);
  a.sw(v0, 4, s1);
  // S_2 ^= x
  a.lw(t0, 8, s1);
  a.xor_(t0, t0, s2);
  a.sw(t0, 8, s1);
  // S_3 += M(x + 5, K, i); new S_3 stays in t0
  a.li(a1, kSeedMultiplier);
  a.jal(mix);
  a.addiu(a0, s2, 5);
  a.lw(t0, 12, s1);
  a.addu(t0, t0, v0);
  a.sw(t0, 12, s1);
  // S_4 += A; new S_4 stays in t2
  a.lw(t2, 16, s1);
  a.addu(t2, t2, t4);
  a.sw(t2, 16, s1);
  // S_5 += B
  a.lw(t3, 20, s1);
  a.addu(t3, t3, t5);
  a.sw(t3, 20, s1);
  // S_6 from the old S_6 (t3) and the new S_3/S_4
  {
    const auto ge = a.new_label();
    const auto done = a.new_label();
    a.lw(t3, 24, s1);
    a.sltu(t6, s2, t3);
    a.beq(t6, zero, ge);
    a.addu(t7, s2, s5);
    a.addu(t6, t0, t3);
    a.b(done);
    a.xor_(t6, t6, t7);
    a.bind(ge);
    a.addu(t6, t2, s2);
    a.xor_(t6, t6, t3);
    a.bind(done);
    a.sw(t6, 24, s1);
  }
  // S_7 = M(S_7, ROTL(x, lo5 v), i)
  emit_rotl(a, a1, s2, s3);
  a.lw(a0, 28, s1);
  a.jal(mix);
  a.nop();
  a.sw(v0, 28, s1);
  // S_8 = M(S_8, ROTR(x, hi5 v), i)
  a.srl(t2, s3, 27);
  emit_rotr(a, a1, s2, t2);
  a.lw(a0, 32, s1);
  a.jal(mix);
  a.nop();
  a.sw(v0, 32, s1);
  // S_9: M when v < x, else add x
  {
    const auto add = a.new_label();
    const auto done = a.new_label();
    a.lw(a0, 36, s1);
    a.sltu(t6, s3, s2);
    a.beq(t6, zero, add);
    a.nop();
    a.jal(mix);
    a.move(a1, s2);
    a.b(done);
    a.move(a0, v0);
    a.bind(add);
    a.addu(a0, a0, s2);
    a.bind(done);
    a.sw(a0, 36, s1);
  }
  a.jr(t1);
  a.nop();

  // ---- M(a0, a1, a2) -> v0 (cic-algorithms.md 1.2). Clobbers a1, t8, t9. ----
  a.bind(mix);
  {
    const auto nonzero = a.new_label();
    const auto keep = a.new_label();
    a.bne(a1, zero, nonzero);
    a.nop();
    a.move(a1, a2);
    a.bind(nonzero);
    a.multu(a0, a1);
    a.mfhi(t8);
    a.mflo(t9);
    a.subu(v0, t8, t9);
    a.bne(v0, zero, keep);
    a.nop();
    a.move(v0, a0);
    a.bind(keep);
    a.jr(ra);
    a.nop();
  }
  if (a.pc() > kChecksumState || kChecksumState + kChecksumStateBytes > kStackTop) {
    throw std::logic_error("IPL2 overlaps its checksum state");
  }
  return a.finish();
}

}  // namespace

std::vector<u32> make_pif_boot_rom() {
  const std::vector<u32> ipl2 = build_ipl2();
  Assembler a(0xBFC0'0000);

  // ---- IPL1 ----
  a.lui(t1, 0x3400);
  a.mtc0(t1, 12);  // Status = 0x3400_0000 (CU1, CU0, FR)
  a.lui(t1, 0x0006);
  a.ori(t1, t1, 0xE463);
  a.mtc0(t1, 16);  // Config = 0x0006_E463
  a.lui(t0, 0xA404);
  {
    const auto wait = a.new_label();
    a.bind(wait);
    a.lw(t1, 0x10, t0);
    a.andi(t1, t1, 0x01);
    a.beq(t1, zero, wait);  // wait for SP halt
    a.nop();
  }
  a.ori(t1, zero, 0x0A);
  a.sw(t1, 0x10, t0);  // SP_STATUS: set halt, clear interrupt
  {
    const auto wait = a.new_label();
    a.bind(wait);
    a.lw(t1, 0x18, t0);
    a.andi(t1, t1, 0x01);
    a.bne(t1, zero, wait);  // SP_DMA_BUSY
    a.nop();
  }
  a.lui(t0, 0xA460);
  a.ori(t1, zero, 0x03);
  a.sw(t1, 0x10, t0);  // PI_STATUS: reset, clear interrupt
  a.lui(t0, 0xA440);
  a.ori(t1, zero, 0x3FF);
  a.sw(t1, 0x0C, t0);    // VI_V_INTR = 0x3FF
  a.sw(zero, 0x24, t0);  // VI_H_VIDEO = 0
  a.sw(zero, 0x10, t0);  // VI_V_CURRENT: clear VI interrupt
  a.lui(t0, 0xA450);
  a.sw(zero, 0x00, t0);  // AI_DRAM_ADDR = 0
  a.sw(zero, 0x04, t0);  // AI_LENGTH = 0
  a.lui(t0, 0xA404);
  {
    const auto wait = a.new_label();
    a.bind(wait);
    a.lw(t1, 0x10, t0);
    a.andi(t1, t1, 0x04);
    a.bne(t1, zero, wait);  // SP_STATUS.DMA_BUSY
    a.nop();
  }
  // Copy IPL2 (stored after IPL1 in this ROM) to IMEM and jump to it.
  const auto copy = a.new_label();
  a.lui(t3, 0xA400);
  a.ori(t3, t3, 0x1000);
  a.lui(t4, 0xBFC0);
  // t4/t5 = start/end of the IPL2 image (patched below once IPL1's size is known).
  const usize patch_lo = a.finish().size();
  a.ori(t4, t4, 0);
  a.lui(t5, 0xBFC0);
  a.ori(t5, t5, 0);
  a.bind(copy);
  a.lw(t1, 0, t4);
  a.addiu(t4, t4, 4);
  a.addiu(t3, t3, 4);
  a.bne(t4, t5, copy);
  a.sw(t1, -4, t3);
  a.lui(t3, 0xA400);
  a.ori(t3, t3, 0x1000);
  a.lui(sp, 0xA400);
  a.jr(t3);
  a.ori(sp, sp, 0x1FF0);
  std::vector<u32> rom = a.finish();

  const u32 start = static_cast<u32>(rom.size() * 4);
  const u32 end = start + static_cast<u32>(ipl2.size() * 4);
  rom[patch_lo] |= start & 0xFFFF;
  rom[patch_lo + 2] |= end & 0xFFFF;
  rom.insert(rom.end(), ipl2.begin(), ipl2.end());
  return rom;
}

}  // namespace ultra
