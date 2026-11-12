#!/usr/bin/env python3
"""Regenerate the scene.hlsl SPIR-V blobs in the native shader header.

Run from any directory:
  python tools/generate_native_spirv.py --dxc /path/to/dxc --check
  python tools/generate_native_spirv.py --dxc /path/to/dxc

Vulkan (Windows Vulkan and the Odin 2) runs the precompiled SPIR-V in
src/native/shaders/spirv/skate3_native_shaders_spirv.h; D3D12 compiles the
HLSL at startup. Every edit to scene.hlsl or one of its .hlsli includes
must be followed by a run of this tool, or Vulkan silently keeps the old
shader.

The tool replaces the eight k_scene_* arrays in place (vs_main, the four
ps_main variants, the two shadow casters and ps_refl_gbuf) and leaves every
other array and the lookup table byte-for-byte. The Hall of Meat and screen
effects blobs have their own tools (generate_hom_spirv.py,
generate_screen_fx_spirv.py). Register bindings follow the frozen Vulkan
set/binding plan the native RHI expects (BINDINGS below; the tool checks
the compiled SPIR-V's decorations against it).

A different DXC version produces different but equivalent bytecode, so
--check against a header built with another compiler reports DIFF even
when the HLSL is unchanged. A dxc from nixpkgs works:
  nix shell nixpkgs#directx-shader-compiler -c python3 tools/generate_native_spirv.py --dxc dxc
Only Python's standard library is needed.
"""

import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SOURCE = REPO_ROOT / "src/native/shaders/scene.hlsl"
DEFAULT_HEADER = REPO_ROOT / "src/native/shaders/spirv/skate3_native_shaders_spirv.h"

# register -> (descriptor set, binding). These match the native RHI layout
# (the same plan as generate_hom_spirv.py; read back from the decorations
# of the checked-in scene blobs).
BINDINGS = {
    "b0": (0, 0), "t2": (0, 1), "b1": (0, 2), "b2": (0, 3),
    "s0": (0, 4), "s1": (0, 5), "t0": (1, 0), "t1": (2, 0),
    "t3": (3, 0), "t4": (4, 0), "t5": (4, 1), "t6": (5, 0),
    "t7": (5, 1), "t8": (6, 0), "t9": (6, 1), "t10": (6, 2),
}
RESOURCE_BINDINGS = dict(zip(
    ("C", "bones", "S", "CH", "smp", "smp_clamp", "diffuse", "lightmap",
     "macro", "decal_art", "normal_map", "env_cube", "shadow_atlas",
     "detail_map", "spec2_map", "static_sun"),
    BINDINGS.values(),
))
# entry, shader model, canonical runtime macro signature, C++ array name.
MATRIX = (
    ("vs_main", "vs_6_0", "", "k_scene_vs_main"),
    ("ps_main", "ps_6_0", "", "k_scene_ps_main"),
    ("ps_main", "ps_6_0", "HDR=1", "k_scene_ps_main_HDR_1"),
    ("ps_main", "ps_6_0", "SHOWCASE=1", "k_scene_ps_main_SHOWCASE_1"),
    ("ps_main", "ps_6_0", "HDR=1;SHOWCASE=1", "k_scene_ps_main_HDR_1_SHOWCASE_1"),
    ("ps_shadow_caster", "ps_6_0", "", "k_scene_ps_shadow_caster"),
    ("ps_shadow_caster_clip", "ps_6_0", "", "k_scene_ps_shadow_caster_clip"),
    ("ps_refl_gbuf", "ps_6_0", "", "k_scene_ps_refl_gbuf"),
)
SCENE_NAMES = {request[3] for request in MATRIX}
ARRAY_PATTERN = re.compile(
    r"(// scene\.hlsl : [^\n]*\n)?static const uint32_t (\w+)\[\] = \{(.*?)\n\};", re.S
)
LOOKUP_PATTERN = re.compile(
    r'\{"([^"\n]+)", "([^"\n]+)", "([^"\n]*)", '
    r'(\w+), sizeof\((\w+)\)\}'
)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def parse_arrays(text):
    result = {}
    for match in ARRAY_PATTERN.finditer(text):
        _, name, body = match.groups()
        if name in result:
            raise ValueError("Duplicate SPIR-V array: " + name)
        words = [int(value, 16) for value in re.findall(r"0x[0-9a-fA-F]+", body)]
        result[name] = struct.pack("<" + "I" * len(words), *words)
    return result


def validate_header(text):
    arrays = parse_arrays(text)
    missing = SCENE_NAMES - set(arrays)
    if missing:
        raise ValueError("Header lacks scene arrays: " + ", ".join(sorted(missing)))
    rows = LOOKUP_PATTERN.findall(text)
    expected = [("scene.hlsl", entry, variant, name, name)
                for entry, _, variant, name in MATRIX]
    actual = [row for row in rows if row[3] in SCENE_NAMES]
    if actual != expected:
        raise ValueError("Scene lookup rows must match the compile matrix exactly")
    return arrays


def spirv_string(words):
    data = struct.pack("<" + "I" * len(words), *words)
    if b"\0" not in data:
        raise ValueError("Unterminated SPIR-V string")
    return data.split(b"\0", 1)[0].decode("utf-8")


