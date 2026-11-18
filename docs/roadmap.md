# Roadmap

The tracker: what's being worked on now, in priority order, and what's next.
**The only ordered list in the docs.** Update it with every commit that
starts or finishes an item; keep each line short and put the details on the
linked page. What the work is for, and what "done" means, is in
[plan.md](plan.md); how each piece is done is in [workflow.md](workflow.md);
the unordered backlog is [open-work.md](open-work.md); bugs are in
[known-issues.md](known-issues.md); what has landed is in
[release-notes/unreleased.md](release-notes/unreleased.md).

## Status (updated 2026-11-18)

**Now:**
1. [~] **RE-9, the FE states class by class** ([re/ui.md](re/ui.md)):
   `FrontEndStateBase` done; next `FrontEndState_Options`' own methods,
   then the FE manager. First real use of the workflow.
2. [~] **Faster test runs** ([open-work.md](open-work.md), tooling):
   condition waits instead of fixed settles; `census_menus` should wait for
   `fe_ready` before its menu presses.
3. [~] **Skills review** ([plan.md](plan.md)): audit the project skills for
   this PC's paths and the Odin 2/root assumptions, generalize them to
   DroidDeck devices, then commit them.

**Next:**
4. [ ] **A scenario that reaches `FrontEndStateBase::Show` and the
   destructor** (native, not yet run by any scenario).
5. [ ] **Frame-time follow-ups** ([open-work.md](open-work.md)), the Odin 2
   texture re-decode hitch first.
6. [ ] **Native-only phase 1** ([native-only.md](native-only.md)), then
   phases 2-5. Row D (the offscreen skater pass in challenges) is the
   biggest remaining cost; it's phase 4.
7. [ ] **RE-7 speed candidates:** the hot direct functions from the call
   counts ([re/hot-functions.md](re/hot-functions.md#call-counts)).

**Reverse engineering items** (one line each; each is defined, with its "done when", in [re/README.md](re/README.md#the-re-items)):
- RE-1 [x] **Toolkit:** done (extended as needed: `leaves.py`, call counts, 2026-11-17). Next: -.
- RE-2 [~] **Names from the Skate 2 debug build:** in progress: 9,326 functions (19.5%); layouts as the work reaches them. Next: BSim for more coverage.
- RE-3 [~] **Skate 3's own names:** in progress: 856 functions from the bindings, the message table. Next: debug strings; the message handlers.
- RE-4 [~] **Career and progression:** mostly done: board sales, challenges, milestones, the save; completion through the game. Next: what [re/career.md](re/career.md) lists as not known yet, as fixes reach it.
- RE-5 [~] **The character skeleton:** in progress: the store serves skaters and pedestrians, on by default. Next: KI-125 (deferred by the user); delete the remaining guesses.
- RE-6 [ ] **Who issues the emulated passes:** not started. Next: with native-only phase 1.
- RE-7 [~] **Native replacements (G2):** in progress: 54 functions native (0.1%) (skinning, culling, jobs, the FE base class, boot pieces); coverage in [re/coverage.md](re/coverage.md). Next: the code each fix touches; the hot direct functions ([re/hot-functions.md](re/hot-functions.md#call-counts)).
- RE-8 [~] **Boot flow:** mapped; skippable intros, the quick title and the logo update native. Next: a native handler chain; the movie handler's side effects (requirements in [re/boot.md](re/boot.md)).
- RE-9 [~] **Frontend and HUD:** in progress: `FrontEndStateBase` and 23 small state methods native. Next: `FrontEndState_Options`, then the FE manager.

**Parked** (the user's call; don't start without asking):
- The photo challenge's jerky closing animation and snapping pose
  ([open-work.md](open-work.md)).
- Pedestrian pieces flagged by char_sync outside reels (spawns differ each
  run; likely nothing).
- One intermittent boot crash in `rexruntime.dll` on Windows (2026-11-15,
  passed on retry, no symbols); watch for a repeat.

**Done recently** (the full list: [release-notes/unreleased.md](release-notes/unreleased.md)):
- 2026-11-18: [plan.md](plan.md) (goals G1-G3, phases P1-P5), the RE hub,
  [workflow.md](workflow.md) with the pre-commit doc check, the coverage
  report ([re/coverage.md](re/coverage.md)).
- 2026-11-17: the replacement standard; FE, jobs, skinning and culling
  rewritten to it; `FrontEndStateBase` native; the call-count build.

## Direction (decided 2026-11-13)

Goals, phases and the target platform are in [plan.md](plan.md); this is
the background.

- **This fork is the main line.** Upstream
  ([rubensteezy/skate3recomp](https://github.com/rubensteezy/skate3recomp),
  and mchughalex before it) has gone quiet. We don't keep changes ready for
  upstream pull requests, and the SDK stays on our fork
  ([chucklesb/rexglue-skate3](https://github.com/chucklesb/rexglue-skate3)).
- **Android devices running DroidDeck (linux-aarch64) are the target; the
  Odin 2 is the reference device** we optimize, test and release for first
  ([plan.md](plan.md), Target platform). The fork still builds for Windows,
  Linux and macOS.
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
