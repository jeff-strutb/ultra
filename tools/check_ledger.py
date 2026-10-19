#!/usr/bin/env python3
"""Check that docs/timing/ledger.md, tests/roms/manifest.txt and the source agree.

- Every manifest entry expected to `fail` names a ledger key (`# ledger <key>`) that is a row
  of the ledger's "Expected-fail manifest entries" table, and every row of that table is named
  by at least one manifest entry. With the harness failing on any unexpected outcome, this makes
  the ledger exact: every known failure is explained, and every explanation is still needed.
- Every `ledger "<key>"` reference in src/, tests/ and tools/ names a row of the ledger.

Exit status 0 when consistent, 1 otherwise. Run by ctest as `ledger-exact`.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


def table_keys(text):
    return {m.group(1) for m in re.finditer(r"^\| `([^`]+)`", text, re.M)}


def main():
    ledger = (ROOT / "docs/timing/ledger.md").read_text()
    params, expected = ledger.split("## Expected-fail", 1)
    param_keys, fail_keys = table_keys(params), table_keys(expected)
    errors = []

    used = set()
    for n, line in enumerate((ROOT / "tests/roms/manifest.txt").read_text().splitlines(), 1):
        fields = line.split("#", 1)[0].split()
        if len(fields) < 3:
            continue
        tag = re.search(r"#.*\bledger (\S+)", line)
        if fields[1] == "fail":
            if not tag:
                errors.append(f"manifest:{n}: {fields[0]} is expected to fail but names no ledger key")
            elif tag.group(1) not in fail_keys:
                errors.append(f"manifest:{n}: {fields[0]} names `{tag.group(1)}`, not an expected-fail row")
            else:
                used.add(tag.group(1))
        elif tag:
            errors.append(f"manifest:{n}: {fields[0]} is expected to pass but names ledger key `{tag.group(1)}`")
    for key in sorted(fail_keys - used):
        errors.append(f"ledger: expected-fail row `{key}` is named by no failing manifest entry")

    for path in sorted(p for d in ("src", "tests", "tools") for p in (ROOT / d).rglob("*") if p.is_file()):
        if path.suffix not in {".cpp", ".hpp", ".h", ".py", ".txt"} or path.name == "check_ledger.py":
            continue
        for n, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
            for key in re.findall(r'ledger "([a-z0-9-]+)"', line):
                if key not in param_keys | fail_keys:
                    errors.append(f"{path.relative_to(ROOT)}:{n}: ledger \"{key}\" is not a ledger row")

    for e in errors:
        print(e)
    print(f"ledger: {len(param_keys)} parameters, {len(fail_keys)} expected-fail rows, "
          f"{len(errors)} problems")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
