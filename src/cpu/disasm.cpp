#include "cpu/disasm.hpp"

#include <cstdio>

#include "cpu/decode.hpp"

namespace reality {

using namespace decode;

namespace {

const char* const kGpr[32] = {"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
                              "t0",   "t1", "t2", "t3", "t4", "t5", "t6", "t7",
                              "s0",   "s1", "s2", "s3", "s4", "s5", "s6", "s7",
                              "t8",   "t9", "k0", "k1", "gp", "sp", "fp", "ra"};

const char* const kCop0[32] = {
    "Index",    "Random",   "EntryLo0", "EntryLo1", "Context", "PageMask", "Wired",   "c0_7",
    "BadVAddr", "Count",    "EntryHi",  "Compare",  "Status",  "Cause",    "EPC",     "PRId",
    "Config",   "LLAddr",   "WatchLo",  "WatchHi",  "XContext", "c0_21",   "c0_22",   "c0_23",
    "c0_24",    "c0_25",    "PErr",     "CacheErr", "TagLo",   "TagHi",    "ErrorEPC", "c0_31"};

struct Out {
  char* buf;
  usize n;
  // Every caller passes a string literal; the template forwarding hides that from the
  // format checker.
  template <typename... A>
  std::string_view put(const char* f, A... a) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-security"
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
    int len = std::snprintf(buf, n, f, a...);
#pragma GCC diagnostic pop
    return {buf, len < 0 ? 0 : (static_cast<usize>(len) < n ? static_cast<usize>(len) : n - 1)};
  }
};

}  // namespace

