# Ultra: a cycle-accurate Nintendo 64

**Started October 17, 2026.** Phase 0 is complete: the VR4300 CPU, the core (scheduler, bus,
trace, save states), the boot path through the PIF for every retail CIC, RDRAM and the MI/PI
interfaces run every CPU test ROM in the gate. The RSP, RDP and the rest of the RCP follow in
later phases. The block below reports only what the gate measures.

**Maintainer:** Jeffrey Wilbur, Strut B, LLC\
**Contact:** [retro@strutb.com](mailto:retro@strutb.com)

## The Accuracy Standard

The purpose of a cycle-accurate emulator, as opposed to a fast one, is preservation: recording
how the machine itself behaves, in a form that outlives the hardware. Running games is the
consequence, not the goal. Accuracy is only claimed where an oracle confirms it: the hardware,
a test ROM, or data measured on hardware. Another emulator's output is never an oracle (the one
exception, Angrylion's pixels for the RDP raster, arrives in Phase 2). Every timing parameter
that no oracle confirms yet is written down as a `Guess`, with the probe that will measure it.

## Project Purpose

An emulator of the **Nintendo 64** (1996) in which every component runs on the clock it has on
the console, every bus transaction takes the time it takes on hardware, and every fact the
code relies on has a source in `docs/hardware/`. It is clean room: other emulators may be read
to learn what the hardware does, never copied, transliterated or mirrored. It does no
high-level emulation and has no per-game fixes and no speed/accuracy switches. It is
deterministic: the same input gives the same bus trace on every host, compiler and thread
count. The full contract is [docs/SPEC.md](docs/SPEC.md); the working rules are in
[CLAUDE.md](CLAUDE.md).

## Hardware background

The console is a NEC VR4300 CPU (a MIPS R4300i at 93.75 MHz) beside Nintendo's RCP, which
contains the RSP (a MIPS core with a vector unit that runs microcode), the RDP (the
rasteriser) and the interfaces to memory and the outside world: MI, VI, AI, PI, SI and RI. All
of them share one Rambus RDRAM channel, and how they contend for it decides how fast a game
runs. Every clock derives from the RCP's 62.5 MHz master clock: the CPU at 3/2 of it, RDRAM at
four times, video and audio at a rational multiple per TV standard. The PIF, a separate
microcontroller, boots the console and checks the cartridge's CIC lockout chip.

## Oracles, not opinions

Each claim in this tree is backed by one of three kinds of evidence, and the ledger says which:

- **Measured.** A test ROM measured it on hardware and the emulator reproduces the measurement.
- **Derived.** Computed from a primary source: the VR4300 manual, n64brew's hardware
  measurements, a datasheet.
- **Guess.** A placeholder. The ledger names the probe ROM that will measure it.

Where a test's reference output itself is wrong (a screenshot captured on a flashcart, a
capture artefact), the ledger records the failure with the evidence that it is the reference
and not the emulator, and the gate keeps it as an expected failure.

## hwprobe: the gold-standard test ROM

`tests/probes/hwprobe/` is the project's own console ROM and its highest authority. It is
run once on a real N64 and records, in a single dump, everything the emulator has to match:

- **Timing:** per-instruction retirement for 59 classes, pipeline interlocks and overlap,
  exceptions and interrupt acceptance, cache and TLB refill, every RCP register's access
  time, RDRAM access patterns and contention with the RSP and RDP running, PI, SI, AI and SP
  DMA, RDP command costs, RSP microcode timing, and the uncached-read loops behind the open
  `cpu-timing-residual`.
- **Behaviour:** every CPU instruction encoding (reserved ones included), the FPU over edge
  operands in every rounding mode, integer edge cases, unaligned and LL/SC access, every RSP
  vector instruction with its accumulator and flags plus the full reciprocal tables, a matrix
  of RDP renders dumped pixel by pixel, Joybus/PIF responses and the controller paks
  (read-only), and the soft-reset path.
- **Power-on state:** the CPU and RCP state at the first instruction of boot code
  (`hwprobe-cold.z64`, our own IPL3, SC64 direct boot), and the TLB and CP0 state the program
  starts with.

It needs no controller, and RESET is the only control. It writes an unlimited full record to
the flashcart's SD card (about 1.7 MB) and a compact copy to FlashRAM. The same ROM runs
headless in Ultra (`ultra_harness hwprobe`, `hwprobe-full`), and `tools/hwprobe.py compare`
diffs the console's record against the emulator's, series by series. The console's record is
the oracle; the emulator's never is. Console records live in
[tests/traces/hwprobe/](tests/traces/hwprobe/). A new hardware unknown gets a series in this
ROM before a number in the code.

