#!/usr/bin/env python3
"""Skate 3's 64-bit name hash, and the career's challenge names.

    challenges.py hash NAME ...             # the game's hash of each name
    challenges.py names SNAP [--game DIR]   # name the challenges a career tracks
    challenges.py show SNAP                 # a snapshot's challenge lists, named

The hash (sub_82B73AC0) is Bob Jenkins' lookup8 with a = b = 0xABCDEF0011223344
(c = the golden ratio); challenge and unlock names are keyed by it
(docs/re/career.md). `names` reads the challenge tracker's lists (active,
Owned, Killed) from a career snapshot (harness `snapshot`), takes the readable
identifiers in the game data (data/big/db.big, miscload.big; most of it is
compressed), learns the <type>_<district>_<NN>[_<suffix>] pattern from them,
tries every combination, and writes the names that match tracked hashes to
docs/re/challenges.tsv (names only, merged with what is already there).
"""
from __future__ import annotations

import argparse
import collections
import itertools
import mmap
import os
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
OUT = REPO / "docs/re/challenges.tsv"
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
    for off, name in ((0x04, "active"), (0x14, "owned"), (0x24, "killed")):
        head = c + off
        n = u(head)
        hs = []
        while n != head and len(hs) < 2000:
            hs.append((u(n + 8) << 32) | u(n + 12))
            n = u(n)
        out[name] = hs
    return out


def load_known() -> dict[int, str]:
    known = {}
    if OUT.exists():
        for line in OUT.read_text().splitlines():
            if line and not line.startswith("#"):
                h, name = line.split("\t")[:2]
                known[int(h, 16)] = name
    return known


def data_identifiers(game: Path) -> set[bytes]:
    found: set[bytes] = set()
    for rel in ("data/big/db.big", "data/big/miscload.big"):
        f = game / rel
        if not f.exists():
            print(f"skipping missing {f}", file=sys.stderr)
            continue
        with open(f, "rb") as fh, mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ) as m:
            found.update(x.group() for x in re.finditer(rb"[A-Za-z0-9_\-.]{3,64}", m))
    return found


def cmd_names(a) -> int:
    lists = tracker_lists(Path(a.snap))
    want = {h for hs in lists.values() for h in hs}
    game = Path(a.game or os.environ.get("SKATE3_GAME_DIR", ""))
    if not (game / "data").exists():
        print("give --game <game data root> (or SKATE3_GAME_DIR)", file=sys.stderr)
        return 1
    print(f"reading identifiers from {game} ...", flush=True)
    idents = data_identifiers(game)
    print(f"{len(idents)} identifiers; learning the name pattern ...", flush=True)
    found = load_known()
    for s in idents:
        h = name_hash(s)
        if h in want:
            found[h] = s.decode()
    pat = re.compile(rb"^([a-z0-9]+)_([a-z]{3,5})_(\d\d)(?:_([a-z0-9_]+))?$")
    types, dists, sufs = collections.Counter(), collections.Counter(), collections.Counter()
    for s in idents:
        m = pat.match(s)
        if m:
            types[m.group(1)] += 1
            dists[m.group(2)] += 1
            if m.group(4) and b"_" not in m.group(4):
                sufs[m.group(4)] += 1
    # Types the readable data misses (race_* are all compressed), from the
    # challenge type names in the image (Street, OwnTheSpot, Slalom, ...).
    tl = sorted({t.decode() for t in types} | {"race", "slalom", "spot", "team", "line", "jam",
                                                "contest", "hall", "pro", "event", "best", "own"})
    dl = [d.decode() for d in dists]
    sl = [""] + ["_" + x.decode() for x in sufs] + ["_sp", "_mp", "_co", "_on"]
    total = len(tl) * len(dl) * 41 * len(sl)
    print(f"trying {total} combinations ({len(tl)} types, {len(dl)} districts, {len(sl)} suffixes) ...",
          flush=True)
    for i, (t, d, n, sf) in enumerate(itertools.product(tl, dl, range(41), sl)):
        if i and i % 200000 == 0:
            print(f"  {100 * i // total}%", flush=True)
        name = f"{t}_{d}_{n:02d}{sf}"
        h = name_hash(name.encode())
        if h in want:
            found[h] = name
    for k, hs in lists.items():
        print(f"{k}: {len(hs)}, named {sum(h in found for h in hs)}")
    with open(OUT, "w") as f:
        f.write("# challenge name hash\tname (tools/re/challenges.py; the game's lookup8 name hash)\n")
        for h, name in sorted(found.items(), key=lambda kv: kv[1]):
            f.write(f"{h:016X}\t{name}\n")
    print(f"{len(found)} names -> {OUT.relative_to(REPO)}")
    return 0


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
    p = sub.add_parser("names")
    p.add_argument("snap")
    p.add_argument("--game")
    p = sub.add_parser("show")
    p.add_argument("snap")
    a = ap.parse_args()
    if a.cmd == "hash":
        for n in a.names:
            print(f"{name_hash(n.encode()):016X}  {n}")
        return 0
    return cmd_names(a) if a.cmd == "names" else cmd_show(a)


if __name__ == "__main__":
    raise SystemExit(main())
