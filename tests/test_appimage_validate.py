"""Checks for the DroidDeck AppImage validator's rules.

Run with: python -B -m unittest discover -s tests -p test_appimage_validate.py
Covers the pure checks; extracting a real image is done by build-appimage.sh
and the cross-tooling workflow.
"""

import importlib.util
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib


SCRIPT = Path(__file__).resolve().parents[1] / "tools/appimage/validate.py"
SPEC = importlib.util.spec_from_file_location("appimage_validate", SCRIPT)
VALIDATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VALIDATE)


def elf_header(machine, appimage=True):
    head = bytearray(64)
    head[:4] = b"\x7fELF"
    head[4], head[5] = 2, 1
    if appimage:
        head[8:11] = b"AI\x02"
    struct.pack_into("<H", head, 18, machine)
    return bytes(head)


def png(width, height):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IEND", b"")


class HeaderTest(unittest.TestCase):
    def test_accepts_aarch64_type2(self):
        self.assertEqual(VALIDATE.droiddeck_header_ok(elf_header(183)), (True, ""))

    def test_rejects_other_arch(self):
        ok, why = VALIDATE.droiddeck_header_ok(elf_header(62))
        self.assertFalse(ok)
        self.assertIn("62", why)

    def test_rejects_missing_magic(self):
        self.assertFalse(VALIDATE.droiddeck_header_ok(elf_header(183, appimage=False))[0])

    def test_rejects_non_elf(self):
        self.assertFalse(VALIDATE.droiddeck_header_ok(b"PK\x03\x04" + bytes(60))[0])


class AssetPickTest(unittest.TestCase):
    def test_names(self):
        self.assertTrue(VALIDATE.droiddeck_picks("Skate3Recomp-2.1.0-aarch64.1-aarch64.AppImage"))
        self.assertTrue(VALIDATE.droiddeck_picks("app-arm64.appimage"))
        self.assertFalse(VALIDATE.droiddeck_picks("app-x86_64.AppImage"))
        self.assertFalse(VALIDATE.droiddeck_picks("Skate3Recomp-Linux-aarch64.zip"))


class PayloadRulesTest(unittest.TestCase):
    def test_forbidden_payload(self):
        for path in ("usr/lib/libvulkan.so.1", "usr/lib/libGLX_mesa.so.0", "usr/lib/libEGL.so.1",
                     "usr/share/vulkan/icd.d/freedreno_icd.aarch64.json"):
            self.assertTrue(VALIDATE.FORBIDDEN_PAYLOAD.search(path), path)
        for path in ("usr/bin/librexgpu-xenos.so", "usr/bin/skate3", "skate3recomp.png"):
            self.assertFalse(VALIDATE.FORBIDDEN_PAYLOAD.search(path), path)

    def test_max_version(self):
        needs = {"GLIBC_2.17", "GLIBC_2.43", "GLIBC_2.9", "GLIBC_PRIVATE", "GLIBCXX_3.4.32"}
        self.assertEqual(VALIDATE.max_version(needs, "GLIBC_"), "2.43")
        self.assertEqual(VALIDATE.max_version(needs, "GLIBCXX_"), "3.4.32")
        self.assertIsNone(VALIDATE.max_version(needs, "CXXABI_"))

    def test_allowlist_covers_game_libraries(self):
        allowed = VALIDATE.load_allowlist(SCRIPT.parent / "allowed-libs.txt")
        for lib in ("librexruntime.so", "librexgpu-xenos.so", "libgtk-3.so.0", "libc.so.6",
                    "libstdc++.so.6", "libX11.so.6", "libwayland-client.so.0"):
            self.assertIn(lib, allowed)
        self.assertNotIn("libvulkan.so.1", allowed)


class DesktopAndIconTest(unittest.TestCase):
    def test_reads_desktop_and_finds_icon(self):
        with tempfile.TemporaryDirectory() as d:
            Path(d, "app.desktop").write_text(
                "[Desktop Entry]\nName=App\nIcon=app\nCategories=Game;\n", encoding="utf-8")
            os.makedirs(Path(d, "usr/share/icons"))
            Path(d, "usr/share/icons/app.png").write_bytes(png(64, 64))
            Path(d, "app.png").write_bytes(png(256, 256))
            name, entry = VALIDATE.read_desktop(d)
            self.assertEqual(name, "app.desktop")
            self.assertEqual(entry["Icon"], "app")
            best_png, svg = VALIDATE.find_icon(d, "app")
            self.assertEqual(best_png[0], 256)
            self.assertIsNone(svg)

    def test_the_shipped_desktop_file(self):
        with tempfile.TemporaryDirectory() as d:
            text = (SCRIPT.parent / "skate3recomp.desktop").read_text(encoding="utf-8")
            Path(d, "skate3recomp.desktop").write_text(text.replace("@VERSION@", "1.0"), encoding="utf-8")
            _, entry = VALIDATE.read_desktop(d)
            for key in ("Name", "Icon", "Categories"):
                self.assertTrue(entry.get(key), key)
            self.assertEqual(entry["X-AppImage-Version"], "1.0")


class ElfReaderTest(unittest.TestCase):
    def test_reads_the_running_interpreter(self):
        with open(os.path.realpath(sys.executable), "rb") as f:
            data = f.read()
        if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
            self.skipTest("interpreter is not a 64-bit little-endian ELF")
        info = VALIDATE.read_elf(data)
        self.assertIsInstance(info.machine, int)
        if info.needed:
            self.assertTrue(any(n.startswith("lib") for n in info.needed))


if __name__ == "__main__":
    unittest.main()
