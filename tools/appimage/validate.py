#!/usr/bin/env python3
"""Check that an AppImage meets what DroidDeck needs to import and run it.

    python3 -I tools/appimage/validate.py <image> [--expect-version X] [...]

Mirrors DroidDeck's AppImageManager/UserApps logic: the import header check
(ELF, aarch64, type-2 'AI\\x02'), the GitHub asset pick (name ends in
.AppImage and mentions aarch64/arm64), extraction with --appimage-extract
(DroidDeck never FUSE-mounts), and the top-level .desktop + icon it reads.
Then checks the payload: aarch64 ELFs with an $ORIGIN RUNPATH, no
/nix/store references, glibc/libstdc++ symbol versions within the target
runtime, only allowlisted DT_NEEDED libraries, and no bundled Vulkan/GL
driver stack (the host's Turnip must be used).

Extraction runs the image natively on aarch64 hosts and through
qemu-aarch64 elsewhere. Exit status 0 means every check passed.
"""

import argparse
import configparser
import hashlib
import os
import platform
import re
import shutil
import struct
import subprocess
import sys
import tempfile

EM_AARCH64 = 183
PT_INTERP = 3
SHT_DYNAMIC = 6
SHT_GNU_VERNEED = 0x6FFFFFFE
DT_NEEDED, DT_RPATH, DT_RUNPATH = 1, 15, 29

FORBIDDEN_PAYLOAD = re.compile(
    r"(^|/)(libvulkan[^/]*\.so[^/]*|libGL[^/]*\.so[^/]*|libEGL[^/]*\.so[^/]*|libgbm[^/]*\.so[^/]*|"
    r"[^/]*_icd[^/]*\.json|libvulkan_freedreno[^/]*)$")


# --------------------------------------------------------------------------
# ELF reading (ELF64 little-endian only; that is all aarch64 Linux uses)
# --------------------------------------------------------------------------

class ElfInfo:
    def __init__(self):
        self.machine = None
        self.interp = None
        self.needed = []
        self.runpath = None
        self.version_needs = set()


def _cstr(data, off):
    end = data.index(b"\0", off)
    return data[off:end].decode("utf-8", "replace")


def read_elf(data):
    if data[:4] != b"\x7fELF":
        raise ValueError("not an ELF file")
    if data[4] != 2 or data[5] != 1:
        raise ValueError("not a 64-bit little-endian ELF")
    info = ElfInfo()
    info.machine = struct.unpack_from("<H", data, 0x12)[0]
    phoff = struct.unpack_from("<Q", data, 0x20)[0]
    shoff = struct.unpack_from("<Q", data, 0x28)[0]
    phentsize, phnum, shentsize, shnum = struct.unpack_from("<HHHH", data, 0x36)

    for i in range(phnum):
        base = phoff + i * phentsize
        p_type = struct.unpack_from("<I", data, base)[0]
        if p_type == PT_INTERP:
            p_offset = struct.unpack_from("<Q", data, base + 8)[0]
            p_filesz = struct.unpack_from("<Q", data, base + 32)[0]
            info.interp = data[p_offset:p_offset + p_filesz].rstrip(b"\0").decode()

    sections = []
    for i in range(shnum):
        base = shoff + i * shentsize
        (_, sh_type, _, _, sh_offset, sh_size, sh_link, sh_info, _, _) = struct.unpack_from(
            "<IIQQQQIIQQ", data, base)
        sections.append((sh_type, sh_offset, sh_size, sh_link, sh_info))

    for sh_type, off, size, link, count in sections:
        if sh_type == SHT_DYNAMIC:
            strtab = sections[link][1]
            for j in range(0, size, 16):
                tag, val = struct.unpack_from("<qQ", data, off + j)
                if tag == DT_NEEDED:
                    info.needed.append(_cstr(data, strtab + val))
                elif tag in (DT_RUNPATH, DT_RPATH):
                    info.runpath = _cstr(data, strtab + val)
        elif sh_type == SHT_GNU_VERNEED:
            strtab = sections[link][1]
            vn = off
            for _ in range(count):
                _, vn_cnt, _, vn_aux, vn_next = struct.unpack_from("<HHIII", data, vn)
                aux = vn + vn_aux
                for _ in range(vn_cnt):
                    _, _, _, vna_name, vna_next = struct.unpack_from("<IHHII", data, aux)
                    info.version_needs.add(_cstr(data, strtab + vna_name))
                    aux += vna_next
                vn += vn_next
    return info


def version_tuple(s):
    return tuple(int(x) for x in s.split("."))


