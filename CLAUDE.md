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
