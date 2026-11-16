#!/usr/bin/env python3
"""Guest CPU profile report (RE-7 target ranking).

Reads <run>/profile.txt (written by the harness `profile` verb,
src/harness/harness_profile.cpp), symbolizes every sampled address against
the local build that matches the run (run_scenario.resolve: llvm-symbolizer
on skate3 / rexruntime, with the RE names from docs/re/symbols.toml and the
Skate 2 name map), and ranks functions by self and inclusive time.

  profile_report.py <run> [--thread main] [--top 40] [--json out.json]
  profile_report.py <base_run> <new_run> [--thread main]   # per-function diff

Times are ms per game frame: samples / hz / frames. Linux samples are thread
CPU time; Windows samples are wall time with idle samples (no CPU cycles
used since the last one) dropped, so both read as CPU time.

The build must be the one the run used (same version): the report checks
build.json against the local build's version and warns otherwise.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_scenario as rs  # noqa: E402

CHUNK = 4000
WAIT = re.compile(r"rex::thread::(Wait|AlertableSleep|Sleep|MaybeYield)|XObject::Wait|"
                  r"XThread::Delay|NtWaitFor|KeWaitFor|KeDelayExecution|NtYield|"
                  r"SleepConditionVariable|WaitForSingleObject|WaitForMultipleObjects|"
                  r"pthread_cond_(timed)?wait|futex|nanosleep|sched_yield")


def load(run: Path) -> dict:
    path = run / "profile.txt"
    if not path.exists():
        rs.die(f"{path} is missing (run a scenario with a `profile` step)")
    prof = {"hz": 1000, "seconds": 0.0, "frames": 0, "threads": {}, "modules": {}, "stacks": [],
            "guest": {}, "exe": None}
    for line in path.read_text().splitlines():
        if line.startswith("guest "):
            # Windows: host function start (exe offset) -> guest address.
            _, off, guest = line.split()
            prof["guest"][int(off, 16)] = guest.upper()
        elif line.startswith("exe "):
            prof["exe"] = int(line.split()[1])
        elif line.startswith("#"):
            f = line.split()
            for k in ("hz", "seconds", "frames", "dropped"):
                if k in f:
                    prof[k] = float(f[f.index(k) + 1])
        elif line.startswith("thread "):
            f = line.split()
            prof["threads"][int(f[1])] = {"name": f[2], "samples": int(f[4]), "idle": int(f[6]),
                                          "ticks": int(f[8]) if len(f) > 8 else 0}
        elif line.startswith("module "):
            _, i, name = line.split(" ", 2)
            prof["modules"][int(i)] = name
        elif line.startswith("s "):
            f = line.split()
            frames = []
            for ref in f[3:]:
                mi, off = ref.split("+")
                frames.append((int(mi), int(off, 16)))
            prof["stacks"].append((int(f[1]), int(f[2]), frames))
    return prof


def target_of(run: Path) -> tuple[str, str]:
    b = json.loads((run / "build.json").read_text())
    target = "odin2" if "arm64" in b.get("platform", "") else "windows"
    config = "relwithdebinfo" if "debinfo" in b.get("build_config", "").lower() else "release"
    return target, config


def symbolize(run: Path, prof: dict) -> dict[tuple[int, int], str]:
    """(module index, offset) -> function name (the outermost frame: the
    physical function, not an inlined callee)."""
    target, config = target_of(run)
    files = rs.module_files(target, config)
    specs: dict[str, tuple[int, int]] = {}
    names: dict[tuple[int, int], str] = {}
    for _, _, frames in prof["stacks"]:
        for mi, off in frames:
            mod = prof["modules"].get(mi, "?")
            if mi == prof["exe"] and prof["guest"]:
                # Windows: offsets in the exe are function starts (the
                # sampler looked them up in the unwind table), so the
                # recompiled ones are named without a PDB.
                if off in prof["guest"]:
                    names[(mi, off)] = rs.game_names(f"sub_{prof['guest'][off]}")
                    continue
            key = rs.module_key(mod, files)
            if key is None:
                names[(mi, off)] = f"[{mod}]"
                continue
            specs[f"{key}+{off:x}"] = (mi, off)
    keys = list(specs)
    print(f"symbolizing {len(keys)} addresses ({target} {config})...", file=sys.stderr)
    for i in range(0, len(keys), CHUNK):
        part = keys[i:i + CHUNK]
        resolved, errors = rs.resolve(target, config, part)
        for e in errors[:3]:
            print(e, file=sys.stderr)
        for spec in part:
            frames = resolved.get(spec) or []
            fn = frames[-1].split("  ")[0] if frames else ""
            names[specs[spec]] = fn if fn and not fn.startswith("??") else spec
        print(f"  {min(i + CHUNK, len(keys))}/{len(keys)}", file=sys.stderr)
    return names


def rank(prof: dict, names: dict, thread: str | None) -> dict[str, dict]:
    tids = {i for i, t in prof["threads"].items() if thread is None or t["name"] == thread}
    stats: dict[str, dict] = defaultdict(lambda: {"self": 0, "incl": 0,
                                                  "callers": defaultdict(int)})
    total = 0
    waiting = 0
    for count, tid, frames in prof["stacks"]:
        if tid not in tids or not frames:
            continue
        fns = [names.get(f, "?") for f in frames]
        # Blocked or sleeping (a wait with a timeout, the fps cap, a yield):
        # not CPU time, so not ranked.
        if any(WAIT.search(fn) for fn in fns):
            waiting += count
            continue
        total += count
        me = fns[0]
        stats[me]["self"] += count
        caller = next((f for f in fns[1:] if f != me), None)
        if caller:
            stats[me]["callers"][caller] += count
        for fn in set(fns):
            stats[fn]["incl"] += count
    for s in stats.values():
        s["total"] = total
    prof["waiting"] = waiting
    return stats


def ms_per_frame(prof: dict, samples: int) -> float:
    """Windows samples come at the sampler's real rate (ticks per thread over
    the profile's seconds), not exactly hz; Linux ones per 1/hz of CPU time."""
    frames = max(1.0, prof["frames"])
    ticks = [t["ticks"] for t in prof["threads"].values() if t.get("ticks")]
    ms_per_sample = (prof["seconds"] * 1000.0 / (sum(ticks) / len(ticks)) if ticks
                     else 1000.0 / prof["hz"])
    return samples * ms_per_sample / frames


