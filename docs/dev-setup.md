# Development setup

What a development machine needs, per host, and the one rule that keeps a
Windows + WSL setup working: **run everything on the side the code is on.**

## Hosts

| Host | Builds | Device harness | Windows A/B |
|---|---|---|---|
| **Linux x64 + nix** | everything: codegen, linux-aarch64 (releases), the Windows cross build ([linux-aarch64.md](linux-aarch64.md), [windows-cross-build.md](windows-cross-build.md)) | yes | no (needs a Windows machine to run the `.exe`) |
| **Windows + WSL2 (Ubuntu) + nix inside WSL** | everything, inside WSL | yes (Windows' `adb.exe` from WSL) | yes: the harness, in WSL, stages and launches the Windows build through WSL interop |
| **Windows without WSL** | upstream's native Windows build (`tools/package_windows.ps1`) | no | no |
| **macOS** | upstream's native build | not yet (the linux-aarch64 cross build is untested on macOS) | no |

The fork's tools (codegen, the cross builds, the harness, `tools/re/`) are
Linux programs run from a POSIX shell in the repo root. Builds go through
`nix develop` shells defined in `flake.nix`.

## Windows + WSL: work inside WSL

Keep the repo on the WSL filesystem (`~/dev/skate3recomp`) and run the
editor, the terminal and Claude Code **inside WSL**:
- the `claude` CLI installed and run in a WSL terminal, or
- VS Code connected to WSL ("WSL: Reopen Folder in WSL").

Don't work on the repo from the Windows side over `\\wsl.localhost`. Every
command then crosses PowerShell or Git Bash and `wsl.exe`, which re-quote
arguments and rewrite paths, and every file write goes over the 9P share,
which drops executable bits and can serve stale contents to a process that
runs right after an edit. Those were the source of a long list of traps
before this rule (2026-11-18).

From WSL, Windows programs still run directly: `adb.exe`, `tasklist.exe`,
`powershell.exe -NoProfile -Command "Start-Process ..."` (a game started
this way outlives the WSL shell). The harness uses this for the Windows
target. Stage Windows builds on a Windows drive
(`SKATE3_WINDOWS_STAGE_ROOT=/mnt/c/...`): running them from `\\wsl.localhost`
is slow.

## Per-machine settings

Copy [`.claude/local.env.example`](../.claude/local.env.example) to
`.claude/local.env` (gitignored) and fill in what applies: the adb binary and
port, the DroidDeck checkout (`DROIDDECK_DIR`, for its public
`droiddeckctl`), your game data, the Windows stage folder, RenderDoc, the
Skate 2 reference files. `tools/harness/rs.sh` reads it and runs the harness:

```sh
tools/harness/rs.sh device scenarios/boot_skip.toml    # a DroidDeck device
tools/harness/rs.sh windows scenarios/boot_skip.toml   # the Windows build (WSL hosts)
```

## The device

Any Android device running DroidDeck (Android 9+, Adreno 730+ or any
Snapdragon 8xx; others are experimental; [plan.md](plan.md#target-platform)).
The reference device is the AYN Odin 2. Connect it with USB debugging on;
on WSL hosts attach it to Windows and use Windows' `adb.exe` (one adb server
owns the USB device).

**WSL hosts: no Linux adb.** Uninstall WSL's own adb (`apt remove adb`, and
no `platform-tools` with a Linux `adb` on the PATH). With
`networkingMode = mirrored` a Linux adb server on port 5037 also takes Windows'
clients, and neither side sees the device. For typing adb commands, link
[`tools/harness/adb`](../tools/harness/adb) onto the PATH as `adb`
(`ln -s "$PWD/tools/harness/adb" ~/.local/bin/adb`). It runs Windows'
`adb.exe` and converts local paths (push sources, pull destinations,
install files) with `wslpath -w`; `shell` arguments pass through untouched.
Put the SDK's `platform-tools` (`/mnt/c/Users/<you>/AppData/Local/Android/Sdk/platform-tools`)
ahead of the Windows PATH so `adb.exe` and `fastboot.exe` are the SDK's: a
different adb version, such as the one bundled with scrcpy, kills the running
server when it connects. The harness itself uses `ADB` from `.claude/local.env`
and converts its own paths.

Alternative: attach the device to WSL with usbipd-win and use a Linux adb.
That needs no `.exe` or path conversion, but Windows tools (scrcpy,
`odin2_record.ps1`) lose the device while it's attached, and it has to be
attached again after every replug or reboot.

**Root is optional.** With root (`adb shell su`), the harness reads the
game's files on the device in under a second; without it, it opens a
DroidDeck session to read them (about 20 s). `SKATE3_ADB_ROOT=0` forces the
session path.

## Once per clone

```sh
tools/re/install-hooks.sh     # the pre-commit doc check (workflow.md, step 8)
git config core.fileMode false   # WSL only: ignore executable-bit noise
```

New scripts are added with `git add --chmod=+x`.
