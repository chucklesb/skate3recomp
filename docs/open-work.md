# Open work

What's planned or waiting for this fork, beyond the bugs in
[known-issues.md](known-issues.md). Items move to known-issues.md once
they're investigated and have a reproduction; finished items are removed
from here (the release notes in [release-notes/](release-notes/) record
what was done).

## Open known issues

| Issue | In short |
|---|---|
| [KI-118](known-issues.md#ki-118-the-screen-goes-black-after-spawning-at-some-locations) | Black screen after spawning at some places |
| [KI-119](known-issues.md#ki-119-enhanced-shadows-put-sunny-places-in-shade) (remainder) | Core Plaza: the overhead structure's live shadow covers a path the game shows in sun |
| [KI-121](known-issues.md#ki-121-on-d3d12-the-scene-is-sometimes-drawn-in-shade) | On D3D12, a sunny scene is sometimes drawn in shade |
| [KI-123](known-issues.md#ki-123-skatereel-playback-is-slow-and-uneven) | Reel playback runs slow and uneven on the Odin 2 |
| [KI-125](known-issues.md#ki-125-the-skater-comes-apart-for-a-frame-or-two-after-a-replay-jumps) | The skater comes apart for a frame or two after a replay jumps |

## Rendering

- **Glass hazed as if it were the sky behind it.** The KI-126 class
  ([hair](known-issues.md#ki-126-the-skaters-hair-looks-transparent), fixed)
  also reaches glass: blended surfaces don't write depth, so our haze pass
  hazes them by the distance of whatever is behind. Checked 2026-11-12 with
  `scenarios/blend_depth_audit.toml` (Windows Vulkan at the Odin 2's
  settings, run `20261112-222142_blend_depth_audit`), measuring each surface
  with haze and SSAO on and off against the game's own frame:
  - **Core Plaza bus-shelter glass roof: affected.** Haze lifts it from
    blue to a washed-out grey (blue channel 56 with haze, 48 without, the
    game 51). SSAO doesn't touch it.
  - **The Villas entrance glass canopy: not affected** (under 1 of 255).
  - Not reached yet: vehicle glass, the sunglasses lens, mist sheets. SSR is
    off by default and in the device settings, so it can't affect players.
  - **Deferred (2026-11-12):** the difference is too slight to see. The
    quick fix (extending the KI-126 depth-only pass to glass) is unsafe: the
    occlusion-culling grid is built from the same depth buffer, so props
    entirely behind glass (shop windows) could be culled. The safe fix gives
    the haze and shaft passes their own linear depth for blended surfaces
    (blended items' linear depth MIN-blended into the haze depth after the
    grid is built), leaving the depth buffer and culling untouched; the hair
    would move onto it too (see the KI-126 caveat).
- **Character shading is harsher than the game's.** Shirt folds, hair
  shadows and muscle definition are sharper and more contrasty; the hair is
  lighter than the game's in shade (KI-124 and KI-126 notes).
- **The skater's body may poke through the shirt** in small spots (seen once
  on Windows, reel `88a86b704bcf2d71`).
- **Distant textures glitch or pop in** on the Odin 2 (no details yet; needs
  a spot where it happens).
- **The skater customization screen has many renderer bugs** (check
  upstream's KNOWN_ISSUES.md first).

## UI and overlays

Plan these together, and with the in-game debug menu below in mind (where
fly mode, recorders and toggles will live), so the overlays share one layout.

- **Renderer indicator inside the performance HUD.** Today the
  NATIVE/EMULATED badge overlaps and hides the performance HUD. Make it the
  HUD's top row: still boxed, its bottom border shared with the HUD's top
  border (the HUD moves down to fit it). Make the HUD fixed-width so it stops
  resizing. The indicator always shows with the HUD, so remove its in-game
  menu setting, recomp menu setting and cvar.
- **Recorder indicators attach to the HUD.** The input record/playback
  indicator (top-left today) attaches below the performance HUD when the HUD
  is on, sharing the HUD's bottom border. With the HUD off it stays a
  standalone badge as today, moved to the top-right.
- **MangoHud support** alongside our performance HUD (feasibility first).
  MangoHud sits top-left, clear of our HUD and indicators.
- **Remove the recomp settings menu** if the in-game settings menu covers
  every option it offers. Check parity first; remove it only if nothing is
  lost and there are no major drawbacks.

## Scenario harness and tooling

- **In-game debug menu** (its own feature branch). A menu in the game's UI
  for the testing tools that today need the harness or a cvar file:
  - record and play back saved input;
  - record and play back repro reels;
  - take screenshots (single or bursts);
  - screen recording of the session;
  - free camera, fly mode and noclip (see below);
  - harness and cvar toggles (renderer switches, debug views, the fix
    toggles used for A/B checks).
- **Fly and noclip mode** to move the camera or skater through geometry, for
  looking at shadows and their casters.
- **Scripted "backwards man" speed glitch** as a timed pad sequence (find the
  jump's apex from the skater's vertical speed rather than fixed delays).
- **Profiling:** a Tracy build option with zones in our code, then GPU
  tracing on the device (Perfetto or Mesa); a performance threshold in
  scenarios.

## Investigations

- **Where else the game's own (emulated) renderer still runs** under the
  native renderer, e.g. photo and video challenges, the photo editor, the
  replay editor's one-shots, menus. KI-123 found two such cases in replays:
  the portrait window (fixed) and per-frame aux skater passes (128x128,
  six meshes a frame). Method: the suppression census and `Main` GPU bucket
  per context, RenderDoc frames (`play_reel stills_capture`, `capture`), and
  for each pass, whether anything visible or the game's CPU reads its result.

- **What in the game is tied to frame rate:** physics, animation,
  pedestrians, camera, input polling, timers. It affects how repeatable
  repros are and how 30 fps play feels.
- **Audio problems on the Windows build** (no details yet).
