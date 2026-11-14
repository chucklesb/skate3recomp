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
- **A multiple-exposure challenge photo had one skater without an upper
  body** (Odin 2, 2026-11-13; the Private Parking rail sign photo
  challenge, skater facing away from the camera): the seventh of eight
  exposures showed only legs, board and an arm. The exposures are captured
  during the showcase animation, not while skating. Seen once; two more
  attempts (one facing the camera, one recorded with scrcpy) were clean.
  Intermittent: if it comes back, record the showcase
  (`tools/harness/odin2_record.ps1`) and check whether that exposure's
  frame is drawn natively or by the emulated display-card path.
- **Hair loses its colour in shadow** (Odin 2, 2026-11-13): dyed hair
  (pink) looks mostly desaturated when the skater is in shade, in normal
  play. Seen first in a multiple-exposure challenge photo, where the
  exposures in shade had dark hair and the sunlit ones pink. Possibly
  related to the KI-126 hair notes (hair lighter than the game's in shade).
  Needs its own repro: the same spot in sun and shade, native vs emulated.
  **Measured 2026-11-13** (`scenarios/hair_shade_stills.toml`, device reel
  b363c5f2700483f3, Tresoutta Plaza arch; hair patch lightness /
  saturation): shade native 0.08 / 0.14, emulated 0.06 / 0.12 (match); sun
  native 0.24 / 0.35, emulated 0.11 / 0.32. So the dark hair in shade is
  the game's own shading (cac_hair gets only a flat `key * c14.w * 0.25`
  ambient; the body gets SH sky light, so the shirt keeps its colour). The
  difference is in sun: native hair is ~2x brighter than emulated, the same
  "native hair lighter" difference seen with brown hair, which then makes
  the drop into shade look extreme. Which sun brightness is right is open:
  native is closer to the Create-a-Skater swatch, and the console should
  decide. Suspects for the native brightness: the strand highlight's
  screen-space tangent (the skinned layout has no vertex tangent), the
  wrapped key ramp, or the static-sun stand-in for shadowWorld.
  **Partly fixed 2026-11-13** from the cac_hair_defaultPS ucode: the port
  clamped N.L to >= 0 before the wrap ramps (the game feeds the raw N.L,
  so the far side goes to 0) and ignored the hair's normal map (tf4, now
  bound; it adds strand detail, not average brightness). Sunlit hair
  0.239 -> 0.169 (emulated 0.111); shade unchanged (it matched).
  **Remaining gap = the strand highlight:** with it off, native is 0.129
  and the green channel matches emulated exactly; neither screen-space
  axis reproduces the game's (U 0.169, V 0.182). The game runs it along
  the mesh's stored tangent (usage 6), which our skinned layout drops.
  Proper fix: carry the hair's tangent through skinning. A/B knobs:
  `skate3_native_render_scene_hair_debug` (bits documented in the cvar),
  scenarios `hair_shade_terms.toml` / `hair_shade_terms2.toml`.
  **Done 2026-11-13:** skinned hair now carries its usage-6 tangent (packed
  into the unused decal-UV slot, skinned in vs_main, `tanb` w = 1) and the
  highlight runs along it. Hair constants on this skater: power c11.w = 5,
  highlight c17 = (0.32, 0.09, 0.13), c16 = (0.24, 0, 0.15) = the pink dye
  (the hair texture is grey). **The 22.80 s frame was a bad reference:** it
  sits on the arch's shadow edge, where the game's coarse shadowWorld and
  our static sun map disagree. In open sun (`scenarios/hair_open_sun.toml`,
  t = 20 s) native is 0.561 lightness vs emulated 0.479, both with a strand
  highlight (green, which only the highlight gives, 101 vs 87): native is
  ~15% brighter, not 2x. Also seen: at 10-15 s under the arch the emulated
  skater stays sunlit while standing in the floor shadow; native shades
  him (the game's coarse shadow misses him; ours is the better result).
  Left for the console: whether the remaining ~15% is real.
- **The skater's body may poke through the shirt** in small spots (seen once
  on Windows, reel `88a86b704bcf2d71`).
- **Distant textures glitch or pop in** on the Odin 2 (no details yet; needs
  a spot where it happens).
