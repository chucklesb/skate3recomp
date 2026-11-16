#!/usr/bin/env python3
"""Name Skate 3 (TU3) functions from the Skate 2 debug build's PDB (roadmap RE-2).

    sk2_match.py prepare REF_DIR      # PDB -> out/re/sk2/functions.tsv, image dump
    sk2_match.py match                # -> out/re/match/sk2_tu3.tsv + stats
    sk2_match.py export               # -> docs/re/names_sk2.tsv (names only)
    sk2_match.py lookup NAME|ADDR     # search the match table

REF_DIR holds sk82_na_m.xex and sk82_na_m.pdb (never committed; also
$SKATE3_RE_REF_DIR). prepare repacks the PDB (msf_repack.py), dumps its
procedures with llvm-pdbutil, and unpacks the XEX with the SDK's
`rexglue dump-xex`.

Matching: each function's instruction words are normalized (branch
displacements masked; `lis` immediates and the low halves that complete
them masked), then hashed.
  exact      same length and hash, unique on both sides
  callgraph  the callees of matched pairs, paired by position where both
             call sequences have the same length and the sizes agree
Only names travel; the reference files stay outside the repo.
"""
from __future__ import annotations

import argparse
import collections
import hashlib
import os
import re
import struct
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SK2 = REPO / "out/re/sk2"
OUT = REPO / "out/re/match"
TU3_BIN = REPO / "out/re/ghidra/skate3_tu3.bin"
TU3_FUNCS = REPO / "out/re/ghidra/functions.txt"
BASE = 0x82000000
VT_ANCHORS = int(os.environ.get('SK2_VT_ANCHORS', 3))
ALIGN_MIN = float(os.environ.get('SK2_ALIGN_MIN', 0.85))
TEXT_RVA = {3: 0x430000}  # section 3 = .text in the Skate 2 image (section headers)


def llvm_bin(tool: str) -> str:
    for p in sorted(Path("/nix/store").glob("*-llvm-20*/bin")):
        if (p / tool).exists():
            return str(p / tool)
    sys.exit(f"{tool} not found (nix store llvm 20)")


# ---------------------------------------------------------------- prepare

def prepare(ref: Path) -> None:
    SK2.mkdir(parents=True, exist_ok=True)
    pdb = SK2 / "sk82_na_m.pdb"
    subprocess.run([sys.executable, "-I", str(REPO / "tools/re/msf_repack.py"),
                    str(ref / "sk82_na_m.pdb"), str(pdb)], check=True)
    syms = SK2 / "symbols.txt"
    with open(syms, "w") as f:
        subprocess.run([llvm_bin("llvm-pdbutil"), "dump", "-symbols", str(pdb)], stdout=f, check=True)
    # Section RVAs from the headers.
    hdr = subprocess.run([llvm_bin("llvm-pdbutil"), "dump", "-section-headers", str(pdb)],
                         capture_output=True, text=True, check=True).stdout
    rvas, sec = {}, None
    for line in hdr.splitlines():
        m = re.match(r"\s*SECTION HEADER #(\d+)", line)
        if m:
            sec = int(m.group(1))
        m = re.match(r"\s*([0-9A-F]+) virtual address", line)
        if m and sec:
            rvas[sec] = int(m.group(1), 16)
    proc = re.compile(r"S_[GL]PROC32 \[size = \d+\] `(.*)`$")
    addr = re.compile(r"addr = (\d{4}):(\d+), code size = (\d+)")
    funcs, name = {}, None
    for line in open(syms, errors="replace"):
        m = proc.search(line)
        if m:
            name = m.group(1)
            continue
        if name:
            m = addr.search(line)
            if m:
                s, off, size = int(m.group(1)), int(m.group(2)), int(m.group(3))
                if s in rvas and size:
                    va = BASE + rvas[s] + off
                    funcs.setdefault(va, (size, name))
            name = None
    with open(SK2 / "functions.tsv", "w") as f:
        for va in sorted(funcs):
            size, n = funcs[va]
            f.write(f"{va:08X}\t{size}\t{n}\n")
    print(f"{len(funcs)} Skate 2 functions -> {SK2 / 'functions.tsv'}")
    img = SK2 / "image"
    subprocess.run([str(REPO / "third_party/rexglue-sdk/out/linux-amd64/rexglue"), "dump-xex",
                    str(ref / "sk82_na_m.xex"), str(img)], check=True, capture_output=True)
    print(f"image -> {next(img.glob('*.bin'))}")


