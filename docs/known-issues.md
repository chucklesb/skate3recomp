# Known issues (this fork)

Problems found while working on this fork, mostly with the
[scenario harness](scenario-harness.md). Upstream's list is
[KNOWN_ISSUES.md](../KNOWN_ISSUES.md); the numbers here continue its
sequence.

Each entry starts with what you would see on screen and how to make it
happen. The technical details follow for whoever works on it.

| Issue | What you see | Status |
|---|---|---|
| [KI-117](#ki-117-the-skaters-feet-and-body-parts-fly-off-at-speed) | Feet and other body parts come off the skater at speed, e.g. setting up a flip | Fixed (Odin 2) |
| [KI-118](#ki-118-the-screen-goes-black-after-spawning-at-some-locations) | Black screen (HUD only) after spawning at some places | Open |
| [KI-119](#ki-119-enhanced-shadows-put-sunny-places-in-shade) | With Enhanced Shadows on, sunny places go dark and the skater loses their shadow | Mostly fixed |
| [KI-120](#ki-120-the-skater-jitters-worse-with-speed) | The skater looks jittery, as if falling behind and catching up, worse with speed | Fixed (Odin 2) |
| [KI-121](#ki-121-on-d3d12-the-scene-is-sometimes-drawn-in-shade) | On D3D12, a sunny scene is sometimes drawn almost all in shade, with no skater shadow | Open |
| [KI-122](#ki-122-canal-water-reflects-orange-brown-instead-of-blue-sky) | Canal water reflects orange-brown instead of blue sky, depending on the view angle | Fixed (Windows) |

---

## KI-117: The skater's feet and body parts fly off at speed

**What you see:** while skating at a moderate speed, especially while
setting up a flip trick (right stick held down), one of the skater's body
pieces (the feet, arms or another skin piece) is drawn behind the rest of the
body for a few frames, sometimes metres away, then snaps back. On the Odin 2
at 30 fps it happens all the time; on a fast PC it's rare. Long falls show it
too.

**How to see it:** on the Odin 2, skate at Lower Campus and hold the right
stick down for flip setups. The harness replays a real session that shows
it: `scenarios/repro_flip_setup_body_parts.toml` (with
`repro_flip_setup_body_parts.s3rec`).

**Status:** fixed (2026-11-12), verified on the Odin 2 by replay and by hand
· **Severity:** medium · **Found:** long-standing.

**What was wrong:** when the renderer can't read a body piece's pose for a
frame, it reuses that piece's last pose, up to 10 frames old, so the piece
doesn't flicker. Those poses are stored in world coordinates, so on a moving
skater the reused pose stayed where the skater *had been*: up to about 2.7 m
behind at 8 m/s. At 30 fps the pose read fails often during flip setups.

**The fix:** the reused pose now moves along with the skater (the owning
character's movement since the pose was stored is applied to it). The piece
stays on the body; at worst its limb pose is a few frames old. The setting
`skate3_native_render_scene_rescue_reanchor` (default on) switches it off for
comparison.

**Results (Odin 2, replaying the recording):**

| | Before | After |
|---|---|---|
| Frames with a piece drawn left behind | 19 (0.26 m, 0.53 m, 0.79 m… growing) | 0 |
| Stretches with a piece frozen in place | 8 (up to 12 frames) | 0 |
| Largest frame-to-frame jump between body and shirt | 3.3 m | 0.54 m |
| Scenario | fail | pass |

By hand on the Odin 2 the limbs stay on the body. A general jitter of the
whole skater remains; that's [KI-120](#ki-120-the-skater-jitters-worse-with-speed),
an older, separate issue.

<details>
<summary>Technical details</summary>

- **Where:** `BuildFrameScene` in `skate3_native_scene.cpp`, the
  per-instance rescue (`dbg_src` 3): a refused or pending skinned character
  capture whose ctx didn't publish re-publishes `g_bones_cache_ctx[ctx]`
  (<= 10 guest frames old). The authoritative palette serve
  (`ServeAuthoritativePalettes`, `dbg_src` 11) runs before it and skips
  pending items, so nothing corrected the stale copy.
- **Fix:** the cache also stores the owner entity's world block
  (`ReadEntityWorldRows`); the rescue applies `E_now · E_cached⁻¹` to every
  bone (`AffineDelta3x4` / `ApplyAffine3x4`; `dbg_src` 13; counter
  `g_rescue_reanchored`). If the owner can't be read, or the delta isn't
  invertible, the old behaviour is kept and logged as
  `stale palette rescue … moved=…`, which the regression scenario fails on
  for the player (`fam=2`).
- **Found with** the `char_track` diagnostic
  (`skate3_native_render_scene_char_track`): per frame, the body's and the
  shirt's raw (`rp`) and drawn (`fp`) positions. The frozen stretches were
  all mesh `4052EFE0` from `dbg_src` 3. The harness `record` command
  (Ctrl+Shift+R doesn't work on Windows) captured the session.
- The original fall repro (`repro_fast_fall_body_parts.toml`) reaches ~96
  m/s but shows the bug only occasionally on the current build.
- **Not covered:** garment (ropa) rescues (`dbg_src` 4, mesh-keyed cache up
  to 30 frames) still serve their cached state without re-anchoring; the
  post-interpolation DETACH guard catches the worst of those.

</details>

---

## KI-118: The screen goes black after spawning at some locations

**What you see:** after teleporting or spawning at certain places, the
screen stays black. The HUD still draws and the game keeps running, but the
world never appears. Waiting doesn't help, so it isn't a slow load.

**How to see it:** teleport to an affected locator with the harness
`teleport` verb. The surveyed locators are in
[teleport-locations.md](teleport-locations.md); the black ones still need to
be marked there.

**Status:** open · **Severity:** low to medium · **Found:** 2026-11-08, while
surveying teleport locations.

**Where it happens:** native renderer or streaming.

<details>
<summary>Technical details</summary>

- Not yet checked with the emulated renderer (F5).
- **Next step:** mark the affected locators, compare native and emulated
  (F5, or the harness `renderer` verb), and take a RenderDoc capture at one.

</details>

---

## KI-119: Enhanced Shadows put sunny places in shade

**What you see:** with **Enhanced Shadows** turned on (Options > Graphics),
places the game normally shows in bright sun are drawn in shade. When the
skater stands there, they lose their ground shadow (it vanishes into the
shade), long hair turns into a dark, hood-like shape, and the whole scene
looks darker. With the option off (the default), everything looks right.

**How to see it:** turn Enhanced Shadows on and play saved replay 2 to the
18-second mark: the open plaza goes dark. Before the fix the Peterson
Pavilion spawn also showed it on the skater. The harness scenario
`scenarios/ki119_static_sun.toml` shoots that replay frame with the option
off, on, and on without the fix.

**Status:** mostly fixed (2026-11-12, verified on Windows and the Odin 2)
· **Severity:** medium · **Found:** 2026-11-11, by the harness's replay
stills.

**What was wrong:** the game has two kinds of building shadows. The
original *baked* shadows are painted into the world's lighting textures
(lightmaps). Enhanced Shadows adds *live* shadows on top, so that skaters and
props also fall into building shade, and the darker of the two wins. The
live shadows used a sun about 15° lower in the sky than the sun the baked
lighting was made with. A lower sun throws longer shadows, so buildings and
canopies shaded ground the game shows as sunny.

**The fix:** the live shadows now use the sun raised by 15°, so they land
where the baked ones are. Only Enhanced Shadows uses the raised sun; the
game's lighting, its own shadows and the sky are unchanged. The amount is a
setting (`skate3_native_render_scene_shadow_static_elevation_offset`,
default 15, 0 = the old behaviour).

**Results:** compared with the option off,

| Place | Before the fix | After the fix |
|---|---|---|
| Replay 2 at 18 s (the plaza), Windows | 67% of the picture wrongly darkened | under 1% |
| Replay 2 at 18 s, Odin 2 | as dark as on Windows (SSIM 0.85) | SSIM 0.99, closer than Windows |
| Replay 2 at 6, 10, 14 s, Windows | slightly darker | matches more closely |
| Peterson Pavilion spawn, Windows | skater and ground in shade | looks normal |

The Odin 2's frames also match the Windows ones for each setting (SSIM
0.97-0.98).

**What remains:**

- **Core Plaza, at the spawn, under the giant overhead structure:** the
  structure's shadow now covers a path that the original game shows in sun
  (with the old sun it slipped in underneath). Physically plausible, but not
  how the original looks. Away from the structure's shadow, Core Plaza looks
  normal.
- **One unexplained D3D12 run (1 of 4):** the plaza showed up dark, but
  unlike the original bug the skater was lit normally, so it looks like a
  separate glitch that only affects world surfaces. It did not come back in
  three repeats. All Vulkan runs and the Odin 2 run were fine.

<details>
<summary>Technical details</summary>

- **Isolating it.** Windows with the test profile's settings differed from
  the Odin 2 (default settings) by up to 54% of the pixels on the same replay
  frame (SSIM 0.82). Turning off only static casters
  (`skate3_native_render_scene_shadow_static_casters`) matched the Odin 2;
  turning off only PCSS changed nothing. Raising the receiver bias
  (0.08 → 0.5 m), shrinking the map's radius (180 → 60 m), or skipping
  oversized or stale casters made no difference either.
- **The map itself is correct.** A RenderDoc capture showed terrain, trees
  and buildings in the right places. The sun axis it uses, c6 (the material
  sun, 47° up), is fixed in the world and does not follow the camera.
- **Measuring the baked sun.** A temporary debug view drew, for each
  lightmapped pixel, the raw lightmap brightness and the live shadow term in
  the same frame. Sweeping the sun override over azimuth and elevation and
  scoring the overlap of baked and live shade (IoU) gave:

  | Location | c6 | Best fit | IoU, c6 → best |
  |---|---|---|---|
  | Replay 2 plaza (4 stills) | az 315°, el 47° | az 308–315°, el 57–62° | 0.71 → 0.81 |
  | Core Plaza | az 298°, el 47° | az 283–298°, el 62–72° | 0.44 → 0.64 |

  So the baked sun has c6's compass direction but sits about 15° higher
  (roughly 60–65°, ±7°; the fit is a plateau). No runtime value holds that
  direction: the sky's sun and the dynamic-object sun equal c6, and the
  game's own shadow projections point straight down.
- **The fix** (`6ad8ae3`): `RenderStaticSunMap` raises c6's elevation by the
  offset before building the map; the override from the lighting lab
  (`skate3_native_render_scene_sun_override`) is used as is. The map's
  rebuild log prints the elevation used and the captured one.
- **Known small mismatches** (would need a shader change plus regenerated
  Vulkan SPIR-V): the receiver's slope bias and the volumetric shafts still
  use c6 (`sh_sun`). On flat ground the bias is about 3 cm larger than
  needed, on the safe side.
- **Tools:** `play_reel stills_variants` shoots one replay frame under
  several settings; the per-rebuild log lines are `static sun rebuild`,
  `csm_axis` and `dynobj_sun`.

</details>

---

## KI-120: The skater jitters, worse with speed

**What you see:** while skating fast, the skater looks jittery against the
background, as if the physics fell behind and caught up again and again. It
gets worse the faster you go. Most visible on the Odin 2 (30 fps).

**How to see it:** on the Odin 2, bomb a long straight run (Observatory
Road, Lower Campus) and watch the skater. Setting
`skate3_native_render_scene_dyn_stamp_mode=0` brings the old behaviour back
for comparison.

**Status:** fixed (2026-11-12), verified on the Odin 2 by hand and with
measurements · **Severity:** medium · **Found:** long-standing.

**What was wrong:** the renderer smooths the camera and the skater
separately and plays both back at the same moment. That only works if both
are timed with the same clock. The camera's samples carry the exact time the
game wrote them. The skater's poses were meant to borrow that time, but only
if it was under 20 ms old, a limit made for a PC's ~200 Hz camera. At 30 fps
the camera updates every 33 ms, so the check always failed and the skater
was timed by the render loop instead. The two clocks differed by a varying
~10 ms, which at 8 m/s is centimetres of back and forth every frame. A
second 30 fps assumption added to it: a rule for slow-updating traffic also
caught the skater (it updates every 33 ms too), drawing it a further 17 ms
behind the camera.

**The fix:** each frame, the skater's pose is timed with the moment the game
wrote that frame's own camera (both come from the same simulation step), and
"slow-updating" is judged against the camera's own update rate instead of a
fixed 60 Hz. Both changes are behind `skate3_native_render_scene_dyn_stamp_mode`
(default 1; 0 = the old behaviour). PCs at high frame rates behave as
before.

**Results (Odin 2, one live session, the setting switched every 12 s while
skating; on-screen movement jitter at 8+ m/s, pixels at 1080p):**

| | Typical frame | Worst 5% |
|---|---|---|
| Before (1422 frames) | 6.5 | 142 |
| After (2708 frames) | 1.3 | 8.3 |
| The game's own frames (reference) | 1.4-1.8 | 7-12 |

On Windows at 30 fps (replaying the recording): before 4.2 / 42, after
1.0 / 13.

<details>
<summary>Technical details</summary>

- **Where:** `InterpolateDynamicItems` in `skate3_native_scene.cpp`, the ring
  pose `stamp` and the per-entity playback point `play_e`. `SmoothCamera`
  now remembers the last 16 sampler views and finds the one equal to this
  frame's guest view (`g_frame_cam_tick`, `g_frame_cam_match`); it also
  exports its tick period (`g_cam_period`).
- **Measured with** new `char_track` body fields: `scr` / `rscr` (the body's
  NDC through the drawn and the raw camera), `stamp`, `tick`, `cam_n`,
  `clamp`, `cm` (frame camera matched) and `sm` (the mode), plus
  `tools/harness/char_jitter.py` (second difference of the on-screen
  position by speed, split by mode when it changed live).
- **Ruled out:** a starved smoothing window (no boxcar tap ever clamped to the
  newest pose) and uneven guest stepping (exactly one camera update per
  frame at 30 fps).
- **Before the fix:** at 30 fps `now - g_latest_cam_tick` was 17-36 ms
  (median 29), so poses were stamped `now` while the camera ring kept sampler
  times.
- **Regression scenario:** `scenarios/ki120_skater_jitter.toml` replays the
  recording (Lower Campus, ~54 s of fast skating); check with
  `char_jitter.py`. Input replays drift (pedestrians, timing), so compare
  speed groups, and prefer a live session that switches the setting for
  A/B.

</details>

---

## KI-121: On D3D12, the scene is sometimes drawn in shade

**What you see:** with Enhanced Shadows on, a sunny scene is now and then
drawn almost entirely in shade: cold, bluish light, only a strip of sun
along one wall, and no shadow under the skater. The same frame on the next
run (or on Vulkan) has normal sun and the skater's shadow. It looks like the
old KI-119 over-shadowing, but it comes and goes.

**How to see it:** run `scenarios/reel_stills.toml` on Windows with
`--backend d3d12` a few times and compare the 18 s still (`reel2_t18.00`).
It happened in 1 of 3 runs on 2026-11-12, and once during the KI-119 work
(2026-11-11). Not seen on Vulkan or on the Odin 2.

**Status:** open, not investigated yet · **Severity:** low to medium ·
**Found:** 2026-11-11, again 2026-11-12.

<details>
<summary>Technical details</summary>

- Runs (Release, 878a567): shaded `out/harness-runs/20261112-062845_reel_stills`;
  normal `20261112-063433_reel_stills`, `20261112-063545_reel_stills` (SSIM
  1.000 against Vulkan `20261112-062953_reel_stills`).
- The static sun map was built identically on both backends in the shaded
  run: `static sun rebuild` lines show the corrected sun (elev 62.4,
  captured 47.4) and ~1600-1660 casters. So the KI-119 offset is applied;
  what differs is what D3D12 renders or samples from the map that time.

</details>

---

## KI-122: Canal water reflects orange-brown instead of blue sky

**What you see:** in the native renderer, the canals' ripples reflect an
orange-brown colour where the game shows blue sky. How much depends on the
angle between the camera and the water: looking along the surface it covers
most of the canal. Any canal water shows it.

**How to see it:** `scenarios/water_probe.toml` teleports to the Downtown
canal by the Villas (Plaza of Solitude) and takes the same view in the
native and emulated renderers (`canal_nat`, `canal_emu`).

**Status:** fixed (Windows, D3D12 and Vulkan) · **Severity:** medium (very
visible) · **Found:** 2026-11-12 by the user.

**Remaining difference:** with the colour fixed, our canal is a brighter,
more even blue than the game's, which is mostly dark with blue glints and
darker under the bridge. That's the reflection's strength and occlusion,
not its colour; not investigated yet.

<details>
<summary>Technical details</summary>

- The canal's water (`flowingwateralpha`) reflects a 128×128 environment
  cube in `k_5_6_5`, the only non-DXT1 cube seen (`native-scene: cube
  408007E0 128x128 fmt=4` with `log_level = "debug"`).
- `k_5_6_5` maps to the host `B5G6R5` format on the understanding that the
  upload swaps red and blue. Both 2D upload paths did that; the cube path
  (it copies the guest's own mip chain for non-DXT1/8888 formats) only
  swapped endianness. Blue sky with red and blue exchanged is orange-brown.
- Fix: `SwapRb565` in `skate3_native_scene_gpu_internal.h`, called by every
  `k_5_6_5` upload path, cubes included.
- Runs: before `out/harness-runs/20261112-113027_water_probe` (Vulkan);
  after `20261112-113232_water_probe` (Vulkan), `20261112-113338_water_probe`
  (D3D12).

</details>
