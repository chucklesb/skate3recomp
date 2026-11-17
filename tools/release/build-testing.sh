#!/usr/bin/env bash
# Build a local (unpublished) linux-aarch64 *testing* AppImage, optionally
# installing it on the Odin 2 in place of the current testing app.
#
#   tools/release/build-testing.sh [--install] [--smoke]
#
# Same build as stage-prerelease.sh (RelWithDebInfo, testing variant: app
# "Skate3Recomp Testing", red icon) from the working tree, versioned by git
# describe (e.g. 2.1.0.6-dev.gab03439); no tag, no upload. An unchanged tree
# rebuilds in about 40 s (codegen skips itself, cmake/RunCodegen.cmake).
# --install runs tools/harness/droiddeck_install_appimage.py (DroidDeck must
# be idle); --smoke also launches it once.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
cd "$repo"
install=0 smoke=()
for arg in "$@"; do
    case $arg in
        --install) install=1 ;;
        --smoke) smoke=(--smoke) ;;
        *) echo "usage: build-testing.sh [--install] [--smoke]" >&2; exit 2 ;;
    esac
done

[ -n "$(command -v nix)" ] || . "$HOME/.nix-profile/etc/profile.d/nix.sh"
export SKATE3_APPIMAGE_VARIANT=testing SKATE3_APPIMAGE_KEEP_DEBUG=1
start=$(date +%s)
nix develop "$repo#cross" --command tools/cross-aarch64/build.sh relwithdebinfo \
    sysroot codegen build appimage
version=$(sed -n 's/^#define SKATE3_VERSION_STRING "\(.*\)"$/\1/p' \
    out/build/linux-aarch64-relwithdebinfo/include/skate3_version.h)
image=out/release/skate3recomp-testing-$version-aarch64.AppImage
[ -f "$image" ] || { echo "build-testing: $image is missing" >&2; exit 1; }
echo "build-testing: $image ($(( $(date +%s) - start )) s)"

if [ $install = 1 ]; then
    python3 tools/harness/droiddeck_install_appimage.py "$image" "${smoke[@]}"
fi
