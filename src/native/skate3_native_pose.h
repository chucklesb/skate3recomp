#pragma once
// Native replacements for the character pose functions (RE-7).
//
// RE-5 specified these recompiled functions completely (docs/re/characters.md,
// docs/re/symbols.toml):
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
// skate3_native_pose_replace = off | shadow | on (default on):
//  - shadow: compute the native result before the recompiled function runs,
//    then compare it byte for byte with what the function wrote (pose: lines);
//  - on: write the native result and skip the recompiled function.
#include <cstdint>

#include <rex/ppc/context.h>

namespace skate3::native_pose {

// 0 off, 1 shadow, 2 on.
int Mode();

// sub_827E5B30 hook. Before: shadow mode computes the expected rows (call
// before the recompiled function). After: compares (call after it).
// Replace: "on" mode writes the result; returns false when the inputs fail
// their bounds (the caller then runs the recompiled function).
void PackBefore(PPCContext& ctx, uint8_t* base);
void PackAfter(uint8_t* base);
bool PackReplace(PPCContext& ctx, uint8_t* base);

// sub_827C1D38 hook, the same three calls (r3 = the entity).
void LwBefore(PPCContext& ctx, uint8_t* base);
void LwAfter(uint8_t* base);
bool LwReplace(PPCContext& ctx, uint8_t* base);

// sub_827A52C8 hook (Sk8::UpdateBoneTransforms: r3 parts, r4 count, r5
// srcPalette, r6 remaps), the same three calls.
void UbtBefore(PPCContext& ctx, uint8_t* base);
void UbtAfter(uint8_t* base);
bool UbtReplace(PPCContext& ctx, uint8_t* base);

// Frame tail: the pose: stats line.
void OnFrameBuilt();

}  // namespace skate3::native_pose
