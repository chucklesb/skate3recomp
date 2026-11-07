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
```

For `odin2` and `live`, `DROIDDECK_DIR` points at a DroidDeck checkout
(default `~/dev/DroidDeck`), and `ADB` / `ANDROID_ADB_SERVER_PORT` select
the adb to use.

Exit codes: 0 pass, 1 a step or expectation failed, 2 the scenario timed
out, 3 the scenario file did not parse.

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
