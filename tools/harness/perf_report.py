#!/usr/bin/env python3
"""Performance numbers from a harness run's log.txt -> perf.json, and run A/B.

Sources (both off by default; scenarios/perf_reel.toml turns them on):
  - the native renderer's CPU stage line (skate3_native_render_scene_perf_log):
      native-scene perf: guest_fps=29 guest_dt_max=75.5ms ... render=0.13/3.84ms ...
    every skate3_native_render_scene_perf_interval frames; "x=avg/max ms" pairs.
  - the backend's GPU timestamp-bucket line (d3d12_gpu_timestamp_buckets /
    vulkan_gpu_timestamp_buckets), every 120 frames, average microseconds per
    frame for each bucket ("Vulkan" lines add a per-frame count in parentheses):
      D3D12 GPU profile (avg us/frame over 120 frames): span=527 buckets=455 untimed=72 | Main=114 ... NatMain=15 ...
    Nat* buckets are the native renderer's passes (Cmd::ProfileRegion stages);
    nat_total is their sum. On Vulkan "ResolveGap" also absorbs idle time under
    a frame cap, so span is not GPU busy time; compare the Nat* buckets.

Lines are grouped into segments: "reel<N>" between the harness's "reel N: playing
from" and "reel N: exit posted" log lines, named ones between a scenario's
`mark text = "perf <name>"` and `"perf end"` (e.g. live gameplay to compare
with the reels; the first line of each segment is dropped: its window started
before it), and "all" for every line after
the scenario's first step. Values are averaged over a segment's lines; *_max
fields keep the worst.

  perf_report.py <run-dir>                 # write perf.json, print the summary
  perf_report.py <base-run> <new-run>      # compare (writes neither)
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

TS = re.compile(r"^\[(\S+ \S+)\]")
CPU = re.compile(r"native-scene perf: (.*)$")
GPU = re.compile(r"(D3D12|Vulkan) GPU profile \(avg us/frame over (\d+) frames\): (.*?) \|(.*)$")
REEL_START = re.compile(r"harness: reel (\d+): playing from")
REEL_END = re.compile(r"harness: reel (\d+): exit posted")
# Named segments from scenario marks: `mark text = "perf <name>"` starts one
# (e.g. a live-gameplay stretch to compare with the reels), `"perf end"` ends it.
MARK_SEG = re.compile(r"harness: mark: perf (\S+)")
SCENARIO_START = re.compile(r"harness: step 1 ")
PAIR_MS = re.compile(r"(\w+)=([\d.]+)/([\d.]+)ms")
KV = re.compile(r"(\w+)=([\d.]+)(?:\((\d+)\))?")


def parse_cpu(rest: str) -> dict:
    out = {}
    m = re.search(r"guest_fps=([\d.]+)", rest)
    if m:
        out["guest_fps"] = float(m.group(1))
    m = re.search(r"guest_dt_max=([\d.]+)ms", rest)
    if m:
        out["guest_dt_max_ms"] = float(m.group(1))
    for name, avg, mx in PAIR_MS.findall(rest):
        out[f"{name}_ms"] = float(avg)
        out[f"{name}_max_ms"] = float(mx)
    return out


def parse_gpu(head: str, buckets: str) -> dict:
    out = {}
    for name, val, _n in KV.findall(head):
        out[name] = float(val)
    nat = 0.0
    for name, val, _n in KV.findall(buckets):
        out[name] = float(val)
        if name.startswith("Nat"):
            nat += float(val)
    out["nat_total"] = nat
    return out


def collect(log: Path) -> dict:
    segs: dict[str, dict] = {}
    started = False
    reel = None
    first_in_reel = {"cpu": False, "gpu": False}
    backend = None

    def add(seg: str, kind: str, values: dict) -> None:
        s = segs.setdefault(seg, {"cpu": [], "gpu": []})
        s[kind].append(values)

    for line in log.read_text(errors="replace").splitlines():
        if not started:
            if SCENARIO_START.search(line):
                started = True
            continue
        m = REEL_START.search(line)
        if m:
            reel = f"reel{m.group(1)}"
            first_in_reel = {"cpu": True, "gpu": True}
            continue
        if REEL_END.search(line):
            reel = None
            continue
        m = MARK_SEG.search(line)
        if m:
            name = m.group(1)
            reel = None if name == "end" else name
            first_in_reel = {"cpu": True, "gpu": True}
            continue
        m = CPU.search(line)
        kind, values = None, None
        if m:
            kind, values = "cpu", parse_cpu(m.group(1))
        else:
            m = GPU.search(line)
            if m:
                backend = m.group(1).lower()
                kind, values = "gpu", parse_gpu(m.group(3), m.group(4))
        if kind is None:
            continue
        add("all", kind, values)
        if reel is not None:
            if first_in_reel[kind]:
                first_in_reel[kind] = False
                continue
            add(reel, kind, values)

    def reduce(rows: list[dict]) -> dict:
        keys = sorted({k for r in rows for k in r})
        out = {}
        for k in keys:
            vals = [r[k] for r in rows if k in r]
            out[k] = max(vals) if k.endswith("_max_ms") else round(sum(vals) / len(vals), 3)
        return out

    result = {"backend": backend, "segments": {}}
    for seg, s in segs.items():
        result["segments"][seg] = {
            "cpu_windows": len(s["cpu"]),
            "gpu_windows": len(s["gpu"]),
            "cpu": reduce(s["cpu"]),
            "gpu": reduce(s["gpu"]),
        }
    return result


def run_meta(run: Path) -> str:
    try:
        r = json.loads((run / "result.json").read_text())
        return f"v{r.get('version')} {r.get('platform')} {r.get('build_config')}"
    except (OSError, ValueError):
        return ""


CPU_SHOW = ["guest_fps", "guest_dt_max_ms", "build_ms", "render_ms", "items_ms", "shadow_ms",
            "twod_ms", "commit_ms"]


def gpu_show(gpu: dict) -> list[str]:
    nat = sorted((k for k in gpu if k.startswith("Nat")), key=lambda k: -gpu[k])
    return ["nat_total"] + nat + [k for k in ("Main", "span") if k in gpu]


def print_summary(run: Path, perf: dict) -> None:
    segs = perf["segments"]
    if not segs:
        return
    print(f"  perf ({perf.get('backend') or 'no GPU lines'}; perf.json):")
    for seg in sorted(segs, key=lambda s: (s == "all", s)):
        d = segs[seg]
        cpu, gpu = d["cpu"], d["gpu"]
        parts = []
        if cpu:
            parts.append(f"fps {cpu.get('guest_fps', 0):.0f} worst {cpu.get('guest_dt_max_ms', 0):.0f} ms, "
                         f"cpu render {cpu.get('render_ms', 0):.2f} ms build {cpu.get('build_ms', 0):.2f} ms")
        if gpu:
            nat = [k for k in gpu if k.startswith("Nat") and gpu[k] > 0]
            top = sorted(nat, key=lambda k: -gpu[k])[:5]
            parts.append(f"gpu native {gpu['nat_total'] / 1000:.2f} ms ("
                         + " ".join(f"{k[3:]}={gpu[k] / 1000:.2f}" for k in top) + ")")
        print(f"    {seg:<12} " + " | ".join(parts) +
              f"  [{d['cpu_windows']} cpu / {d['gpu_windows']} gpu windows]")


def write(run: Path) -> dict | None:
    log = run / "log.txt"
    if not log.exists():
        return None
    perf = collect(log)
    if not perf["segments"]:
        return perf
    (run / "perf.json").write_text(json.dumps(perf, indent=2) + "\n")
    return perf


def load(run: Path) -> dict:
    p = run / "perf.json"
    if p.exists():
        return json.loads(p.read_text())
    perf = write(run)
    if perf is None:
        sys.exit(f"{run}: no log.txt")
    return perf


def compare(base: Path, new: Path) -> int:
    a, b = load(base), load(new)
    print(f"base {base.name} {run_meta(base)} [{a.get('backend')}]")
    print(f"new  {new.name} {run_meta(new)} [{b.get('backend')}]")
    common = [s for s in a["segments"] if s in b["segments"]]
    if not common:
        print("no common segments")
        return 1
    for seg in sorted(common, key=lambda s: (s == "all", s)):
        sa, sb = a["segments"][seg], b["segments"][seg]
        print(f"== {seg}")
        rows = [("cpu", k) for k in CPU_SHOW] + [("gpu", k) for k in gpu_show({**sa["gpu"], **sb["gpu"]})]
        for kind, k in rows:
            va, vb = sa[kind].get(k), sb[kind].get(k)
            if va is None and vb is None:
                continue
            unit = "us" if kind == "gpu" else ""
            if va is None or vb is None:
                fa = "-" if va is None else f"{va:.2f}"
                fb = "-" if vb is None else f"{vb:.2f}"
                if (va or 0) or (vb or 0):
                    print(f"  {kind} {k:<22} {fa:>10} {fb:>10} {unit}")
                continue
            if va == 0 and vb == 0:
                continue
            delta = (vb - va) / va * 100 if va else float("inf")
            print(f"  {kind} {k:<22} {va:>10.2f} {vb:>10.2f} {unit:<2} {delta:+7.1f}%")
    return 0


def main(argv: list[str]) -> int:
    if len(argv) == 2:
        run = Path(argv[1])
        perf = write(run)
        if perf is None:
            print(f"{run}: no log.txt")
            return 1
        if not perf["segments"]:
            print(f"{run}: no perf lines (run with scenarios/perf_reel.toml or the perf cvars)")
            return 1
        print_summary(run, perf)
        return 0
    if len(argv) == 3:
        return compare(Path(argv[1]), Path(argv[2]))
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
