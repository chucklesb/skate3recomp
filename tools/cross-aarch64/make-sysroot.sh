#!/usr/bin/env bash
# Build an Ubuntu 26.04 arm64 sysroot for the linux-aarch64 cross build.
#
#   tools/cross-aarch64/make-sysroot.sh [sysroot-dir]
#
# The default directory is $SKATE3_AARCH64_SYSROOT, or
# out/sysroot/ubuntu-26.04-arm64. Downloads are cached in out/sysroot-cache.
#
# The base image is the one DroidDeck's runtime is built from
# (DroidDeck tools/ubuntu/base.env), so the binaries link against the same
# glibc and libstdc++ the device runs. Needs: curl or python3, gpgv, tar,
# dpkg-deb (all provided by `nix develop .#cross`).

set -euo pipefail

UBUNTU_BASE_URL=https://cdimage.ubuntu.com/ubuntu-base/releases/26.04/release/ubuntu-base-26.04.1-base-arm64.tar.gz
UBUNTU_BASE_SHA256=5a1906794ced63a71a8119c3f211ef5f0bbe0a243001b4bbd41fdf80c5b219fd
MIRROR=${SKATE3_SYSROOT_MIRROR:-http://ports.ubuntu.com/ubuntu-ports}
SUITES=(resolute resolute-updates resolute-security)
COMPONENTS=(main universe)

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
root=${1:-${SKATE3_AARCH64_SYSROOT:-$repo/out/sysroot/ubuntu-26.04-arm64}}
cache=$repo/out/sysroot-cache

stamp_input=$(cat "$here/packages.txt" "$here/sysroot.py" "$0"; echo "$UBUNTU_BASE_SHA256 $MIRROR ${SUITES[*]} ${COMPONENTS[*]}")
stamp=$(printf '%s' "$stamp_input" | sha256sum | cut -d' ' -f1)
if [ -f "$root/.skate3-sysroot-stamp" ] && [ "$(cat "$root/.skate3-sysroot-stamp")" = "$stamp" ]; then
    echo "make-sysroot: $root is up to date"
    exit 0
fi

mkdir -p "$cache"
base_tar=$cache/$(basename "$UBUNTU_BASE_URL")
if [ ! -f "$base_tar" ] || ! echo "$UBUNTU_BASE_SHA256  $base_tar" | sha256sum -c --status; then
    echo "make-sysroot: downloading $UBUNTU_BASE_URL"
    curl -fL --retry 3 -o "$base_tar.part" "$UBUNTU_BASE_URL"
    mv "$base_tar.part" "$base_tar"
fi
echo "$UBUNTU_BASE_SHA256  $base_tar" | sha256sum -c --status || {
    echo "make-sysroot: SHA256 mismatch for $base_tar" >&2
    exit 1
}

echo "make-sysroot: extracting base image into $root"
if [ -d "$root" ]; then
    chmod -R u+w "$root" 2>/dev/null || true
    rm -rf "$root"
fi
mkdir -p "$root"
tar -xzf "$base_tar" -C "$root" --no-same-owner --exclude='./dev/*'

keyring=
for k in usr/share/keyrings/ubuntu-archive-keyring.gpg usr/share/keyrings/ubuntu-archive-keyring.pgp; do
    if [ -f "$root/$k" ]; then keyring=$root/$k; break; fi
done
if [ -z "$keyring" ]; then
    echo "make-sysroot: no Ubuntu archive keyring in the base image" >&2
    exit 1
fi
if [ -f "$root/etc/apt/sources.list.d/ubuntu.sources" ]; then
    echo "make-sysroot: base image apt sources:"
    grep -E '^(URIs|Suites|Components):' "$root/etc/apt/sources.list.d/ubuntu.sources" | sed 's/^/  /'
fi

mapfile -t packages < <(grep -vE '^\s*(#|$)' "$here/packages.txt")

python3 -I "$here/sysroot.py" install \
    --root "$root" --cache "$cache" --keyring "$keyring" \
    --mirror "$MIRROR" --arch arm64 \
    --suites "${SUITES[@]}" --components "${COMPONENTS[@]}" \
    --gcc-from gcc \
    "${packages[@]}"

python3 -I "$here/sysroot.py" fix-links --root "$root"

echo "$stamp" > "$root/.skate3-sysroot-stamp"
echo "make-sysroot: done ($root)"
