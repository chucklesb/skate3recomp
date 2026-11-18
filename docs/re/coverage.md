# Coverage

How much of the game is named, specified and native, per subsystem
([plan.md](../plan.md): P1 drives "Not yet assigned" to 0, P2-P3 drive
"Native" to all). **Generated** by `tools/re/coverage.py --write
[--counts RUN]`; regenerate it with each replacement (the workflow's step 7,
[workflow.md](../workflow.md)). Subsystems are defined in
[subsystems.toml](subsystems.toml).

- **Functions:** recompiled functions in the main image. **(inferred):**
  assigned by their named neighbours, not by a name of their own.
- **Named:** any name (hand map, Skate 3 binding, Skate 2 transfer; the
  Skate 2 names are leads, see [README.md](README.md)). **Specified:** a
  [symbols.toml](symbols.toml) entry.
- **Native:** our own implementation runs (a body in `src/native/`).
  **Hooked:** any body of ours, including hooks that only observe.

Call counts: `out/harness-runs/20261117-122607_callcount_plaza` (call-weighted columns: the subsystem's share of all calls, and how many of its calls reach native code).

| Subsystem | Functions | (inferred) | Named | Specified | Native | Hooked | Calls | Native calls |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| [frontend](ui.md): Frontend and HUD: APT movies, FE states, text | 3,360 | 2,509 | 939 (27.9%) | 44 | 35 | 54 | 3.1% | 2,138 (0.0%) |
| [boot](boot.md): Boot flow and the app tick | 805 | 698 | 111 (13.8%) | 9 | 0 | 12 | 0.0% | 0 (0.0%) |
| [characters](characters.md): Characters: skeleton, skinning, create-a-character, animation | 684 | 484 | 210 (30.7%) | 3 | 0 | 4 | 1.7% | 0 (0.0%) |
| [livingworld](characters.md): LivingWorld: pedestrians and ambient characters | 1,432 | 963 | 511 (35.7%) | 5 | 0 | 4 | 3.7% | 0 (0.0%) |
| [culling](culling.md): Culling | 32 | 3 | 32 (100.0%) | 3 | 3 | 3 | 0.0% | 0 (0.0%) |
| [render](../native-only.md): Game-side rendering: scene, render engine, D3D | 2,050 | 1,096 | 1,006 (49.1%) | 4 | 2 | 23 | 2.5% | 0 (0.0%) |
| [jobs](jobs.md): EA::Jobs and threads | 110 | 35 | 79 (71.8%) | 6 | 5 | 5 | 3.9% | 0 (0.0%) |
| physics: Physics and collision | 1,450 | 1,133 | 325 (22.4%) | 0 | 0 | 0 | 22.0% | 0 (0.0%) |
| audio: Audio | 2,254 | 1,774 | 486 (21.6%) | 2 | 0 | 1 | 3.9% | 0 (0.0%) |
| gameplay: Gameplay: skater behaviours, conditions, score, camera, input, AI | 5,956 | 4,620 | 1,505 (25.3%) | 7 | 0 | 9 | 3.8% | 0 (0.0%) |
| [career](career.md): Career and challenges | 805 | 691 | 168 (20.9%) | 13 | 0 | 9 | 0.0% | 0 (0.0%) |
| reels: skate.Reel replays | 294 | 239 | 80 (27.2%) | 2 | 0 | 1 | 0.0% | 0 (0.0%) |
| data: Game data: Attrib vaults, asset manager, XML | 709 | 497 | 217 (30.6%) | 0 | 0 | 2 | 2.7% | 0 (0.0%) |
| scripting: Lua scripting | 525 | 396 | 340 (64.8%) | 2 | 0 | 0 | 0.0% | 0 (0.0%) |
| online: Online: networking, sockets, EA services | 971 | 667 | 313 (32.2%) | 0 | 0 | 0 | 1.3% | 0 (0.0%) |
| storage: Saves and files | 331 | 129 | 203 (61.3%) | 0 | 0 | 0 | 0.0% | 0 (0.0%) |
| library: Library code: C runtime, EASTL, strings, allocators, math (P3) | 3,875 | 3,163 | 1,112 (28.7%) | 4 | 2 | 5 | 17.7% | 0 (0.0%) |
| unassigned: Not yet assigned (P1) | 22,009 | 0 | 1,523 (6.9%) | 28 | 7 | 59 | 33.6% | 0 (0.0%) |
| **All** | **47,652** | 19,097 | 9,160 (19.2%) | 132 | 54 (0.1%) | 191 | 100% | 2,138 (0.0%) |
