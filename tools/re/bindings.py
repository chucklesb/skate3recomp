#!/usr/bin/env python3
"""Skate 3's Lua script bindings: every {0, 2, "name", lua_CFunction} table in
the TU3 image (roadmap RE-3).

    bindings.py scan [--tsv out/re/bindings.tsv]   # every table and entry
    bindings.py export                             # -> docs/re/names_bindings.tsv
    bindings.py lookup NAME|ADDR                   # search the scan

Script bindings are 16-byte entries {u32 0, u32 2, const char* name,
lua_CFunction} (src/harness/harness_re.cpp), in runs that form one table per
script library. Each function is named by its binding: the game's own name
for the C function behind a script call (e.g. SetSessionMarker). A table's
library name comes from the function that builds the table's address and the
strings that same function loads (the registration call).

Input: the TU3 image and function list from tools/re/ghidra/setup.py
(out/re/ghidra/). Writes out/re/bindings.tsv; `export` merges into
docs/re/names_bindings.tsv (names only).
"""
from __future__ import annotations

import argparse
import collections
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TU3_BIN = REPO / "out/re/ghidra/skate3_tu3.bin"
TU3_FUNCS = REPO / "out/re/ghidra/functions.txt"
OUT = REPO / "out/re/bindings.tsv"
NAMES = REPO / "docs/re/names_bindings.tsv"
BASE = 0x82000000


def cstr(img: bytes, addr: int, limit: int = 96) -> str | None:
    off = addr - BASE
    if not 0 <= off < len(img):
        return None
    s = img[off:off + limit].split(b"\0", 1)[0]
    if not s or not all(32 <= c < 127 for c in s):
        return None
    return s.decode()


def scan_tables(img: bytes, funcs: set[int]) -> list[tuple[int, list[tuple[str, int]]]]:
    n = len(img) // 4
    w = struct.unpack(f">{n}I", img[:n * 4])
    tables, i = [], 0
    while i + 4 <= n:
        entries = []
        j = i
        while j + 4 <= n and w[j] == 0 and 1 <= w[j + 1] <= 15 and w[j + 3] in funcs:
            name = cstr(img, w[j + 2])
            if name is None:
                break
            entries.append((name, w[j + 3], w[j + 1]))
            j += 4
        if len(entries) >= 1:
            tables.append((BASE + i * 4, entries))
            i = j
        else:
            i += 1
    return tables


