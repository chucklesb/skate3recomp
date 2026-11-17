#!/usr/bin/env python3
"""Print a vtable's slots from the TU3 image, with the names we know.

    vtable.py ADDR [--slots N]

Reads big-endian words from out/re/ghidra/skate3_tu3.bin (flat image at
0x82000000) and names each slot from docs/re/symbols.toml (the hand map),
then Skate 3's Lua binding names, then the Skate 2 transfer
(docs/re/names_sk2.tsv), like re_symbols.py lookup. Find a vtable from a
constructor's `lis/addi; stw rX,0(r3)`, or log an object's first word.
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools/harness"))
import re_symbols  # noqa: E402

IMAGE = REPO / "out/re/ghidra/skate3_tu3.bin"
BASE = 0x82000000


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("addr", help="vtable address (hex)")
    ap.add_argument("--slots", type=int, default=24)
    a = ap.parse_args()
    data = IMAGE.read_bytes()
    vt = int(a.addr, 16)
    hand = re_symbols.load()
    bind = re_symbols.load_bindings()
    sk2 = re_symbols.load_sk2()
    for k in range(a.slots):
        o = vt + 4 * k - BASE
        if not 0 <= o <= len(data) - 4:
            break
        fn = struct.unpack(">I", data[o:o + 4])[0]
        key = f"sub_{fn:08X}"
        if key in hand:
            name = f"{hand[key].get('name', '?')} [map]"
        elif key in bind:
            b = bind[key]  # shared stubs carry dozens of binding names
            name = f"{b if len(b) <= 60 else b[:57] + '...'} [binding]"
        elif key in sk2:
            name = f"{sk2[key][0]} [sk2 {sk2[key][1]}, {sk2[key][2]}]"
        else:
            name = ""
        print(f"  +0x{4 * k:02X}  {fn:08X}  {name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
