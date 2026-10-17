# RSP interface (Phase 0 subset)

Phase 0 has no RSP core ("Do not start the RSP"). `src/rsp/sp_interface.*` models only what the
CPU and boot path touch. Sources: **[n64brew]** "Reality Signal Processor/Interface", "Memory
map", "RDRAM Interface".

| Fact | Source |
|---|---|
| DMEM (0x04000000) and IMEM (0x04001000), 4 KiB each, mirror every 8 KiB up to 0x0403FFFF | [n64brew] "Memory map" |
| SP_STATUS starts halted; IPL1 waits for halt, then writes 0x0A (set halt, clear interrupt) | [n64brew] "Initial Program Load" |
| SP_SEMAPHORE: a read returns the bit and sets it; a write clears it | [n64brew] SP_SEMAPHORE |
| SP DMA is double-buffered (one pending request behind the active one); each row moves LEN + 1 bytes (8-byte granularity), COUNT + 1 rows, RDRAM address advancing by SKIP after each row; the SP address wraps within its 4 KiB bank | [n64brew] SP_DMA registers |
| DMA bursts are at most 128 bytes on the RDRAM bus | [n64brew] "RDRAM Interface", Count |
| SP_PC reads and writes the (idle) RSP program counter | [n64brew] SP_PC |

The RSP core, its pipeline and the vector unit are Phase 1.
