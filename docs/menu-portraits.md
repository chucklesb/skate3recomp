# Menu portraits: what still runs on the game's own renderer

Research notes (2026-11-13) for the open-work item "Native rendering for
the menus' remaining emulated pieces". Goal: let the emulated renderer stay
off in menus, as it does in gameplay.

**Measured on:** Windows Vulkan, `scenarios/cas_portrait.toml` with
`native_render_pass_census=true` and `native_render_pass_census_detail=true`
(run `20261113-201025_cas_portrait`). It covers:
- Pause > Edit Skaters (the team screen);
- Create-a-Skater, entered four times;
- an accepted edit each time, which saves a new skater photo.

Read the counts with `tools/harness/census_report.py`, or per 60-frame
interval from the `census` events in `events.jsonl`.

## Findings

### 1. The team screen draws no skater in 3D

The skater boxes are **saved photos**: the 128x256 pictures the game keeps
in the save (see 3). In the screenshot, the selected box shows the full-body
picture on its teal card, and the next box shows an old event photo.

On FE [0,56,63], per frame:

| Pitch | Issued | Executed | What runs |
|---|---|---|---|
| 560 | 187 | 2 colour draws (8 indices) | the 512x512 card compose, resolved to `0x03855000` and `0x03C55000` |
| 600 (4x MSAA) | 54 | 3 small draws | |
| 1200 | ~2,100 | 0 | the world behind the menu |

- The other ~185 draws at 560 and ~51 at 600 are **depth-only**. The
  depth-only gate (`ShouldSuppressExemptDepthOnlyDraws`) drops them, and
  the census counts them as "suppressed". They are probably the world's
  shadow maps (512x512 at pitch 560).
- So the portrait window (suppress mode 3, synchronous shaders) only lets
  the card compose run here; mode 2 would suppress pitch 560.

**Cost:** small. The Odin 2 already measured 52 fps uncapped on this screen
after the testing.6 fix.

### 2. Create-a-Skater renders its whole scene twice

The output is native: 1,792 of 1,792 frames on FE [0,56,63,15]. But
`UpdateSkaterPhotoWindow` holds **suppress mode 4 for the whole stay**, so
the game's emulated pipeline also renders the editor scene every frame,
and nothing on screen shows that copy.

| Pitch | Executed per frame | Indices per frame |
|---|---|---|
| 1200 | 60-72 draws | ~95,000 |
| 160 (a 128x128 target, resolved to `0x041AB000`) | 12 draws | ~36,000 |

- **Why mode 4 is held:** the photo saved when you leave Create-a-Skater
  came out stale when mode 4 was held only for the photo request itself
  (2026-11-13). The game appears to build the photo partly from frames
  rendered before the request.
- **This is the biggest piece left.** Create-a-Skater is the one menu
  where the emulated renderer does real work every frame.

### 3. The skater photo is a one-shot emulated render read by the CPU

When `SkaterPresentationPhoto::GeneratePhotoRequest` fires:
- its render+extract step (`sub_8278E960`, three calls ~65 / 595 / 685 ms
  after the request) renders the skater into the 1152x640 screenshot
  target (pitch 1200);
- the target is resolved to `0x04911000`, among others;
- `PhotoEffects::GeneratePhoto` reads it with the CPU and shrinks it into
  the 128x256 portrait the save keeps.

**It already works:** the window holds mode 4, synchronous shaders and
forced CPU readbacks for ~1.6 s.

**A native version is plausible:** the photo grab already writes native
frames into `0x04911000` in the guest's tiled big-endian layout
(`WriteGrabToGuest`, used by challenge photos and reel thumbnails). It
would need the photo's own camera and skater, not the screen's.

### 4. The 128x128 pitch-160 pass

- **In the replay editor:** it is the per-mesh "aux skater" pass, which
  nothing displayed samples. It is suppressed there
  (`skate3_native_render_scene_replay_aux_skip`, KI-123).
- **In Create-a-Skater:** it runs 12 draws a frame. Its consumer is not
  known yet.
- **If nothing samples it in Create-a-Skater either:** the same pitch
  filter removes it for free.

### 5. Already in place for a native version

- **The portrait scene:** the native scene builder already captures the
  portrait passes' scene and drops it (the "portrait-pass publish skipped"
  gate in `BuildFrameScene`, and "skinned capture refused - aux
  perspective pass" in `CaptureSkinnedState`). Their camera and items are
  known; they are refused today only because they used to poison the
  on-screen player's capture.
- **Texture substitution:** the native 2D pass can swap a draw's texture
  (the FMV path does this in `emit_draw`). A natively rendered card could
  stand in for any menu quad that samples `0x03855000` / `0x03C55000`,
  with no CPU readback.

## Done (2026-11-13, commit 46d93c9)

**Create-a-Skater's hidden emulated copy is gone.**
- **The pitch-1200 scene:** the editor-wide mode-4 hold was not needed.
  The stale team box that motivated it is the box not refreshing within a
  session (see open-work). Across restarts (`gen_cas_portrait.py --only
  on|off`, four runs), the photo saved with mode 4 held only inside the
  skater-photo window is pixel-identical to the one saved with the hold.
  The hold is now off by default
  (`skate3_native_render_scene_cas_scene_photo_hold`).
- **The pitch-160 aux skater pass** is now skipped in Create-a-Skater too.
  With it suppressed, the editor and team-screen shots are unchanged:
  the team box is identical, and the only differences elsewhere are
  sub-pixel shifts of a sliding panel's edge. It still runs inside the
  photo window.
- **Census in Create-a-Skater (Windows Vulkan), indices a frame:**

  | Pitch | Before | After |
  |---|---|---|
  | 1200 | 68,889 | 951 (the photo windows only) |
  | 160 | 26,177 | 244 |

- **Odin 2, uncapped** (`scenarios/cas_perf.toml`, 10 s idle in the
  editor; it never saves): **55 -> 85 fps** (old behaviour:
  `--skate3_native_render_scene_cas_scene_photo_hold=true
  --skate3_native_render_scene_replay_aux_skip=false`). The saved photo
  was checked on Windows only; checking it on the device writes the
  device's save.

So a native skater photo (old step 2) is no longer needed for
performance: the one-shot emulated photo render costs ~1.6 s of mode 4
once per accepted edit.

## Remaining options

1. **The card compose natively** (team screen, Import Skater card). These
   are 2D quads the native 2D pass drops today as "offscreen 2D compose
   draws". Composing them into a native card texture and substituting it
   by address (finding 5) would let the portrait window close for good.
   Low priority: the measured cost is small.
2. **A native skater photo:** only for independence from the emulated
   renderer, not speed.

## Still open

- My Replays screen 36 and the Import Skater card: not measured with the
  pass detail yet. Earlier, screen 36 showed up to ~1,160 scene draws a
  frame, before the mode-3 fix.
- What exactly the 600-pitch MSAA pass is.
- Whether the photo's stale-frame dependence (finding 2) also applies to a
  native photo. The photo would be rendered at the request, so it should
  not.
