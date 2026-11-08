#!/usr/bin/env python3
"""Cross-references from the recompiled game code (reverse-engineering aid).

The generated C++ (generated/skate3_recomp.*.cpp) carries every PowerPC
instruction as a `// ` comment inside DEFINE_REX_FUNC(sub_XXXXXXXX). This
indexes, per function:
  - direct calls (`bl 0x...`) and tail branches to other functions (`b 0x...`);
  - constant addresses built with `lis rX,HI` + `addi/ori/lwz/stw/lfs/... rY,LO(rX)`.

  xrefs.py index                       # build out/re/xrefs.json (about a minute)
  xrefs.py callers 82D55AE8            # who calls this function
  xrefs.py callees 82D55AE8            # what it calls
  xrefs.py refs 0x820963F0 [--span 64] # functions that build an address in range
  xrefs.py dis 82D55AE8 [-n 120]       # the function's disassembly
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from collections import defaultdict
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
GEN = REPO / "generated"
INDEX = REPO / "out/re/xrefs.json"

FUNC_RE = re.compile(r"^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)")
INSN_RE = re.compile(r"^\t// (.*)$")
BL_RE = re.compile(r"^bl 0x([0-9a-f]+)$")
B_RE = re.compile(r"^b 0x([0-9a-f]+)$")
LIS_RE = re.compile(r"^lis (r\d+),(-?\d+)$")
# addi rD,rA,SIMM  |  ori rD,rA,UIMM  |  op rD,SIMM(rA)
ADDI_RE = re.compile(r"^(addi|addic|ori) (r\d+),(r\d+),(-?\d+)$")
MEM_RE = re.compile(r"^(l[bhwd]z?u?x?|lfs|lfd|st[bhwd]u?|stfs|stfd|lwa|lha) (\S+),(-?\d+)\((r\d+)\)$")


def norm(a: str) -> int:
    return int(a, 16) if not a.lower().startswith("0x") else int(a, 0)


def scan_file(path: Path, funcs: dict, refs: dict) -> None:
    cur = None
    hi: dict[str, int] = {}
    for line in path.open(encoding="latin-1"):
        m = FUNC_RE.match(line)
        if m:
            cur = m.group(1)
            funcs.setdefault(cur, {"file": path.name, "calls": set()})
            hi = {}
            continue
        if cur is None:
            continue
        m = INSN_RE.match(line)
        if not m:
            continue
        insn = m.group(1).strip()
        if (b := BL_RE.match(insn)) or (b := B_RE.match(insn)):
            t = int(b.group(1), 16)
            # Branches inside the function are local labels; keep only far ones.
            if b.re is BL_RE or abs(t - int(cur, 16)) > 0x4000:
                funcs[cur]["calls"].add(f"{t:08X}")
            continue
        if (l := LIS_RE.match(insn)):
            hi[l.group(1)] = (int(l.group(2)) & 0xFFFF) << 16
            continue
        if (a := ADDI_RE.match(insn)):
            op, rd, ra, imm = a.groups()
            if ra in hi:
                v = int(imm)
                ea = (hi[ra] | (v & 0xFFFF)) if op == "ori" else (hi[ra] + v) & 0xFFFFFFFF
                refs[f"{ea:08X}"].add(cur)
                if rd != ra:
                    hi.pop(rd, None)
            else:
                hi.pop(rd, None)
            continue
        if (mm := MEM_RE.match(insn)):
            _op, _rt, imm, ra = mm.groups()
            if ra in hi:
                ea = (hi[ra] + int(imm)) & 0xFFFFFFFF
                refs[f"{ea:08X}"].add(cur)
            continue


def cmd_index(_a) -> int:
    funcs: dict = {}
    refs: dict = defaultdict(set)
    files = sorted(GEN.glob("skate3_recomp.*.cpp"))
    for i, f in enumerate(files):
        scan_file(f, funcs, refs)
        if i % 25 == 0:
            print(f"  {i}/{len(files)} {f.name}", file=sys.stderr)
    callers: dict = defaultdict(list)
    for fn, d in funcs.items():
        for t in d["calls"]:
            callers[t].append(fn)
    out = {
        "funcs": {k: {"file": v["file"], "calls": sorted(v["calls"])} for k, v in funcs.items()},
        "callers": {k: sorted(v) for k, v in callers.items()},
        "refs": {k: sorted(v) for k, v in refs.items()},
    }
    INDEX.parent.mkdir(parents=True, exist_ok=True)
    INDEX.write_text(json.dumps(out))
    print(f"{len(funcs)} functions, {len(refs)} referenced addresses -> {INDEX.relative_to(REPO)}")
    return 0


def load() -> dict:
    if not INDEX.exists():
        raise SystemExit(f"{INDEX} is missing: run `xrefs.py index` first")
    return json.loads(INDEX.read_text())


def containing(idx: dict, addr: int) -> str | None:
    """The function whose start is the closest at or below addr."""
    best = None
    for k in idx["funcs"]:
        v = int(k, 16)
        if v <= addr and (best is None or v > best):
            best = v
    return f"{best:08X}" if best is not None else None


def cmd_callers(a) -> int:
    idx = load()
    t = f"{norm(a.func):08X}"
    for c in idx["callers"].get(t, []):
        print(f"sub_{c}  ({idx['funcs'][c]['file']})")
    return 0


def cmd_callees(a) -> int:
    idx = load()
    t = f"{norm(a.func):08X}"
    for c in idx["funcs"].get(t, {}).get("calls", []):
        print(f"sub_{c}")
    return 0


def cmd_refs(a) -> int:
    idx = load()
    lo = norm(a.addr)
    hits = []
    for k, fns in idx["refs"].items():
        v = int(k, 16)
        if lo <= v < lo + a.span:
            hits += [(v, f) for f in fns]
    for v, f in sorted(hits):
        print(f"0x{v:08X}  sub_{f}")
    print(f"{len(hits)} refs")
    return 0


def cmd_dis(a) -> int:
    t = f"{norm(a.func):08X}"
    for f in sorted(GEN.glob("skate3_recomp.*.cpp")):
        text = f.read_text(encoding="latin-1")
        i = text.find(f"DEFINE_REX_FUNC(sub_{t})")
        if i < 0:
            continue
        n = 0
        for line in text[i:].splitlines()[1:]:
            if line.startswith("DEFINE_REX_FUNC"):
                break
            if (m := INSN_RE.match(line)):
                print(m.group(1))
                n += 1
                if n >= a.n:
                    break
        return 0
    print(f"sub_{t} not found", file=sys.stderr)
    return 1


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("index").set_defaults(fn=cmd_index)
    for name, fn in (("callers", cmd_callers), ("callees", cmd_callees)):
        q = sub.add_parser(name)
        q.add_argument("func")
        q.set_defaults(fn=fn)
    q = sub.add_parser("refs")
    q.add_argument("addr")
    q.add_argument("--span", type=lambda s: int(s, 0), default=4)
    q.set_defaults(fn=cmd_refs)
    q = sub.add_parser("dis")
    q.add_argument("func")
    q.add_argument("-n", type=int, default=120)
    q.set_defaults(fn=cmd_dis)
    a = p.parse_args()
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