- **The skater customization screen has many renderer bugs** (check
  upstream's KNOWN_ISSUES.md first). Seen on the Odin 2 (2026-11-13): body
  modifications such as facial feature sizes don't apply, or only some of
  the time; various clothing and hair problems. Needs a lot of work.
- **The team screen's skater portrait doesn't update after Create-a-Skater**
  (Windows, 2026-11-13, `scenarios/cas_portrait.toml`): after a change and
  "Yes", the Edit Skaters screen keeps showing the previous portrait for the
  rest of the session, while the saved photo is right (the next boot shows
  it). Same with the old and new portrait-window behavior. Suspect a cached
  texture in the native 2D path (the card is re-composed at pitch 560 into
  0x03855000 / 0x03C55000), or the game reloading the photo only on boot.
  Check against the fully emulated renderer.
- **Create-a-Park is jittery and slow** on the Odin 2 even though the HUD
  shows a steady 30 fps. The 2026-11-13 census run showed guest frames at an
  even 33.3 ms and almost no emulated rendering there, so it isn't the
  emulated renderer or the guest frame rate. Suspects: frames presented with
  a stale scene or camera, or the editor camera's update. Needs a screen
  recording.
- **Photo challenges show no photo** on the final showcase screen. The game
  grabs the screen as soon as the shutter fires, before the native frame
  reaches the screenshot target (the log says "no native frame ready for
  the grab"); the reel-thumbnail prefetch only ran in the replay editor.
  **Fix written 2026-11-13, untested:** the prefetch also runs while the
  challenge camera (FE 2) is open. Needs a photo challenge played on the
  device.

- **Native rendering for the menus' remaining emulated pieces.** Some menu
  elements still come from the game's own (emulated) renderer: the skater
  portraits (team menu boxes, Import Skater card, Customize and
  Create-a-Skater), drawn into textures that the menus show. To get them,
  the "portrait window" lets the emulated renderer run, which today also
  runs the whole emulated scene (see Investigations). Long term, draw these
  natively (the native renderer already refuses the portrait passes'
  captures) so the emulated renderer can stay off in menus too.

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
- **Capture indicators:** show on screen when the harness is taking
  screenshots, a RenderDoc capture or a screen recording (and when a
  scenario or live session is driving the game), in the same indicator
  stack as the recorder badges. The indicator must stay out of the captured
  images themselves (drawn after the grab, or skipped on capture frames).
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
- **The real Xbox 360 as reference** (the user's modded console, once it's
  set up): learn to drive it (screenshots, saves, maybe input) so the
  original look can settle native-vs-emulated questions; the emulated
  renderer has its own errors (e.g. whether brown hair should be as dark as
  emulated or as light as native, which is closer to the Create-a-Skater
  swatch).
- **A realtime control API** next to the scenarios, for interactive
  driving (input, screenshots, state, cvars). Live mode is a first version:
  `send <verb>` drops a command file in the game's inbox and waits for the
  reply, ~0.5-1 s per command through rs.sh. Ideas: a socket (TCP or
  WebSocket; adb forward on the Odin 2) with no per-command process start,
  batched commands ("press A, wait 15 frames, screenshot"), waits on
  conditions (FE screen, frame count), screenshots returned directly, an
  event stream (FE changes, census, fps), and a small client library or
  REPL. Also fixes `live --target windows` dying when started from WSL.
- **Better screenshots and recordings:** have the renderer(s) write an
  image for every frame of a short capture (frame-exact, no screen
  recorder), on the desktop and the Odin 2; look at what limits today's
  `screenshot` bursts and recordings.
- **Headless desktop runs** for automated testing (Windows, Linux, macOS;
  not needed for linux-aarch64 / the Odin 2): start the game without a
  visible window or focus steal, so scenario runs don't interrupt the
  user's desktop. Look into an offscreen/hidden window or swapchain-less
  output, a virtual display, and what input and audio need.
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
  **First census (Odin 2, 2026-11-13, run `20261113-113428_live`,
  `native_render_pass_census` + `tools/harness/census_report.py`):**
  - No context presents fully emulated frames any more; the pause menu,
    loading screens, challenges and editors are all drawn natively. Outside
    the portrait window only ~10 tiny emulated draws a frame run (pitches 80
    and 320).
  - **The portrait window runs the whole emulated main scene.** The scene's
    surface pitch is 1200 (1152 rounded up to 80-pixel EDRAM tiles), not
    1152, so suppress mode 3's 1152 exclusion never matches. While the
    window is open, up to ~1,160 scene draws a frame execute (My Replays'
    screen 36: 583k indices a frame; Customize, screen 63: 826 draws a
    frame; Create-a-Skater: 61). The window opens on unclassified screens
    and for 3 s after many menu changes. **Fixed 2026-11-13.** Odin 2,
    Pause > Edit Skaters, uncapped (`scenarios/census_menus.toml`): 52 fps,
    worst frame 26 ms, against 29 fps and 100 ms with the old behavior;
    emulated scene draws there 0 against ~1,265 a frame; portraits intact.
    The portrait card is a 512x512 pass at
    pitch 560, so mode 3 now suppresses pitch >= 1152. The skater photo
    taken on leaving Create-a-Skater does render into the 1200 surface;
    new mode 4 lets it run during the skater photo window and while
    Create-a-Skater is open (checked across a reboot with
    `gen_cas_portrait.py --only on|off`). Escape hatch:
    `skate3_native_render_scene_portrait_scene_band`. Still to check: My
    Replays' screen 36, Import Skater, the photo display card.
  - Aux skater passes (pitch 160) run in challenge screens (screen 1: 4–12
    draws a frame) and heavily in photo challenges (screen 2: ~95 draws,
    265k indices a frame). In a photo challenge this may be how the game
    checks the skater is in frame, so don't skip it there without checking.

- **What in the game is tied to frame rate:** physics, animation,
  pedestrians, camera, input polling, timers. It affects how repeatable
  repros are and how 30 fps play feels.
- **Audio problems on the Windows build** (no details yet).
