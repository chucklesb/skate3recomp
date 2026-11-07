#!/usr/bin/env python3
"""Add Ubuntu packages to a cross-compilation sysroot without root or qemu.

The sysroot starts as an extracted ubuntu-base tarball (see make-sysroot.sh).
This script resolves the requested packages' Depends/Pre-Depends against the
archive indices, downloads the .debs and unpacks their file payloads into the
sysroot. Maintainer scripts are never run; a link-only sysroot does not need
them.

Trust chain: the base tarball is pinned by SHA256 and provides Ubuntu's
archive keyring. Each suite's InRelease is checked with gpgv against that
keyring, the Packages indices against the InRelease SHA256 list, and each .deb
against its Packages SHA256.

Subcommands:
  install   resolve, download, verify and unpack packages
  fix-links rewrite absolute symlinks inside the sysroot as relative ones
"""

import argparse
import hashlib
import lzma
import os
import re
import subprocess
import sys
import tempfile
import urllib.request


# --------------------------------------------------------------------------
# Debian version comparison (dpkg's algorithm)
# --------------------------------------------------------------------------

def _order(c):
    if c == "~":
        return -1
    if c.isdigit():
        return 0
    if c.isalpha():
        return ord(c)
    return ord(c) + 256


def _cmp_fragment(a, b):
    i = j = 0
    while i < len(a) or j < len(b):
        first_diff = 0
        while (i < len(a) and not a[i].isdigit()) or (j < len(b) and not b[j].isdigit()):
            ac = _order(a[i]) if i < len(a) else 0
            bc = _order(b[j]) if j < len(b) else 0
            if ac != bc:
                return ac - bc
            i += 1
            j += 1
        while i < len(a) and a[i] == "0":
            i += 1
        while j < len(b) and b[j] == "0":
            j += 1
        while i < len(a) and a[i].isdigit() and j < len(b) and b[j].isdigit():
            if not first_diff:
                first_diff = ord(a[i]) - ord(b[j])
            i += 1
            j += 1
        if i < len(a) and a[i].isdigit():
            return 1
        if j < len(b) and b[j].isdigit():
            return -1
        if first_diff:
            return first_diff
    return 0


def _split_version(v):
    epoch = 0
    if ":" in v:
        e, v = v.split(":", 1)
        epoch = int(e)
    if "-" in v:
        upstream, revision = v.rsplit("-", 1)
    else:
        upstream, revision = v, ""
    return epoch, upstream, revision


def version_compare(a, b):
    ea, ua, ra = _split_version(a)
    eb, ub, rb = _split_version(b)
    if ea != eb:
        return ea - eb
    r = _cmp_fragment(ua, ub)
    if r:
        return r
    return _cmp_fragment(ra, rb)


def version_satisfies(version, op, wanted):
    c = version_compare(version, wanted)
    return {
        "<<": c < 0, "<=": c <= 0, "=": c == 0, ">=": c >= 0, ">>": c > 0,
        "<": c <= 0, ">": c >= 0,
    }[op]


# --------------------------------------------------------------------------
# Control-file parsing
# --------------------------------------------------------------------------

def parse_stanzas(text):
    stanza = {}
    key = None
    for line in text.splitlines():
        if not line.strip():
            if stanza:
                yield stanza
            stanza, key = {}, None
            continue
        if line[0] in " \t":
            if key:
                stanza[key] += "\n" + line.strip()
            continue
        key, _, value = line.partition(":")
        stanza[key] = value.strip()
    if stanza:
        yield stanza


_DEP_RE = re.compile(r"^([a-z0-9][a-z0-9+.\-]*)(?::[a-z0-9]+)?\s*(?:\(\s*([<>=]+)\s*([^)\s]+)\s*\))?")


def parse_dep_field(value):
    """Return a list of groups; each group is a list of (name, op, version)."""
    groups = []
    if not value:
        return groups
    for group in value.split(","):
        alts = []
        for alt in group.split("|"):
            alt = re.sub(r"\[[^\]]*\]|<[^>]*>", "", alt).strip()
            m = _DEP_RE.match(alt)
            if m:
                alts.append((m.group(1), m.group(2), m.group(3)))
        if alts:
            groups.append(alts)
    return groups


