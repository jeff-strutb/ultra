# ADR 0001: Scheduler and the CPU-driven event loop

Status: proposed (Phase 0)

## Context

Every component of the N64 runs from clocks derived from one 62.5 MHz RCP master clock
(MClock): the VR4300 at 3/2 MClock, RDRAM at 4 TCycles per MClock, the VI and AI at VCLK (a
rational multiple of MClock per standard), SI/PIF at MClock/4 and /32 (n64brew "Clock
Timing"). Emulation must be deterministic (the same inputs give the same trace on every host
and thread count) and must let every bus transaction carry a completion time.

## Decision

1. **One time base.** `Tick` is an MClock cycle (`core/clock.hpp`). CPU time is kept in
   PCycles (`CpuCycle`) and converted exactly: 3 PCycles = 2 ticks. VCLK conversions carry an
   exact rational remainder per device (`VideoClockRatio`), so no clock drifts.
2. **One ordered event queue** (`core/scheduler.hpp`): an intrusive binary heap of at most 64
   pre-registered `Event`s, ordered by `(when, seq)`, where `seq` is a monotonic counter
   assigned at `schedule()`. Ties therefore dispatch in schedule order, which makes dispatch a
   pure function of the sequence of schedule calls. Schedule, cancel and dispatch never
   allocate; registration happens once at construction.
3. **The CPU drives time.** The VR4300 interpreter steps one instruction at a time. Before each
   step it dispatches every event due at or before its current tick. A bus access issued at
   tick `t` first runs the scheduler to `t` (`Bus::cpu_read/cpu_write`), so a device always
   observes CPU accesses in timestamp order with its own events. The CPU stalls by advancing
   its cycle counter to the completion tick the bus returns.
4. **Devices never run ahead of the CPU.** `System::run_until` advances in slices of 4096 ticks;
   devices are run to `min(slice end, CPU time)`. When the CPU is frozen (a 64-bit RCP read, a
   cached access outside RDRAM, or the PIF's 5-second lockout), devices continue alone.
   `kTickNever` returned by the bus means the CPU freezes forever, as on hardware.
5. **Save states** record `now`, `next_seq` and each registered event's `(queued, when, seq)`
   by registration id; the heap is rebuilt on load (ADR 0003).

## Consequences

- Determinism follows from (2) and (3): there is no host time, no thread interleaving and no
  unordered container in the dispatch path. `tests/unit/test_machine.cpp` checks that two runs
  produce byte-identical save states.
- Device models must schedule their own completions (DMA steps, VI lines, SI packets) instead
  of being polled; this keeps the CPU loop free of per-device checks.
- The RSP (Phase 1) will join as a second stepped agent; this ADR will need a successor that
  defines how two stepped agents interleave (lock-step by tick, CPU first on ties).
- The 64-event cap is a static limit checked at registration; raising it is a one-line change.

## Alternatives rejected

- **Catch-up scheduling per device** (each device runs to "now" when touched): simpler, but
  device-to-device interactions (VI refresh against PI DMA against CPU reads) would depend on
  which device happened to be touched first.
- **Global cycle loop ticking every device each MClock**: exact by construction but two orders
  of magnitude slower than needed, and still needs an event model for VCLK-domain devices.
