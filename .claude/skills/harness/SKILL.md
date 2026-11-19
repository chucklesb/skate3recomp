---
name: harness
description: Run skate3recomp scenario-harness scenarios on a DroidDeck device (the Odin 2 is the reference device) or the Windows build, or drive a live game session - run a scenario and review its result, screenshots and state; launch live mode; send live commands; fetch the latest run. Use when asked to reproduce a bug, test a change in-game, take in-game screenshots, or run a scenario.
argument-hint: "run <scenario.toml> [--config relwithdebinfo|release] [--no-deploy] [--env K=V] | live | send <verb> [k=v...] | fetch | summary <run> | symbolize <addr>..."
---

# Scenario harness

Reference: `docs/scenario-harness.md` (scenario format, verbs, run folder);
host setup and per-machine settings: `docs/dev-setup.md`. Arguments:
`$ARGUMENTS`.

## How to call it

From the repo root, in the project's shell (Linux, or inside WSL on a
Windows host; see `docs/dev-setup.md`):

```
tools/harness/rs.sh <subcommand> [args]
```

`rs.sh` loads `.claude/local.env` (adb, DroidDeck, the Windows stage paths,
RenderDoc) and nix, then runs `tools/harness/run_scenario.py`. If a run says
a path or tool is missing, the fix is usually a line in `.claude/local.env`
(copy `.claude/local.env.example`).

- Builds and deploys take minutes: use the Bash tool's `run_in_background`
  with the command in the foreground (never a trailing `&`).
- Wrap every live `send` in `timeout N`, so a dead game can't hang the call:
  `S() { timeout 60 tools/harness/rs.sh send --target windows "$@" 2>&1 | cut -c1-200; }`.
- Never discard a run's stderr (`2>/dev/null`): failures then look like
  silent passes.
- **Reusable helpers go in `tools/harness/`** (listed in
  `docs/scenario-harness.md`, Helper tools: `shots.py`, `video_frames.py`,
  `imgdiff.py`, `char_jitter.py`, ...). Check there before writing one; the
  scratchpad is only for one-off glue.
- Python patch scripts that contain backslashes: write them to a file with
  the Write tool and run the file, rather than a shell heredoc.

**Build only (no run)**, with `run_in_background`:
- Windows: `tools/cross-windows/build.sh relwithdebinfo build stage` (stages
  into `SKATE3_WINDOWS_STAGE_ROOT`, from `.claude/local.env`; source it or
  run the build through a shell that has it). After it, `windows ...
  --no-build` runs that stage.
- aarch64: `tools/cross-aarch64/build.sh <release|relwithdebinfo> build`.

## Subcommands

- `device scenarios/<name>.toml` (also `odin2`, the historical name; the
  `--target` of `live`, `send` and `symbolize` takes either):
  - It deploys the current `out/build/linux-aarch64-relwithdebinfo` build (or `--config release`) as the testing AppImage, runs the scenario, dismisses DroidDeck's "The session ended" dialog, fetches the run folder to `out/harness-runs/<stamp>_<name>/` and prints a summary.
  - It builds only when build inputs changed (not for docs/scenarios/tools/harness edits) and deploys only when the device's `payload-key` differs, so plain `device` is cheap when nothing changed; no need to remember `--no-deploy`. `--force-deploy` reinstalls anyway.
  - `--no-deploy` reuses the build already on the device.
  - `--env K=V` sets session environment for this run only.
  - Game flags go after `--`.
  - No manual rebuild needed: changed sources are built automatically before deploying.