```bash
sc64deployer upload --direct --save-type flashram tests/probes/hwprobe/hwprobe-cold.z64
build/release/tests/ultra_harness hwprobe-full tests/probes/hwprobe/hwprobe.z64 emu.bin
python3 tools/hwprobe.py compare console-stage1.bin emu.bin
```

Specification, groups and record format: [docs/timing/hwprobe.md](docs/timing/hwprobe.md).
Building it: [tests/probes/README.md](tests/probes/README.md).

## Current Status

The block below is generated: regenerate it with `python3 tools/progress.py` (it runs the full
gate, about two minutes). Do not hand-edit it.

<!-- STATUS:BEGIN: generated by tools/progress.py; do not edit by hand -->
_Snapshot 2026-10-19._

```
n64-systemtest  CPU, COP0, TLB, COP1 (Phase 0 scope; base, timing, cycle, CP0 hazards)
    ████████████████████████████████████████  100.0%   3,417 / 3,417 tests pass
krom CPUTest   framebuffer identical to the hardware screenshot
    ████████████████████████████████░░░░░░░░   81.2%   91 / 112 ROMs
krom timing    loop counts vs hardware (CPU and CP1 TIMINGNTSC)
    2 / 2 within 2%, worst 1.72%; 0 / 2 within the hardware noise floor
```

**Gate: green.** 117 manifest entries, 0 not as expected. The 21 screens that differ and the exact timing entries are recorded as expected failures in the [ledger](docs/timing/ledger.md) with the evidence for each (8 entries); an unexpected pass fails the gate too.

| Timing and behaviour parameters (ledger) | Count |
|---|---|
| Measured | 1 |
| Derived | 4 |
| Guess | 31 |
| Open | 1 |
| Design | 1 |
<!-- STATUS:END -->

| Phase | Scope | State |
|---|---|---|
| 0 | CPU, core, MI, PI, PIF boot, RDRAM, placeholder arbiter | gate green |
| 1 | RSP (scalar and vector), SI and PIF polling, AI, VI scanline timing, probe ROMs 1-3 | not started |
| 2 | RDP raster (bit-exact) and timing, worker threads, determinism gate | not started |
| 3 | Probe ROMs 4-6 on hardware; fit the arbiter and RDP costs | not started |
| 4 | Commercial corpus | not started |

## More

- [docs/SPEC.md](docs/SPEC.md): the contract, the phases and their gates.
- [docs/timing/ledger.md](docs/timing/ledger.md): every parameter's confidence and every
  expected test failure, with the evidence.
- [docs/hardware/](docs/hardware/): what the hardware does, one page per module, every fact
  cited.
- [docs/adr/](docs/adr/): architecture decisions (scheduler, trace format, save states,
  arbiter).

Query the tree. Do not trust a number in this file.

```bash
ctest --test-dir build/release --output-on-failure            # unit tests and the ROM gate
build/release/tests/ultra_harness manifest tests/roms/manifest.txt --rom-dir tests/roms
build/release/tests/ultra_harness systemtest tests/roms/n64-systemtest/n64-systemtest-cpu.z64
build/release/tests/ultra_harness screen ROM.N64 REFERENCE.png --dump out.ppm
build/release/src/ultra --timing-report ROM                 # arbiter parameters and confidence
build/release/src/ultra --trace run.trc ROM                 # bus-transaction trace
build/release/tools/tracediff a.trc b.trc                     # first divergence between traces
```

### How the gate is verified

The gate is `tests/roms/manifest.txt`, run by `ultra_harness` (the `rom-manifest` ctest).
Each entry states its expected outcome, and any other outcome fails the gate, including an
unexpected pass, so the manifest and the ledger cannot drift apart.

- **systemtest:** boots n64-systemtest through the PIF and its IPL3 and passes when the ROM
  prints `Failed 0` and reaches its end.
- **screen:** runs a krom ROM and compares the framebuffer, presented the way the reference
  screenshots were captured, with the PNG pixel for pixel.
- **kromcount:** reads back the loop counts the krom timing ROMs print, using the ROM's own
  font, and compares each with the hardware count in the ROM's source within a stated tolerance.

