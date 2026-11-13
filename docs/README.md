# Fork documentation

Documentation for what this fork adds on top of upstream
([rubensteezy/skate3recomp](https://github.com/rubensteezy/skate3recomp)).
The top-level [README](../README.md), [KNOWN_ISSUES.md](../KNOWN_ISSUES.md) and
[CONTRIBUTING.md](../CONTRIBUTING.md) are upstream's and are kept as close to
upstream as possible.

| Page | What it covers |
|---|---|
| [known-issues.md](known-issues.md) | Issues found in this fork, each starting with what you see on screen |
| [open-work.md](open-work.md) | What's planned or waiting: open issues at a glance, rendering, harness and tooling, investigations |
| [linux-aarch64.md](linux-aarch64.md) | Cross-compiling for linux-aarch64, the DroidDeck AppImage, releases and testing pre-releases |
| [windows-cross-build.md](windows-cross-build.md) | Building the Windows x64 version from Linux/WSL (for testing and A/B comparisons) |
| [scenario-harness.md](scenario-harness.md) | The scenario test harness: scenario format, verbs, live mode, run folders, A/B runs, replay playback, helper tools |
| [teleport-locations.md](teleport-locations.md) | Vetted teleport locations for scenarios |

## Differences from upstream that players notice

- **Enhanced Shadows and PCSS Shadows are back** in Options > Graphics and
  Settings > Video, off by default. Upstream removed them because they added
  odd extra shadows. On DroidDeck's Turnip driver they look like ordinary hard
  or softened shadows, but cost about 5-6 ms of GPU time per frame on an Odin
  2. Enhanced Shadows' sun was also corrected to match the game's baked
  lighting ([KI-119](known-issues.md#ki-119-enhanced-shadows-put-sunny-places-in-shade)).
  The static shadow map is capped at 16384 pixels wide on every backend,
  which Adreno GPUs require.
  (The upstream README still says these options were removed.)
