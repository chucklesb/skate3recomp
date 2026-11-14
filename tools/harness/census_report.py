#!/usr/bin/env python3
"""Summarise emulated-pass census events per screen context.

Run with native_render_pass_census=true; the harness then writes a `census`
event to events.jsonl every 60 frames and at each frontend-stack change, with
the emulated draws by surface pitch since the previous one. This groups them
by context (frontend stack + replay flag) and prints, per context, how many
frames the native renderer drew, and per pitch the draws per frame that were
suppressed, executed under the native renderer (survived suppression) and
executed with the native renderer inactive (fully emulated frames).

It ends with the native-only audit (docs/native-only.md, phase 0): one line
per context, ZERO when no emulated draw executed and no frame was presented
emulated, otherwise the executed draws per frame, the pitches they ran at and
the windows that were open (the census event's `holders`; none listed means
the default suppress mode let them through).

Usage: census_report.py <run dir or events.jsonl> [--min-frames N] [--all-pitches]
                        [--audit-only]
"""
import argparse
import json
import sys
from collections import defaultdict
from pathlib import Path


def load(path):
    p = Path(path)
    if p.is_dir():
        p = p / "events.jsonl"
    out = []
    with p.open() as f:
        for line in f:
            try:
                e = json.loads(line)
            except json.JSONDecodeError:
                continue
            if e.get("kind") == "census":
                out.append(e)
    return out


def context_key(e):
    fe = ",".join(str(x) for x in e.get("fe", []))
    return f"[{fe}]" + (" replay" if e.get("replay") else "")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run")
    ap.add_argument("--min-frames", type=int, default=10, help="skip contexts with fewer frames")
    ap.add_argument("--all-pitches", action="store_true", help="also list pitches that were only suppressed")
    ap.add_argument("--audit-only", action="store_true", help="print only the native-only audit")
    args = ap.parse_args()

    events = load(args.run)
    if not events:
        print("no census events (was native_render_pass_census on?)")
        return 1

    ctx = defaultdict(lambda: {"frames": 0, "native": 0, "emulated": 0, "events": 0,
                               "first_t": None, "last_t": None,
                               "holders": set(),
                               "p": defaultdict(lambda: [0] * 6)})
    order = []
    for e in events:
        k = context_key(e)
        c = ctx[k]
        if c["events"] == 0:
            order.append(k)
        c["events"] += 1
        c["frames"] += e.get("frames", 0)
        c["native"] += e.get("native_frames", 0)
        c["emulated"] += e.get("emulated_frames", 0)
        t = e.get("t")
        c["first_t"] = t if c["first_t"] is None else c["first_t"]
        c["last_t"] = t
        c["holders"].update(e.get("holders", []))
        for row in e.get("pitches", []):
            acc = c["p"][row[0]]
            for i in range(6):
                acc[i] += row[i + 1]

    for k in order:
        c = ctx[k]
        presented = c["native"] + c["emulated"]
        if presented < args.min_frames or args.audit_only:
            continue
        n = max(presented, 1)
        print(f"{k}: {presented} frames ({c['native']} native, {c['emulated']} emulated), "
              f"t {c['first_t']:.1f}-{c['last_t']:.1f} s")
        print(f"  {'pitch':>6} {'supp/f':>8} {'exec-nat/f':>11} {'exec-emu/f':>11} "
              f"{'idx nat/f':>10} {'idx emu/f':>10}")
        for pitch in sorted(c["p"]):
            a = c["p"][pitch]
            if not args.all_pitches and a[1] == 0 and a[2] == 0:
                continue
            print(f"  {pitch:>6} {a[0] / n:>8.1f} {a[1] / n:>11.1f} {a[2] / n:>11.1f} "
                  f"{a[4] / n:>10.0f} {a[5] / n:>10.0f}")
        if c["holders"]:
            print(f"  windows open: {', '.join(sorted(c['holders']))}")

    print()
    print("native-only audit (executed emulated draws per frame):")
    clean = True
    for k in order:
        c = ctx[k]
        presented = c["native"] + c["emulated"]
        if presented < args.min_frames:
            continue
        n = max(presented, 1)
        executed = {p: a[1] + a[2] for p, a in c["p"].items() if a[1] + a[2] > 0}
        if not executed and c["emulated"] == 0:
            print(f"  ZERO  {k} ({presented} frames)")
            continue
        clean = False
        total = sum(executed.values()) / n
        pitches = " ".join(f"{p}:{v / n:.1f}" for p, v in sorted(executed.items()))
        held = ", ".join(sorted(c["holders"])) or "none (default suppress mode)"
        emu = f", {c['emulated']} frames presented emulated" if c["emulated"] else ""
        print(f"  {total:>5.1f}  {k} ({presented} frames{emu}) pitches {pitches or '-'}; "
              f"windows: {held}")
    print("  all contexts ZERO" if clean else "")
    return 0


if __name__ == "__main__":
    sys.exit(main())
