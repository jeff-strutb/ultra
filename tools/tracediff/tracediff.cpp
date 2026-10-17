// tracediff: compare two bus-transaction traces and report the first divergence with
// surrounding context; or dump a trace in readable form.
//
//   tracediff A.trace B.trace [--context N]
//   tracediff --dump A.trace [--from N] [--count N] [--addr LO HI]
//
// Format: docs/adr/0002-trace-format.md.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "core/trace.hpp"

using namespace ultra;

namespace {

const char* master_name(Master m) {
  switch (m) {
    case Master::CpuData: return "cpu.d";
    case Master::CpuInstr: return "cpu.i";
    case Master::RspDma: return "sp.dma";
    case Master::Rdp: return "rdp";
    case Master::Vi: return "vi";
    case Master::Ai: return "ai";
    case Master::Pi: return "pi";
    case Master::Si: return "si";
    case Master::Rsp: return "rsp";
    case Master::Pif: return "pif";
  }
  return "?";
}

const char* kind_name(TraceKind k) {
  switch (k) {
    case TraceKind::Read: return "R ";
    case TraceKind::Write: return "W ";
    case TraceKind::BlockRead: return "BR";
    case TraceKind::BlockWrite: return "BW";
  }
  return "? ";
}

bool load(const char* path, std::vector<TraceRecord>& out) {
  std::FILE* f = std::fopen(path, "rb");
  if (!f) {
    std::fprintf(stderr, "cannot open %s\n", path);
    return false;
  }
  TraceFileHeader h{};
  if (std::fread(&h, sizeof h, 1, f) != 1 || std::memcmp(h.magic, "ULTRTRC", 8) != 0 ||
      h.version != kTraceVersion || h.record_size != sizeof(TraceRecord)) {
    std::fprintf(stderr, "%s: not a version-%u trace\n", path, kTraceVersion);
    std::fclose(f);
    return false;
  }
  TraceRecord r;
  while (std::fread(&r, sizeof r, 1, f) == 1) out.push_back(r);
  std::fclose(f);
  return true;
}

void print(const TraceRecord& r, usize index) {
  std::printf("#%-9zu t=%-12llu %-6s %s %08x %3u  data=%016llx hash=%08x dur=%u\n", index,
              static_cast<unsigned long long>(r.tick), master_name(r.master), kind_name(r.kind),
              r.address, r.size, static_cast<unsigned long long>(r.data), r.payload_hash, r.duration);
}

bool same(const TraceRecord& a, const TraceRecord& b) { return std::memcmp(&a, &b, sizeof a) == 0; }

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 3 && std::string_view(argv[1]) == "--dump") {
    std::vector<TraceRecord> t;
    if (!load(argv[2], t)) return 2;
    usize from = 0, count = t.size();
    u32 lo = 0, hi = ~0u;
    for (int i = 3; i < argc; ++i) {
      std::string_view a = argv[i];
      if (a == "--from" && i + 1 < argc) from = std::strtoull(argv[++i], nullptr, 0);
      else if (a == "--count" && i + 1 < argc) count = std::strtoull(argv[++i], nullptr, 0);
      else if (a == "--addr" && i + 2 < argc) {
        lo = static_cast<u32>(std::strtoul(argv[++i], nullptr, 0));
        hi = static_cast<u32>(std::strtoul(argv[++i], nullptr, 0));
      }
    }
    usize shown = 0;
    for (usize i = from; i < t.size() && shown < count; ++i) {
      if (t[i].address < lo || t[i].address > hi) continue;
      print(t[i], i);
      ++shown;
    }
    return 0;
  }
  if (argc < 3) {
    std::fprintf(stderr, "usage: tracediff A B [--context N] | tracediff --dump A [...]\n");
    return 2;
  }
  usize context = 8;
  for (int i = 3; i < argc; ++i)
    if (std::string_view(argv[i]) == "--context" && i + 1 < argc) context = std::strtoull(argv[++i], nullptr, 0);
  std::vector<TraceRecord> a, b;
  if (!load(argv[1], a) || !load(argv[2], b)) return 2;
  const usize n = std::min(a.size(), b.size());
  for (usize i = 0; i < n; ++i) {
    if (same(a[i], b[i])) continue;
    std::printf("first divergence at record %zu\n", i);
    const usize s = i > context ? i - context : 0;
    std::printf("--- %s\n", argv[1]);
    for (usize k = s; k < std::min(a.size(), i + context); ++k) print(a[k], k);
    std::printf("--- %s\n", argv[2]);
    for (usize k = s; k < std::min(b.size(), i + context); ++k) print(b[k], k);
    return 1;
  }
  if (a.size() != b.size()) {
    std::printf("traces identical for %zu records; lengths differ (%zu vs %zu)\n", n, a.size(), b.size());
    return 1;
  }
  std::printf("traces identical (%zu records)\n", n);
  return 0;
}
