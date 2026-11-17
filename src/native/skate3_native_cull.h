#pragma once
// Sk8 vectorized frustum culling, native (RE-7; docs/re/culling.md).
//
// BufferedIterativeJobEntryPoint<VectorizedCullingJob> (sub_82812A20) runs
// a batch of culling jobs; each Sk8::VectorizedCullingJob (sub_82817138)
// splats the occluder planes onto its stack and calls DoFrustumCulling
// (sub_82816A40), which tests groups of 4 AABBs against the view frustum,
// the occluders and a projected-size threshold and writes a u16 visibility
// mask per item.
//
// skate3_native_cull_replace = off | shadow | on (default off until shadow
// is exact everywhere): DoFrustumCulling as a Compute family `cull`
// (bit-exact by following the guest's float op order with the recompiler's
// primitives); the job and the entry point as Body families `cull_job`,
// `cull_entry` (on whenever the family is on).
namespace skate3::native_cull {

// Frame tail: cvar refresh.
void OnFrameBuilt();

}  // namespace skate3::native_cull