- `live [--no-deploy]` starts the game in live mode (inbox + Ctrl+Shift+R recording).
- **The installed DroidDeck app in live mode** (what the user plays, not the harness copy in `/root/skate3-harness`): the inbox is in the game's data dir (`/root/.local/share/skate3/harness`), so `send` works with any app dir. Find the app with `adb shell "su -c 'cat <linuxfs>/opt/appimages/user/*/name'"` (`<linuxfs>` = `/data/data/com.droiddeck.launcher/files/linuxfs`; testing = "Skate3Recomp Testing", the GitHub release = "skate3recomp", which also has `github-tag`), then:
  `$DROIDDECK_DIR/tools/droiddeckctl run /usr/local/bin/droiddeck-appimage-run /opt/appimages/user/<id> --skate3_harness=true --skate3_demo_path=true --skate3_demo_path_signed_in=true`
  (`run` brings DroidDeck to the front itself). The demo path skips the movie, logo, language select and title with no presses; then `send wait gameplay=true timeout_s=240`. Installed versions: `strings <linuxfs>/opt/appimages/user/<id>/app/usr/bin/skate3 | grep -m1 "Skate 3 \[v"`. Update the testing app with `tools/release/build-testing.sh --install`.
- `send <verb> key=value ...` sends one live command and prints the JSON reply. Examples:
  - `send query what=state`
  - `send screenshot tag=x count=5 every_frames=2`
  - `send input buttons=a ly=1 frames=30`
  - `send record start=true`
- `fetch` copies the device's latest run folder.
- `pull <guest path> [--to dir]` copies any guest file or folder (settings.toml, logs, saves) to `out/device-files/`.
- `reels [--pull [DIR]] [--install-windows]` lists the device's reels (clip, date, id, duplicate warning), and optionally copies them off and into the Windows test profile (with a backup).
- **These three read the guest's files:** with root on the device (`adb shell su`) in about a second, without it through a DroidDeck session (~20 s; `SKATE3_ADB_ROOT=0` forces it). Writes into the guest always go through the session.
- `summary <run-dir>` reprints a fetched run's result.
- `--config relwithdebinfo` on `device` or `live` deploys `out/build/linux-aarch64-relwithdebinfo` instead of release. Build it first with `tools/cross-aarch64/build.sh relwithdebinfo build`.
  - The AppImage ships stripped copies. The unstripped build-dir files are what `symbolize` reads.
  - `out/harness/deployed-config` records what is on the device. `--no-deploy` warns if it differs from `--config`.
  - `result.json` has `version`, `platform` and `build_config`, and the summary prints them.
- `symbolize [--target device|windows] [--config c] <addr>...` prints the function and `file:line` for each address, inlined frames included:
  - Address forms: `module+0xOFFSET` (offset from the module's load base on Linux, the RVA on Windows; module names like `skate3`, `librexruntimerd.so`, `rexgpu-xenos.dll`), or a bare `0xADDRESS` inside skate3's own address space.
  - `--config` defaults to relwithdebinfo (testing builds: -O3 + debug info, the tree build-testing.sh packages); symbolize defaults to the deployed config for the device.
  - Release builds give function names at best: the ELF symbol table on aarch64, and only exports on Windows, which has no PDB. Use RelWithDebInfo for `file:line`.

## Windows (WSL hosts)

`rs.sh` drives the Windows x64 build too, from inside WSL: it stages the
build into `SKATE3_WINDOWS_STAGE_ROOT` (`<config>/` binaries, a shared
`game/`) and launches the `.exe` through WSL interop.

- `windows scenarios/<name>.toml [--config c] [--backend vulkan|d3d12] [--no-build] [--timeout s]` (vulkan is the default, as on the device) builds and stages with `tools/cross-windows/build.sh` unless `--no-build`, runs the game, and moves the run folder to `out/harness-runs/`.
  - `--display windowed` (default) starts it windowed and minimized without taking focus, audio on; `--display headless` keeps it hidden and muted; `--display fullscreen` is the normal window. A `window show = true` step (or live `send --target windows window show=true`) restores and shows it with audio, `show = false` hides and mutes it again (`scenarios/window_smoke.toml`). It refuses to start if a `skate3.exe` is already running, so ask the user before killing one.
  - A desktop PC runs it uncapped at well over 100 fps, so frame-counted inputs behave differently from the device at 30: cap with `-- --skate3_guest_fps_cap=30 --skate3_guest_fps_cap_auto=false` or a scenario `[cvars]` block (`boot_to_plaza` has one).
- `live --target windows [--backend b]` starts live mode; the game is started through PowerShell's `Start-Process`, so it outlives the command. Then `send --target windows <verb> ...`; `send --target windows exit code=0` closes the game.
  - To reach gameplay and a place: `send wait gameplay=true timeout_s=240`, `send wait frames=90`, `send teleport location=Z_DT_CorePlaza` (names in `docs/teleport-locations.md`).
  - **Teleport to coordinates:** `send teleport "pos=[x,y,z]" yaw=<deg>` with `pos`/`yaw` copied from a `state` reply. That's how to get back to a spot the user found (capture `send state` while they stand there). Far targets (>60 m) hop through the nearest vetted locator first, automatically. A bare sequencer move with no hop falls through the world, because the area isn't streamed yet. Quote the `pos=[...]` argument; `send` passes `[...]`/`{...}` values through as TOML arrays/tables.
  - A delayed screenshot: `sleep N`, then `send screenshot tag=<t>`; the file lands in the live run folder's `shots/`. Skate's camera recenters behind the skater when the right stick is released, so a delayed shot may not show what the user was looking at; offer an immediate one.
- `symbolize --target windows --config relwithdebinfo <module+0xRVA>` resolves Windows addresses through the PDBs.
- The Ctrl+Shift+R record hotkey doesn't work on Windows. Use `send --target windows record start=true` and `start=false`.
- For a comparison against the device, use the `harness-ab` skill.

## Menu navigation (live `send input`, verified 2026-11-13)

Inputs: `input buttons=start|a|b|x|y frames=4-6`, stick `input ly=-1.0 frames=4` (down; ly=1 up), `lx=1.0` (right). Wait 1-3 s between presses (menus animate); check with `screenshot tag=x` (read `shots/*.png` with the Read tool) and `query what=state` (`fe` = frontend stack).
- **Pause menu** (Start from free skate) = FE [0,56], "Career > Main": Challenge Map, Edit Skaters, skate.Profile, Call Skater, skate.Feed, Free Play, Party Play.
- **Pause tabs:** stick right/left (`lx=±1.0`; RB does NOT switch them): Career > Main (star), Xbox LIVE (globe), Create, (school), (settings). **Create** tab: Replay Editor, skate.Reel (A opens a submenu: My Replays, My Photos, Community Videos/Photos), skate.Park, Activate Skate Share Pack. My Replays = FE [0,56,59], a grid of reel thumbnails (Y Delete, A Select = loads the reel: [..,52] then the editor [..,12]; X in the editor = Take Photo → "Save Photo?" No/Yes, Yes writes the save).
- **Edit Skaters** (down 1, A) = team screen FE [0,56,63]: portrait boxes (the first is the saved skater photo), Edit Skater / Change Name / Import Skater.
- **Create-a-Skater** (A on Edit Skater) = FE [0,56,63,15]: Body Mods, Merchandise (Hats, T-Shirts, ... Skateboards), Edit Style, Trucks, Wheels.
  - Hats: the grid's first cell removes the hat; others put one on, then an "Edit Clothing" screen (A to accept).
  - Leaving (B until "Are you finished editing your skater?" → A = Yes) SAVES the profile and regenerates the skater photo. Saves aren't sacred in this dev environment (Windows and the device): no need to ask, but undo test edits where cheap.
  - Scripted: `scenarios/cas_portrait.toml`.
- Other FE ids seen (device census): 1 = challenge screen, 2 = photo-challenge camera, 69 = photo review, 64 = after a challenge, 53 = Create-a-Park menu(?), 59 = My Replays, 36 = a My Replays sub-screen, 12 = replay editor, 17 = loading / challenge map, 52 = loading screen.

## Reel repros (preferred over input recordings when the bug shows in a replay)

1. Live session at the spot.
2. The user skates, saves a Reel, and optionally sets a marker. **Never pick "Save Replay" yourself.**
3. `reels` (no game needed) or `send query what=reels` lists clip / **id** per reel; the new one is the last clip. For a Windows repro: `reels --pull --install-windows` (writing saves is fine in this dev environment).
3a. Find the marker time with a quick `play_reel from = "marker"` run (`playing from X s` in the log) before writing `stills` times (absolute reel seconds).
4. Write the scenario: `play_reel reel = "<id prefix>" from = "marker"`, then stills or `every_frames`, plus `stills_variants` for A/B.
5. Note the reel id and the scenario in the issue's "How to see it" in `docs/known-issues.md`.

Notes:
- **Ids are stable; clip positions shift** when reels are saved or deleted.
- Identical duplicates share an id; use `clip` for those.
- A reel only exists in the save of the machine it was recorded on. To use it elsewhere, copy the whole REPLY_SKATER folder (INDEX is hashed): `reels --pull --install-windows`. **Ask the user before writing the device's saves.**
- `stills_variants` entries use `tag`, and each `set` carries over to the next variant (restore the previous one in the next `set`).
- `play_reel` hides the reel camera's fisheye and vignette by default, on both renderers (`lens = true` keeps them). The game's loading screen shows during the load; the screen is black between it and the first played frame (overlay only, never in screenshots). With `every_frames`, the load is shot too (`<tag>_load`).
- Details: `docs/scenario-harness.md` → Reels as repros.

## Performance

**Before/after and A/B comparisons run uncapped.** At the 30 fps cap (most
scenarios set it in `[cvars]`, and the user plays at 30 for battery and heat)
the fps reads 30 on both sides and hides the difference. Pass `--uncapped`
(`device`, `windows`, `ab`, `live`): it adds `--skate3_guest_fps_cap=0
--skate3_guest_fps_cap_auto=false`, and flags after `--` win over a
scenario's `[cvars]`. The summary line says what the run was paced to
(`frame time (in game), uncapped: ...` or `cap 30`); check it before
comparing.
- **Report:** fps avg, 1% and 0.1% lows, p50/p99/max frame time, jitter,
  hitches and busy p99: `tools/harness/frametime_report.py <base> <new>`
  (A/B table; `--fail-pct 10` flags a regression). Consistency (p99, jitter,
  hitches) counts as much as the average.
- **Also at the cap, when the question is power or heat:** a capped run's fps
  says nothing; compare busy time (`busy p99`; the profiler's ms per frame).
  Work saved at 30 fps is battery and thermals.
- **Uncapped, frame-counted steps change:** input holds and `wait frames`
  last less wall time, so a scenario can play differently (the skater
  rolls less far). Use reels (`perf_reel.toml`) or segments whose content
  doesn't depend on frame counts for fps comparisons.
- **The Odin 2's state is part of the measurement:**
  - The harness sets Performance / Sport before every run (see "Odin 2
    modes" below); check the summary's `device:` line on both sides.
  - Thermals: start each run cool, `$DROIDDECK_DIR/tools/droiddeckctl wait
    cool --below 35 --timeout 600`, and log `droiddeckctl thermal` (battery
    °C, CPU caps) with each run. Run A B B A (or more), never all of A then
    all of B: a warm device throttles whichever runs second.
  - Repeat; one run is an anecdote. Report the spread.
- **Same everything else:** scenario, extras (enhanced shadows, PCSS),
  resolution, build config (relwithdebinfo), backend.

**Odin 2 modes.** Every device run and `live` session sets the test modes
first: performance **Performance** and fan **Sport** (the user's choice;
`ensure_test_modes` in run_scenario.py logs any change, the run folder gets
`device_modes.json`, the summary prints `device: ...`). They are left set
afterwards. `SKATE3_DEVICE_MODES=keep` in the environment of `rs.sh` skips
this for a test of another mode. By hand (Android system settings, applied at
once; set the performance mode first, it rewrites the fan mode):

| `performance_mode` | `fan_mode` choices |
|---|---|
| 0 Standard | 0 Off, 1 Quiet |
| 1 Performance (test default) | 1 Quiet, 4 Smart (+ `smart_fan_mode_switch=1`), 5 Sport (test default) |
| 2 High Performance (every CPU cluster pinned at max) | 4 Smart, 5 Sport, 6 Customize (slider = `fan_speed`, the PWM duty out of 50000; max 35000) |

`adb shell settings get|put system performance_mode|fan_mode`; the fan's
state is in `/sys/class/gpio5_pwm2/{duty,speed}` (root). The quick-settings
tile labels refresh only when the shade opens.

- **Benchmark:** `scenarios/perf_reel.toml` plays reels 2 and 4 with the CPU stage log and GPU timestamp buckets on.
- **Where the numbers go:** each run's summary prints per-reel fps, CPU render/build ms and GPU native pass ms, and writes `perf.json`.
- **Compare two runs:** `run_scenario.py perf <base-run> <new-run>`.
- **Across targets:** `ab scenarios/perf_reel.toml` adds a per-target perf table.
- **Before/after for a renderer change:** run the benchmark on the old and the new build (same backend and same settings), then `perf`.
- **Vulkan caveat:** `span` includes the cap's idle time; compare `nat_total` and the `Nat*` buckets instead.
- Details: `docs/scenario-harness.md` → Performance.

## After a run

- Read `result.json` for status, failures and counts, and `events.jsonl` for the timeline (steps, `fe` screen changes, content opens).
- `state.jsonl` holds skater position/speed, camera and the FE stack.
- View the PNGs in `shots/` with the Read tool (copy them to the scratchpad first if the UNC path fails).
- Exit codes: 0 pass, 1 step failed, 2 timeout, 3 scenario didn't parse, 4 no result (killed), 5 crash.

## Crashes

- A run that crashed reports `crash (code 5)` with the faulting address and the top frames resolved as `module+offset  function  file:line`. This works on both targets.
- Use `--config relwithdebinfo` to get line numbers. Release builds give function names only on aarch64 and only exports on Windows.
- Files in the run folder: `crash.txt` (raw frames; on aarch64 also `/proc/self/maps`), `build.json`, and on Windows `crash.dmp`. Offer the user the dump to open in WinDbg or VS with the PDBs in the stage folder (`$SKATE3_WINDOWS_STAGE_ROOT/relwithdebinfo`).
- To dig deeper, run `symbolize --target <device|windows> --config <c> <module+0x…>` on frames from `crash.txt`.
- Recompiled frames show as `sub_8xxxxxxx` (`__imp__sub_…` on aarch64; same function) with `generated/skate3_recomp.N.cpp:line`. The `8xxxxxxx` part is the guest address, so the user can look it up in their disassembly.
- To reproduce a crash on the other platform, run the same scenario through `harness-ab`. The guest call chain should match across targets.
- `scenarios/crash_smoke.toml` and `scenarios/crash_guest.toml` crash on purpose (`allow_crash = true`). Use them to check the capture itself still works after changing the harness or the SDK's exception handling.

## Writing a scenario for a bug

Start from `scenarios/boot_to_plaza.toml`:
- `wait gameplay`, then `wait frames=90` before a `teleport location=<name>`; a teleport sent right after gameplay starts is ignored. Names are in `docs/teleport-locations.md`.
- Drive to the situation with `input` steps:
  - on the board: A pushes, the left stick steers, the right stick does Flick-it;
  - off the board: the right stick moves the camera.
- Screenshot bursts around the moment.
- `expect` or `no_log` for the pass condition.

## Gameplay-feel bugs (jitter, parts flying off): the user plays, we record

Scripted inputs are poor at these: the skater isn't on the board, or rides off stairs. The reliable repro is the user's own play on the device. At 30 fps these bugs show constantly; on a PC at ~150 fps they're rare.

1. Ask the user for the start spot. Then:
   - `live --config <c> [--no-deploy] -- --skate3_demo_path=true --skate3_demo_path_signed_in=true --skate3_native_render_scene_char_track=true`;
   - `send wait gameplay=true timeout_s=240`, then `send wait frames=90`, then `send teleport location=<spot>`.
2. `send record start=true note=<what>`. The user plays and stops the recording on the device; `send record start=false` also stops it.
3. `fetch` pulls the live run folder, with the `.s3rec` and `log.txt`.
4. Copy the `.s3rec` next to a new scenario. The scenario does:
   - the same teleport;
   - `play file="x.s3rec" async=true`;
   - waits and screenshots;
   - `expect no_log` or `log` on a diagnostic line.
   `rs.sh device` pushes the referenced `.s3rec` automatically.
5. **Before/after on one build:** put the fix behind a cvar, then run the scenario with it `=false` and `=true`.
   - **Input replays drift.** Pedestrians and traffic differ between runs, and the timing differs by frame rate and machine. A replayed run can bail or take another line: in KI-120 one run had 19 fast frames where another had 170.
   - So compare speed groups, not frames.
   - The fairest A/B is **one live session on the device**, switching the cvar every ~12 s while the user skates: `send cvar set.<cvar_name>=<value>` (a TOML dotted key is the `set` table). Log the mode on each diagnostic line (char_track has `sm=`), and split the analysis by it. Script: a loop of `send cvar` and `sleep 12`, ending on the fix.
   - For frame-exact A/B, use a saved skate.Reel with `play_reel` (needs the user to save one; never save replays yourself).

**Diagnostics:**
- `skate3_native_render_scene_char_track=true` logs one `char-track f=N body ...` line and one `... shirt ...` line per frame. They contain:
  - raw vs drawn positions (`rp`, `fp`);
  - the pose source (`src=`, `DrawItem::dbg_src`);
  - ring state;
  - `play=`, the playback delay in ms;
  - since KI-120, these fields:
    - `scr` / `rscr`: the body's screen NDC through the drawn camera and through the raw camera;
    - `stamp` / `tick`: pose timestamp ages in ms;
    - `cam_n`: camera samples per frame;
    - `clamp`: boxcar taps clamped to the newest pose.
- `tools/harness/char_jitter.py <log.txt>` turns those lines into on-screen wobble by speed, drawn vs the game's raw reference, and shows what the wobble goes with.
- For a one-off analysis, write the parser into the scratchpad and keep it. Promote it to `tools/harness/` once it has been useful twice.

## Device etiquette

- Tell the user before any run that takes over the device.
- Never `stop` a session mid-run.
- **Before any raw input on the device** (`adb shell input tap|keyevent|text|swipe`, which go to whatever has focus): check `adb shell "dumpsys window | grep -m1 mCurrentFocus"` names `com.droiddeck.launcher`. The top activity (`topResumedActivity`) isn't enough: the quick-settings shade (`NotificationShade`) or a system dialog can cover DroidDeck while it stays the top activity, and a tap then lands in the shade (2026-11-18: it opened the Internet panel). If something else has focus, stop and tell the user. Never close the shade or press Back to clear it: it may be the user's, opened on purpose. `droiddeck_install_appimage.py` checks this before every tap and key.
- Ask before installing anything into the DroidDeck guest.
- If a run hangs, `$DROIDDECK_DIR/tools/droiddeckctl state` shows the session phase.
- The user may have their own rules for their device (for example, that you may end sessions without asking); follow those.
