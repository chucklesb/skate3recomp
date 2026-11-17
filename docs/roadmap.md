# Roadmap

Where this fork is going, in order. The smaller items, in no particular
order, are in [open-work.md](open-work.md); bugs are in
[known-issues.md](known-issues.md).

## Status (updated 2026-11-15)

The tracker: what's being worked on, in priority order, and what's done.
Update it with every commit that finishes or starts an item. Details live in
the linked pages; the backlog is [open-work.md](open-work.md).

**Now** (finishing milestone 1, phase 0):
1. [x] **Branches:** `native-only` here and in the SDK fork (`native-only`, from
   `linux-aarch64-cross` 062fb8d and SDK `vulkan-debug-labels` 4ef36cf).

**Next:**
2. [ ] **Native-only phase 1** (offscreen 2D compose), then phases 2-5.
   Row D, the offscreen skater pass in challenges (72-96 emulated draws a
   frame), is the biggest remaining cost; it's phase 4.

**Reverse engineering** (a primary goal; plan approved 2026-11-15, [re/README.md](re/README.md)):
- RE-1 [x] **Toolkit:** `mem` search (unknown values included), portable
  snapshots, names in symbolize, the Toolkit section.
- RE-2 [~] **Names from the Skate 2 debug build:** 9,326 functions (19.5%)
  from `sk2_match.py` plus Ghidra Version Tracking, cross-checked; class
  layouts with `sk2_types.py` and [re/types.md](re/types.md) (four
  structures, every offset we use confirmed). Left: more coverage via BSim,
  more layouts as the work reaches them.
- RE-3 [~] **Skate 3's own code:** 856 functions named by the Lua bindings and
  the message table published ([re/README.md](re/README.md)); they also
  showed layout-align names to be unreliable (no longer exported). Left:
  debug strings; handlers for the messages.
- RE-4 [~] **Career pilot:** board sales understood (four counters, summed;
  [re/career.md](re/career.md)), the harness `career` step sets them, and the
  "new team member" prompt is measured (row A). The milestone screens now
  show without a challenge (`career` `notify` + `show`,
  `career_milestone.toml`), and every FE screen id has its name
  ([re/fe_screens.tsv](re/fe_screens.tsv)). The counters are Owned /
  Killed / online board sales (the challenge payout), challenge progress is
  read (Owned and Killed lists), and the save layout is known. Challenges can be
  completed from the harness (`complete = "name"`; names from the game's
  lookup8 hash; every data key named from the game's vaults,
  [re/attrib_names.tsv](re/attrib_names.tsv)). With `pay = true`
  the game's own completion pays the reward and shows the results; the
  `results` step walks them and waits for the save. The `.big` archives
  are readable (`tools/re/big.py`). All four counters are
  identified (`+0x10`: board sales the online service awarded).
- RE-5 [~] **The character skeleton** ([re/characters.md](re/characters.md)):
  one pose per character per tick; the likely fix for
  [KI-125](known-issues.md#ki-125-the-skater-comes-apart-for-a-frame-or-two-after-a-replay-jumps).
  Done: UBT is a pure gather from one pose array (`srcPalette`), and UBT
  part `k` is the entity's LOD instance `k` (the part ↔ draw link,
  checked over a reel). The pack is `transpose(gather × m_MatLtoW)`, exact.
  Steps 1-2 (the store, shadow mode): every skater piece exact against the
  game's upload on Windows and the Odin 2. Step 3 (serve, one reset per
  character), verified on both (char_sync clean). Step 4: stills, fps and
  char_sync show no regression on either platform; **`on` is the default**
  (2026-11-16).
  Step 5: the player's content-matched serve is deleted; the other guesses
  only decide which skater pieces are drawn now, and stay for pedestrians.
  RE-5b (pedestrians): the LivingWorld batch writer is the same gather and
  pack fused; the store rebuilds every pedestrian piece exactly (shadow,
  both platforms) and serves them (`skeleton_lw = on`, the default since
  2026-11-16; no frame-time cost on either platform).
  Next: the remaining char_sync flags at busy spots (visible gap fills,
  mode switches), then delete the remaining per-piece guesses
  ([re/workarounds.md](re/workarounds.md)).
  KI-125 turned out to be the cloth, not the skeleton: a buffer-read timing
  race after a seek, documented, fix deferred.
