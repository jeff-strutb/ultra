# ADR 0003: Save-state format and visitor

Status: proposed (Phase 0)

## Context

Every module must serialize its complete mutable state (CLAUDE.md), save states must round-trip
exactly (a loaded state continues identically to the original), and a state produced by a
different build must fail loudly instead of loading garbage.

## Decision

- **One visitor, two directions** (`core/savestate.hpp`): each module implements
  `void serialize(SaveState&)` and visits its fields in a fixed order. In save mode each visit
  appends bytes; in load mode it overwrites the field from the buffer. Saving and loading
  cannot drift apart because they are the same code.
- **Format:** header (`"ULTR"` magic as a u32, format version 2), then one section per module.
  A section starts with the FNV-1a hash of its name and a module version; on load both must
  match, otherwise loading throws. Values are raw host-endian bytes of trivially copyable
  fields; pointers cannot be serialized (enforced by a `requires` clause).
- **Identity:** the first section (`system`) records the FNV-1a 64 hash of the cartridge ROM,
  the CIC and the TV standard. A load into a machine that differs in any of them throws.
- **Checked fields:** `bool` is stored as one byte that must be 0 or 1, and enums go through
  `enumeration(v, last)`, which rejects values above `last`. A raw copy could produce a value
  the type cannot hold.
- **All or nothing:** `System::load_state` saves the machine first, and restores that copy if the
  load throws at any point (bad header, identity, truncation, trailing data, a section mismatch,
  an invalid field). A failed load leaves the machine exactly as it was.
- **Order:** `System::serialize` visits identity, scheduler, RDRAM (contents and device registers),
  arbiter, RI, MI, PIF, SI, PI, SP, DP, VI, AI, cartridge (ISViewer state, not ROM), CPU
  (registers, COP0/COP1, TLB, caches, write buffer), interrupt pins.
- **Derived state is recomputed on load**, never stored: the scheduler heap (ADR 0001), TLB
  compare masks, RDRAM address map.
- The cartridge ROM and the PIF boot ROM are inputs, not state: loading requires a `System`
  constructed with the same ROM, CIC and TV standard and powered on, and the identity section
  enforces it.

## Consequences

- A field added to a module without a matching visit is caught by `tools/check_serialize.py`
  (ctest `serialize-coverage`). Every data member of a class with a serializer must be named in
  its `serialize()` or marked `// not state: <reason>`; references are wiring and exempt. The
  check works on names. It does not check the fields of a member's own struct type, or that a
  visit restores the value correctly.
- That a state restores correctly is checked by behaviour
  (`tests/unit/test_machine.cpp`, "a loaded save state continues exactly like the original"):
  - save points in the PIF boot ROM, during an in-flight PI DMA, and in the ROM's own code;
  - from each, the original and a fresh machine loaded from the state run 0.1 s;
  - B's bus trace must equal A's after the save point, and the final states must be identical.

  This catches only what shows in bus traffic or saved state within those windows. A field
  that matters for one instruction after it is set, such as the CPU's `prev_dc_stall_`, is
  missed unless a save point lands inside that span. That is why the source check exists.
- Bumping a module's section version is required when its layout changes; old states then fail
  with the section name in the error.
- A load costs one extra save (RDRAM included) for the rollback copy.
- States are host-endian and not portable between big- and little-endian hosts; acceptable
  while every supported host is little-endian.
