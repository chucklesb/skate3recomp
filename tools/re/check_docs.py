#!/usr/bin/env python3
"""Check that code and docs agree (docs/workflow.md, step 8).

    check_docs.py --staged    what a commit adds (the pre-commit hook)
    check_docs.py --all       the whole tree

Errors (block the commit):
  - a native body (REX_FUNC(sub_X) in src/native/) without a
    docs/re/symbols.toml entry or a row in docs/re/replacements.md;
  - with --staged, any new REX_FUNC(sub_X) in src/ without a symbols.toml
    entry (with --all, old hooks outside src/native/ are warnings);
  - an RE item page without its header block (Item, Goal, Done when, Status,
    Next) in its first 30 lines;
  - an RE-n id in docs/ that the hub (docs/re/README.md, "The RE items")
    doesn't define;
  - a relative link in docs/ to a file that doesn't exist;
  - tools/harness/re_symbols.py check problems.
Warnings:
  - staged changes in src/native/ with no staged change in docs/;
  - a new default-on cvar named like a fallback or workaround that
    docs/re/workarounds.md doesn't mention.

SKATE3_SKIP_DOC_CHECK=1 skips the check (an emergency only; the skip is
logged to .git/doc-check-skips.log).
"""
import argparse
import datetime
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DOCS = os.path.join(ROOT, 'docs')
HUB = os.path.join(DOCS, 're', 'README.md')
sys.path.insert(0, os.path.join(ROOT, 'tools', 're'))
import coverage  # noqa: E402  (rex_body_defs: every body of ours in src/)
REX_DEF = re.compile(r'REX_FUNC\((sub_[0-9A-F]{8})\)\s*\{')
HEADER = ('**Item:**', '**Goal:**', '**Done when:**', '**Status:**', '**Next:**')
# Item pages and subsystem pages that must open with the header block.
HEADER_PAGES_EXTRA = ('culling.md', 'jobs.md')


def git(*args):
    return subprocess.run(['git', *args], cwd=ROOT, capture_output=True, text=True).stdout


def read(p):
    with open(p, encoding='utf-8', errors='replace') as f:
        return f.read()


class Report:
    def __init__(self):
        self.errors, self.warnings = [], []

    def error(self, msg):
        self.errors.append(msg)

    def warn(self, msg):
        self.warnings.append(msg)


def symbols_keys():
    text = read(os.path.join(DOCS, 're', 'symbols.toml'))
    return set(re.findall(r'^\[(sub_[0-9A-F]{8})\]', text, re.M))


def hub_items():
    text = read(HUB)
    return set(int(n) for n in re.findall(r'^\| RE-(\d+) \|', text, re.M))


def hub_pages():
    text = read(HUB)
    a = text.index('## The RE items')
    rows = text[a:].split('\n\n', 2)[1]
    pages = set()
    for m in re.finditer(r'^\| RE-\d+ \|.*\]\(([^)#]+)(?:#[^)]*)?\) \|$', rows, re.M):
        p = m.group(1)
        if not p.startswith('..') and p != 'README.md':
            pages.add(p)
    return pages | set(HEADER_PAGES_EXTRA)


def check_native(rep, staged_added, staged_files=None):
    """staged_files: with --staged, old hooks outside src/native/ are only
    reported for the files the commit touches (None: --all, report all)."""
    syms = symbols_keys()
    catalog = read(os.path.join(DOCS, 're', 'replacements.md'))
    for rel, name in sorted(set(coverage.rex_body_defs())):
        native = coverage.is_native_path(rel)
        new = (rel, name) in staged_added
        if name not in syms:
            msg = f'{rel}: {name} has a body here but no docs/re/symbols.toml entry'
            if native or new:
                rep.error(msg)
            elif staged_files is None or rel in staged_files:
                rep.warn(msg)
        if native and name not in catalog:
            rep.error(f'{rel}: native {name} has no row in docs/re/replacements.md '
                      '(write its full sub_ address in the row)')


def check_headers(rep):
    for page in sorted(hub_pages()):
        p = os.path.join(DOCS, 're', page)
        if not os.path.exists(p):
            rep.error(f'docs/re/README.md lists {page}, which does not exist')
            continue
        head = '\n'.join(read(p).split('\n')[:30])
        missing = [h for h in HEADER if h not in head]
        if missing:
            rep.error(f'docs/re/{page}: no header block in its first 30 lines (missing {", ".join(missing)})')


