# Scenario harness

The scenario harness boots the game into a scripted situation, drives the
controller, and records what happened: screenshots, game state, the log, and
a pass/fail result. Use it to reproduce bugs on demand, check fixes, and
compare builds. The code is in `src/harness/`.

The harness is off unless one of these cvars is set:

| Cvar | Effect |
|---|---|
| `--skate3_harness_scenario=<file.toml>` | Run the scenario unattended, then exit with its result code. |
| `--skate3_harness=true` | Live mode: the command inbox and input recording, no scenario. |
| `--skate3_harness_out=<dir>` | Output folder, relative to the working directory (default `harness`). |

## Running scenarios

`tools/harness/run_scenario.py` (Python 3.11+, standard library only) wraps
the common flows:

```sh
# Desktop: any built skate3 executable or AppImage
python3 -I tools/harness/run_scenario.py local scenarios/boot_to_plaza.toml --exe out/build/linux-relwithdebinfo/skate3

# DroidDeck handheld over adb: builds and deploys the testing AppImage,
# runs the scenario, dismisses "The session ended", fetches the run folder
python3 -I tools/harness/run_scenario.py odin2 scenarios/boot_to_plaza.toml

# Live mode on the handheld, then send commands to it
python3 -I tools/harness/run_scenario.py live
python3 -I tools/harness/run_scenario.py send screenshot tag=here count=5 every_frames=2
python3 -I tools/harness/run_scenario.py send query what=state

python3 -I tools/harness/run_scenario.py fetch          # latest run folder from the device
python3 -I tools/harness/run_scenario.py summary <run>  # print a run's result

# Windows x64 build on the WSL host (tools/cross-windows/build.sh; builds
# and stages first unless --no-build)
python3 -I tools/harness/run_scenario.py windows scenarios/boot_to_plaza.toml --backend vulkan
python3 -I tools/harness/run_scenario.py live --target windows
python3 -I tools/harness/run_scenario.py send --target windows query what=state

# A/B: the same scenario on the handheld and on Windows (both backends), compared
python3 -I tools/harness/run_scenario.py ab scenarios/boot_to_plaza.toml

# Function and file:line for addresses (RelWithDebInfo gives line info)
python3 -I tools/harness/run_scenario.py symbolize --target odin2 --config relwithdebinfo skate3+0x1de040c
```

`--config release|relwithdebinfo` picks the build for `odin2`, `live`,
`windows` and `ab`. Every run's `result.json` names the `version`,
`platform` and `build_config` it came from.

**Windows from WSL.** `windows` runs `skate3.exe` from the stage
(`SKATE3_WINDOWS_STAGE_ROOT/<config>`, default `out/stage/windows-x64`) with
the stage as the working directory and `--game_data_root=<stage root>/game`.
It moves the run folder into `--out` like `fetch` does. `--backend` picks
`d3d12` (default) or `vulkan`. `--timeout` overrides the deadline, after which
the game is killed (`taskkill`). A second `skate3.exe` is refused.

**A/B runs.** `ab` builds both targets through their `build.sh`, which
reconfigures, so both report the same version. It then runs the scenario on
the Odin 2 and on each Windows backend in turn and writes
`<out>/ab-<stamp>_<scenario>/`:
- `compare.json`: status, build, frames, fps, counts, failures and the tagged
  `state` rows for each target.
- `shots/<tag>/<target>_<n>.png`: the screenshots side by side.

It prints a table and exits non-zero if any target failed. `--fps-cap 30`
(the default) caps every target's guest frame rate: input holds and waits
count frames, so they only compare at equal frame rates (this PC runs about
150 fps uncapped, the Odin 2 about 30). The skater spawns where the save last
left them, and each target has its own save, so spawn positions (and
anything that depends on them, like rolling speed) differ between targets
for now.

For `odin2` and `live`, `DROIDDECK_DIR` points at a DroidDeck checkout
(default `~/dev/DroidDeck`), and `ADB` / `ANDROID_ADB_SERVER_PORT` select
the adb to use. `--no-deploy` reuses the build already on the device.
`--env KEY=VALUE` (repeatable) sets environment variables for that run's
session only. Game arguments go after `--`.

Exit codes: 0 pass, 1 a step or expectation failed, 2 the scenario timed
out, 3 the scenario file did not parse, 4 no `result.json` (killed or
crashed).

## Run folder

Each run writes `<out>/<YYYYMMDD-HHMMSS>_<name>/`:

