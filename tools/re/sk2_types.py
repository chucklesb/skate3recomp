#!/usr/bin/env python3
"""Structure layouts from the Skate 2 debug build's PDB (roadmap RE-2, types).

    sk2_types.py index                  # parse llvm-pdbutil's type dump -> out/re/sk2/types.json
    sk2_types.py show NAME [--flat]     # a class's layout (NAME: exact, or a unique substring)
    sk2_types.py find REGEX             # class names matching, with their sizes
    sk2_types.py check                  # the layouts in docs/re/types.md against the PDB

The PDB is Skate 2's: layouts may have changed in Skate 3. A layout goes into
docs/re/types.md only once a probe or a known offset confirms it in TU3
(`check` then keeps it honest against the PDB). `--flat` expands base classes
in place, so offsets are from the start of the object.

Needs `sk2_match.py prepare` (out/re/sk2/sk82_na_m.pdb). Parsing the 130 MB
dump takes about a minute; the index is cached.
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SK2 = REPO / "out/re/sk2"
DUMP = SK2 / "types.txt"
INDEX = SK2 / "types.json"
DOC = REPO / "docs/re/types.md"

REC = re.compile(r"^\s*(0x[0-9A-F]+) \| (LF_\w+) \[size = \d+\](?: `(.*)`)?")
SUB = re.compile(r"^\s*- (LF_\w+)(?: \[(.*)\])?(?: type = (0x[0-9A-F]+))?")


def build_index() -> dict:
    if not DUMP.exists():
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        from sk2_match import llvm_bin
        with open(DUMP, "w") as f:
            subprocess.run([llvm_bin("llvm-pdbutil"), "dump", "-types", str(SK2 / "sk82_na_m.pdb")],
                           stdout=f, check=True)
    recs: dict[str, dict] = {}
    cur = None
    member = None
    for line in open(DUMP, errors="replace"):
        m = REC.match(line)
        if m:
            cur = {"kind": m.group(2), "name": m.group(3) or "", "lines": []}
            recs[m.group(1)] = cur
            member = None
            continue
        if cur is None:
            continue
        s = SUB.match(line)
        if s and cur["kind"] == "LF_FIELDLIST":
            member = {"k": s.group(1), "attrs": s.group(2) or "", "type": s.group(3) or ""}
            cur.setdefault("members", []).append(member)
            continue
        if member is not None and cur["kind"] == "LF_FIELDLIST":
            member["attrs"] += " " + line.strip()
        else:
            cur["lines"].append(line.strip())
    types = {}
    for tid, r in recs.items():
        text = " ".join(r["lines"])
        if r["kind"] in ("LF_CLASS", "LF_STRUCTURE", "LF_UNION"):
            m = re.search(r"sizeof (\d+)", text)
            fl = re.search(r"field list: (0x[0-9A-F]+)", text)
            if "forward ref" in text or not fl:
                types[tid] = {"k": "fwd", "name": r["name"]}
                continue
            types[tid] = {"k": r["kind"][3:].lower(), "name": r["name"],
                          "size": int(m.group(1)) if m else 0, "fields": fl.group(1)}
        elif r["kind"] == "LF_POINTER":
            m = re.search(r"referent = (0x[0-9A-F]+|\S+ \(.*?\))", text)
            types[tid] = {"k": "ptr", "to": m.group(1) if m else ""}
        elif r["kind"] == "LF_MODIFIER":
            m = re.search(r"referent = (0x[0-9A-F]+|\S+ \(.*?\)), modifiers = (\w+)", text)
            types[tid] = {"k": "mod", "to": m.group(1) if m else "", "mod": m.group(2) if m else ""}
        elif r["kind"] == "LF_ARRAY":
            m = re.search(r"size: (\d+), index type: .*?, element type: (0x[0-9A-F]+|\S+ \(.*?\))", text)
            types[tid] = {"k": "arr", "bytes": int(m.group(1)) if m else 0, "of": m.group(2) if m else ""}
        elif r["kind"] == "LF_ENUM":
            types[tid] = {"k": "enum", "name": r["name"]}
        elif r["kind"] == "LF_BITFIELD":
            m = re.search(r"type = (\S+).*bit offset = (\d+), # bits = (\d+)", text)
            types[tid] = {"k": "bits", "of": m.group(1) if m else "", "at": int(m.group(2)) if m else 0,
                          "n": int(m.group(3)) if m else 0}
        elif r["kind"] == "LF_FIELDLIST":
            out = []
            for mem in r.get("members", []):
                a = mem["attrs"]
                if mem["k"] == "LF_MEMBER":
                    nm = re.search(r"name = `(.*?)`", a)
                    ty = re.search(r"Type = (0x[0-9A-F]+(?: \(.*?\))?)", a)
                    off = re.search(r"offset = (\d+)", a)
                    if nm and off:
                        out.append(["m", nm.group(1), ty.group(1) if ty else "?", int(off.group(1))])
                elif mem["k"] == "LF_BCLASS":
                    ty = re.search(r"type = (0x[0-9A-F]+)", a)
                    off = re.search(r"offset = (\d+)", a)
                    if ty and off:
                        out.append(["b", "", ty.group(1), int(off.group(1))])
                elif mem["k"] == "LF_VFUNCTAB":
                    out.append(["v", "__vfptr", "", 0])
            types[tid] = {"k": "fields", "list": out}
    # Name -> the complete definition (largest if several).
    by_name: dict[str, str] = {}
    for tid, t in types.items():
        if t["k"] in ("class", "structure", "union") and t["name"]:
            old = by_name.get(t["name"])
            if old is None or types[old]["size"] < t["size"]:
                by_name[t["name"]] = tid
    # Forward refs -> definitions, so member types resolve.
    for tid, t in types.items():
        if t["k"] == "fwd" and t["name"] in by_name:
            types[tid] = {"k": "alias", "to": by_name[t["name"]], "name": t["name"]}
    idx = {"types": types, "by_name": by_name}
    INDEX.write_text(json.dumps(idx))
    print(f"{len(by_name)} classes/structs/unions -> {INDEX}")
    return idx


def load() -> dict:
    if not INDEX.exists():
        return build_index()
    return json.loads(INDEX.read_text())


PRIMITIVE = {0x03: "void", 0x10: "char", 0x20: "unsigned char", 0x68: "int8", 0x69: "uint8",
             0x11: "short", 0x21: "unsigned short", 0x12: "long", 0x22: "unsigned long",
             0x13: "__int64", 0x23: "unsigned __int64", 0x74: "int", 0x75: "unsigned",
             0x40: "float", 0x41: "double", 0x30: "bool", 0x71: "wchar_t", 0x7A: "char16_t"}


def tname(idx: dict, ref: str, depth: int = 0) -> str:
    """A readable type name for a type reference."""
    if not ref.startswith("0x") or " (" in ref:
        m = re.search(r"\((.*)\)", ref)
        return m.group(1) if m else ref
    v = int(ref, 16)
    if v < 0x1000:  # a primitive: low byte = type, mode nibble 4 = 32-bit pointer
        base = PRIMITIVE.get(v & 0xFF, ref)
        return base + ("*" if (v >> 8) & 0xF else "")
    t = idx["types"].get(ref)
    if t is None or depth > 6:
        return ref
    k = t["k"]
    if k in ("class", "structure", "union", "enum", "fwd", "alias"):
        return t["name"]
    if k == "ptr":
        return tname(idx, t["to"], depth + 1) + "*"
    if k == "mod":
        return f"{t['mod']} {tname(idx, t['to'], depth + 1)}"
    if k == "arr":
        return f"{tname(idx, t['of'], depth + 1)}[{t['bytes']}B]"
    if k == "bits":
        return f"{tname(idx, t['of'], depth + 1)}:{t['n']}@{t['at']}"
    return k


def resolve(idx: dict, name: str) -> str | None:
    if name in idx["by_name"]:
        return idx["by_name"][name]
    hits = [n for n in idx["by_name"] if n.endswith("::" + name) or n == name]
    if not hits:
        hits = [n for n in idx["by_name"] if name in n]
    if len(hits) == 1:
        return idx["by_name"][hits[0]]
    if hits:
        print("ambiguous: " + ", ".join(sorted(hits, key=len)[:12]))
    return None


def layout(idx: dict, tid: str, flat: bool, base: int = 0, prefix: str = "") -> list[tuple[int, str, str]]:
    t = idx["types"][tid]
    rows = []
    for k, nm, ty, off in idx["types"].get(t["fields"], {}).get("list", []):
        if k == "b":
            bt = idx["types"].get(ty, {})
            if bt.get("k") == "alias":
                ty = bt["to"]
            if flat and idx["types"].get(ty, {}).get("k") in ("class", "structure"):
                rows += layout(idx, ty, flat, base + off, prefix)
            else:
                rows.append((base + off, f"(base) {tname(idx, ty)}", ""))
        elif k == "v":
            rows.append((base, "__vfptr", "vtable*"))
        else:
            rows.append((base + off, prefix + nm, tname(idx, ty)))
    rows.sort(key=lambda r: r[0])
    return rows


def cmd_show(a) -> int:
    idx = load()
    tid = resolve(idx, a.name)
    if not tid:
        print(f"no class {a.name!r}")
        return 1
    t = idx["types"][tid]
    print(f"{t['name']}  sizeof 0x{t['size']:X} ({t['size']})")
    for off, nm, ty in layout(idx, tid, a.flat):
        print(f"  +0x{off:04X}  {nm:<40} {ty}")
    return 0


def cmd_find(a) -> int:
    idx = load()
    rx = re.compile(a.regex)
    hits = sorted(n for n in idx["by_name"] if rx.search(n))
    for n in hits[:200]:
        print(f"0x{idx['types'][idx['by_name'][n]]['size']:5X}  {n}")
    if len(hits) > 200:
        print(f"... {len(hits) - 200} more")
    return 0 if hits else 1


ROW = re.compile(r"^\|\s*`?([\w:<>, ]+?)`?\s*\|\s*`?\+?0x([0-9A-Fa-f]+)`?\s*\|\s*`?(\w+)`?\s*\|")


def cmd_check(a) -> int:
    """Every `| Class | +0xOFF | field |` row in types.md must match the PDB
    (the field at that offset in the Skate 2 layout, flattened)."""
    idx = load()
    bad = rows = 0
    cls = None
    for line in DOC.read_text().splitlines():
        h = re.match(r"^### `?([\w:]+)`?", line)
        if h:
            cls = h.group(1)
            continue
        m = re.match(r"^\|\s*`?\+?0x([0-9A-Fa-f]+)`?\s*\|\s*`(\w+)`", line)
        if not (m and cls):
            continue
        rows += 1
        tid = resolve(idx, cls)
        if not tid:
            print(f"{cls}: not in the PDB")
            bad += 1
            continue
        off, field = int(m.group(1), 16), m.group(2)
        found = [nm for o, nm, _ in layout(idx, tid, True) if o == off]
        if field not in found:
            print(f"{cls} +0x{off:X} {field}: PDB has {found or 'nothing'} there")
            bad += 1
    print(f"{rows} rows checked, {bad} problem(s)")
    return 1 if bad else 0


def main() -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("index")
    p = sub.add_parser("show")
    p.add_argument("name")
    p.add_argument("--flat", action="store_true")
    p = sub.add_parser("find")
    p.add_argument("regex")
    sub.add_parser("check")
    a = ap.parse_args()
    if a.cmd == "index":
        build_index()
        return 0
    return {"show": cmd_show, "find": cmd_find, "check": cmd_check}[a.cmd](a)


if __name__ == "__main__":
    raise SystemExit(main())
