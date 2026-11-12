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
out, 3 the scenario file did not parse, 4 no `result.json` and no crash
report (killed, or still running), 5 the game crashed (see "Crashes").

## Run folder

Each run writes `<out>/<YYYYMMDD-HHMMSS>_<name>/`:

| File | Contents |
|---|---|
| `result.json` | Status, code, reason, failures, frame count, warning/error counts, last state, and the build (`version`, `platform`, `build_config`). Missing when the game crashed. |
| `build.json` | The build, written at start (a crashed run has no `result.json`). |
| `crash.txt`, `crash.dmp` | Only after a crash (see "Crashes" below); `crash.dmp` on Windows only. |
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
start_spot = "Z_DT_Tresoutta"  # optional: teleport here after the first gameplay wait

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
| `crash` | `where = "runner"` (default; the harness thread) or `"guest"` (the next controller poll, inside a guest call): fault on purpose to test crash capture. Refused unless the scenario sets `allow_crash = true`. |
| `teleport` | `location = "Z_DT_Tresoutta"` (a named world locator), optional `tolerance` (metres, default 5) and `timeout_s` (default 60). It uses the game's own teleport, the pause map's path, then waits until the skater settles near the locator: within `tolerance` horizontally, between 15 m below and `tolerance` above it (many locators float above the ground), and slower than 2 m/s for 30 frames. Before issuing, it waits up to 20 s for free skate, so the previous teleport or a respawn can finish. A fall through a not-yet-loaded floor (more than 15 m below) re-issues the teleport, up to `retries` times (default 3). `settle = false` returns as soon as the skater reaches the locator. `path = "sequencer"` uses the lower-level teleport instead of the pause map's: it doesn't load another map, so into an unloaded district the skater falls through the world (that's what `repro_fast_fall_body_parts.toml` relies on). `map_key = "<hex>"` overrides the district's map key. Free skate only. See "Teleport locations" below. |
| `play_reel` | `clip = N`: plays saved skate.Reel replay N (1-based, as listed in My Replays, oldest first) from start to end at 1×, with no editor or HUD on screen, then returns to free skate. Options: `from = "marker"` starts at the reel's marker (the editor's bookmark; the start if it has none); `stills = [seconds, ...]` stops the clock exactly at those reel times, waits `settle_frames` (default 20), takes a screenshot tagged `reel<N>_t<seconds>` and continues (a still shows the same recorded frame on every run and target, which is what the image diffs compare); `every_frames = N` takes a screenshot every N frames (`tag` defaults to `reel<N>`); `timeout_s` (default 600). It loads the reel the game's own way, from free skate, with no menu input, and drives the replay clock itself (see "skate.Reel playback" below). Events `reel_load`, `reel_start` (`duration_s`, `from`, `start_s`, `marker_s`), `reel_still` (`t`, `file`), `reel_stop` (`played_s`, `editor_frames`, `frames`, `wall_s`) and `reel_exit`. Needs the signed-in profile's saves: `boot = "demo"` with `skate3_demo_path_signed_in = true`, run against the test user data. Examples: `reel_regression.toml`, `reel_stills.toml`. |

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

## Validation layers and RenderDoc

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
- Windows: the LunarG Vulkan SDK (it registers `VK_LAYER_KHRONOS_validation`
  system-wide);
- DroidDeck: `sudo apt-get install vulkan-validationlayers` in the Linux
  environment.

**D3D12 debug layer.** A `vulkan_validation = true` scenario run with
`windows --backend d3d12` gets `--d3d12_debug=true` instead. The SDK then
logs the debug layer's warnings and errors (Windows 11's
`ID3D12InfoQueue1` message callback) as `D3D12 <severity> (debug layer, ID
<n>): ...`. The harness puts them in the same `validation.txt` and
`validation_messages`, and `result.json`'s `validation_layers` says which
layer it was. It needs Windows' "Graphics Tools" optional feature
(`d3d12SDKLayers.dll`). The debug layer's live-object report at process exit
also lands there and can be ignored.

Validation is slow, so give scenarios long timeouts.

**RenderDoc captures.** The `capture` step (`tag`, `frames = 1`) captures
the next N game frames into `<run>/captures/<tag>_capture.rdc`. It brackets
the frames with RenderDoc's explicit start/end calls, so it doesn't depend on
RenderDoc tracking the window, which it doesn't do on Wayland. Without
RenderDoc attached, the step fails with a clear message.

- **Desktop:** launch the game from RenderDoc (qrenderdoc or `renderdoccmd
  capture`) and run a scenario with a `capture` step
  (`scenarios/renderdoc_capture.toml`).
- **Windows from WSL:** `run_scenario.py windows <scenario> --renderdoc
  [DIR]` launches through `<DIR>/renderdoccmd.exe capture` (default DIR:
  `$SKATE3_WINDOWS_RENDERDOC`, a RenderDoc folder; the portable zip works).
  It also sets `VK_ADD_IMPLICIT_LAYER_PATH` to that folder, because a
  portable RenderDoc's Vulkan layer isn't registered. Captures land in the run
  folder as usual. Both backends work. Vulkan captures carry the native
  renderer's labels and object names; D3D12 captures have neither.
