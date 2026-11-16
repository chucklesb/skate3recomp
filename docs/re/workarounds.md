# Guesses to replace

Parts of the fork that **guess** what the game is doing, from the outside,
instead of knowing it. Each is a source of bugs: when a guess is wrong, we
add another guess to cover it. The plan for each is the same: learn the real
answer (reverse engineering, [README.md](README.md)), build that, verify it
A/B, then **delete the guess**. Fallbacks stay off by default
([KI-117 follow-up](../known-issues.md#ki-117-the-skaters-feet-and-body-parts-fly-off-at-speed)).

Status: **open**, **in progress**, **replaced** (the guess is deleted).

## Characters (the native renderer)

The renderer rebuilds every character piece by piece from the GPU constants
the game uploaded for each draw, then smooths each piece on its own clock.
Every row below is a way for one piece to come apart from the body.
**The replacement for all of them:** one skeleton snapshot per character per
tick (`Sk8::UpdateBoneTransforms` already receives the full pose,
`srcPalette`, and the per-part bone remaps; see
[characters.md](characters.md)), every piece built from it, one smoothing
history per character.

**Built and on by default (2026-11-16):** the skeleton store
(`skate3_native_render_scene_skeleton = on`) serves every skater piece from
one pose, exact against the game's upload, and resets each character's
smoothing together. Rows marked "in progress" are bypassed for skaters
under `on`. `on` is the default since RE-5 step 4; deleting them for skaters is step 5.
Pedestrians still use them (RE-5b).

| Guess | Where | What it guesses | Status |
|---|---|---|---|
| Palette capture from the constant bank | `CaptureSkinnedState`, `BankPaletteBase` (`src/skate3_native_scene.cpp`) | Which constant registers hold this piece's bones, from row plausibility | open |
| Palette validation | `RefinePaletteBase` | Whether a palette belongs to this mesh: samples must project on screen and keep a spread between 10-20% and 3× the bind size. Rejected the folded forearm until 2026-11-15 | open |
| Content-matched serve | `ServeAuthoritativePalettesImpl` (`src/native/skate3_native_palette.cpp`) | Which pack belongs to which player piece: same bone count, within 1.5 m of last frame's position, plus a hold-or-advance hysteresis | **replaced** for skaters (RE-5 step 5): the UpdateBoneTransforms snapshots, their content match and the 3x4/4x4 layout vote are deleted; the store builds those pieces. The pack serve by ctx containment and its hysteresis remain for pedestrians and other packed instances (RE-5b) |
| Pending fix-up | `OnDrawDone` (post-draw capture) | That a later draw with the same buffers is this piece | open |
| Rescue and re-anchor | `BuildFrameScene` rescue block (`dbg_src` 3/13) | That a piece's pose from up to 10 frames ago, moved with the owner, is close enough | open |
| Incoherent-palette heal | `dbg_src` 6 | Same, at publish time | open |
| LW gap fill | `dbg_src` 9 | A pedestrian's last pose for up to 2 frames | open |
| Rescue serve, continuity (dougieprishpreed) | cvars `rescue_serve`, `smooth_continuity` | Symptom fixes; **off by default** | open (comparison only) |
| Per-piece smoothing | `InterpolateDynamicItems` | That each piece's pose history lines up in time with the rest of the body | in progress (skaters under `on`: ctx-keyed rings, one reset per character) |
| Grouping a character | char_sync (diagnostic) | Which pieces form one character: LW entity, then pack owner, then memory proximity | open |
| Cloth (ropa) state | `g_ropa_state_cache`, the `flag_x` test, the dropped-garment gate | Whether a garment's cloth sim is active and which layout its bank uses. Right on the first frame after a replay seek too (KI-125 is a buffer-read timing race, not this test) | open |
| Rigid clone pairing | `g_rigid_world_cache`, nearest-world matching | Which clone of a shared mesh a draw belongs to | open |

## The game's own renderer still running

Listed in [native-only.md](../native-only.md) (rows A-F): windows that let
the emulated renderer draw so its output can be read back. Replaced phase by
phase there; the windows are deleted in phase 5.

## Adding to this page

When a fix adds a heuristic, threshold or fallback that infers game state
from the outside, add a row. When an RE-backed replacement lands and the
guess is deleted, mark it **replaced** with the commit.
