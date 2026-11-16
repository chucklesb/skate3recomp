#!/usr/bin/env python3
"""RE-7 native replacements: totals from a run's `replace:` log lines.

  replace_report.py <run> [<run> ...] [--max-diff 0]

Sums each family's counters (calls, exact, diff, native, bad, rows) over the
run (src/native/skate3_native_replace.cpp writes one `replace:` line every
120 frames) and fails (exit 1) when any family's diff exceeds --max-diff, or
when a family ran in shadow mode with no exact call at all.
"""
from __future__ import annotations

import argparse
import re
import sys
from collections import defaultdict
from pathlib import Path

FAMILY = re.compile(r"(\w+)\[(on|shadow) calls=(\d+) exact=(\d+) diff=(\d+) native=(\d+) "
                    r"bad=(\d+) rows=(\d+)\]")
KEYS = ("calls", "exact", "diff", "native", "bad", "rows")


def totals(run: Path) -> dict[str, dict]:
    log = run / "log.txt"
    if not log.exists():
        sys.exit(f"{log} is missing")
    out: dict[str, dict] = defaultdict(lambda: {k: 0 for k in KEYS} | {"modes": set()})
    for line in log.read_text(errors="replace").splitlines():
        if "replace: frame" not in line:
            continue
        for m in FAMILY.finditer(line):
            fam = out[m.group(1)]
            fam["modes"].add(m.group(2))
            for k, v in zip(KEYS, m.groups()[2:]):
                fam[k] += int(v)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("runs", nargs="+", type=Path)
    ap.add_argument("--max-diff", type=int, default=0)
    a = ap.parse_args()
    bad = False
    for run in a.runs:
        fams = totals(run)
        print(f"{run.name}:")
        if not fams:
            print("  no replace: lines (every family off?)")
            continue
        for name, f in sorted(fams.items()):
            modes = "/".join(sorted(f["modes"]))
            print(f"  {name:8s} {modes:11s} calls {f['calls']:8d}  exact {f['exact']:8d}  "
                  f"diff {f['diff']:5d}  native {f['native']:8d}  bad {f['bad']:6d}  "
                  f"rows {f['rows']}")
            if f["diff"] > a.max_diff:
                print(f"  FAIL {name}: {f['diff']} calls differ from the recompiled code")
                bad = True
            if "shadow" in f["modes"] and f["calls"] > f["bad"] and f["exact"] == 0:
                print(f"  FAIL {name}: shadow mode but no call compared exact")
                bad = True
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
