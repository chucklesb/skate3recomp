#!/usr/bin/env python3
"""Screenshots of a live session, taken automatically while the user plays.

    live_shots.py [--target odin2|windows] [--every 4] [--match 0,1]
                  [--max 120] [--minutes 10] [--settle 1.0]

Polls the live game's state (`send query what=state`) about once a second
and takes a screenshot:
  - at every frontend-stack change, --settle seconds later (tag
    `ctx_<stack>`, e.g. ctx_0-1-47-2);
  - every --every seconds while the stack starts with --match (default
    0,1: a challenge and its screens), tag `run_<stack>`.
  - with --video-frames N: on entering a --match context, a frame dump of
    the next N frames instead (`video` verb, video/vid_<stack>/): every
    frame, for screens that last only a few seconds (a photo burst runs
    ~5 s; the polling takes seconds per round trip through adb).
Stack entries match by screen id (`1:47` matches `1`).
Stops after --max shots or --minutes. The files land in the live run's
shots/; `fetch` pulls them. Needs the same environment as run_scenario.py
send (on this PC, run it from the rs.sh wrapper's environment).
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

RUN = Path(__file__).resolve().parent / "run_scenario.py"


def send(target: str, *args: str) -> dict:
    cmd = [sys.executable, "-I", str(RUN), "send"]
    if target != "odin2":
        cmd += ["--target", target]
    out = subprocess.run(cmd + list(args), capture_output=True, text=True, timeout=300).stdout
    for line in reversed(out.strip().splitlines()):
        try:
            return json.loads(line)
        except json.JSONDecodeError:
            continue
    return {}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", default="odin2")
    ap.add_argument("--every", type=float, default=4.0)
    ap.add_argument("--match", default="0,1")
    ap.add_argument("--max", type=int, default=120)
    ap.add_argument("--minutes", type=float, default=10.0)
    ap.add_argument("--settle", type=float, default=1.0)
    ap.add_argument("--video-frames", type=int, default=0)
    a = ap.parse_args()
    match = [x for x in a.match.split(",") if x]
    in_match = None  # the context it starts in doesn't trigger a dump
    end = time.monotonic() + a.minutes * 60
    last_ctx, changed_at, shot_ctx, last_periodic, shots = None, 0.0, None, 0.0, 0
    while time.monotonic() < end and shots < a.max:
        st = send(a.target, "query", "what=state").get("state")
        if st is None:
            print("no state (game gone?)", flush=True)
            time.sleep(2)
            continue
        fe = [str(x) for x in st.get("fe", [])]
        ctx = "-".join(fe) or "none"
        now = time.monotonic()
        if ctx != last_ctx:
            last_ctx, changed_at = ctx, now
        matched = [x.split(":")[0] for x in fe[:len(match)]] == match
        if a.video_frames and matched and in_match is False:
            vtag = "vid_" + ctx.replace(":", "_")
            r = send(a.target, "video", f"frames={a.video_frames}", f"tag={vtag}")
            print(f"{time.strftime('%H:%M:%S')} video {vtag}: {r}", flush=True)
            shots += 1
        in_match = matched
        if a.video_frames and matched:
            time.sleep(1.0)
            continue
        tag = None
        if ctx != shot_ctx and now - changed_at >= a.settle:
            tag, shot_ctx = f"ctx_{ctx}", ctx
        elif matched and now - last_periodic >= a.every:
            tag = f"run_{ctx}"
        if tag:
            r = send(a.target, "screenshot", f"tag={tag}")
            last_periodic = now
            shots += 1
            print(f"{time.strftime('%H:%M:%S')} {tag} {'ok' if r.get('ok') else r}", flush=True)
        time.sleep(1.0)
    print(f"done: {shots} shots", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
