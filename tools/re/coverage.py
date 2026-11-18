#!/usr/bin/env python3
"""How much of the game is named, specified and native (docs/plan.md).

    coverage.py [--counts RUN|call_counts.tsv] [--write]
        the per-subsystem table; --write regenerates docs/re/coverage.md
    coverage.py --subsystem NAME [--counts ...] [--top N]
        one subsystem's functions that aren't native yet, most called first
        (workflow.md step 6: what's left in the code a fix touched)

Counts per subsystem (docs/re/subsystems.toml):
  functions   recompiled functions in the main image (generated/)
  named       with any name: the hand map, a Skate 3 binding, Skate 2
  specified   with a hand-map entry (docs/re/symbols.toml)
  native      replaced by our own code: a REX_FUNC body in src/native/
  hooked      any REX_FUNC body in src/ (native or a hook that observes)
With --counts (a call-count run, scenarios/callcount_plaza.toml), each
subsystem's share of calls and the native share of its calls.

Reads generated/ (local) and the TU3 image's function list; writes only
names and counts, never code.
"""
import argparse
import os
import re
import sys
import tomllib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'harness'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 're'))
import re_symbols  # noqa: E402
import leaves  # noqa: E402

SUBSYSTEMS = os.path.join(ROOT, 'docs', 're', 'subsystems.toml')
OUT = os.path.join(ROOT, 'docs', 're', 'coverage.md')
REX_DEF = re.compile(r'REX_FUNC\((sub_[0-9A-F]{8})\)\s*\{')


def rex_bodies():
    """(hooked, native): functions with a REX_FUNC body in src/, and those
    in src/native/ (our replacements)."""
    hooked, native = set(), set()
    for dirpath, _dirs, files in os.walk(os.path.join(ROOT, 'src')):
        for fn in files:
            if not fn.endswith(('.cpp', '.h')):
                continue
            p = os.path.join(dirpath, fn)
            text = open(p, encoding='utf-8', errors='replace').read()
            found = {int(m[4:], 16) for m in REX_DEF.findall(text)}
            hooked |= found
            if os.sep + 'native' + os.sep in p:
                native |= found
    return hooked, native


def load_counts(arg):
    if not arg:
        return None
    path = arg if arg.endswith('.tsv') else os.path.join(arg, 'call_counts.tsv')
    out = {}
    for line in open(path, encoding='utf-8'):
        if line.startswith('#') or not line.strip():
            continue
        name, n = line.split('\t')
        if name.startswith('sub_'):
            out[int(name[4:], 16)] = int(n)
    return out


def names():
    hand = re_symbols.load()
    bind = re_symbols.load_bindings()
    sk2 = re_symbols.load_sk2()
    out = {}
    for key, (name, _how, _conf) in sk2.items():
        out[int(key[4:], 16)] = name
    for key, n in bind.items():
        if '|' not in n:  # a folded function's many names say nothing
            out[int(key[4:], 16)] = n
    for key, e in hand.items():
        if key.startswith('sub_') and 'name' in e:
            out[int(key[4:], 16)] = e['name']
    specified = {int(k[4:], 16) for k in hand if k.startswith('sub_')}
    return out, specified


def assign(funcs, named, subs):
    """function -> (subsystem, how): how is 'name' or 'neighbours'."""
    rules = [(s, [re.compile(r) for r in cfg.get('names', [])]) for s, cfg in subs.items()]
    out = {}
    for f in funcs:
        n = named.get(f)
        if not n:
            continue
        for s, rx in rules:
            if any(r.search(n) for r in rx):
                out[f] = (s, 'name')
                break
    order = sorted(funcs)
    prev = [None] * len(order)
    last = None
    for i, f in enumerate(order):
        prev[i] = last
        if f in out:
            last = out[f][0]
    nxt = None
    for i in range(len(order) - 1, -1, -1):
        f = order[i]
        if f not in out and prev[i] is not None and prev[i] == nxt:
            out[f] = (nxt, 'neighbours')
        if f in out and out[f][1] == 'name':
            nxt = out[f][0]
    return out


def table(funcs, named, specified, hooked, native, where, subs, counts):
    rows = {}
    for f in funcs:
        s = where.get(f, ('unassigned', ''))[0]
        r = rows.setdefault(s, dict(functions=0, inferred=0, named=0, specified=0, native=0,
                                    hooked=0, calls=0, native_calls=0))
        r['functions'] += 1
        r['inferred'] += where.get(f, ('', ''))[1] == 'neighbours'
        r['named'] += f in named
        r['specified'] += f in specified
        r['native'] += f in native
        r['hooked'] += f in hooked
        if counts:
            c = counts.get(f, 0)
            r['calls'] += c
            r['native_calls'] += c if f in native else 0
    return rows


