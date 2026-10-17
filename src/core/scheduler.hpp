#pragma once
// Single ordered event queue keyed on RCP ticks. See docs/adr/0001-scheduler.md.
//
// Events are intrusive: each device owns its Event objects and registers them once at boot.
// The queue is a fixed-capacity binary heap of pointers, so schedule/cancel/dispatch never
// allocate. Ties at the same tick dispatch in the order they were scheduled (a monotonic
// sequence number), which makes dispatch order a pure function of the schedule calls.

#include <array>
#include <cassert>

#include "common/types.hpp"
#include "core/clock.hpp"

namespace reality {

class SaveState;

struct Event {
  using Callback = void (*)(void* ctx, Tick when);

  const char* name = "";
  Callback fn = nullptr;
  void* ctx = nullptr;

  // Managed by the Scheduler.
  Tick when = kTickNever;
  u64 seq = 0;
  s32 heap_index = -1;  // -1 when not queued
  u16 id = 0;           // registration order, stable across runs; used by save states

  bool scheduled() const noexcept { return heap_index >= 0; }
};

class Scheduler {
 public:
  static constexpr usize kMaxEvents = 64;

  // Register an event once at init. Registration order defines the event's save-state id.
  void add(Event& ev, const char* name, Event::Callback fn, void* ctx);

  void schedule(Event& ev, Tick when);
  void cancel(Event& ev);

  Tick now() const noexcept { return now_; }
  Tick next_tick() const noexcept { return size_ == 0 ? kTickNever : heap_[0]->when; }

  // Dispatch every event with when <= limit in (when, seq) order, then set now() = limit.
  // Callbacks may schedule further events; those at or before limit are dispatched too.
  void run_until(Tick limit);

  // Advance now() without dispatching. Only valid when no event is due at or before t.
  void advance_to(Tick t) noexcept {
    assert(t >= now_ && next_tick() > t);
    now_ = t;
  }

  void serialize(SaveState& s);

 private:
  bool less(const Event* a, const Event* b) const noexcept {
    return a->when != b->when ? a->when < b->when : a->seq < b->seq;
  }
  void sift_up(usize i) noexcept;
  void sift_down(usize i) noexcept;
  void place(usize i, Event* e) noexcept {
    heap_[i] = e;
    e->heap_index = static_cast<s32>(i);
  }
  void remove_at(usize i) noexcept;

  Tick now_ = 0;
  u64 next_seq_ = 0;
  usize size_ = 0;
  std::array<Event*, kMaxEvents> heap_{};
  std::array<Event*, kMaxEvents> registry_{};
  usize registered_ = 0;
};

}  // namespace reality
