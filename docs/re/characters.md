# Characters: skeleton, parts, palettes

- **Item:** RE-5, The character skeleton ([all items](README.md#the-re-items))
- **Goal:** Draw every character from the game's own pose (one per character per tick), not from guesses about uploaded constants.
- **Done when:** Every character piece (skaters and pedestrians) is served from the skeleton store, the character guesses in [workarounds.md](workarounds.md) are deleted, and KI-125 is fixed.
- **Status:** in progress: the store serves skaters and pedestrians, on by default since 2026-11-16 (2026-11-18)
- **Next:** KI-125 (deferred by the user); delete the remaining guesses

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
- **Pedestrians (LivingWorld entities; RE-5b, 2026-11-16):** the batch
  pack writer `sub_827C1D38` is the same two steps fused, over the same
  tables:
  - instances `entity[(lod + 8) * 4]`, count `entity[(lod + 5) * 4]`, `lod`
    at `+0x10` (the skaters' layout);
  - the pose array from the vtable `+0x60` getter (`sub_827B8228`:
    `*(entity + 0x4B0)`), 64-byte matrices;
  - one remap table per instance at `entity + 0x270 + 4k`;
  - `localToWorld` = `m_MatLtoW` (`+0x160`).

  So each row is `transpose(src[remap[meshbone]] × m_MatLtoW)`, exactly the
  skater formula, written straight into `m_matrices`. Models above 80 bones
  are skipped, as in UBT.

## Native replacements (RE-7 candidates)

The goal is to replace recompiled game code with native code that is
known to be right. RE-5 gives the first two fully specified functions, and
the skeleton store already reimplements them on the host:
- `Sk8::UpdateBoneTransforms` (`sub_827A52C8`): a pure gather (above).
- `PackAndMultiplyMatricesForUpload` (`sub_827E5B30`): each gathered row
  times `localToWorld`, transposed to three float4 rows.

**Done (2026-11-16), on by default:** `src/native/skate3_native_pose.{h,cpp}`
runs native versions of all three, in place of the recompiled code:
- `Sk8::UpdateBoneTransforms` (`sub_827A52C8`): the 64-byte gather;
- `PackAndMultiplyMatricesForUpload` (`sub_827E5B30`): in place, bone `i`
  reads 64 bytes at `+64i` and writes 48 at `+48i`;
- the LivingWorld batch pack writer (`sub_827C1D38`): both fused. It runs
  natively only when the entity's vtable `+0x60` is the known pose getter
  (`sub_827B8228`); other classes keep the recompiled function.

**Bit for bit.** The guest computes each row of `M × L` as `m.x·L0`, then
`vmaddfp` of `m.y·L1`, `m.z·L2`, `m.w·L3`. The recompiler emits `vmaddfp` as a
true FMA (`simde_mm_fmadd_ps`) under the VMX flush-to-zero mode
(`third_party/rexglue-sdk/src/codegen/builders/vector.cpp`). The native code
uses the same primitives in the same order after `ctx.fpscr.enableFlushMode()`,
and masks vector addresses to 16 bytes like `lvx`/`stvx`.

Switch: `skate3_native_pose_replace = off | shadow | on` (`replace:` lines,
`tools/harness/replace_report.py`; the framework is
`src/native/skate3_native_replace.{h,cpp}`, see below).
Shadow computes before the recompiled function and compares bytes after.

| Check | Windows Vulkan | Odin 2 |
|---|---|---|
| Shadow, plaza + forearm reel | Pack 54,452, LW 31,633, UBT 5,408 calls, all bit-exact | Pack 49,325, LW 30,372, UBT 5,315, all bit-exact |
| On: skeleton store vs the game's upload | 0 divergent | 0 divergent |
| On: char_sync, forearm reel | 0 bad frames | - |
| Frame time, plaza, off / on (medians) | busy p50 4.62 / 4.68 ms, p99 7.01 / 6.90 | busy p50 6.97 / 6.84 ms, p99 11.62 / 11.04 |

**Lesson:** the first version read guest memory through `GuestTryCopy` (a
non-inlined SEH / `sigsetjmp` call per read) and was slower than the
recompiled code (Windows busy p50 4.51 → 4.82 ms). A replacement only
touches what the guest code touches on the same call, so it reads directly.

**Framework for the next ones** (`src/native/skate3_native_replace.{h,cpp}`,
2026-11-16): a replacement is a `Family` (name, mode function, `Compute`
that fills `Writes`: the rows of big-endian words the recompiled function
would write). The hook body is one call, `native_replace::Run(family, ctx,
base, &__imp__sub_X)`: off runs the recompiled code; shadow computes, runs it
and compares every row; on writes the computed rows (and falls back to the
recompiled code when `Compute` refuses). `skate3_native_replace_all =
default | off | shadow` overrides every family for A/B runs. One `replace:`
line per 120 frames; `tools/harness/replace_report.py <run>` sums them and
fails on any diff.

### Character lighting rows: the BindConstants family (RE-7 Part C, 2026-11-16)

The native renderer takes each character piece's lighting rows (`char_rows`)
from the pixel-shader constant bank after the draw, paired with the piece by
its buffers (the post-draw fix-up, ~95% of pieces). Where those rows come
from:

- `Sk8::PresentationEntity::BindConstants` (`sub_827A6658`, base): for every
  material of every LOD instance (records of 80 bytes, count `u16 +50` of the
  model), a virtual `SetParameter(nameHash, data, count)` on the material's
  parameter block: `0x7DDF552B` from `entity+416`, 4 rows (the world
  matrix).
- `Sk8::SkaterPresEntity::BindConstants` (`sub_82783D68`): `0xD5DF94E7` from
  `+496` (1 row), `0x040E4D1E` from `+512` (9 rows), `0x39E49D7A` from `+656`
  (1 row), and `0x5DA89822` / `0x5DA89821` from two pointers that
  `sub_82C282C8(entity+12, ...)` returns; then `sub_827A6A68`.
- `cLivingWorldPresEntity::BindConstants` (`sub_827C1720`): the same
  `0xD5DF94E7` and `0x39E49D7A` parameters from `+528` and `+592`.
- Its derived class bound by `sub_827C25D8` (Skate 2 lead:
  `MeshIndicator::BindConstants`): `sub_827816C0` copies 9 rows (and 4 words)
  from a global lighting object, stored to `+1232..+1360` and bound as `0x040E4D1E`; plus
  `0xB005EA6C` from `+1168` and `0x576C94F7` from `+1184`.

**`SetParameter` keeps a pointer, not a copy:** the constants reach the GPU
from the entity's memory at upload time. A snapshot taken right after
BindConstants matches the skater's 9 rows 0% of the time; the live fields
read at the draw match 100%.

**Measured register map** (`skate3_native_render_scene_char_rows_probe`,
plaza, Windows; bit-exact matches of the validated bank with the entity's
live fields):

| Family | Rows from the entity | Match |
|---|---|---|
| 2 (skater skin / cloth) | c22 = `+496`; c24..c32 = `+512..+640` (the 9 rows) | 100% (c22 99.7%: the field can change between upload and capture) |
| 4 (hair) | c15 = `+496`; c32 = `+640` | 100% |
| 3 (pedestrian) | c22 = `+1168`; c23 = `+1184` | 100% |
| 6 (vehicles) | c21 = `+1168`; c22 = `+1184` | 100% |
| 5 | c22 / c23 = `+1168` / `+1184` | partial (several shaders share the family) |

Every other row the families use is the same for all entities in a frame
(light direction, key colour, exposure, shadow biases) or a material
constant. So the rows can be built without the buffer pairing: per-entity
rows read from the entity at the draw, frame-global rows from any validated
capture this frame, material rows per mesh.

**Built and on by default (2026-11-16):** `skate3_native_render_scene_char_rows_entity =
off | shadow | on`. The bank → `char_rows` transform is one function
(`CharRowsTransform`); each validated capture keeps its raw rows (c0..c39)
per (mesh, ctx). A rebuild takes those rows, overwrites the entity rows
with the entity's live fields (the map above; gameplay layout only, the
editor `plus` layout is unverified) and the light row with this frame's
(any validated capture of the family), and reruns the transform; the decal
layers come from the cached piece. Shadow compares each validated capture
with the rebuild from the previous one; on serves the rebuild to pieces
whose capture failed this frame (before the per-instance cache).

| Check | Windows | Odin 2 |
|---|---|---|
| Shadow, forearm reel | 9,781 / 9,783 exact (the 2: the first pieces of a seek frame) | - |
| Shadow, plaza | 52,468 / 54,564 (96%) | 54,132 / 55,929 (96.8%) |
| On, plaza: served / char_sync | - | 260 served, 0 frames out of sync |
| On, forearm reel: char_sync | 0 (nothing to serve: every piece validated) | - |

Every plaza difference is the pedestrians' fade opacity (`ch_misc.x`, bank
c21), which is not in the verified map (`+528` matched it live only 39% of
the time): the rebuild carries the previous frame's value, off by ~0.0001
while a pedestrian fades.

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

**Status (2026-11-16): steps 1-5 done for skaters; `on` is the default.**
Step 5 deleted the player's content-matched serve (the UpdateBoneTransforms
part snapshots, their content match and the 3x4/4x4 layout vote,
`772ce82`). The other per-piece guesses still run because pedestrians use
them; for skaters they now only decide whether a piece is drawn, and the
store sets every pose ([workarounds.md](workarounds.md)). They go with
RE-5b (pedestrians).

