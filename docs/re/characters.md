# Characters: skeleton, parts, palettes

What we know about how Skate 3 poses and draws characters (the player,
pedestrians, NPC skaters), and the redesign it points to. Functions:
[symbols.toml](symbols.toml).

## How the game does it

- **A character is one skeleton and several parts.** Each part is a mesh
  (torso, forearms and hands, shoes, head, hair, shirt cloth...) skinned to
  a subset of the skeleton's bones.
- **Each tick**, the animation jobs produce the skeleton's pose, then the
  bone transforms are turned into one **palette per part**: the part's bones,
  multiplied into world space, packed as 3x4 rows for the vertex shader.
- **The player skater:**
  - `Sk8::UpdateBoneTransforms(parts, count, srcPalette, remaps, out)`
    (`sub_827A52C8`) builds every part's palette at once: `parts` is an
    array of `cModelInstance` part records (stride `0x28`), `srcPalette` is
    the whole pose, and `remaps` maps each part's bones to skeleton bones.
  - It runs inside `Sk8::SkaterPresEntity::StartJobs` (`sub_827825B0`);
    `EndJobs` (`sub_82782818`) packs the final locomotion pose.
  - **The parts array is per-frame scratch:** its fields read zero by the end
    of the frame, so the hook exit is the only moment it can be read.
- **Other characters** pack per instance with
  `cModelInstance::PackAndMultiplyMatricesForUpload` (`sub_827E5B30`).
  LOD pedestrians use a fused writer (`sub_827C1D38`) that concatenates
  every mesh's palette into one buffer.

## Structures (known offsets)

| Structure | Offset | Field |
|---|---|---|
| `cModelInstance` | `0x00` | model |
| | `0x04` | `m_arrMeshContext` begin |
| | `0x08` | `m_arrMeshContext` end (not always set in UBT part records) |
| | `0x14` | `m_matrices`: the packed palette, 48-byte 3x4 rows |
| | `0x18` | packed bone count (the highest blend index the mesh uses) |
| | size `0x28` | (the stride of a UBT part array) |
| MeshContext | size `0x50` | one per drawn mesh (part) of an instance |
| | `0x04` | effect list (pass count at `+0x44`) |
| | `0x48` | draw list |
| LivingWorld entity | `+528.x` | the entity's opacity, served as shader output alpha |

Sources: `src/native/skate3_native_palette.cpp`, `skate3_native_lw.cpp`,
`skate3_native_entity.cpp`, `skate3_native_scene.cpp`.

## What the native renderer does today (the guesses)

It never sees the skeleton. Per piece it takes the palette from the draw's
constant bank or from a pack snapshot matched by content, and it validates,
rescues and smooths each piece on its own (the "Characters" rows of
[workarounds.md](workarounds.md)). Each guess is a way for a piece to come
apart from the body: KI-117 and its 2026-11-15 forearm follow-up were two of
them.

## The redesign (not started)

1. **Capture the pose where the game builds it:** at the
   `UpdateBoneTransforms` exit, read `srcPalette` and each part's remap.
   That is one skeleton per character per tick, all from the same moment.
2. **Link each part record to its draw item.** The missing piece: live part
   records reference per-frame copies of their MeshContext, not the
   persistent one our draw items carry. Candidates: the part's mesh / vertex
   buffer, its bone count, and the owning entity
   (`SkaterPresEntity::StartJobs` brackets the call).
3. **Build every piece's palette from that one pose,** and smooth one history
   per character instead of one per piece.
4. **Verify in shadow mode** against today's per-piece palettes (reel
   scenarios, char_sync), then delete the per-piece guesses.
5. **Then** pedestrians and NPC skaters, through the pack and LW paths.