# --------------------------------------------------------------------------
# Archive access
# --------------------------------------------------------------------------

def fetch(url, dest):
    tmp = dest + ".part"
    with urllib.request.urlopen(url, timeout=120) as r, open(tmp, "wb") as f:
        while True:
            chunk = r.read(1 << 20)
            if not chunk:
                break
            f.write(chunk)
    os.replace(tmp, dest)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def verified_inrelease(mirror, suite, keyring, cache):
    raw = os.path.join(cache, f"{suite}_InRelease")
    fetch(f"{mirror}/dists/{suite}/InRelease", raw)
    out = raw + ".verified"
    subprocess.run(["gpgv", "--quiet", "--keyring", keyring, "--output", out, raw],
                   check=True, stdout=subprocess.DEVNULL)
    with open(out, encoding="utf-8") as f:
        release = next(parse_stanzas(f.read()))
    hashes = {}
    for line in release.get("SHA256", "").splitlines():
        parts = line.split()
        if len(parts) == 3:
            hashes[parts[2]] = (parts[0], int(parts[1]))
    return hashes


def load_index(mirror, suite, component, arch, hashes, cache):
    rel = f"{component}/binary-{arch}/Packages.xz"
    if rel not in hashes:
        return ""
    want_sha, want_size = hashes[rel]
    path = os.path.join(cache, f"{suite}_{component}_{arch}_Packages.xz")
    if not (os.path.exists(path) and sha256_file(path) == want_sha):
        fetch(f"{mirror}/dists/{suite}/{rel}", path)
    if os.path.getsize(path) != want_size or sha256_file(path) != want_sha:
        raise SystemExit(f"sysroot: hash mismatch for {suite}/{rel}")
    with lzma.open(path, "rt", encoding="utf-8") as f:
        return f.read()


class Archive:
    def __init__(self):
        self.packages = {}   # name -> best stanza
        self.provides = {}   # virtual name -> set(real names)

    def add(self, text, arch):
        for s in parse_stanzas(text):
            if s.get("Architecture") not in (arch, "all"):
                continue
            name = s["Package"]
            cur = self.packages.get(name)
            if cur is None or version_compare(s["Version"], cur["Version"]) > 0:
                self.packages[name] = s
        self.provides.clear()
        for name, s in self.packages.items():
            for group in parse_dep_field(s.get("Provides", "")):
                for pname, _, _ in group:
                    self.provides.setdefault(pname, set()).add(name)


def read_status(root):
    installed, provided = {}, set()
    path = os.path.join(root, "var/lib/dpkg/status")
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            for s in parse_stanzas(f.read()):
                if "installed" not in s.get("Status", ""):
                    continue
                installed[s["Package"]] = s["Version"]
                for group in parse_dep_field(s.get("Provides", "")):
                    for pname, _, _ in group:
                        provided.add(pname)
    return installed, provided


def resolve(archive, requested, installed, provided):
    selected = {}  # name -> stanza

    def version_of(name):
        if name in selected:
            return selected[name]["Version"]
        return installed.get(name)

    def satisfied(name, op, ver):
        have = version_of(name)
        if have is not None and (op is None or version_satisfies(have, op, ver)):
            return True
        if op is None:
            if name in provided:
                return True
            if any(p in selected for p in archive.provides.get(name, ())):
                return True
        return False

    def candidate(name, op, ver):
        s = archive.packages.get(name)
        if s and (op is None or version_satisfies(s["Version"], op, ver)):
            return s
        if op is None:
            for p in sorted(archive.provides.get(name, ())):
                return archive.packages[p]
        return None

    queue = []

    def select(stanza):
        if stanza["Package"] not in selected:
            selected[stanza["Package"]] = stanza
            queue.append(stanza)

    for name in requested:
        s = candidate(name, None, None)
        if s is None:
            raise SystemExit(f"sysroot: package not found in the archive: {name}")
        select(s)

    while queue:
        s = queue.pop()
        groups = parse_dep_field(s.get("Pre-Depends", "")) + parse_dep_field(s.get("Depends", ""))
        for alts in groups:
            if any(satisfied(*a) for a in alts):
                continue
            for a in alts:
                c = candidate(*a)
                if c is not None:
                    select(c)
                    break
            else:
                wanted = " | ".join(a[0] for a in alts)
                print(f"sysroot: warning: {s['Package']} needs unavailable {wanted}",
                      file=sys.stderr)
    return selected


