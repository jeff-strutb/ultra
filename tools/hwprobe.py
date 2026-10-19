#!/usr/bin/env python3
"""Decode and compare hwprobe records (docs/timing/hwprobe.md).

  hwprobe.py dump SAV [--all]        list every series, check CRCs, summarise
  hwprobe.py json SAV OUT            write every decoded series to OUT
  hwprobe.py compare HW EMU          series-by-series differences, HW being the oracle
  hwprobe.py loops SAV               uncached-read loop times in MasterClocks

SAV is the 128 KiB FlashRAM image: a flashcart's .sav, or ultra_harness hwprobe's output.
Series names and layouts come from tests/probes/hwprobe/series.h.
"""
import json
import re
import struct
import sys
import zlib
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SERIES_H = ROOT / "tests/probes/hwprobe/series.h"

STAGES = ((0x00000, 0x1C000, b"ULTRAHW1"), (0x1C000, 0x04000, b"ULTRAHW2"))
PACK_NONE, PACK_DELTAS, PACK_VALUES = 0, 1, 2
# Count ticks every 2 PCycles, PCycle = 1.5 MasterClocks (n64brew "ROM Header", Clock Rate).
MC_PER_TICK = 4.0 / 3.0
VI_MODES = ("VI off", "240p", "480i")
REGS = ("VI_V_CURRENT", "MI_VERSION", "SP_STATUS", "PI_STATUS")
OPS = ("ALU", "MUL.S 0", "ADD.S", "MULT", "DMULT", "DIV", "DDIV")


def load_names():
    names = {}
    for line in SERIES_H.read_text().splitlines():
        m = re.match(r"#define (S_\w+)\s+0x([0-9A-Fa-f]+)\s*//\s*(\d+)\s*(.*)", line)
        if m:
            names[int(m.group(2), 16)] = (m.group(1), int(m.group(3)), m.group(4))
    return names


NAMES = load_names()


def series_name(sid):
    if 0x3000 <= sid < 0x4000:
        m, r, o = (sid >> 8) & 0xF, (sid >> 4) & 0xF, sid & 0xF
        return f"S_LOOP_RAW[{VI_MODES[m]}, {REGS[r]}, {OPS[o]}]"
    if 0x4000 <= sid < 0x40F0:
        m, o = (sid >> 4) & 0xF, sid & 0xF
        return f"S_LOOP_LINE[{VI_MODES[m]}, {OPS[o]}]"
    return NAMES.get(sid, (f"0x{sid:04X}",))[0]


def normalise(raw):
    """Return the image in the CPU's big-endian byte order."""
    if raw[:8] in (b"ULTRAHW1",) or raw[:4] == b"\xff\xff\xff\xff":
        return raw
    if raw[:8] == b"LURTHA1W":  # 16-bit swapped
        return b"".join(raw[i + 1:i + 2] + raw[i:i + 1] for i in range(0, len(raw), 2))
    if raw[:8] == b"RTLU1WHA":  # 32-bit little endian
        return b"".join(raw[i:i + 4][::-1] for i in range(0, len(raw), 4))
    return raw


def unpack(mode, count, payload):
    first, base, nesc = payload[0], payload[1], struct.unpack(">H", payload[2:4])[0]
    nitems = count - 1 if mode == PACK_DELTAS else count
    nib_bytes = (nitems + 1) // 2
    nibs = payload[4:4 + nib_bytes]
    esc = payload[4 + nib_bytes:4 + nib_bytes + nesc]
    items, e = [], 0
    for i in range(nitems):
        q = (nibs[i >> 1] >> (0 if i & 1 else 4)) & 0xF
        if q == 15:
            items.append(esc[e])
            e += 1
        else:
            items.append((q + base) & 0xFF)
    if mode == PACK_DELTAS:
        out = [first]
        for d in items:
            out.append((out[-1] + d) & 0xFF)
        return out
    return items


def packed_len(mode, count, payload):
    nitems = count - 1 if mode == PACK_DELTAS else count
    nesc = struct.unpack(">H", payload[2:4])[0]
    return 4 + (nitems + 1) // 2 + nesc


