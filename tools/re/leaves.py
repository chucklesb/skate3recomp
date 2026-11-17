#!/usr/bin/env python3
"""Find small leaf functions to reverse engineer, by class (RE-7 / RE-9).

About 5,600 functions are one to eight simple instructions with no branches:
empty overrides, constant returns, field getters and setters, this-adjust
thunks. They are where a class's field layout comes from, so they are the
first methods to name when working through a class. This is a FINDER: it
describes what each one does in words, it never generates code. Replacements
are written by hand over a named struct (docs/re/replacements.md, "The
standard").

    leaves.py vtable ADDR [--slots N]   a vtable's slots: name, and for leaves
                                         what they do ("returns u32 this+280")
    leaves.py lookup ADDR               one function, and the data words that
                                         point at it (vtable slots)
    leaves.py list [--kind K]           every leaf (kinds: empty const gaddr
                                         addthis load store jump thunk seq)
    leaves.py rank RUN [--top N]        the hottest guest functions of a
                                         call-count run (call_counts.tsv from a
                                         SKATE3_COUNT_GUEST_CALLS build,
                                         scenarios/callcount_plaza.toml), then
                                         their classes: calls summed per vtable

Reads the recompiled bodies in generated/ (local; nothing here is committed)
and the TU3 image out/re/ghidra/skate3_tu3.bin.
"""
import argparse
import glob
import json
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'harness'))
import re_symbols  # noqa: E402

GEN = os.path.join(ROOT, 'generated')
IMAGE = os.path.join(ROOT, 'out', 're', 'ghidra', 'skate3_tu3.bin')
IMAGE_BASE = 0x82000000
CACHE = os.path.join(GEN, 'leaves.scan.json')
SCAN_VERSION = 3

FUNC_RE = re.compile(r'DEFINE_REX_FUNC\((sub_[0-9A-F]{8})\) \{\n(.*?)\n\}\n', re.S)
SKIP_LINES = {'REX_FUNC_PROLOGUE();', 'PPCRegister temp{};', 'uint32_t ea{};'}
R = r'ctx\.r(\d+)'
I = r'(-?\d+)'
SUB = r'(sub_[0-9A-F]{8})'
MAX_OPS = 8

# One straight-line statement -> a description fragment.
OPS = [
    (re.compile(R + r'\.s64 = ' + I + r';\Z'), lambda g: f'r{g[0]} = {int(g[1]) & 0xFFFFFFFF:#x}'),
    (re.compile(R + r'\.u64 = ' + R + r'\.u64;\Z'), lambda g: f'r{g[0]} = r{g[1]}'),
    (re.compile(R + r'\.s64 = ' + R + r'\.s64 \+ ' + I + r';\Z'), lambda g: f'r{g[0]} = r{g[1]} + {g[2]}'),
    (re.compile(R + r'\.u64 = ' + R + r'\.u64 \| (\d+);\Z'), lambda g: f'r{g[0]} = r{g[1]} | {int(g[2]):#x}'),
    (re.compile(R + r'\.u64 = ' + R + r'\.u64 & (0x[0-9A-F]+);\Z'), lambda g: f'r{g[0]} = r{g[1]} & {g[2]}'),
    (re.compile(R + r'\.u64 = REX_LOAD_U(8|16|32|64)\(' + R + r'\.u32 \+ ' + I + r'\);\Z'),
     lambda g: f'r{g[0]} = u{g[1]}[r{g[2]}+{g[3]}]'),
    (re.compile(r'REX_STORE_U(8|16|32|64)\(' + R + r'\.u32 \+ ' + I + r', ' + R + r'\.u(?:8|16|32|64)\);\Z'),
     lambda g: f'u{g[0]}[r{g[1]}+{g[2]}] = r{g[3]}'),
    (re.compile(R + r'\.u64 = __builtin_rotateleft64\(' + R + r'\.u32 \| \(' + R
                + r'\.u64 << 32\), (\d+)\) & (0x[0-9A-F]+);\Z'),
     lambda g: f'r{g[0]} = rotl(r{g[1]}, {g[3]}) & {g[4]}'),
    (re.compile(R + r'\.u64 = ' + R + r'\.u32 == 0 \? 32 : __builtin_clz\(' + R + r'\.u32\);\Z'),
     lambda g: f'r{g[0]} = clz(r{g[1]})'),
    (re.compile(r'ctx\.fpscr\.disableFlushModeUnconditional\(\);\Z'), lambda g: None),
    (re.compile(r'temp\.u32 = REX_LOAD_U32\(' + R + r'\.u32 \+ ' + I + r'\);\Z'),
     lambda g: f'tmp = f32[r{g[0]}+{g[1]}]'),
    (re.compile(r'ctx\.f(\d+)\.f64 = double\(temp\.f32\);\Z'), lambda g: f'f{g[0]} = tmp'),
    (re.compile(r'temp\.f32 = float\(ctx\.f(\d+)\.f64\);\Z'), lambda g: f'tmp = f{g[0]}'),
    (re.compile(r'REX_STORE_U32\(' + R + r'\.u32 \+ ' + I + r', temp\.u32\);\Z'),
     lambda g: f'f32[r{g[0]}+{g[1]}] = tmp'),
]
CALL = re.compile(SUB + r'\(ctx, base\);\Z')