def unpack(deb, root):
    fsys = subprocess.Popen(["dpkg-deb", "--fsys-tarfile", deb], stdout=subprocess.PIPE)
    subprocess.run(["tar", "-x", "-C", root, "--keep-directory-symlink",
                    "--no-same-owner", "--delay-directory-restore", "-f", "-"],
                   stdin=fsys.stdout, check=True)
    fsys.stdout.close()
    if fsys.wait() != 0:
        raise SystemExit(f"sysroot: dpkg-deb failed on {deb}")


def cmd_install(args):
    os.makedirs(args.cache, exist_ok=True)
    archive = Archive()
    for suite in args.suites:
        hashes = verified_inrelease(args.mirror, suite, args.keyring, args.cache)
        for comp in args.components:
            archive.add(load_index(args.mirror, suite, comp, args.arch, hashes, args.cache), args.arch)
            archive.add(load_index(args.mirror, suite, comp, "all", hashes, args.cache), args.arch)

    requested = list(args.packages)
    if args.gcc_from:
        gcc = archive.packages.get(args.gcc_from)
        m = gcc and re.search(r"\bgcc-(\d+)\b", gcc.get("Depends", ""))
        if not m:
            raise SystemExit(f"sysroot: cannot find the default GCC version via {args.gcc_from}")
        major = m.group(1)
        print(f"sysroot: default GCC is {major}")
        requested = [p.replace("@GCC@", major) for p in requested]

    installed, provided = read_status(args.root)
    selected = resolve(archive, requested, installed, provided)
    print(f"sysroot: {len(selected)} packages to unpack")

    debs = os.path.join(args.cache, "debs")
    os.makedirs(debs, exist_ok=True)
    manifest = []
    for name in sorted(selected):
        s = selected[name]
        path = os.path.join(debs, os.path.basename(s["Filename"]))
        if not (os.path.exists(path) and sha256_file(path) == s["SHA256"]):
            fetch(f"{args.mirror}/{s['Filename']}", path)
            if sha256_file(path) != s["SHA256"]:
                raise SystemExit(f"sysroot: hash mismatch for {s['Filename']}")
        unpack(path, args.root)
        manifest.append(f"{name} {s['Version']}")

    with open(os.path.join(args.root, ".skate3-sysroot-packages"), "w", encoding="utf-8") as f:
        f.write("\n".join(manifest) + "\n")


def cmd_fix_links(args):
    root = os.path.realpath(args.root)
    fixed = 0
    for dirpath, dirnames, filenames in os.walk(root):
        for n in dirnames + filenames:
            p = os.path.join(dirpath, n)
            if not os.path.islink(p):
                continue
            target = os.readlink(p)
            if not target.startswith("/"):
                continue
            new = os.path.relpath(root + target, dirpath)
            os.unlink(p)
            os.symlink(new, p)
            fixed += 1
    print(f"sysroot: rewrote {fixed} absolute symlinks")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    i = sub.add_parser("install")
    i.add_argument("--root", required=True)
    i.add_argument("--cache", required=True)
    i.add_argument("--keyring", required=True)
    i.add_argument("--mirror", required=True)
    i.add_argument("--arch", required=True)
    i.add_argument("--suites", nargs="+", required=True)
    i.add_argument("--components", nargs="+", required=True)
    i.add_argument("--gcc-from", help="package whose gcc-N dependency replaces @GCC@ in package names")
    i.add_argument("packages", nargs="+")
    i.set_defaults(func=cmd_install)

    f = sub.add_parser("fix-links")
    f.add_argument("--root", required=True)
    f.set_defaults(func=cmd_fix_links)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