def render(rows, subs, counts, totals_calls, src_note):
    order = list(subs) + ['unassigned']
    head = '| Subsystem | Functions | (inferred) | Named | Specified | Native | Hooked |'
    sep = '|---|---:|---:|---:|---:|---:|---:|'
    if counts:
        head += ' Calls | Native calls |'
        sep += '---:|---:|'
    lines = [head, sep]

    def pct(a, b):
        return f'{a:,} ({100 * a / b:.1f}%)' if b else '0'

    tot = dict(functions=0, inferred=0, named=0, specified=0, native=0, hooked=0, calls=0, native_calls=0)
    for s in order:
        r = rows.get(s)
        if not r:
            continue
        for k in tot:
            tot[k] += r[k]
        title = subs[s]['title'] if s in subs else 'Not yet assigned (P1)'
        page = subs.get(s, {}).get('page')
        label = f'[{s}]({page})' if page else s
        line = (f'| {label}: {title} | {r["functions"]:,} | {r["inferred"]:,} | {pct(r["named"], r["functions"])} '
                f'| {r["specified"]:,} | {r["native"]:,} | {r["hooked"]:,} |')
        if counts:
            line += f' {100 * r["calls"] / totals_calls:.1f}% | {pct(r["native_calls"], r["calls"])} |'
        lines.append(line)
    line = (f'| **All** | **{tot["functions"]:,}** | {tot["inferred"]:,} | {pct(tot["named"], tot["functions"])} '
            f'| {tot["specified"]:,} | {pct(tot["native"], tot["functions"])} | {tot["hooked"]:,} |')
    if counts:
        line += f' 100% | {pct(tot["native_calls"], tot["calls"])} |'
    lines.append(line)
    return '\n'.join(lines), tot


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--counts', help='a call-count run folder or call_counts.tsv')
    ap.add_argument('--write', action='store_true', help='regenerate docs/re/coverage.md')
    ap.add_argument('--subsystem')
    ap.add_argument('--top', type=int, default=40)
    a = ap.parse_args()

    with open(SUBSYSTEMS, 'rb') as f:
        subs = tomllib.load(f)
    funcs = leaves.all_functions()
    named, specified = names()
    named = {f: n for f, n in named.items() if f in funcs}
    specified &= funcs
    hooked, native = rex_bodies()
    hooked &= funcs
    native &= funcs
    where = assign(funcs, named, subs)
    counts = load_counts(a.counts)

    if a.subsystem:
        mine = [f for f in funcs if where.get(f, ('unassigned', ''))[0] == a.subsystem]
        left = [f for f in mine if f not in native]
        key = (lambda f: -(counts or {}).get(f, 0)) if counts else (lambda f: f)
        print(f'{a.subsystem}: {len(mine):,} functions, {len(mine) - len(left):,} native, '
              f'{len(left):,} left; {"most called" if counts else "by address"} first:')
        for f in sorted(left, key=key)[:a.top]:
            c = f'{counts.get(f, 0):>12,}  ' if counts else ''
            how = where.get(f, ('', ''))[1]
            spec = 'spec ' if f in specified else '     '
            print(f'  {c}sub_{f:08X}  {spec}{named.get(f, "(unnamed)")[:90]}{"  [inferred]" if how == "neighbours" else ""}')
        return 0

    rows = table(funcs, named, specified, hooked, native, where, subs, counts)
    totals_calls = sum(r['calls'] for r in rows.values()) or 1
    text, tot = render(rows, subs, counts, totals_calls, a.counts)
    if not a.write:
        print(text)
        return 0
    src = (f'Call counts: `{os.path.relpath(a.counts, ROOT)}` (call-weighted columns: the subsystem\'s share '
           'of all calls, and how many of its calls reach native code).') if counts else 'No call counts given.'
    with open(OUT, 'w', encoding='utf-8') as f:
        f.write(f'''# Coverage

How much of the game is named, specified and native, per subsystem
([plan.md](../plan.md): P1 drives "Not yet assigned" to 0, P2-P3 drive
"Native" to all). **Generated** by `tools/re/coverage.py --write
[--counts RUN]`; regenerate it with each replacement (the workflow's step 7,
[workflow.md](../workflow.md)). Subsystems are defined in
[subsystems.toml](subsystems.toml).

- **Functions:** recompiled functions in the main image. **(inferred):**
  assigned by their named neighbours, not by a name of their own.
- **Named:** any name (hand map, Skate 3 binding, Skate 2 transfer; the
  Skate 2 names are leads, see [README.md](README.md)). **Specified:** a
  [symbols.toml](symbols.toml) entry.
- **Native:** our own implementation runs (a body in `src/native/`).
  **Hooked:** any body of ours, including hooks that only observe.

{src}

{text}
''')
    print(f'wrote {os.path.relpath(OUT, ROOT)}: {tot["functions"]:,} functions, {tot["named"]:,} named, '
          f'{tot["native"]:,} native')
    return 0


if __name__ == '__main__':
    sys.exit(main())
