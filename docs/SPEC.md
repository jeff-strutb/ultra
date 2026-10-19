# N64 Cycle-Accurate Emulator — Build Spec

Oct 17, 2026 · @Wilbur

## Goals and definition of correct

The deliverable is a clean-room C++23 Nintendo 64 emulator whose timing model is verifiable against real hardware at the cycle level, and which is usable as a day-to-day emulator even where it falls short of full speed. Name: **Ultra**.

**Correct means, in priority order:**

1. **Bit-exact output.** Every pixel the RDP writes, every sample the AI plays, every value the CPU reads from any register matches hardware. This is fully achievable today; the behavior is known.
2. **Cycle-exact where measurable.** Instruction retirement timing on the VR4300 and RSP, DMA transfer durations, interrupt latency, and VI scanline timing must match hardware-measured counts. The test oracle is a real N64 running probe ROMs; where a measurement exists, the emulator matches it.
3. **Cycle-plausible where unmeasured.** RDRAM arbitration and RDP internal pipeline timing are not fully characterized by anyone. For these the emulator implements an explicit, swappable timing model with named constants and a documented confidence level, so it can be tightened as measurements land without touching the rest of the codebase.

**Determinism is a hard requirement.** Given the same ROM, inputs and save state, every run produces the identical sequence of bus transactions. Multi-threading must never change results; it only changes wall-clock time. This is what makes trace comparison against hardware possible and what keeps the project honest.

**Performance target:** full speed (60 fps NTSC) on a current desktop core for 2D-heavy and light-3D titles; 30–50% speed for RDP-heavy titles in the fully accurate configuration. The architecture must allow a later "fast" tier (recompiler, block-level timing) without changing the timing model's interface, but that tier is out of scope for this spec.

**Non-goals:**

- High-level emulation of any kind (no HLE RSP microcode, no HLE RDP, no game-specific hacks). Ever.
- Upscaling, texture packs, widescreen hacks. A plain integer-scaled framebuffer is the only video path.
- Netplay, rewind, cheats, Lua. Save states are in scope because they are needed for test infrastructure.
- 64DD. Out of scope for v1; the design leaves a PI/DD hook.
- Supporting more than one host OS at first. Linux x86-64 is the development target; macOS and Windows follow once the core is stable.

## Reference material and clean-room policy

All code is written from documentation and observed behavior, not from other emulators' source. Other emulators are consulted to understand *what* hardware does; the *how* is ours.

**Clean-room rules Claude Code must follow:**

