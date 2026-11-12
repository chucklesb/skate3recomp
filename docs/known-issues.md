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
| [KI-120](#ki-120-the-skater-jitters-worse-with-speed) | The skater looks jittery, as if falling behind and catching up, worse with speed | Open |

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

**What you see:** the skater model looks slightly jittery while skating, as
if the physics fell behind and caught up again and again. It seems
intermittent and gets more noticeable the faster you go. Most visible on the
Odin 2 (30 fps).

**How to see it:** skate at speed on the Odin 2 and watch the skater against
the background.

**Status:** open · **Severity:** low to medium · **Found:** long-standing;
reported again 2026-11-12 while checking the KI-117 fix.

<details>
<summary>Technical details</summary>

- On its own the skater's drawn motion is fairly steady (Odin 2,
  `repro_flip_setup_body_parts`): frames render at 33 ms, and the drawn speed
  follows the true speed within 0.2-0.3 m/s typically, 1.7-1.9 m/s at the
  95th percentile at 5-8+ m/s.
- **Hypothesis:** the jitter is the skater relative to the camera. The
  camera is smoothed from ~200 Hz samples on its own track; skater poses
  arrive at 30 Hz and are stamped when the renderer ingests them
  (`g_latest_cam_tick`), not when the game simulated them. Timestamp error
  becomes position error proportional to speed (33 ms at 8 m/s = 0.26 m).
- **Next step:** log the drawn camera position next to `char_track` and
  measure the skater's on-screen wobble at speed; compare with the emulated
  renderer (F5) and a fast PC.

</details>
