# Native-only rendering

The plan to stop the game's own (emulated) renderer from drawing anything, and
then to remove it and the D3D12 backend. The milestones are in
[roadmap.md](roadmap.md).

## Why

The native renderer draws every frame the player sees. In a few places, the
SDK's emulated Xenos pipeline still executes draws, because the game renders
something offscreen and then samples it or reads it with the CPU. Each case
was handled with another "window" that lets the emulated pipeline run for a
while: suppress modes 3 and 4, forced CPU readbacks, synchronous shaders, and
per-pitch exemptions. They keep causing bugs:
- **Dark bands on Vulkan skater photos:** the photo samples the emulated
  shadow atlas.
- **The team box not refreshing on Vulkan:** the photo atlas resolve was
  dropped. It was fixed with yet another exemption (f184bb6).
- **Create-a-Skater rendering its whole scene emulated:** fixed, 55 -> 85 fps
  on the Odin 2 (46d93c9).
- **The replay editor's extra passes** ([KI-123](known-issues.md)).

**Goal:** no emulated draw executes in default play. The windows and overrides
can then be deleted, and later the emulated renderer and D3D12 with them.

**Not a goal: removing the SDK's command processor.** The native renderer is
fed by it: packet parsing, register state, and resolve and copy tracking.
"Native-only" means emulated *draw execution* stops, not packet processing.

## Platforms

Every platform the fork can build for has Vulkan:
- linux-aarch64 (DroidDeck);
- Linux x64;
- Windows;
- macOS, which already renders through MoltenVK in upstream's build
  (`tools/package_macos.sh`).

So dropping D3D12 and the emulated renderer loses no platform. We can't test
macOS here; MoltenVK supports a subset of Vulkan, so it gets checked if a Mac
becomes available.

## What still runs emulated

With default settings, from the code and the 2026-11-13 census:

| # | Where | What runs emulated | How it's let through (`src/skate3_native_scene_gpu.cpp`) |
|---|---|---|---|
| A | Accepting a Create-a-Skater edit, entering Import Skater, the career "new team member" prompt | The skater photo: a 1152x640 render at pitch 1200, read by the CPU (`PhotoEffects::GeneratePhoto`). The team photo atlas, 1536x512 at pitch 1600 | `UpdateSkaterPhotoWindow`: mode 4, sync shaders, readback <= 0x2D0000, pitch 1600 exempt |
| B | Menus: for 3 s after a screen change, and on unclassified screens | The team card compose (512x512, pitch 560, 2 draws). An MSAA pass (pitch 600, 3 draws, not identified) | The portrait window in `YieldForMenus`: mode 3, sync shaders |
| C | A photo challenge's shutter | The shot and the framed-card composite frames, read and composed by the CPU | The shutter burst in `UpdatePhotoGrabWindow`: suppression fully lifted for ~1-3 frames, readbacks |
| D | Challenges and photo challenges | The aux skater pass, pitch 160: 4-95 draws a frame. In a photo challenge it may be how the game checks that the skater is in frame | Mode 2 lets pitch <= 512 run |
| E | Everywhere | ~10 tiny draws a frame at pitches 80 and 320; consumers unknown | Mode 2 lets pitch <= 512 run |
| F | Fallbacks only | An FMV the native path fails to substitute; the photo-grab fallback; the escape hatches `cas_yield`, `photo_display_yield`, `photo_yield`, `menu_unsuppress`, `portrait_scene_band` (all off) | `YieldForMovie`, the photo-grab watchdog, `YieldFor*` |

## Approach

### A shared building block: the native offscreen pass

A, B, C and the atlas have the same shape. The game renders into a target
smaller than the screen and resolves it to a guest address X. Something then
uses the result:
- a menu quad samples it later (B, the atlas), or
- the CPU reads it (A, C).

One mechanism covers both:
- **Native draw:** render the pass natively, either 2D quads (a compose) or a
  3D scene (the photo), into a native texture registered for X.
- **GPU consumers:** `emit_draw` substitutes that texture by address. The FMV
  path already swaps textures this way.
- **CPU consumers:** write the result back to guest memory, tiled and
  big-endian. This generalises `WriteGrabToGuest`, which today handles only
  0x04911000 at 1152x640, to any address, size and format.

Everything goes through the native RHI (`native_rhi_vulkan.cpp`). It already
has offscreen targets and readback buffers (`blur_tex`, `grab_readback`), so no
new RHI features are expected. D3D12 keeps compiling for free until phase 6
removes it.

### Phases

Each phase lands behind a cvar that defaults on. The old window stays as the
fallback until phase 5. The sizes are relative.