def statements(body):
    out = [l.strip() for l in body.split('\n')]
    out = [l for l in out if l and not l.startswith('//') and l not in SKIP_LINES]
    if not out or out[-1] != 'return;':
        return None
    return out[:-1]


def describe(stmts):
    """(kind, text) for a branch-free leaf, or None."""
    if len(stmts) > MAX_OPS:
        return None
    parts, tail = [], None
    for i, s in enumerate(stmts):
        m = CALL.match(s)
        if m:
            if i != len(stmts) - 1:
                return None
            tail = m.group(1)
            continue
        for p, f in OPS:
            m = p.match(s)
            if m:
                t = f(m.groups())
                if t:
                    parts.append(t)
                break
        else:
            return None
    text = '; '.join(parts)
    # A kind for grouping, from the common cases.
    if not parts and not tail:
        kind, text = 'empty', 'returns (empty)'
    elif tail and not parts:
        kind, text = 'jump', f'tail call {tail}'
    elif tail:
        kind = 'thunk' if re.fullmatch(r'r3 = r3 \+ -?\d+', text) else 'seq'
        text = f'{text}; tail call {tail}'
    elif re.fullmatch(r'r3 = 0x[0-9a-f]+( ; r3 = r3 \| 0x[0-9a-f]+)?'.replace(' ;', ';'), text):
        kind = 'const'
    elif re.fullmatch(r'r11 = 0x[0-9a-f]+; r3 = r11 \+ -?\d+', text):
        kind = 'gaddr'
    elif re.fullmatch(r'r3 = r3 \+ -?\d+', text):
        kind = 'addthis'
    elif re.fullmatch(r'r3 = u\d+\[r3\+-?\d+\]|tmp = f32\[r3\+-?\d+\]; f1 = tmp', text):
        kind = 'load'
    elif re.fullmatch(r'(r11 = 0x[0-9a-f]+; )?u\d+\[r3\+-?\d+\] = r(4|5|11)|tmp = f1; f32\[r3\+-?\d+\] = tmp', text):
        kind = 'store'
    else:
        kind = 'seq'
    return kind, text


