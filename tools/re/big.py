#!/usr/bin/env python3
"""Read Skate 3's .big archives (EA "EB" version 3).

    big.py list BIG [--grep REGEX]
    big.py extract BIG [--out DIR] [--grep REGEX]   # default out/re/big/<name>/
    big.py grep BIG|DIR PATTERN [--ignore-case]     # search the decompressed files

Format (big-endian), worked out on data/big/db.big (2026-11-15):
    0x00  "EB", u16 version (3)
    0x04  u32 file count
    0x08  u16 flags, u8 offset shift (6: offsets in 64-byte units), u8 0
    0x0C  u32 names offset, u32 names size
    0x14  u8 name record size (54), u8 folder count, u16 ?
    0x18  u32 0, u32 archive size
    0x30  file entries, 16 bytes: u32 offset >> shift, u32 compressed size
          (0 = stored), u32 size, u32 name hash
    names: one record per entry, in entry order: u16 folder index, then the
          NUL-padded name
Compressed files are RefPack (EA's QFS: 0x10FB, then a 24-bit size).

Game data: extracted files stay under out/ and are never committed.
"""
from __future__ import annotations

import argparse
import re
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


def refpack(src: bytes) -> bytes:
    """Decompress EA RefPack (QFS)."""
    i = 0
    flags = src[0]
    if src[1] != 0xFB:
        raise ValueError("not RefPack")
    i = 2
    big = flags & 0x80
    if flags & 0x01:  # a compressed-size field precedes the size
        i += 4 if big else 3
    size = int.from_bytes(src[i:i + (4 if big else 3)], "big")
    i += 4 if big else 3
    out = bytearray()
    n = len(src)
    while i < n:
        b0 = src[i]
        if b0 < 0x80:
            b1 = src[i + 1]
            lit = b0 & 3
            cnt = ((b0 >> 2) & 7) + 3
            off = ((b0 & 0x60) << 3) + b1 + 1
            i += 2
        elif b0 < 0xC0:
            b1, b2 = src[i + 1], src[i + 2]
            lit = (b1 >> 6) & 3
            cnt = (b0 & 0x3F) + 4
            off = ((b1 & 0x3F) << 8) + b2 + 1
            i += 3
        elif b0 < 0xE0:
            b1, b2, b3 = src[i + 1], src[i + 2], src[i + 3]
            lit = b0 & 3
            cnt = ((b0 & 0x0C) << 6) + b3 + 5
            off = ((b0 & 0x10) << 12) + (b1 << 8) + b2 + 1
            i += 4
        elif b0 < 0xFC:
            lit = ((b0 & 0x1F) << 2) + 4
            out += src[i + 1:i + 1 + lit]
            i += 1 + lit
            continue
        else:
            lit = b0 & 3
            out += src[i + 1:i + 1 + lit]
            break
        out += src[i:i + lit]
        i += lit
        start = len(out) - off
        for k in range(cnt):  # may overlap: byte by byte
            out.append(out[start + k])
    if len(out) != size:
        raise ValueError(f"RefPack: got {len(out)} bytes, header says {size}")
    return bytes(out)


class Big:
    def __init__(self, path: Path):
        self.path = path
        self.d = path.read_bytes()
        d = self.d
        if d[:2] != b"EB":
            raise SystemExit(f"{path}: not an EB archive")
        self.version, self.count = struct.unpack_from(">HI", d, 2)
        self.shift = d[10]
        self.names_off, self.names_size = struct.unpack_from(">II", d, 12)
        self.rec = d[0x14]
        self.entries = []
        for k in range(self.count):
            off, csize, size, h = struct.unpack_from(">4I", d, 0x30 + 16 * k)
            r = d[self.names_off + self.rec * k:self.names_off + self.rec * (k + 1)]
            folder = struct.unpack_from(">H", r, 0)[0]
            name = r[2:].split(b"\0")[0].decode("latin1")
            self.entries.append((name, off << self.shift, csize, size, h, folder))

    def data(self, entry) -> bytes:
        name, pos, csize, size, h, folder = entry
        if csize:
            raw = self.d[pos:pos + csize]
            if len(raw) > 2 and raw[1] == 0xFB:
                return refpack(raw)
            return raw  # unknown compression: raw bytes
        return self.d[pos:pos + size]


def main() -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("list")
    p.add_argument("big")
    p.add_argument("--grep")
    p = sub.add_parser("extract")
    p.add_argument("big")
    p.add_argument("--out")
    p.add_argument("--grep")
    p = sub.add_parser("grep")
    p.add_argument("src")
    p.add_argument("pattern")
    p.add_argument("--ignore-case", "-i", action="store_true")
    a = ap.parse_args()

    if a.cmd == "list":
        b = Big(Path(a.big))
        for e in b.entries:
            if not a.grep or re.search(a.grep, e[0]):
                print(f"{e[0]:56s} {e[3]:9d} {'refpack' if e[2] else 'stored':8s} {e[4]:08X}")
        print(f"{b.count} files", file=sys.stderr)
        return 0
    if a.cmd == "extract":
        b = Big(Path(a.big))
        out = Path(a.out) if a.out else REPO / "out/re/big" / Path(a.big).stem
        out.mkdir(parents=True, exist_ok=True)
        n = bad = 0
        sel = [e for e in b.entries if not a.grep or re.search(a.grep, e[0])]
        for k, e in enumerate(sel):
            if k and k % 200 == 0:
                print(f"  {k}/{len(sel)}", file=sys.stderr, flush=True)
            try:
                data = b.data(e)
            except Exception as ex:  # report, keep going
                print(f"{e[0]}: {ex}", file=sys.stderr)
                bad += 1
                continue
            (out / e[0]).write_bytes(data)
            n += 1
        print(f"{n} files -> {out}" + (f", {bad} failed" if bad else ""))
        return 1 if bad else 0
    # grep
    flags = re.I if a.ignore_case else 0
    pat = re.compile(a.pattern.encode(), flags)
    src = Path(a.src)
    files = []
    if src.is_dir():
        files = [(f.name, f.read_bytes()) for f in sorted(src.iterdir()) if f.is_file()]
    else:
        b = Big(src)
        files = [(e[0], b.data(e)) for e in b.entries]
    for name, data in files:
        for m in pat.finditer(data):
            s = max(0, m.start() - 40)
            ctx = data[s:m.end() + 40]
            print(f"{name}: {ctx!r}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
