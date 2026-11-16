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
  read (Owned and Killed lists), and the save layout is known. Left:
  setting challenge completion, `+0x10`.
- RE-5 [ ] **The character skeleton** ([re/characters.md](re/characters.md)):
  one pose per character per tick; the likely fix for
  [KI-125](known-issues.md#ki-125-the-skater-comes-apart-for-a-frame-or-two-after-a-replay-jumps).
- RE-6 [ ] **Who issues the emulated passes** native-only must replace
  (pitches 160 and 1200).
- RE-7 [ ] **`src/re/` shadow-mode replacements.**

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
