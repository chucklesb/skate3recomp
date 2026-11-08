#!/usr/bin/env bash
# Cross-compile stand-ins for skate3, librexruntime.so and librexgpu-xenos.so
# so the AppImage pipeline can be tested without the game (CI has no Skate 3
# files, so it cannot run codegen). Uses the same sysroot and compiler flags
# as cmake/toolchains/linux-aarch64.cmake.
#
#   tools/appimage/stub-build.sh <out-dir> [version]
#
# Produces <out-dir>/{skate3,librexruntime.so,librexgpu-xenos.so} and
# <out-dir>/include/skate3_version.h, the inputs build-appimage.sh reads.
# Run inside `nix develop .#cross` after make-sysroot.sh.

set -euo pipefail

out=${1:?usage: stub-build.sh <out-dir> [version]}
version=${2:-0.0.0-stub}
mkdir -p "$out/include"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc=(
    "$SKATE3_CROSS_CLANG" --target=aarch64-linux-gnu --sysroot="$SKATE3_AARCH64_SYSROOT"
    "-resource-dir=$SKATE3_CROSS_RESOURCE_DIR" -fuse-ld=lld "--ld-path=$SKATE3_CROSS_LLD"
    -O2 -Wall -Werror
)

cat > "$work/runtime.c" <<'EOF'
int rex_runtime_answer(void) { return 42; }
EOF
cat > "$work/gpu.c" <<'EOF'
int rex_runtime_answer(void);
int rex_gpu_answer(void) { return rex_runtime_answer() + 1; }
EOF
cat > "$work/main.c" <<EOF
#include <stdio.h>
int rex_gpu_answer(void);
static const char title[] = "[v$version-Release]";
int main(void) {
    printf("%s %d\n", title, rex_gpu_answer());
    return 0;
}
EOF

# shellcheck disable=SC2016 # a literal $ORIGIN for the dynamic linker
rpath='-Wl,-rpath,$ORIGIN'

# SKATE3_STUB_POSTFIX=rd mimics a RelWithDebInfo build: rd-suffixed libraries
# and DWARF debug info, as the SDK's CMAKE_RELWITHDEBINFO_POSTFIX produces.
p=${SKATE3_STUB_POSTFIX:-}
[ -n "$p" ] && cc+=(-g)

"${cc[@]}" -shared -fPIC -o "$out/librexruntime$p.so" "$work/runtime.c"
"${cc[@]}" -shared -fPIC -o "$out/librexgpu-xenos$p.so" "$work/gpu.c" \
    -L"$out" "-lrexruntime$p" "$rpath"
"${cc[@]}" -o "$out/skate3" "$work/main.c" -L"$out" "-lrexgpu-xenos$p" "-lrexruntime$p" \
    "$rpath" -Wl,--dynamic-linker=/lib/ld-linux-aarch64.so.1

cat > "$out/include/skate3_version.h" <<EOF
#define SKATE3_VERSION_STRING "$version"
EOF
echo "stub-build: wrote stub binaries for $version to $out"
