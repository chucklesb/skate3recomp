#!/usr/bin/env python3
"""Dump the game's object registry from a .gsnap (reverse-engineering aid).

The registry is an EASTL map {u32 name hash -> entry} reached through
*(0x83083BCC) + 168 (TU3). Nodes are {right +0, left +4, parent +8, color +12,
key +16, entry type byte +20 (the teleport lookup wants 1), object +24}; the
tree root is at registry +16. Keys are
sub_8296EA10(name), an ELF-style hash. The teleport-to-location event looks
names up here (sub_826DFBF0: registry.find(hash(name))->+4).

  registry.py SNAP [--grep REGEX] [--objdump N]

For each entry it prints the key, the value object, and the first string
reachable from the object (directly or one pointer deep) whose hash equals the
key, which recovers the object's name.
"""
from __future__ import annotations

import argparse
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gsnap  # noqa: E402

REGISTRY_PTR = 0x83083BCC
REGISTRY_OFF = 168


def name_hash(s: bytes) -> int:
    """sub_8296EA10: h = (h << 4) + c; if (g = h & 0xF0000000): h ^= g >> 23 (9-bit rot) ^ g."""
    h = 0
    for c in s:
        if c >= 0x80:
            c -= 0x100  # extsb
        h = ((h << 4) + c) & 0xFFFFFFFF
        g = h & 0xF0000000
        if g:
            h ^= ((g >> 23) & 0x1FF) ^ g
    return h


def cstr(s: gsnap.Snapshot, addr: int, n: int = 96) -> bytes | None:
    b = s.read(addr, n)
    if not b:
        return None
    b = b.split(b"\0", 1)[0]
    return b if 3 <= len(b) < n and all(32 <= c < 127 for c in b) else None


def u32(s, a):
    return gsnap.u32_at(s, a)


def walk(s: gsnap.Snapshot, root: int, limit: int = 100000):
    stack, seen = [root], set()
    while stack and len(seen) < limit:
        n = stack.pop()
        if not n or n in seen:
            continue
        seen.add(n)
        right, left = u32(s, n), u32(s, n + 4)
        key, val = u32(s, n + 16), u32(s, n + 24)
        flag = (s.read(n + 20, 1) or b"\xff")[0]
        if key is None:
            continue
        yield n, key, val, flag
        for c in (left, right):
            if c and 0x40000000 <= c < 0xA0000000:
                stack.append(c)


def find_name(s: gsnap.Snapshot, obj: int, key: int, words: int = 48) -> str | None:
    if not obj:
        return None
    for i in range(words):
        p = u32(s, obj + 4 * i)
        if p is None:
            break
        for cand in (p, u32(s, p) if p and 0x40000000 <= p < 0xA0000000 else None):
            if cand and (0x40000000 <= cand < 0xA0000000 or 0x82000000 <= cand < 0x84A10000):
                t = cstr(s, cand)
                if t and name_hash(t) == key:
                    return t.decode()
    return None


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("snap", type=Path)
    p.add_argument("--grep", help="only names matching this regex")
    p.add_argument("--objdump", type=int, default=0, help="words of each value object to print")
    a = p.parse_args()
    s = gsnap.Snapshot(a.snap)
    reg = u32(s, u32(s, REGISTRY_PTR) + REGISTRY_OFF)
    root = u32(s, reg + 16)
    count = u32(s, reg + 24)
    print(f"registry 0x{reg:08X} root 0x{root:08X} count {count}")
    rows = []
    for node, key, val, flag in walk(s, root):
        name = find_name(s, val, key)
        vt = u32(s, val) if val else None
        rows.append((name or "", key, val, vt, flag))
    named = sum(1 for r in rows if r[0])
    for name, key, val, vt, flag in sorted(rows, key=lambda r: (r[0] == "", r[0], r[1])):
        if a.grep and not re.search(a.grep, name, re.I):
            continue
        line = f"{key:08X}  t{flag} obj 0x{(val or 0):08X} vt 0x{(vt or 0):08X}  {name}"
        if a.objdump and val:
            raw = s.read(val, 4 * a.objdump) or b""
            line += "  | " + " ".join(f"{w:08X}" for w in struct.unpack(f">{len(raw) // 4}I", raw))
        print(line)
    print(f"{len(rows)} entries walked, {named} named", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
