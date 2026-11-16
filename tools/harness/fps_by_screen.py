#!/usr/bin/env python3
"""Frame rate per screen context from a run's state.jsonl.

    fps_by_screen.py <run dir> [--min-samples 3] [--skip-s 1.0]

Groups the state trace (one row every state_every_frames frames, with the
game's fps then) by context: the frontend stack plus the replay flag, as
census_report.py names them. Per context: samples, frames covered, median,
5th percentile and minimum fps. The first --skip-s seconds after each context
change are left out (loads and transitions drag the average there).
"""
from __future__ import annotations

import argparse
import json
import statistics
from collections import defaultdict
from pathlib import Path


def context(row: dict) -> str:
    fe = row.get("fe") or []
    name = "[" + ",".join(str(x) for x in fe) + "]"
    if row.get("replay"):
        name += " replay"
    if row.get("movie"):
        name += " movie"
    return name


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("run", type=Path)
    ap.add_argument("--min-samples", type=int, default=3)
    ap.add_argument("--skip-s", type=float, default=1.0)
    a = ap.parse_args()
    rows = []
    for line in open(a.run / "state.jsonl"):
        try:
            r = json.loads(line)
        except json.JSONDecodeError:
            continue
        if "fps" in r and "t" in r:
            rows.append(r)
    groups: dict[str, list[float]] = defaultdict(list)
    first_seen: dict[str, float] = {}
    order: list[str] = []
    cur, since = None, 0.0
    for r in rows:
        c = context(r)
        if c != cur:
            cur, since = c, r["t"]
        if c not in first_seen:
            first_seen[c] = r["t"]
            order.append(c)
        if r["t"] - since >= a.skip_s:
            groups[c].append(float(r["fps"]))
    print(f"{'context':<34} {'samples':>7} {'median':>7} {'p5':>6} {'min':>6}")
    for c in order:
        v = sorted(groups.get(c, []))
        if len(v) < a.min_samples:
            continue
        p5 = v[max(0, int(len(v) * 0.05) - 1)] if len(v) >= 20 else v[0]
        print(f"{c:<34} {len(v):>7} {statistics.median(v):>7.1f} {p5:>6.1f} {v[0]:>6.1f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
