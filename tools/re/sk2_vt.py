#!/usr/bin/env python3
"""Ghidra Auto Version Tracking: Skate 2 (named, from its PDB) -> Skate 3 TU3.

    sk2_vt.py import      # copy the TU3 project, import + analyze the Skate 2 image
    sk2_vt.py run         # AutoVersionTrackingScript: session /sk2_tu3
    sk2_vt.py export      # -> out/re/match/vt_matches.tsv
    sk2_vt.py compare     # VT against sk2_match.py's pairs (agreement per method)

Needs `sk2_match.py prepare` first (out/re/sk2/) and the TU3 project
(tools/re/ghidra/setup.py). Works in out/re/ghidra-vt/, a copy of the TU3
project, so VT's renames never touch the project decomp.py uses. The Skate 2
functions are created at the PDB addresses as sk2_<addr>; names map back
through out/re/sk2/functions.tsv. Analysis of the Skate 2 image takes about
an hour.
"""
from __future__ import annotations

import argparse
import collections
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools/re/ghidra"))
from setup import OUT as TU3_OUT, analyze_headless  # noqa: E402

VT = REPO / "out/re/ghidra-vt"
SK2 = REPO / "out/re/sk2"
MATCH = REPO / "out/re/match"
SCRIPTS = REPO / "tools/re/ghidra"
LANGUAGE = "PowerPC:BE:64:A2ALT-32addr"
ENV = {**os.environ, "GHIDRA_MAXMEM": os.environ.get("GHIDRA_MAXMEM", "12G")}


def headless(*args: str) -> int:
    cmd = [analyze_headless(None), str(VT / "project"), "skate3", *args]
    print(" ".join(cmd), flush=True)
    return subprocess.run(cmd, env=ENV).returncode


def cmd_import() -> int:
    VT.mkdir(parents=True, exist_ok=True)
    if not (VT / "project").exists():
        shutil.copytree(TU3_OUT / "project", VT / "project")
    funcs = VT / "sk2_functions.txt"
    with open(funcs, "w") as f:
        for line in open(SK2 / "functions.tsv"):
            a = line.split("\t", 1)[0]
            f.write(f"{a} sk2_{a}\n")
    image = VT / "sk2.bin"
    shutil.copyfile(next((SK2 / "image").glob("*.bin")), image)
    return headless("-import", str(image), "-loader", "BinaryLoader",
                    "-loader-baseAddr", "0x82000000", "-processor", LANGUAGE,
                    "-scriptPath", str(SCRIPTS), "-preScript", "SetOptions.java",
                    "-postScript", "CreateFunctions.java", str(funcs),
                    "-analysisTimeoutPerFile", "10800")


def cmd_run() -> int:
    return headless("-process", "skate3_tu3.bin", "-noanalysis",
                    "-postScript", "AutoVersionTrackingScript.java", "/", "sk2_tu3", "/sk2.bin")


def cmd_export() -> int:
    MATCH.mkdir(parents=True, exist_ok=True)
    return headless("-process", "skate3_tu3.bin", "-noanalysis",
                    "-scriptPath", str(SCRIPTS),
                    "-postScript", "ExportVTMatches.java", "/sk2_tu3",
                    str(MATCH / "vt_matches.tsv"))


def load_vt(accepted_only: bool = True) -> dict[int, tuple[int, str, float, str]]:
    """TU3 address -> (Skate 2 address, correlator, similarity, status); the
    best-similarity match per destination."""
    best: dict[int, tuple[int, str, float, str]] = {}
    for line in open(MATCH / "vt_matches.tsv"):
        src, dst, corr, sim, _conf, status = line.rstrip("\n").split("\t")
        if accepted_only and status != "ACCEPTED":
            continue
        d, s, simf = int(dst, 16), int(src, 16), float(sim)
        if d not in best or simf > best[d][2]:
            best[d] = (s, corr, simf, status)
    return best


def cmd_compare() -> int:
    vt = load_vt()
    ours = {}
    for line in open(MATCH / "sk2_tu3.tsv"):
        a3, a2, how, _ = line.rstrip("\n").split("\t", 3)
        ours[int(a3, 16)] = (int(a2, 16), how)
    agree = collections.Counter()
    differ = collections.Counter()
    for a3, (a2, how) in ours.items():
        if a3 in vt:
            (agree if vt[a3][0] == a2 else differ)[how] += 1
    by_corr = collections.Counter(v[1] for v in vt.values())
    print(f"VT accepted function matches: {len(vt)}; by correlator: "
          + ", ".join(f"{k} {n}" for k, n in by_corr.most_common()))
    print(f"sk2_match pairs: {len(ours)}; both named: {sum(agree.values()) + sum(differ.values())}")
    for how in sorted(set(agree) | set(differ)):
        a, d = agree[how], differ[how]
        print(f"  {how}: agree {a}, differ {d} ({100 * a / max(a + d, 1):.0f}% agree)")
    only_vt = len(set(vt) - set(ours))
    print(f"VT-only: {only_vt}; union: {len(set(vt) | set(ours))}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["import", "run", "export", "compare"])
    a = ap.parse_args()
    return {"import": cmd_import, "run": cmd_run, "export": cmd_export,
            "compare": cmd_compare}[a.cmd]()


if __name__ == "__main__":
    raise SystemExit(main())
