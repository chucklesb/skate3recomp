#!/usr/bin/env bash
# Download the MSVC CRT/STL and the Windows SDK for the windows-x64 cross build.
#
#   tools/cross-windows/make-winsdk.sh [xwin-dir]
#
# The default directory is $SKATE3_XWIN_ROOT, or out/sysroot/xwin. Downloads
# are cached in out/sysroot-cache/xwin.
#
# Running this accepts the Microsoft Visual Studio / Windows SDK license
# terms (https://go.microsoft.com/fwlink/?LinkId=2086102) on your behalf:
# xwin fetches the packages from Microsoft's servers. Needs xwin (provided by
# `nix develop .#windows`).
#
# The output uses xwin's default splat layout (crt/, sdk/, lowercase arch
# names, casing symlinks) that cmake/toolchains/windows-x64.cmake expects.

set -euo pipefail

# Pinned so A/B builds stay comparable. If Microsoft drops the CRT version from
# the VS manifest, `xwin --accept-license list` shows the current ones.
MANIFEST_VERSION=17
SDK_VERSION=10.0.26100
CRT_VERSION=14.44.17.14

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
root=${1:-${SKATE3_XWIN_ROOT:-$repo/out/sysroot/xwin}}
cache=$repo/out/sysroot-cache/xwin

stamp=$(printf '%s' "$(cat "$0"; xwin --version)" | sha256sum | cut -d' ' -f1)
if [ -f "$root/.skate3-xwin-stamp" ] && [ "$(cat "$root/.skate3-xwin-stamp")" = "$stamp" ]; then
    echo "make-winsdk: $root is up to date"
    exit 0
fi

if [ -d "$root" ]; then
    chmod -R u+w "$root" 2>/dev/null || true
    rm -rf "$root"
fi
mkdir -p "$cache" "$(dirname "$root")"

echo "make-winsdk: fetching CRT $CRT_VERSION and Windows SDK $SDK_VERSION into $root"
echo "make-winsdk: this accepts the Microsoft license terms for those packages"
xwin --accept-license \
    --cache-dir "$cache" \
    --manifest-version "$MANIFEST_VERSION" \
    --sdk-version "$SDK_VERSION" \
    --crt-version "$CRT_VERSION" \
    --arch x86_64 \
    --variant desktop \
    splat --output "$root"

for f in crt/include/vcruntime.h sdk/include/um/Windows.h sdk/include/um/windows.h \
         sdk/include/um/d3d12.h sdk/lib/um/x86_64/d3d12.lib crt/lib/x86_64/libcmt.lib; do
    if [ ! -e "$root/$f" ]; then
        echo "make-winsdk: $root/$f is missing after splat" >&2
        exit 1
    fi
done

echo "$stamp" > "$root/.skate3-xwin-stamp"
echo "make-winsdk: done"