- **Harness behaviour under RenderDoc:**
  - RenderDoc's explicit start and end calls submit work on the app's queues
    from the calling thread. The SDK therefore holds every Vulkan queue across
    them (`rex::ui::renderdoc::SetQueueGuard`). Without that, captures lost the
    device about half the time on an RTX 4070.
  - A finished run closes the game normally instead of exiting at once,
    because a hard exit right after a capture could leave the process stuck in
    the GPU driver. `result.json` is written first either way.
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

## Crashes

While the harness is on, a fault that nothing else handles is written to the
run folder before the process dies (`src/harness/harness_crash.cpp`):

- **Windows:** an unhandled-exception filter writes `crash.txt` (exception code,
  faulting address, and the stack unwound from the faulting context as
  `module+0xRVA`) and `crash.dmp`, a minidump with thread info and
  indirectly referenced memory. The SDK's vectored handler still sees every
  exception first (guest MMIO, memory watches), so only faults it doesn't
  claim reach the filter.
- **Linux/aarch64:** the SDK's signal handler calls the harness's last-chance
  callback (`rex::arch::ExceptionHandler::SetUnhandled`) for a fault no
  handler claims, then restores the original disposition so the process
  dies. Before that change, an unclaimed fault re-executed forever and looked
  like a timeout. The callback writes `crash.txt` with `write(2)` only:
  the signal, the faulting address, pc/lr/sp/fp, a frame-pointer walk, and a
  copy of `/proc/self/maps`.

`run_scenario.py` reports such a run as `crash` (exit code 5). It maps the
frames to `module+offset` and resolves them against the unstripped build
named in `build.json`, as `symbolize` does. RelWithDebInfo gives `file:line`,
including recompiled `sub_8xxxxxxx` functions to their `generated/*.cpp`
lines. Release gives function names on aarch64 (ELF symbols), and only
exported names on Windows (no PDB). `ab` lists a crash's top frames per
target. On Windows, open `crash.dmp` in WinDbg or Visual Studio with the
staged PDBs (`<stage root>/relwithdebinfo/*.pdb`) for locals and every
thread.

`scenarios/crash_smoke.toml` (runner thread) and `scenarios/crash_guest.toml`
(inside a guest call, after gameplay starts) crash on purpose to test this.

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

## Teleport locations

The game keeps 1,080 named world locators. Each one is a 4×4 transform, keyed by a hash of its name, in a registry hanging off the world object. `teleport` hands one to the game's teleport sequencer. Rules for the names:

- **The pause map's destinations** are mostly `Z_*` locators under internal names, e.g. `Z_UN_ClockTower` ("Clock Tower") and `Z_DT_Tresoutta` ("Tresoutta Plaza").
- **Use the vetted names** in `scenarios/teleport_locations.md`: 72 Downtown, University and Industrial locations, checked by a survey that teleported to each one.
- **Districts need a map key.** `teleport` goes through the pause map's own entry point, which loads the destination's map first. That needs the district's map key, captured from the menu: Downtown, University and Industrial so far. Districts without a key (the skate school, the separate skate parks, DLC) fall back to the game's lower-level teleport. That arrives before their map is loaded, so the skater falls through the world (the step fails, because the skater never settles).
- **Regenerating the data:** the full locator list comes from a memory snapshot via `tools/re/registry.py <snapshot.gsnap>`. The locator-to-district table, `src/harness/teleport_districts.inc`, comes from `tools/re/locator_districts.py <game dump>`.

## Notes for later work: game teleport

How the harness teleport works (TU3 addresses). Implemented in `src/harness/harness_game.cpp`.

| Piece | Where |
|---|---|
| world object | `*(0x83083BCC)` |
| locator registry | `world + 168`: an EASTL map, nodes `{right, left, parent, color, key +16, type byte +20, object +24}` |
| name hash | `sub_8296EA10`, ELF-style |
| game-mode owner | `*(world + 156)`; its `+116` is the teleporter, its `+40` the teleport-mode component (vtable `0x8230DF88`, `+44` = teleporting allowed) |
| **the pause map's entry point** (used by the harness) | `sub_82864628(service, u64* map key, const char** name, 0, 1)`. The service is `*(*(0x830CFD94) + 0x108)` (vtable `0x82315370`). It hashes the name, copies the locator's matrix from the registry (`service + 12`), and calls `sub_828646E0` (vtable slot 17). |
| `sub_828646E0` | stores the requested map key in the state block `0x830B7AE8` (`+40` current, `+48` requested). When the map changes, it builds a load-map-then-teleport task (`sub_825582D0`); otherwise it takes the same-map path. It posts `523ED4CC`. |
| map keys | the pause map's, per district: Downtown `4773016A41B2E05E`, University `CB473E68A2EAFABA`, Industrial `A8B1F47090679026`. These are not plain FNV/MD5/SHA hashes of the district names. |
| sequencer | `sub_82706F50(teleporter, locator, hash, flag, 0, 0, 0)`, the lower-level path the harness falls back to for districts without a key |
| enter teleporting state | `sub_82720B50(mode, 0x8270C7D0, teleporter)` |
| the move | the teleporting state posts message `0x8FCDC5E2` (96 bytes, a 4×4 matrix) from `sub_827070C0` |