# ---------------------------------------------------------------- match

def words(img: bytes, va: int, size: int) -> list[int]:
    off = va - BASE
    n = size // 4
    if off < 0 or off + n * 4 > len(img):
        return []
    return list(struct.unpack_from(f">{n}I", img, off))


def normalize(ws: list[int]) -> tuple[bytes, list[int]]:
    """Masked words, and the targets of `bl` calls (absolute), in order."""
    out, calls, lis = [], [], set()
    for i, w in enumerate(ws):
        op = w >> 26
        if op == 18:  # b / bl
            if w & 1:  # bl
                li = w & 0x03FFFFFC
                if li & 0x02000000:
                    li -= 0x04000000
                calls.append(i)  # position; the target is resolved by the caller
            w &= 0xFC000003
        elif op == 16:  # bc
            w &= 0xFFFF0003
        elif op == 15 and ((w >> 16) & 31) == 0:  # lis rD, HI
            lis.add((w >> 21) & 31)
            w &= 0xFFFF0000
        elif op in (14, 24, 32, 34, 36, 38, 40, 42, 44, 46, 48, 50, 52, 54, 58, 62) and \
                ((w >> 16) & 31) in lis:  # the low half of an address built with lis
            w &= 0xFFFF0000
        out.append(w)
    return struct.pack(f">{len(out)}I", *out), calls


def call_targets(ws: list[int], va: int, positions: list[int]) -> list[int]:
    t = []
    for i in positions:
        li = ws[i] & 0x03FFFFFC
        if li & 0x02000000:
            li -= 0x04000000
        t.append(va + 4 * i + li)
    return t


def section_rvas() -> dict[int, int]:
    """PDB section number -> RVA (llvm-pdbutil section headers)."""
    hdr = subprocess.run([llvm_bin("llvm-pdbutil"), "dump", "-section-headers",
                          str(SK2 / "sk82_na_m.pdb")], capture_output=True, text=True,
                         check=True).stdout
    rvas, sec = {}, None
    for line in hdr.splitlines():
        m = re.match(r"\s*SECTION HEADER #(\d+)", line)
        if m:
            sec = int(m.group(1))
        m = re.match(r"\s*([0-9A-F]+) virtual address", line)
        if m and sec:
            rvas[sec] = int(m.group(1), 16)
    return rvas


def undecorate_vtable(sym: str) -> str:
    """'??_7FaceHolder@CharacterFaceManager@@6B@' -> 'CharacterFaceManager::FaceHolder'
    (simple names; templates keep their mangled form)."""
    body = sym[4:].split("@@6B", 1)[0]
    if "?$" in body:
        return sym
    return "::".join(reversed([p for p in body.split("@") if p]))


def sk2_vtables(img: bytes, funcs: set[int]) -> list[tuple[str, list[int]]]:
    """Skate 2's vtables from the PDB publics (??_7...): (class, slots)."""
    rvas = section_rvas()
    pub = re.compile(r"`(\?\?_7.*)`$")
    addr = re.compile(r"addr = (\d{4}):(\d+)")
    out, name = [], None
    n = len(img) // 4
    for line in open(SK2 / "publics.txt", errors="replace"):
        m = pub.search(line)
        if m:
            name = m.group(1)
            continue
        if name:
            m = addr.search(line)
            if m and int(m.group(1)) in rvas:
                va = BASE + rvas[int(m.group(1))] + int(m.group(2))
                i = (va - BASE) // 4
                slots = []
                while i < n:
                    (w,) = struct.unpack_from(">I", img, i * 4)
                    if w not in funcs:
                        break
                    slots.append(w)
                    i += 1
                if slots:
                    out.append((undecorate_vtable(name), slots))
            name = None
    return out


def pointer_runs(img: bytes, funcs: set[int]) -> list[list[int]]:
    """Runs of 2+ consecutive words that are function starts (vtables and
    other function tables), outside the code itself."""
    n = len(img) // 4
    arr = struct.unpack(f">{n}I", img[:n * 4])
    runs, cur = [], []
    for w in arr:
        if w in funcs:
            cur.append(w)
        else:
            if len(cur) >= 2:
                runs.append(cur)
            cur = []
    if len(cur) >= 2:
        runs.append(cur)
    return runs