| File | Contents |
|---|---|
| `result.json` | Status, code, reason, failures, frame count, warning/error counts, last state. |
| `events.jsonl` | Every step and command, frontend screen changes (`fe`), replay/movie playback starts and stops, save content opens, recording start/stop. |
| `state.jsonl` | Game state every `state_every_frames` frames and at every `state` step (see below). |
| `log.txt` | The run's log lines. |
| `shots/` | PNG screenshots, `f<frame>_<tag>_<n>.png`, of the game output (no overlays). |
| `*.s3rec` | Input recordings. |

`<out>/current_run.txt` names the latest run folder.

A state line holds:
- the frame number, time and fps;
- `presence` (1 = gameplay);
- `fe`, the frontend screen stack;
- the player's skater: its position and speed in m/s;
- the camera position;
- whether a replay or movie is playing.

## Scenario files

```toml
name = "boot_to_plaza"
timeout_s = 300           # wall-clock limit, exit code 2 past it
boot = "demo"             # "demo": signed-out boot straight to gameplay; "none": normal boot
state_every_frames = 5    # state.jsonl interval (0 = only at state steps)

[cvars]                   # applied at startup, like --name=value
skate3_native_render_scene = true

[[step]]
do = "wait"
gameplay = true
timeout_s = 240
```

Steps run in order. A failing step ends the run with code 1.

