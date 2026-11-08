#!/usr/bin/env bash
# Cross-build skate3recomp for Windows x64 from an x86_64 Linux (WSL) host.
#
#   tools/cross-windows/build.sh [release|relwithdebinfo] [step...]
#
# Steps (default: all, in this order):
#   winsdk   MSVC CRT + Windows SDK via xwin             (nix develop .#windows)
#   codegen  host x86_64 build of rexglue + generate-all (nix develop .#codegen);
#            skipped when generated/ exists unless SKATE3_FORCE_CODEGEN=1, so
#            a Windows build and a linux-aarch64 build share the same sources
#   build    cross-configure and build                   (nix develop .#windows)
#   stage    copy the exe, DLLs and PDBs to <stage root>/<config>, and the
#            TU3 patches (plus, once, the game dump) to <stage root>/game
#
# The result is for local A/B testing against the linux-aarch64 build, not
# for release (the upstream release path is tools/package_windows.ps1).
# tools/harness/run_scenario.py `windows` runs scenarios against the stage,
# passing --game_data_root=<stage root>/game.
#
# Environment:
#   SKATE3_GAME_DATA_ROOT      game dump (default <repo>/game)
#   SKATE3_WINDOWS_STAGE_ROOT  stage root (default out/stage/windows-x64).
#                              Running from \\wsl.localhost works but is
#                              slow; for this PC use a /mnt/c/... folder.
#   SKATE3_STAGE_GAME=0        don't copy the game dump into <root>/game when
#                              <root>/game/default.xex is missing
#   SKATE3_JOBS, SKATE3_CMAKE_FRESH  as in tools/cross-aarch64/build.sh

set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
cd "$repo"

config=${1:-release}
case "$config" in
    release|relwithdebinfo) shift || true ;;
    winsdk|codegen|build|stage) config=release ;;
    *) echo "build.sh: unknown config '$config'" >&2; exit 2 ;;
esac
steps=("$@")
[ ${#steps[@]} -eq 0 ] && steps=(winsdk codegen build stage)

game_root=${SKATE3_GAME_DATA_ROOT:-$repo/game}
jobs=${SKATE3_JOBS:-$(nproc)}
preset=windows-x64-$config
build_dir=out/build/$preset
stage_root=${SKATE3_WINDOWS_STAGE_ROOT:-$repo/out/stage/windows-x64}
stage=$stage_root/$config
stage_game=$stage_root/game
# The SDK names RelWithDebInfo libraries with an "rd" postfix; the GPU plugin
# loader looks for the matching name.
postfix=
[ "$config" = relwithdebinfo ] && postfix=rd
epoch=$(git -C "$repo" log -1 --format=%ct 2>/dev/null || date +%s)

in_shell() {
    local shell=$1; shift
    nix develop "$repo#$shell" --command bash -euo pipefail -c "export SOURCE_DATE_EPOCH=$epoch; $*"
}

for step in "${steps[@]}"; do
    echo "==> $step"
    case "$step" in
        winsdk)
            in_shell windows "tools/cross-windows/make-winsdk.sh"
            ;;
        codegen)
            if [ -f generated/sources.cmake ] && [ -f generated/eawebkit/sources.cmake ] \
                    && [ -z "${SKATE3_FORCE_CODEGEN:-}" ]; then
                echo "build.sh: generated/ exists; reusing it (SKATE3_FORCE_CODEGEN=1 to regenerate)"
                continue
            fi
            in_shell codegen "
                cmake --preset linux-host-codegen -DSKATE3_GAME_DATA_ROOT='$game_root'
                cmake --build --preset linux-host-codegen --target generate-all --parallel $jobs"
            ;;
        build)
            if [ ! -f generated/sources.cmake ] || [ ! -f generated/eawebkit/sources.cmake ]; then
                echo "build.sh: generated/ is missing; run the codegen step first" >&2
                exit 1
            fi
            mkdir -p "$build_dir"
            in_shell windows "
                cmake --preset $preset ${SKATE3_CMAKE_FRESH:+--fresh} -DSKATE3_GAME_DATA_ROOT='$game_root' 2>&1 | tee $build_dir/configure.log"
            if ! grep -q 'Graphics: D3D12=ON Vulkan=ON' "$build_dir/configure.log"; then
                echo "build.sh: expected both the D3D12 and Vulkan backends; see $build_dir/configure.log" >&2
                exit 1
            fi
            # Header casing symlinks for this tree and the fetched deps.
            in_shell windows "python3 tools/cross-windows/xwin-case-links.py \"\$SKATE3_XWIN_ROOT\" src third_party/rexglue-sdk/src third_party/rexglue-sdk/include third_party/rexglue-sdk/thirdparty $build_dir/_deps"
            in_shell windows "cmake --build --preset $preset --parallel $jobs"
            ;;
        stage)
            bins=(skate3.exe "rexruntime$postfix.dll" "rexgpu-xenos$postfix.dll")
            for f in "${bins[@]}"; do
                [ -f "$build_dir/$f" ] || { echo "build.sh: $build_dir/$f is missing; run the build step" >&2; exit 1; }
            done
            mkdir -p "$stage" "$stage_game/data/webkit"
            for f in "${bins[@]}"; do cp -f "$build_dir/$f" "$stage/"; done
            # skate3.pdb sits in the build dir; the SDK links its DLLs (and
            # their PDBs) into its own out/win-amd64 and only copies the DLLs.
            sdk_out=third_party/rexglue-sdk/out/win-amd64
            for f in "$build_dir/skate3.pdb" "$sdk_out/rexruntime$postfix.pdb" "$sdk_out/rexgpu-xenos$postfix.pdb"; do
                if [ -f "$f" ]; then cp -f "$f" "$stage/"; fi
            done
            # The build stages the TU3 patches next to the exe; the runtime
            # refuses to boot without them. They are the same for every config.
            for f in default.xexp data/webkit/EAWebkit.xexp; do
                if [ -f "$build_dir/game/$f" ]; then cp -f "$build_dir/game/$f" "$stage_game/$f"; fi
            done
            if [ ! -f "$stage_game/default.xex" ] && [ "${SKATE3_STAGE_GAME:-1}" != 0 ]; then
                if [ -f "$game_root/default.xex" ]; then
                    echo "build.sh: copying the game dump ($(du -sh "$game_root" | cut -f1)) into $stage_game"
                    cp -r --no-preserve=mode,ownership --update=none "$game_root/." "$stage_game/"
                else
                    echo "build.sh: $game_root/default.xex not found; put the game dump in $stage_game yourself" >&2
                fi
            fi
            echo "build.sh: staged $preset in $stage (game data: $stage_game)"
            ls -la "$stage"
            ;;
        *)
            echo "build.sh: unknown step '$step'" >&2
            exit 2
            ;;
    esac
done
