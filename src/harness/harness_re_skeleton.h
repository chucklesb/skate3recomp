#pragma once
// RE-5 probe: how the player skater's per-frame part records (the
// Sk8::UpdateBoneTransforms parts array) link to the persistent
// MeshContexts the renderer draws. docs/re/characters.md.
//
// --skate3_harness_re_ubt=N logs, every Nth UBT call inside a skater's
// StartJobs, each part's m_model and its MeshContext's m_pMesh next to the
// owning entity's persistent MeshContexts (from the BindConstants walk),
// and which persistent contexts share the part's mesh or model.
#include <cstdint>

namespace skate3::harness::re_skeleton {

// SkaterPresEntity::StartJobs entry / exit (r3 = the entity; 0 on exit).
void SetOwner(uint32_t entity);

// Sk8::UpdateBoneTransforms exit (sub_827A52C8): r3 parts, r4 count.
void OnBoneTransforms(uint8_t* base, uint32_t parts, uint32_t count);

}  // namespace skate3::harness::re_skeleton
