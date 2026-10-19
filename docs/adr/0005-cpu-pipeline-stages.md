# ADR 0005: VR4300 timing by pipeline stage

Status: proposed (Phase 0, audit finding F3)

## Context

The interpreter retires one instruction per `step()` and adds each stall to a running PCycle
count. The VR4300's five-stage pipeline (IC RF EX DC WB, manual chapter 4) has properties
that a running sum cannot express:

- In one pipeline cycle the DC stage holds instruction i-1, EX holds i, and RF holds i+1, so
  i+1's instruction fetch happens before i's data access (4.1, Figure 4-2). A DC stall is
  resolved before a simultaneous RF stall (4.7.3, "Interaction between RF and DC Stages"). A
  multicycle instruction in EX keeps running while DC or RF stalls (4.6.1, 4.7.3).
- An exception is taken in the stage that detects it (Figure 4-12): RF (instruction address,
  TLB and bus errors), EX (SYSCALL, BREAK, coprocessor unusable, reserved instruction) or DC
  (everything else, including interrupts). The pipeline then stalls 2 PCycles and the excepting
  instruction and all younger ones are aborted (4.7). An EX exception cancels the
  instruction fetch in RF, so it makes no memory request (4.7.3).
- A killed delay slot (branch-likely not taken, the slot after ERET) still occupied a
  pipeline slot.
- Instruction fetches from mapped space go through a 2-entry micro-TLB; a miss costs 3 PCycles
  in RF (1.5.1 "Instruction Micro-TLB (ITLB)", 4.6.2).
- An interrupt is accepted only if the previous PCycle was a run cycle (4.7.1).

## Decision

Keep functional execution one instruction at a time, in program order: it is exact for all
software-visible results and passes n64-systemtest. Account for time by pipeline stage:

- **Fetch one ahead.** While instruction i is in EX, `step()` fetches i+1 (the RF stage) and
  keeps it in a one-entry latch that the next `step()` uses. The fetch is issued after i's EX
  interlocks start and before i's first DC-stage action (data translation, D-cache or bus
  access, DC-stage exception). An instruction with no DC work fetches at the end of its step.
  An EX-stage exception cancels the fetch. A fetch fault is not raised early: the latch
  records it and the instruction's own step raises it in program order.
- **Overlap by the manual's rules.** Stall cycles in one pipeline cycle combine as
  `max(MCI, DC + RF)`. The RF fetch starts at i's run cycle, so its cost overlaps i's LDI/MCI.
  i's MCI overlaps i-1's DC stall unless i uses i-1's load result (4.6.1).
- **Exceptions cost by stage.** The handler's first instruction reaches RF 4 PCycles after the
  end of the detection cycle: 2 stall cycles, then IC, then RF. An interrupt is detected at the
  step boundary, or one cycle later if the previous cycle was a stall.
- **Killed slots cost one cycle.**
- **Micro-TLB:** 2 entries of 4 KiB, LRU, only for fetches that go through the JTLB. Flushed
  on TLBWI/TLBWR and on an EntryHi write that changes the ASID. The flush rule is a Guess
  (ledger `itlb-invalidation`).
- CP0I (4.6.9) is not modelled. Its description (an instruction that caused an exception
  reaches WB while a younger one is in DC) contradicts 4.7, where every younger instruction is
  aborted. It is ledgered with the probe that would settle it.

## Consequences

- Bus transactions now follow the hardware's order of instruction fetch and data access. This
  changes bus traces for uncached code (IPL) and for I-cache misses next to data accesses. Total
  time for a serial bus does not change.
- Exceptions cost 2 to 3 PCycles more than before (DC +3, EX +2, RF +2, interrupt +3). No
  hardware measurement in the tree checks this exact count. The derivation is in
  `docs/hardware/cpu.md`, and the redirect cycle is ledgered (`exception-redirect-cycle`).
- The krom TIMINGNTSC loops are unchanged: they have no I-cache misses, overlaps or
  exceptions. Their residual (`cpu-timing-residual`) is fractional jitter outside the CPU (see
  the ledger).
- vr4300 save-state section version 3: adds the fetch latch, the previous DC stall, the
  stall-ended flag and the micro-TLB.