**RE-5b (2026-11-16), pedestrians:** the store takes LivingWorld ticks
from the batch writer's hook (`OnLwPack`), behind its own switch
`skate3_native_render_scene_skeleton_lw = off | shadow | on` (default on since 2026-11-16).
Check: `scenarios/skel_lw_shadow.toml` (Tresoutta Plaza), then
`skel_shadow_report.py <run> --lw` (the `skel-lw:` lines).

| Run | Pedestrian pieces | Built | Exact vs bank | Divergent |
|---|---|---|---|---|
| Windows Vulkan `20261116-072618_skel_lw_shadow` | 12,678 | all | 11,006 of 11,006 | 0 |
| Odin 2 `20261116-072854_skel_lw_shadow` | 12,525 | all | 10,610 of 10,610 | 0 |

With `on`, every pedestrian piece is served from the store, the LW gap fill
included, at no measurable cost (plaza, medians of 3 runs, off / on):

| Platform | busy p50 | busy p99 | 1% low |
|---|---|---|---|
| Windows Vulkan | 4.81 / 4.60 ms | 7.03 / 7.17 ms | 29.2 / 29.3 fps |
| Odin 2 | 6.96 / 6.97 ms | 11.34 / 11.41 ms | 28.0 / 28.2 fps |

So `on` is the default. char_sync used to flag 55-98 frames a run at the
plaza either way, mostly gap-filled pedestrians at opacity 0 (fading, not
visible); it skips those now (`d537ef7`): 41 frames off, 30 on. The rest
are visible gap-filled pieces for 1-3 frames and pieces switching mode,
the next things to look at.

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

