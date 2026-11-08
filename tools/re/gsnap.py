#!/usr/bin/env python3
"""Read and search .gsnap guest-memory snapshots (reverse-engineering aid).

A .gsnap is written by the native renderer's diagnostics (the harness
`snapshot` verb, F9/F10, or the snapshot-dir trigger file; Windows only):

    "SK3GSNP1"
    repeated: u64 LE guest offset, u64 LE size, <size> bytes
    terminator: offset = 0xFFFFFFFFFFFFFFFF, size = 0

Guest memory is big-endian (Xbox 360 PowerPC), so values are searched and
printed big-endian.

  gsnap.py regions SNAP                      # list the regions
  gsnap.py extract SNAP ADDR SIZE OUT        # copy a guest range to a file
  gsnap.py read SNAP ADDR [--count N] [--as u32|f32|bytes]
  gsnap.py find-u32 SNAP VALUE [--range LO HI] [--align 4]
  gsnap.py find-f32 SNAP X [Y Z] [--tol 0.01] [--range LO HI]
  gsnap.py diff SNAP_A SNAP_B --range LO HI [--as f32|u32] [--min-delta D]
  gsnap.py classof SNAP ADDR...              # RTTI class name of objects
  gsnap.py owner SNAP ADDR... [--back N]     # nearest RTTI objects before ADDRs
  gsnap.py refs SNAP TARGET [--span N] [--name]   # who points into [TARGET, +span)

Addresses take 0x-prefixed hex or decimal.
"""
from __future__ import annotations

import argparse
import mmap
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

import numpy as np

MAGIC = b"SK3GSNP1"
END = 0xFFFFFFFFFFFFFFFF


@dataclass
class Region:
    addr: int   # guest address
    size: int
    pos: int    # file offset of the data


class Snapshot:
    def __init__(self, path: Path):
        self.path = path
        self._f = path.open("rb")
        self._m = mmap.mmap(self._f.fileno(), 0, access=mmap.ACCESS_READ)
        if self._m[:8] != MAGIC:
            raise SystemExit(f"{path}: not a .gsnap (magic {self._m[:8]!r})")
        self.regions: list[Region] = []
        pos = 8
        while pos + 16 <= len(self._m):
            addr, size = struct.unpack_from("<QQ", self._m, pos)
            pos += 16
            if addr == END:
                break
            self.regions.append(Region(addr, size, pos))
            pos += size

    def region_at(self, addr: int) -> Region | None:
        for r in self.regions:
            if r.addr <= addr < r.addr + r.size:
                return r
        return None

    def read(self, addr: int, size: int) -> bytes | None:
        r = self.region_at(addr)
        if r is None or addr + size > r.addr + r.size:
            return None
        start = r.pos + (addr - r.addr)
        return self._m[start:start + size]

    def view(self, r: Region, lo: int | None = None, hi: int | None = None) -> tuple[int, np.ndarray]:
        """The region's bytes clipped to [lo, hi) as a uint8 array, with its guest address."""
        a = r.addr if lo is None else max(r.addr, lo)
        b = r.addr + r.size if hi is None else min(r.addr + r.size, hi)
        if a >= b:
            return a, np.empty(0, np.uint8)
        arr = np.frombuffer(self._m, np.uint8, count=b - a, offset=r.pos + (a - r.addr))
        return a, arr


def num(s: str) -> int:
    return int(s, 0)


# ------------------------------------------------------------------ RTTI
# MSVC RTTI, 32-bit big-endian pointers (Xbox 360):
#   vtable[-1] -> CompleteObjectLocator {u32 signature, u32 offset, u32 cdOffset,
#                                        TypeDescriptor*, ClassHierarchyDescriptor*}
#   TypeDescriptor {void* pVFTable, void* spare, char name[] ".?AVName@Ns@@"}
IMAGE_LO, IMAGE_HI = 0x82000000, 0x84A10000


