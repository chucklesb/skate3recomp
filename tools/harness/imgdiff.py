#!/usr/bin/env python3
"""Compare screenshots: mean absolute difference, % of changed pixels, block SSIM.

Both images are scaled to a common width first (targets render at different
resolutions), so the numbers compare content, not pixel grids.

  imgdiff.py A.png B.png [--heat out.png] [--width 640]   # one pair, JSON on stdout
  imgdiff.py --dir shots/<tag>/ [--heat-dir out/]          # each file vs the first

Metrics (all on the scaled images):
  mae       mean absolute difference per channel, 0..255
  changed   % of pixels whose largest channel difference exceeds 32
  ssim      mean SSIM of 8x8 grayscale blocks, 1.0 = identical

`verdict` is "same" (ssim >= 0.98 and changed <= 0.5 %), "close" (ssim >= 0.90
and changed <= 5 %) or "different". A run of one target against itself (the
same reel still twice) should be "same"; across renderers or backends "close"
is the usual good result.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image

CHANGED_THRESHOLD = 32


def load(path: Path, width: int, height: int | None = None) -> np.ndarray:
    im = Image.open(path).convert("RGB")
    if height is None:
        height = max(1, round(im.height * width / im.width))
    return np.asarray(im.resize((width, height), Image.BILINEAR), dtype=np.float32)


def block_ssim(a: np.ndarray, b: np.ndarray, block: int = 8) -> float:
    ga = a @ np.array([0.299, 0.587, 0.114], dtype=np.float32)
    gb = b @ np.array([0.299, 0.587, 0.114], dtype=np.float32)
    h = ga.shape[0] // block * block
    w = ga.shape[1] // block * block
    ga = ga[:h, :w].reshape(h // block, block, w // block, block)
    gb = gb[:h, :w].reshape(h // block, block, w // block, block)
    ma, mb = ga.mean(axis=(1, 3)), gb.mean(axis=(1, 3))
    va = ga.var(axis=(1, 3))
    vb = gb.var(axis=(1, 3))
    cov = ((ga - ma[:, None, :, None]) * (gb - mb[:, None, :, None])).mean(axis=(1, 3))
    c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    s = ((2 * ma * mb + c1) * (2 * cov + c2)) / ((ma**2 + mb**2 + c1) * (va + vb + c2))
    return float(s.mean())


def verdict(m: dict) -> str:
    if m["ssim"] >= 0.98 and m["changed"] <= 0.5:
        return "same"
    if m["ssim"] >= 0.90 and m["changed"] <= 5.0:
        return "close"
    return "different"


def diff(a_path: Path, b_path: Path, heat: Path | None = None, width: int = 640) -> dict:
    a = load(a_path, width)
    b = load(b_path, width, a.shape[0])
    d = np.abs(a - b)
    m = {
        "a": str(a_path),
        "b": str(b_path),
        "mae": round(float(d.mean()), 3),
        "changed": round(float((d.max(axis=2) > CHANGED_THRESHOLD).mean() * 100), 3),
        "ssim": round(block_ssim(a, b), 4),
    }
    m["verdict"] = verdict(m)
    if heat is not None:
        heat.parent.mkdir(parents=True, exist_ok=True)
        hm = np.clip(d.max(axis=2) * 4, 0, 255).astype(np.uint8)
        Image.fromarray(hm, "L").save(heat)
        m["heat"] = str(heat)
    return m


def diff_dir(folder: Path, heat_dir: Path | None = None, width: int = 640) -> list[dict]:
    files = sorted(folder.glob("*.png"))
    out = []
    for f in files[1:]:
        heat = heat_dir / f"{folder.name}__{files[0].stem}__vs__{f.stem}.png" if heat_dir else None
        out.append(diff(files[0], f, heat, width))
    return out


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("a", nargs="?", type=Path)
    p.add_argument("b", nargs="?", type=Path)
    p.add_argument("--dir", type=Path)
    p.add_argument("--heat", type=Path)
    p.add_argument("--heat-dir", type=Path)
    p.add_argument("--width", type=int, default=640)
    a = p.parse_args()
    if a.dir:
        res = diff_dir(a.dir, a.heat_dir, a.width)
    elif a.a and a.b:
        res = diff(a.a, a.b, a.heat, a.width)
    else:
        p.error("give two images or --dir")
    json.dump(res, sys.stdout, indent=2)
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