def parse_area(img, base, size, magic):
    area = img[base:base + size]
    hdr = area[:64]
    info = {"magic": hdr[:8], "complete": hdr[:8] == magic}
    if info["complete"]:
        ver, nbytes, crc, nseries, status = struct.unpack(">IIIII", hdr[8:28])
        info.update(version=ver, bytes=nbytes, crc=crc, nseries=nseries, status=status,
                    build=hdr[28:44].rstrip(b"\0").decode(errors="replace"),
                    silicon=struct.unpack(">Q", hdr[44:52])[0])
        info["crc_ok"] = zlib.crc32(area[64:64 + nbytes]) == crc
    elif hdr != b"\xff" * 64:
        info["error"] = "header neither valid nor erased"
    series, pos = [], 64
    end = 64 + info["bytes"] if info["complete"] else size
    while pos + 12 <= end:
        sid, esize, flags, count, crc = struct.unpack(">HBBII", area[pos:pos + 12])
        if sid == 0xFFFF:
            break
        data = area[pos + 12:]
        dlen = esize * count if flags == PACK_NONE else packed_len(flags, count, data)
        data = data[:dlen]
        s = {"id": sid, "name": series_name(sid), "esize": esize, "flags": flags,
             "count": count, "crc_ok": zlib.crc32(data) == crc}
        if flags == PACK_NONE:
            fmt = {1: "B", 2: "H", 4: "I", 8: "Q"}[esize]
            s["values"] = list(struct.unpack(f">{count}{fmt}", data))
        else:
            try:
                s["values"] = unpack(flags, count, data)
            except IndexError:
                s["values"], s["crc_ok"] = [], False
        series.append(s)
        pos += 12 + ((dlen + 3) & ~3)
    info["series"] = series
    return info


def parse(path):
    img = normalise(Path(path).read_bytes())
    if len(img) < 0x20000:
        raise SystemExit(f"{path}: {len(img)} bytes, expected 131072 (FlashRAM)")
    return [parse_area(img, b, s, m) for b, s, m in STAGES]


def deltas(vals):
    return [(b - a) & 0xFF for a, b in zip(vals, vals[1:])]


def describe(s):
    v = s["values"]
    if 0x3000 <= s["id"] < 0x4000:
        d = deltas(v)
        c = Counter(d)
        mean = sum(d) / len(d) if d else 0
        top = ", ".join(f"{k}x{n}" for k, n in c.most_common(4))
        return f"{len(d)} deltas, mean {mean:.3f} ticks = {mean * MC_PER_TICK:.3f} MC; {top}"
    if 0x4000 <= s["id"] < 0x40F0:
        c = Counter(v)
        return "per-half-line iterations " + ", ".join(f"{k}x{n}" for k, n in c.most_common(4))
    if len(v) <= 8:
        w = s["esize"] * 2
        return " ".join(f"{x:0{w}X}" for x in v)
    return f"{len(v)} values, min {min(v)}, max {max(v)}"


def cmd_dump(path, show_all=False):
    bad = 0
    for st, info in zip((1, 2), parse(path)):
        if not info["complete"] and not info["series"]:
            print(f"stage {st}: not run")
            continue
        if info["complete"]:
            print(f"stage {st}: build {info['build']}, {info['nseries']} series, {info['bytes']} B, "
                  f"crc {'ok' if info['crc_ok'] else 'BAD'}, status {info['status']:#x}, "
                  f"FlashRAM {info['silicon']:016X}")
            bad += not info["crc_ok"]
        else:
            print(f"stage {st}: header missing (stopped early); {len(info['series'])} series found")
        for s in info["series"]:
            bad += not s["crc_ok"]
            if show_all or not (0x3000 <= s["id"] < 0x40F0):
                print(f"  {s['id']:04X} {s['name']:<44} {'' if s['crc_ok'] else 'CRC BAD '}{describe(s)}")
    return 1 if bad else 0


def cmd_json(path, out):
    stages = parse(path)
    for info in stages:
        info["magic"] = info["magic"].decode(errors="replace")
    Path(out).write_text(json.dumps(stages, indent=1))
    return 0


def cmd_loops(path):
    info = parse(path)[0]
    for s in info["series"]:
        if 0x3000 <= s["id"] < 0x4000:
            print(f"{s['name']:<52} {describe(s)}")
    return 0


def cmd_compare(hw_path, emu_path):
    hw, emu = parse(hw_path), parse(emu_path)
    diff = same = 0
    for st, (h, e) in enumerate(zip(hw, emu), 1):
        es = {s["id"]: s for s in e["series"]}
        for s in h["series"]:
            o = es.get(s["id"])
            if o is None:
                print(f"stage {st} {s['id']:04X} {s['name']}: missing from the emulator record")
                diff += 1
                continue
            if o["values"] == s["values"]:
                same += 1
                continue
            diff += 1
            print(f"stage {st} {s['id']:04X} {s['name']}")
            print(f"    hardware: {describe(s)}")
            print(f"    emulator: {describe(o)}")
            if not (0x3000 <= s["id"] < 0x40F0):
                n = 0
                for i, (a, b) in enumerate(zip(s["values"], o["values"])):
                    if a != b:
                        print(f"    [{i}] hw {a:#x} emu {b:#x}")
                        n += 1
                        if n == 6:
                            break
    print(f"{same} series identical, {diff} differ")
    return 1 if diff else 0


def main(argv):
    if len(argv) >= 3 and argv[1] == "dump":
        return cmd_dump(argv[2], "--all" in argv)
    if len(argv) == 4 and argv[1] == "json":
        return cmd_json(argv[2], argv[3])
    if len(argv) == 3 and argv[1] == "loops":
        return cmd_loops(argv[2])
    if len(argv) == 4 and argv[1] == "compare":
        return cmd_compare(argv[2], argv[3])
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
