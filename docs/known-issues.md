# Known issues (this fork)

Problems found while working on this fork, mostly with the
[scenario harness](scenario-harness.md). Upstream's list is
[KNOWN_ISSUES.md](../KNOWN_ISSUES.md); the numbers here continue its
sequence.

Each entry starts with what you would see on screen and how to make it
happen. The technical details follow for whoever works on it.

| Issue | What you see | Status |
|---|---|---|
| [KI-117](#ki-117-the-skaters-feet-and-body-parts-fly-off-at-high-speed) | Feet and other body parts come off the skater at high speed | Open |
| [KI-118](#ki-118-the-screen-goes-black-after-spawning-at-some-locations) | Black screen (HUD only) after spawning at some places | Open |
| [KI-119](#ki-119-enhanced-shadows-put-sunny-places-in-shade) | With Enhanced Shadows on, sunny places go dark and the skater loses their shadow | Mostly fixed |

---

## KI-117: The skater's feet and body parts fly off at high speed

**What you see:** while moving very fast (a long fall, a fast bomb), the
skater's feet, and sometimes other body parts, separate from the body and
trail behind or fly away.

**How to see it:** fall through the world. The harness does it reliably:
`scenarios/repro_fast_fall_body_parts.toml` teleports into a district that
isn't loaded yet, the skater drops through the ground, and screenshots every
20 frames show the parts coming off. Spawning at any invalid location does
the same.

**Status:** open, cause unknown · **Severity:** medium · **Found:** long-standing;
the harness gave it a reliable repro.

**Where it happens:** native renderer, in how the skater's skinned mesh is
captured and interpolated between frames.

<details>
<summary>Technical details</summary>

- At speed the log shows `ropa interp DETACH repaired` from
  `skate3_native_scene.cpp`: the bone-ring interpolation's detach guard is
  firing, which points at skinned capture or pose interpolation.
- Not yet checked with the emulated renderer (F5).
- **Next step:** run the repro with native and emulated stills (the harness
  `renderer` verb) to confirm it is native-only, then compare the detach
  guard's thresholds with the measured speeds.

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
