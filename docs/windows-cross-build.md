# Windows x64 Cross Build from Linux (Experimental)

The same Nix setup can cross-compile a Windows x64 build. This is for testing
one commit on both Windows and linux-aarch64, not for releases (those use the
native Windows build and `tools/package_windows.ps1`). It uses the compiler mode
upstream uses on Windows: clang's GNU-style driver targeting the MSVC ABI,
linked with lld-link against the static MSVC runtime. Both the DirectX 12 and
Vulkan backends are built.

```sh
tools/cross-windows/build.sh            # release; or: build.sh relwithdebinfo
```

Steps (run them one at a time with `build.sh release <step>...`):

1. `winsdk`: `tools/cross-windows/make-winsdk.sh` uses
   [xwin](https://github.com/Jake-Shadle/xwin) to download the pinned MSVC CRT
   and Windows SDK into `out/sysroot/xwin`. **Running it accepts the Microsoft
   license terms for those packages.**
2. `codegen`: the same step as the aarch64 build. It is skipped when
   `generated/` already exists (set `SKATE3_FORCE_CODEGEN=1` to regenerate),
   so the Windows and aarch64 builds share the same recompiled sources.
3. `build`: configures and builds the `windows-x64-release` preset in
   `nix develop .#windows` (`cmake/toolchains/windows-x64.cmake`).
4. `stage`: copies `skate3.exe`, the runtime and GPU plugin DLLs and any PDBs
   to `<stage root>/<config>`. The TU3 patches go to one shared
   `<stage root>/game`, which the first stage also fills with the game dump.
   The stage root defaults to `out/stage/windows-x64`; set
   `SKATE3_WINDOWS_STAGE_ROOT` to stage elsewhere. To run the build on the
   WSL host, use a `/mnt/c/...` folder, because starting the game from
   `\\wsl.localhost` is slow.
   `tools/harness/run_scenario.py windows` runs scenarios against the stage, and
   `run_scenario.py ab` compares them with the linux-aarch64 build (see
   [scenario-harness.md](scenario-harness.md)).

`relwithdebinfo` builds with `-O2 -g` and writes PDBs next to the binaries
(`skate3.pdb`, `rexruntimerd.pdb`, `rexgpu-xenosrd.pdb`). The SDK adds an `rd`
suffix to its DLLs in that configuration. WinDbg or Visual Studio can then
show symbolized stacks for crashes, including the recompiled `sub_8xxxxxxx`
guest functions.
