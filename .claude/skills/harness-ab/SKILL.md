---
name: harness-ab
description: A/B-test skate3recomp on Windows x64 against linux-aarch64 (a DroidDeck device; the Odin 2 is the reference) with the scenario harness. One command builds both targets from the same commit, runs a scenario on the device and on Windows D3D12 and Vulkan, and compares status, fps, state and paired screenshots. Use when asked to compare platforms or backends, check whether a bug is platform-specific, or verify that a change behaves the same everywhere.
argument-hint: "<scenario.toml> [--config relwithdebinfo|release] [--backends d3d12,vulkan] [--no-build] [--no-odin2] [--fps-cap N]"
---

# A/B runs: the device vs Windows

Arguments: `$ARGUMENTS`. Background: `docs/scenario-harness.md` ("A/B runs") and the `harness` skill (wrapper, device etiquette, writing scenarios).

## Run it

Needs a host that can run both (Windows + WSL, `docs/dev-setup.md`). With `run_in_background` (both builds plus three runs take several minutes):

```
tools/harness/rs.sh ab scenarios/<name>.toml [options]
```

- **Builds:** `ab` builds both targets through their `build.sh`, which reconfigures, so both report the same version (the table warns if they don't). `--no-build` skips that; only use it right after a build of the current commit.
- **Device:** the DroidDeck device must be connected. Tell the user before a run, because it takes over both the device and the PC's screen. `--no-odin2` runs the Windows backends only, and `--no-deploy` reuses the build on the device.
- **Frame cap:** `--fps-cap 30` (default) caps every target, because scenario frame counts only compare at equal frame rates. `--fps-cap 0` leaves them uncapped, which is only valid for scenarios that wait on state or time (`ms`, `gameplay`, `speed_gt`), not on frame counts.
- **Debug info:** `--config relwithdebinfo` gives `file:line` for `symbolize`.
- **Game arguments** go after `--` and apply to every target.

## Read the result

The command prints a table (target, status, build, frames, sec, fps, warnings, errors), then the tagged `state` rows per target. Then look at `out/harness-runs/ab-<stamp>_<name>/`:
- `compare.json`: the same data, machine-readable.
- `shots/<tag>/<target>_<n>.png`: open the same tag from each target with the Read tool. Copy the files to the scratchpad first if the UNC path fails.
- Each target's own run folder (`dir` in `compare.json`) has `log.txt`, `events.jsonl` and `state.jsonl`.

## What counts as a difference

- **Expected, not a bug:** the skater's spawn position, and anything that follows from it (speed after the same pushes, what's in view). The game spawns at the last location visited, and each target has its own save. A spawn-anywhere feature is planned on a separate branch.
- Small frame-count and fps differences are also expected; the device sits a little under the cap.
- **Worth reporting:**
  - status or failures differing between targets;
  - one target timing out, or crashing (status `crash`, with its top frames under FAIL; the run folder has `crash.txt`, plus `crash.dmp` on Windows; see "Crashes" in the `harness` skill);
  - FE screens (`fe`) diverging;
  - rendering differences in the paired shots of the same tag: missing geometry, shading, shadows or post effects.

  Compare D3D12 against Vulkan on Windows first. If they differ, it's a backend issue. If both Windows backends differ from the device, suspect aarch64 or the device's Vulkan driver (Turnip on Adreno).
- Error and warning counts differ by platform noise. Read `log.txt` in each run before calling a count difference meaningful.
