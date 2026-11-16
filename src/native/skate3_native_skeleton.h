#pragma once

// Character skeleton store (RE-5, docs/re/characters.md "one tick from pose
// to GPU").
//
// One pose per character per tick, captured where the game builds it, and
// every piece's palette rebuilt from it exactly:
//
//  1. SkaterPresEntity::StartJobs -> Sk8::UpdateBoneTransforms (sub_827A52C8)
//     gathers each LOD instance k's bone matrices out of ONE pose array,
//     srcPalette (r5), through the mesh's bone list (u16 at mesh+0x4C, count
//     mesh+0x48, meshes concatenated) and the part's remap table (r6[k], u32).
//     The parts array (r3) IS the entity's current-LOD instance array.
//  2. SkaterPresEntity::EndJobs -> PackAndMultiplyMatricesForUpload
//     (sub_827E5B30) packs each instance in place with localToWorld (r4) =
//     the entity's m_MatLtoW.
//  3. So every packed row is transpose(srcPalette[remap[meshbone[i]]] x L),
//     rows 0-2: the 3 float4 per bone DrawItem::bones holds.
//
// The store keeps, per entity, the last two complete ticks: the pose rows
// the gather references, each part's gather list, and L. A draw item's
// persistent MeshContext maps to (entity, instance) through the identity
// store (native_entity::LookupCtx), and instance k of the tick's parts array
// is the piece.
//
// skate3_native_render_scene_skeleton = off | shadow. In shadow, every
// skinned item of a skater-family entity gets its palette rebuilt from the
// store and compared, without changing anything, with the bank capture (the
// game's own upload) and with what today's palette serve chose; a
// "skel-shadow:" line reports the counts (tools/harness/skel_shadow_report.py).
// A piece the store can't build is counted as missing, never filled.

#include <cstddef>
#include <cstdint>

namespace skate3::native_scene {
struct FrameScene;
}  // namespace skate3::native_scene

namespace skate3::native_skeleton {

// True when the store captures (cvar not "off").
bool Active();

// Sk8::UpdateBoneTransforms EXIT, with r5/r6 as they were at entry, inside a
// pack-owner bracket (owner = the entity whose StartJobs/driver runs it).
void OnBoneTransforms(uint8_t* base, uint32_t owner, uint32_t parts, uint32_t count,
                      uint32_t src_palette, uint32_t remaps);

// PackAndMultiplyMatricesForUpload ENTRY: r3 the instance, r4 localToWorld.
void OnPack(uint8_t* base, uint32_t owner, uint32_t instance, uint32_t local_to_world);

// Scene builder, around the first ServeAuthoritativePalettes call: Before
// builds every covered item's palette from the store and compares it with
// the item's bank capture; After compares it with the served palette.
void ShadowBeforeServe(uint8_t* base, const skate3::native_scene::FrameScene& scene);
void ShadowAfterServe(const skate3::native_scene::FrameScene& scene);

// Frame tail: expiry and the stats line.
void OnFrameBuilt();

}  // namespace skate3::native_skeleton
