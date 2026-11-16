#!/usr/bin/env python3
"""Names for the game's 64-bit data keys (EA Attrib vaults in the .big archives).

    attrib.py names [--game DIR]   # extract the archives' vault reports, write docs/re/attrib_names.tsv
    attrib.py lookup HASH|TEXT     # a key's class and record, or records matching TEXT

The game's databases are Attrib vaults (.vlt + .bin pairs in data/big/*.big,
read with tools/re/big.py). Each vault ships a *_summaryreport.txt listing
its records as /<class>.class/<record>.xml. Code looks records up by the
64-bit name hash of the class and of the record (sub_82B73AC0, lookup8; see
tools/re/challenges.py), so this table turns those constants back into
names: e.g. 0xA5970491380FF1BE = class progression_action, the career's
locked unlock actions; challenges are records of challenge_local_data.

Names only (no game data) go in the TSV: hash, kind (class|record), class,
name.
"""
from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools/re"))
from big import Big  # noqa: E402
from challenges import name_hash  # noqa: E402

OUT = REPO / "docs/re/attrib_names.tsv"
REPORT = re.compile(r"^/([A-Za-z0-9_]+)\.class/([^/\n]+)\.xml\s*$", re.M)


def load() -> dict[int, tuple[str, str, str]]:
    out = {}
    if OUT.exists():
        for line in OUT.read_text().splitlines():
            if line and not line.startswith("#"):
                h, kind, cls, name = line.split("\t")
                out[int(h, 16)] = (kind, cls, name)
    return out


def cmd_names(a) -> int:
    game = Path(a.game or os.environ.get("SKATE3_GAME_DIR", ""))
    bigs = sorted((game / "data/big").glob("*.big")) if game.name else []
    if not bigs:
        print("give --game <game data root> (or SKATE3_GAME_DIR)", file=sys.stderr)
        return 1
    records: set[tuple[str, str]] = set()
    for path in bigs:
        print(f"reading {path.name} ...", file=sys.stderr, flush=True)
        b = Big(path)
        for e in b.entries:
            if e[0].endswith("_summaryreport.txt"):
                text = b.data(e).decode("latin1")
                records.update(REPORT.findall(text))
    # One row per name; a record name used in several classes lists them all
    # (e.g. dist_downtown is an aud_worlddata and a world record; the map keys
    # are the world ones).
    table: dict[int, tuple[str, str, str]] = {}
    in_classes: dict[str, set[str]] = {}
    clash = 0
    for cls, rec in sorted(records):
        table.setdefault(name_hash(cls.encode()), ("class", cls, cls))
        in_classes.setdefault(rec, set()).add(cls)
    for rec, classes in sorted(in_classes.items()):
        h = name_hash(rec.encode())
        if h in table and table[h][2] != rec:
            clash += 1
            continue
        if h not in table:
            table[h] = ("record", ",".join(sorted(classes)), rec)
    with open(OUT, "w") as f:
        f.write("# hash\tkind\tclass\tname (tools/re/attrib.py; the game's lookup8 name hash of Attrib "
                "vault classes and records)\n")
        for h, (kind, cls, name) in sorted(table.items(), key=lambda kv: (kv[1][1], kv[1][0] != "class",
                                                                        kv[1][2])):
            f.write(f"{h:016X}\t{kind}\t{cls}\t{name}\n")
    classes = sum(1 for v in table.values() if v[0] == "class")
    print(f"{len(table)} names ({classes} classes) -> {OUT.relative_to(REPO)}"
          + (f"; {clash} records share a name across classes" if clash else ""))
    return 0


def cmd_lookup(a) -> int:
    table = load()
    q = a.query.removeprefix("0x").removeprefix("0X")
    hits = []
    if re.fullmatch(r"[0-9A-Fa-f]{16}", q):
        h = int(q, 16)
        if h in table:
            hits.append((h, table[h]))
    else:
        q = a.query.lower()
        hits = [(h, v) for h, v in table.items() if q in v[2].lower() or (q.endswith(".class") and
                                                                           q[:-6] in v[1].split(","))]
    for h, (kind, cls, name) in hits[:200]:
        print(f"{h:016X}  {kind:6s} {cls}/{name}" if kind == "record" else f"{h:016X}  class  {cls}")
    if not hits:
        print("no match (a hash not in the vault reports; `challenges.py hash NAME` to test a name)")
    return 0 if hits else 1


def main() -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("names")
    p.add_argument("--game")
    p = sub.add_parser("lookup")
    p.add_argument("query")
    a = ap.parse_args()
    return cmd_names(a) if a.cmd == "names" else cmd_lookup(a)


if __name__ == "__main__":
    raise SystemExit(main())
