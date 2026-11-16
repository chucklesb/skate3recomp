#!/usr/bin/env python3
"""Turn a play_reel run into an x264 MP4 at true speed.

    reel_video.py <run dir> <out.mp4> [--label TEXT] [--slow N] [--max-mb 10]
                  [--tag rf] [--fps 30]

Frame dumps (`play_reel ... video = true`, or the `video` verb): the run has
every rendered frame in video/<tag>/f<n>.png with the reel clock stepping
1/step_fps per frame, so the frames play back at step_fps as they are. The
script checks frames.jsonl for clock steps that aren't one step (a dropped or
repeated frame) and reports them.

Older runs with periodic screenshots (`every_frames = N`): each still is held
for the frames until the next one (from the frame number in its name), at
--fps.

--slow N appends the same frames at 1/N speed (off by default). --label draws
a caption. The x264 quality steps down until the file is under --max-mb.
Needs ffmpeg with libx264 and drawtext.
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


def reel_done(run: Path, tag: str) -> dict:
    for line in open(run / "events.jsonl"):
        e = json.loads(line)
        if e.get("kind") == "done" and e.get("tag") == tag and e.get("verb") in ("play_reel", "video"):
            return e
    sys.exit(f"no play_reel/video done event with tag {tag!r} in {run}")


def check_steps(meta: Path, fps: float) -> None:
    rows = [json.loads(l) for l in open(meta)]
    ts = [r["reel_t"] for r in rows]
    step = 1.0 / fps
    odd = [(rows[i]["i"], ts[i] - ts[i - 1]) for i in range(1, len(ts))
           if abs((ts[i] - ts[i - 1]) - step) > step * 0.25]
    wall = rows[-1]["t"] - rows[0]["t"] if len(rows) > 1 else 0
    print(f"{len(rows)} frames, reel {ts[0]:.2f}-{ts[-1]:.2f} s, captured in {wall:.1f} s wall; "
          f"{len(odd)} clock step(s) off 1/{fps:g} s")
    for i, d in odd[:10]:
        print(f"  frame {i}: step {d * 1000:.1f} ms")


def concat_list(shots: list[Path], fps: float, slow: float, path: Path) -> None:
    frames = [int(re.search(r"f(\d+)_", s.name).group(1)) for s in shots]
    with open(path, "w") as f:
        for i, s in enumerate(shots):
            gap = (frames[i + 1] - frames[i]) if i + 1 < len(shots) else 1
            f.write(f"file '{s}'\nduration {gap * slow / fps:.6f}\n")
        f.write(f"file '{shots[-1]}'\n")  # concat demuxer: last duration needs a repeat


def caption(label: str, note: str) -> str:
    text = f"{label}  ({note})" if label and note else label or note
    text = text.replace(":", r"\:").replace("'", "")
    if not text:
        return ""
    return (f",drawtext=fontfile={FONT}:text='{text}':x=24:y=24:fontsize=30:"
            f"fontcolor=white:box=1:boxcolor=black@0.6:boxborderw=10:expansion=none")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("run", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--label", default="")
    ap.add_argument("--slow", type=float, default=0.0, help="also append a 1/N speed pass")
    ap.add_argument("--max-mb", type=float, default=10.0)
    ap.add_argument("--tag", default="rf")
    ap.add_argument("--fps", type=float, default=30.0, help="screenshot runs only")
    a = ap.parse_args()
    done = reel_done(a.run, a.tag)
    a.out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=a.out.parent) as tmp:
        speeds = [(1.0, "real speed" if a.slow else "")] + (
            [(a.slow, f"1/{a.slow:g} speed")] if a.slow else [])
        parts = []
        if "video" in done:
            vdir = (a.run / done["video"]).resolve()
            fps = float(done.get("step_fps") or 30.0)
            check_steps(vdir / "frames.jsonl", fps)
            count = done["video_frames"]
            for speed, note in speeds:
                part = Path(tmp) / f"part_{len(parts)}.mp4"
                subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-framerate", f"{fps / speed:g}",
                                "-i", str(vdir / "f%06d.png"),
                                "-vf", f"scale=1280:-2,fps={fps:g}" + caption(a.label, note),
                                "-c:v", "libx264", "-crf", "12", "-preset", "slow",
                                "-pix_fmt", "yuv420p", str(part)], check=True)
                parts.append(part)
        else:
            shots = [(a.run / s).resolve() for s in done["shots"]]
            count = len(shots)
            for speed, note in speeds:
                lst = Path(tmp) / f"list_{len(parts)}.txt"
                concat_list(shots, a.fps, speed, lst)
                part = Path(tmp) / f"part_{len(parts)}.mp4"
                subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-f", "concat", "-safe", "0",
                                "-i", str(lst), "-vf", f"scale=1280:-2,fps={a.fps:g}" + caption(a.label, note),
                                "-c:v", "libx264", "-crf", "12", "-preset", "slow",
                                "-pix_fmt", "yuv420p", str(part)], check=True)
                parts.append(part)
        joined = Path(tmp) / "joined.txt"
        joined.write_text("".join(f"file '{p}'\n" for p in parts))
        for crf in (18, 20, 23, 26, 29, 32, 35):
            subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-f", "concat", "-safe", "0",
                            "-i", str(joined), "-c:v", "libx264", "-crf", str(crf),
                            "-preset", "slow", "-pix_fmt", "yuv420p", "-movflags", "+faststart",
                            str(a.out)], check=True)
            mb = a.out.stat().st_size / 1e6
            if mb < a.max_mb:
                print(f"{a.out}: {count} frames, crf {crf}, {mb:.1f} MB")
                return 0
        sys.exit(f"{a.out}: still {mb:.1f} MB at crf 35")


if __name__ == "__main__":
    raise SystemExit(main())
