#!/usr/bin/env python3
"""One-time Ghidra project for the TU3 game image (reverse-engineering aid).

The recompiler's addresses are TU3: the base default.xex with the title
update's default.xexp applied. An XEX loader would give the unpatched base,
so this imports the patched image straight from a guest snapshot (.gsnap,
see gsnap.py): REX_IMAGE_BASE..+REX_IMAGE_SIZE as raw big-endian PowerPC
(Xenon: PowerPC:BE:64:A2ALT-32addr) at 0x82000000, then creates every
recompiled function (generated/skate3_init.cpp) under its sub_XXXXXXXX name.

  tools/re/ghidra/setup.py SNAP.gsnap [--ghidra DIR]

Needs Ghidra (analyzeHeadless; `nix profile install nixpkgs#ghidra`).
Output: out/re/ghidra/ (project skate3, image, function list). Then use
tools/re/decomp.py.
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "tools/re"))
import gsnap  # noqa: E402

IMAGE_BASE, IMAGE_SIZE = 0x82000000, 0x11B0000
OUT = REPO / "out/re/ghidra"
LANGUAGE = "PowerPC:BE:64:A2ALT-32addr"


def analyze_headless(ghidra: str | None) -> str:
    for cand in filter(None, [ghidra and str(Path(ghidra) / "support/analyzeHeadless"),
                              shutil.which("ghidra-analyzeHeadless"),
                              shutil.which("analyzeHeadless")]):
        if Path(cand).exists():
            return cand
    for p in Path.home().glob(".nix-profile/lib/ghidra/support/analyzeHeadless"):
        return str(p)
    sys.exit("analyzeHeadless not found (install Ghidra: nix profile install nixpkgs#ghidra)")


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("snap", type=Path)
    p.add_argument("--ghidra", help="Ghidra install folder")
    a = p.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)

    image = OUT / "skate3_tu3.bin"
    data = gsnap.Snapshot(a.snap).read(IMAGE_BASE, IMAGE_SIZE)
    if not data or len(data) != IMAGE_SIZE:
        sys.exit(f"{a.snap} does not hold the whole image")
    image.write_bytes(data)

    funcs = OUT / "functions.txt"
    pat = re.compile(r"\{ 0x([0-9A-F]{8}), (sub_[0-9A-F]{8}|__imp__\w+) \}")
    lines = [f"{m.group(1)} {m.group(2)}"
             for m in pat.finditer((REPO / "generated/skate3_init.cpp").read_text())]
    funcs.write_text("\n".join(lines) + "\n")
    print(f"image {image} ({len(data):#x} bytes), {len(lines)} functions")

    proj = OUT / "project"
    proj.mkdir(exist_ok=True)
    cmd = [analyze_headless(a.ghidra), str(proj), "skate3", "-overwrite",
           "-import", str(image), "-loader", "BinaryLoader",
           "-loader-baseAddr", hex(IMAGE_BASE), "-processor", LANGUAGE,
           "-scriptPath", str(Path(__file__).resolve().parent),
           "-preScript", "SetOptions.java",
           "-postScript", "CreateFunctions.java", str(funcs),
           "-analysisTimeoutPerFile", "7200"]
    print(" ".join(cmd))
    return subprocess.run(cmd, env={**os.environ, "GHIDRA_MAXMEM": os.environ.get("GHIDRA_MAXMEM", "12G")}).returncode


if __name__ == "__main__":
    sys.exit(main())