| Phase | What | Removes | Size |
|---|---|---|---|
| 0 | **Measure.** The native-only audit: census events record the windows that were open (`holders`), and `census_report.py` ends with one line per screen context: ZERO, or the executed emulated draws per frame, their pitches and the windows open. Run the native tour scenario (see "Before we start") on Windows Vulkan, then on the Odin 2, as the baseline | nothing | S |
| 1 | **Offscreen 2D compose** (B, then the atlas). The native 2D pass drops "offscreen 2D compose draws" today. Draw them into native targets keyed by resolve address, and substitute them in `emit_draw` / `resolve_2d_texture` | the portrait window (mode 3) | M |
| 2 | **Native skater photo** (A). Stop refusing the photo pass (`CaptureSkinnedState`'s aux refusal, `BuildFrameScene`'s portrait-publish gate). Capture it into a separate scene slot with its own palettes, so it can't disturb the on-screen player's. Render it once per `GeneratePhotoRequest` into a 1152x640 target with the photo camera, and write it to guest memory | the skater photo window: mode 4, readbacks, sync shaders, pitch 1600. Should also fix the photo bands | L |
| 3 | **Photo-challenge card** (C): phase 1's mechanism, plus writing the shot and composite back to guest memory | the shutter burst | M |
| 4 | **Small leftovers** (D, E, the MSAA pass). Find what reads each one. If nothing does, suppress it (checked with A/B screenshots and `shots.py classify`); otherwise use phase 1's mechanism | mode 2's pitch <= 512 allowance | S-M |
| 5 | **Tighten.** The default suppress mode becomes "nothing executes". Delete the windows, the `SdkCvarOverride` holders, the photo-grab fallback and the dead escape hatches | the window machinery | S |
| 6 | **Shed the baggage**, once phase 5 has held up in play (its own detailed plan then). (1) Remove D3D12: `native_rhi_d3d12.cpp`, the d3d12 command processor and presenter, the Windows build's D3D12 parts, and the harness's `--backend d3d12` and A/B D3D12 leg. (2) Remove the emulated draw path: pipeline and shader translation, the render-target/EDRAM cache, the emulated swap, F5, the suppress modes, and the census "emulated" columns. Packet processing and resolve tracking stay as far as the native renderer needs them | D3D12, the emulated renderer | L |

**After phase 6:**
- There is no fallback when the native renderer breaks, and no emulated
  reference frame for A/B checks. Phase 0's audit must show zero emulated
  draws across the full tour first.
- Every shader edit needs `tools/generate_native_spirv.py`. The quick path of
  D3D12 compiling HLSL at startup goes away.

## Before we start

**Status, 2026-11-14:**
- **Done:** item 1 (published as the full release v2.1.0-aarch64.3, not a
  testing pre-release); the Windows parts of 3-5 (see Baseline).
- **Still to do:**
  - the Odin 2 baseline run of the tour;
  - item 2, the branches.

1. **A known-good build for testers:** publish the current state as a testing
   pre-release before phase 1 lands, so there is something to compare
   against.
2. **Branches:** a `native-only` branch in this repo and a matching branch in
   the SDK fork. Each phase merges into `linux-aarch64-cross` once verified.
3. **A native tour scenario** that reaches every row of the table. These are
   missing today:
   - a photo challenge's shutter and display card (C);
   - the career "new team member" prompt (A);
   - the intro FMV (F);
   - a challenge screen (D).

   Some need a save placed at the right point. Test saves may be written
   freely.
4. **Baseline numbers:** phase 0's audit and fps per context, recorded here.
5. **Reference shots** from the current build for each context, for
   `shots.py classify`.
6. **Done means, for each phase:**
   - the audit shows ZERO for that context;
   - shots classify as the baseline, except for intended fixes (for example
     the photo bands);
   - fps is no worse;
   - the fallback cvar still restores the old behaviour.

## Baseline (phase 0)

**Scenarios:** `scenarios/native_tour.toml` and `native_tour_boot.toml`
(generated by `tools/harness/gen_native_tour.py`). Read a run with
`census_report.py <run> --audit-only`; the numbers are emulated draws
executed per frame.

**Windows Vulkan, 2026-11-14** (runs `20261114-011647_native_tour`,
`20261114-012141_native_tour_boot`):