def scan():
    paths = sorted(glob.glob(os.path.join(GEN, 'skate3_recomp.*.cpp')))
    key = [SCAN_VERSION] + [[os.path.basename(p), os.path.getsize(p), int(os.path.getmtime(p))] for p in paths]
    try:
        with open(CACHE, encoding='utf-8') as f:
            c = json.load(f)
        if c['key'] == key:
            return {n: tuple(v) for n, v in c['found'].items()}
    except (OSError, ValueError, KeyError):
        pass
    found = {}
    for path in paths:
        with open(path, encoding='utf-8', errors='replace') as f:
            text = f.read()
        for m in FUNC_RE.finditer(text):
            st = statements(m.group(2))
            if st is None:
                continue
            d = describe(st)
            if d:
                found[m.group(1)] = d
    try:
        with open(CACHE, 'w', encoding='utf-8') as f:
            json.dump({'key': key, 'found': found}, f)
    except OSError:
        pass
    return found


def name_of(addr, maps):
    hand, bind, sk2 = maps
    key = f'sub_{addr:08X}'
    if key in hand:
        return hand[key].get('name', '?') + ' [map]'
    if key in bind:
        return bind[key] + ' [binding]'
    if key in sk2:
        return f'{sk2[key][0]} [sk2?]'
    return ''


def load_maps():
    try:
        return (re_symbols.load(), re_symbols.load_bindings(), re_symbols.load_sk2())
    except Exception:  # the maps are optional here
        return ({}, {}, {})


FUNCS_CACHE = os.path.join(GEN, 'leaves.funcs.json')
# A function in more vtables than this is shared code (the binary's one empty
# function, return 0, ...): it says nothing about a class.
SHARED_TABLES = 3


def all_functions():
    """Every recompiled function's address (cached like the scan)."""
    paths = sorted(glob.glob(os.path.join(GEN, 'skate3_recomp.*.cpp')))
    key = [[os.path.basename(p), os.path.getsize(p), int(os.path.getmtime(p))] for p in paths]
    try:
        with open(FUNCS_CACHE, encoding='utf-8') as f:
            c = json.load(f)
        if c['key'] == key:
            return set(c['funcs'])
    except (OSError, ValueError, KeyError):
        pass
    funcs = set()
    for path in paths:
        with open(path, encoding='utf-8', errors='replace') as f:
            funcs.update(int(m, 16) for m in re.findall(r'DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)', f.read()))
    try:
        with open(FUNCS_CACHE, 'w', encoding='utf-8') as f:
            json.dump({'key': key, 'funcs': sorted(funcs)}, f)
    except OSError:
        pass
    return funcs