def string_refs(ws: list[int], img: bytes) -> set[bytes]:
    """C strings (4+ printable bytes) whose address the function builds with
    `lis rD, HI` + `addi rX, rD, LO`."""
    hi_of: dict[int, int] = {}
    out = set()
    for w in ws:
        op = w >> 26
        rd, ra = (w >> 21) & 31, (w >> 16) & 31
        if op == 15 and ra == 0:  # lis
            hi_of[rd] = (w & 0xFFFF) << 16
            continue
        if op == 14 and ra in hi_of:  # addi
            lo = w & 0xFFFF
            if lo & 0x8000:
                lo -= 0x10000
            addr = (hi_of[ra] + lo) & 0xFFFFFFFF
            off = addr - BASE
            if 0 <= off < len(img):
                s = img[off:off + 96].split(b"\0", 1)[0]
                if len(s) >= 4 and all(32 <= c < 127 for c in s):
                    out.add(s)
    return out


def strip_asserts(ws: list[int], va: int, debugbreak: set[int]) -> list[int]:
    """Skate 2 without its assert blocks, as retail TU3 was built: for each
    `bl DebugBreak`, drop the conditional branch that skips over it (target =
    the next instruction), the compare just before that branch, and
    everything in between (message, line, `bl FailAssert`)."""
    drop = set()
    for k, w in enumerate(ws):
        if w >> 26 != 18 or not (w & 1):
            continue
        li = w & 0x03FFFFFC
        if li & 0x02000000:
            li -= 0x04000000
        if va + 4 * k + li not in debugbreak:
            continue
        for b in range(k - 1, max(-1, k - 40), -1):
            wb = ws[b]
            if wb >> 26 == 16:  # bc
                bd = wb & 0xFFFC
                if bd & 0x8000:
                    bd -= 0x10000
                if b + bd // 4 == k + 1:
                    lo = b - 1 if b > 0 and (ws[b - 1] >> 26) in (10, 11, 31) else b
                    drop.update(range(lo, k + 1))
                break
    return [w for i, w in enumerate(ws) if i not in drop] if drop else ws


def trim(ws: list[int]) -> list[int]:
    while ws and ws[-1] in (0, 0x60000000):  # padding, nop
        ws = ws[:-1]
    return ws