std::string_view disassemble(u32 i, u64 pc, char* buf, usize n) {
  Out o{buf, n};
  const char* RS = kGpr[rs(i)];
  const char* RT = kGpr[rt(i)];
  const char* RD = kGpr[rd(i)];
  const s32 off = static_cast<s16>(imm(i));
  const u64 btarget = pc + 4 + (static_cast<u64>(static_cast<s64>(off)) << 2);
  const unsigned long long bt = btarget;
  auto mem = [&](const char* m) { return o.put("%-7s %s, %d(%s)", m, RT, off, RS); };
  auto br2 = [&](const char* m) { return o.put("%-7s %s, %s, 0x%llx", m, RS, RT, bt); };
  auto br1 = [&](const char* m) { return o.put("%-7s %s, 0x%llx", m, RS, bt); };
  auto alui = [&](const char* m) { return o.put("%-7s %s, %s, %d", m, RT, RS, off); };
  auto aluu = [&](const char* m) { return o.put("%-7s %s, %s, 0x%x", m, RT, RS, imm(i)); };
  auto r3 = [&](const char* m) { return o.put("%-7s %s, %s, %s", m, RD, RS, RT); };
  auto sh = [&](const char* m) { return o.put("%-7s %s, %s, %u", m, RD, RT, sa(i)); };
  auto shv = [&](const char* m) { return o.put("%-7s %s, %s, %s", m, RD, RT, RS); };
  auto fmem = [&](const char* m) { return o.put("%-7s f%u, %d(%s)", m, rt(i), off, RS); };

  if (i == 0) return o.put("nop");
  switch (opcode(i)) {
    case 0x00:
      switch (funct(i)) {
        case 0x00: return sh("sll");
        case 0x02: return sh("srl");
        case 0x03: return sh("sra");
        case 0x04: return shv("sllv");
        case 0x06: return shv("srlv");
        case 0x07: return shv("srav");
        case 0x08: return o.put("%-7s %s", "jr", RS);
        case 0x09: return o.put("%-7s %s, %s", "jalr", RD, RS);
        case 0x0C: return o.put("syscall");
        case 0x0D: return o.put("break");
        case 0x0F: return o.put("sync");
        case 0x10: return o.put("%-7s %s", "mfhi", RD);
        case 0x11: return o.put("%-7s %s", "mthi", RS);
        case 0x12: return o.put("%-7s %s", "mflo", RD);
        case 0x13: return o.put("%-7s %s", "mtlo", RS);
        case 0x14: return shv("dsllv");
        case 0x16: return shv("dsrlv");
        case 0x17: return shv("dsrav");
        case 0x18: return o.put("%-7s %s, %s", "mult", RS, RT);
        case 0x19: return o.put("%-7s %s, %s", "multu", RS, RT);
        case 0x1A: return o.put("%-7s %s, %s", "div", RS, RT);
        case 0x1B: return o.put("%-7s %s, %s", "divu", RS, RT);
        case 0x1C: return o.put("%-7s %s, %s", "dmult", RS, RT);
        case 0x1D: return o.put("%-7s %s, %s", "dmultu", RS, RT);
        case 0x1E: return o.put("%-7s %s, %s", "ddiv", RS, RT);
        case 0x1F: return o.put("%-7s %s, %s", "ddivu", RS, RT);
        case 0x20: return r3("add");
        case 0x21: return r3("addu");
        case 0x22: return r3("sub");
        case 0x23: return r3("subu");
        case 0x24: return r3("and");
        case 0x25: return r3("or");
        case 0x26: return r3("xor");
        case 0x27: return r3("nor");
        case 0x2A: return r3("slt");
        case 0x2B: return r3("sltu");
        case 0x2C: return r3("dadd");
        case 0x2D: return r3("daddu");
        case 0x2E: return r3("dsub");
        case 0x2F: return r3("dsubu");
        case 0x30: return o.put("%-7s %s, %s", "tge", RS, RT);
        case 0x31: return o.put("%-7s %s, %s", "tgeu", RS, RT);
        case 0x32: return o.put("%-7s %s, %s", "tlt", RS, RT);
        case 0x33: return o.put("%-7s %s, %s", "tltu", RS, RT);
        case 0x34: return o.put("%-7s %s, %s", "teq", RS, RT);
        case 0x36: return o.put("%-7s %s, %s", "tne", RS, RT);
        case 0x38: return sh("dsll");
        case 0x3A: return sh("dsrl");
        case 0x3B: return sh("dsra");
        case 0x3C: return sh("dsll32");
        case 0x3E: return sh("dsrl32");
        case 0x3F: return sh("dsra32");
        default: break;
      }
      break;
    case 0x01: {
      static const char* const kRegimm[32] = {
          "bltz", "bgez", "bltzl", "bgezl", nullptr, nullptr, nullptr, nullptr,
          "tgei", "tgeiu", "tlti", "tltiu", "teqi", nullptr, "tnei", nullptr,
          "bltzal", "bgezal", "bltzall", "bgezall"};
      const char* m = rt(i) < 20 ? kRegimm[rt(i)] : nullptr;
      if (!m) break;
      if (rt(i) >= 8 && rt(i) < 16) return o.put("%-7s %s, %d", m, RS, off);
      return br1(m);
    }
    case 0x02: return o.put("%-7s 0x%llx", "j", static_cast<unsigned long long>(((pc + 4) & ~u64{0x0FFFFFFF}) | (u64{target(i)} << 2)));
    case 0x03: return o.put("%-7s 0x%llx", "jal", static_cast<unsigned long long>(((pc + 4) & ~u64{0x0FFFFFFF}) | (u64{target(i)} << 2)));
    case 0x04: return br2("beq");
    case 0x05: return br2("bne");
    case 0x06: return br1("blez");
    case 0x07: return br1("bgtz");
    case 0x08: return alui("addi");
    case 0x09: return alui("addiu");
    case 0x0A: return alui("slti");
    case 0x0B: return alui("sltiu");
    case 0x0C: return aluu("andi");
    case 0x0D: return aluu("ori");
    case 0x0E: return aluu("xori");
    case 0x0F: return o.put("%-7s %s, 0x%x", "lui", RT, imm(i));
    case 0x10:
      switch (rs(i)) {
        case 0x00: return o.put("%-7s %s, %s", "mfc0", RT, kCop0[rd(i)]);
        case 0x01: return o.put("%-7s %s, %s", "dmfc0", RT, kCop0[rd(i)]);
        case 0x04: return o.put("%-7s %s, %s", "mtc0", RT, kCop0[rd(i)]);
        case 0x05: return o.put("%-7s %s, %s", "dmtc0", RT, kCop0[rd(i)]);
        default:
          if (rs(i) >= 0x10) {
            switch (funct(i)) {
              case 0x01: return o.put("tlbr");
              case 0x02: return o.put("tlbwi");
              case 0x06: return o.put("tlbwr");
              case 0x08: return o.put("tlbp");
              case 0x18: return o.put("eret");
              default: return o.put("cop0    0x%x", funct(i));
            }
          }
      }
      break;
    case 0x11:
      switch (rs(i)) {
        case 0x00: return o.put("%-7s %s, f%u", "mfc1", RT, fs(i));
        case 0x01: return o.put("%-7s %s, f%u", "dmfc1", RT, fs(i));
        case 0x02: return o.put("%-7s %s, fcr%u", "cfc1", RT, fs(i));
        case 0x04: return o.put("%-7s %s, f%u", "mtc1", RT, fs(i));
        case 0x05: return o.put("%-7s %s, f%u", "dmtc1", RT, fs(i));
        case 0x06: return o.put("%-7s %s, fcr%u", "ctc1", RT, fs(i));
        case 0x08: {
          static const char* const kBc[4] = {"bc1f", "bc1t", "bc1fl", "bc1tl"};
          return o.put("%-7s 0x%llx", kBc[rt(i) & 3], bt);
        }
        default: {
          static const char* const kFmt[32] = {
              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
              "s", "d", nullptr, nullptr, "w", "l"};
          static const char* const kOps[64] = {
              "add", "sub", "mul", "div", "sqrt", "abs", "mov", "neg",
              "round.l", "trunc.l", "ceil.l", "floor.l", "round.w", "trunc.w", "ceil.w", "floor.w",
              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
              "cvt.s", "cvt.d", nullptr, nullptr, "cvt.w", "cvt.l", nullptr, nullptr,
              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
              "c.f", "c.un", "c.eq", "c.ueq", "c.olt", "c.ult", "c.ole", "c.ule",
              "c.sf", "c.ngle", "c.seq", "c.ngl", "c.lt", "c.nge", "c.le", "c.ngt"};
          const char* f = kFmt[rs(i)];
          const char* m = kOps[funct(i)];
          if (!f || !m) return o.put("cop1    0x%08x", i);
          if (funct(i) >= 0x30) return o.put("%s.%s f%u, f%u", m, f, fs(i), ft(i));
          if (funct(i) < 4) return o.put("%s.%s f%u, f%u, f%u", m, f, fd(i), fs(i), ft(i));
          return o.put("%s.%s f%u, f%u", m, f, fd(i), fs(i));
        }
      }
    case 0x14: return br2("beql");
    case 0x15: return br2("bnel");
    case 0x16: return br1("blezl");
    case 0x17: return br1("bgtzl");
    case 0x18: return alui("daddi");
    case 0x19: return alui("daddiu");
    case 0x1A: return mem("ldl");
    case 0x1B: return mem("ldr");
    case 0x20: return mem("lb");
    case 0x21: return mem("lh");
    case 0x22: return mem("lwl");
    case 0x23: return mem("lw");
    case 0x24: return mem("lbu");
    case 0x25: return mem("lhu");
    case 0x26: return mem("lwr");
    case 0x27: return mem("lwu");
    case 0x28: return mem("sb");
    case 0x29: return mem("sh");
    case 0x2A: return mem("swl");
    case 0x2B: return mem("sw");
    case 0x2C: return mem("sdl");
    case 0x2D: return mem("sdr");
    case 0x2E: return mem("swr");
    case 0x2F: return o.put("%-7s 0x%x, %d(%s)", "cache", rt(i), off, RS);
    case 0x30: return mem("ll");
    case 0x31: return fmem("lwc1");
    case 0x34: return mem("lld");
    case 0x35: return fmem("ldc1");
    case 0x37: return mem("ld");
    case 0x38: return mem("sc");
    case 0x39: return fmem("swc1");
    case 0x3C: return mem("scd");
    case 0x3D: return fmem("sdc1");
    case 0x3F: return mem("sd");
    default: break;
  }
  return o.put(".word   0x%08x", i);
}

}  // namespace reality
