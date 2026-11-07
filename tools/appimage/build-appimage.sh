#!/usr/bin/env bash
# Assemble a type-2 AppImage from a linux-aarch64 build.
#
#   tools/appimage/build-appimage.sh <build-dir> [out-dir]
#
# Writes <out-dir>/Skate3Recomp-<version>-aarch64.AppImage and a .sha256
# next to it (out-dir defaults to out/release), then runs validate.py on it
# (SKATE3_APPIMAGE_SKIP_VALIDATE=1 skips that). Run inside
# `nix develop .#cross`, which provides mksquashfs, rsvg-convert,
# desktop-file-validate and SKATE3_APPIMAGE_RUNTIME (the pinned aarch64
# type-2 runtime).
#
# The image bundles only the game's own three ELFs, the launcher and the
# license texts. System libraries (GTK, Vulkan loader, X11/Wayland, audio)
# come from the host: the Vulkan driver in particular must be the host's.

set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
build_dir=$(cd "${1:?usage: build-appimage.sh <build-dir> [out-dir]}" && pwd)
out_dir=${2:-$repo/out/release}
runtime=${SKATE3_APPIMAGE_RUNTIME:?SKATE3_APPIMAGE_RUNTIME is not set; run inside nix develop .#cross}

version=$(sed -n 's/^#define SKATE3_VERSION_STRING "\(.*\)"$/\1/p' "$build_dir/include/skate3_version.h")
if [ -z "$version" ]; then
    echo "build-appimage: no SKATE3_VERSION_STRING in $build_dir/include/skate3_version.h" >&2
    exit 1
fi

bins=(skate3 librexruntime.so librexgpu-xenos.so)
for f in "${bins[@]}"; do
    [ -f "$build_dir/$f" ] || { echo "build-appimage: $build_dir/$f is missing" >&2; exit 1; }
done

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
appdir=$work/AppDir
mkdir -p "$appdir/usr/bin" "$appdir/usr/share/licenses/skate3recomp"

install -m 755 "$here/AppRun" "$appdir/AppRun"
for f in "${bins[@]}"; do
    install -m 755 "$build_dir/$f" "$appdir/usr/bin/$f"
done

sed "s/@VERSION@/$version/" "$here/skate3recomp.desktop" > "$appdir/skate3recomp.desktop"
desktop-file-validate "$appdir/skate3recomp.desktop"
install -m 644 "$here/skate3recomp.svg" "$appdir/skate3recomp.svg"
rsvg-convert -w 256 -h 256 "$here/skate3recomp.svg" -o "$appdir/skate3recomp.png"
cp "$appdir/skate3recomp.png" "$appdir/.DirIcon"

licenses=$appdir/usr/share/licenses/skate3recomp
sdk=$repo/third_party/rexglue-sdk
cp "$sdk/LICENSE" "$licenses/rexglue-sdk.LICENSE"
# Statically linked third-party code; test-only and macOS-only trees are skipped.
while IFS= read -r -d '' f; do
    rel=${f#"$sdk/thirdparty/"}
    case "$rel" in catch2/*|moltenvk/*) continue ;; esac
    cp "$f" "$licenses/$(printf '%s' "$rel" | tr '/' '_')"
done < <(find "$sdk/thirdparty" -mindepth 2 -maxdepth 3 -type f \
    \( -iname 'LICEN[CS]E*' -o -iname 'COPYING*' -o -iname 'NOTICE*' \) -print0)
commit=$(git -C "$repo" rev-parse HEAD 2>/dev/null || echo unknown)
sdk_commit=$(git -C "$sdk" rev-parse HEAD 2>/dev/null || echo unknown)
source_url=${SKATE3_SOURCE_URL:-$(git -C "$repo" remote get-url "${SKATE3_RELEASE_REMOTE:-fork}" 2>/dev/null \
    || git -C "$repo" remote get-url origin 2>/dev/null || echo unknown)}
cat > "$licenses/SOURCE.txt" <<EOF
Skate3Recomp $version (linux-aarch64)
Source: $source_url (commit $commit)
rexglue SDK commit: $sdk_commit
Third-party sources, including FFmpeg (LGPL), are in third_party/rexglue-sdk/thirdparty
at that commit. No Skate 3 game data is included.
EOF

mkdir -p "$out_dir"
name=Skate3Recomp-$version-aarch64.AppImage
image=$out_dir/$name
mksquashfs "$appdir" "$work/fs.sqfs" -root-owned -noappend -comp zstd -Xcompression-level 19 \
    -b 1M -quiet -no-progress
cat "$runtime" "$work/fs.sqfs" > "$image.part"
chmod 755 "$image.part"
mv "$image.part" "$image"
(cd "$out_dir" && sha256sum "$name" > "$name.sha256")

echo "build-appimage: wrote $image ($(du -h "$image" | cut -f1))"

if [ -z "${SKATE3_APPIMAGE_SKIP_VALIDATE:-}" ]; then
    python3 -I "$here/validate.py" "$image" --expect-version "$version"
fi
