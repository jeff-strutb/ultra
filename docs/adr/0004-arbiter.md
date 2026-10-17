# ADR 0004: RDRAM arbiter interface

Status: proposed (Phase 0)

## Context

How long an RDRAM access takes depends on who else is using the RDRAM channel (CPU refills, RSP
and RDP traffic, PI/SI/AI/VI DMA, refresh) and on RDRAM row state. These costs are largely
unmeasured. The spec requires the timing model to be replaceable as hardware measurements
arrive, with every parameter carrying its confidence.

## Decision

- Every RDRAM transaction goes through `Ri` (`memory/ri.hpp`), which charges it to an
  `IArbiter` (`memory/arbiter.hpp`): `Tick request(const ArbiterRequest&)` returns the
  completion tick for a request (start tick, requester, physical address, size, read or
  write); `refresh(tick)` is called on VI
  HSYNC; `params()` lists every timing parameter with its unit, `Confidence` and source;
  `serialize()` saves its state.
- `IArbiter` is one of the two virtual interfaces allowed on the hot path (CLAUDE.md). One
  arbiter instance exists per machine; the call is not inlined, which costs a few ns per RDRAM
  transaction.
- **v1 implementation `BaseRdramArbiter`**: a single channel that is busy until its last
  transaction ends; each transaction costs the request packet, read or write delay, data at 2
  bytes per TCycle, a row-miss penalty per 1 MiB bank (8 banks tracked, 2 KiB rows), a dirty
  row extra, a fixed RI overhead, and refresh occupancy. Protocol-derived constants are
  `Derived`; row miss, dirty extra, RI overhead and refresh are `Guess`
  (`memory/arbiter_params.h`, `docs/timing/ledger.md`).
- `ultra --timing-report` prints the parameter table.

## Consequences

- Phase 3 replaces or refits the arbiter without touching requesters; probes 4 and 6 settle the
  `Guess` entries.
- Requesters must not cache latency results; they ask per transaction.
- A transaction's completion tick is a pure function of the request sequence, preserving
  determinism (ADR 0001).
