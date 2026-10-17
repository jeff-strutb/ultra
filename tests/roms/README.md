# Test ROMs

Third-party test ROMs and their reference outputs, run by `ultra_harness manifest
tests/roms/manifest.txt --rom-dir tests/roms` (the `rom-manifest` ctest). Expected outcomes and
every expected failure are explained in `docs/timing/ledger.md`.

## n64-systemtest

- Upstream: https://github.com/lemmy-64/n64-systemtest, commit
  `196f5421173220eb2f63a7a99c64795dc0ea0698`. Licence: MIT (`n64-systemtest/LICENSE`).
- Built with the toolchain the repository pins (`nightly-2022-07-10`) and `nust64` 0.4.1, which
  links libdragon's open-source IPL3.
- `n64-systemtest-full.z64` (sha256 `d161ce45…dacb8ed`): unmodified build. Its RSP tests wait
  for an RSP, so it does not finish before Phase 1.
- `n64-systemtest-cpu.z64` (sha256 `60df9940…6f0f3a2`): built with
  `phase0-cpu-scope.patch`, which only removes the RSP and RDP test groups from
  `src/tests/testlist.rs`. This is the Phase 0 gate ROM (CPU, COP0, TLB, COP1, MI, PI, SP and
  PIF memory tests).
- Upstream marks the project as superseded by nemu64-test; the tests used here are unchanged.

## krom (PeterLemon/N64) CPUTest

- Upstream: https://github.com/PeterLemon/N64, commit
  `7085543e4a19d8c539fc9e0a4d2869e788b4ed4b`, directory `CPUTest/` (ROMs, sources, fonts and
  reference screenshots; `make.bat` omitted). Licence: Unlicense (`krom/LICENSE`).
- **The ROM images are not in this repository.** Every krom ROM embeds `LIB/N64_BOOTCODE.BIN`,
  Nintendo's 6102 IPL3 (ROM bytes 0x40-0xFFF). That code is not krom's, the Unlicense cannot
  cover it, and it is never published here; `.gitignore` excludes every `*.N64` under `krom/`
  and every `N64_BOOTCODE.BIN`. Sources, fonts, reference screenshots and our `.counts` files
  are tracked. To run the gate, obtain PeterLemon/N64 at the commit above yourself and copy
  each `CPUTest/**/*.N64` to the same path under `tests/roms/krom/`, then check them with
  `cd tests/roms/krom && shasum -a 256 -c ROMS.sha256` (114 images, upstream bytes unchanged).
- Local changes: `CP0/COP0Register/COP0Register - before overflow.png` and `... - after
  overflow.png` renamed to `COP0Register-before-overflow.png` / `-after-overflow.png` (the
  manifest is whitespace-separated).
- `CPU/TIMINGNTSC/CPUTIMINGNTSC.counts` and `CP1/TIMINGNTSC/CP1TIMINGNTSC.counts` are ours: the
  hardware loop counts each timing ROM embeds (its `<LABEL>COUNT` words) with the screen row
  they are printed on, extracted from the `.asm` sources for the harness's `kromcount` check.
- Reference screenshots are compared with the framebuffer as the screenshots present it (the
  displayed 640 x 240 region scaled by X/Y_SCALE, resampled nearest-neighbour to the PNG size;
  `Vi::capture_presented`).
