#pragma once
// Bus-transaction trace. See docs/adr/0002-trace-format.md for the on-disk format.
//
// The timing thread appends fixed-size records into a single-producer/single-consumer ring;
// a writer thread drains the ring to disk. The producer never locks or allocates. If the ring
// is full the producer waits for the writer (backpressure), so the trace is always complete
// and its content never depends on host timing.

#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include "common/types.hpp"
#include "core/clock.hpp"

namespace ultra {

// Who drove the transaction.
enum class Master : u8 {
  CpuData = 0,   // VR4300 load/store (uncached) or data-cache refill / write-back
  CpuInstr = 1,  // VR4300 instruction fetch (uncached) or instruction-cache refill
  RspDma = 2,
  Rdp = 3,
  Vi = 4,
  Ai = 5,
  Pi = 6,
  Si = 7,
  Rsp = 8,       // RSP core access to SP memory / registers
  Pif = 9,       // PIF-side accesses (Joybus, CIC), not on the RDRAM bus
};

enum class TraceKind : u8 {
  Read = 0,
  Write = 1,
  BlockRead = 2,   // cache refill or DMA burst read
  BlockWrite = 3,  // cache write-back or DMA burst write
};

#pragma pack(push, 1)
struct TraceRecord {
  u64 tick;       // MClock tick the transaction started on the bus
  u32 address;    // physical address
  u32 duration;   // ticks until completion (completion = tick + duration)
  u64 data;       // first (up to) 8 bytes of payload, big-endian value as seen on the bus
  u32 payload_hash;  // FNV-1a over the full payload (0 when size <= 8)
  u16 size;       // payload size in bytes
  Master master;
  TraceKind kind;
};
#pragma pack(pop)
static_assert(sizeof(TraceRecord) == 32);

struct TraceFileHeader {
  char magic[8];   // "ULTRTRC\0"
  u32 version;     // kTraceVersion
  u32 record_size; // sizeof(TraceRecord)
};
inline constexpr u32 kTraceVersion = 1;

class TraceSink {
 public:
  TraceSink() = default;
  ~TraceSink();
  TraceSink(const TraceSink&) = delete;
  TraceSink& operator=(const TraceSink&) = delete;

  // Opens the output file and starts the writer thread. Ring capacity is fixed here.
  bool open(const std::string& path, usize ring_records = 1u << 16);
  void close();
  bool enabled() const noexcept { return enabled_; }

  void record(const TraceRecord& r) noexcept {
    if (!enabled_) return;
    push(r);
  }

  u64 count() const noexcept { return produced_; }

  static u32 hash_payload(const u8* p, usize n) noexcept {
    u32 h = 2166136261u;
    for (usize i = 0; i < n; ++i) {
      h ^= p[i];
      h *= 16777619u;
    }
    return h;
  }

 private:
  void push(const TraceRecord& r) noexcept;
  void writer_main();

  bool enabled_ = false;
  std::FILE* file_ = nullptr;
  std::unique_ptr<TraceRecord[]> ring_;
  usize capacity_ = 0;
  u64 produced_ = 0;                  // producer-private copy of head
  std::atomic<u64> head_{0};          // written by producer
  std::atomic<u64> tail_{0};          // written by consumer
  std::atomic<bool> stop_{false};
  std::atomic<u32> wake_{0};          // bumped on every push and on close; writer sleeps on it
  std::thread writer_;
};

}  // namespace ultra
