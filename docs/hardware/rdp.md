# RDP command interface (Phase 0 subset)

Phase 0 has no RDP. `src/rdp/dp_interface.*` models only the DPC registers that the CPU and
boot code touch; the command processor and the raster pipeline arrive in Phase 2. Sources:
**[n64brew]** "Reality Display Processor/Interface", "Memory map".

| Fact | Source |
|---|---|
| The DPC registers are at 0x04100000 and mirror every 0x20 bytes; the span registers (DPS) are at 0x04200000 | [n64brew] "Memory map" |
| DPC_START and DPC_END are 24-bit RDRAM addresses in 8-byte units (bits 23:3) | [n64brew] DPC_START, DPC_END |
| Writing DPC_START sets START_PENDING (DPC_STATUS bit 10); the next DPC_END write copies START into DPC_CURRENT and clears it | [n64brew] DPC_STATUS, DPC_START |
| DPC_STATUS write layout: (clear, set) pairs for XBUS (bits 0/1), FREEZE (2/3) and FLUSH (4/5); bit 9 CLR_CLOCK resets DPC_CLOCK. Setting and clearing together changes nothing | [n64brew] DPC_STATUS write |
| DPC_STATUS read layout: XBUS bit 0, FREEZE bit 1, FLUSH bit 2, START_PENDING bit 10 (the busy and ready bits are not modelled while no RDP runs) | [n64brew] DPC_STATUS read |
| DPC_CLOCK is a 24-bit counter at the RCP clock | [n64brew] DPC_CLOCK |
| DPC_BUFBUSY, DPC_PIPEBUSY and DPC_TMEM read 0 (no RDP activity) | Phase 0 scope |
| DPS span registers read 0 and ignore writes | ledger `dp-span` |

Unit tests: `tests/unit/test_io.cpp` ("dp: ...").
