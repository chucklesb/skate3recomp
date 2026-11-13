#!/usr/bin/env python3
"""Extract the thumbnails from a REPLY_SKATER folder's reel headers (N.H).

Each N.H is an MC02 container (see src/harness/harness_reels.h) whose payload
holds the reel's My Replays thumbnail as a JPEG. This writes <out>/<slot>.jpg
per header and prints slot, size and the JPEG's dimensions, plus a hash so
identical (stale, copied) thumbnails stand out.

  reel_thumbs.py <REPLY_SKATER dir> [--out DIR]   (default: <dir>/thumbs)
"""
from __future__ import annotations

import argparse
import hashlib
import struct
import sys
from pathlib import Path


def jpeg_size(data: bytes) -> tuple[int, int] | None:
    i = 2
    while i + 9 < len(data):
        if data[i] != 0xFF:
            return None
        marker = data[i + 1]
        seglen = struct.unpack(">H", data[i + 2:i + 4])[0]
        if marker in (0xC0, 0xC1, 0xC2):
            h, w = struct.unpack(">HH", data[i + 5:i + 9])
            return w, h
        i += 2 + seglen
    return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("dir", type=Path)
    ap.add_argument("--out", type=Path)
    a = ap.parse_args()
    out = a.out or a.dir / "thumbs"
    out.mkdir(parents=True, exist_ok=True)
    seen: dict[str, int] = {}
    for h in sorted(a.dir.glob("*.H"), key=lambda p: int(p.stem, 16)):
        slot = int(h.stem, 16)
        raw = h.read_bytes()
        start = raw.find(b"\xff\xd8\xff")
        if start < 0:
            print(f"slot {slot:3d}: no JPEG in {h.name} ({len(raw)} bytes)")
            continue
        end = raw.find(b"\xff\xd9", start)
        jpg = raw[start:end + 2] if end > 0 else raw[start:]
        digest = hashlib.sha1(jpg).hexdigest()[:10]
        dup = f"  SAME AS slot {seen[digest]}" if digest in seen else ""
        seen.setdefault(digest, slot)
        (out / f"{slot:02d}.jpg").write_bytes(jpg)
        size = jpeg_size(jpg)
        dims = f"{size[0]}x{size[1]}" if size else "?"
        print(f"slot {slot:3d}: jpeg at +{start} {len(jpg)} bytes {dims} sha1 {digest}{dup}")
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
