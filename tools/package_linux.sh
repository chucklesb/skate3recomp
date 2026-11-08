#!/bin/sh
# Package a linux-release build as Skate3Recomp-Linux.zip.
#
# Run from the repository root after building the linux-release preset:
#   tools/package_linux.sh [build-dir]
#
# build-dir defaults to out/build/linux-release. The zip holds a
# Skate3Recomp-Linux folder with the executable, the runtime library and the
# GPU plugin library, and is written to out/release. SKATE3_PACKAGE_NAME
# overrides the folder/zip name (e.g. Skate3Recomp-Linux-aarch64).

set -eu

build_dir=${1:-out/build/linux-release}
# RelWithDebInfo builds name the SDK libraries with an "rd" postfix.
postfix=
[ -f "$build_dir/librexruntimerd.so" ] && postfix=rd
files="skate3 librexruntime$postfix.so librexgpu-xenos$postfix.so"

for f in $files; do
    if [ ! -f "$build_dir/$f" ]; then
        echo "package_linux: $build_dir/$f is missing; build the linux-release preset first" >&2
        exit 1
    fi
done

name=${SKATE3_PACKAGE_NAME:-Skate3Recomp-Linux}
stage=out/release/$name
rm -rf "$stage" "out/release/$name.zip"
mkdir -p "$stage"

for f in $files; do
    cp "$build_dir/$f" "$stage/"
    chmod 755 "$stage/$f"
done

(cd out/release && zip -q -r -X "$name.zip" "$name")
echo "package_linux: wrote out/release/$name.zip"