| Verb | Keys |
|---|---|
| `wait` | One of `gameplay = true`, `frames = N`, `ms = N`, `fe_screen = N`, `presence = N`, `log = "regex"`, `speed_gt = x`, `speed_lt = x`. Optional `timeout_s` (default 120). |
| `input` | `buttons = "a+lb+up"`, `lt`/`rt` (0..1), `lx`/`ly`/`rx`/`ry` (-1..1), `frames = N` to hold. `mode = "override"` (default: replaces the real pad) or `"overlay"` (merges with it). `async = true` continues without waiting. |
| `play` | `file = "x.s3rec"` (relative to the scenario, then the run folder). `mode` and `async` work as for `input`. |
| `record` | `start = true` / `false`, optional `file`, `note`. |
| `cvar` | `set = { name = value, ... }`, applied live. |
| `screenshot` | `tag`, `count = 1`, `every_frames = 1`. |
| `state` | `tag`, written to `state.jsonl` now. `every_frames = N` changes the trace interval. |
| `mark` | `text`, written to the log. |
| `snapshot` | A full guest-memory snapshot through the native renderer's diagnostics trigger. |
| `pause` / `step` / `resume` | Hold the game at a frame boundary. `step frames = N` advances N frames while paused. |
| `expect` | `log = "regex"` (seen in this run's log), `no_log = "regex"`, `speed_gt`, `speed_lt`, `presence`. |
| `exit` | `code = 0`. |
| `capture` | `tag`, `frames = 1`: RenderDoc capture of the next N frames (see below). |

Button names: `a b x y lb rb l3 r3 start back up down left right guide`.

Skate 3 controls:
- **On the board:** A pushes, the left stick steers, and the right stick does Flick-it tricks.
- **Off the board:** the right stick moves the camera.

## Live commands

Drop a TOML file into `<out>/inbox/`. It uses the same keys as one
`[[step]]`, plus an optional `id`. The reply appears as
`<out>/outbox/<id>.json`. `query` is live-only: `what = "state"` or
`"status"`. `run_scenario.py send` does all of this for you.

## Input recording

In live mode, or during a scenario, **Ctrl+Shift+R** starts and stops
recording, or use a `record` step or command. A red `REC` counter shows in
the top-left corner while recording, `PLAY` during playback. Playback
reproduces the inputs poll for poll. The game is not deterministic, so start
from the same place and check the result in `state.jsonl` rather than
expecting a pixel-exact path.

`.s3rec` is text: a `# s3rec 1 {json}` header, then one line per input change:
`<poll> <frame> <buttons hex> <lt> <rt> <lx> <ly> <rx> <ry>`.

## Vulkan validation and RenderDoc

The native renderer labels each pass for Vulkan debug tools ("native: main
scene", "native: shadow atlas", "native: bloom + tonemap", …). It also names
its render targets, buffers, shaders and pipelines. The labels are on while
RenderDoc is attached or with `--gpu_debug_markers=true`. This needs the
rexglue SDK fork `chucklesb/rexglue-skate3`, branch `vulkan-debug-labels`.

**Validation layers.** Add `vulkan_validation = true` to a scenario
(`scenarios/validation_smoke.toml`). `run_scenario.py` then passes
`--vulkan_validation_enabled=true --vulkan_log_debug_messages=true`. The
Vulkan instance exists before the harness starts, so scenario `[cvars]`
can't enable it. Every message goes to `validation.txt` in the run folder,
and `result.json` counts them in `validation_messages`.

Install the layers first:
- desktop: the Vulkan SDK or the distro's `vulkan-validationlayers`;
- DroidDeck: `sudo apt-get install vulkan-validationlayers` in the Linux
  environment.

Validation is slow, so give scenarios long timeouts.

**RenderDoc captures.** The `capture` step (`tag`, `frames = 1`) captures
the next N game frames into `<run>/captures/<tag>_capture.rdc`. It brackets
the frames with RenderDoc's explicit start/end calls, so it doesn't depend on
RenderDoc tracking the window, which it doesn't do on Wayland. Without
RenderDoc attached, the step fails with a clear message.

- **Desktop:** launch the game from RenderDoc (qrenderdoc or `renderdoccmd
  capture`) and run a scenario with a `capture` step
  (`scenarios/renderdoc_capture.toml`).
- **DroidDeck handheld:** RenderDoc has no arm64 Linux release, so build one:

  ```sh
  tools/harness/build-renderdoc-aarch64.sh          # v1.46, Vulkan only -> out/renderdoc-aarch64
  python3 -I tools/harness/run_scenario.py install-renderdoc
  python3 -I tools/harness/run_scenario.py odin2 scenarios/renderdoc_capture.toml \
      --env SKATE3_RENDERDOC=/root/renderdoc
  ```

  `SKATE3_RENDERDOC` makes the AppImage's `AppRun` preload RenderDoc and
  enable its Vulkan layer for the game process only. DroidDeck's session
  environment would otherwise load it into a launcher process first, and
  RenderDoc clears its variables for that process's children. Open the
  fetched `.rdc` (a gameplay frame is about 200 MB) in desktop RenderDoc
  v1.46. Verified on an Odin 2 Portal with Turnip: every native pass is
  labelled and the targets are named.

Known noise on DroidDeck: every present reports
`VUID-VkSwapchainPresentModeInfoKHR-swapchainCount-07760`. The game presents
with no `pNext`, so a layer in DroidDeck's stack adds that structure,
probably gamescope's WSI layer.

## How it hooks the game (TU3 addresses)

- **Pad:** the game's `XInputGetState` wrapper `sub_82EE22C0` is its only
  path to `XamInputGetState`. The harness reads (recording) and rewrites
  (input, playback) the state it returns, per poll.
- **Frame clock and state sampling:** the guest D3D Swap, `sub_82B82E08`.
- **Skater:** entities from `Sk8::SkaterPresEntity::BindConstants`
  (`sub_82783D68`), world matrix at +352. The player's skater is the one
  nearest the camera; stand-ins at the origin are skipped.
- **Frontend:** the FrontEndManager push-state stack (`0x830CFE14`,
  +0x210/+0x214, 20-byte records).
- **Screenshots:** the presenter's guest-output capture, encoded with
  `src/thirdparty/stb/stb_image_write.h`.

## Notes for later work: skate.Reel playback

Found with the harness, kept here for a future reverse-engineering pass that
would play the Reel directly instead of by menu input.

- **Frontend screens:**

  | Id | Screen |
  |---|---|
  | 0 | gameplay root |
  | 5 | press-start |
  | 12 | replay editor (also the Reel's playback) |
  | 17 | pause map |
  | 22 | startup flow |
  | 24 | movie |
  | 47 | language select |
  | 52 | Reel clip panel |
  | 56 | pause |
  | 59 | Reel browser |

- **Boot path:** `[0,47] → [0] → [0,5] → [0,24] → [0,22] → [0]`.
- **Reel playback path:** `[0,56] → [0,56,59] → [0,56,59,52] → [0,56,59,12]`.
- **Where saved replays live:** all saved replays are in one save file, `rmcsave` / `REPLY_SKATER` (content type 1), next to `ALIAS_SKATER` and `CFOTO_SKATER`.
- **Detection:** Reel playback ticks neither `PhotoReplayController::Update` nor `MovieDecoder::Decode`. Detect it by screen 12 sitting on top of 59.