def max_version(needs, prefix):
    versions = [n[len(prefix):] for n in needs if n.startswith(prefix)
                and re.fullmatch(r"[0-9.]+", n[len(prefix):])]
    return max(versions, key=version_tuple) if versions else None


# --------------------------------------------------------------------------
# Checks
# --------------------------------------------------------------------------

class Report:
    def __init__(self):
        self.failures = 0

    def check(self, ok, what, detail=""):
        print(f"[{'ok' if ok else 'FAIL'}] {what}{': ' + detail if detail else ''}")
        if not ok:
            self.failures += 1
        return ok


def droiddeck_header_ok(head):
    """AppImageManager.problem(): ELF magic, aarch64, type-2 AppImage magic."""
    if len(head) < 20 or head[:4] != b"\x7fELF":
        return False, "not an ELF file"
    machine = struct.unpack_from("<H", head, 18)[0]
    if machine != EM_AARCH64:
        return False, f"e_machine {machine}, expected {EM_AARCH64} (aarch64)"
    if head[8:11] != b"AI\x02":
        return False, "missing type-2 AppImage magic 'AI\\x02' at offset 8"
    return True, ""


def droiddeck_picks(name):
    """UserApps.pickAsset(): first .AppImage asset that mentions aarch64/arm64."""
    return name.lower().endswith(".appimage") and re.search(r"aarch64|arm64", name, re.I) is not None


def read_desktop(appdir):
    entries = sorted(f for f in os.listdir(appdir) if f.endswith(".desktop"))
    if not entries:
        return None, None
    parser = configparser.ConfigParser(interpolation=None, strict=False)
    parser.optionxform = str
    with open(os.path.join(appdir, entries[0]), encoding="utf-8") as f:
        parser.read_string(f.read())
    if "Desktop Entry" not in parser:
        return entries[0], None
    return entries[0], dict(parser["Desktop Entry"])


def png_size(path):
    with open(path, "rb") as f:
        head = f.read(24)
    if head[:8] != b"\x89PNG\r\n\x1a\n" or head[12:16] != b"IHDR":
        return None
    return struct.unpack(">II", head[16:24])


def find_icon(appdir, icon):
    best_png, svg = None, None
    for root, dirs, files in os.walk(appdir):
        if root[len(appdir):].count(os.sep) >= 8:
            dirs[:] = []
        for f in files:
            if f == icon + ".png":
                size = png_size(os.path.join(root, f))
                if size and (best_png is None or size[0] > best_png[0]):
                    best_png = (size[0], os.path.join(root, f))
            elif f == icon + ".svg":
                svg = os.path.join(root, f)
    return best_png, svg


def load_allowlist(path):
    with open(path, encoding="utf-8") as f:
        return {line.strip() for line in f if line.strip() and not line.startswith("#")}


def extract(image, dest):
    # Run an executable copy, as DroidDeck's importer does: downloads (e.g.
    # `gh release download`) do not keep the executable bit.
    runnable = os.path.join(dest, "image.AppImage")
    shutil.copyfile(image, runnable)
    os.chmod(runnable, 0o755)
    cmd = [runnable, "--appimage-extract"]
    if platform.machine() not in ("aarch64", "arm64"):
        qemu = shutil.which("qemu-aarch64") or shutil.which("qemu-aarch64-static")
        if not qemu:
            raise RuntimeError("not on aarch64 and no qemu-aarch64 on PATH")
        cmd.insert(0, qemu)
    subprocess.run(cmd, cwd=dest, check=True, stdout=subprocess.DEVNULL)
    return os.path.join(dest, "squashfs-root")


