#!/usr/bin/env python3
"""Per-screen boot timing from harness runs (docs/re/boot.md).

  boot_timing.py RUN [RUN ...]

Reads each run's events.jsonl: the frontend stack changes (`fe`) and movie
starts/stops, and prints one row per boot screen (the top of the FE stack):
the frame and time it appeared, and how long it stayed, in frames and
seconds. Runs at different frame-rate caps side by side show whether a
screen is timed in frames (same frames, different seconds) or in time.
"""
import json
import sys
from pathlib import Path


def screens(run: Path):
    rows = []  # (top, frame, t)
    movies = []
    with open(run / "events.jsonl") as f:
        for line in f:
            try:
                e = json.loads(line)
            except ValueError:
                continue
            k = e.get("kind")
            if k == "fe":
                stack = e.get("stack") or []
                top = str(stack[-1]) if stack else "-"
                rows.append((top, e["frame"], e["t"]))
            elif k in ("movie_start", "movie_stop"):
                movies.append((k, e["frame"], e["t"]))
    return rows, movies


def cap_of(run: Path) -> str:
    try:
        cvars = json.loads((run / "result.json").read_text()).get("cvars", {})
        return str(cvars.get("skate3_guest_fps_cap", "?"))
    except (OSError, ValueError):
        return "?"


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    for arg in sys.argv[1:]:
        run = Path(arg)
        rows, movies = screens(run)
        print(f"== {run.name} (fps cap {cap_of(run)})")
        print(f"  {'screen':>8} {'from frame':>10} {'at s':>7} {'frames':>7} {'seconds':>8}")
        for i, (top, frame, t) in enumerate(rows):
            if i + 1 < len(rows):
                nf, nt = rows[i + 1][1] - frame, rows[i + 1][2] - t
                print(f"  {top:>8} {frame:>10} {t:>7.2f} {nf:>7} {nt:>8.2f}")
            else:
                print(f"  {top:>8} {frame:>10} {t:>7.2f} {'(open)':>7}")
        for k, frame, t in movies:
            print(f"  {k:>11} frame {frame} at {t:.2f} s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
