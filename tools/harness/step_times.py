#!/usr/bin/env python3
"""Where scenario run time goes: per-step durations from events.jsonl.

    step_times.py [RUN ...] [--platform win-amd64|linux-aarch64] [--all]
                  [--top N] [--runs] [--json]

With no RUN, reads the latest run of every scenario that is still in
scenarios/ (--all: every scenario name in out/harness-runs). Each step's time is its `done` event minus
its `step` event. Steps are grouped by verb and what they wait for
("wait frames=120", "wait gameplay", "teleport (start_spot)", ...), so the
fixed delays that add up across scenarios stand out.

Runs from before 2026-11-17 have no `cond` / `implicit` fields in their step
events; the step is then looked up in scenarios/<name>.toml (with the
harness's start_spot settle + teleport inserted after the first gameplay
wait, as harness_script.cpp does).
"""
from __future__ import annotations

import argparse
import json
import re
import sys
import tomllib
from collections import defaultdict
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
RUNS = REPO / "out/harness-runs"
WAIT_KEYS = ("gameplay", "frames", "ms", "fe_screen", "presence", "log", "speed_gt", "speed_lt")


def wait_cond(step: dict) -> str:
    parts = []
    for k in WAIT_KEYS:
        if k not in step:
            continue
        v = step[k]
        if k == "gameplay":
            if v:
                parts.append("gameplay")
        elif k in ("log", "speed_gt", "speed_lt"):
            parts.append(k)
        else:
            parts.append(f"{k}={v}")
    return " ".join(parts)


def scenario_steps(name: str) -> list[dict] | None:
    """The scenario's steps as the harness runs them (start_spot expanded)."""
    path = REPO / "scenarios" / f"{name}.toml"
    if not path.is_file():
        return None
    try:
        doc = tomllib.loads(path.read_text())
    except (tomllib.TOMLDecodeError, OSError):
        return None
    steps = [dict(s) for s in doc.get("step", [])]
    if doc.get("start_spot"):
        for i, s in enumerate(steps):
            if s.get("do") == "wait" and s.get("gameplay"):
                steps[i + 1:i + 1] = [
                    {"do": "wait", "frames": 90, "_implicit": "start_spot"},
                    {"do": "teleport", "_implicit": "start_spot"},
                ]
                break
    return steps


def describe(ev: dict, toml_step: dict | None) -> str:
    verb = ev.get("verb", "?")
    implicit = ev.get("implicit") or (toml_step or {}).get("_implicit")
    cond = ev.get("cond")
    if cond is None and verb == "wait" and toml_step is not None:
        cond = wait_cond(toml_step)
    key = verb + (f" {cond}" if verb == "wait" and cond else "")
    if verb == "input" and toml_step and toml_step.get("until_screen_changes"):
        key += " until_screen_changes"
    if implicit:
        key += f" ({implicit})"
    return key


def read_run(run: Path) -> dict | None:
    ev_path = run / "events.jsonl"
    if not ev_path.is_file():
        return None
    events = []
    for line in ev_path.read_text(errors="replace").splitlines():
        try:
            events.append(json.loads(line))
        except ValueError:
            continue
    name = re.sub(r"^\d{8}-\d{6}_", "", run.name)
    toml_steps = scenario_steps(name)
    starts: dict[int, dict] = {}
    steps = []
    total = None
    for ev in events:
        kind = ev.get("kind")
        if kind == "step" and "step" in ev:
            starts[ev["step"]] = ev
        elif kind == "done" and ev.get("step") in starts:
            st = starts.pop(ev["step"])
            idx = st["step"]
            ts = None
            if toml_steps and 0 < idx <= len(toml_steps):
                ts = toml_steps[idx - 1]
                if ts.get("do") and ts["do"] != st.get("verb"):
                    ts = None  # the scenario changed since this run
            steps.append((idx, describe(st, ts), ev["t"] - st["t"]))
        elif kind == "finish":
            total = ev.get("t")
    platform = ""
    try:
        platform = json.loads((run / "build.json").read_text()).get("platform", "")
    except (OSError, ValueError):
        pass
    if total is None:
        total = max((e.get("t", 0) for e in events), default=0)
    first = next((e["t"] for e in events if e.get("kind") == "step"), 0.0)
    return {"run": run.name, "name": name, "platform": platform, "total": total,
            "before_first_step": first, "steps": steps}


def latest_runs(platform: str | None, include_gone: bool = False) -> list[Path]:
    best: dict[str, Path] = {}
    for run in sorted(RUNS.iterdir()) if RUNS.is_dir() else []:
        if not (run / "events.jsonl").is_file():
            continue
        if platform:
            try:
                if json.loads((run / "build.json").read_text()).get("platform") != platform:
                    continue
            except (OSError, ValueError):
                continue
        name = re.sub(r"^\d{8}-\d{6}_", "", run.name)
        if not include_gone and not (REPO / "scenarios" / f"{name}.toml").is_file():
            continue  # a one-off scenario that no longer exists
        best[name] = run  # sorted by stamp: the last one wins
    return list(best.values())


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("runs", nargs="*", type=Path)
    ap.add_argument("--platform", help="only runs from this platform (build.json)")
    ap.add_argument("--top", type=int, default=25, help="rows in the grouped table")
    ap.add_argument("--runs", dest="per_run", action="store_true",
                    help="also print every run's steps")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--all", action="store_true",
                    help="also runs of scenarios no longer in scenarios/ (their waits show no condition)")
    a = ap.parse_args()
    paths = [p.resolve() for p in a.runs] or latest_runs(a.platform, a.all)
    runs = [r for r in (read_run(p) for p in paths) if r and r["steps"]]
    if not runs:
        print("no runs with step events", file=sys.stderr)
        return 1
    groups: dict[str, list[float]] = defaultdict(list)
    where: dict[str, set[str]] = defaultdict(set)
    for r in runs:
        for _, key, dt in r["steps"]:
            groups[key].append(dt)
            where[key].add(r["name"])
    grand = sum(r["total"] for r in runs)
    if a.json:
        print(json.dumps({"runs": runs, "groups": {k: {"count": len(v), "total_s": sum(v)}
                                                  for k, v in groups.items()}}, indent=1))
        return 0
    if a.per_run:
        for r in sorted(runs, key=lambda r: -r["total"]):
            print(f"== {r['run']} ({r['platform']}): {r['total']:.1f} s")
            for idx, key, dt in r["steps"]:
                print(f"   {idx:3d} {dt:7.2f} s  {key}")
    boot = sum(r["before_first_step"] for r in runs)
    print(f"{len(runs)} runs, {grand:.0f} s in all (harness time; {boot:.0f} s before the first step)")
    print(f"{'total s':>8} {'share':>6} {'count':>5} {'mean s':>7} {'max s':>6}  step (scenarios)")
    for key, v in sorted(groups.items(), key=lambda kv: -sum(kv[1]))[:a.top]:
        names = sorted(where[key])
        shown = ", ".join(names[:4]) + (f" +{len(names) - 4}" if len(names) > 4 else "")
        print(f"{sum(v):8.1f} {100 * sum(v) / grand:5.1f}% {len(v):5d} {sum(v) / len(v):7.2f} "
              f"{max(v):6.2f}  {key}  ({shown})")
    print("\nper scenario (longest first):")
    for r in sorted(runs, key=lambda r: -r["total"])[:a.top]:
        print(f"  {r['total']:7.1f} s  {r['name']}  [{r['platform']}]")
    return 0


if __name__ == "__main__":
    sys.exit(main())
