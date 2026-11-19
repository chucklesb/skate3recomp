# Unreleased

What has landed since the last release, newest first. The next release's
notes (`vX.Y.Z-...md`) are written from this; this list is then cleared.
Moved from the roadmap's "Done recently" on 2026-11-18.

- 2026-11-18: **the project skills committed** (`.claude/skills/`,
  host-neutral: settings in `.claude/local.env`, see
  [dev-setup.md](../dev-setup.md)); the harness runs from a Claude Code
  session inside WSL with no wrapper (`tools/harness/rs.sh`, the `device`
  target); `tools/harness/adb`, an `adb` for WSL hosts that runs Windows'
  `adb.exe` with local paths converted.
- 2026-11-18: **uncapped perf runs:** flags after `--` win over a scenario's
  `[cvars]` (an uncapped A/B through `perf_reel` had stayed at 30 fps),
  `--uncapped` on `device`/`windows`/`ab`/`live`, and the summary names the
  cap a run was paced to (`result.json` `fps_cap`).
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
  functions named from the Skate 2 PDB; [re/README.md](../re/README.md)).
- 2026-11-15: **photo-challenge reference shots** (live on the Odin 2;
  [native-only.md](../native-only.md)); `live_shots.py` shoots a live session
  automatically.
- 2026-11-15: **the Odin 2 tour**: audit and fps per screen recorded in
  [native-only.md](../native-only.md) (30 fps everywhere; Import Skater's photo
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
  ([re/README.md](../re/README.md)).
