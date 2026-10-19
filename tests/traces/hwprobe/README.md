# hwprobe console records

The hardware oracle: records that `tests/probes/hwprobe` wrote on real consoles. Never put an
emulator's record here.

One directory per run, named `<date>-<console>-<cart>-<boot>`, for example
`2026-10-20-ntsc-nus001-sc64-direct`. Each holds:

- `stage1.bin`, `stage2.bin`: the full records from the SD card (`ultra_hwprobe/`);
- `hwprobe.sav`: the 128 KiB FlashRAM save;
- `NOTES.md`: the console (region, board revision if known), the Expansion Pak, the flashcart
  and its firmware, the boot method (direct or menu), the ROM's build id (shown on screen and
  in the record header), and what was plugged into the controller ports.

```bash
python3 tools/hwprobe.py dump tests/traces/hwprobe/<run>/stage1.bin
python3 tools/hwprobe.py compare tests/traces/hwprobe/<run>/stage1.bin emu.bin
```
