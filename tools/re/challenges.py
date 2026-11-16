#!/usr/bin/env python3
"""Skate 3's 64-bit name hash, and the career's challenge lists by name.

    challenges.py hash NAME ...   # the game's hash of each name
    challenges.py show SNAP       # a career snapshot's challenges and locked actions, named

The hash (sub_82B73AC0) is Bob Jenkins' lookup8 with a = b = 0xABCDEF0011223344
(c = the golden ratio); challenge, unlock and data names are keyed by it
(docs/re/career.md). `show` reads the challenge tracker's lists (active,
Owned, Killed) and the locked unlock actions from a career snapshot (harness
`snapshot`) and names them from docs/re/attrib_names.tsv (tools/re/attrib.py).
"""
from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
M = (1 << 64) - 1


def _mix(a: int, b: int, c: int) -> tuple[int, int, int]:
    for sa, sb, sc in ((43, 9, 8), (38, 23, 5), (35, 49, 11), (12, 18, 22)):
        a = (a - b - c) & M; a ^= c >> sa
        b = (b - c - a) & M; b ^= (a << sb) & M
        c = (c - a - b) & M; c ^= b >> sc
    return a, b, c


def name_hash(k: bytes) -> int:
    """The game's 64-bit name hash (lookup8, a = b = 0xABCDEF0011223344)."""
    n = len(k)
    a = b = 0xABCDEF0011223344
    c = 0x9E3779B97F4A7C13
    i = 0
    while n - i >= 24:
        a = (a + int.from_bytes(k[i:i + 8], "little")) & M
        b = (b + int.from_bytes(k[i + 8:i + 16], "little")) & M
        c = (c + int.from_bytes(k[i + 16:i + 24], "little")) & M
        a, b, c = _mix(a, b, c)
        i += 24
    c = (c + n) & M
    r = k[i:]
    for j in range(len(r) - 1, -1, -1):
        if j >= 16:
            c = (c + (r[j] << (8 * (j - 15)))) & M
        elif j >= 8:
            b = (b + (r[j] << (8 * (j - 8)))) & M
        else:
            a = (a + (r[j] << (8 * j))) & M
    return _mix(a, b, c)[2]


def tracker_lists(snap_path: Path) -> dict[str, list[int]]:
    sys.path.insert(0, str(REPO / "tools/re"))
    import gsnap  # noqa: E402

    s = gsnap.Snapshot(snap_path)

    def u(a: int) -> int:
        v = gsnap.u32_at(s, a)
        if v is None:
            raise SystemExit(f"{snap_path}: 0x{a:08X} not in the snapshot (not in a career?)")
        return v

    k = u(u(0x830CFD94) + 0xE0)  # the career object
    c = u(k + 0x44)              # the challenge tracker
    out = {}
    a = u(k + 0x40)              # the actions tracker
    for base, off, name in ((c, 0x04, "active"), (c, 0x14, "owned"), (c, 0x24, "killed"),
                            (a, 0x04, "locked actions")):
        head = base + off
        n = u(head)
        hs = []
        while n != head and len(hs) < 2000:
            hs.append((u(n + 8) << 32) | u(n + 12))
            n = u(n)
        out[name] = hs
    return out


def load_known() -> dict[int, str]:
    sys.path.insert(0, str(REPO / "tools/re"))
    import attrib  # noqa: E402
    return {h: name for h, (kind, cls, name) in attrib.load().items()}


def cmd_show(a) -> int:
    known = load_known()
    for k, hs in tracker_lists(Path(a.snap)).items():
        print(f"{k} ({len(hs)}):")
        print("  " + ", ".join(sorted(known.get(h, f"{h:016X}") for h in hs)))
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("hash")
    p.add_argument("names", nargs="+")
    p = sub.add_parser("show")
    p.add_argument("snap")
    a = ap.parse_args()
    if a.cmd == "hash":
        for n in a.names:
            print(f"{name_hash(n.encode()):016X}  {n}")
        return 0
    return cmd_show(a)


if __name__ == "__main__":
    raise SystemExit(main())
