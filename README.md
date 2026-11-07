<picture>
  <source media="(prefers-color-scheme: dark)" srcset="banner.png">
  <source media="(prefers-color-scheme: light)" srcset="banner-light.png">
  <img alt="Skate 3 Native PC Recompilation" src="banner-light.png">
</picture>

An unofficial native recompilation of the Xbox 360 version of Skate 3, supporting Windows, Linux, and macOS.

As of v2.0.0, the game runs on a native renderer built directly on Direct3D 12 and Vulkan instead of emulating the Xbox 360 GPU. Compared to the emulated renderer it delivers more than twice the frame rate at roughly a quarter of the GPU power draw, and on Apple Silicon the frame rate uplift is closer to 10x.

The native renderer is still early and is likely to have issues; not every part of the game has been tested all the way through.

The project does not include Skate 3 retail game files. To run or build the project, you must provide files from your own legally obtained Xbox 360 copy of Skate 3.

Native Rendering Showcase (click to go to YouTube):

<p align="center">
  <a href="https://youtu.be/ETXCOsip1Uo">
    <img src="https://img.youtube.com/vi/ETXCOsip1Uo/maxresdefault.jpg" alt="Skate 3 Recomp native rendering showcase" width="420">
  </a>
</p>

## About This Fork

This is a continuation of [mchughalex/skate3recomp](https://github.com/mchughalex/skate3recomp), the original Skate 3 recompilation by mchughalex. The original repository has been inactive since July 2026, with open pull requests left unreviewed, so development continues here. All credit for the recompilation and the native renderer goes to the original author.

Changes in this fork so far:

- The PC settings are now also in the game's own Options menu, on Display, Graphics, View, PC Controls and System pages drawn by the game's UI. Settings that need a restart are staged and applied through the game's own Restart Required prompt.
- Fixed streamed textures briefly showing solid colors or another texture's content while they load in.
- Fixed Edit Skater showing red or patterned arms, see-through eyes and the wrong hair for a few seconds after skin-tone and other edits.
- Create-a-skater hair now matches the original shading: the strand highlight is restored, and the hair darkens in shadow along with the face and body.
- Moved to rexglue SDK 0.10.0. GPU emulation now ships as a separate library next to the executable.
- Fixed a Direct3D 12 crash after extended play: the shader-visible descriptor heap leaked binding-table slots during world streaming until it ran out, which led to device removal (fixed in the SDK fork).
- Fixed static shadows rendering pure black on frames with no dynamic shadow caster, such as in-game cutscene cameras.
- Enhanced Shadows and PCSS Shadows are back in Options > Graphics and Settings > Video, off by default. Upstream removed them because they added odd extra shadows; on DroidDeck's Turnip driver they look like ordinary hard or softened shadows, but cost about 5-6 ms of GPU time per frame on an Odin 2. The static shadow map is also capped at 16384 pixels wide on every backend, which Adreno GPUs require.
- Merged community fixes that were never reviewed upstream: Hall of Meat injury highlights and lens effects, seamless character fades (returning to a marker no longer fades the skater in as separate patches with visible seams where body pieces overlap), the shirt vanishing or lagging while its cloth sim is active, custom-map (SkateIT/ArenaBuilder) geometry and texture coordinates, and cross-platform drone/freecam input.
- Fresh recursive clones build again: the SDK's imgui submodule pointed at commits that were never published, and now fetches from a published fork.
- Building from source can use the title update patches already present in an installed game folder, so the separate title update package is no longer required.

Bug reports and pull requests are welcome; see [CONTRIBUTING.md](CONTRIBUTING.md).

## How Do I Play?

Notes:

- The Windows version is the most tested, followed by Linux, and then macOS.
- On some hardware configurations, you may have a better experience running the Windows version through a translation layer like Proton rather than the native Linux build itself.
- The macOS ARM build is experimental and more prone to issues.
- The Linux release is built on Ubuntu 24.04 and needs glibc 2.38 or newer. On older distributions, [build from source](#building-from-source).
- Releases are published on this fork's [releases page](https://github.com/rubensteezy/skate3recomp/releases).

### Windows

1. Download the latest release Skate3Recomp-Windows.zip from the [releases page](https://github.com/rubensteezy/skate3recomp/releases).
2. Extract it anywhere you like, to a folder you control.
3. Run skate3.exe.
4. Click "Select ISO" to select your legally obtained copy of Skate 3.
5. Wait for the installer to extract the game files.
6. Click "Start Game".

### Linux

1. Download the latest release Skate3Recomp-Linux.zip from the [releases page](https://github.com/rubensteezy/skate3recomp/releases).
2. Extract it anywhere you like, to a folder you control.
3. Run skate3.
4. Click "Select ISO" to select your legally obtained copy of Skate 3.
5. Wait for the installer to extract the game files.
6. Click "Start Game".

### macOS (ARM / Experimental)

1. Download the latest release Skate3Recomp-macOS.zip from the [releases page](https://github.com/rubensteezy/skate3recomp/releases).
2. Extract it anywhere you like, to a folder you control.
3. Run the game by opening the skate3recomp app. Game files, saves and settings are kept in the folder containing the app, so keep it in a folder you control rather than in Downloads or Applications.
4. The first time, right-click the app and choose Open, or approve it under System Settings > Privacy & Security, before macOS will allow it to run.
5. Click "Select ISO" to select your legally obtained copy of Skate 3.
6. Wait for the installer to extract the game files.
7. Click "Start Game".

## Native Renderer

Since v2.0.0 the game no longer relies on emulating the Xbox 360 GPU. A native renderer draws the game directly through Direct3D 12 or Vulkan, covering the entire game: gameplay, menus, HUD, loading screens, videos, and the photo, replay, skater, and park editors. It runs exact ports of the game's own material shading for the world, characters, vehicles, and water, so the image stays at close visual parity with the original console output while running far faster and more efficiently.

- The native renderer is on by default. Settings > Video > Renderer switches between Native and Emulated live, and F5 hot-toggles between them at any time. If the native renderer ever hits an unrecoverable error, the game falls back to the emulated renderer and shows a corner indicator; F5 retries the native path.
- On Windows, a single build ships both graphics API backends: Settings > Video > Graphics API selects DirectX 12 or Vulkan (applied with Apply & Restart; DirectX 12 is preferred by default). Linux uses Vulkan, and macOS uses Vulkan through MoltenVK.
- Optional enhancements beyond the original game live under Settings > Video: MSAA up to 8x, ambient occlusion, bloom, volumetric lighting, extended draw distance and world streaming, render scale up to 3x, and true ultrawide.

Known issues:

- Skate parks and park editing currently have some general visual parity / rendering issues

See [KNOWN_ISSUES.md](KNOWN_ISSUES.md) for the full list and the status of each fix.

## Installing DLC

To use DLC, you must provide package files from your own legally obtained Xbox 360 DLC.

Create a `dlc` folder either beside the executable, inside the installed game folder,
or in the user data folder. Place the DLC package files in that
folder and start the game.

## Saves and Portable Mode

Saved games live in the user data folder by default (`%APPDATA%\skate3`, i.e.
`AppData\Roaming\skate3`, on Windows). Two portable options are available:

- Create a `saves` folder next to the executable and the game keeps saved games
  there instead. The folder must exist before launch, and existing saves are not
  migrated automatically - copy them over from the user data folder.
- Create an empty `portable.txt` file next to the executable to keep all user
  data in the executable's folder.

## True Ultrawide

The builds include an experimental true ultrawide mode: the native renderer draws the world at your display's full aspect ratio (21:9 and wider) with a matching wider field of view, while the HUD and menus stay centered and undistorted. Enable it via the Aspect Ratio setting or `skate3_ultrawide = true`. It requires the native renderer; with the emulated renderer the game presents in standard 16:9. Rendering more of the scene costs proportionally more GPU time.

## Controls

- Standard Xbox controls using an Xbox controller are the preferred and main input method.
- PlayStation (DualShock/DualSense), Switch and most generic controllers are supported through the SDL controller backend: set Settings > Controls > Controller Backend to SDL and restart the game. Steam Input through XInput also works. On Linux and macOS the SDL backend is always used, so these controllers work out of the box.
- Keyboard controls can be enabled in the game settings menu.
- Press Escape on keyboard or (RB + Start) on the controller to open the game settings menu. The chord can be changed in Settings > Controls.

### Keyboard Keybinds

- Left stick: W/A/S/D
- Right stick: mouse movement
- A/B/X/Y: Space/C/E/F
- LT/RT: RMB/LMB
- LB/RB: Q/R
- Left stick press: Shift
- Right stick press: MMB
- Back/Start: Tab/Return

## Building from Source

All platforms build with CMake 3.25+, Ninja, and Clang 18 or newer - ReXGlue
requires Clang, so MSVC and Apple Clang are not supported. On Windows, install
[LLVM for Windows](https://releases.llvm.org/) and Ninja.

Alternatively on Windows, the Clang and Ninja that ship with Visual Studio work:
install the "C++ Clang tools for Windows" component, open an
"x64 Native Tools Command Prompt", put Visual Studio's LLVM folder first on
`PATH` (`set "PATH=%VCINSTALLDIR%Tools\Llvm\x64\bin;%PATH%"`), and add
`-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++` to the configure
commands below.

Clone with submodules:

```sh
git clone --recursive https://github.com/rubensteezy/skate3recomp.git skate3recomp
cd skate3recomp
```

If you already cloned without submodules:

```sh
git submodule sync --recursive
git submodule update --init --recursive --jobs "$(nproc 2>/dev/null || echo 4)"
```

The build-time codegen needs an extracted game dump containing `default.xex` and
`data/webkit/EAWebkit.xex`. Put that dump in `game/`, or pass a path with
`SKATE3_GAME_DATA_ROOT`.

The codegen also needs Skate 3 Title Update 3. One way to provide it is the
title update package, the same `TU_12K2276_000000C000000.00000000000O3` file
the in-game title update installer downloads. Place it in the repository root
under its original name, or pass a path with `-DSKATE3_TITLE_UPDATE_PACKAGE=`.
The build extracts the update patches and recompiles the TU3-patched
executables. The runtime requires the title update to be staged before the
game will boot. The game hooks target TU3 addresses, so the unpatched retail
image can't be built: if neither the package nor the staged patches below are
found, configure stops with an error.

If you don't have the package but already play the game through a release
build, point `SKATE3_GAME_DATA_ROOT` at that install's `game` folder instead:
the in-game title update installer extracts the two TU3 patch files
(`default.xexp` and `data/webkit/EAWebkit.xexp`) there. When no package is
given, the build checks those files against the exact TU3 payload hashes and,
if they match, uses them for the same TU3 codegen. The configure output says
`Skate 3 title update codegen enabled: TU3 patches installed in ...` when this
path is taken.

`generate-all` still prints some `Unresolved conditional branch` and
`cannot seal` lines with the title update applied; these are expected, and the
run is fine as long as it ends with `Done in ...`.

Generate the recompiled source first:

```sh
cmake --preset relwithdebinfo -DSKATE3_GAME_DATA_ROOT="$PWD/game"
cmake --build --preset relwithdebinfo --target generate-all --parallel
```

Then reconfigure so CMake sees the generated source lists and build:

```sh
cmake --preset relwithdebinfo -DSKATE3_GAME_DATA_ROOT="$PWD/game"
cmake --build --preset relwithdebinfo --parallel
```

For release packaging, use the `release` preset on Windows, the
`linux-release` preset on Linux, or the `macos-release` preset on macOS.
Build from the release tag so the version comes out right, then run
`powershell -File tools/package_windows.ps1` on Windows,
`tools/package_linux.sh` on Linux, or `tools/package_macos.sh` on macOS.
Each writes the release zip to `out/release`; the macOS script wraps the
build in an ad-hoc signed `skate3recomp.app`.

The native renderer's HLSL sources live in `src/native/shaders` and are
embedded into the build automatically. The matching Vulkan SPIR-V binaries are
pre-compiled offline with DXC and committed under `src/native/shaders/spirv`,
so building needs no shader tooling; the SPIR-V header only needs regenerating
after editing the HLSL.

## Ubuntu/Linux Build

These instructions target Ubuntu 24.04 LTS on x86_64. Other distributions need
the same toolchain shape: CMake, Ninja, Clang 20 or newer, Vulkan development
headers, GTK 3 development headers, X11 and Wayland development headers for the
SDL3 window layer, and SDL-compatible audio/input development packages.

ReXGlue requires Clang. Clang 20 is recommended on Ubuntu because it matches the
Linux toolchain used by the rexglue SDK CI and avoids Ubuntu 24.04's
Clang 18/libstdc++ `std::expected` feature-test mismatch.

Install LLVM's apt repository and dependencies:

```sh
sudo apt update
sudo apt install -y wget gnupg lsb-release software-properties-common
wget https://apt.llvm.org/llvm.sh
chmod +x llvm.sh
sudo ./llvm.sh 20
sudo apt install -y \
  git cmake ninja-build build-essential pkg-config p7zip-full \
  clang-20 lld-20 \
  libgtk-3-dev libx11-xcb-dev libxss-dev \
  libwayland-dev libwayland-bin wayland-protocols libxkbcommon-dev libdecor-0-dev \
  libvulkan-dev vulkan-tools mesa-vulkan-drivers \
  libasound2-dev libpulse-dev libpipewire-0.3-dev libudev-dev
```

Optional packages improve controller/input and diagnostics coverage in SDL:

```sh
sudo apt install -y libusb-1.0-0-dev libunwind-dev libibus-1.0-dev liburing-dev
```

Initialize submodules as described above, then configure, generate,
reconfigure, and build a development build:

```sh
cmake --preset linux-relwithdebinfo -DSKATE3_GAME_DATA_ROOT="$PWD/game"
cmake --build --preset linux-relwithdebinfo --target generate-all --parallel
cmake --preset linux-relwithdebinfo -DSKATE3_GAME_DATA_ROOT="$PWD/game"
cmake --build --preset linux-relwithdebinfo --parallel
```

Build a Linux release:

```sh
cmake --preset linux-release -DSKATE3_GAME_DATA_ROOT="$PWD/game"
cmake --build --preset linux-release --target generate-all --parallel
cmake --preset linux-release -DSKATE3_GAME_DATA_ROOT="$PWD/game"
cmake --build --preset linux-release --parallel
```

The release artifacts are:

```text
out/build/linux-release/skate3
out/build/linux-release/librexruntime.so
out/build/linux-release/librexgpu-xenos.so
```

## Linux aarch64 Cross Build (Experimental)

An x86_64 Linux host (WSL2 works) with [Nix](https://nixos.org/) and flakes
enabled can cross-compile a linux-aarch64 build. The binaries link against an
Ubuntu 26.04 arm64 sysroot (glibc 2.43, GCC 15 libstdc++), so they run on an
Ubuntu 26.04 arm64 userland or anything newer. That includes the DroidDeck
runtime on Snapdragon handhelds. The kernel must use 4K pages.

```sh
tools/cross-aarch64/build.sh            # release; or: build.sh relwithdebinfo
```

The script runs five steps, which can also be run one at a time with
`build.sh release <step>...`:

1. `sysroot`: `tools/cross-aarch64/make-sysroot.sh` downloads the pinned
   `ubuntu-base-26.04.1-base-arm64` image. It verifies the archive signatures
   with the keyring from that image and unpacks the `-dev` packages listed in
   `tools/cross-aarch64/packages.txt` into `out/sysroot/ubuntu-26.04-arm64`.
   No root or qemu is needed.
2. `codegen`: builds `rexglue` natively in `nix develop .#codegen` with the
   `linux-host-codegen` preset, then runs `generate-all`. The recompiled
   sources in `generated/` are the same for every target architecture.
3. `build`: configures and builds the `linux-aarch64-release` preset in
   `nix develop .#cross`. That preset uses
   `cmake/toolchains/linux-aarch64.cmake`: unwrapped clang/lld targeting
   `aarch64-linux-gnu` with the sysroot.
4. `package`: writes `out/release/Skate3Recomp-Linux-aarch64.zip`.
5. `appimage`: writes `out/release/Skate3Recomp-<version>-aarch64.AppImage`
   (see below) and validates it.

The game dump and Title Update 3 requirements are the same as for a native
build. On the device, the runtime needs `libgtk-3-0`, a Vulkan driver with
geometry shader and BC texture support (Turnip on Adreno), and a writable
`/dev/shm`.

### AppImage for DroidDeck

`tools/appimage/build-appimage.sh` packs the three binaries, a launcher
(`AppRun`), a desktop entry, an icon cropped from `banner.png`, and the
third-party license texts into a type-2 AppImage. It bundles no system libraries: GTK, the Vulkan loader and
driver, X11/Wayland and audio come from the host. That matters on DroidDeck,
whose own Turnip driver must be the one used.

[DroidDeck](https://github.com/Droid-Deck) installs it from **Desktop → Add App
→ GitHub** once AppImages are enabled under Setup → Launcher → Linux apps.
DroidDeck extracts the image once rather than mounting it.
`tools/appimage/validate.py` checks the image against DroidDeck's import rules,
asset-name matching and runtime library set. `build-appimage.sh` runs it
automatically.

The AppImage itself is read-only, so `AppRun` keeps everything the game
writes under `${XDG_DATA_HOME:-~/.local/share}/skate3`. It passes
`--game_data_root` pointing at the first of these that holds `default.xex`:

1. `$SKATE3_GAME_DATA_ROOT`
2. `~/.local/share/skate3/game`
3. `/storage/emulated/0/skate3recomp/game` (Android shared storage inside DroidDeck)

If none holds the game, it falls back to an empty `~/.local/share/skate3/game`
for the ISO installer. Logs go to `~/.local/share/skate3/logs`. Passing your
own `--game_data_root=` or `--log_file=` overrides these.

### Publishing aarch64 releases (fork workflow)

CI cannot build the game, because codegen needs your Skate 3 files. Release
binaries are therefore built locally and CI only validates and publishes them:

1. Add your fork as a remote (`git remote add fork <url>`), and run
   `gh auth login` (`gh` is in `nix develop .#cross`). The fork's default
   branch must contain `.github/workflows/release-aarch64.yml`.
2. Tag the release commit `v<upstream version>-aarch64.<n>`, for example
   `v2.1.0-aarch64.1`, then push the tag to the fork.
3. Run `tools/release/stage-release.sh v2.1.0-aarch64.1`. It builds from the
   tag, checks that the embedded version matches, and uploads the AppImage
   and zip (with `.sha256` files) to a draft release. It then starts the
   `release-aarch64` workflow, which re-validates the assets on an arm64
   runner, including a real `--appimage-extract`, and publishes the release.
   A failed check leaves the release as a draft.

#### Testing pre-releases

Test builds go out as GitHub pre-releases. DroidDeck's **Add App → GitHub**
install skips pre-releases, so they only reach testers who download the
AppImage and import it by hand. The testing AppImage is a separate app,
**Skate3Recomp Testing** with a red icon
(`skate3recomp-testing-<version>-aarch64.AppImage`). It installs next to the
release app, and shares its game files, settings and saves.

1. Tag the commit `v<upstream version>-testing.<n>`, for example
   `v2.1.0-testing.1`, and push the tag to the fork.
2. Run `tools/release/stage-prerelease.sh v2.1.0-testing.1`. It builds with
   `SKATE3_APPIMAGE_VARIANT=testing` and uploads the AppImage to a draft
   pre-release. It then starts the `prerelease-aarch64` workflow, which
   validates the image and publishes it as a pre-release, never as latest.

`SKATE3_APPIMAGE_VARIANT=testing` also works with
`tools/appimage/build-appimage.sh` for a local testing image.

The `cross-tooling` workflow exercises the toolchain, sysroot and AppImage
pipeline (both variants) with stub binaries on every change to these tools.

## macOS Build (Apple Silicon)

macOS builds use Homebrew LLVM - the `macos-*` presets expect the toolchain at
`/opt/homebrew/opt/llvm`. Vulkan is provided by MoltenVK: the build compiles the
Vulkan loader and MoltenVK revisions the SDK pins as submodules, and stages them
with the MoltenVK ICD manifest in a `vulkan` folder next to the executable.

```sh
brew install llvm cmake ninja vulkan-loader molten-vk
```

Configure, generate, reconfigure, and build a release:

```sh
cmake --preset macos-release -DSKATE3_GAME_DATA_ROOT="$PWD/game"
cmake --build --preset macos-release --target generate-all --parallel
cmake --preset macos-release -DSKATE3_GAME_DATA_ROOT="$PWD/game"
cmake --build --preset macos-release --parallel
```

The release artifacts are `out/build/macos-release/skate3`,
`librexruntime.dylib` and `librexgpu-xenos.dylib`, plus the `vulkan` folder
beside them.

## Running a Development Build

Run the built executable directly with a game dump. On Linux:

```sh
LD_LIBRARY_PATH="$PWD/third_party/rexglue-sdk/out/linux-amd64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  ./out/build/linux-relwithdebinfo/skate3 --game_data_root="$PWD/game"
```

On Windows:

```powershell
.\out\build\relwithdebinfo\skate3.exe --game_data_root="$PWD\game"
```

Keyboard-to-controller emulation is off by default; enable it in
Settings > Controls > Mouse & Keyboard Mode.

Fullscreen is on by default. Pass `--fullscreen=false` to start windowed.
On Windows, ultrawide displays are detected automatically in fullscreen. For an
ultrawide window, pass matching `--window_width` and `--window_height` values;
the native renderer then draws true widescreen frames at that aspect.

## rexglue Fork

`third_party/rexglue-sdk` is pinned as a Git submodule to the
`skate3-sdk-clean` branch of the Skate-specific rexglue fork, maintained
alongside this repository at
[rubensteezy/rexglue-skate3](https://github.com/rubensteezy/rexglue-skate3)
(continued from [mchughalex/rexglue-skate3](https://github.com/mchughalex/rexglue-skate3)).
Its imgui submodule comes from
[rubensteezy/imgui](https://github.com/rubensteezy/imgui) (branch
`rexglue-v1.92.5`: imgui 1.92.5 plus the `ImFontConfig::RasterizerGamma` font
patch the SDK's UI uses). Clone recursively or run:

```sh
git submodule sync --recursive
git submodule update --init --recursive --jobs "$(nproc 2>/dev/null || echo 4)"
```

The fork tracks the upstream rexglue releases (currently 0.10.0) and adds the
Skate 3 runtime, codegen, input, timing, the native renderer's Direct3D 12 and
Vulkan backends, the settings overlay, and the Linux and macOS fixes needed by
this project. GPU emulation is built as a separate library (`rexgpu-xenos`)
that ships next to the executable and the runtime library.

## Credits

- [mchughalex/skate3recomp](https://github.com/mchughalex/skate3recomp), the
  original Skate 3 recompilation and native renderer this fork continues.
- Community contributors whose upstream pull requests are merged here: zeezme,
  andrewnakas, elespink, and schillers14-lab.
- [rexglue SDK](https://github.com/rexglue/rexglue-sdk), the recompilation SDK
  used by this project.
- [Dear ImGui](https://github.com/ocornut/imgui), used for the settings overlay.
- [Xenia](https://github.com/xenia-project/xenia), whose Xbox 360 research and
  tooling have helped the broader recompilation ecosystem.
