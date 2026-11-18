# Culling (Sk8 vectorized frustum culling)

- **Item:** part of RE-7 ([all items](README.md#the-re-items))
- **Goal:** the vectorized culling job as our own code.
- **Done when:** the culling job chain is native and bit-exact on both platforms.
- **Status:** `DoFrustumCulling` and its job wrappers native over typed structs, bit-exact in shadow mode (2026-11-18)
- **Next:** the occluder setup that fills `CullingParams`

How Skate 3 decides which objects are visible, as far as the culling job
goes. Native implementation: `src/native/skate3_native_cull.cpp`
(`skate3_native_cull_replace`, RE-7; [replacements.md](replacements.md)).
Names are Skate 2 leads (`sk2_match.py`), confirmed by what the code does.

## The job chain
- `BufferedIterativeJobEntryPoint<&Sk8::VectorizedCullingJob>`
  (`sub_82812A20`, `(records, n)`): an EA::Jobs job body. Runs
  `VectorizedCullingJob` for each 16-byte record (four words, passed as the
  job's four arguments).
- `Sk8::VectorizedCullingJob` (`sub_82817138`, `(?, desc, ?, ?)`):
  - `params = desc+8`;
  - copies the occluder planes from `params+0` (32 bytes per plane: a
    point, then a normal; 5 planes per occluder, `params+5260` occluders)
    onto its stack as splatted vectors (6 × 16 bytes per plane: point x, y,
    z, normal x, y, z), zeroes a 4-lane change counter, and calls
    `DoFrustumCulling(desc, params, planes, occluder count, counter)`.
- `DoFrustumCulling` (`sub_82816A40`): the test itself, pure VMX math.

## Data
**The job descriptor** (`desc`):

| Offset | Type | What |
|---|---|---|
| +0 | u32 | the items' AABBs: min (16 bytes), then max (16 bytes) per item |
| +4 | u32 | per-item size vectors (16 bytes each), or 0 for no size test |
| +8 | u32 | `params` |
| +12 | u32 | output: a u16 visibility mask per item (0xFFFF visible, 0 culled) |
| +16 | u16 | item count (processed in groups of 4) |

**`params`:**

| Offset | Type | What |
|---|---|---|
| +0 | 32 bytes × 5 per occluder | occluder planes: point (x, y, z, -), normal (x, y, z, -) |
| +5136 | vec4 | size axis: the absolute values weight an item's size vector |
| +5152..+5232 | 6 × vec4 | frustum planes (nx, ny, nz, d); inside means n·p + d ≥ 0 |
| +5260 | u32 | occluder count |
| +5264 | f32 | the smallest projected size kept |
| +5268 | f32 | the depth from which the size test applies |
| +5272 | f32 | the depth scale (its reciprocal is used) |

## The test, per group of 4 items
1. **Frustum.** For each AABB, center c = (min + max)·0.5 and extent
   e = max − c. For each of the 6 planes: distance n·c + d plus the box's
   projected radius |n|·e. The item is visible if the smallest of the six
   is ≥ 0 (no plane has the whole box outside).
2. **Occluders** (only for items still visible, and only when there are
   occluders). An item is hidden when all 8 of its box corners lie behind
   (≤ 0) all 5 planes of one occluder. Each plane's test is
   (corner − point)·normal. A group stops early once no item is left.
3. **Projected size** (only with size vectors, any item visible, and any
   size vector's 4th component nonzero; and only on the occluder path, so
   never when there are no occluders). Frustum plane 4 (`+5216`) is the
   depth plane: take the box corner nearest it (per axis, min or max by
   the normal's sign), depth = max(n·corner + d, 1). From depth `+5268` on,
   the item is kept only if size·|axis| / (depth² / scale) ≥ `+5264`,
   with size from the size vector and `+5136`. The reciprocals use one
   Newton step.
4. **Output:** a u16 mask per item. The counter adds 1 per lane whose
   result the occluders or the size test changed. It lives on the job's
   stack and is discarded, so it was probably a debug statistic.

The constants the code loads (`0x8231A910` zero, `0x82312170` all-ones,
`0x8232F650` 0.5, `0x8231BA60` 1.0) are uniform vectors, so the "selects"
in the occluder setup are plain copies (min/max corners).

## Verification (2026-11-16)
- **Shadow** (native computed before the recompiled function, every
  output byte compared): bit-exact on every call.
  - Windows: plaza 190,086 calls, forearm reel 56,317, `perf_reel` 181,769.
  - Odin 2: plaza 267,781 calls, forearm reel 50,561.
- **On** (the default since 2026-11-16): reel stills identical to off
  (SSIM 1.0), forearm reel char_sync 0. Job-thread CPU, plaza at 30 fps:

  | | off | on |
  |---|---|---|
  | Windows (2 runs) | 9.57-9.71 | 8.80-8.99 ms/frame (-8%) |
  | Odin 2 (median of 3) | 4.26 | 4.06 ms/frame (-5%) |

  Frame times level on both (Odin 2 busy p99 11.6-11.9 ms on, 11.7-12.7 off).

## Layouts (source of truth: `src/native/skate3_native_cull.cpp`)

- `CullingParams` (the job's data, `*(job + 8)`): `occluder_planes[160]`
  (32 bytes each: point, normal; 5 per occluder) at +0, `size_axis` +5136,
  `frustum[6]` (x, y, z, d) +5152 (plane 4 also gives the size test's depth),
  `occluder_count` +5260, `size_min` +5264, `size_far` +5268, `size_scale`
  +5272.
- `CullDescriptor` (DoFrustumCulling's first argument): `boxes` (32-byte
  min/max `Box`es), `sizes` (16-byte vectors, or 0), +8 unknown,
  `visibility` (u16 per item), `count` (u16, +16).
- VectorizedCullingJob's stack: the change counter at +80, the splatted
  occluder planes (96 bytes each: px, py, pz, nx, ny, nz) at +96.