def match(holdout: float = 0.0) -> None:
    sk2_img = next((SK2 / "image").glob("*.bin")).read_bytes()
    tu3_img = TU3_BIN.read_bytes()
    sk2 = {}
    for line in open(SK2 / "functions.tsv"):
        a, s, n = line.rstrip("\n").split("\t", 2)
        sk2[int(a, 16)] = (int(s), n)
    starts = sorted(int(l.split()[0], 16) for l in open(TU3_FUNCS))
    tu3 = {}
    for i, a in enumerate(starts):
        end = starts[i + 1] if i + 1 < len(starts) else a + 4
        tu3[a] = trim(words(tu3_img, a, min(end - a, 0x20000)))

    def index(funcs: dict, img_words) -> tuple[dict, dict, dict]:
        by_hash = collections.defaultdict(list)
        calls, ws_of = {}, {}
        for a, ws in funcs.items():
            if len(ws) < 4:
                continue
            h, pos = normalize(ws)
            by_hash[(len(ws), hashlib.sha1(h).digest())].append(a)
            calls[a] = call_targets(ws, a, pos)
            ws_of[a] = ws
        return by_hash, calls, ws_of

    debugbreak = {a for a, (_, n) in sk2.items() if n == "DebugBreak"}
    sk2_raw = {a: words(sk2_img, a, s) for a, (s, _) in sk2.items()}
    sk2_words = {a: strip_asserts(ws, a, debugbreak) for a, ws in sk2_raw.items()}
    h2, _, _ = index(sk2_words, None)
    # Call targets from the unstripped code (stripping moves instructions).
    _, calls2, _ = index(sk2_raw, None)
    h3, calls3, _ = index(tu3, None)
    pairs: dict[int, tuple[int, str]] = {}  # tu3 -> (sk2, method)
    used2 = set()
    for k, l3 in h3.items():
        l2 = h2.get(k)
        if l2 and len(l2) == 1 and len(l3) == 1:
            pairs[l3[0]] = (l2[0], "exact")
            used2.add(l2[0])
    exact = len(pairs)
    # Precision check: hide a share of the exact pairs (near-certain), let the
    # other passes rediscover them, and score them against the hidden truth.
    truth: dict[int, int] = {}
    if holdout:
        import random
        rng = random.Random(1)
        for a3 in [a for a in pairs if rng.random() < holdout]:
            truth[a3] = pairs.pop(a3)[0]
            used2.discard(truth[a3])
    # Strings: a literal used by exactly one function in each build pairs
    # them (most of Skate 3's own code has no exact twin, but keeps its
    # strings). Skate 2's assert messages are left out (asserts are gone in
    # TU3): only strings the TU3 image also contains count.
    tu3_strings = {a: string_refs(ws, tu3_img) for a, ws in tu3.items()}
    sk2_strings = {a: string_refs(ws, sk2_img) for a, ws in sk2_raw.items()}
    users3, users2 = collections.defaultdict(list), collections.defaultdict(list)
    for a, ss in tu3_strings.items():
        for s in ss:
            users3[s].append(a)
    for a, ss in sk2_strings.items():
        for s in ss:
            if s in users3:
                users2[s].append(a)
    str_votes: dict[int, collections.Counter] = collections.defaultdict(collections.Counter)
    for s, l2 in users2.items():
        l3 = users3[s]
        if len(l2) == 1 and len(l3) == 1:
            str_votes[l3[0]][l2[0]] += 1
    str_pairs = []
    for a3, c in str_votes.items():
        (a2, n), *rest = c.most_common()
        if not rest or rest[0][1] < n:
            str_pairs.append((n, a3, a2))
    # Ambiguous hashes (same code under several names, e.g. thunks): left out.
    sizes3 = {a: len(ws) for a, ws in tu3.items()}

    # The Skate 2 build keeps its asserts (xcore::FailAssert + DebugBreak,
    # ~20k calls each); retail TU3 doesn't. Leave those calls out of Skate 2's
    # call sequences so they line up.
    noise2 = {a for a, (_, n) in sk2.items() if n in ("DebugBreak", "xcore::FailAssert")}
    calls2 = {a: [t for t in c if t not in noise2] for a, c in calls2.items()}

    def size_ok(n2: int, n3: int) -> bool:
        # Skate 2 functions carry their asserts: allow them to be bigger.
        return n3 > 0 and 0.25 * n2 - 4 <= n3 <= 1.3 * n2 + 4

    def add(a3: int, a2: int, how: str) -> bool:
        if a3 in pairs or a2 in used2 or a2 not in sk2 or a3 not in sizes3:
            return False
        pairs[a3] = (a2, how)
        used2.add(a2)
        return True

    for _, a3, a2 in sorted(str_pairs, reverse=True):
        add(a3, a2, "strings")

    def callgraph_pass() -> int:
        new = 0
        for a3, (a2, _) in list(pairs.items()):
            c3, c2 = calls3.get(a3, []), calls2.get(a2, [])
            if len(c3) != len(c2) or not c3:
                continue
            for t3, t2 in zip(c3, c2):
                if t2 in sk2 and t3 in sizes3 and size_ok(sk2[t2][0] // 4, sizes3[t3]):
                    new += add(t3, t2, "callgraph")
        return new

    # Vtables. Skate 2's are named by the PDB (??_7Class@@6B@); TU3's are
    # unnamed runs of function pointers (no RTTI in either build). A matched
    # function at run index i and Skate 2 vtable index j lines the two up at
    # offset i - j; two or more agreeing anchors name every slot.
    vt2 = sk2_vtables(sk2_img, set(sk2))
    runs3 = pointer_runs(tu3_img, set(tu3))
    pos3 = collections.defaultdict(list)
    for r, run in enumerate(runs3):
        for i, f in enumerate(run):
            pos3[f].append((r, i))
    pos2 = collections.defaultdict(list)
    for v, (_, slots) in enumerate(vt2):
        for j, f in enumerate(slots):
            pos2[f].append((v, j))
    vt_named: dict[int, str] = {}  # TU3 vtable address -> class

    def vtable_pass() -> int:
        votes = collections.Counter()
        for a3, (a2, _) in pairs.items():
            p3, p2 = pos3.get(a3, ()), pos2.get(a2, ())
            if len(p3) > 8 or len(p2) > 8:
                continue  # shared stubs (default dtors, pure-virtual): no anchor
            for r, i in p3:
                for v, j in p2:
                    votes[(r, v, i - j)] += 1
        new = 0
        for (r, v, off), n in votes.items():
            if n < VT_ANCHORS:
                continue
            run, (cls, slots) = runs3[r], vt2[v]
            if off >= 0 and off < len(run):
                vt_named.setdefault(run[0] if off == 0 else -1, cls)
            for k, s2 in enumerate(slots):
                i = off + k
                if 0 <= i < len(run):
                    s3 = run[i]
                    if s3 in sizes3 and size_ok(sk2[s2][0] // 4, sizes3[s3]):
                        new += add(s3, s2, "vtable")
        return new

    # Link order: both builds link mostly the same objects in the same order.
    # Between two matched pairs that are in order on both sides, the
    # unmatched functions correspond in sequence: equal gaps pair by
    # position, unequal gaps are aligned by similarity (stripped size, call
    # count).
    order3 = sorted(tu3)
    order2 = sorted(sk2)
    idx3 = {a: i for i, a in enumerate(order3)}
    idx2 = {a: i for i, a in enumerate(order2)}
    ncalls3 = {a: len(c) for a, c in calls3.items()}
    ncalls2 = {a: len(c) for a, c in calls2.items()}
    len2 = {a: len(ws) for a, ws in sk2_words.items()}

    def similar(a3: int, a2: int) -> float:
        n3, n2 = sizes3.get(a3, 0), len2.get(a2, 0)
        if not n3 or not n2:
            return 0.0
        s = min(n3, n2) / max(n3, n2)
        c3, c2 = ncalls3.get(a3, 0) + 1, ncalls2.get(a2, 0) + 1
        return 0.6 * s + 0.4 * (min(c3, c2) / max(c3, c2))

    def lis(seq: list[tuple[int, int]]) -> list[tuple[int, int]]:
        """Longest run of pairs increasing in both coordinates (patience sort)."""
        import bisect
        tails, tails_i, prev = [], [], [-1] * len(seq)
        for i, (_, y) in enumerate(seq):
            k = bisect.bisect_left(tails, y)
            if k == len(tails):
                tails.append(y)
                tails_i.append(i)
            else:
                tails[k] = y
                tails_i[k] = i
            prev[i] = tails_i[k - 1] if k else -1
        out, i = [], tails_i[-1] if tails_i else -1
        while i >= 0:
            out.append(seq[i])
            i = prev[i]
        return out[::-1]

    def layout_pass() -> int:
        anchors = sorted((idx3[a3], idx2[a2]) for a3, (a2, _) in pairs.items()
                         if a3 in idx3 and a2 in idx2)
        chain = lis(anchors)
        new = 0
        for (i0, j0), (i1, j1) in zip(chain, chain[1:]):
            g3 = [a for a in order3[i0 + 1:i1] if a not in pairs]
            g2 = [a for a in order2[j0 + 1:j1] if a not in used2]
            if not g3 or not g2 or len(g3) > 60 or len(g2) > 60:
                continue
            if len(g3) == len(g2):
                for a3, a2 in zip(g3, g2):
                    if similar(a3, a2) >= 0.55:
                        new += add(a3, a2, "layout")
                continue
            # Needleman-Wunsch: match = similarity, gap = 0.
            n, m = len(g3), len(g2)
            score = [[0.0] * (m + 1) for _ in range(n + 1)]
            for i in range(1, n + 1):
                for j in range(1, m + 1):
                    score[i][j] = max(score[i - 1][j], score[i][j - 1],
                                      score[i - 1][j - 1] + similar(g3[i - 1], g2[j - 1]))
            i, j = n, m
            while i and j:
                s = similar(g3[i - 1], g2[j - 1])
                if score[i][j] == score[i - 1][j - 1] + s and s >= ALIGN_MIN:
                    new += add(g3[i - 1], g2[j - 1], "layout-align")
                    i, j = i - 1, j - 1
                elif score[i][j] == score[i - 1][j]:
                    i -= 1
                else:
                    j -= 1
        return new

    rounds = 0
    while True:
        rounds += 1
        n = callgraph_pass() + vtable_pass()
        if not n:
            n = layout_pass()
        if not n or rounds > 40:
            break
    print(f"vtables: Skate 2 {len(vt2)} named, TU3 pointer runs {len(runs3)}; {rounds} rounds")
    if truth:
        res = collections.defaultdict(lambda: [0, 0])
        for a3, a2 in truth.items():
            if a3 in pairs and pairs[a3][1] != "exact":
                res[pairs[a3][1]][0] += 1
                res[pairs[a3][1]][1] += pairs[a3][0] == a2
        found = sum(v[0] for v in res.values())
        print(f"holdout: {len(truth)} hidden exact pairs, {found} rediscovered")
        for h, (n, ok) in sorted(res.items()):
            print(f"  {h}: {ok}/{n} correct ({100 * ok / max(n, 1):.0f}%)")
        return
    OUT.mkdir(parents=True, exist_ok=True)
    with open(OUT / "sk2_tu3.tsv", "w") as f:
        for a3 in sorted(pairs):
            a2, how = pairs[a3]
            f.write(f"{a3:08X}\t{a2:08X}\t{how}\t{sk2[a2][1]}\n")
    print(f"TU3 functions {len(tu3)}, Skate 2 named {len(sk2)}")
    # Consistency: the share of each method's pairs that sit on the longest
    # in-order chain (a pair off the chain is suspect, not necessarily wrong:
    # Skate 3 also moved code).
    import bisect
    seq = sorted((a3, a2) for a3, (a2, _) in pairs.items())
    tails, tails_i, prev = [], [], [-1] * len(seq)
    for i, (_, y) in enumerate(seq):
        k = bisect.bisect_left(tails, y)
        if k == len(tails):
            tails.append(y)
            tails_i.append(i)
        else:
            tails[k] = y
            tails_i[k] = i
        prev[i] = tails_i[k - 1] if k else -1
    on_chain, i = set(), tails_i[-1] if tails_i else -1
    while i >= 0:
        on_chain.add(seq[i][0])
        i = prev[i]
    by = collections.defaultdict(lambda: [0, 0])
    for a3, (_, h) in pairs.items():
        by[h][0] += 1
        by[h][1] += a3 in on_chain
    print("in link order: " + ", ".join(f"{h} {c}/{n} ({100 * c // max(n, 1)}%)"
                                         for h, (n, c) in sorted(by.items())))
    how = collections.Counter(h for _, h in pairs.values())
    print(f"matched {len(pairs)} ({100 * len(pairs) / len(tu3):.1f}%): "
          + ", ".join(f"{k} {v}" for k, v in how.most_common()) + f" -> {OUT / 'sk2_tu3.tsv'}")


CONFIDENCE = {"exact": "likely", "strings": "likely", "callgraph": "likely", "layout": "likely",
              "vtable": "guess", "layout-align": "guess"}
NAMES = REPO / "docs/re/names_sk2.tsv"


def export() -> None:
    """The match table as docs/re/names_sk2.tsv: names only (no Skate 2
    addresses or code), one line per TU3 function."""
    names2 = {}
    for line in open(SK2 / "functions.tsv"):
        a, _s, n = line.rstrip("\n").split("\t", 2)
        names2[int(a, 16)] = n
    # TU3 function lengths (words), to distrust exact matches of tiny functions.
    starts = sorted(int(l.split()[0], 16) for l in open(TU3_FUNCS))
    length = {a: (starts[i + 1] - a) // 4 for i, a in enumerate(starts[:-1])}
    best: dict[int, tuple[str, str, str]] = {}  # TU3 -> (name, method, confidence)
    dropped = 0
    for line in open(OUT / "sk2_tu3.tsv"):
        a3, _a2, how, name = line.rstrip("\n").split("\t", 3)
        a = int(a3, 16)
        # layout-align: ~80% right on code unchanged since Skate 2, but 2 of 19
        # against Skate 3's own Lua binding names (RE-3): not exported.
        if how == "layout-align":
            dropped += 1
            continue
        conf = CONFIDENCE.get(how, "guess")
        if how == "exact" and length.get(a, 99) < 6:
            conf = "guess"  # tiny functions (stubs) match unrelated tiny functions
        best[a] = (name, how, conf)
    # Ghidra Auto Version Tracking (tools/re/sk2_vt.py), when it has run: its
    # accepted matches fill gaps and win over everything but our exact and
    # string pairs (it agreed with those 99-100%).
    vt_file = OUT / "vt_matches.tsv"
    vt_added = vt_won = 0
    if vt_file.exists():
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import sk2_vt
        strong = {"Exact Function Bytes Match": "vt-bytes",
                  "Exact Function Instructions Match": "vt-instructions",
                  "Exact Function Mnemonics Match": "vt-mnemonics",
                  "Combined Function and Data Reference Match": "vt-references"}
        weak = {"Duplicate Function Instructions Match": "vt-duplicate",
                "Implied Match": "vt-implied"}
        for a3, (a2, corr, _sim, _st) in sk2_vt.load_vt().items():
            if a2 not in names2:
                continue
            how = strong.get(corr) or weak.get(corr) or "vt"
            row = (names2[a2], how, "likely" if corr in strong else "guess")
            cur = best.get(a3)
            if cur is None:
                best[a3] = row
                vt_added += 1
            elif cur[1] not in ("exact", "strings") and cur[0] != row[0]:
                best[a3] = row
                vt_won += 1
    with open(NAMES, "w") as f:
        f.write("# Skate 3 (TU3) function names transferred from the Skate 2 debug build's PDB\n"
                "# by tools/re/sk2_match.py and Ghidra Auto Version Tracking (tools/re/sk2_vt.py)\n"
                "# (roadmap RE-2; docs/re/README.md). Generated: do not edit; confirmed names go\n"
                "# in symbols.toml, which takes priority.\n"
                "# Precision, measured by hiding 30% of the exact pairs (sk2_match.py match\n"
                "# --holdout 0.3) and by agreement with Version Tracking: exact, strings and\n"
                "# vt-* (exact correlators) ~99-100%; callgraph ~93%, layout ~95%, vtable\n"
                "# ~84%; on functions unchanged since Skate 2, lower on changed ones.\n"
                "# Exact matches of functions under 6 instructions are 'guess' (stubs match\n"
                "# unrelated stubs); layout-align pairs are left out (2 of 19 right against\n"
                "# the game's Lua binding names). Skate 3's own names (names_bindings.tsv)\n"
                "# take priority. Treat every name here as a lead until a probe confirms it.\n"
                "# address\tname\tmethod\tconfidence\n")
        for a3 in sorted(best):
            name, how, conf = best[a3]
            f.write(f"{a3:08X}\t{name}\t{how}\t{conf}\n")
    print(f"{len(best)} names -> {NAMES} (Version Tracking: {vt_added} added, "
          f"{vt_won} replaced a weaker pair; {dropped} layout-align pairs left out)")


def lookup(q: str) -> int:
    hits = 0
    for line in open(OUT / "sk2_tu3.tsv"):
        a3, a2, how, name = line.rstrip("\n").split("\t", 3)
        if q.lower() in name.lower() or q.upper().removeprefix("SUB_").removeprefix("0X") == a3:
            print(f"sub_{a3}  {name}  [{how}, sk2 {a2}]")
            hits += 1
    return 0 if hits else 1


def main() -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("prepare")
    p.add_argument("ref", nargs="?", default=os.environ.get("SKATE3_RE_REF_DIR"))
    p = sub.add_parser("match")
    p.add_argument("--holdout", type=float, default=0.0,
                   help="estimate precision: hide this share of exact pairs")
    sub.add_parser("export")
    p = sub.add_parser("lookup")
    p.add_argument("q")
    a = ap.parse_args()
    if a.cmd == "prepare":
        if not a.ref:
            sys.exit("give REF_DIR or set SKATE3_RE_REF_DIR")
        prepare(Path(a.ref))
    elif a.cmd == "match":
        match(a.holdout)
    elif a.cmd == "export":
        export()
    else:
        return lookup(a.q)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
