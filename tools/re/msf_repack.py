#!/usr/bin/env python3
"""Repack an old big-directory MSF 7.00 PDB so LLVM tools can read it.

    msf_repack.py IN.pdb OUT.pdb [--block-size 4096]

The Skate 2 debug build's PDB (Xbox 360 toolchain, 2009) uses 1 KB blocks
and a stream directory so large that its block list spans several blocks:
the superblock then holds an array of block-map block numbers, which
llvm-pdbutil rejects ("Too many directory blocks"). This reads every stream
through that array and writes the same streams into a standard MSF 7.00 file
with bigger blocks, where the directory's block list fits in one block. The
stream contents are copied byte for byte.
"""
from __future__ import annotations

import argparse
import math
import struct
import sys
from pathlib import Path

MAGIC = b"Microsoft C/C++ MSF 7.00\r\n\x1aDS\x00\x00\x00"
NIL = 0xFFFFFFFF


def read_streams(data: bytes) -> list[bytes | None]:
    if not data.startswith(MAGIC):
        sys.exit("not an MSF 7.00 file")
    bs, _fpm, nblocks, dir_bytes, _unk = struct.unpack_from("<5I", data, 32)
    dir_blocks = math.ceil(dir_bytes / bs)
    map_blocks = math.ceil(dir_blocks * 4 / bs)
    # The block-map block numbers start at offset 52 (one for standard MSFs).
    map_list = struct.unpack_from(f"<{map_blocks}I", data, 52)
    index = b"".join(data[b * bs:(b + 1) * bs] for b in map_list)
    dir_list = struct.unpack_from(f"<{dir_blocks}I", index, 0)
    directory = b"".join(data[b * bs:(b + 1) * bs] for b in dir_list)[:dir_bytes]
    (count,) = struct.unpack_from("<I", directory, 0)
    sizes = struct.unpack_from(f"<{count}I", directory, 4)
    pos = 4 + 4 * count
    streams: list[bytes | None] = []
    for size in sizes:
        if size == NIL:
            streams.append(None)
            continue
        n = math.ceil(size / bs)
        blocks = struct.unpack_from(f"<{n}I", directory, pos)
        pos += 4 * n
        if any(b >= nblocks for b in blocks):
            sys.exit("stream block out of range")
        streams.append(b"".join(data[b * bs:(b + 1) * bs] for b in blocks)[:size])
    return streams


def write_msf(streams: list[bytes | None], bs: int) -> bytes:
    blocks: list[bytes] = [b""]  # block 0: superblock (filled last)
    fpm_period = bs  # every bs blocks: FPM copies at offsets 1 and 2 of the interval

    def is_fpm(i: int) -> bool:
        return i % fpm_period in (1, 2)

    def alloc(payload: bytes) -> list[int]:
        out = []
        for k in range(0, max(len(payload), 1) if payload else 0, bs):
            while is_fpm(len(blocks)):
                blocks.append(b"")  # FPM blocks, filled last
            out.append(len(blocks))
            blocks.append(payload[k:k + bs])
        return out

    stream_blocks = [alloc(s) if s else [] for s in streams]
    directory = struct.pack("<I", len(streams))
    directory += b"".join(struct.pack("<I", NIL if s is None else len(s)) for s in streams)
    for bl in stream_blocks:
        directory += b"".join(struct.pack("<I", b) for b in bl)
    dir_list = alloc(directory)
    index = b"".join(struct.pack("<I", b) for b in dir_list)
    if len(index) > bs:
        sys.exit("directory still too big for one block-map block; raise --block-size")
    (map_block,) = alloc(index)
    nblocks = len(blocks)
    # Free block map: one bit per block, 1 = free. Everything allocated is used.
    used_bits = bytearray(math.ceil(nblocks / 8))
    fpm = bytearray(b"\xff" * len(used_bits))
    for i in range(nblocks):
        fpm[i // 8] &= ~(1 << (i % 8)) & 0xFF
    fpm_bytes = bytes(fpm)
    for i in range(nblocks):
        if i and is_fpm(i):
            interval = i // fpm_period
            chunk = fpm_bytes[interval * bs:(interval + 1) * bs] if i % fpm_period == 1 else b""
            blocks[i] = chunk
    blocks[0] = MAGIC + struct.pack("<6I", bs, 1, nblocks, len(directory), 0, map_block)
    return b"".join(b.ljust(bs, b"\0") for b in blocks)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("inp", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--block-size", type=int, default=4096)
    a = ap.parse_args()
    streams = read_streams(a.inp.read_bytes())
    present = sum(1 for s in streams if s is not None)
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_bytes(write_msf(streams, a.block_size))
    print(f"{a.out}: {len(streams)} streams ({present} present), block size {a.block_size}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
