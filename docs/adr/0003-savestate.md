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
- **Format:** header (`"ULTR"` magic as a u32, format version 1), then one section per module.
  A section starts with the FNV-1a hash of its name and a module version; on load both must
  match, otherwise loading throws. Values are raw host-endian bytes of trivially copyable
  fields; pointers cannot be serialized (enforced by a `requires` clause).
- **Order:** `System::serialize` visits scheduler, RDRAM (contents and device registers),
  arbiter, RI, MI, PIF, SI, PI, SP, DP, VI, AI, cartridge (ISViewer state, not ROM), CPU
  (registers, COP0/COP1, TLB, caches, write buffer), interrupt pins.
- **Derived state is recomputed on load**, never stored: the scheduler heap (ADR 0001), TLB
  compare masks, RDRAM address map.
- The cartridge ROM and the PIF boot ROM are inputs, not state: loading requires a `System`
  constructed with the same ROM and powered on.

## Consequences

- Any field added to a module without a matching visit is caught by the round-trip test
  (`tests/unit/test_machine.cpp`: save at 0.3 s, run to 0.6 s; load into a fresh machine, run to
  0.6 s; the two final states must be byte-identical).
- Bumping a module's section version is required when its layout changes; old states then fail
  with the section name in the error.
- States are host-endian and not portable between big- and little-endian hosts; acceptable
  while every supported host is little-endian.