def md_files():
    for dirpath, _d, files in os.walk(DOCS):
        if os.sep + 'internal' in dirpath:
            continue
        for fn in files:
            if fn.endswith('.md'):
                yield os.path.join(dirpath, fn)


def check_ids_and_links(rep):
    items = hub_items()
    link = re.compile(r'\]\(([^)\s]+)\)')
    for p in md_files():
        rel = os.path.relpath(p, ROOT)
        text = read(p)
        for n in sorted(set(int(x) for x in re.findall(r'\bRE-(\d+)', text))):
            if n not in items:
                rep.error(f'{rel}: RE-{n} is not defined in docs/re/README.md ("The RE items")')
        for t in link.findall(text):
            if t.startswith(('http:', 'https:', 'mailto:', '#')):
                continue
            target = t.split('#')[0]
            if not target:
                continue
            full = os.path.normpath(os.path.join(os.path.dirname(p), target))
            if not os.path.exists(full):
                rep.error(f'{rel}: broken link to {t}')


def check_symbols_tool(rep):
    r = subprocess.run([sys.executable, '-I', os.path.join(ROOT, 'tools', 'harness', 're_symbols.py'), 'check'],
                       cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0 or ' 0 problem' not in r.stdout:
        rep.error('re_symbols.py check: ' + (r.stdout + r.stderr).strip().replace('\n', '; ')[:400])


def staged_info():
    names = git('diff', '--cached', '--name-only').split()
    diff = git('diff', '--cached', '-U0')
    added, cvars, cur = set(), [], None
    for line in diff.split('\n'):
        if line.startswith('+++ b/'):
            cur = line[6:]
        elif line.startswith('+') and not line.startswith('+++') and cur:
            for name in REX_DEF.findall(line):
                added.add((cur, name))
            m = re.search(r'REXCVAR_DEFINE_BOOL\((\w+),\s*true', line)
            if m:
                cvars.append((cur, m.group(1)))
    return names, added, cvars


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument('--staged', action='store_true')
    g.add_argument('--all', action='store_true')
    a = ap.parse_args()

    if os.environ.get('SKATE3_SKIP_DOC_CHECK') == '1':
        log = os.path.join(ROOT, '.git', 'doc-check-skips.log')
        with open(log, 'a', encoding='utf-8') as f:
            f.write(f'{datetime.datetime.now().isoformat(timespec="seconds")} skipped; staged: '
                    f'{" ".join(git("diff", "--cached", "--name-only").split())}\n')
        print('check_docs: skipped (SKATE3_SKIP_DOC_CHECK=1), logged to .git/doc-check-skips.log')
        return 0

    rep = Report()
    added = set()
    staged_files = None
    if a.staged:
        names, added, cvars = staged_info()
        staged_files = set(names)
        if any(n.startswith('src/native/') for n in names) and not any(n.startswith('docs/') for n in names):
            rep.warn('src/native/ changed but nothing in docs/ did: symbols.toml, the catalog, '
                     'the subsystem page, the RE item status, coverage.md?')
        work = read(os.path.join(DOCS, 're', 'workarounds.md'))
        for f, cv in cvars:
            if re.search(r'fallback|workaround|rescue|guess', cv) and cv not in work:
                rep.warn(f'{f}: new default-on cvar {cv} looks like a fallback; fallbacks ship '
                         'default-off and are listed in docs/re/workarounds.md')
    check_native(rep, added, staged_files)
    check_headers(rep)
    check_ids_and_links(rep)
    check_symbols_tool(rep)

    for w in rep.warnings:
        print('warning:', w)
    for e in rep.errors:
        print('error:', e)
    if rep.errors:
        print(f'check_docs: {len(rep.errors)} error(s); fix them (docs/workflow.md) or, in an '
              'emergency, SKATE3_SKIP_DOC_CHECK=1')
        return 1
    print(f'check_docs: ok ({len(rep.warnings)} warning(s))')
    return 0


if __name__ == '__main__':
    sys.exit(main())
