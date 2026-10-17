#include "core/scheduler.hpp"

#include <stdexcept>

#include "core/savestate.hpp"

namespace ultra {

void Scheduler::add(Event& ev, const char* name, Event::Callback fn, void* ctx) {
  if (registered_ == kMaxEvents) throw std::logic_error("Scheduler: too many events");
  ev.name = name;
  ev.fn = fn;
  ev.ctx = ctx;
  ev.when = kTickNever;
  ev.heap_index = -1;
  ev.id = static_cast<u16>(registered_);
  registry_[registered_++] = &ev;
}

void Scheduler::schedule(Event& ev, Tick when) {
  assert(ev.fn != nullptr && "event not registered");
  if (ev.scheduled()) remove_at(static_cast<usize>(ev.heap_index));
  ev.when = when;
  ev.seq = next_seq_++;
  usize i = size_++;
  place(i, &ev);
  sift_up(i);
}

void Scheduler::cancel(Event& ev) {
  if (ev.scheduled()) remove_at(static_cast<usize>(ev.heap_index));
  ev.when = kTickNever;
}

void Scheduler::run_until(Tick limit) {
  while (size_ != 0 && heap_[0]->when <= limit) {
    Event* e = heap_[0];
    remove_at(0);
    // Time never runs backwards: an event scheduled in the past fires "now".
    if (e->when > now_) now_ = e->when;
    Tick when = e->when;
    e->when = kTickNever;
    e->fn(e->ctx, when);
  }
  if (limit > now_) now_ = limit;
}

void Scheduler::sift_up(usize i) noexcept {
  Event* e = heap_[i];
  while (i > 0) {
    usize parent = (i - 1) / 2;
    if (!less(e, heap_[parent])) break;
    place(i, heap_[parent]);
    i = parent;
  }
  place(i, e);
}

void Scheduler::sift_down(usize i) noexcept {
  Event* e = heap_[i];
  for (;;) {
    usize l = 2 * i + 1;
    if (l >= size_) break;
    usize r = l + 1;
    usize m = (r < size_ && less(heap_[r], heap_[l])) ? r : l;
    if (!less(heap_[m], e)) break;
    place(i, heap_[m]);
    i = m;
  }
  place(i, e);
}

void Scheduler::remove_at(usize i) noexcept {
  Event* e = heap_[i];
  e->heap_index = -1;
  --size_;
  if (i == size_) return;
  place(i, heap_[size_]);
  heap_[size_] = nullptr;
  if (i > 0 && less(heap_[i], heap_[(i - 1) / 2]))
    sift_up(i);
  else
    sift_down(i);
}

void Scheduler::serialize(SaveState& s) {
  s.section("scheduler", 1);
  s.value(now_);
  s.value(next_seq_);
  u64 count = registered_;
  s.value(count);
  if (s.loading() && count != registered_)
    throw std::runtime_error("save state: scheduler event count mismatch");
  // Events are saved by registration id; the heap is rebuilt on load.
  if (s.loading()) {
    for (usize i = 0; i < size_; ++i) heap_[i]->heap_index = -1;
    size_ = 0;
  }
  for (usize i = 0; i < registered_; ++i) {
    Event& e = *registry_[i];
    u8 queued = e.scheduled() ? 1 : 0;
    s.value(queued);
    s.value(e.when);
    s.value(e.seq);
    if (s.loading() && queued) {
      usize at = size_++;
      place(at, &e);
      sift_up(at);
    }
  }
}

}  // namespace ultra
