#!/usr/bin/env python3
"""Export the game's message type table (id -> cMsg class name) from a .gsnap.

The image holds {u32 type id, const char* "cMsg..."} pairs, TU3 at about
0x822EB000-0x822F2000. Message ids appear in the RE probe's census
(`--skate3_harness_re_probe`), which also prints these names at runtime.

  messages.py SNAP [--grep REGEX] [--out out/re/messages.tsv]
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gsnap  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
TABLE_LO, TABLE_HI = 0x822EB000, 0x822F2000


def load(s: gsnap.Snapshot) -> dict[int, str]:
    lo, hi = gsnap.IMAGE_LO, gsnap.IMAGE_HI
    img = s.read(lo, hi - lo)
    names = {lo + m.start(): m.group()[:-1].decode()
             for m in re.finditer(rb"cMsg[A-Za-z0-9_]{3,80}\0", img)}
    w = np.frombuffer(img, ">u4")
    table: dict[int, str] = {}
    for i in range((TABLE_LO - lo) // 4, (TABLE_HI - lo) // 4):
        ptr = int(w[i + 1])
        if ptr in names:
            table.setdefault(int(w[i]), names[ptr])
    return table


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("snap", type=Path)
    p.add_argument("--grep")
    p.add_argument("--out", type=Path, default=REPO / "out/re/messages.tsv")
    a = p.parse_args()
    table = load(gsnap.Snapshot(a.snap))
    a.out.parent.mkdir(parents=True, exist_ok=True)
    with a.out.open("w") as f:
        f.write("# id\tname\n")
        for i, n in sorted(table.items(), key=lambda kv: kv[1]):
            f.write(f"{i:08X}\t{n}\n")
    for i, n in sorted(table.items(), key=lambda kv: kv[1]):
        if a.grep and re.search(a.grep, n, re.I):
            print(f"{i:08X}  {n}")
    print(f"{len(table)} message types -> {a.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