def vtable_index(data, funcs):
    """function -> the vtables (start addresses) holding it. A vtable is a
    run of 2+ aligned words that are all function starts."""
    words = struct.unpack(f'>{len(data) // 4}I', data[:len(data) // 4 * 4])
    owners = {}
    i, n = 0, len(words)
    while i < n:
        if words[i] not in funcs:
            i += 1
            continue
        j = i
        while j < n and words[j] in funcs:
            j += 1
        if j - i >= 2:
            start = IMAGE_BASE + 4 * i
            for k in range(i, j):
                owners.setdefault(words[k], []).append(start)
        i = j
    return owners


def rank(a, found, maps):
    run = a.addr
    path = run if run.endswith('.tsv') else os.path.join(run, 'call_counts.tsv')
    counts = []
    with open(path, encoding='utf-8') as f:
        for line in f:
            if line.startswith('#') or not line.strip():
                continue
            name, n = line.split('\t')
            # Non-sub_ names are the register save/restore helpers.
            counts.append((int(n), norm(name) if name.startswith('sub_') else name))
    counts.sort(key=lambda c: -c[0])
    total = sum(n for n, _ in counts) or 1
    data = open(IMAGE, 'rb').read()
    owners = vtable_index(data, all_functions())

    def tables(fn):
        t = sorted(set(owners.get(fn, [])))
        if len(t) > SHARED_TABLES:
            return f'shared ({len(t)} tables)'
        return ' '.join(f'{v:08X}' for v in t) or '-'

    print(f'{len(counts)} functions called, {total:,} calls ({path})')
    helpers = sum(n for n, fn in counts if isinstance(fn, str))
    print(f'register save/restore helpers: {helpers:,} calls ({100 * helpers / total:.1f}%; '
          f'one pair per non-leaf call, listed with --helpers)\n')
    if not a.helpers:
        counts = [c for c in counts if not isinstance(c[1], str)]
    print(f'{"calls":>12} {"%":>5}  function      {"vtables":24} what')
    for n, fn in counts[:a.top]:
        if isinstance(fn, str):
            print(f'{n:>12,} {100 * n / total:5.2f}  {fn:12}  {"-":24} (register save/restore helper)')
            continue
        k, t = found.get(f'sub_{fn:08X}', ('', ''))
        what = name_of(fn, maps) or ''
        if k:
            what = f'{what}  [{k}: {t}]' if what else f'[{k}: {t}]'
        print(f'{n:>12,} {100 * n / total:5.2f}  sub_{fn:08X}  {tables(fn):24.24} {what[:110]}')

    # Classes: each vtable's calls over the methods it doesn't share widely.
    per = {}
    for n, fn in counts:
        if isinstance(fn, str):
            continue
        t = sorted(set(owners.get(fn, [])))
        if not t or len(t) > SHARED_TABLES:
            continue
        for v in t:
            e = per.setdefault(v, [0, []])
            e[0] += n
            e[1].append((n, fn))
    print(f'\n{"calls":>12} {"%":>5}  vtable    methods called  hottest')
    for v, (n, fns) in sorted(per.items(), key=lambda kv: -kv[1][0])[:a.top]:
        hot = ', '.join(f'sub_{fn:08X} {c:,}' for c, fn in sorted(fns, reverse=True)[:3])
        print(f'{n:>12,} {100 * n / total:5.2f}  {v:08X}  {len(fns):>14}  {hot}')
    return 0


def norm(a):
    return int(a.upper().removeprefix('SUB_').removeprefix('0X'), 16)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('cmd', choices=['vtable', 'lookup', 'list', 'rank'])
    ap.add_argument('addr', nargs='?')
    ap.add_argument('--slots', type=int, default=32)
    ap.add_argument('--kind', default=None)
    ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--helpers', action='store_true', help='rank: list the save/restore helpers too')
    a = ap.parse_args()
    found = scan()

    if a.cmd == 'list':
        counts = {}
        for n in sorted(found):
            k, t = found[n]
            counts[k] = counts.get(k, 0) + 1
            if a.kind is None or k == a.kind:
                print(f'{n}  {k:8} {t}')
        print('#', len(found), 'leaves:', ' '.join(f'{k}={v}' for k, v in sorted(counts.items())), file=sys.stderr)
        return 0

    maps = load_maps()
    if a.cmd == 'rank':
        return rank(a, found, maps)
    data = open(IMAGE, 'rb').read()
    if a.cmd == 'lookup':
        addr = norm(a.addr)
        n = f'sub_{addr:08X}'
        k, t = found.get(n, ('-', 'not a leaf (has branches or other ops)'))
        print(f'{n}  {name_of(addr, maps) or "(unnamed)"}\n  {k}: {t}')
        pat = struct.pack('>I', addr)
        refs, i = [], data.find(pat)
        while i >= 0 and len(refs) < 40:
            if i % 4 == 0:
                refs.append(IMAGE_BASE + i)
            i = data.find(pat, i + 1)
        print('  data words pointing at it:', ' '.join(f'{r:08X}' for r in refs) or 'none')
        return 0

    vt = norm(a.addr)
    for k in range(a.slots):
        o = vt + 4 * k - IMAGE_BASE
        if not 0 <= o <= len(data) - 4:
            break
        fn = struct.unpack_from('>I', data, o)[0]
        if not 0x82000000 <= fn < 0x83000000:
            print(f'+{4 * k:<4} {fn:08X}  (not code: end of vtable?)')
            break
        n = f'sub_{fn:08X}'
        kind, text = found.get(n, ('', ''))
        print(f'+{4 * k:<4} {n}  {name_of(fn, maps):40.40} {kind + ": " + text if kind else ""}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