def validate(args):
    r = Report()
    image = args.image
    name = os.path.basename(image)

    with open(image, "rb") as f:
        head = f.read(64)
    ok, why = droiddeck_header_ok(head)
    r.check(ok, "DroidDeck import header (ELF, aarch64, AI\\x02)", why)
    r.check(droiddeck_picks(name), "DroidDeck GitHub asset pick (.AppImage, aarch64|arm64)", name)
    if args.expect_version:
        r.check(f"-{args.expect_version}-" in name, "file name carries the version",
                f"{args.expect_version} in {name}")

    sha_file = args.sha256 or (image + ".sha256" if os.path.exists(image + ".sha256") else None)
    if sha_file:
        with open(sha_file, encoding="utf-8") as f:
            want = f.read().split()[0]
        h = hashlib.sha256()
        with open(image, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        r.check(h.hexdigest() == want, "sha256 matches", os.path.basename(sha_file))

    if r.failures and not args.keep_going:
        return r.failures

    with tempfile.TemporaryDirectory() as tmp:
        try:
            appdir = extract(image, tmp)
        except (subprocess.CalledProcessError, RuntimeError, OSError) as e:
            r.check(False, "--appimage-extract", str(e))
            return r.failures
        r.check(os.path.isdir(appdir), "--appimage-extract produced squashfs-root")

        apprun = os.path.join(appdir, "AppRun")
        r.check(os.path.isfile(apprun) and os.access(apprun, os.X_OK), "AppRun present and executable")

        desktop_name, entry = read_desktop(appdir)
        r.check(entry is not None, "top-level .desktop with [Desktop Entry]", desktop_name or "none")
        entry = entry or {}
        for key in ("Name", "Icon", "Categories"):
            r.check(bool(entry.get(key)), f".desktop has {key}", entry.get(key, ""))
        if args.expect_version:
            r.check(entry.get("X-AppImage-Version") == args.expect_version,
                    ".desktop X-AppImage-Version", entry.get("X-AppImage-Version", ""))
        if entry.get("Icon"):
            best_png, svg = find_icon(appdir, entry["Icon"])
            r.check((best_png and best_png[0] >= 128) or svg is not None,
                    "icon >= 128 px PNG or SVG",
                    f"png {best_png[0]}px" if best_png else ("svg" if svg else "not found"))

        allowed = load_allowlist(args.allowed_libs)
        bundled = {f for _, _, files in os.walk(appdir) for f in files if ".so" in f}
        glibc_max, glibcxx_max = version_tuple(args.max_glibc), version_tuple(args.max_glibcxx)
        elf_count = 0
        for root, _, files in os.walk(appdir):
            for f in files:
                path = os.path.join(root, f)
                rel = os.path.relpath(path, appdir)
                if FORBIDDEN_PAYLOAD.search(rel):
                    r.check(False, "no bundled Vulkan/GL driver stack", rel)
                if os.path.islink(path) or not os.path.isfile(path):
                    continue
                with open(path, "rb") as fh:
                    data = fh.read()
                if data[:4] != b"\x7fELF":
                    continue
                elf_count += 1
                try:
                    info = read_elf(data)
                except (ValueError, struct.error, IndexError) as e:
                    r.check(False, f"{rel}: readable ELF", str(e))
                    continue
                r.check(info.machine == EM_AARCH64, f"{rel}: aarch64", f"e_machine {info.machine}")
                r.check(b"/nix/store" not in data, f"{rel}: no /nix/store references")
                if info.interp is not None:
                    r.check(info.interp == "/lib/ld-linux-aarch64.so.1", f"{rel}: interpreter", info.interp)
                if set(info.needed) & bundled:
                    r.check(info.runpath == "$ORIGIN", f"{rel}: RUNPATH finds bundled libraries",
                            str(info.runpath))
                elif info.runpath is not None:
                    r.check(info.runpath == "$ORIGIN", f"{rel}: RUNPATH stays inside the image",
                            info.runpath)
                bad = sorted(set(info.needed) - allowed)
                r.check(not bad, f"{rel}: DT_NEEDED allowlisted", ", ".join(bad))
                g = max_version(info.version_needs, "GLIBC_")
                if g:
                    r.check(version_tuple(g) <= glibc_max, f"{rel}: max GLIBC_{g} <= {args.max_glibc}")
                gx = max_version(info.version_needs, "GLIBCXX_")
                if gx:
                    r.check(version_tuple(gx) <= glibcxx_max,
                            f"{rel}: max GLIBCXX_{gx} <= {args.max_glibcxx}")
                if args.expect_version and f == "skate3":
                    r.check(f"[v{args.expect_version}-".encode() in data,
                            "skate3 embeds the expected version", args.expect_version)
        r.check(elf_count > 0, "payload contains ELF binaries", str(elf_count))

    return r.failures


def main(argv=None):
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image")
    ap.add_argument("--sha256", help="checksum file (default: <image>.sha256 if present)")
    ap.add_argument("--expect-version", help="version the file name, .desktop and skate3 must carry")
    ap.add_argument("--allowed-libs", default=os.path.join(here, "allowed-libs.txt"))
    # Ubuntu 26.04 (DroidDeck's runtime): glibc 2.43, libstdc++.so.6.0.35.
    ap.add_argument("--max-glibc", default="2.43")
    ap.add_argument("--max-glibcxx", default="3.4.35")
    ap.add_argument("--keep-going", action="store_true", help="extract even if header checks fail")
    args = ap.parse_args(argv)
    failures = validate(args)
    print("validate: " + ("all checks passed" if not failures else f"{failures} check(s) failed"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
