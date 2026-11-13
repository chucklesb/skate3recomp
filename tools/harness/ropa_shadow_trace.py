#!/usr/bin/env python3
"""Per-reel-pass trace of the garment shadow offset (KI-124) from a run's log.

    python3 tools/harness/ropa_shadow_trace.py <run>/log.txt [--window LO HI]

Needs a run with `skate3_native_render_scene_char_track = true`, which logs
one `ropa-shadow f=... resident_vs_drawn_cm=... cast=...` line per frame:
how far the newest decoded cloth shape (the old shadow caster) sat from the
shape the main pass draws. Each reel pass starts at the log's
`harness: reel N: playing from` line (t = 0) and ends at `back in free
skate`. Prints per pass the median / p95 / max offset, the biggest spikes,
and the series in the window (default 0.8..1.5 s after the reel starts,
around the KI-124 flash). Standard library only.

Example: scenarios/ki124_ropa_caster_ab.toml (pass A = old caster, pass B =
fix); a one-frame shadow flash should line up with a spike in pass A.
"""
from __future__ import annotations

import argparse
import re
from datetime import datetime

TS = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\]")
RS = re.compile(r"ropa-shadow f=(\d+) casters=(\d+) ropa=(\d+) worst_mesh=(\w+) "
                r"resident_vs_drawn_cm=(-?[\d.]+) cast=(\w+)")


def parse(log: str) -> list[dict]:
    passes: list[dict] = []
    cur = None
    for line in open(log, errors="replace"):
        m = TS.match(line)
        if not m:
            continue
        t = datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f").timestamp()
        if "harness: reel" in line and "playing from" in line:
            cur = {"t0": t, "rows": [], "label": line.split("] ", 3)[-1].strip()}
            passes.append(cur)
        elif "back in free skate" in line:
            cur = None
        elif cur is not None and (r := RS.search(line)):
            cur["rows"].append({"t": t - cur["t0"], "frame": int(r.group(1)),
                                "ropa": int(r.group(3)), "mesh": r.group(4),
                                "cm": float(r.group(5)), "cast": r.group(6)})
    return passes


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("--window", nargs=2, type=float, default=(0.8, 1.5), metavar=("LO", "HI"))
    a = ap.parse_args()
    passes = parse(a.log)
    if not passes:
        print("no reel passes with ropa-shadow lines (char_track off?)")
        return 1
    for i, p in enumerate(passes):
        rows = p["rows"]
        print(f"=== pass {i}: {len(rows)} frames ({p['label'][:90]})")
        devs = sorted(r["cm"] for r in rows if r["cm"] >= 0)
        if devs:
            print(f"  offset cm: median {devs[len(devs) // 2]:.2f}  "
                  f"p95 {devs[int(len(devs) * 0.95)]:.2f}  max {devs[-1]:.2f}  "
                  f"cast={rows[0]['cast']}")
        print("  biggest (t, frame, cm):")
        for r in sorted(rows, key=lambda r: -r["cm"])[:8]:
            print(f"    t={r['t']:6.3f} f={r['frame']} cm={r['cm']:6.2f} "
                  f"garments={r['ropa']} mesh={r['mesh']}")
        print(f"  series {a.window[0]}..{a.window[1]} s:")
        prev = None
        for r in rows:
            if a.window[0] <= r["t"] <= a.window[1]:
                dt = (r["t"] - prev) * 1e3 if prev is not None else 0.0
                print(f"    t={r['t']:6.3f} (+{dt:5.1f} ms) f={r['frame']} cm={r['cm']:6.2f}")
            prev = r["t"]
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
