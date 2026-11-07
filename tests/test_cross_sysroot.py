"""Checks for the aarch64 sysroot builder's Debian logic.

Run with: python -B -m unittest discover -s tests -p test_cross_sysroot.py
No network: the archive and dpkg status are built in memory.
"""

import importlib.util
from pathlib import Path
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "tools/cross-aarch64/sysroot.py"
SPEC = importlib.util.spec_from_file_location("cross_sysroot", SCRIPT)
SYSROOT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SYSROOT)


def stanza(name, version, depends="", provides="", arch="arm64"):
    lines = [f"Package: {name}", f"Version: {version}", f"Architecture: {arch}",
             f"Filename: pool/{name}_{version}_{arch}.deb", "SHA256: 00"]
    if depends:
        lines.append(f"Depends: {depends}")
    if provides:
        lines.append(f"Provides: {provides}")
    return "\n".join(lines) + "\n\n"


class VersionCompareTest(unittest.TestCase):
    def assertOrder(self, lower, higher):
        self.assertLess(SYSROOT.version_compare(lower, higher), 0, f"{lower} < {higher}")
        self.assertGreater(SYSROOT.version_compare(higher, lower), 0, f"{higher} > {lower}")

    def test_dpkg_ordering(self):
        self.assertOrder("1.0~rc1", "1.0")
        self.assertOrder("1.9", "1.10")
        self.assertOrder("1.0", "1.0a")
        self.assertOrder("2.0", "1:0.9")
        self.assertOrder("2.43-2ubuntu2", "2.43-2ubuntu2.4")
        self.assertOrder("1.0-1", "1.0-1+b1")
        self.assertOrder("1.0~~", "1.0~")

    def test_equal(self):
        self.assertEqual(SYSROOT.version_compare("1.2.3-4", "1.2.3-4"), 0)
        self.assertEqual(SYSROOT.version_compare("0:1.0", "1.0"), 0)
        self.assertEqual(SYSROOT.version_compare("1.01", "1.1"), 0)

    def test_satisfies(self):
        self.assertTrue(SYSROOT.version_satisfies("2.43-2ubuntu2.4", ">=", "2.43"))
        self.assertTrue(SYSROOT.version_satisfies("1.0", "<<", "1.1"))
        self.assertFalse(SYSROOT.version_satisfies("1.1", "<<", "1.1"))
        self.assertTrue(SYSROOT.version_satisfies("1.1", "=", "1.1"))


class DependencyParsingTest(unittest.TestCase):
    def test_groups_alternatives_and_qualifiers(self):
        groups = SYSROOT.parse_dep_field(
            "libc6 (>= 2.38), libgcc-s1 | libgcc1, python3:any (<< 3.14) [arm64], foo <!nocheck>")
        self.assertEqual(groups, [
            [("libc6", ">=", "2.38")],
            [("libgcc-s1", None, None), ("libgcc1", None, None)],
            [("python3", "<<", "3.14")],
            [("foo", None, None)],
        ])

    def test_empty(self):
        self.assertEqual(SYSROOT.parse_dep_field(""), [])


class ResolveTest(unittest.TestCase):
    def archive(self, *stanzas):
        archive = SYSROOT.Archive()
        archive.add("".join(stanzas), "arm64")
        return archive

    def test_upgrades_installed_package_that_violates_constraint(self):
        archive = self.archive(
            stanza("libc6-dev", "2.43-2", depends="libc6 (= 2.43-2)"),
            stanza("libc6", "2.43-2"),
        )
        selected = SYSROOT.resolve(archive, ["libc6-dev"], {"libc6": "2.43-1"}, set())
        self.assertEqual(sorted(selected), ["libc6", "libc6-dev"])

    def test_keeps_installed_package_that_satisfies(self):
        archive = self.archive(
            stanza("app", "1", depends="libc6 (>= 2.40)"),
            stanza("libc6", "2.43-2"),
        )
        selected = SYSROOT.resolve(archive, ["app"], {"libc6": "2.43-1"}, set())
        self.assertEqual(sorted(selected), ["app"])

    def test_virtual_package_and_alternatives(self):
        archive = self.archive(
            stanza("app", "1", depends="missing-pkg | libfoo-dev, libgl-dev"),
            stanza("libfoo-dev", "1"),
            stanza("libglvnd-dev", "1", provides="libgl-dev"),
        )
        selected = SYSROOT.resolve(archive, ["app"], {}, set())
        self.assertEqual(sorted(selected), ["app", "libfoo-dev", "libglvnd-dev"])

    def test_highest_version_wins_and_arch_filtered(self):
        archive = self.archive(
            stanza("lib", "1.0"),
            stanza("lib", "1.2"),
            stanza("lib", "9.9", arch="amd64"),
            stanza("data", "1", arch="all"),
        )
        self.assertEqual(archive.packages["lib"]["Version"], "1.2")
        self.assertIn("data", archive.packages)

    def test_unknown_request_fails(self):
        with self.assertRaises(SystemExit):
            SYSROOT.resolve(self.archive(), ["nope"], {}, set())


if __name__ == "__main__":
    unittest.main()
