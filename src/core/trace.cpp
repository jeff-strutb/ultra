#include "core/trace.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

namespace reality {

TraceSink::~TraceSink() { close(); }

bool TraceSink::open(const std::string& path, usize ring_records) {
  close();
  file_ = std::fopen(path.c_str(), "wb");
  if (!file_) return false;
  TraceFileHeader h{};
  std::memcpy(h.magic, "RLTYTRC", 8);
  h.version = kTraceVersion;
  h.record_size = sizeof(TraceRecord);
  std::fwrite(&h, sizeof h, 1, file_);
  capacity_ = std::bit_ceil(ring_records);
  ring_ = std::make_unique<TraceRecord[]>(capacity_);
  produced_ = 0;
  head_.store(0, std::memory_order_relaxed);
  tail_.store(0, std::memory_order_relaxed);
  stop_.store(false, std::memory_order_relaxed);
  enabled_ = true;
  writer_ = std::thread([this] { writer_main(); });
  return true;
}

void TraceSink::close() {
  if (!enabled_) return;
  stop_.store(true, std::memory_order_release);
  wake_.fetch_add(1, std::memory_order_release);
  wake_.notify_one();
  if (writer_.joinable()) writer_.join();
  std::fclose(file_);
  file_ = nullptr;
  ring_.reset();
  enabled_ = false;
}

void TraceSink::push(const TraceRecord& r) noexcept {
  // Backpressure: wait until the writer has freed a slot.
  for (;;) {
    u64 tail = tail_.load(std::memory_order_acquire);
    if (produced_ - tail < capacity_) break;
    tail_.wait(tail, std::memory_order_acquire);
  }
  ring_[produced_ & (capacity_ - 1)] = r;
  ++produced_;
  head_.store(produced_, std::memory_order_release);
  wake_.fetch_add(1, std::memory_order_release);
  wake_.notify_one();
}

void TraceSink::writer_main() {
  u64 tail = 0;
  for (;;) {
    u32 seen = wake_.load(std::memory_order_acquire);
    u64 head = head_.load(std::memory_order_acquire);
    if (head == tail) {
      // stop_ is set after the producer's last push; re-read head once stop is seen so a
      // push that raced with the first head load is not lost.
      if (stop_.load(std::memory_order_acquire)) {
        if (head_.load(std::memory_order_acquire) == tail) break;
        continue;
      }
      wake_.wait(seen, std::memory_order_acquire);
      continue;
    }
    while (tail != head) {
      usize idx = tail & (capacity_ - 1);
      usize run = std::min<u64>(head - tail, capacity_ - idx);
      std::fwrite(&ring_[idx], sizeof(TraceRecord), run, file_);
      tail += run;
      tail_.store(tail, std::memory_order_release);
      tail_.notify_one();
    }
  }
  std::fflush(file_);
}

}  // namespace reality
