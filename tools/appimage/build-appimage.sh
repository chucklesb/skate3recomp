#!/usr/bin/env bash
# Assemble a type-2 AppImage from a linux-aarch64 build.
#
#   tools/appimage/build-appimage.sh <build-dir> [out-dir]
#
# Writes <out-dir>/Skate3Recomp-<version>-aarch64.AppImage and a .sha256
# next to it (out-dir defaults to out/release), then runs validate.py on it
# (SKATE3_APPIMAGE_SKIP_VALIDATE=1 skips that).
#
# SKATE3_APPIMAGE_VARIANT=testing builds the pre-release variant instead:
# app name "Skate3Recomp Testing", the red skate3recomp-testing.png icon
# and skate3recomp-testing-<version>-aarch64.AppImage, so DroidDeck keeps
# it as a separate app next to the release one. Run inside
# `nix develop .#cross`, which provides mksquashfs,
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

variant=${SKATE3_APPIMAGE_VARIANT:-release}
case "$variant" in
    release) stem=skate3recomp; app_name=Skate3Recomp; file_prefix=Skate3Recomp ;;
    testing) stem=skate3recomp-testing; app_name="Skate3Recomp Testing"; file_prefix=skate3recomp-testing ;;
    *) echo "build-appimage: unknown SKATE3_APPIMAGE_VARIANT '$variant' (release|testing)" >&2; exit 1 ;;
esac

# RelWithDebInfo builds name the SDK libraries with an "rd" postfix
# (librexruntimerd.so); skate3's DT_NEEDED uses those names, so ship them as is.
if [ -f "$build_dir/librexruntimerd.so" ] && [ -f "$build_dir/librexruntime.so" ]; then
    echo "build-appimage: $build_dir has both librexruntime.so and librexruntimerd.so; use a clean build dir" >&2
    exit 1
fi
postfix=
[ -f "$build_dir/librexruntimerd.so" ] && postfix=rd
bins=(skate3 "librexruntime$postfix.so" "librexgpu-xenos$postfix.so")
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
# A RelWithDebInfo build carries its DWARF in the ELFs. The image ships them
# without it (SKATE3_APPIMAGE_KEEP_DEBUG=1 keeps it); the unstripped files in
# the build dir have the same build ID and symbolize addresses from a device.
if [ -n "$postfix" ] && [ -z "${SKATE3_APPIMAGE_KEEP_DEBUG:-}" ]; then
    strip_tool=${SKATE3_CROSS_LLVM_BIN:+$SKATE3_CROSS_LLVM_BIN/}llvm-strip
    for f in "${bins[@]}"; do
        "$strip_tool" --strip-debug "$appdir/usr/bin/$f"
    done
    echo "build-appimage: stripped debug info from the packed copies ($(du -ch "${bins[@]/#/$build_dir/}" | tail -1 | cut -f1) unstripped)"
fi

sed -e "s/@VERSION@/$version/" -e "s/^Name=.*/Name=$app_name/" -e "s/^Icon=.*/Icon=$stem/" \
    "$here/skate3recomp.desktop" > "$appdir/$stem.desktop"
desktop-file-validate "$appdir/$stem.desktop"
# 256x256, cropped from the repository's banner.png (testing: red background).
install -m 644 "$here/$stem.png" "$appdir/$stem.png"
install -m 644 "$here/$stem.png" "$appdir/.DirIcon"

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
# Real releases statically link these; their notices must ship. The CI stub
# build checks out the SDK without its nested submodules.
if [ -z "${SKATE3_APPIMAGE_STUB:-}" ]; then
    for required in FFmpeg_COPYING.LGPLv2.1 sdl3_LICENSE.txt fmt_LICENSE spdlog_LICENSE; do
        [ -f "$licenses/$required" ] || {
            echo "build-appimage: missing $required; run git submodule update --init --recursive" >&2
            exit 1
        }
    done
fi
commit=$(git -C "$repo" rev-parse HEAD 2>/dev/null || echo unknown)
sdk_commit=$(git -C "$sdk" rev-parse HEAD 2>/dev/null || echo unknown)
source_url=${SKATE3_SOURCE_URL:-$(git -C "$repo" remote get-url "${SKATE3_RELEASE_REMOTE:-fork}" 2>/dev/null \
    || git -C "$repo" remote get-url origin 2>/dev/null || echo unknown)}
cat > "$licenses/SOURCE.txt" <<EOF
$app_name $version (linux-aarch64)
Source: $source_url (commit $commit)
rexglue SDK commit: $sdk_commit
Third-party sources, including FFmpeg (LGPL), are in third_party/rexglue-sdk/thirdparty
at that commit. No Skate 3 game data is included.
EOF

mkdir -p "$out_dir"
name=$file_prefix-$version-aarch64.AppImage
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
