# The plan

What this fork is for, what "done" means, and how we get there. This page
changes rarely; what's being worked on now is in [roadmap.md](roadmap.md),
and how each piece of work is done is in [workflow.md](workflow.md).

## Why

The goal is to improve skate3recomp. Upstream has many bugs, most of them in
the emulated and native renderers. A bug can be worked around, but every
workaround is technical debt: a guess about what the game is doing, which
breaks the next time the guess is wrong. Fixing a bug properly means knowing
*why* it happens, and that means reverse engineering the game's code.

Code we understand, we can rewrite as our own: native C++ over named data,
in place of the recompiled code ReXGlue generated. Native code makes bugs
visible and fixes certain instead of guessed. And once the game is our own
readable code, we own it: a new feature or an improvement is ordinary code,
not a reverse-engineer-shim-and-hack job.

## Goals

| Goal | What it means | Done when |
|---|---|---|
| **G1. Fix issues properly** | Every fix starts from an understood root cause. The code the fix touches becomes native. A workaround ships only default-off (a comparison switch) and is listed in [re/workarounds.md](re/workarounds.md). | No open issue is fixed by a guess (rolling: it holds for every fix as it lands) |
| **G2. Own the game code** | Every recompiled function replaced by our own implementation over named types ([re/replacements.md](re/replacements.md), "The standard"). This is RE-7. | No recompiled (ReXGlue-generated) function is left in the build |
| **G3. Own the runtime** | The game runs on our own runtime: settings, logging, platform layer, app base, memory, threads, files, audio, input and graphics. | The binary links none of the ReXGlue runtime libraries (table below) |

G1 never really ends; G2 and G3 are long. Work on them is driven by fixes,
changes and features (G1), with planned coverage filling the gaps: at the
end of each fix we check how much of the touched subsystem is native and
decide whether to fill its gaps then or later ([workflow.md](workflow.md),
step 6).

## Target platform

Android devices running DroidDeck, which runs
our linux-aarch64 AppImage. DroidDeck's stated requirements: Android 9 or
newer on Adreno 730 or newer, or any Snapdragon 8xx; Adreno 6xx, Mali,
Xclipse and PowerVR are experimental, Adreno 710 unsupported (skate3recomp
has run on an Adreno 710 phone). The **reference device** is the AYN Odin 2
(Snapdragon 8 Gen 2, Adreno 740), the most common SoC among Android
handhelds: we optimize, test and release for it first.

Windows builds exist for A/B tests. The fork still builds for Windows, Linux
and macOS, all on Vulkan ([roadmap.md](roadmap.md), Direction).

## Phases (G2 and G3)

| Phase | What | Exit criterion |
|---|---|---|
| **P1. Map** | Names and layouts for the whole binary: the Skate 2 PDB transfer, Skate 3's own names, the RE toolkit (RE-1 to RE-3) | Every function belongs to a subsystem ([re/coverage.md](re/coverage.md) "unassigned" is 0) |
| **P2. Native over guest data** | Subsystem by subsystem, our own code in place of recompiled functions. The data still lives in guest memory, read through `rex::be<T>` structs. **We are here.** | Every subsystem but the library code is native |
| **P3. Library code** | The C runtime, memcpy and strings, EA's containers, math: functions with known behaviour, replaced by host equivalents (each one understood, never transliterated). The SDK already implements parts of the game's CRT natively (`src/kernel/crt`: heap, memory, string, threading, files). | No recompiled library function is left |
| **P4. Native-owned data** | Structures leave guest memory: native layouts, host pointers, no byte swapping | No game structure lives in guest memory |
| **P5. Own the runtime** | Remove the ReXGlue runtime pieces in dependency order (table below) | G3 |

Native-only rendering ([native-only.md](native-only.md)) is G1 and G2 for
the renderer; its phase 6 (removing the emulated renderer and D3D12) is the
first step of P5.

## The ReXGlue runtime (G3)

Our SDK fork ([chucklesb/rexglue-skate3](https://github.com/chucklesb/rexglue-skate3))
builds the runtime as two shared libraries, `rexruntime` and `rexgpu-xenos`,
plus object libraries (`rexcore`, `rexui`, `rexinput`, `rexaudio`,
`rexfilesystem`) and the code generator. Upstream ReXGlue names the same
pieces separately; both names are listed.

| Piece (upstream name) | What it provides | What replaces it | When it can go |
|---|---|---|---|
| Base (rexbase; ours: `rexcore`, part of `rexruntime`) | Settings (cvars), logging, the platform layer, `rex::Init` | Our own settings, logging and platform layer | Once nothing of ours includes `rex/cvar.h`, `rex/logging.h`, `rex/platform` |
| PPC (rexppc) | `PPCContext` (the guest register state every recompiled function receives), hardware register access routing | Nothing: native code has no guest registers | When the last recompiled function is gone (end of P3) |
| Xenos (rexxenos; ours: in `rexgpu-xenos`) | The Xenos GPU's register, format and texture layout definitions; the emulated GPU | The native renderer (RHI, Vulkan) | Native-only phase 6 |
| Kernel (rexkernel; ours: `src/kernel`, `src/system` in `rexruntime`) | The xboxkrnl exports, guest memory, threads, files, `TitleRuntime` (loads and runs the game) | Our own platform calls, called directly by native code | When native code no longer calls kernel exports (P3/P4) |
| XAM (rexxam; ours: `src/kernel/xam`) | The XAM exports: profiles, content, UI, achievements | Our own profile, save and UI services | With the kernel |
| XBDM (rexxbdm; ours: `src/kernel/xbdm`) | The debug monitor exports | Nothing (debug only) | Early: as soon as nothing calls them |
| App (rexapp; ours: `rex/rex_app.h`) | `App` and `TitleApp`, the program's base classes | Our own app base | Last, with the base |

The table is a first map from the SDK's layout and our includes
(2026-11-18); P5 starts with a measured list of what the game and our code
call in each piece.

## How progress is measured

- **Coverage** ([re/coverage.md](re/coverage.md), generated by
  `tools/re/coverage.py`): per subsystem and overall, how many functions
  are named, specified and native, and the native share of calls when a
  call-count run is given.
- **The RE items** ([re/README.md](re/README.md)): each with a goal, a
  "done when" and its status.
- **Known issues** ([known-issues.md](known-issues.md)): each fixed issue
  records its root cause and what became native.