The unit tests (`tests/unit/`) boot the machine and run assembled programs against the CPU:
branch delay slots, exceptions, TLB mapping, FPU early completion, SysAD stalls, plus
save-state round trips, determinism of two runs, PI DMA and VI field timing.

### Keeping it that way

- Every hardware constant is a named `constexpr` with its source in a comment; a value without a
  primary source names its ledger entry.
- A failing test ROM is debugged from its first bus-trace divergence (`tracediff`) to the root
  cause, and the fix comes with a unit test that would have caught it. No test is special-cased.
- Changes to the scheduler, bus, arbiter interface, trace or save-state format need an ADR
  first.
- Every module ships its serializer, unit tests and `docs/hardware/` page.
- All four presets build warning-free with `-Wall -Wextra -Wpedantic -Werror`, on Clang and GCC;
  asan and tsan run before any change that touches threads. Bus traces are byte-identical
  between runs and between Clang and GCC builds.

### Architecture

    src/core/        clock domains, scheduler, bus decode, trace sink, save states, System
    src/cpu/         VR4300: interpreter, pipeline stall model, caches, TLB, COP0, COP1
    src/memory/      RDRAM devices, RI, the arbiter interface and its v1 implementation
    src/io/          MI, PI, SI, PIF and its clean-room boot ROM, CIC, cartridge, VI, AI
    src/rsp/         SP interface (DMEM/IMEM, DMA, semaphore); the RSP core is Phase 1
    src/rdp/         DP interface registers; the RDP is Phase 2
    src/common/      fixed-width types, register bit fields, a MIPS assembler for tests and boot
    src/app/         the `ultra` command-line runner
    tests/unit/      Catch2 tests
    tests/harness/   the ROM gate runner (manifest, systemtest, screen, kromcount, hwprobe)
    tests/roms/      vendored test ROMs, licences, the manifest
    tests/probes/    hwprobe, the gold-standard console ROM (source and built ROMs)
    tests/traces/    console records from hwprobe: the hardware oracle
    tools/           tracediff, progress.py, hwprobe.py (record decoder and comparer)
    docs/            spec, ADRs, hardware notes, timing ledger
    third_party/     reference mirrors (n64brew, VR4300 manual); not tracked
    build/           everything generated

The CPU drives time: it steps one instruction, charges the pipeline stalls the hardware would
take, and every bus access first brings the devices up to that tick (ADR 0001). Device events
sit in one ordered queue, so the machine is a pure function of its inputs.

### Building & Setup

Requirements: CMake 3.28+, Ninja, Clang 18+ or GCC 14+. Dependencies (fmt, Catch2, and SDL3
for the later front end) are fetched at pinned commits by CMake.

```bash
cmake --preset release && cmake --build --preset release
ctest --test-dir build/release
```

Presets: `debug`, `release`, `asan` (address and undefined-behaviour sanitizers), `tsan`, and
`gcc-release` (GCC 16). Phase 0 is developed and gated on macOS arm64; the spec's reference
host is Linux x86-64.

### Test ROMs (vendored; see [tests/roms/README.md](tests/roms/README.md))

```
tests/roms/n64-systemtest/   lemmy-64/n64-systemtest @196f542, MIT
    n64-systemtest-cpu.z64   Phase 0 gate (RSP and RDP groups removed by phase0-cpu-scope.patch;
                             Timing, Cycle and CP0-hazards groups built in)
    n64-systemtest-full.z64  unmodified, for Phase 1
tests/roms/krom/CPUTest/     PeterLemon/N64 @7085543, Unlicense, with reference screenshots
```

The krom ROMs embed Nintendo's 6102 IPL3, which their licence cannot cover; that must be
resolved before this directory is published. Nintendo's PIF boot ROM is never needed: the
emulator boots through its own clean-room IPL1/IPL2 ([docs/hardware/pif.md](docs/hardware/pif.md)).

Commercial ROMs never enter the repository (`corpus/` is ignored); only their hashes will.

## Licence

Copyright (C) 2026 Jeffrey Wilbur. Ultra is licensed under the GNU Affero General Public
License, version 3 ([LICENSE](LICENSE)). Anyone who distributes Ultra or a work based on it,
or lets users interact with a modified version over a network, must keep the copyright and
[NOTICE](NOTICE) and publish the complete source under the same licence. Third-party test
ROMs keep their own licences ([tests/roms/README.md](tests/roms/README.md)).