def u32_at(s: Snapshot, addr: int) -> int | None:
    b = s.read(addr, 4)
    return struct.unpack(">I", b)[0] if b else None


def demangle(name: str) -> str:
    """'.?AVSkaterPresEntity@Sk8@@' -> 'Sk8::SkaterPresEntity' (simple names only)."""
    if not name.startswith(".?A") or len(name) < 5:
        return name
    parts = name[4:].rstrip("@").split("@")
    return "::".join(reversed([p for p in parts if p]))


def class_of_vtable(s: Snapshot, vt: int) -> tuple[str, int] | None:
    """(class name, this-offset in the complete object) for a vtable address."""
    if not (IMAGE_LO <= vt < IMAGE_HI):
        return None
    col = u32_at(s, vt - 4)
    if col is None or not (IMAGE_LO <= col < IMAGE_HI):
        return None
    hdr = s.read(col, 20)
    if not hdr:
        return None
    sig, off, _cd, td, _chd = struct.unpack(">IIIII", hdr)
    if sig != 0 or not (IMAGE_LO <= td < IMAGE_HI):
        return None
    raw = s.read(td + 8, 128)
    if not raw or not raw.startswith(b".?A"):
        return None
    return demangle(raw.split(b"\0", 1)[0].decode("latin-1")), off


def class_of(s: Snapshot, obj: int) -> tuple[str, int] | None:
    vt = u32_at(s, obj)
    return class_of_vtable(s, vt) if vt is not None else None


def cmd_classof(a) -> int:
    s = Snapshot(a.snap)
    for addr in a.addrs:
        c = class_of(s, addr)
        print(f"0x{addr:08X}  {c[0] + (f' (subobject at +{c[1]:#x})' if c[1] else '') if c else '-'}")
    return 0


def cmd_owner(a) -> int:
    """For each address, the nearest preceding objects that have an RTTI vtable."""
    s = Snapshot(a.snap)
    for addr in a.addrs:
        found = []
        p = addr & ~3
        while p >= addr - a.back and len(found) < a.depth:
            c = class_of(s, p)
            if c:
                found.append((p, c))
            p -= 4
        desc = "; ".join(f"{n}{f'[sub+{o:#x}]' if o else ''} @0x{o_:08X} (+0x{addr - o_:X})"
                         for o_, (n, o) in found) or "-"
        print(f"0x{addr:08X}  {desc}")
    return 0


def cmd_refs(a) -> int:
    """Words anywhere outside the image that equal a pointer (who points at this object)."""
    s = Snapshot(a.snap)
    hits = 0
    for r in s.regions:
        if IMAGE_LO <= r.addr < IMAGE_HI:
            continue
        base, arr = s.view(r)
        vals, addrs = words(arr, base, 4, ">u4")
        lo, hi = a.target, a.target + a.span
        m = (vals >= np.uint32(lo)) & (vals < np.uint32(hi))
        for i in np.nonzero(m)[0]:
            ref = int(addrs[i])
            own = ""
            if a.name:
                p = ref & ~3
                while p >= ref - 0x400:
                    c = class_of(s, p)
                    if c:
                        own = f"  in {c[0]} @0x{p:08X} +0x{ref - p:X}"
                        break
                    p -= 4
            print(f"0x{ref:08X} -> 0x{int(vals[i]):08X}{own}")
            hits += 1
            if hits >= a.limit:
                print(f"(stopped at {a.limit})")
                return 0
    print(f"{hits} refs")
    return 0


def words(arr: np.ndarray, base: int, align: int, dtype: str) -> tuple[np.ndarray, np.ndarray]:
    """Big-endian 4-byte values at every `align` step, and their guest addresses."""
    skip = (-base) % 4
    usable = (len(arr) - skip) // 4 * 4
    vals = arr[skip:skip + usable].view(dtype)
    addrs = base + skip + np.arange(len(vals), dtype=np.int64) * 4
    if align % 4 == 0 and align > 4:
        keep = (addrs % align) == 0
        vals, addrs = vals[keep], addrs[keep]
    return vals, addrs


