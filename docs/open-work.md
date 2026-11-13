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

- **Blended surfaces vs the depth-reading post passes** (in progress). Hair
  was hazed as if it were the background behind it
  ([KI-126](known-issues.md#ki-126-the-skaters-hair-looks-transparent)).
  Other surfaces drawn blended without writing depth may show the same kind
  of fault in haze, light shafts, SSAO or SSR: glass canopies and shop
  windows, vehicle glass, the sunglasses lens, mist sheets, canal water.
- **Character shading is harsher than the game's.** Shirt folds, hair
  shadows and muscle definition are sharper and more contrasty; the hair is
  lighter than the game's in shade (KI-124 and KI-126 notes).
- **The skater's body may poke through the shirt** in small spots (seen once
  on Windows, reel `88a86b704bcf2d71`).
- **Distant textures glitch or pop in** on the Odin 2 (no details yet; needs
  a spot where it happens).
- **The skater customization screen has many renderer bugs** (check
  upstream's KNOWN_ISSUES.md first).

## Scenario harness and tooling

- **Reel lens toggle:** hide the replay camera's fisheye and vignette for
  testing (`skate3_native_render_scene_screen_fx`, `play_reel lens = false`;
  see KI-123).
- **Fly and noclip mode** to move the camera or skater through geometry, for
  looking at shadows and their casters.
- **Scripted "backwards man" speed glitch** as a timed pad sequence (find the
  jump's apex from the skater's vertical speed rather than fixed delays).
- **Profiling:** a Tracy build option with zones in our code, then GPU
  tracing on the device (Perfetto or Mesa); a performance threshold in
  scenarios.

## Investigations

- **What in the game is tied to frame rate:** physics, animation,
  pedestrians, camera, input polling, timers. It affects how repeatable
  repros are and how 30 fps play feels.
- **Audio problems on the Windows build** (no details yet).
