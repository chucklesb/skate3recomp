{
  description = "skate3recomp build environments (host codegen + linux-aarch64 cross build)";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
      llvm = pkgs.llvmPackages_20;

      # Static aarch64 AppImage type-2 runtime (supports --appimage-extract,
      # zstd squashfs). Pinned to a dated release, not "continuous".
      appimageRuntime = pkgs.fetchurl {
        url = "https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-aarch64";
        sha256 = "00cbdfcf917cc6c0ff6d3347d59e0ca1f7f45a6df1a428a0d6d8a78664d87444";
      };

      commonTools = with pkgs; [
        cmake
        ninja
        python3
        git
        curl
        zip
        unzip
        file
      ];
    in
    {
      devShells.${system} = {
        # Native x86_64 build used only to run the rexglue codegen tool
        # (generate-all). Configuring the project still needs GTK3 and the
        # SDL3 video/audio headers, so they come from nixpkgs here.
        codegen = (pkgs.mkShell.override { stdenv = llvm.stdenv; }) {
          packages = commonTools ++ (with pkgs; [
            llvm.lld
            pkg-config
            wayland-scanner
            gtk3
            xorg.libX11
            xorg.libXext
            xorg.libXcursor
            xorg.libXi
            xorg.libXrandr
            xorg.libXfixes
            xorg.libXScrnSaver
            xorg.libXtst
            libxkbcommon
            wayland
            wayland-protocols
            libdecor
            libglvnd
            alsa-lib
            libpulseaudio
            pipewire
            systemdMinimal
            dbus
          ]);
        };

        # linux-aarch64 cross build against an Ubuntu 26.04 arm64 sysroot.
        # No compiler wrapper and no host libraries: everything the target
        # links comes from the sysroot (tools/cross-aarch64/make-sysroot.sh).
        cross = pkgs.mkShellNoCC {
          packages = commonTools ++ (with pkgs; [
            llvm.clang-unwrapped
            llvm.lld
            llvm.llvm
            pkg-config-unwrapped
            wayland-scanner
            dpkg
            gnupg
            gnutar
            qemu-user
            squashfsTools
            desktop-file-utils
            shellcheck
            gh
          ]);

          SKATE3_APPIMAGE_RUNTIME = "${appimageRuntime}";

          SKATE3_CROSS_CLANG = "${llvm.clang-unwrapped}/bin/clang";
          SKATE3_CROSS_CLANGXX = "${llvm.clang-unwrapped}/bin/clang++";
          # clang-unwrapped ships without its builtin headers (arm_neon.h,
          # stddef.h, ...); the wrapper package's resource-root has them.
          SKATE3_CROSS_RESOURCE_DIR = "${llvm.clang}/resource-root";
          SKATE3_CROSS_LLVM_BIN = "${llvm.llvm}/bin";
          SKATE3_CROSS_LLD = "${llvm.lld}/bin/ld.lld";
          SKATE3_CROSS_PKG_CONFIG = "${pkgs.pkg-config-unwrapped}/bin/pkg-config";
          SKATE3_CROSS_WAYLAND_SCANNER = "${pkgs.lib.getBin pkgs.wayland-scanner}/bin/wayland-scanner";

          shellHook = ''
            export SKATE3_AARCH64_SYSROOT=''${SKATE3_AARCH64_SYSROOT:-$PWD/out/sysroot/ubuntu-26.04-arm64}
          '';
        };

        default = self.devShells.${system}.cross;
      };
    };
}
