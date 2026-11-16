#!/usr/bin/env python3
"""Turn a play_reel capture into an x264 MP4 at true speed (plus a slow pass).

    reel_video.py <run dir> <out.mp4> [--label TEXT] [--slow 4] [--max-mb 10]
                  [--tag rf] [--fps 30]

The harness captures a reel's frames as screenshots (`play_reel ...
every_frames = N`), but it can't always keep up: shots land every 2-3 game
frames. Each still is held for the frames until the next one (from the
frame number in its name), so the video plays at the game's real speed at
--fps. With --slow N the same frames follow at 1/N speed. --label draws a
caption. The x264 quality steps down until the file is under --max-mb.

Shots come from the play_reel "done" event in events.jsonl (the reel's own
frames, not the loading shots). Needs ffmpeg with libx264 and drawtext.
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"


def reel_shots(run: Path, tag: str) -> list[Path]:
    for line in open(run / "events.jsonl"):
        e = json.loads(line)
        if e.get("verb") == "play_reel" and e.get("kind") == "done" and e.get("tag") == tag:
            return [(run / s).resolve() for s in e["shots"]]
    sys.exit(f"no play_reel done event with tag {tag!r} in {run}")


def concat_list(shots: list[Path], fps: float, slow: float, path: Path) -> None:
    frames = [int(re.search(r"f(\d+)_", s.name).group(1)) for s in shots]
    with open(path, "w") as f:
        for i, s in enumerate(shots):
            gap = (frames[i + 1] - frames[i]) if i + 1 < len(shots) else 1
            f.write(f"file '{s}'\nduration {gap * slow / fps:.6f}\n")
        f.write(f"file '{shots[-1]}'\n")  # concat demuxer: last duration needs a repeat


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("run", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--label", default="")
    ap.add_argument("--slow", type=float, default=4.0, help="0 = no slow pass")
    ap.add_argument("--max-mb", type=float, default=10.0)
    ap.add_argument("--tag", default="rf")
    ap.add_argument("--fps", type=float, default=30.0)
    a = ap.parse_args()
    shots = reel_shots(a.run, a.tag)
    a.out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=a.out.parent) as tmp:
        parts = []
        for speed, note in [(1.0, "real speed")] + ([(a.slow, f"1/{a.slow:g} speed")] if a.slow else []):
            lst = Path(tmp) / f"list_{len(parts)}.txt"
            concat_list(shots, a.fps, speed, lst)
            text = f"{a.label}  ({note})" if a.label else note
            text = text.replace(":", r"\:").replace("'", "")
            vf = (f"scale=1280:-2,fps={a.fps:g},"
                  f"drawtext=fontfile={FONT}:text='{text}':x=24:y=24:fontsize=30:"
                  f"fontcolor=white:box=1:boxcolor=black@0.6:boxborderw=10:"
                  f"expansion=none")
            part = Path(tmp) / f"part_{len(parts)}.mp4"
            subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-f", "concat", "-safe", "0",
                            "-i", str(lst), "-vf", vf, "-c:v", "libx264", "-crf", "16",
                            "-preset", "slow", "-pix_fmt", "yuv420p", str(part)], check=True)
            parts.append(part)
        joined = Path(tmp) / "joined.txt"
        joined.write_text("".join(f"file '{p}'\n" for p in parts))
        for crf in (20, 23, 26, 29, 32, 35):
            subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-f", "concat", "-safe", "0",
                            "-i", str(joined), "-c:v", "libx264", "-crf", str(crf),
                            "-preset", "slow", "-pix_fmt", "yuv420p", "-movflags", "+faststart",
                            str(a.out)], check=True)
            mb = a.out.stat().st_size / 1e6
            if mb < a.max_mb:
                print(f"{a.out}: {len(shots)} shots, crf {crf}, {mb:.1f} MB")
                return 0
        sys.exit(f"{a.out}: still {mb:.1f} MB at crf 35")


if __name__ == "__main__":
    raise SystemExit(main())