## Progress (moved from the roadmap, 2026-11-18)

**The character skeleton** ([re/characters.md](characters.md)):
one pose per character per tick; the likely fix for
[KI-125](../known-issues.md#ki-125-the-skater-comes-apart-for-a-frame-or-two-after-a-replay-jumps).
Done: UBT is a pure gather from one pose array (`srcPalette`), and UBT
part `k` is the entity's LOD instance `k` (the part ↔ draw link,
checked over a reel). The pack is `transpose(gather × m_MatLtoW)`, exact.
Steps 1-2 (the store, shadow mode): every skater piece exact against the
game's upload on Windows and the Odin 2. Step 3 (serve, one reset per
character), verified on both (char_sync clean). Step 4: stills, fps and
char_sync show no regression on either platform; **`on` is the default**
(2026-11-16).
Step 5: the player's content-matched serve is deleted; the other guesses
only decide which skater pieces are drawn now, and stay for pedestrians.
RE-5b (pedestrians): the LivingWorld batch writer is the same gather and
pack fused; the store rebuilds every pedestrian piece exactly (shadow,
both platforms) and serves them (`skeleton_lw = on`, the default since
2026-11-16; no frame-time cost on either platform).
Next: the remaining char_sync flags at busy spots (visible gap fills,
mode switches), then delete the remaining per-piece guesses
([re/workarounds.md](workarounds.md)).
KI-125 turned out to be the cloth, not the skeleton: a buffer-read timing
race after a seek, documented, fix deferred.
