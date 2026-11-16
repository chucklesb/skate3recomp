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
  - **What it computes (RE-5, from the disassembly; Ghidra can't decompile
    its VMX128 loop):** nothing but a gather. For each part `p`
    (stride `0x28`) with `remap = remaps[p]` (a u32 array) and
    `out = p->m_matrices`: if `p->m_model->m_iNumBones` (`+0x34`) is at most
    80, for each mesh `m` of the model (`m_pMeshTable` at `+0x24`, 8-byte
    entries, count `+0x32`) and each `i` below `m+0x48`:
    `out[i] = srcPalette[remap[u16 (m+0x4C)[i]]]`, a whole 64-byte Matrix44.
    So every part's palette is a selection from **one** pose array,
    `srcPalette`, through two index tables: the mesh's bones to the model's
    bones (`m+0x4C`), and the model's bones to the skeleton's (`remap`).
  - **Which draw a part feeds (RE-5, `--skate3_harness_re_ubt=N`):** part `k`
    of a `StartJobs` call is the entity's current-LOD instance `k`
    (`entity + 16` LOD index, instances at `entity + (lod + 8) * 4`, stride
    `0x28`), and each part has exactly one MeshContext: its copy points at
    the same mesh (`m_pMesh`, `+0`) and model as instance `k`'s persistent
    MeshContext 0, the one the renderer's draw items carry. Checked over a
    reel (run `20261115-231543_re_ubt_reel`): 135 samples, 7 skater entities
    (the player and the replay's skaters), 1,312 parts, every one part `k`
    = instance `k`, including an entity switching LOD (8 and 10 parts). The
    Odin 2 agrees (`20261115-231919_re_skeleton_link`: 154 samples, 8
    entities, 1,468 parts).
- **A skater's palette, one tick from pose to GPU** (RE-5 step 0, run
  `20261115-233443_re_skeleton_link`, 170 packs, every one matching):
  1. `StartJobs`: UBT gathers each LOD instance's 4x4 bone matrices from
     `srcPalette` into the instance's own `m_matrices` buffer. **The UBT
     "parts" array is the entity's LOD instance array** (same address,
     `entity + (lod + 8) * 4`), not a copy.
  2. `EndJobs`: `PackAndMultiplyMatricesForUpload` (`sub_827E5B30`) packs
     each of those instances in place, with `localToWorld` = the entity's
     `m_MatLtoW` (`+0x160`).
  3. So every packed bone row is
     `transpose(srcPalette[remap[meshbone[i]]] × m_MatLtoW)`, rows 0-2: the
     3 float4 per bone the native shader reads. Rotation and translation
     match to float rounding (3e-5).
  The instance arrays, `srcPalette` and the remap tables stay put across
  ticks for an entity, but move now and then (reel loads, LOD switches),
  so they are re-read every tick.
- **Other characters** pack per instance with
  `cModelInstance::PackAndMultiplyMatricesForUpload` (`sub_827E5B30`).
  LOD pedestrians use a fused writer (`sub_827C1D38`) that concatenates
  every mesh's palette into one buffer.

## Native replacements (RE-7 candidates)

The goal is to replace recompiled game code with native code that is
known to be right. RE-5 gives the first two fully specified functions, and
the skeleton store already reimplements them on the host:
- `Sk8::UpdateBoneTransforms` (`sub_827A52C8`): a pure gather (above).
- `PackAndMultiplyMatricesForUpload` (`sub_827E5B30`): each gathered row
  times `localToWorld`, transposed to three float4 rows.

