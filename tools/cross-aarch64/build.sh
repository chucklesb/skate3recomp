#!/usr/bin/env bash
# Cross-build skate3recomp for linux-aarch64 from an x86_64 Linux host.
#
#   tools/cross-aarch64/build.sh [release|relwithdebinfo] [step...]
#
# Steps (default: all, in this order):
#   sysroot   build the Ubuntu 26.04 arm64 sysroot      (nix develop .#cross)
#   codegen   host x86_64 build of rexglue + generate-all (nix develop .#codegen)
#   build     cross-configure and build                 (nix develop .#cross)
#   package   zip into out/release/Skate3Recomp-Linux-aarch64.zip
#   appimage  out/release/Skate3Recomp-<version>-aarch64.AppImage, validated
#             against DroidDeck's import rules (tools/appimage/)
#
# The recompiled sources in generated/ do not depend on the host CPU, so a
# host-built rexglue generates them for the aarch64 build too.
#
# SKATE3_GAME_DATA_ROOT defaults to <repo>/game. Title Update 3 must be
# available (see README "Building from Source").

set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
cd "$repo"

config=${1:-release}
case "$config" in
    release|relwithdebinfo) shift || true ;;
    sysroot|codegen|build|package|appimage) config=release ;;
    *) echo "build.sh: unknown config '$config'" >&2; exit 2 ;;
esac
steps=("$@")
[ ${#steps[@]} -eq 0 ] && steps=(sysroot codegen build package appimage)

game_root=${SKATE3_GAME_DATA_ROOT:-$repo/game}
jobs=${SKATE3_JOBS:-$(nproc)}
preset=linux-aarch64-$config
# nix shells pin SOURCE_DATE_EPOCH to 1980; use the commit time so the
# build stamp is both reproducible and meaningful.
epoch=$(git -C "$repo" log -1 --format=%ct 2>/dev/null || date +%s)

in_shell() {
    local shell=$1; shift
    nix develop "$repo#$shell" --command bash -euo pipefail -c "export SOURCE_DATE_EPOCH=$epoch; $*"
}

for step in "${steps[@]}"; do
    echo "==> $step"
    case "$step" in
        sysroot)
            in_shell cross "tools/cross-aarch64/make-sysroot.sh"
            ;;
        codegen)
            in_shell codegen "
                cmake --preset linux-host-codegen -DSKATE3_GAME_DATA_ROOT='$game_root'
                cmake --build --preset linux-host-codegen --target generate-all --parallel $jobs"
            ;;
        build)
            if [ ! -f generated/sources.cmake ] || [ ! -f generated/eawebkit/sources.cmake ]; then
                echo "build.sh: generated/ is missing; run the codegen step first" >&2
                exit 1
            fi
            mkdir -p "out/build/$preset"
            in_shell cross "
                cmake --preset $preset ${SKATE3_CMAKE_FRESH:+--fresh} -DSKATE3_GAME_DATA_ROOT='$game_root' 2>&1 | tee out/build/$preset/configure.log"
            # SDL quietly drops video drivers whose dependencies it cannot
            # find; a build without them cannot open a window.
            drivers=$(grep -m1 'Video drivers:' "out/build/$preset/configure.log" || true)
            if ! grep -q 'x11' <<<"$drivers" || ! grep -q 'wayland' <<<"$drivers"; then
                echo "build.sh: SDL3 is missing the x11/wayland video drivers (${drivers:-no summary})" >&2
                echo "build.sh: see out/build/$preset/configure.log" >&2
                exit 1
            fi
            in_shell cross "cmake --build --preset $preset --parallel $jobs"
            ;;
        package)
            in_shell cross "SKATE3_PACKAGE_NAME=Skate3Recomp-Linux-aarch64 sh tools/package_linux.sh out/build/$preset"
            ;;
        appimage)
            in_shell cross "tools/appimage/build-appimage.sh out/build/$preset out/release"
            ;;
        *)
            echo "build.sh: unknown step '$step'" >&2
            exit 2
            ;;
    esac
done
