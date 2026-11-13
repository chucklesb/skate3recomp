#!/usr/bin/env python3
"""Side-by-side comparisons of screenshots or video frames.

    shots.py strip OUT.jpg IMG[=LABEL] ... [--box X0,Y0,X1,Y1] [--width W] [--pixel]

Each image is cropped to --box (optional, the same box for all), scaled to
--width pixels wide (default 960; --pixel scales with nearest-neighbour for
zoomed-in pixel inspection instead of smoothing), labelled top-left with
LABEL if given, and placed left to right with a white gap. JPG or PNG by the
extension.

Examples:
  # zoom on the skater's head in three runs' shots, 3x, unlabelled
  shots.py strip /tmp/head.png a.png b.png c.png --box 520,120,680,240 --width 480 --pixel
  # a labelled before/after for docs
  shots.py strip out/illustrations/ki126.jpg off.png="Before" on.png="After (Odin 2)"

Needs Pillow: from WSL,
  nix shell nixpkgs#python3Packages.pillow -c python3 tools/harness/shots.py ...
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    sys.exit("shots.py needs Pillow: nix shell nixpkgs#python3Packages.pillow -c python3 "
             "tools/harness/shots.py ...")


def load_font(size: int):
    try:
        return ImageFont.load_default(size=size)
    except TypeError:  # Pillow < 10.1
        return ImageFont.load_default()


def tile(path: Path, box, width: int, pixel: bool) -> Image.Image:
    im = Image.open(path).convert("RGB")
    if box:
        im = im.crop(box)
    h = max(1, round(im.height * width / im.width))
    return im.resize((width, h), Image.NEAREST if pixel else Image.LANCZOS)


def label(im: Image.Image, text: str, font) -> Image.Image:
    d = ImageDraw.Draw(im)
    pad = 10
    l, t, r, b = d.textbbox((0, 0), text, font=font)
    d.rectangle((0, 0, r - l + 2 * pad, b - t + 2 * pad), fill=(0, 0, 0))
    d.text((pad - l, pad - t), text, fill=(255, 255, 255), font=font)
    return im


def strip(out: Path, items: list[str], box, width: int, pixel: bool, gap: int) -> None:
    font = load_font(max(14, width // 36))
    tiles = []
    for item in items:
        path, _, text = item.partition("=")
        t = tile(Path(path), box, width, pixel)
        tiles.append(label(t, text, font) if text else t)
    h = max(t.height for t in tiles)
    canvas = Image.new("RGB", (sum(t.width for t in tiles) + gap * (len(tiles) - 1), h),
                       (255, 255, 255))
    x = 0
    for t in tiles:
        canvas.paste(t, (x, 0))
        x += t.width + gap
    out.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(out, quality=88) if out.suffix.lower() in (".jpg", ".jpeg") else canvas.save(out)
    print(f"{out} {canvas.size[0]}x{canvas.size[1]}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("strip", help="crop, scale, label and place images side by side")
    p.add_argument("out", type=Path)
    p.add_argument("items", nargs="+", metavar="IMG[=LABEL]")
    p.add_argument("--box", help="crop X0,Y0,X1,Y1 (source pixels)")
    p.add_argument("--width", type=int, default=960, help="width of each tile (default 960)")
    p.add_argument("--pixel", action="store_true", help="nearest-neighbour scaling")
    p.add_argument("--gap", type=int, default=8)
    a = ap.parse_args()
    box = tuple(int(v) for v in a.box.split(",")) if a.box else None
    if box is not None and len(box) != 4:
        ap.error("--box needs X0,Y0,X1,Y1")
    strip(a.out, a.items, box, a.width, a.pixel, a.gap)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