def validate_spirv(data, entry, profile):
    if len(data) < 20 or len(data) % 4:
        raise ValueError("Invalid SPIR-V byte length")
    words = struct.unpack("<" + "I" * (len(data) // 4), data)
    if words[0] != 0x07230203 or not 0x00010000 <= words[1] <= 0x00010300:
        raise ValueError("Expected Vulkan 1.1 compatible SPIR-V")
    names, sets, bindings, entries = {}, {}, {}, []
    offset = 5
    while offset < len(words):
        count, opcode = words[offset] >> 16, words[offset] & 0xFFFF
        if not count or offset + count > len(words):
            raise ValueError("Malformed SPIR-V instruction stream")
        args = words[offset + 1:offset + count]
        if opcode == 5:  # OpName
            names[args[0]] = spirv_string(args[1:])
        elif opcode == 15:  # OpEntryPoint
            entries.append((args[0], spirv_string(args[2:])))
        elif opcode == 71 and len(args) >= 2 and args[1] in (33, 34):  # OpDecorate
            values = bindings if args[1] == 33 else sets
            values[args[0]] = args[2]
        offset += count
    expected_model = 0 if profile.startswith("vs_") else 4
    if entries != [(expected_model, entry)]:
        raise ValueError("Unexpected SPIR-V entry point for " + entry)
    if sets.keys() != bindings.keys():
        raise ValueError("Descriptor set/binding decorations are incomplete")
    for resource_id, descriptor_set in sets.items():
        name = names.get(resource_id)
        pair = (descriptor_set, bindings[resource_id])
        if name not in RESOURCE_BINDINGS or RESOURCE_BINDINGS[name] != pair:
            raise ValueError(f"Resource violates the frozen scene binding plan: {name} -> {pair}")


def compile_blobs(dxc, source, directory):
    result = {}
    for entry, profile, variant, name in MATRIX:
        output = directory / (name + ".spv")
        args = [str(dxc), str(source), "-E", entry, "-T", profile,
                "-spirv", "-fspv-target-env=vulkan1.1", "-O3", "-Fo", str(output)]
        for register, (descriptor_set, binding) in BINDINGS.items():
            args.extend(["-fvk-bind-register", register, "0", str(binding), str(descriptor_set)])
        for define in filter(None, variant.split(";")):
            args.extend(["-D", define])
        subprocess.run(args, check=True)
        data = output.read_bytes()
        validate_spirv(data, entry, profile)
        result[name] = data
    return result


def array_text(entry, variant, name, data, compiler_short):
    words = struct.unpack("<" + "I" * (len(data) // 4), data)
    label = f" [{variant}]" if variant else ""
    lines = [f"// scene.hlsl : {entry}{label} ({len(data)} bytes; DXC {compiler_short})",
             f"static const uint32_t {name}[] = {{"]
    for index in range(0, len(words), 8):
        lines.append("    " + ", ".join(f"0x{word:08x}" for word in words[index:index + 8]) + ",")
    lines.append("};")
    return "\n".join(lines)


def replace_arrays(original, blobs, compiler_short):
    text = original.decode("utf-8")
    before = validate_header(text)
    newline = "\r\n" if "\r\n" in text else "\n"
    by_name = {entry_name: (entry, variant) for entry, _, variant, entry_name in MATRIX}

    def substitute(match):
        _, name, _ = match.groups()
        if name not in SCENE_NAMES:
            return match.group(0)
        entry, variant = by_name[name]
        return array_text(entry, variant, name, blobs[name], compiler_short).replace("\n", newline)

    updated_text = ARRAY_PATTERN.sub(substitute, text)
    after = validate_header(updated_text)
    untouched = {name: data for name, data in before.items() if name not in SCENE_NAMES}
    if untouched != {name: data for name, data in after.items() if name not in SCENE_NAMES}:
        raise ValueError("An existing non-scene SPIR-V blob changed")
    if LOOKUP_PATTERN.findall(text) != LOOKUP_PATTERN.findall(updated_text):
        raise ValueError("The shader lookup table changed")
    if {name: after[name] for name in SCENE_NAMES} != blobs:
        raise ValueError("Embedded scene arrays differ from the compiled SPIR-V")
    return updated_text.encode("utf-8"), len(untouched)


def write_atomic(path, data):
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, prefix=path.name + ".",
                                         suffix=".tmp", delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, path.stat().st_mode)
        os.replace(temporary, path)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dxc", default="dxc", help="DXC executable (default: dxc on PATH)")
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE, help="HLSL source (default: repo scene.hlsl)")
    parser.add_argument("--header", type=Path, default=DEFAULT_HEADER, help="Native SPIR-V header (default: repo header)")
    parser.add_argument("--check", action="store_true", help="Compile and compare without writing; exit 1 if regeneration differs")
    options = parser.parse_args(argv)
    dxc = shutil.which(options.dxc) or str(Path(options.dxc).resolve())
    source, header = options.source.resolve(), options.header.resolve()
    original = header.read_bytes()
    validate_header(original.decode("utf-8"))
    compiler = subprocess.check_output([dxc, "--version"], text=True).strip().splitlines()[0]
    version = re.search(r"\(([\d.]+)\)", compiler)
    compiler_short = version.group(1) if version else compiler
    with tempfile.TemporaryDirectory(prefix="native-spirv-") as directory:
        blobs = compile_blobs(dxc, source, Path(directory))
    updated, preserved_count = replace_arrays(original, blobs, compiler_short)
    changed = updated != original
    if header.read_bytes() != original:
        raise ValueError("Header changed during compilation; refusing to overwrite or check stale contents")
    if not options.check and changed:
        write_atomic(header, updated)
    print(f"{'DIFF' if options.check and changed else 'PASS'}: {len(MATRIX)} scene blobs compiled with {compiler}; "
          f"{preserved_count} other blobs and the lookup table preserved")
    for _, _, variant, name in MATRIX:
        print(f"{name}: {len(blobs[name])} bytes, SHA256 {digest(blobs[name])[:16]}")
    if options.check and changed:
        print("Regeneration differs; --check left the header unchanged.", file=sys.stderr)
        return 1
    if not changed:
        print("Header is byte-for-byte current; no write performed.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print("error: " + str(error), file=sys.stderr)
        sys.exit(2)
