#!/usr/bin/env python3
"""Check that every module's serializer visits every data member (ADR 0003).

For each class in src/ that declares `void serialize(SaveState&)`, every data member it declares
directly (names ending in `_`, the project's member convention) must be named in the body of its
serialize function, unless it is:

- a reference (wiring to another module, fixed at construction); or
- marked as not state, by a comment `// not state: <reason>` either at the end of its line or on
  the line(s) directly above a group of members (the group ends at the first blank line).

Fields of a member's own struct type (`fetch_latch_.vaddr`) are not checked individually.

Exit status 0 when every member is accounted for, 1 otherwise. Run by ctest as
`serialize-coverage`.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SKIP_START = re.compile(
    r"^(static|using|enum|struct|class|friend|return|typedef|template|constexpr|public|private|"
    r"protected|virtual|explicit|inline|if|for|while|switch|case|default|else|do)\b")
MARK = "not state:"


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def block(text, start):
    """The brace-balanced block whose opening brace is the first at or after `start`."""
    i = text.index("{", start)
    depth = 0
    for j in range(i, len(text)):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                return text[i:j + 1]
    raise ValueError("unbalanced braces")


def split_top(s, sep=","):
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch in "<([{":
            depth += 1
        elif ch in ">)]}":
            depth -= 1
        if ch == sep and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    out.append(cur)
    return out


def members(body):
    """(name, line, marked) for each data member declared at the class's own depth."""
    out = []
    depth = 0
    marked_group = False
    for raw in body.splitlines()[1:-1]:
        line = raw.strip()
        at_top = depth == 0
        depth += line.count("{") - line.count("}")
        if not at_top:
            continue
        if not line:
            marked_group = False
            continue
        if line.startswith("//"):
            if MARK in line:
                marked_group = True
            continue
        code, _, comment = raw.partition("//")
        code = code.strip()
        if not code.endswith(";") or SKIP_START.match(code):
            continue
        head = re.split(r"[={]", code, maxsplit=1)[0]
        if "(" in head or "operator" in head:
            continue  # a function declaration
        marked = marked_group or MARK in comment
        parts = split_top(code.rstrip(";"))
        first = re.match(r"(.*?)(\w+_)\s*(\[[^\]]*\])?\s*(=.*|\{.*\})?$", parts[0].strip(), re.S)
        if not first:
            continue
        typ = first.group(1)
        if typ.strip().endswith("&"):
            continue
        names = [first.group(2)]
        for p in parts[1:]:
            m = re.match(r"\s*(\w+_)\b", p)
            if m:
                names.append(m.group(1))
        out.extend((n, line, marked) for n in names)
    return out


def serialize_body(cls, class_body, sources):
    for text in sources:
        m = re.search(rf"\bvoid {cls}::serialize\(SaveState", text)
        if m:
            return block(text, m.start())
    stripped = strip_comments(class_body)
    k = stripped.find("void serialize(SaveState")
    brace, semi = stripped.find("{", k), stripped.find(";", k)
    if brace < 0 or semi < brace:
        return None  # declared here, defined nowhere we can see (or pure virtual)
    return block(stripped, k)


def main():
    sources = [strip_comments(p.read_text()) for p in sorted(ROOT.glob("src/**/*.cpp"))]
    errors, checked = [], 0
    for header in sorted(ROOT.glob("src/**/*.hpp")):
        text = header.read_text()
        for m in re.finditer(r"^(?:class|struct) (\w+)\b[^;{]*\{", text, re.M):
            cls = m.group(1)
            body = block(text, m.start())
            if "void serialize(SaveState" not in body:
                continue
            ser = serialize_body(cls, body, sources)
            if ser is None:
                if "= 0;" not in body:
                    errors.append(f"{header.relative_to(ROOT)}: {cls}::serialize has no body")
                continue
            checked += 1
            for name, line, marked in members(body):
                if marked:
                    continue
                if not re.search(rf"\b{name}\b", ser):
                    errors.append(f"{header.relative_to(ROOT)}: {cls}::{name} is neither "
                                  f"visited by serialize() nor marked `// {MARK} ...`: {line}")
    for e in errors:
        print(e)
    print(f"serialize: {checked} classes, {len(errors)} problems")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
