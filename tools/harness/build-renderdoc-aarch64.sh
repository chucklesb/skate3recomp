#!/usr/bin/env bash
# Cross-build RenderDoc's Vulkan capture layer and renderdoccmd for
# linux-aarch64 (RenderDoc publishes no arm64 Linux builds), for capturing
# frames on DroidDeck handhelds.
#
#   tools/harness/build-renderdoc-aarch64.sh [out-dir]   (default out/renderdoc-aarch64)
#
# Run from the repository; it enters `nix develop .#cross` itself and uses
# the same toolchain and Ubuntu 26.04 arm64 sysroot as the game
# (tools/cross-aarch64/make-sysroot.sh must have run). Produces:
#   <out>/librenderdoc.so, <out>/renderdoccmd
#   <out>/layers/renderdoc_capture.json   (library_path = $RENDERDOC_DEVICE_DIR)
# Install on the device with `run_scenario.py install-renderdoc`, then run
# with `--env SKATE3_RENDERDOC=/root/renderdoc` (AppRun loads it into the
# game process only). Open the .rdc files on a desktop RenderDoc of the same
# version (v1.46).
#
# Only Vulkan, Xlib and Wayland are built: no Qt UI, Python, GL or GLES.

set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
out=$(mkdir -p "${1:-$repo/out/renderdoc-aarch64}" && cd "${1:-$repo/out/renderdoc-aarch64}" && pwd)
version=v1.46
sha256=4d23c2b77f6142bfeaa1563621d7a0be394294ed750f67d472277d14aae9bbcc
device_dir=${RENDERDOC_DEVICE_DIR:-/root/renderdoc}
work=$repo/out/renderdoc-aarch64-build

mkdir -p "$work"
if [ ! -f "$work/src/CMakeLists.txt" ]; then
    curl -sSfL -o "$work/renderdoc.tar.gz" \
        "https://github.com/baldurk/renderdoc/archive/refs/tags/$version.tar.gz"
    echo "$sha256  $work/renderdoc.tar.gz" | sha256sum -c -
    rm -rf "$work/src"
    mkdir -p "$work/src"
    tar -xzf "$work/renderdoc.tar.gz" -C "$work/src" --strip-components=1
fi

if [ -z "${SKATE3_AARCH64_SYSROOT:-}" ]; then
    exec nix develop "$repo#cross" --command "$0" "$out"
fi

export PKG_CONFIG_SYSROOT_DIR=$SKATE3_AARCH64_SYSROOT
export PKG_CONFIG_LIBDIR=$SKATE3_AARCH64_SYSROOT/usr/lib/aarch64-linux-gnu/pkgconfig:$SKATE3_AARCH64_SYSROOT/usr/share/pkgconfig
# Setting CMAKE_SHARED_LINKER_FLAGS replaces the toolchain's lld selection,
# so it is restated. --undefined-version: RenderDoc's version script exports
# GL/exec hooks this Vulkan-only build does not compile, which lld rejects.
cmake -S "$work/src" -B "$work/build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$repo/cmake/toolchains/linux-aarch64.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_QRENDERDOC=OFF -DENABLE_PYRENDERDOC=OFF \
    -DENABLE_GL=OFF -DENABLE_GLES=OFF -DENABLE_EGL=OFF \
    -DENABLE_VULKAN=ON -DENABLE_XLIB=ON -DENABLE_XCB=OFF \
    -DENABLE_UNSUPPORTED_EXPERIMENTAL_POSSIBLY_BROKEN_WAYLAND=ON \
    -DCMAKE_SHARED_LINKER_FLAGS="-fuse-ld=lld --ld-path=$SKATE3_CROSS_LLD -Wl,--undefined-version" \
    >/dev/null
cmake --build "$work/build" -j"$(nproc)"

mkdir -p "$out/layers"
install -m 755 "$work/build/lib/librenderdoc.so" "$work/build/bin/renderdoccmd" "$out/"
sed "s#\"library_path\": \"[^\"]*\"#\"library_path\": \"$device_dir/librenderdoc.so\"#" \
    "$work/build/lib/renderdoc_capture.json" > "$out/layers/renderdoc_capture.json"
echo "build-renderdoc-aarch64: wrote $out (layer library_path $device_dir/librenderdoc.so)"
