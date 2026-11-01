# Known Issues

The working list of known bugs in the recomp, what's being done about each one,
and where the fix lives. Player-facing bugs also get a line under "Known
issues" in the [README](README.md#native-renderer). That line comes out once
the fix ships.

## How to use this file

- Each issue gets the next free `KI-N` id. Ids are never reused.
- **Status** is one of `open`, `investigating`, `fix in review`, `fixed`
  (merged and confirmed in-game), `won't fix`, or `could not reproduce`.
- **Tracking** lists the PRs, GitHub issues and sessions working on it.
- Fixed issues move to [Resolved](#resolved) with the PR that fixed them. The
  repro and cause notes stay there for when a regression turns up.
- A PR that fixes an issue names its id (for example `Fixes KI-16`) and updates
  its entry in the same change.
- Name locations by file and function, not line numbers; line numbers drift.
- For rendering bugs, always say whether the emulated renderer (F5) shows the
  same problem. A bug that only shows on the native renderer is a native
  renderer bug.

Capture recipe for native renderer bugs: launch with
`--skate3_native_render_capture_hotkeys=true`, switch to the native renderer
with F6, toggle F5/F6 to compare with the emulated renderer, then press F10.
That dumps the draws and a full guest-memory `.gsnap` into
`<cwd>/native_render_snapshots`.

### Code review findings (KI-8 to KI-33)

KI-8 to KI-33 come from a full code review in October 2026. They were first
tracked as `BT-01` to `BT-26` in `docs/bug-tracker.md`, which this file
replaces; the KI id is the BT number plus 7 (BT-09 is KI-16). PR #19 and the
commits from #17 and #19 still use the BT ids. Most of these are code-level
risks found by reading the code, not bugs reported from play, so they have no
repro steps; the entry says where the problem is and how it would show.

**Severity** (code review entries only):
- **medium:** wrong output, stuck state or memory safety in normal play.
- **low–medium:** slow leaks, debug-only memory hazards, or features that
  silently stop working.
- **low:** cleanup, edge cases, cosmetic.

## Open

### KI-3: Park editor warehouses render wrong on the native renderer

- **Status:** open
- **Area:** native renderer, park editor
- **Tracking:** none yet
- **Symptom:** skate parks and park editing have visual parity problems on the
  native renderer. The clearest case is the park-editor warehouse interiors.
  This is the README's remaining "Skate parks and park editing" known issue.
- **Cause so far:** the warehouse interior environment shader never passes the
  world-row capture gate, so the native path never captures it as world
  geometry.
- **Next step:** take an F10 capture inside a park-editor warehouse to see what
  the interior draws look like and why the gate rejects them.

### KI-6: Tattoos don't show on the player character

- **Status:** open
- **Area:** emulation (both renderers), character customization
- **Tracking:** none yet
- **Symptom:** tattoos added in Edit Skater don't appear on the skater. This
  happens on the emulated renderer (F5) as well as the native one. Split out
  of KI-4.
- **Cause so far:** unknown. Since the emulated renderer is wrong too, the
  native port isn't missing anything here. The native renderer already follows
  the game's tattoo path (`cacstamp_skin` samples `i_decal` at tf6 through the
  `i_customGraphic` affine, c190/c191). A fix earlier in the pipeline should
  show on both renderers.
- **Next step:** take an F10 capture in Edit Skater with a tattoo applied.
  Check what the `cacstamp_skin` draw binds at tf6 (a blank or default
  texture, or the real art) and the values in c190/c191. A blank tf6 means the
  tattoo never reaches the texture. The texture-space composite pass may be
  suppressed or failing, its resolve may be lost, or the recomp may break a
  CPU-side step.

### KI-18: Draw-distance slots never freed; LOD distances scaled in place

- **Status:** open · **Severity:** low–medium · was BT-11
- **Area:** draw distance
- **Where:** `src/skate3_draw_distance.cpp`
- **Problem:** the 16 fixed cull-threshold slots are never freed, so
  draw-distance scaling silently stops once enough cull objects have been
  seen. LOD distances are multiplied in place every frame, which relies on the
  game rewriting them each frame.

### KI-20: Depth-view caches compare textures by pointer

- **Status:** open · **Severity:** low–medium · was BT-13
- **Area:** native renderer, post effects
- **Where:** `s_pfx_depth_tex` (photo FX in `RenderScene`), and
  `ao_depth_srv_of`, `ssr_depth_srv_of`, `vol_depth_srv_of`
  (`src/skate3_native_scene_post.cpp`)
- **Problem:** a rebuilt depth texture is detected by comparing pointers. A
  texture freed and reallocated at the same address keeps the old view,
  possibly with the wrong MSAA dimension. The photo FX one is most exposed
  because it can sit idle across several rebuilds (for example MSAA changes
  while the photo editor is closed).
- **Fix idea:** a generation counter bumped on every depth rebuild.

### KI-22: Shadow-dump readback buffers not resized (debug only)

- **Status:** open · **Severity:** low–medium · was BT-15
- **Area:** native renderer, diagnostics
- **Where:** `src/skate3_native_scene_gpu.cpp`
- **Problem:** the shadow-dump readback buffers keep their first size after a
  shadow tile change, so a later recording copies out of bounds on the GPU.

### KI-29: Icon updater leaves `.icon-new` behind

- **Status:** open · **Severity:** low · was BT-22
- **Where:** `src/skate3_win_icon.cpp`
- **Problem:** a failed swap leaves the `.icon-new` file in place.

### KI-37: Dead code for building without Title Update 3

