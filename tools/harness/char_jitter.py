#!/usr/bin/env python3
"""Skater-vs-camera jitter from a char_track log (KI-120).

Run a game with skate3_native_render_scene_char_track=true; each rendered
frame logs a `char-track f=N body ...` line with the body's screen position
through the drawn camera (scr, NDC) and through the guest's raw camera (rscr).

The eye sees the skater's on-screen motion. Smooth motion has a near-zero
second difference per frame; "falls behind and catches up" shows as large
second differences that grow with speed. rscr is the game's own reference
(the raw pose with the raw camera).

Usage: char_jitter.py <log.txt> [--width 1920 --height 1080]
"""
import argparse
import math
import re
import statistics
import sys
from datetime import datetime

PAT = re.compile(
    r"^\[(\S+ \S+)\].*char-track f=(\d+) body mesh=(\w+).*?"
    r"rp=\(([-\d.]+),([-\d.]+),([-\d.]+)\).*?"
    r"ing=(\d).*?raw=(\d)\] ba=([-\d.]+) box=(\d) play=([-\d.]+) "
    r"scr=\(([-\d.]+),([-\d.]+)\) rscr=\(([-\d.]+),([-\d.]+)\) "
    r"stamp=([-\d.]+) tick=([-\d.]+) cam_n=(\d+) clamp=(\d+)(?: cm=(\d) sm=(\d))?")


def pct(xs, q):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(len(xs) * q))] if xs else float("nan")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--width", type=int, default=1920)
    ap.add_argument("--height", type=int, default=1080)
    a = ap.parse_args()
    sx, sy = a.width / 2, a.height / 2
    pts = []
    for line in open(a.log, errors="replace"):
        m = PAT.search(line)
        if not m:
            continue
        g = m.groups()
        pts.append(dict(
            t=datetime.strptime(g[0], "%Y-%m-%d %H:%M:%S.%f").timestamp(),
            f=int(g[1]), mesh=g[2], rp=tuple(map(float, g[3:6])), ing=int(g[6]),
            raw=int(g[7]), play=float(g[10]),
            scr=(float(g[11]) * sx, float(g[12]) * sy),
            rscr=(float(g[13]) * sx, float(g[14]) * sy),
            stamp=float(g[15]), tick=float(g[16]), cam_n=int(g[17]), clamp=int(g[18]),
            sm=int(g[20]) if g[20] is not None else -1))
    if len(pts) < 10:
        print(f"{len(pts)} char-track body lines with screen fields; need a build with KI-120 fields")
        return 1

    # True speed: raw displacement over a 6-frame window (same body mesh).
    speed = {}
    for i in range(6, len(pts)):
        p0, p1 = pts[i - 6], pts[i]
        if p0["mesh"] == p1["mesh"] and p1["t"] > p0["t"]:
            speed[i] = math.dist(p0["rp"], p1["rp"]) / (p1["t"] - p0["t"])

    # Screen-space second difference per frame, consecutive frames, same mesh,
    # both points on screen.
    rows = []
    for i in range(2, len(pts)):
        a0, a1, a2 = pts[i - 2], pts[i - 1], pts[i]
        if not (a0["f"] + 2 == a1["f"] + 1 == a2["f"]) or not (a0["mesh"] == a1["mesh"] == a2["mesh"]):
            continue
        if any(abs(c) > 2 * max(sx, sy) for q in (a0, a1, a2) for c in q["scr"]):
            continue
        d = lambda k: math.hypot(a2[k][0] - 2 * a1[k][0] + a0[k][0], a2[k][1] - 2 * a1[k][1] + a0[k][1])
        rows.append(dict(i=i, v=speed.get(i), w=d("scr"), rw=d("rscr"), p=a2))

    def bucket(v):
        return None if v is None else "0-2" if v < 2 else "2-5" if v < 5 else "5-8" if v < 8 else "8+"

    print(f"{len(pts)} frames, {len(rows)} usable; screen {a.width}x{a.height}")
    print("on-screen wobble (|2nd difference| px/frame^2), drawn vs raw-game reference, by speed m/s:")
    for k in ("0-2", "2-5", "5-8", "8+"):
        xs = [r for r in rows if bucket(r["v"]) == k]
        if not xs:
            continue
        w = [r["w"] for r in xs]
        rw = [r["rw"] for r in xs]
        print(f"  {k:>4}: {len(xs):5d} frames  drawn median {statistics.median(w):5.1f} p95 {pct(w, .95):6.1f}"
              f"  |  raw median {statistics.median(rw):5.1f} p95 {pct(rw, .95):6.1f}")

    modes = sorted({r["p"]["sm"] for r in rows if r["p"]["sm"] >= 0})
    if len(modes) > 1:
        # A session that switched skate3_native_render_scene_dyn_stamp_mode
        # live: the same player, place and frame rate under each mode.
        print("by dyn_stamp_mode (sm), same session:")
        for sm in modes:
            for k in ("5-8", "8+"):
                xs = [r for r in rows if r["p"]["sm"] == sm and bucket(r["v"]) == k]
                if len(xs) < 5:
                    continue
                w = [r["w"] for r in xs]
                rw = [r["rw"] for r in xs]
                print(f"  sm={sm} {k:>4}: {len(xs):5d} frames  drawn median {statistics.median(w):5.1f} "
                      f"p95 {pct(w, .95):6.1f}  |  raw median {statistics.median(rw):5.1f} p95 {pct(rw, .95):6.1f}")

    fast = [r for r in rows if r["v"] is not None and r["v"] >= 5]
    if not fast:
        fast = rows
        print("(no frames at >= 5 m/s; correlations use all frames)")

    def grp(name, key):
        print(f"  by {name}:")
        vals = sorted({key(r) for r in fast})
        for v in vals:
            xs = [r["w"] for r in fast if key(r) == v]
            if len(xs) >= 5:
                print(f"    {v!s:>8}: {len(xs):5d} frames, wobble median {statistics.median(xs):5.1f} p95 {pct(xs, .95):6.1f}")

    print(f"fast frames ({len(fast)}), what the wobble goes with:")
    grp("clamped taps (H2)", lambda r: min(r["p"]["clamp"], 4))
    grp("camera samples this frame (H3)", lambda r: min(r["p"]["cam_n"], 12))
    stamps = [r["p"]["stamp"] for r in fast if r["p"]["ing"] == 1 and r["p"]["stamp"] >= 0]
    if stamps:
        med = statistics.median(stamps)
        grp("stamp offset from median, ms (H1)",
            lambda r: "no-ingest" if r["p"]["ing"] != 1 or r["p"]["stamp"] < 0
            else f"{5 * round((r['p']['stamp'] - med) / 5):+d}")
        print(f"  stamp age: median {med:.1f} ms, p5 {pct(stamps, .05):.1f}, p95 {pct(stamps, .95):.1f}")
    clamped = sum(1 for r in fast if r["p"]["clamp"] > 0)
    print(f"  frames with clamped taps: {clamped} ({100 * clamped / max(1, len(fast)):.0f}%)")
    cams = [r["p"]["cam_n"] for r in fast]
    print(f"  camera samples per frame: median {statistics.median(cams)}, p5 {pct(cams, .05)}, p95 {pct(cams, .95)}")
    plays = [p["play"] for p in pts]
    print(f"playback delay: median {statistics.median(plays):.1f} ms, min {min(plays):.1f}, max {max(plays):.1f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
