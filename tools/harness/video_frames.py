#!/usr/bin/env python3
"""Screen recordings (scrcpy) as evidence: make them seekable, pull frames,
and find the same moment in another recording.

    video_frames.py remux  VIDEO.mkv                  # -> VIDEO_fixed.mkv (seekable)
    video_frames.py frames VIDEO --at T [--dur S] --out DIR
    video_frames.py match  REF.png DIR [--top 3]       # closest frames in DIR to REF

`remux`: a recording stopped by killing scrcpy has no seek index; this
copies the streams into a fresh container (no re-encode) so players and
`frames` can seek. `frames`: every frame from T for S seconds (default 0.2)
as DIR/f_NNN.png. `match`: mean absolute difference on 160x90 greyscale
thumbnails; use it to find the same reel moment in a second pass or a second
recording (the game runs 30 fps inside a 60 fps recording, so frames come in
identical pairs).

Recording the Odin 2 (from PowerShell on this PC; Windows scrcpy, with ADB
set to the SDK adb.exe):
  scrcpy.exe --no-window --no-audio --record=C:\\Users\\user\\Videos\\x.mkv
             --video-bit-rate=40M --max-fps=60
Stop it with Stop-Process (exit 255 is expected), then remux.

Needs ffmpeg (remux, frames) and Pillow (match): from WSL,
  nix shell nixpkgs#ffmpeg nixpkgs#python3Packages.pillow -c python3 tools/harness/video_frames.py ...
"""
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path


def need(tool: str) -> None:
    if shutil.which(tool) is None:
        sys.exit(f"{tool} not found: nix shell nixpkgs#ffmpeg nixpkgs#python3Packages.pillow "
                 "-c python3 tools/harness/video_frames.py ...")


def remux(video: Path) -> int:
    need("ffmpeg")
    out = video.with_name(video.stem + "_fixed" + video.suffix)
    subprocess.run(["ffmpeg", "-y", "-v", "error", "-i", str(video), "-c", "copy", str(out)],
                   check=True)
    dur = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration",
                          "-of", "csv=p=0", str(out)], capture_output=True, text=True).stdout
    print(f"{out} ({float(dur or 0):.1f} s)")
    return 0


def frames(video: Path, at: float, dur: float, out: Path) -> int:
    need("ffmpeg")
    out.mkdir(parents=True, exist_ok=True)
    for old in out.glob("f_*.png"):
        old.unlink()
    subprocess.run(["ffmpeg", "-v", "error", "-ss", f"{at:.3f}", "-i", str(video),
                    "-t", f"{dur:.3f}", str(out / "f_%03d.png")], check=True)
    n = len(list(out.glob("f_*.png")))
    print(f"{n} frames from {at:.3f} s -> {out}")
    return 0


def match(ref: Path, folder: Path, top: int) -> int:
    try:
        from PIL import Image, ImageChops, ImageStat
    except ImportError:
        sys.exit("match needs Pillow: nix shell nixpkgs#python3Packages.pillow -c python3 ...")

    def thumb(p: Path):
        return Image.open(p).convert("L").resize((160, 90))

    r = thumb(ref)
    scores = []
    for p in sorted(folder.glob("*.png")):
        scores.append((ImageStat.Stat(ImageChops.difference(r, thumb(p))).mean[0], p.name))
    scores.sort()
    for s, name in scores[:top]:
        print(f"{name}  {s:.2f}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("remux")
    p.add_argument("video", type=Path)
    p = sub.add_parser("frames")
    p.add_argument("video", type=Path)
    p.add_argument("--at", type=float, required=True, help="start time (s)")
    p.add_argument("--dur", type=float, default=0.2, help="length (s), default 0.2")
    p.add_argument("--out", type=Path, required=True)
    p = sub.add_parser("match")
    p.add_argument("ref", type=Path)
    p.add_argument("folder", type=Path)
    p.add_argument("--top", type=int, default=3)
    a = ap.parse_args()
    if a.cmd == "remux":
        return remux(a.video)
    if a.cmd == "frames":
        return frames(a.video, a.at, a.dur, a.out)
    return match(a.ref, a.folder, a.top)


if __name__ == "__main__":
    raise SystemExit(main())