def cmd_regions(a) -> int:
    s = Snapshot(a.snap)
    total = 0
    for r in s.regions:
        print(f"0x{r.addr:08X}-0x{r.addr + r.size:08X}  {r.size / 2**20:9.2f} MiB")
        total += r.size
    print(f"{len(s.regions)} regions, {total / 2**20:.1f} MiB")
    return 0


def cmd_extract(a) -> int:
    s = Snapshot(a.snap)
    data = s.read(a.addr, a.size)
    if data is None:
        print(f"0x{a.addr:08X}+0x{a.size:X} is not inside one region", file=sys.stderr)
        return 1
    a.out.write_bytes(data)
    print(f"wrote {a.size} bytes from 0x{a.addr:08X} to {a.out}")
    return 0


def cmd_read(a) -> int:
    s = Snapshot(a.snap)
    width = 1 if a.as_ == "bytes" else 4
    data = s.read(a.addr, a.count * width)
    if data is None:
        print("unmapped", file=sys.stderr)
        return 1
    if a.as_ == "bytes":
        for i in range(0, len(data), 16):
            print(f"0x{a.addr + i:08X}  {data[i:i + 16].hex(' ')}")
        return 0
    fmt = ">I" if a.as_ == "u32" else ">f"
    for i in range(a.count):
        (v,) = struct.unpack_from(fmt, data, i * 4)
        print(f"0x{a.addr + i * 4:08X}  {v:#010x}" if a.as_ == "u32" else
              f"0x{a.addr + i * 4:08X}  {v:.6g}")
    return 0


def search_ranges(s: Snapshot, rng):
    lo, hi = (rng if rng else (None, None))
    for r in s.regions:
        if hi is not None and r.addr >= hi:
            continue
        if lo is not None and r.addr + r.size <= lo:
            continue
        yield s.view(r, lo, hi)


def cmd_find_u32(a) -> int:
    s = Snapshot(a.snap)
    hits = 0
    for base, arr in search_ranges(s, a.range):
        vals, addrs = words(arr, base, a.align, ">u4")
        for addr in addrs[vals == np.uint32(a.value)]:
            print(f"0x{int(addr):08X}")
            hits += 1
            if hits >= a.limit:
                print(f"(stopped at {a.limit})")
                return 0
    print(f"{hits} hits")
    return 0


def cmd_find_f32(a) -> int:
    s = Snapshot(a.snap)
    target = [float(x) for x in a.values]
    hits = 0
    for base, arr in search_ranges(s, a.range):
        vals, addrs = words(arr, base, 4, ">f4")
        with np.errstate(invalid="ignore"):
            m = np.abs(vals - target[0]) <= a.tol
            for k, t in enumerate(target[1:], start=1):
                if len(vals) <= k:
                    m[:] = False
                    break
                nxt = np.zeros_like(m)
                nxt[:-k] = np.abs(vals[k:] - t) <= a.tol
                m &= nxt
        for i in np.nonzero(m)[0]:
            got = ", ".join(f"{vals[i + k]:.4f}" for k in range(len(target)))
            print(f"0x{int(addrs[i]):08X}  [{got}]")
            hits += 1
            if hits >= a.limit:
                print(f"(stopped at {a.limit})")
                return 0
    print(f"{hits} hits")
    return 0


