#!/usr/bin/env python3
"""Side-by-side comparisons of screenshots or video frames.

    shots.py strip OUT.jpg IMG[=LABEL] ... [--box X0,Y0,X1,Y1] [--width W] [--pixel]
    shots.py stats IMG[=LABEL] ... [--box X0,Y0,X1,Y1]   # region colour + change vs first
    shots.py classify IMG ... --ref LABEL=IMG --ref LABEL=IMG [--box X0,Y0,X1,Y1]

classify answers "which known state does each shot show?" (e.g. the old or
the new photo in a menu box): per shot, the % of box pixels differing by more
than 24 from each reference, and the nearest reference. Use it instead of
judging by eye or by brightness; a darker render is not a different state.

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


def stats(items: list[str], box) -> None:
    """Mean RGB of a region per image, and the mean absolute difference
    against the first image (0..255): how much a toggle changes one surface,
    without the rest of the frame diluting it."""
    first = None
    for item in items:
        path, _, text = item.partition("=")
        im = Image.open(path).convert("RGB")
        if box:
            im = im.crop(box)
        px = list(im.getdata())
        n = len(px)
        mean = [sum(p[c] for p in px) / n for c in range(3)]
        if first is None:
            first, diff = px, 0.0
        else:
            m = min(len(first), n)
            diff = sum(abs(a[c] - b[c]) for a, b in zip(first[:m], px[:m]) for c in range(3)) / (3 * m)
        print(f"{text or Path(path).name:<24} rgb=({mean[0]:6.1f},{mean[1]:6.1f},{mean[2]:6.1f})"
              f"  |diff| vs first={diff:6.2f}")


def changed_pct(a: Image.Image, b: Image.Image, threshold: int = 24) -> float:
    if a.size != b.size:
        b = b.resize(a.size, Image.BILINEAR)
    pa, pb = a.getdata(), b.getdata()
    n = sum(1 for x, y in zip(pa, pb) if max(abs(x[0] - y[0]), abs(x[1] - y[1]),
                                              abs(x[2] - y[2])) > threshold)
    return 100.0 * n / len(pa)


def classify(items: list[str], refs: list[str], box) -> None:
    def load(path: str) -> Image.Image:
        im = Image.open(path).convert("RGB")
        return im.crop(box) if box else im

    ref_ims = []
    for r in refs:
        name, sep, path = r.partition("=")
        if not sep:
            sys.exit(f"--ref needs LABEL=IMG, got {r}")
        ref_ims.append((name, load(path)))
    for item in items:
        im = load(item)
        scores = [(changed_pct(im, rim), name) for name, rim in ref_ims]
        best = min(scores)
        cols = "  ".join(f"{name}={pct:5.1f}%" for pct, name in scores)
        print(f"{Path(item).name:<40} {cols}  -> {best[1]}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("stats", help="mean colour of a region per image, and its mean "
                                     "absolute difference from the first image")
    s.add_argument("items", nargs="+", metavar="IMG[=LABEL]")
    s.add_argument("--box", help="region X0,Y0,X1,Y1 (default: whole image)")
    c = sub.add_parser("classify", help="nearest reference image per shot, by % of "
                                        "changed pixels in the box")
    c.add_argument("items", nargs="+", metavar="IMG")
    c.add_argument("--ref", action="append", required=True, metavar="LABEL=IMG")
    c.add_argument("--box", help="region X0,Y0,X1,Y1 (default: whole image)")
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
    if a.cmd == "stats":
        stats(a.items, box)
        return 0
    if a.cmd == "classify":
        classify(a.items, a.ref, box)
        return 0
    strip(a.out, a.items, box, a.width, a.pixel, a.gap)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
