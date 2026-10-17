# ADR 0002: Bus-transaction trace format

Status: proposed (Phase 0)

## Context

The determinism gate (`--threads=1` vs `--threads=8`, Phase 2) and every timing investigation
need a record of what happened on the buses and when. The trace must not perturb timing, must
be complete (never drop records) and must be byte-identical across runs with the same inputs.

## Decision

**File layout** (`core/trace.hpp`): a 16-byte header followed by fixed 32-byte records, all
little-endian host layout (the development hosts are little-endian; the header's
`record_size` guards layout changes).

| Header field | Size | Value |
|---|---|---|
| `magic` | 8 | `"RLTYTRC\0"` |
| `version` | 4 | 1 (`kTraceVersion`) |
| `record_size` | 4 | 32 |

| Record field | Size | Meaning |
|---|---|---|
| `tick` | 8 | MClock tick the transaction started on its bus |
| `address` | 4 | physical address (RDRAM, RCP register, PI or SI bus address) |
| `duration` | 4 | ticks until completion; `0xFFFFFFFF` = never completes (CPU freeze) |
| `data` | 8 | first up-to-8 payload bytes, as a big-endian value |
| `payload_hash` | 4 | FNV-1a over the whole payload when `size > 8`, else 0 |
| `size` | 2 | payload bytes |
| `master` | 1 | `Master` enum: CpuData, CpuInstr, RspDma, Rdp, Vi, Ai, Pi, Si, Rsp, Pif |
| `kind` | 1 | Read, Write, BlockRead (refill / DMA read), BlockWrite |

Records appear in the order the timing thread issued them, which is the deterministic event
order of ADR 0001 (not sorted by `tick`: a buffered CPU write is recorded when it reaches the
bus).

**Producer path:** the timing thread appends to a fixed-size single-producer/single-consumer
ring allocated at `open()`. It never locks or allocates. When the ring is full the producer
blocks on the consumer's tail index (`std::atomic::wait`) until the writer frees space
(backpressure), so no record is ever dropped and the trace
content cannot depend on host speed. A writer thread drains the ring with `fwrite`; it sleeps
on an atomic counter that the producer bumps on every push and that `close()` bumps after
setting `stop`, and it re-checks the head after observing `stop`, so the final records are
always flushed.

**Tooling:** `tools/tracediff` diffs two traces (first divergence with context) or dumps a
range filtered by address.

## Consequences

- One record per bus transaction costs 32 bytes; a second of CPU-heavy emulation is a few
  hundred MB. Tracing is opt-in (`--trace FILE`) and off by default.
- Adding fields needs a version bump; `tracediff` refuses mismatched versions.
- Hashing large payloads keeps records fixed-size while still catching data divergence.

## Alternatives rejected

- Text traces: 5-10x larger and slower; diffing is easy with `tracediff` instead.
- Dropping records under pressure: makes the trace host-dependent, defeating its purpose.
