# Roadmap

Where this fork is going, in order. The smaller items, in no particular
order, are in [open-work.md](open-work.md); bugs are in
[known-issues.md](known-issues.md).

## Status (updated 2026-11-17)

The tracker: what's being worked on, in priority order, and what's done.
Update it with every commit that finishes or starts an item. Details live in
the linked pages; the backlog is [open-work.md](open-work.md).

**Now** (2026-11-17):
0. [~] **RE: our own implementations, class by class** (the user's
   direction, 2026-11-17). Every native replacement is reverse-engineered
   code over named structs, never rexglue's statements copied
   ([re/replacements.md](re/replacements.md), "The standard"). Done: the
   generated "trivial functions" layer removed; FE, boot, jobs, skinning
   and culling rewritten over typed structs (shadow mode bit-exact on both
   platforms); the FE base class `FrontEndStateBase` named from the Skate 2
   PDB and its screen life native; a call-count build to rank what the
   game calls most (`tools/re/leaves.py rank`). Next: RE-9 classes
   (`FrontEndState_Options`, then the FE manager).
1. [~] **Faster test runs:** the boot-screen scenarios no longer idle
   (158 s -> 58 s); world loads run at frame pace, so Fast Loading lifts
   the cap while loading (teleport 15.4 -> 7.4 s, replay load 13.2 -> 5.7 s
   at 30 fps); `step_times.py` shows where run time goes. Windows, before
   -> after: boot_to_plaza 45.7 -> 30.1 s, reel_regression 187 -> 94 s,
   career_complete_challenge 33 -> 25 s, native_tour 293 -> 261 s. Next:
   an "input accepted" signal from the FE for the menu settles (RE-9).
2. [x] **Codegen re-runs only for codegen changes** (2026-11-17): a UI-only
   SDK edit no longer reruns the ~2.5 min analysis (10 s).
3. [x] **Branches:** `native-only` here and in the SDK fork (`native-only`, from
   `linux-aarch64-cross` 062fb8d and SDK `vulkan-debug-labels` 4ef36cf).

**Next:**
4. [ ] **Native-only phase 1** (offscreen 2D compose), then phases 2-5.
   Row D, the offscreen skater pass in challenges (72-96 emulated draws a
   frame), is the biggest remaining cost; it's phase 4.

**Reverse engineering** ([plan.md](plan.md) G2; every item defined in [re/README.md](re/README.md#the-re-items)):
- RE-1 [x] **Toolkit:** done (extended as needed: `leaves.py`, call counts, 2026-11-17). Next: -.
- RE-2 [~] **Names from the Skate 2 debug build:** in progress: 9,326 functions (19.5%); layouts as the work reaches them. Next: BSim for more coverage.
- RE-3 [~] **Skate 3's own names:** in progress: 856 functions from the bindings, the message table. Next: debug strings; the message handlers.
- RE-4 [~] **Career and progression:** mostly done: board sales, challenges, milestones, the save; completion through the game. Next: what [re/career.md](re/career.md) lists as not known yet, as fixes reach it.
- RE-5 [~] **The character skeleton:** in progress: the store serves skaters and pedestrians, on by default. Next: KI-125 (deferred by the user); delete the remaining guesses.
- RE-6 [ ] **Who issues the emulated passes:** not started. Next: with native-only phase 1.
- RE-7 [~] **Native replacements (G2):** in progress: about 60 functions native (skinning, culling, jobs, the FE base class, boot pieces); coverage in [re/coverage.md](re/coverage.md). Next: the code each fix touches; the hot direct functions ([re/hot-functions.md](re/hot-functions.md#call-counts)).
- RE-8 [~] **Boot flow:** mapped; skippable intros, the quick title and the logo update native. Next: a native handler chain; the movie handler's side effects (requirements in [re/boot.md](re/boot.md)).
- RE-9 [~] **Frontend and HUD:** in progress: `FrontEndStateBase` and 23 small state methods native. Next: `FrontEndState_Options`, then the FE manager.

**Parked** (the user's call; don't start without asking):
- The photo challenge's jerky closing animation and snapping pose
  ([open-work.md](open-work.md)).
- Pedestrian pieces flagged by char_sync outside reels (spawns differ each
  run; likely nothing).
- One intermittent boot crash in `rexruntime.dll` on Windows (2026-11-15,
  passed on retry, no symbols); watch for a repeat.

**Done recently:**
- 2026-11-17 (later): **the replacement standard** (reverse-engineered
  code over named structs; the generated trivial-function layer removed),
  FE / jobs / skinning / culling rewritten to it, `FrontEndStateBase`
  native, the call-count build and `leaves.py rank`.
- 2026-11-17: **testing builds** (`relwithdebinfo`: harness runs, testing
  pre-releases) compile at -O3 like releases, and the harness defaults to
  them; **controller free-fly** (sticks, triggers, Back exits); harness
  `--display windowed|headless|fullscreen` (minimized by default, SDK
  `window_minimized`); boot-screen scenarios 158 s -> 58 s; RE-9 written up.
- 2026-11-16: **the slow boot at a frame cap fixed** (SDK auto-reset event
  hand-off; Odin 2 language select 1.3 s instead of up to 77 s), the pad
  ring native; RE-8 boot flow, skippable intros and the quick title;
  the skeleton store on by default (RE-5) and the first RE-7 replacements;
  v2.1.0-testing.8.
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
