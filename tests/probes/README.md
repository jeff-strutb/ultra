# Probe ROMs

Our own test ROMs, built with libdragon (MIT; SPEC.md, Third-party code). The built `.z64` is
tracked so that the gate runs without the MIPS toolchain.

- `hwprobe/`: the console measurement ROM. What it measures, how to run it on a console and
  the record format: `docs/timing/hwprobe.md`.

## Toolchain

libdragon publishes no macOS toolchain, so build it from source (about 15 minutes on an Apple
M-series machine):

```bash
git clone https://github.com/DragonMinded/libdragon.git third_party/libdragon
export N64_INST=$PWD/third_party/n64inst
mkdir -p third_party/toolchain-build
BUILD_PATH=$PWD/third_party/toolchain-build bash third_party/libdragon/tools/build-toolchain.sh
make -C third_party/libdragon -j8 libdragon tools install tools-install
```

The tracked ROM was built with libdragon e356bf3f56f7 (2026-09-15) and GCC 16.2.0.

## Building and running

```bash
make -C tests/probes/hwprobe N64_INST=$PWD/third_party/n64inst
build/release/tests/ultra_harness hwprobe tests/probes/hwprobe/hwprobe.z64 /tmp/emu.sav
python3 tools/hwprobe.py dump /tmp/emu.sav
python3 tools/hwprobe.py compare console.sav /tmp/emu.sav
```
