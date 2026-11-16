#!/usr/bin/env python3
"""Skeleton-store shadow report (RE-5 step 2) from a game log.

Run a game with skate3_native_render_scene_skeleton="shadow" or "on" (for
example scenarios/skel_shadow.toml, skel_on.toml). The store rebuilds every
skater piece's palette from one pose per character per tick, and a `skel:`
line every N frames counts how the rebuilt pieces compare (in "on" mode also
how many it served, and the bank compare still runs before the serve):

    items     skinned draws of skater-family entities
    built     of those, pieces the store could rebuild; the rest are
              miss[noent | notick | nopart | skipped]
    bank[...] against the draw's bank capture (the game's own upload):
              new / old = exact against the newest / the previous tick,
              close = within 5 cm, div = further, none = no fresh capture
    serve[...] the same against today's palette serve (dbg_src 11);
              unserved = the serve did not take the item
    store[...] ticks captured, unpacked (a UBT with no pack before the
              next), unowned UBT calls, packs of other instances, and a
              tick with two different localToWorlds (ltow_var)
    jump[...] the characters' jump counter rising: root (a root step the
              motion doesn't explain: teleport, replay jump), gap (> 100
              ms between ticks), parts (another instance array: LOD)

    skel_shadow_report.py <run dir | log.txt> ... [--reel] [--max-div N]

--reel limits each run to the reel playback. --max-div N exits 1 when any
run has more than N divergent pieces (bank or serve) or any miss.
"""
from __future__ import annotations

import argparse
import collections
import re
import sys
from pathlib import Path

# "skel-shadow:" is the line's name before step 3 (2026-11-16).
STAT = re.compile(r"^\[\S+ (\S+)\].*skel(?:-shadow)?: (?:mode (\w+) )?frame (\d+) (.*)$")
DIV = re.compile(r"^\[\S+ (\S+)\].*skel(?:-shadow)?: divergent vs (\w+) (.*)$")
JUMP = re.compile(r"^\[\S+ (\S+)\].*skel: jump (.*)$")
FIELD = re.compile(r"(\w+)=(\d+)")
GROUP = re.compile(r"(\w+)\[([^\]]*)\]")
PLAIN = re.compile(r"\b(items|built|served|tail|unmapped) (\d+)")


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
    ap.add_argument("--max-div", type=int, default=None)
    ap.add_argument("--top", type=int, default=6)
    a = ap.parse_args()
    failed = False
    for arg in a.logs:
        lines = open(find_log(arg), errors="replace").read().splitlines()
        lo, hi = "", "99"
        if a.reel:
            for ln in lines:
                if "harness: reel" in ln and "playing from" in ln:
                    lo = ln.split()[1]
                if "harness: reel" in ln and "exit posted" in ln:
                    hi = ln.split()[1]
        tot = collections.Counter()
        divs = []
        jumps = []
        modes = set()
        nlines = 0
        for ln in lines:
            m = STAT.search(ln)
            if m and lo <= m.group(1) <= hi:
                nlines += 1
                modes.add(m.group(2) or "shadow")
                body = m.group(4)
                for k, v in PLAIN.findall(body):
                    tot[k] += int(v)
                for g, inner in GROUP.findall(body):
                    if g == "worst":
                        continue
                    for k, v in FIELD.findall(inner):
                        tot[f"{g}.{k}"] += int(v)
                continue
            d = DIV.search(ln)
            if d and lo <= d.group(1) <= hi and len(divs) < a.top:
                divs.append(f"vs {d.group(2)} {d.group(3)}")
            j = JUMP.search(ln)
            if j and lo <= j.group(1) <= hi and len(jumps) < a.top:
                jumps.append(f"{j.group(1)} {j.group(2)}")
        print(f"== {arg}{' (reel only)' if a.reel else ''}: {nlines} stat lines, "
              f"mode {'/'.join(sorted(modes)) or '-'}")
        if not nlines:
            print("  no skel: lines (skeleton cvar off?)")
            failed = True
            continue
        items, built = tot["items"], tot["built"]
        miss = sum(tot[f"miss.{k}"] for k in ("noent", "notick", "nopart", "skipped"))
        print(f"  items {items}, built {built} ({100.0 * built / max(items, 1):.2f}%), "
              f"missing {miss}: " + ", ".join(f"{k}={tot['miss.' + k]}" for k in
                                             ("noent", "notick", "nopart", "skipped")))
        for g, extra in (("bank", "none"), ("serve", "unserved")):
            print(f"  vs {g:<5} " + "  ".join(f"{k}={tot[f'{g}.{k}']}" for k in
                                              ("new", "old", "close", "div", extra)))
        print("  store    " + "  ".join(f"{k}={tot['store.' + k]}" for k in
                                        ("ticks", "unpacked", "unowned", "foreign", "ltow_var",
                                         "part_skip", "part_bad", "pose_unread")))
        if "on" in modes:
            print(f"  served   {tot['served']} pieces, {tot['tail']} more in the rescued tail")
        print("  jumps    " + "  ".join(f"{k}={tot['jump.' + k]}" for k in ("root", "gap", "parts")))
        for jl in jumps:
            print(f"    jump {jl}")
        for d in divs:
            print(f"    {d}")
        ndiv = tot["bank.div"] + tot["serve.div"]
        if a.max_div is not None and (ndiv > a.max_div or miss > 0):
            print(f"FAIL: {ndiv} divergent pieces (max {a.max_div}), {miss} missing")
            failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
