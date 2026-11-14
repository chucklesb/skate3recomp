# Roadmap

Where this fork is going, in order. The smaller items, in no particular
order, are in [open-work.md](open-work.md); bugs are in
[known-issues.md](known-issues.md).

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