1. Reading another emulator's source to learn a hardware fact is allowed. Copying, transliterating, or structurally mirroring its code is not. Any fact learned this way is recorded in `docs/hardware/` with a citation before it is implemented, so the implementation derives from our notes, not from their code.
2. Lookup tables, constants and magic numbers are derived from the primary source (the hardware manual, a patent, a test ROM's measured result) and cited inline. Angrylion's tables are a known trap: several are derived empirically and have no primary source. Where that is the case, we regenerate them from first principles or from our own hardware captures, and document which.
3. No vendored third-party emulator code. Test ROMs and test harness code under permissive licenses may be vendored into `tests/roms/` with their licenses intact.
4. The repository license is MIT.

**Primary sources (authoritative):**

| Source | Covers | Notes |
| --- | --- | --- |
| [NEC VR4300 User's Manual](https://n64brew.dev/wiki/VR4300) | CPU ISA, pipeline, caches, TLB, COP0, FPU | The single most complete document in the project. Pipeline chapter is the timing model. |
| [N64brew Wiki](https://n64brew.dev/wiki/) | Every RCP interface register, memory map, PIF, RSP ISA and pipeline, RDP command set, VI timing, RDRAM/RI | Living document; mirror it locally on day one. [RSP CPU Pipeline](https://n64brew.dev/wiki/Reality_Signal_Processor/CPU_Pipeline) and [RDRAM Interface](https://n64brew.dev/wiki/RDRAM_Interface) pages are directly load-bearing. |
| US Patent 6,593,929 | RCP memory controller and RI register semantics | Cited by the RI wiki page; the only SGI-authored description of RDRAM handling. |
| Rambus Base/Concurrent RDRAM datasheets | Bus protocol, row/bank timing, refresh | The N64 uses the earliest Base RDRAM protocol. Needed to build a believable arbiter. |
| [libdragon](https://github.com/DragonMinded/libdragon) | Toolchain and SDK for building our own probe ROMs | Build dependency for `tests/probes/`. |

**Test oracles (ROMs we run; results are ground truth):**

| Corpus | License | Use |
| --- | --- | --- |
| [n64-systemtest](https://github.com/lemmy-64/n64-systemtest) | MIT | CPU, COP0, TLB, COP1, RSP functional and timing tests. First CI gate. |
| PeterLemon (krom) N64 tests | Unlicense | \~200 small ROMs covering CPU, RSP, RDP primitives, VI. |
| Dillon's n64-tests | none | Basic CPU tests; run externally, not vendored. |
| Our own probe ROMs | MIT | Timing probes measured on real hardware. See the Unknowns section. |

**Secondary references (read for understanding only):**

- [ares](https://github.com/ares-emulator/ares) — Near's architecture is the lineage; its N64 core is the best public LLE reference. Study its scheduler and register semantics; never its code paths.
- Angrylion-RDP-Plus — the bit-exact RDP behavior reference. Use as a black-box oracle (render a command stream, compare pixels). No license file; do not vendor.
- [paraLLEl-RDP](https://github.com/simple64/parallel-rdp-standalone) — written from scratch against Angrylion as an oracle; its test-suite design (an RDP command-stream generator plus pixel diffs) is the pattern we copy for our own.
- CEN64 — the prior cycle-accuracy attempt; study its RDRAM and RSP timing notes and where it stalled.
- [RustyN64](https://github.com/doublegate/RustyN64) — a 2026 clean-room Rust LLE emulator built with an AI pair in phases against the same oracles. Its ADR-driven process, oracle-first CI gates and commercial-ROM ledger are worth copying wholesale; its architecture is single-threaded and not timing-focused.

## Hardware model

The emulator models three clock domains and one shared memory bus. Everything hard about N64 timing comes from the bus.

| Domain | Clock | Contents | Timing confidence |
| --- | --- | --- | --- |
| CPU | 93.75 MHz (NTSC) | VR4300: 5-stage pipeline, 16 KB I-cache, 8 KB D-cache, 32-entry TLB, COP0, FPU | High: NEC manual plus n64-systemtest |
| RCP | 62.5 MHz | RSP (scalar + 8-lane vector, 4 KB IMEM, 4 KB DMEM, DMA), RDP (fixed-function rasterizer, 4 KB TMEM), MI, VI, AI, PI, SI, RI | RSP high; RDP pipeline medium; RI/arbiter low |
| Bus | 250 MHz RDRAM (500 MB/s nominal) | 4 MB base RDRAM, 4 MB Expansion Pak, 9th-bit coverage/alpha plane | Low: protocol known, arbitration not measured |

**Master clock.** The scheduler runs on a 64-bit tick counter at the RCP clock. CPU cycles are 2:3 against it (3 CPU cycles per 2 RCP cycles); the emulator tracks the CPU in RCP ticks with a fractional accumulator so no drift accumulates. RDRAM bus cycles are 4:1 against RCP.

**What must be cycle-exact in v1 (measurable now):**

- VR4300 instruction retirement, including load interlocks, multiply/divide latency, FPU latency, cache hit/miss and refill timing, TLB refill exception timing, branch delay and branch-likely behavior.
- RSP instruction retirement: 1 cycle per instruction nominal; write-latency stalls on scalar and vector registers; dual-issue of one SU and one VU instruction per cycle, with the known exclusion cases; branch bubble; the load-then-store stall.
- RSP DMA duration and the DMA-busy/semaphore handshake visible to both CPU and RSP.
- PI DMA duration as a function of the cartridge domain timing registers (latency, pulse width, page size, release).
- SI/PIF transaction timing (64-byte RAM exchange, controller polling).
- AI DMA cadence and the audio FIFO's two-slot behavior.
- VI: scanline counter, half-line interrupt, vsync, field toggle, the relationship of VI timing to RDRAM refresh.
- MI interrupt assertion and the CPU's observed interrupt latency.

**What is modeled explicitly but with stated confidence (see Unknowns):**

- RDRAM arbitration among CPU, RSP DMA, RDP, VI, AI and PI requestors, including row-buffer open/close latency and refresh stalls.
- RDP internal pipeline: command fetch from DMEM or RDRAM, span setup, per-pixel throughput as a function of cycle type (1-cycle, 2-cycle, copy, fill), texture-fetch and framebuffer read-modify-write costs, and the `DP_STATUS` busy/pipe/command-buffer flags over time.

**Everything the software can observe must be modeled.** That includes the 9th RDRAM bit, unaligned and cached-vs-uncached access rules, the CPU's write buffer, open-bus and unmapped-register reads, PIF boot (CIC checksum handling, seed values), and register side effects on write. The IPL3 bootcode runs unmodified from the ROM, as on hardware; there is no HLE boot.

## Threading architecture

One thread owns time; every other thread does work whose cost that thread has already charged. That is the only multi-core design that stays deterministic at a 16 ns cycle, because cross-core synchronization costs 50 to 150 ns and lockstepping chips across cores is slower than a single thread.

&#91;embedded content: thread model · one timing thread, one worker pool\]

The lower arrow is the fence: when the timing thread must observe RDP output, it waits for the pool to reach that timestamp.

**Timing thread (one core, the only source of truth).** Runs the scheduler and everything whose timing is coupled through the RDRAM bus: VR4300, RSP, RI/RDRAM arbiter, all DMA engines, MI and interrupt delivery, and the *timing* half of the RDP. The RDP timing half parses each command, computes its cycle cost from the pipeline model, advances `DP_STATUS` and the command buffer state, and charges the arbiter for its memory traffic. It never touches a pixel.

**RDP worker pool (N cores).** Receives the same command stream, already timestamped, and rasterizes it into RDRAM. Workers own the pixel pipeline, TMEM, the combiner, blender and coverage logic. They can run arbitrarily far behind the timing thread. Determinism holds because command order is fixed and each command's output depends only on prior commands and RDRAM contents at that point in the stream.

**Fences.** Whenever the timing thread would observe RDP output, it waits for the workers to catch up to that timestamp. Three events trigger a fence: a CPU or RSP DMA read of an address range the RDP has pending writes to (tracked with a per-64-byte dirty map of outstanding RDP writes); a VI scanout of a framebuffer line with pending writes; a `DP_STATUS` read where the full-sync behavior matters. Fences are the correctness mechanism. Their frequency is the performance cost; a title that reads back its framebuffer every frame fences every frame, which still parallelizes the frame's rendering.

**RDP timing feedback.** The pipeline cost model is a function of the command and the state the timing thread already has (cycle type, texture format, framebuffer format, span width, Z and coverage flags). It does not need pixel results. Where real hardware timing depends on data (a rejected pixel costs the same as an accepted one in the span pipeline, which is why this works), the cost model is validated against hardware traces and tightened in the Unknowns section.

**Other threads.**

- **VI/present:** copies a completed frame from RDRAM to the host framebuffer and presents it. Runs on the VI's own schedule; fenced per frame.
- **Audio:** drains the AI's completed DMA buffers into the host audio API. The AI's DMA timing stays on the timing thread; only the host output is off-thread.
- **Input/PIF host side:** polls host controllers and feeds the PIF model; PIF timing stays on the timing thread.
- **Trace writer:** serializes bus transaction logs for test comparison without stalling the timing thread.

**Scheduler design.** A single ordered event queue keyed on RCP ticks. Each unit on the timing thread is stepped by the scheduler in strict tick order; the scheduler never lets a unit run ahead of the earliest pending bus transaction. Units that are idle (CPU stalled on a bus access, RSP halted, DMA idle) are skipped. This is a timestamp model, not cothreads: no context switches, no per-chip stacks, trivially serializable for save states.

**Determinism guarantee, enforced in CI.** Every build runs the trace corpus with 1 worker and with N workers and diffs the bus-transaction logs. Any difference fails the build.

**Configurations.** `ultra --threads=1` runs the whole machine on one thread for debugging and as the reference. `--threads=N` uses the worker pool. There is no configuration that trades accuracy for speed; speed comes only from parallelism.

## Module requirements

Each module is a library with a narrow interface to the scheduler and the bus, its own unit tests, and a `docs/hardware/<module>.md` that records every hardware fact the implementation relies on with its source.

### Bus and scheduler (`core/`)

- `Scheduler`: 64-bit tick clock, intrusive event queue, `schedule(tick, callback)`, `run_until(tick)`. No heap allocation on the hot path.
- `Bus`: physical address decode for the full 32-bit map; routes to RDRAM, RCP registers, SP memory, PI domains, SI/PIF, open bus. Every access returns `(value, completion_tick)`; nothing on the bus is instantaneous.
- `TraceSink`: optional per-transaction log (tick, master, address, size, read/write, data) in a fixed binary record. This is the primary test artifact.
- `SaveState`: every module serializes through one visitor; states round-trip bit-exactly and are versioned.

### VR4300 (`cpu/`)

- Full MIPS III ISA in 32-bit and 64-bit modes, all addressing modes, all COP0 registers with correct read/write masks, all exceptions with correct priority and EPC/BadVAddr/Context behavior, TLB with 32 entries, page sizes 4 KB to 16 MB, and the known VR4300 TLB quirks (including the shutdown on overlapping entries).
- FPU: all COP1 operations including the rounding modes, denormal handling flags, `FS` bit, and the VR4300's non-IEEE edge behaviors documented in the manual and n64-systemtest.
- Pipeline model: five stages (IC, RF, EX, DC, WB) with the manual's interlock table. Load-use interlock, multiply and divide latencies (per-opcode table), FPU latencies, branch delay and branch-likely squash, exception timing at the stage the exception is detected.
- Caches: 16 KB direct-mapped I-cache (32-byte lines), 8 KB direct-mapped D-cache (16-byte lines), write-back with dirty bit, all `CACHE` op variants, refill timing driven by actual bus transactions through the arbiter. Uncached accesses stall the pipeline for the full bus round trip.
- Write buffer semantics as observed by software.
- No recompiler. Interpreter with a precomputed decode table; decoding is a 64K-entry switch on opcode bits, not a chain of ifs.
- `COUNT` register increments at half the CPU clock, and `COMPARE` interrupt timing is exact.

### RSP (`rsp/`)

- Scalar unit: the MIPS subset the RSP implements, with unmapped opcodes behaving as hardware does. 4 KB IMEM with wrap-around fetch, 4 KB DMEM with the unaligned and wrap-around load/store rules.
- Vector unit: all COP2 instructions, the 48-bit accumulator per lane, the VCC/VCO/VCE flag registers, the reciprocal and square-root tables (generated from the documented algorithm, not copied), element selectors, and the load/store family with their exact DMEM byte semantics.
- Pipeline model: one instruction per cycle nominal; dual issue of one SU and one VU instruction with the documented exclusion rules; write-latency stalls; branch bubble; the store-after-load stall. The model is validated against the n64brew pipeline page and our own probes, not against another emulator.
- DMA: SP to and from RDRAM with the length/skip/count semantics, the DMA-busy and DMA-full status bits, the semaphore, and transfer durations charged through the arbiter.
- Status register, halt/break/interrupt handshake with MI, single-step, and the CPU-visible IMEM/DMEM window with its access rules while the RSP runs.
- Implementation note for Claude Code: implement the vector unit with portable scalar code first, behind a `VectorOps` interface, then add an SSE4.1/AVX2 implementation gated behind the same tests. Both must pass bit-exact.

### RDP (`rdp/`), split into timing and raster halves

- Command decoder: all 64-bit commands, the XBUS (DMEM) and RDRAM command sources, `DP_START`/`DP_END`/`DP_CURRENT` register behavior while commands stream, the command buffer FIFO depth and its `DP_STATUS` flags.
- Timing half: a cost model per command, parameterized by cycle type (1-cycle, 2-cycle, copy, fill), span width and count, texture tile format and size, framebuffer format, Z-buffer read/write enables, and coverage mode. Outputs ticks and arbiter traffic. Implemented as a separate class so it can be replaced without touching rasterization.
- Raster half: span walking with the hardware's edge-stepping precision, texture pipeline (TMEM load, tile descriptors, all formats, 3-point and copy filtering, LOD, mirroring/clamping), color combiner (both cycles), blender (both cycles, all modes), Z compare and update with the hardware's compressed Z format and delta-Z, coverage and anti-aliasing, dithering (RGB and alpha, all modes), fill and copy modes, and the 9th-bit coverage write. Output must be bit-exact against the Angrylion oracle on the full conformance suite.
- Full sync and the `DP_INTR` to MI.
- Worker pool: command stream distributed in order; parallelism within a command (span-level or tile-level) is allowed as long as results are identical.

### RDRAM and RI (`memory/`)

- 4 MB plus 4 MB expansion, 9 bits per byte. The expansion pak is detected the way IPL3 detects it.
- RI registers with the documented semantics, including the init sequence IPL3 performs, the refresh register and its coupling to VI HSYNC, and the error flags.
- Arbiter: the pluggable timing model. v1 ships a documented Base RDRAM protocol model (row open/close, packet overhead, refresh windows) with a fixed-priority requestor order; every constant is named, and the model's confidence level is recorded in the Unknowns section.

### Interfaces (`io/`)

- **MI:** interrupt mask and pending registers, version register, `MI_MODE` including the repeat-count and EBUS test modes, with exact assertion of the CPU's `IP2` line.
- **PI:** DMA to and from cartridge domains 1 and 2 with per-domain latency, pulse width, page size and release registers driving transfer duration; the DMA-busy and IO-busy flags; unaligned DMA behavior; the error bit. Cartridge model: ROM, SRAM, FlashRAM (with the command state machine), EEPROM via SI, and an interface for an ISViewer-style debug port.
- **SI:** 64-byte DMA to and from PIF RAM, the SI busy and interrupt flags, and transaction duration.
- **PIF:** boot sequence (CIC handshake for all retail CIC variants, seed and checksum behavior), the PIF RAM command protocol, Joybus for four controllers plus EEPROM, controller accessories (Controller Pak, Rumble Pak, Transfer Pak with a Game Boy cartridge model sufficient for the Stadium titles), and the reset and NMI paths.
- **AI:** DMA address and length registers, the two-slot FIFO, DAC rate and bit-rate registers, interrupt on buffer consumption, and sample output at the exact rate the registers define.
- **VI:** every register, the scanline counter and `VI_V_CURRENT`, the half-line interrupt, field alternation for interlaced modes, horizontal and vertical scaling and the subpixel filtering (AA modes, gamma, dither filter, divot) applied at scanout, and NTSC/PAL/MPAL timing. The host sees exactly what the VI would send to the DAC.

### Frontend (`app/`)

- A plain SDL3 window with integer scaling, host audio via SDL, controller mapping, save-state hotkeys, and a command-line interface for headless runs. Nothing else. The frontend is deliberately thin so that it never influences the core.

## Unknowns and the hardware-measurement plan

Two parts of the machine have no complete public timing description. The spec handles them by isolating each behind an interface, shipping a documented best model, and defining the measurements that replace it. Claude Code builds the probes and the comparison tooling; the measurements themselves need the hardware below.

| Unknown | What we know | v1 model | What closes it |
| --- | --- | --- | --- |
| RDRAM arbitration | Base RDRAM protocol, packet format, row/bank structure, refresh tied to VI HSYNC, RI register semantics | Fixed-priority arbiter with protocol-derived packet and row-miss costs; named constants in `memory/arbiter_params.h` | CPU-side probes (below) first; bus logic-analyzer captures for the residual |
| RDP pipeline timing | Command semantics, cycle types, the fact that the span pipeline is fixed-function | Per-command cost table derived from first-principles pipeline stages; confidence flagged per entry | `DP_CLOCK` and `COUNT` differential probes per primitive class; then bus captures |
| Interaction of the two | RDP memory traffic stalls CPU/RSP and vice versa | Arbiter charges RDP traffic from the cost table | Probes that run CPU memory loops concurrently with controlled RDP loads |

**Interfaces that isolate the unknowns:**

- `IArbiter::request(master, address, size, kind, tick) -> completion_tick`. The arbiter is the only place bus latency is decided. Swapping it changes timing, never results.
- `IRdpCost::cost(const Command&, const RdpState&) -> {ticks, traffic[]}`. The RDP timing half calls this and nothing else.
- Both carry a `confidence()` enum (`Measured`, `Derived`, `Guess`) per parameter, surfaced in `ultra --timing-report`.

**Probe ROMs (`tests/probes/`, built with libdragon, MIT).** Each probe runs a controlled workload, measures with `COUNT` and `DP_CLOCK`, and prints results over the ISViewer/UNFLoader debug channel and to the screen. The same ROM runs in the emulator; the harness diffs the numbers. All six probes, plus functional sweeps and the cold-boot capture, are combined in one ROM, `tests/probes/hwprobe` (docs/timing/hwprobe.md), the project's gold-standard test ROM.

1. **CPU retirement:** straight-line instruction sequences per opcode class, cached and uncached, measuring `COUNT` deltas. Establishes the pipeline table. Mostly a cross-check of n64-systemtest, which already covers much of this.
2. **Cache and TLB:** refill timing for I and D cache, write-back cost, TLB refill exception latency.
3. **RSP retirement:** microcode sequences exercising dual-issue, each stall rule, and DMA durations, timed with the RSP's own loop counts and the CPU's `COUNT`.
4. **RDRAM access patterns:** uncached CPU reads and writes with varying stride and size, with the RSP DMA engine idle, then with RSP DMA running, then with the RDP filling. Establishes arbitration priority and row-miss cost as seen from the CPU.
5. **RDP primitive costs:** for each cycle type and a matrix of texture/framebuffer/Z configurations, draw N identical primitives and measure `DP_CLOCK` and wall `COUNT` across a full sync. Establishes the cost table.
6. **VI and refresh coupling:** change VI HSYNC and measure CPU memory throughput.

**Hardware you need:**

- An NTSC N64 (a second PAL unit later for PAL timing).
- An SC64 or EverDrive 64 X7 flashcart with a USB debug channel. SC64 is preferred: open hardware, UNFLoader support, and its own SD card for logging.
- For the residual after probes 4 and 5: a logic analyzer able to capture the RDRAM bus (250 MHz, 9-bit data, \~16 lines including control). A DSLogic U3Pro32 or a Saleae Logic Pro 16 is enough to capture command and strobe lines at the needed rate; data lines need the DSLogic's 1 GS/s on a reduced channel set. This is a soldering job onto the RDRAM module pads; it is the one step with real risk to the console, so it is scheduled last and only if the probe results leave gaps that matter for titles.

**What "done" means for each unknown:** every parameter in the two interfaces is `Measured` or `Derived` with a probe ROM that reproduces the measurement, and the emulator's result on that probe matches hardware within the probe's own noise floor (a few `COUNT` ticks, which the probe reports).

## Repository, build and coding standards

**Layout:**

```
ultra/
  CMakeLists.txt            C++23, CMake 3.28+, Ninja; presets for debug, release, asan, tsan
  CLAUDE.md                 Claude Code's standing instructions (see handoff section)
  docs/
    adr/                    Architecture decision records, numbered, never edited after acceptance
    hardware/               One file per module: every hardware fact relied on, with its source
    timing/                 Measured constants, probe results, confidence ledger
  src/
    core/                   scheduler, bus, trace, savestate
    cpu/                    VR4300
    rsp/                    RSP
    rdp/                    timing/, raster/, workers/
    memory/                 rdram, ri, arbiter
    io/                     mi, pi, si, pif, ai, vi, cart
    app/                    SDL3 frontend, CLI
  tests/
    unit/                   Catch2; per-module, no ROMs
    roms/                   vendored permissive test ROMs with LICENSE files
    probes/                 our libdragon probe ROMs, source and built .z64
    traces/                 hardware-captured and golden trace files
    harness/                runner: boots a ROM headless, compares traces/screens/probe output
  tools/
    tracediff/              bus-trace comparison
    rdpconform/             RDP command-stream generator and Angrylion pixel diff
```

**Toolchain.** Clang 18+ and GCC 14+ both build warning-free with `-Wall -Wextra -Wpedantic -Werror`. Dependencies are fetched with CMake `FetchContent` pinned to commit hashes: SDL3, Catch2, fmt. Nothing else. No Boost, no Qt, no package manager beyond CMake.

**Coding rules (enforced in review and by `clang-tidy` config in the repo):**

1. No virtual dispatch on the hot path. Module interfaces that the scheduler calls per tick are templates or direct calls; `IArbiter` and `IRdpCost` are the only virtual boundaries and are called per transaction, not per tick.
2. No heap allocation after initialization in the timing thread. All buffers are sized at boot.
3. No `std::mutex` on the timing thread. Worker communication uses a single-producer bounded ring with acquire/release atomics; fences are a futex-style wait on a monotonic completion counter.
4. Every hardware constant is a named `constexpr` with a comment citing its source document and page or wiki section, or the probe that measured it.
5. Every register has a struct with explicit bit fields and read/write mask functions; no raw shifting at call sites.
6. Integer types are explicit widths (`u8`, `u16`, `u32`, `u64`, `s16`, etc.). Signed overflow is UB; use the `wrap_` helpers.
7. No floating point in any emulation path except the FPU model, which uses host IEEE operations with explicit rounding-mode control and a conformance test.
8. Save-state serialization is written alongside the state it serializes; a module without a serializer does not merge.
9. A hardware fact learned from another emulator's source is written to `docs/hardware/` with a citation *before* the code that uses it. Review rejects code with facts that have no note.
10. ADRs for: scheduler design, arbiter interface, RDP split, worker protocol, trace format, save-state format, and any later change to them.

**Branching.** Trunk-based. Every merge passes the CI gates in the Verification section. A gate that currently fails is recorded as failing in `docs/timing/ledger.md` with an issue; CI asserts the ledger is exact, so an unexpected pass is also a failure (it means the ledger is stale).

## Verification

The oracle is always hardware or a hardware-derived artifact, never another emulator's output, with one exception: the RDP raster half is checked against Angrylion pixels because Angrylion is itself verified bit-exact against hardware and pixel captures from real consoles are impractical at scale.

**CI gates, in the order they are added:**

| Gate | Oracle | Pass condition | Added in phase |
| --- | --- | --- | --- |
| Unit tests | Hand-derived expectations from the manuals | 100% pass | 0 |
| n64-systemtest CPU/COP0/TLB/COP1 | The ROM's own assertions | `Failed: 0`, and the ROM runs to its exit call (a mid-suite hang is a failure) | 1 |
| krom CPU and RSP ROMs | Screen hash per ROM captured from hardware photos or the ROM's documented expected output | Hash match | 1 |
| n64-systemtest RSP | The ROM's assertions | `Failed: 0` | 2 |
| Probe ROMs 1–3 (CPU, cache, RSP timing) | Numbers measured on hardware, committed under `tests/traces/` | Within the probe's reported noise floor | 2 |
| RDP conformance | Angrylion pixel output on a generated command corpus (10,000+ streams covering the configuration matrix) | Bit-exact framebuffer and 9th-bit plane | 3 |
| Determinism | The emulator itself at `--threads=1` | `--threads=N` bus trace is byte-identical | 3 |
| Probe ROMs 4–6 (RDRAM, RDP cost, VI) | Hardware measurements | Within noise floor; failing entries listed in the ledger with confidence `Guess` or `Derived` | 4 |
| Commercial boot corpus | A local, uncommitted set of ROMs; hashes only in the repo | Boots to a committed screen hash at a fixed frame number; per-title runtime budget | 4 |
| Save-state round trip | The emulator | Save at frame N, load, run to N+1000; trace identical to an uninterrupted run | 4 |

**Trace comparison.** `tools/tracediff` compares two bus-transaction logs and reports the first divergence with surrounding context (the CPU PC, RSP PC and RDP command at that tick). Every accuracy bug in this project is found as a trace divergence first and a visible symptom second.

**Commercial ROMs.** Never committed. The harness reads them from a path outside the repo; CI runs that corpus only on a self-hosted runner. The ledger records, per title, the frame hash, the fence count per frame, and the speed on the reference machine, so performance regressions are caught alongside accuracy ones.

**Independent review step.** At the end of each phase a fresh Claude Code session with no context from the implementation session audits the phase against this spec and the ADRs, reruns every gate from a clean clone, and writes a findings file. The implementation session does not grade its own work.

## Phased milestones

Five phases, each gated by oracles that already exist before the phase starts. Estimates assume Claude Code doing the implementation with you reviewing ADRs and running hardware: Phase 0 about 6 weeks, Phase 1 about 6 weeks, Phase 2 about 10 weeks, Phase 3 depends on flashcart turnaround (plan 8 weeks of calendar time), Phase 4 about 6 weeks. Phases 3 and 4 can overlap. Treat these as order-of-magnitude; the gates, not the dates, decide when a phase ends.

&#91;embedded content: roadmap · 5 phases, each with its gate\]

Phase 2 is the accent because it is where the threading design is proven or disproven; everything after it is refinement.

## Claude Code handoff

Start every phase in a fresh session with this doc exported to `docs/SPEC.md`, the `CLAUDE.md` below at the repo root, and the phase prompt as the first message. Finish every phase with a separate audit session.

**`CLAUDE.md` (verbatim, at the repo root):**

```markdown
# Ultra: cycle-accurate N64 emulator

Read docs/SPEC.md before any work. It is the contract; this file is the working rules.

## Non-negotiables
- Clean room. You may read other emulators to learn what hardware does. You may not copy,
  transliterate or structurally mirror their code. Any fact learned from another emulator's
  source goes into docs/hardware/<module>.md with a citation BEFORE you write code using it.
- No HLE of any kind. No game-specific hacks. No accuracy/speed toggles.
- Determinism: --threads=1 and --threads=N must produce byte-identical bus traces.
- Every hardware constant is a named constexpr with a source citation in a comment.
- Every module ships with its serializer, its unit tests, and its docs/hardware/ note.
- Oracles are hardware, test ROMs, or hardware-derived data. Never another emulator's output,
  except Angrylion pixels for the RDP raster half.

## Workflow
- Trunk-based. One feature per branch. CI gates in docs/SPEC.md must stay green; a known
  failure is recorded in docs/timing/ledger.md, and CI asserts the ledger is exact.
- Write an ADR in docs/adr/ before changing the scheduler, bus, arbiter interface, RDP split,
  worker protocol, trace format or save-state format.
- When a test ROM fails: find the first bus-trace divergence with tools/tracediff, fix the
  root cause, add a unit test that would have caught it. Never special-case a test.
- When a hardware fact is unknown: say so in the ledger with confidence Guess, implement the
  best-documented model behind the existing interface, and open an issue naming the probe
  that would measure it. Do not invent a number and move on.
- Build: cmake --preset release && cmake --build --preset release. Test: ctest --preset release.
  Also run the asan and tsan presets before any merge touching threads.

## Style
- C++23, Clang 18+ and GCC 14+, -Werror. Explicit-width integer types. No virtual dispatch
  on the per-tick path. No allocation or mutexes on the timing thread after init.
- Registers are structs with bit fields and mask functions. No raw shifts at call sites.
```

**Phase prompts.** Each is the first message of a new session.

*Phase 0:*

> Build Phase 0 of Ultra per docs/SPEC.md. Deliver: the CMake project with debug, release, asan and tsan presets; `core/` (scheduler, bus, trace sink, save-state visitor) with an ADR each for the scheduler design and the trace format; `cpu/` as a complete VR4300 interpreter with the five-stage pipeline model, caches and TLB; `io/` with MI, PI (ROM only) and the PIF boot path for all retail CICs; `memory/` with RDRAM and a placeholder arbiter that returns protocol-derived fixed latencies, marked `Guess` in the ledger. Vendor n64-systemtest and the krom CPU ROMs under `tests/roms/` with licenses. Gate: n64-systemtest CPU/COP0/TLB/COP1 reports `Failed: 0` and runs to its exit call; the krom CPU ROMs match their documented output. Write `docs/hardware/cpu.md` as you go. Do not start the RSP.

*Phase 1:*

> Build Phase 1 per docs/SPEC.md on the Phase 0 tree. Deliver: `rsp/` with scalar and vector units, the pipeline model from the n64brew RSP pipeline page, SP DMA and the status/semaphore handshake, a portable `VectorOps` plus an SSE4.1 implementation both passing the same tests; SI and PIF controller polling; AI with the two-slot FIFO; VI with exact scanline timing. Build probe ROMs 1 to 3 under `tests/probes/` with libdragon and the harness that diffs their output against committed hardware results (I will supply the hardware numbers; until then the gate runs against placeholders marked `Guess`). Gate: n64-systemtest RSP `Failed: 0`; krom RSP ROMs match.

*Phase 2:*

> Build Phase 2 per docs/SPEC.md. Write the ADRs for the RDP timing/raster split and the worker protocol first and stop for my review before implementing. Then deliver: `rdp/raster/` bit-exact against Angrylion on `tools/rdpconform`, which you also build (a command-stream generator over the full configuration matrix, 10,000+ streams, pixel and 9th-bit diff); `rdp/timing/` with the first cost table, every entry marked `Derived` or `Guess`; the worker pool, the dirty map, the three fence triggers; the determinism CI gate. Gate: conformance bit-exact; `--threads=1` and `--threads=8` traces identical over the full test corpus; at least five commercial titles boot to a stable frame (local corpus, hashes only committed).

*Phase 3:*

> Phase 3 per docs/SPEC.md. Build probe ROMs 4 to 6. For each, write the expected-result format, the harness comparison, and a `docs/timing/` page explaining what the probe measures and how to read its output. I will run them on hardware and commit results. After each result lands, fit the arbiter and cost-table parameters, move each from `Guess` to `Measured` or `Derived` in the ledger with the fit documented, and rerun the full corpus. Stop and report after each probe's fit rather than batching.

*Phase 4:*

> Phase 4 per docs/SPEC.md. Deliver the SDL3 frontend (window, integer scaling, audio, controller mapping, save-state hotkeys, headless CLI), Controller Pak, Rumble Pak, Transfer Pak with a Game Boy cartridge model sufficient for Pokemon Stadium, SRAM/FlashRAM/EEPROM saves, and the commercial-corpus harness with per-title frame hash, fence count and speed. Gate: 50 titles boot with stable hashes; 2D titles run at full speed on the reference machine; the ledger lists every title below full speed with its fence count.

*Audit prompt (after every phase, in a fresh session):*

> You are auditing Phase N of Ultra against docs/SPEC.md and docs/adr/. You did not write this code. Clone clean, build all four presets, run every CI gate, and verify the ledger is exact. Then review: every hardware constant has a citation; every fact in docs/hardware/ has a source; no code structurally mirrors another emulator (spot-check against ares and Angrylion for the module in question); the threading rules in CLAUDE.md hold (grep for mutexes and allocations in the timing-thread paths); save states round-trip. Write `docs/audit/phase-N.md` with findings ranked by severity and a yes/no on whether the gate is honestly met. Do not fix anything.

**What you do between phases:** review and accept each ADR; run the probe ROMs on hardware and commit the results; read each audit; decide whether a `Guess` is acceptable for the next phase or blocks it. Those are the only decisions the spec leaves to a human, and they are the ones that matter.