| Context (FE stack) | Draws/frame | Pitches | Windows open | Table row |
|---|---|---|---|---|
| Free skate `[0]`, loading, challenge map `[0,17]`, challenge `[0,1]`, intro movies and title `[0,47]` `[0,5]` `[0,24]` `[0,22:0]` | 10.0 | 80: 6, 320: 4 | none | E |
| Pause `[0,56]` | 10.6 | + 560, 600 | portrait_window | B |
| Team screen `[0,56,63]` | 15.1 | + 560: 2, 600: 3 | portrait_window | B |
| Create-a-Skater `[0,56,63,15]`, two accepted edits | 15.6 | + 560, 600, 1200: 0.5 | portrait_window, skater_photo, replay_aux | A, B |
| Import Skater `[0,56,63,68]` | 19.4 | + 560, 600, 1200: 4.1 | portrait_window, skater_photo | A, B |
| Replay editor `[0,56,12]`, Take Photo | 11.2 | + 560, 600, 640 | photo_flow, portrait_window, replay_aux | B |
| My Replays `[0,56,59]` | 10.5 | + 560, 600 | portrait_window | B |
| Photo viewer `[0,56,59,36]` | 15.0 | + 560: 2, 600: 3 | portrait_window | B |

- **No frame was presented emulated.** The intro movies render natively.
- **The replay editor's Take Photo is already native:** it opens only
  `photo_flow` (synchronous shaders), with no extra emulated draws.
- **Not reached by the tour:** a photo challenge's running, shutter and
  photo screens (rows C and D), measured below from a live session instead,
  and the career "new team member" prompt (row A), still unmeasured.

**Photo challenges, Odin 2, 2026-11-15** (run `20261115-153557_live`: the
user played two photo challenges live with the census on; no input replay):

| Context (FE stack) | Draws/frame | Pitches | Windows open | Table row |
|---|---|---|---|---|
| Free skate `[0]`, challenge map `[0,17]`, challenge intro `[0,1]` | 10.0-11.1 | 80: 6, 320: 4 (+ 160: 1.1 in the intro) | none | E |
| First challenge, running `[0,1:47]` | 82.3 | + 160: 72.3 | none | D |
| First challenge, shutter and shots `[0,1:47,2]` | 111.1 | + 160: 96, 1200: 4.1, 1600: 0.5, 1280: 0.2, 600, 640 | photo_burst, photo_flow | C, A |
| First challenge, photo card `[0,1:47,2,69]` | 119.2 | + 160: 96, 1200: 10.6, 1600: 1.4 | photo_burst, photo_flow | C, A |
| Second challenge, running `[0,1:11]` | 22.0 | + 160: 12 | photo_flow | D |
| Second challenge, shots `[0,1:11,2]` | 22.0 | + 160: 12 | photo_flow | D |
| Second challenge, photo card `[0,1:11,2,69]` (left up, 7260 frames) | 22.7 | + 160: 12, 1200: 0.7 | photo_burst, photo_flow | C, D |

- **Row D is the biggest emulated cost anywhere:** the aux skater pass at
  pitch 160 runs 72-96 draws a frame through the first challenge, from the
  start of the challenge to the photo card. The second challenge's is smaller
  (12 a frame).
- **The photo screens** also run the skater-photo pitches (1200, 1600) of
  row A, through the photo windows.
- **Seen there, not chased:** the user reported that the animation at the
  end of a photo challenge looks jerky, as if it steps back a frame at each
  photo, and that the skater's pose snaps into place while choosing a shot
  ([open-work.md](open-work.md)).

## Verification

- **Windows Vulkan first:** the phase's cvar on, then off. Check the audit, and
  `shots.py classify` against the reference shots.
- **Then the Odin 2:** the same scenario and the audit, plus fps
  (`cas_perf.toml`, `census_menus*.toml`). One device run at a time.
- **D3D12** only has to compile until phase 6.
- **Docs:** this page, [menu-portraits.md](menu-portraits.md) and
  [open-work.md](open-work.md); release notes when a phase ships.

## Code

- `src/skate3_native_scene_gpu.cpp`:
  - the windows and overrides (`SdkCvarOverride`, `YieldForMenus`,
    `UpdatePhotoGrabWindow`, `UpdateSkaterPhotoWindow`);
  - `WriteGrabToGuest`, `emit_draw`, `resolve_2d_texture`, `RenderScene`.
- `src/skate3_native_scene.cpp`: `CaptureSkinnedState`, the portrait-publish
  gate in `BuildFrameScene`, the cvars.
- SDK:
  - `src/graphics/native_guest_renderer.cpp`: suppress modes,
    `ShouldSuppressPassAtPitch`;
  - `src/graphics/vulkan/command_processor.cpp`: the resolve gate, the census.
- Tools: `tools/harness/census_report.py`, `shots.py`, `gen_cas_portrait.py`;
  the scenarios `cas_portrait`, `census_menus*` and `cas_perf`.
