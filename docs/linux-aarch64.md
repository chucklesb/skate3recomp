# Linux aarch64 Cross Build (Experimental)

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

`build.sh relwithdebinfo` builds with `-O2 -g`. The SDK libraries are then
named `librexruntimerd.so` and `librexgpu-xenosrd.so`, which the packaging
scripts pick up. The AppImage ships stripped copies, while the unstripped
files in `out/build/linux-aarch64-relwithdebinfo` symbolize addresses from the
device (`tools/harness/run_scenario.py symbolize`).

The game dump and Title Update 3 requirements are the same as for a native
build. On the device, the runtime needs `libgtk-3-0`, a Vulkan driver with
geometry shader and BC texture support (Turnip on Adreno), and a writable
`/dev/shm`.

## AppImage for DroidDeck

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

## Publishing aarch64 releases (fork workflow)

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

### Testing pre-releases

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