def short(fn: str, n: int = 70) -> str:
    return fn if len(fn) <= n else fn[: n - 1] + "…"


def report(run: Path, thread: str | None, top: int, out_json: Path | None) -> None:
    prof = load(run)
    b = json.loads((run / "build.json").read_text())
    names = symbolize(run, prof)
    stats = rank(prof, names, thread)
    total = next(iter(stats.values()))["total"] if stats else 0
    print(f"{run.name}: {b.get('version')} {b.get('platform')} {b.get('build_config')}")
    print(f"  {prof['hz']:.0f} Hz, {prof['seconds']:.1f} s, {prof['frames']:.0f} frames, "
          f"{total} samples ({thread or 'all threads'}), "
          f"{ms_per_frame(prof, total):.2f} ms/frame busy, "
          f"{ms_per_frame(prof, prof['waiting']):.2f} ms/frame waiting (not ranked)")
    for i, t in sorted(prof["threads"].items()):
        print(f"  thread {i} {t['name']}: {t['samples']} samples, {t['idle']} idle")
    print(f"\n  {'self%':>6} {'ms/f':>6} {'incl%':>6} {'ms/f':>6}  function  <- top caller")
    rows = sorted(stats.items(), key=lambda kv: -kv[1]["self"])[:top]
    for fn, s in rows:
        caller = max(s["callers"].items(), key=lambda kv: kv[1])[0] if s["callers"] else ""
        print(f"  {100 * s['self'] / total:6.2f} {ms_per_frame(prof, s['self']):6.2f} "
              f"{100 * s['incl'] / total:6.2f} {ms_per_frame(prof, s['incl']):6.2f}  "
              f"{short(fn)}" + (f"  <- {short(caller, 50)}" if caller else ""))
    print(f"\n  by inclusive time (top {top // 2}):")
    for fn, s in sorted(stats.items(), key=lambda kv: -kv[1]["incl"])[: top // 2]:
        print(f"  {100 * s['incl'] / total:6.2f} {ms_per_frame(prof, s['incl']):6.2f}  {short(fn)}")
    if out_json:
        out_json.write_text(json.dumps({
            "run": run.name, "build": b, "hz": prof["hz"], "frames": prof["frames"],
            "total": total, "thread": thread,
            "functions": [{"name": fn, "self": s["self"], "incl": s["incl"],
                           "self_ms": ms_per_frame(prof, s["self"]),
                           "incl_ms": ms_per_frame(prof, s["incl"]),
                           "callers": dict(sorted(s["callers"].items(),
                                                  key=lambda kv: -kv[1])[:5])}
                          for fn, s in sorted(stats.items(), key=lambda kv: -kv[1]["self"])],
        }, indent=1))
        print(f"\nwrote {out_json}")


def diff(base: Path, new: Path, thread: str | None, top: int) -> None:
    out = {}
    for run in (base, new):
        prof = load(run)
        stats = rank(prof, symbolize(run, prof), thread)
        out[run] = (prof, stats)
    (pb, sb), (pn, sn) = out[base], out[new]
    tb = sum(s["self"] for s in sb.values())
    tn = sum(s["self"] for s in sn.values())
    print(f"base {base.name}: {ms_per_frame(pb, tb):.2f} ms/frame sampled")
    print(f"new  {new.name}: {ms_per_frame(pn, tn):.2f} ms/frame sampled")
    fns = set(sb) | set(sn)
    rows = []
    for fn in fns:
        a = ms_per_frame(pb, sb[fn]["self"]) if fn in sb else 0.0
        c = ms_per_frame(pn, sn[fn]["self"]) if fn in sn else 0.0
        rows.append((c - a, a, c, fn))
    print(f"\n  {'base':>6} {'new':>6} {'delta':>7}  function (self ms/frame)")
    for d, a, c, fn in sorted(rows, key=lambda r: -abs(r[0]))[:top]:
        print(f"  {a:6.3f} {c:6.3f} {d:+7.3f}  {short(fn)}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("runs", nargs="+", type=Path, help="run folder, or base and new")
    ap.add_argument("--thread", help="main or render (default: all)")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--json", type=Path, help="write the full ranking as JSON")
    a = ap.parse_args()
    if len(a.runs) == 1:
        report(a.runs[0], a.thread, a.top, a.json)
    elif len(a.runs) == 2:
        diff(a.runs[0], a.runs[1], a.thread, a.top)
    else:
        ap.error("one run, or base and new")
    return 0


if __name__ == "__main__":
    sys.exit(main())