- **The fallback sequencer path:** `sub_82864C40` decides whether the area must be streamed in, and the sequencer returns that. When it returns 0, the sequencer moves the player at once, but free skate ignores that instant move, so the harness forces 1 during its own call. That path never loads another map, and in a freshly streamed district the skater can arrive before the collision does.
- **How the menu does it:** `sub_8270B3D0` on the mode component handles the event-driven variant (hash the name, sequencer, teleporting state). The pause map itself calls `sub_82864628` with the destination district's key (captured with the RE probe's teleport-start hooks).
- **Finding these again:** the harness RE probe (`--skate3_harness_re_probe=true`, `src/harness/harness_re.cpp`) logs every new game message type with its sender. That's how the teleport message showed up. `tools/re/xrefs.py` (callers, callees, address references and disassembly from the generated code) and `tools/re/gsnap.py` (snapshot search and diff) are the static side.

## skate.Reel playback (`play_reel`)

How `play_reel` works (TU3 addresses; RE with the probe in `harness_re.cpp`):

- **Reels:** all saved replays are in one save file, `rmcsave/REPLY_SKATER` (`1..5.B` bodies, `.H` headers). A reel can't be trimmed: the editor's marker only bookmarks a point, a reel opens on its last frame, and Play runs the whole reel at 1×. The game never leaves the player on its own.
- **Frontend screens:** 59 = My Replays, 52 = loading screen, 12 = replay editor (it opens by itself once a reel has loaded, paused on the last frame).
- **Game-mode context:** `gctx = *(*(0x83083BCC) + 156)` is a table of the game-mode state objects (`+0x28` mode owner, whose current state is at `+8`; `0x8270C890` = free skate).
- **Threading:** game logic must run on the main (simulation) thread. The harness runs its requests from a hook on the game-state tick `sub_826D34B8`, never from the render thread: the game's allocators pick heaps per thread.
- **Load:**
  1. `cMsgPauseSim{1, 11}`, as the pause menu does (`PostSimMessage`: queue `*(world+144)+24`, lock at `+16`). A reel loaded from live free skate hangs or crashes on playback.
  2. My Replays' own "view replay" action, `sub_82627128(this)`:
     - it reads the slot from `browser+80`, with `browser = *(*(sub_824AD240()+8)+236)`;
     - it queues a body load of entry [slot] of the content manager `*(0x830670A0)` (entries `+384`, count `+392`, filled from the headers at boot);
     - its completion calls `*(browser+5352)` slot 13/14, which is My Replays' handler and null while the screen is closed. A no-op stand-in goes there (`sub_82B61BB8` = `blr`).
  3. The loaded body becomes a type-27 game event. That starts StateReplay, then FE 52, then FE 12.
- **Clock:** the replay editor's time object is `E = **(0x8303740C)`. It has a state at `E+10696` (8 = paused, 5 = playing) and a normalized position at `E+4`. While paused, the editor update `sub_82C7EA50(this, dt)` (render thread) pins the replay's time track (time controller `SetTime`, `sub_82833748`) to `E+4 × duration`, where duration = `*(*E + 0xC)` seconds. `play_reel` keeps the editor paused and sets `E+4 = t / duration` before each editor update, with `t += dt`: the harness is the clock, at exactly 1×. The timeline `*(*(0x83083C38)+0x70)` reports the displayed frame (60 Hz ticks) at `+0x28`, as output only.
- **Marker:** the editor's timeline keeps keys per channel: the list for channel `c` is at `E + 556 + c*16` (`+2` u16 count, `+12` a u16 array, positions /65535). `sub_82506BE0(E, i)` returns key `i`'s normalized position on the current channel (0 = start, past the last key = end), which is what the D-pad jump uses. The marker is key 1. The keys arrive about half a second after the editor settles, so `play_reel` waits for them (2 s at most).
- **Stills:** the clock (`sub_82C7EA50` hook) stops exactly at each still time and holds there until the harness thread has taken the screenshot.
- **No UI:** `FrontEndManager::Render2D` (`sub_825D9168`) is skipped while a reel is active (`game::SuppressUi`).
- **Exit:** the editor's Finished → Exit action:
  - sets `x+52 = 1` and the byte `x+140 = 1` on `x = *(*(sub_824AD240()+8)+48)`, and clears the byte at `*(0x83067060+36)+12`;
  - posts a type-145 game event through `sub_824FCAD8(gctx, ref*)`.

  StateReplay then restreams free skate, and the harness sends `cMsgPauseSim{0, 11}`.
- **RE helpers:** `--skate3_harness_re_dump="ADDR:N[,*PTR+OFF:N]"` and `--skate3_harness_re_poke="ADDR=VALUE"` read and write guest words live (`cvar` command). crash.txt names the guest function of each game frame.
