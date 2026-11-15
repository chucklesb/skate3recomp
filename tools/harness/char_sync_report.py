#!/usr/bin/env python3
"""Per-character body-piece sync report from a char_sync log (KI-117 class).

Run a game with skate3_native_render_scene_char_sync=true. The native
renderer rebuilds each character piece by piece; every frame, a `char-sync`
line names the pieces of one character that were NOT drawn from the same
moment as the rest of the body, or that vanished, and why:

    stale=<src>/<N>f  a cached pose N frames old (rescue / reanchor / heal /
                      gapfill): the piece floats behind the body
    gone(<why>)       a piece drawn last frame is missing now (cacheNf = its
                      cached pose was too old to reuse; nocache; absent =
                      the game did not submit it): the piece vanishes
    mode=<m>, lag=    drawn with another smoothing mode / delay than the
                      body (1 = raw newest pose, ahead of the camera)
    prevpack          served from the previous frame's packs while the rest
                      of the body was not (counted only when partial)
    tick=, dup=, div= a different pack call, a part record shared with
                      another piece, a served pose far from this frame's
                      bank capture (wrong serve)

    char_sync_report.py <run dir | log.txt> ... [--reel] [--top 8] [--max-bad N]

--reel limits each run to the reel playback (between the harness "playing
from" and "exit posted" lines). Several runs print an A/B table.
--max-bad N exits 1 when any run has more than N frames out of sync (a
regression check; the reel start has ~2 frames before any pose history).
"""
from __future__ import annotations

import argparse
import collections
import re
import sys
from pathlib import Path

LINE = re.compile(r"^\[\S+ (\S+)\].*char-sync f=(\d+) grp=(\w+) n=(\d+) .*?issues:(.*)$")
ISSUE = re.compile(r"([0-9A-F]{8})/([0-9A-F]{8}):(\S+)")
KINDS = ("stale", "gone", "mode", "lag", "prevpack", "tick", "dup", "div")


def find_log(arg: str) -> Path:
    p = Path(arg)
    if p.is_dir():
        if (p / "log.txt").exists():
            return p / "log.txt"
        sys.exit(f"no log.txt in {p}")
    return p


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("logs", nargs="+")
    ap.add_argument("--reel", action="store_true")
    ap.add_argument("--top", type=int, default=8)
    ap.add_argument("--max-bad", type=int, default=None)
    a = ap.parse_args()
    table = []
    for arg in a.logs:
        lines = open(find_log(arg), errors="replace").read().splitlines()
        lo, hi = "", "99"
        if a.reel:
            for ln in lines:
                if "harness: reel" in ln and "playing from" in ln:
                    lo = ln.split()[1]
                if "harness: reel" in ln and "exit posted" in ln:
                    hi = ln.split()[1]
        frames = collections.Counter()      # kind -> frames with it
        per_mesh = collections.Counter()    # (kind, mesh) -> frames
        examples = collections.defaultdict(list)
        seen_frames = set()
        for ln in lines:
            m = LINE.search(ln)
            if not m or not (lo <= m.group(1) <= hi):
                continue
            f, n = int(m.group(2)), int(m.group(4))
            issues = ISSUE.findall(m.group(5))
            prevpack = [i for i in issues if i[2] == "prevpack"]
            kinds_here = set()
            for ctx, mesh, what in issues:
                kind = re.split(r"[=(]", what)[0]
                if kind == "prevpack" and len(prevpack) >= n:
                    continue  # the whole body from one pack set: consistent
                kinds_here.add(kind)
                per_mesh[(kind, mesh)] += 1
                if len(examples[kind]) < a.top:
                    examples[kind].append(f"f={f} {ctx}/{mesh}:{what}")
            for k in kinds_here:
                frames[k] += 1
            if kinds_here:
                seen_frames.add(f)
        print(f"== {arg}{' (reel only)' if a.reel else ''}: {len(seen_frames)} frames with a piece out of sync")
        for k in KINDS:
            if frames[k]:
                worst = sorted(((c, mesh) for (kk, mesh), c in per_mesh.items() if kk == k), reverse=True)[:3]
                print(f"  {k:<9} {frames[k]:>5} frames  pieces: " +
                      ", ".join(f"{mesh} x{c}" for c, mesh in worst))
                for e in examples[k][:3]:
                    print(f"            {e}")
        table.append((arg, len(seen_frames), [frames[k] for k in KINDS]))
    if len(table) > 1:
        print()
        print(f"{'run':<44} {'bad':>5} " + " ".join(f"{k:>8}" for k in KINDS))
        for name, bad, ks in table:
            print(f"{name[-44:]:<44} {bad:>5} " + " ".join(f"{v:>8}" for v in ks))
    if a.max_bad is not None and any(bad > a.max_bad for _, bad, _ in table):
        print(f"FAIL: more than {a.max_bad} frames out of sync")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