- RE-6 [ ] **Who issues the emulated passes** native-only must replace
  (pitches 160 and 1200).
- RE-7 [~] **Shadow-mode replacements:** native code in place of
  recompiled functions, verified in shadow mode first. Catalog of what is
  specified vs replaced: [re/replacements.md](re/replacements.md); replace
  trivial specified functions at once, cycles or not. Done (2026-11-16, on
  by default): `UpdateBoneTransforms`, `PackAndMultiplyMatricesForUpload`
  and the LivingWorld batch pack writer, bit-exact on both platforms, no
  frame-time cost (`src/native/skate3_native_pose.cpp`,
  [re/characters.md](re/characters.md#native-replacements-re-7-candidates)).
  Profiler (2026-11-16): `profile` verb + `profile_report.py`; first
  ranking in [re/hot-functions.md](re/hot-functions.md): at the 30 fps cap
  the Odin 2 uses ~9.7 ms of CPU per frame over all game threads, flat (no
  function over ~0.5 ms), so replacements are for understanding and
  correctness more than frame time (and CPU, i.e. battery and heat).
  Replacement framework (2026-11-16): `native_replace::Run` +
  `replace_report.py`; the pose functions run on it. Character path:
  BindConstants specified and its register map measured (the lighting rows
  come from entity fields through a pointer, [re/characters.md](re/characters.md)).
  Done and on by default: `char_rows` rebuilt from the entity for pieces
  whose capture fails (no stale caches; 96.8% bit-exact in shadow, the rest
  pedestrian fade opacity), and the D3D fence wait blocks instead of
  spinning (Odin 2 uncapped: render thread -20% CPU, better 1% lows;
  [re/hot-functions.md](re/hot-functions.md)). The job system (2026-11-16):
  the workers don't spin, they wake for nothing (two thirds of wakes find no
  job); `WakeThreads` now signals only workers that may be waiting
  (`skate3_native_job_wake`, on by default: job threads -6% CPU on the
  Odin 2 at 30 fps, no lost wakes); the worker loop, WakeThreads, the
  waits and the spin-wait run as native bodies (the framework's Body
  families). Culling: `DoFrustumCulling` + its job wrappers native and
  bit-exact ([re/culling.md](re/culling.md); job threads -5% CPU on the
  Odin 2). Later the same day (Windows; the Odin 2 check skipped by
  choice): `sub_827816C0` turned out to be a 160-byte row copy
  (`lwrows`, bit-exact), and `WaitForSingleObjectEx` with its timeout
  and last-error helpers run as native bodies. Next: the vrsqrtefp helper
  (SDK), RunSingleJob's slot layout, the BindConstants bodies
  ([re/replacements.md](re/replacements.md)).
- RE-8 [~] **Boot flow** ([re/boot.md](re/boot.md)): the handler chain from
  launch to the title is mapped (language select, logo, the EA / Black Box
  FMV, a load wait, the title). No screen is frame-dependent (all time
  `dt` in ms; same seconds at 30, 60 and ~500 fps). None blocks the main
  loop, but the boot load only starts when the FMV plays, so the 10 s
  language timeout and the 4 s logo delay it. Done (2026-11-16, Windows):
  any button skips the logo and the FMV during the boot
  (`skate3_intro_movie_skip`, "Skippable Intros" in the options menu;
  title at 6.2 s instead of 25.8 s); the logo's update is native
  (`boot_logo`). Next: a native handler chain that starts the load at
  once, and the movie handler's side effects (requirements in boot.md).

**Parked** (the user's call; don't start without asking):
- The photo challenge's jerky closing animation and snapping pose
  ([open-work.md](open-work.md)).
- Pedestrian pieces flagged by char_sync outside reels (spawns differ each
  run; likely nothing).
- One intermittent boot crash in `rexruntime.dll` on Windows (2026-11-15,
  passed on retry, no symbols); watch for a repeat.

**Done recently:**
- 2026-11-15: **Row A measured** (the "new team member" prompt, 36.4 draws a
  frame) through RE-4: board sales decoded and settable (`career` step).
- 2026-11-15: **RE-1 toolkit** and **RE-2 first pass** (8,020 Skate 3
  functions named from the Skate 2 PDB; [re/README.md](re/README.md)).
- 2026-11-15: **photo-challenge reference shots** (live on the Odin 2;
  [native-only.md](native-only.md)); `live_shots.py` shoots a live session
  automatically.
- 2026-11-15: **the Odin 2 tour**: audit and fps per screen recorded in
  [native-only.md](native-only.md) (30 fps everywhere; Import Skater's photo
  pass runs 23 draws a frame on the device, 4 on Windows).
- 2026-11-15: **frame dumps** for videos: `play_reel video = true` and the
  `video` verb write every rendered frame (the presenter's frame tap), the
  reel clock stepping 1/30 s per frame; `video_reel_forearm.toml`.
- 2026-11-15: **v2.1.0-aarch64.4** released. The forearm that froze,
  vanished and floated (KI-117 follow-up): root cause fixed, fallbacks off by
  default, the char_sync check, `repro_reel_forearm.toml`.
- 2026-11-15: phase 0 audit tools, the tour scenarios, the Windows baseline,
  and the photo-challenge census on the Odin 2.
- 2026-11-15: reverse-engineering workstream set up in this repo
  ([re/README.md](re/README.md)).

## Reverse engineering (continuous)

Alongside every milestone: what we learn about the game's code while
debugging goes into [re/](re/README.md) (the function map, per-system
pages), and every place the fork guesses at game state is listed in
[re/workarounds.md](re/workarounds.md) until an RE-backed replacement
deletes it. Decided 2026-11-15: in this repo, not a separate fork. The game
code is the same in every fork, so a clean host layer comes from replacing
the guesses, not from a fresh start.

## Direction (decided 2026-11-13)

- **This fork is the main line.** Upstream
  ([rubensteezy/skate3recomp](https://github.com/rubensteezy/skate3recomp),
  and mchughalex before it) has gone quiet. We don't keep changes ready for
  upstream pull requests, and the SDK stays on our fork
  ([chucklesb/rexglue-skate3](https://github.com/chucklesb/rexglue-skate3)).
- **The Odin 2 (DroidDeck, linux-aarch64) is the target we optimize and
  release for.** The fork still builds for Windows, Linux and macOS.
- **Vulkan everywhere.** Every platform has it (macOS through MoltenVK), so
  the native Vulkan renderer is the only renderer we keep.

## Milestones

1. **Native-only rendering**
   ([native-only.md](native-only.md), phases 0-5). The game's own renderer
   stops drawing anything in default play, and the windows and overrides
   that let it run are deleted. Before phase 1: a testing pre-release of the
   current state and a native tour scenario (see "Before we start" there).
2. **Shed the baggage** ([native-only.md](native-only.md), phase 6). Remove
   the D3D12 backend and the emulated renderer, once milestone 1 has held up
   in play.
3. **Builds for other platforms.** Not decided. Releases are linux-aarch64
   only today; Windows builds are for A/B tests. Decide after milestone 2
   whether to publish Windows, Linux x64 or macOS builds.

## Alongside

Not ordered against the milestones; see [open-work.md](open-work.md):
- the open known issues;
- the UI and overlay layout;
- the in-game debug menu;
- harness and tooling work.