The store's `BuildPiece` (`src/native/skate3_native_skeleton.cpp`) matched
the game's own upload on every skater piece in shadow mode (6,336 on
Windows, 4,016 on the Odin 2). A native replacement would run the host code
in place of the recompiled function and write the same `m_matrices`. Shadow
first (compare the guest's result each call), then replace. Not started.

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

## The redesign (in progress)

**Status (2026-11-16): step 4 done, `on` is the default.** Step 5 (deleting
the per-piece guesses for skaters) is next.

**Step 4 (2026-11-16), the checks before default on:**
- Held reel stills, each shot with the store off and on
  (`scenarios/skel_stills.toml`, `shots.py pairs <run> off on`): all 10
  pairs pixel-identical on Windows and the Odin 2. Held frames give the old
  path time to settle, so this rules out regressions only.
- `skel_off.toml` vs `skel_on.toml` (free skate and the forearm reel),
  medians of 3-4 runs each, all char_sync clean (0 bad frames):

  | Platform | busy p50 off / on | busy p99 off / on | 1% low off / on |
  |---|---|---|---|
  | Odin 2 | 6.28 / 6.10 ms | 15.02 / 15.01 ms | 26.4 / 27.4 fps |
  | Windows Vulkan | 1.88 / 2.05 ms | 4.62 / 4.57 ms | 29.4 / 29.4 fps |

  No regression; single pairs vary more than these differences (Odin 2
  busy p99 13.7-17.3 ms within one setting).
- Store counters with `on`: Windows 6,352 pieces and Odin 2 5,191, all
  built (0 missing), 0 divergent from the game's upload.

**Step 3 (2026-11-16):**
`skate3_native_render_scene_skeleton = on` serves every skinned piece of a
skater-family entity from the store (`dbg_src` 14). Each character also gets
one jump counter, raised for:
- a root step that the last two ticks' motion doesn't explain (over 1.5 m);
- a gap of more than 100 ms;
- an LOD switch.

Served pieces smooth in rings keyed by their draw context and reset together
when the counter changes, instead of each piece deciding on its own
distance gate. Check: `scenarios/skel_on.toml`, then
`skel_shadow_report.py <run> --max-div 0` and `char_sync_report.py <run> --reel`.

| Run | Served | Exact vs bank | char_sync (reel) |
|---|---|---|---|
| Windows Vulkan `20261116-001153_skel_on` | 6,336 | all | 0 bad frames |
| Odin 2 `20261116-002508_skel_on` | 4,634 | all | 0 bad frames |

KI-125 (every frame of the reel jump, `scenarios/video_ki125.toml`): the
skater's body is whole on the first frame after the jump with the store on
and off. The cloth-simulated pieces (ropa: the long hair and the shirt) are
not: they are outside the store's reach, and their drape on that frame is
one the game draws only because its GPU reads the buffer late. See KI-125.

**Cloth (ropa) after a replay seek (2026-11-16):**
- After the seek the skeleton holds one tick for several frames (the game
  re-settles), while the cloth payload changes between the first and second
  frame. The game's frames show the cloth attached throughout.
- Each garment's deformed VB is a double buffer (`SkaterPresEntity +1936`,
  two VB objects per garment); `DoubleBuffer` (`sub_82783038`) flips them once
  per completed cloth tick. Our draws alternate between the two halves
  frame by frame, as the game's do.
- The capture's mode test (bank flag row x exactly 1.0 = sim inactive)
  reads 0.0 for the hair on every frame, including the bad one, and that
  is right: the game draws the same buffer in the same mode.
- **The race (found 2026-11-16):** on the first frame after a seek the
  game draws from the buffer still on screen (old drape), and during the
  next guest frame rewrites that same buffer with the new drape before the
  flips resume. The GPU reads vertex memory about a frame after the draw,
  so the game's frame shows the new drape. Any native reader that copies
  cloth VBs at draw capture sees the old one. Evidence: the emulated
  RenderDoc capture (`scenarios/ki125_capture.toml`,
  `tools/harness/rdc_fetch_scan.py`) and the `ropa-mem:` probe lines
  (fingerprints at capture, frame end and next build). A native cloth
  path must therefore read the garment VB at the *next* build, which is
  also what a native replacement of the cloth upload (RE-7) has to
  reproduce.

**Debugging a character frame by frame:**
`skate3_native_render_scene_skeleton_log_jumps = true` logs, for 4 frames
after any character's jump counter rises:
- `skel-jf:` every item of that character as drawn (mode, source, world,
  cloth flag, cloth tick age, drape centroid);
- `skel-new:` skater pieces drawn without a store entry;
- `skel-draw:` raw cloth draws, with whether the decode they bind matches
  this frame's payload.

Pair it with `scenarios/video_ki125.toml` (every frame to PNG). A one-frame
bug falls between screenshots; the video catches it.

**Steps 1 and 2 (2026-11-15), shadow mode:**
`src/native/skate3_native_skeleton.{h,cpp}` keeps one pose per skater per
tick and rebuilds every piece from it. It's behind
`skate3_native_render_scene_skeleton = off | shadow` (default off then; `on` since step 4). In shadow
it compares each rebuilt palette with the draw's bank capture (the game's own
upload) and with today's serve, and draws nothing from it. Check:
`scenarios/skel_shadow.toml`, then `tools/harness/skel_shadow_report.py <run>`.

| Run | Pieces | Built | Exact vs bank (newest tick) | Exact vs serve | Not served today |
|---|---|---|---|---|---|
| Windows Vulkan `20261115-234516_skel_shadow` | 6,336 | all | 6,336 | 4,262 | 2,074 |
| Odin 2 `20261115-234759_skel_shadow` | 4,016 | all | 4,016 | 3,664 | 352 |

There were no divergent pieces and no misses. The draw always uses the newest
complete tick. The pieces today's serve skipped (whole 30-frame windows at times)
keep their bank capture; the store covers them too.


1. **Capture the pose where the game builds it:** at the
   `UpdateBoneTransforms` exit, read `srcPalette` and each part's remap.
   That is one skeleton per character per tick, all from the same moment;
   the gather above rebuilds every part's palette from it exactly.
2. **Link each part record to its draw item:** solved (above). A draw
   item's persistent MeshContext gives (entity, instance index) through the
   entity store (`native_entity::LookupCtx`), and instance index `k` is UBT
   part `k` of that entity's `StartJobs`.
3. **Build every piece's palette from that one pose,** and smooth one history
   per character instead of one per piece.
4. **Verify in shadow mode** against today's per-piece palettes (reel
   scenarios, char_sync), then delete the per-piece guesses.
5. **Then** pedestrians and NPC skaters, through the pack and LW paths.
