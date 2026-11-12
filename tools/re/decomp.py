#!/usr/bin/env python3
"""Decompile game functions by address with Ghidra (reverse-engineering aid).

  tools/re/decomp.py ADDR [ADDR ...] [--fresh]

ADDR is any address inside the function (hex, e.g. 0x82627128). Output is the
decompiled C, cached per function in out/re/decomp/<ADDR>.c. Needs the project
made once by tools/re/ghidra/setup.py. Complements xrefs.py (callers,
disassembly of the recompiled code) and gsnap.py (live memory).
VMX128 (Xenon-only vector) instructions don't decode; their functions show
gaps.
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools/re/ghidra"))
from setup import OUT, analyze_headless  # noqa: E402

CACHE = REPO / "out/re/decomp"


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("addrs", nargs="+")
    p.add_argument("--fresh", action="store_true", help="ignore the cache")
    p.add_argument("--ghidra")
    a = p.parse_args()
    CACHE.mkdir(parents=True, exist_ok=True)
    addrs = [f"{int(x, 16):08X}" for x in a.addrs]
    todo = [x for x in addrs if a.fresh or not (CACHE / f"{x}.c").exists()]
    if todo:
        if not (OUT / "project" / "skate3.gpr").exists():
            sys.exit("no Ghidra project: run tools/re/ghidra/setup.py SNAP.gsnap first")
        cmd = [analyze_headless(a.ghidra), str(OUT / "project"), "skate3",
               "-process", "skate3_tu3.bin", "-noanalysis", "-readOnly",
               "-scriptPath", str(REPO / "tools/re/ghidra"),
               "-postScript", "Decompile.java", *todo]
        out = subprocess.run(cmd, capture_output=True, text=True,
                             env={**os.environ, "GHIDRA_MAXMEM": os.environ.get("GHIDRA_MAXMEM", "12G")}).stdout
        for m in re.finditer(r"=== decomp ([0-9A-F]+) (\S+)\n(.*?)\n=== end", out, re.S):
            (CACHE / f"{m.group(1)}.c").write_text(f"// {m.group(2)}\n{m.group(3)}\n")
    for x in addrs:
        f = CACHE / f"{x}.c"
        print(f.read_text() if f.exists() else f"// {x}: no output (see Ghidra's log)\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
