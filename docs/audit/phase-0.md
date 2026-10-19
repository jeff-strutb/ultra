# Phase 0 audit

Audited 2026-10-17 against `docs/SPEC.md`, `docs/adr/0001`–`0004` and `CLAUDE.md`, at commit
`91b00bc` (main). The auditor did not write this code and fixed nothing. Host: macOS arm64
(an approved deviation from the spec's Linux x86-64), Apple clang 21.0.0 and Homebrew GCC 16.1.0.

## Verdict

**Gate honestly met: No.**

- **First half of the gate: met.** n64-systemtest CPU/COP0/TLB/COP1 reports
  `Base: Failed 0 of 3408 tests` and runs to its summary.
- **Second half: not met.** The gate says "the krom CPU ROMs match their documented output",
  and they do not:
  - 17 of the 114 vendored krom CPU ROMs (every ROM under `CPUTest/Exceptions/`) are missing
    from the manifest and are recorded nowhere. All 17 fail against their reference
    screenshots (F2).
  - Of the 97 ROMs in the manifest, 5 fail and are recorded as expected failures. Their
    explanations hold up (one checked pixel by pixel). The timing ROMs pass only at a 2%
    tolerance the project chose itself (F5).
- **A clean-room non-negotiable is broken.** `src/io/cic.cpp` transliterates two
  third-party C sources, and the hardware notes say no code was copied (F1).

The CPU core itself is in good shape:

- All five presets build warning-free.
- Every gate as written passes in release, gcc-release, debug and asan. Under tsan the ROM
  gate only times out (F11).
- Bus traces are byte-identical between the clang and GCC builds.
- Save states round-trip exactly, compared by bus trace.

The "No" is about the gate's scope being quietly narrowed and about F1, not about the CPU
results.

## What was run

All runs used the existing `build/<preset>` directories, reconfigured with `--fresh` and
rebuilt with `--clean-first`. Nothing was cloned or fetched. The FetchContent sources
(fmt, Catch2) were already present in each build tree.

| Check | Result |
|---|---|
| Build: release, debug, asan, tsan, gcc-release | All pass, no warnings under `-Werror` (7, 6, 9, 7, 13 s) |
| `ctest --preset release` | 17/17 pass (16 Catch2 cases + `rom-manifest`), 98 s |
| `ctest --preset gcc-release` | 17/17 pass, 100 s |
| `ctest --preset debug` | 17/17 pass, 897 s |
| `ctest --preset asan` (ASan + UBSan) | 17/17 pass, 333 s, no sanitizer reports |
| `ctest --preset tsan` | **16/17.** The 16 unit tests pass with no ThreadSanitizer reports. `rom-manifest` hits its 1200 s ctest timeout (F11). A direct rerun with no timeout was stopped after about 35 min without a result, so whether the ROM gate passes under tsan is unknown |
| `ultra_harness manifest tests/roms/manifest.txt` | 100 entries, 0 not as expected |
| `python3 tools/progress.py --check` | Exit 0 (93 s) |
| Ledger vs manifest | Exact. The 5 ledger expected-fail keys cover exactly the 7 manifest `fail` lines (`cpu-timing-residual` covers two), and every other entry is `pass` and passes. "Exact" does not mean complete; see F2 |
| n64-systemtest ROM hashes | Match `tests/roms/README.md` |
| Patched vs unmodified n64-systemtest | Every test the unmodified build runs before it hangs at `RSP BREAK` also runs in the patched gate ROM. This is consistent with the patch removing only RSP/RDP groups. It could not be rebuilt from source here (needs the Rust toolchain and network) |
| Save-state round trip by bus trace (auditor's own harness, see below) | Pass at all 5 save points |
| Trace determinism, clang vs GCC, n64-systemtest 2 s (3.4 M records) | Byte-identical |
| Trace, `--threads 1` vs `--threads 8` | Byte-identical, but `--threads` is ignored in Phase 0 (`src/app/main.cpp:77`), so this proves nothing yet |
| The 17 unlisted krom `Exceptions` ROMs | 17/17 fail (F2) |

**Save-state check.** A scratch program, not committed, linked against the release libraries.
It ran three machines:

- **C:** runs uninterrupted from 0 to the end time.
- **A:** runs to the save point, saves, and continues to the end.
- **B:** a fresh machine that loads A's state and runs to the end.

It then required three things: trace A equal to trace C; A's records after the save point
equal to all of B's trace; and identical final state bytes for A, B and C. Save points:

- CPUADD at 0.05 s (during the PIF boot)
- CPUADD at 0.235 s (IPL3's PI DMA in flight)
- CPUADD at 0.4 s
- n64-systemtest at 1.5 s (mid-suite)
- CPUTIMINGNTSC at 1.7 s

All passed. This is a stronger check than the repo's own test, which compares only final state
bytes for one ROM.

## Findings

Ranked by severity. File references are to commit `91b00bc`.

### Critical

**F1. `src/io/cic.cpp` transliterates third-party code; `docs/hardware/pif.md` says it does not.**

- `cic6105_challenge` (cic.cpp:130-151) is the same statement sequence as `Cic6105Algo` in
  jago85/UltraCIC_C `cic_c.c`, with the same variable roles and the same operations in the
  same order. It is a line-for-line port. Its own comment cites that function as the source.
- `ipl2_checksum` (cic.cpp:49-112) follows `CalculateChecksum`/`FinalizeChecksum` in
  jago85/PifChecksum:
  - the same 16-word state with the same indices assigned in the same order;
  - the same `loop == 1008` exit placed mid-body;
  - the same final four-accumulator fold.
  Only the rotate idiom changes, to `std::rotl`/`rotr`.
- Both upstream files sit in the git-ignored `third_party/refsrc/`.
- `docs/hardware/pif.md:7` states "Only facts were taken from the two jago85 sources; no code
  was copied." That is not accurate.

Why it matters:

- CLAUDE.md forbids copying, transliterating or structurally mirroring another
  implementation's code. The spec says "All code is written from documentation and observed
  behavior". These are a CIC firmware re-implementation and a checksum tool, not emulators,
  but the same rule applies.
- PifChecksum is MIT and requires its copyright notice to accompany substantial portions.
  No notice is present.
- `cic_c.c`'s licence could not be determined from the local copy.
- Per the local `origin/main` ref, this commit has been pushed to
  `github.com/jeff-strutb/ultra`. The auditor did not contact GitHub to check whether the repo
  is public.
- The algorithms themselves are documented on n64brew ("PIF-NUS", IPL3 checksum; "CIC-NUS").
  A clean rewrite from those notes is feasible, and the IPL2 already computes the same
  checksum in MIPS code (`io/pif_boot_rom.cpp`).

### High

**F2. 17 vendored krom CPU ROMs are silently excluded from the gate, and all 17 fail.**

- `tests/roms/krom/CPUTest/Exceptions/{Compare,Syscall,TLB,Trap,Unaligned,VIIntr}/*.n64` are
  vendored, each with a reference PNG. None appears in `manifest.txt`, the ledger,
  `tests/roms/README.md` or the README status block. The status block's "91 / 95 ROMs" counts
  only the manifest.
- Run through `ultra_harness screen`, all 17 differ from their references.
- The auditor classified each by the rows the ROM itself marks FAIL:
  - **15 of 17** fail on the `Status` row. The references show 0x241000E0, the EverDrive-launch
    value that ledger `krom-reference-cop0register` already documents. Ours shows 0x34000000,
    which is correct for a PIF boot.
  - **TLBReadMiss, TLBReadMissDelay, TLBWriteMiss, TLBWriteMissDelay** also fail `Handler PC`:
    ours 0x80000180, expected 0x80000080.
  - **TLBReadMissNested and TLBReadMissNestedDelay** also fail `BadVAddr`: ours 0x10, expected
    0x20.
  - **CompareDisabled and VIIntrDisabled** have no FAIL rows of their own. They differ only in
    displayed values: Cause power-on bits, and an EPC inside the ROM's accepted range.
- The TLB differences follow from two things:
  - The EverDrive Status has UX = 1, which gives an XTLB refill to 0x80.
  - Our power-on TLB is all zeros (`src/cpu/vr4300.cpp:33`). A zero entry matches VPN2 0 /
    ASID 0 with V = 0, so the access to 0x10 is a TLB *Invalid* exception (to 0x180), not a
    refill. The nested test's second access never happens.

  Neither is evidence of a CPU bug. But the zero-TLB choice decides the outcome of these ROMs,
  and it is not in the ledger (F7).
- The gate result depends on which ROMs were listed, and the omission is recorded nowhere.

**F3. The VR4300 timing model is not the five-stage pipeline model the Phase 0 prompt asks
for.**

- The interpreter retires one instruction per step and adds stall cycles from tables
  (`docs/hardware/cpu.md`, "Pipeline timing model"). There is no IC/RF/EX/DC/WB state, so
  interactions between stages are approximated by per-pair rules.
- The krom timing ROMs read 0.14-1.72% slow against hardware (ledger
  `cpu-timing-residual`, status Open).
- n64-systemtest's own timing and cycle groups were not built
  (`n64-systemtest 2.1.0 (base=1 timing=0 cycle=0 cp0-hazards=0)`). The one hardware oracle in
  the tree that measures CPU cycle behaviour therefore never runs.
- The spec's 64K-entry decode table was also not implemented. That deviation is documented
  and left open (`cpu.md`, last section).

**F4. There is no CI.**

- The repository has no CI configuration (no `.github/`, nothing equivalent). "CI asserts the
  ledger is exact" and "every merge passes the CI gates" are currently true only when someone
  runs `ctest` locally.
- The spec's `clang-tidy` config, which enforces the coding rules, does not exist either.
- The README status block embeds `datetime.date.today()` (`tools/progress.py:87`), so
  `progress.py --check` fails on any day after the snapshot without any code change. It is not
  usable as a CI gate in its current form.

### Medium

**F5. The timing ROMs pass at a 2% tolerance the project chose itself.**

- `krom/CPU/TIMINGNTSC` and `krom/CP1/TIMINGNTSC` pass at 2.0%. The worst entry is MULTU at
  +1.72%; ROUNDWD is also +1.72%. The exact variants are recorded as failing
  (`cpu-timing-residual`).
- This is disclosed honestly. But the spec's oracle is "the ROM's documented output", which
  here is the hardware count. The `pass` entries measure closeness, not a match.

**F6. Load path: no input validation, and not atomic.**

- `System::load_state` (system.cpp:133) checks only RDRAM size (rdram.cpp:247), section names
  and section versions. It does not check:
  - the cartridge ROM;
  - the CIC;
  - the TV standard, which sets the VI/AI clock ratios.

  ADR 0003 says loading "requires a `System` constructed with the same ROM", but nothing
  enforces it, so a mismatched state loads silently and then runs wrongly.
- A load that throws partway (a truncated buffer, or a section mismatch in a later module)
  leaves earlier modules overwritten, including the emptied scheduler heap
  (scheduler.cpp:95-98). The machine is then inconsistent.
  `tests/unit/test_machine.cpp:41-49` keeps using the same machine after two failed loads and
  checks only that they throw.
- Fields are restored by `memcpy` into `bool` and enum members. A corrupt state can produce
  invalid values.

**F7. Unledgered guesses and source conflicts.**

- **Power-on TLB contents.** All-zero entries, `vr4300.cpp:33`. The manual says undefined.
  The choice changes the exception taken in F2's ROMs. It is not in the ledger.
- **FCR0 has two values.**
  - `Vr4300::kFcr0 = 0x0B00` (vr4300.hpp:91, cited to n64brew), which is unused.
  - `kFcr0Value = 0x0A00` (cop1.cpp:54, cited to n64-systemtest).

  The conflict is not recorded.
- **EntryLo write mask.** `EntryLo::kWriteMask = 0x3FFFFFFF` (cop0_regs.hpp:119) has no
  citation and keeps bits 29:26, which UM 5.4.3 shows as zero. n64-systemtest's EntryLo
  masking tests pass, so the value may be right, but nothing documents it.
- **Ledger `cop0-cause-poweron` (0xB000007C).** It is supported by COP0Cause's reference, but
  VIIntrDisabled's reference shows Cause = 0 at the same point. The conflicting evidence is
  not listed.
- **Unnamed, unledgered 1-tick write-acceptance latencies:** `pi.cpp:153,162`,
  `si.cpp:113,119`, `mi.cpp:139`.
- **`mi-reg-read` arithmetic.** The ledger turns "5-6 vs about 2 PClocks" into "3 ticks
  less". That difference is 2.0-2.7 ticks, so 3 is a rounding choice. The entry is marked
  Guess, so this is minor.
- **Derived constants missing from the ledger.** Nine Derived constants in
  `memory/arbiter_params.h` have no ledger rows, although the ledger header says Derived
  parameters belong there. The four Guess constants match the ledger.

**F8. Many hardware constants are inline and uncited, and raw shifts appear at call sites.**

Both are against CLAUDE.md. The timing constants are well done: every value in
`cpu/timing.hpp`, `core/clock.hpp` and `memory/arbiter_params.h` is named and cited, and the
ones spot-checked against the local VR4300 manual and n64brew copies are correct. The rest of
the code is uneven:

- **Address map named but uncited:** core/bus.cpp:32-39. Plus inline region bounds at
  bus.cpp:115-164.
- **Register write masks inline:** VI (vi.cpp:104-118), PI (pi.cpp:114-129), AI (ai.cpp:51,
  53, 68), SP (sp_interface.cpp:67-71, 124-154), RI (ri.cpp:88-125).
- **CPU:**
  - reset vector repeated inline (vr4300.cpp:54, 64, 73);
  - `cause_ = 0xB000'007C` inline (vr4300.cpp:17);
  - EntryHi mask repeated inline at vr4300.cpp:223, 232, 347 although `EntryHi::kWriteMask`
    exists;
  - TLB size as `& 31` at five sites.
- **Duplicated facts:**
  - RDRAM row and bank sizes in both `arbiter_params.h` and `ri.cpp:15-16`;
  - the 2 KiB page as `0x800` in sp_interface.cpp:127 and `0x7FF` in rdram.cpp:119, 144;
  - the checksum magic in both `cic.cpp` and `pif_boot_rom.cpp`.
- **About 40 raw register-field shifts at call sites.** Examples: `cause_ |= 1u << 15`
  (vr4300.cpp), MI mask pairs (mi.cpp:69-72), AI_STATUS assembled by hand (ai.cpp:39-42),
  dp_interface.cpp:48-51, ri.cpp:30-36.
- **Mis-citations:**
  - Count cited as UM 6.3.4; the correct section is 6.3.3.
  - `timing.hpp:45` calls the extra FPU cycle a "DC->EX bypass", but UM 7.5.6 says the bypass
    is *not* performed. The value is right.
  - `cic.cpp:22` cites the n64brew "IPL3 32-bit Magic" column for 0x6C078965. That column
    gives it only for 6103/6106, so it does not support an IPL2 constant.

**F9. Unit-test coverage is thin for "every module ships with its unit tests".**

There are 16 Catch2 cases in total. They cover:

- the scheduler;
- save-state basics;
- the trace sink;
- MI;
- a handful of CPU behaviours;
- PI DMA;
- VI field rate;
- the CIC.

There are none for:

- RDRAM and RI (the IPL3 init sequence and the refresh register);
- the arbiter;
- SI and PIF commands;
- AI;
- SP and DP;
- the caches (refill, writeback, CACHE ops);
- the write buffer;
- COP0 read/write masks.

ROM tests cover much of this indirectly, but the CLAUDE.md rule "add a unit test that would
have caught it" assumes per-module tests exist. `rdp/dp_interface` also has no
`docs/hardware/` note.

**F10. The ADR 0003 claim about the round-trip test is overstated.**

ADR 0003 says that "any field added to a module without a matching visit is caught by the
round-trip test". The test compares final state bytes for one ROM over one window, so it
catches only fields that change saved state within that window. The auditor's trace-based
check (above) shows the round trip is currently correct, but the repo's test would not have
proven it.

**F11. The ROM gate cannot finish under the tsan preset within its own timeout.**

- `tests/CMakeLists.txt` sets `TIMEOUT 1200` on `rom-manifest`, and `ctest --preset tsan`
  fails on that timeout. The four presets ran in parallel on a 14-core machine, each harness
  at about 100% of one core, so they were not starved for CPU.
- CLAUDE.md requires the tsan preset before any merge that touches threads, so as configured
  that gate always fails.
- The ROM gate never opens a trace, so under tsan it exercises no second thread anyway (see
  Threading rules).

### Low

- **ISViewer output is not saved.** `Cartridge::isv_out_`, `isv_dropped_` and
  `isviewer_enabled_` are neither serialized nor recomputed (cart.hpp:58-61). After a load,
  previously printed text is gone and the 4 MiB output cap restarts. `isviewer_enabled_`
  selects ISViewer vs open bus on reads, but nothing currently changes it from its default.
- **The PIF boot ROM image is serialized** (pif.cpp:158), although ADR 0003 calls it an input,
  not state. It is harmless, but contradicts the ADR.
- **Trace producer cost.** It calls `wake_.notify_one()` on every record
  (trace.cpp:54), a wake syscall per bus transaction on the timing thread. There is no
  correctness issue; it is a cost to measure when tracing at speed.
- **Halted CPU noticed late.** `System::run_until` checks `pif_.halted_cpu()` only once per
  4096-tick slice (system.cpp:72). After a CIC checksum mismatch the CPU can execute up to a
  slice further than hardware would. This is deterministic, and only matters for bad
  cartridges.
- **The harness can end a systemtest run early.** It stops at the "Slowest tests" text
  (harness.cpp:66) rather than at the ROM's exit call. The spec's wording is "runs to its exit
  call". In practice the summary is printed at the end, so this is cosmetic.
- **The harness header comment is stale.** It says `screen` compares the raw framebuffer; it
  uses `capture_presented` (a nearest-neighbour resample to the PNG size).
- **The n64-systemtest patch changes the stress-test features.** Removing the
  `append_stress_tests` push lines leaves their `#[cfg(feature = ...)]` attributes stacked on
  the `cop1_stress_test` block, so that block now needs all 17 features. The stress tests are
  off by default, so the gate is unaffected.
- **Process.**
  - Phase 0 landed as one commit on `main`, not one feature per branch.
  - The ledger's "open an issue naming the probe" step has no issue references.
  - SI, VI, AI and SP DMA were built beyond the Phase 0 prompt's scope. They were needed by
    the ROMs; noted, not a defect.

## Threading rules (CLAUDE.md)

- **Threads, atomics and mutexes.** Phase 0 creates exactly one thread: the trace writer
  (`core/trace.cpp:27`). `src/` has no `std::mutex`, locks or condition variables. The trace
  ring is single-producer/single-consumer with acquire/release atomics, and backpressure uses
  `std::atomic::wait`, as ADR 0002 describes.
- **Virtual dispatch.** `IArbiter` is the only virtual interface.
- **Allocation on the timing thread after init:**
  - ISViewer output appends into a buffer reserved at construction and capped at that
    capacity, so it does not reallocate.
  - `System::save_state()` grows a `std::vector`. That is a user action, not the per-tick
    path.
  - Error paths build `std::string` before throwing.
  - Nothing allocates per tick or per transaction.
- **Scope of the tsan run.** The ROM gate never opens a trace, so the tsan run of
  `rom-manifest` exercises no second thread. Only the `trace:` unit test does.

## Clean-room spot-check: scope limits

- **Checked:** the two third-party sources present locally (`third_party/refsrc/`), with the
  result in F1.
- **Not checked:** the spec asks for a spot-check against ares and Angrylion. Neither is in
  the repo, and this audit was limited to the repo with no network access, so that comparison
  was not done.
- **Other observations:**
  - `docs/hardware/` cites no emulator source.
  - The PIF boot ROM (`io/pif_boot_rom.cpp`) is an original MIPS program assembled at runtime,
    not a copy of Nintendo's IPL1/IPL2.

## Licensing note (not a gate item)

- `tests/roms/README.md` already warns that every krom ROM and each `LIB/N64_BOOTCODE.BIN`
  contains Nintendo's 6102 IPL3, which the Unlicense cannot cover. It asks for a decision
  "before pushing this directory anywhere".
- Per the local `origin/main` ref, the directory has been pushed. The decision still appears
  to be open.

## Remediation log

- **2026-10-19, F1.** `src/io/cic.cpp` and the IPL2 checksum routine in `src/io/pif_boot_rom.cpp`
  (which the remediation found also mirrored PifChecksum.c) were replaced through a two-party
  clean room. One party wrote `docs/hardware/cic-algorithms.md` from the jago85 sources; the
  implementer wrote both from that specification only. Git history was rewritten so that no
  commit contains the earlier versions. Checks: the new `ipl2_checksum` and
  `cic6105_challenge` match the old functions' outputs on 800 black-box vectors; both real
  IPL3s give the n64brew value 0xA536C0F1D859; unit tests run the emulated IPL2 on random
  IPL3s for each of the four seeds and compare what it hands the PIF with `ipl2_checksum`.
- **2026-10-19, licensing note.** The krom ROM images and `N64_BOOTCODE.BIN` (Nintendo's 6102
  IPL3) are no longer tracked, and history was rewritten to remove them from every commit.
  `tests/roms/krom/ROMS.sha256` pins the images to obtain locally.
- **2026-10-19, F2 (and the power-on TLB part of F7).** The 17 `CPUTest/Exceptions` ROMs are in
  the manifest as expected failures, each explained row by row from a side-by-side capture in
  three ledger entries: `krom-reference-everdrive-exceptions` (10 ROMs: launcher Status and
  leftover values only), `krom-reference-exceptions-tlb` (6 ROMs: launcher Status/KX/TLB, and
  our power-on TLB) and `exception-compare-latency` (1 ROM: every check passes; unexplained
  Count-delta spread, open). New parameter rows `tlb-poweron` and `tlb-shutdown` record the
  all-zero power-on TLB and the unmodelled TLB shutdown as Guesses with probes. Every manifest
  `fail` line now names its ledger key. Gate: 117 entries, 0 not as expected; the status block
  reads 91 / 112 ROMs.
- **2026-10-19, F4 and F11 (partly).** Added `.github/workflows/ci.yml`: release (Clang 18),
  gcc-release (GCC 14), asan and tsan on ubuntu-24.04, each running ctest. The harness now
  treats an entry with missing files as not as expected (previously a missing ROM counted as an
  expected failure), or, with `--allow-missing` (`ULTRA_ROMS_OPTIONAL=ON`, used by CI), as a
  reported skip. The machine-level unit tests boot the tracked n64-systemtest ROM to IPL3's
  handover and reset the caches and TLB from RSP IMEM before taking over, so they run from a
  fresh clone; the one test that needs Nintendo's IPL3 skips visibly. `tools/check_ledger.py`
  (ctest `ledger-exact`) checks that every expected failure names an expected-fail ledger row,
  every row is still used, and every `ledger "<key>"` in the source exists. The tsan test
  preset excludes the ROM gate (label `rom`), which starts no second thread. Correction to F4:
  `progress.py --check` already ignores the snapshot date (checked with a backdated README; exit
  0). Not done: the `clang-tidy` config (clang-tidy is not installed on this host), and the
  workflow has not yet run on GitHub.
- **2026-10-19, F3 (partly).** n64-systemtest's Timing, Cycle and CP0-hazards groups now run in
  the gate: `n64-systemtest-cpu.z64` is rebuilt from the same tree with those features (the
  default-feature build reproduces the previous ROM byte for byte). The harness previously
  read only the "Base" summary line, so it would have passed this ROM with failures; it now
  requires every reported group to pass, and `--log` writes the ROM's output. All 9 tests in
  those groups failed at first, from three defects, each now fixed with a unit test that fails
  on the old code: a conditional branch in a jump's delay slot offset from its own address
  instead of the outer target (4 tests); a Wired write reset Random one instruction early
  (3); an FPE raised by CTC1 did not take Cause.CE from the next instruction (2). Result:
  3417 of 3417. The behaviour beyond what the ROM measures is ledgered (`cause-ce-next-instr`,
  `jump-region-in-delay-slot`). Not done: the five-stage pipeline model and the krom timing
  residual (`cpu-timing-residual`, F5).
- **2026-10-19, F3 (pipeline model) and F5 (investigated).** Time is now accounted by pipeline
  stage (ADR 0005, `docs/hardware/cpu.md` "Pipeline timing model"):
  - instruction i+1 is fetched in RF, before instruction i's DC access, and held in a latch;
  - RF stalls overlap EX interlocks, and a multicycle instruction overlaps the previous load's
    DC stall;
  - exceptions cost by detecting stage (RF 4, EX 5, DC 6 PCycles; previously 2-3), and
    interrupts wait for a run cycle;
  - killed delay slots cost a cycle;
  - the 2-entry instruction micro-TLB charges its 3-PCycle refill.

  Each rule has a unit test (`tests/unit/test_cpu_pipeline.cpp`) that fails when the rule is
  removed. n64-systemtest still passes 3417 of 3417. The krom TIMINGNTSC counts are unchanged
  (±1, start phase): their loops have no overlap, killed slot or exception. Three new Guess rows
  cover the parts the manual leaves open (`itlb-invalidation`, `exception-redirect-cycle`,
  `cp0-bypass-interlock`).

  F5: the residual is not in the CPU model. The model's loops are exact whole MasterClocks, and
  hardware separates loops the model times equally. One busy window per scanline, a periodic
  window of any period and a VI-clock synchronizer were each fitted to the seven loop classes
  and rejected (ledger `cpu-timing-residual`). Closing it needs probes 1 and 6 on hardware. The
  2 % tolerance stays, with the exact variants ledgered as expected failures.
- **2026-10-19, F6.** Save-state loading is checked and atomic (ADR 0003, format version 2):
  - a `system` identity section (ROM hash, CIC, TV standard) rejects a state made by a
    different machine;
  - `bool` and enum fields are range-checked instead of copied raw (PIF CIC, SI op, CPU
    load-target and write-buffer master);
  - `System::load_state` restores a copy taken first if anything throws (trailing data was
    already rejected).

  `tests/unit/test_machine.cpp` checks that a failed load leaves the machine byte-identical
  and still running like an untouched one, for a bad header, truncation and trailing data. It
  also checks rejection of another ROM, CIC and TV standard, and of out-of-range bool and
  enum bytes. Each check fails when its guard is removed.
- **2026-10-19, F7.** Every item now has a ledger row (49 parameters):
  - `fcr0-value`: the unused `Vr4300::kFcr0` (0x0B00) is removed. 0xA00 stays, because
    n64-systemtest fails with 0x0B00. The row records that the manual (7.2.5) and n64brew both
    say 0x0B00.
  - `entrylo-write-mask`: 0x3FFFFFFF is cited. n64-systemtest's four EntryLo masking tests
    fail if bits 29:26 are cleared or bit 30 is kept. The row records the conflict with
    Figure 5-10.
  - `write-accept`: the 1-tick acceptance in PI, SI, MI and the bus is one named Guess,
    `kWriteAcceptTicks`.
  - `mi-reg-read`: the text now gives the 2.0-2.7 tick range and says 3 is a rounding choice.
  - `cop0-cause-poweron`: now names the VIIntrDisabled reference showing Cause = 0.
  - `arb-*`: seven rows cover the nine Derived arbiter constants, and the source comments name
    them.
