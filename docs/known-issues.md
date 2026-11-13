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
| [KI-122](#ki-122-canal-water-reflects-orange-brown-instead-of-blue-sky) | Canal water reflects orange-brown instead of blue sky, depending on the view angle; no shadows on the canals | Fixed |
| [KI-123](#ki-123-skatereel-playback-is-slow-and-uneven) | Reel playback runs in the teens with uneven pacing, where gameplay holds near 60 | Open |
| [KI-124](#ki-124-shadows-on-the-sun-facing-side-of-the-skaters-sleeves) | Shadow patches on the side of the skater's sleeves that faces the sun | Investigating (matches the game on Windows) |
| [KI-125](#ki-125-the-skater-comes-apart-for-a-frame-or-two-after-a-replay-jumps) | For a frame or two after a replay jumps to a new time, the skater's arms or legs are drawn detached | Open |

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

**Status:** fixed (Windows D3D12 and Vulkan, Odin 2) · **Severity:** medium (very
visible) · **Found:** 2026-11-12 by the user.

**Follow-up, also fixed:** with the colour fixed, the canal was still a
bright, even blue with no shadows on it (not the bridge's, not the
skater's), where the game shows murky water with blue glints, darker under
the bridge. The canal's shader had never been ported: it fell back to an
old approximation with no lightmap or shadow term. It now runs a port of
the game's own `wateralpha_defaultPS` and matches the emulated frame closely
(slightly brighter). Optional, on by default:
`skate3_native_render_scene_water_detile` (0..1) breaks up the per-metre
repeat of the ripple texture; 0 is the game's look.

**Second follow-up, Odin 2:** with the port, the Odin 2 drew the canal as a
bright, blocky sky-blue with dark specks, where Windows and the emulated
frame show the dark murk. The canal is almost all reflection, and its
reflection cube was the only cube kept at a partial mip chain, so the look
depended on how each GPU picks the cube's mip level. It now gets the full
chain like the other reflection cubes, and the Odin 2 matches the emulated
frame.

<details>
<summary>Technical details</summary>

- The canal material is `water.alpha` (`wateralpha_defaultPS`), not
  `water.flowingalpha`; only the latter had an exact branch. The port is
  fam 33 in `scene_water.hlsli` (`ShadeWaterAlpha`): a PCA normal from the
  `normal` (t3) and `normal2` (t8) maps at two scales, per-frame PCA weights
  (PS c9..c14) animating the waves, a refraction wobble on the diffuse and
  mask lookups, the 4-tap lightmap box clamped by shadow + shadow colour,
  phong sun spec and the cube reflection. Its constants (PS c8..c22 and its
  own sun c6) are captured when the canal's pixel bank uploads; the literal
  constants were read from the shader's constant block in guest memory.
- The canals have no bed: with the surface hidden there is only black, so a
  see-through surface has nothing to show (tried: a clarity + volume-fog
  option, dropped).
- The canal's water reflects a 128×128 environment
  cube in `k_5_6_5`, the only non-DXT1 cube seen (`native-scene: cube
  408007E0 128x128 fmt=4` with `log_level = "debug"`).
- `k_5_6_5` maps to the host `B5G6R5` format on the understanding that the
  upload swaps red and blue. Both 2D upload paths did that; the cube path
  (it copies the guest's own mip chain for non-DXT1/8888 formats) only
  swapped endianness. Blue sky with red and blue exchanged is orange-brown.
- Fix: `SwapRb565` in `skate3_native_scene_gpu_internal.h`, called by every
  `k_5_6_5` upload path, cubes included.
- Odin 2: the cube upload copied the guest's own chain for `k_5_6_5`, and
  only down to 32 px (smaller levels are packed in a shared tile). The
  canal's reflection runs at a deep gradient LOD, so the fetch clamped to a
  sharp level, and Turnip's cube LOD landed on blocky sky and black texels
  where desktop GPUs returned grey. `k_5_6_5` cubes now decode to RGBA8 and
  box-filter the full chain to 1×1, like DXT1 and 8888 cubes (the
  `rgba_chain` path); the red/blue order is handled in the decode.
- Debug view: `skate3_native_render_scene_water_debug` (1 diffuse, 2 masks,
  3 lightmap × shadow, 4 cube, 5 normal, 6 kd/shadow/lightmap, 7 fog and
  exposure) with `scenarios/water_debug.toml`, one shot per view. Comparing
  view 4 between the Odin 2 and Windows found the cube difference.
- Runs: before `out/harness-runs/20261112-113027_water_probe` (Vulkan);
  after `20261112-113232_water_probe` (Vulkan), `20261112-113338_water_probe`
  (D3D12); wateralpha port `20261112-160258_water_probe` (Vulkan),
  `20261112-160347_water_probe` (D3D12); Odin 2 before the full chain
  `20261112-161542_water_probe` (debug views `20261112-162653_water_debug`),
  after `20261112-163539_water_debug`; Windows after the full chain
  `20261112-163006_water_debug` (Vulkan), `20261112-163737_water_probe`
  (D3D12).

</details>

## KI-123: skate.Reel playback is slow and uneven

**What you see:** while a saved skate.Reel replay plays, the frame rate is
low, dipping into the teens at times, and both the frame rate and the frame
pacing are uneven. Playing the game live at the same places holds a
near-steady 60 fps in most areas (30 at worst) with no noticeable stutters.

**How to see it:** play any saved reel from My Replays on the Odin 2 and
compare with skating live in the same area.

**Status:** open, to be looked at later · **Severity:** medium (testing and
repro work relies on reels) · **Found:** 2026-11-12 by the user, Odin 2.

**Related, planned:** a way to turn off the camera lens effect (the fisheye
and vignette) during reel playback, so reel stills used for testing and
repros show the plain scene. See the technical details.

<details>
<summary>Technical details</summary>

Not investigated yet. Leads from reading the code:

- The harness `play_reel` path runs the reel as normal gameplay (presence 1,
  FE stack `[0,12]`); its last run held the 30 fps cap. The user's slow case
  is the game's own My Replays flow, which can enter code paths the harness
  never does:
  - **Portrait window.** `PortraitRttWindowActive`
    (`src/skate3_native_scene.cpp`, the FE id check) treats only FE ids
    0/56/24/17/59 as safe, so the replay editor (12) reached from the pause
    menu may keep the window open: suppress mode 3 and **synchronous shader
    compilation** for the whole replay. Log: "portrait window", "menu
    context - shader compilation synchronous".
  - **Photo flow.** `PhotoEditorSignal` (`src/skate3_native_scene_gpu.cpp`)
    fires on the `PhotoReplayController::Update` heartbeat; it arms the
    per-frame photo grab and synchronous compiles. Log: "photo flow".
- **To measure:** `scenarios/perf_reel.toml` uncapped, with a gameplay `wait`
  segment added to `tools/harness/perf_report.py` (it only splits `reelN`
  segments today), comparing `nat_total` and `guest_fps`; plus one run
  through the real My Replays flow, grepping the log for the lines above.
- **Lens effect:** the game's own final pass, `postfx_basictex_fisheye`
  (fisheye warp and vignette), drawn natively by the screen-fx path
  (`src/native/screen_fx.h`, `CaptureScreenFxState`, the apply step in
  `RenderScene`). Nothing turns it off today. Plan: a cvar
  `skate3_native_render_scene_screen_fx` (default on) that skips the capture
  (still clearing `g_frame_screen_fx`), and a `play_reel` option
  `lens = false` that sets it for the reel, like `SuppressUi()` hides the UI.

</details>

## KI-124: Shadows on the sun-facing side of the skater's sleeves

**What you see:** with the skater's back to the sun, dark shadow patches
sometimes appear on the back of the shirt sleeves, the side facing the sun.

**How to see it:** the user's Odin 2 reel, id `88a86b704bcf2d71` (clip 6
of the test saves; marker at 19.5 s of 25.2 s, sun high and behind the
skater). `scenarios/ki124_motion_native.toml` and
`scenarios/ki124_motion_emulated.toml` play it from the marker with a shot
every 2 frames on each renderer; `ki124_renderers.toml` holds five moments
on both; `ki124_motion_debug.toml` runs the character debug view.

**Status:** investigating · **Severity:** low · **Found:** 2026-11-12 by
the user, Odin 2.

**So far (Windows):** our frames match the game's own (emulated) frames,
held and in motion. The darker sleeve sides are the torso's own shadow
falling on the arm with the sun high behind the skater, and the game draws
the same. Whether the Odin 2 draws more than that is the open question; the
user confirms the artifact they saw on the device does not show on Windows.
Odin 2 runs `20261112-191628_ki124_motion_native`, `..191250_ki124_motion_emulated`
and `..191437_ki124_motion_debug` (about every second frame from the marker;
the device screenshots cannot keep up with every frame) did not catch it
either; a dark notch at a sleeve hem in `f0000901` is the cloth folding and
showing the sleeve's inside, which the emulated frames show too.

**Caught on video (Odin 2):** with a second reel, id `411a7b5e827d3d5f`
(clip 7, marker at 21.1 s of 24.8 s), recorded with scrcpy at 1x, the user
found it: for **one frame**, about 1.1 s after the marker (reel time ~22.2
s), a broad dark diagonal shadow band crosses the back and left side of the
shirt, with darker hair shadow on the shoulders, gone the next frame.
- **Repeatable:** the same moment in two passes of `ki124_realtime_ab.toml`
  (`C:\Users\user\Videos\ki124_realtime_ab_fixed.mkv` at 0:34.016 and
  0:58.376), so it is tied to the reel, not random.
- **Not the static sun map:** pass B had it off and still flashed.
- **Only in moving 1x playback:** not at quarter speed
  (`ki124_slowmo.toml`), and not in held stills of every frame from 22.10
  to 22.30 s (`ki124_flash_stills.toml`) on either the Odin 2 or Windows,
  where native also matches emulated.

So it depends on the sequence of frames, not on a frame's content:
something in the dynamic shadow path (the caster pose, the shadow rows)
belongs to a different moment than the drawn skater on that frame.

**Candidate fix (2026-11-12, awaiting the user's check on video):** the
shirt cast its shadow from a different shape than the one drawn. The
shadow pass bound the newest decoded cloth state, while the main pass draws
a blend of recent cloth states timed to the body. Measured at the Odin 2's
settings, the shadow-casting shirt sat 1.3 to 5.5 cm from the drawn shirt on
every frame, on both the Odin 2 and Windows. Garments now cast with the
drawn shape (`skate3_native_render_scene_shadow_ropa_drawn_shape`, on by
default; off restores the old caster). The trace shows no one-frame jump
at the flash moment on either platform, so whether this removes the flash
rests on the recording: `scenarios/ki124_ropa_caster_ab.toml` plays the
reel with the old caster (pass A) and then the fix (pass B); Odin 2 run
`20261112-211050_ki124_ropa_caster_ab`, video
`C:\Users\user\Videos\ki124_ropa_caster_ab_fixed.mkv` (pass A's reel starts
at about 0:24.2, pass B's at about 0:48.5). If pass B still flashes, the
next suspect is the per-frame caster set (`shadow_caster_parity`).

**Also noted (the user):** fabric wrinkles, hair shadows and muscle
definition look harsher than they should. Windows held stills against the
emulated frame (`20261112-205027_ki124_flash_stills`) agree: our shirt
folds are sharper and more contrasty, the game's broad and soft.

**Related, not investigated:** on Windows the user noticed what looked like
small areas of the skater's body poking through the shirt in this reel.

<details>
<summary>Technical details</summary>

- Character shading (`ShadeCharacter`, fam 1/2) is the key light
  `ch_key * saturate(N·ch_light) * csm`; pixels facing away from the light
  already get no key light, so sun-side patches can only come from the
  shadow term.
- Ruled out on Windows Vulkan:
  - **Light vs shadow direction:** the characters' light row (PS c9) equals
    the world sun c6 for every character family (0.0°; log line
    `native-scene: char light vs world sun`).
  - **The cloth shirt casting from another shape (on Windows only):** the
    shadow caster bound the resident (newest) shape while the visible shirt
    blends shape generations; `skate3_native_render_scene_ropa_blend =
    false` made no difference on Windows (upper-body red pixels per frame,
    mean 236 vs 242), but Windows never showed the flash, so this did not
    rule it out for the Odin 2. Now fixed regardless (see the candidate fix
    above); with `skate3_native_render_scene_char_track` on, the log line
    `ropa-shadow f=... resident_vs_drawn_cm=` gives the old caster's offset
    per frame, and `BlendRopaShape` (`skate3_native_scene_gpu.cpp`) builds
    the one shape both passes use.
  - **Shadow tile size:** 512 and 1024 look the same. (The Odin 2's
    settings set 1024; the Windows test profile now matches the device's
    settings: 1280x720, a fixed 30 fps cap, 1024 tile and static map.)
- Native vs emulated: `20261112-185310_ki124_renderers` (held, SSIM 0.93
  "close"), `20261112-185506_ki124_motion_native` vs
  `20261112-185548_ki124_motion_emulated` (in motion): the same shading on
  the sleeves.
- Debug view `skate3_native_render_scene_char_debug`: 1 shadow term, 2 N·L,
  3 shadow with sun-facing-but-shadowed pixels in red, 4 the dynamic
  shadow map only. View 3 marks legitimate self-shadow too (an arm inside
  the torso's shadow faces the sun but is occluded), so compare its red
  against the emulated frame, not against zero.
- Next: the same motion scenarios on the Odin 2 (the reel is on the device
  as clip 6), against Windows. A device-only difference would point at
  Turnip (shadow depth precision or compare).

</details>

## KI-125: The skater comes apart for a frame or two after a replay jumps

**What you see:** right after a replay jumps to another time (the harness
starting a reel from its marker; likely also scrubbing in the replay
editor), the skater is drawn wrong for a frame or two: an arm missing from
its sleeve, a hand or leg floating apart from the body, and no ground
shadow. The game's own (emulated) frames at the same moment are fine.

**How to see it:** `scenarios/ki124_motion_native.toml` on the Odin 2, the
second shot after the jump: `out/harness-runs/20261112-191628_ki124_motion_native/shots/f0000764_reel_88a86b704bcf2d71_00.png`
(found by the user). Compare `20261112-191250_ki124_motion_emulated`.

**Status:** open · **Severity:** low (one or two frames; mostly hurts
replay stills and repros) · **Found:** 2026-11-12 by the user.

<details>
<summary>Technical details</summary>

Not investigated. Likely the pose smoothing (KI-117/KI-120: the body and
cloth pose rings, and the 8-tap boxcar the drawn pose is filtered with)
spanning the jump, so pieces are drawn from poses on either side of it,
and the shadow casters drop out for those frames. Suspected fix: detect
the discontinuity (a large root jump between consecutive poses, or the
replay clock jumping) and reset the rings, as a teleport should.

</details>
