#!/usr/bin/env python3
"""Frame-time report from a harness run's frames.csv, and run A/B.

The harness writes frames.csv: one row per guest frame (the interval between
two guest swaps), split into the parts measured at the swap hook:

    guest    the game's own work on its thread (what is left)
    native   the native renderer's frame end on the game thread
    pace     the frame-cap wait (skate3_guest_fps_cap): idle time
    swap     the guest swap (present, GPU back-pressure)
    harness  the harness's own frame hook

plus the frontend top / depth, flags (1 replay, 2 movie, 4 paused) and the
running step. Paused frames are left out.

Per segment it prints average fps, frame-time percentiles, the 1% / 0.1% lows
(the fps over the worst 1% / 0.1% of frames), hitches (frames over 2x the
median, over 100 ms), jitter (mean change between consecutive frames, and the
standard deviation), and "busy" (the frame without the cap wait). Under a
frame cap the fps stays at the cap while busy shows how much headroom is
left, so busy is the number to watch for regressions and optimization.

Segments: all; ingame (frontend [0], no movie: free skate and reels);
reel<N> (reel_start .. reel_stop); "perf <name>" marks (a `mark` step with
text "perf <name>" up to the next mark or "perf end"); and with --by-step,
every step.

    frametime_report.py <run>                      # report
    frametime_report.py <run> --svg [out.svg]      # also a frame-time chart
    frametime_report.py <base-run> <new-run> ...   # A/B table
        [--fail-pct 10]   # exit 1 when a shared segment's ms_p99, busy_p99
                          # or 1% low got worse by more than 10%
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import sys
from pathlib import Path

PARTS = ("guest", "native", "pace", "swap", "harness")


def run_dir(arg: str) -> Path:
    p = Path(arg)
    if p.is_file():
        p = p.parent
    if not (p / "frames.csv").exists():
        sys.exit(f"no frames.csv in {p} (a build from before 2026-11-16?)")
    return p


def load(d: Path):
    rows = []
    with open(d / "frames.csv", newline="") as f:
        for r in csv.DictReader(f):
            rows.append({k: (float(v) if k in ("t", "dt") + PARTS else int(v)) for k, v in r.items()})
    events = []
    ev = d / "events.jsonl"
    if ev.exists():
        for ln in ev.read_text(errors="replace").splitlines():
            try:
                events.append(json.loads(ln))
            except json.JSONDecodeError:
                pass
    return rows, events


def segments(rows, events, by_step: bool):
    """name -> (first frame, last frame) or a row predicate."""
    segs = {"all": lambda r: True,
            "ingame": lambda r: r["fe_depth"] == 1 and r["fe_top"] == 0 and not r["flags"] & 2}
    last = rows[-1]["frame"] if rows else 0
    reel_n = 0
    start = None
    mark = None
    for e in events:
        k, fr = e.get("kind"), e.get("frame", 0)
        if k == "reel_start":
            reel_n += 1
            start = fr
        elif k in ("reel_stop", "reel_exit") and start is not None:
            segs[f"reel{reel_n}"] = (start, fr)
            start = None
        elif k == "step" and e.get("verb") == "mark":
            text = str(e.get("text", ""))
            if mark is not None:
                segs[mark[0]] = (mark[1], fr)
                mark = None
            if text.startswith("perf ") and text != "perf end":
                mark = (text, fr)
    if start is not None:
        segs[f"reel{reel_n}"] = (start, last)
    if mark is not None:
        segs[mark[0]] = (mark[1], last)
    if by_step:
        steps = sorted({r["step"] for r in rows})
        for s in steps:
            segs[f"step{s}"] = (lambda s: lambda r: r["step"] == s)(s)
    return segs


def pick(rows, seg):
    if callable(seg):
        return [r for r in rows if seg(r) and not r["flags"] & 4]
    lo, hi = seg
    return [r for r in rows if lo < r["frame"] <= hi and not r["flags"] & 4]


def pct(sorted_vals, p):
    return sorted_vals[min(len(sorted_vals) - 1, int(p * (len(sorted_vals) - 1) + 0.5))]


def stats(rs):
    if len(rs) < 2:
        return None
    dt = [r["dt"] for r in rs]
    s = sorted(dt)
    n, total = len(dt), sum(dt)

    def low(frac):
        k = max(1, int(n * frac))
        return 1000.0 * k / sum(s[-k:])

    p50 = pct(s, 0.5)
    busy = sorted(r["dt"] - r["pace"] for r in rs)
    mean = total / n
    out = {
        "frames": n, "seconds": total / 1000.0, "fps_avg": 1000.0 * n / total,
        "fps_low_1pct": low(0.01), "fps_low_0_1pct": low(0.001),
        "ms_p50": p50, "ms_p90": pct(s, 0.9), "ms_p99": pct(s, 0.99), "ms_p99_9": pct(s, 0.999),
        "ms_max": s[-1],
        "ms_stddev": math.sqrt(max(0.0, sum(d * d for d in dt) / n - mean * mean)),
        "jitter_ms": sum(abs(a - b) for a, b in zip(dt, dt[1:])) / (n - 1),
        "hitches_2x": sum(d > 2 * p50 for d in dt), "hitches_100ms": sum(d > 100 for d in dt),
        "busy_ms_p50": pct(busy, 0.5), "busy_ms_p99": pct(busy, 0.99),
    }
    for p in PARTS:
        v = sorted(r[p] for r in rs)
        out[f"{p}_ms_avg"] = sum(v) / n
        out[f"{p}_ms_p99"] = pct(v, 0.99)
    return out


def context(r):
    c = f"fe {r['fe_top']}/{r['fe_depth']}"
    if r["flags"] & 1:
        c += " replay"
    if r["flags"] & 2:
        c += " movie"
    return c


def near_events(events, frame, span=3):
    out = []
    for e in events:
        if abs(e.get("frame", -99) - frame) <= span and e.get("kind") not in ("prepare",):
            k = e["kind"]
            if k == "step":
                k += f" {e.get('step')} {e.get('verb')}"
            elif k == "fe":
                k += " " + json.dumps(e.get("stack"))
            out.append(f"{k}@{e['frame']}")
    return ", ".join(out[:4])


def print_report(name, rows, events, segs, top):
    print(f"== {name}: {len(rows)} frames")
    print(f"  {'segment':<16} {'frames':>6} {'fps':>6} {'1%low':>6} {'.1%low':>6} {'p50':>6} "
          f"{'p99':>6} {'max':>7} {'jit':>5} {'hitch':>5} {'busy50':>6} {'busy99':>6} | "
          + " ".join(f"{p:>7}" for p in PARTS) + "  (ms avg)")
    for sname, seg in segs.items():
        st = stats(pick(rows, seg))
        if not st:
            continue
        print(f"  {sname[:16]:<16} {st['frames']:>6} {st['fps_avg']:>6.1f} {st['fps_low_1pct']:>6.1f} "
              f"{st['fps_low_0_1pct']:>6.1f} {st['ms_p50']:>6.1f} {st['ms_p99']:>6.1f} "
              f"{st['ms_max']:>7.1f} {st['jitter_ms']:>5.1f} {st['hitches_2x']:>5} "
              f"{st['busy_ms_p50']:>6.1f} {st['busy_ms_p99']:>6.1f} | "
              + " ".join(f"{st[p + '_ms_avg']:>7.2f}" for p in PARTS))
    ing = pick(rows, segs["ingame"]) or pick(rows, segs["all"])
    if top and ing:
        p50 = pct(sorted(r["dt"] for r in ing), 0.5)
        worst = [r for r in sorted(ing, key=lambda r: -r["dt"])[:top] if r["dt"] > 1.25 * p50]
        print(f"  worst in-game frames over 1.25x the median ({p50:.1f} ms): "
              + ("" if worst else "none"))
        for r in sorted(worst, key=lambda r: r["frame"]):
            parts = {p: r[p] for p in PARTS if p != "pace"}
            dom = max(parts, key=parts.get)
            print(f"    f={r['frame']:<6} t={r['t']:7.2f}s {r['dt']:7.1f} ms  mostly {dom} "
                  f"({parts[dom]:.1f} ms; " + " ".join(f"{p}={r[p]:.1f}" for p in PARTS)
                  + f")  {context(r)}  step {r['step']}"
                  + (f"  [{near_events(events, r['frame'])}]" if near_events(events, r['frame']) else ""))


def write_svg(path: Path, rows, segs, title):
    w, h, pad = 1200, 360, 40
    if not rows:
        return
    f0, f1 = rows[0]["frame"], rows[-1]["frame"]
    ymax = max(50.0, min(200.0, pct(sorted(r["dt"] for r in rows), 0.999) * 1.2))

    def x(fr):
        return pad + (w - 2 * pad) * (fr - f0) / max(1, f1 - f0)

    def y(ms):
        return h - pad - (h - 2 * pad) * min(ms, ymax) / ymax

    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" font-family="sans-serif" font-size="11">',
           f'<rect width="{w}" height="{h}" fill="#fff"/>',
           f'<text x="{pad}" y="16" font-size="13">{title} (ms per frame; grey = busy, red = frame)</text>']
    for name, seg in segs.items():
        if callable(seg):
            continue
        out.append(f'<rect x="{x(seg[0]):.1f}" y="{pad}" width="{max(1.0, x(seg[1]) - x(seg[0])):.1f}" '
                   f'height="{h - 2 * pad}" fill="#e8f0ff"/><text x="{x(seg[0]) + 3:.1f}" y="{pad + 12}">{name}</text>')
    for ms in (16.7, 33.3, 50, 100, 150, 200):
        if ms <= ymax:
            out.append(f'<line x1="{pad}" x2="{w - pad}" y1="{y(ms):.1f}" y2="{y(ms):.1f}" stroke="#ccc"/>'
                       f'<text x="4" y="{y(ms) + 4:.1f}">{ms:g}</text>')
    busy = " ".join(f"{x(r['frame']):.1f},{y(r['dt'] - r['pace']):.1f}" for r in rows)
    dt = " ".join(f"{x(r['frame']):.1f},{y(r['dt']):.1f}" for r in rows)
    out.append(f'<polyline fill="none" stroke="#999" stroke-width="0.8" points="{busy}"/>')
    out.append(f'<polyline fill="none" stroke="#d33" stroke-width="0.8" points="{dt}"/>')
    out.append(f'<text x="{pad}" y="{h - 10}">frame {f0}</text><text x="{w - pad - 60}" y="{h - 10}">frame {f1}</text>')
    out.append("</svg>")
    path.write_text("\n".join(out))
    print(f"  chart: {path}")


KEYS = (("fps_avg", +1), ("fps_low_1pct", +1), ("fps_low_0_1pct", +1), ("ms_p50", -1),
        ("ms_p99", -1), ("ms_max", -1), ("jitter_ms", -1), ("hitches_2x", -1),
        ("busy_ms_p50", -1), ("busy_ms_p99", -1), ("guest_ms_avg", -1), ("native_ms_avg", -1),
        ("swap_ms_avg", -1))
GATED = ("ms_p99", "busy_ms_p99", "fps_low_1pct")


def compare(dirs, by_step, fail_pct):
    loaded = [load(d) for d in dirs]
    per = []
    for rows, events in loaded:
        segs = segments(rows, events, by_step)
        per.append({n: stats(pick(rows, s)) for n, s in segs.items()})
    base = per[0]
    bad = []
    for seg in base:
        if base[seg] is None or any(p.get(seg) is None for p in per[1:]):
            continue
        print(f"== {seg}")
        print(f"  {'metric':<16}" + "".join(f"{d.name[-24:]:>26}" for d in dirs))
        for k, better in KEYS:
            b = base[seg][k]
            cells = [f"{b:>26.2f}"]
            for p in per[1:]:
                v = p[seg][k]
                rel = (v - b) / b * 100.0 if b else 0.0
                worse = rel * better < 0
                cells.append(f"{v:>15.2f} ({rel:+6.1f}%){'!' if worse and abs(rel) > (fail_pct or 1e9) else ' '}")
                if fail_pct is not None and k in GATED and worse and abs(rel) > fail_pct:
                    bad.append(f"{seg} {k} {rel:+.1f}%")
            print(f"  {k:<16}" + "".join(cells))
    if bad:
        print("FAIL: " + "; ".join(bad))
        return 1
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--by-step", action="store_true")
    ap.add_argument("--top", type=int, default=8, help="worst frames to list")
    ap.add_argument("--svg", nargs="?", const="", default=None)
    ap.add_argument("--fail-pct", type=float, default=None)
    ap.add_argument("--json", action="store_true", help="write frametime.json in the run")
    a = ap.parse_args()
    dirs = [run_dir(x) for x in a.runs]
    if len(dirs) > 1:
        return compare(dirs, a.by_step, a.fail_pct)
    rows, events = load(dirs[0])
    segs = segments(rows, events, a.by_step)
    print_report(dirs[0].name, rows, events, segs, a.top)
    if a.json:
        (dirs[0] / "frametime.json").write_text(
            json.dumps({n: stats(pick(rows, s)) for n, s in segs.items()}, indent=1))
    if a.svg is not None:
        write_svg(Path(a.svg) if a.svg else dirs[0] / "frametimes.svg", rows, segs, dirs[0].name)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
