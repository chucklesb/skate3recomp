#!/usr/bin/env python3
"""The reverse-engineering function map (docs/re/symbols.toml).

    re_symbols.py lookup <sub_XXXXXXXX | XXXXXXXX | name text>
    re_symbols.py coverage [--all]
    re_symbols.py check

lookup: entries whose address or name matches (name text: case-insensitive
substring).
coverage: game functions referenced in src/ (sub_8XXXXXXX) that have no
map entry, most-referenced first, with the files that use them (--all lists
every one, default 40).
check: the map parses, every key is sub_ + 8 hex digits, every entry has a
name and a valid confidence.
"""
from __future__ import annotations

import argparse
import collections
import re
import sys
import tomllib
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
MAP = REPO / "docs/re/symbols.toml"
CONFIDENCE = {"confirmed", "likely", "guess"}
SUB = re.compile(r"\bsub_(8[0-9A-Fa-f]{7})\b")


def load() -> dict:
    with open(MAP, "rb") as f:
        return tomllib.load(f)


def show(key: str, e: dict) -> None:
    print(f"{key}  {e.get('name', '?')}  [{e.get('confidence', '?')}]")
    for field in ("signature", "role", "evidence", "hooked"):
        if field in e:
            print(f"    {field}: {e[field]}")


def main() -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("lookup")
    p.add_argument("query")
    p = sub.add_parser("coverage")
    p.add_argument("--all", action="store_true")
    sub.add_parser("check")
    a = ap.parse_args()
    syms = load()

    if a.cmd == "lookup":
        q = a.query.removeprefix("sub_").upper()
        hits = [(k, e) for k, e in syms.items()
                if k.upper() == f"SUB_{q}" or a.query.lower() in e.get("name", "").lower()]
        for k, e in hits:
            show(k, e)
        return 0 if hits else 1

    if a.cmd == "coverage":
        refs = collections.defaultdict(set)
        for path in (REPO / "src").rglob("*"):
            if path.suffix in (".cpp", ".h", ".hpp", ".inc"):
                for m in SUB.finditer(path.read_text(errors="replace")):
                    refs[f"sub_{m.group(1).upper()}"].add(str(path.relative_to(REPO)))
        known = {k.upper() for k in syms}
        missing = sorted((k for k in refs if k.upper() not in known),
                         key=lambda k: (-len(refs[k]), k))
        print(f"{len(refs)} game functions referenced in src/, {len(refs) - len(missing)} in the "
              f"map, {len(missing)} missing")
        for k in missing if a.all else missing[:40]:
            files = sorted(refs[k])
            print(f"  {k}  {len(files)} file(s): {', '.join(files[:3])}{' ...' if len(files) > 3 else ''}")
        return 0

    bad = 0
    for k, e in syms.items():
        if not re.fullmatch(r"sub_8[0-9A-F]{7}", k):
            print(f"bad key {k!r} (want sub_ + 8 upper-case hex digits)")
            bad += 1
        if not e.get("name"):
            print(f"{k}: no name")
            bad += 1
        if e.get("confidence") not in CONFIDENCE:
            print(f"{k}: confidence {e.get('confidence')!r} not in {sorted(CONFIDENCE)}")
            bad += 1
    print(f"{len(syms)} entries, {bad} problem(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
