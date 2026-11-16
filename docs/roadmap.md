# Roadmap

Where this fork is going, in order. The smaller items, in no particular
order, are in [open-work.md](open-work.md); bugs are in
[known-issues.md](known-issues.md).

## Status (updated 2026-11-15)

The tracker: what's being worked on, in priority order, and what's done.
Update it with every commit that finishes or starts an item. Details live in
the linked pages; the backlog is [open-work.md](open-work.md).

**Now** (finishing milestone 1, phase 0):
1. [ ] **The tour on the Odin 2:** `native_tour` and `native_tour_boot`, the
   audit and fps per screen, recorded in [native-only.md](native-only.md).
2. [ ] **Photo-challenge reference shots:** a short live session (the user
   plays, screenshots by command) for the running challenge and the
   shot-choosing screen. *Needs the user.*
3. [ ] **The career "new team member" prompt (row A):** a save placed just
   before it, or the user plays to it in a live session.
4. [ ] **Branches:** `native-only` here and in the SDK fork.

**Next:**
5. [ ] **Native-only phase 1** (offscreen 2D compose), then phases 2-5.
   Row D, the offscreen skater pass in challenges (72-96 emulated draws a
   frame), is the biggest remaining cost; it's phase 4.
6. [ ] **Reverse engineering: the character skeleton**
   ([re/characters.md](re/characters.md)): link `UpdateBoneTransforms`'
   part records to draw items, then one pose per character per tick. It
   replaces most of the "Characters" guesses in
   [re/workarounds.md](re/workarounds.md), and is the likely fix for
   [KI-125](known-issues.md#ki-125-the-skater-comes-apart-for-a-frame-or-two-after-a-replay-jumps).

**Parked** (the user's call; don't start without asking):
- The photo challenge's jerky closing animation and snapping pose
  ([open-work.md](open-work.md)).
- Pedestrian pieces flagged by char_sync outside reels (spawns differ each
  run; likely nothing).
- One intermittent boot crash in `rexruntime.dll` on Windows (2026-11-15,
  passed on retry, no symbols); watch for a repeat.

**Done recently:**
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