- **Status:** open · **Severity:** low
- **Area:** build, codegen
- **Tracking:** none yet. Follow-up to KI-33 (#27).
- **Problem:** since #27, configure fails without TU3 codegen, so
  `SKATE3_HAS_TITLE_UPDATE_CODEGEN` and `SKATE3_HAS_TITLE_UPDATE` are always
  on. Everything behind the off branch is dead:
  - `CMakeLists.txt`: the default `SKATE3_FUNCTION_CONFIG_INCLUDES` pointing at
    `config/skate3_functions.toml`, the `if(SKATE3_HAS_TITLE_UPDATE_CODEGEN)`
    wrappers, and the `=0` branch of the compile definition.
  - `config/skate3_functions.toml`: the retail-only function boundaries.
    Nothing reads it any more.
  - `src/exception_compat.cpp`: the `#else` branch (the retail guard
    `sub_82F44E40` and its trampoline).
  - `src/skate3_app_common.cpp`: the `#if !SKATE3_HAS_TITLE_UPDATE`
    `Sub82EBAE4CImpl` and its `SetFunction(0x82EBAE4C)` registration, the
    non-TU `#else` of the macOS ISO-install chain, and the always-true guard
    around the title update installer check.
  - `src/skate3_guest_heap_diag.cpp`: the `#else` stub of
    `Skate3GuestHeapCategoryFreeBytes`.
  - `src/skate3_options_menu.cpp`: the always-true `SKATE3_HAS_TITLE_UPDATE`
    guards and any `#else` stubs paired with them.
  - The static_asserts #27 added to the hook blocks can go if the macro itself
    is removed. Keep them if it stays.
- **Fix:** delete the dead branches. Behaviour in TU3 builds doesn't change.
  Test with configure + build, then the unattended `--skate3_demo_path=true`
  boot.

### KI-36: Event-start 2D draws with a 32-byte vertex layout are dropped

- **Status:** open
- **Area:** HUD / UI, events (native renderer)
- **Tracking:** none yet. Found while fixing KI-34 (#25).
- **Symptom:** none seen in play yet. The logs show that at the start of an Own
  the Lot event some 2D draws are never drawn on the native renderer, so an
  intro or banner element may be missing.
- **Evidence:** temporary per-second 2D stream stats (not committed) logged
  draws with a 32-byte vertex stride as quad lists (prim 13) in the
  event-start window: 64 in one second and 83 the next. `g_draws_2d_other`
  rose from 0 to 147 at the same time. A free-roam run showed other bursts in
  `g_draws_2d_other` (+240, +176), but their stride wasn't logged.
- **Cause so far:** `Publish2dDraws` (`skate3_native_scene.cpp`) handles only
  16, 20, 24 and 28-byte vertex layouts. Any other stride is counted in
  `g_draws_2d_other` and dropped. The 32-byte layout isn't decoded yet; it
  could be pos4 + color + uv + one more field, or pos4 + uv + uv2.
- **Emulated (F5):** not checked.
- **Next step:** take an F10 capture right at event start. The `.draws.bin`
  records the 2D draws with their inline vertex data. Decode the 32-byte
  vertices, compare with F5, then add the layout to `Publish2dDraws`.

### KI-39: The native RHI has no way to wait on a GPU submission

- **Status:** open · **Severity:** low
- **Area:** SDK native RHI
- **Tracking:** none yet. Found while fixing KI-16 (#28).
- **Where:** rexglue-sdk `include/rex/graphics/native_rhi.h`. `nrhi::Device`
  has `CurrentSubmission()` and `CompletedSubmission()` but nothing that
  blocks.
- **Problem:** game code can't wait for the GPU. `CompletedSubmission()` is a
  cached value the command processor refreshes in `BeginSubmission`, so
  spinning on it on the render thread never advances. #28 therefore reuses
  the oldest region and logs, instead of waiting.
- **Next step:** add `Device::WaitForSubmission(uint64_t)`, backed by
  `CheckSubmissionFence` / `CheckSubmissionFenceAndDeviceLoss`. That needs an
  SDK-fork PR plus an "sdk: bump" commit. `UploadRingRegions::Acquire` can
  then wait, with a rate-limited log, instead of racing.
- **Lower priority (from KI-38, #49):** frames in flight never exceed 3 on
  either backend, even when GPU-bound. With #28's 8 regions, the fallback
  that would need this wait can't trigger in normal operation.

### KI-40: Photo FX constant slots have no bounds check

- **Status:** open · **Severity:** low
- **Area:** native renderer, photo FX
- **Tracking:** none yet. Found while fixing KI-16 (#28).
- **Where:** `src/skate3_native_scene_gpu.cpp`, `RenderScene`, the photo FX
  `fill_cb` lambda.
- **Problem:** `cb_slot_next++` is never checked against
  `RendererState::kPfxCbSlots` (10). Today at most 10 slots are used: depth
  pack, accum feed, 6 passes, accum mode 2 and the debug view. One more pass
  would silently write into the next frame's region, or past the end of the
  buffer for the last region.
- **Fix:** clamp it, or assert and log when `cb_slot_next` reaches
  `kPfxCbSlots`.

### KI-42: Perf log line repeats the texture count

- **Status:** open · **Severity:** low
- **Area:** native renderer, diagnostics
- **Tracking:** none yet. Found while fixing KI-17 (#29).
- **Where:** `LogFrameStats` in `src/skate3_native_scene_gpu.cpp`, the
  `native-scene: frame N ...` line.
- **Problem:** `g_r.tex_store.size()` is logged three times: as
  `2d[... textures=]`, as `textures=` and as `store[n=]`. The 2D
  `textures=` field looks like a leftover from a separate 2D cache that no
  longer exists, so it misleads.
- **Fix:** drop the 2D `textures=` field, or make it count something
  2D-specific.

### KI-43: A script/debug getter reads the scaled cull threshold

- **Status:** open (unconfirmed) · **Severity:** low
- **Area:** draw distance
- **Tracking:** none yet. Found while fixing KI-18 (#30).
- **Where:** `sub_825B5730` (`generated/skate3_recomp.29.cpp`). It has no
  direct callers and is reached only through a registered function pointer.
  It reads the global render context's cull object
  (`[[0x820CFD94]+292]+6064`) and returns
  `clamp(int((threshold - c0) * c1), 0, 10)` through `sub_82E86550`.
- **Problem:** it sees the draw-distance scaled value (0.2 becomes 0.05), so
  any script or menu that reads this level gets a shifted number. If a
  matching setter writes a level back through a path
  `src/skate3_draw_distance.cpp` doesn't hook, the scaled value becomes the
  new base and the scale compounds.
- **Next step:** find who registers or calls `sub_825B5730`, and any setter for
  that level, then watch it during a test.

### KI-44: Live draw-distance changes skip setter-only cull objects

- **Status:** open · **Severity:** low
- **Area:** draw distance
- **Tracking:** none yet. Found while fixing KI-18 (#30).
- **Where:** `src/skate3_draw_distance.cpp`.
- **Problem:** a hot change of `skate3_draw_distance_scale` reaches only the
  `view+8` cull objects, which are rescaled every frame. Objects seen only
  through the setter hook keep the old scale until the next config apply.
  Low impact: no setter calls showed up in a boot to gameplay.
- **Next step:** when the cvar value changes, rescale every load-bearing slot
  from the per-frame hook.

### KI-45: XMA context input offset exceeds its buffer

- **Status:** open (unconfirmed) · **Severity:** low
- **Area:** audio (SDK APU)
- **Tracking:** none yet. Seen in a normal boot log while testing KI-18 (#30).
- **Symptom:** the log repeats
  `[apu] XmaContext 31 old: input offset 16416 exceeds buffer size 16384`.
  Whether any sound is affected is unknown.
- **Next step:** find the check in the rexglue SDK's XMA decoder and which
  game sound uses context 31. Compare with upstream rexglue-sdk, then listen
  for a missing or clipped sound at the moment the line is logged.

### KI-47: Other startup filesystem calls can throw

- **Status:** open · **Severity:** low
- **Area:** app startup
- **Tracking:** none yet. Found while fixing KI-19 (#31). Code reading only,
  untested.
- **Where:**
  - `src/skate3_app_common.cpp`: `std::filesystem::exists` at the
    portable.txt check, the config/settings paths, `FirstExistingFontPath`,
    the JP font path, and the profiles/config checks in the defaults setup.
    Also `is_directory(defaults.game_data_root)`, and
    `is_directory(portable_saves)` in the post-launch setup.
  - `src/skate3_user_settings.cpp`: `exists(profiles_path)`.
  - `src/skate3_screenshot.cpp`, `ScreenshotPath`: `absolute(...)`.
- **Problem:** the same failure as KI-19. A permissions error on one of these
  paths becomes an uncaught exception.
- **Fix:** one sweep to the `error_code` overloads.

### KI-48: Guest-output view can go stale after an RHI mailbox eviction

- **Status:** open (unconfirmed) · **Severity:** low
- **Area:** native renderer, SDK native RHI
- **Tracking:** none yet. Found while fixing KI-20 (#32).
- **Where:** `EnsureOutputSrv` in `src/skate3_native_scene_gpu.cpp` (added in
  #32), and the SDK's `guest_outputs_` map in `native_rhi_d3d12.cpp` and
  `native_rhi_vulkan.cpp`.
- **Problem:** once the map holds more than 6 entries (after several resizes),
  the RHI deletes old texture wrappers and allocates new ones with no serial.
  A new wrapper at the same address and size, for example after resizing back
  to an earlier size, still matches the pointer-plus-size key, so the view
  keeps pointing at the old texture.
- **Evidence:** code reading only, not reproduced.
- **Next step:** give `nrhi::Texture` a creation serial in the SDK (SDK-fork PR
  plus an "sdk: bump" commit) and key the view on it.

### KI-49: Guest-output view may be rebuilt every frame

- **Status:** open (unconfirmed) · **Severity:** low
- **Area:** native renderer, performance
- **Tracking:** none yet. Found while fixing KI-20 (#32).
- **Where:** `EnsureOutputSrv` in `src/skate3_native_scene_gpu.cpp`.
- **Problem:** the SDK comments describe a "small guest-output mailbox" in the
  presenter. If it rotates images, `context.guest_output` changes every frame,
  so the view is destroyed and recreated every frame.
- **Next step:** log how many times per second the view is rebuilt. If it's
  every frame, keep one view per mailbox image.

### KI-50: Photo FX depth view isn't checked for null

- **Status:** open · **Severity:** low
- **Area:** native renderer, photo FX
- **Tracking:** none yet. Found while fixing KI-20 (#32).
- **Where:** `RenderScene` in `src/skate3_native_scene_gpu.cpp`.
- **Problem:** the result of `CreateTextureView` for `pfx_srv[7]` is passed to
  `pfx_bind_all(..., g_r.pfx_srv[7])` without a null check. A failed create
  binds null for that frame, and it retries the next frame.
- **Fix:** fall back to `g_r.white.srv`, or skip the depth pack when the view
  is null.

### KI-51: Occlusion grid can miss thin gaps at high render scales

- **Status:** open (unconfirmed) · **Severity:** low–medium
- **Area:** native renderer, occlusion culling
- **Tracking:** none yet. Found while fixing KI-21 (#33).
- **Where:** `ps_depth_max` in `src/native/shaders/ssao.hlsl`.
- **Problem:** the grid reduce takes only 8x8 point samples per tile. Its
  comment accepts that a gap under 3 px can be missed at 4K because "the
  consumer is telemetry, not culling". That's no longer true: the occlusion
  cull reads this grid, so a missed gap can hide a distant object that is
  visible through railings or fence slats. The 1 m depth margin doesn't help
  with gaps. At `resolution_scale = 3` (the user's setting), tiles reach
  about 24 px and the samples are about 3 px apart.
- **Next step:** sample every texel of the tile's footprint, or reduce through
  a two-level chain, and update the comment. Regenerate the SPIR-V. Test by
  looking through fences and railings at render scale 3x, with the occlusion
  cull on and then off.

### KI-52: Demo-path map-switch inputs may not work

- **Status:** open (unconfirmed) · **Severity:** low
- **Area:** test tooling
- **Tracking:** none yet. Found while testing KI-21 (#33).
- **Where:** the input sequence in the help text of
  `skate3_demo_path_gameplay_inputs` (`src/skate3_demo_path.cpp`).
- **Problem:** in a signed-out demo boot all 14 inputs were injected, but
  nothing loaded and the item counts stayed the same, so the documented
  map-switch sequence seems to do nothing. Unattended tests that rely on it
  would silently stay in one area.
- **Likely cause (from KI-28, #40):** in the current build, LB, LT and RT don't
  change pause-menu tabs; d-pad left/right does. The help text's map-switch
  example uses `rt` for tabs.
- **Seen again (KI-41, #51):** with the documented string
  `start,a,rt:1500,rt:1500,down,down,a,down,down,down,down,down,a,a`, the skater
  stayed in the plaza, standing off the board, and no map change happened.
- **Next step:** update the example to use d-pad left/right, check that it
  reaches a map change (signed in, if that's needed), and fix the help text.

### KI-53: Render targets used before their first clear (D3D12 debug layer)

- **Status:** open · **Severity:** low
- **Area:** native renderer, shadows and startup
- **Tracking:** none yet. Found while fixing KI-22 (#34).
- **Where:** `EnsureShadowResources` / `RenderShadowAtlas` in
  `src/skate3_native_scene_gpu.cpp`, plus unidentified resources at boot.
- **Problem:** the D3D12 debug layer reports
  `RENDER_TARGET_OR_DEPTH_STENCIL_RESOUCE_NOT_INITIALIZED` (#1422) twice after
  every shadow atlas recreation. That fits `shadow_mid` and `shadow_final`:
  the blur pass draws into them, but only `shadow_raw` is ever cleared. It's
  probably harmless, since the blur writes every texel of each tile. The same
  error also appears 4 times at boot and 17 times when gameplay starts, from
  resources not yet identified.
- **Next step:** clear or discard `shadow_mid` and `shadow_final` once after
  creation. Then run with the debug layer and find the boot-time ones (the
  KI-22 agent's `dbwin.ps1` captures debug-layer output without a debugger).

### KI-54: Shadow-dump readback buffers are never freed (debug only)

- **Status:** open · **Severity:** low
- **Area:** native renderer, diagnostics
- **Tracking:** none yet. Found while fixing KI-22 (#34).
- **Where:** `RenderShadowAtlas` in `src/skate3_native_scene_gpu.cpp`.
- **Problems:**
  - After one shadow recording, the four readback buffers stay allocated for
    the rest of the session. At tile 4096 that is about 400 MB each, about
    1.6 GB in all. The only release is the size-mismatch path #34 added.
  - Dump file names use a seconds timestamp plus the frame index, so two
    recordings in the same second overwrite each other.
  - A recording usually yields 3 dumps, not `kShadowDumpFrames` = 4, because
    the recording window is shorter than 4 shadow frames.
- **Next step:** release the buffers once a session's dumps are written, and
  add a counter to the file name.

### KI-55: App callbacks that capture `this` are never cleared on shutdown

- **Status:** open · **Severity:** low
- **Area:** app lifetime
- **Tracking:** none yet. Found while fixing KI-23 (#35).
- **Where:** `OnPostSetup` / `OnShutdown` in `src/skate3_app_common.cpp`.
- **Problem:** six callbacks capture `this` and are never cleared:
  `input_system->SetActiveCallback`, `SetMenuChordCallback`,
  `demo_path::SetUiInputProvider` and the three `options_menu::Set*Callback`.
  The active callback also reads `simple_settings_dialog_` on the input path
  while `OnShutdown` resets it on the UI thread. That's a data race or
  use-after-free on any path where `OnShutdown` runs while the guest is
  still alive. Today that is mainly Close Game on macOS (`QuitFromUIThread`,
  which ends the guest after `OnShutdown`).
- **Fix:** clear these callbacks (null or no-op) at the top of `OnShutdown`.

### KI-56: SDK key binds: duplicate registration and callbacks under the lock

- **Status:** open · **Severity:** low
- **Area:** SDK input
- **Tracking:** none yet. Found while fixing KI-23 (#35).
- **Where:** `RegisterBind` / `UnregisterBind` / `ProcessKeyEvent` in
  rexglue-sdk `src/ui/keybinds.cpp`.
- **Problem:** re-registering a name appends a duplicate entry and registers
  the cvar again, while `UnregisterBind` only nulls the first match. Bind
  callbacks also run under the non-recursive `g_binds_mutex`, so a bind that
  registers or unregisters a bind would deadlock. Neither happens today,
  because `RestartGame` starts a new process instead of re-registering.
- **Next step:** make `RegisterBind` replace by name, and call callbacks
  outside the lock. That needs an SDK-fork PR plus an "sdk: bump" commit.

### KI-57: SDK `fullscreen` change callback is never unregistered

- **Status:** open · **Severity:** low
- **Area:** SDK app lifetime
- **Tracking:** none yet. Found while fixing KI-23 (#35).
- **Where:** rexglue-sdk `src/ui/rex_app.cpp`. `SetupPresentation` registers a
  `fullscreen` change callback that captures `this`, and `OnDestroy` only
  unregisters `show_fps_counter`.
- **Problem:** a `fullscreen` change after the app object is destroyed would
  call into it. Unlikely, since the process exits right after.
- **Fix:** add `UnregisterChangeCallbacks("fullscreen")` to `OnDestroy`.

### KI-58: SDK `ParseXuid` misreads decimal XUIDs

- **Status:** open · **Severity:** low
- **Area:** SDK user profile
- **Tracking:** none yet. Found while fixing KI-24 (#36).
- **Where:** `ParseXuid` in rexglue-sdk `src/system/xam/user_profile.cpp`, used
  by `UserProfile::UserProfile`.
- **Problem:** it's a copy of the game's old parser and still tries hex
  first, so a decimal `--user_profile_xuid=12345` passed by hand becomes XUID
  0x12345. The game itself always passes hex from `FormatXuid`, so normal
  play is unaffected.
- **Fix:** apply #36's rules in the SDK fork (a `0x` prefix or 16 hex digits
  is hex; other all-digit strings are decimal), or have the SDK call one
  shared parser.

### KI-60: Gamertag truncation can produce invalid UTF-8

- **Status:** open · **Severity:** low
- **Area:** SDK settings overlay, profiles
- **Tracking:** none yet. Found while fixing KI-24 (#36).
- **Where:** `CopyToBuffer` in rexglue-sdk
  `src/ui/overlay/simple_settings_overlay.cpp`.
- **Problem:** the gamertag is copied into `gamertag_buf_[32]` byte by byte,
  which can cut a multi-byte UTF-8 character in half. `SaveProfile` then
  writes invalid UTF-8, toml++ rejects the file, and KI-59 wipes the
  profiles. It needs a gamertag over 31 bytes with a non-ASCII character at
  the cut, so in practice only hand-edited names hit it.
- **Fix:** truncate at a UTF-8 character boundary.

### KI-62: Vehicle retention box may not track the car's real position

- **Status:** open (unconfirmed) · **Severity:** low
- **Area:** native renderer, vehicles
- **Tracking:** none yet. Found while fixing KI-25 (#37).
- **Where:** vehicle retention in `BuildFrameScene` (`src/skate3_native_scene.cpp`),
  the `bone_center` / `DynRetained::half` box.
- **Evidence:** in two unattended runs, 7,052 (D3D12) and 16,649 (Vulkan) live,
  non-caster vehicle captures had a box entirely outside even the wide guest
  frustum. Either the game submits cars for other perspective passes (such as
  reflections), or the averaged bone position plus the bind-pose bbox diagonal
  isn't a reliable proxy. If it's the proxy, every retention decision for
  vehicles is less reliable than it looks.
- **Next step:** log which plane rejects these boxes and their `dbg_src`.

### KI-63: Traffic may despawn by 16:9 visibility on ultrawide

- **Status:** open (unconfirmed guess) · **Severity:** low
- **Area:** ultrawide, game logic
- **Tracking:** none yet. Raised while fixing KI-25 (#37).
- **Problem:** only the cull block's left and right planes are widened for
  ultrawide. If the game's traffic despawn or "out of view" logic uses its own
  16:9 visibility, cars could despawn while visible in the side bands.
  Retention can't hide that, since it covers only 10 frames.
- **Next step:** only if edge popping is still seen after #37: find the
  traffic despawn visibility check in the recompiled code and see which
  frustum it uses.

### KI-66: Prewarm workers re-queue texture and cube misses as meshes at startup

- **Status:** open (unconfirmed) · **Severity:** low
- **Area:** native renderer, streaming
- **Tracking:** none yet. Found while fixing KI-26 (#38).
- **Where:** `PrewarmWorkerLoop` in `src/skate3_native_scene_gpu.cpp`.
- **Problem:** when `base` or `g_r.device` is still null at startup, the
  worker re-queues the whole entry with `g_prewarm_retry.push_back(e)`.
  `PrewarmCommit` moves retries into `g_prewarm_queue` without keeping track
  of what kind of entry each was, so a texture-only or cube entry is later
  decoded as a mesh and that miss is lost. Startup only, low impact.
- **Next step:** confirm by reading the code, then send misses back to
  `g_miss_queue` with their kind kept.

### KI-67: Mesh decodes that always fail are retried every frame

- **Status:** open · **Severity:** low
- **Area:** native renderer, streaming
- **Tracking:** none yet. Found while fixing KI-27 (#39).
- **Where:** `ProcessPrewarmEntry` → `PrewarmCommit` → the draw path in
  `src/skate3_native_scene_gpu.cpp`.
- **Problem:** a failed mesh decode is committed with `mesh_valid=false` and
  its in-flight key is released. The next draw-path miss calls
  `EnqueueMeshMiss` again, with no negative cache or backoff (textures have
  `RetryBackoff`, meshes don't). So a mesh that can never decode costs a
  pointless worker decode every frame it's visible. #39's new rejections are
  cheap and run before any copy, but other failures still do the full work.
- **Fix:** a per-mesh fail count with backoff, like `GuestTexture::fail_count`.

### KI-68: Prewarm log counts are misleading

- **Status:** open · **Severity:** low
- **Area:** native renderer, diagnostics
- **Tracking:** none yet. Found while fixing KI-27 (#39).
- **Where:** `PrewarmCommit` and the settle warm loop (the `need_mesh` branch
  near the `REVALIDATE` debug log) in `src/skate3_native_scene_gpu.cpp`.
- **Problems:**
  - `g_prewarm_done` counts every result, including `mesh_valid=false` and
    texture-only results, so "N meshes decoded" isn't the number of
    successful decodes. Baseline comparisons between agent runs used it.
  - The settle loop's comment says failures are "logged + counted" through the
    draw path, but the failure itself isn't counted there; only the later
    draw-path retry is (`g_rr_decode_fail`).
- **Fix:** count successes separately (or rename the log field), and fix the
  comment.

### KI-69: Staged restart-required choices can linger in the Options menu

- **Status:** open · **Severity:** low
- **Area:** Options menu
- **Tracking:** none yet. Found while fixing KI-28 (#40).
- **Where:** `Row::pending` in `src/skate3_options_menu.cpp`, cleared only by
  the restart prompt's buttons (`ApplyPendingAndRestart` / `DiscardPending`).
- **Problem:** leaving a page without Back leaves the staged choices in place.
  That happens when the pause menu is closed from a page, on a popup flush, or
  when `ShowRestartPrompt` fails because the manager pointer is null and falls
  through to the game's Back. The next visit shows them again, and Back
  prompts again.
- **Next step:** decide whether leaving the Options state should discard them;
  that means hooking the Options state's exit, which isn't identified yet. A
  small debug cvar that triggers a popup flush (`sub_825D8AF0` →
  `sub_82D0A738`) would let the flush path be tested unattended.

### KI-70: Restart prompt's Discard doesn't check it's acting on the Options state

- **Status:** open · **Severity:** low
- **Area:** Options menu
- **Tracking:** none yet. Found while fixing KI-28 (#40). Code reading only.
- **Where:** the Discard branch of the `sub_8260EA48` hook in
  `src/skate3_options_menu.cpp`.
- **Problem:** it uses `OptionsState()` (`owner+180`) without checking that the
  object is a FrontEndState_Options. If the Options state is gone when Discard
  fires, `PageOf` reads +68 of whatever object is there and could call
  `sub_8260D078` on it. Unlikely, because the popup is modal over Options.
- **Fix:** check the state's vtable against 0x823034B0 before acting.

### KI-71: Restart prompt shows no title

- **Status:** open · **Severity:** low
- **Area:** Options menu, cosmetic
- **Tracking:** none yet. Found while fixing KI-28 (#40).
- **Where:** `ShowRestartPrompt` in `src/skate3_options_menu.cpp`.
- **Problem:** it writes the "Restart Required" title ID, but the in-game
  popup shows only the message and the buttons.
- **Next step:** check whether the popup layout uses the +0 field at all, or
  needs another flag to show a title.

### KI-72: The Defender trojan trigger still ships on main

- **Status:** open · **Severity:** medium (blocks the next release)
- **Area:** Windows release, exe icon
- **Tracking:** rubensteezy/skate3recomp#18 (open). Raised again while fixing
  KI-29 (#41).
- **Where:** `EnsureExeIconResource` and `SpawnPreIconDeleter` in
  `src/skate3_win_icon.cpp`.
- **Problem:** #18 removes the self-patching exe icon that makes Defender flag
  the Windows zip as a trojan, but it isn't merged. So `main` still patches
  every fresh `skate3.exe` on first run, parks the old exe in `%TEMP%`, and
  starts a hidden `powershell.exe -WindowStyle Hidden` deleter. Any release cut
  from `main` keeps the trigger.
- **Also:**
  - #18 removes the `%TEMP%\skate3-pre-icon-*.old` sweep and has no
    `.icon-new` cleanup. Once it merges, the hidden 100 MB parked exes and
    crash leftovers already on v2.1.0 users' machines are never removed.
  - #41 (KI-29) hardens the same self-patch code, so it conflicts with #18.
- **Next step:** decide on #18 and merge it before the next release. Keep a
  one-time, delete-only sweep of the old leftovers in it: either rebase #41
  down to just its `CleanupStaleIconTemp`, or close #41.

### KI-73: Exe path lookup truncates on long install paths

- **Status:** open · **Severity:** low
- **Area:** Windows, exe icon
- **Tracking:** none yet. Found while fixing KI-29 (#41).
- **Where:** `EnsureExeIconResource` and the icon thread in
  `src/skate3_win_icon.cpp`.
- **Problem:** they call `GetModuleFileNameW` with a `MAX_PATH` buffer. For an
  install path over 260 characters it truncates but still returns non-zero,
  so the code works on a wrong path. Low risk, since the moves then just
  fail. Moot if #18 removes this code.
- **Fix:** use a growing buffer and check for `ERROR_INSUFFICIENT_BUFFER`.

### KI-74: SDK cvar overrides drop user changes made while held

- **Status:** open · **Severity:** low
- **Area:** native renderer, settings
- **Tracking:** none yet. Found while fixing KI-30 (#42).
- **Where:** `SdkCvarOverride` in `src/skate3_native_scene_gpu.cpp`.
- **Problem:** while an override holds a cvar, `Release` restores `saved_`, so
  any write another thread makes in the meantime is dropped. That includes
  the options menu, the settings overlay and the console. There's also a
  microsecond window where the F12 dialog sees "not held", the user clicks,
  and the render thread acquires before the write lands. #42 adds a warning,
  "SDK cvar override on ... was overwritten while held", that shows when this
  happens.
- **Next step:** if that warning ever appears, add a "user set while held" path
  that updates `saved_`.

### KI-75: Scene ring dumps taken in the same second overwrite each other

- **Status:** open · **Severity:** low
- **Area:** native renderer, diagnostics
- **Tracking:** none yet. Found while fixing KI-32 (#44).
- **Where:** `MaybeDumpSceneRing` in `src/skate3_native_scene_state.h`.
  `SnapshotStem` in `src/native/skate3_native_diagnostics.cpp` uses the same
  seconds-only naming.
- **Problem:** the dump file is `logs/scene_ring_<time in seconds>.csv`, so an
  F7 dump and an RB+A dump in the same second overwrite each other. That's the
  same bug #44 fixes for screenshots. A snapshot takes several frames, so a
  clash there is unlikely.
- **Also:** the comment in `skate3_screenshot.h` says A/B captures "share one
  timestamp". They actually share a tag with its own HH:MM:SS, and the file
  stamps differ.
- **Fix:** reuse #44's millisecond stamp, and reword the comment.

### KI-76: Scene ring may use about 150 MB while on (debug only)

- **Status:** open (estimate, not measured) · **Severity:** low
- **Area:** native renderer, diagnostics
- **Tracking:** none yet. Found while fixing KI-32 (#44).
- **Where:** `kSceneRingFrames` (2400) and `SceneRingItem` (about 80 bytes).
- **Problem:** at about 800 items a frame, 2400 frames is roughly 150 MB of RAM
  while `skate3_native_render_scene_ring` is on, and a dump would be a few
  hundred MB of CSV. It's off by default and debug-only.
- **Next step:** measure it, and put the real size in the cvar help text.

### KI-77: Photo grab assumes 4-byte output pixels

- **Status:** open · **Severity:** low
- **Area:** native renderer, photo grab
- **Tracking:** none yet. Found while fixing KI-32 (#44).
- **Where:** `RendererState::kGrabRowPitch = kBlurWidth * 4`; the grab reads
  `blur_tex[0]`, which uses the output format.
- **Problem:** that's correct for today's fixed R10G10B10A2 output. If the output
  format ever changed (the case #44 now guards for the blur targets), the
  pitch and the CPU tiling would be wrong. The output format is a compile-time
  constant in the SDK (`kGuestOutputFormat`), so nothing at runtime changes it
  today.
- **Fix:** add a static check or a comment next to #44's format key.

### KI-78: SDK settings.toml handling has the same reset and flush gaps as KI-59

- **Status:** open (code reading only) · **Severity:** low–medium
- **Area:** SDK settings
- **Tracking:** none yet. Found while fixing KI-59 (#45).
- **Where:** `SaveConfigValues` / `WriteConfigFile` in rexglue-sdk
  `src/core/cvar.cpp`, and `EnsureSimpleSettingsConfig` in
  `src/ui/overlay/simple_settings_overlay.cpp`.
- **Problem:** `toml::parse_file` also throws `parse_error` when it can't open
  the file. So a locked or unreadable `settings.toml` is treated as malformed
  and rewritten with only the given cvars, which drops every other key. A
  malformed file is rewritten with no backup. `WriteConfigFile` uses
  temp-and-rename but never flushes the temp file to disk.
- **Next step:** in the SDK fork, read the bytes separately before parsing,
  back up a malformed file the way #45 does, and flush before the rename.
  That needs an SDK-fork PR plus an "sdk: bump" commit.

### KI-79: With profiles.toml unreadable, the career save goes to a temporary profile

- **Status:** open · **Severity:** low
- **Area:** profiles, saves
- **Tracking:** none yet. Found while fixing KI-59 (#45).
- **Where:** `LoadProfiles` and `OnFinalizePaths` in `src/skate3_user_settings.cpp`
  and `src/skate3_app_common.cpp`.
- **Problem:** after #45, when the file can't be read (it's locked, or the
  backup couldn't be verified), the store is `save_blocked`. The game then
  runs signed in as a temporary default "Player", and the career save goes to
  that profile's XUID folder. In the locked test, a `B13E07DFF9AB6772` save
  folder was created; the real file and saves were untouched.
- **Also:** the startup default-profile write only happens when
  `<exe dir>\skate3.toml` exists and `game_data_root` is set, so a dev build
  run with neither never writes a default `profiles.toml`. That's probably
  intended, but it isn't documented.
- **Next step:** when `save_blocked` is set, show a dialog or boot signed out.
  Confirm and document the default-profile write condition.

### KI-81: Non-ASCII paths are mis-decoded, and `path.string()` may throw on them

- **Status:** open (mis-decoding confirmed; throwing unverified) ·
  **Severity:** low–medium
- **Area:** SDK app startup, settings, app logging
- **Tracking:** none yet. Found while fixing KI-59 (#45); confirmed while
  fixing KI-46 (#46).
- **Problems:**
  - **Mis-decoding (confirmed):** the SDK's path setup in `src/ui/rex_app.cpp`
    assigns a UTF-8 `std::string` straight to `std::filesystem::path`
    (`user_dir = user_data_cvar;`). Windows reads that as the ANSI code page.
    The same pattern is used for `game_data_root`, `cache_root`,
    `update_data_root` and the settings.toml path values. With
    `--user_data_root=...\na_Jiří_日本` the game booted, but all user data
    (cache, dlc, settings.toml) went into a mojibake folder
    `na_JiÅ™Ã­_æ—¥æœ¬`, and the intended folder stayed empty. The log still
    shows the right name.
  - **Throwing (unverified):** `std::filesystem::path::string()` converts to the
    ANSI code page and can throw for characters outside it. The SDK passes it
    to `toml::parse_file` in `ReXApp::SetupEnvironment`,
    `ConfigFileContainsAnyKey` and `SaveConfigValues`. The game's own `src/`
    has 52 more `.string()` calls across 9 files, many in log lines on user
    paths: the DLC scan, "Skate 3 saves: using portable folder", and
    `content_root.string()` in `InstallRecipeOverlay`. These are reachable
    through the default AppData path (a non-ASCII Windows username) or a
    portable install folder, so they could crash startup for those users.
- **Next step:** in the SDK, build these paths with `rex::to_path(utf8)` and
  replace `.string()` with UTF-8 conversion (SDK-fork PR plus an "sdk: bump"
  commit). In the game, sweep `src/` to `rex::path_to_utf8`. Then test with a
  portable install in a non-ASCII folder.

### KI-82: Persistent shader storage is disabled on long user-data paths

- **Status:** open · **Severity:** low
- **Area:** SDK D3D12 pipeline cache
- **Tracking:** none yet. Found while fixing KI-46 (#46).
- **Where:** `InitializeShaderStorage` in rexglue-sdk
  `src/graphics/d3d12/pipeline_cache.cpp`.
- **Problem:** with a 218-character user-data root, it logged "Failed to open
  the Direct3D 12 pipeline description storage file for writing, persistent
  shader storage will be disabled". The storage file path was about
  290 characters. The game runs, but shaders recompile on every boot. Vulkan
  wasn't checked.
- **Next step:** use `\\?\` paths for the cache root in the SDK, the way #46
  does for the overlay. Adding `longPathAware` to `src/skate3_app.manifest`
  only helps where the LongPathsEnabled policy is set.

### KI-83: Saves may fail on long user-data paths

- **Status:** open (code reading only) · **Severity:** low–medium
- **Area:** SDK content, saves
- **Tracking:** none yet. Found while fixing KI-46 (#46).
- **Where:** `ContentManager::ResolvePackagePath` builds
  `<root>\<xuid 16>\<title 8>\<type 8>\<name>\...` with plain paths, and
  `HostPathEntry::CreateEntryInternal` and the file opens use no `\\?\`
  prefix.
- **Problem:** a long AppData or portable path would hit MAX_PATH at a slightly
  shorter root than the overlay did, so career saves could fail to write.
- **Next step:** give content roots the same `\\?\` treatment in the SDK, then
  run a save test on a long root.

### KI-84: MAX_PATH buffers truncate long exe and file-dialog paths

- **Status:** open (code reading only) · **Severity:** low
- **Area:** app, installers
- **Tracking:** none yet. Found while fixing KI-46 (#46). See also KI-73.
- **Where:**
  - `Skate3BaseApp::RestartGame` (`src/skate3_app_common.cpp`):
    `GetModuleFileNameW` with a MAX_PATH buffer. On truncation it doesn't
    return 0, so `CreateProcessW` gets a cut path, the restart fails, and the
    log is misleading.
  - `PickIsoFile` (`src/skate3_iso_installer.cpp`) and `PickTitleUpdateFile`
    (`src/skate3_title_update_installer.cpp`): MAX_PATH file-dialog buffers,
    so picking a file deeper than 260 characters fails.
- **Fix:** use the SDK's `rex::filesystem::GetExecutablePath()`, which already
  handles long paths, and bigger dialog buffers.

### KI-85: Dropping a speculative prewarm entry clears a live miss's in-flight key

- **Status:** open · **Severity:** low
- **Area:** native renderer, streaming
- **Tracking:** none yet. Found while fixing KI-65 (#47).
- **Where:** the `if (dropped)` branch of `ProcessPrewarmEntry` in
  `src/skate3_native_scene_gpu.cpp`.
- **Problem:** it calls `g_miss_inflight_mesh.erase(e.mesh)` without checking
  `e.miss`. When a speculative entry for mesh X is dropped while a miss for X
  is still in flight, the miss's key is cleared, and the draw path can queue
  a second decode of X. That wastes worker time but has no visual effect.
- **Fix:** erase only when `e.miss` is set.

### KI-86: The prewarm miss queue pops from the front of a vector

- **Status:** open · **Severity:** low
- **Area:** native renderer, streaming performance
- **Tracking:** none yet. Found while fixing KI-65 (#47).
- **Where:** `PrewarmWorkerLoop` in `src/skate3_native_scene_gpu.cpp`, and
  `skate3_native_scene_state.h`.
- **Problem:** `g_miss_queue.erase(g_miss_queue.begin())` costs time in
  proportion to the queue length (capped at 65536) while `g_prewarm_mutex` is
  held, so a large streaming burst could stall the workers. `g_dyn_jobs` has
  the same pattern.
- **Also:** the null base/device re-queue in `PrewarmWorkerLoop` is close to
  dead code (the workers start only once `g_r.device` is set, and nothing
  resets it). It never decrements retries, so if it ever ran for long it
  would spin entries every 100 ms per worker without end.
- **Next step:** switch both queues to `std::deque` if a profile shows lock
  contention. Turn the null re-queue into a plain wait.

### KI-87: Raw guest loads in the `CaptureDynamicState` pre-check

- **Status:** open · **Severity:** low–medium
- **Area:** native renderer, guest memory safety
- **Tracking:** none yet. Found while fixing KI-64 (#48). The biggest remaining
  risk of the KI-64 kind.
- **Where:** the world-path pre-check of `CaptureDynamicState` in
  `src/skate3_native_scene.cpp`, called about 800 times a frame from
  `OnSceneDrawList`. It reads `ctx → record → mesh → vdesc` and the vertex
  elements raw (`REX_LOAD_U32(ctx)`, `REX_LOAD_U32(record)`,
  `REX_LOAD_U16(vdesc + 8)` and others). Further down, `ctx + kCtxEffectList`
  and `effect_list + kEffectListPassCount` are raw too.
- **Problem:** a stale pointer there crashes the guest render thread, the same
  way as KI-64.
- **Fix:** reuse one ctx-head guarded copy, as #48's `BuildItemGeometry` does,
  and measure the cost the same way #48 did.

### KI-88: Remaining raw guest loads elsewhere in capture (low risk)

- **Status:** open · **Severity:** low
- **Area:** native renderer, guest memory safety
- **Tracking:** none yet. Found while fixing KI-64 (#48).
- **Where** (`src/skate3_native_scene.cpp` unless noted):
  - `CaptureClothDraw`: `vb_obj + 0x18/0x20`.
  - The neighbour read in `ComputeItemFingerprint`:
    `mix(REX_LOAD_U64(addr + off))`.
  - `OnSetViewport` and `OnSetScissor`.
  - The `REX_LOAD_U8(obj + 0x54 + k)` shader-name loops.
  - The `device + 0x480` fetch-constant reads.
- **Problem:** the same fault risk as KI-64, but most of these read device or
  current-draw state, so the risk is low.
- **Also:**
  - `GuestReadableApprox` only checks `addr >= 0x10000` (its `base` argument
    is unused), but its name suggests a real readability check.
  - `OnSceneDrawList` and `FilterSceneDrawList` (`src/skate3_native_render.cpp`)
    both copy the same sort-list segment per dispatch. The cost is
    negligible.
- **Next step:** no code change until a crash points at one of these. Rename or
  document `GuestReadableApprox`. Optionally, pass the captured segment to
  the filter.

### KI-89: Raw writes through just-read pointers in the auto-exposure hook

- **Status:** open · **Severity:** low
- **Area:** native renderer, guest memory safety
- **Tracking:** none yet. Found while fixing KI-64 (#48).
- **Where:** `sub_827F0D00` in `src/skate3_native_render.cpp`:
  `REX_STORE_U32(inst_a + 48 / inst_b + 56)`.
- **Problem:** these write through pointers read a moment earlier. A stale
  instance pointer would be a write fault, or would corrupt guest memory if
  the page has been reused.
- **Next step:** low priority. Check `inst_a` / `inst_b` against a guarded read
  of their vtable or type before the stores.

### KI-90: The bone-ring comment blames the wrong cause for hair shimmer

- **Status:** open · **Severity:** low
- **Area:** native renderer, characters
- **Tracking:** none yet. Found while checking KI-38 (#49).
- **Where:** the `RendererState::kBoneRegions` comment in
  `src/skate3_native_scene_gpu_internal.h`.
- **Problem:** it blames hair shimmer and collapsed garments on the Vulkan GPU
  running more than 3 frames behind, which is why the ring is 8 deep. #49
  measured that frames never go past 3 on either backend. Submissions do:
  Vulkan reached 6 in flight while loading, because there are several per
  frame. A ring keyed by frame number with 4 or more regions is never reused
  while the GPU still reads it, so that corruption had another cause.
- **Also:** #28's "K/N regions in flight" and "all N regions in flight" upload
  ring lines can't fire in normal operation for the same reason. Consider
  lowering their threshold to "more than 3".
- **Next step:** if the shimmer comes back, look for a `RenderScene` that runs
  more than once per `g_frames_rendered` value, or at the ROPA generation
  ring. Then fix the comment.

### KI-91: Frame rate is uncapped by default

- **Status:** open (decision) · **Severity:** low
- **Area:** settings defaults
- **Tracking:** none yet. Noticed while checking KI-38 (#49).
- **Problem:** the SDK `vsync` cvar defaults to false, and at default settings
  both backends ran at about 4.4 ms per frame (about 225 fps). That may cost
  power and heat for no benefit, especially on laptops.
- **Next step:** decide whether release defaults should turn on vsync or a
  frame cap. Settings > Display already has Framerate Cap and V-Sync rows.

### KI-92: `ScoreRigidAffine` has no in-front-of-camera check

- **Status:** open · **Severity:** low
- **Area:** native renderer, player garments
- **Tracking:** none yet. Found while fixing KI-61 (#50).
- **Where:** `ScoreRigidAffine` in `src/skate3_native_scene.cpp`.
- **Problem:** its strict count accepts a sample when `|x|` and `|y|` are within
  1.5·|w|, without requiring `clip[3] > 0`. So a matrix that places the
  garment behind the camera can still score well. `RefinePaletteBase` has
  that check, and its comment explains it exists for exactly this case. The
  structural tail-row check on the matrix block reduces the risk.
- **Fix:** add `clip[3] > 0.0f` to the strict count, then re-test the player's
  tee (Ropa, sim active) up close.

### KI-93: Character shadows may be cut off at the edges on 32:9 ultrawide

- **Status:** open (unverified) · **Severity:** low–medium
- **Area:** ultrawide, shadows
- **Tracking:** none yet. Raised while fixing KI-61 (#50).
- **Problem:** at 32:9, the game's sun-shadow bank puts characters in the side
  bands at |x|/w up to about 2.0; at 16:9 that never passes 1.5. So the
  game's shadow projection seems to be fitted to the 16:9 view only, and
  shadows of characters or cars near the far edges may be missing or cut
  off.
- **Next step:** at 32:9, compare character and car shadows at the far edges
  with the centre. If they're missing, check which projection the native
  shadow atlas uses, and whether it can be widened the way the cull frustum
  is.

### KI-94: Demo-path inputs can't move the camera

- **Status:** open · **Severity:** low
- **Area:** test tooling
- **Tracking:** none yet. Found while testing KI-61 (#50).
- **Where:** `skate3_demo_path_gameplay_inputs` in `src/skate3_demo_path.cpp`.
- **Problem:** it only takes button tokens, no sticks, so unattended runs
  can't pan the camera or put characters at the screen edges on purpose.
  That limited the testing of KI-25 (#37) and KI-61 (#50).
- **Fix:** add stick tokens, for example `rs_left:ms` and `ls_up:ms`.

### KI-95: Shadow-atlas casters don't stamp their mesh entries

- **Status:** open · **Severity:** low–medium
- **Area:** native renderer, shadows, mesh cache
- **Tracking:** none yet. Found while fixing KI-41 (#51). The same kind of bug
  as KI-41, for meshes.
- **Where:** the caster loop of `RenderShadowAtlas` in
  `src/skate3_native_scene_gpu.cpp`. It checks `g_r.meshes.contains(item.mesh)`
  and later does `g_r.meshes.find(...)`, but never sets the mesh's
  `last_used_frame`. `RenderStaticSunMap` does
  (`mit->second.last_used_frame = frame_number`).
- **Problem:** under mesh-store pressure
  (`skate3_native_render_scene_mesh_store_mb`), the mesh of a caster the main
  pass doesn't draw can be evicted. The caster then silently drops out of the
  shadow, because the pass `continue`s on a missing mesh.
- **Evidence:** a temporary counter found 5,995 to 25,377 caster-frames per
  150 s run whose mesh hadn't been stamped for more than 5 s. No mesh was
  actually skipped, because the 1024 MB mesh budget was never under pressure.
- **Fix:** stamp `mit->second.last_used_frame` in the atlas loop, then test with
  a small mesh budget.

### KI-96: The texture store budget can't go below 256 MB for testing

- **Status:** open · **Severity:** low
- **Area:** test tooling
- **Tracking:** none yet. Found while fixing KI-41 (#51).
- **Where:** the `.range(256, 16384)` on `skate3_native_render_scene_tex_store_mb`
  in `src/skate3_native_scene.cpp`, and `std::max(256, …)` in `EvictTexStore`.
- **Problem:** the floor is enforced twice. A light area's working set is
  about 225 MB, so at 256 MB the LRU never starts, and reproducing store
  pressure there needs a code change.
- **Next step:** if testing at low budgets matters, lower the floor for dev
  builds or add a debug cvar.

### KI-98: B doesn't dismiss the Options menu's custom popups

- **Status:** open · **Severity:** low
- **Area:** Options menu
- **Tracking:** none yet. Found while building KI-97 (#52).
- **Where:** the `sub_8260EA48` hook in `src/skate3_options_menu.cpp`, and the
  popup manager's input path near `sub_825EF698`.
- **Problem:** pressing B while the quit prompt is up does nothing, and no
  callback fires. The restart prompt (#40) probably behaves the same, and the
  KI-28 tests found it is A-only. #52 puts Cancel first so the highlighted
  button is the safe one.
- **Next step:** find how the popup manager handles B, if B-to-cancel is wanted.

### KI-99: The pause menu's Options tab has no quit item

- **Status:** open (feature idea) · **Severity:** low
- **Area:** pause menu
- **Tracking:** none yet. Raised while building KI-97 (#52).
- **Where:** the pause menu (the "crossbar"). Its Options tab (gear) lists Game
  Settings, Music Player, Extras and Sign in to EA Nation. The item table is
  around `0x830275A0`, in 20-byte entries {name, title ID, icon, int, helper ID}.
  It already has an `ExitDemo` entry at `0x830278C8` (`ID_DEMO_EXIT` "Exit
  Demo", icon "quit"), used only by the trial build.
- **Idea:** a Quit item after Sign in to EA Nation, reusing #52's prompt and
  `CloseGame()`, would be one step quicker than Options > Game Settings >
  Quit Game.

### KI-100: The root Options rows are cut off in a small window

- **Status:** open · **Severity:** low
- **Area:** Options menu, layout
- **Tracking:** none yet. Found while building KI-97 (#52).
- **Problem:** in a 1294x758 window, the Quit Game row (the seventh added row) is
  half off the bottom of the screen. Fullscreen is probably fine.
- **Next step:** check whether the game's list scrolls. If not, consider
  grouping the PC rows under one entry.

### KI-101: Distant pop-in during free roam

- **Status:** open · **Severity:** low
- **Area:** native renderer, streaming / LOD
- **Tracking:** none yet. Seen by the user while testing KI-21 (#33) on
  2026-10-28.
- **Symptom:** objects pop in at a distance while skating, noticed with
  Ambient Occlusion off (D3D12, 4K, MSAA 8x). On Vulkan, a nearby bike rack
  also pops in as the skater passes it.
- **Ruled out:** the occlusion cull. Both the distant pop-in (D3D12) and the
  bike rack (Vulkan) stayed with
  `--skate3_native_render_scene_occlusion_cull=false`.
- **With #30 (KI-18):** trees and other far scenery stayed drawn at the
  extended distance, but the bike rack still popped. To the user it looks
  like LOD or shadow drawing, not draw distance.
- **Not yet checked:** whether it also shows with AO on, or on the emulated
  renderer (F5). Also whether it's the object itself or only its shadow that
  pops: compare with dynamic shadows off in F12, and take an F10 capture just
  before and after it pops. It may be the game's own
  streaming/LOD distance rather than a native-renderer bug.
- **Next step:** compare the same route on F5 and at a higher
  `skate3_draw_distance_scale` (see KI-18, #30).

### KI-102: Linux crashes at the ISO picker on X11 (GTK has no display)

- **Status:** investigating
- **Area:** Linux, installer file picker
- **Tracking:** GitHub issue rubensteezy/skate3recomp#53 (reported by
  cproo12); fix agent working on it.
- **Symptom:** on Linux Mint 22.3 with an X11 session (Intel UHD 630), the
  v2.1.0 release crashes when it opens the "Select ISO" dialog: GTK prints
  `Gtk-ERROR: Can't create a GtkStyleContext without a display connection`
  and the process dies with a trace trap. The log shows
  `SDL video driver: x11` just before. The same code is reported to work under
  Wayland.
- **Where:** `PickIsoFile` in `src/skate3_iso_installer.cpp` and
  `PickTitleUpdateFile` in `src/skate3_title_update_installer.cpp` call
  `gtk_file_chooser_dialog_new` directly on non-Windows, non-macOS builds.
- **Cause so far (unconfirmed):** nothing in `src/` calls `gtk_init` or
  `gtk_init_check`, so GTK never opens its own display connection. Under
  Wayland something else may initialise GTK as a side effect (for example the
  libdecor GTK plugin that SDL loads for window decorations), which would hide
  the bug. Under X11 nothing does. The "X11 vs Wayland" framing in the GitHub
  thread may therefore be a symptom, not the cause.
- **Next step:** confirm by reading the code and the SDL/libdecor sources, then
  call `gtk_init_check` before the first dialog and fall back to something
  that cannot crash (a clear error, a `zenity`/`kdialog` call, or SDL3's
  `SDL_ShowOpenFileDialog`). Testing needs an X11 Linux machine; there is
  currently none (see the Linux build note in the project memory).

### KI-103: GTK 3 is a hard runtime dependency of the Linux zip

- **Status:** open · **Severity:** low–medium
- **Area:** Linux packaging
- **Tracking:** none yet. Found while fixing KI-102 (#55).
- **Where:** the `UNIX AND NOT APPLE` block of `CMakeLists.txt`
  (`pkg_check_modules(GTK3 REQUIRED gtk+-3.0)`) and `tools/package_linux.sh`,
  which bundles only `skate3`, `librexruntime.so` and `librexgpu-xenos.so`.
- **Problem:** the game links libgtk-3 only for the two file dialogs (ISO and
  title update). On a system without GTK 3 (a KDE-only or minimal install) the
  game would fail to start at all, before any dialog.
- **Next step:** replace the GTK dialogs with SDL3's `SDL_ShowOpenFileDialog`
  (`SDL_DIALOG` is already on in the SDK). It is asynchronous and uses the
  desktop portal, so the synchronous picker needs a wait, and it needs a Linux
  tester to validate.

### KI-104: GDK may install its own X error handler once GTK opens an X11 display

- **Status:** open (unverified) · **Severity:** low
- **Area:** Linux, X11
- **Tracking:** none yet. Raised while fixing KI-102 (#55), from memory.
- **Problem:** GDK installs an Xlib error handler when it opens an X11
  display. SDL's X11 backend lives in the same process, so an X error after
  the dialog could exit the process with a GDK message. #55 initialises GTK
  lazily, only when the first dialog opens, which limits this to the
  installer phase.
- **Next step:** the tester should watch for it after the dialog closes (step
  6 of #55's test steps).

### KI-105: A failed Linux file picker shows nothing in the UI

- **Status:** open · **Severity:** low
- **Area:** Linux, installer UI
- **Tracking:** none yet. Found while fixing KI-102 (#55).
- **Problem:** when no file chooser can be shown, the only feedback is a log
  line, and the install wizard just stays open. Also, in the `zenity` and
  `kdialog` fallbacks, an exit code other than 0 is treated as "cancelled", and
  `kdialog` is not tried after `zenity` runs but fails. That is right for a
  normal cancel, but a display failure inside `zenity` is not reported.
- **Next step:** show a message box (or an ImGui notice in the wizard) with
  the `SKATE3_INSTALL_ISO` hint.

## Resolved

### KI-1: Texture pop-in and flicker while streaming

- **Status:** fixed in rubensteezy/skate3recomp#3 (README entry removed in #5)
- **Symptom:** streamed surfaces briefly showed solid colours or other texture
  layers.
- **Fix:** in-place texture payload changes are confirmed before the texture
  is decoded again. Confirmed in-game on 2026-10-12.

### KI-2: Skater rendering in Edit Skater

- **Status:** fixed in rubensteezy/skate3recomp#4, #6 and #8
- **Symptoms:**
  - After skin-tone edits, the native skater showed red patterned arms,
    see-through eyes and the wrong hair for a few seconds (#4).
  - The skater's hair in Edit Skater looked darker than on the emulated
    renderer (#6 and #8).
- **Fix:**
  - #4 keys the texture hold and sticky sites by piece signature, so rebuilt
    pieces no longer pick up each other's textures.
  - #6 ports the `cac_hair` Kajiya-Kay strand highlight.
  - #8 shades `cac_hair` with the world sun shadow.

### KI-4: Bruises and dirt don't show on the player character

- **Status:** fixed in rubensteezy/skate3recomp#20. Bruises and dirt confirmed
  in-game on 2026-10-12. Tattoos were split out as KI-6.
- **Symptom:** bruising and dirt didn't appear on the skater on the native
  renderer. The emulated renderer showed them.
- **Cause:** the CAC pixel shaders (`cacstamp_skin`, `cac_face`,
  `cacstamp_cloth`, `cac_cloth`, `cacstamp_shift`) blend up to two extra layers
  over the tinted diffuse before lighting:
  - `i_decal` is the tattoo stamp, placed with the vertex shader's
    `i_customGraphic` affine of texcoord 1.
  - `i_decal2` is bruise/dirt at uv0, scaled by `i_params.z` on skin or `.y`
    on cloth.

  The native fam-2 branch only drew the diffuse.
- **Fix:** `CaptureCacDecals` captures the layers' fetch words, the affine and
  the amounts into `char_rows`. The renderer binds them at t8/t9, and
  `scene_char.hlsli` composites them. Cvar:
  `skate3_native_render_scene_char_decals`.

### KI-5: Enhanced shadows show artifacts

- **Status:** won't fix (shelved) in rubensteezy/skate3recomp#21. The cause
  was never found. The options were removed instead.
- **Symptom:** with Enhanced Shadows (the native static sun-shadow map) and
  PCSS on, shadows looked blocky or the wrong shape, not better than stock.
  Repro: D3D12, render scale 3x, MSAA 4x/8x.
- **What #21 did:** removed the options from Settings > Video, Options >
  Graphics and F12 MAX QUALITY, and turned them off by default. The code is
  kept behind `--skate3_native_render_scene_experimental_static_shadows=true`
  and `--skate3_native_render_scene_experimental_pcss=true`.
- **Ruled out:** the static caster cache holding `proxyworld.default`
  stand-in meshes. Dropping them made no visible difference.
- **Main suspect if revisited:** the map casts along the material sun (c6),
  which is about 43 degrees off the game's own stylized shadow axis and may
  disagree with the baked lightmaps.

### KI-7: Dev builds hang on the splash screen with the native renderer on at boot

- **Status:** could not reproduce on `main` (c152af0). No code change. Reopen
  if it happens again, and save the log right away, because only the last 5
  logs are kept.
- **Report:** one dev run on 2026-10-12 (D3D12, KI-5's experimental build,
  33cd9b7 plus an uncommitted shadow change). After language select the intro
  video never started. Frames kept running but slowed, and the window was
  closed after a minute. That log has since rotated away.
- **Misread evidence:** the main-checkout logs that seemed to show the hang
  (`skate3_014`..`018`, c8ba8ac) were shader-disassembly runs. They are killed
  on purpose about 2 s after launch, which is why they stop with no mem lines
  and no "Title terminated" line. `skate3_019`, from the same build with the
  scene on from boot, booted normally. No Vulkan hang was ever seen.
- **Checked:** 20 automated boots of `main` with the native renderer on from
  boot, on D3D12 and Vulkan. That includes 8 with `--skate3_demo_path=true`,
  which injects a pad A at language select. All of them reached the intro
  video or gameplay. The user then booted it with a controller on both
  backends and reached the menus. Notes: KI-7-splash-hang.md in the session
  notes folder.

### KI-8: ISO installer path traversal and partial installs

- **Status:** fixed in rubensteezy/skate3recomp#17, tested in-game · was BT-01
- **Problem:** `IsUnsafeIsoPath` only split on `/`, so `\`, drive letters and
  NTFS alternate data streams could escape the install folder on Windows.
  `IsGameInstalled` only checked for `default.xex`, so an interrupted
  extraction counted as installed.
- **Fix:** paths are checked on both separators and rejected for drive
  letters, `:`, control bytes and dot-only components, before anything is
  written. A `.skate3_install_incomplete` marker sits in the game root for
  the whole extraction (`src/skate3_iso_installer.cpp`).

### KI-9: Freecam kept the guest camera after F5 + freecam off

- **Status:** fixed in rubensteezy/skate3recomp#17, tested in-game · was BT-02
- **Problem:** `g_freecam_guest_active` was only cleared in `UpdateFreecam`,
  which `BuildFrameScene` skips on its early returns (scene off, no perspective
  view), so the guest camera rewrite and draw-distance recentring stayed on.
- **Fix:** `ReleaseFreecamIfOff` runs at the top of every `BuildFrameScene`.

### KI-10: Freecam needed smooth_camera to take over the guest camera

- **Status:** fixed in rubensteezy/skate3recomp#17, tested in-game · was BT-03
- **Problem:** `g_sampler_viewcam` was only stored inside the smooth_camera
  block, so with smoothing off the SetViewMatrix override never matched and
  the game culled around the real camera.
- **Fix:** it is stored every frame; only starting the sampler thread still
  depends on smoothing.

### KI-11: SDK cvar overrides stuck after F5; overlapping save/restore

- **Status:** fixed in rubensteezy/skate3recomp#17, tested in-game · was BT-04
- **Problem:** the menu yield, photo-grab window and photo-display yield only
  restored their SDK cvar overrides from RenderScene's normal path, which is
  skipped when the scene is off. Overlapping windows on the same cvar saved
  each other's forced values as the user's.
- **Fix:** one owner-tracked `SdkCvarOverride` per cvar
  (`src/skate3_native_scene_gpu.cpp`); `RestoreAllYieldOverrides()` runs on
  RenderScene's scene-off return.
- **Not tested:** re-taking the overrides after F5 back to native.

### KI-12: MAX QUALITY values written to settings.toml

- **Status:** fixed in rubensteezy/skate3recomp#17, tested in-game · was BT-05
- **Problem:** any settings save while F12 MAX QUALITY was on wrote its maxed
  values (MSAA, shadows, draw distance and so on) into `settings.toml`.
- **Fix:** saves run inside `WithMaxQualitySuspended`, and the SDK settings
  overlay suspends the mode while it is open.

### KI-13: Shader classifier caches keyed by recycled guest addresses

- **Status:** fixed in rubensteezy/skate3recomp#19, tested in-game · was BT-06
- **Problem:** seven classifiers cached their shader debug-path verdict by
  object address forever, but the game recycles shader objects while
  streaming. The env-receiver cache could let an unrelated shader win the
  receiver-row capture, the same failure as the flowingwater dark-world bug.
- **Fix:** `ShaderVerdictMemo` (`src/skate3_native_scene.cpp`) trusts a
  verdict only within the guest frame that computed it.
- **Remaining gap:** a shader recycled within a single frame keeps the old
  verdict until the next frame.

### KI-14: Skinned palette capture read past the VS constant bank

- **Status:** fixed in rubensteezy/skate3recomp#19, tested in-game · was BT-07
- **Problem:** `CaptureSkinnedState` copied 84 bones from palette bases up to
  c8, reading up to 4 registers past c255.
- **Fix:** only whole bones inside c0..c255 are read; the rest are zero.

### KI-15: Publish-gap tracker swapped a clone's capture for its twin's

- **Status:** fixed in rubensteezy/skate3recomp#19, tested in-game · was BT-08
- **Problem:** the `BuildFrameScene` publish-gap tracker was keyed by mesh, so
  a cloned caster-bank instance was replaced by its twin's same-frame capture
  and drawn at the twin's pose.
- **Fix:** the tracker handles single-instance meshes only.
- **Trade-off:** cloned characters no longer get the caster-bank swap or the
  2-frame gap fill. A `(mesh, ctx)` key could restore both once `ctx` is
  confirmed stable across frames.

### KI-31: C++ tests not built, no CI

- **Status:** fix in review in rubensteezy/skate3recomp#43 · **Severity:**
  low · was BT-24
- **Tracking:** #43. CI: all five `tests` jobs pass on the PR (Python 24/24;
  C++ 3/3 on Windows MSVC 19.51 and clang 22.1, Ubuntu clang 18.1 and
  GCC 13.3).
- **Problem:** `tests/*.cpp` (character fade, Hall of Meat, screen FX) weren't
  wired into CMake, and they used `assert`, which does nothing in release
  builds. This repository had no CI, so neither these nor the Python tests
  (`python -m unittest discover -s tests`) ran automatically.
- **Fix:**
  - `tests/CMakeLists.txt` builds `character_fade_test`, `hall_of_meat_test`
    and `screen_fx_test` and registers them with CTest (the `skate3_tests`
    target builds all three). All three test header-only logic in
    `src/native/` that includes only the standard library, so none was left
    out and no stubs were needed.
  - `-DSKATE3_TESTS_ONLY=ON` configures only the tests, with no SDK checkout,
    game data, title update or codegen; `-DSKATE3_BUILD_TESTS=ON` adds them to
    a normal configure. Commands are in CONTRIBUTING.md under Testing.
  - `assert` is replaced by `CHECK` (`tests/test_check.h`), which runs in
    every build type, reports each failed expression, and makes the test
    exit nonzero.
  - `.github/workflows/tests.yml` runs on pull requests and pushes to `main`:
    the Python tests on Ubuntu, and the C++ tests with MSVC and clang on
    Windows and clang and GCC on Ubuntu. It checks out no submodules.
- **Checked locally:** Python tests (24) pass; all three C++ tests pass with
  MSVC 19.51 (Release and Debug) and VS clang 22.1.3 (Release, Ninja), using
  the workflow's own commands. A deliberately failing `CHECK` built with
  `-DNDEBUG` exits 1.
- **Candidates to add later:** KI-24's standalone round trip for
  `src/skate3_user_settings.cpp` and KI-27's DecodeMesh validator test. Their
  fixes (#36, #39) aren't on `main` yet and neither adds a test file, so
  nothing is wired up for them here. #39's `src/native/mesh_decode_validate.h`
  only includes the standard library, so its test can go straight into
  `tests/CMakeLists.txt` once #39 merges. KI-24's parsing and escaping live in
  `src/skate3_user_settings.cpp` next to SDK code and need moving into such a
  header first.

### KI-33: Hook addresses not gated on Title Update 3

- **Status:** fixed in rubensteezy/skate3recomp#27, tested in-game · was BT-26
- **Problem:** the game hooks use hard-coded guest addresses that are only
  correct in the Title Update 3 image, but the build still allowed codegen
  from the unpatched retail image. Release builds were never affected: they
  always use TU3.
- **Checked:** base-XEX codegen (retail `default.xex`,
  `config/skate3_functions.toml`) against the TU3 `generated/`, comparing each
  function's instruction stream with branch targets masked. TU3 shifts most
  code by about 0x24000 to 0x2B000 (for example the front-end state setter is
  `0x82CDFFB8` in retail, `0x82D0AFA0` in TU3). `src/` hooks or calls 90
  TU3 function addresses. 88 are not function starts in retail. The other
  two, `sub_82782818` and `sub_82B78FF0`, are different functions there,
  which is the silent wrong-hook case. The options menu, heap diagnostics and
  exception guard were already gated. The other 61 addresses were not: the
  native renderer, scene, draw-distance and demo-path hooks. Guest data
  addresses (globals, vtables) were not diffed. They sit behind the same
  gate now.
  The demo-path intro-movie codegen patch (`ApplySkate3CodegenPatches.cmake`)
  also anchors on TU3 addresses. The ultrawide frustum and FOV patches match
  instruction patterns instead.
- **Effect:** a retail build failed to link with
  `undefined reference to __imp__sub_...`, because each hook calls the
  original through a symbol the retail codegen doesn't emit. A reconfigure
  that lost the game root (and with it the TU3 fallback) produced the same
  error.
- **Fix:** configure now stops with an error that says how to provide TU3
  when no TU3 codegen input is found. A `static_assert` on
  `SKATE3_HAS_TITLE_UPDATE` at the top of each ungated hook block marks the
  addresses as TU3-only.
- **Leftover:** the retail-only branches (`#if !SKATE3_HAS_TITLE_UPDATE` in
  `src/skate3_app_common.cpp` and `src/exception_compat.cpp`, the
  retail `config/skate3_functions.toml`) are now unreachable.

### KI-34: Own the Spot / Own the Lot UI pops in and out or draws wrong

- **Status:** fixed in rubensteezy/skate3recomp#25. Confirmed in-game on
  D3D12 and Vulkan on 2026-10-12.
- **Symptom:** during Own the Lot (and Own the Spot) events the score and
  timer, objective text, challenge list and spot markers flickered in and out
  or drew incompletely. Native renderer only: the cause is in the native
  renderer's own vertex upload, so the emulated renderer (F5) is not affected.
- **Cause:** the in-world spline pass (the neon boundary/marker strips) and the
  HUD/2D pass share one per-frame upload region for their vertices, splines
  first. An Own the Lot boundary is up to 54 strips, about 1.03 MB, and the
  region was 1 MB. With the whole outline in view the spline pass filled it,
  so its last strip and every HUD draw after it were skipped without a log
  line (7 HUD draws skipped per frame in a measured run). The camera moving
  the outline in and out of view made the HUD pop in and out.
- **Fix:** the region (`RendererState::kUiRegionSize`,
  `skate3_native_scene_gpu_internal.h`) is now 4 MB, and the spline pass in
  `RenderScene` stops at `kUiSplineBudget`, 1 MB short of the end, so the HUD
  always has room. A skipped spline or 2D draw now logs a rolling-capped
  "UI vertex region full" warning.

### KI-35: Team-member portraits in character creation show old photos

- **Status:** fixed in rubensteezy/skate3recomp#26. Confirmed in-game on D3D12 and Vulkan on
  2026-10-12.
- **Symptom:** in Pause > Create Skater, the boxes of the extra team members
  showed photos from earlier events instead of each skater's portrait, a
  different old photo per member. The emulated renderer (F5) showed the same
  images because the portrait is saved with the team member. The bad members
  came from the career "new team member" prompt on the native renderer
  (v2.1.0).
- **Cause:** `SkaterPresentationPhoto` (request `sub_8278EE78`, render and
  extract step `sub_8278E960`) renders the skater into the postfx manager's
  1152x640 screenshot target, and `PhotoEffects::GeneratePhoto` CPU-reads that
  resolve from guest memory at 0x04911000 into the 128x256 portrait. That's the
  same surface the photo grab fills. On the native renderer the resolve only
  reached CPU memory when the pause-menu portrait window (suppress mode 3,
  picked by FE-stack heuristics in `YieldForMenus`) and the SDK's targeted
  0x04911000 copy (non-gameplay contexts only) both applied. The career prompt
  fell outside them, so the portrait was cut from whatever the last photo grab
  left there. Reproduced on main with
  `--skate3_native_render_scene_menu_rtt_passes=false`: take a photo, edit a
  member, and its portrait becomes that photo.
- **Fix:** hooks on the request and the step (`skate3_native_render.cpp`)
  drive `UpdateSkaterPhotoWindow` (`skate3_native_scene_gpu.cpp`). From the
  request until the step reports done, it holds suppress mode 3, synchronous
  shaders and the forced readback of resolves up to 0x2D0000, in any presence
  context. The native photo grab also stops writing 0x04911000 for that time.
  The log shows "skater photo window OPEN" and "skater photo generated (...
  window armed)". Existing bad portraits heal by editing the member once.
- **Emulated (F5), not fixed here:** in a gameplay presence context with
  `readback_resolve=none`, the emulated renderer would read the same stale
  memory (the SDK's targeted copy skips gameplay contexts). Not seen in play.

### KI-38: Frames-in-flight limit may skip frames only the native renderer draws

- **Status:** not a bug. No code change. Measured on `main` (f4d8500) with
  temporary logging, unattended, on D3D12 and Vulkan.
- **Suspicion:** rexglue-sdk's command processors only wait for a free frame
  (`kQueueFrames` / `kMaxFramesInFlight` = 3, in `BeginSubmission`) when a
  frame opens, and `EndSubmission` only closes a frame when `frame_open_` is
  set. If the native frame's `EndSubmission(true)` ran with no frame open, the
  GPU could fall any number of frames behind.
- **Why it can't happen:** `IssueSwap` on both backends calls
  `BeginSubmission(true)` before it calls the presenter's refresher ("in case
  the swap command is the only one in the frame"), and the Vulkan refresher
  calls it again. That opens the frame, and waits for the frame 3 back, even
  when every guest draw is suppressed. The native renderer runs inside the
  refresher, and nothing in the native RHI (`native_rhi_d3d12.cpp`,
  `native_rhi_vulkan.cpp`) ends a submission, so the frame is still open when
  the refresher's `EndSubmission(true)` closes it.
- **Measured:** temporary logging at the native `EndSubmission(true)` on both
  backends recorded `frame_open_` before the call and the frames whose closing
  submission the real fence hadn't reached yet (D3D12 `GetCompletedValue`,
  Vulkan `vkGetFenceStatus`). `--skate3_demo_path=true`, native renderer on,
  about 100 s of each run in gameplay. #28's upload-ring code was applied on
  top for the measurement.

  | Run | Native frames | `frame_open_` false | Frames in flight (max) | Frame time |
  |---|---|---|---|---|
  | D3D12, defaults | 25,200 | 0 | 1 (99.8%), 2 (51 frames) | 4.4 ms |
  | Vulkan, defaults | 26,400 | 0 | 1 (99.99%), 2 (1 frame) | 4.5 ms |
  | D3D12, GPU-bound | 7,800 | 0 | 3 in gameplay, never 4 | 15.9 ms |
  | Vulkan, GPU-bound | 9,600 | 0 | 3 in gameplay, never 4 | 12.9 ms |

  GPU-bound runs added `--resolution_scale=6
  --skate3_native_render_scene_msaa=8 --skate3_native_render_scene_ssr=true
  --skate3_native_render_scene_ssr_steps=64
  --skate3_native_render_scene_shafts_res=2
  --skate3_native_render_scene_ssao_full_res=true`. The cap holds at exactly
  3. #28's "upload ring <name>: K/N regions in flight" (logged at half the
  8-region ring or more) and "all N regions in flight" lines never appeared.
- **The "more than 3 frames behind" comment:** the bone-ring comment in
  `skate3_native_scene_gpu_internal.h` most likely counted submissions, not
  frames. Vulkan reached 6 submissions in flight while loading (several
  submissions per frame) with frames still at 3 or fewer. A ring keyed by
  frame number with 4 or more regions is never reused while the GPU reads it,
  so the hair and garment corruption that comment describes had some other
  cause.

### KI-28: Options menu edge cases

- **Status:** fixed in rubensteezy/skate3recomp#40, tested in-game on
  2026-10-28 · **Severity:** low · was BT-21
- **Where:** `src/skate3_options_menu.cpp`
- **Problem:** `StepRow` took `% count` with no guard for an empty label
  list (divide by zero), and the row value getter indexed `label_ids` with
  whatever the row's getter returned. `g_restart_popup_open` was set when the
  restart prompt opened and cleared only in its callback.
- **Cause:** the prompt reuses the Video Calibration popup's callback
  (`sub_8260EA48`), and the hook answered for the prompt whenever the bool
  was set. A screen can close a popup by id without calling its callback
  (`sub_82D0A628`), so the bool could stay set. The next Video Calibration
  answer would then have been treated as the restart prompt's: its first
  button would have applied the staged settings and restarted the game.
- **Fix:** the popup manager returns an id for each popup and passes it to the
  callback in r3, both for a button press (`sub_825EF698`) and when it flushes
  all its popups (`sub_82D0A738`, button -2). The hook now stores the
  prompt's id and answers only a callback carrying that id. Ids come from an
  ever-increasing counter, so a stale one never matches. Whether the prompt is
  open is read from the manager's popup queue (`PopupShown`), which also
  stops a second Back from stacking another prompt. A flush keeps the staged
  choices and the screen as they are. `StepRow` is a no-op for a row with no
  labels (no sound, input not consumed). The value getters show nothing
  for an out-of-range label. The number and string choice setters ignore
  out-of-range indices.
- **Tests:** a standalone check of the step and label logic (empty list,
  wrap, out-of-range getters, popup-id matching) passes. In-game, driven by
  `skate3_demo_path_gameplay_inputs` (Pause > Options tab > Game Settings >
  Graphics, step MSAA, Back): the prompt shows ("restart prompt shown"), B
  doesn't close it (it is A-only), Discard Changes logs button 1 and returns
  to Game Settings, and the PC rows still step afterwards. Restart Now logs
  button 0, applies the setting and relaunches. After a Discard, Video
  Settings > Video Calibration still opens the game's own calibration
  screens.
- **In-game test (user, D3D12, 2026-10-28):** three restart prompts answered
  with Discard, each returning the old value with rows still stepping. The
  Video Calibration popup that followed was correctly left to the game.
  Restart Now saved, exited cleanly and relaunched with MSAA 8x changed to 4x.
  Every row on Display, Graphics, View, PC Controls and System stepped
  correctly, including wrap-around.
- **Not tested:** a popup flush (button -2) while the prompt is up, and the
  Esc overlay opened over the prompt (it was opened just before one instead).

### KI-30: F12 dialog rewrites a cvar the render thread overrides

- **Status:** fixed in rubensteezy/skate3recomp#42, tested in-game on 2026-10-28 · **Severity:** low · was BT-23
- **Where:** `src/skate3_native_debug_dialog.cpp` (every section),
  `SdkCvarOverride` in `src/skate3_native_scene_gpu.cpp`
- **Cause:** almost every control was written as
  `REXCVAR_SET(x, CvarCheckbox(..., REXCVAR_GET(x)))`, so each UI frame read
  the cvar and wrote it straight back, edit or not. `REXCVAR_SET` is a plain
  store, so a write by another thread that landed between the read and the
  write-back was undone. For `native_render_suppress_emulated_draws` that
  other writer is the render thread's `SdkCvarOverride` (KI-11). Undoing its
  forced value left the menu or photo window running without the override,
  because `Acquire` never writes again while held. Undoing its restore left the
  forced value in place after the window closed. The same pattern raced the
  options menu, the settings overlay, MAX QUALITY and the hotkeys on every
  other cvar in the dialog. The F5 checkbox read the cvar twice, so an F5
  press between the reads was toggled straight back.
- **Fix:** `CvarCheckbox`, `CvarSlider` and `ValueCombo` take the cvar's
  storage and write it only on the frame ImGui reports an edit. The sun
  override and F5 checkboxes use the `Checkbox` return value too. The
  controls that were already conditional (`SliderInt`, `Combo`, `InputInt`
  and the explicit `SliderFloat`s) are unchanged. While an override holds the
  suppress cvar, the dialog shows the user's value from the new
  `SdkCvarOverride::HeldView` (an atomic mirror of the saved value, through
  `native_scene::SuppressEmulatedDrawsOverride`), greys the box out and adds
  an "(overridden)" note. `Release` now logs a warning if the forced value
  was overwritten while held. MAX QUALITY's suspend and restore (KI-12) is
  unchanged. New debug cvar `skate3_native_debug_open_at_boot` opens F12 at
  startup for unattended runs.
- **Tested:** three unattended demo-path boots with F12 open from startup and
  `--skate3_native_render_scene_menu_unsuppress=true`, so the menu context
  holds the suppress override. The boot menu ("portrait window - suppress
  mode -> 3") and the pause menu showed the box ticked, greyed and
  "(overridden)". After resuming ("leaving menu context - emulated draw
  suppression restored") it was ticked and editable again. No "overwritten
  while held" warning in any run. Clicks can't be injected, so control edits
  and the F5 path ("SDK cvar overrides ... restored") were not exercised.
- **In-game test (2026-10-28):** user on D3D12: checkbox and slider edits (SSAO, Bloom, Dynamic shadows, bloom threshold, fly speed, MSAA 8x to 4x, shadow tile) stuck across closing and reopening F12; with F12 open during the Edit Skater photo, Suppress emulated draws showed greyed out and (overridden), portraits rendered correctly; F12 MAX QUALITY on and off three times maxed out and restored the user's settings each time; no 'overwritten while held' warning in the log.

### KI-23: OnShutdown missed several UnregisterBind calls

- **Status:** fixed in rubensteezy/skate3recomp#35, tested in-game on 2026-10-28
- **Problem:** `Skate3BaseApp::OnCreateDialogs` registers ten key binds, but
  `OnShutdown` only unregistered six. The screenshot bind (F6, captures
  `this`), the F5 renderer toggle, the Ctrl+Shift+B showcase bind and the End
  freecam bind stayed live. The SDK's bind registry is a process-lifetime
  static, so a key event reaching it after the app object was gone would call
  into a destroyed object.
- **Reach:** low in practice. The normal window close (and the overlay's Close
  Game on Windows/Linux) ends in `ReXApp::OnClosing`, which hard-exits before
  `OnDestroy`, so `OnShutdown` never runs there. It runs when the app quits
  through `QuitFromUIThread` or a failed `OnInitialize`: a cancelled or failed
  ISO / title update install, a `ConstructRuntime` failure, and Close Game on
  macOS.
- **Fix:** binds are registered through `RegisterAppBind`, which records each
  name; `OnShutdown` calls `UnregisterAppBinds`, which unregisters them all in
  reverse order before the dialogs they reach are reset, and logs
  "Skate 3 shutdown: unregistered N key binds". The SDK has no RAII bind
  handle. `UnregisterBind` only nulls the callback, so it is safe at any point
  in `OnShutdown` (the registry outlives the app, and `ReXApp::OnDestroy`
  calls `OnShutdown` before its own overlay teardown).
- **Tested:** relwithdebinfo build. A bogus `SKATE3_INSTALL_ISO` into an empty
  game root makes the installer fail and quit through `OnShutdown`: the log
  shows "unregistered 10 key binds", exit code 0, no WER report (the baseline
  build exits the same way without the line). `--skate3_demo_path=true` to
  gameplay, then WM_CLOSE: "Title terminated; hard-exiting process.", exit
  code 0, no WER report, same as baseline.
- **Not tested:** pressing the bind keys in this build (synthetic keys could
  not reach the unfocused window).
- **In-game test (2026-10-28):** user on D3D12, fullscreen: F6 saved a screenshot, F5 switched to emulated and back, End engaged and released freecam (422 guest SetViewMatrix rewrites), Esc > Close Game and Alt+F4 both logged 'Window closing' then 'Title terminated' with no crash dump.

### KI-32: Small cleanups

- **Status:** fixed in rubensteezy/skate3recomp#44, tested in-game on 2026-10-28 · **Severity:** low · was BT-25
- **Problems and fixes:**
  - Screenshots taken within the same second got the same filename, so the
    later save overwrote the earlier one. `ScreenshotPath`
    (`src/skate3_screenshot.cpp`) now writes
    `shot_YYYYMMDD_HHMMSS_mmm[_tag].png`. The millisecond stamp is strictly
    increasing per process, so captures in the same millisecond still
    differ, and name order stays capture order.
  - `blur_tex` (popup blur, also the photo-grab source) was created once and
    never rebuilt, and `menu_blur_tex` was keyed on size only. Both now
    record the format they were built with and are recreated when the guest
    output format changes (`EnsureBlurOutlineTargets`,
    `src/skate3_native_scene_gpu.cpp`), like the blur PSOs. The guest output
    is a fixed R10G10B10A2 on both backends today, so this guards a future
    change; HDR on/off changes the scene format, not this one.
  - `kSceneRingFrames` is 2400 on purpose: the RB+A note says the dump is
    retroactive about 17 s, which is 2400 frames at 144 fps. The four
    "~900 frames" comments were stale and now say 2400.
  - `skate3_native_scene_state.h` (`MaybeDumpSceneRing`) now includes
    `<ctime>` and `<fstream>`. Compiled on its own, the old header failed on
    `std::ofstream`; the new one compiles.
- **Tested:** a unit check of the naming code (3000-name burst, 4 threads)
  found no duplicates and sorted order equal to capture order. Two
  unattended boots (HDR on, HDR off) ran the post sweep, which saved 7
  screenshots with the new names (up to three in one second), opened the
  pause menu with its popup blur showing, and logged no blur errors. The
  settings-overlay blur (`menu_blur_tex`, Esc/F1) needs a key press and was
  not exercised.
- **In-game test (2026-10-28):** user on D3D12, 4K fullscreen: five F6 presses within one second saved five files with distinct millisecond stamps (shot_20261028_211526_110 to _760); the Esc settings overlay and the pause-menu popup blur looked correct each time. The HDR-toggle recreate path was not exercised (the output format is fixed today).

### KI-17: Texture routing maps grew without bound

- **Status:** fixed in rubensteezy/skate3recomp#29, tested in-game on 2026-10-28 · **Severity:** low–medium · was BT-10
- **Area:** native renderer, texture cache
- **Problem:** `tex_routes`, `tex_sticky`, `words_sticky` and
  `tex_pending_first` were only cleared by the debug cache flush, so they grew
  for the whole session.
- **Fix:** every entry is stamped with `last_used_frame` when it is used, and
  `PruneTexRouting` (in `skate3_native_scene_gpu.cpp`, run right after
  `EvictTexStore`) sweeps the maps every 120 frames with the store LRU's idle
  guard (45 s of wall-clock time, never less than 8× the detail-downgrade
  hold). An idle route or sticky site is only dropped once its words key has
  no valid decode in the store, because then it serves the same thing a fresh
  entry would. Anything idle for 5 minutes goes regardless. An entry used this
  frame is never dropped, so the Edit Skater piece-signature hold (KI-2) and
  the streaming sticky serving (KI-1) keep their state while it matters.
- **Stats:** the `native-scene: tex routing pruned N (routes= sticky=
  words_sticky= pending_first= store=)` line logs at most once a minute when
  something was pruned. The perf log line (`skate3_native_render_scene_perf_log`)
  shows the sizes in `store[...]` too.
- **In-game test (2026-10-28):** user on D3D12, 4K, about 5.5 minutes: free roam with streaming showed no texture flicker or swaps, the replay editor kept a normal frame rate, and skin-tone and clothing edits in Edit Skater showed no wrong textures. The log showed three prune passes (67, 1 and 151 entries) with routes holding at about 1,400 to 1,500; tex_sticky still grew slowly (18,284 to 19,270), so whether it plateaus over a long session is not yet shown.

### KI-19: DLC and recipe directory scans can throw at startup

- **Status:** fixed in rubensteezy/skate3recomp#31, tested in-game on 2026-10-28 · **Severity:** low–medium · was BT-12
- **Area:** app startup
- **Where:** `Skate3BaseApp::InstallDlcPackages`,
  `Skate3BaseApp::DiscoverRecipeAliases` and `Skate3BaseApp::InstallRecipeOverlay`
  (`src/skate3_app_common.cpp`), `skate3::shader_disasm::RunIfRequested`
  (`src/skate3_shader_disasm.cpp`, dev tool)
- **Problem:** the scans used the non-throwing iterator constructor inside a
  range-for, but the range-for's `operator++` throws on any error partway
  through a folder. The DLC scan is recursive, so a subfolder of a `dlc`
  folder without read access threw `filesystem_error` out of startup. The
  throwing `is_directory`, `is_regular_file` and `exists` calls around the
  scans could do the same.
- **Fix:** each loop steps with `increment(ec)` and logs a warning naming the
  folder when it fails. The standard library ends the iteration on such an
  error, so the DLC scan also probes each subfolder before recursing and skips
  an unreadable one with `Skipping unreadable Skate 3 DLC subfolder ...`
  instead of losing the rest of the folder. The status checks in and around
  the loops use the `error_code` overloads. Nothing changes when no error
  occurs. The installer and screenshot code have no directory scans.
- **Tested:** relwithdebinfo, unattended boot (`--skate3_demo_path=true`)
  with `--user_data_root=` on a scratch folder whose `dlc` holds an
  unreadable subfolder (`icacls /deny %USERNAME%:(RX)`), a loose file after
  it and a readable nested subfolder. The boot logged the skip warning for
  the unreadable subfolder, still scanned the two files after it and reached
  gameplay. With the deny removed it scanned all three files, no warning.
  The recipe overlay logged 11 aliases, the same as earlier builds. Not yet
  tested: a normal boot with real DLC installed.
- **In-game test (2026-10-28):** user boot on D3D12 with an empty dlc folder: 'No Skate 3 DLC packages found', the BIG-directory VFS overlay installed with 11 aliases, gameplay prewarm decoded 3,144 meshes with 0 dropped, and Alt+F4 exited cleanly. Not tested with real DLC packages present (the user has none installed); the unreadable-subfolder path was covered by the unattended test.

### KI-46: Long user-data paths silently drop a VFS overlay alias

- **Status:** fixed in rubensteezy/skate3recomp#46, tested in-game on 2026-10-28
- **Where:** `src/skate3_app_common.cpp`, `Skate3BaseApp::InstallRecipeOverlay`
  / `CreateOverlayDirectory`.
- **Problem:** with a long user-data path, the overlay folder
  `<cache_root>\vfs_big_directory_aliases\data\state\livingworldentities\pedestrian\plugin`
  passed CreateDirectoryW's 248-character limit and was never created. The
  failure was only counted, never logged, so a user with a long AppData or
  portable path silently lost the pedestrian plugin alias. A test run from a
  long scratch path logged "overlay with 10 aliases"; the same build from a
  short path logged 11.
- **Cause:** the overlay used plain Win32 paths. CreateDirectoryW stops at 248
  characters, and the SDK host device's `FindFirstFileW` /
  `GetFileAttributesExW` calls (`ListFiles`, `GetInfo` in
  `filesystem_win.cpp`) at MAX_PATH. The exe manifest has no `longPathAware`,
  and that would also need the OS LongPathsEnabled policy.
- **Fix:** `ToExtendedLengthPath` turns the overlay root into an absolute,
  normalized `\\?\` path (`\\?\UNC\` for shares) before the folders are
  created and before it becomes the `skate3bigdirs:` host device root. The
  SDK device builds every entry path from that root and passes it straight to
  the wide Win32 calls, so the whole overlay tree is long-path safe with no
  SDK change and no OS policy. Each alias that can't be created now logs a
  warning with the guest path, host path, its length and the `error_code`
  message and Win32 code, plus a summary line ("N of M alias directories
  could not be created"). Linux and macOS are unchanged.
- **Tested unattended** (relwithdebinfo, `--skate3_demo_path=true`, cwd = the
  install folder), each run reached "gameplay reached":
  - short user-data root (pedestrian alias path 153 characters): "overlay
    with 11 aliases".
  - long user-data root (218 characters, pedestrian alias path 299): "overlay
    with 11 aliases", and the folder exists on disk. The pedestrian and
    PluginDescriptor `NtCreateFile` probes log the same results as on the
    short root.
  - a file planted where the pedestrian `plugin` folder goes: the new
    warning names the alias and path with "error 183", then "1 of 11 alias
    directories could not be created" and "overlay with 10 aliases".
- **In-game test (2026-10-28):** user boot on D3D12 with the normal (short) AppData path: the overlay installed with 11 aliases through the new \?\ root, shader storage initialized normally, gameplay prewarm decoded 3,138 meshes with 0 dropped, and Alt+F4 exited cleanly. The long-path case was covered by the unattended test.

### KI-21: Occlusion culling silently inactive with SSAO off

- **Status:** fixed in rubensteezy/skate3recomp#33, tested in-game on 2026-10-28 · **Severity:** low–medium · was BT-14
- **Area:** native renderer, performance. The emulated renderer (F5) has no
  occlusion cull, so it is not affected.
- **Problem:** with Ambient Occlusion off, the occlusion cull never ran and
  nothing said so. Unattended boots into free roam (1600x900, MSAA 8x),
  read from the `native-scene: frame` perf line:

  | Build, backend | SSAO | items | draws | culled/frame | guest skips/frame | render thread |
  | --- | --- | --- | --- | --- | --- | --- |
  | before, D3D12 | on | 678 | 1095 | 137 | 540 | 0.9 ms |
  | before, D3D12 | off | 1048 | 1922 | 0 | 0 | 1.4 ms |
  | before, Vulkan | on | 675 | 1080 | 139 | 539 | 0.9 ms |
  | before, Vulkan | off | 1056 | 1931 | 0 | 0 | 1.4 ms |
  | after, D3D12 | on | 674 | 1088 | 137 | 539 | 0.9 ms |
  | after, D3D12 | off | 681 | 1106 | 139 | 539 | 1.0 ms |
  | after, Vulkan | on | 670 | 1072 | 139 | 541 | 0.9 ms |
  | after, Vulkan | off | 666 | 1087 | 138 | 540 | 1.0 ms |

- **Cause:** the cull reads a small depth grid (a tile-MAX reduce of the
  linearized scene depth, read back 1-2 frames late). Only `ApplySsaoPass`
  (`src/skate3_native_scene_post.cpp`) built it, from the GTAO chain's
  linear-depth plane. With SSAO off no grid was ever built, the grid went
  invalid after 240 frames, and `RenderScene` kept the cull, the build-side
  skip and the guest dispatch filter off without a log line.
- **Fix:** the grid build is factored out of `ApplySsaoPass` into
  `RecordOcclusionGrid`, with shared helpers for the binding layout, the
  linear-depth plane, the depth view and the linearize draw. When the GTAO
  chain does not run, the new `ApplyOcclusionGridPass` does only the
  full-res linearize (its own `ps_linearize` PSO) and the grid reduce. It
  runs only on frames where `ApplySsaoPass` did not, so there is one grid
  build per frame either way. SSR and the volumetrics reuse the linear depth
  it leaves, as they already did after the SSAO pass. No shader changes.
  `RenderScene` now logs `native-scene: occlusion cull active (<grid
  source>)` or `... inactive (<reason>)` when the cull's state holds for 30
  frames, at most one line per 5 s.
- **Tested:** unattended `--skate3_demo_path=true` boots on D3D12 and
  Vulkan, SSAO on and off, MSAA 8x and 1x. Screenshots of a D3D12 run with
  SSAO off while the skater pushes showed nothing missing. Vulkan frames
  could not be captured by the test script.
- **Not tested:** free roam by hand, toggling SSAO live, and loading or
  map-switch transitions.
- **In-game test (2026-10-28):** user on D3D12 and Vulkan, 4K, MSAA 8x. With AO off the standalone depth pass came up ('grid built without SSAO') and the cull stayed active: on D3D12 occl_culled kept rising (about 1.4M to 4.7M over five minutes) at a steady 113-120 fps, including the replay editor. The cull went inactive only once, after a large camera move, with no flapping. Distant pop-in (D3D12) and a bike rack popping in up close (Vulkan) were also seen with culling forced off, so they are not this change (tracked as KI-101).

### KI-97: No way to quit from the game's own menus in fullscreen

- **Status:** fixed in rubensteezy/skate3recomp#52, tested in-game on 2026-10-29
- **Area:** Options menu, app exit
- **Tracking:** #52. Feature request from the user, 2026-10-28.
- **Symptom:** in fullscreen there's no window X. A controller player could
  quit only through the Esc overlay's Close Game. The game's own Options menu
  had no quit option.
- **Fix:** `src/skate3_options_menu.cpp` adds a Quit Game row after All PC
  Settings on the root Options screen (Pause > Options > Game Settings, and
  the front-end Options).
  - Selecting it shows a popup through the same popup-id mechanism as the
    restart prompt (`ShowPrompt`, answered in the `sub_8260EA48` hook). The
    message is "Quit Skate 3? Progress since the last save will be lost."
    (this popup style shows only the message, not the title). The buttons
    are the game's own localized `ID_COMMON_CANCEL` and `ID_COMMON_QUIT`, in
    that order, like the game's own quit prompts (No before Yes), so the
    default button cancels.
  - Quit saves the settings and then calls `Skate3BaseApp::CloseGame` in
    `src/skate3_app_common.cpp`. That is the overlay's Close Game exit,
    factored out so both use it: `window()->RequestClose()` on Windows and
    Linux ("Window closing, shutting down..." then "Title terminated"),
    `QuitFromUIThread` on macOS, plus the 10 s watchdog on macOS and Linux.
  - Quitting waits (up to 15 s) for a saved game that's open for writing.
    The XDK `XContentCreate` / `XContentClose` wrappers (`sub_82EDE718` /
    `sub_82EDE7A8`) are hooked to track saved-game content opened with any
    disposition other than "open existing".
  - The seventh root row needed a seventh spare option id. The only ones
    left (13, 24, 26) are still live selectors in the input handler
    (`sub_8260C258`) and the value getter (`sub_8260D968`). Id 24 (units) is
    used, and both hooks now answer for our root rows, so left/right on
    them do nothing.
- **Tested unattended (demo-path pad inputs, windowed):** the row shows
  after All PC Settings, A on it shows the prompt ("quit prompt shown"
  logged, PrintWindow screenshot), and the game's `ID_COMMON_QUIT` /
  `ID_COMMON_CANCEL` resolve to "Quit" / "Cancel". At boot the career save
  `rmcsave` is opened five times, all "open existing" (reads), and each is
  closed within about 10 ms, so the save tracking doesn't hold a quit back
  on reads.
- **Not tested:** Quit, Cancel, a quit during an autosave, and macOS/Linux.
  B does not dismiss this popup: in the one run, B on the open prompt did
  nothing and no callback fired. Cancel is the highlighted button and needs A.
- **In-game test (2026-10-29):** user on D3D12, fullscreen: Options > Game Settings > Quit Game showed the prompt, and choosing Quit saved settings and exited cleanly ('quit prompt answered (button 1)', 'Quit Game confirmed; saving settings and closing the game', 'Window closing', 'Title terminated', no crash dump). Cancel, B on the popup and quitting during an autosave were not exercised in this run.

### KI-16: Upload rings reused without a GPU completion check

- **Status:** fixed in rubensteezy/skate3recomp#28, tested in-game on 2026-10-29
- **Problem:** the native renderer's per-frame upload rings (bone + ROPA
  vertices, UI vertices, shadow constants, photo FX constants, screen FX
  constants) picked their region with `frame_number % N` and never checked
  that the GPU had finished with it. A GPU more than N-1 frames behind would
  read a later frame's HUD vertices, bones or constants. The bone ring had
  already been deepened from 4 to 8 after hair and garment corruption on
  Vulkan under load.
- **Fix:** `UploadRingRegions` (`skate3_native_scene_gpu_internal.h`, acquire
  in `skate3_native_scene_gpu.cpp`) is shared by all five rings. Each region
  records the last submission that used it (`CurrentSubmission()`), and a new
  frame takes the next region whose submission has completed
  (`CompletedSubmission()`). Normally that is the same round-robin region as
  before, so there is no stall. The photo FX and screen FX rings went from 4
  to 8 regions like the others.
- **Remaining gap:** nrhi has no fence wait, so if every region is still in
  flight the oldest is reused (the old behavior) and the log says "upload ring
  <name>: all N regions in flight" (rate-limited). "upload ring <name>: K/N
  regions in flight" lines log a new high of half the ring or more.
- **Reported regression (2026-10-28, D3D12, 4K, MSAA 8x):** with #28's
  build the replay editor dropped to a low frame rate that seemed to get
  worse; a main build from 2026-10-13 felt fine. The run log shows the
  native frame rate falling from 120 fps to 42-50 fps in two windows,
  21:27:41-21:27:53 and 21:28:18 onward, with 120 fps in between.
- **Cause:** not #28. Another worktree's full game build
  (`agent-a164568a7fb15f931`, compiling the `skate3_recomp.*.cpp` units on
  every core) ran at the same time. Its `.ninja_log` and object timestamps
  put two builds at 21:27:39-21:27:57 and 21:28:17-21:31:19. Those match the
  two slow windows to within about 2 s, and the 120 fps stretch is the gap
  between them. The "SLOW frame maintenance ... dissolve=11930us/1" lines fit
  CPU starvation too. That step normally takes about 0.1 ms for one view, and
  #28 creates and destroys no views. #28's region picker is pure CPU work of a
  few nanoseconds per call. A standalone copy measured 1.6-17 ns per call, with
  exactly one pick per ring per frame, no growth, and no allocations. Every
  `Region()` call is inside `RenderScene`, which runs once per native frame on
  every screen: gameplay, replay editor, photo editor, Create-a-Skater and the
  pause menu.
- **Diagnostics added:** `--skate3_native_render_scene_upload_ring_log=true`
  logs one "upload rings:" line per perf interval. It shows the window's
  fps, ms per frame and pick time, GPU lag in submissions, and for each ring
  the calls, picks, skips, fallbacks and peak regions in flight.
  `skate3_native_render_scene_upload_ring_gate` (default on) switches back to
  the old `frame_number % N` pick at runtime, for an A/B check. Both are also
  checkboxes in F12 > Diagnostics > "Upload rings (KI-16)".
- **To confirm:** run the replay editor for 2 minutes with no build running
  on the machine. Frame time should match main, and toggling the gate should
  not change it.
- **In-game test (2026-10-29):** user on D3D12, 4K, about 6.5 minutes with the upload-ring log on and nothing compiling in the background: replay editor ran smoothly. Log: 118 fps average at the 120 fps cap (one 5 s dip to 94 fps), 0 fallbacks, no 'regions in flight' warnings, GPU at most 2 frames behind, region pick cost about 0.3 us per frame; switching the gate off and on again left the frame rate at 120. The earlier replay-editor slowdown was a build running on the same machine, not this change. Not re-tested on Vulkan.

### KI-41: Shadow-pass textures could be evicted while still in use

- **Status:** fixed in rubensteezy/skate3recomp#56, tested in-game on 2026-10-31 · **Severity:** low–medium
- **Area:** native renderer, shadows, texture cache
- **Where:** `LookupResolvedTexture` in `src/skate3_native_scene_gpu.cpp`.
  `RenderShadowAtlas` calls it for alpha-clip casters (`item.dynobj == 2`) and
  `RenderStaticSunMap` (experimental since KI-5) for phase-1
  `rec.diffuse_tex`.
- **Problem:** the lookup stamped the route but not the `tex_store` entry.
  `EvictTexStore` picks candidates by `last_used_frame` alone, with a 5 s idle
  guard under byte pressure. A texture that only the shadow passes sample (a
  caster the main pass occlusion-culls or does not draw) looked idle and was
  evicted. The caster passes never enqueue a decode, so it stayed gone and the
  alpha-tested caster cast a solid shape until the main pass drew the item
  again.
- **Evidence:** reproduced with temporary counters (not committed) in the
  lookup, on unattended `--skate3_demo_path=true` boots in the plaza spawn
  with `--skate3_native_render_scene_experimental_static_shadows=true` and a
  64 MB store budget (the debug build lowered the 256 MB floor). Over about
  33,000 frames, shadow lookups of a key the shadow pass had sampled before but
  that was no longer in the store: 228 of 9,588 on D3D12 and 229 of 9,588 on
  Vulkan. With the stamp: 0 on both. The store held the same size (about
  430 entries, 65 MB) either way. In this spawn, every lookup came from the
  static sun map: no `dynobj == 2` casters were in view, so the default
  settings did not hit it there. The evicted caster was off-screen, so the
  screenshots showed no visible change.
- **Fix:** `LookupResolvedTexture` also sets the store entry's
  `last_used_frame` to the current frame. Both callers go through it.
- **In-game test (2026-10-31):** Played ~15 min on the big rig (D3D12, 4K) with --skate3_native_render_scene_tex_store_mb=256: no tree/fence shadow cut-outs seen, 'tex store LRU start' logged 71 times (eviction pressure reached), clean exit. No log counter exists for the evicted-texture lookup, so the evidence is visual only.

### KI-26: ProcessPrewarmEntry did an unguarded read on a worker

- **Status:** fixed in rubensteezy/skate3recomp#38, tested in-game on 2026-10-31
- **Tracking:** rubensteezy/skate3recomp#38
- **Problem:** `ProcessPrewarmEntry` (`src/skate3_native_scene_gpu.cpp`) made
  a guarded copy of the mesh record into `record` and never used it. It then
  read the island count (+0x38) and island-params pointer (+0x44) with raw
  `REX_LOAD_U32` on the prewarm decode worker. `BuildItemFromMesh`
  (`src/skate3_native_scene.cpp`), which the worker calls next, also read the
  mesh's bbox, material, vertex descriptor and buffer pointers with raw loads.
- **Cause:** registration hands the workers mesh addresses that can go stale
  (map change, streaming decommit) before a worker gets to them. On Windows a
  raw load of an unmapped page there is an access violation that kills the
  process. On POSIX the worker's read-fault recovery hides it but reads junk.
  The guarded copy only covered the record at the moment it was taken.
- **Fix:** the worker reads +0x38 and +0x44 from the guarded `record`
  snapshot. `BuildItemFromMesh` now takes one guarded copy of the mesh header
  (+0x00..+0x37) and reads every field from it, so both the prewarm and the
  capture callers fail cleanly instead of faulting. An unreadable record
  skips the entry through the existing frame-paced retry/drop path, is
  counted in `g_prewarm_unreadable`, and logs
  `prewarm skipped unreadable mesh record` (first 16 only).
- **Tested:** unattended demo-path boots on D3D12 and Vulkan. The
  `gameplay (prewarm: N meshes ...)` counts match a same-machine baseline
  build without the fix (D3D12 3068 vs 3070, Vulkan 3056 vs 3056, 0 dropped,
  no unreadable records), with no new errors. Not yet played in-game.
- **In-game test (2026-10-31):** Played ~100 min on the big rig (D3D12, 4K, MSAA x2) as one build stacking #38+#39+#47+#48 on main, with --skate3_native_render_scene_perf_log=true --skate3_native_render_scene_perf_items=true. User saw no problems. Log: prewarm 0 dropped and 0 decode refusals on every summary, prewarm[miss_retry=0] in all 1,202 samples, no 'guarded guest read failed' or 'DecodeMesh rejected', guest fps min 102 / avg 120, clean exit. Only ~2 area changes; not compared numerically with main; this PR was not tested on its own.

### KI-27: DecodeMesh input validation

- **Status:** fixed in rubensteezy/skate3recomp#39, tested in-game on 2026-10-31 · **Severity:** low · was BT-20
- **Problem:** element offsets aren't checked against the vertex stride (a
  host over-read on a malformed declaration), and several `Map()` results
  aren't checked for null.
- **Cause:** `DecodeMesh` (`src/skate3_native_scene_gpu.cpp`) reads each
  element at `v * stride + offset`. The offsets are 16-bit fields from the
  guest vertex declaration and the stride is a separate byte, so nothing tied
  them together: an element placed past the stride read past the end of the
  host payload copy on the last vertex. The vertex count was also divided by
  the stride before anything checked it, and the cloth snapshot path passed
  payload pointers with no size. Both upload-buffer `Map()` results were
  written through unchecked, as were the staging `Map()`s in the texture
  decode paths (`UploadGeneratedMips`, `EnsureGuestTextureFromWords`, both
  branches of `EnsureGuestCubeTexture`) and in `EnsureFallbackTextures`.
- **Fix:** `ValidateMeshDecodeInput` (`src/native/mesh_decode_validate.h`)
  runs before any payload read. It checks stride > 0, at least one vertex,
  vertex and index counts under a cap (1M / 8M), payload sizes covering
  count × stride and count × 2, a decodable position format, and
  offset + bytes read <= stride for every element `DecodeMesh` reads. It uses
  the bytes actually read, not the format's nominal size, because the
  synthetic cloth-quad item declares a float2 UV as `k_32_32_32_32_FLOAT`.
  A rejected mesh fails the decode like any other failure (nothing is cached,
  the draw-path miss re-requests it) and is counted in
  `g_mesh_decode_rejected`, with a rate-limited warning (8 lines per 10 s).
  `DecodeMesh` now maps both upload buffers up front; on a `Map()` failure it
  unmaps what was mapped, releases both buffers and fails the decode, counted
  in `g_mesh_map_failed`. The texture paths release the texture and staging
  buffer and fail the decode; `EnsureFallbackTextures` sets `g_r.failed` like
  its other failures. The "gameplay (prewarm: ...)" log line reports both
  counters.
- **Tested:** unit check of the validator against the real `DrawItem` with
  crafted bad declarations (element past the stride, garbage offsets and
  counts, short snapshots) plus the real cloth-quad layout. Unattended
  `--skate3_demo_path=true` boots on D3D12 (3067 meshes) and Vulkan (3056),
  inside the usual 3056 to 3070 range, with 0 dropped, 0 layout rejections
  and 0 map failures. Not yet checked in a hands-on session.
- **In-game test (2026-10-31):** Played ~100 min on the big rig (D3D12, 4K, MSAA x2) as one build stacking #38+#39+#47+#48 on main, with --skate3_native_render_scene_perf_log=true --skate3_native_render_scene_perf_items=true. User saw no problems. Log: prewarm 0 dropped and 0 decode refusals on every summary, prewarm[miss_retry=0] in all 1,202 samples, no 'guarded guest read failed' or 'DecodeMesh rejected', guest fps min 102 / avg 120, clean exit. Only ~2 area changes; not compared numerically with main; this PR was not tested on its own.

### KI-65: Prewarm retries lost the visible-miss priority

- **Status:** fixed in rubensteezy/skate3recomp#47, tested in-game on 2026-10-31
- **Tracking:** rubensteezy/skate3recomp#47. Found while fixing KI-26 (#38).
- **Problem:** a mesh the player can see (a draw-path miss) whose first build
  failed waited behind the speculative prewarm backlog and the per-frame
  commit cap, which shows as longer pop-in or white surfaces while streaming.
- **Cause:** the retry branch of `ProcessPrewarmEntry`
  (`src/skate3_native_scene_gpu.cpp`) pushed
  `g_prewarm_retry.push_back({e.mesh, uint16_t(e.retries - 1)})`, a new entry
  with `miss` cleared. `PrewarmCommit` then put every retry into the LIFO
  `g_prewarm_queue`. The retry lost its place in the miss FIFO, and its
  result lost `PrewarmResult::miss`, so the commit no longer let it past the
  32-per-frame gameplay cap.
- **Fix:** the retry copies `e` and decrements its retries, so `miss` (and
  every other field) survives. `PrewarmCommit` sends retries with `miss` set
  back to `g_miss_queue` and the speculative ones back to `g_prewarm_queue`.
  The retry count and backoff are unchanged (that's KI-67). Miss retries
  routed back are counted in `g_prewarm_miss_retries`, shown as
  `/ N miss retries` on the "taking over natively" line and as
  `prewarm[miss_retry=N]` at the end of the perf line
  (`skate3_native_render_scene_perf_log`).
- **Tested:** unattended `--skate3_demo_path=true` boots, cwd = the install
  folder. Prewarm counts match the usual baseline: D3D12 3068 and 3070,
  Vulkan 3056, 0 dropped, 0 miss retries (no natural build failures in the
  demo). With a temporary local-only hook that kept failing every 8th
  speculative mesh (356 dropped, so the draw path missed them) and failed the
  first build of each such miss, 91 (D3D12) and 92 (Vulkan) miss retries went
  back to `g_miss_queue`, workers took them from there with `retries=7`, every
  one built with `res.miss=true`, and the counter then stayed flat. Forced
  speculative retries still went to `g_prewarm_queue`. Not yet checked in a
  hands-on streaming session.
- **In-game test (2026-10-31):** Played ~100 min on the big rig (D3D12, 4K, MSAA x2) as one build stacking #38+#39+#47+#48 on main, with --skate3_native_render_scene_perf_log=true --skate3_native_render_scene_perf_items=true. User saw no problems. Log: prewarm 0 dropped and 0 decode refusals on every summary, prewarm[miss_retry=0] in all 1,202 samples, no 'guarded guest read failed' or 'DecodeMesh rejected', guest fps min 102 / avg 120, clean exit. Only ~2 area changes; not compared numerically with main; this PR was not tested on its own.

### KI-64: Raw guest loads in the capture path and render hooks

- **Status:** fixed in rubensteezy/skate3recomp#48, tested in-game on 2026-10-31
- **Tracking:** rubensteezy/skate3recomp#48. Found while fixing KI-26 (#38).
- **Problem:** the frame build and the capture hooks read guest memory with
  raw `REX_LOAD`s on the guest render thread:
  - `src/skate3_native_scene.cpp`: `BuildItemFromMeshCached` read the mesh's
    IB/VB pointers (+0x30/+0x34) on every cache-hit check, and
    `BuildItemGeometry` read the MeshContext (record, draw count, draw
    list), the record and the draw list entries.
  - `src/skate3_native_render.cpp`: `OnSceneDrawList` and
    `FilterSceneDrawList` read the sort vector and its entries, and the
    auto-exposure hook (`sub_827F0D00`) read `self + 3240` and the ownership
    bytes at `self + 3396/3397`.
- **Cause:** if streaming unmaps one of these between being recorded and
  being read, the load is an access violation that kills the process.
  `GuestReadRecoveryScope` is a no-op on Windows, so nothing caught it. The
  risk was low, because the pointers come from what the game is drawing
  right now. No crash had been reported.
- **Fix:** every one of those reads now goes through `GuestTryCopy` or
  `GuestTryLoadU32`, batched where the fields sit together: one 8-byte copy
  for the cache-hit IB/VB check, one copy of the MeshContext head
  (+0x00..+0x4B), one copy of the whole draw list, and one copy of the
  sort-list segment per hook call. A failed read skips that item (counted in
  `rej[chain]` or `rej[draws]`), skips that sort list's capture, leaves the
  list unfiltered, or skips that frame's exposure pin. Every skip is counted
  by `NoteGuestReadSkip`, which logs `guarded guest read failed at <site>`
  with the running total at most once per 10 s. `BuildItemFromMesh` is left
  to #38 (KI-26), which already reads its mesh header through one guarded
  copy.
- **Cost:** measured with `--skate3_native_render_scene_perf_log=true
  --skate3_native_render_scene_perf_items=true`, 6 unattended demo-path
  runs per build (3 D3D12, 3 Vulkan), against a same-machine build of main.
  The frame-build walk (`wloop`) went from 453 to 462 ns per record (+2%,
  about 8 µs per frame at ~920 records), which is +0.4% of the build
  (1.199 to 1.204 ms) and well under 1% of capture plus build. Guest fps
  (219.7 in both), item cache validate cost (`core` 0.397 µs in both),
  items (588 vs 593) and draws (990 vs 991) didn't change. A microbenchmark
  puts one guarded copy at 3 to 4.5 ns against 1.2 ns for two raw loads.
- **Tested:** the runs above all reached gameplay, logged no guarded-read
  skips and no `rej` counts, and showed the same error and warning lines as
  main. Not yet played in-game.
- **In-game test (2026-10-31):** Played ~100 min on the big rig (D3D12, 4K, MSAA x2) as one build stacking #38+#39+#47+#48 on main, with --skate3_native_render_scene_perf_log=true --skate3_native_render_scene_perf_items=true. User saw no problems. Log: prewarm 0 dropped and 0 decode refusals on every summary, prewarm[miss_retry=0] in all 1,202 samples, no 'guarded guest read failed' or 'DecodeMesh rejected', guest fps min 102 / avg 120, clean exit. Only ~2 area changes; not compared numerically with main; this PR was not tested on its own.

### KI-61: Palette scoring rejects samples at the edges of very wide views

- **Status:** fixed in rubensteezy/skate3recomp#50, tested in-game on 2026-10-31 · **Severity:** low–medium
- **Area:** native renderer, ultrawide, skinned characters
- **Where:** `RefinePaletteBase` (the `score` lambda) and `ScoreRigidAffine`
  in `src/skate3_native_scene.cpp`.
- **Problem:** both accept a sample only if it projects within 1.5x of the
  bank's own clip volume. The banks hold the game's 16:9 matrices, but on a
  wide output the game culls to the wide view, so meshes in the side bands
  reach |x|/w = 1/scale (1.33 at 21:9, 2.0 at 32:9). Confirmed at 32:9, not
  at 21:9:
  - At 32:9 about 2% of all palette scorings were refused only because of the
    x band (they pass with a wider x band). Every one of them was a
    character, and every one resolved to the caller's own guessed base, so
    these were correct palettes being refused, not foreign ones.
  - They all came from the sun-shadow pass (ortho bank, w = 1, pre-pass
    layout), whose projection covers only the 16:9 view, with samples at
    |x|/w 1.5 to 2.03. The main camera pass wasn't affected: its loose
    fallback (`cam_at_c4`, 6x band) already accepted those palettes.
  - A refused shadow-pass capture leaves the item pending, so it waits for
    the main-pass draw or falls back to the previous frame's palette for up
    to 10 frames. No mangled characters were seen in screenshots, so the
    visible effect is at most stale poses or shadows at the side edges.
  - At 21:9 only 4 of 1.38 million scorings were affected.
  - `ScoreRigidAffine` (Ropa garments, in practice the player's tee) was
    never affected in these runs, since the player stays near the centre.
    It gets the same band so a garment at the edge (freecam, cutscenes)
    scores the same way.
- **Fix:** `PaletteScoreHorScale()` returns the Hor+ scale against the
  game's fixed 16:9 aspect (1.0 when the output isn't wide), and both
  functions use an x band of `1.5f / scale`. The y band and the 6x loose
  band are unchanged. At 16:9 the band is exactly 1.5, so nothing changes.
- **Tested:** unattended demo-path boots, signed in on a copy of the user's
  save, D3D12, 1920-wide windows, about 100 s each, with temporary counters
  that ran both bands on the same scorings. Refused scorings, as a share of
  all scorings:

  | Output | Old band | Wider band, same frames | Fixed build (wide band live) |
  | --- | --- | --- | --- |
  | 16:9 (2 runs) | 1.89%, 1.78% | same | n/a (scale 1.0) |
  | 21:9, 2.37 | 2.09% | 2.09% (4 rescued) | 1.72% |
  | 32:9, 3.56 (3 runs) | 3.42%, 4.22%, 3.51% | 1.69%, 1.87%, 1.68% | 2.00%, 1.76% |

  Runs differ by about 0.4 points from scene timing alone (the two 21:9
  runs differ that much with only 4 scorings affected), so compare within
  a column. The wider band never refused a palette the old band accepted.
  In 10 and 12 scorings of two 32:9 runs (of about 1.2 million each) the
  bands picked different homes. The wider band always accepts at least
  what the old one does, so there the old band had refused an earlier
  candidate and fallen through to a later home. No sample with |x|/w over
  1.5 was ever seen at 16:9. Not yet watched in play: needs characters
  near the left and right edges at 32:9.
- **In-game test (2026-10-31):** Played on the big rig as one build stacking #37+#50 on main 7e03084, 32:9 window (--skate3_ultrawide=true --skate3_ultrawide_target_aspect=3.56, 3840x1080 windowed, perf log on): one D3D12 run (~3.5 min gameplay, fps min 102 / avg 120) and one Vulkan run (~3.3 min, fps min 89 / avg 119). User saw nothing wrong at the screen edges on either backend. Both logs exited cleanly with no errors besides the known XMA noise. No log line proves the ultrawide path ran, so the evidence is visual; 21:9 was not run; this PR was not tested on its own.

### KI-25: Vehicle retention frustum test ignores the ultrawide scale

- **Status:** fixed in rubensteezy/skate3recomp#37, tested in-game on 2026-10-31 · **Severity:** low · was BT-18
- **Where:** `BuildFrameScene` and `CornersOutsideFrustum` in
  `src/skate3_native_scene.cpp`
- **Problem:** on ultrawide outputs (`skate3_ultrawide = true`) the game's
  main-world cull is widened to the wide view
  (`Skate3UltrawideGameFrustumPatchScope`), but the vehicle retention test
  checked its "the guest camera can see it" condition against the narrow
  16:9 guest frustum. The static retention test passes `wide_hor_scale`; the
  vehicle test didn't. The review guessed vehicles would drop out of
  retention too early. The effect is the other way round: a vehicle that
  stopped being submitted in the side bands (outside 16:9, inside the wide
  view) counted as view-culled, so a despawned or LOD-swapped car there was
  re-published as a ghost copy for up to 10 frames.
- **Second problem, same code:** the static test applied the scale by
  dividing the whole margin, which widens the top and bottom planes too
  (about 1.27x at 21:9). Statics the game view-culled just above or below
  the screen counted as "really removed" and were dropped, so retention
  stopped covering vertical pans on ultrawide.
- **Fix:** `CornersOutsideFrustum`, `ItemOutsideFrustum` and
  `BoxOutsideFrustum` take the Hor+ scale as its own argument and widen only
  the left and right planes. `wide_hor_scale` is computed before the vehicle
  pass, and both passes share the margins (`kRetainGuestCullMargin`,
  `kRetainDrawMargin`). With a 16:9 output (scale 1.0) nothing changes.
- **Tested:** unattended demo-path boots in a 2560x1080 window with
  `--skate3_ultrawide=true --skate3_ultrawide_target_aspect=2.37` (21:9,
  scale 0.75), signed in on a copy of the user's save, D3D12 and Vulkan, with a
  temporary counter that evaluated the old and new tests on the same frames.
  Traffic was submitted inside the side bands (5,149 live captures), which
  confirms vehicles are culled to the wide view, not 16:9. Old and new vehicle
  decisions agreed on all 602 re-publish candidates (static camera, so no
  pans and no side-band despawns in the sample). The static test kept about
  0.1% more entries and drew 13% (D3D12) and 3% (Vulkan) more retained items,
  all in the top and bottom bands, which is the 16:9 behaviour. Not yet
  watched in play: needs a pan near traffic on an ultrawide output.
- **In-game test (2026-10-31):** Played on the big rig as one build stacking #37+#50 on main 7e03084, 32:9 window (--skate3_ultrawide=true --skate3_ultrawide_target_aspect=3.56, 3840x1080 windowed, perf log on): one D3D12 run (~3.5 min gameplay, fps min 102 / avg 120) and one Vulkan run (~3.3 min, fps min 89 / avg 119). User saw nothing wrong at the screen edges on either backend. Both logs exited cleanly with no errors besides the known XMA noise. The user approved keeping #37's extra top/bottom (vertical plane) static-retention change. No log line proves the ultrawide path ran, so the evidence is visual; 21:9 was not run; this PR was not tested on its own.

### KI-24: XUID parsing and TOML escaping in user settings

- **Status:** fixed in rubensteezy/skate3recomp#36, tested in-game on 2026-10-31 · **Severity:** low · was BT-17
- **Where:** `ParseXuidString` and `TomlString` in
  `src/skate3_user_settings.cpp` (reader and writer of `profiles.toml`)
- **Problem:** `ParseXuidString` tried hex first and only fell back to
  decimal when that failed, so any decimal XUID string of up to 16 digits
  was read as hex (`"12345"` became `0x12345`, a different profile identity
  and a different `saves/<xuid>` folder). Decimal strings of 17 to 20 digits
  happened to work because they overflow as hex. `TomlString` only escaped
  `"` and `\`, so a gamertag with a control character (tab, newline, U+0001)
  was written raw. TOML forbids that, toml++ rejects the whole file, and
  `LoadProfiles` then returns an empty store: every profile is lost and the
  next save replaces the file with a default one.
- **Fix:** `ParseXuidString` reads a `0x` prefix or exactly 16 hex digits as
  hex, an all-digit string of any other length as decimal, and a shorter hex
  string that contains a letter as hex. Anything else (signs, junk, overflow)
  is 0. The one ambiguous case, 16 decimal digits, reads as hex because that
  is what `FormatXuid` (the only writer: 16 zero-padded uppercase hex digits)
  produces, so every file the game wrote reads back the same. A 16-digit
  decimal XUID has to be written as a TOML integer, which `LoadProfiles`
  already accepts. `TomlString` now escapes per the TOML spec: `\b \t \n \f
  \r \" \\`, and `\uXXXX` for the other U+0000-U+001F characters and U+007F.
  The reader is toml++, which decodes all of these.
- **Tests:** a standalone check (compiles `skate3_user_settings.cpp` with a
  stub `rex/cvar.h` against the SDK's toml++) covers the parsing rules,
  100,000 `FormatXuid` round trips, toml++ decoding every byte 0x00-0x7F,
  and a `LoadProfiles` / `SaveProfiles` / `LoadProfiles` round trip of a
  file with decimal, hex, `0x` and integer XUIDs and a control-character
  gamertag, with byte-identical saves. In the relwithdebinfo build, a boot
  with a crafted `profiles.toml` (selected profile `xuid = "12345"`,
  gamertag `"Dec\u0001Tag"`, plus a hex profile with a tab) loaded it and
  the signed-in demo path created its save under `0000000000003039`
  (12345); the old parser would have used `0000000000012345`. A boot with
  no `profiles.toml` wrote the default profile, and the next boot read it
  back unchanged and saved under that XUID.
- **Not tested in the game:** saving from the profile page of the settings
  dialog, which needs a person at the controls. It calls the same
  `SaveProfiles` as the tests.
- **Not covered:** the SDK's own copy of the parser,
  `ParseXuid` in `third_party/rexglue-sdk/src/system/xam/user_profile.cpp`,
  still tries hex first. It only sees `FormatXuid` output from the game, so
  it is only wrong for a decimal `--user_profile_xuid=` passed by hand.
- **In-game test (2026-10-31):** Played on the big rig as one build stacking #36+#45 on main 097e823, with the user's real profiles.toml (backed up first). Changed the gamertag Tom -> Tom- on the Esc overlay profile page and saved: the file was rewritten as valid TOML with the xuid unchanged (B13E07DFF9AB6772), no .tmp or backup left in the folder. After a restart the game showed Tom- and did not rewrite the file. No "Profiles:" log lines, clean exits. Not exercised in-game: a gamertag with a quote or backslash (escaping) and an unparseable file (backup path); those are covered only by the unattended tests in the PR. This PR was not tested on its own.

### KI-59: A malformed profiles.toml silently wiped all profiles

- **Status:** fixed in rubensteezy/skate3recomp#45, tested in-game on 2026-10-31 · **Severity:** low–medium
- **Area:** user settings, profiles
- **Tracking:** rubensteezy/skate3recomp#45. Found while fixing KI-24 (#36).
- **Where:** `LoadProfiles` and `SaveProfiles` in
  `src/skate3_user_settings.cpp`, the new `src/skate3_safe_file.cpp`, and the
  `save_profile` lambda and `OnFinalizePaths` in `src/skate3_app_common.cpp`.
- **Cause:** `LoadProfiles` caught `toml::parse_error`, returned an empty store
  and logged nothing. toml++'s `parse_file` also throws `parse_error` when it
  can't open the file, so a locked or unreadable file looked the same. The
  next profile save then ran `EnsureUsableProfileStore`, which adds a default
  "Player" profile, and `SaveProfiles` truncated the user's file and wrote
  that in its place. Any malformed `profiles.toml` (a hand edit, invalid
  UTF-8 from KI-60, a crash mid-write) lost every profile. The save itself
  truncated in place, so a crash during it could also leave a partial file.
- **Fix:**
  - `LoadProfiles` reads the bytes itself (`safe_file::ReadWholeFile`), so a
    missing file, an unreadable file and a parse error are told apart.
  - Parse error: logs the path, line, column and toml++'s description, then
    moves the file to `profiles.toml.bad-YYYYMMDD-HHMMSS` in the same folder
    (`MoveFileExW` without replace, or `link`+`unlink` on POSIX, so an
    existing backup is never clobbered). If the move fails (the file is open
    without delete sharing), it copies it and checks the copy matches the
    bytes it read. The newest 5 backups are kept. Only then is the store
    treated as empty, and a "PROFILES RESET ... backed up as <path>" error is
    logged. If no verified backup could be made, the store is marked
    `save_blocked`.
  - Read error (locked, permission denied): logged, and the store is marked
    `save_blocked`. The session runs on a temporary default profile and
    `SaveProfiles` refuses to write until a later load can read the file.
  - `SaveProfiles` writes `profiles.toml.tmp` in the same folder, flushes it
    (`FlushFileBuffers` / `fsync`) and replaces the target with
    `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)` /
    `rename`. On failure the target is untouched, the temp file is removed
    and the error is logged. Line endings stay CRLF on Windows, so a file the
    game wrote saves back byte for byte.
  - Entries without a usable id, gamertag and xuid are still dropped, but now
    with a warning.
- **Tests:** a standalone harness (compiles `skate3_safe_file.cpp` and
  `skate3_user_settings.cpp` with stub `rex/cvar.h` and `rex/logging.h`
  against the SDK's toml++) passes: a good file round-trips byte-identical;
  bad TOML and invalid UTF-8 are moved to a backup with the original bytes
  before the default is saved; backups prune to 5; a file held open with no
  sharing is not read as empty and not overwritten; a file held open with
  read sharing only loads, and saves fail cleanly; a malformed file held open
  with read sharing gets a verified copy backup; a read-only file is not
  replaced; and a child process killed 8 times while writing 512 MB through
  `WriteFileAtomic` always left the old file intact. In the relwithdebinfo
  build with `--user_data_root=<scratch>` and the signed-in demo path: a good
  two-profile file loaded (the save was created under the selected profile's
  XUID) and was unchanged after the run. Bad TOML and invalid UTF-8 logged
  the error with line and column, were backed up byte-identical, and then
  the startup save wrote the default profile. A file held open with
  `FileShare.None` logged "cannot read" and was unchanged.
- **Not tested in the game:** saving from the profile page of the settings
  dialog, which needs a person at the controls. It calls the same
  `LoadProfiles` and `SaveProfiles` as the tests.
- **Not covered:** `settings.toml` is written by the SDK
  (`WriteConfigFile` in `third_party/rexglue-sdk/src/core/cvar.cpp`), which
  already writes a temp file and renames it, but doesn't flush it to disk and
  rewrites a malformed file without a backup.
- **In-game test (2026-10-31):** Played on the big rig as one build stacking #36+#45 on main 097e823, with the user's real profiles.toml (backed up first). Changed the gamertag Tom -> Tom- on the Esc overlay profile page and saved: the file was rewritten as valid TOML with the xuid unchanged (B13E07DFF9AB6772), no .tmp or backup left in the folder. After a restart the game showed Tom- and did not rewrite the file. No "Profiles:" log lines, clean exits. Not exercised in-game: a gamertag with a quote or backslash (escaping) and an unparseable file (backup path); those are covered only by the unattended tests in the PR. This PR was not tested on its own.

### KI-80: Profiles with a missing id, gamertag or XUID are lost on the next save

- **Status:** fix in review in rubensteezy/skate3recomp#58, tested unattended /
  awaiting in-game test · **Severity:** low–medium
- **Area:** profiles
- **Tracking:** rubensteezy/skate3recomp#58. Found while fixing KI-59 (#45).
- **Where:** `LoadProfiles` and `SaveProfiles` in
  `src/skate3_user_settings.cpp`, `WriteBackupCopy` in
  `src/skate3_safe_file.cpp`.
- **Cause:** `LoadProfiles` skips `profiles` entries without a usable id,
  gamertag and xuid (a hand-edited XUID that #36's stricter
  `ParseXuidString` reads as 0, a missing field, a non-table entry). The
  next save rewrites the file from the in-memory store, so they were gone
  from disk with only a load-time warning.
- **Fix:** before `SaveProfiles` replaces the file, it reads the current
  file and checks whether the save would drop anything: an entry
  `LoadProfiles` would skip, a `profiles` key that isn't an array, or a file
  that no longer parses. If so, it first writes the old bytes to
  `profiles.toml.dropped-YYYYMMDD-HHMMSS` in the same folder (crash-safe, via
  `WriteFileAtomic`; the newest 5 are kept, separately from the `.bad-`
  backups) and logs "the old file is backed up as <path>". If that backup
  can't be written, the save is refused and the file is left as it was.
  After the first such save the file has no unusable entries, so later saves
  make no more backups. The load warning now says the next save backs the
  file up first.
- **Why a backup and not keeping the entries:** writing an unusable entry
  back unchanged means re-serialising arbitrary TOML (nested tables,
  non-table array items, comments) inside `[[profiles]]`, which the writer
  can't do faithfully. A byte-exact copy of the old file loses nothing and
  reuses the KI-59 backup code; the check runs at save time on the file
  actually being replaced, so repeated `LoadProfiles` calls and repeated
  saves of one store don't create duplicate backups.
- **Tests:** a standalone harness (real `skate3_safe_file.cpp` and
  `skate3_user_settings.cpp`, stub `rex/cvar.h` and `rex/logging.h`, the
  SDK's toml++) passes: a file with one good entry and one entry with
  `xuid = "12-34"` loads 1 profile with the warning; the first save leaves
  one `.dropped-` backup byte-identical to the old file and a new file
  without the bad entry; reloading logs no warning and two more saves make
  no more backups; a clean file and a missing file make no backup;
  `profiles = "oops"` and an entry without a gamertag are backed up; 7
  rounds prune to 5 `.dropped-` backups and leave `.bad-` files alone; with
  every backup name taken the save returns false and the file is unchanged;
  an unparseable file at load still goes to `.bad-` (KI-59) with no extra
  `.dropped-` copy; a file broken after load is backed up before the save.
  Demo-path boot (relwithdebinfo, `--user_data_root=<scratch>`, signed in)
  with a file holding one good profile and one with `xuid = "12-34-56"`:
  logged the "skipped 1 entry ... backs the file up" warning, used the good
  profile (save created under `B13E07DFF9AB6772`), and left the file
  byte-identical. Startup only saves when there is no file, so the
  backup-on-save path itself is covered by the harness only.
- **Not tested in the game:** saving from the profile page of the settings
  dialog with an unusable entry in the file, which needs a person at the
  controls.

### Reviewed and ruled out (code review)

- Ultrawide target aspect persistence: not in the saved settings list.
- `BuildItemFromMesh` initialisation: items are always fresh.
- Photo FX MSAA latch: handled in `RenderScene`.
- Null hair PSOs: checked at the point of use.
