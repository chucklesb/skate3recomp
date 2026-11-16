#!/usr/bin/env python3
"""The front end's screen table: FE screen id -> screen name and Flash movie.

    fe_screens.py export   # writes docs/re/fe_screens.tsv
    fe_screens.py show [ID ...]

The table is static data in the TU3 image at 0x8302F490: 16-byte entries
{u32 0|1, u32 -1, char* name, char* swf}, indexed by the id the FE push
functions take (sub_82D0ABE0 etc.; harness `fe` stacks print these ids).
Confirmed on 64-66 (MilestoneUnlock, TeammateUnlock, FirstTeamMember pushed
by the career code, 2026-11-15). Needs out/re/ghidra/skate3_tu3.bin
(tools/re/ghidra/setup.py).
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
IMAGE = REPO / "out/re/ghidra/skate3_tu3.bin"
OUT = REPO / "docs/re/fe_screens.tsv"
BASE = 0x82000000
TABLE = 0x8302F490


def read_table() -> list[tuple[int, int, str, str]]:
    d = IMAGE.read_bytes()

    def cstr(va: int) -> str | None:
        if not BASE <= va < BASE + len(d):
            return None
        o = va - BASE
        return d[o:d.index(b"\0", o)].decode("latin1")

    rows = []
    i = 0
    while True:
        o = TABLE + 16 * i - BASE
        flag, minus1, name, swf = struct.unpack(">4I", d[o:o + 16])
        n, s = cstr(name), cstr(swf)
        if minus1 != 0xFFFFFFFF or n is None or s is None:
            break
        rows.append((i, flag, n, s))
        i += 1
    return rows


def main() -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("export")
    p = sub.add_parser("show")
    p.add_argument("ids", nargs="*", type=int)
    a = ap.parse_args()
    if not IMAGE.exists():
        print(f"missing {IMAGE} (run tools/re/ghidra/setup.py)", file=sys.stderr)
        return 1
    rows = read_table()
    if a.cmd == "export":
        with open(OUT, "w") as f:
            f.write("# FE screen id\tname\tflash movie\tflag (tools/re/fe_screens.py, table 0x8302F490)\n")
            for i, flag, n, s in rows:
                f.write(f"{i}\t{n}\t{s}\t{flag}\n")
        print(f"{len(rows)} screens -> {OUT.relative_to(REPO)}")
        return 0
    for i, flag, n, s in rows:
        if not a.ids or i in a.ids:
            print(f"{i:3d}  {n:28s} {s}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