def address_builders(img: bytes, starts: list[int], targets: set[int]) -> dict[int, list[int]]:
    """target address -> functions that build it with lis + addi, plus the
    strings each such function loads (for the library name)."""
    n = len(img) // 4
    w = struct.unpack(f">{n}I", img[:n * 4])
    out: dict[int, list[int]] = collections.defaultdict(list)
    for k, a in enumerate(starts):
        end = starts[k + 1] if k + 1 < len(starts) else a + 4
        hi: dict[int, int] = {}
        for i in range((a - BASE) // 4, min((end - BASE) // 4, n)):
            x = w[i]
            op, rd, ra = x >> 26, (x >> 21) & 31, (x >> 16) & 31
            if op == 15 and ra == 0:
                hi[rd] = (x & 0xFFFF) << 16
            elif op == 14 and ra in hi:
                # addi: the value carries on (base register + offset chains)
                lo = x & 0xFFFF
                lo = lo - 0x10000 if lo & 0x8000 else lo
                t = (hi[ra] + lo) & 0xFFFFFFFF
                hi[rd] = t
                if t in targets:
                    out[t].append(a)
    return out


def strings_of(img: bytes, a: int, end: int) -> list[str]:
    n = len(img) // 4
    w = struct.unpack(f">{n}I", img[:n * 4])
    hi: dict[int, int] = {}
    out = []
    for i in range((a - BASE) // 4, min((end - BASE) // 4, n)):
        x = w[i]
        op, rd, ra = x >> 26, (x >> 21) & 31, (x >> 16) & 31
        if op == 15 and ra == 0:
            hi[rd] = (x & 0xFFFF) << 16
        elif op == 14 and ra in hi:
            lo = x & 0xFFFF
            lo = lo - 0x10000 if lo & 0x8000 else lo
            s = cstr(img, (hi[ra] + lo) & 0xFFFFFFFF)
            if s and len(s) >= 3:
                out.append(s)
    return out


def cmd_scan(a) -> int:
    img = TU3_BIN.read_bytes()
    starts = sorted(int(l.split()[0], 16) for l in open(TU3_FUNCS))
    funcs = set(starts)
    tables = scan_tables(img, funcs)
    builders = address_builders(img, starts, {t for t, _ in tables})
    nxt = {a: starts[i + 1] for i, a in enumerate(starts[:-1])}
    rows = []
    for addr, entries in tables:
        regs = builders.get(addr, [])
        lib = ""
        if regs:
            names = {e[0] for e in entries}
            cands = [s for s in strings_of(img, regs[0], nxt.get(regs[0], regs[0] + 4))
                     if s not in names]
            lib = cands[0] if cands else ""
        for name, fn, kind in entries:
            rows.append((fn, name, lib, addr, regs[0] if regs else 0, kind))
    with open(a.tsv, "w") as f:
        f.write("# function\tbinding\tlibrary (guess)\ttable\tregistered by\tentry type\n")
        for fn, name, lib, addr, reg, kind in rows:
            f.write(f"{fn:08X}\t{name}\t{lib}\t{addr:08X}\t{reg:08X}\t{kind}\n")
    funcs_named = len({r[0] for r in rows})
    print(f"{len(tables)} tables, {len(rows)} bindings, {funcs_named} distinct functions -> {a.tsv}")
    big = sorted(tables, key=lambda t: -len(t[1]))[:15]
    for addr, entries in big:
        lib = next((r[2] for r in rows if r[3] == addr), "")
        print(f"  {addr:08X}  {len(entries):3} entries  lib {lib!r:24}  e.g. "
              + ", ".join(e[0] for e in entries[:4]))
    return 0


def cmd_export(a) -> int:
    """docs/re/names_bindings.tsv: one line per function. A function bound under
    several names is a shared stub (debug bindings compiled out to one
    function): all names, joined with '|'."""
    names: dict[str, list[tuple[str, str]]] = {}
    for line in open(OUT):
        if line.startswith("#"):
            continue
        fn, name, _lib, table, _reg, kind = line.rstrip("\n").split("\t")
        names.setdefault(fn, []).append((name, table))
        names[fn][-1] = (name + (" (enum)" if kind == "3" else ""), table)
    with open(NAMES, "w") as f:
        f.write("# Skate 3 (TU3) functions named by the game's own Lua script bindings\n"
                "# ({0, type, \"name\", function} tables in the image; tools/re/bindings.py,\n"
                "# roadmap RE-3). The name is the script-facing name of the C function\n"
                "# behind it; '(enum)' = the function that exports that enum to scripts.\n"
                "# Several names on one function = a shared stub. Priority: symbols.toml,\n"
                "# then this file, then names_sk2.tsv.\n"
                "# address\tbinding name(s)\ttable\n")
        for fn in sorted(names):
            ns = names[fn]
            f.write(f"{fn}\t{'|'.join(n for n, _ in ns)}\t{ns[0][1]}\n")
    stubs = sum(1 for v in names.values() if len(v) > 1)
    print(f"{len(names)} functions ({stubs} shared stubs) -> {NAMES}")
    return 0


def cmd_lookup(a) -> int:
    q = a.q.upper().removeprefix("SUB_").removeprefix("0X")
    hits = 0
    for line in open(OUT):
        if line.startswith("#"):
            continue
        fn, name, lib, table, reg, _kind = line.rstrip("\n").split("\t")
        if a.q.lower() in name.lower() or q == fn:
            print(f"sub_{fn}  {lib + '.' if lib else ''}{name}  (table {table})")
            hits += 1
    return 0 if hits else 1


def main() -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("scan")
    p.add_argument("--tsv", type=Path, default=OUT)
    sub.add_parser("export")
    p = sub.add_parser("lookup")
    p.add_argument("q")
    a = ap.parse_args()
    return {"scan": cmd_scan, "export": cmd_export, "lookup": cmd_lookup}[a.cmd](a)


if __name__ == "__main__":
    raise SystemExit(main())