def cmd_diff(a) -> int:
    sa, sb = Snapshot(a.snap_a), Snapshot(a.snap_b)
    lo, hi = a.range
    dtype = ">f4" if a.as_ == "f32" else ">u4"
    shown = 0
    for r in sa.regions:
        base, arr_a = sa.view(r, lo, hi)
        if not len(arr_a):
            continue
        data_b = sb.read(base, len(arr_a))
        if data_b is None:
            continue
        va, addrs = words(arr_a, base, 4, dtype)
        vb, _ = words(np.frombuffer(data_b, np.uint8), base, 4, dtype)
        with np.errstate(invalid="ignore"):
            if a.as_ == "f32":
                changed = np.isfinite(va) & np.isfinite(vb) & (np.abs(vb - va) >= a.min_delta) & \
                          (np.abs(va) < 1e7) & (np.abs(vb) < 1e7)
            else:
                changed = va != vb
        for i in np.nonzero(changed)[0]:
            if a.as_ == "f32":
                print(f"0x{int(addrs[i]):08X}  {va[i]:.4f} -> {vb[i]:.4f}  (d {vb[i] - va[i]:+.4f})")
            else:
                print(f"0x{int(addrs[i]):08X}  {int(va[i]):#010x} -> {int(vb[i]):#010x}")
            shown += 1
            if shown >= a.limit:
                print(f"(stopped at {a.limit})")
                return 0
    print(f"{shown} changed")
    return 0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = p.add_subparsers(dest="cmd", required=True)

    q = sub.add_parser("regions")
    q.add_argument("snap", type=Path)
    q.set_defaults(fn=cmd_regions)

    q = sub.add_parser("extract")
    q.add_argument("snap", type=Path)
    q.add_argument("addr", type=num)
    q.add_argument("size", type=num)
    q.add_argument("out", type=Path)
    q.set_defaults(fn=cmd_extract)

    q = sub.add_parser("read")
    q.add_argument("snap", type=Path)
    q.add_argument("addr", type=num)
    q.add_argument("--count", type=int, default=16)
    q.add_argument("--as", dest="as_", choices=("u32", "f32", "bytes"), default="u32")
    q.set_defaults(fn=cmd_read)

    for name, fn in (("find-u32", cmd_find_u32), ("find-f32", cmd_find_f32)):
        q = sub.add_parser(name)
        q.add_argument("snap", type=Path)
        if name == "find-u32":
            q.add_argument("value", type=num)
            q.add_argument("--align", type=int, default=4)
        else:
            q.add_argument("values", nargs="+", help="1-4 consecutive floats")
            q.add_argument("--tol", type=float, default=0.01)
        q.add_argument("--range", type=num, nargs=2, metavar=("LO", "HI"))
        q.add_argument("--limit", type=int, default=200)
        q.set_defaults(fn=fn)

    q = sub.add_parser("classof", help="RTTI class of objects (first word = vtable)")
    q.add_argument("snap", type=Path)
    q.add_argument("addrs", type=num, nargs="+")
    q.set_defaults(fn=cmd_classof)

    q = sub.add_parser("owner", help="nearest preceding RTTI objects of addresses")
    q.add_argument("snap", type=Path)
    q.add_argument("addrs", type=num, nargs="+")
    q.add_argument("--back", type=num, default=0x2000)
    q.add_argument("--depth", type=int, default=2)
    q.set_defaults(fn=cmd_owner)

    q = sub.add_parser("refs", help="pointers to [target, target+span) outside the image")
    q.add_argument("snap", type=Path)
    q.add_argument("target", type=num)
    q.add_argument("--span", type=num, default=4)
    q.add_argument("--name", action="store_true", help="name the object holding each ref")
    q.add_argument("--limit", type=int, default=200)
    q.set_defaults(fn=cmd_refs)

    q = sub.add_parser("diff")
    q.add_argument("snap_a", type=Path)
    q.add_argument("snap_b", type=Path)
    q.add_argument("--range", type=num, nargs=2, metavar=("LO", "HI"), required=True)
    q.add_argument("--as", dest="as_", choices=("f32", "u32"), default="f32")
    q.add_argument("--min-delta", type=float, default=0.001)
    q.add_argument("--limit", type=int, default=500)
    q.set_defaults(fn=cmd_diff)

    a = p.parse_args()
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
