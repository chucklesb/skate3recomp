#pragma once
// RE-5 probe: how the player skater's per-frame part records (the
// Sk8::UpdateBoneTransforms parts array) link to the persistent
// MeshContexts the renderer draws, and what happens to a part's palette
// between UBT and the pack. docs/re/characters.md.
//
// --skate3_harness_re_ubt=N samples every Nth skater StartJobs. For a
// sampled entity it logs, until its next StartJobs:
//  - each UBT call ("re-ubt:"): every part's m_model, its MeshContext's
//    m_pMesh and m_matrices, against the entity's persistent LOD instances
//    (which share the mesh, model and matrix buffer), plus srcPalette and the
//    remap table pointers;
//  - each pack ("re-pack:", PackAndMultiplyMatricesForUpload) inside the
//    entity's brackets: the instance packed, its LOD index, the bracket
//    (StartJobs / EndJobs / driver), and the first matrix before and after.
#include <cstdint>

namespace skate3::harness::re_skeleton {

enum Bracket : int { kNone = 0, kStartJobs = 1, kEndJobs = 2, kDriver = 3 };

// Entry of a skater bracket (r3 = the entity); returns the previous owner
// and bracket so the caller restores them on exit with RestoreOwner.
uint64_t EnterOwner(uint8_t* base, uint32_t entity, Bracket bracket);
void RestoreOwner(uint64_t saved);

// Sk8::UpdateBoneTransforms exit (sub_827A52C8): r3 parts, r4 count, and
// r5 srcPalette, r6 remaps as they were at entry.
void OnBoneTransforms(uint8_t* base, uint32_t parts, uint32_t count, uint32_t src_palette,
                      uint32_t remaps);

// PackAndMultiplyMatricesForUpload (sub_827E5B30): entry and exit, r3 the
// instance, r4 its localToWorld argument; the entry copy is compared with
// the exit.
void OnPackEnter(uint8_t* base, uint32_t instance, uint32_t local_to_world);
void OnPackExit(uint8_t* base, uint32_t instance);

}  // namespace skate3::harness::re_skeleton
