#!/usr/bin/env python3
"""Add the header casing symlinks an xwin splat is missing for this project.

    tools/cross-windows/xwin-case-links.py <xwin-root> <source-dir>...

Windows sources include SDK headers in whatever case they like (<ObjBase.h>,
<Xinput.h>), and the real files are often lowercase. xwin adds symlinks for
the common variants only. This scans the sources for #include lines and,
wherever a header exists in an xwin include dir under a different case, adds
a symlink with the spelling the source uses. It is idempotent and cheap, so
the build step runs it after every configure (fetched dependencies under the
build dir only exist by then).
"""

import os
import re
import sys
from pathlib import Path

INCLUDE_RE = re.compile(rb'^\s*#\s*include\s*[<"]([^>"\n]+)[>"]', re.M)
SOURCE_EXTS = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".inc", ".rc", ".ipp"}
SKIP_DIRS = {".git", "generated", "CMakeFiles"}


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    xwin = Path(sys.argv[1])
    inc_dirs = [xwin / "crt/include"] + [xwin / "sdk/include" / d for d in ("ucrt", "um", "shared", "winrt")]
    inc_dirs = [d for d in inc_dirs if d.is_dir()]

    wanted: set[str] = set()
    for root in sys.argv[2:]:
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
            for name in filenames:
                if os.path.splitext(name)[1].lower() not in SOURCE_EXTS:
                    continue
                try:
                    data = Path(dirpath, name).read_bytes()
                except OSError:
                    continue
                for m in INCLUDE_RE.finditer(data):
                    wanted.add(m.group(1).decode("utf-8", "replace").replace("\\", "/"))

    # Lowercase name -> actual entry, per directory, built lazily.
    listing: dict[Path, dict[str, str]] = {}

    def entries(d: Path) -> dict[str, str]:
        if d not in listing:
            listing[d] = {e.lower(): e for e in os.listdir(d)} if d.is_dir() else {}
        return listing[d]

    added = 0
    for inc in sorted(wanted):
        parts = [p for p in inc.split("/") if p not in ("", ".")]
        if not parts or ".." in parts:
            continue
        for base in inc_dirs:
            if (base / inc).exists():
                break
            # Resolve each component case-insensitively.
            cur = base
            for p in parts[:-1]:
                real = entries(cur).get(p.lower())
                if real is None:
                    cur = None
                    break
                cur = cur / real
            if cur is None:
                continue
            real = entries(cur).get(parts[-1].lower())
            if real is None:
                continue
            link = cur / parts[-1]
            if not link.exists() and not link.is_symlink():
                link.symlink_to(real)
                entries(cur)[parts[-1].lower()] = real
                added += 1
            break
    print(f"xwin-case-links: {added} symlink(s) added for {len(wanted)} distinct includes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
