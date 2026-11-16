#pragma once
// Native replacements for the character pose functions (RE-7).
//
// RE-5 specified these recompiled functions completely (docs/re/characters.md,
// docs/re/symbols.toml):
//  - Sk8::UpdateBoneTransforms (sub_827A52C8): a 64-byte gather;
//  - cModelInstance::PackAndMultiplyMatricesForUpload (sub_827E5B30): in
//    place, bone i's 4x4 m_matrices row block M becomes three float4 rows,
//    the first three of transpose(M x localToWorld);
//  - the LivingWorld batch pack writer (sub_827C1D38): the same rows, with
//    M gathered from the entity's pose array through the mesh bone lists and
//    remap tables.
//
// The guest's arithmetic is reproduced bit for bit: each row of M x L is
// m.x * L0, then fused multiply-adds of m.y * L1, m.z * L2, m.w * L3 in
// that order (vmulfp, then vmaddfp, which the recompiler emits as a true
// FMA), under the guest's VMX flush-to-zero mode.
//
// skate3_native_pose_replace = off | shadow | on (default on) sets all three
// families; the hooks run them through native_replace::Run (shadow compares,
// on replaces; replace: lines).
#include "native/skate3_native_replace.h"

namespace skate3::native_pose {

// 0 off, 1 shadow, 2 on (the cvar).
int Mode();

extern native_replace::Family g_pack;  // sub_827E5B30 (r3 instance, r4 localToWorld)
extern native_replace::Family g_lw;    // sub_827C1D38 (r3 entity)
extern native_replace::Family g_ubt;   // sub_827A52C8 (r3 parts, r4 count, r5 src, r6 remaps)

// Frame tail: re-reads the cvar.
void OnFrameBuilt();

}  // namespace skate3::native_pose
